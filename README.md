# lock-free-ring-buf

A C++20 lock-free ring buffer for shared-memory messaging. It supports **MPSC** and **MPMC** (multi‑producer, multi‑consumer) with variable‑size payloads and a memory‑mapped file backend. It is designed to minimize false sharing with cache‑line padded control fields.

## Features
- MPSC lock-free ring buffer (shared memory via `mmap`).
- MPMC lock-free ring buffer (shared memory via `mmap`).
- Variable-size payloads with a small record header (`size`, `type`, `flags`).
- Cache-line padded control block to reduce false sharing.
- Unit tests (GoogleTest) and benchmarks (Google Benchmark).

## Status / Roadmap
- MPMC support: **implemented** (separate class).
- Typed message helpers / serialization helpers: **planned**.
- Windows shared-memory backend: **planned**.

## Build
```bash
cmake -S . -B build
cmake --build build
```

## Usage (MPSC)
```cpp
#include "lf_ring/shared_ring_buffer.hpp"
#include <cstdint>
#include <vector>

int main() {
  auto ring = lfring::SharedRingBuffer::create("/tmp/lfring.bin", 1 << 20);

  const char payload[] = "hello";
  ring.try_push(payload, sizeof(payload), /*type=*/1);

  std::vector<std::byte> out;
  std::uint16_t type = 0;
  if (ring.try_pop(out, type)) {
    // process message
  }
}
```

## Usage (MPMC)
```cpp
#include "lf_ring/shared_ring_buffer_mpmc.hpp"
#include <cstdint>
#include <vector>

int main() {
  auto ring = lfring::SharedRingBufferMPMC::create("/tmp/lfring_mpmc.bin", 1 << 20);

  const char payload[] = "hello";
  ring.try_push(payload, sizeof(payload), /*type=*/1);

  std::vector<std::byte> out;
  std::uint16_t type = 0;
  if (ring.try_pop(out, type)) {
    // process message
  }
}
```

## Tests
```bash
ctest --test-dir build
```

## Benchmarks
```bash
./build/lf_ring_bench --benchmark_format=console
```

## Notes
- Linux/POSIX backend only (`mmap`, `open`, `ftruncate`).
- Requires lock‑free 64‑bit atomics.
- MPSC only: a single consumer thread/process must call `try_pop`.
