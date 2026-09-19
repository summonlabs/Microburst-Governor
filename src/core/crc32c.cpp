// Microburst Governor - CRC-32C anchor and known-answer self checks.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/core/crc32c.hpp"

#include <array>

namespace mbg {
namespace {

constexpr std::array<std::byte, 9> kCheck123456789 = {
    std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
    std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};

// Known-answer vectors pin the polynomial so a future refactor cannot silently change the digest
// of durable state. 0xE3069283 is the published CRC-32C of "123456789".
static_assert(Crc32c::compute(std::span<const std::byte>{}) == 0x00000000U);
static_assert(Crc32c::compute(kCheck123456789) == 0xE3069283U);

}  // namespace
}  // namespace mbg
