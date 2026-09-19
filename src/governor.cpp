// Microburst Governor - the authoritative detection and bounded response engine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/governor.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <utility>

#include "mbg/core/checked.hpp"
#include "mbg/core/hash.hpp"

namespace mbg {
namespace {

/// Deterministic intervention identity: one intent per (event, kind, creation instant).
[[nodiscard]] InterventionId make_intervention_id(EventId event, InterventionKind kind,
                                                  Tick issued) noexcept {
  std::uint64_t h = 0x1D1E2D3C4B5A6978ULL;
  h = hash_combine(h, event.raw());
  h = hash_combine(h, static_cast<std::uint64_t>(kind));
  h = hash_combine(h, issued.value());
  return InterventionId::from_raw(h == InterventionId::kInvalid ? h ^ 0x9E3779B97F4A7C15ULL : h);
}

/// Severity magnitude scaling: a stronger classification justifies a larger bounded request. The
/// result is always inside (0, maximum].
[[nodiscard]] std::uint32_t scale_magnitude(std::uint32_t maximum,
                                            SeverityClass severity) noexcept {
  const auto ordinal = static_cast<std::uint64_t>(severity);
  const std::uint64_t scaled = (static_cast<std::uint64_t>(maximum) * ordinal + 3ULL) / 4ULL;
  const std::uint64_t bounded = std::min<std::uint64_t>(scaled, maximum);
  return static_cast<std::uint32_t>(std::max<std::uint64_t>(bounded, 1ULL));
}

[[nodiscard]] Tick add_ticks_checked(Tick base, std::uint64_t span) noexcept {
  const auto sum = add_checked<std::uint64_t>(base.value(), span);
  return Tick::from_raw(sum.has_value() ? *sum : ~std::uint64_t{0});
}

}  // namespace

const char* to_string(EmissionKind kind) noexcept {
  switch (kind) {
    case EmissionKind::kEpisodeOpened: return "episode-opened";
    case EmissionKind::kEpisodeClosed: return "episode-closed";
    case EmissionKind::kEpisodeFenced: return "episode-fenced";
    case EmissionKind::kInterventionRequested: return "intervention-requested";
    case EmissionKind::kInterventionStateChanged: return "intervention-state-changed";
    case EmissionKind::kPolicyInstalled: return "policy-installed";
    case EmissionKind::kAuthorityBound: return "authority-bound";
  }
  return "unknown";
}

/// Re-entrancy detecting state lock.
///
/// A non-recursive mutex re-entered on the same thread is undefined behaviour in the standard
/// library and deadlocks in practice. This guard turns that class of defect into an immediate,
/// explicit abort, so the lock re-entrancy audit has a runtime backstop and not only a code review.
class Governor::StateLock {
 public:
  StateLock(std::mutex& mutex, std::atomic<std::thread::id>& owner) : mutex_(mutex), owner_(owner) {
    mutex_.lock();
    std::thread::id expected{};
    const std::thread::id self = std::this_thread::get_id();
    if (!owner_.compare_exchange_strong(expected, self)) {
      std::fputs("mbg: fatal: governor state lock re-entered on the same thread\n", stderr);
      std::fflush(stderr);
      std::abort();
    }
  }

  ~StateLock() {
    owner_.store(std::thread::id{});
    mutex_.unlock();
  }

  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;
  StateLock(StateLock&&) = delete;
  StateLock& operator=(StateLock&&) = delete;

 private:
  std::mutex& mutex_;
  std::atomic<std::thread::id>& owner_;
};

Governor::Governor() : Governor(GovernorConfig{}) {}

Governor::Governor(const GovernorConfig& config) : config_(config) {
  static_cast<void>(policies_.set_fallback(config.policy));
  context_.policy_generation = policies_.generation();
  context_.policy_digest = policies_.digest();
  context_.epoch = config.epoch;
  context_.incarnation = config.incarnation;
  context_.boot = config.boot;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

Status Governor::install_policy(Policy policy) {
  StateLock guard(mutex_, owner_);
  return install_policy_locked(std::move(policy));
}

Status Governor::install_policy_locked(Policy policy) {
  const Status status = policies_.set_fallback(std::move(policy));
  if (!status.ok()) {
    return status;
  }
  context_.policy_generation = policies_.generation();
  context_.policy_digest = policies_.digest();
  stats_.policy_changes += 1;

  for (auto& [key, entry] : streams_) {
    static_cast<void>(key);
    const Policy& resolved = policies_.resolve(entry.detector.stream().resource);
    const auto fenced = entry.detector.rearm(resolved, policies_.generation(), last_tick_);
    if (fenced.has_value()) {
      BurstEvent event = entry.detector.episode();
      event.epoch = context_.epoch;
      event.incarnation = context_.incarnation;
      event.boot = context_.boot;
      open_events_.erase(event.id);
      event.lifecycle = EventLifecycle::kFenced;
      record_event_locked(event);
      stats_.episodes_fenced += 1;
      Emission emission;
      emission.kind = EmissionKind::kEpisodeFenced;
      emission.event = event;
      emission.tick = last_tick_;
      push_emission_locked(emission);
    }
  }

  std::size_t revoked = 0;
  refresh_authority_locked(last_tick_, &revoked);

  Emission emission;
  emission.kind = EmissionKind::kPolicyInstalled;
  emission.policy_generation = policies_.generation();
  emission.epoch = context_.epoch;
  emission.incarnation = context_.incarnation;
  emission.tick = last_tick_;
  push_emission_locked(emission);

  if (store_ != nullptr && config_.enable_journal) {
    std::vector<std::byte> payload;
    ByteWriter writer(payload);
    writer.u32(policies_.generation().raw());
    encode(policies_.fallback(), writer);
    append_record_locked(persist::RecordType::kPolicyInstalled, payload, last_tick_);
  }
  return Status{};
}

Status Governor::set_resource_policy(ResourceId resource, Policy policy) {
  StateLock guard(mutex_, owner_);
  const Status status = policies_.set_override(resource, std::move(policy));
  if (!status.ok()) {
    return status;
  }
  context_.policy_generation = policies_.generation();
  context_.policy_digest = policies_.digest();
  stats_.policy_changes += 1;

  for (auto& [key, entry] : streams_) {
    static_cast<void>(key);
    if (entry.detector.stream().resource != resource) {
      continue;
    }
    const Policy& resolved = policies_.resolve(resource);
    const auto fenced = entry.detector.rearm(resolved, policies_.generation(), last_tick_);
    if (fenced.has_value()) {
      BurstEvent event = entry.detector.episode();
      event.epoch = context_.epoch;
      event.incarnation = context_.incarnation;
      event.boot = context_.boot;
      open_events_.erase(event.id);
      event.lifecycle = EventLifecycle::kFenced;
      record_event_locked(event);
      stats_.episodes_fenced += 1;
      Emission emission;
      emission.kind = EmissionKind::kEpisodeFenced;
      emission.event = event;
      emission.tick = last_tick_;
      push_emission_locked(emission);
    }
  }
  std::size_t revoked = 0;
  refresh_authority_locked(last_tick_, &revoked);
  return Status{};
}

Status Governor::remove_resource_policy(ResourceId resource) {
  StateLock guard(mutex_, owner_);
  const Status status = policies_.remove_override(resource);
  if (!status.ok()) {
    return status;
  }
  context_.policy_generation = policies_.generation();
  context_.policy_digest = policies_.digest();
  stats_.policy_changes += 1;
  std::size_t revoked = 0;
  refresh_authority_locked(last_tick_, &revoked);
  return Status{};
}

Status Governor::bind_authority(Epoch epoch, IncarnationId incarnation, BootId boot) {
  StateLock guard(mutex_, owner_);
  if (!epoch.valid() || epoch.raw() == 0) {
    return Status::failure(StatusCode::kInvalidArgument, "authority epoch must be non-zero");
  }
  context_.epoch = epoch;
  context_.incarnation = incarnation;
  context_.boot = boot;
  stats_.authority_changes += 1;

  std::size_t revoked = 0;
  refresh_authority_locked(last_tick_, &revoked);

  Emission emission;
  emission.kind = EmissionKind::kAuthorityBound;
  emission.epoch = epoch;
  emission.incarnation = incarnation;
  emission.tick = last_tick_;
  push_emission_locked(emission);

  if (store_ != nullptr && config_.enable_journal) {
    std::vector<std::byte> payload;
    ByteWriter writer(payload);
    writer.u32(epoch.raw());
    writer.u64(incarnation.raw());
    writer.u64(boot.raw());
    append_record_locked(persist::RecordType::kAuthorityBound, payload, last_tick_);
  }
  return Status{};
}

Status Governor::set_resource_generation(ResourceId resource, ResourceGeneration generation) {
  StateLock guard(mutex_, owner_);
  if (!resource.valid()) {
    return Status::failure(StatusCode::kInvalidArgument, "resource identity is invalid");
  }
  resource_generations_[resource] = generation;
  std::size_t revoked = 0;
  refresh_authority_locked(last_tick_, &revoked);
  return Status{};
}

void Governor::set_interventions_enabled(bool enabled) noexcept {
  StateLock guard(mutex_, owner_);
  const bool was_enabled = config_.enable_interventions;
  config_.enable_interventions = enabled;
  if (!enabled && was_enabled) {
    // Withdrawing the operator mandate revokes every live intent. Re-enabling does not resurrect
    // them: a new intent requires a new classification.
    std::size_t revoked = 0;
    revoke_all_live_locked(AuthorityVerdict::kInterventionsDisabled, last_tick_, &revoked);
  }
}

bool Governor::interventions_enabled() const noexcept {
  StateLock guard(mutex_, owner_);
  return config_.enable_interventions;
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

IngestOutcome Governor::ingest(const Sample& sample) {
  StateLock guard(mutex_, owner_);
  IngestOutcome outcome;

  const Status valid = validate_sample(sample);
  if (!valid.ok()) {
    outcome.status = valid;
    stats_.samples_rejected += 1;
    return outcome;
  }

  auto found = streams_.find(sample.stream);
  if (found == streams_.end()) {
    if (streams_.size() >= limits::kMaxStreams) {
      outcome.status = Status::failure(StatusCode::kCapacityExceeded, "stream capacity exhausted");
      stats_.samples_rejected += 1;
      return outcome;
    }
    const Policy& resolved = policies_.resolve(sample.stream.resource);
    StreamEntry entry;
    entry.detector.configure(sample.stream, resolved, policies_.generation());
    found = streams_.emplace(sample.stream, std::move(entry)).first;
    stats_.streams = streams_.size();
  }
  if (sample.resource_generation.valid()) {
    resource_generations_[sample.stream.resource] = sample.resource_generation;
  }

  StreamEntry& entry = found->second;
  DetectionDecision decision;
  const Status status = entry.detector.ingest(sample, decision);
  outcome.status = status;
  if (!status.ok()) {
    stats_.samples_rejected += 1;
    return outcome;
  }

  outcome.accept = entry.detector.last_accept();
  switch (outcome.accept) {
    case WindowAccept::kDuplicate: stats_.samples_duplicate += 1; break;
    case WindowAccept::kReordered: stats_.samples_reordered += 1; break;
    case WindowAccept::kDiscontinuity: stats_.samples_discontinuity += 1; break;
    case WindowAccept::kAccepted:
    case WindowAccept::kCapacityChanged:
      stats_.samples_ingested += 1;
      break;
  }

  if (!entry.has_processed || tick_after(entry.last_processed, decision.tick)) {
    entry.has_processed = true;
    entry.last_processed = decision.tick;
    process_decision_locked(entry, decision, decision.tick);
    outcome.evaluated = true;
    if (decision.tick.value() > last_tick_.value()) {
      last_tick_ = decision.tick;
    }
  }
  // Authority is revalidated on every accepted sample, not only on an explicit time advance, so a
  // stream that is being fed continuously can never keep an expired intent alive.
  refresh_authority_locked(decision.tick, nullptr);
  outcome.classification = decision.classification;
  outcome.decision = decision;
  return outcome;
}

AdvanceResult Governor::advance(Tick now) {
  StateLock guard(mutex_, owner_);
  AdvanceResult result;
  for (auto& [key, entry] : streams_) {
    static_cast<void>(key);
    if (entry.detector.has_evaluated() && !tick_after(entry.detector.last_evaluated(), now)) {
      continue;
    }
    DetectionDecision decision;
    static_cast<void>(entry.detector.advance(now, decision));
    result.streams_evaluated += 1;
    if (!entry.has_processed || tick_after(entry.last_processed, decision.tick)) {
      entry.has_processed = true;
      entry.last_processed = decision.tick;
      process_decision_locked(entry, decision, now);
      result.decisions_changed += 1;
    }
  }
  std::size_t revoked = 0;
  refresh_authority_locked(now, &revoked);
  result.interventions_changed = revoked;
  if (now.value() > last_tick_.value()) {
    last_tick_ = now;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Decision processing
// ---------------------------------------------------------------------------

void Governor::process_decision_locked(StreamEntry& entry, const DetectionDecision& decision,
                                       Tick now) {
  stats_.evaluations += 1;
  stats_.decisions += 1;
  decision_seq_ = DecisionSeq::from_raw(decision_seq_.raw() + 1U);

  const bool has_event = decision.opened || decision.updated || decision.closed;
  BurstEvent event;
  if (has_event) {
    event = entry.detector.episode();
    event.epoch = context_.epoch;
    event.incarnation = context_.incarnation;
    event.boot = context_.boot;
    event.last_seq = decision_seq_;
    if (decision.opened) {
      event.opened_seq = decision_seq_;
    }
  }

  if (decision.opened) {
    open_events_[event.id] = event;
    stats_.episodes_opened += 1;
    Emission emission;
    emission.kind = EmissionKind::kEpisodeOpened;
    emission.event = event;
    emission.tick = now;
    push_emission_locked(emission);
    if (store_ != nullptr && config_.enable_journal) {
      std::vector<std::byte> payload;
      encode(event, ByteWriter(payload));
      append_record_locked(persist::RecordType::kEpisodeOpened, payload, now);
    }
  } else if (decision.updated && !decision.closed) {
    open_events_[event.id] = event;
  }

  if (decision.closed) {
    open_events_.erase(event.id);
    // The episode that justified an intervention has ended, so its intents lose their justification
    // at that instant: they are not left live until the next periodic sweep.
    std::size_t revoked_for_event = 0;
    refresh_authority_locked(now, &revoked_for_event, event.id);
    record_event_locked(event);
    if (decision.fenced) {
      stats_.episodes_fenced += 1;
    } else {
      stats_.episodes_closed += 1;
    }
    Emission emission;
    emission.kind = decision.fenced ? EmissionKind::kEpisodeFenced : EmissionKind::kEpisodeClosed;
    emission.event = event;
    emission.tick = now;
    push_emission_locked(emission);
    if (store_ != nullptr && config_.enable_journal) {
      std::vector<std::byte> payload;
      encode(event, ByteWriter(payload));
      append_record_locked(
          decision.fenced ? persist::RecordType::kEpisodeFenced : persist::RecordType::kEpisodeClosed,
          payload, now);
    }
  }

  if (is_positive(decision.classification) && !decision.suppressed_by_cooldown) {
    maybe_request_interventions_locked(decision, now);
  }
}

void Governor::record_event_locked(const BurstEvent& event) {
  if (history_.size() < limits::kMaxEventHistory) {
    history_.push_back(event);
    return;
  }
  history_[history_head_] = event;
  history_head_ = (history_head_ + 1U) % limits::kMaxEventHistory;
  stats_.events_evicted += 1;
}

void Governor::maybe_request_interventions_locked(const DetectionDecision& decision, Tick now) {
  if (!config_.enable_interventions) {
    stats_.interventions_suppressed += 1;
    return;
  }
  const Policy& policy = policies_.resolve(decision.stream.resource);
  const InterventionPolicy& intervention_policy = policy.intervention;

  std::size_t live_for_stream = 0;
  std::size_t live_total = 0;
  for (const auto& [id, intent] : interventions_) {
    static_cast<void>(id);
    if (!intent.live()) {
      continue;
    }
    live_total += 1;
    if (intent.stream == decision.stream) {
      live_for_stream += 1;
    }
  }

  for (std::size_t index = 0; index < kInterventionKindCount; ++index) {
    const auto kind = static_cast<InterventionKind>(index);
    const InterventionKindLimit& limit = intervention_policy.kinds[index];

    if (!limit.enabled) {
      stats_.interventions_suppressed += 1;
      continue;
    }
    if (!severity_at_least(decision.severity, limit.min_severity)) {
      stats_.interventions_suppressed += 1;
      continue;
    }
    if (intervention_policy.require_authoritative_evidence &&
        decision.authority != EvidenceAuthority::kAuthoritative) {
      stats_.interventions_suppressed += 1;
      continue;
    }

    bool refreshed = false;
    for (auto& [id, intent] : interventions_) {
      static_cast<void>(id);
      if (!intent.live() || intent.event != decision.event || intent.kind != kind) {
        continue;
      }
      const std::uint32_t magnitude = scale_magnitude(limit.magnitude_permille, decision.severity);
      intent.magnitude_permille = std::max(intent.magnitude_permille, magnitude);
      intent.authority.expiry_tick = add_ticks_checked(now, limit.ttl_ticks);
      intent.state = InterventionState::kActive;
      intent.state_changed_tick = now;
      intent.seq = decision_seq_;
      intent.justification_severity = severity_max(intent.justification_severity, decision.severity);
      refreshed = true;
      stats_.interventions_refreshed += 1;
      Emission emission;
      emission.kind = EmissionKind::kInterventionStateChanged;
      emission.intervention = intent;
      emission.tick = now;
      push_emission_locked(emission);
      if (store_ != nullptr && config_.enable_journal) {
        std::vector<std::byte> payload;
        encode(intent, ByteWriter(payload));
        append_record_locked(persist::RecordType::kInterventionStateChanged, payload, now);
      }
      break;
    }
    if (refreshed) {
      continue;
    }

    if (live_for_stream >= intervention_policy.max_concurrent_per_stream ||
        live_total >= intervention_policy.max_concurrent_total ||
        interventions_.size() >= limits::kMaxInterventions) {
      stats_.interventions_suppressed += 1;
      continue;
    }

    InterventionIntent intent;
    intent.id = make_intervention_id(decision.event, kind, now);
    intent.event = decision.event;
    intent.stream = decision.stream;
    intent.kind = kind;
    intent.state = InterventionState::kRequested;
    intent.magnitude_permille = std::min(scale_magnitude(limit.magnitude_permille, decision.severity),
                                         limits::kMaxMagnitudePermille);
    intent.ttl_ticks = Tick::from_raw(limit.ttl_ticks);
    intent.authority.policy_generation = context_.policy_generation;
    intent.authority.policy_digest = context_.policy_digest;
    intent.authority.evidence_generation =
        EvidenceGeneration::from_raw(hash_combine(decision.event.raw(), decision.tick.value()));
    const auto generation = resource_generations_.find(decision.stream.resource);
    intent.authority.resource_generation =
        generation != resource_generations_.end() ? generation->second : ResourceGeneration{};
    intent.authority.epoch = context_.epoch;
    intent.authority.incarnation = context_.incarnation;
    intent.authority.boot = context_.boot;
    intent.authority.issued_tick = now;
    intent.authority.expiry_tick = add_ticks_checked(now, limit.ttl_ticks);
    intent.seq = decision_seq_;
    intent.justification_severity = decision.severity;
    intent.trigger = ReasonCode::kInterventionRequested;
    intent.state_changed_tick = now;

    interventions_.emplace(intent.id, intent);
    live_for_stream += 1;
    live_total += 1;
    stats_.interventions_requested += 1;

    Emission emission;
    emission.kind = EmissionKind::kInterventionRequested;
    emission.intervention = intent;
    emission.tick = now;
    push_emission_locked(emission);
    if (store_ != nullptr && config_.enable_journal) {
      std::vector<std::byte> payload;
      encode(intent, ByteWriter(payload));
      append_record_locked(persist::RecordType::kInterventionRequested, payload, now);
    }
  }
}

void Governor::refresh_authority_locked(Tick now, std::size_t* revoked, EventId only_event) {
  std::vector<InterventionId> retire;
  retire.reserve(interventions_.size());
  for (auto& [id, intent] : interventions_) {
    if (only_event.valid() && intent.event != only_event) {
      continue;
    }
    if (!intent.live()) {
      retire.push_back(id);
      continue;
    }
    const auto generation = resource_generations_.find(intent.stream.resource);
    const ResourceGeneration observed =
        generation != resource_generations_.end() ? generation->second : ResourceGeneration{};
    AuthorityVerdict verdict = evaluate_authority(intent.authority, context_, observed, now);
    if (verdict == AuthorityVerdict::kValid &&
        open_events_.find(intent.event) == open_events_.end()) {
      // The episode that justified the request has ended: the justification ended with it.
      verdict = AuthorityVerdict::kEventClosed;
    }
    if (verdict == AuthorityVerdict::kValid) {
      continue;
    }
    retire_intervention_locked(intent, verdict, now, revoked);
    retire.push_back(id);
  }
  for (const InterventionId id : retire) {
    interventions_.erase(id);
  }
}

void Governor::retire_intervention_locked(InterventionIntent& intent, AuthorityVerdict verdict,
                                          Tick now, std::size_t* revoked) {
  intent.state = verdict == AuthorityVerdict::kExpired ? InterventionState::kExpired
                                                       : InterventionState::kRevoked;
  intent.revocation_verdict = verdict;
  intent.state_changed_tick = now;
  if (intent.state == InterventionState::kExpired) {
    stats_.interventions_expired += 1;
  } else {
    stats_.interventions_revoked += 1;
  }
  if (revoked != nullptr) {
    *revoked += 1;
  }
  intervention_history_.push_back(intent);
  if (intervention_history_.size() > limits::kMaxInterventions) {
    intervention_history_.erase(intervention_history_.begin());
  }
  Emission emission;
  emission.kind = EmissionKind::kInterventionStateChanged;
  emission.intervention = intent;
  emission.tick = now;
  push_emission_locked(emission);
  if (store_ != nullptr && config_.enable_journal) {
    std::vector<std::byte> payload;
    encode(intent, ByteWriter(payload));
    append_record_locked(persist::RecordType::kInterventionStateChanged, payload, now);
  }
}

void Governor::revoke_all_live_locked(AuthorityVerdict verdict, Tick now, std::size_t* revoked) {
  std::vector<InterventionId> retire;
  retire.reserve(interventions_.size());
  for (auto& [id, intent] : interventions_) {
    if (!intent.live()) {
      retire.push_back(id);
      continue;
    }
    retire_intervention_locked(intent, verdict, now, revoked);
    retire.push_back(id);
  }
  for (const InterventionId id : retire) {
    interventions_.erase(id);
  }
}

void Governor::append_record_locked(persist::RecordType type, std::span<const std::byte> payload,
                                    Tick tick) {
  if (store_ == nullptr) {
    return;
  }
  record_seq_ = RecordSeq::from_raw(record_seq_.raw() + 1U);
  Status status = store_->append(type, record_seq_, tick, payload);
  if (status.code() == StatusCode::kCapacityExceeded) {
    // Bounded durable growth: compact and retry exactly once.
    const Status compacted = compact_locked();
    if (compacted.ok()) {
      stats_.durable_compactions += 1;
      status = store_->append(type, record_seq_, tick, payload);
    } else {
      stats_.durable_append_failures += 1;
      return;
    }
  }
  if (!status.ok()) {
    // A record that could not be made durable is accounted for rather than silently dropped.
    stats_.durable_append_failures += 1;
    return;
  }
  stats_.durable_records_written += 1;
}

void Governor::push_emission_locked(Emission emission) {
  if (emissions_.size() >= limits::kMaxPendingEmissions) {
    emissions_.pop_front();
    stats_.emissions_dropped += 1;
  }
  emissions_.push_back(std::move(emission));
  stats_.emissions_emitted += 1;
}

std::vector<BurstEvent> Governor::history_ordered_locked() const {
  std::vector<BurstEvent> out;
  out.reserve(history_.size());
  for (std::size_t i = 0; i < history_.size(); ++i) {
    out.push_back(history_[(history_head_ + i) % history_.size()]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

GovernorStats Governor::stats() const {
  StateLock guard(mutex_, owner_);
  return stats_;
}

std::vector<BurstEvent> Governor::event_history() const {
  StateLock guard(mutex_, owner_);
  return history_ordered_locked();
}

std::vector<BurstEvent> Governor::open_episodes() const {
  StateLock guard(mutex_, owner_);
  std::vector<BurstEvent> out;
  out.reserve(open_events_.size());
  for (const auto& [id, event] : open_events_) {
    static_cast<void>(id);
    out.push_back(event);
  }
  std::sort(out.begin(), out.end(),
            [](const BurstEvent& a, const BurstEvent& b) { return a.id < b.id; });
  return out;
}

std::vector<InterventionIntent> Governor::live_interventions() const {
  StateLock guard(mutex_, owner_);
  std::vector<InterventionIntent> out;
  out.reserve(interventions_.size());
  for (const auto& [id, intent] : interventions_) {
    static_cast<void>(id);
    if (intent.live()) {
      out.push_back(intent);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const InterventionIntent& a, const InterventionIntent& b) { return a.id < b.id; });
  return out;
}

std::vector<InterventionIntent> Governor::intervention_history() const {
  StateLock guard(mutex_, owner_);
  return intervention_history_;
}

std::size_t Governor::stream_count() const {
  StateLock guard(mutex_, owner_);
  return streams_.size();
}

bool Governor::is_open(EventId event) const {
  StateLock guard(mutex_, owner_);
  return open_events_.find(event) != open_events_.end();
}

const Policy& Governor::policy_for(ResourceId resource) const {
  StateLock guard(mutex_, owner_);
  return policies_.resolve(resource);
}

PolicyGeneration Governor::policy_generation() const {
  StateLock guard(mutex_, owner_);
  return policies_.generation();
}

std::uint64_t Governor::policy_digest() const {
  StateLock guard(mutex_, owner_);
  return policies_.digest();
}

Epoch Governor::epoch() const {
  StateLock guard(mutex_, owner_);
  return context_.epoch;
}

IncarnationId Governor::incarnation() const {
  StateLock guard(mutex_, owner_);
  return context_.incarnation;
}

BootId Governor::boot() const {
  StateLock guard(mutex_, owner_);
  return context_.boot;
}

AuthorityContext Governor::authority_context() const {
  StateLock guard(mutex_, owner_);
  return context_;
}

std::vector<Emission> Governor::drain_emissions() {
  StateLock guard(mutex_, owner_);
  std::vector<Emission> out(emissions_.begin(), emissions_.end());
  emissions_.clear();
  return out;
}

// ---------------------------------------------------------------------------
// Durability
// ---------------------------------------------------------------------------

Status Governor::bind_store(persist::DurableStore* store) {
  StateLock guard(mutex_, owner_);
  store_ = store;
  if (store_ != nullptr) {
    const RecordSeq last = store_->last_sequence();
    record_seq_ = last.valid() ? last : RecordSeq::from_raw(0);
  }
  return Status{};
}

persist::PersistedState Governor::export_state() const {
  StateLock guard(mutex_, owner_);
  persist::PersistedState state;
  state.boot = context_.boot;
  state.epoch = context_.epoch;
  state.incarnation = context_.incarnation;
  state.policies = policies_;
  state.events = history_ordered_locked();
  for (const auto& [id, event] : open_events_) {
    static_cast<void>(id);
    state.events.push_back(event);
  }
  state.interventions = collect_persisted_interventions_locked();
  state.last_decision_seq = decision_seq_;
  state.last_record_seq = record_seq_;
  state.last_tick = last_tick_;
  state.decisions = stats_.decisions;
  state.episodes_opened = stats_.episodes_opened;
  state.episodes_closed = stats_.episodes_closed;
  state.episodes_fenced = stats_.episodes_fenced;
  state.interventions_requested = stats_.interventions_requested;
  state.interventions_revoked = stats_.interventions_revoked;
  return state;
}

Status Governor::compact_locked() {
  if (store_ == nullptr) {
    return Status::failure(StatusCode::kNotFound, "no durable store is bound");
  }
  persist::PersistedState state;
  state.boot = context_.boot;
  state.epoch = context_.epoch;
  state.incarnation = context_.incarnation;
  state.policies = policies_;
  state.events = history_ordered_locked();
  // In-flight episodes are persisted *as open* so recovery can classify them as unfinished
  // attempts. Dropping them here would make an interrupted episode invisible after a crash.
  for (const auto& [id, event] : open_events_) {
    static_cast<void>(id);
    state.events.push_back(event);
  }
  state.interventions = collect_persisted_interventions_locked();
  state.last_decision_seq = decision_seq_;
  state.last_record_seq = record_seq_;
  state.last_tick = last_tick_;
  state.decisions = stats_.decisions;
  state.episodes_opened = stats_.episodes_opened;
  state.episodes_closed = stats_.episodes_closed;
  state.episodes_fenced = stats_.episodes_fenced;
  state.interventions_requested = stats_.interventions_requested;
  state.interventions_revoked = stats_.interventions_revoked;

  std::vector<std::byte> payload;
  persist::encode(state, payload);
  if (payload.size() > limits::kMaxSnapshotBytes) {
    return Status::failure(StatusCode::kOversized, "checkpoint payload exceeds the bound");
  }
  return store_->write_snapshot(payload, record_seq_, last_tick_);
}

std::vector<InterventionIntent> Governor::collect_persisted_interventions_locked() const {
  std::vector<InterventionIntent> out = intervention_history_;
  for (const auto& [id, intent] : interventions_) {
    static_cast<void>(id);
    out.push_back(intent);
  }
  // Live authority is persisted so that its revocation can be proven after a restart, never so that
  // it can be resumed. The combined list stays inside the durable bound by retiring the oldest
  // historical entries first.
  if (out.size() > limits::kMaxInterventions) {
    out.erase(out.begin(),
              out.begin() + static_cast<std::ptrdiff_t>(out.size() - limits::kMaxInterventions));
  }
  return out;
}

Status Governor::checkpoint() {
  StateLock guard(mutex_, owner_);
  return compact_locked();
}

void Governor::accumulate_state_locked(const persist::PersistedState& state,
                                       RecoveryAccumulator& acc) {
  acc.fallback = state.policies.fallback();
  acc.overrides = state.policies.overrides();
  acc.generation = state.policies.generation();
  for (const BurstEvent& event : state.events) {
    if (event.open()) {
      acc.open_events[event.id] = event;
    } else {
      acc.closed_events.push_back(event);
    }
  }
  for (const InterventionIntent& intent : state.interventions) {
    const auto found = acc.intervention_index.find(intent.id);
    if (found == acc.intervention_index.end()) {
      if (acc.interventions.size() >= limits::kMaxInterventions) {
        continue;
      }
      acc.intervention_index.emplace(intent.id, acc.interventions.size());
      acc.interventions.push_back(intent);
    } else {
      acc.interventions[found->second] = intent;
    }
  }
  acc.counters = state;
}

void Governor::accumulate_record_locked(const persist::JournaledRecord& record,
                                        RecoveryAccumulator& acc) {
  ByteReader reader(record.payload);
  switch (record.type) {
    case persist::RecordType::kPolicyInstalled: {
      const std::uint32_t generation = reader.u32();
      Policy policy;
      if (decode(reader, policy).ok()) {
        acc.fallback = policy;
        acc.generation = PolicyGeneration::from_raw(generation);
      }
      break;
    }
    case persist::RecordType::kEpisodeOpened: {
      BurstEvent event;
      if (decode(reader, event).ok()) {
        acc.open_events[event.id] = event;
      }
      break;
    }
    case persist::RecordType::kEpisodeClosed:
    case persist::RecordType::kEpisodeFenced: {
      BurstEvent event;
      if (decode(reader, event).ok()) {
        acc.open_events.erase(event.id);
        acc.closed_events.push_back(event);
      }
      break;
    }
    case persist::RecordType::kInterventionRequested:
    case persist::RecordType::kInterventionStateChanged: {
      InterventionIntent intent;
      if (decode(reader, intent).ok()) {
        const auto found = acc.intervention_index.find(intent.id);
        if (found == acc.intervention_index.end()) {
          if (acc.interventions.size() < limits::kMaxInterventions) {
            acc.intervention_index.emplace(intent.id, acc.interventions.size());
            acc.interventions.push_back(intent);
          }
        } else {
          acc.interventions[found->second] = intent;
        }
      }
      break;
    }
    case persist::RecordType::kAuthorityBound: {
      acc.counters.epoch = Epoch::from_raw(reader.u32());
      acc.counters.incarnation = IncarnationId::from_raw(reader.u64());
      acc.counters.boot = BootId::from_raw(reader.u64());
      break;
    }
    case persist::RecordType::kCheckpoint:
    case persist::RecordType::kUnknown:
      break;
  }
}

Status Governor::restore(persist::RestoreReport& report) {
  StateLock guard(mutex_, owner_);
  report = persist::RestoreReport{};
  if (store_ == nullptr) {
    return Status::failure(StatusCode::kNotFound, "no durable store is bound");
  }

  std::vector<persist::JournaledRecord> records;
  std::vector<std::byte> snapshot_payload;
  persist::RecoveryReport recovery;
  const Status replayed = store_->replay(
      [&records](const persist::JournaledRecord& record) -> Status {
        if (records.size() >= limits::kMaxRecoveredRecords) {
          return Status::failure(StatusCode::kOversized, "journal record count exceeds the bound");
        }
        records.push_back(record);
        return Status{};
      },
      recovery, snapshot_payload);
  if (!replayed.ok()) {
    return replayed;
  }
  report.snapshot_present = recovery.snapshot_present;
  report.records_applied = recovery.records_replayed;
  report.recovery_outcome = persist::to_string(recovery.outcome);
  report.detail = recovery.detail;
  if (recovery.outcome == persist::RecoveryOutcome::kTruncatedTail ||
      recovery.outcome == persist::RecoveryOutcome::kCorruptTail) {
    // The region after the last verified record has an unknown outcome. It is never guessed at, and
    // it is reported as ambiguous rather than silently applied.
    report.ambiguous_outcomes += 1;
  }

  RecoveryAccumulator acc;
  if (recovery.snapshot_present && !snapshot_payload.empty()) {
    persist::PersistedState snapshot_state;
    const Status decoded = persist::decode(snapshot_payload, snapshot_state);
    if (!decoded.ok()) {
      report.ambiguous_outcomes += 1;
      report.detail.append("; snapshot payload rejected: ");
      report.detail.append(decoded.detail());
    } else {
      accumulate_state_locked(snapshot_state, acc);
    }
  }
  for (const persist::JournaledRecord& record : records) {
    if (record.seq.raw() <= recovery.snapshot_covered_seq) {
      continue;
    }
    accumulate_record_locked(record, acc);
  }

  // Durable configuration is restored.
  if (acc.fallback.has_value()) {
    PolicySet restored;
    const Status status =
        PolicySet::restore(*acc.fallback, acc.overrides, acc.generation, restored);
    if (status.ok()) {
      policies_ = restored;
      context_.policy_generation = policies_.generation();
      context_.policy_digest = policies_.digest();
      report.configuration_items += 1 + acc.overrides.size();
    } else {
      report.ambiguous_outcomes += 1;
    }
  }

  // Committed history is restored verbatim.
  for (const BurstEvent& event : acc.closed_events) {
    BurstEvent copy = event;
    copy.epoch = context_.epoch;
    copy.incarnation = context_.incarnation;
    copy.boot = context_.boot;
    record_event_locked(copy);
    report.history_items += 1;
  }
  // Unfinished episodes are fenced. Liveness is never restored.
  for (const auto& [id, event] : acc.open_events) {
    static_cast<void>(id);
    BurstEvent copy = event;
    copy.lifecycle = EventLifecycle::kFenced;
    copy.close_reason = CloseReason::kRestart;
    copy.closed_tick = copy.metrics.last_update_tick;
    copy.epoch = context_.epoch;
    copy.incarnation = context_.incarnation;
    copy.boot = context_.boot;
    copy.revision = copy.revision + 1U;
    record_event_locked(copy);
    report.unfinished_attempts += 1;
    report.history_items += 1;
  }
  // Stale live authority is recorded as revoked, never resumed.
  for (const InterventionIntent& intent : acc.interventions) {
    if (!intent.live()) {
      continue;
    }
    InterventionIntent copy = intent;
    copy.state = InterventionState::kFenced;
    copy.revocation_verdict = AuthorityVerdict::kBootChanged;
    copy.state_changed_tick = last_tick_;
    intervention_history_.push_back(copy);
    if (intervention_history_.size() > limits::kMaxInterventions) {
      intervention_history_.erase(intervention_history_.begin());
    }
    report.stale_authority_items += 1;
  }

  stats_.decisions = acc.counters.decisions;
  stats_.episodes_opened = acc.counters.episodes_opened;
  stats_.episodes_closed = acc.counters.episodes_closed;
  stats_.episodes_fenced = acc.counters.episodes_fenced;
  stats_.interventions_requested = acc.counters.interventions_requested;
  stats_.interventions_revoked = acc.counters.interventions_revoked;
  if (acc.counters.last_decision_seq.raw() > decision_seq_.raw()) {
    decision_seq_ = acc.counters.last_decision_seq;
  }
  if (acc.counters.last_record_seq.raw() > record_seq_.raw()) {
    record_seq_ = acc.counters.last_record_seq;
  }
  if (acc.counters.last_tick.value() > last_tick_.value()) {
    last_tick_ = acc.counters.last_tick;
  }

  // Evidence freshness is never restored: every previously known resource must re-establish its
  // window from fresh samples before a classification is authoritative again.
  std::unordered_map<std::uint64_t, bool> resources;
  for (const BurstEvent& event : history_) {
    resources[event.stream.resource.raw()] = true;
  }
  report.evidence_requiring_revalidation = resources.size();

  Emission emission;
  emission.kind = EmissionKind::kAuthorityBound;
  emission.epoch = context_.epoch;
  emission.incarnation = context_.incarnation;
  emission.tick = last_tick_;
  push_emission_locked(emission);

  if (config_.enable_journal) {
    std::vector<std::byte> payload;
    ByteWriter writer(payload);
    writer.u32(context_.epoch.raw());
    writer.u64(context_.incarnation.raw());
    writer.u64(context_.boot.raw());
    append_record_locked(persist::RecordType::kAuthorityBound, payload, last_tick_);
  }
  return Status{};
}

}  // namespace mbg
