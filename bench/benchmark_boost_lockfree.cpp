// Boost.Lockfree comparison benchmarks.
//
// Mirrors the lf_ring benchmarks so numbers can be compared directly:
//   BM_Boost_SPSC_Small        <-> BM_Ring_SPSC_Small / BM_FixedSPSC_Small
//   BM_Boost_SPSC_RobotState   <-> BM_Typed_SPSC_RobotState / BM_FixedSPSC_RobotState
//   BM_Boost_MPMC_Small        <-> BM_Ring_MPSC_Small
//   BM_Boost_MPMC_RobotState   <-> BM_Typed_MPSC_RobotState
//
// Note: boost::lockfree::queue is MPMC (no dedicated MPSC variant),
// so it does more synchronization work per operation than lf_ring's MPSC.

#include "lf_ring/spsc_queue.hpp"

#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include <unistd.h>

namespace {

// ── payload types ──────────────────────────────────────────────────────

using SmallPayload = std::array<std::byte, 32>;

struct RobotState {
  std::uint64_t tick;
  std::uint32_t id;
  std::uint32_t mode;
  double position[3];
  double velocity[3];
  float joints[32];
};

SmallPayload make_small_payload() {
  SmallPayload p{};
  std::memset(p.data(), 0x5A, p.size());
  return p;
}

RobotState make_robot_state() {
  RobotState s{};
  s.tick = 123;
  s.id = 7;
  s.mode = 2;
  s.position[0] = 1.0;
  s.position[1] = 2.0;
  s.position[2] = 3.0;
  s.velocity[0] = 0.1;
  s.velocity[1] = 0.2;
  s.velocity[2] = 0.3;
  for (int i = 0; i < 32; ++i) {
    s.joints[i] = static_cast<float>(i);
  }
  return s;
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

// ── SPSC benchmarks ────────────────────────────────────────────────────

static void BM_Boost_SPSC_Small(benchmark::State& state) {
  static boost::lockfree::spsc_queue<SmallPayload, boost::lockfree::capacity<32768>> queue;

  if (state.threads() != 2) {
    state.SkipWithError("BM_Boost_SPSC_Small requires exactly 2 threads");
    return;
  }

  const auto payload = make_small_payload();

  if (state.thread_index() == 0) {
    // consumer
    SmallPayload out{};
    for (auto _ : state) {
      while (!queue.pop(out)) {
        benchmark::DoNotOptimize(out.data());
      }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  } else {
    // producer
    for (auto _ : state) {
      while (!queue.push(payload)) {
        benchmark::DoNotOptimize(payload.data());
      }
    }
  }
}

static void BM_Boost_SPSC_RobotState(benchmark::State& state) {
  static boost::lockfree::spsc_queue<RobotState, boost::lockfree::capacity<16384>> queue;

  if (state.threads() != 2) {
    state.SkipWithError("BM_Boost_SPSC_RobotState requires exactly 2 threads");
    return;
  }

  auto robot = make_robot_state();

  if (state.thread_index() == 0) {
    RobotState out{};
    for (auto _ : state) {
      while (!queue.pop(out)) {
        benchmark::DoNotOptimize(out.tick);
      }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  } else {
    for (auto _ : state) {
      while (!queue.push(robot)) {
        benchmark::DoNotOptimize(robot.tick);
      }
    }
  }
}

// ── Fixed SPSC benchmarks (lf_ring::SPSCQueue) ────────────────────────

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

lfring::SPSCQueue<SmallPayload, 32768>& fixed_spsc_small() {
  static TempFile tmp("lfring_boost_bench_fixed_spsc_small");
  static auto q = lfring::SPSCQueue<SmallPayload, 32768>::create(tmp.path);
  return q;
}

lfring::SPSCQueue<RobotState, 16384>& fixed_spsc_robot() {
  static TempFile tmp("lfring_boost_bench_fixed_spsc_robot");
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

  auto robot = make_robot_state();

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
      while (!q.try_push(robot)) {
        benchmark::DoNotOptimize(robot.tick);
      }
    }
  }
}

// ── MPMC benchmarks ────────────────────────────────────────────────────

static void BM_Boost_MPMC_Small(benchmark::State& state) {
  static boost::lockfree::queue<SmallPayload, boost::lockfree::capacity<32768>> queue;

  const auto payload = make_small_payload();
  const int producers = static_cast<int>(state.threads()) - 1;

  if (state.thread_index() == 0) {
    SmallPayload out{};
    for (auto _ : state) {
      for (int i = 0; i < producers; ++i) {
        while (!queue.pop(out)) {
          benchmark::DoNotOptimize(out.data());
        }
      }
    }
  } else {
    for (auto _ : state) {
      while (!queue.push(payload)) {
        benchmark::DoNotOptimize(payload.data());
      }
    }
  }

  SetBenchmarkCounters(state, static_cast<std::int64_t>(state.iterations()) * producers, producers);
}

static void BM_Boost_MPMC_RobotState(benchmark::State& state) {
  static boost::lockfree::queue<RobotState, boost::lockfree::capacity<16384>> queue;

  auto robot = make_robot_state();
  const int producers = static_cast<int>(state.threads()) - 1;

  if (state.thread_index() == 0) {
    RobotState out{};
    for (auto _ : state) {
      for (int i = 0; i < producers; ++i) {
        while (!queue.pop(out)) {
          benchmark::DoNotOptimize(out.tick);
        }
      }
    }
  } else {
    for (auto _ : state) {
      while (!queue.push(robot)) {
        benchmark::DoNotOptimize(robot.tick);
      }
    }
  }

  SetBenchmarkCounters(state, static_cast<std::int64_t>(state.iterations()) * producers, producers);
}

} // namespace

BENCHMARK(BM_Boost_SPSC_Small)->Threads(2)->UseRealTime();
BENCHMARK(BM_Boost_SPSC_RobotState)->Threads(2)->UseRealTime();
BENCHMARK(BM_FixedSPSC_Small)->Threads(2)->UseRealTime();
BENCHMARK(BM_FixedSPSC_RobotState)->Threads(2)->UseRealTime();
BENCHMARK(BM_Boost_MPMC_Small)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Boost_MPMC_RobotState)->ThreadRange(2, 8)->UseRealTime();

BENCHMARK_MAIN();
