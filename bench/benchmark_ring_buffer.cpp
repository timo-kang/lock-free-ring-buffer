#include "lf_ring/shared_ring_buffer.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <queue>
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

LockedQueue& locked_queue() {
  static LockedQueue queue;
  return queue;
}

std::vector<std::byte> make_payload(std::size_t size) {
  std::vector<std::byte> data(size);
  std::memset(data.data(), 0x5A, data.size());
  return data;
}

} // namespace

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

BENCHMARK(BM_Ring_MPSC_Small)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Ring_MPSC_Variable)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_Locked_MPSC_Small)->ThreadRange(2, 8)->UseRealTime();

BENCHMARK_MAIN();
