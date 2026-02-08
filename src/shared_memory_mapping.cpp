#include "lf_ring/shared_memory_mapping.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lfring::detail {

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

} // namespace

Mapping::~Mapping() {
  reset();
}

void Mapping::reset() {
  if (mapping != nullptr && mapping_size > 0) {
    ::munmap(mapping, mapping_size);
  }
  if (fd >= 0) {
    ::close(fd);
  }
  fd = -1;
  mapping = nullptr;
  mapping_size = 0;
  header = nullptr;
  control = nullptr;
  ring = nullptr;
  capacity = 0;
}

Mapping::Mapping(Mapping&& other) noexcept {
  *this = std::move(other);
}

Mapping& Mapping::operator=(Mapping&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  reset();

  fd = other.fd;
  mapping = other.mapping;
  mapping_size = other.mapping_size;
  header = other.header;
  control = other.control;
  ring = other.ring;
  capacity = other.capacity;

  other.fd = -1;
  other.mapping = nullptr;
  other.mapping_size = 0;
  other.header = nullptr;
  other.control = nullptr;
  other.ring = nullptr;
  other.capacity = 0;

  return *this;
}

Mapping create_mapping(const std::filesystem::path& path, std::size_t capacity_bytes, std::uint64_t flags) {
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
  header->flags = flags;

  auto* control = reinterpret_cast<ControlBlock*>(reinterpret_cast<std::byte*>(mapping) + header->control_offset);
  control->head_reserve.value.store(0, std::memory_order_relaxed);
  control->head_publish.value.store(0, std::memory_order_relaxed);
  control->tail_reserve.value.store(0, std::memory_order_relaxed);
  control->tail_publish.value.store(0, std::memory_order_relaxed);
  control->sequence.value.store(0, std::memory_order_relaxed);

  auto* ring = reinterpret_cast<std::byte*>(mapping) + header->ring_offset;

  Mapping result;
  result.fd = fd;
  result.mapping = mapping;
  result.mapping_size = mapping_size;
  result.header = header;
  result.control = control;
  result.ring = ring;
  result.capacity = cap;
  return result;
}

Mapping open_mapping(const std::filesystem::path& path, std::uint64_t expected_flags) {
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
  if (header->flags != expected_flags) {
    ::munmap(mapping, mapping_size);
    ::close(fd);
    throw std::runtime_error("ring buffer mode mismatch");
  }

  std::size_t cap = static_cast<std::size_t>(header->capacity_bytes);
  std::size_t ring_offset = static_cast<std::size_t>(header->ring_offset);
  std::size_t control_offset = static_cast<std::size_t>(header->control_offset);

  if (ring_offset + cap > mapping_size) {
    ::munmap(mapping, mapping_size);
    ::close(fd);
    throw std::runtime_error("ring buffer mapping size mismatch");
  }
  if (control_offset + sizeof(ControlBlock) > mapping_size) {
    ::munmap(mapping, mapping_size);
    ::close(fd);
    throw std::runtime_error("ring buffer control block out of range");
  }

  auto* control = reinterpret_cast<ControlBlock*>(reinterpret_cast<std::byte*>(mapping) + control_offset);
  auto* ring = reinterpret_cast<std::byte*>(mapping) + ring_offset;

  Mapping result;
  result.fd = fd;
  result.mapping = mapping;
  result.mapping_size = mapping_size;
  result.header = header;
  result.control = control;
  result.ring = ring;
  result.capacity = cap;
  return result;
}

} // namespace lfring::detail
