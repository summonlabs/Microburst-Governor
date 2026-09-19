// Microburst Governor - status vocabulary rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/core/status.hpp"

namespace mbg {

const char* to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::kOk: return "ok";
    case StatusCode::kInvalidArgument: return "invalid-argument";
    case StatusCode::kOutOfRange: return "out-of-range";
    case StatusCode::kStale: return "stale";
    case StatusCode::kUnauthorized: return "unauthorized";
    case StatusCode::kCorrupt: return "corrupt";
    case StatusCode::kTruncated: return "truncated";
    case StatusCode::kOversized: return "oversized";
    case StatusCode::kUnsupported: return "unsupported";
    case StatusCode::kNotFound: return "not-found";
    case StatusCode::kConflict: return "conflict";
    case StatusCode::kCapacityExceeded: return "capacity-exceeded";
    case StatusCode::kProtocol: return "protocol";
    case StatusCode::kIo: return "io";
    case StatusCode::kOverflow: return "overflow";
    case StatusCode::kCancelled: return "cancelled";
    case StatusCode::kInternal: return "internal";
  }
  return "unknown";
}

}  // namespace mbg
