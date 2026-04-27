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

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lfring {

#if defined(__cpp_lib_hardware_interference_size)
constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kCacheLineSize = 64;
#endif

constexpr std::size_t kAlignment = 8;
constexpr std::uint32_t kMagic = 0x4C465242; // 'LFRB'
constexpr std::uint16_t kVersion = 4;
constexpr std::uint16_t kWrapFlag = 0x1;
constexpr std::uint64_t kFlagMPSC = 0x1;
constexpr std::uint64_t kFlagMPMC = 0x2;
constexpr std::uint64_t kFlagSPSC = 0x4;
constexpr std::uint64_t kFlagLatest = 0x8;
constexpr std::uint64_t kFlagFixedSPSC = 0x10;

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

struct Header {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t header_size;
  std::uint64_t capacity_bytes;
  std::uint64_t control_offset;
  std::uint64_t ring_offset;
  std::uint64_t alignment;
  std::uint64_t flags;
};

struct RecordHeader {
  std::uint32_t size;
  std::uint16_t type;
  std::uint16_t flags;
  std::uint64_t sequence;
};

static_assert(sizeof(RecordHeader) == 16, "RecordHeader must be 16 bytes");

static_assert(kCacheLineSize >= sizeof(std::atomic<std::uint64_t>), "Cache line too small");

template <std::size_t N>
struct PaddingBytes {
  std::byte bytes[N];
};

template <>
struct PaddingBytes<0> {};

struct alignas(kCacheLineSize) PaddedAtomicU64 {
  std::atomic<std::uint64_t> value;
  PaddingBytes<kCacheLineSize - sizeof(std::atomic<std::uint64_t>)> padding;
};

// Field usage by type:
//   MPSC/MPMC:    head_reserve, head_publish, tail_reserve, tail_publish, sequence
//   SPSC (var):   head_reserve, head_publish, tail_reserve, tail_publish, sequence
//   SPSCQueue:    head_publish, tail_publish
//   SharedLatest: sequence (seqlock), head_reserve (heartbeat_ns),
//                 tail_publish (futex notification counter for write_and_notify/wait_for_update)
struct ControlBlock {
  PaddedAtomicU64 head_reserve;
  PaddedAtomicU64 head_publish;
  PaddedAtomicU64 tail_reserve;
  PaddedAtomicU64 tail_publish;
  PaddedAtomicU64 sequence;
};

inline std::size_t header_region_size() {
  return align_up(sizeof(Header), kCacheLineSize);
}

inline std::size_t control_region_size() {
  return align_up(sizeof(ControlBlock), kCacheLineSize);
}

inline std::size_t required_mapping_size(std::size_t capacity_bytes) {
  std::size_t cap = align_up(capacity_bytes, kAlignment);
  return header_region_size() + control_region_size() + cap;
}

// Result of a claim() operation on any shared-memory primitive.
enum class ClaimResult : std::uint8_t {
  kCreated,    // new shared memory file was created
  kResumed,    // existing file, previous writer exited cleanly
  kRecovered,  // existing file, recovered from dead writer
};

} // namespace lfring
