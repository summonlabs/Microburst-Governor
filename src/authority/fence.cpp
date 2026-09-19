// Microburst Governor - authority binding and claim validation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/authority/fence.hpp"

#include "mbg/core/checked.hpp"

namespace mbg {

const char* to_string(ClaimVerdict verdict) noexcept {
  switch (verdict) {
    case ClaimVerdict::kAccepted: return "accepted";
    case ClaimVerdict::kVersionMismatch: return "version-mismatch";
    case ClaimVerdict::kEpochMismatch: return "epoch-mismatch";
    case ClaimVerdict::kBootMismatch: return "boot-mismatch";
    case ClaimVerdict::kNotBound: return "not-bound";
    case ClaimVerdict::kMalformed: return "malformed";
  }
  return "unknown";
}

ClaimVerdict validate_claim(const AuthorityState& current, const AuthorityState& claim,
                            std::uint16_t claimed_protocol,
                            std::uint16_t expected_protocol) noexcept {
  if (!current.valid()) {
    return ClaimVerdict::kNotBound;
  }
  if (claimed_protocol != expected_protocol) {
    return ClaimVerdict::kVersionMismatch;
  }
  if (!claim.epoch.valid() || claim.epoch.raw() == 0) {
    return ClaimVerdict::kMalformed;
  }
  if (claim.epoch != current.epoch) {
    return ClaimVerdict::kEpochMismatch;
  }
  if (claim.boot != current.boot) {
    return ClaimVerdict::kBootMismatch;
  }
  return ClaimVerdict::kAccepted;
}

Status advance_authority(const AuthorityState& previous, AuthorityState& next) noexcept {
  const auto epoch = add_checked<std::uint32_t>(previous.epoch.raw(), 1U);
  if (!epoch.has_value()) {
    return Status::failure(StatusCode::kOverflow, "authority epoch is exhausted");
  }
  const auto boot = add_checked<std::uint64_t>(previous.boot.raw(), 1U);
  if (!boot.has_value()) {
    return Status::failure(StatusCode::kOverflow, "boot generation is exhausted");
  }
  next = previous;
  next.epoch = Epoch::from_raw(*epoch);
  next.boot = BootId::from_raw(*boot);
  return Status{};
}

}  // namespace mbg
