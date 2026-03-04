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

// Adapter layer that wraps lf_ring primitives with a publish/subscribe
// interface. This enables gradual, per-topic migration from mutex-based
// SharedMemory transports to lock-free alternatives without changing
// application code.
//
// Provided adapters:
//
//   SharedLatestPublisher<T>  — wraps SharedLatest<T>::write()
//   SharedLatestSubscriber<T> — wraps SharedLatest<T>::try_read() in a polling thread
//   SpscPublisher<T,N>        — wraps SPSCQueue<T,N>::try_push()
//   SpscSubscriber<T,N>       — wraps SPSCQueue<T,N>::try_pop() in a polling thread
//   SpscBytePublisher         — wraps SharedRingBufferSPSC::try_push()
//   SpscByteSubscriber        — wraps SharedRingBufferSPSC::try_pop() in a polling thread
//
// Topic naming:
//   Shared memory files are placed under /dev/shm (via std::filesystem::temp_directory_path
//   or an explicit base path) with the pattern:  <prefix>___<topic>
//
// Threading model:
//   Publishers are synchronous (write/push returns immediately).
//   Subscribers run a background polling thread that invokes a user callback
//   on each new message. The poll interval is configurable.

#include "lf_ring/shared_latest.hpp"
#include "lf_ring/shared_ring_buffer_spsc.hpp"
#include "lf_ring/spsc_queue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lfring {

// ── Topic path helpers ───────────────────────────────────────────────

namespace detail {

inline std::filesystem::path topic_path(const std::filesystem::path& base_dir,
                                        const std::string& prefix,
                                        const std::string& topic) {
  // Replace '/' in topic names with '___' for filesystem compatibility.
  std::string sanitized = topic;
  for (auto& c : sanitized) {
    if (c == '/') c = '_';
  }
  return base_dir / (prefix + "___" + sanitized);
}

inline std::filesystem::path default_shm_dir() {
  // On Linux, /dev/shm is the standard tmpfs mount for POSIX shared memory.
  std::filesystem::path shm("/dev/shm");
  if (std::filesystem::exists(shm)) {
    return shm;
  }
  return std::filesystem::temp_directory_path();
}

} // namespace detail

// ── SharedLatestPublisher ────────────────────────────────────────────
//
// Wraps SharedLatest<T>::create() + write(). Single writer per topic.
//
// Usage:
//   SharedLatestPublisher<Command> pub("command");
//   pub.publish(cmd);

template <class T>
class SharedLatestPublisher {
public:
  explicit SharedLatestPublisher(const std::string& topic,
                                 const std::string& prefix = "lf",
                                 const std::filesystem::path& base_dir = detail::default_shm_dir())
      : path_(detail::topic_path(base_dir, prefix, topic)),
        latest_(SharedLatest<T>::create(path_)) {}

  void publish(const T& value) noexcept {
    latest_.write(value);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

  // Expose raw SharedLatest for advanced use (e.g. is_writer_alive).
  SharedLatest<T>& raw() noexcept { return latest_; }
  const SharedLatest<T>& raw() const noexcept { return latest_; }

private:
  std::filesystem::path path_;
  SharedLatest<T> latest_;
};

// ── SharedLatestSubscriber ───────────────────────────────────────────
//
// Wraps SharedLatest<T>::open() + try_read() in a background polling thread.
// Invokes a user callback whenever a new value is read (detected via sequence
// number change). Optionally invokes a death callback when the writer dies.
//
// Usage:
//   SharedLatestSubscriber<Command> sub("command",
//       [](const Command& cmd) { /* handle */ },
//       [](ReadResult r) { /* writer died */ });
//   sub.start();       // begins polling
//   sub.stop();        // or destructor stops automatically

template <class T>
class SharedLatestSubscriber {
public:
  using Callback = std::function<void(const T&)>;
  using DeathCallback = std::function<void(ReadResult)>;

  SharedLatestSubscriber(const std::string& topic,
                         Callback on_message,
                         DeathCallback on_death = nullptr,
                         const std::string& prefix = "lf",
                         const std::filesystem::path& base_dir = detail::default_shm_dir())
      : path_(detail::topic_path(base_dir, prefix, topic)),
        latest_(SharedLatest<T>::open(path_)),
        on_message_(std::move(on_message)),
        on_death_(std::move(on_death)) {}

  ~SharedLatestSubscriber() { stop(); }

  SharedLatestSubscriber(const SharedLatestSubscriber&) = delete;
  SharedLatestSubscriber& operator=(const SharedLatestSubscriber&) = delete;
  SharedLatestSubscriber(SharedLatestSubscriber&&) = delete;
  SharedLatestSubscriber& operator=(SharedLatestSubscriber&&) = delete;

  // Start the polling thread.
  void start(std::chrono::microseconds poll_interval = std::chrono::microseconds{100},
             std::chrono::nanoseconds dead_threshold = std::chrono::milliseconds{500}) {
    if (running_.load(std::memory_order_relaxed)) return;
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this, poll_interval, dead_threshold] {
      poll_loop(poll_interval, dead_threshold);
    });
  }

  // Stop the polling thread (blocks until joined).
  void stop() {
    if (!running_.load(std::memory_order_relaxed)) return;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      running_.store(false, std::memory_order_relaxed);
    }
    cv_.notify_one();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

  // Attempt a single synchronous read (no thread needed).
  ReadResult try_read_once(T& out,
                           std::chrono::nanoseconds dead_threshold = std::chrono::milliseconds{500}) const {
    return latest_.try_read(out, dead_threshold);
  }

  bool is_writer_alive(std::chrono::nanoseconds threshold = std::chrono::milliseconds{500}) const noexcept {
    return latest_.is_writer_alive(threshold);
  }

  const std::filesystem::path& path() const noexcept { return path_; }
  const SharedLatest<T>& raw() const noexcept { return latest_; }

private:
  void poll_loop(std::chrono::microseconds poll_interval,
                 std::chrono::nanoseconds dead_threshold) {
    std::uint64_t last_seq = 0;
    T value{};

    while (running_.load(std::memory_order_relaxed)) {
      auto result = latest_.try_read(value, dead_threshold);

      switch (result) {
        case ReadResult::kSuccess: {
          std::uint64_t seq = latest_.raw_sequence();
          if (seq != last_seq) {
            last_seq = seq;
            if (on_message_) on_message_(value);
          }
          break;
        }
        case ReadResult::kWriterDead:
          if (on_death_) on_death_(result);
          break;
        case ReadResult::kEmpty:
        case ReadResult::kContended:
          break;
      }

      // Sleep using condition_variable to allow fast wakeup on stop().
      std::unique_lock<std::mutex> lk(mutex_);
      cv_.wait_for(lk, poll_interval, [this] {
        return !running_.load(std::memory_order_relaxed);
      });
    }
  }

  std::filesystem::path path_;
  SharedLatest<T> latest_;
  Callback on_message_;
  DeathCallback on_death_;

  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

// ── SpscPublisher ────────────────────────────────────────────────────
//
// Wraps SPSCQueue<T, Capacity>::create() + try_push(). Single producer.

template <class T, std::size_t Capacity>
class SpscPublisher {
public:
  explicit SpscPublisher(const std::string& topic,
                         const std::string& prefix = "lf",
                         const std::filesystem::path& base_dir = detail::default_shm_dir())
      : path_(detail::topic_path(base_dir, prefix, topic)),
        queue_(SPSCQueue<T, Capacity>::create(path_)) {}

  // Returns false if the queue is full.
  bool publish(const T& value) noexcept {
    return queue_.try_push(value);
  }

  const std::filesystem::path& path() const noexcept { return path_; }
  SPSCQueue<T, Capacity>& raw() noexcept { return queue_; }

private:
  std::filesystem::path path_;
  SPSCQueue<T, Capacity> queue_;
};

// ── SpscSubscriber ───────────────────────────────────────────────────
//
// Wraps SPSCQueue<T, Capacity>::open() + try_pop() in a polling thread.
// Single consumer. Invokes callback for every message (FIFO ordering preserved).

template <class T, std::size_t Capacity>
class SpscSubscriber {
public:
  using Callback = std::function<void(const T&)>;

  SpscSubscriber(const std::string& topic,
                 Callback on_message,
                 const std::string& prefix = "lf",
                 const std::filesystem::path& base_dir = detail::default_shm_dir())
      : path_(detail::topic_path(base_dir, prefix, topic)),
        queue_(SPSCQueue<T, Capacity>::open(path_)),
        on_message_(std::move(on_message)) {}

  ~SpscSubscriber() { stop(); }

  SpscSubscriber(const SpscSubscriber&) = delete;
  SpscSubscriber& operator=(const SpscSubscriber&) = delete;
  SpscSubscriber(SpscSubscriber&&) = delete;
  SpscSubscriber& operator=(SpscSubscriber&&) = delete;

  void start(std::chrono::microseconds poll_interval = std::chrono::microseconds{100}) {
    if (running_.load(std::memory_order_relaxed)) return;
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this, poll_interval] { poll_loop(poll_interval); });
  }

  void stop() {
    if (!running_.load(std::memory_order_relaxed)) return;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      running_.store(false, std::memory_order_relaxed);
    }
    cv_.notify_one();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

  const std::filesystem::path& path() const noexcept { return path_; }
  SPSCQueue<T, Capacity>& raw() noexcept { return queue_; }

private:
  void poll_loop(std::chrono::microseconds poll_interval) {
    T value{};
    while (running_.load(std::memory_order_relaxed)) {
      // Drain all available messages before sleeping.
      bool got_any = false;
      while (queue_.try_pop(value)) {
        if (on_message_) on_message_(value);
        got_any = true;
      }

      if (!got_any) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait_for(lk, poll_interval, [this] {
          return !running_.load(std::memory_order_relaxed);
        });
      }
    }

    // Final drain on shutdown.
    T value_drain{};
    while (queue_.try_pop(value_drain)) {
      if (on_message_) on_message_(value_drain);
    }
  }

  std::filesystem::path path_;
  SPSCQueue<T, Capacity> queue_;
  Callback on_message_;

  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

// ── SpscBytePublisher ────────────────────────────────────────────────
//
// Wraps SharedRingBufferSPSC::create() + try_push() for variable-size payloads.
// Suitable for audio data streaming.

class SpscBytePublisher {
public:
  explicit SpscBytePublisher(const std::string& topic,
                             std::size_t capacity_bytes,
                             const std::string& prefix = "lf",
                             const std::filesystem::path& base_dir = detail::default_shm_dir())
      : path_(detail::topic_path(base_dir, prefix, topic)),
        ring_(SharedRingBufferSPSC::create(path_, capacity_bytes)) {}

  // Returns false if the ring buffer is full.
  bool publish(const void* data, std::uint32_t size, std::uint16_t type = 0) {
    return ring_.try_push(data, size, type);
  }

  bool publish(std::span<const std::byte> data, std::uint16_t type = 0) {
    return ring_.try_push(data, type);
  }

  const std::filesystem::path& path() const noexcept { return path_; }
  SharedRingBufferSPSC& raw() noexcept { return ring_; }

private:
  std::filesystem::path path_;
  SharedRingBufferSPSC ring_;
};

// ── SpscByteSubscriber ───────────────────────────────────────────────
//
// Wraps SharedRingBufferSPSC::open() + try_pop() in a polling thread.
// Suitable for consuming variable-size payloads (e.g. audio chunks).

class SpscByteSubscriber {
public:
  using Callback = std::function<void(std::span<const std::byte> data, std::uint16_t type)>;

  SpscByteSubscriber(const std::string& topic,
                     Callback on_message,
                     const std::string& prefix = "lf",
                     const std::filesystem::path& base_dir = detail::default_shm_dir())
      : path_(detail::topic_path(base_dir, prefix, topic)),
        ring_(SharedRingBufferSPSC::open(path_)),
        on_message_(std::move(on_message)) {}

  ~SpscByteSubscriber() { stop(); }

  SpscByteSubscriber(const SpscByteSubscriber&) = delete;
  SpscByteSubscriber& operator=(const SpscByteSubscriber&) = delete;
  SpscByteSubscriber(SpscByteSubscriber&&) = delete;
  SpscByteSubscriber& operator=(SpscByteSubscriber&&) = delete;

  void start(std::chrono::microseconds poll_interval = std::chrono::microseconds{100}) {
    if (running_.load(std::memory_order_relaxed)) return;
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this, poll_interval] { poll_loop(poll_interval); });
  }

  void stop() {
    if (!running_.load(std::memory_order_relaxed)) return;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      running_.store(false, std::memory_order_relaxed);
    }
    cv_.notify_one();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

  const std::filesystem::path& path() const noexcept { return path_; }

private:
  void poll_loop(std::chrono::microseconds poll_interval) {
    std::vector<std::byte> buf;
    std::uint16_t type = 0;

    while (running_.load(std::memory_order_relaxed)) {
      bool got_any = false;
      while (ring_.try_pop(buf, type)) {
        if (on_message_) {
          on_message_(std::span<const std::byte>(buf.data(), buf.size()), type);
        }
        got_any = true;
      }

      if (!got_any) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait_for(lk, poll_interval, [this] {
          return !running_.load(std::memory_order_relaxed);
        });
      }
    }

    // Final drain.
    while (ring_.try_pop(buf, type)) {
      if (on_message_) {
        on_message_(std::span<const std::byte>(buf.data(), buf.size()), type);
      }
    }
  }

  std::filesystem::path path_;
  SharedRingBufferSPSC ring_;
  Callback on_message_;

  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

} // namespace lfring
