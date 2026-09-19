// Microburst Governor - monotonic logical tick arithmetic.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <optional>

namespace mbg {

/// A monotonic logical tick. Authoritative ordering never uses wall clock: evidence carries logical
/// ticks, and all comparisons use wrapping-safe distance so a counter rollover does not reorder
/// history.
class Tick {
 public:
  constexpr Tick() noexcept = default;
  constexpr explicit Tick(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Tick from_raw(std::uint64_t value) noexcept { return Tick(value); }
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(Tick, Tick) noexcept = default;

  /// Lexical ordering. Only meaningful when both ticks are inside the same un-wrapped range; use
  /// tick_delta() for authoritative ordering across a rollover boundary.
  friend constexpr auto operator<=>(Tick, Tick) noexcept = default;

 private:
  std::uint64_t value_{0};
};

/// Signed, wrapping-safe distance from the first tick to the second.
///
/// Returns a positive value when the second tick is after the first. Distances of 2^63 or more are
/// not representable and are clamped by the modular interpretation; callers that can observe such
/// spans must bound their windows, and the governor always does.
[[nodiscard]] constexpr std::int64_t tick_delta(Tick from, Tick to) noexcept {
  return static_cast<std::int64_t>(to.value() - from.value());
}

/// True when the second tick is strictly after the first in wrapping-safe order.
[[nodiscard]] constexpr bool tick_after(Tick from, Tick to) noexcept {
  return tick_delta(from, to) > 0;
}

/// Unsigned forward distance; empty when the second tick is not after the first.
[[nodiscard]] constexpr std::optional<std::uint64_t> tick_forward_distance(Tick from,
                                                                          Tick to) noexcept {
  const std::int64_t delta = tick_delta(from, to);
  if (delta < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(delta);
}

/// Tick addition that refuses to wrap.
[[nodiscard]] constexpr std::optional<Tick> tick_add(Tick base, std::uint64_t span) noexcept {
  const std::uint64_t value = base.value();
  if (value > ~std::uint64_t{0} - span) {
    return std::nullopt;
  }
  return Tick(value + span);
}

/// Tick subtraction that refuses to underflow.
[[nodiscard]] constexpr std::optional<Tick> tick_sub(Tick base, std::uint64_t span) noexcept {
  const std::uint64_t value = base.value();
  if (span > value) {
    return std::nullopt;
  }
  return Tick(value - span);
}

/// A bounded span in ticks.
struct TickSpan {
  std::uint64_t ticks{0};

  friend constexpr bool operator==(const TickSpan&, const TickSpan&) noexcept = default;
  friend constexpr auto operator<=>(const TickSpan&, const TickSpan&) noexcept = default;
};

/// Monotonicity tracker for a logical tick source.
///
/// Evidence is only accepted when ticks advance. A tick that goes backwards is classified as a
/// reorder (inside the reorder tolerance) or as a discontinuity, and a tick that wraps is reported
/// as a rollover so that detection can decide whether the window is still admissible.
class TickSource {
 public:
  enum class Observation : std::uint8_t {
    kFirst = 0,
    kAdvance = 1,
    kDuplicate = 2,
    kReorder = 3,
    kRollover = 4,
    kDiscontinuity = 5,
  };

  struct Step {
    Observation observation{Observation::kFirst};
    std::uint64_t forward_ticks{0};  ///< distance from the previous accepted tick
  };

  [[nodiscard]] static const char* to_string(Observation observation) noexcept;

  /// Feeds a tick. The reorder tolerance bounds how far backwards a tick may go and still be
  /// treated as a reorder rather than a discontinuity.
  [[nodiscard]] Step observe(Tick tick, std::uint64_t reorder_tolerance_ticks = 0) noexcept;

  [[nodiscard]] bool has_observation() const noexcept { return has_observation_; }
  [[nodiscard]] Tick last() const noexcept { return last_; }
  [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_; }
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
  [[nodiscard]] std::uint64_t observed_rollovers() const noexcept { return rollovers_; }

 private:
  Tick last_{};
  bool has_observation_{false};
  std::uint64_t accepted_{0};
  std::uint64_t rejected_{0};
  std::uint64_t rollovers_{0};
};

}  // namespace mbg
