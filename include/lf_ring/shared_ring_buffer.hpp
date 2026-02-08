#pragma once

#include "lf_ring/ring_layout.hpp"
#include "lf_ring/shared_memory_mapping.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace lfring {

class SharedRingBuffer {
public:
  static SharedRingBuffer create(const std::filesystem::path& path, std::size_t capacity_bytes);
  static SharedRingBuffer open(const std::filesystem::path& path);

  SharedRingBuffer(SharedRingBuffer&& other) noexcept;
  SharedRingBuffer& operator=(SharedRingBuffer&& other) noexcept;
  SharedRingBuffer(const SharedRingBuffer&) = delete;
  SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;
  ~SharedRingBuffer() = default;

  bool try_push(const void* data, std::uint32_t size, std::uint16_t type);
  bool try_push(std::span<const std::byte> data, std::uint16_t type) {
    return try_push(data.data(), static_cast<std::uint32_t>(data.size()), type);
  }

  bool try_pop(std::vector<std::byte>& out, std::uint16_t& type);
  bool try_pop(std::vector<std::byte>& out, std::uint16_t& type, std::uint64_t& sequence);

  std::size_t capacity() const noexcept { return mapping_.capacity; }
  std::size_t approx_size() const noexcept;

  static std::size_t required_mapping_size(std::size_t capacity_bytes) {
    return lfring::required_mapping_size(capacity_bytes);
  }

private:
  explicit SharedRingBuffer(detail::Mapping mapping) : mapping_(std::move(mapping)) {}

  bool try_pop_internal(std::vector<std::byte>& out, std::uint16_t& type, std::uint64_t* sequence);

  detail::Mapping mapping_{};
};

} // namespace lfring
