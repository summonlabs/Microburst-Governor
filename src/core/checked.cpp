// Microburst Governor - translation unit anchoring the checked arithmetic helpers.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/core/checked.hpp"

namespace mbg {

// The checked arithmetic helpers are constexpr and fully defined in the header; this anchor keeps
// the header honest (it must compile standalone) and gives the build a place to hang any future
// out-of-line specialisation or limit table.

static_assert(add_checked<std::uint64_t>(1, 2).value() == 3);
static_assert(!add_checked<std::uint64_t>(~0ULL, 1).has_value());
static_assert(!sub_checked<std::uint64_t>(0, 1).has_value());
static_assert(!mul_checked<std::int64_t>(std::numeric_limits<std::int64_t>::max(), 2).has_value());
static_assert(!div_checked<std::int64_t>(0, 0).has_value());

}  // namespace mbg
