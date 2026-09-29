#pragma once

// Synthetic plant and simulated domain authorities.
//
// This file is the only place in the runtime that fabricates facility
// behaviour. Nothing here models a real device: the plant is a deterministic
// state machine over an observed snapshot, and every outcome it reports is
// marked with kSimulatedProvenance so that evidence produced by a model run can
// never be mistaken for evidence of physical hardware.
//
// Determinism contract
// --------------------
// SimulatedPlant::apply() is a pure function of (request, plant state, injected
// faults, now_micros). It reads no clock, uses no randomness, holds no static
// mutable state, and does not depend on container iteration order: the same
// call sequence with the same arguments always yields byte-identical outcomes
// and an identical snapshot digest.
//
// Idempotency contract
// --------------------
// Every application is recorded under its idempotency key. A repeat of a key
// that has already been applied changes nothing and returns
// SubmitOutcome::DuplicateOfAppliedRequest carrying the observed values of the
// ORIGINAL application (observation sequence, generation set, hardware and
// firmware generations, content digest, observed_at_micros, and the resulting
// AssetRecord), with EvidenceOutcome::EffectObserved. Two deliberate
// reinforcements of that rule:
//   * a key reused for a DIFFERENT request is an IdempotencyConflict error, not
//     a duplicate. Answering a second, different request with the first
//     request's observation would report an effect that was never requested;
//   * an unset idempotency key cannot be deduplicated at all and is rejected
//     with MalformedInput before anything is applied. The synthetic plant never
//     applies an action it cannot key.
//
// Observation sequence
// --------------------
// The sequence is per (domain, incarnation): it starts at 1 and increments once
// per applied action in that domain, so reordered or replayed observations are
// detectable by their sequence alone. SimulatedAuthority registers its
// (domain, incarnation) pair with the plant when it is constructed; a new
// incarnation therefore starts its own sequence at 1 instead of inheriting the
// previous incarnation's watermark. A plant driven without any authority (a
// direct apply() call) sequences under an unset incarnation.
//
// seed_observation_sequence() raises the watermark of the incarnation currently
// answering for a domain, so a plant reconstructed in a new process continues
// from the durable watermark instead of restarting at 1; the evidence ledger,
// which requires strictly increasing per-source sequences across restarts,
// therefore accepts the reconstructed plant's observations.
//
// Content identity
// ----------------
// content_digest is SHA-256 over the exact byte string:
//     "fco/simulated-observation/1" || u8(domain) || record.digest().bytes()
// where record is the resulting AssetRecord and record.digest() is the
// canonical fco/asset-record/1 encoding defined in src/facility.cpp. It
// therefore changes whenever the resulting record changes, and two domains that
// produce the same record still report different content digests.
//
// Injected faults (all disabled by default)
// -----------------------------------------
//   * stall_domain(domain, true): the modelled device never answers. The next
//     submit to that domain (and every submit while the stall is set) returns
//     an accepted-looking but indeterminate outcome - SubmitOutcome::
//     Indeterminate, EvidenceOutcome::Indeterminate - and NOTHING is applied:
//     the snapshot, generations, facility revision, applied-action count and
//     idempotency record are all unchanged. This models a lost response.
//   * fail_next(domain, outcome, detail): the next submit to that domain
//     returns exactly that outcome and exactly that detail and applies nothing;
//     the injection is consumed by that one call. Only Rejected (evidence
//     ActionRejected) and Indeterminate (evidence Indeterminate) are
//     meaningful. Any other injected value is consumed as well and answered
//     with an error instead of an outcome, because "accepted with nothing
//     applied and nothing observed" and "a duplicate of an application that
//     never happened" are contradictions the plant refuses to report:
//     ImpossibleCombination for Accepted and DuplicateOfAppliedRequest,
//     InvalidEnumValue for a value outside the SubmitOutcome domain.
//   * set_observation_lag(domain, true): the action IS applied (the snapshot,
//     generations, revision and idempotency record all move) but the response
//     is SubmitOutcome::Accepted with EvidenceOutcome::Indeterminate, no
//     observation sequence, no content digest and no observed asset. It models
//     "acknowledged, not yet observed". The engine must obtain a later
//     observation; the plant provides it by answering a replay of the same
//     idempotency key with DuplicateOfAppliedRequest and the recorded
//     observation.
//
// Duplicate detection precedes fault injection: a request that was already
// applied is answered from the record, not from the unreliable channel, because
// the plant knows the fact regardless of what the modelled wire does.
//
// Changes no plan caused
// ----------------------
//   * force_generation(kind, value) moves one authority generation without
//     touching the facility revision, the applied-action count or the snapshot
//     records. It is an authority-side generation move, not a facility change.
//     A zero value, or GenerationKind::Unspecified, is ignored so that the
//     plant's generation set always stays complete and valid.
//   * force_facility_change(record) really changes the facility: the record is
//     installed in the snapshot (a new identifier adds an asset) and the
//     facility revision is advanced by one. It does NOT touch generations, the
//     applied-action count or the idempotency records, because no action was
//     applied. This is how a stale plan is fenced: the world moved underneath
//     it with nothing in the plan to explain the move.
//
// Facility revision
// -----------------
// The plant starts at facility revision 1: the initial snapshot is revision 1
// of the modelled facility. Every applied action and every forced facility
// change advances it by exactly one.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fco/authority.hpp"
#include "fco/digest.hpp"
#include "fco/domain.hpp"
#include "fco/error.hpp"
#include "fco/evidence.hpp"
#include "fco/facility.hpp"
#include "fco/ports.hpp"
#include "fco/strong.hpp"

namespace fco {

// Provenance marker. Everything produced by this translation unit models
// facility behaviour that was NOT exercised on physical hardware.
inline constexpr std::string_view kSimulatedProvenance = "SYNTHETIC";

class SimulatedPlant {
 public:
  explicit SimulatedPlant(FacilitySnapshot initial);

  [[nodiscard]] const FacilitySnapshot& snapshot() const noexcept;
  [[nodiscard]] GenerationSet generations() const noexcept;
  [[nodiscard]] FacilityRevision revision() const noexcept;
  [[nodiscard]] std::uint64_t applied_actions() const noexcept;
  [[nodiscard]] bool has_applied(const Digest& idempotency_key) const noexcept;

  // Deterministic application of one action by the authority that owns it.
  // Applies project_action(), bumps the generation returned by
  // generation_advanced_by(request.kind) when that is not Unspecified, bumps the
  // facility revision, records the idempotency key, and returns an ActionOutcome
  // whose observed_generations is the post-action generation set.
  //
  // Errors (never outcomes): InvalidEnumValue for an unset action kind,
  // MalformedInput for an unset idempotency key, UnknownIdentity for an asset
  // the plant does not hold, AuthorityMismatch when request.owner is not the
  // domain that owns request.kind, IdempotencyConflict when the key was already
  // used for a different request, and whatever project_action() reports.
  [[nodiscard]] Result<ActionOutcome> apply(const ActionRequest& request,
                                            const AttemptId& attempt,
                                            const Digest& idempotency_key,
                                            std::uint64_t now_micros);

  // Deterministic fault injection, all disabled by default.
  void fail_next(DomainKind domain, SubmitOutcome outcome, std::string detail);
  void stall_domain(DomainKind domain, bool stalled);
  void set_observation_lag(DomainKind domain, bool lag);   // authority acks but does not yet observe

  // Raises the next observation sequence this domain will report, so that a
  // plant reconstructed in a new process continues from the durable watermark
  // instead of restarting at 1. The next applied action in that domain reports
  // at least `next` (max(current_next, next)); an invalid domain and a next
  // value below 1 are ignored, and the seed applies to the incarnation that is
  // currently answering for that domain.
  void seed_observation_sequence(DomainKind domain, std::uint64_t next);

  // The sequence the next applied action in this domain will report: at least 1,
  // and 0 when the domain is invalid. Never unset, because a caller that reads it
  // is about to persist a watermark.
  [[nodiscard]] std::uint64_t next_observation_sequence(DomainKind domain) const noexcept;

  // Changes that no plan caused, used to prove that stale plans are fenced.
  void force_generation(GenerationKind kind, std::uint64_t value);
  void force_facility_change(const AssetRecord& record);

  [[nodiscard]] std::string describe() const;

 private:
  friend class SimulatedAuthority;  // registers (domain, incarnation) for sequencing

  // Per-domain injected fault state. Default constructed means "no fault".
  struct DomainFault {
    bool stalled = false;
    bool observation_lag = false;
    bool has_pending_failure = false;
    SubmitOutcome pending_outcome = SubmitOutcome::Unspecified;
    std::string pending_detail;
  };

  // One recorded application, keyed by idempotency key. outcome is the
  // canonical observed outcome of the original application (evidence
  // EffectObserved, observed asset present); a duplicate replay returns it with
  // only the outcome code and detail rewritten.
  struct AppliedEntry {
    Digest request_digest;
    DomainKind domain = DomainKind::Unspecified;
    IncarnationId incarnation;
    ActionOutcome outcome;
  };

  void note_incarnation(DomainKind domain, IncarnationId incarnation);
  [[nodiscard]] IncarnationId incarnation_for(DomainKind domain) const noexcept;
  [[nodiscard]] std::pair<std::uint8_t, std::uint64_t> sequence_key(DomainKind domain) const noexcept;
  [[nodiscard]] ObservationSequence advance_sequence(DomainKind domain);
  [[nodiscard]] ActionOutcome unapplied_outcome(SubmitOutcome outcome, EvidenceOutcome evidence,
                                                std::string detail, std::uint64_t now_micros,
                                                const AssetRecord* current) const;

  FacilitySnapshot snapshot_;
  GenerationSet generations_;
  FacilityRevision revision_{1};
  std::uint64_t applied_actions_ = 0;
  std::map<std::uint8_t, DomainFault> faults_;
  std::map<std::uint8_t, IncarnationId> incarnations_;
  std::map<std::pair<std::uint8_t, std::uint64_t>, ObservationSequence> sequences_;
  std::map<Digest, AppliedEntry> applied_;
};

class SimulatedAuthority final : public DomainAuthority {
 public:
  SimulatedAuthority(DomainKind domain, SimulatedPlant& plant, IncarnationId incarnation,
                     ControlEpoch control_epoch);
  [[nodiscard]] DomainKind domain() const noexcept override;
  [[nodiscard]] IncarnationId incarnation() const noexcept override;
  [[nodiscard]] ControlEpoch control_epoch() const noexcept override;
  [[nodiscard]] Result<ActionOutcome> submit(const ActionRequest& request, const AttemptId& attempt,
                                             const Digest& idempotency_key,
                                             std::uint64_t now_micros) override;

 private:
  DomainKind domain_ = DomainKind::Unspecified;
  SimulatedPlant* plant_ = nullptr;
  IncarnationId incarnation_;
  ControlEpoch control_epoch_;
};

class SimulatedAuthoritySet {
 public:
  SimulatedAuthoritySet(SimulatedPlant& plant, IncarnationId incarnation, ControlEpoch epoch);
  [[nodiscard]] DomainAuthority* find(DomainKind domain) noexcept;
  [[nodiscard]] const DomainAuthority* find(DomainKind domain) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
 private:
  // owns one SimulatedAuthority per valid DomainKind
  std::vector<std::unique_ptr<SimulatedAuthority>> authorities_;
};

// Deterministic example facility: one site with three racks, three assets each
// (nine assets), all Active/On/Normal/None/Attached/Present with distinct
// hardware and firmware generations (3 and 7 respectively: every asset carries
// both, and the two values differ so the two fields can never be confused) and
// part of the capacity reserved (16 of 64 units per asset).
//
// Identifiers are literals: site "site-alpha", racks "rack-a1".."rack-a3",
// assets "<rack>-node-1".."<rack>-node-3", tenant "tenant-blue". Because the
// site, the epoch and the generation set come from the caller, an incomplete
// one of those makes FacilitySnapshot::create() reject the whole facility; in
// that case a default (invalid) snapshot is returned and valid() is false.
[[nodiscard]] FacilitySnapshot make_example_facility(SiteId site, FacilityEpoch epoch,
                                                     GenerationSet generations);
}
