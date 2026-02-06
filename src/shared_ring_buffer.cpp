#include "lf_ring/shared_ring_buffer.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lfring {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free for this ring buffer");

namespace {

[[noreturn]] void throw_errno(const char* message) {
  throw std::runtime_error(std::string(message) + ": " + std::strerror(errno));
}

int open_file_create(const std::filesystem::path& path) {
  int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    throw_errno("open (create) failed");
  }
  return fd;
}

int open_file_existing(const std::filesystem::path& path) {
  int flags = O_RDWR;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  int fd = ::open(path.c_str(), flags);
  if (fd < 0) {
    throw_errno("open failed");
  }
  return fd;
}

std::size_t header_region_size() {
  return align_up(sizeof(Header), kCacheLineSize);
}

std::size_t control_region_size() {
  return align_up(sizeof(ControlBlock), kCacheLineSize);
}

} // namespace

SharedRingBuffer::SharedRingBuffer(int fd, void* mapping, std::size_t mapping_size, Header* header,
                                   ControlBlock* control, std::byte* ring, std::size_t capacity)
    : fd_(fd), mapping_(mapping), mapping_size_(mapping_size), header_(header),
      control_(control), ring_(ring), capacity_(capacity) {}

SharedRingBuffer::~SharedRingBuffer() {
  if (mapping_ != nullptr && mapping_size_ > 0) {
    ::munmap(mapping_, mapping_size_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

SharedRingBuffer::SharedRingBuffer(SharedRingBuffer&& other) noexcept {
  *this = std::move(other);
}

SharedRingBuffer& SharedRingBuffer::operator=(SharedRingBuffer&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (mapping_ != nullptr && mapping_size_ > 0) {
    ::munmap(mapping_, mapping_size_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }

  fd_ = other.fd_;
  mapping_ = other.mapping_;
  mapping_size_ = other.mapping_size_;
  header_ = other.header_;
  control_ = other.control_;
  ring_ = other.ring_;
  capacity_ = other.capacity_;

  other.fd_ = -1;
  other.mapping_ = nullptr;
  other.mapping_size_ = 0;
  other.header_ = nullptr;
  other.control_ = nullptr;
  other.ring_ = nullptr;
  other.capacity_ = 0;

  return *this;
}

std::size_t SharedRingBuffer::required_mapping_size(std::size_t capacity_bytes) {
  std::size_t cap = align_up(capacity_bytes, kAlignment);
  return header_region_size() + control_region_size() + cap;
}

SharedRingBuffer SharedRingBuffer::create(const std::filesystem::path& path,
                                          std::size_t capacity_bytes) {
  if (capacity_bytes == 0) {
    throw std::runtime_error("capacity_bytes must be > 0");
  }

  std::size_t cap = align_up(capacity_bytes, kAlignment);
  std::size_t header_size = header_region_size();
  std::size_t control_size = control_region_size();
  std::size_t ring_offset = header_size + control_size;
  std::size_t mapping_size = ring_offset + cap;

  int fd = open_file_create(path);

  if (::ftruncate(fd, static_cast<off_t>(mapping_size)) != 0) {
    ::close(fd);
    throw_errno("ftruncate failed");
  }

  void* mapping = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    ::close(fd);
    throw_errno("mmap failed");
  }

  std::memset(mapping, 0, mapping_size);

  auto* header = reinterpret_cast<Header*>(mapping);
  header->magic = kMagic;
  header->version = kVersion;
  header->header_size = static_cast<std::uint16_t>(sizeof(Header));
  header->capacity_bytes = cap;
  header->control_offset = header_size;
  header->ring_offset = ring_offset;
  header->alignment = kAlignment;
  header->flags = 0;

  auto* control = reinterpret_cast<ControlBlock*>(reinterpret_cast<std::byte*>(mapping) + header->control_offset);
  control->head.value.store(0, std::memory_order_relaxed);
  control->tail_reserve.value.store(0, std::memory_order_relaxed);
  control->tail_publish.value.store(0, std::memory_order_relaxed);

  auto* ring = reinterpret_cast<std::byte*>(mapping) + header->ring_offset;

  return SharedRingBuffer(fd, mapping, mapping_size, header, control, ring, cap);
}

SharedRingBuffer SharedRingBuffer::open(const std::filesystem::path& path) {
  int fd = open_file_existing(path);

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw_errno("fstat failed");
  }
  if (st.st_size < static_cast<off_t>(sizeof(Header))) {
    ::close(fd);
    throw std::runtime_error("shared memory file too small");
  }

  std::size_t mapping_size = static_cast<std::size_t>(st.st_size);

  void* mapping = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    ::close(fd);
    throw_errno("mmap failed");
  }

  auto* header = reinterpret_cast<Header*>(mapping);
  if (header->magic != kMagic) {
    ::munmap(mapping, mapping_size);
    ::close(fd);
    throw std::runtime_error("invalid ring buffer magic");
  }
  if (header->version != kVersion) {
    ::munmap(mapping, mapping_size);
    ::close(fd);
    throw std::runtime_error("unsupported ring buffer version");
  }

  std::size_t cap = static_cast<std::size_t>(header->capacity_bytes);
  std::size_t ring_offset = static_cast<std::size_t>(header->ring_offset);
  if (ring_offset + cap > mapping_size) {
    ::munmap(mapping, mapping_size);
    ::close(fd);
    throw std::runtime_error("ring buffer mapping size mismatch");
  }

  auto* control = reinterpret_cast<ControlBlock*>(reinterpret_cast<std::byte*>(mapping) + header->control_offset);
  auto* ring = reinterpret_cast<std::byte*>(mapping) + ring_offset;

  return SharedRingBuffer(fd, mapping, mapping_size, header, control, ring, cap);
}

bool SharedRingBuffer::try_push(const void* data, std::uint32_t size, std::uint16_t type) {
  if (data == nullptr && size != 0) {
    return false;
  }

  std::size_t record_size = align_up(sizeof(RecordHeader) + static_cast<std::size_t>(size), kAlignment);
  if (record_size > capacity_) {
    return false;
  }

  std::uint64_t start = 0;
  std::size_t padding = 0;
  bool needs_wrap_record = false;

  for (;;) {
    std::uint64_t head = control_->head.value.load(std::memory_order_acquire);
    std::uint64_t tail = control_->tail_reserve.value.load(std::memory_order_relaxed);

    std::uint64_t used = tail - head;
    if (used > capacity_) {
      return false;
    }

    std::size_t pos = static_cast<std::size_t>(tail % capacity_);
    std::size_t remaining = capacity_ - pos;
    padding = 0;
    needs_wrap_record = false;

    if (remaining < sizeof(RecordHeader)) {
      padding = remaining;
    } else if (remaining < record_size) {
      padding = remaining;
      needs_wrap_record = true;
    }

    std::size_t total = padding + record_size;
    if (used + total > capacity_) {
      return false;
    }

    std::uint64_t desired = tail + total;
    if (control_->tail_reserve.value.compare_exchange_weak(
            tail, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      start = tail;
      break;
    }
  }

  std::size_t pos = static_cast<std::size_t>(start % capacity_);
  if (padding > 0) {
    if (needs_wrap_record && padding >= sizeof(RecordHeader)) {
      auto* wrap = reinterpret_cast<RecordHeader*>(ring_ + pos);
      wrap->size = 0;
      wrap->type = 0;
      wrap->flags = kWrapFlag;
    }
    pos = 0;
  }

  auto* header = reinterpret_cast<RecordHeader*>(ring_ + pos);
  header->size = size;
  header->type = type;
  header->flags = 0;

  if (size > 0) {
    std::memcpy(ring_ + pos + sizeof(RecordHeader), data, size);
  }

  std::uint64_t publish_at = start;
  std::uint64_t publish_to = start + padding + record_size;
  while (control_->tail_publish.value.load(std::memory_order_acquire) != publish_at) {
    // wait for earlier producers to publish
  }
  control_->tail_publish.value.store(publish_to, std::memory_order_release);
  return true;
}

bool SharedRingBuffer::try_pop(std::vector<std::byte>& out, std::uint16_t& type) {
  return try_pop_internal(out, type);
}

bool SharedRingBuffer::try_pop_internal(std::vector<std::byte>& out, std::uint16_t& type) {
  for (;;) {
    std::uint64_t tail = control_->tail_publish.value.load(std::memory_order_acquire);
    std::uint64_t head = control_->head.value.load(std::memory_order_relaxed);

    if (head == tail) {
      return false;
    }

    std::size_t pos = static_cast<std::size_t>(head % capacity_);
    std::size_t remaining = capacity_ - pos;

    if (remaining < sizeof(RecordHeader)) {
      head += remaining;
      control_->head.value.store(head, std::memory_order_release);
      continue;
    }

    auto* header = reinterpret_cast<RecordHeader*>(ring_ + pos);

    if ((header->flags & kWrapFlag) != 0) {
      head += remaining;
      control_->head.value.store(head, std::memory_order_release);
      continue;
    }

    std::size_t payload_size = static_cast<std::size_t>(header->size);
    std::size_t record_size = align_up(sizeof(RecordHeader) + payload_size, kAlignment);

    if (record_size > remaining) {
      return false;
    }

    if (tail - head < record_size) {
      return false;
    }

    if (out.size() < payload_size) {
      out.resize(payload_size);
    }

    if (payload_size > 0) {
      std::memcpy(out.data(), ring_ + pos + sizeof(RecordHeader), payload_size);
    }

    type = header->type;
    head += record_size;
    control_->head.value.store(head, std::memory_order_release);
    return true;
  }
}

std::size_t SharedRingBuffer::approx_size() const noexcept {
  std::uint64_t head = control_->head.value.load(std::memory_order_acquire);
  std::uint64_t tail = control_->tail_publish.value.load(std::memory_order_acquire);
  std::uint64_t used = tail - head;
  if (used > capacity_) {
    return capacity_;
  }
  return static_cast<std::size_t>(used);
}

} // namespace lfring
