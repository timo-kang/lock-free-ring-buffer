#include "lf_ring/shared_ring_buffer.hpp"

#include "lf_ring/ring_writer.hpp"

#include <cstring>
#include <stdexcept>

namespace lfring {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free for this ring buffer");

SharedRingBuffer::SharedRingBuffer(SharedRingBuffer&& other) noexcept = default;
SharedRingBuffer& SharedRingBuffer::operator=(SharedRingBuffer&& other) noexcept = default;

SharedRingBuffer SharedRingBuffer::create(const std::filesystem::path& path,
                                          std::size_t capacity_bytes) {
  return SharedRingBuffer(detail::create_mapping(path, capacity_bytes, kFlagMPSC));
}

SharedRingBuffer SharedRingBuffer::open(const std::filesystem::path& path) {
  return SharedRingBuffer(detail::open_mapping(path, kFlagMPSC));
}

bool SharedRingBuffer::try_push(const void* data, std::uint32_t size, std::uint16_t type) {
  return detail::try_push(mapping_.control, mapping_.ring, mapping_.capacity, data, size, type);
}

bool SharedRingBuffer::try_pop(std::vector<std::byte>& out, std::uint16_t& type) {
  return try_pop_internal(out, type);
}

bool SharedRingBuffer::try_pop_internal(std::vector<std::byte>& out, std::uint16_t& type) {
  for (;;) {
    std::uint64_t tail = mapping_.control->tail_publish.value.load(std::memory_order_acquire);
    std::uint64_t head = mapping_.control->head_publish.value.load(std::memory_order_relaxed);

    if (head == tail) {
      return false;
    }

    std::size_t pos = static_cast<std::size_t>(head % mapping_.capacity);
    std::size_t remaining = mapping_.capacity - pos;

    if (remaining < sizeof(RecordHeader)) {
      head += remaining;
      mapping_.control->head_publish.value.store(head, std::memory_order_release);
      mapping_.control->head_reserve.value.store(head, std::memory_order_relaxed);
      continue;
    }

    auto* header = reinterpret_cast<RecordHeader*>(mapping_.ring + pos);

    if ((header->flags & kWrapFlag) != 0) {
      head += remaining;
      mapping_.control->head_publish.value.store(head, std::memory_order_release);
      mapping_.control->head_reserve.value.store(head, std::memory_order_relaxed);
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
      std::memcpy(out.data(), mapping_.ring + pos + sizeof(RecordHeader), payload_size);
    }

    type = header->type;
    head += record_size;
    mapping_.control->head_publish.value.store(head, std::memory_order_release);
    mapping_.control->head_reserve.value.store(head, std::memory_order_relaxed);
    return true;
  }
}

std::size_t SharedRingBuffer::approx_size() const noexcept {
  std::uint64_t head = mapping_.control->head_publish.value.load(std::memory_order_acquire);
  std::uint64_t tail = mapping_.control->tail_publish.value.load(std::memory_order_acquire);
  std::uint64_t used = tail - head;
  if (used > mapping_.capacity) {
    return mapping_.capacity;
  }
  return static_cast<std::size_t>(used);
}

} // namespace lfring
