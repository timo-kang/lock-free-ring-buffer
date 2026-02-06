#pragma once

#include "lf_ring/ring_layout.hpp"

#include <cstddef>
#include <filesystem>

namespace lfring::detail {

struct Mapping {
  int fd = -1;
  void* mapping = nullptr;
  std::size_t mapping_size = 0;
  Header* header = nullptr;
  ControlBlock* control = nullptr;
  std::byte* ring = nullptr;
  std::size_t capacity = 0;

  Mapping() = default;
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  Mapping(Mapping&& other) noexcept;
  Mapping& operator=(Mapping&& other) noexcept;
  ~Mapping();

  void reset();
};

Mapping create_mapping(const std::filesystem::path& path, std::size_t capacity_bytes, std::uint64_t flags);
Mapping open_mapping(const std::filesystem::path& path, std::uint64_t expected_flags);

} // namespace lfring::detail
