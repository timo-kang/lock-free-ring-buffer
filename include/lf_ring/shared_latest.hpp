#pragma once

#include "lf_ring/ring_layout.hpp"
#include "lf_ring/shared_memory_mapping.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <type_traits>

namespace lfring {

// SharedLatest is a shared-memory "latest value" primitive for SWMR (single writer, many readers).
// It uses a seqlock-style sequence counter: writer flips seq odd/even around the memcpy.
//
// Properties:
// - Readers never see torn payloads (they retry until they observe a stable even seq).
// - Not FIFO; overwrites old values.
// - Not crash-fault-tolerant: if the writer dies mid-write, seq may remain odd and readers will retry forever.
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

  // Single-writer only.
  void write(const T& value) noexcept {
    auto& seq = mapping_.control->sequence.value;

    // Begin write (odd).
    std::uint64_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_release);

    std::memcpy(mapping_.ring, &value, sizeof(T));

    // Commit (even).
    seq.store(s + 2, std::memory_order_release);
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

  // Returns the seqlock counter; even values correspond to a fully committed write.
  std::uint64_t raw_sequence() const noexcept {
    return mapping_.control->sequence.value.load(std::memory_order_acquire);
  }

  static std::size_t required_mapping_size() {
    return lfring::required_mapping_size(sizeof(T));
  }

private:
  explicit SharedLatest(detail::Mapping mapping) : mapping_(std::move(mapping)) {}

  detail::Mapping mapping_{};
};

} // namespace lfring

