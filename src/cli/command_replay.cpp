// Microburst Governor - mbgctl replay.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>
#include <vector>

#include "cli_common.hpp"
#include "mbg/governor.hpp"
#include "mbg/synthetic/generator.hpp"

namespace mbg::cli {

int run_replay(const Arguments& arguments) {
  const std::optional<std::string> trace_path = arguments.value("--trace");
  if (!trace_path.has_value()) {
    std::fputs("mbgctl replay: --trace FILE is required\n", stderr);
    return 2;
  }
  std::string text;
  if (!read_file(*trace_path, limits::kMaxSnapshotBytes, text)) {
    std::fputs("mbgctl replay: trace file is unreadable or exceeds the bound\n", stderr);
    return 2;
  }
  std::vector<Sample> samples;
  const Status decoded = synthetic::decode_trace(text, samples, 1U << 22);
  if (!decoded.ok()) {
    std::fprintf(stderr, "mbgctl replay: trace rejected: %s (%s)\n", decoded.detail(),
                 to_string(decoded.code()));
    return 2;
  }

  GovernorConfig config;
  config.policy = make_default_policy();
  if (const auto window = arguments.number("--policy-window"); window.has_value()) {
    config.policy.detection.window_capacity = *window;
  }
  if (const auto interventions = arguments.number("--interventions"); interventions.has_value()) {
    config.enable_interventions = *interventions != 0;
  }
  config.incarnation = IncarnationId::from_raw(0xC0FFEEULL);
  config.boot = BootId::from_raw(1);

  Governor governor(config);
  Tick highest{};
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  for (const Sample& sample : samples) {
    const IngestOutcome outcome = governor.ingest(sample);
    if (outcome.status.ok()) {
      accepted += 1;
    } else {
      rejected += 1;
    }
    if (sample.tick.value() > highest.value()) {
      highest = sample.tick;
    }
  }
  static_cast<void>(governor.advance(Tick::from_raw(highest.value() + 1024U)));

  const GovernorStats stats = governor.stats();
  std::printf("replay: samples=%zu accepted=%llu rejected=%llu\n", samples.size(),
              static_cast<unsigned long long>(accepted),
              static_cast<unsigned long long>(rejected));
  std::printf(
      "replay: streams=%llu decisions=%llu episodes_opened=%llu episodes_closed=%llu "
      "episodes_fenced=%llu interventions_requested=%llu interventions_revoked=%llu\n",
      static_cast<unsigned long long>(stats.streams),
      static_cast<unsigned long long>(stats.decisions),
      static_cast<unsigned long long>(stats.episodes_opened),
      static_cast<unsigned long long>(stats.episodes_closed),
      static_cast<unsigned long long>(stats.episodes_fenced),
      static_cast<unsigned long long>(stats.interventions_requested),
      static_cast<unsigned long long>(stats.interventions_revoked));

  if (arguments.has("--events")) {
    for (const BurstEvent& event : governor.event_history()) {
      std::printf("history: %s\n", render_event_line(event).c_str());
    }
    for (const BurstEvent& event : governor.open_episodes()) {
      std::printf("open: %s\n", render_event_line(event).c_str());
    }
    for (const InterventionIntent& intent : governor.intervention_history()) {
      std::printf("intervention: %s\n", render_intervention_line(intent).c_str());
    }
  }
  return 0;
}

}  // namespace mbg::cli
