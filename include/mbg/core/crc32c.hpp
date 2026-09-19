// Microburst Governor - CRC-32C (Castagnoli) integrity checking.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace mbg {
namespace detail {

/// Compile-time CRC-32C lookup table (reversed Castagnoli polynomial 0x1EDC6F41).
struct Crc32cTable {
  std::uint32_t entries[256]{};

  constexpr Crc32cTable() noexcept {
    for (std::uint32_t index = 0; index < 256U; ++index) {
      std::uint32_t crc = index;
      for (int bit = 0; bit < 8; ++bit) {
        crc = ((crc & 1U) != 0U) ? ((crc >> 1U) ^ 0x82F63B78U) : (crc >> 1U);
      }
      entries[index] = crc;
    }
  }
};

inline constexpr Crc32cTable kCrc32cTable{};

}  // namespace detail

/// Software CRC-32C. Deliberately table driven and platform independent so that a digest computed
/// on one host verifies on another and so that durable state carries a reproducible checksum.
class Crc32c {
 public:
  static constexpr std::uint32_t kInitial = 0xFFFFFFFFU;

  constexpr Crc32c() noexcept = default;

  constexpr void update(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = state_;
    for (const std::byte raw : bytes) {
      const std::uint8_t index =
          static_cast<std::uint8_t>((crc ^ static_cast<std::uint32_t>(raw)) & 0xFFU);
      crc = (crc >> 8U) ^ detail::kCrc32cTable.entries[index];
    }
    state_ = crc;
  }

  constexpr void update(const void* data, std::size_t length) noexcept {
    update(std::span<const std::byte>(static_cast<const std::byte*>(data), length));
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return state_ ^ kInitial; }

  [[nodiscard]] static constexpr std::uint32_t compute(std::span<const std::byte> bytes) noexcept {
    Crc32c crc;
    crc.update(bytes);
    return crc.value();
  }

 private:
  std::uint32_t state_{kInitial};
};

}  // namespace mbg
