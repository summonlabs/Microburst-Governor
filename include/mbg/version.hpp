// Microburst Governor - version and build identity.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>

#define MBG_VERSION_MAJOR 1
#define MBG_VERSION_MINOR 0
#define MBG_VERSION_PATCH 0
#define MBG_VERSION_STRING "1.0.0"

namespace mbg {

/// Semantic version triple of the runtime that produced a piece of state.
struct Version {
  std::uint16_t major{0};
  std::uint16_t minor{0};
  std::uint16_t patch{0};

  friend constexpr bool operator==(const Version&, const Version&) noexcept = default;
  friend constexpr auto operator<=>(const Version&, const Version&) noexcept = default;
};

inline constexpr Version kRuntimeVersion{MBG_VERSION_MAJOR, MBG_VERSION_MINOR, MBG_VERSION_PATCH};

/// On-disk / on-wire state format version. Bumped only for incompatible layout changes.
inline constexpr std::uint16_t kStateFormatVersion = 1;

/// Wire protocol version for the framed coordinator/worker transport.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

/// Human readable runtime version string.
[[nodiscard]] const char* version_string() noexcept;

}  // namespace mbg
