#pragma once

#include "lf_ring/ring_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace lfring {

template <typename T>
struct MessageType {
  static constexpr bool defined = false;
  static constexpr std::uint16_t value = 0;
};

template <typename T, typename Enable = void>
struct MessageCodec {
  static bool encode(const T&, std::vector<std::byte>&) {
    static_assert(sizeof(T) == 0, "MessageCodec<T>::encode must be specialized for this type");
    return false;
  }

  static bool decode(std::span<const std::byte>, T&) {
    static_assert(sizeof(T) == 0, "MessageCodec<T>::decode must be specialized for this type");
    return false;
  }
};

template <typename T>
struct MessageCodec<T, std::enable_if_t<std::is_trivially_copyable_v<T>>> {
  static bool encode(const T& value, std::vector<std::byte>& out) {
    out.resize(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return true;
  }

  static bool decode(std::span<const std::byte> payload, T& out) {
    if (payload.size() != sizeof(T)) {
      return false;
    }
    std::memcpy(&out, payload.data(), sizeof(T));
    return true;
  }
};

template <>
struct MessageCodec<std::string> {
  static bool encode(const std::string& value, std::vector<std::byte>& out) {
    out.resize(value.size());
    if (!value.empty()) {
      std::memcpy(out.data(), value.data(), value.size());
    }
    return true;
  }

  static bool decode(std::span<const std::byte> payload, std::string& out) {
    out.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
    return true;
  }
};

template <typename T>
struct MessageCodec<std::vector<T>, std::enable_if_t<std::is_trivially_copyable_v<T>>> {
  static bool encode(const std::vector<T>& value, std::vector<std::byte>& out) {
    std::size_t bytes = value.size() * sizeof(T);
    out.resize(bytes);
    if (bytes > 0) {
      std::memcpy(out.data(), value.data(), bytes);
    }
    return true;
  }

  static bool decode(std::span<const std::byte> payload, std::vector<T>& out) {
    if (payload.size() % sizeof(T) != 0) {
      return false;
    }
    std::size_t count = payload.size() / sizeof(T);
    out.resize(count);
    if (payload.size() > 0) {
      std::memcpy(out.data(), payload.data(), payload.size());
    }
    return true;
  }
};

struct MessageView {
  std::uint16_t type = 0;
  std::uint64_t sequence = 0;
  std::span<const std::byte> payload{};
};

template <typename Ring>
class TypedMessageWriter {
public:
  explicit TypedMessageWriter(Ring& ring) : ring_(ring) {}

  bool try_push_bytes(std::uint16_t type, std::span<const std::byte> payload) {
    return ring_.try_push(payload, type);
  }

  template <typename T>
  bool try_push_typed(const T& value, std::uint16_t type) {
    if constexpr (std::is_trivially_copyable_v<T>) {
      return ring_.try_push(&value, static_cast<std::uint32_t>(sizeof(T)), type);
    } else {
      scratch_.clear();
      if (!MessageCodec<T>::encode(value, scratch_)) {
        return false;
      }
      return ring_.try_push(scratch_, type);
    }
  }

  template <typename T>
  bool try_push(const T& value) {
    static_assert(MessageType<T>::defined, "MessageType<T> specialization required for try_push<T>");
    return try_push_typed(value, MessageType<T>::value);
  }

private:
  Ring& ring_;
  std::vector<std::byte> scratch_;
};

template <typename Ring>
class TypedMessageReader {
public:
  explicit TypedMessageReader(Ring& ring) : ring_(ring) {}

  bool try_pop(MessageView& view) {
    if (!ring_.try_pop(buffer_, type_, sequence_)) {
      return false;
    }
    view.type = type_;
    view.sequence = sequence_;
    view.payload = std::span<const std::byte>(buffer_.data(), buffer_.size());
    return true;
  }

  template <typename T>
  bool try_pop_typed(std::uint16_t expected_type, T& out) {
    MessageView view;
    if (!try_pop(view)) {
      return false;
    }
    if (view.type != expected_type) {
      return false;
    }
    return decode_payload(view.payload, out);
  }

  template <typename T>
  bool try_pop(T& out) {
    static_assert(MessageType<T>::defined, "MessageType<T> specialization required for try_pop<T>");
    return try_pop_typed(MessageType<T>::value, out);
  }

private:
  template <typename T>
  bool decode_payload(std::span<const std::byte> payload, T& out) {
    if constexpr (std::is_trivially_copyable_v<T>) {
      if (payload.size() != sizeof(T)) {
        return false;
      }
      std::memcpy(&out, payload.data(), sizeof(T));
      return true;
    } else {
      return MessageCodec<T>::decode(payload, out);
    }
  }

  Ring& ring_;
  std::vector<std::byte> buffer_;
  std::uint16_t type_ = 0;
  std::uint64_t sequence_ = 0;
};

} // namespace lfring
