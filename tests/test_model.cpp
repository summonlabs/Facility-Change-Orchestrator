// Tests for fco/model.hpp and fco/model.cpp: deterministic topological order,
// the plan state machine, request/plan validation and its deterministic primary
// error, and the digest/state helpers of ChangePlan.

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fco/model.hpp"

namespace fco::test {

template <>
struct ValuePrinter<fco::Digest> {
  [[nodiscard]] static std::string print(const fco::Digest& value) { return value.to_hex(); }
};

}  // namespace fco::test

namespace {

using fco::ActionKind;
using fco::ActionRequest;
using fco::AssetId;
using fco::AttemptOrdinal;
using fco::AttemptRecord;
using fco::AttemptStatus;
using fco::ChangePlan;
using fco::ChangeRequest;
using fco::ChangeStep;
using fco::DomainKind;
using fco::EffectKind;
using fco::EffectSpec;
using fco::PlanId;
using fco::PlanRevision;
using fco::PlanScope;
using fco::PlanState;
using fco::Predicate;
using fco::PredicateKind;
using fco::Reversibility;
using fco::SafetyClass;
using fco::StepId;
using fco::StepStatus;
using fco::VerificationMode;

constexpr std::uint32_t kGenerationCount = static_cast<std::uint32_t>(fco::kGenerationKindMax);

[[nodiscard]] fco::GenerationSet make_generations(std::uint64_t base) {
  fco::GenerationSet set;
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    set.set(static_cast<fco::GenerationKind>(raw), base + raw);
  }
  return set;
}

[[nodiscard]] fco::AuthorityContext make_authority() {
  fco::AuthorityContext context;
  context.incarnation = fco::IncarnationId(7);
  context.control_epoch = fco::ControlEpoch(11);
  context.actor = fco::PrincipalId("operator-1");
  context.policy = fco::PolicyId("policy-1");
  context.policy_digest = fco::Sha256::of("policy-1");
  return context;
}

[[nodiscard]] PlanScope make_scope() {
  PlanScope scope;
  scope.site = fco::SiteId("site-1");
  scope.rack = fco::RackId("rack-1");
  scope.assets = {AssetId("asset-1")};
  return scope;
}

[[nodiscard]] ActionRequest make_action_request(ActionKind kind, const char* asset) {
  ActionRequest request;
  request.kind = kind;
  request.owner = fco::owning_domain(kind);
  request.asset = AssetId(asset);
  request.rack = fco::RackId("rack-1");
  request.site = fco::SiteId("site-1");
  request.lifecycle_generation = fco::LifecycleGeneration(5);
  return request;
}

[[nodiscard]] Predicate predecessor_verified(const char* step) {
  Predicate predicate;
  predicate.kind = PredicateKind::PredecessorVerified;
  predicate.predecessor = StepId(step);
  return predicate;
}

[[nodiscard]] ChangeStep make_step(const char* id, ActionKind action, const char* asset) {
  ChangeStep step;
  step.id = StepId(id);
  step.owner = fco::owning_domain(action);
  step.action = action;
  step.safety = SafetyClass::Standard;
  step.reversibility = Reversibility::Reversible;
  step.verification = VerificationMode::Required;
  step.request = make_action_request(action, asset);
  step.expected_effect.kind = EffectKind::LifecycleStateChange;
  step.expected_effect.asset = AssetId(asset);
  step.expected_effect.expected_state = static_cast<std::uint32_t>(fco::AssetLifecycle::Active);
  return step;
}

// A well-formed request: four typed steps, a real DAG, every field bound.
[[nodiscard]] ChangeRequest make_well_formed_request() {
  ChangeRequest request;
  request.id = fco::ChangeRequestId("request-1");
  request.kind = fco::ChangeKind("rack-replacement");
  request.site = fco::SiteId("site-1");
  request.intent = "replace the failed asset in rack-1";
  request.scope = make_scope();
  request.facility_epoch = fco::FacilityEpoch(3);
  request.actor = fco::PrincipalId("operator-1");
  request.revision = fco::Revision(1);
  request.bound_generations = make_generations(10);
  request.evidence_digest = fco::Sha256::of("evidence");

  ChangeStep drain = make_step("drain", ActionKind::DrainWorkloads, "asset-1");
  ChangeStep isolate = make_step("isolate", ActionKind::MaintenanceIsolate, "asset-1");
  isolate.depends_on = {StepId("drain")};
  isolate.preconditions = {predecessor_verified("drain")};

  ChangeStep swap = make_step("swap", ActionKind::HardwareSwap, "asset-1");
  swap.safety = SafetyClass::Critical;
  swap.reversibility = Reversibility::NonReversible;
  swap.point_of_no_return = true;
  swap.depends_on = {StepId("isolate")};
  swap.preconditions = {predecessor_verified("isolate")};

  ChangeStep recommission = make_step("recommission", ActionKind::Recommission, "asset-1");
  recommission.depends_on = {StepId("swap")};
  recommission.preconditions = {predecessor_verified("swap")};

  request.steps = {drain, isolate, swap, recommission};
  return request;
}

[[nodiscard]] ChangePlan make_well_formed_plan() {
  const ChangeRequest request = make_well_formed_request();
  ChangePlan plan;
  plan.id = PlanId("plan-1");
  plan.request_id = request.id;
  plan.revision = PlanRevision(1);
  plan.state = PlanState::Draft;
  plan.kind = request.kind;
  plan.site = request.site;
  plan.intent = request.intent;
  plan.scope = request.scope;
  plan.facility_epoch = request.facility_epoch;
  plan.policy_generation = fco::PolicyGeneration(11);
  plan.bound_generations = request.bound_generations;
  plan.request_digest = fco::Sha256::of("request");
  plan.evidence_digest = request.evidence_digest;
  plan.facility_digest = fco::Sha256::of("facility");
  plan.authority = make_authority();
  plan.steps = request.steps;
  for (const ChangeStep& step : plan.steps) {
    plan.step_status[step.id] = StepStatus::Pending;
  }
  return plan;
}

[[nodiscard]] AttemptRecord make_attempt(const ChangePlan& plan, const char* step_id,
                                         std::uint64_t ordinal) {
  AttemptRecord attempt;
  attempt.id =
      fco::derive_attempt_id(plan.id, plan.revision, StepId(step_id), AttemptOrdinal(ordinal));
  attempt.plan = plan.id;
  attempt.plan_revision = plan.revision;
  attempt.step = StepId(step_id);
  attempt.ordinal = AttemptOrdinal(ordinal);
  attempt.status = AttemptStatus::PossiblyIssued;
  attempt.intent_digest = fco::Sha256::of("intent");
  return attempt;
}

// A plan holding only the given steps, for graph-shaped tests.
[[nodiscard]] ChangePlan make_plan_from_steps(std::vector<ChangeStep> steps) {
  ChangePlan plan;
  plan.id = PlanId("plan-graph");
  plan.request_id = fco::ChangeRequestId("request-1");
  plan.revision = PlanRevision(1);
  plan.state = PlanState::Draft;
  plan.kind = fco::ChangeKind("rack-replacement");
  plan.site = fco::SiteId("site-1");
  plan.scope = make_scope();
  plan.facility_epoch = fco::FacilityEpoch(3);
  plan.policy_generation = fco::PolicyGeneration(11);
  plan.bound_generations = make_generations(10);
  plan.request_digest = fco::Sha256::of("request");
  plan.evidence_digest = fco::Sha256::of("evidence");
  plan.facility_digest = fco::Sha256::of("facility");
  plan.authority = make_authority();
  plan.steps = std::move(steps);
  return plan;
}

void expect_request_defect(void (*mutate)(ChangeRequest&), fco::ErrorCode expected,
                           const char* label) {
  ChangeRequest request = make_well_formed_request();
  mutate(request);
  fco::Diagnostics diagnostics;
  fco::validate_request(request, diagnostics);
  if (diagnostics.empty()) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string(label) + ": expected a diagnostic, got none");
    return;
  }
  const fco::Error& primary = diagnostics.primary();
  ::fco::test::check_true(
      primary.code == expected,
      (std::string(label) + ": expected " + std::string(fco::to_string(expected)) + ", got " +
       fco::describe(primary))
          .c_str(),
      __FILE__, __LINE__);
}

void expect_plan_defect(void (*mutate)(ChangePlan&), fco::ErrorCode expected, const char* label) {
  ChangePlan plan = make_well_formed_plan();
  mutate(plan);
  fco::Diagnostics diagnostics;
  fco::validate_plan(plan, diagnostics);
  if (diagnostics.empty()) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string(label) + ": expected a diagnostic, got none");
    return;
  }
  const fco::Error& primary = diagnostics.primary();
  ::fco::test::check_true(
      primary.code == expected,
      (std::string(label) + ": expected " + std::string(fco::to_string(expected)) + ", got " +
       fco::describe(primary))
          .c_str(),
      __FILE__, __LINE__);
}

// --- request mutations -----------------------------------------------------
void add_duplicate_step(ChangeRequest& request) { request.steps.push_back(request.steps.front()); }
void wrong_owner(ChangeRequest& request) {
  request.steps.front().owner = DomainKind::Power;
  request.steps.front().request.owner = DomainKind::Power;
}
void compensable_without_compensation(ChangeRequest& request) {
  request.steps.front().reversibility = Reversibility::Compensable;
  request.steps.front().compensation.reset();
}
void non_reversible_with_compensation(ChangeRequest& request) {
  ChangeStep& step = request.steps.front();
  step.reversibility = Reversibility::NonReversible;
  step.compensation = make_action_request(ActionKind::RestoreWorkloads, "asset-1");
}
void point_of_no_return_on_reversible_step(ChangeRequest& request) {
  ChangeStep& step = request.steps.front();
  step.reversibility = Reversibility::Reversible;
  step.safety = SafetyClass::Critical;
  step.point_of_no_return = true;
}
void point_of_no_return_with_advisory_safety(ChangeRequest& request) {
  ChangeStep& step = request.steps.front();
  step.reversibility = Reversibility::NonReversible;
  step.point_of_no_return = true;
  step.safety = SafetyClass::Advisory;
}
void unknown_dependency(ChangeRequest& request) {
  request.steps.front().depends_on.push_back(StepId("ghost"));
}
void self_dependency(ChangeRequest& request) {
  request.steps.front().depends_on.push_back(request.steps.front().id);
}
void unknown_predecessor_precondition(ChangeRequest& request) {
  request.steps.front().preconditions.push_back(predecessor_verified("ghost"));
}
void asset_outside_scope(ChangeRequest& request) {
  ChangeStep& step = request.steps.front();
  step.request.asset = AssetId("asset-9");
  step.expected_effect.asset = AssetId("asset-9");
}
void missing_evidence_digest(ChangeRequest& request) { request.evidence_digest = fco::Digest{}; }
void empty_steps(ChangeRequest& request) { request.steps.clear(); }
void incomplete_generations(ChangeRequest& request) {
  request.bound_generations.set(fco::GenerationKind::Capacity, 0);
}
void empty_intent(ChangeRequest& request) { request.intent.clear(); }
void unset_revision(ChangeRequest& request) { request.revision = fco::Revision{}; }
void scope_site_mismatch(ChangeRequest& request) { request.scope.site = fco::SiteId("site-2"); }
void unset_classification(ChangeRequest& request) {
  request.steps.front().safety = SafetyClass::Unspecified;
}
void incomplete_action_request(ChangeRequest& request) {
  request.steps.front().request.site = fco::SiteId{};
}
void predicate_asset_outside_scope(ChangeRequest& request) {
  Predicate predicate;
  predicate.kind = PredicateKind::PowerStateIs;
  predicate.asset = AssetId("asset-9");
  predicate.expected_state = static_cast<std::uint32_t>(fco::PowerState::On);
  request.steps.front().preconditions.push_back(predicate);
}
void unset_predecessor_precondition(ChangeRequest& request) {
  Predicate predicate;
  predicate.kind = PredicateKind::PredecessorVerified;
  request.steps.front().preconditions.push_back(predicate);
}
void unset_dependency_edge(ChangeRequest& request) {
  request.steps.front().depends_on.push_back(StepId{});
}
void unset_expected_effect(ChangeRequest& request) {
  request.steps.front().expected_effect.kind = EffectKind::Unspecified;
}
void effect_asset_mismatch(ChangeRequest& request) {
  request.steps.front().expected_effect.asset = AssetId("asset-2");
}
void action_request_kind_mismatch(ChangeRequest& request) {
  request.steps.front().request.kind = ActionKind::PowerUpAsset;
}
void compensation_asset_mismatch(ChangeRequest& request) {
  request.steps.front().compensation = make_action_request(ActionKind::RestoreWorkloads, "asset-2");
}
void compensation_is_not_the_inverse(ChangeRequest& request) {
  request.steps.front().compensation = make_action_request(ActionKind::PowerUpAsset, "asset-1");
}

// --- plan mutations --------------------------------------------------------
void unset_plan_state(ChangePlan& plan) { plan.state = PlanState::Unspecified; }
void clear_authority(ChangePlan& plan) { plan.authority = fco::AuthorityContext{}; }
void clear_request_digest(ChangePlan& plan) { plan.request_digest = fco::Digest{}; }
void clear_facility_digest(ChangePlan& plan) { plan.facility_digest = fco::Digest{}; }
void drop_step_status(ChangePlan& plan) { plan.step_status.erase(StepId("drain")); }
void status_for_unknown_step(ChangePlan& plan) {
  plan.step_status.erase(StepId("drain"));
  plan.step_status[StepId("ghost")] = StepStatus::Pending;
}
void duplicate_step(ChangePlan& plan) { plan.steps.push_back(plan.steps.front()); }
void attempt_from_the_future(ChangePlan& plan) {
  AttemptRecord attempt = make_attempt(plan, "drain", 1);
  attempt.plan_revision = PlanRevision(plan.revision.value() + 1);
  plan.attempts.push_back(attempt);
}
void attempt_without_revision(ChangePlan& plan) {
  AttemptRecord attempt = make_attempt(plan, "drain", 1);
  attempt.plan_revision = PlanRevision{};
  plan.attempts.push_back(attempt);
}
void attempt_on_unknown_step(ChangePlan& plan) {
  AttemptRecord attempt = make_attempt(plan, "drain", 1);
  attempt.step = StepId("ghost");
  plan.attempts.push_back(attempt);
}
void attempt_from_another_plan(ChangePlan& plan) {
  AttemptRecord attempt = make_attempt(plan, "drain", 1);
  attempt.plan = PlanId("plan-other");
  plan.attempts.push_back(attempt);
}
void plan_cycle(ChangePlan& plan) {
  plan.steps[0].depends_on = {StepId("recommission")};
}
void clear_steps(ChangePlan& plan) {
  plan.steps.clear();
  plan.step_status.clear();
}
void self_advanced_twice(ChangePlan& plan) {
  fco::GenerationDelta first;
  first.kind = fco::GenerationKind::Power;
  first.bound = 1;
  first.current = 2;
  fco::GenerationDelta second = first;
  plan.self_advanced = {first, second};
}
void self_advanced_backwards(ChangePlan& plan) {
  fco::GenerationDelta delta;
  delta.kind = fco::GenerationKind::Power;
  delta.bound = 2;
  delta.current = 2;
  plan.self_advanced = {delta};
}
void self_advanced_unset_kind(ChangePlan& plan) {
  fco::GenerationDelta delta;
  delta.bound = 1;
  delta.current = 2;
  plan.self_advanced = {delta};
}

// --- transition table ------------------------------------------------------
[[nodiscard]] bool expected_legal_transition(PlanState from, PlanState to) {
  if (!fco::is_valid(from) || !fco::is_valid(to)) return false;
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

using Transition = std::pair<PlanState, PlanState>;
using TransitionSet = std::set<Transition>;

// Parses the documented table rendered by plan_transition_table().
[[nodiscard]] TransitionSet documented_transitions() {
  TransitionSet pairs;
  const std::string_view table = fco::plan_transition_table();
  std::size_t position = 0;
  while (true) {
    const std::size_t arrow = table.find("->{", position);
    if (arrow == std::string_view::npos) break;
    std::string from_name(table.substr(position, arrow - position));
    while (!from_name.empty() && from_name.front() == ' ') from_name.erase(from_name.begin());

    const std::size_t close = table.find('}', arrow);
    if (close == std::string_view::npos) break;
    const std::string targets(table.substr(arrow + 3, close - (arrow + 3)));

    const auto from = fco::parse_plan_state(from_name);
    if (from.ok()) {
      std::size_t start = 0;
      while (true) {
        const std::size_t comma = targets.find(',', start);
        const std::string name = targets.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!name.empty()) {
          const auto to = fco::parse_plan_state(name);
          if (to.ok()) pairs.emplace(from.value(), to.value());
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
    position = close + 1;
  }
  return pairs;
}

}  // namespace

// ---------------------------------------------------------------------------
// Topological order.
// ---------------------------------------------------------------------------
FCO_TEST(model, topological_order_is_deterministic_for_a_diamond) {
  ChangeStep a = make_step("s-a", ActionKind::DrainWorkloads, "asset-1");
  ChangeStep b = make_step("s-b", ActionKind::MaintenanceIsolate, "asset-1");
  b.depends_on = {StepId("s-a")};
  ChangeStep c = make_step("s-c", ActionKind::CoolingIsolate, "asset-1");
  c.depends_on = {StepId("s-a")};
  ChangeStep d = make_step("s-d", ActionKind::PowerUpAsset, "asset-1");
  d.depends_on = {StepId("s-b"), StepId("s-c")};

  const ChangePlan plan = make_plan_from_steps({a, b, c, d});
  const auto order = plan.topological_order();
  FCO_REQUIRE(order.ok());
  FCO_REQUIRE(order.value().size() == 4);
  FCO_CHECK_EQ(order.value()[0].to_string(), std::string("s-a"));
  FCO_CHECK_EQ(order.value()[1].to_string(), std::string("s-b"));
  FCO_CHECK_EQ(order.value()[2].to_string(), std::string("s-c"));
  FCO_CHECK_EQ(order.value()[3].to_string(), std::string("s-d"));

  // Reversing the insertion order of independent steps must not change the
  // result: the tie-break is lexicographic on the step id.
  const ChangePlan reversed = make_plan_from_steps({d, c, b, a});
  const auto reversed_order = reversed.topological_order();
  FCO_REQUIRE(reversed_order.ok());
  FCO_REQUIRE(reversed_order.value().size() == 4);
  FCO_CHECK_EQ(reversed_order.value()[0].to_string(), std::string("s-a"));
  FCO_CHECK_EQ(reversed_order.value()[1].to_string(), std::string("s-b"));
  FCO_CHECK_EQ(reversed_order.value()[2].to_string(), std::string("s-c"));
  FCO_CHECK_EQ(reversed_order.value()[3].to_string(), std::string("s-d"));

  // A duplicate dependency edge is counted once, not twice.
  ChangeStep twice = make_step("s-e", ActionKind::PowerDownAsset, "asset-1");
  twice.depends_on = {StepId("s-a"), StepId("s-a")};
  const ChangePlan with_duplicate_edge = make_plan_from_steps({twice, a});
  const auto duplicate_order = with_duplicate_edge.topological_order();
  FCO_REQUIRE(duplicate_order.ok());
  FCO_REQUIRE(duplicate_order.value().size() == 2);
  FCO_CHECK_EQ(duplicate_order.value()[0].to_string(), std::string("s-a"));
  FCO_CHECK_EQ(duplicate_order.value()[1].to_string(), std::string("s-e"));
}

FCO_TEST(model, topological_order_reports_graph_defects) {
  ChangeStep a = make_step("s-a", ActionKind::DrainWorkloads, "asset-1");
  ChangeStep b = make_step("s-b", ActionKind::MaintenanceIsolate, "asset-1");
  b.depends_on = {StepId("s-a")};
  ChangeStep c = make_step("s-c", ActionKind::CoolingIsolate, "asset-1");
  c.depends_on = {StepId("s-b")};
  ChangeStep again = make_step("s-a", ActionKind::DrainWorkloads, "asset-1");

  const ChangePlan self = make_plan_from_steps({a});
  ChangeStep self_dep = a;
  self_dep.depends_on = {StepId("s-a")};
  FCO_CHECK_ERROR(make_plan_from_steps({self_dep}).topological_order(), fco::ErrorCode::PlanCycle);

  ChangeStep unknown_dep = a;
  unknown_dep.depends_on = {StepId("ghost")};
  FCO_CHECK_ERROR(make_plan_from_steps({unknown_dep}).topological_order(),
                  fco::ErrorCode::MissingPredecessor);

  FCO_CHECK_ERROR(make_plan_from_steps({a, again}).topological_order(),
                  fco::ErrorCode::DuplicateIdentity);

  // Three steps in a cycle: a -> b -> c -> a.
  ChangeStep cyclic_a = a;
  cyclic_a.depends_on = {StepId("s-c")};
  FCO_CHECK_ERROR(make_plan_from_steps({cyclic_a, b, c}).topological_order(),
                  fco::ErrorCode::PlanCycle);
}

FCO_TEST(model, compensate_order_is_the_reverse_of_topological_order) {
  const ChangePlan plan = make_well_formed_plan();
  const auto forward = plan.topological_order();
  const auto backward = plan.compensate_order();
  FCO_REQUIRE(forward.ok());
  FCO_REQUIRE(backward.ok());
  FCO_REQUIRE(forward.value().size() == backward.value().size());
  FCO_REQUIRE(forward.value().size() == 4);
  for (std::size_t i = 0; i < forward.value().size(); ++i) {
    FCO_CHECK_EQ(backward.value()[i].to_string(),
                 forward.value()[forward.value().size() - 1 - i].to_string());
  }
  FCO_CHECK_EQ(backward.value().front().to_string(), std::string("recommission"));
  FCO_CHECK_EQ(backward.value().back().to_string(), std::string("drain"));
}

FCO_TEST(model, direct_predecessors_and_successors) {
  ChangeStep a = make_step("s-a", ActionKind::DrainWorkloads, "asset-1");
  ChangeStep b = make_step("s-b", ActionKind::MaintenanceIsolate, "asset-1");
  b.depends_on = {StepId("s-a")};
  ChangeStep c = make_step("s-c", ActionKind::CoolingIsolate, "asset-1");
  c.depends_on = {StepId("s-a")};
  ChangeStep d = make_step("s-d", ActionKind::PowerUpAsset, "asset-1");
  d.depends_on = {StepId("s-c"), StepId("s-b")};

  const ChangePlan plan = make_plan_from_steps({a, b, c, d});

  const auto root_predecessors = plan.direct_predecessors(StepId("s-a"));
  FCO_REQUIRE(root_predecessors.ok());
  FCO_CHECK(root_predecessors.value().empty());

  const auto diamond_predecessors = plan.direct_predecessors(StepId("s-d"));
  FCO_REQUIRE(diamond_predecessors.ok());
  FCO_REQUIRE(diamond_predecessors.value().size() == 2);
  FCO_CHECK_EQ(diamond_predecessors.value()[0].to_string(), std::string("s-b"));
  FCO_CHECK_EQ(diamond_predecessors.value()[1].to_string(), std::string("s-c"));

  const auto root_successors = plan.direct_successors(StepId("s-a"));
  FCO_REQUIRE(root_successors.ok());
  FCO_REQUIRE(root_successors.value().size() == 2);
  FCO_CHECK_EQ(root_successors.value()[0].to_string(), std::string("s-b"));
  FCO_CHECK_EQ(root_successors.value()[1].to_string(), std::string("s-c"));

  const auto leaf_successors = plan.direct_successors(StepId("s-d"));
  FCO_REQUIRE(leaf_successors.ok());
  FCO_CHECK(leaf_successors.value().empty());

  FCO_CHECK_ERROR(plan.direct_predecessors(StepId("ghost")), fco::ErrorCode::UnknownStep);
  FCO_CHECK_ERROR(plan.direct_successors(StepId("ghost")), fco::ErrorCode::UnknownStep);
}

// ---------------------------------------------------------------------------
// Plan state machine.
// ---------------------------------------------------------------------------
FCO_TEST(model, plan_transition_matrix_matches_the_documented_table) {
  const std::uint32_t max_raw = static_cast<std::uint32_t>(fco::kPlanStateMax);
  FCO_CHECK_EQ(max_raw, std::uint32_t{13});

  std::size_t legal_count = 0;
  TransitionSet derived;
  for (std::uint32_t from_raw = 0; from_raw <= max_raw; ++from_raw) {
    for (std::uint32_t to_raw = 0; to_raw <= max_raw; ++to_raw) {
      const auto from = static_cast<PlanState>(from_raw);
      const auto to = static_cast<PlanState>(to_raw);
      const bool actual = fco::is_legal_plan_transition(from, to);
      const bool expected = expected_legal_transition(from, to);
      if (actual) ++legal_count;
      if (actual != expected) {
        ::fco::test::report_failure(
            __FILE__, __LINE__,
            std::string("transition ") + std::string(fco::to_string(from)) + " -> " +
                std::string(fco::to_string(to)) + " was " + (actual ? "accepted" : "rejected"));
      }
      if (actual) derived.emplace(from, to);
    }
  }
  FCO_CHECK_EQ(legal_count, std::size_t{39});

  const TransitionSet documented = documented_transitions();
  FCO_CHECK_EQ(documented.size(), std::size_t{39});
  FCO_CHECK(documented == derived);

  // Unspecified is never a source or a target, and nothing is its own successor.
  for (std::uint32_t raw = 0; raw <= max_raw; ++raw) {
    const auto state = static_cast<PlanState>(raw);
    FCO_CHECK(!fco::is_legal_plan_transition(state, state));
    FCO_CHECK(!fco::is_legal_plan_transition(PlanState::Unspecified, state));
    FCO_CHECK(!fco::is_legal_plan_transition(state, PlanState::Unspecified));
  }

  // Values outside the enum domain are rejected in either position.
  const auto outside = static_cast<PlanState>(max_raw + 1u);
  FCO_CHECK(!fco::is_legal_plan_transition(outside, PlanState::Executing));
  FCO_CHECK(!fco::is_legal_plan_transition(PlanState::Executing, outside));
  FCO_CHECK(!fco::is_legal_plan_transition(static_cast<PlanState>(200), static_cast<PlanState>(200)));

  // Terminal states have no outgoing transition at all.
  for (PlanState terminal : {PlanState::Completed, PlanState::ReplanRequired,
                             PlanState::Cancelled}) {
    for (std::uint32_t raw = 0; raw <= max_raw; ++raw) {
      FCO_CHECK(!fco::is_legal_plan_transition(terminal, static_cast<PlanState>(raw)));
    }
  }
}

FCO_TEST(model, is_terminal_plan_state_and_settled_step_status) {
  FCO_CHECK(fco::is_terminal_plan_state(PlanState::Completed));
  FCO_CHECK(fco::is_terminal_plan_state(PlanState::Failed));
  FCO_CHECK(fco::is_terminal_plan_state(PlanState::RolledBack));
  FCO_CHECK(fco::is_terminal_plan_state(PlanState::Cancelled));
  FCO_CHECK(fco::is_terminal_plan_state(PlanState::ReplanRequired));
  FCO_CHECK(!fco::is_terminal_plan_state(PlanState::Draft));
  FCO_CHECK(!fco::is_terminal_plan_state(PlanState::Executing));
  FCO_CHECK(!fco::is_terminal_plan_state(PlanState::Unspecified));

  FCO_CHECK(fco::is_settled_step_status(StepStatus::Verified));
  FCO_CHECK(fco::is_settled_step_status(StepStatus::Failed));
  FCO_CHECK(fco::is_settled_step_status(StepStatus::Compensated));
  FCO_CHECK(fco::is_settled_step_status(StepStatus::Blocked));
  FCO_CHECK(fco::is_settled_step_status(StepStatus::NonCompensable));
  FCO_CHECK(!fco::is_settled_step_status(StepStatus::Pending));
  FCO_CHECK(!fco::is_settled_step_status(StepStatus::Issued));
  FCO_CHECK(!fco::is_settled_step_status(StepStatus::Unresolved));
}

// ---------------------------------------------------------------------------
// Request validation.
// ---------------------------------------------------------------------------
FCO_TEST(model, well_formed_request_produces_no_diagnostics) {
  const ChangeRequest request = make_well_formed_request();
  fco::Diagnostics diagnostics;
  fco::validate_request(request, diagnostics);
  FCO_REQUIRE(diagnostics.empty());
  FCO_CHECK_EQ(diagnostics.size(), std::size_t{0});
  FCO_CHECK_EQ(diagnostics.primary().code, fco::ErrorCode::Ok);
  FCO_CHECK(request.evidence_digest.valid());
  FCO_CHECK(request.digest().valid());
}

FCO_TEST(model, request_validation_reports_one_defect_at_a_time) {
  expect_request_defect(add_duplicate_step, fco::ErrorCode::DuplicateIdentity, "duplicate step id");
  expect_request_defect(wrong_owner, fco::ErrorCode::AuthorityMismatch, "wrong owner");
  expect_request_defect(compensable_without_compensation, fco::ErrorCode::NonReversibleStep,
                        "compensable without compensation");
  expect_request_defect(non_reversible_with_compensation, fco::ErrorCode::NonReversibleStep,
                        "compensation on a non-reversible step");
  expect_request_defect(point_of_no_return_on_reversible_step, fco::ErrorCode::ImpossibleCombination,
                        "point of no return on a reversible step");
  expect_request_defect(point_of_no_return_with_advisory_safety,
                        fco::ErrorCode::SafetyClassViolation,
                        "point of no return with advisory safety");
  expect_request_defect(unknown_dependency, fco::ErrorCode::MissingPredecessor,
                        "unknown dependency");
  expect_request_defect(self_dependency, fco::ErrorCode::PlanCycle, "self dependency");
  expect_request_defect(unknown_predecessor_precondition, fco::ErrorCode::UnknownStep,
                        "unknown predecessor precondition");
  expect_request_defect(asset_outside_scope, fco::ErrorCode::PreconditionUnsatisfied,
                        "asset outside scope");
  expect_request_defect(missing_evidence_digest, fco::ErrorCode::UnboundEvidence,
                        "missing evidence digest");
  expect_request_defect(empty_steps, fco::ErrorCode::EmptyPlan, "empty steps");
  expect_request_defect(incomplete_generations, fco::ErrorCode::MissingAuthority,
                        "incomplete generation binding");
  expect_request_defect(empty_intent, fco::ErrorCode::MalformedInput, "empty intent");
  expect_request_defect(unset_revision, fco::ErrorCode::InvalidIdentity, "unset revision");
  expect_request_defect(scope_site_mismatch, fco::ErrorCode::ImpossibleCombination,
                        "scope site mismatch");
  expect_request_defect(unset_classification, fco::ErrorCode::InvalidEnumValue,
                        "unset step classification");
  expect_request_defect(incomplete_action_request, fco::ErrorCode::MalformedInput,
                        "incomplete action request");
  expect_request_defect(predicate_asset_outside_scope, fco::ErrorCode::PreconditionUnsatisfied,
                        "precondition asset outside scope");
  expect_request_defect(unset_predecessor_precondition, fco::ErrorCode::MalformedInput,
                        "predecessor precondition without a predecessor");
  expect_request_defect(unset_dependency_edge, fco::ErrorCode::InvalidIdentity,
                        "unset dependency edge");
  expect_request_defect(unset_expected_effect, fco::ErrorCode::MalformedInput,
                        "incomplete expected effect");
  expect_request_defect(effect_asset_mismatch, fco::ErrorCode::ImpossibleCombination,
                        "effect asset mismatch");
  expect_request_defect(action_request_kind_mismatch, fco::ErrorCode::ImpossibleCombination,
                        "action request kind mismatch");
  expect_request_defect(compensation_asset_mismatch, fco::ErrorCode::ImpossibleCombination,
                        "compensation asset mismatch");
  expect_request_defect(compensation_is_not_the_inverse, fco::ErrorCode::NonReversibleStep,
                        "compensation is not the inverse action");
}

// Regression test: every documented forward action has exactly one inverse
// action accepted as its compensation, in both directions of the pair.
FCO_TEST(model, compensation_must_be_the_documented_inverse) {
  ChangeRequest request = make_well_formed_request();
  ChangeStep& step = request.steps[0];
  step.action = ActionKind::RestoreWorkloads;
  step.owner = fco::owning_domain(step.action);
  step.request.kind = ActionKind::RestoreWorkloads;
  step.request.owner = step.owner;
  step.reversibility = Reversibility::Compensable;
  step.compensation = make_action_request(ActionKind::DrainWorkloads, "asset-1");

  fco::Diagnostics diagnostics;
  fco::validate_request(request, diagnostics);
  if (!diagnostics.empty()) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string("restore-workloads compensated by drain-workloads: ") +
                                    fco::describe(diagnostics.primary()));
  }
  FCO_CHECK(diagnostics.empty());

  const ActionKind inverses[][2] = {
      {ActionKind::DrainWorkloads, ActionKind::RestoreWorkloads},
      {ActionKind::RestoreWorkloads, ActionKind::DrainWorkloads},
      {ActionKind::MaintenanceIsolate, ActionKind::MaintenanceRelease},
      {ActionKind::PowerDownAsset, ActionKind::PowerUpAsset},
      {ActionKind::CoolingIsolate, ActionKind::CoolingRestore},
      {ActionKind::CapacityReserve, ActionKind::CapacityRelease},
      {ActionKind::NetworkQuiesce, ActionKind::NetworkRestore},
  };

  for (const auto& pair : inverses) {
    ChangeRequest candidate = make_well_formed_request();
    ChangeStep& candidate_step = candidate.steps[0];
    candidate_step.action = pair[0];
    candidate_step.owner = fco::owning_domain(pair[0]);
    candidate_step.request = make_action_request(pair[0], "asset-1");
    candidate_step.reversibility = Reversibility::Compensable;
    candidate_step.compensation = make_action_request(pair[1], "asset-1");

    fco::Diagnostics candidate_diagnostics;
    fco::validate_request(candidate, candidate_diagnostics);
    ::fco::test::check_true(
        candidate_diagnostics.empty(),
        (std::string(fco::to_string(pair[0])) + " compensated by " +
         std::string(fco::to_string(pair[1])) + " must validate, got " +
         (candidate_diagnostics.empty() ? std::string("none")
                                        : fco::describe(candidate_diagnostics.primary())))
            .c_str(),
        __FILE__, __LINE__);
  }

  // Any other action is not the inverse and is rejected.
  ChangeRequest wrong = make_well_formed_request();
  wrong.steps[0].reversibility = Reversibility::Compensable;
  wrong.steps[0].compensation = make_action_request(ActionKind::PowerUpAsset, "asset-1");
  fco::Diagnostics wrong_diagnostics;
  fco::validate_request(wrong, wrong_diagnostics);
  FCO_REQUIRE(!wrong_diagnostics.empty());
  FCO_CHECK_EQ(wrong_diagnostics.primary().code, fco::ErrorCode::NonReversibleStep);
}

// SUSPECTED INCOMPLETE FIX (src/model.cpp, is_reverse_edge_ok): the table
// recognises the inverse relation in one direction only for every pair except
// drain/restore. "compensation X is not the inverse of Y" is a symmetric claim:
// if RestoreWorkloads undoes DrainWorkloads -- and that direction is recognised
// -- then DrainWorkloads undoes RestoreWorkloads, MaintenanceIsolate undoes
// MaintenanceRelease, PowerDownAsset undoes PowerUpAsset, CoolingIsolate undoes
// CoolingRestore, CapacityReserve undoes CapacityRelease, and NetworkQuiesce
// undoes NetworkRestore. A rollback of a "release" or "restore" step needs
// exactly those compensations.
FCO_TEST(model, compensation_reverse_direction_for_every_pair) {
  const ActionKind reverse_pairs[][2] = {
      {ActionKind::MaintenanceRelease, ActionKind::MaintenanceIsolate},
      {ActionKind::PowerUpAsset, ActionKind::PowerDownAsset},
      {ActionKind::CoolingRestore, ActionKind::CoolingIsolate},
      {ActionKind::CapacityRelease, ActionKind::CapacityReserve},
      {ActionKind::NetworkRestore, ActionKind::NetworkQuiesce},
  };

  for (const auto& pair : reverse_pairs) {
    ChangeRequest candidate = make_well_formed_request();
    ChangeStep& candidate_step = candidate.steps[0];
    candidate_step.action = pair[0];
    candidate_step.owner = fco::owning_domain(pair[0]);
    candidate_step.request = make_action_request(pair[0], "asset-1");
    candidate_step.reversibility = Reversibility::Compensable;
    candidate_step.compensation = make_action_request(pair[1], "asset-1");

    fco::Diagnostics candidate_diagnostics;
    fco::validate_request(candidate, candidate_diagnostics);
    ::fco::test::check_true(
        candidate_diagnostics.empty(),
        (std::string(fco::to_string(pair[0])) + " compensated by " +
         std::string(fco::to_string(pair[1])) + " must validate, got " +
         (candidate_diagnostics.empty() ? std::string("none")
                                        : fco::describe(candidate_diagnostics.primary())))
            .c_str(),
        __FILE__, __LINE__);
  }
}

FCO_TEST(model, request_primary_error_is_the_lowest_numeric_code) {
  // Defects introduced in one order.
  ChangeRequest forward = make_well_formed_request();
  unknown_dependency(forward);                    // 505 on "drain"
  point_of_no_return_with_advisory_safety(forward);  // 507 on "drain" (already 505 there)
  missing_evidence_digest(forward);               // 405

  fco::Diagnostics forward_diagnostics;
  fco::validate_request(forward, forward_diagnostics);
  FCO_REQUIRE(!forward_diagnostics.empty());
  FCO_CHECK_EQ(forward_diagnostics.primary().code, fco::ErrorCode::UnboundEvidence);

  // The same defects introduced in the opposite order resolve identically.
  ChangeRequest backward = make_well_formed_request();
  missing_evidence_digest(backward);
  point_of_no_return_with_advisory_safety(backward);
  unknown_dependency(backward);

  fco::Diagnostics backward_diagnostics;
  fco::validate_request(backward, backward_diagnostics);
  FCO_REQUIRE(!backward_diagnostics.empty());
  FCO_CHECK_EQ(backward_diagnostics.primary().code, fco::ErrorCode::UnboundEvidence);

  const std::vector<fco::Error> forward_ordered = forward_diagnostics.ordered();
  const std::vector<fco::Error> backward_ordered = backward_diagnostics.ordered();
  FCO_REQUIRE(forward_ordered.size() == backward_ordered.size());
  for (std::size_t i = 0; i < forward_ordered.size(); ++i) {
    FCO_CHECK(fco::error_equal(forward_ordered[i], backward_ordered[i]));
  }
}

FCO_TEST(model, request_primary_error_spans_every_precedence_band) {
  // A defect in the identity band (202) outranks the evidence band (405) and
  // the plan band (503/504/505).
  ChangeRequest mixed = make_well_formed_request();
  unknown_predecessor_precondition(mixed);  // UnknownStep 504
  unknown_dependency(mixed);                // MissingPredecessor 505
  missing_evidence_digest(mixed);           // UnboundEvidence 405
  unset_dependency_edge(mixed);             // InvalidIdentity 202
  fco::Diagnostics mixed_diagnostics;
  fco::validate_request(mixed, mixed_diagnostics);
  FCO_REQUIRE(!mixed_diagnostics.empty());
  FCO_CHECK_EQ(mixed_diagnostics.primary().code, fco::ErrorCode::InvalidIdentity);

  // Without the identity defect the evidence band wins.
  ChangeRequest evidence = make_well_formed_request();
  unknown_predecessor_precondition(evidence);
  unknown_dependency(evidence);
  missing_evidence_digest(evidence);
  fco::Diagnostics evidence_diagnostics;
  fco::validate_request(evidence, evidence_diagnostics);
  FCO_REQUIRE(!evidence_diagnostics.empty());
  FCO_CHECK_EQ(evidence_diagnostics.primary().code, fco::ErrorCode::UnboundEvidence);

  // Without identity or evidence defects the plan band decides, and within a
  // band the numeric order decides.
  ChangeRequest plan_band = make_well_formed_request();
  unknown_predecessor_precondition(plan_band);
  unknown_dependency(plan_band);
  fco::Diagnostics plan_diagnostics;
  fco::validate_request(plan_band, plan_diagnostics);
  FCO_REQUIRE(!plan_diagnostics.empty());
  FCO_CHECK_EQ(plan_diagnostics.primary().code, fco::ErrorCode::UnknownStep);
}

FCO_TEST(model, request_primary_error_tie_break_is_the_subject_key) {
  ChangeRequest one = make_well_formed_request();
  one.steps[3].depends_on.push_back(StepId("recommission"));  // PlanCycle on "recommission"
  one.steps[0].depends_on.push_back(StepId("drain"));         // PlanCycle on "drain"

  fco::Diagnostics diagnostics;
  fco::validate_request(one, diagnostics);
  FCO_REQUIRE(!diagnostics.empty());
  FCO_CHECK_EQ(diagnostics.primary().code, fco::ErrorCode::PlanCycle);
  FCO_CHECK_EQ(diagnostics.primary().subject, std::string("drain"));

  ChangeRequest two = make_well_formed_request();
  two.steps[0].depends_on.push_back(StepId("drain"));
  two.steps[3].depends_on.push_back(StepId("recommission"));

  fco::Diagnostics reversed_diagnostics;
  fco::validate_request(two, reversed_diagnostics);
  FCO_REQUIRE(!reversed_diagnostics.empty());
  FCO_CHECK_EQ(reversed_diagnostics.primary().subject, std::string("drain"));
  FCO_CHECK(fco::error_equal(diagnostics.primary(), reversed_diagnostics.primary()));
}

FCO_TEST(model, request_digest_is_independent_of_step_insertion_order) {
  const ChangeRequest forward = make_well_formed_request();

  ChangeRequest backward = forward;
  backward.steps.clear();
  for (auto it = forward.steps.rbegin(); it != forward.steps.rend(); ++it) {
    backward.steps.push_back(*it);
  }

  FCO_REQUIRE(backward.steps.size() == forward.steps.size());
  FCO_CHECK_EQ(backward.digest(), forward.digest());

  ChangeRequest changed = forward;
  changed.intent = "a different intent";
  FCO_CHECK(changed.digest() != forward.digest());

  changed = forward;
  changed.revision = fco::Revision(2);
  FCO_CHECK(changed.digest() != forward.digest());

  changed = forward;
  changed.steps[0].safety = SafetyClass::Restricted;
  FCO_CHECK(changed.digest() != forward.digest());

  changed = forward;
  changed.scope.assets = {AssetId("asset-1"), AssetId("asset-2")};
  FCO_CHECK(changed.digest() != forward.digest());
}

// ---------------------------------------------------------------------------
// Plan validation.
// ---------------------------------------------------------------------------
FCO_TEST(model, well_formed_plan_produces_no_diagnostics) {
  const ChangePlan plan = make_well_formed_plan();
  fco::Diagnostics diagnostics;
  fco::validate_plan(plan, diagnostics);
  if (!diagnostics.empty()) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string("well-formed plan reported ") +
                                    fco::describe(diagnostics.primary()));
  }
  FCO_CHECK(diagnostics.empty());
  FCO_CHECK(plan.digest().valid());
}

FCO_TEST(model, plan_validation_reports_one_defect_at_a_time) {
  expect_plan_defect(unset_plan_state, fco::ErrorCode::InvalidEnumValue, "unset plan state");
  expect_plan_defect(clear_authority, fco::ErrorCode::MissingAuthority, "incomplete authority");
  expect_plan_defect(clear_request_digest, fco::ErrorCode::UnboundEvidence, "no request digest");
  expect_plan_defect(clear_facility_digest, fco::ErrorCode::UnboundEvidence, "no facility digest");
  expect_plan_defect(drop_step_status, fco::ErrorCode::ImpossibleCombination,
                     "step status map does not cover the steps");
  expect_plan_defect(status_for_unknown_step, fco::ErrorCode::UnknownStep,
                     "step status for an unknown step");
  expect_plan_defect(attempt_from_the_future, fco::ErrorCode::PlanRevisionMismatch,
                     "attempt recorded against a future revision");
  expect_plan_defect(attempt_without_revision, fco::ErrorCode::PlanRevisionMismatch,
                     "attempt without a plan revision");
  expect_plan_defect(attempt_on_unknown_step, fco::ErrorCode::UnknownStep,
                     "attempt on an unknown step");
  expect_plan_defect(attempt_from_another_plan, fco::ErrorCode::ImpossibleCombination,
                     "attempt from another plan");
  expect_plan_defect(plan_cycle, fco::ErrorCode::PlanCycle, "cycle in the plan");
  expect_plan_defect(clear_steps, fco::ErrorCode::EmptyPlan, "empty plan");
  expect_plan_defect(self_advanced_unset_kind, fco::ErrorCode::InvalidEnumValue,
                     "self-advanced generation with an unset kind");
  expect_plan_defect(self_advanced_backwards, fco::ErrorCode::GenerationRegression,
                     "self-advanced generation that does not move forward");
  expect_plan_defect(self_advanced_twice, fco::ErrorCode::ImpossibleCombination,
                     "self-advanced generation recorded twice");
}

FCO_TEST(model, plan_duplicate_step_is_reported_but_outranked) {
  ChangePlan plan = make_well_formed_plan();
  duplicate_step(plan);

  fco::Diagnostics diagnostics;
  fco::validate_plan(plan, diagnostics);
  FCO_REQUIRE(!diagnostics.empty());

  // The duplicate is recorded, and the step/status coverage mismatch (107)
  // outranks it numerically.
  bool saw_duplicate = false;
  for (const fco::Error& error : diagnostics.all()) {
    if (error.code == fco::ErrorCode::DuplicateIdentity) saw_duplicate = true;
  }
  FCO_CHECK(saw_duplicate);
  FCO_CHECK_EQ(diagnostics.primary().code, fco::ErrorCode::ImpossibleCombination);
}

// ---------------------------------------------------------------------------
// ChangePlan helpers.
// ---------------------------------------------------------------------------
FCO_TEST(model, plan_digest_changes_with_every_hashed_component) {
  const ChangePlan base = make_well_formed_plan();
  FCO_REQUIRE(base.digest().valid());
  FCO_CHECK_EQ(base.digest(), make_well_formed_plan().digest());

  ChangePlan changed = base;
  changed.state = PlanState::Executing;
  FCO_CHECK(changed.digest() != base.digest());

  changed = base;
  changed.revision = PlanRevision(2);
  FCO_CHECK(changed.digest() != base.digest());

  changed = base;
  changed.step_status[StepId("drain")] = StepStatus::Verified;
  FCO_CHECK(changed.digest() != base.digest());

  changed = base;
  changed.attempts.push_back(make_attempt(base, "drain", 1));
  FCO_CHECK(changed.digest() != base.digest());

  changed = base;
  changed.transition_note = "operator approved";
  FCO_CHECK(changed.digest() == base.digest());  // notes are not part of the identity

  changed = base;
  fco::GenerationDelta delta;
  delta.kind = fco::GenerationKind::Power;
  delta.bound = 1;
  delta.current = 2;
  changed.self_advanced = {delta};
  FCO_CHECK(changed.digest() != base.digest());

  changed = base;
  changed.explained_revision = fco::FacilityRevision(4);
  FCO_CHECK(changed.digest() != base.digest());

  changed = base;
  changed.steps[0].safety = SafetyClass::Restricted;
  FCO_CHECK(changed.digest() != base.digest());
}

FCO_TEST(model, plan_status_lookup_and_ordinals) {
  const ChangePlan plan = make_well_formed_plan();

  FCO_CHECK_EQ(plan.status_of(StepId("drain")), StepStatus::Pending);
  FCO_CHECK_EQ(plan.status_of(StepId("ghost")), StepStatus::Unspecified);
  FCO_CHECK_EQ(plan.count_with_status(StepStatus::Pending), std::size_t{4});
  FCO_CHECK_EQ(plan.count_with_status(StepStatus::Verified), std::size_t{0});

  FCO_REQUIRE(plan.find_step(StepId("swap")) != nullptr);
  FCO_CHECK(plan.find_step(StepId("ghost")) == nullptr);
  FCO_CHECK_EQ(plan.find_step(StepId("swap"))->action, ActionKind::HardwareSwap);

  FCO_CHECK_EQ(plan.next_ordinal(StepId("drain")).value(), std::uint64_t{1});
  FCO_CHECK_EQ(plan.next_ordinal(StepId("ghost")).value(), std::uint64_t{1});

  ChangePlan with_attempts = plan;
  with_attempts.attempts.push_back(make_attempt(plan, "drain", 1));
  with_attempts.attempts.push_back(make_attempt(plan, "drain", 2));
  with_attempts.attempts.push_back(make_attempt(plan, "swap", 4));

  FCO_CHECK_EQ(with_attempts.next_ordinal(StepId("drain")).value(), std::uint64_t{3});
  FCO_CHECK_EQ(with_attempts.next_ordinal(StepId("swap")).value(), std::uint64_t{5});
  FCO_CHECK_EQ(with_attempts.next_ordinal(StepId("recommission")).value(), std::uint64_t{1});
  FCO_CHECK(with_attempts.find_attempt(with_attempts.attempts.front().id) != nullptr);

  FCO_CHECK(!with_attempts.has_unresolved_attempts());
  FCO_CHECK_EQ(with_attempts.unresolved_attempt_count(), std::size_t{0});
  FCO_CHECK_EQ(with_attempts.attempts.front().status, AttemptStatus::PossiblyIssued);
  FCO_CHECK(!with_attempts.attempts.front().terminal());
  FCO_CHECK(with_attempts.attempts.front().resolved() == false);

  ChangePlan unresolved = with_attempts;
  unresolved.attempts[0].status = AttemptStatus::Unresolved;
  unresolved.attempts[1].status = AttemptStatus::ResolutionRequired;
  unresolved.attempts[2].status = AttemptStatus::Verified;
  FCO_CHECK_EQ(unresolved.unresolved_attempt_count(), std::size_t{2});
  FCO_CHECK(unresolved.has_unresolved_attempts());
  FCO_CHECK(unresolved.attempts[0].resolved());
  FCO_CHECK(unresolved.attempts[2].terminal());
  FCO_CHECK(unresolved.attempts[2].resolved());
}

FCO_TEST(model, plan_state_helpers) {
  ChangePlan plan = make_well_formed_plan();
  FCO_CHECK(!plan.terminal());
  FCO_CHECK(!plan.partially_applied());
  FCO_CHECK(!plan.any_non_reversible_applied());

  plan.state = PlanState::Completed;
  FCO_CHECK(plan.terminal());

  plan = make_well_formed_plan();
  plan.step_status[StepId("drain")] = StepStatus::Verified;
  FCO_CHECK(plan.partially_applied());
  FCO_CHECK(!plan.any_non_reversible_applied());

  plan.step_status[StepId("swap")] = StepStatus::Verified;
  FCO_CHECK(plan.any_non_reversible_applied());

  // Every step verified is not "partially" applied.
  plan.step_status[StepId("drain")] = StepStatus::Verified;
  plan.step_status[StepId("isolate")] = StepStatus::Verified;
  plan.step_status[StepId("recommission")] = StepStatus::Verified;
  FCO_CHECK(!plan.partially_applied());

  // Some verified and some outstanding is.
  plan.step_status[StepId("recommission")] = StepStatus::Pending;
  FCO_CHECK(plan.partially_applied());
}

FCO_TEST(model, plan_scope_behaviour) {
  const PlanScope scope = make_scope();
  FCO_CHECK(!scope.empty());
  FCO_CHECK(scope.contains(AssetId("asset-1")));
  FCO_CHECK(!scope.contains(AssetId("asset-2")));
  FCO_CHECK(!scope.contains(AssetId{}));
  FCO_CHECK(scope.describe().find("site-1") != std::string::npos);

  PlanScope nothing;
  FCO_CHECK(nothing.empty());
  FCO_CHECK(!nothing.contains(AssetId("asset-1")));

  PlanScope rack_only;
  rack_only.rack = fco::RackId("rack-1");
  FCO_CHECK(!rack_only.empty());
}

FCO_TEST_MAIN
