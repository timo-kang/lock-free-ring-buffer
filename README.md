# lock-free-ring-buf

A C++20 lock-free ring buffer for shared-memory messaging. It supports **MPSC** and **MPMC** (multi‑producer, multi‑consumer) with variable‑size payloads and a memory‑mapped file backend. It is designed to minimize false sharing with cache‑line padded control fields.

## Features
- MPSC lock-free ring buffer (shared memory via `mmap`).
- MPMC lock-free ring buffer (shared memory via `mmap`).
- `SharedLatest<T>` seqlock-style "latest value" for SWMR (single writer, many readers).
- Variable-size payloads with a small record header (`size`, `type`, `flags`).
- Global monotonic sequence number per record (for loss detection / diagnostics).
- Cache-line padded control block to reduce false sharing.
- Unit tests (GoogleTest) and benchmarks (Google Benchmark).

## Status / Roadmap
- MPMC support: **implemented** (separate class).
- Typed message helpers / serialization helpers: **implemented**.
- SPSC FIFO (dedicated implementation): **in progress**.
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

## Typed Messages
Typed helpers wrap the raw byte API. Define a message type ID and use the writer/reader:
```cpp
#include "lf_ring/shared_ring_buffer.hpp"
#include "lf_ring/typed_message.hpp"

struct PriceUpdate {
  uint32_t id;
  double price;
};

namespace lfring {
template <>
struct MessageType<PriceUpdate> {
  static constexpr bool defined = true;
  static constexpr uint16_t value = 42;
};
} // namespace lfring

int main() {
  auto ring = lfring::SharedRingBuffer::create("/tmp/lfring.bin", 1 << 20);
  lfring::TypedMessageWriter<lfring::SharedRingBuffer> writer(ring);
  lfring::TypedMessageReader<lfring::SharedRingBuffer> reader(ring);

  writer.try_push(PriceUpdate{7, 99.5});

  PriceUpdate out{};
  reader.try_pop(out);
}
```

## Latest-Value Topics (SharedLatest)
For robotics style "state/latest" topics (robot state, telemetry, latest command), FIFO is often the wrong semantic.
`SharedLatest<T>` provides SWMR latest-value semantics with a seqlock-style counter so readers do not observe torn reads.
```cpp
#include "lf_ring/shared_latest.hpp"

struct RobotState {
  double x;
  double y;
  double yaw;
  uint64_t stamp_ns;
};

int main() {
  auto latest = lfring::SharedLatest<RobotState>::create("/tmp/robot_state.bin");
  latest.write(RobotState{1.0, 2.0, 0.5, 123});

  RobotState out{};
  if (latest.try_read(out)) {
    // out is a consistent snapshot
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
- SharedLatest is SWMR only (one writer; many readers). If the writer dies mid-write, readers may spin unless you cap retries.
