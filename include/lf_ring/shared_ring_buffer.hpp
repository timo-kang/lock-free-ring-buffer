#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace lfring {

#if defined(__cpp_lib_hardware_interference_size)
constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kCacheLineSize = 64;
#endif

constexpr std::size_t kAlignment = 8;
constexpr std::uint32_t kMagic = 0x4C465242; // 'LFRB'
constexpr std::uint16_t kVersion = 2;
constexpr std::uint16_t kWrapFlag = 0x1;

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
};

static_assert(sizeof(RecordHeader) == 8, "RecordHeader must be 8 bytes");

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
  PaddedAtomicU64 head;
  PaddedAtomicU64 tail_reserve;
  PaddedAtomicU64 tail_publish;
};

class SharedRingBuffer {
public:
  static SharedRingBuffer create(const std::filesystem::path& path, std::size_t capacity_bytes);
  static SharedRingBuffer open(const std::filesystem::path& path);

  SharedRingBuffer(SharedRingBuffer&& other) noexcept;
  SharedRingBuffer& operator=(SharedRingBuffer&& other) noexcept;
  SharedRingBuffer(const SharedRingBuffer&) = delete;
  SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;
  ~SharedRingBuffer();

  bool try_push(const void* data, std::uint32_t size, std::uint16_t type);
  bool try_push(std::span<const std::byte> data, std::uint16_t type) {
    return try_push(data.data(), static_cast<std::uint32_t>(data.size()), type);
  }

  bool try_pop(std::vector<std::byte>& out, std::uint16_t& type);

  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t approx_size() const noexcept;

  static std::size_t required_mapping_size(std::size_t capacity_bytes);

private:
  SharedRingBuffer(int fd, void* mapping, std::size_t mapping_size, Header* header,
                   ControlBlock* control, std::byte* ring, std::size_t capacity);

  bool try_pop_internal(std::vector<std::byte>& out, std::uint16_t& type);

  int fd_ = -1;
  void* mapping_ = nullptr;
  std::size_t mapping_size_ = 0;
  Header* header_ = nullptr;
  ControlBlock* control_ = nullptr;
  std::byte* ring_ = nullptr;
  std::size_t capacity_ = 0;
};

} // namespace lfring
