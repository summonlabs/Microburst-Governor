// Microburst Governor - severity and classification rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/model/severity.hpp"

namespace mbg {

const char* to_string(SeverityClass severity) noexcept {
  switch (severity) {
    case SeverityClass::kNone: return "none";
    case SeverityClass::kMinor: return "minor";
    case SeverityClass::kModerate: return "moderate";
    case SeverityClass::kSevere: return "severe";
    case SeverityClass::kCritical: return "critical";
  }
  return "unknown";
}

const char* to_string(EvidenceAuthority authority) noexcept {
  switch (authority) {
    case EvidenceAuthority::kAuthoritative: return "authoritative";
    case EvidenceAuthority::kDegraded: return "degraded";
    case EvidenceAuthority::kUnknown: return "unknown";
  }
  return "unknown";
}

const char* to_string(Classification classification) noexcept {
  switch (classification) {
    case Classification::kUnknown: return "UNKNOWN";
    case Classification::kQuiescent: return "QUIESCENT";
    case Classification::kCandidate: return "CANDIDATE";
    case Classification::kMicroburst: return "BURST_DETECTED";
    case Classification::kDegradedMicroburst: return "BURST_DETECTED_DEGRADED";
    case Classification::kRecovering: return "RECOVERING";
    case Classification::kFenced: return "FENCED";
  }
  return "UNKNOWN";
}

}  // namespace mbg
