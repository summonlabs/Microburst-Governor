// Microburst Governor - the authoritative detection and bounded response engine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mbg/core/status.hpp"
#include "mbg/detect/detector.hpp"
#include "mbg/limits.hpp"
#include "mbg/model/event.hpp"
#include "mbg/model/intervention.hpp"
#include "mbg/model/policy.hpp"
#include "mbg/model/sample.hpp"
#include "mbg/persist/journal.hpp"
#include "mbg/persist/snapshot.hpp"

namespace mbg {

/// What the governor did with an offered sample.
struct IngestOutcome {
  Status status{};
  WindowAccept accept{WindowAccept::kAccepted};
  Classification classification{Classification::kUnknown};
  bool evaluated{false};
  DetectionDecision decision{};
};

/// A change the governor wants the operator, the journal or a downstream consumer to observe.
enum class EmissionKind : std::uint8_t {
  kEpisodeOpened = 0,
  kEpisodeClosed = 1,
  kEpisodeFenced = 2,
  kInterventionRequested = 3,
  kInterventionStateChanged = 4,
  kPolicyInstalled = 5,
  kAuthorityBound = 6,
};

[[nodiscard]] const char* to_string(EmissionKind kind) noexcept;

struct Emission {
  EmissionKind kind{EmissionKind::kEpisodeOpened};
  Tick tick{};
  BurstEvent event{};
  InterventionIntent intervention{};
  PolicyGeneration policy_generation{};
  Epoch epoch{};
  IncarnationId incarnation{};
};

struct GovernorStats {
  std::uint64_t samples_ingested{0};
  std::uint64_t samples_rejected{0};
  std::uint64_t samples_duplicate{0};
  std::uint64_t samples_reordered{0};
  std::uint64_t samples_discontinuity{0};
  std::uint64_t evaluations{0};
  std::uint64_t decisions{0};
  std::uint64_t episodes_opened{0};
  std::uint64_t episodes_closed{0};
  std::uint64_t episodes_fenced{0};
  std::uint64_t interventions_requested{0};
  std::uint64_t interventions_refreshed{0};
  std::uint64_t interventions_revoked{0};
  std::uint64_t interventions_expired{0};
  std::uint64_t interventions_suppressed{0};
  std::uint64_t policy_changes{0};
  std::uint64_t authority_changes{0};
  std::uint64_t emissions_emitted{0};
  std::uint64_t emissions_dropped{0};
  std::uint64_t events_evicted{0};
  std::uint64_t streams{0};
  std::uint64_t reentrancy_rejections{0};
  std::uint64_t durable_records_written{0};
  std::uint64_t durable_append_failures{0};
  std::uint64_t durable_compactions{0};

  friend constexpr bool operator==(const GovernorStats&, const GovernorStats&) noexcept = default;
};

struct AdvanceResult {
  std::size_t streams_evaluated{0};
  std::size_t decisions_changed{0};
  std::size_t interventions_changed{0};
};

struct GovernorConfig {
  Policy policy{make_default_policy()};
  Epoch epoch{Epoch::from_raw(1)};
  IncarnationId incarnation{};
  BootId boot{};
  bool enable_interventions{true};
  bool enable_journal{true};
};

/// The governor.
///
/// Owns detection, classification, episode lifecycle, bounded intervention intent and the authority
/// binding for one process incarnation. It never performs the adjacent actions it requests: it
/// produces intent, and the systems that own pacing, admission, buffers and the congestion fabric
/// decide whether to honour it.
///
/// Thread safety and locking contract.
///
/// Every public method takes the single non-recursive state mutex through StateLock, which detects
/// same-thread re-entry and aborts loudly instead of deadlocking. There are no reader/writer locks
/// anywhere in the runtime, so a read-to-write upgrade on the same lock cannot occur.
///
/// The global lock order is: governor state mutex, then durable store mutex. Nothing in the store
/// ever calls back into the governor, so the order is total and cannot be inverted. No member
/// ending in _locked acquires a lock: those are the bodies of already-locked entry points.
///
/// No callback, sink or visitor is invoked while the state mutex is held. Emissions are appended to
/// a bounded internal buffer and drained by an explicit call, and the durable store's replay visitor
/// runs after the store has finished reading the journal into memory.
class Governor {
 public:
  Governor();
  explicit Governor(const GovernorConfig& config);
  Governor(const Governor&) = delete;
  Governor& operator=(const Governor&) = delete;

  // --- configuration ------------------------------------------------------

  [[nodiscard]] Status install_policy(Policy policy);
  [[nodiscard]] Status set_resource_policy(ResourceId resource, Policy policy);
  [[nodiscard]] Status remove_resource_policy(ResourceId resource);

  /// Rebinds the authority this governor acts under. Any live intervention that was issued under a
  /// different epoch, incarnation or boot is revoked immediately and cannot be resumed.
  [[nodiscard]] Status bind_authority(Epoch epoch, IncarnationId incarnation, BootId boot);

  [[nodiscard]] Status set_resource_generation(ResourceId resource, ResourceGeneration generation);

  void set_interventions_enabled(bool enabled) noexcept;
  [[nodiscard]] bool interventions_enabled() const noexcept;

  // --- evidence -----------------------------------------------------------

  [[nodiscard]] IngestOutcome ingest(const Sample& sample);

  /// Advances logical time for every known stream. Time driven rules (sustain, release, episode
  /// ceilings, cooldown, intervention expiry) only progress here.
  [[nodiscard]] AdvanceResult advance(Tick now);

  // --- observation --------------------------------------------------------

  [[nodiscard]] GovernorStats stats() const;
  [[nodiscard]] std::vector<BurstEvent> event_history() const;
  [[nodiscard]] std::vector<BurstEvent> open_episodes() const;
  [[nodiscard]] std::vector<InterventionIntent> live_interventions() const;
  [[nodiscard]] std::vector<InterventionIntent> intervention_history() const;
  [[nodiscard]] std::size_t stream_count() const;
  [[nodiscard]] bool is_open(EventId event) const;
  [[nodiscard]] const Policy& policy_for(ResourceId resource) const;
  [[nodiscard]] PolicyGeneration policy_generation() const;
  [[nodiscard]] std::uint64_t policy_digest() const;
  [[nodiscard]] Epoch epoch() const;
  [[nodiscard]] IncarnationId incarnation() const;
  [[nodiscard]] BootId boot() const;
  [[nodiscard]] AuthorityContext authority_context() const;

  /// Drains buffered emissions. Nothing is delivered by callback, so a consumer controls exactly
  /// when it observes the governor.
  [[nodiscard]] std::vector<Emission> drain_emissions();

  // --- durability ---------------------------------------------------------

  [[nodiscard]] Status bind_store(persist::DurableStore* store);
  [[nodiscard]] Status checkpoint();
  [[nodiscard]] Status restore(persist::RestoreReport& report);
  [[nodiscard]] persist::PersistedState export_state() const;

 private:
  struct StreamEntry {
    Detector detector;
    Tick last_processed{};
    bool has_processed{false};
  };

  /// Everything recovery can rebuild from durable state. Live authority is never part of it.
  struct RecoveryAccumulator {
    std::unordered_map<EventId, BurstEvent> open_events{};
    std::vector<BurstEvent> closed_events{};
    std::unordered_map<InterventionId, std::size_t> intervention_index{};
    std::vector<InterventionIntent> interventions{};
    std::optional<Policy> fallback{};
    std::vector<PolicyOverride> overrides{};
    PolicyGeneration generation{};
    persist::PersistedState counters{};
  };

  class StateLock;

  [[nodiscard]] Status install_policy_locked(Policy policy);
  void process_decision_locked(StreamEntry& entry, const DetectionDecision& decision, Tick now);
  void record_event_locked(const BurstEvent& event);
  void maybe_request_interventions_locked(const DetectionDecision& decision, Tick now);
  /// Re-evaluates every live intervention against the authority in force.
  ///
  /// When only_event is valid, only interventions bound to that event are considered, which is how a
  /// closing episode revokes its own intents immediately instead of waiting for the next sweep.
  void refresh_authority_locked(Tick now, std::size_t* revoked, EventId only_event = EventId{});
  void revoke_all_live_locked(AuthorityVerdict verdict, Tick now, std::size_t* revoked);
  void retire_intervention_locked(InterventionIntent& intent, AuthorityVerdict verdict, Tick now,
                                  std::size_t* revoked);
  void append_record_locked(persist::RecordType type, std::span<const std::byte> payload, Tick tick);
  void push_emission_locked(Emission emission);
  [[nodiscard]] std::vector<BurstEvent> history_ordered_locked() const;
  [[nodiscard]] std::vector<InterventionIntent> collect_persisted_interventions_locked() const;
  [[nodiscard]] Status compact_locked();
  void accumulate_state_locked(const persist::PersistedState& state, RecoveryAccumulator& acc);
  void accumulate_record_locked(const persist::JournaledRecord& record, RecoveryAccumulator& acc);

  mutable std::mutex mutex_{};
  mutable std::atomic<std::thread::id> owner_{};

  GovernorConfig config_{};
  PolicySet policies_{};
  std::map<StreamKey, StreamEntry> streams_{};
  std::unordered_map<ResourceId, ResourceGeneration> resource_generations_{};

  std::unordered_map<EventId, BurstEvent> open_events_{};
  std::vector<BurstEvent> history_{};
  std::size_t history_head_{0};

  std::unordered_map<InterventionId, InterventionIntent> interventions_{};
  std::vector<InterventionIntent> intervention_history_{};
  std::size_t intervention_history_head_{0};

  std::deque<Emission> emissions_{};
  GovernorStats stats_{};

  AuthorityContext context_{};
  DecisionSeq decision_seq_{};
  RecordSeq record_seq_{};

  persist::DurableStore* store_{nullptr};
  Tick last_tick_{};
};

}  // namespace mbg
