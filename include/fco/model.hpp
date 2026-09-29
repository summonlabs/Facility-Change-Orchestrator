#pragma once

// Change request, change plan, and the durable attempt record.
//
// A plan is a DAG of typed steps. Each step names the domain authority that
// owns the action, the preconditions that must hold, the effect that must be
// observed before the step counts as done, and how (or whether) it can be
// undone. Plan generation is not execution authority: producing a plan grants
// nothing. Only authorization under a live authority context, with current
// generations, permits a step to be issued.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "fco/authority.hpp"
#include "fco/digest.hpp"
#include "fco/domain.hpp"
#include "fco/error.hpp"
#include "fco/strong.hpp"
#include "fco/version.hpp"

namespace fco {

struct PlanScope {
  SiteId site;
  RackId rack;
  std::vector<AssetId> assets;

  [[nodiscard]] bool contains(const AssetId& id) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return !site.valid() && !rack.valid() && assets.empty(); }
  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] std::string describe() const;
};

struct ChangeStep {
  StepId id;
  DomainKind owner = DomainKind::Unspecified;
  ActionKind action = ActionKind::Unspecified;
  SafetyClass safety = SafetyClass::Unspecified;
  Reversibility reversibility = Reversibility::Unspecified;
  VerificationMode verification = VerificationMode::Unspecified;
  bool point_of_no_return = false;
  ActionRequest request;
  std::vector<Predicate> preconditions;
  std::vector<StepId> depends_on;
  EffectSpec expected_effect;
  std::optional<ActionRequest> compensation;
  std::string rationale;

  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;
  [[nodiscard]] std::string describe() const;
};

// Durable record of one attempt to issue one step. The attempt is written
// before the action leaves the process, so that a crash between "we decided to
// act" and "we observed the effect" leaves a PossiblyIssued record instead of an
// unrecorded destructive action.
struct AttemptRecord {
  AttemptId id;
  PlanId plan;
  PlanRevision plan_revision;
  StepId step;
  AttemptOrdinal ordinal;
  AttemptStatus status = AttemptStatus::NotIssued;
  Digest intent_digest;
  Digest idempotency_key;
  GenerationSet bound_generations;
  AuthorityContext bound_authority;
  ObservationSequence last_observation_sequence;
  Digest last_evidence;
  std::uint64_t issued_at_micros = 0;
  std::uint64_t updated_at_micros = 0;
  std::string note;

  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;
  [[nodiscard]] bool terminal() const noexcept;
  [[nodiscard]] bool resolved() const noexcept;
  [[nodiscard]] std::string describe() const;
};

struct ChangeRequest {
  ChangeRequestId id;
  ChangeKind kind;
  SiteId site;
  std::string intent;
  PlanScope scope;
  FacilityEpoch facility_epoch;
  PrincipalId actor;
  Revision revision;
  GenerationSet bound_generations;
  Digest evidence_digest;
  std::vector<ChangeStep> steps;

  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;
  [[nodiscard]] std::string describe() const;
};

struct ChangePlan {
  PlanId id;
  ChangeRequestId request_id;
  PlanRevision revision;
  PlanState state = PlanState::Unspecified;
  ChangeKind kind;
  SiteId site;
  std::string intent;
  PlanScope scope;
  FacilityEpoch facility_epoch;
  PolicyGeneration policy_generation;
  GenerationSet bound_generations;
  Digest request_digest;
  Digest evidence_digest;
  Digest facility_digest;
  AuthorityContext authority;
  std::vector<ChangeStep> steps;
  std::map<StepId, StepStatus> step_status;
  std::vector<AttemptRecord> attempts;
  // Generations this plan has itself advanced by verified effects, recorded with
  // the exact value it produced. An advance to any other value, for any other
  // generation, fences the remaining steps as stale.
  std::vector<GenerationDelta> self_advanced;
  // The facility revision produced by this plan's own last verified effect. A
  // facility revision that moved for any other reason means the world changed
  // underneath the plan, and the plan is fenced rather than re-interpreted.
  FacilityRevision explained_revision;
  std::string transition_note;
  std::uint64_t created_at_micros = 0;
  std::uint64_t updated_at_micros = 0;

  [[nodiscard]] const ChangeStep* find_step(const StepId& id) const noexcept;
  [[nodiscard]] ChangeStep* find_step(const StepId& id) noexcept;
  [[nodiscard]] StepStatus status_of(const StepId& id) const noexcept;
  [[nodiscard]] const AttemptRecord* find_attempt(const AttemptId& id) const noexcept;
  [[nodiscard]] AttemptOrdinal next_ordinal(const StepId& id) const noexcept;
  [[nodiscard]] std::size_t count_with_status(StepStatus status) const noexcept;
  [[nodiscard]] std::size_t unresolved_attempt_count() const noexcept;
  [[nodiscard]] bool has_unresolved_attempts() const noexcept;
  [[nodiscard]] bool terminal() const noexcept;
  [[nodiscard]] bool partially_applied() const noexcept;
  [[nodiscard]] bool any_non_reversible_applied() const noexcept;

  // Deterministic Kahn topological order with lexicographic tie-break on step
  // id, so the same DAG always yields the same order regardless of container
  // iteration or insertion order.
  [[nodiscard]] Result<std::vector<StepId>> topological_order() const;
  [[nodiscard]] Result<std::vector<StepId>> direct_predecessors(const StepId& id) const;
  [[nodiscard]] Result<std::vector<StepId>> direct_successors(const StepId& id) const;
  [[nodiscard]] Result<std::vector<StepId>> compensate_order() const;

  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;
  [[nodiscard]] std::string describe() const;
};

// Validates the internal consistency of a request: identities, generations,
// scope, step vocabulary, duplicates, edges, compensation semantics, and
// point-of-no-return sanity. Diagnostics are collected in full and the primary
// condition is selected by documented precedence.
void validate_request(const ChangeRequest& request, Diagnostics& diagnostics);

// Validates a plan built from a request.
void validate_plan(const ChangePlan& plan, Diagnostics& diagnostics);

[[nodiscard]] constexpr bool is_terminal_plan_state(PlanState state) noexcept {
  return state == PlanState::Completed || state == PlanState::Failed ||
         state == PlanState::RolledBack || state == PlanState::Cancelled ||
         state == PlanState::ReplanRequired;
}

[[nodiscard]] constexpr bool is_settled_step_status(StepStatus status) noexcept {
  return status == StepStatus::Verified || status == StepStatus::Failed ||
         status == StepStatus::Compensated || status == StepStatus::Blocked ||
         status == StepStatus::NonCompensable;
}

// The explicit plan state machine. Any transition not listed is rejected with
// InvalidStateTransition; there are no implicit transitions.
[[nodiscard]] bool is_legal_plan_transition(PlanState from, PlanState to) noexcept;
[[nodiscard]] std::string_view plan_transition_table() noexcept;

}  // namespace fco
