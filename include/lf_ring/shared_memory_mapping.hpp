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
