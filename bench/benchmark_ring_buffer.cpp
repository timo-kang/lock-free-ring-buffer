#include "lf_ring/shared_ring_buffer.hpp"
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

LockedQueue& locked_queue() {
  static LockedQueue queue;
  return queue;
}

std::vector<std::byte> make_payload(std::size_t size) {
  std::vector<std::byte> data(size);
  std::memset(data.data(), 0x5A, data.size());
  return data;
}

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

  state.SetItemsProcessed(state.iterations() * producers);
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

  state.SetItemsProcessed(state.iterations() * producers);
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

  state.SetItemsProcessed(state.iterations() * producers);
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

  state.SetItemsProcessed(state.iterations() * producers);
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

  state.SetItemsProcessed(state.iterations() * producers * burst);
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

  state.SetItemsProcessed(state.iterations() * producers);
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

  state.SetItemsProcessed(state.iterations() * producers);
}

} // namespace

BENCHMARK(BM_Ring_MPSC_Small)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Typed_MPSC_Trivial)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Typed_MPSC_String)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Typed_MPSC_RobotState)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Typed_MPSC_RobotState_Burst)->ThreadRange(2, 8)->Arg(1)->Arg(8)->Arg(32)->Arg(128)->UseRealTime();
BENCHMARK(BM_Ring_MPSC_Variable)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Locked_MPSC_Small)->ThreadRange(2, 8)->UseRealTime();

BENCHMARK_MAIN();
