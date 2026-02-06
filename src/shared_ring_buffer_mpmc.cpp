#include "lf_ring/shared_ring_buffer_mpmc.hpp"

#include "lf_ring/ring_writer.hpp"

#include <cstring>

namespace lfring {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free for this ring buffer");

SharedRingBufferMPMC::SharedRingBufferMPMC(SharedRingBufferMPMC&& other) noexcept = default;
SharedRingBufferMPMC& SharedRingBufferMPMC::operator=(SharedRingBufferMPMC&& other) noexcept = default;

SharedRingBufferMPMC SharedRingBufferMPMC::create(const std::filesystem::path& path,
                                                  std::size_t capacity_bytes) {
  return SharedRingBufferMPMC(detail::create_mapping(path, capacity_bytes, kFlagMPMC));
}

SharedRingBufferMPMC SharedRingBufferMPMC::open(const std::filesystem::path& path) {
  return SharedRingBufferMPMC(detail::open_mapping(path, kFlagMPMC));
}

bool SharedRingBufferMPMC::try_push(const void* data, std::uint32_t size, std::uint16_t type) {
  return detail::try_push(mapping_.control, mapping_.ring, mapping_.capacity, data, size, type);
}

bool SharedRingBufferMPMC::try_pop(std::vector<std::byte>& out, std::uint16_t& type) {
  for (;;) {
    std::uint64_t tail = mapping_.control->tail_publish.value.load(std::memory_order_acquire);
    std::uint64_t head = mapping_.control->head_reserve.value.load(std::memory_order_relaxed);

    if (head == tail) {
      return false;
    }

    std::size_t pos = static_cast<std::size_t>(head % mapping_.capacity);
    std::size_t remaining = mapping_.capacity - pos;
    std::uint64_t available = tail - head;

    if (remaining < sizeof(RecordHeader)) {
      if (available < remaining) {
        return false;
      }
      std::uint64_t new_head = head + remaining;
      if (!mapping_.control->head_reserve.value.compare_exchange_weak(
              head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        continue;
      }
      while (mapping_.control->head_publish.value.load(std::memory_order_acquire) != head) {
      }
      mapping_.control->head_publish.value.store(new_head, std::memory_order_release);
      continue;
    }

    if (available < sizeof(RecordHeader)) {
      return false;
    }

    auto* header = reinterpret_cast<RecordHeader*>(mapping_.ring + pos);

    if ((header->flags & kWrapFlag) != 0) {
      if (available < remaining) {
        return false;
      }
      std::uint64_t new_head = head + remaining;
      if (!mapping_.control->head_reserve.value.compare_exchange_weak(
              head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        continue;
      }
      while (mapping_.control->head_publish.value.load(std::memory_order_acquire) != head) {
      }
      mapping_.control->head_publish.value.store(new_head, std::memory_order_release);
      continue;
    }

    std::size_t payload_size = static_cast<std::size_t>(header->size);
    std::size_t record_size = align_up(sizeof(RecordHeader) + payload_size, kAlignment);

    if (record_size > remaining) {
      return false;
    }

    if (available < record_size) {
      return false;
    }

    std::uint64_t new_head = head + record_size;
    if (!mapping_.control->head_reserve.value.compare_exchange_weak(
            head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      continue;
    }

    if (out.size() < payload_size) {
      out.resize(payload_size);
    }

    if (payload_size > 0) {
      std::memcpy(out.data(), mapping_.ring + pos + sizeof(RecordHeader), payload_size);
    }

    type = header->type;

    while (mapping_.control->head_publish.value.load(std::memory_order_acquire) != head) {
    }
    mapping_.control->head_publish.value.store(new_head, std::memory_order_release);
    return true;
  }
}

std::size_t SharedRingBufferMPMC::approx_size() const noexcept {
  std::uint64_t head = mapping_.control->head_publish.value.load(std::memory_order_acquire);
  std::uint64_t tail = mapping_.control->tail_publish.value.load(std::memory_order_acquire);
  std::uint64_t used = tail - head;
  if (used > mapping_.capacity) {
    return mapping_.capacity;
  }
  return static_cast<std::size_t>(used);
}

} // namespace lfring
