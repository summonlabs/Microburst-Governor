// Microburst Governor - status and result plumbing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <utility>

namespace mbg {

/// Stable, comparable failure vocabulary. Codes are part of the public contract: they appear in
/// CLI output, in diagnostics and in test assertions, so they are never renumbered.
enum class StatusCode : std::uint8_t {
  kOk = 0,
  kInvalidArgument = 1,
  kOutOfRange = 2,
  kStale = 3,
  kUnauthorized = 4,
  kCorrupt = 5,
  kTruncated = 6,
  kOversized = 7,
  kUnsupported = 8,
  kNotFound = 9,
  kConflict = 10,
  kCapacityExceeded = 11,
  kProtocol = 12,
  kIo = 13,
  kOverflow = 14,
  kCancelled = 15,
  kInternal = 16,
};

[[nodiscard]] const char* to_string(StatusCode code) noexcept;

/// A status carries a code plus a static detail string. Detail strings are compile-time literals so
/// that failure paths never allocate and never truncate.
class Status {
 public:
  constexpr Status() noexcept = default;
  constexpr Status(StatusCode code, const char* detail) noexcept : code_(code), detail_(detail) {}

  // A default constructed Status is success, so there is deliberately no static ok() factory:
  // a static and a non-static member with the same parameter list cannot coexist, and the member
  // predicate is the one callers need.
  [[nodiscard]] static constexpr Status failure(StatusCode code, const char* detail) noexcept {
    return Status(code, detail);
  }

  [[nodiscard]] constexpr bool ok() const noexcept { return code_ == StatusCode::kOk; }
  [[nodiscard]] constexpr StatusCode code() const noexcept { return code_; }
  [[nodiscard]] constexpr const char* detail() const noexcept { return detail_; }

  friend constexpr bool operator==(const Status&, const Status&) noexcept = default;

 private:
  StatusCode code_{StatusCode::kOk};
  const char* detail_{""};
};

/// Result of an operation that produces a value.
template <class T>
class Result {
 public:
  Result(T value) noexcept : value_(std::move(value)), status_(Status{}) {}
  Result(Status status) noexcept : status_(status) {}

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

  /// Precondition: ok(). Returns the produced value.
  [[nodiscard]] T& value() noexcept { return value_; }
  [[nodiscard]] const T& value() const noexcept { return value_; }

 private:
  T value_{};
  Status status_;
};

}  // namespace mbg
