#include "fco/model.hpp"

#include <algorithm>
#include <set>

namespace fco {
namespace {

constexpr std::string_view kTransitionTable =
    "draft->{evaluated,cancelled} "
    "evaluated->{authorized,replan-required,cancelled} "
    "authorized->{executing,replan-required,cancelled} "
    "executing->{paused,partially-applied,verifying,completed,failed,rolling-back,"
    "replan-required,cancelled} "
    "paused->{executing,rolling-back,replan-required,cancelled} "
    "partially-applied->{executing,verifying,completed,failed,rolling-back,replan-required,"
    "cancelled} "
    "verifying->{completed,failed,partially-applied,replan-required} "
    "rolling-back->{rolled-back,failed,replan-required} "
    "failed->{rolling-back,replan-required,cancelled} "
    "rolled-back->{replan-required,cancelled} "
    "replan-required->{} cancelled->{} completed->{}";

// The compensation table is symmetric: whatever an action does, its inverse is
// the action that undoes it, in both directions.
[[nodiscard]] bool is_reverse_edge_ok(ActionKind forward, ActionKind backward) noexcept {
  const auto inverse_of = [](ActionKind action) -> ActionKind {
    switch (action) {
      case ActionKind::DrainWorkloads:
        return ActionKind::RestoreWorkloads;
      case ActionKind::RestoreWorkloads:
        return ActionKind::DrainWorkloads;
      case ActionKind::MaintenanceIsolate:
        return ActionKind::MaintenanceRelease;
      case ActionKind::MaintenanceRelease:
        return ActionKind::MaintenanceIsolate;
      case ActionKind::PowerDownAsset:
        return ActionKind::PowerUpAsset;
      case ActionKind::PowerUpAsset:
        return ActionKind::PowerDownAsset;
      case ActionKind::CoolingIsolate:
        return ActionKind::CoolingRestore;
      case ActionKind::CoolingRestore:
        return ActionKind::CoolingIsolate;
      case ActionKind::CapacityReserve:
        return ActionKind::CapacityRelease;
      case ActionKind::CapacityRelease:
        return ActionKind::CapacityReserve;
      case ActionKind::NetworkQuiesce:
        return ActionKind::NetworkRestore;
      case ActionKind::NetworkRestore:
        return ActionKind::NetworkQuiesce;
      case ActionKind::Unspecified:
      default:
        return ActionKind::Unspecified;
    }
  };
  const ActionKind inverse = inverse_of(forward);
  return is_valid(inverse) && inverse == backward;
}

}  // namespace

bool PlanScope::contains(const AssetId& id) const noexcept {
  if (!id.valid()) return false;
  return std::find(assets.begin(), assets.end(), id) != assets.end();
}

void PlanScope::hash_into(Sha256& hasher) const noexcept {
  hasher.update(site.value());
  hasher.update_u8(0);
  hasher.update(rack.value());
  hasher.update_u8(0);
  std::vector<AssetId> sorted = assets;
  std::sort(sorted.begin(), sorted.end());
  hasher.update_u64(static_cast<std::uint64_t>(sorted.size()));
  for (const AssetId& id : sorted) {
    hasher.update(id.value());
    hasher.update_u8(0);
  }
}

std::string PlanScope::describe() const {
  std::string out = "site=" + (site.valid() ? site.to_string() : std::string("<unset>"));
  if (rack.valid()) out += " rack=" + rack.to_string();
  out += " assets=" + std::to_string(assets.size());
  return out;
}

void ChangeStep::hash_into(Sha256& hasher) const noexcept {
  hasher.update(id.value());
  hasher.update_u8(0);
  hasher.update_u8(static_cast<std::uint8_t>(owner));
  hasher.update_u8(static_cast<std::uint8_t>(action));
  hasher.update_u8(static_cast<std::uint8_t>(safety));
  hasher.update_u8(static_cast<std::uint8_t>(reversibility));
  hasher.update_u8(static_cast<std::uint8_t>(verification));
  hasher.update_u8(point_of_no_return ? 1u : 0u);
  request.hash_into(hasher);
  hasher.update_u64(static_cast<std::uint64_t>(preconditions.size()));
  for (const Predicate& predicate : preconditions) predicate.hash_into(hasher);
  std::vector<StepId> edges = depends_on;
  std::sort(edges.begin(), edges.end());
  hasher.update_u64(static_cast<std::uint64_t>(edges.size()));
  for (const StepId& edge : edges) {
    hasher.update(edge.value());
    hasher.update_u8(0);
  }
  expected_effect.hash_into(hasher);
  hasher.update_u8(compensation.has_value() ? 1u : 0u);
  if (compensation.has_value()) compensation->hash_into(hasher);
}

Digest ChangeStep::digest() const {
  Sha256 hasher;
  hasher.update("fco/change-step/1");
  hash_into(hasher);
  return hasher.finish();
}

std::string ChangeStep::describe() const {
  std::string out = id.to_string() + " [" + std::string(to_string(owner)) + "/" +
                    std::string(to_string(action)) + "] " + request.describe();
  out += " safety=" + std::string(to_string(safety));
  out += " reversibility=" + std::string(to_string(reversibility));
  out += " verification=" + std::string(to_string(verification));
  if (point_of_no_return) out += " point-of-no-return";
  return out;
}

void AttemptRecord::hash_into(Sha256& hasher) const noexcept {
  hasher.update(id.value());
  hasher.update_u8(0);
  hasher.update(plan.value());
  hasher.update_u8(0);
  hasher.update_u64(plan_revision.value());
  hasher.update(step.value());
  hasher.update_u8(0);
  hasher.update_u64(ordinal.value());
  hasher.update_u8(static_cast<std::uint8_t>(status));
  hasher.update(intent_digest.bytes().data(), intent_digest.bytes().size());
  hasher.update(idempotency_key.bytes().data(), idempotency_key.bytes().size());
  bound_generations.hash_into(hasher);
  bound_authority.hash_into(hasher);
  hasher.update_u64(last_observation_sequence.value());
  hasher.update(last_evidence.bytes().data(), last_evidence.bytes().size());
  hasher.update_u64(issued_at_micros);
  hasher.update_u64(updated_at_micros);
}

Digest AttemptRecord::digest() const {
  Sha256 hasher;
  hasher.update("fco/attempt-record/1");
  hash_into(hasher);
  return hasher.finish();
}

bool AttemptRecord::terminal() const noexcept {
  switch (status) {
    case AttemptStatus::Verified:
    case AttemptStatus::Failed:
    case AttemptStatus::Compensated:
    case AttemptStatus::Rejected:
      return true;
    case AttemptStatus::Unspecified:
    case AttemptStatus::NotIssued:
    case AttemptStatus::PossiblyIssued:
    case AttemptStatus::Acknowledged:
    case AttemptStatus::Observed:
    case AttemptStatus::Unresolved:
    case AttemptStatus::ResolutionRequired:
    default:
      return false;
  }
}

bool AttemptRecord::resolved() const noexcept {
  return terminal() || status == AttemptStatus::Unresolved;
}

std::string AttemptRecord::describe() const {
  std::string out = id.to_string() + " step=" + step.to_string();
  out += " ordinal=" + ordinal.to_string();
  out += " status=" + std::string(to_string(status));
  if (!note.empty()) out += " note=" + note;
  return out;
}

void ChangeRequest::hash_into(Sha256& hasher) const noexcept {
  hasher.update(id.value());
  hasher.update_u8(0);
  hasher.update(kind.value());
  hasher.update_u8(0);
  hasher.update(site.value());
  hasher.update_u8(0);
  hasher.update(intent);
  hasher.update_u8(0);
  scope.hash_into(hasher);
  hasher.update_u64(facility_epoch.value());
  hasher.update(actor.value());
  hasher.update_u8(0);
  hasher.update_u64(revision.value());
  bound_generations.hash_into(hasher);
  hasher.update(evidence_digest.bytes().data(), evidence_digest.bytes().size());
  hasher.update_u64(static_cast<std::uint64_t>(steps.size()));
  std::vector<const ChangeStep*> ordered;
  ordered.reserve(steps.size());
  for (const ChangeStep& step : steps) ordered.push_back(&step);
  std::sort(ordered.begin(), ordered.end(),
            [](const ChangeStep* a, const ChangeStep* b) { return a->id < b->id; });
  for (const ChangeStep* step : ordered) step->hash_into(hasher);
}

Digest ChangeRequest::digest() const {
  Sha256 hasher;
  hasher.update("fco/change-request/1");
  hash_into(hasher);
  return hasher.finish();
}

std::string ChangeRequest::describe() const {
  std::string out = id.to_string() + " kind=" + kind.to_string();
  out += " " + scope.describe();
  out += " steps=" + std::to_string(steps.size());
  out += " epoch=" + facility_epoch.to_string();
  out += " revision=" + revision.to_string();
  return out;
}

const ChangeStep* ChangePlan::find_step(const StepId& step_id) const noexcept {
  for (const ChangeStep& step : steps) {
    if (step.id == step_id) return &step;
  }
  return nullptr;
}

ChangeStep* ChangePlan::find_step(const StepId& step_id) noexcept {
  for (ChangeStep& step : steps) {
    if (step.id == step_id) return &step;
  }
  return nullptr;
}

StepStatus ChangePlan::status_of(const StepId& step_id) const noexcept {
  const auto it = step_status.find(step_id);
  return it == step_status.end() ? StepStatus::Unspecified : it->second;
}

const AttemptRecord* ChangePlan::find_attempt(const AttemptId& attempt_id) const noexcept {
  for (const AttemptRecord& attempt : attempts) {
    if (attempt.id == attempt_id) return &attempt;
  }
  return nullptr;
}

AttemptOrdinal ChangePlan::next_ordinal(const StepId& step_id) const noexcept {
  std::uint64_t highest = 0;
  for (const AttemptRecord& attempt : attempts) {
    if (attempt.step == step_id && attempt.ordinal.value() > highest) {
      highest = attempt.ordinal.value();
    }
  }
  return AttemptOrdinal(highest + 1);
}

std::size_t ChangePlan::count_with_status(StepStatus status) const noexcept {
  std::size_t count = 0;
  for (const auto& entry : step_status) {
    if (entry.second == status) ++count;
  }
  return count;
}

std::size_t ChangePlan::unresolved_attempt_count() const noexcept {
  std::size_t count = 0;
  for (const AttemptRecord& attempt : attempts) {
    if (attempt.status == AttemptStatus::Unresolved ||
        attempt.status == AttemptStatus::ResolutionRequired) {
      ++count;
    }
  }
  return count;
}

bool ChangePlan::has_unresolved_attempts() const noexcept { return unresolved_attempt_count() > 0; }

bool ChangePlan::terminal() const noexcept { return is_terminal_plan_state(state); }

bool ChangePlan::partially_applied() const noexcept {
  const std::size_t done = count_with_status(StepStatus::Verified);
  return done > 0 && done < steps.size();
}

bool ChangePlan::any_non_reversible_applied() const noexcept {
  for (const ChangeStep& step : steps) {
    if (step.reversibility != Reversibility::Compensable &&
        step.reversibility != Reversibility::Reversible) {
      if (status_of(step.id) == StepStatus::Verified || status_of(step.id) == StepStatus::Observed ||
          status_of(step.id) == StepStatus::Acknowledged) {
        return true;
      }
    }
    if (step.point_of_no_return && status_of(step.id) == StepStatus::Verified) return true;
  }
  return false;
}

Result<std::vector<StepId>> ChangePlan::topological_order() const {
  std::set<StepId> known;
  for (const ChangeStep& step : steps) {
    if (!step.id.valid()) {
      return failure<std::vector<StepId>>(ErrorCode::InvalidIdentity, "step",
                                          "plan contains a step with an unset identifier");
    }
    if (!known.insert(step.id).second) {
      return failure<std::vector<StepId>>(ErrorCode::DuplicateIdentity, step.id.to_string(),
                                          "duplicate step identifier in plan");
    }
  }

  std::map<StepId, std::size_t> indegree;
  std::map<StepId, std::vector<StepId>> successors;
  for (const ChangeStep& step : steps) {
    indegree.emplace(step.id, 0u);
  }
  for (const ChangeStep& step : steps) {
    std::set<StepId> seen;
    for (const StepId& dependency : step.depends_on) {
      if (dependency == step.id) {
        return failure<std::vector<StepId>>(ErrorCode::PlanCycle, step.id.to_string(),
                                            "step depends on itself");
      }
      if (known.find(dependency) == known.end()) {
        return failure<std::vector<StepId>>(ErrorCode::MissingPredecessor, step.id.to_string(),
                                            "step depends on unknown step " +
                                                dependency.value());
      }
      if (!seen.insert(dependency).second) continue;
      indegree[step.id] += 1u;
      successors[dependency].push_back(step.id);
    }
  }

  std::set<StepId> ready;
  for (const auto& entry : indegree) {
    if (entry.second == 0u) ready.insert(entry.first);
  }

  std::vector<StepId> order;
  order.reserve(steps.size());
  while (!ready.empty()) {
    const StepId current = *ready.begin();
    ready.erase(ready.begin());
    order.push_back(current);
    for (const StepId& successor : successors[current]) {
      auto it = indegree.find(successor);
      if (it == indegree.end() || it->second == 0u) continue;
      it->second -= 1u;
      if (it->second == 0u) ready.insert(successor);
    }
  }

  if (order.size() != steps.size()) {
    return failure<std::vector<StepId>>(ErrorCode::PlanCycle, "plan",
                                        "dependency graph contains a cycle");
  }
  return success(std::move(order));
}

Result<std::vector<StepId>> ChangePlan::direct_predecessors(const StepId& step_id) const {
  const ChangeStep* step = find_step(step_id);
  if (step == nullptr) {
    return failure<std::vector<StepId>>(ErrorCode::UnknownStep, step_id.to_string(),
                                        "step is not part of this plan");
  }
  std::vector<StepId> out = step->depends_on;
  std::sort(out.begin(), out.end());
  return success(std::move(out));
}

Result<std::vector<StepId>> ChangePlan::direct_successors(const StepId& step_id) const {
  if (find_step(step_id) == nullptr) {
    return failure<std::vector<StepId>>(ErrorCode::UnknownStep, step_id.to_string(),
                                        "step is not part of this plan");
  }
  std::vector<StepId> out;
  for (const ChangeStep& step : steps) {
    for (const StepId& dependency : step.depends_on) {
      if (dependency == step_id) {
        out.push_back(step.id);
        break;
      }
    }
  }
  std::sort(out.begin(), out.end());
  return success(std::move(out));
}

Result<std::vector<StepId>> ChangePlan::compensate_order() const {
  auto order = topological_order();
  if (!order.ok()) return order;
  std::vector<StepId> reversed = order.value();
  std::reverse(reversed.begin(), reversed.end());
  return success(std::move(reversed));
}

void ChangePlan::hash_into(Sha256& hasher) const noexcept {
  hasher.update(id.value());
  hasher.update_u8(0);
  hasher.update(request_id.value());
  hasher.update_u8(0);
  hasher.update_u64(revision.value());
  hasher.update_u8(static_cast<std::uint8_t>(state));
  hasher.update(kind.value());
  hasher.update_u8(0);
  hasher.update(site.value());
  hasher.update_u8(0);
  hasher.update(intent);
  hasher.update_u8(0);
  scope.hash_into(hasher);
  hasher.update_u64(facility_epoch.value());
  hasher.update_u64(policy_generation.value());
  bound_generations.hash_into(hasher);
  hasher.update(request_digest.bytes().data(), request_digest.bytes().size());
  hasher.update(evidence_digest.bytes().data(), evidence_digest.bytes().size());
  hasher.update(facility_digest.bytes().data(), facility_digest.bytes().size());
  authority.hash_into(hasher);
  hasher.update_u64(static_cast<std::uint64_t>(steps.size()));
  for (const ChangeStep& step : steps) step.hash_into(hasher);
  for (const auto& entry : step_status) {
    hasher.update(entry.first.value());
    hasher.update_u8(0);
    hasher.update_u8(static_cast<std::uint8_t>(entry.second));
  }
  hasher.update_u64(static_cast<std::uint64_t>(attempts.size()));
  for (const AttemptRecord& attempt : attempts) attempt.hash_into(hasher);
  hasher.update_u64(static_cast<std::uint64_t>(self_advanced.size()));
  for (const GenerationDelta& delta : self_advanced) {
    hasher.update_u8(static_cast<std::uint8_t>(delta.kind));
    hasher.update_u64(delta.bound);
    hasher.update_u64(delta.current);
  }
  hasher.update_u64(explained_revision.value());
}

Digest ChangePlan::digest() const {
  Sha256 hasher;
  hasher.update("fco/change-plan/1");
  hash_into(hasher);
  return hasher.finish();
}

std::string ChangePlan::describe() const {
  std::string out = id.to_string() + " revision=" + revision.to_string();
  out += " state=" + std::string(to_string(state));
  out += " kind=" + kind.to_string();
  out += " steps=" + std::to_string(steps.size());
  out += " verified=" + std::to_string(count_with_status(StepStatus::Verified));
  if (has_unresolved_attempts()) {
    out += " unresolved=" + std::to_string(unresolved_attempt_count());
  }
  return out;
}

void validate_request(const ChangeRequest& request, Diagnostics& diagnostics) {
  if (!request.id.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "request.id", "change request identifier is unset");
  } else if (!is_valid_identifier(request.id.value())) {
    diagnostics.add(ErrorCode::InvalidIdentity, "request.id",
                    "change request identifier is empty, longer than 64 bytes, or contains an "
                    "unsupported character");
  }
  if (request.kind.valid() && !is_valid_identifier(request.kind.value())) {
    diagnostics.add(ErrorCode::InvalidIdentity, "request.kind",
                    "change kind is empty, longer than 64 bytes, or contains an unsupported "
                    "character");
  }
  for (const AssetId& asset : request.scope.assets) {
    if (!is_valid_identifier(asset.value())) {
      diagnostics.add(ErrorCode::InvalidIdentity, asset.value(),
                      "request scope contains an identifier the durable layer cannot encode");
    }
  }
  if (!request.kind.valid()) {
    diagnostics.add(ErrorCode::InvalidEnumValue, "request.kind", "change kind is unset");
  }
  if (!request.site.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "request.site", "request site is unset");
  }
  if (request.intent.empty()) {
    diagnostics.add(ErrorCode::MalformedInput, "request.intent",
                    "request intent must be stated explicitly");
  }
  if (request.intent.size() > kMaxStringBytes) {
    diagnostics.add(ErrorCode::BoundedLimitExceeded, "request.intent",
                    "request intent exceeds the supported bound");
  }
  if (!request.facility_epoch.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "request.facility_epoch",
                    "request facility epoch is unset");
  }
  if (!request.actor.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "request.actor", "request actor is unset");
  }
  if (!request.revision.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "request.revision",
                    "request revision must be at least 1");
  }
  if (!request.bound_generations.complete()) {
    diagnostics.add(ErrorCode::MissingAuthority, "request.generations",
                    "request generation binding is incomplete");
  }
  if (!request.evidence_digest.valid()) {
    diagnostics.add(ErrorCode::UnboundEvidence, "request.evidence_digest",
                    "request does not bind an evidence digest");
  }
  if (request.scope.empty()) {
    diagnostics.add(ErrorCode::MalformedInput, "request.scope", "request scope is empty");
  }
  if (request.scope.site.valid() && request.site.valid() && request.scope.site != request.site) {
    diagnostics.add(ErrorCode::ImpossibleCombination, "request.scope",
                    "request scope site does not match the request site");
  }
  if (request.steps.empty()) {
    diagnostics.add(ErrorCode::EmptyPlan, "request.steps", "request contains no steps");
  }
  if (request.steps.size() > static_cast<std::size_t>(kMaxPlanSteps)) {
    diagnostics.add(ErrorCode::BoundedLimitExceeded, "request.steps",
                    "request exceeds the supported step bound");
  }

  std::set<StepId> seen;
  for (const ChangeStep& step : request.steps) {
    const std::string subject = step.id.valid() ? step.id.to_string() : std::string("<unset-step>");
    if (!step.id.valid()) {
      diagnostics.add(ErrorCode::InvalidIdentity, subject, "step identifier is unset");
      continue;
    }
    if (!is_valid_identifier(step.id.value())) {
      diagnostics.add(ErrorCode::InvalidIdentity, subject,
                      "step identifier is empty, longer than 64 bytes, or contains an "
                      "unsupported character");
      continue;
    }
    if (!seen.insert(step.id).second) {
      diagnostics.add(ErrorCode::DuplicateIdentity, subject, "duplicate step identifier");
      continue;
    }
    if (!is_valid(step.owner) || !is_valid(step.action) || !is_valid(step.safety) ||
        !is_valid(step.reversibility) || !is_valid(step.verification)) {
      diagnostics.add(ErrorCode::InvalidEnumValue, subject,
                      "step carries an unset classification field");
    }
    if (is_valid(step.action) && is_valid(step.owner) &&
        owning_domain(step.action) != step.owner) {
      diagnostics.add(ErrorCode::AuthorityMismatch, subject,
                      std::string("action ") + std::string(to_string(step.action)) +
                          " is owned by " + std::string(to_string(owning_domain(step.action))) +
                          " but the step declares " + std::string(to_string(step.owner)));
    }
    if (!step.request.complete()) {
      diagnostics.add(ErrorCode::MalformedInput, subject, "step action request is incomplete");
    }
    if (is_valid(step.request.owner) && is_valid(step.owner) &&
        step.request.owner != step.owner) {
      diagnostics.add(ErrorCode::AuthorityMismatch, subject + ".request",
                      "action request owner does not match the step owner");
    }
    if (is_valid(step.action) && is_valid(step.request.kind) && step.request.kind != step.action) {
      diagnostics.add(ErrorCode::ImpossibleCombination, subject + ".request",
                      "action request kind does not match the step action");
    }
    if (!step.expected_effect.complete()) {
      diagnostics.add(ErrorCode::MalformedInput, subject + ".effect",
                      "step does not declare a complete expected effect");
    }
    if (step.request.asset.valid() && step.expected_effect.asset.valid() &&
        step.request.asset != step.expected_effect.asset) {
      diagnostics.add(ErrorCode::ImpossibleCombination, subject + ".effect",
                      "expected effect targets a different asset than the action");
    }
    if (step.request.asset.valid() && step.request.asset != request.scope.assets.front() &&
        !request.scope.contains(step.request.asset)) {
      diagnostics.add(ErrorCode::PreconditionUnsatisfied, subject,
                      "step targets an asset outside the request scope");
    }
    if (step.point_of_no_return) {
      if (step.reversibility != Reversibility::NonReversible) {
        diagnostics.add(ErrorCode::ImpossibleCombination, subject,
                        "point of no return requires non-reversible classification");
      }
      if (is_valid(step.safety) &&
          static_cast<std::uint8_t>(step.safety) <
              static_cast<std::uint8_t>(SafetyClass::Restricted)) {
        diagnostics.add(ErrorCode::SafetyClassViolation, subject,
                        "point of no return requires restricted or critical safety class");
      }
    }
    switch (step.reversibility) {
      case Reversibility::Compensable:
        if (!step.compensation.has_value()) {
          diagnostics.add(ErrorCode::NonReversibleStep, subject,
                          "compensable step does not declare a compensation");
        }
        break;
      case Reversibility::NonReversible:
        if (step.compensation.has_value()) {
          diagnostics.add(ErrorCode::NonReversibleStep, subject,
                          "non-reversible step must not claim a compensation");
        }
        break;
      case Reversibility::Reversible:
      case Reversibility::Unspecified:
      default:
        break;
    }
    if (step.compensation.has_value()) {
      const ActionRequest& compensation = *step.compensation;
      if (!compensation.complete()) {
        diagnostics.add(ErrorCode::MalformedInput, subject + ".compensation",
                        "compensation action request is incomplete");
      }
      if (is_valid(step.action) && is_valid(compensation.kind) &&
          !is_reverse_edge_ok(step.action, compensation.kind)) {
        diagnostics.add(ErrorCode::NonReversibleStep, subject + ".compensation",
                        std::string("compensation ") +
                            std::string(to_string(compensation.kind)) +
                            " is not the inverse of " + std::string(to_string(step.action)));
      }
      if (compensation.asset.valid() && step.request.asset.valid() &&
          compensation.asset != step.request.asset) {
        diagnostics.add(ErrorCode::ImpossibleCombination, subject + ".compensation",
                        "compensation targets a different asset than the action");
      }
    }
    for (const Predicate& predicate : step.preconditions) {
      const std::string predicate_subject = subject + "/" + predicate.subject_key();
      if (!is_valid(predicate.kind)) {
        diagnostics.add(ErrorCode::InvalidEnumValue, predicate_subject,
                        "precondition kind is unset");
        continue;
      }
      switch (predicate.kind) {
        case PredicateKind::PredecessorVerified:
        case PredicateKind::PredecessorNotVerified:
          if (!predicate.predecessor.valid()) {
            diagnostics.add(ErrorCode::MalformedInput, predicate_subject,
                            "predecessor precondition has no predecessor");
          } else if (request.steps.end() ==
                     std::find_if(request.steps.begin(), request.steps.end(),
                                  [&](const ChangeStep& candidate) {
                                    return candidate.id == predicate.predecessor;
                                  })) {
            diagnostics.add(ErrorCode::UnknownStep, predicate_subject,
                            "predecessor precondition references a step outside the request");
          }
          break;
        default:
          if (!predicate.asset.valid()) {
            diagnostics.add(ErrorCode::MalformedInput, predicate_subject,
                            "precondition has no asset");
          } else if (!request.scope.contains(predicate.asset)) {
            diagnostics.add(ErrorCode::PreconditionUnsatisfied, predicate_subject,
                            "precondition targets an asset outside the request scope");
          }
          break;
      }
    }
    for (const StepId& dependency : step.depends_on) {
      if (!dependency.valid()) {
        diagnostics.add(ErrorCode::InvalidIdentity, subject,
                        "dependency edge carries an unset step identifier");
        continue;
      }
      if (dependency == step.id) {
        diagnostics.add(ErrorCode::PlanCycle, subject, "step depends on itself");
        continue;
      }
      const bool present = std::any_of(request.steps.begin(), request.steps.end(),
                                       [&](const ChangeStep& candidate) {
                                         return candidate.id == dependency;
                                       });
      if (!present) {
        diagnostics.add(ErrorCode::MissingPredecessor, subject,
                        "dependency references unknown step " + dependency.value());
      }
    }
  }
}

void validate_plan(const ChangePlan& plan, Diagnostics& diagnostics) {
  if (!plan.id.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "plan.id", "plan identifier is unset");
  }
  if (!plan.request_id.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "plan.request_id",
                    "plan does not reference a change request");
  }
  if (!plan.revision.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "plan.revision",
                    "plan revision must be at least 1");
  }
  if (!is_valid(plan.state)) {
    diagnostics.add(ErrorCode::InvalidEnumValue, "plan.state", "plan state is unset");
  }
  if (!plan.kind.valid()) {
    diagnostics.add(ErrorCode::InvalidEnumValue, "plan.kind", "plan change kind is unset");
  }
  if (!plan.site.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "plan.site", "plan site is unset");
  }
  if (!plan.facility_epoch.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "plan.facility_epoch",
                    "plan facility epoch is unset");
  }
  if (!plan.policy_generation.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "plan.policy_generation",
                    "plan policy generation is unset");
  }
  if (!plan.bound_generations.complete()) {
    diagnostics.add(ErrorCode::MissingAuthority, "plan.generations",
                    "plan generation binding is incomplete");
  }
  if (!plan.authority.complete()) {
    diagnostics.add(ErrorCode::MissingAuthority, "plan.authority",
                    "plan authority context is incomplete");
  }
  if (!plan.request_digest.valid()) {
    diagnostics.add(ErrorCode::UnboundEvidence, "plan.request_digest",
                    "plan does not bind the request digest");
  }
  if (!plan.evidence_digest.valid()) {
    diagnostics.add(ErrorCode::UnboundEvidence, "plan.evidence_digest",
                    "plan does not bind an evidence digest");
  }
  if (!plan.facility_digest.valid()) {
    diagnostics.add(ErrorCode::UnboundEvidence, "plan.facility_digest",
                    "plan does not bind the facility snapshot digest");
  }
  if (plan.steps.empty()) {
    diagnostics.add(ErrorCode::EmptyPlan, "plan.steps", "plan contains no steps");
  }
  if (plan.steps.size() > static_cast<std::size_t>(kMaxPlanSteps)) {
    diagnostics.add(ErrorCode::BoundedLimitExceeded, "plan.steps",
                    "plan exceeds the supported step bound");
  }

  if (plan.id.valid() && !is_valid_identifier(plan.id.value())) {
    diagnostics.add(ErrorCode::InvalidIdentity, "plan.id",
                    "plan identifier is empty, longer than 64 bytes, or contains an unsupported "
                    "character");
  }
  if (plan.request_id.valid() && !is_valid_identifier(plan.request_id.value())) {
    diagnostics.add(ErrorCode::InvalidIdentity, "plan.request_id",
                    "request identifier is empty, longer than 64 bytes, or contains an "
                    "unsupported character");
  }
  if (plan.kind.valid() && !is_valid_identifier(plan.kind.value())) {
    diagnostics.add(ErrorCode::InvalidIdentity, "plan.kind",
                    "change kind is empty, longer than 64 bytes, or contains an unsupported "
                    "character");
  }

  std::set<StepId> known;
  for (const ChangeStep& step : plan.steps) {
    if (!step.id.valid()) {
      diagnostics.add(ErrorCode::InvalidIdentity, "plan.steps", "plan contains an unset step id");
      continue;
    }
    if (!is_valid_identifier(step.id.value())) {
      diagnostics.add(ErrorCode::InvalidIdentity, step.id.value(),
                      "step identifier is empty, longer than 64 bytes, or contains an "
                      "unsupported character");
      continue;
    }
    if (!known.insert(step.id).second) {
      diagnostics.add(ErrorCode::DuplicateIdentity, step.id.to_string(),
                      "duplicate step identifier in plan");
    }
  }

  for (const auto& entry : plan.step_status) {
    if (known.find(entry.first) == known.end()) {
      diagnostics.add(ErrorCode::UnknownStep, entry.first.to_string(),
                      "step status refers to a step that is not in the plan");
    }
    if (!is_valid(entry.second)) {
      diagnostics.add(ErrorCode::InvalidEnumValue, entry.first.to_string(),
                      "step status is unset");
    }
  }
  if (plan.step_status.size() != plan.steps.size()) {
    diagnostics.add(ErrorCode::ImpossibleCombination, "plan.step_status",
                    "step status map does not cover every step exactly once");
  }

  std::set<AttemptId> attempt_ids;
  for (const AttemptRecord& attempt : plan.attempts) {
    if (!attempt.id.valid()) {
      diagnostics.add(ErrorCode::InvalidIdentity, "plan.attempts",
                      "plan contains an attempt with an unset identifier");
      continue;
    }
    if (!attempt_ids.insert(attempt.id).second) {
      diagnostics.add(ErrorCode::DuplicateIdentity, attempt.id.to_string(),
                      "duplicate attempt identifier in plan");
    }
    if (attempt.plan != plan.id) {
      diagnostics.add(ErrorCode::ImpossibleCombination, attempt.id.to_string(),
                      "attempt belongs to a different plan");
    }
    if (!attempt.plan_revision.valid() ||
        attempt.plan_revision.value() > plan.revision.value()) {
      diagnostics.add(ErrorCode::PlanRevisionMismatch, attempt.id.to_string(),
                      "attempt was recorded against a future plan revision");
    }
    if (known.find(attempt.step) == known.end()) {
      diagnostics.add(ErrorCode::UnknownStep, attempt.id.to_string(),
                      "attempt refers to a step that is not in the plan");
    }
    if (!attempt.ordinal.valid()) {
      diagnostics.add(ErrorCode::InvalidIdentity, attempt.id.to_string(),
                      "attempt ordinal must be at least 1");
    }
    if (!is_valid(attempt.status)) {
      diagnostics.add(ErrorCode::InvalidEnumValue, attempt.id.to_string(),
                      "attempt status is unset");
    }
    if (!attempt.intent_digest.valid()) {
      diagnostics.add(ErrorCode::UnboundEvidence, attempt.id.to_string(),
                      "attempt does not bind the issued intent digest");
    }
  }

  std::set<GenerationKind> advanced_kinds;
  for (const GenerationDelta& delta : plan.self_advanced) {
    if (!is_valid(delta.kind)) {
      diagnostics.add(ErrorCode::InvalidEnumValue, "plan.self_advanced",
                      "plan records an unset self-advanced generation");
      continue;
    }
    if (!advanced_kinds.insert(delta.kind).second) {
      diagnostics.add(ErrorCode::ImpossibleCombination, "plan.self_advanced",
                      std::string("plan records the generation ") +
                          std::string(to_string(delta.kind)) + " as self-advanced twice");
    }
    if (delta.current <= delta.bound) {
      diagnostics.add(ErrorCode::GenerationRegression, "plan.self_advanced",
                      std::string("self-advance of ") + std::string(to_string(delta.kind)) +
                          " does not move the generation forward");
    }
  }
  if (plan.explained_revision.valid() && !plan.facility_epoch.valid()) {
    diagnostics.add(ErrorCode::ImpossibleCombination, "plan.explained_revision",
                    "plan records a facility revision without a facility epoch");
  }

  auto order = plan.topological_order();
  if (!order.ok()) {
    diagnostics.add(order.error());
  }
  if (plan.attempts.size() > static_cast<std::size_t>(kMaxCollectionEntries)) {
    diagnostics.add(ErrorCode::BoundedLimitExceeded, "plan.attempts",
                    "plan attempt history exceeds the supported bound");
  }
}

bool is_legal_plan_transition(PlanState from, PlanState to) noexcept {
  if (!is_valid(from) || !is_valid(to)) return false;
  if (from == to) return false;
  switch (from) {
    case PlanState::Draft:
      return to == PlanState::Evaluated || to == PlanState::Cancelled;
    case PlanState::Evaluated:
      return to == PlanState::Authorized || to == PlanState::ReplanRequired ||
             to == PlanState::Cancelled;
    case PlanState::Authorized:
      return to == PlanState::Executing || to == PlanState::ReplanRequired ||
             to == PlanState::Cancelled;
    case PlanState::Executing:
      return to == PlanState::Paused || to == PlanState::PartiallyApplied ||
             to == PlanState::Verifying || to == PlanState::Completed ||
             to == PlanState::Failed || to == PlanState::RollingBack ||
             to == PlanState::ReplanRequired || to == PlanState::Cancelled;
    case PlanState::Paused:
      return to == PlanState::Executing || to == PlanState::RollingBack ||
             to == PlanState::ReplanRequired || to == PlanState::Cancelled;
    case PlanState::PartiallyApplied:
      return to == PlanState::Executing || to == PlanState::Verifying ||
             to == PlanState::Completed || to == PlanState::Failed ||
             to == PlanState::RollingBack || to == PlanState::ReplanRequired ||
             to == PlanState::Cancelled;
    case PlanState::Verifying:
      return to == PlanState::Completed || to == PlanState::Failed ||
             to == PlanState::PartiallyApplied || to == PlanState::ReplanRequired;
    case PlanState::RollingBack:
      return to == PlanState::RolledBack || to == PlanState::Failed ||
             to == PlanState::ReplanRequired;
    case PlanState::Failed:
      return to == PlanState::RollingBack || to == PlanState::ReplanRequired ||
             to == PlanState::Cancelled;
    case PlanState::RolledBack:
      return to == PlanState::ReplanRequired || to == PlanState::Cancelled;
    case PlanState::Completed:
    case PlanState::ReplanRequired:
    case PlanState::Cancelled:
    case PlanState::Unspecified:
    default:
      return false;
  }
}

std::string_view plan_transition_table() noexcept { return kTransitionTable; }

}  // namespace fco
