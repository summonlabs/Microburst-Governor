// Microburst Governor - core primitives tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <limits>
#include <string>
#include <vector>

#include "mbg/core/bytes.hpp"
#include "mbg/core/checked.hpp"
#include "mbg/core/crc32c.hpp"
#include "mbg/core/strong_id.hpp"
#include "mbg/detect/detector.hpp"
#include "mbg/detect/quantities.hpp"
#include "mbg/detect/window.hpp"
#include "mbg/model/policy.hpp"
#include "mbg/model/sample.hpp"
#include "mbg/model/tick.hpp"
#include "test_framework.hpp"

using namespace mbg;

MBG_TEST(core, strong_id_domains_are_distinct) {
  const ResourceId resource = ResourceId::from_raw(7);
  const QueueId queue = QueueId::from_raw(7);
  MBG_CHECK(resource.raw() == queue.raw());
  MBG_CHECK(resource.valid());
  const ResourceId empty;
  MBG_CHECK(!empty.valid());
  const ResourceId other{resource};
  MBG_CHECK(other == resource);
  // Compile time separation is the real assertion: the following would not compile.
  //   queue = resource;
}

MBG_TEST(core, checked_arithmetic_reports_overflow) {
  MBG_CHECK(add_checked<std::uint64_t>(1, 2).value() == 3);
  MBG_CHECK(!add_checked<std::uint64_t>(std::numeric_limits<std::uint64_t>::max(), 1).has_value());
  MBG_CHECK(!sub_checked<std::uint64_t>(0, 1).has_value());
  MBG_CHECK(!mul_checked<std::uint64_t>(1ULL << 63, 2).has_value());
  MBG_CHECK(!div_checked<std::uint64_t>(1, 0).has_value());
  MBG_CHECK(!div_checked<std::int64_t>(std::numeric_limits<std::int64_t>::min(), -1).has_value());
  MBG_CHECK(saturating_add<std::uint64_t>(~0ULL, 5U) == ~0ULL);
  MBG_CHECK(saturating_sub<std::uint64_t>(0, 5U) == 0);
  MBG_CHECK(narrow_checked<std::uint16_t>(70000U).has_value() == false);
  MBG_CHECK(narrow_checked<std::uint16_t>(65535U).value() == 65535U);
  MBG_CHECK(ceil_div_u64(9, 4).value() == 3);
  MBG_CHECK(!ceil_div_u64(9, 0).has_value());
}

MBG_TEST(core, crc32c_known_answers) {
  const std::array<std::byte, 9> check = {std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
                                          std::byte{'4'}, std::byte{'5'}, std::byte{'6'},
                                          std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
  MBG_CHECK_EQ(Crc32c::compute(check), 0xE3069283U);
  MBG_CHECK_EQ(Crc32c::compute(std::span<const std::byte>{}), 0x00000000U);
  // Incremental use must match the one shot digest.
  Crc32c incremental;
  incremental.update(std::span<const std::byte>(check.data(), 4));
  incremental.update(std::span<const std::byte>(check.data() + 4, 5));
  MBG_CHECK_EQ(incremental.value(), Crc32c::compute(check));
}

MBG_TEST(core, byte_codec_round_trips_and_rejects_underflow) {
  std::vector<std::byte> buffer;
  ByteWriter writer(buffer);
  writer.u8(0x12);
  writer.u16(0x3456);
  writer.u32(0x789ABCDEU);
  writer.u64(0x0123456789ABCDEFULL);
  writer.i64(-5);
  writer.text("microburst");

  ByteReader reader(buffer);
  MBG_CHECK_EQ(reader.u8(), 0x12);
  MBG_CHECK_EQ(reader.u16(), 0x3456);
  MBG_CHECK_EQ(reader.u32(), 0x789ABCDEU);
  MBG_CHECK_EQ(reader.u64(), 0x0123456789ABCDEFULL);
  MBG_CHECK_EQ(reader.i64(), -5);
  MBG_CHECK_EQ(reader.text(), std::string("microburst"));
  MBG_CHECK(reader.ok());
  MBG_CHECK(reader.at_end());

  ByteReader truncated(buffer.data(), 3);
  static_cast<void>(truncated.u32());
  MBG_CHECK(!truncated.ok());
  MBG_CHECK_EQ(truncated.remaining(), 0U);
}

MBG_TEST(core, tick_arithmetic_handles_rollover) {
  const Tick near_max = Tick::from_raw(~std::uint64_t{0} - 2U);
  const Tick wrapped = Tick::from_raw(5);
  MBG_CHECK(tick_after(near_max, wrapped));
  MBG_CHECK_EQ(tick_delta(near_max, wrapped), 8);
  MBG_CHECK(!tick_after(wrapped, near_max));
  MBG_CHECK(tick_add(Tick::from_raw(~0ULL), 1U).has_value() == false);
  MBG_CHECK(tick_sub(Tick::from_raw(0), 1U).has_value() == false);
  MBG_CHECK_EQ(tick_forward_distance(wrapped, near_max).has_value(), false);
}

MBG_TEST(core, tick_source_classifies_observation) {
  TickSource source;
  MBG_CHECK(source.observe(Tick::from_raw(10)).observation == TickSource::Observation::kFirst);
  MBG_CHECK(source.observe(Tick::from_raw(11)).observation == TickSource::Observation::kAdvance);
  MBG_CHECK(source.observe(Tick::from_raw(11)).observation == TickSource::Observation::kDuplicate);
  MBG_CHECK(source.observe(Tick::from_raw(9), 4).observation == TickSource::Observation::kReorder);
  MBG_CHECK(source.observe(Tick::from_raw(3)).observation ==
            TickSource::Observation::kDiscontinuity);
  MBG_CHECK_EQ(source.observed_rollovers(), 0U);
}

MBG_TEST(core, counter_resolution_handles_rollover_and_reset) {
  const CounterResolution advance = resolve_counter(100, 150, false);
  MBG_CHECK(advance.usable);
  MBG_CHECK_EQ(advance.delta, 50U);
  MBG_CHECK(advance.disposition == CounterDisposition::kAdvance);

  const CounterResolution rollover = resolve_counter(~std::uint64_t{0} - 4U, 5, false);
  MBG_CHECK(rollover.usable);
  MBG_CHECK_EQ(rollover.delta, 10U);
  MBG_CHECK(rollover.disposition == CounterDisposition::kRollover);

  const CounterResolution regression = resolve_counter(1000, 10, false);
  MBG_CHECK(!regression.usable);
  MBG_CHECK(regression.disposition == CounterDisposition::kRegression);

  const CounterResolution declared = resolve_counter(1000, 10, true);
  MBG_CHECK(!declared.usable);
  MBG_CHECK(declared.disposition == CounterDisposition::kDeclaredReset);

  CounterCursor cursor;
  MBG_CHECK(!cursor.observe(10, false).usable);
  MBG_CHECK_EQ(cursor.observe(20, false).delta, 10U);
  MBG_CHECK_EQ(cursor.rollovers, 0U);
}

MBG_TEST(core, slope_and_permille_are_exact) {
  MBG_CHECK_EQ(compute_slope_q16(10, 1), static_cast<RateQ16>(10) << kRateFractionBits);
  MBG_CHECK_EQ(compute_slope_q16(-10, 2), static_cast<RateQ16>(-5) << kRateFractionBits);
  MBG_CHECK_EQ(compute_slope_q16(1, 0), 0);
  MBG_CHECK_EQ(compute_permille(1, 2), 500U);
  MBG_CHECK_EQ(compute_permille(0, 0), 0U);
  MBG_CHECK_EQ(compute_permille(5, 0), 1000U);
  MBG_CHECK_EQ(compute_permille(3, 2), 1000U);
  // The exact value for a numerator that cannot be scaled directly.
  MBG_CHECK_EQ(compute_permille(std::numeric_limits<std::uint64_t>::max() - 1U,
                                std::numeric_limits<std::uint64_t>::max()),
               999U);
}

MBG_TEST(core, explanation_is_bounded_and_digestible) {
  Explanation explanation;
  for (std::size_t i = 0; i < Explanation::kCapacity + 8U; ++i) {
    explanation.add(ReasonCode::kOnsetDepthCrossed, static_cast<std::int64_t>(i), 100, Tick{});
  }
  MBG_CHECK_EQ(explanation.size(), Explanation::kCapacity);
  MBG_CHECK(explanation.truncated());
  MBG_CHECK(explanation.digest() != 0U);
  MBG_CHECK(!explanation.render().empty());
  MBG_CHECK(explanation.contains(ReasonCode::kOnsetDepthCrossed));

  Explanation other;
  other.add(ReasonCode::kOnsetDepthCrossed, 0, 100, Tick{});
  MBG_CHECK(!(other == explanation));
}

MBG_TEST(core, sample_validation_rejects_contradictions) {
  Sample sample;
  MBG_CHECK(!validate_sample(sample).ok());  // no stream identity
  sample.stream.resource = ResourceId::from_raw(1);
  sample.stream.queue = QueueId::from_raw(1);
  MBG_CHECK(validate_sample(sample).ok());
  sample.capacity_bytes = 100;
  sample.occupancy_bytes = 200;
  MBG_CHECK(validate_sample(sample).code() == StatusCode::kConflict);
  sample.flags = SampleFlag::kCapacityChange;
  MBG_CHECK(validate_sample(sample).ok());
  sample.affected_class_count = 9;
  MBG_CHECK(!validate_sample(sample).ok());
}
