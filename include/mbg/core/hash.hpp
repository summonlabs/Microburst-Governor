// Microburst Governor - deterministic integer hashing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace mbg {

/// FNV-1a 64-bit. Deterministic on every platform (pure 64-bit integer arithmetic).
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view text) noexcept {
  std::uint64_t h = 1469598103934665603ULL;
  for (const char c : text) {
    h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    h *= 1099511628211ULL;
  }
  return h;
}

[[nodiscard]] constexpr std::uint64_t fnv1a64_seed(std::uint64_t seed,
                                                   std::string_view text) noexcept {
  std::uint64_t h = seed;
  for (const char c : text) {
    h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    h *= 1099511628211ULL;
  }
  return h;
}

/// splitmix64 finalizer: avalanche step used to fold heterogeneous fields together.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

[[nodiscard]] constexpr std::uint64_t hash_combine(std::uint64_t seed,
                                                   std::uint64_t value) noexcept {
  return mix64(seed ^ mix64(value));
}

/// Deterministic pseudo random generator (splitmix64). Used by seeded randomized tests and by
/// synthetic trace generation only; never by an authoritative decision path.
class DeterministicRng {
 public:
  constexpr explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] constexpr std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  [[nodiscard]] constexpr std::uint64_t bounded(std::uint64_t bound) noexcept {
    return bound == 0 ? 0 : (next() % bound);
  }

 private:
  std::uint64_t state_;
};

}  // namespace mbg
