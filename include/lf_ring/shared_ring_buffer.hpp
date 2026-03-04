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
