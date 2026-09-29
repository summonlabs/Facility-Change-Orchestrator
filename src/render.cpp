#include "fco/render.hpp"

#include <algorithm>
#include <sstream>

namespace fco {
namespace {

[[nodiscard]] std::string status_marker(StepStatus status) {
  switch (status) {
    case StepStatus::Verified:
      return "[verified]";
    case StepStatus::Compensated:
      return "[compensated]";
    case StepStatus::Failed:
      return "[failed]";
    case StepStatus::Unresolved:
      return "[unresolved]";
    case StepStatus::Issued:
      return "[issued]";
    case StepStatus::Acknowledged:
      return "[acknowledged]";
    case StepStatus::Observed:
      return "[observed]";
    case StepStatus::Blocked:
      return "[blocked]";
    case StepStatus::NonCompensable:
      return "[non-compensable]";
    case StepStatus::Ready:
      return "[ready]";
    case StepStatus::Pending:
      return "[pending]";
    case StepStatus::Unspecified:
    default:
      return "[unspecified]";
  }
}

}  // namespace

std::string render_plan(const ChangePlan& plan) {
  std::ostringstream out;
  out << "plan " << plan.id.value() << " revision " << plan.revision.to_string() << '\n';
  out << "  state: " << to_string(plan.state) << '\n';
  out << "  kind: " << plan.kind.value() << '\n';
  out << "  intent: " << plan.intent << '\n';
  out << "  scope: " << plan.scope.describe() << '\n';
  out << "  request: " << plan.request_id.value() << " digest="
      << plan.request_digest.to_hex().substr(0, 16) << '\n';
  out << "  evidence-digest: " << plan.evidence_digest.to_hex().substr(0, 16) << '\n';
  out << "  facility-digest: " << plan.facility_digest.to_hex().substr(0, 16) << '\n';
  out << "  facility-epoch: " << plan.facility_epoch.to_string() << '\n';
  out << "  authority: " << plan.authority.describe() << '\n';
  out << "  generations: " << plan.bound_generations.describe() << '\n';
  out << "  explained-facility-revision: " << plan.explained_revision.to_string() << '\n';
  if (!plan.self_advanced.empty()) {
    out << "  self-advanced:";
    for (const GenerationDelta& delta : plan.self_advanced) {
      out << ' ' << to_string(delta.kind) << ' ' << delta.bound << "->" << delta.current;
    }
    out << '\n';
  }
  if (!plan.transition_note.empty()) {
    out << "  note: " << plan.transition_note << '\n';
  }

  out << "  steps (" << plan.steps.size() << "):\n";
  for (const ChangeStep& step : plan.steps) {
    const StepStatus status = plan.status_of(step.id);
    out << "    " << status_marker(status) << ' ' << step.id.value() << " owner="
        << to_string(step.owner) << " action=" << to_string(step.action)
        << " safety=" << to_string(step.safety)
        << " reversibility=" << to_string(step.reversibility)
        << " verification=" << to_string(step.verification);
    if (step.point_of_no_return) out << " point-of-no-return";
    out << '\n';
    out << "      target: " << step.request.describe() << '\n';
    out << "      expects: " << step.expected_effect.describe() << '\n';
    if (step.compensation.has_value()) {
      out << "      compensation: " << step.compensation->describe() << '\n';
    } else if (step.reversibility == Reversibility::NonReversible) {
      out << "      compensation: none (the effect cannot be undone)\n";
    }
    if (!step.depends_on.empty()) {
      out << "      after:";
      for (const StepId& dependency : step.depends_on) out << ' ' << dependency.value();
      out << '\n';
    }
    if (!step.preconditions.empty()) {
      out << "      requires:\n";
      for (const Predicate& predicate : step.preconditions) {
        out << "        - " << predicate.describe() << '\n';
      }
    }
    if (!step.rationale.empty()) {
      out << "      rationale: " << step.rationale << '\n';
    }
  }

  if (!plan.attempts.empty()) {
    out << "  attempts (" << plan.attempts.size() << "):\n";
    for (const AttemptRecord& attempt : plan.attempts) {
      out << "    " << attempt.id.value() << " step=" << attempt.step.value()
          << " ordinal=" << attempt.ordinal.to_string()
          << " status=" << to_string(attempt.status)
          << " key=" << attempt.idempotency_key.to_hex().substr(0, 12);
      if (!attempt.note.empty()) out << " note=" << attempt.note;
      out << '\n';
    }
  }
  return out.str();
}

std::string render_evaluation(const ChangePlan& plan, const PlanEvaluation& evaluation) {
  std::ostringstream out;
  out << "evaluation of " << plan.id.value() << " revision " << plan.revision.to_string() << '\n';
  out << "  accepted: " << (evaluation.accepted() ? "yes" : "no") << '\n';
  if (!evaluation.accepted()) {
    out << "  primary-error: " << describe(evaluation.diagnostics.primary()) << '\n';
    out << "  all-findings:\n";
    for (const Error& error : evaluation.diagnostics.ordered()) {
      out << "    - " << describe(error) << '\n';
    }
  }
  for (const std::string& line : evaluation.explanation) {
    out << "  " << line << '\n';
  }
  out << "  readiness:\n";
  for (const StepReadiness& readiness : evaluation.readiness) {
    out << "    " << readiness.step.value()
        << " eligible=" << (readiness.eligible ? "yes" : "no")
        << " ready=" << (readiness.ready ? "yes" : "no");
    if (!readiness.blockages.empty()) {
      out << " blocked-by:";
      for (const std::string& blockage : readiness.blockages) out << " [" << blockage << ']';
    }
    out << '\n';
  }
  return out.str();
}

std::string render_state(const OrchestratorState& state, const RecoveryReport& recovery) {
  std::ostringstream out;
  if (!state.initialized()) {
    out << "store: not initialized\n";
    out << "recovery: " << recovery.describe() << '\n';
    return out.str();
  }
  out << "store revision=" << state.revision.to_string()
      << " commit-sequence=" << state.commit_sequence.to_string()
      << " facility-revision=" << state.facility_revision.to_string() << '\n';
  out << "authority: " << state.authority().describe() << '\n';
  out << "facility-epoch: " << state.facility_epoch.to_string() << '\n';
  out << "generations: " << state.generations.describe() << '\n';
  out << "site: " << state.facility.site().value()
      << " assets=" << state.facility.size() << '\n';
  out << "evidence-records: " << state.evidence.size() << '\n';
  out << "plans:\n";
  if (state.plans.empty()) {
    out << "  (none)\n";
  }
  for (const auto& entry : state.plans) {
    out << "  " << entry.second.describe() << '\n';
  }
  out << "recovery: " << recovery.describe() << '\n';
  for (const std::string& note : recovery.notes) {
    out << "  note: " << note << '\n';
  }
  return out.str();
}

std::string render_closure(const OrchestratorState& state, const ClosureReport& report) {
  std::ostringstream out;
  out << "closure report\n";
  out << "  store-initialized: " << (report.store_initialized ? "yes" : "no") << '\n';
  out << "  revision: " << report.revision.to_string()
      << " commit-sequence: " << report.commit_sequence.to_string()
      << " generation: " << report.generation << '\n';
  out << "  plans: " << report.plans_total << " completed=" << report.plans_completed
      << " open=" << report.plans_open
      << " terminal-unfinished=" << report.plans_terminal_unfinished << '\n';
  out << "  steps-verified: " << report.verified_steps << " of " << report.total_steps << '\n';
  out << "  unresolved-attempts: " << report.unresolved_attempts << '\n';
  out << "  non-compensable-applied: " << report.non_compensable_applied << '\n';
  out << "  evidence-records: " << report.evidence_records << '\n';
  out << "  facility-assets: " << report.facility_assets << '\n';
  for (const PlanId& id : report.open_plans) {
    out << "  open-plan: " << id.value() << '\n';
  }
  for (const std::string& note : report.notes) {
    out << "  note: " << note << '\n';
  }
  out << "  closed: " << (report.closed() ? "yes" : "no") << '\n';
  if (state.initialized()) {
    out << "  facility-digest: " << state.facility.digest().to_hex() << '\n';
  }
  return out.str();
}

std::string render_recovery(const RecoverySummary& summary) {
  std::ostringstream out;
  out << "recovery summary: " << summary.describe() << '\n';
  for (const AttemptClassification& attempt : summary.attempts) {
    out << "  " << attempt.plan.value() << " " << attempt.step.value() << " "
        << attempt.attempt.value() << " status=" << to_string(attempt.status)
        << " disposition=" << attempt.disposition << '\n';
  }
  return out.str();
}

std::string render_evidence(const EvidenceRecord& record) {
  std::ostringstream out;
  out << record.id.value() << " plan=" << record.plan.value()
      << " revision=" << record.plan_revision.to_string() << " step=" << record.step.value()
      << " attempt=" << record.attempt.value() << '\n';
  out << "  source=" << to_string(record.source_domain)
      << " incarnation=" << record.source_incarnation.to_string()
      << " control-epoch=" << record.source_control_epoch.to_string() << '\n';
  out << "  observation-sequence=" << record.observation_sequence.to_string()
      << " outcome=" << to_string(record.outcome) << '\n';
  out << "  content-digest=" << record.content_digest.to_hex() << '\n';
  out << "  observed-generations: " << record.observed_generations.describe() << '\n';
  if (record.has_observed_asset) {
    out << "  observed-asset: " << record.observed_asset.describe() << '\n';
  } else {
    out << "  observed-asset: none recorded\n";
  }
  return out.str();
}

std::string render_facility(const FacilitySnapshot& facility) {
  std::ostringstream out;
  out << "facility " << facility.site().value() << " epoch=" << facility.epoch().to_string()
      << " assets=" << facility.size() << '\n';
  out << "  generations: " << facility.generations().describe() << '\n';
  out << "  digest: " << facility.digest().to_hex() << '\n';
  for (const auto& entry : facility.assets()) {
    out << "  " << entry.second.describe() << '\n';
  }
  return out.str();
}

}  // namespace fco
