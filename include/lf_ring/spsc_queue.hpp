// Copyright 2025 Jonghyeok Kang
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "lf_ring/ring_layout.hpp"
#include "lf_ring/shared_memory_mapping.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <type_traits>

namespace lfring {

// SPSCQueue is a fixed-size, single-producer single-consumer queue backed by shared memory.
//
// Unlike SharedRingBufferSPSC (which handles variable-size payloads with RecordHeaders,
// sequence counters, and modulo indexing), this uses compile-time typed fixed slots with
// monotonic 64-bit indices and bitmasked slot lookup — eliminating all per-message overhead.
//
// Properties:
// - T must be trivially_copyable (memcpy is used for mmap-backed raw storage).
// - Capacity must be a power-of-two (enables bitwise AND for slot indexing).
// - FIFO ordering is guaranteed.
// - Not crash-fault-tolerant: if the producer dies mid-push, the slot may contain partial data.
template <class T, std::size_t Capacity>
class SPSCQueue {
  static_assert(std::is_trivially_copyable_v<T>, "SPSCQueue<T> requires trivially copyable T");
  static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                "SPSCQueue Capacity must be a power of two");
  static_assert(alignof(T) <= kCacheLineSize,
                "SPSCQueue<T> requires alignof(T) <= cache line size");

  static constexpr std::size_t kMask = Capacity - 1;

public:
  static SPSCQueue create(const std::filesystem::path& path) {
    return SPSCQueue(detail::create_mapping(path, Capacity * sizeof(T), kFlagFixedSPSC));
  }

  static SPSCQueue open(const std::filesystem::path& path) {
    auto mapping = detail::open_mapping(path, kFlagFixedSPSC);
    if (mapping.capacity != Capacity * sizeof(T)) {
      throw std::runtime_error(
          "SPSCQueue::open capacity mismatch: file has " + std::to_string(mapping.capacity) +
          " bytes, expected " + std::to_string(Capacity * sizeof(T)));
    }
    return SPSCQueue(std::move(mapping));
  }

  // Claims ownership of a SPSCQueue slot, creating the file or resuming from an existing one.
  //
  // Unlike SharedLatest, SPSCQueue does not need sequence reset or ring cleanup on recovery:
  // try_push() advances tail_publish only AFTER memcpy completes, so a crash mid-push leaves
  // the in-flight slot invisible to the consumer. Committed messages are preserved.
  static SPSCQueue claim(const std::filesystem::path& path,
                         ClaimResult* result_out = nullptr) {
    if (!std::filesystem::exists(path)) {
      if (result_out) *result_out = ClaimResult::kCreated;
      return SPSCQueue(detail::create_mapping(path, Capacity * sizeof(T), kFlagFixedSPSC));
    }

    auto mapping = detail::open_mapping(path, kFlagFixedSPSC);
    if (mapping.capacity != Capacity * sizeof(T)) {
      throw std::runtime_error(
          "SPSCQueue::claim capacity mismatch: file has " + std::to_string(mapping.capacity) +
          " bytes, expected " + std::to_string(Capacity * sizeof(T)));
    }

    if (result_out) *result_out = ClaimResult::kResumed;
    return SPSCQueue(std::move(mapping));
  }

  SPSCQueue(SPSCQueue&& other) noexcept = default;
  SPSCQueue& operator=(SPSCQueue&& other) noexcept = default;
  SPSCQueue(const SPSCQueue&) = delete;
  SPSCQueue& operator=(const SPSCQueue&) = delete;
  ~SPSCQueue() = default;

  // Producer only. Returns false if the queue is full.
  bool try_push(const T& value) noexcept {
    auto& tail = mapping_.control->tail_publish.value;
    auto& head = mapping_.control->head_publish.value;

    std::uint64_t t = tail.load(std::memory_order_relaxed);
    std::uint64_t h = head.load(std::memory_order_acquire);

    if (t - h >= Capacity) {
      return false;
    }

    std::byte* slot = mapping_.ring + (t & kMask) * sizeof(T);
    std::memcpy(slot, &value, sizeof(T));

    tail.store(t + 1, std::memory_order_release);
    return true;
  }

  // Consumer only. Returns false if the queue is empty.
  bool try_pop(T& out) noexcept {
    auto& head = mapping_.control->head_publish.value;
    auto& tail = mapping_.control->tail_publish.value;

    std::uint64_t h = head.load(std::memory_order_relaxed);
    std::uint64_t t = tail.load(std::memory_order_acquire);

    if (h == t) {
      return false;
    }

    const std::byte* slot = mapping_.ring + (h & kMask) * sizeof(T);
    std::memcpy(&out, slot, sizeof(T));

    head.store(h + 1, std::memory_order_release);
    return true;
  }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

  std::size_t size() const noexcept {
    std::uint64_t t = mapping_.control->tail_publish.value.load(std::memory_order_acquire);
    std::uint64_t h = mapping_.control->head_publish.value.load(std::memory_order_acquire);
    return static_cast<std::size_t>(t - h);
  }

  bool empty() const noexcept {
    return size() == 0;
  }

private:
  explicit SPSCQueue(detail::Mapping mapping) : mapping_(std::move(mapping)) {}

  detail::Mapping mapping_{};
};

} // namespace lfring
