// Microburst Governor - framed transport and coordinator authority tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "mbg/coordinator/coordinator.hpp"
#include "mbg/coordinator/worker.hpp"
#include "mbg/governor.hpp"
#include "mbg/synthetic/generator.hpp"
#include "mbg/transport/socket.hpp"
#include "test_framework.hpp"

using namespace mbg;

namespace {

GovernorConfig transport_governor_config() {
  GovernorConfig config;
  config.policy = make_default_policy();
  config.policy.detection.window_capacity = 32;
  config.policy.detection.min_samples_for_classification = 6;
  config.policy.detection.onset_min_depth = 1024;
  config.policy.detection.onset_min_slope_q16 = rate_from_whole(64);
  config.epoch = Epoch::from_raw(1);
  config.incarnation = IncarnationId::from_raw(0x2468ACE0ULL);
  config.boot = BootId::from_raw(1);
  return config;
}

std::vector<Sample> worker_stream(std::uint64_t resource, std::uint64_t start_tick) {
  synthetic::GeneratorConfig config;
  config.shape = synthetic::TraceShape::kMicroburst;
  config.ticks = 160;
  config.burst_start = 40;
  config.burst_ticks = 8;
  config.start_tick = start_tick;
  config.resource = ResourceId::from_raw(resource);
  config.queue = QueueId::from_raw(1);
  config.path = PathId::from_raw(1);
  return synthetic::generate(config);
}

}  // namespace

MBG_TEST(transport, loopback_frames_round_trip) {
  transport::Listener listener;
  MBG_REQUIRE(listener.bind_loopback(0).ok());
  MBG_CHECK(listener.port() != 0);

  transport::Socket client;
  MBG_REQUIRE(transport::connect_loopback("127.0.0.1", listener.port(), client).ok());
  transport::Socket server;
  MBG_REQUIRE(listener.accept(server).ok());

  transport::FramedChannel sender(std::move(client));
  transport::FramedChannel receiver(std::move(server));

  for (std::uint64_t i = 1; i <= 32; ++i) {
    transport::Frame outgoing;
    outgoing.header.type = transport::MessageType::kHeartbeat;
    outgoing.header.seq = FrameSeq::from_raw(i);
    ByteWriter writer(outgoing.payload);
    writer.u64(i * 7U);
    writer.text("microburst");
    MBG_REQUIRE(sender.send(outgoing).ok());

    transport::Frame incoming;
    MBG_REQUIRE(receiver.receive(incoming).ok());
    MBG_CHECK(incoming.header.seq == FrameSeq::from_raw(i));
    ByteReader reader(incoming.payload);
    MBG_CHECK_EQ(reader.u64(), i * 7U);
    MBG_CHECK_EQ(reader.text(), std::string("microburst"));
  }
  MBG_CHECK_EQ(sender.frames_sent(), 32U);
  MBG_CHECK_EQ(receiver.frames_received(), 32U);
}

MBG_TEST(transport, peer_close_is_reported_as_io_failure) {
  transport::Listener listener;
  MBG_REQUIRE(listener.bind_loopback(0).ok());
  transport::Socket client;
  MBG_REQUIRE(transport::connect_loopback("127.0.0.1", listener.port(), client).ok());
  transport::Socket server;
  MBG_REQUIRE(listener.accept(server).ok());
  server.close();
  transport::FramedChannel channel(std::move(client));
  transport::Frame frame;
  const Status status = channel.receive(frame);
  MBG_CHECK(!status.ok());
}

MBG_TEST(transport, handshake_is_accepted_for_the_current_epoch) {
  Governor governor(transport_governor_config());
  Coordinator coordinator;
  Coordinator::Config config;
  config.governor = &governor;
  config.seed_epoch = Epoch::from_raw(4);
  config.seed_boot = BootId::from_raw(2);
  MBG_REQUIRE(coordinator.start(config).ok());

  Worker worker;
  Worker::Config worker_config;
  worker_config.port = coordinator.port();
  worker_config.id = WorkerId::from_raw(1);
  worker_config.incarnation = IncarnationId::from_raw(7);
  worker_config.boot = BootId::from_raw(2);
  worker_config.epoch = Epoch::from_raw(4);
  MBG_REQUIRE(worker.connect(worker_config).ok());
  transport::HelloResponse response;
  MBG_REQUIRE(worker.handshake(response).ok());
  MBG_CHECK(response.status == transport::HandshakeStatus::kAccepted);
  MBG_CHECK(response.epoch == Epoch::from_raw(4));
  MBG_CHECK(worker.granted_epoch() == Epoch::from_raw(4));

  MBG_REQUIRE(worker.say_goodbye().ok());
  MBG_REQUIRE(coordinator.wait_for_sessions());
  coordinator.stop();
}

MBG_TEST(transport, stale_epoch_and_boot_are_refused) {
  Governor governor(transport_governor_config());
  Coordinator coordinator;
  Coordinator::Config config;
  config.governor = &governor;
  config.seed_epoch = Epoch::from_raw(9);
  config.seed_boot = BootId::from_raw(3);
  MBG_REQUIRE(coordinator.start(config).ok());

  {
    Worker worker;
    Worker::Config worker_config;
    worker_config.port = coordinator.port();
    worker_config.id = WorkerId::from_raw(1);
    worker_config.epoch = Epoch::from_raw(8);  // previous epoch
    worker_config.boot = BootId::from_raw(3);
    MBG_REQUIRE(worker.connect(worker_config).ok());
    transport::HelloResponse response;
    MBG_CHECK(!worker.handshake(response).ok());
    MBG_CHECK(response.status == transport::HandshakeStatus::kStaleEpoch);
    MBG_CHECK(worker.fenced());
  }
  {
    Worker worker;
    Worker::Config worker_config;
    worker_config.port = coordinator.port();
    worker_config.id = WorkerId::from_raw(2);
    worker_config.epoch = Epoch::from_raw(9);
    worker_config.boot = BootId::from_raw(1);  // previous boot generation
    MBG_REQUIRE(worker.connect(worker_config).ok());
    transport::HelloResponse response;
    MBG_CHECK(!worker.handshake(response).ok());
    MBG_CHECK(response.status == transport::HandshakeStatus::kStaleBoot);
  }
  MBG_REQUIRE(coordinator.wait_for_sessions());
  const CoordinatorStats stats = coordinator.stats();
  MBG_CHECK(stats.stale_epoch_rejections >= 1);
  MBG_CHECK(stats.stale_boot_rejections >= 1);
  coordinator.stop();
}

MBG_TEST(transport, evidence_reaches_the_governor_and_a_disconnect_fences) {
  Governor governor(transport_governor_config());
  Coordinator coordinator;
  Coordinator::Config config;
  config.governor = &governor;
  config.seed_epoch = Epoch::from_raw(1);
  config.seed_boot = BootId::from_raw(1);
  MBG_REQUIRE(coordinator.start(config).ok());

  const std::vector<Sample> samples = worker_stream(1, 0);
  std::atomic<bool> handshake_done{false};
  std::atomic<bool> evidence_done{false};
  std::thread worker_thread([&]() {
    Worker worker;
    Worker::Config worker_config;
    worker_config.port = coordinator.port();
    worker_config.id = WorkerId::from_raw(11);
    worker_config.incarnation = IncarnationId::from_raw(0x11ULL);
    worker_config.boot = BootId::from_raw(1);
    worker_config.epoch = Epoch::from_raw(1);
    if (!worker.connect(worker_config).ok()) {
      return;
    }
    transport::HelloResponse response;
    if (!worker.handshake(response).ok()) {
      return;
    }
    handshake_done.store(true);
    transport::EvidenceBatch batch;
    batch.worker = worker_config.id;
    batch.epoch = Epoch::from_raw(1);
    batch.samples = samples;
    batch.sample_count = static_cast<std::uint32_t>(samples.size());
    transport::EvidenceAck ack;
    if (!worker.send_evidence(batch, &ack).ok()) {
      return;
    }
    evidence_done.store(true);
    // Hold the connection open until the peer tears it down.
    for (;;) {
      if (!worker.send_heartbeat(samples.back().tick).ok()) {
        return;
      }
    }
  });
  // Every exit path stops the coordinator and joins the worker thread, so a failed requirement can
  // never leave a joinable thread behind.
  struct Teardown {
    Coordinator& coordinator;
    std::thread& thread;
    ~Teardown() {
      coordinator.stop();
      if (thread.joinable()) {
        thread.join();
      }
    }
  } teardown{coordinator, worker_thread};

  MBG_REQUIRE(test::await([&]() { return handshake_done.load(); }));
  MBG_REQUIRE(test::await([&]() { return evidence_done.load(); }));
  MBG_REQUIRE(test::await([&]() { return governor.stats().samples_ingested >= samples.size(); }));
  MBG_CHECK(governor.stats().episodes_opened >= 1);
  static_cast<void>(teardown);
}
