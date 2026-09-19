// Microburst Governor - evidence sample model.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

#include "mbg/core/status.hpp"
#include "mbg/limits.hpp"
#include "mbg/model/ids.hpp"
#include "mbg/model/tick.hpp"

namespace mbg {

/// Rates are exact integers in Q16.16 (units per tick scaled by 65536). Floating point never enters
/// the authoritative path, so classification is bit-for-bit reproducible across platforms.
using RateQ16 = std::int64_t;

inline constexpr int kRateFractionBits = 16;
inline constexpr std::int64_t kRateOne = 1LL << kRateFractionBits;

[[nodiscard]] constexpr RateQ16 rate_from_whole(std::uint64_t whole) noexcept {
  return static_cast<RateQ16>(whole) << kRateFractionBits;
}

[[nodiscard]] constexpr std::uint64_t rate_floor_whole(RateQ16 value) noexcept {
  return value <= 0 ? 0U : (static_cast<std::uint64_t>(value) >> kRateFractionBits);
}

/// Streaming properties of a single observation, as reported by the collector.
enum class SampleFlag : std::uint32_t {
  kNone = 0,
  kCounterReset = 1U << 0,      ///< producer declares its cumulative counters restarted
  kCapacityChange = 1U << 1,    ///< buffer capacity changed since the previous sample
  kSparse = 1U << 2,            ///< collector sampled on a degraded schedule
  kSynthetic = 1U << 3,         ///< sample originates from a synthetic trace, never physical
  kPartialInterval = 1U << 4,   ///< interval was clipped at a window boundary
};

[[nodiscard]] constexpr std::uint32_t to_bits(SampleFlag flag) noexcept {
  return static_cast<std::uint32_t>(flag);
}

[[nodiscard]] constexpr SampleFlag operator|(SampleFlag a, SampleFlag b) noexcept {
  return static_cast<SampleFlag>(to_bits(a) | to_bits(b));
}

[[nodiscard]] constexpr SampleFlag operator&(SampleFlag a, SampleFlag b) noexcept {
  return static_cast<SampleFlag>(to_bits(a) & to_bits(b));
}

[[nodiscard]] constexpr bool has_flag(SampleFlag value, SampleFlag flag) noexcept {
  return (to_bits(value) & to_bits(flag)) != 0U;
}

[[nodiscard]] const char* to_string(SampleFlag flag) noexcept;

/// Identity of the evidence stream. Resource plus queue is the minimum identity; the path refines it
/// when the collector can attribute a burst to a specific forwarding path.
struct StreamKey {
  ResourceId resource;
  QueueId queue;
  PathId path;

  friend constexpr bool operator==(const StreamKey&, const StreamKey&) noexcept = default;
  friend constexpr auto operator<=>(const StreamKey&, const StreamKey&) noexcept = default;

  [[nodiscard]] std::uint64_t hash() const noexcept;
  [[nodiscard]] bool valid() const noexcept { return resource.valid() && queue.valid(); }
};

struct StreamKeyHash {
  [[nodiscard]] std::size_t operator()(const StreamKey& key) const noexcept {
    return static_cast<std::size_t>(key.hash());
  }
};

/// A single high-resolution evidence observation.
struct Sample {
  StreamKey stream;
  Tick tick{};
  SampleSeq seq{};
  ResourceGeneration resource_generation{};

  std::uint64_t depth{0};             ///< queued items at the observation instant
  std::uint64_t occupancy_bytes{0};   ///< buffer bytes in use
  std::uint64_t capacity_bytes{0};    ///< buffer capacity in bytes (0 = unknown)

  RateQ16 ingress_rate_q16{0};        ///< offered rate over the interval
  RateQ16 egress_rate_q16{0};         ///< drained rate over the interval

  std::uint64_t ingress_total{0};     ///< cumulative ingress counter (may wrap)
  std::uint64_t egress_total{0};      ///< cumulative egress counter (may wrap)
  std::uint64_t drop_total{0};        ///< cumulative drop counter (may wrap)
  std::uint64_t mark_total{0};        ///< cumulative ECN/CNP mark counter (may wrap)

  SampleFlag flags{SampleFlag::kNone};
  ProvenanceId provenance{};

  std::array<ClassId, 4> affected_classes{};
  std::uint8_t affected_class_count{0};
};

/// Validates structural invariants of a sample before it can influence detection.
///
/// Rejects contradictions (occupancy above capacity without a declared capacity change, unknown
/// stream identity, class count above the fixed capacity) rather than silently repairing them.
[[nodiscard]] Status validate_sample(const Sample& sample) noexcept;

/// Disposition of a cumulative counter observation.
enum class CounterDisposition : std::uint8_t {
  kFirstObservation = 0,  ///< no previous value; delta is not defined
  kAdvance = 1,           ///< monotonic forward step
  kRollover = 2,          ///< modular wrap, still usable
  kRegression = 3,        ///< went backwards without a declared reset: not usable
  kDeclaredReset = 4,     ///< producer declared a restart: not usable
};

struct CounterResolution {
  std::uint64_t delta{0};
  CounterDisposition disposition{CounterDisposition::kFirstObservation};
  bool usable{false};
};

/// Resolves a cumulative counter step.
///
/// A backwards step whose modular distance is below 2^63 is treated as a counter rollover and stays
/// usable. A backwards step beyond that, or an explicit reset declaration, is a regression: the step
/// is reported as unusable so that no derivative is derived from it.
[[nodiscard]] CounterResolution resolve_counter(std::uint64_t previous, std::uint64_t current,
                                               bool reset_declared) noexcept;

/// Rolling previous-sample state used to derive increments without trusting the producer counters.
struct CounterCursor {
  std::uint64_t previous{0};
  bool has_previous{false};
  std::uint64_t rollovers{0};
  std::uint64_t regressions{0};
  std::uint64_t declared_resets{0};

  [[nodiscard]] CounterResolution observe(std::uint64_t current, bool reset_declared) noexcept;
};

}  // namespace mbg
