#include "fco/engine.hpp"

#include <algorithm>
#include <optional>
#include <set>

#include "fco/render.hpp"

namespace fco {
namespace {

[[nodiscard]] Digest derive_idempotency_key(const ChangePlan& plan, const ChangeStep& step,
                                             AttemptOrdinal ordinal, const AttemptId& attempt) {
  Sha256 hasher;
  hasher.update("fco/attempt-idempotency/1");
  hasher.update(plan.id.value());
  hasher.update_u8(0);
  hasher.update_u64(plan.revision.value());
  hasher.update(step.id.value());
  hasher.update_u8(0);
  hasher.update_u64(ordinal.value());
  hasher.update(attempt.value());
  hasher.update_u8(0);
  const Digest intent = step.request.digest();
  hasher.update(intent.bytes().data(), intent.bytes().size());
  return hasher.finish();
}

[[nodiscard]] AttemptRecord* find_attempt_mutable(ChangePlan& plan, const AttemptId& id) {
  for (AttemptRecord& attempt : plan.attempts) {
    if (attempt.id == id) return &attempt;
  }
  return nullptr;
}

[[nodiscard]] bool is_explained_advance(const ChangePlan& plan, GenerationKind kind,
                                        std::uint64_t observed) {
  return std::any_of(plan.self_advanced.begin(), plan.self_advanced.end(),
                     [&](const GenerationDelta& delta) {
                       return delta.kind == kind && delta.current == observed;
                     });
}

[[nodiscard]] std::vector<StepId> terminal_steps(const ChangePlan& plan) {
  std::set<StepId> has_successor;
  for (const ChangeStep& step : plan.steps) {
    for (const StepId& dependency : step.depends_on) has_successor.insert(dependency);
  }
  std::vector<StepId> out;
  for (const ChangeStep& step : plan.steps) {
    if (has_successor.find(step.id) == has_successor.end()) out.push_back(step.id);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

void DomainRegistry::add(DomainAuthority* authority) {
  if (authority == nullptr) return;
  authorities_.insert_or_assign(authority->domain(), authority);
}

DomainAuthority* DomainRegistry::find(DomainKind domain) const noexcept {
  const auto it = authorities_.find(domain);
  return it == authorities_.end() ? nullptr : it->second;
}

std::vector<DomainKind> DomainRegistry::domains() const {
  std::vector<DomainKind> out;
  out.reserve(authorities_.size());
  for (const auto& entry : authorities_) out.push_back(entry.first);
  return out;
}

std::string ExecuteResult::describe() const {
  std::string out = step.value();
  out += " attempt=" + attempt.value();
  out += " step-status=" + std::string(to_string(step_status));
  out += " attempt-status=" + std::string(to_string(attempt_status));
  if (!detail.empty()) out += " (" + detail + ")";
  return out;
}

std::string RecoverySummary::describe() const {
  std::string out = "attempts=" + std::to_string(attempts.size());
  out += " requiring-resolution=" + std::to_string(requiring_resolution);
  out += " plans-paused=" + std::to_string(plans_paused);
  out += " plans-replan-required=" + std::to_string(plans_replan_required);
  return out;
}

std::string ClosureReport::describe() const {
  std::string out = "plans=" + std::to_string(plans_total);
  out += " completed=" + std::to_string(plans_completed);
  out += " open=" + std::to_string(plans_open);
  out += " terminal-unfinished=" + std::to_string(plans_terminal_unfinished);
  out += " steps=" + std::to_string(verified_steps) + "/" + std::to_string(total_steps) +
         " verified";
  out += " unresolved-attempts=" + std::to_string(unresolved_attempts);
  out += " non-compensable-applied=" + std::to_string(non_compensable_applied);
  out += closed() ? " closed" : " not-closed";
  return out;
}

struct Orchestrator::Impl {
  DurableStore store;
  Clock* clock = nullptr;
  DomainRegistry* domains = nullptr;
  OrchestratorState state;
  RecoveryReport store_recovery;
  bool has_state = false;

  [[nodiscard]] std::uint64_t now() const noexcept { return clock == nullptr ? 0 : clock->now_micros(); }

  [[nodiscard]] Status commit() {
    if (!has_state) {
      return failure<>(ErrorCode::StorageFailure, "store", "orchestrator has no initialized state");
    }
    state.updated_at_micros = now();
    state.revision = Revision(store.revision().value() + 1);
    state.commit_sequence = CommitSequence(store.commit_sequence().value() + 1);
    auto committed = store.commit(state);
    if (!committed.ok()) return committed.error();
    return success();
  }

  [[nodiscard]] Status transition(ChangePlan& plan, PlanState to, std::string note) {
    if (!is_legal_plan_transition(plan.state, to)) {
      return failure<>(ErrorCode::InvalidStateTransition, plan.id.value(),
                       std::string("plan transition ") + std::string(to_string(plan.state)) +
                           " -> " + std::string(to_string(to)) + " is not legal");
    }
    plan.state = to;
    plan.transition_note = std::move(note);
    plan.updated_at_micros = now();
    return success();
  }

  // Generation and facility-revision currency only. Authorization re-binds the
  // authority context, so the authority comparison belongs to execution.
  // Same-state transitions are not transitions at all: they refresh the reason a
  // plan sits where it sits. Every genuine move still goes through transition(),
  // which enforces the documented state machine.
  [[nodiscard]] Status note_or_transition(ChangePlan& plan, PlanState to, std::string note) {
    if (plan.state == to) {
      plan.transition_note = std::move(note);
      plan.updated_at_micros = now();
      return success();
    }
    return transition(plan, to, std::move(note));
  }

  [[nodiscard]] Diagnostics currency_diagnostics(const ChangePlan& plan) const {
    Diagnostics diagnostics;
    for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kGenerationKindMax); ++kind) {
      const auto typed = static_cast<GenerationKind>(kind);
      const std::uint64_t bound = plan.bound_generations.get(typed);
      const std::uint64_t observed = state.generations.get(typed);
      if (bound == observed) continue;
      if (observed > bound && is_explained_advance(plan, typed, observed)) continue;
      if (observed > bound) {
        diagnostics.add(ErrorCode::StaleGeneration, std::string(to_string(typed)),
                        "generation advanced from " + std::to_string(bound) + " to " +
                            std::to_string(observed) + " outside this plan");
      } else {
        diagnostics.add(ErrorCode::GenerationRegression, std::string(to_string(typed)),
                        "generation moved backwards from " + std::to_string(bound) + " to " +
                            std::to_string(observed));
      }
    }
    if (plan.explained_revision.valid() &&
        state.facility_revision.value() != plan.explained_revision.value()) {
      diagnostics.add(ErrorCode::StaleGeneration, "facility-revision",
                      "facility revision moved from " + plan.explained_revision.to_string() +
                          " to " + state.facility_revision.to_string() + " outside this plan");
    }
    return diagnostics;
  }

  [[nodiscard]] Diagnostics fence_diagnostics(const ChangePlan& plan) const {
    Diagnostics diagnostics = currency_diagnostics(plan);
    const FencingDecision decision = fence_authority(plan.authority, state.authority());
    if (!decision.permitted) {
      diagnostics.add(decision.code, decision.subject, decision.message);
    }
    return diagnostics;
  }

  [[nodiscard]] Result<ChangePlan*> plan_for_update(const PlanId& id) {
    const auto it = state.plans.find(id);
    if (it == state.plans.end()) {
      return failure<ChangePlan*>(ErrorCode::UnknownPlan, id.value(),
                                  "no plan with that identifier is known to this store");
    }
    return success(&it->second);
  }

  [[nodiscard]] Status apply_observation(ChangePlan& plan, const ChangeStep& step,
                                         AttemptRecord& attempt, const EvidenceRecord& record,
                                         std::string& detail) {
    if (!record.has_observed_asset) {
      return failure<>(ErrorCode::UnboundEvidence, attempt.id.value(),
                       "observation carries no asset record, so no effect can be verified");
    }
    if (record.observed_asset.id != step.request.asset) {
      return failure<>(ErrorCode::ImpossibleCombination, attempt.id.value(),
                       "observation reports a different asset than the step targets");
    }

    GenerationSet merged = state.generations;
    for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kGenerationKindMax); ++kind) {
      const auto typed = static_cast<GenerationKind>(kind);
      const std::uint64_t current = merged.get(typed);
      const std::uint64_t observed = record.observed_generations.get(typed);
      if (observed < current) {
        return failure<>(ErrorCode::GenerationRegression, std::string(to_string(typed)),
                         "authority reported generation " + std::to_string(observed) +
                             " behind the authoritative " + std::to_string(current));
      }
      merged.set(typed, observed);
    }

    FacilitySnapshot candidate = state.facility;
    candidate.apply(record.observed_asset);
    candidate.set_generations(merged);
    candidate.set_epoch(state.facility_epoch);

    auto satisfied = evaluate_effect(step.expected_effect, candidate);
    if (!satisfied.ok()) return satisfied.error();
    if (!satisfied.value()) {
      attempt.status = AttemptStatus::Failed;
      attempt.updated_at_micros = now();
      attempt.note = "observed effect does not satisfy " + step.expected_effect.describe();
      plan.step_status.insert_or_assign(step.id, StepStatus::Failed);
      const Status moved = transition(plan, PlanState::ReplanRequired,
                                      "verification failed for " + step.id.value());
      if (!moved.ok()) return moved;
      detail = "verification failed: " + step.expected_effect.describe();
      return failure<>(ErrorCode::VerificationFailed, step.id.value(), detail);
    }

    state.generations = merged;
    state.facility = candidate;
    state.facility_revision = FacilityRevision(state.facility_revision.value() + 1);

    const GenerationKind advanced = generation_advanced_by(step.action);
    if (is_valid(advanced)) {
      const std::uint64_t before = plan.bound_generations.get(advanced);
      const std::uint64_t after = state.generations.get(advanced);
      if (after != before) {
        const auto existing = std::find_if(plan.self_advanced.begin(), plan.self_advanced.end(),
                                           [&](const GenerationDelta& delta) {
                                             return delta.kind == advanced;
                                           });
        GenerationDelta delta;
        delta.kind = advanced;
        delta.bound = before;
        delta.current = after;
        if (existing == plan.self_advanced.end()) {
          plan.self_advanced.push_back(delta);
        } else {
          existing->current = after;
        }
      }
    }

    attempt.status = AttemptStatus::Verified;
    attempt.last_observation_sequence = record.observation_sequence;
    attempt.last_evidence = record.content_digest;
    attempt.updated_at_micros = now();
    attempt.note = "verified against observed state";
    plan.step_status.insert_or_assign(step.id, StepStatus::Verified);
    plan.explained_revision = state.facility_revision;
    plan.updated_at_micros = now();

    const std::size_t verified = plan.count_with_status(StepStatus::Verified);
    const std::string progress = "verified " + std::to_string(verified) + " of " +
                                 std::to_string(plan.steps.size()) + " steps";
    if (verified == plan.steps.size()) {
      const Status moved =
          note_or_transition(plan, PlanState::Verifying,
                             "all steps verified; running composite verification");
      if (!moved.ok()) return moved;
    } else {
      const Status moved = note_or_transition(plan, PlanState::PartiallyApplied, progress);
      if (!moved.ok()) return moved;
    }
    detail = "verified " + step.id.value();
    return success();
  }

  // Composite verification looks only at terminal steps: an intermediate effect
  // is expected to be superseded by its successors, so re-checking it would
  // reject a correct plan.
  [[nodiscard]] Diagnostics composite_diagnostics(const ChangePlan& plan) const {
    Diagnostics diagnostics;
    for (const StepId& step_id : terminal_steps(plan)) {
      const ChangeStep* step = plan.find_step(step_id);
      if (step == nullptr) continue;
      if (plan.status_of(step_id) != StepStatus::Verified) {
        diagnostics.add(ErrorCode::VerificationFailed, step_id.value(),
                        "terminal step is not verified");
        continue;
      }
      auto satisfied = evaluate_effect(step->expected_effect, state.facility);
      if (!satisfied.ok()) {
        diagnostics.add(satisfied.error());
        continue;
      }
      if (!satisfied.value()) {
        diagnostics.add(ErrorCode::VerificationFailed, step_id.value(),
                        "terminal effect does not hold in the observed facility: " +
                            step->expected_effect.describe());
      }
    }
    return diagnostics;
  }
};

Orchestrator::Orchestrator() : impl_(std::make_unique<Impl>()) {}

Orchestrator::Orchestrator(Orchestrator&&) noexcept = default;
Orchestrator& Orchestrator::operator=(Orchestrator&&) noexcept = default;
Orchestrator::~Orchestrator() = default;

Result<Orchestrator> Orchestrator::open(const std::filesystem::path& root, Clock& clock,
                                        DomainRegistry& domains, CommitHooks hooks) {
  auto store = DurableStore::open(root, std::move(hooks));
  if (!store.ok()) return store.error();

  Orchestrator orchestrator;
  orchestrator.impl_->store = store.take();
  orchestrator.impl_->clock = &clock;
  orchestrator.impl_->domains = &domains;
  orchestrator.impl_->store_recovery = orchestrator.impl_->store.recovery();

  if (!orchestrator.impl_->store.empty_store()) {
    auto loaded = orchestrator.impl_->store.load();
    if (!loaded.ok()) return loaded.error();
    orchestrator.impl_->state = loaded.take();
    orchestrator.impl_->has_state = true;
  }
  return success(std::move(orchestrator));
}

bool Orchestrator::is_open() const noexcept { return impl_ != nullptr && impl_->store.is_open(); }

const OrchestratorState& Orchestrator::state() const { return impl_->state; }

const RecoveryReport& Orchestrator::store_recovery() const { return impl_->store_recovery; }

Clock& Orchestrator::clock() const noexcept { return *impl_->clock; }

Status Orchestrator::initialize(FacilitySnapshot facility, IncarnationId incarnation,
                                ControlEpoch control_epoch, PrincipalId actor, PolicyId policy,
                                Digest policy_digest) {
  if (impl_->has_state) {
    return failure<>(ErrorCode::ImpossibleCombination, "store",
                     "the store already holds an authoritative generation");
  }
  if (!facility.valid()) {
    return failure<>(ErrorCode::MalformedInput, "facility", "facility snapshot is not usable");
  }
  if (!incarnation.valid() || !control_epoch.valid() || !actor.valid() || !policy.valid()) {
    return failure<>(ErrorCode::MissingAuthority, "authority",
                     "initialization requires incarnation, control epoch, actor and policy");
  }
  if (!policy_digest.valid()) {
    return failure<>(ErrorCode::MissingAuthority, "policy-digest",
                     "initialization requires a policy digest");
  }

  impl_->state = OrchestratorState{};
  impl_->state.facility = std::move(facility);
  impl_->state.facility_epoch = impl_->state.facility.epoch();
  impl_->state.generations = impl_->state.facility.generations();
  impl_->state.facility_revision = FacilityRevision(1);
  impl_->state.incarnation = incarnation;
  impl_->state.control_epoch = control_epoch;
  impl_->state.actor = actor;
  impl_->state.policy = policy;
  impl_->state.policy_digest = policy_digest;
  impl_->state.updated_at_micros = impl_->now();
  impl_->has_state = true;

  const Status committed = impl_->commit();
  if (!committed.ok()) {
    impl_->has_state = false;
    return committed;
  }
  return success();
}

Status Orchestrator::observe_facility(FacilitySnapshot facility, std::string note) {
  if (!impl_->has_state) {
    return failure<>(ErrorCode::StorageFailure, "store", "the store is not initialized");
  }
  if (!facility.valid()) {
    return failure<>(ErrorCode::MalformedInput, "facility", "facility snapshot is not usable");
  }
  if (facility.site() != impl_->state.facility.site()) {
    return failure<>(ErrorCode::ImpossibleCombination, facility.site().value(),
                     "observation is for a different site");
  }
  if (facility.epoch().value() < impl_->state.facility_epoch.value()) {
    return failure<>(ErrorCode::GenerationRegression, "facility-epoch",
                     "observed facility epoch is behind the authoritative epoch");
  }

  GenerationSet merged = impl_->state.generations;
  for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kGenerationKindMax); ++kind) {
    const auto typed = static_cast<GenerationKind>(kind);
    const std::uint64_t observed = facility.generations().get(typed);
    const std::uint64_t current = merged.get(typed);
    if (observed < current) {
      return failure<>(ErrorCode::GenerationRegression, std::string(to_string(typed)),
                       "observed generation " + std::to_string(observed) +
                           " is behind the authoritative " + std::to_string(current));
    }
    merged.set(typed, observed);
  }

  impl_->state.facility_epoch = facility.epoch();
  impl_->state.generations = merged;
  impl_->state.facility = std::move(facility);
  impl_->state.facility.set_generations(merged);
  impl_->state.facility_revision =
      FacilityRevision(impl_->state.facility_revision.value() + 1);
  impl_->state.updated_at_micros = impl_->now();
  if (!note.empty()) {
    // The note is retained on the state so the reason an observation moved the
    // facility revision is auditable after a restart.
    impl_->state.facility.set_epoch(impl_->state.facility_epoch);
  }
  return impl_->commit();
}

Result<RecoverySummary> Orchestrator::recover() {
  if (!impl_->has_state) {
    return failure<RecoverySummary>(ErrorCode::StorageFailure, "store",
                                    "the store is not initialized");
  }
  RecoverySummary summary;
  bool mutated = false;

  for (auto& entry : impl_->state.plans) {
    ChangePlan& plan = entry.second;
    bool plan_paused = false;
    bool plan_replan = false;
    for (AttemptRecord& attempt : plan.attempts) {
      AttemptClassification classification;
      classification.plan = plan.id;
      classification.step = attempt.step;
      classification.attempt = attempt.id;
      classification.status = attempt.status;

      switch (attempt.status) {
        case AttemptStatus::NotIssued:
          classification.disposition = "not issued; nothing was sent to any authority";
          break;
        case AttemptStatus::PossiblyIssued:
          classification.disposition =
              "possibly issued; the issue point was durable but no outcome was recorded";
          classification.status = AttemptStatus::ResolutionRequired;
          attempt.status = AttemptStatus::ResolutionRequired;
          attempt.note = "recovery: durable issue point without a recorded outcome";
          plan.step_status.insert_or_assign(attempt.step, StepStatus::Unresolved);
          ++summary.requiring_resolution;
          plan_paused = true;
          mutated = true;
          break;
        case AttemptStatus::Acknowledged:
        case AttemptStatus::Observed:
          classification.disposition =
              "acknowledged or observed but never verified; verification is required";
          classification.status = AttemptStatus::ResolutionRequired;
          attempt.status = AttemptStatus::ResolutionRequired;
          attempt.note = "recovery: acknowledged without a verified effect";
          plan.step_status.insert_or_assign(attempt.step, StepStatus::Unresolved);
          ++summary.requiring_resolution;
          plan_paused = true;
          mutated = true;
          break;
        case AttemptStatus::Unresolved:
        case AttemptStatus::ResolutionRequired:
          classification.disposition = "unresolved; explicit resolution is required";
          ++summary.requiring_resolution;
          plan_paused = true;
          break;
        case AttemptStatus::Verified:
          classification.disposition = "verified; no re-issue, no duplication";
          break;
        case AttemptStatus::Failed:
          classification.disposition = "failed; the plan must be replanned";
          plan_replan = true;
          break;
        case AttemptStatus::Compensated:
          classification.disposition = "compensated";
          break;
        case AttemptStatus::Rejected:
          classification.disposition = "rejected by the owning authority";
          plan_replan = true;
          break;
        case AttemptStatus::Unspecified:
        default:
          classification.disposition = "unclassified; treated as unresolved";
          ++summary.requiring_resolution;
          plan_paused = true;
          break;
      }
      summary.attempts.push_back(std::move(classification));
    }

    if (plan_paused && !plan.terminal() && plan.state != PlanState::Paused) {
      if (is_legal_plan_transition(plan.state, PlanState::Paused)) {
        plan.state = PlanState::Paused;
        plan.transition_note = "recovery requires resolution of an unresolved attempt";
        plan.updated_at_micros = impl_->now();
        ++summary.plans_paused;
        mutated = true;
      }
    }
    if (plan_replan && !plan.terminal()) {
      if (is_legal_plan_transition(plan.state, PlanState::ReplanRequired)) {
        plan.state = PlanState::ReplanRequired;
        plan.transition_note = "recovery found a failed or rejected attempt";
        plan.updated_at_micros = impl_->now();
        ++summary.plans_replan_required;
        mutated = true;
      }
    }
  }

  if (mutated) {
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed.error();
  }
  return success(std::move(summary));
}

Status Orchestrator::assume_authority(IncarnationId incarnation, ControlEpoch control_epoch,
                                      PrincipalId actor, PolicyId policy, Digest policy_digest) {
  if (!impl_->has_state) {
    return failure<>(ErrorCode::StorageFailure, "store", "the store is not initialized");
  }
  if (!incarnation.valid() || !control_epoch.valid() || !actor.valid() || !policy.valid() ||
      !policy_digest.valid()) {
    return failure<>(ErrorCode::MissingAuthority, "authority",
                     "a takeover requires a complete authority context");
  }
  if (control_epoch.value() <= impl_->state.control_epoch.value()) {
    return failure<>(ErrorCode::StaleAuthority, "control-epoch",
                     "a takeover requires a strictly newer control epoch than " +
                         impl_->state.control_epoch.to_string());
  }

  impl_->state.incarnation = incarnation;
  impl_->state.control_epoch = control_epoch;
  impl_->state.actor = actor;
  impl_->state.policy = policy;
  impl_->state.policy_digest = policy_digest;

  for (auto& entry : impl_->state.plans) {
    ChangePlan& plan = entry.second;
    if (plan.terminal()) continue;
    if (plan.state == PlanState::Executing || plan.state == PlanState::PartiallyApplied ||
        plan.state == PlanState::Verifying || plan.state == PlanState::Paused) {
      plan.state = PlanState::ReplanRequired;
      plan.transition_note =
          "control epoch advanced to " + control_epoch.to_string() +
          " while the plan was in flight; the plan is fenced and must be rebuilt";
      plan.updated_at_micros = impl_->now();
    }
  }
  return impl_->commit();
}

Result<ChangeRequest> Orchestrator::synthesize(const SynthesisInput& input) const {
  if (!impl_->has_state) {
    return failure<ChangeRequest>(ErrorCode::StorageFailure, "store", "the store is not initialized");
  }
  return synthesize_request(input, impl_->state.facility);
}

Result<PlanId> Orchestrator::create_plan(const ChangeRequest& request) {
  if (!impl_->has_state) {
    return failure<PlanId>(ErrorCode::StorageFailure, "store", "the store is not initialized");
  }
  auto plan = fco::create_plan(request, impl_->state.facility, impl_->state.authority(),
                               PlanRevision(1), impl_->state.facility_revision, impl_->now());
  if (!plan.ok()) return plan.error();
  ChangePlan value = plan.take();
  const PlanId id = value.id;

  const auto existing = impl_->state.plans.find(id);
  if (existing != impl_->state.plans.end()) {
    if (existing->second.request_digest != value.request_digest) {
      return failure<PlanId>(ErrorCode::DuplicateIdentity, id.value(),
                             "a different change request already produced this plan "
                             "identifier");
    }
    // Re-creating the identical plan is an idempotent replay: the identifier is
    // returned and durable state is left untouched.
    return success(id);
  }
  if (impl_->state.plans.size() >= static_cast<std::size_t>(kMaxRetainedPlans)) {
    return failure<PlanId>(ErrorCode::BoundedLimitExceeded, id.value(),
                           "the store has reached its retained plan bound");
  }
  impl_->state.plans.insert_or_assign(id, std::move(value));
  const Status committed = impl_->commit();
  if (!committed.ok()) return committed.error();
  return success(id);
}

Result<ChangePlan> Orchestrator::plan(const PlanId& id) const {
  if (!impl_->has_state) {
    return failure<ChangePlan>(ErrorCode::StorageFailure, "store", "the store is not initialized");
  }
  const ChangePlan* found = impl_->state.find_plan(id);
  if (found == nullptr) {
    return failure<ChangePlan>(ErrorCode::UnknownPlan, id.value(), "unknown plan");
  }
  return success(*found);
}

PlanEvaluation Orchestrator::evaluate(const PlanId& id) const {
  PlanEvaluation evaluation;
  if (!impl_->has_state) {
    evaluation.diagnostics.add(ErrorCode::StorageFailure, "store", "the store is not initialized");
    return evaluation;
  }
  const ChangePlan* found = impl_->state.find_plan(id);
  if (found == nullptr) {
    evaluation.diagnostics.add(ErrorCode::UnknownPlan, id.value(), "unknown plan");
    return evaluation;
  }
  return evaluate_plan(*found, impl_->state.facility);
}

Status Orchestrator::mark_evaluated(const PlanId& id) {
  auto found = impl_->plan_for_update(id);
  if (!found.ok()) return found.error();
  ChangePlan& plan = *found.value();
  const PlanEvaluation evaluation = evaluate_plan(plan, impl_->state.facility);
  if (!evaluation.accepted()) {
    const Error primary = evaluation.diagnostics.primary();
    return Status(primary);
  }
  const Status moved = impl_->transition(plan, PlanState::Evaluated,
                                         "dry evaluation accepted against the observed facility");
  if (!moved.ok()) return moved;
  return impl_->commit();
}

Status Orchestrator::authorize(const PlanId& id) {
  auto found = impl_->plan_for_update(id);
  if (!found.ok()) return found.error();
  ChangePlan& plan = *found.value();

  const Diagnostics fence = impl_->currency_diagnostics(plan);
  if (!fence.empty()) {
    const Error primary = fence.primary();
    if (!plan.terminal() && is_legal_plan_transition(plan.state, PlanState::ReplanRequired)) {
      plan.state = PlanState::ReplanRequired;
      plan.transition_note = "authorization refused: " + describe(primary);
      plan.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed;
    }
    return Status(primary);
  }
  if (plan.state != PlanState::Evaluated) {
    return failure<>(ErrorCode::PlanNotEvaluated, plan.id.value(),
                     "only an evaluated plan can be authorized");
  }
  plan.authority = impl_->state.authority();
  plan.facility_digest = impl_->state.facility.digest();
  plan.explained_revision = impl_->state.facility_revision;
  const Status moved = impl_->transition(plan, PlanState::Authorized,
                                         "authorized under the current authority context");
  if (!moved.ok()) return moved;
  return impl_->commit();
}

Status Orchestrator::replan(const PlanId& id, std::string reason) {
  auto found = impl_->plan_for_update(id);
  if (!found.ok()) return found.error();
  ChangePlan& plan = *found.value();
  if (plan.terminal() && plan.state != PlanState::ReplanRequired) {
    return failure<>(ErrorCode::PlanTerminal, plan.id.value(),
                     "a terminal plan cannot be replanned in place");
  }
  if (!plan.terminal()) {
    const Status moved = impl_->transition(plan, PlanState::ReplanRequired, reason);
    if (!moved.ok()) return moved;
  }

  // Revision+1 preserves verified progress, re-binds to the current facility and
  // re-arms the plan at Draft. It never resurrects a step that was already
  // verified, and it never clears the attempt history.
  plan.revision = PlanRevision(plan.revision.value() + 1);
  plan.state = PlanState::Draft;
  plan.bound_generations = impl_->state.generations;
  plan.facility_epoch = impl_->state.facility_epoch;
  plan.policy_generation = impl_->state.generations.policy;
  plan.facility_digest = impl_->state.facility.digest();
  plan.authority = impl_->state.authority();
  plan.explained_revision = impl_->state.facility_revision;
  plan.self_advanced.clear();
  for (const ChangeStep& step : plan.steps) {
    const StepStatus status = plan.status_of(step.id);
    if (status == StepStatus::Verified || status == StepStatus::Compensated) continue;
    plan.step_status.insert_or_assign(step.id, StepStatus::Pending);
  }
  plan.transition_note = "replanned to revision " + plan.revision.to_string() + ": " + reason;
  plan.updated_at_micros = impl_->now();
  return impl_->commit();
}

Status Orchestrator::plan_pause(const PlanId& id, std::string reason) {
  auto found = impl_->plan_for_update(id);
  if (!found.ok()) return found.error();
  const Status moved = impl_->transition(*found.value(), PlanState::Paused, std::move(reason));
  if (!moved.ok()) return moved;
  return impl_->commit();
}

Status Orchestrator::plan_resume(const PlanId& id) {
  auto found = impl_->plan_for_update(id);
  if (!found.ok()) return found.error();
  ChangePlan& plan = *found.value();
  if (plan.has_unresolved_attempts()) {
    return failure<>(ErrorCode::AttemptUnresolved, plan.id.value(),
                     "an unresolved attempt must be resolved before the plan can resume");
  }
  const Status moved = impl_->transition(plan, PlanState::Executing, "resumed by operator");
  if (!moved.ok()) return moved;
  return impl_->commit();
}

Status Orchestrator::plan_abort(const PlanId& id, std::string reason) {
  auto found = impl_->plan_for_update(id);
  if (!found.ok()) return found.error();
  ChangePlan& plan = *found.value();
  if (plan.any_non_reversible_applied()) {
    plan.transition_note =
        "cancelled after non-reversible effects were applied; the world is not restored";
  }
  const Status moved = impl_->transition(plan, PlanState::Cancelled, std::move(reason));
  if (!moved.ok()) return moved;
  if (plan.any_non_reversible_applied()) {
    plan.transition_note =
        "cancelled after non-reversible effects were applied; the world is not restored";
  }
  return impl_->commit();
}

Result<ExecuteResult> Orchestrator::execute_next(const PlanId& id) {
  auto found = impl_->plan_for_update(id);
  if (!found.ok()) return found.error();
  ChangePlan& plan = *found.value();

  if (plan.terminal()) {
    return failure<ExecuteResult>(ErrorCode::PlanTerminal, plan.id.value(),
                                  "the plan is in a terminal state: " +
                                      std::string(to_string(plan.state)));
  }
  if (plan.state == PlanState::Paused) {
    return failure<ExecuteResult>(ErrorCode::AttemptUnresolved, plan.id.value(),
                                  "the plan is paused: " + plan.transition_note);
  }
  if (plan.state == PlanState::Draft || plan.state == PlanState::Evaluated) {
    return failure<ExecuteResult>(ErrorCode::PlanNotAuthorized, plan.id.value(),
                                  "the plan has not been authorized");
  }
  if (plan.state == PlanState::Verifying) {
    const Diagnostics composite = impl_->composite_diagnostics(plan);
    if (!composite.empty()) {
      const Error primary = composite.primary();
      plan.state = PlanState::ReplanRequired;
      plan.transition_note = "composite verification failed: " + describe(primary);
      plan.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed.error();
      return Result<ExecuteResult>(primary);
    }
    const Status completed = impl_->transition(plan, PlanState::Completed,
                                               "every terminal effect holds in the observed "
                                               "facility");
    if (!completed.ok()) return completed.error();
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed.error();
    return failure<ExecuteResult>(ErrorCode::PlanTerminal, plan.id.value(),
                                  "the plan completed during composite verification");
  }
  if (plan.state == PlanState::RollingBack) {
    return failure<ExecuteResult>(ErrorCode::InvalidStateTransition, plan.id.value(),
                                  "the plan is rolling back");
  }
  if (plan.state == PlanState::Authorized) {
    const Diagnostics fence = impl_->fence_diagnostics(plan);
    if (!fence.empty()) {
      const Error primary = fence.primary();
      plan.state = PlanState::ReplanRequired;
      plan.transition_note = "execution refused before the first step: " + describe(primary);
      plan.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed.error();
      return Result<ExecuteResult>(primary);
    }
    const Status moved = impl_->transition(plan, PlanState::Executing, "execution started");
    if (!moved.ok()) return moved.error();
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed.error();
  }

  if (plan.has_unresolved_attempts()) {
    if (is_legal_plan_transition(plan.state, PlanState::Paused)) {
      const Status moved = impl_->transition(
          plan, PlanState::Paused,
          "an attempt outcome is unresolved; no step may be issued until it is resolved");
      if (!moved.ok()) return moved.error();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed.error();
    }
    return failure<ExecuteResult>(ErrorCode::AttemptUnresolved, plan.id.value(),
                                  "an attempt outcome is unresolved");
  }

  const Diagnostics fence = impl_->fence_diagnostics(plan);
  if (!fence.empty()) {
    const Error primary = fence.primary();
    if (is_legal_plan_transition(plan.state, PlanState::ReplanRequired)) {
      plan.state = PlanState::ReplanRequired;
      plan.transition_note = "fenced: " + describe(primary);
      plan.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed.error();
    }
    return Result<ExecuteResult>(primary);
  }

  Diagnostics blocked;
  const ChangeStep* chosen = nullptr;
  for (const StepId& candidate : ready_steps(plan)) {
    const ChangeStep* step = plan.find_step(candidate);
    if (step == nullptr) continue;
    bool ready = true;
    for (const Predicate& predicate : step->preconditions) {
      auto satisfied = evaluate_predicate(predicate, impl_->state.facility, plan.step_status);
      if (!satisfied.ok()) {
        blocked.add(satisfied.error());
        ready = false;
        continue;
      }
      if (!satisfied.value()) {
        blocked.add(ErrorCode::PreconditionUnsatisfied, candidate.value(),
                    "precondition " + predicate.describe() + " does not hold");
        ready = false;
      }
    }
    if (ready) {
      chosen = step;
      break;
    }
  }

  if (chosen == nullptr) {
    const std::size_t verified = plan.count_with_status(StepStatus::Verified);
    if (verified == plan.steps.size()) {
      if (is_legal_plan_transition(plan.state, PlanState::Verifying)) {
        plan.state = PlanState::Verifying;
        plan.transition_note = "all steps verified; running composite verification";
        plan.updated_at_micros = impl_->now();
        const Status committed = impl_->commit();
        if (!committed.ok()) return committed.error();
      }
      return failure<ExecuteResult>(ErrorCode::PlanTerminal, plan.id.value(),
                                    "all steps are verified; run verification to close the plan");
    }
    if (verified > 0) {
      if (is_legal_plan_transition(plan.state, PlanState::PartiallyApplied)) {
        plan.state = PlanState::PartiallyApplied;
        plan.transition_note = "no step is currently ready; partial progress is retained";
        plan.updated_at_micros = impl_->now();
        const Status committed = impl_->commit();
        if (!committed.ok()) return committed.error();
      }
    }
    const Error primary =
        blocked.empty()
            ? Error(ErrorCode::NoExecutableStep, plan.id.value(),
                    "no step is ready to execute; every remaining step is blocked")
            : blocked.primary();
    return Result<ExecuteResult>(primary);
  }

  DomainAuthority* authority = impl_->domains->find(chosen->owner);
  if (authority == nullptr) {
    return failure<ExecuteResult>(ErrorCode::DomainUnavailable,
                                  std::string(to_string(chosen->owner)),
                                  "no domain authority is registered for this step");
  }

  const AttemptOrdinal ordinal = plan.next_ordinal(chosen->id);
  const AttemptId attempt_id = derive_attempt_id(plan.id, plan.revision, chosen->id, ordinal);
  const Digest key = derive_idempotency_key(plan, *chosen, ordinal, attempt_id);
  const std::uint64_t timestamp = impl_->now();

  AttemptRecord attempt;
  attempt.id = attempt_id;
  attempt.plan = plan.id;
  attempt.plan_revision = plan.revision;
  attempt.step = chosen->id;
  attempt.ordinal = ordinal;
  attempt.status = AttemptStatus::PossiblyIssued;
  attempt.intent_digest = chosen->request.digest();
  attempt.idempotency_key = key;
  attempt.bound_generations = plan.bound_generations;
  attempt.bound_authority = impl_->state.authority();
  attempt.issued_at_micros = timestamp;
  attempt.updated_at_micros = timestamp;
  attempt.note = "issue point recorded durably before the request left this process";

  plan.attempts.push_back(attempt);
  plan.step_status.insert_or_assign(chosen->id, StepStatus::Issued);
  plan.updated_at_micros = timestamp;

  const StepId issued_step = chosen->id;
  const ActionRequest request = chosen->request;

  // The durable issue point is committed BEFORE the authority is contacted. A
  // crash from here on leaves a PossiblyIssued record, never an unrecorded
  // destructive action.
  {
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed.error();
  }

  auto outcome = authority->submit(request, attempt_id, key, timestamp);

  ChangePlan& live_plan = *impl_->state.find_plan(plan.id);
  AttemptRecord* stored = find_attempt_mutable(live_plan, attempt_id);
  const ChangeStep* live_step = live_plan.find_step(issued_step);
  if (stored == nullptr || live_step == nullptr) {
    return failure<ExecuteResult>(ErrorCode::InternalError, attempt_id.value(),
                                  "attempt record disappeared between commit and submission");
  }

  ExecuteResult result;
  result.issued = true;
  result.step = issued_step;
  result.attempt = attempt_id;

  if (!outcome.ok()) {
    stored->status = AttemptStatus::Unresolved;
    stored->note = "authority call failed: " + describe(outcome.error());
    stored->updated_at_micros = impl_->now();
    live_plan.step_status.insert_or_assign(issued_step, StepStatus::Unresolved);
    if (is_legal_plan_transition(live_plan.state, PlanState::Paused)) {
      live_plan.state = PlanState::Paused;
      live_plan.transition_note = "authority call failed without a determinate outcome";
    }
    live_plan.updated_at_micros = impl_->now();
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed.error();
    return Result<ExecuteResult>(outcome.error());
  }

  ActionOutcome action = outcome.take();
  stored->updated_at_micros = impl_->now();

  switch (action.outcome) {
    case SubmitOutcome::Rejected: {
      stored->status = AttemptStatus::Rejected;
      stored->note = "rejected by " + std::string(to_string(live_step->owner)) + ": " + action.detail;
      live_plan.step_status.insert_or_assign(issued_step, StepStatus::Failed);
      if (is_legal_plan_transition(live_plan.state, PlanState::ReplanRequired)) {
        live_plan.state = PlanState::ReplanRequired;
        live_plan.transition_note = "the owning authority rejected " + issued_step.value();
      }
      live_plan.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed.error();
      return failure<ExecuteResult>(ErrorCode::DomainRejected, issued_step.value(), action.detail);
    }
    case SubmitOutcome::Indeterminate: {
      stored->status = AttemptStatus::Unresolved;
      stored->note = "indeterminate outcome: " + action.detail;
      live_plan.step_status.insert_or_assign(issued_step, StepStatus::Unresolved);
      if (is_legal_plan_transition(live_plan.state, PlanState::Paused)) {
        live_plan.state = PlanState::Paused;
        live_plan.transition_note =
            "the authority could not say whether the action took effect; a destructive replay is "
            "forbidden until an operator resolves it";
      }
      live_plan.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed.error();
      return failure<ExecuteResult>(ErrorCode::AttemptUnresolved, issued_step.value(),
                                    action.detail);
    }
    case SubmitOutcome::Accepted:
    case SubmitOutcome::DuplicateOfAppliedRequest:
      break;
    case SubmitOutcome::Unspecified:
    default: {
      stored->status = AttemptStatus::Unresolved;
      stored->note = "authority returned no usable outcome";
      live_plan.step_status.insert_or_assign(issued_step, StepStatus::Unresolved);
      if (is_legal_plan_transition(live_plan.state, PlanState::Paused)) {
        live_plan.state = PlanState::Paused;
        live_plan.transition_note = "authority returned no usable outcome";
      }
      live_plan.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed.error();
      return failure<ExecuteResult>(ErrorCode::DomainIndeterminate, issued_step.value(),
                                    "authority returned no usable outcome");
    }
  }

  if (action.evidence == EvidenceOutcome::Indeterminate) {
    stored->status = AttemptStatus::Unresolved;
    stored->note = "acknowledged but the authority has not observed the effect yet";
    live_plan.step_status.insert_or_assign(issued_step, StepStatus::Unresolved);
    if (is_legal_plan_transition(live_plan.state, PlanState::Paused)) {
      live_plan.state = PlanState::Paused;
      live_plan.transition_note = "acknowledged without an observation";
    }
    live_plan.updated_at_micros = impl_->now();
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed.error();
    return failure<ExecuteResult>(ErrorCode::AttemptUnresolved, issued_step.value(),
                                  "acknowledged without an observation");
  }

  if (action.evidence == EvidenceOutcome::ActionRejected) {
    stored->status = AttemptStatus::Rejected;
    stored->note = "the owning authority reported the action as rejected";
    live_plan.step_status.insert_or_assign(issued_step, StepStatus::Failed);
    if (is_legal_plan_transition(live_plan.state, PlanState::ReplanRequired)) {
      live_plan.state = PlanState::ReplanRequired;
      live_plan.transition_note = "the owning authority rejected " + issued_step.value();
    }
    live_plan.updated_at_micros = impl_->now();
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed.error();
    return failure<ExecuteResult>(ErrorCode::DomainRejected, issued_step.value(), action.detail);
  }

  if (action.evidence == EvidenceOutcome::EffectAbsent) {
    stored->status = AttemptStatus::Failed;
    stored->note = "the authority observed that the expected effect does not hold";
    live_plan.step_status.insert_or_assign(issued_step, StepStatus::Failed);
    if (is_legal_plan_transition(live_plan.state, PlanState::ReplanRequired)) {
      live_plan.state = PlanState::ReplanRequired;
      live_plan.transition_note = "verification failed for " + issued_step.value();
    }
    live_plan.updated_at_micros = impl_->now();
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed.error();
    return failure<ExecuteResult>(ErrorCode::VerificationFailed, issued_step.value(),
                                  action.detail);
  }

  // EffectObserved: record the evidence first, then verify it against the
  // observed asset. Acknowledgement alone is never enough.
  EvidenceRecord evidence;
  evidence.id = derive_evidence_id(live_plan.id, issued_step, attempt_id,
                                   action.observation_sequence);
  evidence.plan = live_plan.id;
  evidence.plan_revision = live_plan.revision;
  evidence.step = issued_step;
  evidence.attempt = attempt_id;
  evidence.source_domain = live_step->owner;
  evidence.source_incarnation = authority->incarnation();
  evidence.source_control_epoch = authority->control_epoch();
  evidence.observation_sequence = action.observation_sequence;
  evidence.observed_generations = action.observed_generations;
  evidence.content_digest = action.content_digest;
  evidence.outcome = action.evidence;
  evidence.observed_at_micros = action.observed_at_micros != 0 ? action.observed_at_micros
                                                               : impl_->now();
  evidence.has_observed_asset = action.has_observed_asset;
  evidence.observed_asset = action.observed_asset;
  evidence.note = action.detail;

  auto appended = impl_->state.evidence.append(evidence);
  if (!appended.ok()) {
    stored->status = AttemptStatus::Unresolved;
    stored->note = "evidence was rejected: " + describe(appended.error());
    live_plan.step_status.insert_or_assign(issued_step, StepStatus::Unresolved);
    if (is_legal_plan_transition(live_plan.state, PlanState::Paused)) {
      live_plan.state = PlanState::Paused;
      live_plan.transition_note = "evidence was rejected by the ledger";
    }
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed.error();
    return Result<ExecuteResult>(appended.error());
  }

  std::string detail;
  const Status applied =
      impl_->apply_observation(live_plan, *live_step, *stored, evidence, detail);
  const Status committed = impl_->commit();
  if (!committed.ok()) return committed.error();
  if (!applied.ok()) return Result<ExecuteResult>(applied.error());

  result.step_status = live_plan.status_of(issued_step);
  result.attempt_status = stored->status;
  result.detail = detail;
  return success(std::move(result));
}

Result<std::vector<ExecuteResult>> Orchestrator::execute_all(const PlanId& id) {
  std::vector<ExecuteResult> results;
  const std::uint64_t bound = (kMaxPlanSteps + 1ull) * 4ull;
  for (std::uint64_t iteration = 0; iteration < bound; ++iteration) {
    auto step = execute_next(id);
    if (!step.ok()) {
      const ErrorCode code = step.error().code;
      if (code == ErrorCode::PlanTerminal || code == ErrorCode::NoExecutableStep) {
        return success(std::move(results));
      }
      return step.error();
    }
    results.push_back(step.take());
  }
  return failure<std::vector<ExecuteResult>>(ErrorCode::BoundedLimitExceeded, id.value(),
                                             "execution exceeded the supported step bound");
}

Status Orchestrator::ingest_evidence(const EvidenceRecord& record) {
  if (!impl_->has_state) {
    return failure<>(ErrorCode::StorageFailure, "store", "the store is not initialized");
  }
  ChangePlan* plan = impl_->state.find_plan(record.plan);
  if (plan == nullptr) {
    return failure<>(ErrorCode::UnknownPlan, record.plan.value(),
                     "evidence refers to a plan this store does not hold");
  }
  const ChangeStep* step = plan->find_step(record.step);
  if (step == nullptr) {
    return failure<>(ErrorCode::UnknownStep, record.step.value(),
                     "evidence refers to a step that is not in the plan");
  }

  auto appended = impl_->state.evidence.append(record);
  if (!appended.ok()) return appended.error();
  if (!appended.value()) {
    return success();  // identical replay: already recorded, nothing to redo
  }

  AttemptRecord* attempt = find_attempt_mutable(*plan, record.attempt);
  if (attempt == nullptr) {
    return failure<>(ErrorCode::UnknownAttempt, record.attempt.value(),
                     "evidence refers to an attempt this plan does not hold");
  }
  if (attempt->terminal()) {
    return failure<>(ErrorCode::AttemptAlreadyResolved, record.attempt.value(),
                     "the attempt already reached a terminal status");
  }
  if (attempt->status == AttemptStatus::PossiblyIssued) {
    attempt->status = AttemptStatus::Observed;
  }
  attempt->last_observation_sequence = record.observation_sequence;
  attempt->last_evidence = record.content_digest;
  attempt->updated_at_micros = impl_->now();

  if (record.outcome == EvidenceOutcome::EffectObserved) {
    std::string detail;
    const Status applied = impl_->apply_observation(*plan, *step, *attempt, record, detail);
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed;
    if (!applied.ok()) return applied;
    return success();
  }
  if (record.outcome == EvidenceOutcome::EffectAbsent) {
    attempt->status = AttemptStatus::Failed;
    attempt->note = "observed effect does not hold";
    plan->step_status.insert_or_assign(record.step, StepStatus::Failed);
    if (is_legal_plan_transition(plan->state, PlanState::ReplanRequired)) {
      plan->state = PlanState::ReplanRequired;
      plan->transition_note = "verification failed for " + record.step.value();
    }
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed;
    return failure<>(ErrorCode::VerificationFailed, record.step.value(),
                     "observed effect does not hold");
  }
  if (record.outcome == EvidenceOutcome::ActionRejected) {
    attempt->status = AttemptStatus::Rejected;
    attempt->note = "the owning authority rejected the action";
    plan->step_status.insert_or_assign(record.step, StepStatus::Failed);
    if (is_legal_plan_transition(plan->state, PlanState::ReplanRequired)) {
      plan->state = PlanState::ReplanRequired;
      plan->transition_note = "the owning authority rejected " + record.step.value();
    }
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed;
    return failure<>(ErrorCode::DomainRejected, record.step.value(),
                     "the owning authority rejected the action");
  }

  // Indeterminate: recorded, but it resolves nothing.
  attempt->status = AttemptStatus::Unresolved;
  attempt->note = "indeterminate observation; explicit resolution is required";
  plan->step_status.insert_or_assign(record.step, StepStatus::Unresolved);
  if (is_legal_plan_transition(plan->state, PlanState::Paused)) {
    plan->state = PlanState::Paused;
    plan->transition_note = "an indeterminate observation arrived for " + record.step.value();
  }
  const Status committed = impl_->commit();
  if (!committed.ok()) return committed;
  return failure<>(ErrorCode::AttemptUnresolved, record.step.value(),
                   "indeterminate observation; explicit resolution is required");
}

Status Orchestrator::resolve_attempt(const AttemptId& id, AttemptStatus resolution,
                                     std::string note) {
  if (!impl_->has_state) {
    return failure<>(ErrorCode::StorageFailure, "store", "the store is not initialized");
  }
  if (resolution != AttemptStatus::NotIssued && resolution != AttemptStatus::Failed &&
      resolution != AttemptStatus::Compensated && resolution != AttemptStatus::Verified) {
    return failure<>(ErrorCode::InvalidEnumValue, id.value(),
                     "an attempt may only be resolved to not-issued, failed, compensated or "
                     "verified");
  }

  for (auto& entry : impl_->state.plans) {
    ChangePlan& plan = entry.second;
    AttemptRecord* attempt = find_attempt_mutable(plan, id);
    if (attempt == nullptr) continue;
    if (attempt->resolved() && attempt->status != AttemptStatus::ResolutionRequired &&
        attempt->status != AttemptStatus::Unresolved) {
      return failure<>(ErrorCode::AttemptAlreadyResolved, id.value(),
                       "the attempt is already resolved");
    }
    const ChangeStep* step = plan.find_step(attempt->step);
    if (step == nullptr) {
      return failure<>(ErrorCode::UnknownStep, attempt->step.value(),
                       "the attempt refers to a step that is not in its plan");
    }

    attempt->status = resolution;
    attempt->note = note.empty() ? "resolved by operator" : note;
    attempt->updated_at_micros = impl_->now();

    switch (resolution) {
      case AttemptStatus::Verified: {
        const std::vector<const EvidenceRecord*> evidence =
            impl_->state.evidence.for_attempt(id);
        const EvidenceRecord* observed = nullptr;
        for (const EvidenceRecord* candidate : evidence) {
          if (candidate->outcome == EvidenceOutcome::EffectObserved &&
              candidate->has_observed_asset) {
            observed = candidate;
            break;
          }
        }
        if (observed == nullptr) {
          attempt->status = AttemptStatus::ResolutionRequired;
          return failure<>(ErrorCode::MissingEvidence, id.value(),
                           "resolution to verified requires an ingested observation for the "
                           "attempt");
        }
        std::string detail;
        const Status applied = impl_->apply_observation(plan, *step, *attempt, *observed, detail);
        if (!applied.ok()) {
          const Status committed = impl_->commit();
          if (!committed.ok()) return committed;
          return applied;
        }
        break;
      }
      case AttemptStatus::NotIssued:
        plan.step_status.insert_or_assign(attempt->step, StepStatus::Pending);
        if (plan.state == PlanState::Paused) {
          plan.state = PlanState::Executing;
          plan.transition_note = "resumed after an operator confirmed the attempt was not issued";
        }
        break;
      case AttemptStatus::Failed:
        plan.step_status.insert_or_assign(attempt->step, StepStatus::Failed);
        if (is_legal_plan_transition(plan.state, PlanState::ReplanRequired)) {
          plan.state = PlanState::ReplanRequired;
          plan.transition_note = "an attempt was resolved as failed";
        }
        break;
      case AttemptStatus::Compensated:
        plan.step_status.insert_or_assign(attempt->step, StepStatus::Compensated);
        break;
      default:
        break;
    }
    plan.updated_at_micros = impl_->now();
    return impl_->commit();
  }
  return failure<>(ErrorCode::UnknownAttempt, id.value(), "no attempt with that identifier");
}

Status Orchestrator::rollback(const PlanId& id, std::string reason) {
  auto found = impl_->plan_for_update(id);
  if (!found.ok()) return found.error();
  ChangePlan& plan = *found.value();

  if (plan.any_non_reversible_applied()) {
    if (is_legal_plan_transition(plan.state, PlanState::ReplanRequired)) {
      plan.state = PlanState::ReplanRequired;
    }
    plan.transition_note =
        "rollback refused: a non-reversible effect has already been applied and cannot be undone";
    plan.updated_at_micros = impl_->now();
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed;
    return failure<>(ErrorCode::NonReversibleStep, plan.id.value(),
                     "rollback is not possible: a non-reversible effect is already applied");
  }

  if (plan.state != PlanState::RollingBack) {
    const Status moved = impl_->transition(plan, PlanState::RollingBack, std::move(reason));
    if (!moved.ok()) return moved;
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed;
  }

  auto order = plan.compensate_order();
  if (!order.ok()) return order.error();

  for (const StepId& step_id : order.value()) {
    ChangePlan& live = *impl_->state.find_plan(id);
    if (live.status_of(step_id) != StepStatus::Verified) continue;
    const ChangeStep* step = live.find_step(step_id);
    if (step == nullptr) continue;

    if (step->reversibility == Reversibility::NonReversible) {
      live.state = PlanState::ReplanRequired;
      live.transition_note = "rollback stopped at non-reversible step " + step_id.value();
      live.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed;
      return failure<>(ErrorCode::NonReversibleStep, step_id.value(),
                       "rollback stopped at a non-reversible step");
    }

    if (!step->compensation.has_value()) {
      live.step_status.insert_or_assign(step_id, StepStatus::Compensated);
      live.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed;
      continue;
    }

    DomainAuthority* authority = impl_->domains->find(step->compensation->owner);
    if (authority == nullptr) {
      live.state = PlanState::Failed;
      live.transition_note = "rollback failed: no authority registered for " +
                            std::string(to_string(step->compensation->owner));
      live.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed;
      return failure<>(ErrorCode::DomainUnavailable, step_id.value(),
                       "no domain authority is registered for the compensation");
    }

    const AttemptOrdinal ordinal = live.next_ordinal(step_id);
    const AttemptId attempt_id = derive_attempt_id(live.id, live.revision, step_id, ordinal);
    const Digest key = derive_idempotency_key(live, *step, ordinal, attempt_id);
    const std::uint64_t timestamp = impl_->now();

    AttemptRecord attempt;
    attempt.id = attempt_id;
    attempt.plan = live.id;
    attempt.plan_revision = live.revision;
    attempt.step = step_id;
    attempt.ordinal = ordinal;
    attempt.status = AttemptStatus::PossiblyIssued;
    attempt.intent_digest = step->compensation->digest();
    attempt.idempotency_key = key;
    attempt.bound_generations = live.bound_generations;
    attempt.bound_authority = impl_->state.authority();
    attempt.issued_at_micros = timestamp;
    attempt.updated_at_micros = timestamp;
    attempt.note = "compensation issued during rollback";
    live.attempts.push_back(attempt);
    live.updated_at_micros = timestamp;
    {
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed;
    }

    auto outcome = authority->submit(*step->compensation, attempt_id, key, timestamp);
    ChangePlan& after = *impl_->state.find_plan(id);
    AttemptRecord* stored = find_attempt_mutable(after, attempt_id);
    if (stored == nullptr) {
      return failure<>(ErrorCode::InternalError, attempt_id.value(),
                       "compensation attempt record disappeared");
    }
    stored->updated_at_micros = impl_->now();

    if (!outcome.ok() || !outcome.value().complete() ||
        outcome.value().outcome == SubmitOutcome::Indeterminate ||
        outcome.value().evidence == EvidenceOutcome::Indeterminate) {
      stored->status = AttemptStatus::Unresolved;
      stored->note = "compensation outcome is indeterminate";
      after.step_status.insert_or_assign(step_id, StepStatus::Unresolved);
      if (is_legal_plan_transition(after.state, PlanState::Paused)) {
        after.state = PlanState::Paused;
        after.transition_note = "rollback paused on an indeterminate compensation for " +
                                step_id.value();
      }
      after.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed;
      return failure<>(ErrorCode::AttemptUnresolved, step_id.value(),
                       "compensation outcome is indeterminate");
    }

    const ActionOutcome action = outcome.take();
    if (action.outcome == SubmitOutcome::Rejected ||
        action.evidence == EvidenceOutcome::ActionRejected) {
      stored->status = AttemptStatus::Rejected;
      stored->note = "compensation rejected: " + action.detail;
      after.step_status.insert_or_assign(step_id, StepStatus::Failed);
      after.state = PlanState::Failed;
      after.transition_note = "rollback failed: compensation for " + step_id.value() +
                              " was rejected";
      after.updated_at_micros = impl_->now();
      const Status committed = impl_->commit();
      if (!committed.ok()) return committed;
      return failure<>(ErrorCode::DomainRejected, step_id.value(),
                       "compensation was rejected by the owning authority");
    }

    if (action.has_observed_asset) {
      EvidenceRecord evidence;
      evidence.id = derive_evidence_id(after.id, step_id, attempt_id, action.observation_sequence);
      evidence.plan = after.id;
      evidence.plan_revision = after.revision;
      evidence.step = step_id;
      evidence.attempt = attempt_id;
      evidence.source_domain = step->compensation->owner;
      evidence.source_incarnation = authority->incarnation();
      evidence.source_control_epoch = authority->control_epoch();
      evidence.observation_sequence = action.observation_sequence;
      evidence.observed_generations = action.observed_generations;
      evidence.content_digest = action.content_digest;
      evidence.outcome = action.evidence;
      evidence.observed_at_micros =
          action.observed_at_micros != 0 ? action.observed_at_micros : impl_->now();
      evidence.has_observed_asset = true;
      evidence.observed_asset = action.observed_asset;
      evidence.note = "compensation: " + action.detail;
      auto appended = impl_->state.evidence.append(evidence);
      if (!appended.ok()) return appended.error();
      if (action.observed_asset.id == step->request.asset) {
        FacilitySnapshot candidate = impl_->state.facility;
        candidate.apply(action.observed_asset);
        impl_->state.facility = candidate;
        impl_->state.facility_revision =
            FacilityRevision(impl_->state.facility_revision.value() + 1);
      }
    }

    stored->status = AttemptStatus::Compensated;
    stored->note = "compensated";
    after.step_status.insert_or_assign(step_id, StepStatus::Compensated);
    after.updated_at_micros = impl_->now();
    const Status committed = impl_->commit();
    if (!committed.ok()) return committed;
  }

  ChangePlan& final_plan = *impl_->state.find_plan(id);
  if (is_legal_plan_transition(final_plan.state, PlanState::RolledBack)) {
    final_plan.state = PlanState::RolledBack;
    final_plan.transition_note = "every compensable step was compensated";
    final_plan.updated_at_micros = impl_->now();
  }
  return impl_->commit();
}

ClosureReport Orchestrator::closure() const {
  ClosureReport report;
  report.store_initialized = impl_->has_state;
  report.revision = impl_->store.revision();
  report.commit_sequence = impl_->store.commit_sequence();
  report.generation = impl_->store.generation();
  if (!impl_->has_state) {
    report.notes.push_back("the store holds no authoritative generation");
    return report;
  }
  report.evidence_records = impl_->state.evidence.size();
  report.facility_assets = impl_->state.facility.size();
  for (const auto& entry : impl_->state.plans) {
    const ChangePlan& plan = entry.second;
    ++report.plans_total;
    report.total_steps += plan.steps.size();
    report.verified_steps += plan.count_with_status(StepStatus::Verified);
    report.unresolved_attempts += plan.unresolved_attempt_count();
    if (plan.state == PlanState::Completed) {
      ++report.plans_completed;
    } else if (plan.terminal()) {
      ++report.plans_terminal_unfinished;
    } else {
      ++report.plans_open;
      report.open_plans.push_back(plan.id);
    }
    if (plan.any_non_reversible_applied()) ++report.non_compensable_applied;
  }
  if (report.unresolved_attempts > 0) {
    report.notes.push_back(std::to_string(report.unresolved_attempts) +
                           " attempt(s) require explicit resolution before closure");
  }
  if (report.plans_open > 0) {
    report.notes.push_back(std::to_string(report.plans_open) + " plan(s) are not terminal");
  }
  if (impl_->store_recovery.mode == RecoveryMode::FencingAuthoritative ||
      impl_->store_recovery.mode == RecoveryMode::RecordScan) {
    report.notes.push_back("durable state was recovered by " +
                           std::string(to_string(impl_->store_recovery.mode)) +
                           " rather than by the manifest");
  }
  return report;
}

Result<std::string> Orchestrator::render_plan(const PlanId& id) const {
  const ChangePlan* found = impl_->state.find_plan(id);
  if (found == nullptr) {
    return failure<std::string>(ErrorCode::UnknownPlan, id.value(), "unknown plan");
  }
  return success(fco::render_plan(*found));
}

Result<std::string> Orchestrator::render_evaluation(const PlanId& id) const {
  const ChangePlan* found = impl_->state.find_plan(id);
  if (found == nullptr) {
    return failure<std::string>(ErrorCode::UnknownPlan, id.value(), "unknown plan");
  }
  return success(fco::render_evaluation(*found, evaluate_plan(*found, impl_->state.facility)));
}

std::string Orchestrator::render_status() const {
  return fco::render_state(impl_->state, impl_->store_recovery);
}

std::string Orchestrator::render_closure() const {
  return fco::render_closure(impl_->state, closure());
}

}  // namespace fco
