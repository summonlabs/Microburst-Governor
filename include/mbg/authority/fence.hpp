// Microburst Governor - authority binding and claim validation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "mbg/core/status.hpp"
#include "mbg/model/ids.hpp"

namespace mbg {

/// The authority a process acts under: which epoch, which incarnation, which boot generation.
struct AuthorityState {
  Epoch epoch{};
  IncarnationId incarnation{};
  BootId boot{};

  [[nodiscard]] bool valid() const noexcept {
    return epoch.valid() && epoch.raw() != 0 && incarnation.valid() && boot.valid();
  }

  friend constexpr bool operator==(const AuthorityState&, const AuthorityState&) noexcept = default;
};

/// Why a claim was refused.
enum class ClaimVerdict : std::uint8_t {
  kAccepted = 0,
  kVersionMismatch = 1,
  kEpochMismatch = 2,
  kBootMismatch = 3,
  kNotBound = 4,
  kMalformed = 5,
};

[[nodiscard]] const char* to_string(ClaimVerdict verdict) noexcept;

/// Validates a claim against the authority in force.
///
/// Only the fencing dimensions are compared: protocol version, authority epoch and boot generation.
/// The claimant incarnation is deliberately not compared, because a worker is always a different
/// incarnation from the coordinator that admits it; the incarnation is recorded for fencing a
/// specific process rather than used as an admission test.
///
/// A claim is accepted only when every compared component matches exactly. There is no "close
/// enough" path: a stale epoch or a previous boot generation is refused, which is what stops a
/// restarted process from inheriting live authority.
[[nodiscard]] ClaimVerdict validate_claim(const AuthorityState& current, const AuthorityState& claim,
                                          std::uint16_t claimed_protocol,
                                          std::uint16_t expected_protocol) noexcept;

/// Advances a boot generation and an epoch together, refusing to wrap.
[[nodiscard]] Status advance_authority(const AuthorityState& previous, AuthorityState& next) noexcept;

}  // namespace mbg
