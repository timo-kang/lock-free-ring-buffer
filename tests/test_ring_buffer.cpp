#include "lf_ring/shared_ring_buffer.hpp"
#include "lf_ring/shared_ring_buffer_mpmc.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
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

} // namespace

TEST(SharedRingBuffer, CreateOpenPushPop) {
  TempFile tmp("lfring_basic");
  auto ring = lfring::SharedRingBuffer::create(tmp.path, 4096);

  const char payload[] = "hello";
  ASSERT_TRUE(ring.try_push(payload, sizeof(payload), 7));

  std::vector<std::byte> out;
  std::uint16_t type = 0;
  ASSERT_TRUE(ring.try_pop(out, type));
  ASSERT_EQ(type, 7u);
  ASSERT_EQ(out.size(), sizeof(payload));
  ASSERT_EQ(std::memcmp(out.data(), payload, sizeof(payload)), 0);

  auto reopened = lfring::SharedRingBuffer::open(tmp.path);
  ASSERT_TRUE(reopened.try_push(payload, sizeof(payload), 9));
  ASSERT_TRUE(reopened.try_pop(out, type));
  ASSERT_EQ(type, 9u);
}

TEST(SharedRingBuffer, VariablePayloadSizes) {
  TempFile tmp("lfring_var");
  auto ring = lfring::SharedRingBuffer::create(tmp.path, 4096);

  std::vector<std::byte> small(3, std::byte{0x1});
  std::vector<std::byte> large(129, std::byte{0x2});

  ASSERT_TRUE(ring.try_push(small, 1));
  ASSERT_TRUE(ring.try_push(large, 2));

  std::vector<std::byte> out;
  std::uint16_t type = 0;

  ASSERT_TRUE(ring.try_pop(out, type));
  ASSERT_EQ(type, 1u);
  ASSERT_EQ(out.size(), small.size());
  ASSERT_EQ(std::memcmp(out.data(), small.data(), small.size()), 0);

  ASSERT_TRUE(ring.try_pop(out, type));
  ASSERT_EQ(type, 2u);
  ASSERT_EQ(out.size(), large.size());
  ASSERT_EQ(std::memcmp(out.data(), large.data(), large.size()), 0);
}

TEST(SharedRingBuffer, WrapAround) {
  TempFile tmp("lfring_wrap");
  auto ring = lfring::SharedRingBuffer::create(tmp.path, 256);

  std::vector<std::byte> payload(60, std::byte{0x5});
  std::uint16_t type = 0;
  std::vector<std::byte> out;

  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(ring.try_push(payload, static_cast<std::uint16_t>(i)));
    ASSERT_TRUE(ring.try_pop(out, type));
    ASSERT_EQ(type, static_cast<std::uint16_t>(i));
  }
}

TEST(SharedRingBuffer, MPSCStress) {
  TempFile tmp("lfring_mpsc");
  auto ring = lfring::SharedRingBuffer::create(tmp.path, 1 << 20);

  struct Message {
    std::uint32_t producer;
    std::uint32_t sequence;
  };

  constexpr std::uint32_t producers = 4;
  constexpr std::uint32_t per_producer = 5000;
  const std::uint32_t total = producers * per_producer;

  std::atomic<std::uint32_t> produced{0};
  std::atomic<std::uint32_t> consumed{0};
  std::vector<std::uint8_t> seen(total, 0);

  std::vector<std::thread> threads;
  threads.reserve(producers + 1);

  threads.emplace_back([&] {
    std::vector<std::byte> out;
    std::uint16_t type = 0;

    auto start = std::chrono::steady_clock::now();
    while (consumed.load(std::memory_order_relaxed) < total) {
      if (ring.try_pop(out, type)) {
        if (out.size() == sizeof(Message)) {
          Message msg{};
          std::memcpy(&msg, out.data(), sizeof(Message));
          std::uint32_t id = msg.producer * per_producer + msg.sequence;
          if (id < total) {
            seen[id] = 1;
          }
          consumed.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        std::this_thread::yield();
      }

      auto now = std::chrono::steady_clock::now();
      if (now - start > std::chrono::seconds(5)) {
        break;
      }
    }
  });

  for (std::uint32_t p = 0; p < producers; ++p) {
    threads.emplace_back([&, p] {
      for (std::uint32_t i = 0; i < per_producer; ++i) {
        Message msg{p, i};
        std::span<const std::byte> data(reinterpret_cast<const std::byte*>(&msg), sizeof(msg));
        while (!ring.try_push(data, 3)) {
          std::this_thread::yield();
        }
        produced.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  ASSERT_EQ(produced.load(), total);
  ASSERT_EQ(consumed.load(), total);

  for (std::uint32_t i = 0; i < total; ++i) {
    ASSERT_EQ(seen[i], 1) << "missing id " << i;
  }
}

TEST(SharedRingBufferMPMC, MPmcStress) {
  TempFile tmp("lfring_mpmc");
  auto ring = lfring::SharedRingBufferMPMC::create(tmp.path, 1 << 20);

  struct Message {
    std::uint32_t producer;
    std::uint32_t sequence;
  };

  constexpr std::uint32_t producers = 4;
  constexpr std::uint32_t consumers = 3;
  constexpr std::uint32_t per_producer = 4000;
  const std::uint32_t total = producers * per_producer;

  std::atomic<std::uint32_t> produced{0};
  std::atomic<std::uint32_t> consumed{0};
  std::vector<std::atomic_uint8_t> seen(total);
  for (auto& cell : seen) {
    cell.store(0, std::memory_order_relaxed);
  }

  std::vector<std::thread> threads;
  threads.reserve(producers + consumers);

  for (std::uint32_t c = 0; c < consumers; ++c) {
    threads.emplace_back([&] {
      std::vector<std::byte> out;
      std::uint16_t type = 0;
      auto start = std::chrono::steady_clock::now();
      while (consumed.load(std::memory_order_relaxed) < total) {
        if (ring.try_pop(out, type)) {
          if (out.size() == sizeof(Message)) {
            Message msg{};
            std::memcpy(&msg, out.data(), sizeof(Message));
            std::uint32_t id = msg.producer * per_producer + msg.sequence;
            if (id < total) {
              seen[id].fetch_add(1, std::memory_order_relaxed);
            }
            consumed.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          std::this_thread::yield();
        }

        auto now = std::chrono::steady_clock::now();
        if (now - start > std::chrono::seconds(5)) {
          break;
        }
      }
    });
  }

  for (std::uint32_t p = 0; p < producers; ++p) {
    threads.emplace_back([&, p] {
      for (std::uint32_t i = 0; i < per_producer; ++i) {
        Message msg{p, i};
        std::span<const std::byte> data(reinterpret_cast<const std::byte*>(&msg), sizeof(msg));
        while (!ring.try_push(data, 5)) {
          std::this_thread::yield();
        }
        produced.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  ASSERT_EQ(produced.load(), total);
  ASSERT_EQ(consumed.load(), total);

  for (std::uint32_t i = 0; i < total; ++i) {
    ASSERT_EQ(seen[i].load(std::memory_order_relaxed), 1u) << "missing or duplicate id " << i;
  }
}
