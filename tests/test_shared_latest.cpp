#include "lf_ring/shared_latest.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
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

struct RobotState {
  double x;
  double y;
  double yaw;
  std::uint64_t stamp_ns;
};

using namespace std::chrono_literals;

} // namespace

TEST(SharedLatest, WriteAndRead) {
  TempFile tmp("sl_write_read");
  auto sl = lfring::SharedLatest<RobotState>::create(tmp.path);

  RobotState state{1.0, 2.0, 0.5, 123};
  sl.write(state);

  RobotState out{};
  ASSERT_TRUE(sl.try_read(out));
  ASSERT_DOUBLE_EQ(out.x, 1.0);
  ASSERT_DOUBLE_EQ(out.y, 2.0);
  ASSERT_DOUBLE_EQ(out.yaw, 0.5);
  ASSERT_EQ(out.stamp_ns, 123u);
}

TEST(SharedLatest, ReadEmpty) {
  TempFile tmp("sl_read_empty");
  auto sl = lfring::SharedLatest<std::uint64_t>::create(tmp.path);

  // The original try_read returns true even on seq=0 because 0 is even (valid seqlock state).
  // It reads zero-initialized memory. Only the ReadResult overload distinguishes "empty."
  std::uint64_t out = 42;
  ASSERT_TRUE(sl.try_read(out));
  ASSERT_EQ(out, 0u);
  ASSERT_EQ(sl.raw_sequence(), 0u);
}

TEST(SharedLatest, ReadEmptyStatus) {
  TempFile tmp("sl_read_empty_status");
  auto sl = lfring::SharedLatest<std::uint64_t>::create(tmp.path);

  std::uint64_t out = 42;
  auto result = sl.try_read(out, 1s);
  ASSERT_EQ(result, lfring::ReadResult::kEmpty);
}

TEST(SharedLatest, MultipleWrites) {
  TempFile tmp("sl_multi_write");
  auto sl = lfring::SharedLatest<std::uint64_t>::create(tmp.path);

  for (std::uint64_t i = 0; i < 100; ++i) {
    sl.write(i);
  }

  std::uint64_t out = 0;
  ASSERT_TRUE(sl.try_read(out));
  ASSERT_EQ(out, 99u);
}

TEST(SharedLatest, ConcurrentWriterReader) {
  TempFile tmp("sl_concurrent");
  auto writer = lfring::SharedLatest<RobotState>::create(tmp.path);

  constexpr int kIterations = 100000;
  std::atomic<bool> done{false};
  std::atomic<int> reads_ok{0};

  std::thread reader_thread([&] {
    auto reader = lfring::SharedLatest<RobotState>::open(tmp.path);
    while (!done.load(std::memory_order_relaxed)) {
      RobotState out{};
      if (reader.try_read(out)) {
        // Verify consistency: all fields should come from the same write.
        // We encode the iteration in all fields.
        double expected_yaw = out.x + out.y;
        ASSERT_DOUBLE_EQ(out.yaw, expected_yaw);
        reads_ok.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  for (int i = 0; i < kIterations; ++i) {
    double x = static_cast<double>(i);
    double y = static_cast<double>(i * 2);
    RobotState state{x, y, x + y, static_cast<std::uint64_t>(i)};
    writer.write(state);
  }

  done.store(true, std::memory_order_relaxed);
  reader_thread.join();

  ASSERT_GT(reads_ok.load(), 0);
}

TEST(SharedLatest, WriterDeathDetection) {
  TempFile tmp("sl_writer_death");

  // Parent creates the shared memory file.
  { auto sl = lfring::SharedLatest<std::uint64_t>::create(tmp.path); }

  pid_t pid = fork();
  ASSERT_NE(pid, -1) << "fork() failed";

  if (pid == 0) {
    // Child: complete a write (sets heartbeat), sleep to make heartbeat stale,
    // then poke sequence to odd via raw mmap to simulate death mid-write.
    {
      auto sl = lfring::SharedLatest<std::uint64_t>::open(tmp.path);
      std::uint64_t val = 42;
      sl.write(val);
    }

    // Sleep so the heartbeat becomes stale.
    std::this_thread::sleep_for(10ms);

    // Raw mmap to set sequence to odd without updating heartbeat.
    auto file_size = lfring::SharedLatest<std::uint64_t>::required_mapping_size();
    int fd = ::open(tmp.path.c_str(), O_RDWR);
    if (fd < 0) _exit(1);

    void* mem = ::mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { ::close(fd); _exit(1); }

    auto* hdr = static_cast<lfring::Header*>(mem);
    auto* ctrl = reinterpret_cast<lfring::ControlBlock*>(
        static_cast<std::byte*>(mem) + hdr->control_offset);

    ctrl->sequence.value.store(3, std::memory_order_release);

    ::munmap(mem, file_size);
    ::close(fd);
    _exit(0);
  }

  // Parent: wait for child to finish.
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  // Now open and read — should detect writer death.
  auto reader = lfring::SharedLatest<std::uint64_t>::open(tmp.path);

  // Verify seq is odd.
  ASSERT_EQ(reader.raw_sequence() & 1u, 1u);

  std::uint64_t out = 0;
  // Use a small dead_threshold (1ms) — the heartbeat is at least 10ms stale.
  auto result = reader.try_read(out, 1ms, 256);
  ASSERT_EQ(result, lfring::ReadResult::kWriterDead);
}

TEST(SharedLatest, IsWriterAlive) {
  TempFile tmp("sl_alive");
  auto sl = lfring::SharedLatest<std::uint64_t>::create(tmp.path);

  // No writes yet — heartbeat is 0, should return false.
  ASSERT_FALSE(sl.is_writer_alive(1s));

  sl.write(42u);

  // Just wrote — should be alive with a generous threshold.
  ASSERT_TRUE(sl.is_writer_alive(1s));
}

TEST(SharedLatest, IsWriterAliveStale) {
  TempFile tmp("sl_alive_stale");
  auto sl = lfring::SharedLatest<std::uint64_t>::create(tmp.path);

  sl.write(42u);
  std::this_thread::sleep_for(50ms);

  // After 50ms, a 10ms threshold should report dead.
  ASSERT_FALSE(sl.is_writer_alive(10ms));
  // But a 1s threshold should still report alive.
  ASSERT_TRUE(sl.is_writer_alive(1s));
}

TEST(SharedLatest, ReadResultSuccess) {
  TempFile tmp("sl_result_success");
  auto sl = lfring::SharedLatest<std::uint64_t>::create(tmp.path);

  sl.write(42u);

  std::uint64_t out = 0;
  auto result = sl.try_read(out, 1s);
  ASSERT_EQ(result, lfring::ReadResult::kSuccess);
  ASSERT_EQ(out, 42u);
}

TEST(SharedLatest, ReadResultContended) {
  TempFile tmp("sl_contended");

  // Create and do a normal write.
  { auto sl = lfring::SharedLatest<std::uint64_t>::create(tmp.path);
    sl.write(42u); }

  // Manually set seq to odd with a fresh heartbeat via raw mmap.
  {
    auto file_size = lfring::SharedLatest<std::uint64_t>::required_mapping_size();
    int fd = ::open(tmp.path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);

    void* mem = ::mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(mem, MAP_FAILED);

    auto* hdr = static_cast<lfring::Header*>(mem);
    auto* ctrl = reinterpret_cast<lfring::ControlBlock*>(
        static_cast<std::byte*>(mem) + hdr->control_offset);

    // Set seq odd.
    ctrl->sequence.value.store(3, std::memory_order_release);
    // Set heartbeat to now (fresh).
    auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    ctrl->head_reserve.value.store(now, std::memory_order_release);

    ::munmap(mem, file_size);
    ::close(fd);
  }

  auto reader = lfring::SharedLatest<std::uint64_t>::open(tmp.path);

  std::uint64_t out = 0;
  // Use very high dead_threshold so heartbeat appears fresh, low retries.
  auto result = reader.try_read(out, 10s, 128);
  ASSERT_EQ(result, lfring::ReadResult::kContended);
}

// ═══════════════════════════════════════════════════════════════════════
// Futex notification tests
// ═══════════════════════════════════════════════════════════════════════

TEST(SharedLatestFutex, NotifyCounterIncrementsOnWrite) {
  TempFile tmp("sl_futex_counter");
  auto sl = lfring::SharedLatest<std::uint64_t>::create(tmp.path);

  // Initially zero.
  ASSERT_EQ(sl.notify_counter(), 0u);

  // Regular write does NOT increment the counter.
  sl.write(1u);
  ASSERT_EQ(sl.notify_counter(), 0u);

  // write_and_notify DOES increment.
  sl.write_and_notify(2u);
  ASSERT_EQ(sl.notify_counter(), 1u);

  sl.write_and_notify(3u);
  ASSERT_EQ(sl.notify_counter(), 2u);

  // Data should still be correct.
  std::uint64_t out = 0;
  ASSERT_TRUE(sl.try_read(out));
  ASSERT_EQ(out, 3u);
}

TEST(SharedLatestFutex, WaitTimesOutWhenNoWrite) {
  TempFile tmp("sl_futex_timeout");
  auto sl = lfring::SharedLatest<std::uint64_t>::create(tmp.path);

  std::uint32_t seen = sl.notify_counter();

  auto start = std::chrono::steady_clock::now();
  bool woken = sl.wait_for_update(seen, 50ms);
  auto elapsed = std::chrono::steady_clock::now() - start;

  // Should time out (no writer called write_and_notify).
  ASSERT_FALSE(woken);
  // Should have waited approximately 50ms.
  ASSERT_GE(elapsed, 40ms);
  ASSERT_LE(elapsed, 200ms);
}

TEST(SharedLatestFutex, WriterWakesReader) {
  TempFile tmp("sl_futex_wake");
  auto writer = lfring::SharedLatest<RobotState>::create(tmp.path);

  std::atomic<bool> reader_started{false};
  std::atomic<bool> reader_woken{false};
  std::atomic<std::uint64_t> wakeup_time_ns{0};

  std::thread reader_thread([&] {
    auto reader = lfring::SharedLatest<RobotState>::open(tmp.path);
    std::uint32_t seen = reader.notify_counter();
    reader_started.store(true, std::memory_order_release);

    // Block until writer notifies (or timeout after 5s as safety).
    bool woken = reader.wait_for_update(seen, 5000ms);
    wakeup_time_ns.store(
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()),
        std::memory_order_release);
    reader_woken.store(woken, std::memory_order_release);
  });

  // Wait for reader to be blocking.
  while (!reader_started.load(std::memory_order_acquire)) {}
  std::this_thread::sleep_for(10ms);  // ensure reader is in futex_wait

  auto write_time = std::chrono::steady_clock::now();
  RobotState state{1.0, 2.0, 0.5, 42};
  writer.write_and_notify(state);

  reader_thread.join();

  ASSERT_TRUE(reader_woken.load());

  // Verify data was readable.
  auto reader2 = lfring::SharedLatest<RobotState>::open(tmp.path);
  RobotState out{};
  ASSERT_TRUE(reader2.try_read(out));
  ASSERT_DOUBLE_EQ(out.x, 1.0);
  ASSERT_DOUBLE_EQ(out.y, 2.0);
  ASSERT_EQ(out.stamp_ns, 42u);

  // Verify wakeup happened promptly (within 10ms of write).
  auto write_ns = static_cast<std::uint64_t>(write_time.time_since_epoch().count());
  auto wake_ns = wakeup_time_ns.load();
  auto latency_us = (wake_ns - write_ns) / 1000;
  ASSERT_LT(latency_us, 10000u) << "Wakeup latency " << latency_us << "us exceeds 10ms";
}

TEST(SharedLatestFutex, MultipleWritesMultipleWakes) {
  TempFile tmp("sl_futex_multi");
  auto writer = lfring::SharedLatest<std::uint64_t>::create(tmp.path);

  constexpr int kRounds = 100;
  std::atomic<int> rounds_completed{0};
  // Handoff flag: 0 = reader waiting, 1 = writer published.
  std::atomic<int> handoff{0};

  std::thread reader_thread([&] {
    auto reader = lfring::SharedLatest<std::uint64_t>::open(tmp.path);

    for (int i = 0; i < kRounds; ++i) {
      // Wait for notification via futex.
      std::uint32_t seen = reader.notify_counter();
      // If the writer already published since last check, skip wait.
      if (handoff.load(std::memory_order_acquire) == 0) {
        reader.wait_for_update(seen, 1000ms);
      }

      std::uint64_t val = 0;
      ASSERT_TRUE(reader.try_read(val));
      ASSERT_GE(val, static_cast<std::uint64_t>(i + 1));
      rounds_completed.fetch_add(1, std::memory_order_relaxed);
      // Signal the writer we consumed.
      handoff.store(0, std::memory_order_release);
    }
  });

  for (int i = 0; i < kRounds; ++i) {
    writer.write_and_notify(static_cast<std::uint64_t>(i + 1));
    handoff.store(1, std::memory_order_release);
    // Wait for reader to consume before writing next.
    while (handoff.load(std::memory_order_acquire) != 0) {
      std::this_thread::sleep_for(10us);
    }
  }

  reader_thread.join();
  ASSERT_EQ(rounds_completed.load(), kRounds);
}

TEST(SharedLatestFutex, ConcurrentWriterReaderWithNotify) {
  TempFile tmp("sl_futex_concurrent");
  auto writer = lfring::SharedLatest<RobotState>::create(tmp.path);

  constexpr int kIterations = 50000;
  std::atomic<bool> done{false};
  std::atomic<int> reads_ok{0};
  std::atomic<int> wakeups{0};

  std::thread reader_thread([&] {
    auto reader = lfring::SharedLatest<RobotState>::open(tmp.path);

    while (!done.load(std::memory_order_relaxed)) {
      std::uint32_t seen = reader.notify_counter();
      bool woken = reader.wait_for_update(seen, 10ms);
      if (woken) {
        wakeups.fetch_add(1, std::memory_order_relaxed);
      }

      RobotState out{};
      if (reader.try_read(out)) {
        double expected_yaw = out.x + out.y;
        ASSERT_DOUBLE_EQ(out.yaw, expected_yaw);
        reads_ok.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  for (int i = 0; i < kIterations; ++i) {
    double x = static_cast<double>(i);
    double y = static_cast<double>(i * 2);
    RobotState state{x, y, x + y, static_cast<std::uint64_t>(i)};
    writer.write_and_notify(state);
  }

  // Let the reader catch up.
  std::this_thread::sleep_for(50ms);
  done.store(true, std::memory_order_relaxed);
  // One more notify to unblock the reader if it's in futex_wait.
  writer.write_and_notify(RobotState{0, 0, 0, 0});
  reader_thread.join();

  ASSERT_GT(reads_ok.load(), 0);
  ASSERT_GT(wakeups.load(), 0);
}

TEST(SharedLatestFutex, CrossProcessFutexNotify) {
  TempFile tmp("sl_futex_xproc");
  auto writer = lfring::SharedLatest<std::uint64_t>::create(tmp.path);

  pid_t pid = fork();
  ASSERT_NE(pid, -1) << "fork() failed";

  if (pid == 0) {
    // Child: open and wait for notification.
    auto reader = lfring::SharedLatest<std::uint64_t>::open(tmp.path);
    std::uint32_t seen = reader.notify_counter();

    bool woken = reader.wait_for_update(seen, 5000ms);
    if (!woken) _exit(1);

    std::uint64_t val = 0;
    if (!reader.try_read(val)) _exit(2);
    if (val != 99u) _exit(3);
    _exit(0);
  }

  // Parent: give child time to block, then write.
  std::this_thread::sleep_for(50ms);
  writer.write_and_notify(99u);

  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0) << "Child exit code: " << WEXITSTATUS(status);
}
