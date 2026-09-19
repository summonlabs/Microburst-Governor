// Microburst Governor - deterministic derived quantities.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "mbg/model/sample.hpp"

namespace mbg {

/// Exact Q16.16 slope of a numerator over a tick denominator.
///
/// Integer only: two runs over identical evidence produce the identical slope on every platform.
/// The result saturates at the representable extremes instead of wrapping; saturation is
/// unrepresentable in practice because depth and span are both bounded by policy.
[[nodiscard]] RateQ16 compute_slope_q16(std::int64_t numerator, std::uint64_t denominator) noexcept;

/// value/limit expressed in per-mille, clamped to [0, 1000]. A zero limit yields 1000 when value is
/// positive (the quantity is unmeasurable but present) and 0 when value is zero.
[[nodiscard]] std::uint32_t compute_permille(std::uint64_t value, std::uint64_t limit) noexcept;

/// Modular forward distance between two cumulative counters, ignoring the direction check. Used for
/// window aggregates where a declared reset already invalidated the interval.
[[nodiscard]] std::uint64_t modular_distance(std::uint64_t from, std::uint64_t to) noexcept;

/// Aggregate of a cumulative counter across a window, honouring rollover and refusing regressions.
[[nodiscard]] std::uint64_t window_counter_delta(std::uint64_t first, std::uint64_t last,
                                                 bool reset_declared) noexcept;

}  // namespace mbg
