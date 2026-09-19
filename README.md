# Microburst Governor

Open-source, vendor-neutral C++20 runtime for generation-bound detection, classification, and
bounded response to transient network queue explosions and short-lived congestion bursts.

**Version 1.0.0** | Apache License 2.0 | Copyright 2026 Summon Software Labs

---

## Core question

Given authoritative high-resolution queue, buffer, rate, timing, path, and capacity evidence: is a
transient microburst occurring now, where, how severe is it, what bounded intervention is justified,
and when must the event be dismissed, demoted, fenced, or considered stale?

Microburst Governor answers that question and nothing else. It turns high-resolution evidence into a
classification with an explanation and a bounded corrective intent.

## Systems boundary

**Owned by Microburst Governor**

- transient burst detection and classification from queue, buffer, rate, timing and capacity evidence;
- episode identity, lifecycle and generation binding;
- bounded corrective intent (a request, never an action);
- evidence admissibility, staleness refusal and degraded-mode policy;
- hysteresis, cooldown and duplicate suppression for its own episodes;
- durable episode history and recovery classification of its own state;
- authority epoch, incarnation and boot fencing for its own decisions.

**Explicitly not owned**

- general congestion state, queue lifecycle, buffer allocation;
- pacing execution, rate enforcement, flow scheduling, admission control execution;
- routing, generic backpressure, elephant/incast/hotspot governance;
- physical telemetry collection.

The governor never paces, drops, resizes, schedules or reconfigures anything. It emits intent. The
systems that own those mechanisms decide whether to honour it.

## What is actually implemented

### Evidence model

- Exact integer arithmetic throughout the authoritative path. Rates are Q16.16 fixed point
  (`RateQ16`); no floating point participates in any decision, so classification is bit-for-bit
  reproducible across platforms and compiler versions.
- Monotonic logical ticks. Authoritative ordering uses wrapping-safe tick distance, never wall
  clock. Counter rollover, declared resets and backwards steps are classified explicitly
  (`kAdvance`, `kRollover`, `kRegression`, `kDeclaredReset`, `kFirstObservation`).
- Cumulative counters may wrap. A modular forward step below 2^63 is a rollover and stays usable;
  anything else is a regression and yields no derivative.
- Samples carry resource, queue and path identity, a resource generation, a provenance reference,
  streaming flags, and up to four affected traffic classes.
- Structurally contradictory samples are rejected, not repaired: occupancy above capacity without a
  declared capacity change, negative rates, out-of-range depths, invalid identities.

### Detection

Deterministic, explicit rules evaluated once per logical tick per stream:

| Rule | Meaning |
| --- | --- |
| R1 depth | queue depth is at or above `onset_min_depth` |
| R2 rise | the steepest of the last-interval slope and the window slope is at or above `onset_min_slope_q16`, judged only inside `onset_slope_window_ticks` of the first crossing of R1 |
| R3 hold | R1 has held continuously for at least `onset_sustain_ticks` |
| R4 evidence | the evidence window is admissible (see below) |
| R5 release depth | depth is at or below `release_depth` |
| R6 release slope | slope is at or below `release_slope_q16` |
| R7 release hold | R5 and R6 held for `release_sustain_ticks` |
| R8 ceiling | episode duration reached `max_episode_ticks` |

R2 is a latch inside a bounded onset window rather than a per-tick condition. A queue that merely
sits above the threshold and later drifts upward is not a microburst; a step-shaped queue explosion
is. Requiring the derivative on every tick would make the step shape undetectable.

**Evidence admissibility.** A classification is authoritative only when the window has enough
samples, meets the completeness floor, is fresh within `staleness_limit_ticks`, has no gap inside the
retained window above `max_sample_gap_ticks`, and has a stable buffer capacity. The gap is judged
over the samples still retained, so one historical gap cannot poison a stream forever. When any of
those checks fails, the result is `UNKNOWN` and positive classification is impossible.

**UNKNOWN cannot become BURST_DETECTED.** Staleness is never relaxed, not even by a degraded policy.
`allow_degraded_classification` relaxes only the sample-count and completeness floors; the resulting
classification is surfaced as `BURST_DETECTED_DEGRADED`, carries `EvidenceAuthority::kDegraded`, and
its severity is capped by `degraded_severity_cap`, which validation requires to stay below severe.

**Hysteresis and cooldown.** Release thresholds must be strictly below onset thresholds; validation
rejects a policy where they are not. A closed episode enters a cooldown during which a new episode
cannot open, which bounds how many episodes a pathological oscillation can produce.

**Identity.** An episode identity is a deterministic function of the stream key and the onset tick.
Replaying the same evidence produces the same identity on any run, in any process. Re-triggering
inside a recovering episode merges into the same episode instead of creating a new one, so one
episode cannot explode into unbounded duplicates.

### Classification and explanation

Every decision carries a fixed-capacity explanation of rule outcomes with observed value, threshold
and tick, plus an evidence summary (samples, expected samples, completeness per-mille, gaps,
reorders, duplicates, counter resets, rollovers, provenance) and episode metrics (onset, peak,
duration, peak depth, peak occupancy, peak slope, drop and mark deltas). Severity is graded
independently on occupancy, depth, slope, duration and drops; the overall grade is the maximum and
every crossed threshold is recorded. Explanation size is bounded and truncation is reported.

### Bounded corrective intent

`kRequestPacing`, `kTemporaryAdmissionReduction`, `kTemporaryHeadroomIncrease` and
`kEscalateToCongestionFabric` are the only intents the runtime can express. Each intent is bounded by
policy in magnitude (per-mille, hard ceiling 1000), TTL, minimum justifying severity, per-stream
concurrency and total concurrency. Magnitude is a deterministic function of the classification
severity.

Every intent carries an authority vector: policy generation and digest, evidence generation,
resource generation, authority epoch, incarnation, boot generation, issue tick and expiry tick. An
intent is revoked the instant any component stops matching:

- its episode closes (`event-closed`);
- the policy changes (`policy-changed`);
- the epoch, incarnation or boot generation changes (`epoch-changed`, `incarnation-changed`,
  `boot-changed`);
- the resource generation changes (`resource-generation-changed`);
- its TTL elapses (`expired`);
- the operator disables interventions (`interventions-disabled`).

Revocation is evaluated on every accepted sample and every time advance, not only on a periodic
sweep, so an intent cannot silently survive stale authority.

### Durability

A versioned, integrity-checked, crash-safe store: an append-only journal plus an atomic snapshot.

- Every record is framed with a magic, a format version, an explicit type, a sequence, a tick, a
  length bound, a header checksum and a payload checksum.
- A record is acknowledged only after it is flushed and, when configured, committed to stable
  storage.
- Recovery scans the journal, verifies every record, and retires a partially written or corrupt tail
  instead of guessing at it. A clean journal tail and a corrupt one are distinguished (`kClean`,
  `kTruncatedTail`, `kCorruptTail`, `kVersionMismatch`, `kOversized`, `kMissing`).
- Snapshots are written to a temporary file, flushed, and atomically renamed; the journal is then
  truncated. Durable growth is bounded; when the journal budget is exhausted the governor compacts
  and retries exactly once, and any record it cannot make durable is counted rather than silently
  dropped.

Recovery classifies what it finds, in the vocabulary the invariants require:
`durable-configuration`, `committed-history`, `unfinished-attempt`, `ambiguous-outcome`,
`stale-live-authority`, `evidence-requiring-revalidation`.

**Durable state never restores liveness.** After a restart:

- policy documents and committed episode history are restored;
- an episode that was in flight is fenced with reason `restart` and recorded as history;
- every intervention that was live is recorded as fenced with verdict `boot-changed` and is never
  resumed;
- no stream liveness is restored: every previously known resource must re-establish its evidence
  window from fresh samples before any classification is authoritative again.

### Authority, epochs and processes

The coordinator owns the authority epoch for a deployment and advances it on every start. Workers
present protocol version, epoch and boot generation; a claim is accepted only when every compared
component matches exactly. A stale epoch or a previous boot generation is refused with a distinct
handshake status, counted separately, and is never admitted.

Transport is real framed TCP over loopback: a fixed 32-byte header with a magic, a protocol version,
a message type, flags, a sequence, a length bound, a header checksum and a payload checksum. The
encoder stamps the declared payload length from the payload itself, so a frame image can never
disagree with the bytes it carries. Malformed, truncated, oversized, contradictory and
trailing-byte frames are rejected with specific statuses.

No socket timeouts exist anywhere in the runtime. A stalled peer is broken by an explicit
`shutdown()` from the owning thread or by the peer closing the connection.

## Capabilities at a glance

- Deterministic detection, classification and intervention under an explicit policy generation.
- Strongly typed identities: resource, queue, path, class, resource generation, evidence generation,
  event, intervention, policy, policy generation, decision sequence, epoch, incarnation, boot,
  worker, attempt, frame sequence, record sequence.
- Bounded everything: window capacity, stream count, event history, intervention table, policy
  overrides, pending emissions, journal and snapshot size, record size, frame size, explanation size,
  rendered text.
- Checked arithmetic for every externally influenced size, capacity, rate, counter and time unit.
- Single state mutex with a re-entrancy guard that aborts loudly instead of deadlocking; no callback,
  sink or visitor is ever invoked while the mutex is held.
- An `mbgctl` tool for synthetic trace generation, trace replay, durable store verification, a
  deterministic self test, and benchmarking.
- An installable, exported CMake package (`find_package(mbg 1.0 CONFIG REQUIRED)`, target
  `mbg::mbg`).

## Repository layout

    include/mbg/core/        checked arithmetic, status, CRC-32C, byte codec, strong identities
    include/mbg/model/       ticks, samples, policy, events, interventions, explanation, severity
    include/mbg/detect/      evidence window, detector state machine, severity classifier
    include/mbg/authority/   authority binding and claim validation
    include/mbg/persist/     durable store and persisted state
    include/mbg/transport/   framed wire protocol and sockets
    include/mbg/coordinator/ coordinator authority and worker transport
    include/mbg/             governor, limits, version
    src/                     the implementation of the above
    bench/                   synthetic benchmark entry point
    tests/                   test suite and the downstream consumer project

## Build

Requires CMake 3.25 or newer and a C++20 compiler. MSVC 19.44 (Visual Studio 2022 17.14) with
`/W4 /WX` is the primary validated toolchain; the sources are portable C++20 and also build with GCC
and Clang (POSIX sockets are implemented alongside Winsock).

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `MBG_BUILD_TESTS` | `ON` | build the test suite |
| `MBG_BUILD_TOOLS` | `ON` | build `mbgctl` |
| `MBG_BUILD_BENCH` | `ON` | build `mbg_bench` |
| `MBG_WARNINGS_AS_ERRORS` | `ON` | promote first-party warnings to errors |
| `MBG_ENABLE_ASAN` | `OFF` | enable AddressSanitizer when the toolchain provides a runtime |
| `MBG_ENABLE_ANALYZE` | `OFF` | run MSVC `/analyze` |

No test carries a CTest timeout property. A hanging test is a defect to diagnose, not something to
terminate with a watchdog.

## Install and consume

    cmake --install build --prefix /path/to/prefix
    cmake -S tests/consumer -B build-consumer -DCMAKE_PREFIX_PATH=/path/to/prefix
    cmake --build build-consumer

`tests/consumer` is an independent project. It is not part of the main build and only ever sees the
installed package.

## Tool

    mbgctl selftest
    mbgctl generate --shape microburst --ticks 512 --seed 7 --out trace.txt
    mbgctl replay --trace trace.txt --events
    mbgctl verify --store ./state
    mbgctl bench --resources 32 --ticks 8192 --window 64 --bursts 8 --seed 20260101

Shapes: `steady`, `microburst`, `ramp`, `oscillating`, `noisy-steady`, `sparse-sampling`,
`counter-reset`, `capacity-change`, `missing-samples`, `reordered`, `duplicate`, `shallow-noise`,
`long-burst`.

## Minimal example

    #include "mbg/governor.hpp"

    mbg::GovernorConfig config;
    config.policy = mbg::make_default_policy();
    config.epoch = mbg::Epoch::from_raw(1);
    config.incarnation = mbg::IncarnationId::from_raw(0x1234);
    config.boot = mbg::BootId::from_raw(1);

    mbg::Governor governor(config);

    mbg::IngestOutcome outcome = governor.ingest(sample);
    if (mbg::is_positive(outcome.classification)) {
      // outcome.decision.explanation explains exactly which rules fired and with what operands.
      // outcome.decision.severity is the bounded severity of the classification.
    }
    governor.advance(tick);   // time driven rules: sustain, release, ceiling, cooldown, expiry
    auto intents = governor.live_interventions();   // bounded corrective intent, never an action

## Policy

A policy is a value object with a canonical digest. Installing one advances the policy generation,
so every decision records the exact policy that justified it. Detection policy controls window
capacity, sampling cadence expectations, completeness and staleness floors, onset and release
thresholds, severity ladders, episode shaping and degraded operation. Intervention policy controls,
per intent kind, whether it is enabled, its magnitude ceiling, its TTL and its minimum justifying
severity, plus per-stream and total concurrency limits.

The default policy enables pacing, temporary admission reduction and escalation to the congestion
fabric, and leaves temporary headroom increase disabled.

## Test suite

Six executables, all run plainly by `ctest` with no timeouts:

| Target | Coverage |
| --- | --- |
| `mbg_core_test` | identity domains, checked arithmetic, CRC-32C, byte codec, tick rollover, counter resolution, explanation bounds, policy validation and generations, evidence window behaviour, trace round trips |
| `mbg_detector_test` | onset and release rules, hysteresis, cooldown, episode ceiling, degraded mode, staleness refusal, policy rearm fencing, idempotent evaluation, run-to-run determinism |
| `mbg_governor_test` | episodes, bounded intent, revocation on close, policy change and authority rebind, concurrency limits, bounded history, deterministic identity, property tests over seeded populations, multithreaded ingest, hardening |
| `mbg_durability_test` | journal round trips, torn tails, corrupt middles, snapshots, journal-only recovery, restart fencing; plus malformed frame, payload, trace, policy, event and durable-state rejection |
| `mbg_transport_test` | real loopback framing, peer close, handshake acceptance, stale epoch and boot refusal, evidence into the governor, disconnect fencing |
| `mbg_multiprocess_test` | real OS processes: a coordinator child spawns a worker grandchild, hard-kills it, detects the crash and fences the incarnation; then a restarted coordinator advances the epoch and refuses a worker presenting the previous epoch |

## Proof surface

**REAL**

- Real operating system processes for the multiprocess authority scenarios, including an
  unconditional hard kill (`TerminateProcess` / `SIGKILL`) and post-crash fencing.
- Real framed TCP transport over loopback, using the same framing code a deployment would use.
- Real file I/O with explicit flush and commit semantics, real atomic snapshot replacement, real
  journal truncation.
- Real multithreaded access to a single governor instance.
- Real durable-state recovery from committed sources across process boundaries.

**SYNTHETIC**

- All evidence used by the tests and benchmarks is generated by `mbg::synthetic` and labelled
  `SampleFlag::kSynthetic`. Every synthetic sample is flagged, and the flag is carried into the
  evidence summary.
- Benchmarks measure completed work over a synthetic population. They are not measurements of any
  physical network.

**UNSUPPORTED / NOT CLAIMED**

- No physical NIC, switch, DPU, RDMA, NVLink, optical or multi-node fabric validation has been
  performed. None is claimed.
- No claim is made about behaviour under real production traffic.
- AddressSanitizer was exercised on the 32-bit MSVC toolchain; the 64-bit MSVC AddressSanitizer
  runtime was not installed on the validation machine.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
