// Microburst Governor - checked arithmetic for externally influenced quantities.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace mbg {

/// Checked integer arithmetic. Every capacity, rate, counter, size and time unit that can be
/// influenced by evidence, policy or wire input goes through these helpers so that overflow is a
/// reported failure and never silent wraparound.

template <class T>
inline constexpr bool kIsIntegral = std::is_integral_v<T> && !std::is_same_v<T, bool>;

template <class T>
[[nodiscard]] constexpr std::optional<T> add_checked(T a, T b) noexcept {
  static_assert(kIsIntegral<T>, "add_checked requires a non-bool integral type");
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
      return std::nullopt;
    }
  } else {
    if (b > 0 && a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
      return std::nullopt;
    }
    if (b < 0 && a < static_cast<T>(std::numeric_limits<T>::min() - b)) {
      return std::nullopt;
    }
  }
  return static_cast<T>(a + b);
}

template <class T>
[[nodiscard]] constexpr std::optional<T> sub_checked(T a, T b) noexcept {
  static_assert(kIsIntegral<T>, "sub_checked requires a non-bool integral type");
  if constexpr (std::is_unsigned_v<T>) {
    if (b > a) {
      return std::nullopt;
    }
  } else {
    if (b < 0 && a > static_cast<T>(std::numeric_limits<T>::max() + b)) {
      return std::nullopt;
    }
    if (b > 0 && a < static_cast<T>(std::numeric_limits<T>::min() + b)) {
      return std::nullopt;
    }
  }
  return static_cast<T>(a - b);
}

template <class T>
[[nodiscard]] constexpr std::optional<T> mul_checked(T a, T b) noexcept {
  static_assert(kIsIntegral<T>, "mul_checked requires a non-bool integral type");
  if (a == 0 || b == 0) {
    return static_cast<T>(0);
  }
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) {
      return std::nullopt;
    }
  } else {
    if (a > 0) {
      if (b > 0) {
        if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) {
          return std::nullopt;
        }
      } else {
        if (b < static_cast<T>(std::numeric_limits<T>::min() / a)) {
          return std::nullopt;
        }
      }
    } else {
      if (b > 0) {
        if (a < static_cast<T>(std::numeric_limits<T>::min() / b)) {
          return std::nullopt;
        }
      } else {
        if (a < static_cast<T>(std::numeric_limits<T>::max() / b)) {
          return std::nullopt;
        }
      }
    }
  }
  return static_cast<T>(a * b);
}

template <class T>
[[nodiscard]] constexpr std::optional<T> div_checked(T a, T b) noexcept {
  static_assert(kIsIntegral<T>, "div_checked requires a non-bool integral type");
  if (b == 0) {
    return std::nullopt;
  }
  if constexpr (std::is_signed_v<T>) {
    if (a == std::numeric_limits<T>::min() && b == static_cast<T>(-1)) {
      return std::nullopt;
    }
  }
  return static_cast<T>(a / b);
}

/// Narrowing conversion that fails instead of truncating.
template <class To, class From>
[[nodiscard]] constexpr std::optional<To> narrow_checked(From value) noexcept {
  static_assert(kIsIntegral<To> && kIsIntegral<From>, "narrow_checked requires integral types");
  if constexpr (std::is_signed_v<From> == std::is_signed_v<To>) {
    if (value < static_cast<From>(std::numeric_limits<To>::min()) ||
        value > static_cast<From>(std::numeric_limits<To>::max())) {
      return std::nullopt;
    }
  } else if constexpr (std::is_signed_v<From>) {
    if (value < 0) {
      return std::nullopt;
    }
    using UFrom = std::make_unsigned_t<From>;
    if (static_cast<UFrom>(value) > static_cast<UFrom>(std::numeric_limits<To>::max())) {
      return std::nullopt;
    }
  } else {
    if (value > static_cast<From>(std::numeric_limits<To>::max())) {
      return std::nullopt;
    }
  }
  return static_cast<To>(value);
}

/// Saturating helpers for accounting counters where a clamp is preferable to a hard failure.
template <class T>
[[nodiscard]] constexpr T saturating_add(T a, T b) noexcept {
  const auto r = add_checked(a, b);
  if (r.has_value()) {
    return *r;
  }
  return b > 0 ? std::numeric_limits<T>::max() : std::numeric_limits<T>::min();
}

template <class T>
[[nodiscard]] constexpr T saturating_sub(T a, T b) noexcept {
  const auto r = sub_checked(a, b);
  if (r.has_value()) {
    return *r;
  }
  return b > 0 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
}

template <class T>
[[nodiscard]] constexpr T saturating_mul(T a, T b) noexcept {
  const auto r = mul_checked(a, b);
  if (r.has_value()) {
    return *r;
  }
  const bool negative = (a < 0) != (b < 0);
  if constexpr (std::is_unsigned_v<T>) {
    return std::numeric_limits<T>::max();
  } else {
    return negative ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
  }
}

/// Ceiling division that returns nullopt on a zero divisor or overflowing increment.
[[nodiscard]] constexpr std::optional<std::uint64_t> ceil_div_u64(std::uint64_t numerator,
                                                                 std::uint64_t denominator) noexcept {
  if (denominator == 0) {
    return std::nullopt;
  }
  const std::uint64_t quotient = numerator / denominator;
  if (numerator % denominator == 0) {
    return quotient;
  }
  return add_checked<std::uint64_t>(quotient, 1U);
}

[[nodiscard]] constexpr bool is_power_of_two(std::uint64_t value) noexcept {
  return value != 0 && (value & (value - 1U)) == 0;
}

/// Rounds down to the largest power of two not exceeding value (0 maps to 0).
[[nodiscard]] constexpr std::uint64_t floor_power_of_two(std::uint64_t value) noexcept {
  if (value == 0) {
    return 0;
  }
  std::uint64_t result = 1;
  while (result <= value / 2U) {
    result *= 2U;
  }
  return result;
}

}  // namespace mbg
