// Microburst Governor - deterministic derived quantities.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/detect/quantities.hpp"

#include <limits>

namespace mbg {

RateQ16 compute_slope_q16(std::int64_t numerator, std::uint64_t denominator) noexcept {
  if (denominator == 0) {
    return 0;
  }
  const std::int64_t quotient = numerator / static_cast<std::int64_t>(denominator);
  const std::int64_t remainder = numerator % static_cast<std::int64_t>(denominator);
  constexpr std::int64_t kShiftLimit = std::numeric_limits<std::int64_t>::max() >> kRateFractionBits;
  if (quotient > kShiftLimit) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (quotient < -kShiftLimit) {
    return std::numeric_limits<std::int64_t>::min();
  }
  const std::int64_t whole = quotient << kRateFractionBits;
  // The remainder is smaller than the denominator, which is bounded by the tick span ceiling, so the
  // shifted remainder cannot overflow.
  const std::int64_t fraction =
      (remainder << kRateFractionBits) / static_cast<std::int64_t>(denominator);
  return static_cast<RateQ16>(whole + fraction);
}

namespace {

/// Exact floor((hi:lo) / divisor) for a 128-bit dividend whose quotient is known to fit in 64 bits,
/// using binary restoring division. The running remainder is reduced on every step, so it always
/// stays strictly below the divisor and no intermediate value can overflow.
std::uint64_t divide_128_by_64(std::uint64_t hi, std::uint64_t lo, std::uint64_t divisor) noexcept {
  std::uint64_t quotient = 0;
  std::uint64_t remainder = 0;
  for (int bit = 127; bit >= 0; --bit) {
    const std::uint64_t next =
        (bit >= 64) ? ((hi >> (bit - 64)) & 1ULL) : ((lo >> bit) & 1ULL);
    const std::uint64_t carry = remainder >> 63;
    remainder = (remainder << 1U) | next;
    if (carry != 0U || remainder >= divisor) {
      remainder -= divisor;
      quotient |= (1ULL << bit);
    }
  }
  return quotient;
}

/// 128-bit product of a 64-bit value and a small multiplier.
void multiply_128(std::uint64_t value, std::uint64_t multiplier, std::uint64_t& hi,
                  std::uint64_t& lo) noexcept {
  const std::uint64_t low_limb = value & 0xFFFFFFFFULL;
  const std::uint64_t high_limb = value >> 32U;
  const std::uint64_t p0 = low_limb * multiplier;
  const std::uint64_t p1 = high_limb * multiplier;
  lo = p0 + (p1 << 32U);
  hi = (p1 >> 32U) + (lo < p0 ? 1ULL : 0ULL);
}

}  // namespace

std::uint32_t compute_permille(std::uint64_t value, std::uint64_t limit) noexcept {
  if (limit == 0) {
    return value == 0 ? 0U : 1000U;
  }
  if (value >= limit) {
    return 1000U;
  }
  // value < limit, so the ratio is strictly below 1000 and the quotient always fits in 64 bits.
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  multiply_128(value, 1000U, hi, lo);
  const std::uint64_t ratio = divide_128_by_64(hi, lo, limit);
  return static_cast<std::uint32_t>(ratio > 1000U ? 1000U : ratio);
}

std::uint64_t modular_distance(std::uint64_t from, std::uint64_t to) noexcept {
  return to - from;
}

std::uint64_t window_counter_delta(std::uint64_t first, std::uint64_t last,
                                   bool reset_declared) noexcept {
  if (reset_declared) {
    return 0;
  }
  const CounterResolution resolution = resolve_counter(first, last, false);
  return resolution.usable ? resolution.delta : 0;
}

}  // namespace mbg
