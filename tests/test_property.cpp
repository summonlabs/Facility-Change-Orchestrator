// Deterministic property tests.
//
// Every generator is seeded with a fixed literal, so a failure is reproducible
// from the seed and the iteration index that the failure message prints. The
// properties are the ones the headers promise: deterministic topological order,
// a digest that is exactly the equality of the hashed fields, the currency
// outcome, store and codec round-trips, and the fencing decision.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "fco/authority.hpp"
#include "fco/codec.hpp"
#include "fco/digest.hpp"
#include "fco/error.hpp"
#include "fco/evidence.hpp"
#include "fco/facility.hpp"
#include "fco/model.hpp"
#include "fco/state.hpp"
#include "fco/store.hpp"
#include "fco/strong.hpp"
#include "test_support.hpp"

namespace {

namespace fs = std::filesystem;

using fco::ActionKind;
using fco::ActionRequest;
using fco::AssetId;
using fco::AssetLifecycle;
using fco::AssetRecord;
using fco::AttemptId;
using fco::AttemptOrdinal;
using fco::AttemptRecord;
using fco::AttemptStatus;
using fco::AuthorityContext;
using fco::ChangeKind;
using fco::ChangePlan;
using fco::ChangeRequestId;
using fco::ChangeStep;
using fco::CommitSequence;
using fco::ControlEpoch;
using fco::CoolingState;
using fco::CurrencyCheck;
using fco::CurrencyOutcome;
using fco::Digest;
using fco::DomainKind;
using fco::DurableStore;
using fco::EffectKind;
using fco::EffectSpec;
using fco::ErrorCode;
using fco::EvidenceId;
using fco::EvidenceOutcome;
using fco::EvidenceRecord;
using fco::FacilityEpoch;
using fco::FacilityRevision;
using fco::FacilitySnapshot;
using fco::FencingDecision;
using fco::FirmwareGeneration;
using fco::GenerationDelta;
using fco::GenerationKind;
using fco::GenerationSet;
using fco::HardwareGeneration;
using fco::IncarnationId;
using fco::LifecycleGeneration;
using fco::MaintenanceGeneration;
using fco::MaintenanceState;
using fco::NetworkState;
using fco::OrchestratorState;
using fco::PlanId;
using fco::PlanRevision;
using fco::PlanState;
using fco::PolicyGeneration;
using fco::PolicyId;
using fco::PowerState;
using fco::Predicate;
using fco::PredicateKind;
using fco::PrincipalId;
using fco::Revision;
using fco::SafetyClass;
using fco::Sha256;
using fco::SiteId;
using fco::StepId;
using fco::StepStatus;
using fco::TenantId;
using fco::VerificationMode;
using fco::WorkloadState;

constexpr std::uint32_t kGenerationCount = static_cast<std::uint32_t>(fco::kGenerationKindMax);

void fail_property(const char* property, std::uint64_t seed, std::size_t iteration,
                   const std::string& message) {
  std::ostringstream text;
  text << property << " [seed=0x" << std::hex << seed << std::dec
       << " iteration=" << iteration << "]: " << message;
  ::fco::test::report_failure(__FILE__, __LINE__, text.str());
}

[[nodiscard]] std::uint64_t pick(std::mt19937_64& rng, std::uint64_t bound) {
  return bound == 0 ? 0 : rng() % bound;
}

[[nodiscard]] std::string hex_of(std::uint64_t value) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(kDigits[(value >> shift) & 0x0Full]);
  }
  return out;
}

[[nodiscard]] Digest random_digest(std::mt19937_64& rng) {
  Digest::Bytes bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(pick(rng, 256u));
  }
  if (bytes[0] == 0u) bytes[0] = 1u;
  return Digest::from_bytes(bytes);
}

[[nodiscard]] GenerationSet random_generations(std::mt19937_64& rng) {
  GenerationSet set;
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    set.set(static_cast<GenerationKind>(raw), 1u + pick(rng, 1000u));
  }
  return set;
}

[[nodiscard]] AuthorityContext random_authority(std::mt19937_64& rng, bool complete) {
  AuthorityContext context;
  context.incarnation = IncarnationId(1u + pick(rng, 3u));
  context.control_epoch = ControlEpoch(1u + pick(rng, 3u));
  context.actor = PrincipalId(pick(rng, 2u) == 0u ? "principal-a" : "principal-b");
  context.policy = PolicyId(pick(rng, 2u) == 0u ? "policy-a" : "policy-b");
  context.policy_digest = random_digest(rng);
  if (!complete) {
    switch (pick(rng, 4u)) {
      case 0u:
        context.incarnation = IncarnationId{};
        break;
      case 1u:
        context.control_epoch = ControlEpoch{};
        break;
      case 2u:
        context.actor = PrincipalId{};
        break;
      default:
        context.policy = PolicyId{};
        break;
    }
  }
  return context;
}

[[nodiscard]] AssetRecord random_asset(std::mt19937_64& rng, const std::string& id) {
  AssetRecord record;
  record.id = AssetId(id);
  record.rack = fco::RackId("rack-" + std::to_string(pick(rng, 4u)));
  record.site = SiteId("site-alpha");
  record.lifecycle =
      static_cast<AssetLifecycle>(1u + pick(rng, static_cast<std::uint64_t>(fco::kAssetLifecycleMax)));
  record.power =
      static_cast<PowerState>(1u + pick(rng, static_cast<std::uint64_t>(fco::kPowerStateMax)));
  record.cooling =
      static_cast<CoolingState>(1u + pick(rng, static_cast<std::uint64_t>(fco::kCoolingStateMax)));
  record.maintenance = static_cast<MaintenanceState>(
      1u + pick(rng, static_cast<std::uint64_t>(fco::kMaintenanceStateMax)));
  record.network =
      static_cast<NetworkState>(1u + pick(rng, static_cast<std::uint64_t>(fco::kNetworkStateMax)));
  record.workloads =
      static_cast<WorkloadState>(1u + pick(rng, static_cast<std::uint64_t>(fco::kWorkloadStateMax)));
  record.hardware_generation = HardwareGeneration(1u + pick(rng, 50u));
  record.firmware_generation = FirmwareGeneration(1u + pick(rng, 50u));
  record.lifecycle_generation = LifecycleGeneration(1u + pick(rng, 50u));
  record.maintenance_generation = MaintenanceGeneration(1u + pick(rng, 50u));
  record.capacity_generation = fco::CapacityGeneration(1u + pick(rng, 50u));
  if (pick(rng, 2u) == 1u) record.tenant = TenantId("tenant-" + std::to_string(pick(rng, 3u)));
  record.capacity_total_units = static_cast<std::uint32_t>(pick(rng, 1000u));
  record.capacity_reserved_units =
      static_cast<std::uint32_t>(pick(rng, record.capacity_total_units + 1u));
  return record;
}

[[nodiscard]] ActionRequest random_request(std::mt19937_64& rng, const AssetRecord& asset) {
  ActionRequest request;
  request.kind = static_cast<ActionKind>(
      1u + pick(rng, static_cast<std::uint64_t>(fco::kActionKindMax)));
  request.owner = fco::owning_domain(request.kind);
  request.asset = asset.id;
  request.rack = asset.rack;
  request.site = asset.site;
  request.hardware_generation = asset.hardware_generation;
  request.firmware_generation = asset.firmware_generation;
  request.lifecycle_generation = asset.lifecycle_generation;
  request.target_state = static_cast<std::uint32_t>(pick(rng, 100u));
  request.target_value = pick(rng, 100u);
  return request;
}

[[nodiscard]] ChangeStep random_step(std::mt19937_64& rng, const std::string& id,
                                     const AssetRecord& asset,
                                     const std::vector<StepId>& possible_predecessors) {
  ChangeStep step;
  step.id = StepId(id);
  step.action = static_cast<ActionKind>(
      1u + pick(rng, static_cast<std::uint64_t>(fco::kActionKindMax)));
  step.owner = fco::owning_domain(step.action);
  step.safety = static_cast<SafetyClass>(
      1u + pick(rng, static_cast<std::uint64_t>(fco::kSafetyClassMax)));
  step.reversibility = static_cast<fco::Reversibility>(
      1u + pick(rng, static_cast<std::uint64_t>(fco::kReversibilityMax)));
  step.verification = static_cast<VerificationMode>(
      1u + pick(rng, static_cast<std::uint64_t>(fco::kVerificationModeMax)));
  step.point_of_no_return = pick(rng, 2u) == 1u;
  step.request = random_request(rng, asset);
  step.action = step.request.kind;
  step.owner = step.request.owner;
  const std::size_t predicates = static_cast<std::size_t>(pick(rng, 3u));
  for (std::size_t index = 0; index < predicates; ++index) {
    Predicate predicate;
    predicate.kind = static_cast<PredicateKind>(
        1u + pick(rng, static_cast<std::uint64_t>(fco::kPredicateKindMax)));
    predicate.asset = asset.id;
    if (predicate.kind == PredicateKind::PredecessorVerified ||
        predicate.kind == PredicateKind::PredecessorNotVerified) {
      if (!possible_predecessors.empty()) {
        predicate.predecessor =
            possible_predecessors[static_cast<std::size_t>(pick(rng, possible_predecessors.size()))];
      }
    }
    predicate.expected_state = static_cast<std::uint32_t>(pick(rng, 20u));
    predicate.expected_value = pick(rng, 100u);
    step.preconditions.push_back(predicate);
  }
  if (!possible_predecessors.empty() && pick(rng, 2u) == 1u) {
    step.depends_on.push_back(
        possible_predecessors[static_cast<std::size_t>(pick(rng, possible_predecessors.size()))]);
  }
  EffectSpec effect;
  effect.kind = static_cast<EffectKind>(
      1u + pick(rng, static_cast<std::uint64_t>(fco::kEffectKindMax)));
  effect.asset = asset.id;
  effect.expected_state = static_cast<std::uint32_t>(pick(rng, 20u));
  effect.expected_value = pick(rng, 100u);
  step.expected_effect = effect;
  if (step.reversibility == fco::Reversibility::Compensable) {
    step.compensation = random_request(rng, asset);
  }
  step.rationale = "rationale " + std::to_string(pick(rng, 1000u));
  return step;
}

[[nodiscard]] AttemptRecord random_attempt(std::mt19937_64& rng, const PlanId& plan,
                                           const GenerationSet& generations,
                                           const std::vector<StepId>& steps) {
  AttemptRecord attempt;
  attempt.id = AttemptId("attempt-" + hex_of(rng()));
  attempt.plan = plan;
  attempt.plan_revision = PlanRevision(1u + pick(rng, 3u));
  if (!steps.empty()) attempt.step = steps[static_cast<std::size_t>(pick(rng, steps.size()))];
  attempt.ordinal = AttemptOrdinal(1u + pick(rng, 5u));
  attempt.status = static_cast<AttemptStatus>(
      1u + pick(rng, static_cast<std::uint64_t>(fco::kAttemptStatusMax)));
  attempt.intent_digest = random_digest(rng);
  attempt.idempotency_key = random_digest(rng);
  attempt.bound_generations = generations;
  attempt.bound_authority = random_authority(rng, true);
  attempt.last_observation_sequence = fco::ObservationSequence(1u + pick(rng, 100u));
  attempt.last_evidence = random_digest(rng);
  attempt.issued_at_micros = pick(rng, 1000000u);
  attempt.updated_at_micros = pick(rng, 1000000u);
  attempt.note = "note " + std::to_string(pick(rng, 100u));
  return attempt;
}

[[nodiscard]] ChangePlan random_plan(std::mt19937_64& rng, const AssetRecord& asset,
                                     const FacilitySnapshot& facility,
                                     const GenerationSet& generations) {
  ChangePlan plan;
  plan.id = PlanId("plan-" + hex_of(rng()));
  plan.request_id = ChangeRequestId("request-" + hex_of(rng()));
  plan.revision = PlanRevision(1u + pick(rng, 4u));
  plan.state =
      static_cast<PlanState>(1u + pick(rng, static_cast<std::uint64_t>(fco::kPlanStateMax)));
  plan.kind = ChangeKind("rack-replacement");
  plan.site = facility.site();
  plan.intent = "intent " + std::to_string(pick(rng, 1000u));
  plan.scope.site = facility.site();
  plan.scope.rack = asset.rack;
  plan.scope.assets = {asset.id};
  plan.facility_epoch = facility.epoch();
  plan.policy_generation = generations.policy;
  plan.bound_generations = generations;
  plan.request_digest = random_digest(rng);
  plan.evidence_digest = random_digest(rng);
  plan.facility_digest = random_digest(rng);
  plan.authority = random_authority(rng, true);

  const std::size_t step_count = 1u + static_cast<std::size_t>(pick(rng, 4u));
  std::vector<StepId> step_ids;
  for (std::size_t index = 0; index < step_count; ++index) {
    step_ids.push_back(StepId("step-" + std::to_string(index)));
  }
  for (std::size_t index = 0; index < step_count; ++index) {
    std::vector<StepId> predecessors(step_ids.begin(),
                                     step_ids.begin() + static_cast<std::ptrdiff_t>(index));
    plan.steps.push_back(
        random_step(rng, step_ids[index].value(), asset, predecessors));
    plan.step_status.insert_or_assign(
        step_ids[index],
        static_cast<StepStatus>(1u + pick(rng, static_cast<std::uint64_t>(fco::kStepStatusMax))));
  }

  const std::size_t attempt_count = static_cast<std::size_t>(pick(rng, 3u));
  for (std::size_t index = 0; index < attempt_count; ++index) {
    plan.attempts.push_back(random_attempt(rng, plan.id, generations, step_ids));
  }

  std::vector<GenerationKind> advanced_kinds;
  const std::size_t advanced_count = static_cast<std::size_t>(pick(rng, 3u));
  for (std::size_t index = 0; index < advanced_count; ++index) {
    const GenerationKind kind = static_cast<GenerationKind>(1u + pick(rng, kGenerationCount));
    if (std::find(advanced_kinds.begin(), advanced_kinds.end(), kind) != advanced_kinds.end()) {
      continue;
    }
    advanced_kinds.push_back(kind);
    GenerationDelta delta;
    delta.kind = kind;
    delta.bound = generations.get(kind);
    delta.current = delta.bound + 1u + pick(rng, 5u);
    plan.self_advanced.push_back(delta);
  }

  plan.explained_revision = FacilityRevision(1u + pick(rng, 20u));
  plan.transition_note = "transition " + std::to_string(pick(rng, 100u));
  plan.created_at_micros = pick(rng, 1000000u);
  plan.updated_at_micros = pick(rng, 1000000u);
  return plan;
}

[[nodiscard]] OrchestratorState random_state(std::mt19937_64& rng) {
  OrchestratorState state;
  const GenerationSet generations = random_generations(rng);

  std::vector<AssetRecord> records;
  const std::size_t asset_count = 1u + static_cast<std::size_t>(pick(rng, 3u));
  for (std::size_t index = 0; index < asset_count; ++index) {
    records.push_back(random_asset(rng, "asset-" + std::to_string(index)));
  }
  auto facility = FacilitySnapshot::create(SiteId("site-alpha"), generations.facility_epoch,
                                           generations, records);
  ::fco::test::check_true(facility.ok(), "random facility snapshot", __FILE__, __LINE__);
  if (facility.ok()) state.facility = facility.value();

  state.revision = Revision(1u + pick(rng, 1000u));
  state.commit_sequence = CommitSequence(1u + pick(rng, 1000u));
  state.facility_epoch = generations.facility_epoch;
  state.generations = generations;
  state.facility_revision = FacilityRevision(1u + pick(rng, 100u));
  state.incarnation = IncarnationId(1u + pick(rng, 10u));
  state.control_epoch = ControlEpoch(1u + pick(rng, 10u));
  state.actor = PrincipalId("principal-" + std::to_string(pick(rng, 3u)));
  state.policy = PolicyId("policy-" + std::to_string(pick(rng, 3u)));
  state.policy_digest = random_digest(rng);
  state.updated_at_micros = pick(rng, 10000000u);

  const AssetRecord lead = records.front();
  const std::size_t plan_count = static_cast<std::size_t>(pick(rng, 3u));
  for (std::size_t index = 0; index < plan_count; ++index) {
    ChangePlan plan = random_plan(rng, lead, state.facility, generations);
    state.plans.insert_or_assign(plan.id, plan);
  }

  const std::size_t evidence_count = static_cast<std::size_t>(pick(rng, 4u));
  for (std::size_t index = 0; index < evidence_count; ++index) {
    EvidenceRecord record;
    record.id = EvidenceId("evidence-" + std::to_string(index));
    PlanId plan_id("plan-absent");
    StepId step_id("step-0");
    AttemptId attempt_id("attempt-absent");
    if (!state.plans.empty()) {
      const ChangePlan& plan = state.plans.begin()->second;
      plan_id = plan.id;
      if (!plan.steps.empty()) step_id = plan.steps.front().id;
      if (!plan.attempts.empty()) attempt_id = plan.attempts.front().id;
    }
    record.plan = plan_id;
    record.plan_revision = PlanRevision(1u + pick(rng, 3u));
    record.step = step_id;
    record.attempt = attempt_id;
    record.source_domain =
        static_cast<DomainKind>(1u + pick(rng, static_cast<std::uint64_t>(fco::kDomainKindMax)));
    record.source_incarnation = IncarnationId(1u + pick(rng, 5u));
    record.source_control_epoch = ControlEpoch(1u + pick(rng, 5u));
    record.observation_sequence = fco::ObservationSequence(1u + pick(rng, 100u));
    record.observed_generations = generations;
    record.content_digest = random_digest(rng);
    record.outcome = static_cast<EvidenceOutcome>(1u + pick(rng, 4u));
    record.observed_at_micros = pick(rng, 1000000u);
    record.has_observed_asset = pick(rng, 2u) == 1u;
    if (record.has_observed_asset) record.observed_asset = lead;
    record.note = "evidence note " + std::to_string(index);
    state.evidence.restore(record);
  }
  state.evidence.rebuild_watermarks();
  return state;
}

[[nodiscard]] ChangePlan make_dag_plan(const std::vector<std::string>& ids,
                                       const std::vector<std::vector<std::size_t>>& dependencies,
                                       const std::vector<std::size_t>& insertion) {
  ChangePlan plan;
  plan.id = PlanId("plan-dag");
  for (const std::size_t index : insertion) {
    ChangeStep step;
    step.id = StepId(ids[index]);
    for (const std::size_t dependency : dependencies[index]) {
      step.depends_on.push_back(StepId(ids[dependency]));
    }
    plan.steps.push_back(step);
  }
  return plan;
}

class TempRoot {
 public:
  TempRoot() {
    static unsigned counter = 0;
    ++counter;
    std::ostringstream name;
    name << "fco-property-" << counter << '-'
         << std::chrono::steady_clock::now().time_since_epoch().count();
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

}  // namespace

// ---------------------------------------------------------------------------
// Topological order
// ---------------------------------------------------------------------------
FCO_TEST(property, topological_order_is_a_deterministic_permutation) {
  constexpr std::uint64_t kSeed = 0x5EED0001ull;
  constexpr std::size_t kIterations = 200;
  std::mt19937_64 rng(kSeed);

  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    const std::size_t count = 1u + static_cast<std::size_t>(pick(rng, 24u));
    std::vector<std::string> ids;
    for (std::size_t index = 0; index < count; ++index) {
      ids.push_back("step-" + std::to_string(index));
    }
    std::vector<std::vector<std::size_t>> dependencies(count);
    for (std::size_t index = 0; index < count; ++index) {
      const std::size_t wanted = static_cast<std::size_t>(pick(rng, 4u));
      for (std::size_t edge = 0; edge < wanted && index > 0; ++edge) {
        const std::size_t predecessor = static_cast<std::size_t>(pick(rng, index));
        if (std::find(dependencies[index].begin(), dependencies[index].end(), predecessor) ==
            dependencies[index].end()) {
          dependencies[index].push_back(predecessor);
        }
      }
    }

    std::vector<std::size_t> natural(count);
    for (std::size_t index = 0; index < count; ++index) natural[index] = index;
    std::vector<std::size_t> shuffled = natural;
    std::shuffle(shuffled.begin(), shuffled.end(), rng);

    const ChangePlan plan = make_dag_plan(ids, dependencies, natural);
    auto order = plan.topological_order();
    if (!order.ok()) {
      fail_property("topological_order", kSeed, iteration,
                    "an acyclic DAG was rejected: " + fco::describe(order.error()));
      continue;
    }
    if (order.value().size() != count) {
      fail_property("topological_order", kSeed, iteration, "order is not a permutation");
      continue;
    }
    std::map<std::string, std::size_t> position;
    for (std::size_t index = 0; index < order.value().size(); ++index) {
      if (!position.insert({order.value()[index].value(), index}).second) {
        fail_property("topological_order", kSeed, iteration,
                      "a step appears twice in the order");
      }
    }
    if (position.size() != count) continue;
    for (std::size_t index = 0; index < count; ++index) {
      for (const std::size_t dependency : dependencies[index]) {
        if (position[ids[dependency]] >= position[ids[index]]) {
          fail_property("topological_order", kSeed, iteration,
                        "edge " + ids[dependency] + " -> " + ids[index] + " is inverted");
        }
      }
    }

    const ChangePlan reordered = make_dag_plan(ids, dependencies, shuffled);
    auto other = reordered.topological_order();
    if (!other.ok() || other.value() != order.value()) {
      fail_property("topological_order", kSeed, iteration,
                    "a different insertion order produced a different order");
    }

    // A back edge is always a cycle, never a silent reordering.
    ChangePlan cyclic = plan;
    if (dependencies.empty() || count < 2) {
      continue;
    }
    bool introduced = false;
    for (std::size_t index = 0; index < count && !introduced; ++index) {
      if (dependencies[index].empty()) continue;
      const std::size_t dependency = dependencies[index].front();
      cyclic.steps[dependency].depends_on.push_back(cyclic.steps[index].id);
      introduced = true;
    }
    if (!introduced) {
      cyclic.steps[1].depends_on.push_back(cyclic.steps[0].id);
      cyclic.steps[0].depends_on.push_back(cyclic.steps[1].id);
    }
    auto cyclic_order = cyclic.topological_order();
    if (cyclic_order.ok()) {
      fail_property("topological_order", kSeed, iteration,
                    "a graph with a back edge produced an order instead of PlanCycle");
    } else if (cyclic_order.error().code != ErrorCode::PlanCycle) {
      fail_property("topological_order", kSeed, iteration,
                    "a back edge produced " + std::string(fco::to_string(cyclic_order.error().code)) +
                        " instead of PlanCycle");
    }
  }
}

// ---------------------------------------------------------------------------
// Plan digest
// ---------------------------------------------------------------------------
FCO_TEST(property, plan_digest_equals_equality_of_hashed_fields) {
  constexpr std::uint64_t kSeed = 0x5EED0002ull;
  constexpr std::size_t kIterations = 200;
  std::mt19937_64 rng(kSeed);

  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    ChangePlan base = random_plan(rng, random_asset(rng, "asset-0"), FacilitySnapshot{},
                                  random_generations(rng));
    // random_plan needs a facility only for its site and epoch; an empty
    // snapshot is fine because the digest does not consult it.
    base.site = SiteId("site-alpha");
    base.scope.site = SiteId("site-alpha");
    base.facility_epoch = FacilityEpoch(4);
    base.scope.rack = fco::RackId("rack-0");

    const Digest base_digest = base.digest();
    const ChangePlan untouched = base;
    if (untouched.digest() != base_digest) {
      fail_property("plan_digest", kSeed, iteration,
                    "two plans with equal fields have different digests");
    }

    std::vector<ChangePlan> mutations;
    ChangePlan mutated = base;
    mutated.state =
        static_cast<PlanState>(1u + (static_cast<std::uint8_t>(base.state) %
                                     static_cast<std::uint8_t>(fco::kPlanStateMax)));
    mutations.push_back(mutated);

    mutated = base;
    mutated.revision = PlanRevision(base.revision.value() + 1u);
    mutations.push_back(mutated);

    mutated = base;
    if (!mutated.step_status.empty()) {
      auto entry = mutated.step_status.begin();
      const StepStatus replacement =
          entry->second == StepStatus::Pending ? StepStatus::Verified : StepStatus::Pending;
      entry->second = replacement;
      mutations.push_back(mutated);
    }

    mutated = base;
    if (!mutated.attempts.empty()) {
      AttemptStatus replacement = AttemptStatus::Verified;
      if (mutated.attempts.front().status == AttemptStatus::Verified) {
        replacement = AttemptStatus::Failed;
      }
      mutated.attempts.front().status = replacement;
      mutations.push_back(mutated);
    }

    mutated = base;
    {
      GenerationDelta delta;
      delta.kind = GenerationKind::Dfi;
      delta.bound = 1u;
      delta.current = 2u;
      mutated.self_advanced.push_back(delta);
      mutations.push_back(mutated);
    }

    mutated = base;
    mutated.explained_revision = FacilityRevision(base.explained_revision.value() + 1u);
    mutations.push_back(mutated);

    mutated = base;
    mutated.intent += " (changed)";
    mutations.push_back(mutated);

    for (const ChangePlan& candidate : mutations) {
      if (candidate.digest() == base_digest) {
        fail_property("plan_digest", kSeed, iteration,
                      "mutating a hashed field did not change the digest");
      }
    }

    // Fields the canonical hash deliberately excludes: a transition note and
    // the timestamps are provenance, not identity.
    ChangePlan unhashed = base;
    unhashed.transition_note += " (different note)";
    unhashed.created_at_micros += 1u;
    unhashed.updated_at_micros += 1u;
    if (unhashed.digest() != base_digest) {
      fail_property("plan_digest", kSeed, iteration,
                    "a field outside the canonical hash changed the digest");
    }
  }
}

// ---------------------------------------------------------------------------
// Generation currency
// ---------------------------------------------------------------------------
FCO_TEST(property, currency_outcome_matches_the_comparison) {
  constexpr std::uint64_t kSeed = 0x5EED0003ull;
  constexpr std::size_t kIterations = 500;
  std::mt19937_64 rng(kSeed);

  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    GenerationSet bound;
    GenerationSet observed;
    std::vector<GenerationKind> expected_advanced;
    std::vector<GenerationKind> expected_regressed;
    for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
      const GenerationKind kind = static_cast<GenerationKind>(raw);
      const std::uint64_t value = 1u + pick(rng, 1000u);
      bound.set(kind, value);
      switch (pick(rng, 3u)) {
        case 0u:
          observed.set(kind, value);
          break;
        case 1u:
          observed.set(kind, value + 1u + pick(rng, 5u));
          expected_advanced.push_back(kind);
          break;
        default:
          observed.set(kind, value - 1u);
          expected_regressed.push_back(kind);
          break;
      }
    }

    const CurrencyCheck check = fco::check_currency(bound, observed);
    CurrencyOutcome expected = CurrencyOutcome::Current;
    ErrorCode expected_code = ErrorCode::Ok;
    if (!expected_advanced.empty() && !expected_regressed.empty()) {
      expected = CurrencyOutcome::Mixed;
      expected_code = ErrorCode::StaleGeneration;
    } else if (!expected_advanced.empty()) {
      expected = CurrencyOutcome::Advanced;
      expected_code = ErrorCode::StaleGeneration;
    } else if (!expected_regressed.empty()) {
      expected = CurrencyOutcome::Regressed;
      expected_code = ErrorCode::GenerationRegression;
    }
    if (check.outcome != expected) {
      fail_property("currency", kSeed, iteration,
                    "expected outcome " +
                        std::to_string(static_cast<int>(expected)) + " but got " +
                        std::to_string(static_cast<int>(check.outcome)));
    }
    if (check.primary_code() != expected_code) {
      fail_property("currency", kSeed, iteration,
                    "primary code " + std::string(fco::to_string(check.primary_code())) +
                        " does not match the documented mapping");
    }
    if (check.current() != (expected == CurrencyOutcome::Current)) {
      fail_property("currency", kSeed, iteration, "current() disagrees with the outcome");
    }
    if (check.advanced.size() != expected_advanced.size() ||
        check.regressed.size() != expected_regressed.size()) {
      fail_property("currency", kSeed, iteration,
                    "advanced/regressed vectors do not match the moved generations");
      continue;
    }
    for (std::size_t index = 0; index < expected_advanced.size(); ++index) {
      const GenerationDelta& delta = check.advanced[index];
      if (delta.kind != expected_advanced[index] ||
          delta.bound != bound.get(delta.kind) || delta.current != observed.get(delta.kind) ||
          delta.current <= delta.bound) {
        fail_property("currency", kSeed, iteration, "an advanced delta is not consistent");
      }
    }
    for (std::size_t index = 0; index < expected_regressed.size(); ++index) {
      const GenerationDelta& delta = check.regressed[index];
      if (delta.kind != expected_regressed[index] ||
          delta.bound != bound.get(delta.kind) || delta.current != observed.get(delta.kind) ||
          delta.current >= delta.bound) {
        fail_property("currency", kSeed, iteration, "a regressed delta is not consistent");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Store round-trip
// ---------------------------------------------------------------------------
FCO_TEST(property, store_round_trips_random_valid_states) {
  constexpr std::uint64_t kSeed = 0x5EED0004ull;
  constexpr std::size_t kIterations = 40;
  std::mt19937_64 rng(kSeed);

  TempRoot temp;
  Digest last_digest;
  bool have_last = false;
  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    auto opened = DurableStore::open(temp.path());
    if (!opened.ok()) {
      fail_property("store_round_trip", kSeed, iteration,
                    "open failed: " + fco::describe(opened.error()));
      return;
    }
    DurableStore store = opened.take();

    OrchestratorState state = random_state(rng);
    state.revision = Revision(iteration + 1u);
    state.commit_sequence = CommitSequence(iteration + 1u);
    const Digest expected = state.digest();
    auto committed = store.commit(state);
    if (!committed.ok()) {
      fail_property("store_round_trip", kSeed, iteration,
                    "commit failed: " + fco::describe(committed.error()));
      continue;
    }
    store.close();

    // A fresh DurableStore must load exactly the state that was committed.
    auto reopened = DurableStore::open(temp.path());
    if (!reopened.ok()) {
      fail_property("store_round_trip", kSeed, iteration,
                    "reopen failed: " + fco::describe(reopened.error()));
      continue;
    }
    auto loaded = reopened.value().load();
    if (!loaded.ok()) {
      fail_property("store_round_trip", kSeed, iteration,
                    "load failed: " + fco::describe(loaded.error()));
      continue;
    }
    if (loaded.value().digest() != expected) {
      fail_property("store_round_trip", kSeed, iteration,
                    "the loaded state digest differs from the committed digest");
    }
    if (reopened.value().generation() != iteration + 1u) {
      fail_property("store_round_trip", kSeed, iteration,
                    "the store generation did not advance by exactly one");
    }
    if (reopened.value().revision() != state.revision ||
        reopened.value().commit_sequence() != state.commit_sequence) {
      fail_property("store_round_trip", kSeed, iteration,
                    "the store accessors disagree with the committed counters");
    }
    last_digest = expected;
    have_last = true;
    reopened.value().close();
  }
  if (have_last) {
    auto final_open = DurableStore::open(temp.path());
    FCO_REQUIRE(final_open.ok());
    auto final_load = final_open.value().load();
    FCO_REQUIRE(final_load.ok());
    FCO_CHECK_EQ(final_load.value().digest(), last_digest);
  }
}

// ---------------------------------------------------------------------------
// Codec canonical round-trip
// ---------------------------------------------------------------------------
FCO_TEST(property, codec_round_trips_random_valid_states_canonically) {
  constexpr std::uint64_t kSeed = 0x5EED0005ull;
  constexpr std::size_t kIterations = 200;
  std::mt19937_64 rng(kSeed);

  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    const OrchestratorState state = random_state(rng);
    auto first = fco::encode_state_bytes(state);
    if (!first.ok()) {
      fail_property("codec_round_trip", kSeed, iteration,
                    "encode failed: " + fco::describe(first.error()));
      continue;
    }
    auto decoded = fco::decode_state(first.value().data(), first.value().size());
    if (!decoded.ok()) {
      fail_property("codec_round_trip", kSeed, iteration,
                    "decode failed: " + fco::describe(decoded.error()));
      continue;
    }
    auto second = fco::encode_state_bytes(decoded.value());
    if (!second.ok()) {
      fail_property("codec_round_trip", kSeed, iteration,
                    "re-encode failed: " + fco::describe(second.error()));
      continue;
    }
    if (second.value() != first.value()) {
      fail_property("codec_round_trip", kSeed, iteration,
                    "encode(decode(bytes)) is not byte-identical to bytes");
    }
    if (decoded.value().digest() != state.digest()) {
      fail_property("codec_round_trip", kSeed, iteration,
                    "the decoded state digest differs from the encoded state digest");
    }
  }
}

// ---------------------------------------------------------------------------
// Fencing
// ---------------------------------------------------------------------------
FCO_TEST(property, fence_authority_permits_exactly_the_matching_context) {
  constexpr std::uint64_t kSeed = 0x5EED0006ull;
  constexpr std::size_t kIterations = 300;
  std::mt19937_64 rng(kSeed);

  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    const bool bound_complete = pick(rng, 4u) != 0u;
    const bool current_complete = pick(rng, 4u) != 0u;
    const AuthorityContext bound = random_authority(rng, bound_complete);
    AuthorityContext current = random_authority(rng, current_complete);
    // Half the time the current context is derived from the bound one, so that
    // single-condition mismatches are exercised rather than random noise.
    if (pick(rng, 2u) == 0u) {
      current = bound;
      switch (pick(rng, 6u)) {
        case 0u:
          current.incarnation = IncarnationId(bound.incarnation.value() + 1u);
          break;
        case 1u:
          current.control_epoch = ControlEpoch(bound.control_epoch.value() + 1u);
          break;
        case 2u:
          if (bound.control_epoch.value() > 1u) {
            current.control_epoch = ControlEpoch(bound.control_epoch.value() - 1u);
          }
          break;
        case 3u:
          current.actor = PrincipalId("principal-other");
          break;
        case 4u:
          current.policy = PolicyId("policy-other");
          break;
        default:
          current.policy_digest = random_digest(rng);  // not part of the comparison
          break;
      }
      if (!current_complete) current.actor = PrincipalId{};
    }

    const FencingDecision decision = fco::fence_authority(bound, current);
    const bool both_complete = bound.complete() && current.complete();
    const bool conditions_hold =
        bound.incarnation == current.incarnation &&
        bound.control_epoch == current.control_epoch && bound.actor == current.actor &&
        bound.policy == current.policy;
    const bool should_permit = both_complete && conditions_hold;
    if (decision.permitted != should_permit) {
      fail_property("fence_authority", kSeed, iteration,
                    std::string("permitted=") + (decision.permitted ? "true" : "false") +
                        " but the four bound fields " +
                        (conditions_hold ? "match" : "differ"));
      continue;
    }
    if (decision.permitted) {
      if (decision.code != ErrorCode::Ok) {
        fail_property("fence_authority", kSeed, iteration,
                      "a permitted decision did not carry Ok");
      }
      continue;
    }

    ErrorCode expected = ErrorCode::MissingAuthority;
    if (both_complete) {
      std::vector<ErrorCode> held;
      if (bound.incarnation != current.incarnation) held.push_back(ErrorCode::FencedAuthority);
      if (current.control_epoch < bound.control_epoch) {
        held.push_back(ErrorCode::StaleAuthority);
      } else if (current.control_epoch > bound.control_epoch) {
        held.push_back(ErrorCode::FencedAuthority);
      }
      if (bound.actor != current.actor) held.push_back(ErrorCode::AuthorityMismatch);
      if (bound.policy != current.policy) held.push_back(ErrorCode::PolicyViolation);
      expected = held.front();
      for (const ErrorCode code : held) {
        if (fco::precedence(code) < fco::precedence(expected)) expected = code;
      }
    }
    if (decision.code != expected) {
      fail_property("fence_authority", kSeed, iteration,
                    "expected " + std::string(fco::to_string(expected)) + " but got " +
                        std::string(fco::to_string(decision.code)));
    }
  }
}

FCO_TEST_MAIN
