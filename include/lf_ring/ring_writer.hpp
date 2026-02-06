#pragma once

#include "lf_ring/ring_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lfring::detail {

inline bool try_push(ControlBlock* control, std::byte* ring, std::size_t capacity,
                     const void* data, std::uint32_t size, std::uint16_t type) {
  if (data == nullptr && size != 0) {
    return false;
  }

  std::size_t record_size = align_up(sizeof(RecordHeader) + static_cast<std::size_t>(size), kAlignment);
  if (record_size > capacity) {
    return false;
  }

  std::uint64_t start = 0;
  std::size_t padding = 0;
  bool needs_wrap_record = false;

  for (;;) {
    std::uint64_t head = control->head_publish.value.load(std::memory_order_acquire);
    std::uint64_t tail = control->tail_reserve.value.load(std::memory_order_relaxed);

    std::uint64_t used = tail - head;
    if (used > capacity) {
      return false;
    }

    std::size_t pos = static_cast<std::size_t>(tail % capacity);
    std::size_t remaining = capacity - pos;
    padding = 0;
    needs_wrap_record = false;

    if (remaining < sizeof(RecordHeader)) {
      padding = remaining;
    } else if (remaining < record_size) {
      padding = remaining;
      needs_wrap_record = true;
    }

    std::size_t total = padding + record_size;
    if (used + total > capacity) {
      return false;
    }

    std::uint64_t desired = tail + total;
    if (control->tail_reserve.value.compare_exchange_weak(
            tail, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      start = tail;
      break;
    }
  }

  std::size_t pos = static_cast<std::size_t>(start % capacity);
  if (padding > 0) {
    if (needs_wrap_record && padding >= sizeof(RecordHeader)) {
      auto* wrap = reinterpret_cast<RecordHeader*>(ring + pos);
      wrap->size = 0;
      wrap->type = 0;
      wrap->flags = kWrapFlag;
    }
    pos = 0;
  }

  auto* header = reinterpret_cast<RecordHeader*>(ring + pos);
  header->size = size;
  header->type = type;
  header->flags = 0;

  if (size > 0) {
    std::memcpy(ring + pos + sizeof(RecordHeader), data, size);
  }

  std::uint64_t publish_at = start;
  std::uint64_t publish_to = start + padding + record_size;
  while (control->tail_publish.value.load(std::memory_order_acquire) != publish_at) {
    // wait for earlier producers to publish
  }
  control->tail_publish.value.store(publish_to, std::memory_order_release);
  return true;
}

} // namespace lfring::detail
