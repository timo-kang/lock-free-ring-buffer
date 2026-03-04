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

// Benchmark: SharedLatest futex notification overhead and handoff latency.
//
// Compares:
//   1. write() baseline (no futex syscall)
//   2. write_and_notify() (seqlock + futex_wake syscall)
//   3. 2-thread handoff with futex wait/wake (measures end-to-end latency)
//   4. 2-thread handoff with busy-spin (for comparison)
//
// Build:
//   cmake -S . -B build -DLF_RING_BUILD_BENCHMARKS=ON
//   cmake --build build --target lf_ring_bench_futex

#include "lf_ring/futex.hpp"
#include "lf_ring/shared_latest.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace {

// ── RT thread setup ─────────────────────────────────────────────────
//
// Sets SCHED_FIFO and pins thread to a specific physical core.
// Must be called at the start of each benchmark thread.
//
// Linux enumerates physical cores first (0..N-1), then HT siblings
// (N..2N-1).  Using thread_idx directly gives each thread its own
// physical core for up to N threads, avoiding HT contention.

void setup_rt_thread(int thread_idx) {
  struct sched_param param{};
  param.sched_priority = 80;
  ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param);

  // Pin to physical core thread_idx (cores 0..N-1 are physical).
  // Cap to online CPUs to avoid silent failure on smaller machines.
  int ncpus = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
  int core = thread_idx % ncpus;

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);
  ::sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

// ── temp file helper ─────────────────────────────────────────────────

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

// ── payload ──────────────────────────────────────────────────────────

struct Command {
  double x_pos;
  double y_pos;
  double x_vel;
  double y_vel;
  double yaw_rate;
  double pitch_angle;
  double body_height;
  double pan_dir;
  double tilt_dir;
};
static_assert(sizeof(Command) == 72);

Command make_command() {
  return Command{0.0, 0.0, 1.0, 0.5, 0.1, 0.0, 0.3, 0.0, 0.0};
}

// ── static instances ─────────────────────────────────────────────────

lfring::SharedLatest<Command>& get_sl_write() {
  static TempFile tmp("bench_futex_write");
  static auto sl = lfring::SharedLatest<Command>::create(tmp.path);
  return sl;
}

lfring::SharedLatest<Command>& get_sl_notify() {
  static TempFile tmp("bench_futex_notify");
  static auto sl = lfring::SharedLatest<Command>::create(tmp.path);
  return sl;
}

// =====================================================================
// Group 1: write() vs write_and_notify() — single-thread overhead
// =====================================================================

static void BM_Write_NoFutex(benchmark::State& state) {
  auto& sl = get_sl_write();
  auto cmd = make_command();

  for (auto _ : state) {
    cmd.x_vel += 0.001;
    sl.write(cmd);
    benchmark::DoNotOptimize(cmd);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(sizeof(Command)));
}

static void BM_WriteAndNotify_WithFutex(benchmark::State& state) {
  auto& sl = get_sl_notify();
  auto cmd = make_command();

  for (auto _ : state) {
    cmd.x_vel += 0.001;
    sl.write_and_notify(cmd);
    benchmark::DoNotOptimize(cmd);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(sizeof(Command)));
}

// =====================================================================
// Group 2: 2-thread handoff — futex wait/wake (no CPU burn)
// =====================================================================

static void BM_Handoff_Futex(benchmark::State& state) {
  static TempFile tmp_futex("bench_handoff_futex");
  static auto& sl_futex = [] () -> auto& {
    static auto sl = lfring::SharedLatest<Command>::create(tmp_futex.path);
    return sl;
  }();

  if (state.threads() != 2) {
    state.SkipWithError("Requires exactly 2 threads");
    return;
  }

  auto cmd = make_command();

  // Ping-pong: writer→reader via SharedLatest futex, reader→writer via
  // futex on ack word.  No spin-waits — both sides block in kernel.
  // This is safe under SCHED_FIFO (spin-waits can starve co-scheduled
  // threads at the same priority).
  static std::atomic<std::uint64_t> ack{0};

  setup_rt_thread(state.thread_index());

  if (state.thread_index() == 0) {
    // Reader: wait for futex notification, read, ack back via futex.
    Command out{};
    auto seen = sl_futex.notify_counter();
    for (auto _ : state) {
      sl_futex.wait_for_update(seen, std::chrono::milliseconds(100));
      seen = sl_futex.notify_counter();
      sl_futex.try_read(out);
      ack.fetch_add(1, std::memory_order_release);
      lfring::detail::futex_wake_all(ack);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    benchmark::DoNotOptimize(out);
  } else {
    // Writer: write+notify, wait for reader ack via futex.
    auto ack_seen = lfring::detail::futex_load(ack);
    for (auto _ : state) {
      cmd.x_vel += 0.001;
      sl_futex.write_and_notify(cmd);
      lfring::detail::futex_wait_timeout(ack, ack_seen, std::chrono::milliseconds(100));
      ack_seen = lfring::detail::futex_load(ack);
    }
  }
}

// =====================================================================
// Group 3: 2-thread handoff — busy spin (for comparison)
// =====================================================================

static void BM_Handoff_Spin(benchmark::State& state) {
  static TempFile tmp_spin("bench_handoff_spin");
  static auto& sl_spin = [] () -> auto& {
    static auto sl = lfring::SharedLatest<Command>::create(tmp_spin.path);
    return sl;
  }();

  if (state.threads() != 2) {
    state.SkipWithError("Requires exactly 2 threads");
    return;
  }

  auto cmd = make_command();
  static std::atomic<int> spin_ready{0};

  setup_rt_thread(state.thread_index());

  if (state.thread_index() == 0) {
    // Reader (busy-spin).
    Command out{};
    for (auto _ : state) {
      while (spin_ready.load(std::memory_order_acquire) == 0) {}
      sl_spin.try_read(out);
      spin_ready.store(0, std::memory_order_release);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    benchmark::DoNotOptimize(out);
  } else {
    // Writer (no futex, just write).
    for (auto _ : state) {
      cmd.x_vel += 0.001;
      sl_spin.write(cmd);
      spin_ready.store(1, std::memory_order_release);
      while (spin_ready.load(std::memory_order_acquire) != 0) {}
    }
  }
}

// =====================================================================
// Group 4: read-only (no contention) — baseline read cost
// =====================================================================

static void BM_TryRead_Baseline(benchmark::State& state) {
  static TempFile tmp_read("bench_read_baseline");
  static auto& sl_read = [] () -> auto& {
    static auto sl = lfring::SharedLatest<Command>::create(tmp_read.path);
    auto cmd = make_command();
    sl.write(cmd);
    return sl;
  }();

  Command out{};
  for (auto _ : state) {
    sl_read.try_read(out);
    benchmark::DoNotOptimize(out);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

// =====================================================================
// Group 5: Concurrent write + read (no synchronization)
//
// This is the actual production pattern: writer publishes continuously,
// reader reads continuously. No ping-pong — both run at full speed.
// The seqlock handles torn-read prevention internally.
// =====================================================================

// Writer continuously writes; reader continuously reads (spin-based).
// Measures throughput under real contention.
static void BM_Concurrent_WriteRead_Spin(benchmark::State& state) {
  static TempFile tmp_conc("bench_concurrent_spin");
  static auto& sl_conc = [] () -> auto& {
    static auto sl = lfring::SharedLatest<Command>::create(tmp_conc.path);
    auto cmd = make_command();
    sl.write(cmd);  // seed initial value
    return sl;
  }();

  if (state.threads() != 2) {
    state.SkipWithError("Requires exactly 2 threads");
    return;
  }

  setup_rt_thread(state.thread_index());

  if (state.thread_index() == 0) {
    // Writer: publish as fast as possible.
    auto cmd = make_command();
    for (auto _ : state) {
      cmd.x_vel += 0.001;
      sl_conc.write(cmd);
      benchmark::DoNotOptimize(cmd);
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(sizeof(Command)));
  } else {
    // Reader: read as fast as possible.
    Command out{};
    for (auto _ : state) {
      sl_conc.try_read(out);
      benchmark::DoNotOptimize(out);
    }
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

// Writer writes with futex notify; reader reads with futex wait.
// Futex-based ack ensures 1:1 iteration pairing (writer can't run ahead).
static void BM_Concurrent_WriteRead_Futex(benchmark::State& state) {
  static TempFile tmp_conc_f("bench_concurrent_futex");
  static auto& sl_conc_f = [] () -> auto& {
    static auto sl = lfring::SharedLatest<Command>::create(tmp_conc_f.path);
    auto cmd = make_command();
    sl.write_and_notify(cmd);  // seed initial value
    return sl;
  }();

  if (state.threads() != 2) {
    state.SkipWithError("Requires exactly 2 threads");
    return;
  }

  static std::atomic<std::uint64_t> conc_ack{0};

  setup_rt_thread(state.thread_index());

  if (state.thread_index() == 0) {
    // Reader: wait for notification, read, ack via futex.
    Command out{};
    auto seen = sl_conc_f.notify_counter();
    for (auto _ : state) {
      sl_conc_f.wait_for_update(seen, std::chrono::milliseconds(100));
      seen = sl_conc_f.notify_counter();
      sl_conc_f.try_read(out);
      conc_ack.fetch_add(1, std::memory_order_release);
      lfring::detail::futex_wake_all(conc_ack);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    benchmark::DoNotOptimize(out);
  } else {
    // Writer: publish with futex notification, wait for ack.
    auto cmd = make_command();
    auto ack_seen = lfring::detail::futex_load(conc_ack);
    for (auto _ : state) {
      cmd.x_vel += 0.001;
      sl_conc_f.write_and_notify(cmd);
      lfring::detail::futex_wait_timeout(conc_ack, ack_seen,
                                         std::chrono::milliseconds(100));
      ack_seen = lfring::detail::futex_load(conc_ack);
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(sizeof(Command)));
  }
}

// Futex handoff with per-iteration timing breakdown.
// Reports write_ns (writer side) and wake_read_ns (reader side) as counters.
static void BM_ControlLoop_1kHz_Futex(benchmark::State& state) {
  static TempFile tmp_loop("bench_loop_futex");
  static auto& sl_loop = [] () -> auto& {
    static auto sl = lfring::SharedLatest<Command>::create(tmp_loop.path);
    auto cmd = make_command();
    sl.write_and_notify(cmd);
    return sl;
  }();

  if (state.threads() != 2) {
    state.SkipWithError("Requires exactly 2 threads");
    return;
  }

  static std::atomic<std::uint64_t> loop_ack{0};

  setup_rt_thread(state.thread_index());

  if (state.thread_index() == 0) {
    // Reader: block until notification, measure wake-to-read latency, ack.
    Command out{};
    auto seen = sl_loop.notify_counter();
    for (auto _ : state) {
      auto t0 = std::chrono::steady_clock::now();
      sl_loop.wait_for_update(seen, std::chrono::milliseconds(100));
      seen = sl_loop.notify_counter();
      sl_loop.try_read(out);
      auto t1 = std::chrono::steady_clock::now();
      auto wake_read_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      state.counters["wake_read_ns"] = benchmark::Counter(
          static_cast<double>(wake_read_ns), benchmark::Counter::kAvgIterations);
      loop_ack.fetch_add(1, std::memory_order_release);
      lfring::detail::futex_wake_all(loop_ack);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    benchmark::DoNotOptimize(out);
  } else {
    // Writer: write+notify, measure write time, wait for reader ack.
    auto cmd = make_command();
    auto ack_seen = lfring::detail::futex_load(loop_ack);
    for (auto _ : state) {
      cmd.x_vel += 0.001;
      auto t0 = std::chrono::steady_clock::now();
      sl_loop.write_and_notify(cmd);
      auto t1 = std::chrono::steady_clock::now();
      auto write_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      state.counters["write_ns"] = benchmark::Counter(
          static_cast<double>(write_ns), benchmark::Counter::kAvgIterations);
      lfring::detail::futex_wait_timeout(loop_ack, ack_seen,
                                         std::chrono::milliseconds(100));
      ack_seen = lfring::detail::futex_load(loop_ack);
    }
  }
}

} // namespace

// ─── Group 1: Single-thread write overhead ────────────────────────────
BENCHMARK(BM_Write_NoFutex)->UseRealTime();
BENCHMARK(BM_WriteAndNotify_WithFutex)->UseRealTime();

// ─── Group 2: Handoff with futex (no CPU burn) ───────────────────────
BENCHMARK(BM_Handoff_Futex)->Threads(2)->UseRealTime();

// ─── Group 3: Handoff with busy-spin (for comparison) ────────────────
BENCHMARK(BM_Handoff_Spin)->Threads(2)->UseRealTime();

// ─── Group 4: Read baseline ──────────────────────────────────────────
BENCHMARK(BM_TryRead_Baseline)->UseRealTime();

// ─── Group 5: Concurrent write+read (production pattern) ─────────────
BENCHMARK(BM_Concurrent_WriteRead_Spin)->Threads(2)->UseRealTime();
BENCHMARK(BM_Concurrent_WriteRead_Futex)->Threads(2)->UseRealTime();
BENCHMARK(BM_ControlLoop_1kHz_Futex)->Threads(2)->UseRealTime();

int main(int argc, char** argv) {
  // Match production: raisin_master runs control threads with SCHED_FIFO.
  // This eliminates CFS scheduler latency from futex wake measurements.
  // Requires: sudo setcap cap_sys_nice+ep <binary>
  struct sched_param param{};
  param.sched_priority = 80;
  if (::sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
    fprintf(stderr,
            "WARNING: Could not set SCHED_FIFO (errno=%d).\n"
            "  Futex benchmarks will show CFS latency (~ms) instead of RT (~us).\n"
            "  Fix: sudo setcap cap_sys_nice+ep %s\n",
            errno, argv[0]);
  } else {
    fprintf(stderr, "INFO: Running with SCHED_FIFO priority %d\n",
            param.sched_priority);
  }

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
