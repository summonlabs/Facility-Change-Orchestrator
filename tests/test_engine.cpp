// Orchestrator integration tests.
//
// These exercise the composition runtime end to end against the synthetic plant
// and the real durable store: full lifecycle, deterministic ordering, generation
// and authority fencing, point-of-no-return semantics, rollback, unresolved
// attempts, restart recovery, and idempotent replay.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "fco/engine.hpp"
#include "fco/evidence.hpp"
#include "fco/planner.hpp"
#include "fco/sim.hpp"
#include "fco/store.hpp"
#include "test_support.hpp"

namespace {

namespace fs = std::filesystem;

class TempRoot {
 public:
  TempRoot() {
    static unsigned counter = 0;
    ++counter;
    std::ostringstream name;
    name << "fco-engine-" << counter << '-' << std::chrono::steady_clock::now()
                                                   .time_since_epoch()
                                                   .count();
    root_ = fs::temp_directory_path() / name.str();
    std::error_code code;
    fs::remove_all(root_, code);
    fs::create_directories(root_, code);
  }
  ~TempRoot() {
    std::error_code code;
    fs::remove_all(root_, code);
  }
  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;
  [[nodiscard]] const fs::path& path() const noexcept { return root_; }

 private:
  fs::path root_;
};

[[nodiscard]] fco::GenerationSet generations_for(std::uint64_t epoch) {
  fco::GenerationSet generations;
  generations.facility_epoch = fco::FacilityEpoch(epoch);
  generations.policy = fco::PolicyGeneration(2);
  generations.dependency = fco::DependencyGeneration(5);
  generations.topology = fco::TopologyGeneration(7);
  generations.capacity = fco::CapacityGeneration(3);
  generations.lifecycle = fco::LifecycleGeneration(4);
  generations.maintenance = fco::MaintenanceGeneration(6);
  generations.power = fco::PowerGeneration(8);
  generations.cooling = fco::CoolingGeneration(9);
  generations.asi = fco::AsiGeneration(11);
  generations.dfi = fco::DfiGeneration(12);
  return generations;
}

struct Runtime {
  fco::ManualClock clock{1000000};
  fco::DomainRegistry registry;
  std::unique_ptr<fco::SimulatedPlant> plant;
  std::unique_ptr<fco::SimulatedAuthoritySet> authorities;
  std::optional<fco::Orchestrator> orchestrator;

  [[nodiscard]] bool open(const fs::path& root) {
    auto created = fco::Orchestrator::open(root, clock, registry);
    if (!created.ok()) return false;
    orchestrator = created.take();
    return true;
  }

  [[nodiscard]] bool initialize() {
    fco::FacilitySnapshot facility =
        fco::make_example_facility(fco::SiteId("site-alpha"), fco::FacilityEpoch(4),
                                   generations_for(4));
    const fco::Status status = orchestrator->initialize(
        std::move(facility), fco::IncarnationId(1), fco::ControlEpoch(1),
        fco::PrincipalId("principal-facility-ops"), fco::PolicyId("policy-change-control-v1"),
        fco::Sha256::of("policy-change-control-v1"));
    return status.ok();
  }

  [[nodiscard]] bool attach_plant() {
    if (!orchestrator->state().initialized()) return false;
    const fco::OrchestratorState& state = orchestrator->state();
    plant = std::make_unique<fco::SimulatedPlant>(state.facility);
    authorities = std::make_unique<fco::SimulatedAuthoritySet>(*plant, state.incarnation,
                                                              state.control_epoch);
    for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(fco::kDomainKindMax); ++kind) {
      const auto domain = static_cast<fco::DomainKind>(kind);
      fco::DomainAuthority* authority = authorities->find(domain);
      if (authority == nullptr) continue;
      registry.add(authority);
      const fco::ObservationSequence watermark = state.evidence.watermark(domain, state.incarnation);
      plant->seed_observation_sequence(domain, watermark.valid() ? watermark.value() + 1u : 1u);
    }
    return true;
  }

  [[nodiscard]] fco::Result<fco::PlanId> synthesize(std::string_view kind,
                                                    const std::vector<std::string>& assets) {
    fco::SynthesisInput input;
    input.kind = fco::ChangeKind(std::string(kind));
    input.id = fco::ChangeRequestId("req-" + std::string(kind));
    input.site = orchestrator->state().facility.site();
    input.intent = "integration test change";
    input.scope.site = input.site;
    input.scope.rack = fco::RackId("rack-a1");
    for (const std::string& asset : assets) input.scope.assets.push_back(fco::AssetId(asset));
    input.facility_epoch = orchestrator->state().facility_epoch;
    input.actor = orchestrator->state().actor;
    input.revision = fco::Revision(1);
    input.bound_generations = orchestrator->state().generations;
    input.evidence_digest = orchestrator->state().facility.digest();
    auto request = orchestrator->synthesize(input);
    if (!request.ok()) return request.error();
    return orchestrator->create_plan(request.value());
  }

  [[nodiscard]] fco::Result<std::vector<fco::StepId>> execute_all_collecting(
      const fco::PlanId& plan) {
    std::vector<fco::StepId> issued;
    for (int iteration = 0; iteration < 256; ++iteration) {
      auto step = orchestrator->execute_next(plan);
      if (!step.ok()) {
        if (step.error().code == fco::ErrorCode::PlanTerminal ||
            step.error().code == fco::ErrorCode::NoExecutableStep) {
          return fco::success(std::move(issued));
        }
        return step.error();
      }
      issued.push_back(step.value().step);
    }
    return fco::failure<std::vector<fco::StepId>>(fco::ErrorCode::BoundedLimitExceeded, "test",
                                                  "execution did not settle");
  }

  // Rebuilds the simulated authorities from the orchestrator's current observed
  // facility. Needed whenever an external observation moves the facility view:
  // an authority whose own view is behind the authoritative one is correctly
  // rejected as a generation regression, so the two must be brought back into
  // agreement before execution is attempted again.
  [[nodiscard]] bool rebuild_plant() {
    authorities.reset();
    plant.reset();
    return attach_plant();
  }

  [[nodiscard]] bool prepare(const fs::path& root) {
    return open(root) && initialize() && attach_plant();
  }

  void close() {
    authorities.reset();
    plant.reset();
    orchestrator.reset();
  }
};

constexpr const char* kAsset = "rack-a1-node-1";

}  // namespace

FCO_TEST(engine, rack_replacement_completes_and_closes) {
  TempRoot temp;
  Runtime runtime;
  FCO_CHECK(runtime.prepare(temp.path()));

  auto plan_id = runtime.synthesize("rack-replacement", {kAsset});
  FCO_CHECK(plan_id.ok());
  if (!plan_id.ok()) return;
  const fco::PlanId plan = plan_id.value();

  auto plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK_EQ(plan_value.value().steps.size(), std::size_t{13});
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().state)), std::string("draft"));

  const fco::PlanEvaluation evaluation = runtime.orchestrator->evaluate(plan);
  FCO_CHECK(evaluation.accepted());
  FCO_CHECK_EQ(evaluation.initially_ready.size(), std::size_t{1});
  FCO_CHECK_EQ(evaluation.order.size(), std::size_t{13});

  FCO_CHECK(runtime.orchestrator->mark_evaluated(plan).ok());
  FCO_CHECK(runtime.orchestrator->authorize(plan).ok());

  auto issued = runtime.execute_all_collecting(plan);
  FCO_CHECK(issued.ok());
  if (!issued.ok()) return;
  FCO_CHECK_EQ(issued.value().size(), std::size_t{13});

  plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().state)), std::string("completed"));
  FCO_CHECK_EQ(plan_value.value().count_with_status(fco::StepStatus::Verified), std::size_t{13});
  FCO_CHECK_EQ(plan_value.value().unresolved_attempt_count(), std::size_t{0});

  const fco::ClosureReport closure = runtime.orchestrator->closure();
  FCO_CHECK(closure.closed());
  FCO_CHECK_EQ(closure.plans_completed, std::size_t{1});
  FCO_CHECK_EQ(closure.unresolved_attempts, std::size_t{0});

  // Every issued step was applied exactly once by the owning authority.
  FCO_CHECK_EQ(runtime.plant->applied_actions(), std::uint64_t{13});
}

FCO_TEST(engine, execution_order_is_deterministic) {
  TempRoot temp;
  Runtime runtime;
  FCO_CHECK(runtime.prepare(temp.path()));

  auto plan_id = runtime.synthesize("rack-replacement", {kAsset});
  FCO_CHECK(plan_id.ok());
  if (!plan_id.ok()) return;
  const fco::PlanId plan = plan_id.value();
  FCO_CHECK(runtime.orchestrator->mark_evaluated(plan).ok());
  FCO_CHECK(runtime.orchestrator->authorize(plan).ok());

  auto issued = runtime.execute_all_collecting(plan);
  FCO_CHECK(issued.ok());
  if (!issued.ok()) return;

  const std::string suffix = std::string("-") + kAsset;
  const std::vector<std::string> expected = {
      "drain" + suffix,     "isolate" + suffix,     "power-off" + suffix,
      "cooling-off" + suffix, "net-off" + suffix,   "swap" + suffix,
      "net-on" + suffix,    "power-on" + suffix,    "cooling-on" + suffix,
      "commission" + suffix, "maint-off" + suffix,  "verify" + suffix,
      "restore" + suffix};
  FCO_CHECK_EQ(issued.value().size(), expected.size());
  for (std::size_t index = 0;
       index < std::min(issued.value().size(), expected.size()); ++index) {
    FCO_CHECK_EQ(issued.value()[index].value(), expected[index]);
  }
}

FCO_TEST(engine, external_generation_advance_fences_the_plan) {
  TempRoot temp;
  Runtime runtime;
  FCO_CHECK(runtime.prepare(temp.path()));

  auto plan_id = runtime.synthesize("rack-replacement", {kAsset});
  FCO_CHECK(plan_id.ok());
  if (!plan_id.ok()) return;
  const fco::PlanId plan = plan_id.value();
  FCO_CHECK(runtime.orchestrator->mark_evaluated(plan).ok());
  FCO_CHECK(runtime.orchestrator->authorize(plan).ok());

  // An external lifecycle change: the world moved without this plan.
  fco::FacilitySnapshot bumped = runtime.orchestrator->state().facility;
  fco::GenerationSet generations = runtime.orchestrator->state().generations;
  generations.lifecycle = fco::LifecycleGeneration(generations.lifecycle.value() + 1);
  bumped.set_generations(generations);
  FCO_CHECK(runtime.orchestrator->observe_facility(bumped, "external lifecycle event").ok());
  FCO_CHECK(runtime.rebuild_plant());

  auto step = runtime.orchestrator->execute_next(plan);
  FCO_CHECK(!step.ok());
  if (!step.ok()) {
    FCO_CHECK_ERROR(step, fco::ErrorCode::StaleGeneration);
  }

  auto plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().state)),
               std::string("replan-required"));

  // Replanning re-bases the binding on the observed facility and preserves the
  // verified progress, which here is none.
  FCO_CHECK(runtime.orchestrator->replan(plan, "generations moved").ok());
  plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK_EQ(plan_value.value().revision.value(), std::uint64_t{2});
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().state)), std::string("draft"));

  FCO_CHECK(runtime.orchestrator->mark_evaluated(plan).ok());
  FCO_CHECK(runtime.orchestrator->authorize(plan).ok());
  auto issued = runtime.execute_all_collecting(plan);
  FCO_CHECK(issued.ok());
  if (!issued.ok()) return;
  FCO_CHECK_EQ(issued.value().size(), std::size_t{13});
  plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().state)), std::string("completed"));
}

FCO_TEST(engine, authority_takeover_fences_an_authorized_plan) {
  TempRoot temp;
  Runtime runtime;
  FCO_CHECK(runtime.prepare(temp.path()));

  auto plan_id = runtime.synthesize("rack-replacement", {kAsset});
  FCO_CHECK(plan_id.ok());
  if (!plan_id.ok()) return;
  const fco::PlanId plan = plan_id.value();
  FCO_CHECK(runtime.orchestrator->mark_evaluated(plan).ok());
  FCO_CHECK(runtime.orchestrator->authorize(plan).ok());

  const fco::Status takeover = runtime.orchestrator->assume_authority(
      fco::IncarnationId(2), fco::ControlEpoch(2), fco::PrincipalId("principal-facility-ops"),
      fco::PolicyId("policy-change-control-v1"), fco::Sha256::of("policy-change-control-v1"));
  FCO_CHECK(takeover.ok());

  auto step = runtime.orchestrator->execute_next(plan);
  FCO_CHECK(!step.ok());
  if (!step.ok()) {
    FCO_CHECK_ERROR(step, fco::ErrorCode::FencedAuthority);
  }
  auto plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().state)),
               std::string("replan-required"));

  // A takeover with an older epoch is refused outright.
  const fco::Status regression = runtime.orchestrator->assume_authority(
      fco::IncarnationId(3), fco::ControlEpoch(1), fco::PrincipalId("principal-facility-ops"),
      fco::PolicyId("policy-change-control-v1"), fco::Sha256::of("policy-change-control-v1"));
  FCO_CHECK(!regression.ok());
  FCO_CHECK_ERROR(regression, fco::ErrorCode::StaleAuthority);
}

FCO_TEST(engine, rollback_is_refused_after_the_point_of_no_return) {
  TempRoot temp;
  Runtime runtime;
  FCO_CHECK(runtime.prepare(temp.path()));

  auto plan_id = runtime.synthesize("rack-replacement", {kAsset});
  FCO_CHECK(plan_id.ok());
  if (!plan_id.ok()) return;
  const fco::PlanId plan = plan_id.value();
  FCO_CHECK(runtime.orchestrator->mark_evaluated(plan).ok());
  FCO_CHECK(runtime.orchestrator->authorize(plan).ok());

  const fco::StepId swap(std::string("swap-") + kAsset);
  bool reached = false;
  for (int iteration = 0; iteration < 64; ++iteration) {
    auto step = runtime.orchestrator->execute_next(plan);
    if (!step.ok()) break;
    auto plan_value = runtime.orchestrator->plan(plan);
    if (!plan_value.ok()) break;
    if (plan_value.value().status_of(swap) == fco::StepStatus::Verified) {
      reached = true;
      break;
    }
  }
  FCO_CHECK(reached);
  if (!reached) return;

  const fco::Status rolled = runtime.orchestrator->rollback(plan, "attempted rollback");
  FCO_CHECK(!rolled.ok());
  FCO_CHECK_ERROR(rolled, fco::ErrorCode::NonReversibleStep);

  auto plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK(plan_value.value().any_non_reversible_applied());
  FCO_CHECK(plan_value.value().transition_note.find("cannot be undone") != std::string::npos);
}

FCO_TEST(engine, rollback_compensates_before_the_point_of_no_return) {
  TempRoot temp;
  Runtime runtime;
  FCO_CHECK(runtime.prepare(temp.path()));

  auto plan_id = runtime.synthesize("rack-replacement", {kAsset});
  FCO_CHECK(plan_id.ok());
  if (!plan_id.ok()) return;
  const fco::PlanId plan = plan_id.value();
  FCO_CHECK(runtime.orchestrator->mark_evaluated(plan).ok());
  FCO_CHECK(runtime.orchestrator->authorize(plan).ok());

  FCO_CHECK(runtime.orchestrator->execute_next(plan).ok());
  FCO_CHECK(runtime.orchestrator->execute_next(plan).ok());

  const fco::StepId drain(std::string("drain-") + kAsset);
  const fco::StepId isolate(std::string("isolate-") + kAsset);
  auto plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().status_of(drain))),
               std::string("verified"));

  FCO_CHECK(runtime.orchestrator->rollback(plan, "operator rolled back").ok());
  plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().state)), std::string("rolled-back"));
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().status_of(drain))),
               std::string("compensated"));
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().status_of(isolate))),
               std::string("compensated"));

  // The compensation restored the workloads, and no forward step was issued.
  const fco::AssetRecord* record = runtime.orchestrator->state().facility.find(fco::AssetId(kAsset));
  FCO_CHECK(record != nullptr);
  if (record != nullptr) {
    FCO_CHECK_EQ(std::string(fco::to_string(record->workloads)), std::string("present"));
  }
}

FCO_TEST(engine, unresolved_attempt_blocks_execution_until_resolved) {
  TempRoot temp;
  Runtime runtime;
  FCO_CHECK(runtime.prepare(temp.path()));

  auto plan_id = runtime.synthesize("rack-replacement", {kAsset});
  FCO_CHECK(plan_id.ok());
  if (!plan_id.ok()) return;
  const fco::PlanId plan = plan_id.value();
  FCO_CHECK(runtime.orchestrator->mark_evaluated(plan).ok());
  FCO_CHECK(runtime.orchestrator->authorize(plan).ok());

  runtime.plant->stall_domain(fco::DomainKind::Asi, true);
  auto blocked_step = runtime.orchestrator->execute_next(plan);
  FCO_CHECK(!blocked_step.ok());
  if (!blocked_step.ok()) {
    FCO_CHECK_ERROR(blocked_step, fco::ErrorCode::AttemptUnresolved);
  }

  auto plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().state)), std::string("paused"));
  FCO_CHECK_EQ(plan_value.value().unresolved_attempt_count(), std::size_t{1});

  // Nothing may be re-issued while the outcome is unknown.
  auto again = runtime.orchestrator->execute_next(plan);
  FCO_CHECK(!again.ok());
  FCO_CHECK_EQ(runtime.plant->applied_actions(), std::uint64_t{0});

  const fco::AttemptRecord& unresolved = plan_value.value().attempts.front();
  const fco::AttemptId attempt = unresolved.id;
  FCO_CHECK(runtime.orchestrator->resolve_attempt(attempt, fco::AttemptStatus::NotIssued,
                                                  "confirmed not issued")
                .ok());

  plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  FCO_CHECK_EQ(std::string(fco::to_string(plan_value.value().state)), std::string("executing"));

  runtime.plant->stall_domain(fco::DomainKind::Asi, false);
  auto issued = runtime.execute_all_collecting(plan);
  FCO_CHECK(issued.ok());
  if (!issued.ok()) return;
  FCO_CHECK_EQ(issued.value().size(), std::size_t{13});
}

FCO_TEST(engine, restart_recovery_reclassifies_without_reissuing) {
  TempRoot temp;
  {
    Runtime runtime;
    FCO_CHECK(runtime.prepare(temp.path()));
    auto plan_id = runtime.synthesize("rack-replacement", {kAsset});
    FCO_CHECK(plan_id.ok());
    if (!plan_id.ok()) return;
    const fco::PlanId plan = plan_id.value();
    FCO_CHECK(runtime.orchestrator->mark_evaluated(plan).ok());
    FCO_CHECK(runtime.orchestrator->authorize(plan).ok());
    FCO_CHECK(runtime.orchestrator->execute_next(plan).ok());
    FCO_CHECK(runtime.orchestrator->execute_next(plan).ok());
    FCO_CHECK_EQ(runtime.plant->applied_actions(), std::uint64_t{2});
  }

  Runtime restarted;
  FCO_CHECK(restarted.open(temp.path()));
  FCO_CHECK(restarted.attach_plant());
  FCO_CHECK_EQ(restarted.plant->applied_actions(), std::uint64_t{0});

  auto summary = restarted.orchestrator->recover();
  FCO_CHECK(summary.ok());
  if (!summary.ok()) return;
  FCO_CHECK_EQ(summary.value().requiring_resolution, std::size_t{0});

  // The two verified steps are still verified, so nothing is re-issued for them.
  const fco::ChangePlan& plan = restarted.orchestrator->state().plans.begin()->second;
  FCO_CHECK_EQ(plan.count_with_status(fco::StepStatus::Verified), std::size_t{2});
  const fco::StepId drain(std::string("drain-") + kAsset);
  FCO_CHECK_EQ(std::string(fco::to_string(plan.status_of(drain))), std::string("verified"));

  auto issued = restarted.execute_all_collecting(plan.id);
  FCO_CHECK(issued.ok());
  if (!issued.ok()) return;
  FCO_CHECK_EQ(issued.value().size(), std::size_t{11});
  FCO_CHECK_EQ(restarted.plant->applied_actions(), std::uint64_t{11});
  const fco::ChangePlan& finished = restarted.orchestrator->state().plans.begin()->second;
  FCO_CHECK_EQ(std::string(fco::to_string(finished.state)), std::string("completed"));
}

FCO_TEST(engine, evidence_ledger_rejects_reordering_and_accepts_replay) {
  fco::EvidenceLedger ledger;
  fco::GenerationSet generations = generations_for(4);

  auto build = [&](std::uint64_t sequence, const char* note) {
    fco::EvidenceRecord record;
    record.id = fco::EvidenceId(std::string("ev-") + note);
    record.plan = fco::PlanId("plan-1");
    record.plan_revision = fco::PlanRevision(1);
    record.step = fco::StepId("drain-asset-1");
    record.attempt = fco::AttemptId("att-1");
    record.source_domain = fco::DomainKind::Asi;
    record.source_incarnation = fco::IncarnationId(1);
    record.source_control_epoch = fco::ControlEpoch(1);
    record.observation_sequence = fco::ObservationSequence(sequence);
    record.observed_generations = generations;
    record.content_digest = fco::Sha256::of(note);
    record.outcome = fco::EvidenceOutcome::Indeterminate;
    return record;
  };

  FCO_CHECK(ledger.append(build(5, "five")).ok());
  FCO_CHECK_EQ(ledger.size(), std::size_t{1});

  auto replayed = ledger.append(build(5, "five"));
  FCO_CHECK(replayed.ok());
  if (replayed.ok()) FCO_CHECK(!replayed.value());
  FCO_CHECK_EQ(ledger.size(), std::size_t{1});

  auto reordered = ledger.append(build(4, "four"));
  FCO_CHECK(!reordered.ok());
  if (!reordered.ok()) {
    FCO_CHECK_ERROR(reordered, fco::ErrorCode::ReorderedEvidence);
  }

  auto conflicting = ledger.append(build(6, "five"));
  FCO_CHECK(!conflicting.ok());
  if (!conflicting.ok()) {
    FCO_CHECK_ERROR(conflicting, fco::ErrorCode::DuplicateIdentity);
  }

  FCO_CHECK_EQ(ledger.watermark(fco::DomainKind::Asi, fco::IncarnationId(1)).value(),
               std::uint64_t{5});
  FCO_CHECK(!ledger.watermark(fco::DomainKind::Asi, fco::IncarnationId(9)).valid());
}

FCO_TEST(engine, request_validation_resolves_to_the_documented_primary_error) {
  const fco::FacilitySnapshot facility = fco::make_example_facility(
      fco::SiteId("site-alpha"), fco::FacilityEpoch(4), generations_for(4));
  const fco::AuthorityContext authority{
      fco::IncarnationId(1), fco::ControlEpoch(1), fco::PrincipalId("principal-facility-ops"),
      fco::PolicyId("policy-change-control-v1"), fco::Sha256::of("policy-change-control-v1")};

  auto make_step = [](const std::string& id, fco::DomainKind owner, fco::ActionKind action,
                      bool with_dependency) {
    fco::ChangeStep step;
    step.id = fco::StepId(id);
    step.owner = owner;
    step.action = action;
    step.safety = fco::SafetyClass::Standard;
    step.reversibility = fco::Reversibility::Reversible;
    step.verification = fco::VerificationMode::Required;
    step.request.kind = action;
    step.request.owner = owner;
    step.request.asset = fco::AssetId("rack-a1-node-1");
    step.request.rack = fco::RackId("rack-a1");
    step.request.site = fco::SiteId("site-alpha");
    step.request.hardware_generation = fco::HardwareGeneration(3);
    step.request.firmware_generation = fco::FirmwareGeneration(7);
    step.request.lifecycle_generation = fco::LifecycleGeneration(2);
    step.expected_effect.kind = fco::EffectKind::WorkloadStateChange;
    step.expected_effect.asset = fco::AssetId("rack-a1-node-1");
    step.expected_effect.expected_state =
        static_cast<std::uint32_t>(static_cast<std::uint8_t>(fco::WorkloadState::Drained));
    if (with_dependency) step.depends_on.push_back(fco::StepId("step-missing"));
    return step;
  };

  fco::ChangeRequest request;
  request.id = fco::ChangeRequestId("req-defective");
  request.kind = fco::ChangeKind("rack-replacement");
  request.site = fco::SiteId("site-alpha");
  request.intent = "defective request";
  request.scope.site = fco::SiteId("site-alpha");
  request.scope.rack = fco::RackId("rack-a1");
  request.scope.assets.push_back(fco::AssetId("rack-a1-node-1"));
  request.facility_epoch = fco::FacilityEpoch(4);
  request.actor = fco::PrincipalId("principal-facility-ops");
  request.revision = fco::Revision(1);
  request.bound_generations = generations_for(4);

  // Three independent defects at once: a wrong owning authority (301), a missing
  // evidence binding (405) and an unknown dependency edge (505). The documented
  // precedence must pick 301 regardless of the order in which they are found.
  request.steps.push_back(make_step("step-a", fco::DomainKind::Power,
                                    fco::ActionKind::LifecycleTransition, false));
  request.steps.push_back(
      make_step("step-b", fco::DomainKind::Asi, fco::ActionKind::DrainWorkloads, true));

  auto created = fco::create_plan(request, facility, authority, fco::PlanRevision(1),
                                  fco::FacilityRevision(1), 1000000);
  FCO_CHECK(!created.ok());
  if (!created.ok()) {
    FCO_CHECK_ERROR(created, fco::ErrorCode::AuthorityMismatch);
  }

  // Removing the highest-precedence defect must surface the next one, and
  // removing that must surface the last.
  request.steps[0].owner = fco::DomainKind::Lifecycle;
  request.steps[0].request.owner = fco::DomainKind::Lifecycle;
  created = fco::create_plan(request, facility, authority, fco::PlanRevision(1),
                             fco::FacilityRevision(1), 1000000);
  FCO_CHECK(!created.ok());
  if (!created.ok()) {
    FCO_CHECK_ERROR(created, fco::ErrorCode::UnboundEvidence);
  }

  request.evidence_digest = fco::Sha256::of("evidence-package");
  created = fco::create_plan(request, facility, authority, fco::PlanRevision(1),
                             fco::FacilityRevision(1), 1000000);
  FCO_CHECK(!created.ok());
  if (!created.ok()) {
    FCO_CHECK_ERROR(created, fco::ErrorCode::MissingPredecessor);
  }

  request.steps[1].depends_on.clear();
  created = fco::create_plan(request, facility, authority, fco::PlanRevision(1),
                             fco::FacilityRevision(1), 1000000);
  FCO_CHECK(created.ok());
  if (created.ok()) {
    FCO_CHECK_EQ(created.value().steps.size(), std::size_t{2});
    FCO_CHECK_EQ(std::string(fco::to_string(created.value().state)), std::string("draft"));
  }
}

FCO_TEST(engine, attempt_replay_does_not_apply_twice) {
  TempRoot temp;
  Runtime runtime;
  FCO_CHECK(runtime.prepare(temp.path()));

  auto plan_id = runtime.synthesize("power-maintenance", {kAsset});
  FCO_CHECK(plan_id.ok());
  if (!plan_id.ok()) return;
  const fco::PlanId plan = plan_id.value();
  FCO_CHECK(runtime.orchestrator->mark_evaluated(plan).ok());
  FCO_CHECK(runtime.orchestrator->authorize(plan).ok());
  auto issued = runtime.execute_all_collecting(plan);
  FCO_CHECK(issued.ok());
  if (!issued.ok()) return;

  auto plan_value = runtime.orchestrator->plan(plan);
  FCO_CHECK(plan_value.ok());
  const std::size_t verified =
      plan_value.value().count_with_status(fco::StepStatus::Verified);
  FCO_CHECK_EQ(runtime.plant->applied_actions(), static_cast<std::uint64_t>(verified));
  FCO_CHECK_EQ(plan_value.value().attempts.size(), verified);

  // A completed plan refuses further execution and applies nothing.
  auto terminal = runtime.orchestrator->execute_next(plan);
  FCO_CHECK(!terminal.ok());
  if (!terminal.ok()) {
    FCO_CHECK_ERROR(terminal, fco::ErrorCode::PlanTerminal);
  }
  FCO_CHECK_EQ(runtime.plant->applied_actions(), static_cast<std::uint64_t>(verified));
}

FCO_TEST_MAIN

