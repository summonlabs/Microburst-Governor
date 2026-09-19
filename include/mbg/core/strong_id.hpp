// Microburst Governor - strongly typed identity primitives.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace mbg {

/// Compile-time description attached to every identity domain.
struct IdTag {
  static constexpr const char* kName = "mbg::IdTag";
};

/// An opaque, strongly typed identifier.
///
/// Distinct tags produce distinct, non-convertible types, so a ResourceId can never be
/// substituted for a QueueId or an Epoch. The default constructed value is the invalid
/// sentinel; there is deliberately no implicit conversion to or from the representation.
template <class Tag, class Rep = std::uint64_t>
class StrongId {
 public:
  using tag_type = Tag;
  using rep_type = Rep;

  static_assert(std::is_integral_v<Rep>, "StrongId requires an integral representation");
  static_assert(!std::is_same_v<Rep, bool>, "StrongId cannot use bool as representation");

  static constexpr Rep kInvalid = static_cast<Rep>(~static_cast<std::make_unsigned_t<Rep>>(0));

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongId from_raw(Rep value) noexcept { return StrongId(value); }

  [[nodiscard]] constexpr Rep raw() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != kInvalid; }

  /// Explicit invalidation; the only supported way to clear an identity.
  constexpr void reset() noexcept { value_ = kInvalid; }

  friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
  friend constexpr auto operator<=>(StrongId, StrongId) noexcept = default;

 private:
  Rep value_{kInvalid};
};

}  // namespace mbg

namespace std {

template <class Tag, class Rep>
struct hash<mbg::StrongId<Tag, Rep>> {
  [[nodiscard]] std::size_t operator()(const mbg::StrongId<Tag, Rep>& id) const noexcept {
    return hash<Rep>{}(id.raw());
  }
};

}  // namespace std
