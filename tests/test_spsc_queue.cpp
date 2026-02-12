#include "lf_ring/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <thread>

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
  std::uint64_t tick;
  std::uint32_t id;
  std::uint32_t mode;
  double position[3];
  double velocity[3];
  float joints[32];
};

} // namespace

TEST(SPSCQueue, CreateAndOpen) {
  TempFile tmp("spscq_create");
  auto q = lfring::SPSCQueue<std::uint64_t, 16>::create(tmp.path);
  auto q2 = lfring::SPSCQueue<std::uint64_t, 16>::open(tmp.path);

  ASSERT_TRUE(q.empty());
  ASSERT_EQ(q.size(), 0u);
  ASSERT_EQ(q.capacity(), 16u);
}

TEST(SPSCQueue, PushPopSingle) {
  TempFile tmp("spscq_single");
  auto q = lfring::SPSCQueue<std::uint64_t, 16>::create(tmp.path);

  std::uint64_t val = 42;
  ASSERT_TRUE(q.try_push(val));

  std::uint64_t out = 0;
  ASSERT_TRUE(q.try_pop(out));
  ASSERT_EQ(out, 42u);
}

TEST(SPSCQueue, FullQueue) {
  TempFile tmp("spscq_full");
  auto q = lfring::SPSCQueue<std::uint32_t, 4>::create(tmp.path);

  for (std::uint32_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(q.try_push(i));
  }

  ASSERT_EQ(q.size(), 4u);
  ASSERT_FALSE(q.try_push(99u));

  std::uint32_t out = 0;
  ASSERT_TRUE(q.try_pop(out));
  ASSERT_EQ(out, 0u);

  ASSERT_TRUE(q.try_push(99u));
}

TEST(SPSCQueue, EmptyQueue) {
  TempFile tmp("spscq_empty");
  auto q = lfring::SPSCQueue<std::uint64_t, 8>::create(tmp.path);

  std::uint64_t out = 0;
  ASSERT_FALSE(q.try_pop(out));
  ASSERT_TRUE(q.empty());
}

TEST(SPSCQueue, WrapAround) {
  TempFile tmp("spscq_wrap");
  auto q = lfring::SPSCQueue<std::uint32_t, 4>::create(tmp.path);

  // Push and pop more items than capacity to force index wrapping.
  for (std::uint32_t i = 0; i < 20; ++i) {
    ASSERT_TRUE(q.try_push(i));
    std::uint32_t out = 0;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_EQ(out, i);
  }
}

TEST(SPSCQueue, FIFOOrdering) {
  TempFile tmp("spscq_fifo");
  auto q = lfring::SPSCQueue<std::uint32_t, 8>::create(tmp.path);

  for (std::uint32_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(q.try_push(i));
  }

  for (std::uint32_t i = 0; i < 8; ++i) {
    std::uint32_t out = 0;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_EQ(out, i);
  }
}

TEST(SPSCQueue, LargeType) {
  TempFile tmp("spscq_large");
  auto q = lfring::SPSCQueue<RobotState, 8>::create(tmp.path);

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

  ASSERT_TRUE(q.try_push(msg));

  RobotState out{};
  ASSERT_TRUE(q.try_pop(out));
  ASSERT_EQ(out.tick, 123u);
  ASSERT_EQ(out.id, 7u);
  ASSERT_EQ(out.mode, 2u);
  ASSERT_DOUBLE_EQ(out.position[0], 1.0);
  ASSERT_DOUBLE_EQ(out.velocity[2], 0.3);
  ASSERT_FLOAT_EQ(out.joints[31], 31.0f);
}

TEST(SPSCQueue, Stress) {
  TempFile tmp("spscq_stress");
  auto q = lfring::SPSCQueue<std::uint32_t, 1024>::create(tmp.path);

  constexpr std::uint32_t total = 200000;
  std::atomic<std::uint32_t> consumed{0};

  std::thread consumer([&] {
    std::uint32_t expected = 0;
    auto start = std::chrono::steady_clock::now();
    while (consumed.load(std::memory_order_relaxed) < total) {
      std::uint32_t out = 0;
      if (q.try_pop(out)) {
        ASSERT_EQ(out, expected);
        expected++;
        consumed.fetch_add(1, std::memory_order_relaxed);
      } else {
        std::this_thread::yield();
      }

      auto now = std::chrono::steady_clock::now();
      if (now - start > std::chrono::seconds(5)) {
        break;
      }
    }
  });

  std::thread producer([&] {
    for (std::uint32_t i = 0; i < total; ++i) {
      while (!q.try_push(i)) {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  ASSERT_EQ(consumed.load(), total);
}

TEST(SPSCQueue, SizeAndEmpty) {
  TempFile tmp("spscq_size");
  auto q = lfring::SPSCQueue<std::uint64_t, 8>::create(tmp.path);

  ASSERT_TRUE(q.empty());
  ASSERT_EQ(q.size(), 0u);

  q.try_push(1u);
  ASSERT_FALSE(q.empty());
  ASSERT_EQ(q.size(), 1u);

  q.try_push(2u);
  ASSERT_EQ(q.size(), 2u);

  std::uint64_t out = 0;
  q.try_pop(out);
  ASSERT_EQ(q.size(), 1u);

  q.try_pop(out);
  ASSERT_TRUE(q.empty());
}

TEST(SPSCQueue, OpenCapacityMismatch) {
  TempFile tmp("spscq_mismatch");
  lfring::SPSCQueue<std::uint32_t, 16>::create(tmp.path);

  ASSERT_THROW(
      (lfring::SPSCQueue<std::uint32_t, 32>::open(tmp.path)),
      std::runtime_error);
}
