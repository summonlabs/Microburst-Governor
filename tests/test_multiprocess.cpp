// Microburst Governor - real multiprocess authority tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every scenario here uses real operating system processes and real framed TCP transport. The
// coordinator image is spawned as a child, that child spawns a worker, and the worker is terminated
// with an unconditional hard kill. Nothing is simulated inside a single address space.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "mbg/coordinator/coordinator.hpp"
#include "mbg/coordinator/worker.hpp"
#include "mbg/core/status.hpp"
#include "mbg/governor.hpp"
#include "mbg/persist/journal.hpp"
#include "mbg/platform/process.hpp"
#include "mbg/synthetic/generator.hpp"
#include "test_framework.hpp"

using namespace mbg;

namespace {

// Exit codes used to report a scenario outcome across a process boundary.
constexpr int kOk = 0;
constexpr int kStoreFailed = 10;
constexpr int kCoordinatorFailed = 11;
constexpr int kWorkerNeverReported = 12;
constexpr int kWorkerNotFenced = 13;
constexpr int kEpochNotAdvanced = 14;
constexpr int kStaleWorkerAccepted = 15;
constexpr int kWorkerSetupFailed = 16;

/// Absolute path of this test image, captured from argv[0] so the scenarios can re-execute
/// themselves as the coordinator and worker process images.
std::string g_executable;

GovernorConfig multiprocess_config() {
  GovernorConfig config;
  config.policy = make_default_policy();
  config.policy.detection.window_capacity = 32;
  config.policy.detection.min_samples_for_classification = 6;
  config.policy.detection.onset_min_depth = 1024;
  config.policy.detection.onset_min_slope_q16 = rate_from_whole(64);
  config.boot = BootId::from_raw(1);
  return config;
}

std::vector<Sample> worker_samples(std::uint64_t resource) {
  synthetic::GeneratorConfig config;
  config.shape = synthetic::TraceShape::kMicroburst;
  config.ticks = 120;
  config.burst_start = 30;
  config.burst_ticks = 8;
  config.resource = ResourceId::from_raw(resource);
  config.queue = QueueId::from_raw(1);
  config.path = PathId::from_raw(1);
  return synthetic::generate(config);
}

/// Worker process image.
int run_worker_mode(const std::vector<std::string>& arguments) {
  if (arguments.size() < 5) {
    return kWorkerSetupFailed;
  }
  const std::uint16_t port = static_cast<std::uint16_t>(std::stoul(arguments[1]));
  const std::uint32_t epoch = static_cast<std::uint32_t>(std::stoul(arguments[2]));
  const std::uint64_t resource = std::stoull(arguments[3]);
  const std::string mode = arguments[4];

  Worker worker;
  Worker::Config config;
  config.port = port;
  config.id = WorkerId::from_raw(resource);
  config.incarnation = IncarnationId::from_raw(resource * 7919U + 13U);
  config.boot = BootId::from_raw(1);
  config.epoch = Epoch::from_raw(epoch);
  if (!worker.connect(config).ok()) {
    return kWorkerSetupFailed;
  }
  transport::HelloResponse response;
  const Status handshake = worker.handshake(response);
  if (mode == "reject") {
    if (response.status == transport::HandshakeStatus::kStaleEpoch) {
      return kOk;
    }
    return handshake.ok() ? kStaleWorkerAccepted : kWorkerSetupFailed;
  }
  if (!handshake.ok()) {
    return kWorkerSetupFailed;
  }

  const std::vector<Sample> samples = worker_samples(resource);
  transport::EvidenceBatch batch;
  batch.worker = config.id;
  batch.epoch = Epoch::from_raw(epoch);
  batch.samples = samples;
  batch.sample_count = static_cast<std::uint32_t>(samples.size());
  transport::EvidenceAck ack;
  if (!worker.send_evidence(batch, &ack).ok()) {
    return kWorkerSetupFailed;
  }
  // Hold the connection open until the peer terminates this process.
  for (;;) {
    if (!worker.send_heartbeat(samples.back().tick).ok()) {
      return kOk;
    }
    platform::sleep_millis(1);
  }
}

/// Coordinator process image.
int run_coordinator_mode(const std::vector<std::string>& arguments) {
  if (arguments.size() < 4) {
    return kCoordinatorFailed;
  }
  const std::string executable = arguments[1];
  const std::string directory = arguments[2];
  const std::string phase = arguments[3];

  persist::DurableStore store;
  persist::DurableStore::Config store_config;
  store_config.directory = directory;
  store_config.fsync_on_append = phase == "fence";
  if (!store.open(store_config).ok()) {
    return kStoreFailed;
  }

  GovernorConfig governor_config = multiprocess_config();
  if (phase == "restart") {
    governor_config.epoch = Epoch::from_raw(1);
    governor_config.boot = BootId::from_raw(1);
  } else {
    governor_config.epoch = Epoch::from_raw(1);
    governor_config.boot = BootId::from_raw(1);
  }
  Governor governor(governor_config);
  static_cast<void>(governor.bind_store(&store));

  Coordinator coordinator;
  Coordinator::Config coordinator_config;
  coordinator_config.governor = &governor;
  coordinator_config.store = &store;
  coordinator_config.seed_epoch = Epoch::from_raw(1);
  coordinator_config.seed_boot = BootId::from_raw(1);
  coordinator_config.recover_authority = true;
  if (!coordinator.start(coordinator_config).ok()) {
    return kCoordinatorFailed;
  }

  const std::string port_text = std::to_string(coordinator.port());
  int result = kOk;

  if (phase == "fence") {
    platform::ChildProcess worker;
    const Status spawned = platform::ChildProcess::spawn(
        executable, {"--worker", port_text, std::to_string(coordinator.epoch().raw()), "1",
                     "produce"},
        worker);
    if (!spawned.ok()) {
      coordinator.stop();
      return kWorkerSetupFailed;
    }
    // Wait for the worker to actually deliver a frame before terminating it. This is a
    // synchronisation wait on a required event, not a watchdog: if the frame never arrives the
    // scenario fails explicitly.
    const bool reported = test::await(
        [&coordinator]() { return coordinator.stats().frames_received >= 1U; }, 60000, 2);
    if (!reported) {
      static_cast<void>(worker.kill_hard());
      static_cast<void>(worker.wait());
      coordinator.stop();
      return kWorkerNeverReported;
    }
    // Hard kill: no shutdown handshake, no flush, no cooperation from the worker.
    static_cast<void>(worker.kill_hard());
    static_cast<void>(worker.wait());
    static_cast<void>(coordinator.wait_for_sessions());
    const std::vector<WorkerRecord> fenced = coordinator.fenced_workers();
    if (fenced.empty() || fenced.front().connected) {
      result = kWorkerNotFenced;
    } else if (!fenced.front().fence_attempt.valid()) {
      result = kWorkerNotFenced;
    }
    if (result == kOk) {
      static_cast<void>(governor.checkpoint());
    }
  } else if (phase == "restart") {
    if (!coordinator.authority_recovered() ||
        coordinator.epoch().raw() != coordinator.previous_epoch().raw() + 1U ||
        coordinator.previous_epoch().raw() == 0) {
      result = kEpochNotAdvanced;
    } else {
      // Present the previous epoch from a fresh worker process and require an explicit refusal.
      platform::ChildProcess stale;
      const Status spawned = platform::ChildProcess::spawn(
          executable,
          {"--worker", port_text, std::to_string(coordinator.previous_epoch().raw()), "2", "reject"},
          stale);
      if (!spawned.ok()) {
        result = kWorkerSetupFailed;
      } else {
        const int code = stale.wait();
        result = code == kOk ? kOk : kStaleWorkerAccepted;
      }
      static_cast<void>(coordinator.wait_for_sessions());
    }
  } else {
    result = kCoordinatorFailed;
  }

  coordinator.stop();
  return result;
}

}  // namespace

MBG_TEST(multiprocess, hard_kill_is_detected_and_the_incarnation_is_fenced) {
  test::TempDir directory("mp-fence");
  platform::ChildProcess coordinator;
  MBG_REQUIRE(platform::ChildProcess::spawn(
                  g_executable,
                  {"--coordinator", g_executable, directory.path(), "fence"},
                  coordinator)
                  .ok());
  const int code = coordinator.wait();
  MBG_CHECK_EQ(code, kOk);
}

MBG_TEST(multiprocess, restart_advances_the_epoch_and_rejects_the_previous_one) {
  test::TempDir directory("mp-restart");
  {
    platform::ChildProcess first;
    MBG_REQUIRE(platform::ChildProcess::spawn(
                    g_executable,
                    {"--coordinator", g_executable, directory.path(), "fence"},
                    first)
                    .ok());
    MBG_CHECK_EQ(first.wait(), kOk);
  }
  {
    platform::ChildProcess second;
    MBG_REQUIRE(platform::ChildProcess::spawn(
                    g_executable,
                    {"--coordinator", g_executable, directory.path(), "restart"},
                    second)
                    .ok());
    MBG_CHECK_EQ(second.wait(), kOk);
  }
  std::error_code ec;
  const auto journal = std::filesystem::path(directory.path()) / "mbg-state.journal";
  MBG_CHECK(std::filesystem::exists(journal, ec));
}

int main(int argc, char** argv) {
  if (argc > 0 && argv[0] != nullptr) {
    g_executable = argv[0];
  }
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (token == "--worker") {
      std::vector<std::string> arguments;
      for (int j = i; j < argc; ++j) {
        arguments.emplace_back(argv[j]);
      }
      return run_worker_mode(arguments);
    }
    if (token == "--coordinator") {
      std::vector<std::string> arguments;
      for (int j = i; j < argc; ++j) {
        arguments.emplace_back(argv[j]);
      }
      return run_coordinator_mode(arguments);
    }
  }
  return ::mbg::test::run_all(argc, argv);
}
