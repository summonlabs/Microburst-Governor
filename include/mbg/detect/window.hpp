// Microburst Governor - bounded evidence window.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

#include "mbg/limits.hpp"
#include "mbg/model/policy.hpp"
#include "mbg/model/sample.hpp"
#include "mbg/model/tick.hpp"

namespace mbg {

/// Result of offering a sample to a window.
enum class WindowAccept : std::uint8_t {
  kAccepted = 0,
  kDuplicate = 1,
  kReordered = 2,
  kDiscontinuity = 3,
  kCapacityChanged = 4,
};

[[nodiscard]] const char* to_string(WindowAccept accept) noexcept;

/// A bounded, ordered evidence window for one stream.
///
/// The window keeps the most recent accepted samples in tick order. Duplicate, reordered and
/// discontinuous samples are counted but never inserted, so the derivative is always computed over a
/// monotonic tick sequence. Nothing here allocates after construction.
class EvidenceWindow {
 public:
  EvidenceWindow() = default;
  explicit EvidenceWindow(std::size_t capacity);

  void reset(std::size_t capacity);
  void clear() noexcept;

  [[nodiscard]] WindowAccept offer(const Sample& sample, std::uint64_t reorder_tolerance_ticks);

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  /// 0 is the oldest retained sample, size()-1 the newest.
  [[nodiscard]] const Sample& at(std::size_t index) const noexcept;
  [[nodiscard]] const Sample& newest() const noexcept { return at(size_ - 1); }
  [[nodiscard]] const Sample& oldest() const noexcept { return at(0); }
  [[nodiscard]] const Sample* previous() const noexcept {
    return size_ >= 2 ? &at(size_ - 2) : nullptr;
  }

  /// Cumulative counters describing everything the window refused or could not trust.
  [[nodiscard]] std::uint64_t duplicates() const noexcept { return duplicates_; }
  [[nodiscard]] std::uint64_t reordered() const noexcept { return reordered_; }
  [[nodiscard]] std::uint64_t discontinuities() const noexcept { return discontinuities_; }
  [[nodiscard]] std::uint64_t counter_resets() const noexcept { return counter_resets_; }
  [[nodiscard]] std::uint64_t counter_rollovers() const noexcept { return counter_rollovers_; }
  [[nodiscard]] std::uint64_t capacity_changes() const noexcept { return capacity_changes_; }
  [[nodiscard]] std::uint64_t sparse_samples() const noexcept { return sparse_samples_; }
  [[nodiscard]] std::uint64_t synthetic_samples() const noexcept { return synthetic_samples_; }
  [[nodiscard]] std::uint64_t accepted_total() const noexcept { return accepted_total_; }
  /// Largest gap ever observed on this stream, over its whole life. Reported for diagnostics; the
  /// admissibility rule uses the largest gap still inside the window instead.
  [[nodiscard]] std::uint64_t max_observed_gap() const noexcept { return max_observed_gap_; }
  [[nodiscard]] std::uint64_t last_gap() const noexcept { return last_gap_; }

  struct Metrics {
    std::uint64_t samples{0};
    std::uint64_t expected_samples{0};
    std::uint32_t completeness_permille{0};
    std::uint64_t span_ticks{0};

    std::uint64_t latest_depth{0};
    std::uint64_t peak_depth{0};
    std::uint64_t latest_occupancy_bytes{0};
    std::uint64_t peak_occupancy_bytes{0};
    std::uint64_t capacity_bytes{0};
    bool capacity_known{false};
    bool capacity_changed{false};

    RateQ16 window_slope_q16{0};
    RateQ16 recent_slope_q16{0};
    RateQ16 imbalance_q16{0};

    std::uint64_t drop_delta{0};
    std::uint64_t mark_delta{0};
    std::uint64_t ingress_delta{0};
    std::uint64_t egress_delta{0};

    std::uint64_t max_gap_ticks{0};
    bool sparse{false};
    bool synthetic{false};
    ProvenanceId provenance{};
    std::uint32_t provenance_count{0};
    Tick window_start{};
    Tick window_end{};
  };

  /// Computes the window metrics under the supplied detection policy. O(samples in window).
  [[nodiscard]] Metrics metrics(const DetectionPolicy& policy) const noexcept;

  /// Number of ticks between the newest sample and the supplied tick, or empty when the window is
  /// empty. Uses wrapping-safe ordering.
  [[nodiscard]] std::uint64_t staleness_ticks(Tick now) const noexcept;

 private:
  std::vector<Sample> samples_{};
  std::size_t capacity_{0};
  std::size_t head_{0};  ///< index of the oldest sample
  std::size_t size_{0};

  std::uint64_t duplicates_{0};
  std::uint64_t reordered_{0};
  std::uint64_t discontinuities_{0};
  std::uint64_t counter_resets_{0};
  std::uint64_t counter_rollovers_{0};
  std::uint64_t capacity_changes_{0};
  std::uint64_t sparse_samples_{0};
  std::uint64_t synthetic_samples_{0};
  std::uint64_t accepted_total_{0};
  std::uint64_t max_observed_gap_{0};
  std::uint64_t last_gap_{0};

  TickSource tick_source_{};
};

}  // namespace mbg
