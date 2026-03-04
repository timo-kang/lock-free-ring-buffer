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

#include "lf_ring/futex.hpp"
#include "lf_ring/ring_layout.hpp"
#include "lf_ring/shared_memory_mapping.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <type_traits>

namespace lfring {

// Result of a SharedLatest read with writer-death detection.
enum class ReadResult : std::uint8_t {
  kSuccess,    // consistent snapshot read
  kEmpty,      // no data ever written (seq == 0)
  kContended,  // writer is mid-write, retries exhausted but heartbeat is fresh
  kWriterDead, // writer appears dead (seq odd + stale heartbeat)
};

// SharedLatest is a shared-memory "latest value" primitive for SWMR (single writer, many readers).
// It uses a seqlock-style sequence counter: writer flips seq odd/even around the memcpy.
//
// Properties:
// - Readers never see torn payloads (they retry until they observe a stable even seq).
// - Not FIFO; overwrites old values.
// - Writer-death detection: the new try_read overload with dead_threshold checks a heartbeat
//   timestamp (stored in head_reserve) to distinguish "writer is mid-write" from "writer is dead."
template <class T>
class SharedLatest {
  static_assert(std::is_trivially_copyable_v<T>, "SharedLatest<T> requires trivially copyable T");

public:
  static SharedLatest create(const std::filesystem::path& path) {
    return SharedLatest(detail::create_mapping(path, sizeof(T), kFlagLatest));
  }

  static SharedLatest open(const std::filesystem::path& path) {
    return SharedLatest(detail::open_mapping(path, kFlagLatest));
  }

  SharedLatest(SharedLatest&& other) noexcept = default;
  SharedLatest& operator=(SharedLatest&& other) noexcept = default;
  SharedLatest(const SharedLatest&) = delete;
  SharedLatest& operator=(const SharedLatest&) = delete;
  ~SharedLatest() = default;

  // Single-writer only. Does NOT wake waiters (use write_and_notify for that).
  void write(const T& value) noexcept {
    auto& seq = mapping_.control->sequence.value;

    // Begin write (odd).
    std::uint64_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_release);

    std::memcpy(mapping_.ring, &value, sizeof(T));

    // Commit (even).
    seq.store(s + 2, std::memory_order_release);

    // Update heartbeat after successful write.
    heartbeat().store(now_ns(), std::memory_order_release);
  }

  // Single-writer only. Writes the value AND wakes any futex waiters.
  // Use this when readers block via wait_for_update().
  void write_and_notify(const T& value) noexcept {
    write(value);

    // Increment the notification counter (lower 32 bits used by futex).
    notify_word().fetch_add(1, std::memory_order_release);
    detail::futex_wake_all(notify_word());
  }

  // Block until the notification counter changes from `last_seen`, or timeout.
  // Returns true if woken (new data likely available), false on timeout.
  bool wait_for_update(std::uint32_t last_seen,
                       std::chrono::milliseconds timeout) const noexcept {
    // const_cast is safe: futex_wait only reads the word, and the underlying
    // shared memory is mutable (mmap'd with PROT_READ|PROT_WRITE).
    return detail::futex_wait_timeout(
        const_cast<std::atomic<std::uint64_t>&>(notify_word()),
        last_seen, timeout);
  }

  // Returns the current notification counter (lower 32 bits of tail_publish).
  // Use this to snapshot the counter before calling wait_for_update().
  std::uint32_t notify_counter() const noexcept {
    return detail::futex_load(notify_word());
  }

  // Attempts to read a consistent snapshot. Returns false if it fails to observe a stable
  // even sequence within max_retries (useful to avoid infinite loops if writer is dead).
  bool try_read(T& out, std::size_t max_retries = 1024) const noexcept {
    const auto& seq = mapping_.control->sequence.value;

    for (std::size_t i = 0; i < max_retries; ++i) {
      std::uint64_t s1 = seq.load(std::memory_order_acquire);
      if ((s1 & 1u) != 0u) {
        continue;
      }

      std::memcpy(&out, mapping_.ring, sizeof(T));

      std::atomic_thread_fence(std::memory_order_acquire);
      std::uint64_t s2 = seq.load(std::memory_order_acquire);
      if (s1 == s2) {
        return true;
      }
    }
    return false;
  }

  // Rich-status read with writer-death detection.
  // When the sequence is odd (writer mid-write), periodically checks a heartbeat timestamp
  // to determine if the writer is alive or dead.
  ReadResult try_read(T& out,
                      std::chrono::nanoseconds dead_threshold,
                      std::size_t max_retries = 1024) const noexcept {
    const auto& seq = mapping_.control->sequence.value;

    std::uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 == 0u) {
      return ReadResult::kEmpty;
    }

    for (std::size_t i = 0; i < max_retries; ++i) {
      s1 = seq.load(std::memory_order_acquire);
      if ((s1 & 1u) != 0u) {
        // Odd seq — writer is mid-write. Check heartbeat every 64 spins.
        if ((i & 0x3Fu) == 0x3Fu) {
          std::uint64_t hb = heartbeat().load(std::memory_order_acquire);
          std::uint64_t now = now_ns();
          if (hb != 0u && (now - hb) > static_cast<std::uint64_t>(dead_threshold.count())) {
            return ReadResult::kWriterDead;
          }
        }
        continue;
      }

      std::memcpy(&out, mapping_.ring, sizeof(T));

      std::atomic_thread_fence(std::memory_order_acquire);
      std::uint64_t s2 = seq.load(std::memory_order_acquire);
      if (s1 == s2) {
        return ReadResult::kSuccess;
      }
    }
    return ReadResult::kContended;
  }

  // Check writer liveness independently.
  bool is_writer_alive(std::chrono::nanoseconds threshold) const noexcept {
    std::uint64_t hb = heartbeat().load(std::memory_order_acquire);
    if (hb == 0u) {
      return false;
    }
    std::uint64_t now = now_ns();
    return (now - hb) < static_cast<std::uint64_t>(threshold.count());
  }

  // Returns the seqlock counter; even values correspond to a fully committed write.
  std::uint64_t raw_sequence() const noexcept {
    return mapping_.control->sequence.value.load(std::memory_order_acquire);
  }

  static std::size_t required_mapping_size() {
    return lfring::required_mapping_size(sizeof(T));
  }

private:
  explicit SharedLatest(detail::Mapping mapping) : mapping_(std::move(mapping)) {}

  static std::uint64_t now_ns() noexcept {
    auto tp = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(tp.time_since_epoch().count());
  }

  std::atomic<std::uint64_t>& heartbeat() const noexcept {
    return mapping_.control->head_reserve.value;
  }

  // Notification counter for futex wait/wake. Uses tail_publish which is
  // otherwise unused by SharedLatest (see ring_layout.hpp ControlBlock comment).
  std::atomic<std::uint64_t>& notify_word() const noexcept {
    return mapping_.control->tail_publish.value;
  }

  detail::Mapping mapping_{};
};

} // namespace lfring
