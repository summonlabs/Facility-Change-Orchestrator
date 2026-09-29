# Facility Change Orchestrator

Facility Change Orchestrator (FCO) is the physical-fleet lifecycle composition runtime of the
Data Center Control Plane (DCCP). It plans and orchestrates multi-domain facility changes that
span physical assets, lifecycle state, power, cooling, capacity, fabric attachment, maintenance
state, and the ASI and DFI control systems below it. It is DCCP repository 40 of 72, tranche 5.

C++20, CMake, no third-party dependencies, no telemetry transmission.

## Systems boundary and explicit non-ownership

FCO **owns**:

* the immutable change intent, its scope, and the evidence digest it was planned against;
* the evaluated plan: an ordered DAG of typed steps with owning domain, preconditions, expected
  effect evidence, verification gates, compensation semantics, safety class, and dependency edges;
* authorization of that plan against a live authority context and current generations;
* the durable attempt history, the evidence ledger, and crash/restart recovery of both;
* the fencing of stale work when generations, authority, or observed facility state move;
* the decision to stop, roll back, replan, or refuse — including the refusal to invent a rollback
  for an effect that cannot be undone.

FCO explicitly **does not**:

* perform power switching, cooling actuation, workload scheduling, fabric routing, firmware
  flashing, commissioning internals, decommissioning internals, or maintenance work;
* replace, reimplement, or second-guess any domain authority it composes;
* decide policy, capacity entitlement, tenancy, or safety approval;
* observe the facility on its own. Every observation it holds was reported to it, and every
  observation is bound to the authority, incarnation, control epoch, and generation set that
  produced it;
* measure hardware. Nothing in this repository was validated against physical data-center
  equipment; see [Provenance](#provenance-synthetic-versus-real).

A plan is a proposal. Producing one grants no authority to act.

## Core question

> Given the current facility state and a requested multi-domain change, what ordered plan is valid
> now, which domain authorities must act, which preconditions and verification gates are required,
> how is partial progress represented, and when must the plan stop, roll back, replan, or be fenced
> as stale?

Every command, structure, and test in this repository exists to answer that question without
overclaiming.

## Authority, generations, and fencing

An `AuthorityContext` is the tuple (incarnation, control epoch, actor, policy, policy digest).
A `GenerationSet` is eleven monotonic counters:

`facility-epoch`, `policy`, `dependency`, `topology`, `capacity`, `lifecycle`,
`maintenance`, `power`, `cooling`, `asi`, `dfi`.

A plan binds both at authorization. It may proceed only while both still hold:

* **Authority.** If the incarnation changed, or the control epoch advanced, the plan is *fenced*:
  it is never silently inherited by the new control epoch. A caller whose control epoch is behind
  the durable one is refused with `stale-authority`, and a plan bound to a superseded epoch is
  refused with `fenced-authority`. A different actor is `authority-mismatch`; a different policy
  is `policy-violation`. When several of these hold at once, the reported error is the one with
  the lowest numeric `ErrorCode`, which is the documented precedence order — not the first one
  detected.
* **Generations.** If a generation the plan was bound to has moved, the plan is `stale-generation`
  and moves to `ReplanRequired`. A generation that moved *backwards* is `generation-regression`
  and is treated as an untrustworthy observation, never as "no change".
* **Explained advances.** A plan's own verified effects legitimately advance the generation that
  owns them, which is exactly what `generation_advanced_by(action)` names. Such an advance is
  recorded in the plan as a `GenerationDelta` with the exact value produced. Only that exact value
  is accepted; any other move of the same generation fences the plan like any external change.
* **Facility revision.** Every applied observation advances a monotone facility revision. A plan
  records the revision its own last verified effect produced. A revision that moved for any other
  reason means the world changed underneath the plan, and the plan is fenced rather than
  re-interpreted against the new state.

Taking over authority is explicit: `assume_authority` requires a strictly newer control epoch and
moves plans that were actively executing to `ReplanRequired`. Plans that were merely authorized keep
their binding and are fenced the moment execution is attempted.

## Lifecycle and state model

```
Draft -> Evaluated -> Authorized -> Executing -> PartiallyApplied -> Verifying -> Completed
                                        |    \            |
                                        |     \           +-> Failed
                                        |      +-> Paused -> Executing
                                        |      +-> RollingBack -> RolledBack
                                        +-> ReplanRequired      (terminal for that revision)
                                        +-> Cancelled           (terminal)
```

Every transition is checked against an explicit table; a transition that is not in the table is
rejected with `invalid-state-transition`. There are no implicit transitions and no same-state
"transitions" — re-entering a state only refreshes its recorded reason.

A step moves through `Pending -> Ready -> Issued -> Acknowledged -> Observed -> Verified`, or to
`Failed`, `Unresolved`, `Compensated`, `Blocked`, or `NonCompensable`. The distinctions that
matter:

* **Acknowledged is not observed.** An authority that acknowledges without reporting an observation
  leaves the attempt `Unresolved`, pauses the plan, and forbids re-issuing that step. A destructive
  action is never blindly replayed.
* **Observed is not verified.** An observation becomes `Verified` only when it satisfies the step's
  declared expected effect. An observation that contradicts the expectation fails the step and moves
  the plan to `ReplanRequired`.
* **Verified is not composite completion.** When every step is verified the plan enters `Verifying`
  and every *terminal* step's expected effect is re-evaluated against the observed facility.
  Intermediate effects are expected to be superseded by their successors, so only terminal effects
  define the composite outcome. If any terminal effect does not hold, the change is not complete.
* **Partial progress is explicit.** A plan with some verified steps is `PartiallyApplied`, and its
  status map distinguishes verified, failed, compensated, unresolved, and untouched steps.
* **Point of no return.** A step marked `point_of_no_return` must be classified `NonReversible`,
  must not declare a compensation, and must carry safety class restricted or critical. Rollback of a
  plan that has already applied such a step is refused with `non-reversible-step`; the plan records
  that the world is not restored rather than fabricating an undo.
* **Replanning preserves progress.** `replan` moves the plan to the next revision, re-binds it to
  the currently observed generations, facility digest, and authority, keeps every verified or
  compensated step where it is, resets the rest to pending, and keeps the whole attempt history.

## Persistence and recovery

Durable state lives under a store root:

```
<root>/fco.lock                       exclusive OS-level writer lock, held for the process lifetime
<root>/fco.manifest                   the publication point: names the authoritative generation
<root>/fco.fencing                    the highest published generation
<root>/records/state-<gen>.fco        integrity-checked state records
<root>/staging/                       transient staging area, never authoritative
```

Each record is a frame:

| offset | field |
| --- | --- |
| 0 | magic `FCOJ` |
| 4 | `u16` format version (currently 1; any other value is `unsupported-format-version`) |
| 6 | `u16` record kind (manifest, state, fencing) |
| 8 | `u32` flags, must be zero on read |
| 12 | `u32` payload length, bounded |
| 16 | `u32` CRC-32 of the payload |
| 20 | `u32` CRC-32 of bytes 0..19 with this field treated as zero |
| 24 | payload |
| 24 + payload | SHA-256 over the final header and the payload |

The file length must be exactly `24 + payload + 32`; anything else is rejected. Decoding enforces
exact length, no trailing bytes, canonical booleans, in-domain enumerators, non-zero counters,
bounded strings and collections, and reserved fields that must be zero. An encoded record is
canonical: encoding the same state twice produces byte-identical output.

Commit order is fixed:

```
encode -> stage -> flush -> read back and verify -> atomically publish the record
       -> stage/flush/verify/publish the manifest -> advance the fencing record -> prune
```

The manifest is the publication point. Fencing is only ever advanced, and only after publication.
A crash before the manifest is published leaves the published record orphaned and it is deleted, so
a partially committed generation can never be mistaken for the authoritative one.

Recovery selects exactly one authoritative generation and never merges partial states. It reports
which mechanism it used: `manifest-authoritative` (normal), `fencing-authoritative` (the manifest was
unreadable or stale and the fencing record identified a verified record), `record-scan` (both were
unusable and the highest fully verified record was recovered), or `fresh-store`. **If state exists but
nothing verifies, opening fails with `ambiguous-recovery` rather than silently starting empty.**

Restart recovery classifies every in-flight attempt without re-issuing anything:

| durable status | disposition |
| --- | --- |
| `not-issued` | nothing was sent to any authority |
| `possibly-issued` | the durable issue point was written but no outcome was recorded: requires explicit resolution |
| `acknowledged` / `observed` | acknowledged without a verified effect: requires explicit resolution |
| `verified` | no re-issue, no duplication |
| `failed` / `rejected` | the plan moves to `ReplanRequired` |
| `compensated` | already undone |
| `unresolved` | explicit resolution required |

An unresolved attempt blocks the plan until an operator resolves it with
`fco attempt resolve --resolution not-issued|failed|compensated|verified`. Resolving to `verified`
requires that an observation for that attempt has already been ingested; the resolution never
invents one.

The attempt record is written durably *before* the request leaves the process, and each attempt
carries an idempotency key derived from the plan, revision, step, ordinal, attempt identifier, and
the action request digest. Replaying the same key cannot compound a physical effect.

## Error model and deterministic validation precedence

`ErrorCode` is a closed, numbered domain. **The numeric value is the documented evaluation
precedence**: the lower the number, the earlier the condition is evaluated and the higher its
precedence when several conditions hold at the same time. `static_assert`s in
`include/fco/error.hpp` are the machine-checked statement of that ordering.

```
100..199  input shape, encoding, integrity      (malformed-input, trailing-bytes, integrity-failure, ...)
200..299  identity                              (unknown-identity, duplicate-identity, invalid-identity)
300..399  authority, epochs, incarnation        (missing-authority, authority-mismatch, stale-authority,
                                                 fenced-authority, policy-violation)
400..499  generations and evidence currency     (stale-generation, generation-regression,
                                                 evidence-digest-mismatch, reordered-evidence, ...)
500..599  plan, state machine, preconditions    (unknown-plan, invalid-state-transition, plan-cycle,
                                                 precondition-unsatisfied, point-of-no-return, ...)
600..699  attempts and idempotency              (unknown-attempt, attempt-unresolved, idempotency-conflict)
700..799  durable storage and locking           (storage-failure, lock-unavailable, ambiguous-recovery)
800..899  domain authority ports                (domain-rejected, domain-unavailable, domain-indeterminate)
900..999  invocation surface                    (usage-error, unknown-command)
1000+     internal invariant breaches
```

Within one code the primary error is chosen by the deterministic tuple (subject, ordinal, message),
where the subject is a canonical key such as a step identifier or field path — never an address, an
iteration index, or a pointer hash. The same invalid request therefore resolves to the same primary
error regardless of incidental map ordering or thread scheduling.

Missing, unknown, or unmeasured values are never converted to zero, false, healthy, ready, safe, or
permitted. `Unknown` is a first-class enumerator, a state predicate naming a real state is never
satisfied by `Unknown`, and an unset identifier is rejected at every boundary rather than defaulting.

## Concurrency model

FCO is a single-threaded component and holds no locks of its own.

* **Exclusive writer.** The store takes a real OS-level lock on `fco.lock`: on Windows
  `CreateFileW` with a share mode of zero, on POSIX `open` + `flock(LOCK_EX | LOCK_NB)`. A second
  open of the same root is refused with `lock-unavailable` — from another process *and* from the same
  process, because Windows enforces a share mode per handle rather than per process. The lock is
  released by the operating system when the holder dies, which is proven with a real process that is
  terminated abruptly.
* **No callbacks under lock, no lock ordering.** Domain authorities are plain request/response ports.
  They are always called outside any store operation and they never re-enter the orchestrator. There
  is no worker thread to join, no cancellation-order inversion, and no nested acquisition.
* **One documented callback.** `CommitHooks::on_stage` fires after each commit stage so that
  observability and the crash-consistency harness can see the exact point a commit reached. It must
  not call back into the store; that precondition is documented at the hook and is what keeps the
  commit path free of reentrancy.
* **Single-writer discipline.** Every durable transition is committed before it is acted on, so a
  crash can leave work in flight but can never leave work unrecorded.

## Building, installing, and consuming

Requires CMake 3.21+ and a C++20 compiler. On MSVC the project builds with `/W4 /WX`; elsewhere with
`-Wall -Wextra -Werror`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /some/prefix
```

The install exports the namespaced imported target `fco::fco` and a package config, so a downstream
project that has never seen this source tree can consume it with:

```cmake
find_package(fco CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE fco::fco)
```

`tests/consumer` is exactly such a project. It is built out of tree against an installed prefix and
exercises the installed library: it reads the compiled-in release identity, checks a SHA-256
known-answer vector against the installed binary, and drives a complete plan lifecycle through the
installed orchestrator and durable store. `scripts/validate_package.ps1` performs the whole proof —
build, install, verify the installed file set and exported targets, build and run the consumer, and
run the installed command line tool.

## Command line

`fco <group> <action> [options]`. `--name VALUE` takes the next token as its value; `--name` alone
is a switch whose value is `"true"`. An unknown option is a usage error rather than something
silently ignored.

```
fco version | help
fco init --root DIR [--site S] [--epoch N] [--facility FILE] [--actor A] [--policy P]
fco facility show   --root DIR
fco facility observe --root DIR --file FACILITY.json [--note TEXT]
fco facility export --root DIR --out FILE
fco request synth --root DIR --kind KIND --site S [--rack R] --assets a,b [--out FILE]
fco plan create   --root DIR --file REQUEST.json
fco plan list     --root DIR
fco plan show     --root DIR --plan ID
fco plan render   --root DIR --plan ID
fco plan evaluate --root DIR --plan ID [--apply]
fco plan authorize --root DIR --plan ID
fco plan pause    --root DIR --plan ID [--reason TEXT]
fco plan resume   --root DIR --plan ID
fco plan abort    --root DIR --plan ID [--reason TEXT]
fco plan replan   --root DIR --plan ID [--reason TEXT]
fco plan rollback --root DIR --plan ID [--reason TEXT]
fco execute next  --root DIR --plan ID
fco execute all   --root DIR --plan ID
fco evidence ingest --root DIR --file EVIDENCE.json
fco attempt resolve --root DIR --attempt ID --resolution STATUS [--note TEXT]
fco recover --root DIR
fco status  --root DIR
fco closure --root DIR
fco example rack-replacement --root DIR [--rack R] [--assets a,b]
```

Known change kinds: `rack-replacement`, `rack-decommission`, `firmware-upgrade`,
`power-maintenance`.

Exit codes: `0` success, `1` failure, `2` usage, `3` the store is locked by another process,
`4` durable state could not be recovered or committed, `5` an attempt outcome is unresolved.

### Request, facility, and evidence documents

`fco request synth` writes a request document; `plan create` accepts one whose `steps` array is
empty together with a recognised `kind`, in which case the domain step chain is synthesised from the
observed facility. Fields left out are filled from the current authoritative state.

```json
{
  "id": "req-rack-a1", "kind": "rack-replacement", "site": "site-alpha",
  "intent": "replace every node in rack-a1",
  "actor": "principal-facility-ops", "revision": 1,
  "evidence-digest": "e3b0c442...",
  "scope": { "site": "site-alpha", "rack": "rack-a1", "assets": ["rack-a1-node-1"] }
}
```

A facility document carries `site`, `epoch`, `generations` (one entry per generation kind), and
`assets`, each asset carrying `id`, `rack`, `site`, the five observed states
(`lifecycle`, `power`, `cooling`, `maintenance`, `network`, `workloads`), the per-asset
generations (`hardware-generation`, `firmware-generation`, `lifecycle-generation`,
`maintenance-generation`, `capacity-generation`), optional `tenant`, and optional
`capacity-total`/`capacity-reserved`. `fco facility export` writes one.

An evidence document carries the binding (`plan`, `plan-revision`, `step`, `attempt`), the source
(`source-domain`, `source-incarnation`, `source-control-epoch`, `observation-sequence`),
`observed-generations`, `content-digest`, `outcome`
(`effect-observed`|`effect-absent`|`indeterminate`|`action-rejected`), optional `observed-at`,
and the `observed-asset` record the authority reports. An `effect-observed` record without an
observed asset is rejected: an observation that carries no record confirms nothing.

## Worked example: rack replacement

`fco example rack-replacement --root DIR` runs the whole flow in one command. For each asset the
synthesised chain is:

```
drain -> isolate -> power-off -> {cooling-isolate, network-quiesce} -> hardware-swap
      -> {power-on, network-restore} -> cooling-restore -> recommission
      -> maintenance-release -> verify-restoration -> restore-workloads
```

`hardware-swap` is the point of no return: lifecycle-owned, critical, non-reversible, with no
compensation. Every other consequential step names its precondition, its owner, and the effect that
must be observed before its successors may run. `verify-restoration` is satisfied only by an observed
asset that is active, powered, normally cooled, fabric-attached, out of maintenance, and has no
workload assignment outstanding; it is not satisfied by any acknowledgement.

The same flow can be driven one step at a time:

```sh
fco init --root ./state
fco request synth --root ./state --kind rack-replacement --request-id req-1 \
    --site site-alpha --assets rack-a1-node-1 --out request.json
fco plan create  --root ./state --file request.json
fco plan evaluate --root ./state --plan <id> --apply
fco plan authorize --root ./state --plan <id>
fco execute next  --root ./state --plan <id>
fco status  --root ./state
fco closure --root ./state
```

## Provenance: SYNTHETIC versus REAL

* **SYNTHETIC.** Every domain action in this repository is applied by an in-process simulated plant
  (`include/fco/sim.hpp`, `src/sim.cpp`). No physical data-center hardware was available, so the
  facility behaviour of power, cooling, fabric, lifecycle, and workload authorities is modelled, and
  every command that drives one prints a `plant: SYNTHETIC` banner. The plant is a deterministic
  state machine: the same call sequence with the same arguments always produces byte-identical
  outcomes and an identical snapshot digest, and it supports deterministic fault injection
  (stall, one-shot rejection, observation lag) so that the unresolved-attempt paths can be exercised
  rather than assumed.
* **REAL.** The durable record format, atomic commit, flush and read-back verification, the OS-level
  single-writer lock and its release on process death, crash consistency under real process
  termination, restart recovery, package install and export, and out-of-tree consumption are all
  exercised on this host against the real filesystem, the real operating system, and real,
  independent processes.
* **Not validated.** The POSIX branch of the store (flock, fsync, rename) is implemented and
  compiles, but this host is Windows: that branch is not exercised here and is not claimed to be
  validated. No claim is made about behaviour against real power, cooling, fabric, firmware, or
  scheduling systems.

## Validation actually performed

Everything below was executed on this host. Nothing in this section is projected, estimated, or
carried over from another machine.

Host and toolchain: Windows 10.0.26200 on x64, MSVC 19.44.35222 (Visual Studio 2022 Build Tools),
CMake 4.3.2, Ninja 1.13.2. Release and Debug configurations were both built with
`/W4 /WX /permissive- /Zc:__cplusplus /utf-8 /EHsc`.

### Builds

| configuration | result |
| --- | --- |
| Release (`-DCMAKE_BUILD_TYPE=Release`) | library, CLI, harnesses and every test target built: **0 warnings, 0 errors** |
| Debug (`-DCMAKE_BUILD_TYPE=Debug`) | same target set: **0 warnings, 0 errors** |

### Test suites

16 executables, 219 cases, **219 passed / 0 failed** in Release and again in Debug.

| suite | cases | result |
| --- | --- | --- |
| `test_error` | 14 | 14 passed, 0 failed |
| `test_digest` | 12 | 12 passed, 0 failed |
| `test_strong` | 14 | 14 passed, 0 failed |
| `test_domain` | 9 | 9 passed, 0 failed |
| `test_authority` | 14 | 14 passed, 0 failed |
| `test_facility` | 15 | 15 passed, 0 failed |
| `test_model` | 21 | 21 passed, 0 failed |
| `test_json` | 24 | 24 passed, 0 failed |
| `test_codec` | 16 | 16 passed, 0 failed |
| `test_store` | 20 | 20 passed, 0 failed |
| `test_planner` | 8 | 8 passed, 0 failed |
| `test_evidence` | 8 | 8 passed, 0 failed |
| `test_property` | 6 | 6 passed, 0 failed |
| `test_adversarial` | 20 | 20 passed, 0 failed |
| `test_engine` | 11 | 11 passed, 0 failed |
| `test_process` | 7 | 7 passed, 0 failed |

The case counts are cases, not assertions: most cases carry dozens of individual checks, and a
failing check never aborts its case or its suite.

What each family actually exercises:

* **Known-answer and property proof.** SHA-256 against the published vectors for the empty message,
  `"abc"`, the 56-byte two-block message, one million `'a'` characters and the pangram; CRC-32
  against `0xCBF43926`. Seeded property tests (`std::mt19937_64` with fixed literals, seed and
  iteration printed on failure) check that incremental hashing in random chunk splits equals one-shot
  hashing, that a topological order is a permutation satisfying every edge and is independent of
  insertion order, that plan digests are injective over every hashed field, that generation currency
  classifies every random bound/observed pair, that 40 random valid states survive a real
  commit/load round trip, that 200 random states encode → decode → encode byte-identically, and that
  `fence_authority` permits exactly the matching pairs and otherwise returns the lowest-precedence
  code among the conditions that hold.
* **Durable format and store.** Round-trip of a fully populated state; canonical encoding; decoder
  rejection of truncation at every byte prefix, trailing bytes, wrong format version, non-zero
  reserved fields, non-canonical booleans, out-of-domain enumerators, zero counters, over-long
  strings and over-large collection counts; the exact twelve-stage commit-hook order; retention
  bounding; long paths (441 characters); corruption of payload, header CRC, flags, version, record
  kind, manifest, fencing and combinations of them, each asserted to recover a **fully verified**
  older generation or to fail with a documented code — never to return a state that fails to decode.
* **Adversarial.** Records patched to claim duplicate plan and evidence identifiers; a manifest
  naming a generation whose record is missing; a manifest naming an older generation than the
  fencing record (fencing wins); a fencing record ahead of every record; non-record files and
  directories in `records/`; identifiers at exactly 64 and at 65 bytes; an identifier containing a
  NUL; hostile store roots (a path containing `..`, an embedded NUL, a path that is a file); every
  enum domain at `0` and at `max + 1`; a facility with 100 001 assets; an evidence ledger filled to
  `kMaxCollectionEntries`; saturating generations; 25 open/commit/close cycles.
* **Real processes (`test_process`).** These are independent operating system processes, not
  threads. They prove: the store is initialized exactly once; state written by one process is read by
  the next; a complete plan lifecycle driven across five separate process invocations reaches
  `Completed` and a closed store; a second process is refused with exit code 3 while a holder owns
  the lock; the lock is available again after the holder is killed outright; and a real process is
  terminated at **each of the twelve commit stages** in turn, after which a fresh process recovers
  exactly one authoritative generation — always either the previous one or the new one, never a
  third state — and the recovered store can still commit.

### Packaging and downstream consumption

`scripts/validate_package.ps1` passed: build, install into a private prefix, verify all 22 required
installed files, verify that `fcoTargets.cmake` declares the namespaced imported target
`fco::fco`, configure and build `tests/consumer` **out of tree** with
`find_package(fco CONFIG REQUIRED)` against that prefix only, run the consumer (which checks a
SHA-256 known-answer vector against the installed binary and drives a full plan lifecycle through the
installed library, then reopens the store in a fresh orchestrator), and run the installed `fco`
executable.

### Fresh-clone closure

`scripts/fresh_clone_check.ps1` clones the release commit into a directory that has never been
built in, configures, builds, runs the whole suite and re-runs the package validation there. It is
the final closure gate and is executed against the release commit before the tag is created.

It passed: the clone configured and built with zero warnings, all 16 suites passed, the package
validation passed inside the clone, and the temporary clone was removed.

### Benchmarks

Measured with `bench/bench_main.cpp`, 300 durable commits and 20 complete plan lifecycles, on the
release build of this host. Completed operations only; durable cost included; warmed up before
measurement; `std::chrono::steady_clock`; a single run; **not comparable across hosts**.

| measurement | provenance | operations | wall seconds | completed ops/s | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| durable commit (encode, stage, flush, verify, publish record, publish manifest, advance fencing, retention) | REAL filesystem durability | 300 | 6.281 | 47.76 | 20.11 ms | 39.62 ms | 117.25 ms |
| plan lifecycle (synthesise, create, evaluate, authorize, execute a six-step chain to completion, composite verification) | SYNTHETIC plant, REAL durable store | 20 | 7.922 | 2.52 | 399.6 ms | 430.0 ms | 438.1 ms |

The commit figure is dominated by the durability barriers themselves — `FlushFileBuffers` on the
staged record, the atomic publication, and the write-through rename of the manifest — which is
exactly the cost a durable operation should include. The plan-lifecycle figure is roughly 4 durable
commits per step: the durable issue point before the request leaves the process, and the verified
outcome after it.

### Not validated here

The POSIX branch of the durable store (`flock`, `fsync`, `rename`) is implemented and compiles,
but this host is Windows and no POSIX toolchain is installed, so that branch is **not executed or
claimed**. No physical data-center hardware was available, so no claim is made about real power,
cooling, fabric, firmware, or scheduling systems; those are modelled, and the model is labelled
SYNTHETIC everywhere it is used.

## Repository layout

```
include/fco/     public headers (one per concern; the headers document the semantics)
src/             implementation
cli/main.cpp     the fco executable
tests/           test suites, the crash harness, the lock probe, and the out-of-tree consumer
bench/           benchmark harness
cmake/           package config template
scripts/         validation scripts
```

Key headers: `error.hpp` (error domain and precedence, `Result`), `strong.hpp` (identities and
counters), `digest.hpp` (SHA-256, CRC-32), `domain.hpp` (the typed domain vocabulary),
`authority.hpp` (generation currency and fencing), `facility.hpp` (observed facility and predicate
evaluation), `model.hpp` (request, plan, attempts, state machine), `planner.hpp` (evaluation and
change-kind templates), `evidence.hpp` (the evidence ledger), `state.hpp` and `codec.hpp` (durable
state and its encoding), `store.hpp` (the durable store), `ports.hpp` (domain authority ports),
`engine.hpp` (the orchestrator), `sim.hpp` (the synthetic plant), `render.hpp`, `json.hpp`,
`cli.hpp`.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
