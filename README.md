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
- SPSC FIFO (dedicated implementation): **implemented** (`SPSCQueue<T, Capacity>`).
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

## SPSC Queue (SPSCQueue)
A fixed-size, typed single-producer single-consumer queue backed by shared memory:
```cpp
#include "lf_ring/spsc_queue.hpp"

int main() {
  auto q = lfring::SPSCQueue<std::uint64_t, 1024>::create("/tmp/spsc.bin");

  q.try_push(42u);

  std::uint64_t out = 0;
  if (q.try_pop(out)) {
    // out == 42
  }
}
```

## Latest-Value Topics (SharedLatest)
For robotics style "state/latest" topics (robot state, telemetry, latest command), FIFO is often the wrong semantic.
`SharedLatest<T>` provides SWMR latest-value semantics with a seqlock-style counter so readers do not observe torn reads.

The `ReadResult` overload of `try_read` can detect writer death — if the writer process dies mid-write, readers get `kWriterDead` instead of silently spinning:
```cpp
#include "lf_ring/shared_latest.hpp"
using namespace std::chrono_literals;

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

  // Simple read (backward compatible):
  if (latest.try_read(out)) {
    // out is a consistent snapshot
  }

  // Rich-status read with writer-death detection:
  auto result = latest.try_read(out, /*dead_threshold=*/100ms);
  switch (result) {
    case lfring::ReadResult::kSuccess:    /* consistent snapshot */ break;
    case lfring::ReadResult::kEmpty:      /* no data written yet */ break;
    case lfring::ReadResult::kContended:  /* writer is mid-write, still alive */ break;
    case lfring::ReadResult::kWriterDead: /* writer died mid-write */ break;
  }

  // Check liveness independently:
  if (!latest.is_writer_alive(100ms)) {
    // writer hasn't written in 100ms
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
- SharedLatest is SWMR only (one writer; many readers). Use the `ReadResult` overload of `try_read` to detect writer death via heartbeat checking.
