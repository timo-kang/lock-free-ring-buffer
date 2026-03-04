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

// Linux futex helpers for SharedLatest notification.
//
// The futex syscall operates on a 32-bit word in shared memory. We use the
// lower 32 bits of a std::atomic<uint64_t> (which on little-endian x86 is at
// the base address of the atomic) as the futex word.
//
// This enables efficient blocking: the reader calls futex_wait() which
// atomically checks if the value changed and blocks in the kernel if not.
// Zero CPU burn while waiting.

#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstdint>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace lfring {
namespace detail {

// Extract a pointer to the lower 32 bits of an atomic<uint64_t>.
// On little-endian (x86/ARM), this is the base address.
// Futex operates on 32-bit aligned words.
inline std::uint32_t* futex_word(std::atomic<std::uint64_t>& a) noexcept {
  return reinterpret_cast<std::uint32_t*>(&a);
}

inline const std::uint32_t* futex_word(const std::atomic<std::uint64_t>& a) noexcept {
  return reinterpret_cast<const std::uint32_t*>(&a);
}

// Read the current 32-bit futex value from an atomic<uint64_t>.
inline std::uint32_t futex_load(const std::atomic<std::uint64_t>& a,
                                std::memory_order order = std::memory_order_acquire) noexcept {
  return static_cast<std::uint32_t>(a.load(order));
}

// Wake all threads waiting on this futex word.
// Returns the number of waiters woken.
inline int futex_wake_all(std::atomic<std::uint64_t>& a) noexcept {
  return static_cast<int>(
      ::syscall(SYS_futex, futex_word(a), FUTEX_WAKE, INT_MAX,
                nullptr, nullptr, 0));
}

// Block until the 32-bit futex word differs from `expected`, or timeout.
//
// Returns:
//   true  — woken (value changed or spurious wakeup)
//   false — timed out with value unchanged
inline bool futex_wait_timeout(std::atomic<std::uint64_t>& a,
                               std::uint32_t expected,
                               std::chrono::milliseconds timeout) noexcept {
  struct timespec ts{};
  ts.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  ts.tv_nsec = static_cast<long>((timeout.count() % 1000) * 1000000L);

  int ret = static_cast<int>(
      ::syscall(SYS_futex, futex_word(a), FUTEX_WAIT, expected,
                &ts, nullptr, 0));

  if (ret == 0) {
    return true;  // woken by FUTEX_WAKE
  }

  // EAGAIN: value already changed before we blocked (good — new data ready)
  // EINTR:  interrupted by signal (treat as spurious wakeup)
  if (errno == EAGAIN || errno == EINTR) {
    return true;
  }

  // ETIMEDOUT: no wakeup within timeout
  return false;
}

} // namespace detail
} // namespace lfring
