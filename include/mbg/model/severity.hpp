// Microburst Governor - severity vocabulary.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace mbg {

/// Ordered severity of a detected microburst. The numeric ordering is part of the contract: rule
/// tables rely on it and it is persisted, so values are never reordered.
enum class SeverityClass : std::uint8_t {
  kNone = 0,
  kMinor = 1,
  kModerate = 2,
  kSevere = 3,
  kCritical = 4,
};

[[nodiscard]] const char* to_string(SeverityClass severity) noexcept;

[[nodiscard]] constexpr SeverityClass severity_max(SeverityClass a, SeverityClass b) noexcept {
  return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a : b;
}

[[nodiscard]] constexpr SeverityClass severity_min(SeverityClass a, SeverityClass b) noexcept {
  return static_cast<std::uint8_t>(a) <= static_cast<std::uint8_t>(b) ? a : b;
}

[[nodiscard]] constexpr bool severity_at_least(SeverityClass value, SeverityClass floor) noexcept {
  return static_cast<std::uint8_t>(value) >= static_cast<std::uint8_t>(floor);
}

[[nodiscard]] constexpr bool is_valid_severity(std::uint8_t raw) noexcept {
  return raw <= static_cast<std::uint8_t>(SeverityClass::kCritical);
}

/// Authority of a classification. A classification never claims more authority than the evidence
/// that produced it.
enum class EvidenceAuthority : std::uint8_t {
  /// Authoritative: full, fresh, contiguous evidence inside an admissible window.
  kAuthoritative = 0,
  /// Degraded: an explicit degraded policy permitted classification on incomplete evidence. The
  /// severity is capped and the classification is always surfaced as degraded.
  kDegraded = 1,
  /// No classification is possible. UNKNOWN stays UNKNOWN.
  kUnknown = 2,
};

[[nodiscard]] const char* to_string(EvidenceAuthority authority) noexcept;

/// Result of evaluating the detection rules for one stream at one tick.
enum class Classification : std::uint8_t {
  kUnknown = 0,
  kQuiescent = 1,
  kCandidate = 2,
  kMicroburst = 3,
  kDegradedMicroburst = 4,
  kRecovering = 5,
  kFenced = 6,
};

[[nodiscard]] const char* to_string(Classification classification) noexcept;

[[nodiscard]] constexpr bool is_positive(Classification classification) noexcept {
  return classification == Classification::kMicroburst ||
         classification == Classification::kDegradedMicroburst;
}

}  // namespace mbg
