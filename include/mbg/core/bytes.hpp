// Microburst Governor - bounds checked big-endian byte codec.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mbg {

/// Append-only big-endian writer. Encoding is fixed-width and byte ordered so that durable state
/// and wire frames are identical across compilers, endianness and architectures.
class ByteWriter {
 public:
  explicit ByteWriter(std::vector<std::byte>& out) noexcept : out_(&out) {}

  void u8(std::uint8_t value) { out_->push_back(static_cast<std::byte>(value)); }

  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    u8(static_cast<std::uint8_t>(value & 0xFFU));
  }

  void u32(std::uint32_t value) {
    u16(static_cast<std::uint16_t>((value >> 16U) & 0xFFFFU));
    u16(static_cast<std::uint16_t>(value & 0xFFFFU));
  }

  void u64(std::uint64_t value) {
    u32(static_cast<std::uint32_t>((value >> 32U) & 0xFFFFFFFFULL));
    u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
  }

  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  void raw(const void* data, std::size_t length) {
    if (length == 0) {
      return;
    }
    const auto* first = static_cast<const std::byte*>(data);
    out_->insert(out_->end(), first, first + length);
  }

  void raw(std::span<const std::byte> data) { raw(data.data(), data.size()); }

  void text(const std::string& value) {
    u32(static_cast<std::uint32_t>(value.size()));
    raw(value.data(), value.size());
  }

  void fill_zero(std::size_t count) { out_->insert(out_->end(), count, std::byte{0}); }

  [[nodiscard]] std::size_t size() const noexcept { return out_->size(); }

 private:
  std::vector<std::byte>* out_;
};

/// Bounds checked big-endian reader. Underflow latches ok() to false; every accessor returns a
/// neutral value once the reader has failed, so callers check ok() once at the end of a decode.
class ByteReader {
 public:
  ByteReader(const std::byte* data, std::size_t length) noexcept : data_(data), length_(length) {}
  explicit ByteReader(std::span<const std::byte> bytes) noexcept
      : data_(bytes.data()), length_(bytes.size()) {}
  explicit ByteReader(const std::vector<std::byte>& bytes) noexcept
      : data_(bytes.data()), length_(bytes.size()) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? length_ - offset_ : 0; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool at_end() const noexcept { return ok_ && offset_ == length_; }

  void fail() noexcept { ok_ = false; }

  std::uint8_t u8() {
    if (!ensure(1)) {
      return 0;
    }
    const auto value = static_cast<std::uint8_t>(data_[offset_]);
    offset_ += 1;
    return value;
  }

  std::uint16_t u16() {
    const auto hi = static_cast<std::uint16_t>(u8());
    const auto lo = static_cast<std::uint16_t>(u8());
    return static_cast<std::uint16_t>((hi << 8U) | lo);
  }

  std::uint32_t u32() {
    const auto hi = static_cast<std::uint32_t>(u16());
    const auto lo = static_cast<std::uint32_t>(u16());
    return (hi << 16U) | lo;
  }

  std::uint64_t u64() {
    const auto hi = static_cast<std::uint64_t>(u32());
    const auto lo = static_cast<std::uint64_t>(u32());
    return (hi << 32U) | lo;
  }

  std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

  /// Borrowed view of the next length bytes; null on underflow.
  [[nodiscard]] const std::byte* raw(std::size_t length) {
    if (!ensure(length)) {
      return nullptr;
    }
    const std::byte* first = data_ + offset_;
    offset_ += length;
    return first;
  }

  [[nodiscard]] std::span<const std::byte> span(std::size_t length) {
    const std::byte* first = raw(length);
    return first == nullptr ? std::span<const std::byte>{} : std::span<const std::byte>(first, length);
  }

  [[nodiscard]] bool skip(std::size_t length) {
    if (!ensure(length)) {
      return false;
    }
    offset_ += length;
    return true;
  }

  [[nodiscard]] std::string text() {
    const std::uint32_t length = u32();
    const std::byte* first = raw(length);
    if (first == nullptr) {
      return std::string{};
    }
    return std::string(reinterpret_cast<const char*>(first), length);
  }

 private:
  [[nodiscard]] bool ensure(std::size_t count) noexcept {
    if (!ok_) {
      return false;
    }
    if (count > length_ - offset_) {
      ok_ = false;
      return false;
    }
    return true;
  }

  const std::byte* data_{nullptr};
  std::size_t length_{0};
  std::size_t offset_{0};
  bool ok_{true};
};

}  // namespace mbg
