// Independent downstream consumer smoke test.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This program is deliberately not part of the main build: it is configured against an installed
// package with find_package(mbg CONFIG REQUIRED) to prove that the export, the headers and the
// target interface are all usable from outside the source tree.

#include <cstdio>
#include <vector>

#include "mbg/governor.hpp"
#include "mbg/synthetic/generator.hpp"
#include "mbg/version.hpp"

int main() {
  mbg::GovernorConfig config;
  config.policy = mbg::make_default_policy();
  config.epoch = mbg::Epoch::from_raw(1);
  config.incarnation = mbg::IncarnationId::from_raw(0xC0FFEEULL);
  config.boot = mbg::BootId::from_raw(1);

  mbg::Governor governor(config);

  mbg::synthetic::GeneratorConfig generator;
  generator.shape = mbg::synthetic::TraceShape::kMicroburst;
  generator.ticks = 200;
  generator.burst_start = 60;
  generator.burst_ticks = 10;
  const std::vector<mbg::Sample> samples = mbg::synthetic::generate(generator);

  std::uint64_t positives = 0;
  for (const mbg::Sample& sample : samples) {
    const mbg::IngestOutcome outcome = governor.ingest(sample);
    if (mbg::is_positive(outcome.classification)) {
      positives += 1;
    }
  }
  static_cast<void>(governor.advance(mbg::Tick::from_raw(samples.back().tick.value() + 4096U)));

  const mbg::GovernorStats stats = governor.stats();
  std::printf(
      "mbg_consumer: version=%s samples=%zu positives=%llu episodes_opened=%llu "
      "episodes_closed=%llu interventions_requested=%llu live_interventions=%zu\n",
      mbg::version_string(), samples.size(), static_cast<unsigned long long>(positives),
      static_cast<unsigned long long>(stats.episodes_opened),
      static_cast<unsigned long long>(stats.episodes_closed),
      static_cast<unsigned long long>(stats.interventions_requested),
      governor.live_interventions().size());

  if (positives == 0 || stats.episodes_opened == 0) {
    std::fputs("mbg_consumer: FAILED: the installed package did not detect the synthetic burst\n",
               stderr);
    return 1;
  }
  std::puts("mbg_consumer: ok");
  return 0;
}
