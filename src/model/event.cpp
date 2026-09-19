// Microburst Governor - burst episode model.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/model/event.hpp"

#include <string>

#include "mbg/core/hash.hpp"

namespace mbg {

const char* to_string(EventLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case EventLifecycle::kOpen: return "open";
    case EventLifecycle::kRecovering: return "recovering";
    case EventLifecycle::kClosed: return "closed";
    case EventLifecycle::kFenced: return "fenced";
  }
  return "unknown";
}

const char* to_string(CloseReason reason) noexcept {
  switch (reason) {
    case CloseReason::kNone: return "none";
    case CloseReason::kReleaseConfirmed: return "release-confirmed";
    case CloseReason::kEpisodeTimeout: return "episode-timeout";
    case CloseReason::kEvidenceStale: return "evidence-stale";
    case CloseReason::kPolicyChanged: return "policy-changed";
    case CloseReason::kAuthorityLost: return "authority-lost";
    case CloseReason::kRestart: return "restart";
    case CloseReason::kCapacityChanged: return "capacity-changed";
    case CloseReason::kReplayEnd: return "replay-end";
  }
  return "unknown";
}

EventId make_event_id(const StreamKey& stream, Tick onset_tick) noexcept {
  std::uint64_t h = 0xA5A5A5A5DEADBEEFULL;
  h = hash_combine(h, stream.resource.raw());
  h = hash_combine(h, stream.queue.raw());
  h = hash_combine(h, stream.path.valid() ? stream.path.raw() : 0U);
  h = hash_combine(h, onset_tick.value());
  return EventId::from_raw(h == EventId::kInvalid ? h ^ 0x5DEECE66DULL : h);
}

namespace {

void encode_summary(const EvidenceSummary& summary, ByteWriter w) {
  w.u64(summary.window_start.value());
  w.u64(summary.window_end.value());
  w.u64(summary.samples);
  w.u64(summary.expected_samples);
  w.u32(summary.completeness_permille);
  w.u64(summary.gaps);
  w.u64(summary.reordered);
  w.u64(summary.duplicates);
  w.u64(summary.counter_resets);
  w.u64(summary.counter_rollovers);
  w.u64(summary.dropped_samples);
  w.u8(summary.capacity_known ? 1U : 0U);
  w.u64(summary.capacity_bytes);
  w.u8(summary.sparse ? 1U : 0U);
  w.u8(summary.synthetic ? 1U : 0U);
  w.u64(summary.provenance.raw());
  w.u32(summary.provenance_count);
}

Status decode_summary(ByteReader& r, EvidenceSummary& summary) {
  summary.window_start = Tick::from_raw(r.u64());
  summary.window_end = Tick::from_raw(r.u64());
  summary.samples = r.u64();
  summary.expected_samples = r.u64();
  summary.completeness_permille = r.u32();
  summary.gaps = r.u64();
  summary.reordered = r.u64();
  summary.duplicates = r.u64();
  summary.counter_resets = r.u64();
  summary.counter_rollovers = r.u64();
  summary.dropped_samples = r.u64();
  summary.capacity_known = r.u8() != 0;
  summary.capacity_bytes = r.u64();
  summary.sparse = r.u8() != 0;
  summary.synthetic = r.u8() != 0;
  summary.provenance = ProvenanceId::from_raw(r.u64());
  summary.provenance_count = r.u32();
  if (!r.ok()) {
    return Status::failure(StatusCode::kTruncated, "evidence summary payload is truncated");
  }
  if (summary.completeness_permille > 1000) {
    return Status::failure(StatusCode::kCorrupt, "evidence completeness above 1000 per-mille");
  }
  return Status{};
}

}  // namespace

void encode(const BurstEvent& event, ByteWriter w) {
  w.u64(event.id.raw());
  w.u64(event.stream.resource.raw());
  w.u64(event.stream.queue.raw());
  w.u64(event.stream.path.valid() ? event.stream.path.raw() : PathId::kInvalid);
  w.u32(event.resource_generation.raw());
  w.u8(static_cast<std::uint8_t>(event.lifecycle));
  w.u8(static_cast<std::uint8_t>(event.severity));
  w.u8(static_cast<std::uint8_t>(event.authority));

  w.u64(event.metrics.peak_depth);
  w.u64(event.metrics.peak_occupancy_bytes);
  w.u64(event.metrics.capacity_bytes);
  w.i64(event.metrics.peak_slope_q16);
  w.i64(event.metrics.peak_imbalance_q16);
  w.u64(event.metrics.onset_tick.value());
  w.u64(event.metrics.peak_tick.value());
  w.u64(event.metrics.last_update_tick.value());
  w.u64(event.metrics.end_tick.value());
  w.u64(event.metrics.duration_ticks);
  w.u64(event.metrics.drop_delta);
  w.u64(event.metrics.mark_delta);

  encode_summary(event.evidence, w);

  w.u64(event.policy_id.raw());
  w.u32(event.policy_generation.raw());
  w.u64(event.policy_digest);
  w.u32(event.epoch.raw());
  w.u64(event.incarnation.raw());
  w.u64(event.boot.raw());
  w.u8(static_cast<std::uint8_t>(event.close_reason));
  w.u64(event.closed_tick.value());
  w.u64(event.opened_seq.raw());
  w.u64(event.last_seq.raw());
  w.u32(event.revision);
}

Status decode(ByteReader& r, BurstEvent& event) {
  event.id = EventId::from_raw(r.u64());
  event.stream.resource = ResourceId::from_raw(r.u64());
  event.stream.queue = QueueId::from_raw(r.u64());
  event.stream.path = PathId::from_raw(r.u64());
  event.resource_generation = ResourceGeneration::from_raw(r.u32());
  event.lifecycle = static_cast<EventLifecycle>(r.u8());
  event.severity = static_cast<SeverityClass>(r.u8());
  event.authority = static_cast<EvidenceAuthority>(r.u8());

  event.metrics.peak_depth = r.u64();
  event.metrics.peak_occupancy_bytes = r.u64();
  event.metrics.capacity_bytes = r.u64();
  event.metrics.peak_slope_q16 = r.i64();
  event.metrics.peak_imbalance_q16 = r.i64();
  event.metrics.onset_tick = Tick::from_raw(r.u64());
  event.metrics.peak_tick = Tick::from_raw(r.u64());
  event.metrics.last_update_tick = Tick::from_raw(r.u64());
  event.metrics.end_tick = Tick::from_raw(r.u64());
  event.metrics.duration_ticks = r.u64();
  event.metrics.drop_delta = r.u64();
  event.metrics.mark_delta = r.u64();

  const Status summary_status = decode_summary(r, event.evidence);
  if (!summary_status.ok()) {
    return summary_status;
  }

  event.policy_id = PolicyId::from_raw(r.u64());
  event.policy_generation = PolicyGeneration::from_raw(r.u32());
  event.policy_digest = r.u64();
  event.epoch = Epoch::from_raw(r.u32());
  event.incarnation = IncarnationId::from_raw(r.u64());
  event.boot = BootId::from_raw(r.u64());
  event.close_reason = static_cast<CloseReason>(r.u8());
  event.closed_tick = Tick::from_raw(r.u64());
  event.opened_seq = DecisionSeq::from_raw(r.u64());
  event.last_seq = DecisionSeq::from_raw(r.u64());
  event.revision = r.u32();

  if (!r.ok()) {
    return Status::failure(StatusCode::kTruncated, "burst event payload is truncated");
  }
  if (!event.stream.valid()) {
    return Status::failure(StatusCode::kCorrupt, "burst event stream identity is incomplete");
  }
  if (static_cast<std::uint8_t>(event.lifecycle) >
      static_cast<std::uint8_t>(EventLifecycle::kFenced)) {
    return Status::failure(StatusCode::kCorrupt, "burst event lifecycle is out of range");
  }
  if (!is_valid_severity(static_cast<std::uint8_t>(event.severity))) {
    return Status::failure(StatusCode::kCorrupt, "burst event severity is out of range");
  }
  if (static_cast<std::uint8_t>(event.authority) >
      static_cast<std::uint8_t>(EvidenceAuthority::kUnknown)) {
    return Status::failure(StatusCode::kCorrupt, "burst event authority is out of range");
  }
  if (event.positive() && event.authority == EvidenceAuthority::kUnknown) {
    return Status::failure(StatusCode::kCorrupt,
                           "positive severity cannot carry unknown evidence authority");
  }
  return Status{};
}

std::string render_event_line(const BurstEvent& event) {
  std::string out;
  out.reserve(256);
  out.append("event=");
  out.append(std::to_string(event.id.raw()));
  out.append(" resource=");
  out.append(std::to_string(event.stream.resource.raw()));
  out.append(" queue=");
  out.append(std::to_string(event.stream.queue.raw()));
  out.append(" lifecycle=");
  out.append(to_string(event.lifecycle));
  out.append(" severity=");
  out.append(to_string(event.severity));
  out.append(" authority=");
  out.append(to_string(event.authority));
  out.append(" onset=");
  out.append(std::to_string(event.metrics.onset_tick.value()));
  out.append(" peak=");
  out.append(std::to_string(event.metrics.peak_tick.value()));
  out.append(" peak_depth=");
  out.append(std::to_string(event.metrics.peak_depth));
  out.append(" duration=");
  out.append(std::to_string(event.metrics.duration_ticks));
  out.append(" completeness=");
  out.append(std::to_string(event.evidence.completeness_permille));
  out.append(" policy_gen=");
  out.append(std::to_string(event.policy_generation.raw()));
  out.append(" close=");
  out.append(to_string(event.close_reason));
  if (out.size() > limits::kMaxRenderedExplanationBytes) {
    out.resize(limits::kMaxRenderedExplanationBytes);
  }
  return out;
}

}  // namespace mbg
