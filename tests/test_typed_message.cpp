#include "lf_ring/shared_ring_buffer.hpp"
#include "lf_ring/typed_message.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
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

struct PriceUpdate {
  std::uint32_t id;
  double price;
};

} // namespace

namespace lfring {

template <>
struct MessageType<PriceUpdate> {
  static constexpr bool defined = true;
  static constexpr std::uint16_t value = 42;
};

} // namespace lfring

TEST(TypedMessage, TriviallyCopyable) {
  TempFile tmp("lfring_typed");
  auto ring = lfring::SharedRingBuffer::create(tmp.path, 4096);

  lfring::TypedMessageWriter<lfring::SharedRingBuffer> writer(ring);
  lfring::TypedMessageReader<lfring::SharedRingBuffer> reader(ring);

  PriceUpdate update{123, 456.75};
  ASSERT_TRUE(writer.try_push(update));

  PriceUpdate out{};
  ASSERT_TRUE(reader.try_pop(out));
  ASSERT_EQ(out.id, update.id);
  ASSERT_DOUBLE_EQ(out.price, update.price);
}

TEST(TypedMessage, StringPayload) {
  TempFile tmp("lfring_typed_str");
  auto ring = lfring::SharedRingBuffer::create(tmp.path, 4096);

  lfring::TypedMessageWriter<lfring::SharedRingBuffer> writer(ring);
  lfring::TypedMessageReader<lfring::SharedRingBuffer> reader(ring);

  std::string payload = "hello typed";
  ASSERT_TRUE(writer.try_push_typed(payload, 7));

  std::string out;
  ASSERT_TRUE(reader.try_pop_typed(7, out));
  ASSERT_EQ(out, payload);
}

TEST(TypedMessage, VectorPayload) {
  TempFile tmp("lfring_typed_vec");
  auto ring = lfring::SharedRingBuffer::create(tmp.path, 4096);

  lfring::TypedMessageWriter<lfring::SharedRingBuffer> writer(ring);
  lfring::TypedMessageReader<lfring::SharedRingBuffer> reader(ring);

  std::vector<std::uint32_t> values = {1, 2, 3, 4, 5};
  ASSERT_TRUE(writer.try_push_typed(values, 9));

  std::vector<std::uint32_t> out;
  ASSERT_TRUE(reader.try_pop_typed(9, out));
  ASSERT_EQ(out, values);
}

TEST(TypedMessage, SequenceNumbers) {
  TempFile tmp("lfring_typed_seq");
  auto ring = lfring::SharedRingBuffer::create(tmp.path, 4096);

  lfring::TypedMessageWriter<lfring::SharedRingBuffer> writer(ring);
  lfring::TypedMessageReader<lfring::SharedRingBuffer> reader(ring);

  ASSERT_TRUE(writer.try_push_typed(std::string("a"), 7));
  ASSERT_TRUE(writer.try_push_typed(std::string("b"), 7));

  lfring::MessageView view{};
  ASSERT_TRUE(reader.try_pop(view));
  ASSERT_EQ(view.sequence, 1u);
  ASSERT_TRUE(reader.try_pop(view));
  ASSERT_EQ(view.sequence, 2u);
}
