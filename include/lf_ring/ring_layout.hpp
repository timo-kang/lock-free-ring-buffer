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

} // namespace lfring
