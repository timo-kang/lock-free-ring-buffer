#include "lf_ring/shared_ring_buffer_spsc.hpp"

#include <cstring>

namespace lfring {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free for this ring buffer");

SharedRingBufferSPSC::SharedRingBufferSPSC(SharedRingBufferSPSC&& other) noexcept = default;
SharedRingBufferSPSC& SharedRingBufferSPSC::operator=(SharedRingBufferSPSC&& other) noexcept = default;

SharedRingBufferSPSC SharedRingBufferSPSC::create(const std::filesystem::path& path,
                                                  std::size_t capacity_bytes) {
  return SharedRingBufferSPSC(detail::create_mapping(path, capacity_bytes, kFlagSPSC));
}

SharedRingBufferSPSC SharedRingBufferSPSC::open(const std::filesystem::path& path) {
  return SharedRingBufferSPSC(detail::open_mapping(path, kFlagSPSC));
}

bool SharedRingBufferSPSC::try_push(const void* data, std::uint32_t size, std::uint16_t type) {
  if (data == nullptr && size != 0) {
    return false;
  }

  const std::size_t record_size =
      align_up(sizeof(RecordHeader) + static_cast<std::size_t>(size), kAlignment);
  if (record_size > mapping_.capacity) {
    return false;
  }

  auto* control = mapping_.control;

  for (;;) {
    const std::uint64_t head = control->head_publish.value.load(std::memory_order_acquire);
    std::uint64_t tail = control->tail_publish.value.load(std::memory_order_relaxed);

    const std::uint64_t used = tail - head;
    if (used > mapping_.capacity) {
      return false;
    }

    std::size_t pos = static_cast<std::size_t>(tail % mapping_.capacity);
    std::size_t remaining = mapping_.capacity - pos;

    // If we can't even place a header, skip to the start.
    if (remaining < sizeof(RecordHeader)) {
      if (used + remaining > mapping_.capacity) {
        return false;
      }
      tail += remaining;
      control->tail_reserve.value.store(tail, std::memory_order_relaxed);
      control->tail_publish.value.store(tail, std::memory_order_release);
      continue;
    }

    // If the record doesn't fit, add a wrap record and skip the remainder.
    if (remaining < record_size) {
      const std::size_t padding = remaining;
      if (used + padding + record_size > mapping_.capacity) {
        return false;
      }

      if (padding >= sizeof(RecordHeader)) {
        auto* wrap = reinterpret_cast<RecordHeader*>(mapping_.ring + pos);
        wrap->size = 0;
        wrap->type = 0;
        wrap->flags = kWrapFlag;
        wrap->sequence = 0;
      }

      tail += padding;
      control->tail_reserve.value.store(tail, std::memory_order_relaxed);
      control->tail_publish.value.store(tail, std::memory_order_release);
      continue;
    }

    // Fits at current position.
    if (used + record_size > mapping_.capacity) {
      return false;
    }

    auto* header = reinterpret_cast<RecordHeader*>(mapping_.ring + pos);
    header->size = size;
    header->type = type;
    header->flags = 0;
    header->sequence = 0;

    if (size > 0) {
      std::memcpy(mapping_.ring + pos + sizeof(RecordHeader), data, size);
    }

    header->sequence = control->sequence.value.fetch_add(1, std::memory_order_relaxed) + 1;

    tail += record_size;
    control->tail_reserve.value.store(tail, std::memory_order_relaxed);
    control->tail_publish.value.store(tail, std::memory_order_release);
    return true;
  }
}

bool SharedRingBufferSPSC::try_pop(std::vector<std::byte>& out, std::uint16_t& type) {
  return try_pop_internal(out, type, nullptr);
}

bool SharedRingBufferSPSC::try_pop(std::vector<std::byte>& out, std::uint16_t& type,
                                   std::uint64_t& sequence) {
  return try_pop_internal(out, type, &sequence);
}

bool SharedRingBufferSPSC::try_pop_internal(std::vector<std::byte>& out, std::uint16_t& type,
                                            std::uint64_t* sequence) {
  auto* control = mapping_.control;

  for (;;) {
    const std::uint64_t tail = control->tail_publish.value.load(std::memory_order_acquire);
    std::uint64_t head = control->head_publish.value.load(std::memory_order_relaxed);

    if (head == tail) {
      return false;
    }

    std::size_t pos = static_cast<std::size_t>(head % mapping_.capacity);
    std::size_t remaining = mapping_.capacity - pos;

    if (remaining < sizeof(RecordHeader)) {
      head += remaining;
      control->head_reserve.value.store(head, std::memory_order_relaxed);
      control->head_publish.value.store(head, std::memory_order_release);
      continue;
    }

    auto* header = reinterpret_cast<RecordHeader*>(mapping_.ring + pos);
    if ((header->flags & kWrapFlag) != 0) {
      head += remaining;
      control->head_reserve.value.store(head, std::memory_order_relaxed);
      control->head_publish.value.store(head, std::memory_order_release);
      continue;
    }

    const std::size_t payload_size = static_cast<std::size_t>(header->size);
    const std::size_t record_size = align_up(sizeof(RecordHeader) + payload_size, kAlignment);

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
    if (sequence != nullptr) {
      *sequence = header->sequence;
    }

    head += record_size;
    control->head_reserve.value.store(head, std::memory_order_relaxed);
    control->head_publish.value.store(head, std::memory_order_release);
    return true;
  }
}

std::size_t SharedRingBufferSPSC::approx_size() const noexcept {
  const std::uint64_t head = mapping_.control->head_publish.value.load(std::memory_order_acquire);
  const std::uint64_t tail = mapping_.control->tail_publish.value.load(std::memory_order_acquire);
  const std::uint64_t used = tail - head;
  if (used > mapping_.capacity) {
    return mapping_.capacity;
  }
  return static_cast<std::size_t>(used);
}

} // namespace lfring

