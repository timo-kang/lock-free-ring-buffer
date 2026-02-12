#include "lf_ring/shared_ring_buffer.hpp"
#include "lf_ring/shared_latest.hpp"
#include "lf_ring/shared_ring_buffer_spsc.hpp"
#include "lf_ring/spsc_queue.hpp"
#include "lf_ring/typed_message.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

std::filesystem::path make_temp_path(const char* prefix) {
  static std::atomic<unsigned long long> counter{0};
  auto id = counter.fetch_add(1, std::memory_order_relaxed);
  auto pid = static_cast<unsigned long long>(::getpid());
  auto name = std::string(prefix) + "_" + std::to_string(pid) + "_" + std::to_string(id);
  return std::filesystem::temp_directory_path() / name;
}

struct TempFile {
  std::filesystem::path path;
  explicit TempFile(const char* prefix) : path(make_temp_path(prefix)) {}
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

class LockedQueue {
public:
  void enqueue(const std::vector<std::byte>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(data);
  }

  bool dequeue(std::vector<std::byte>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop();
    return true;
  }

private:
  std::mutex mutex_;
  std::queue<std::vector<std::byte>> queue_;
};

struct Tick {
  std::uint64_t seq;
  double value;
};

struct RobotState {
  std::uint64_t tick;
  std::uint32_t id;
  std::uint32_t mode;
  double position[3];
  double velocity[3];
  float joints[32];
};

lfring::SharedRingBuffer& ring_small() {
  static TempFile tmp("lfring_bench_small");
  static lfring::SharedRingBuffer ring = lfring::SharedRingBuffer::create(tmp.path, 1 << 20);
  return ring;
}

lfring::SharedRingBuffer& ring_large() {
  static TempFile tmp("lfring_bench_large");
  static lfring::SharedRingBuffer ring = lfring::SharedRingBuffer::create(tmp.path, 4 << 20);
  return ring;
}

lfring::SharedRingBuffer& ring_typed_small() {
  static TempFile tmp("lfring_bench_typed_small");
  static lfring::SharedRingBuffer ring = lfring::SharedRingBuffer::create(tmp.path, 1 << 20);
  return ring;
}

lfring::SharedRingBuffer& ring_typed_string() {
  static TempFile tmp("lfring_bench_typed_string");
  static lfring::SharedRingBuffer ring = lfring::SharedRingBuffer::create(tmp.path, 2 << 20);
  return ring;
}

lfring::SharedRingBuffer& ring_typed_robot() {
  static TempFile tmp("lfring_bench_typed_robot");
  static lfring::SharedRingBuffer ring = lfring::SharedRingBuffer::create(tmp.path, 4 << 20);
  return ring;
}

lfring::SharedRingBufferSPSC& ring_spsc_small() {
  static TempFile tmp("lfring_bench_spsc_small");
  static lfring::SharedRingBufferSPSC ring = lfring::SharedRingBufferSPSC::create(tmp.path, 1 << 20);
  return ring;
}

lfring::SharedRingBufferSPSC& ring_typed_spsc_robot() {
  static TempFile tmp("lfring_bench_typed_spsc_robot");
  static lfring::SharedRingBufferSPSC ring = lfring::SharedRingBufferSPSC::create(tmp.path, 4 << 20);
  return ring;
}

lfring::SharedLatest<RobotState>& latest_robot() {
  static TempFile tmp("lfring_bench_latest_robot");
  static lfring::SharedLatest<RobotState> latest = lfring::SharedLatest<RobotState>::create(tmp.path);
  return latest;
}

LockedQueue& locked_queue() {
  static LockedQueue queue;
  return queue;
}

std::vector<std::byte> make_payload(std::size_t size) {
  std::vector<std::byte> data(size);
  std::memset(data.data(), 0x5A, data.size());
  return data;
}

inline void SetBenchmarkCounters(benchmark::State& state, std::int64_t total_items, int producers) {
  if (state.thread_index() != 0) {
    return;
  }
  state.SetItemsProcessed(total_items);
  if (producers > 0) {
    state.counters["items_per_producer"] = benchmark::Counter(
        static_cast<double>(total_items) / static_cast<double>(producers),
        benchmark::Counter::kIsRate);
  }
}

} // namespace

namespace lfring {

template <>
struct MessageType<Tick> {
  static constexpr bool defined = true;
  static constexpr std::uint16_t value = 77;
};

template <>
struct MessageType<RobotState> {
  static constexpr bool defined = true;
  static constexpr std::uint16_t value = 78;
};

} // namespace

namespace {

static void BM_Ring_MPSC_Small(benchmark::State& state) {
  auto& ring = ring_small();
  auto payload = make_payload(32);
  const int producers = static_cast<int>(state.threads()) - 1;

  if (state.thread_index() == 0) {
    std::vector<std::byte> out;
    std::uint16_t type = 0;
    for (auto _ : state) {
      for (int i = 0; i < producers; ++i) {
        while (!ring.try_pop(out, type)) {
          benchmark::DoNotOptimize(out.data());
        }
      }
    }
  } else {
    for (auto _ : state) {
      while (!ring.try_push(payload, 1)) {
        benchmark::DoNotOptimize(payload.data());
      }
    }
  }

  SetBenchmarkCounters(state, static_cast<std::int64_t>(state.iterations()) * producers, producers);
}

static void BM_Ring_SPSC_Small(benchmark::State& state) {
  auto& ring = ring_spsc_small();
  auto payload = make_payload(32);
  const int threads = static_cast<int>(state.threads());

  if (threads != 2) {
    state.SkipWithError("BM_Ring_SPSC_Small requires exactly 2 threads (1 producer, 1 consumer)");
    return;
  }

  if (state.thread_index() == 0) {
    std::vector<std::byte> out;
    std::uint16_t type = 0;
    for (auto _ : state) {
      while (!ring.try_pop(out, type)) {
        benchmark::DoNotOptimize(out.data());
      }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  } else {
    for (auto _ : state) {
      while (!ring.try_push(payload, 1)) {
        benchmark::DoNotOptimize(payload.data());
      }
    }
  }
}

static void BM_Typed_MPSC_Trivial(benchmark::State& state) {
  auto& ring = ring_typed_small();
  const int producers = static_cast<int>(state.threads()) - 1;
  const Tick tick{42, 123.5};

  if (state.thread_index() == 0) {
    lfring::TypedMessageReader<lfring::SharedRingBuffer> reader(ring);
    Tick out{};
    for (auto _ : state) {
      for (int i = 0; i < producers; ++i) {
        while (!reader.try_pop(out)) {
          benchmark::DoNotOptimize(out.seq);
          benchmark::DoNotOptimize(out.value);
        }
      }
    }
  } else {
    lfring::TypedMessageWriter<lfring::SharedRingBuffer> writer(ring);
    for (auto _ : state) {
      while (!writer.try_push(tick)) {
        benchmark::DoNotOptimize(tick.seq);
        benchmark::DoNotOptimize(tick.value);
      }
    }
  }

  SetBenchmarkCounters(state, static_cast<std::int64_t>(state.iterations()) * producers, producers);
}

static void BM_Typed_MPSC_String(benchmark::State& state) {
  auto& ring = ring_typed_string();
  const int producers = static_cast<int>(state.threads()) - 1;
  const std::string payload(256, 'x');

  if (state.thread_index() == 0) {
    lfring::TypedMessageReader<lfring::SharedRingBuffer> reader(ring);
    std::string out;
    for (auto _ : state) {
      for (int i = 0; i < producers; ++i) {
        while (!reader.try_pop_typed(88, out)) {
          benchmark::DoNotOptimize(out.data());
        }
      }
    }
  } else {
    lfring::TypedMessageWriter<lfring::SharedRingBuffer> writer(ring);
    for (auto _ : state) {
      while (!writer.try_push_typed(payload, 88)) {
        benchmark::DoNotOptimize(payload.data());
      }
    }
  }

  SetBenchmarkCounters(state, static_cast<std::int64_t>(state.iterations()) * producers, producers);
}

static void BM_Typed_MPSC_RobotState(benchmark::State& state) {
  auto& ring = ring_typed_robot();
  const int producers = static_cast<int>(state.threads()) - 1;
  RobotState state_msg{};
  state_msg.tick = 123;
  state_msg.id = 7;
  state_msg.mode = 2;
  state_msg.position[0] = 1.0;
  state_msg.position[1] = 2.0;
  state_msg.position[2] = 3.0;
  state_msg.velocity[0] = 0.1;
  state_msg.velocity[1] = 0.2;
  state_msg.velocity[2] = 0.3;
  for (int i = 0; i < 32; ++i) {
    state_msg.joints[i] = static_cast<float>(i);
  }

  if (state.thread_index() == 0) {
    lfring::TypedMessageReader<lfring::SharedRingBuffer> reader(ring);
    RobotState out{};
    for (auto _ : state) {
      for (int i = 0; i < producers; ++i) {
        while (!reader.try_pop(out)) {
          benchmark::DoNotOptimize(out.tick);
        }
      }
    }
  } else {
    lfring::TypedMessageWriter<lfring::SharedRingBuffer> writer(ring);
    for (auto _ : state) {
      while (!writer.try_push(state_msg)) {
        benchmark::DoNotOptimize(state_msg.tick);
      }
    }
  }

  SetBenchmarkCounters(state, static_cast<std::int64_t>(state.iterations()) * producers, producers);
}

static void BM_Typed_MPSC_RobotState_Burst(benchmark::State& state) {
  auto& ring = ring_typed_robot();
  const int producers = static_cast<int>(state.threads()) - 1;
  const int burst = static_cast<int>(state.range(0));
  RobotState state_msg{};
  state_msg.tick = 123;
  state_msg.id = 7;
  state_msg.mode = 2;
  state_msg.position[0] = 1.0;
  state_msg.position[1] = 2.0;
  state_msg.position[2] = 3.0;
  state_msg.velocity[0] = 0.1;
  state_msg.velocity[1] = 0.2;
  state_msg.velocity[2] = 0.3;
  for (int i = 0; i < 32; ++i) {
    state_msg.joints[i] = static_cast<float>(i);
  }

  if (state.thread_index() == 0) {
    lfring::TypedMessageReader<lfring::SharedRingBuffer> reader(ring);
    RobotState out{};
    for (auto _ : state) {
      for (int i = 0; i < producers * burst; ++i) {
        while (!reader.try_pop(out)) {
          benchmark::DoNotOptimize(out.tick);
        }
      }
    }
  } else {
    lfring::TypedMessageWriter<lfring::SharedRingBuffer> writer(ring);
    for (auto _ : state) {
      for (int i = 0; i < burst; ++i) {
        while (!writer.try_push(state_msg)) {
          benchmark::DoNotOptimize(state_msg.tick);
        }
      }
    }
  }

  SetBenchmarkCounters(state, static_cast<std::int64_t>(state.iterations()) * producers * burst, producers);
}

static void BM_Typed_SPSC_RobotState(benchmark::State& state) {
  auto& ring = ring_typed_spsc_robot();

  RobotState state_msg{};
  state_msg.tick = 123;
  state_msg.id = 7;
  state_msg.mode = 2;
  state_msg.position[0] = 1.0;
  state_msg.position[1] = 2.0;
  state_msg.position[2] = 3.0;
  state_msg.velocity[0] = 0.1;
  state_msg.velocity[1] = 0.2;
  state_msg.velocity[2] = 0.3;
  for (int i = 0; i < 32; ++i) {
    state_msg.joints[i] = static_cast<float>(i);
  }

  if (state.threads() != 2) {
    state.SkipWithError("BM_Typed_SPSC_RobotState requires exactly 2 threads (1 producer, 1 consumer)");
    return;
  }

  if (state.thread_index() == 0) {
    lfring::TypedMessageReader<lfring::SharedRingBufferSPSC> reader(ring);
    RobotState out{};
    for (auto _ : state) {
      while (!reader.try_pop(out)) {
        benchmark::DoNotOptimize(out.tick);
      }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  } else {
    lfring::TypedMessageWriter<lfring::SharedRingBufferSPSC> writer(ring);
    for (auto _ : state) {
      while (!writer.try_push(state_msg)) {
        benchmark::DoNotOptimize(state_msg.tick);
      }
    }
  }
}

static void BM_Latest_RobotState_Write(benchmark::State& state) {
  auto& latest = latest_robot();
  RobotState msg{};
  msg.tick = 123;
  msg.id = 7;
  msg.mode = 2;
  msg.position[0] = 1.0;
  msg.position[1] = 2.0;
  msg.position[2] = 3.0;
  msg.velocity[0] = 0.1;
  msg.velocity[1] = 0.2;
  msg.velocity[2] = 0.3;
  for (int i = 0; i < 32; ++i) {
    msg.joints[i] = static_cast<float>(i);
  }

  for (auto _ : state) {
    msg.tick++;
    latest.write(msg);
    benchmark::DoNotOptimize(msg.tick);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

static void BM_Latest_RobotState_Read(benchmark::State& state) {
  auto& latest = latest_robot();

  RobotState seed{};
  seed.tick = 123;
  seed.id = 7;
  seed.mode = 2;
  for (int i = 0; i < 32; ++i) {
    seed.joints[i] = static_cast<float>(i);
  }
  latest.write(seed);

  RobotState out{};
  std::int64_t ok = 0;
  std::int64_t fail = 0;
  for (auto _ : state) {
    if (latest.try_read(out, 64)) {
      ++ok;
      benchmark::DoNotOptimize(out.tick);
    } else {
      ++fail;
    }
  }
  state.SetItemsProcessed(ok);
  state.counters["read_fail"] = static_cast<double>(fail);
}

static void BM_Latest_RobotState_RW(benchmark::State& state) {
  auto& latest = latest_robot();

  RobotState msg{};
  msg.tick = 123;
  msg.id = 7;
  msg.mode = 2;
  for (int i = 0; i < 32; ++i) {
    msg.joints[i] = static_cast<float>(i);
  }

  if (state.threads() != 2) {
    state.SkipWithError("BM_Latest_RobotState_RW requires exactly 2 threads (1 writer, 1 reader)");
    return;
  }

  if (state.thread_index() == 0) {
    RobotState out{};
    std::int64_t ok = 0;
    std::int64_t fail = 0;
    for (auto _ : state) {
      if (latest.try_read(out, 64)) {
        ++ok;
        benchmark::DoNotOptimize(out.tick);
      } else {
        ++fail;
      }
    }
    // Only thread 0 reports counters to avoid framework summing across threads.
    state.SetItemsProcessed(ok);
    state.counters["read_fail"] = static_cast<double>(fail);
  } else {
    for (auto _ : state) {
      msg.tick++;
      latest.write(msg);
      benchmark::DoNotOptimize(msg.tick);
    }
  }
}

static void BM_Ring_MPSC_Variable(benchmark::State& state) {
  auto& ring = ring_large();
  std::array<std::size_t, 4> sizes = {64, 256, 1024, 4096};
  const int producers = static_cast<int>(state.threads()) - 1;

  if (state.thread_index() == 0) {
    std::vector<std::byte> out;
    std::uint16_t type = 0;
    for (auto _ : state) {
      for (int i = 0; i < producers; ++i) {
        while (!ring.try_pop(out, type)) {
          benchmark::DoNotOptimize(out.data());
        }
      }
    }
  } else {
    std::array<std::vector<std::byte>, 4> payloads = {
        make_payload(sizes[0]), make_payload(sizes[1]), make_payload(sizes[2]), make_payload(sizes[3])};
    std::size_t index = static_cast<std::size_t>(state.thread_index()) % sizes.size();
    for (auto _ : state) {
      auto& payload = payloads[index];
      while (!ring.try_push(payload, 2)) {
        benchmark::DoNotOptimize(payload.data());
      }
      index = (index + 1) % sizes.size();
    }
  }

  SetBenchmarkCounters(state, static_cast<std::int64_t>(state.iterations()) * producers, producers);
}

static void BM_Locked_MPSC_Small(benchmark::State& state) {
  auto& queue = locked_queue();
  auto payload = make_payload(32);
  const int producers = static_cast<int>(state.threads()) - 1;

  if (state.thread_index() == 0) {
    std::vector<std::byte> out;
    for (auto _ : state) {
      for (int i = 0; i < producers; ++i) {
        while (!queue.dequeue(out)) {
          benchmark::DoNotOptimize(out.data());
        }
      }
    }
  } else {
    for (auto _ : state) {
      queue.enqueue(payload);
    }
  }

  SetBenchmarkCounters(state, static_cast<std::int64_t>(state.iterations()) * producers, producers);
}

using SmallPayload = std::array<std::byte, 32>;

lfring::SPSCQueue<SmallPayload, 32768>& fixed_spsc_small() {
  static TempFile tmp("lfring_bench_fixed_spsc_small");
  static auto q = lfring::SPSCQueue<SmallPayload, 32768>::create(tmp.path);
  return q;
}

lfring::SPSCQueue<RobotState, 16384>& fixed_spsc_robot() {
  static TempFile tmp("lfring_bench_fixed_spsc_robot");
  static auto q = lfring::SPSCQueue<RobotState, 16384>::create(tmp.path);
  return q;
}

static void BM_FixedSPSC_Small(benchmark::State& state) {
  auto& q = fixed_spsc_small();

  if (state.threads() != 2) {
    state.SkipWithError("BM_FixedSPSC_Small requires exactly 2 threads");
    return;
  }

  SmallPayload payload{};
  std::memset(payload.data(), 0x5A, payload.size());

  if (state.thread_index() == 0) {
    SmallPayload out{};
    for (auto _ : state) {
      while (!q.try_pop(out)) {
        benchmark::DoNotOptimize(out.data());
      }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  } else {
    for (auto _ : state) {
      while (!q.try_push(payload)) {
        benchmark::DoNotOptimize(payload.data());
      }
    }
  }
}

static void BM_FixedSPSC_RobotState(benchmark::State& state) {
  auto& q = fixed_spsc_robot();

  if (state.threads() != 2) {
    state.SkipWithError("BM_FixedSPSC_RobotState requires exactly 2 threads");
    return;
  }

  RobotState msg{};
  msg.tick = 123;
  msg.id = 7;
  msg.mode = 2;
  msg.position[0] = 1.0;
  msg.position[1] = 2.0;
  msg.position[2] = 3.0;
  msg.velocity[0] = 0.1;
  msg.velocity[1] = 0.2;
  msg.velocity[2] = 0.3;
  for (int i = 0; i < 32; ++i) {
    msg.joints[i] = static_cast<float>(i);
  }

  if (state.thread_index() == 0) {
    RobotState out{};
    for (auto _ : state) {
      while (!q.try_pop(out)) {
        benchmark::DoNotOptimize(out.tick);
      }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  } else {
    for (auto _ : state) {
      while (!q.try_push(msg)) {
        benchmark::DoNotOptimize(msg.tick);
      }
    }
  }
}

} // namespace

BENCHMARK(BM_Ring_MPSC_Small)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Ring_SPSC_Small)->Threads(2)->UseRealTime();
BENCHMARK(BM_Typed_MPSC_Trivial)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Typed_MPSC_String)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Typed_MPSC_RobotState)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Typed_MPSC_RobotState_Burst)->ThreadRange(2, 8)->Arg(1)->Arg(8)->Arg(32)->Arg(128)->UseRealTime();
BENCHMARK(BM_Typed_SPSC_RobotState)->Threads(2)->UseRealTime();
BENCHMARK(BM_Latest_RobotState_Write)->UseRealTime();
BENCHMARK(BM_Latest_RobotState_Read)->UseRealTime();
BENCHMARK(BM_Latest_RobotState_RW)->Threads(2)->UseRealTime();
BENCHMARK(BM_Ring_MPSC_Variable)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Locked_MPSC_Small)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_FixedSPSC_Small)->Threads(2)->UseRealTime();
BENCHMARK(BM_FixedSPSC_RobotState)->Threads(2)->UseRealTime();

BENCHMARK_MAIN();
