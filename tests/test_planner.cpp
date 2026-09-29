// Planning, evaluation and synthesis tests.
//
// src/planner.cpp is the only place that turns a change kind into a step chain,
// and include/fco/planner.hpp documents what evaluation promises: a
// deterministic order, readiness computed from verified predecessors and
// evaluated preconditions, fencing reasons that name every generation that moved
// outside the plan, and a ready set that never contains a settled step.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fco/authority.hpp"
#include "fco/error.hpp"
#include "fco/facility.hpp"
#include "fco/model.hpp"
#include "fco/planner.hpp"
#include "fco/sim.hpp"
#include "fco/strong.hpp"
#include "test_support.hpp"

namespace {

using fco::ActionKind;
using fco::AssetId;
using fco::AuthorityContext;
using fco::ChangePlan;
using fco::ChangeRequest;
using fco::ChangeStep;
using fco::ControlEpoch;
using fco::Diagnostics;
using fco::ErrorCode;
using fco::FacilityEpoch;
using fco::FacilityRevision;
using fco::FacilitySnapshot;
using fco::GenerationDelta;
using fco::GenerationKind;
using fco::GenerationSet;
using fco::IncarnationId;
using fco::PlanEvaluation;
using fco::PlanId;
using fco::PlanRevision;
using fco::PolicyId;
using fco::PrincipalId;
using fco::Revision;
using fco::SafetyClass;
using fco::Sha256;
using fco::SiteId;
using fco::StepId;
using fco::StepStatus;
using fco::SynthesisInput;

constexpr std::uint32_t kGenerationCount = static_cast<std::uint32_t>(fco::kGenerationKindMax);

[[nodiscard]] GenerationSet generations_for(std::uint64_t epoch) {
  GenerationSet set;
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    set.set(static_cast<GenerationKind>(raw), epoch + raw);
  }
  return set;
}

[[nodiscard]] FacilitySnapshot example_facility() {
  return fco::make_example_facility(SiteId("site-alpha"), FacilityEpoch(4), generations_for(4));
}

[[nodiscard]] AuthorityContext example_authority() {
  AuthorityContext authority;
  authority.incarnation = IncarnationId(1);
  authority.control_epoch = ControlEpoch(1);
  authority.actor = PrincipalId("principal-facility-ops");
  authority.policy = PolicyId("policy-change-control-v1");
  authority.policy_digest = Sha256::of("policy-change-control-v1");
  return authority;
}

[[nodiscard]] SynthesisInput make_input(const FacilitySnapshot& snapshot, const std::string& kind,
                                        const std::vector<std::string>& assets) {
  SynthesisInput input;
  input.kind = fco::ChangeKind(kind);
  input.id = fco::ChangeRequestId("request-" + kind);
  input.site = snapshot.site();
  input.intent = "planned change for " + kind;
  input.scope.site = snapshot.site();
  input.scope.rack = fco::RackId("rack-a1");
  for (const std::string& asset : assets) input.scope.assets.push_back(AssetId(asset));
  input.facility_epoch = snapshot.epoch();
  input.actor = PrincipalId("principal-facility-ops");
  input.revision = Revision(1);
  input.bound_generations = snapshot.generations();
  input.evidence_digest = snapshot.digest();
  return input;
}

void expect_clean(const Diagnostics& diagnostics, const char* label) {
  if (diagnostics.empty()) return;
  ::fco::test::report_failure(__FILE__, __LINE__,
                              std::string(label) + ": unexpected diagnostic " +
                                  fco::describe(diagnostics.primary()));
}

void expect_primary(const Diagnostics& diagnostics, ErrorCode expected, const char* label) {
  if (diagnostics.empty()) {
    ::fco::test::report_failure(
        __FILE__, __LINE__,
        std::string(label) + ": expected " + std::string(fco::to_string(expected)) +
            " but no diagnostic was produced");
    return;
  }
  ::fco::test::check_error(diagnostics.primary(), expected, label, __FILE__, __LINE__);
}

[[nodiscard]] bool contains(const std::vector<StepId>& steps, const StepId& id) {
  return std::find(steps.begin(), steps.end(), id) != steps.end();
}

[[nodiscard]] bool reason_contains(const std::vector<std::string>& reasons, std::string_view text) {
  for (const std::string& reason : reasons) {
    if (reason.find(text) != std::string::npos) return true;
  }
  return false;
}

[[nodiscard]] std::string step_id_for(const char* prefix, const char* asset) {
  return std::string(prefix) + "-" + asset;
}

constexpr const char* kAssetA = "rack-a1-node-1";
constexpr const char* kAssetB = "rack-a2-node-1";

}  // namespace

// ---------------------------------------------------------------------------
// Synthesis
// ---------------------------------------------------------------------------
FCO_TEST(planner, every_change_kind_synthesises_a_clean_validated_request) {
  const FacilitySnapshot snapshot = example_facility();
  FCO_REQUIRE(snapshot.valid());
  const AuthorityContext authority = example_authority();
  const std::vector<std::string> kinds = {"rack-replacement", "rack-decommission",
                                          "firmware-upgrade", "power-maintenance"};
  const std::vector<std::string> assets = {kAssetA, kAssetB};

  for (const std::string& kind : kinds) {
    auto request = fco::synthesize_request(make_input(snapshot, kind, assets), snapshot);
    if (!request.ok()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  kind + ": synthesis failed: " + fco::describe(request.error()));
      continue;
    }
    Diagnostics diagnostics;
    fco::validate_request(request.value(), diagnostics);
    expect_clean(diagnostics, kind.c_str());
    FCO_CHECK(!request.value().steps.empty());
    FCO_CHECK_EQ(request.value().scope.assets.size(), std::size_t{2});

    auto plan = fco::create_plan(request.value(), snapshot, authority, PlanRevision(1),
                                 FacilityRevision(3), 1000);
    if (!plan.ok()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  kind + ": create_plan failed: " + fco::describe(plan.error()));
      continue;
    }
    FCO_CHECK_EQ(plan.value().state, fco::PlanState::Draft);
    FCO_CHECK_EQ(plan.value().steps.size(), request.value().steps.size());

    // The same request built with the asset list reversed produces the same plan.
    std::vector<std::string> reversed(assets.rbegin(), assets.rend());
    auto reversed_request =
        fco::synthesize_request(make_input(snapshot, kind, reversed), snapshot);
    FCO_REQUIRE(reversed_request.ok());
    FCO_CHECK(reversed_request.value().steps.size() == request.value().steps.size());
    auto reversed_plan = fco::create_plan(reversed_request.value(), snapshot, authority,
                                          PlanRevision(1), FacilityRevision(3), 2000);
    FCO_REQUIRE(reversed_plan.ok());

    auto order = plan.value().topological_order();
    auto reversed_order = reversed_plan.value().topological_order();
    FCO_REQUIRE(order.ok());
    FCO_REQUIRE(reversed_order.ok());
    FCO_CHECK(order.value() == reversed_order.value());
    FCO_CHECK(plan.value().digest() == reversed_plan.value().digest());
  }
}

FCO_TEST(planner, rack_replacement_chain_shape) {
  const FacilitySnapshot snapshot = example_facility();
  FCO_REQUIRE(snapshot.valid());
  auto request =
      fco::synthesize_request(make_input(snapshot, "rack-replacement", {kAssetA}), snapshot);
  FCO_REQUIRE(request.ok());

  FCO_CHECK_EQ(request.value().steps.size(), std::size_t{13});
  const std::vector<std::string> expected_ids = {
      step_id_for("drain", kAssetA),       step_id_for("isolate", kAssetA),
      step_id_for("power-off", kAssetA),   step_id_for("cooling-off", kAssetA),
      step_id_for("net-off", kAssetA),     step_id_for("swap", kAssetA),
      step_id_for("power-on", kAssetA),    step_id_for("cooling-on", kAssetA),
      step_id_for("net-on", kAssetA),      step_id_for("commission", kAssetA),
      step_id_for("maint-off", kAssetA),   step_id_for("verify", kAssetA),
      step_id_for("restore", kAssetA),
  };
  for (const std::string& id : expected_ids) {
    const bool present = std::any_of(request.value().steps.begin(), request.value().steps.end(),
                                     [&](const ChangeStep& step) {
                                       return step.id.value() == id;
                                     });
    FCO_CHECK(present);
  }

  std::size_t point_of_no_return_count = 0;
  ActionKind point_of_no_return_action = ActionKind::Unspecified;
  std::size_t compensable_count = 0;
  for (const ChangeStep& step : request.value().steps) {
    if (step.point_of_no_return) {
      ++point_of_no_return_count;
      point_of_no_return_action = step.action;
    }
    if (step.reversibility == fco::Reversibility::Compensable) {
      ++compensable_count;
      FCO_CHECK(step.compensation.has_value());
    }
    if (step.reversibility == fco::Reversibility::NonReversible) {
      FCO_CHECK(!step.compensation.has_value());
    }
    FCO_CHECK(step.request.complete());
    FCO_CHECK(step.expected_effect.complete());
  }
  FCO_CHECK_EQ(point_of_no_return_count, std::size_t{1});
  FCO_CHECK_EQ(point_of_no_return_action, ActionKind::HardwareSwap);
  FCO_CHECK_EQ(compensable_count, std::size_t{2});

  // Two assets produce thirteen steps each, and still exactly one swap per asset.
  auto two_assets =
      fco::synthesize_request(make_input(snapshot, "rack-replacement", {kAssetA, kAssetB}),
                              snapshot);
  FCO_REQUIRE(two_assets.ok());
  FCO_CHECK_EQ(two_assets.value().steps.size(), std::size_t{26});
  std::size_t swaps = 0;
  for (const ChangeStep& step : two_assets.value().steps) {
    if (step.point_of_no_return) {
      ++swaps;
      FCO_CHECK_EQ(step.action, ActionKind::HardwareSwap);
    }
  }
  FCO_CHECK_EQ(swaps, std::size_t{2});
}

FCO_TEST(planner, synthesis_rejects_invalid_input) {
  const FacilitySnapshot snapshot = example_facility();
  FCO_REQUIRE(snapshot.valid());

  auto unknown_kind = fco::synthesize_request(make_input(snapshot, "rack-melting", {kAssetA}),
                                              snapshot);
  FCO_CHECK_ERROR(unknown_kind, ErrorCode::InvalidEnumValue);

  auto empty_scope = fco::synthesize_request(make_input(snapshot, "rack-replacement", {}),
                                             snapshot);
  FCO_CHECK_ERROR(empty_scope, ErrorCode::MalformedInput);

  auto unknown_asset =
      fco::synthesize_request(make_input(snapshot, "rack-replacement", {"no-such-asset"}),
                              snapshot);
  FCO_CHECK_ERROR(unknown_asset, ErrorCode::UnknownIdentity);

  auto duplicate =
      fco::synthesize_request(make_input(snapshot, "rack-replacement", {kAssetA, kAssetA}),
                              snapshot);
  FCO_CHECK_ERROR(duplicate, ErrorCode::DuplicateIdentity);

  // A 60-character asset id derives step identifiers longer than the 64-byte
  // identifier bound ("cooling-off-" is twelve bytes, so 12 + 60 = 72).
  const std::string long_asset(60, 'a');
  auto too_long = fco::synthesize_request(
      make_input(snapshot, "rack-replacement", {long_asset}), snapshot);
  FCO_CHECK_ERROR(too_long, ErrorCode::BoundedLimitExceeded);

  // The longest derived prefix is "cooling-off-", twelve bytes, so an asset id of
  // exactly 52 characters still derives a 64-byte identifier and 53 does not.
  const std::string over(53, 'b');
  FCO_CHECK(!fco::is_valid_identifier(step_id_for("cooling-off", over.c_str())));
  const std::string exact(52, 'c');
  FCO_CHECK(fco::is_valid_identifier(step_id_for("cooling-off", exact.c_str())));

  // An asset id of exactly the identifier bound derives an invalid step id.
  const std::string bound_asset(64, 'd');
  FCO_CHECK(fco::is_valid_identifier(bound_asset));
  auto bound = fco::synthesize_request(make_input(snapshot, "rack-replacement", {bound_asset}),
                                       snapshot);
  FCO_CHECK_ERROR(bound, ErrorCode::BoundedLimitExceeded);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------
FCO_TEST(planner, evaluation_finds_the_single_initially_ready_root_step) {
  const FacilitySnapshot snapshot = example_facility();
  FCO_REQUIRE(snapshot.valid());
  const AuthorityContext authority = example_authority();
  auto request =
      fco::synthesize_request(make_input(snapshot, "rack-replacement", {kAssetA}), snapshot);
  FCO_REQUIRE(request.ok());
  auto created = fco::create_plan(request.value(), snapshot, authority, PlanRevision(1),
                                  FacilityRevision(3), 1000);
  FCO_REQUIRE(created.ok());
  const ChangePlan plan = created.value();

  const PlanEvaluation evaluation = fco::evaluate_plan(plan, snapshot);
  expect_clean(evaluation.diagnostics, "fresh plan");
  FCO_CHECK_EQ(evaluation.order.size(), std::size_t{13});
  FCO_CHECK_EQ(evaluation.initially_ready.size(), std::size_t{1});
  FCO_CHECK_EQ(evaluation.initially_ready[0].value(), step_id_for("drain", kAssetA));
  FCO_CHECK(evaluation.initially_blocked.empty());

  // Verifying the root step makes exactly its successor eligible and ready.
  ChangePlan advanced = plan;
  advanced.step_status.insert_or_assign(StepId(step_id_for("drain", kAssetA)),
                                        StepStatus::Verified);
  const PlanEvaluation second = fco::evaluate_plan(advanced, snapshot);
  expect_clean(second.diagnostics, "root verified");
  FCO_CHECK(contains(second.initially_ready, StepId(step_id_for("isolate", kAssetA))));
  FCO_CHECK_EQ(second.initially_ready.size(), std::size_t{2});

  // ... and the step after that is still not eligible.
  ChangePlan further = advanced;
  further.step_status.insert_or_assign(StepId(step_id_for("isolate", kAssetA)),
                                       StepStatus::Verified);
  const PlanEvaluation third = fco::evaluate_plan(further, snapshot);
  expect_clean(third.diagnostics, "two roots verified");
  FCO_CHECK(contains(third.initially_ready, StepId(step_id_for("power-off", kAssetA))));
  FCO_CHECK(!contains(third.initially_ready, StepId(step_id_for("cooling-off", kAssetA))));
}

FCO_TEST(planner, evaluation_reports_unknown_asset_and_invalid_precondition) {
  const FacilitySnapshot snapshot = example_facility();
  FCO_REQUIRE(snapshot.valid());
  const AuthorityContext authority = example_authority();
  auto request =
      fco::synthesize_request(make_input(snapshot, "rack-replacement", {kAssetA}), snapshot);
  FCO_REQUIRE(request.ok());
  auto created = fco::create_plan(request.value(), snapshot, authority, PlanRevision(1),
                                  FacilityRevision(3), 1000);
  FCO_REQUIRE(created.ok());
  const ChangePlan plan = created.value();

  // A snapshot that does not hold the step's asset.
  const fco::AssetRecord* foreign = snapshot.find(AssetId("rack-a3-node-3"));
  FCO_REQUIRE(foreign != nullptr);
  auto other = FacilitySnapshot::create(snapshot.site(), snapshot.epoch(), snapshot.generations(),
                                        {*foreign});
  FCO_REQUIRE(other.ok());
  const PlanEvaluation missing = fco::evaluate_plan(plan, other.value());
  expect_primary(missing.diagnostics, ErrorCode::UnknownIdentity, "missing asset");

  // A precondition whose expected state is outside its enum domain.
  ChangePlan malformed = plan;
  malformed.step_status.insert_or_assign(StepId(step_id_for("drain", kAssetA)),
                                         StepStatus::Verified);
  ChangeStep* isolate = malformed.find_step(StepId(step_id_for("isolate", kAssetA)));
  FCO_REQUIRE(isolate != nullptr);
  if (isolate->preconditions.size() < 2) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                "the isolation step no longer carries two preconditions");
    return;
  }
  isolate->preconditions[1].expected_state = 99u;
  const PlanEvaluation invalid_state = fco::evaluate_plan(malformed, snapshot);
  expect_primary(invalid_state.diagnostics, ErrorCode::InvalidEnumValue,
                 "expected state outside the enum domain");
}

FCO_TEST(planner, evaluation_fences_moved_generations_unless_self_advanced) {
  const FacilitySnapshot snapshot = example_facility();
  FCO_REQUIRE(snapshot.valid());
  const AuthorityContext authority = example_authority();
  auto request =
      fco::synthesize_request(make_input(snapshot, "rack-replacement", {kAssetA}), snapshot);
  FCO_REQUIRE(request.ok());
  auto created = fco::create_plan(request.value(), snapshot, authority, PlanRevision(1),
                                  FacilityRevision(3), 1000);
  FCO_REQUIRE(created.ok());
  const ChangePlan plan = created.value();

  // The world moved: the policy generation advanced on its own.
  GenerationSet moved = snapshot.generations();
  moved.policy = fco::PolicyGeneration(snapshot.generations().policy.value() + 1u);
  std::vector<fco::AssetRecord> records;
  for (const auto& entry : snapshot.assets()) records.push_back(entry.second);
  auto moved_snapshot =
      FacilitySnapshot::create(snapshot.site(), snapshot.epoch(), moved, records);
  FCO_REQUIRE(moved_snapshot.ok());

  const PlanEvaluation fenced = fco::evaluate_plan(plan, moved_snapshot.value());
  expect_primary(fenced.diagnostics, ErrorCode::StaleGeneration, "moved generation");

  // The plan itself advanced that generation to exactly the observed value.
  ChangePlan explained = plan;
  GenerationDelta delta;
  delta.kind = GenerationKind::Policy;
  delta.bound = snapshot.generations().policy.value();
  delta.current = moved.policy.value();
  explained.self_advanced.push_back(delta);
  const PlanEvaluation accepted = fco::evaluate_plan(explained, moved_snapshot.value());
  expect_clean(accepted.diagnostics, "self-advanced generation");
  FCO_CHECK(accepted.accepted());

  // An advance to a different value is still fenced.
  ChangePlan wrong_value = plan;
  GenerationDelta other = delta;
  other.current = moved.policy.value() + 1u;
  wrong_value.self_advanced.push_back(other);
  const PlanEvaluation still_fenced = fco::evaluate_plan(wrong_value, moved_snapshot.value());
  expect_primary(still_fenced.diagnostics, ErrorCode::StaleGeneration,
                 "self-advance to a different value");

  // A generation that moved backwards is never explained by a self-advance.
  GenerationSet regressed = snapshot.generations();
  regressed.policy = fco::PolicyGeneration(snapshot.generations().policy.value() - 1u);
  auto regressed_snapshot =
      FacilitySnapshot::create(snapshot.site(), snapshot.epoch(), regressed, records);
  FCO_REQUIRE(regressed_snapshot.ok());
  const PlanEvaluation backward = fco::evaluate_plan(plan, regressed_snapshot.value());
  expect_primary(backward.diagnostics, ErrorCode::GenerationRegression, "regressed generation");
}

// ---------------------------------------------------------------------------
// Fencing reasons and the ready set
// ---------------------------------------------------------------------------
FCO_TEST(planner, fencing_reasons_name_every_move) {
  const FacilitySnapshot snapshot = example_facility();
  FCO_REQUIRE(snapshot.valid());
  const AuthorityContext authority = example_authority();
  auto request =
      fco::synthesize_request(make_input(snapshot, "rack-replacement", {kAssetA}), snapshot);
  FCO_REQUIRE(request.ok());
  auto created = fco::create_plan(request.value(), snapshot, authority, PlanRevision(1),
                                  FacilityRevision(3), 1000);
  FCO_REQUIRE(created.ok());
  const ChangePlan plan = created.value();

  const std::vector<std::string> current =
      fco::fencing_reasons(plan, snapshot, authority, FacilityRevision(3));
  FCO_CHECK(current.empty());

  AuthorityContext superseded = authority;
  superseded.control_epoch = ControlEpoch(authority.control_epoch.value() + 1u);
  const std::vector<std::string> epoch_reasons =
      fco::fencing_reasons(plan, snapshot, superseded, FacilityRevision(3));
  FCO_CHECK_EQ(epoch_reasons.size(), std::size_t{1});
  FCO_CHECK(reason_contains(epoch_reasons, fco::to_string(ErrorCode::FencedAuthority)));
  FCO_CHECK(reason_contains(epoch_reasons, "control-epoch"));

  AuthorityContext different_actor = authority;
  different_actor.actor = PrincipalId("principal-someone-else");
  const std::vector<std::string> actor_reasons =
      fco::fencing_reasons(plan, snapshot, different_actor, FacilityRevision(3));
  FCO_CHECK_EQ(actor_reasons.size(), std::size_t{1});
  FCO_CHECK(reason_contains(actor_reasons, fco::to_string(ErrorCode::AuthorityMismatch)));

  AuthorityContext different_policy = authority;
  different_policy.policy = PolicyId("policy-other");
  const std::vector<std::string> policy_reasons =
      fco::fencing_reasons(plan, snapshot, different_policy, FacilityRevision(3));
  FCO_CHECK_EQ(policy_reasons.size(), std::size_t{1});
  FCO_CHECK(reason_contains(policy_reasons, fco::to_string(ErrorCode::PolicyViolation)));

  AuthorityContext older = authority;
  older.control_epoch = ControlEpoch(0);
  const std::vector<std::string> older_reasons =
      fco::fencing_reasons(plan, snapshot, older, FacilityRevision(3));
  FCO_CHECK_EQ(older_reasons.size(), std::size_t{1});

  // A generation moved outside the plan.
  GenerationSet moved = snapshot.generations();
  moved.topology = fco::TopologyGeneration(snapshot.generations().topology.value() + 2u);
  std::vector<fco::AssetRecord> records;
  for (const auto& entry : snapshot.assets()) records.push_back(entry.second);
  auto moved_snapshot =
      FacilitySnapshot::create(snapshot.site(), snapshot.epoch(), moved, records);
  FCO_REQUIRE(moved_snapshot.ok());
  const std::vector<std::string> generation_reasons =
      fco::fencing_reasons(plan, moved_snapshot.value(), authority, FacilityRevision(3));
  FCO_CHECK_EQ(generation_reasons.size(), std::size_t{1});
  FCO_CHECK(reason_contains(generation_reasons, "topology moved from"));

  // The same move is explained by a recorded self-advance.
  ChangePlan explained = plan;
  GenerationDelta delta;
  delta.kind = GenerationKind::Topology;
  delta.bound = snapshot.generations().topology.value();
  delta.current = moved.topology.value();
  explained.self_advanced.push_back(delta);
  FCO_CHECK(fco::fencing_reasons(explained, moved_snapshot.value(), authority, FacilityRevision(3))
                .empty());

  // A facility revision that moved outside the plan.
  const std::vector<std::string> revision_reasons =
      fco::fencing_reasons(plan, snapshot, authority, FacilityRevision(4));
  FCO_CHECK_EQ(revision_reasons.size(), std::size_t{1});
  FCO_CHECK(reason_contains(revision_reasons, "facility revision moved from"));

  // Several moves at once produce one reason per move, never a merged reason.
  const std::vector<std::string> both =
      fco::fencing_reasons(plan, moved_snapshot.value(), superseded, FacilityRevision(4));
  FCO_CHECK_EQ(both.size(), std::size_t{3});
}

FCO_TEST(planner, ready_steps_never_settles_or_skips_a_predecessor) {
  const FacilitySnapshot snapshot = example_facility();
  FCO_REQUIRE(snapshot.valid());
  const AuthorityContext authority = example_authority();
  auto request =
      fco::synthesize_request(make_input(snapshot, "rack-replacement", {kAssetA}), snapshot);
  FCO_REQUIRE(request.ok());
  auto created = fco::create_plan(request.value(), snapshot, authority, PlanRevision(1),
                                  FacilityRevision(3), 1000);
  FCO_REQUIRE(created.ok());
  const ChangePlan plan = created.value();

  const std::vector<StepId> fresh = fco::ready_steps(plan);
  FCO_CHECK_EQ(fresh.size(), std::size_t{1});
  FCO_CHECK_EQ(fresh[0].value(), step_id_for("drain", kAssetA));

  ChangePlan verified_root = plan;
  verified_root.step_status.insert_or_assign(StepId(step_id_for("drain", kAssetA)),
                                             StepStatus::Verified);
  const std::vector<StepId> second = fco::ready_steps(verified_root);
  FCO_CHECK_EQ(second.size(), std::size_t{1});
  FCO_CHECK_EQ(second[0].value(), step_id_for("isolate", kAssetA));

  ChangePlan failed_root = plan;
  failed_root.step_status.insert_or_assign(StepId(step_id_for("drain", kAssetA)),
                                           StepStatus::Failed);
  FCO_CHECK(fco::ready_steps(failed_root).empty());

  ChangePlan issued_root = plan;
  issued_root.step_status.insert_or_assign(StepId(step_id_for("drain", kAssetA)),
                                           StepStatus::Issued);
  FCO_CHECK(fco::ready_steps(issued_root).empty());

  // Every status the plan can carry is checked against the documented contract.
  const std::vector<StepStatus> statuses = {
      StepStatus::Pending,      StepStatus::Ready,      StepStatus::Issued,
      StepStatus::Acknowledged, StepStatus::Observed,   StepStatus::Verified,
      StepStatus::Failed,       StepStatus::Compensated, StepStatus::Blocked,
      StepStatus::Unresolved,   StepStatus::NonCompensable,
  };
  for (const StepStatus status : statuses) {
    ChangePlan variant = plan;
    variant.step_status.insert_or_assign(StepId(step_id_for("drain", kAssetA)),
                                         StepStatus::Verified);
    variant.step_status.insert_or_assign(StepId(step_id_for("isolate", kAssetA)), status);
    const std::vector<StepId> ready = fco::ready_steps(variant);
    for (const StepId& id : ready) {
      const StepStatus current_status = variant.status_of(id);
      FCO_CHECK(!fco::is_settled_step_status(current_status));
      FCO_CHECK(current_status != StepStatus::Issued);
      FCO_CHECK(current_status != StepStatus::Acknowledged);
      FCO_CHECK(current_status != StepStatus::Observed);
      FCO_CHECK(current_status != StepStatus::Unresolved);
      const ChangeStep* step = variant.find_step(id);
      FCO_REQUIRE(step != nullptr);
      for (const StepId& dependency : step->depends_on) {
        FCO_CHECK_EQ(variant.status_of(dependency), StepStatus::Verified);
      }
    }
    // The isolation step itself is ready only while its predecessor is verified
    // and it is not already settled or in flight.
    const bool isolate_ready = contains(ready, StepId(step_id_for("isolate", kAssetA)));
    const bool isolate_expected = !fco::is_settled_step_status(status) &&
                                  status != StepStatus::Issued &&
                                  status != StepStatus::Acknowledged &&
                                  status != StepStatus::Observed &&
                                  status != StepStatus::Unresolved;
    FCO_CHECK_EQ(isolate_ready, isolate_expected);
  }
}

FCO_TEST_MAIN
