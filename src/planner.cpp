#include "fco/planner.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>

namespace fco {
namespace {

[[nodiscard]] Predicate predecessor_verified(const StepId& predecessor) {
  Predicate predicate;
  predicate.kind = PredicateKind::PredecessorVerified;
  predicate.predecessor = predecessor;
  return predicate;
}

[[nodiscard]] Predicate state_is(PredicateKind kind, const AssetId& asset, std::uint32_t state) {
  Predicate predicate;
  predicate.kind = kind;
  predicate.asset = asset;
  predicate.expected_state = state;
  return predicate;
}

template <class Enum>
[[nodiscard]] std::uint32_t code(Enum value) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(value));
}

[[nodiscard]] ActionRequest base_request(const AssetRecord& record, ActionKind action) {
  ActionRequest request;
  request.kind = action;
  request.owner = owning_domain(action);
  request.asset = record.id;
  request.rack = record.rack;
  request.site = record.site;
  request.hardware_generation = record.hardware_generation;
  request.firmware_generation = record.firmware_generation;
  request.lifecycle_generation = record.lifecycle_generation;
  return request;
}

[[nodiscard]] ChangeStep make_step(StepId id, SafetyClass safety, Reversibility reversibility,
                                   VerificationMode verification, bool point_of_no_return,
                                   ActionRequest request, EffectSpec effect,
                                   std::vector<Predicate> preconditions,
                                   std::vector<StepId> depends_on,
                                   std::optional<ActionRequest> compensation,
                                   std::string rationale) {
  ChangeStep step;
  step.owner = request.owner;
  step.action = request.kind;
  step.safety = safety;
  step.reversibility = reversibility;
  step.verification = verification;
  step.point_of_no_return = point_of_no_return;
  step.request = std::move(request);
  step.expected_effect = effect;
  step.preconditions = std::move(preconditions);
  step.depends_on = std::move(depends_on);
  step.compensation = std::move(compensation);
  step.rationale = std::move(rationale);
  step.id = std::move(id);
  return step;
}

struct StepIds {
  StepId drain;
  StepId isolate;
  StepId power_off;
  StepId cooling_off;
  StepId network_off;
  StepId swap;
  StepId power_on;
  StepId cooling_on;
  StepId network_on;
  StepId commission;
  StepId maintenance_off;
  StepId verify;
  StepId restore;
};

[[nodiscard]] Result<StepIds> derive_step_ids(const AssetId& asset) {
  StepIds ids;
  const std::string suffix = "-" + asset.value();
  const std::pair<StepId*, const char*> table[] = {
      {&ids.drain, "drain"},          {&ids.isolate, "isolate"},
      {&ids.power_off, "power-off"},  {&ids.cooling_off, "cooling-off"},
      {&ids.network_off, "net-off"},  {&ids.swap, "swap"},
      {&ids.power_on, "power-on"},    {&ids.cooling_on, "cooling-on"},
      {&ids.network_on, "net-on"},    {&ids.commission, "commission"},
      {&ids.maintenance_off, "maint-off"},
      {&ids.verify, "verify"},        {&ids.restore, "restore"},
  };
  for (const auto& entry : table) {
    const std::string id = std::string(entry.second) + suffix;
    if (!is_valid_identifier(id)) {
      return failure<StepIds>(
          ErrorCode::BoundedLimitExceeded, asset.to_string(),
          "derived step identifier '" + id + "' is not a valid identifier of at most 64 bytes");
    }
    *entry.first = StepId(id);
  }
  return success(std::move(ids));
}

// Builds the predecessor preconditions for a chain-supplied predecessor set, so
// that a step never carries a precondition or an edge that references a step the
// chain does not contain.
[[nodiscard]] std::vector<Predicate> after_all(const std::vector<StepId>& predecessors) {
  std::vector<Predicate> predicates;
  predicates.reserve(predecessors.size());
  for (const StepId& predecessor : predecessors) {
    predicates.push_back(predecessor_verified(predecessor));
  }
  return predicates;
}

[[nodiscard]] ChangeStep step_drain(const AssetRecord& record, const StepIds& ids) {
  ActionRequest request = base_request(record, ActionKind::DrainWorkloads);
  ActionRequest compensation = base_request(record, ActionKind::RestoreWorkloads);
  EffectSpec effect;
  effect.kind = EffectKind::WorkloadStateChange;
  effect.asset = record.id;
  effect.expected_state = code(WorkloadState::Drained);
  return make_step(ids.drain, SafetyClass::Standard, Reversibility::Compensable,
                   VerificationMode::Required, false, request, effect, {},
                   {}, compensation,
                   "workloads must leave the asset before any physical work begins");
}

[[nodiscard]] ChangeStep step_isolate(const AssetRecord& record, const StepIds& ids,
                                      const std::vector<StepId>& predecessors) {
  ActionRequest request = base_request(record, ActionKind::MaintenanceIsolate);
  EffectSpec effect;
  effect.kind = EffectKind::MaintenanceStateChange;
  effect.asset = record.id;
  effect.expected_state = code(MaintenanceState::Isolated);
  std::vector<Predicate> preconditions = after_all(predecessors);
  preconditions.push_back(state_is(PredicateKind::MaintenanceStateIs, record.id,
                                   code(MaintenanceState::None)));
  return make_step(ids.isolate, SafetyClass::Restricted, Reversibility::Reversible,
                   VerificationMode::Required, false, request, effect, std::move(preconditions),
                   predecessors, std::nullopt,
                   "the maintenance authority owns isolation; the orchestrator does not");
}

[[nodiscard]] ChangeStep step_power_off(const AssetRecord& record, const StepIds& ids) {
  ActionRequest request = base_request(record, ActionKind::PowerDownAsset);
  EffectSpec effect;
  effect.kind = EffectKind::PowerStateChange;
  effect.asset = record.id;
  effect.expected_state = code(PowerState::Off);
  return make_step(ids.power_off, SafetyClass::Restricted, Reversibility::Reversible,
                   VerificationMode::Required, false, request, effect,
                   {predecessor_verified(ids.isolate),
                    state_is(PredicateKind::PowerStateIs, record.id, code(PowerState::On))},
                   {ids.isolate}, std::nullopt, "power down is requested from the power authority");
}

[[nodiscard]] ChangeStep step_cooling_off(const AssetRecord& record, const StepIds& ids) {
  ActionRequest request = base_request(record, ActionKind::CoolingIsolate);
  EffectSpec effect;
  effect.kind = EffectKind::CoolingStateChange;
  effect.asset = record.id;
  effect.expected_state = code(CoolingState::Isolated);
  return make_step(ids.cooling_off, SafetyClass::Restricted, Reversibility::Reversible,
                   VerificationMode::Required, false, request, effect,
                   {predecessor_verified(ids.power_off)}, {ids.power_off}, std::nullopt,
                   "loop isolation must be observed before hardware is touched");
}

[[nodiscard]] ChangeStep step_network_off(const AssetRecord& record, const StepIds& ids) {
  ActionRequest request = base_request(record, ActionKind::NetworkQuiesce);
  EffectSpec effect;
  effect.kind = EffectKind::NetworkStateChange;
  effect.asset = record.id;
  effect.expected_state = code(NetworkState::Quiesced);
  return make_step(ids.network_off, SafetyClass::Restricted, Reversibility::Reversible,
                   VerificationMode::Required, false, request, effect,
                   {predecessor_verified(ids.power_off)}, {ids.power_off}, std::nullopt,
                   "fabric authority quiesces the attachment before the swap");
}

[[nodiscard]] ChangeStep step_swap(const AssetRecord& record, const StepIds& ids) {
  ActionRequest request = base_request(record, ActionKind::HardwareSwap);
  EffectSpec effect;
  effect.kind = EffectKind::HardwareGenerationChange;
  effect.asset = record.id;
  effect.expected_value = record.hardware_generation.value() + 1ull;
  return make_step(ids.swap, SafetyClass::Critical, Reversibility::NonReversible,
                   VerificationMode::Required, true, request, effect,
                   {predecessor_verified(ids.cooling_off),
                    predecessor_verified(ids.network_off),
                    state_is(PredicateKind::PowerStateIs, record.id, code(PowerState::Off)),
                    state_is(PredicateKind::CoolingStateIs, record.id, code(CoolingState::Isolated)),
                    state_is(PredicateKind::NetworkStateIs, record.id, code(NetworkState::Quiesced))},
                   {ids.cooling_off, ids.network_off}, std::nullopt,
                   "physical replacement is a point of no return and is never fabricated as "
                   "rollback-able");
}

[[nodiscard]] ChangeStep step_power_on(const AssetRecord& record, const StepIds& ids,
                                      const std::vector<StepId>& predecessors) {
  ActionRequest request = base_request(record, ActionKind::PowerUpAsset);
  EffectSpec effect;
  effect.kind = EffectKind::PowerStateChange;
  effect.asset = record.id;
  effect.expected_state = code(PowerState::On);
  std::vector<Predicate> preconditions = after_all(predecessors);
  preconditions.push_back(
      state_is(PredicateKind::PowerStateIs, record.id, code(PowerState::Off)));
  return make_step(ids.power_on, SafetyClass::Restricted, Reversibility::Reversible,
                   VerificationMode::Required, false, request, effect, std::move(preconditions),
                   predecessors, std::nullopt,
                   "power the asset up once the preceding work has been verified");
}

[[nodiscard]] ChangeStep step_cooling_on(const AssetRecord& record, const StepIds& ids) {
  ActionRequest request = base_request(record, ActionKind::CoolingRestore);
  EffectSpec effect;
  effect.kind = EffectKind::CoolingStateChange;
  effect.asset = record.id;
  effect.expected_state = code(CoolingState::Normal);
  return make_step(ids.cooling_on, SafetyClass::Restricted, Reversibility::Reversible,
                   VerificationMode::Required, false, request, effect,
                   {predecessor_verified(ids.power_on)}, {ids.power_on}, std::nullopt,
                   "restore cooling to the normal operating point");
}

[[nodiscard]] ChangeStep step_network_on(const AssetRecord& record, const StepIds& ids) {
  ActionRequest request = base_request(record, ActionKind::NetworkRestore);
  EffectSpec effect;
  effect.kind = EffectKind::NetworkStateChange;
  effect.asset = record.id;
  effect.expected_state = code(NetworkState::Attached);
  return make_step(ids.network_on, SafetyClass::Restricted, Reversibility::Reversible,
                   VerificationMode::Required, false, request, effect,
                   {predecessor_verified(ids.swap)}, {ids.swap}, std::nullopt,
                   "reattach the fabric path for the replacement hardware");
}

// The recommission step is shared by several chains, so its predecessor set is
// supplied by the chain. A precondition may only reference a step that exists in
// the same request; the chains that omit cooling or fabric work pass a smaller
// predecessor set rather than a dangling reference.
[[nodiscard]] ChangeStep step_commission(const AssetRecord& record, const StepIds& ids,
                                         const std::vector<StepId>& predecessors) {
  ActionRequest request = base_request(record, ActionKind::Recommission);
  EffectSpec effect;
  effect.kind = EffectKind::LifecycleStateChange;
  effect.asset = record.id;
  effect.expected_state = code(AssetLifecycle::Active);
  std::vector<Predicate> preconditions;
  for (const StepId& predecessor : predecessors) {
    preconditions.push_back(predecessor_verified(predecessor));
  }
  preconditions.push_back(
      state_is(PredicateKind::PowerStateIs, record.id, code(PowerState::On)));
  return make_step(ids.commission, SafetyClass::Restricted, Reversibility::NonReversible,
                   VerificationMode::Required, false, request, effect, std::move(preconditions),
                   predecessors, std::nullopt,
                   "the lifecycle authority returns the asset to the active state");
}

[[nodiscard]] ChangeStep step_maintenance_off(const AssetRecord& record, const StepIds& ids) {
  ActionRequest request = base_request(record, ActionKind::MaintenanceRelease);
  EffectSpec effect;
  effect.kind = EffectKind::MaintenanceStateChange;
  effect.asset = record.id;
  effect.expected_state = code(MaintenanceState::None);
  return make_step(ids.maintenance_off, SafetyClass::Restricted, Reversibility::Reversible,
                   VerificationMode::Required, false, request, effect,
                   {predecessor_verified(ids.commission),
                    state_is(PredicateKind::MaintenanceStateIs, record.id,
                             code(MaintenanceState::Isolated))},
                   {ids.commission}, std::nullopt, "release maintenance isolation");
}

[[nodiscard]] ChangeStep step_verify(const AssetRecord& record, const StepIds& ids) {
  ActionRequest request = base_request(record, ActionKind::VerifyRestoration);
  EffectSpec effect;
  effect.kind = EffectKind::RestorationVerified;
  effect.asset = record.id;
  return make_step(ids.verify, SafetyClass::Standard, Reversibility::Reversible,
                   VerificationMode::Required, false, request, effect,
                   {predecessor_verified(ids.maintenance_off)}, {ids.maintenance_off},
                   std::nullopt,
                   "restoration is verified from observed state, not from acknowledgement");
}

[[nodiscard]] ChangeStep step_restore(const AssetRecord& record, const StepIds& ids) {
  ActionRequest request = base_request(record, ActionKind::RestoreWorkloads);
  ActionRequest compensation = base_request(record, ActionKind::DrainWorkloads);
  EffectSpec effect;
  effect.kind = EffectKind::WorkloadStateChange;
  effect.asset = record.id;
  effect.expected_state = code(WorkloadState::Present);
  return make_step(ids.restore, SafetyClass::Standard, Reversibility::Compensable,
                   VerificationMode::Required, false, request, effect,
                   {predecessor_verified(ids.verify)}, {ids.verify}, compensation,
                   "workloads return only after restoration has been verified");
}

void append_full_chain(std::vector<ChangeStep>& steps, const AssetRecord& record) {
  const auto ids = derive_step_ids(record.id);
  if (!ids.ok()) return;
  const StepIds& value = ids.value();
  steps.push_back(step_drain(record, value));
  steps.push_back(step_isolate(record, value, {value.drain}));
  steps.push_back(step_power_off(record, value));
  steps.push_back(step_cooling_off(record, value));
  steps.push_back(step_network_off(record, value));
  steps.push_back(step_swap(record, value));
  steps.push_back(step_power_on(record, value, {value.swap}));
  steps.push_back(step_cooling_on(record, value));
  steps.push_back(step_network_on(record, value));
  steps.push_back(step_commission(record, value, {value.cooling_on, value.network_on}));
  steps.push_back(step_maintenance_off(record, value));
  steps.push_back(step_verify(record, value));
  steps.push_back(step_restore(record, value));
}

void append_decommission_chain(std::vector<ChangeStep>& steps, const AssetRecord& record) {
  const auto ids = derive_step_ids(record.id);
  if (!ids.ok()) return;
  const StepIds& value = ids.value();
  steps.push_back(step_drain(record, value));
  steps.push_back(step_isolate(record, value, {value.drain}));
  steps.push_back(step_power_off(record, value));
  steps.push_back(step_network_off(record, value));
  ActionRequest request = base_request(record, ActionKind::Decommission);
  EffectSpec effect;
  effect.kind = EffectKind::LifecycleStateChange;
  effect.asset = record.id;
  effect.expected_state = code(AssetLifecycle::Decommissioned);
  steps.push_back(make_step(value.commission, SafetyClass::Critical,
                            Reversibility::NonReversible, VerificationMode::Required, true, request,
                            effect,
                            {predecessor_verified(value.power_off),
                             predecessor_verified(value.network_off),
                             state_is(PredicateKind::PowerStateIs, record.id,
                                      code(PowerState::Off))},
                            {value.power_off, value.network_off}, std::nullopt,
                            "decommissioning ends the asset's service life; it is not reversible"));
}

void append_firmware_chain(std::vector<ChangeStep>& steps, const AssetRecord& record) {
  const auto ids = derive_step_ids(record.id);
  if (!ids.ok()) return;
  const StepIds& value = ids.value();
  steps.push_back(step_drain(record, value));
  steps.push_back(step_isolate(record, value, {value.drain}));
  steps.push_back(step_power_off(record, value));
  ActionRequest request = base_request(record, ActionKind::FirmwareStage);
  EffectSpec effect;
  effect.kind = EffectKind::FirmwareGenerationChange;
  effect.asset = record.id;
  effect.expected_value = record.firmware_generation.value() + 1ull;
  steps.push_back(make_step(value.swap, SafetyClass::Restricted, Reversibility::NonReversible,
                            VerificationMode::Required, false, request, effect,
                            {predecessor_verified(value.power_off),
                             state_is(PredicateKind::PowerStateIs, record.id,
                                      code(PowerState::Off))},
                            {value.power_off}, std::nullopt,
                            "firmware staging is owned by the lifecycle authority"));
  steps.push_back(step_power_on(record, value, {value.swap}));
  steps.push_back(step_commission(record, value, {value.power_on}));
  steps.push_back(step_maintenance_off(record, value));
  steps.push_back(step_verify(record, value));
  steps.push_back(step_restore(record, value));
}

void append_power_maintenance_chain(std::vector<ChangeStep>& steps, const AssetRecord& record) {
  const auto ids = derive_step_ids(record.id);
  if (!ids.ok()) return;
  const StepIds& value = ids.value();
  steps.push_back(step_isolate(record, value, {}));
  steps.push_back(step_power_off(record, value));
  steps.push_back(step_power_on(record, value, {value.power_off}));
  steps.push_back(step_commission(record, value, {value.power_on}));
  steps.push_back(step_maintenance_off(record, value));
  steps.push_back(step_verify(record, value));
}

}  // namespace

bool is_known_change_kind(std::string_view kind) noexcept {
  return kind == "rack-replacement" || kind == "rack-decommission" || kind == "firmware-upgrade" ||
         kind == "power-maintenance";
}

std::vector<std::string> known_change_kinds() {
  return {"rack-replacement", "rack-decommission", "firmware-upgrade", "power-maintenance"};
}

Result<ChangeRequest> synthesize_request(const SynthesisInput& input,
                                         const FacilitySnapshot& snapshot) {
  if (!is_known_change_kind(input.kind.value())) {
    return failure<ChangeRequest>(ErrorCode::InvalidEnumValue, input.kind.to_string(),
                                  "unknown change kind template");
  }
  if (input.scope.assets.empty()) {
    return failure<ChangeRequest>(ErrorCode::MalformedInput, "scope",
                                  "a synthesised request requires at least one asset in scope");
  }

  for (const AssetId& asset : input.scope.assets) {
    if (!asset.valid()) {
      return failure<ChangeRequest>(ErrorCode::InvalidIdentity, "scope",
                                    "request scope contains an unset asset identifier");
    }
    const auto ids = derive_step_ids(asset);
    if (!ids.ok()) return ids.error();
    const AssetRecord* record = snapshot.find(asset);
    if (record == nullptr) {
      return failure<ChangeRequest>(ErrorCode::UnknownIdentity, asset.to_string(),
                                    "asset is not present in the facility snapshot");
    }
    // The hardware and firmware chains express their expected effect as the next
    // generation number, so a saturated generation cannot be planned against.
    constexpr std::uint64_t kGenerationCeiling = (std::numeric_limits<std::uint64_t>::max)();
    if (record->hardware_generation.value() == kGenerationCeiling ||
        record->firmware_generation.value() == kGenerationCeiling) {
      return failure<ChangeRequest>(
          ErrorCode::BoundedLimitExceeded, asset.to_string(),
          "the asset's hardware or firmware generation is exhausted, so the expected "
          "post-change generation cannot be expressed");
    }
  }

  std::vector<AssetId> assets = input.scope.assets;
  std::sort(assets.begin(), assets.end());
  assets.erase(std::unique(assets.begin(), assets.end()), assets.end());
  if (assets.size() != input.scope.assets.size()) {
    return failure<ChangeRequest>(ErrorCode::DuplicateIdentity, "scope",
                                  "request scope lists the same asset more than once");
  }

  ChangeRequest request;
  request.id = input.id;
  request.kind = input.kind;
  request.site = input.site;
  request.intent = input.intent;
  request.scope = input.scope;
  request.scope.assets = assets;
  request.facility_epoch = input.facility_epoch;
  request.actor = input.actor;
  request.revision = input.revision;
  request.bound_generations = input.bound_generations;
  request.evidence_digest = input.evidence_digest;

  const std::string kind = input.kind.value();
  for (const AssetId& asset : assets) {
    const AssetRecord* record = snapshot.find(asset);
    if (record == nullptr) {
      return failure<ChangeRequest>(ErrorCode::UnknownIdentity, asset.to_string(),
                                    "asset disappeared from the snapshot during synthesis");
    }
    if (kind == "rack-replacement") {
      append_full_chain(request.steps, *record);
    } else if (kind == "rack-decommission") {
      append_decommission_chain(request.steps, *record);
    } else if (kind == "firmware-upgrade") {
      append_firmware_chain(request.steps, *record);
    } else {
      append_power_maintenance_chain(request.steps, *record);
    }
  }

  if (request.steps.empty()) {
    return failure<ChangeRequest>(ErrorCode::EmptyPlan, "steps",
                                  "synthesis produced no steps");
  }
  if (request.steps.size() > static_cast<std::size_t>(kMaxPlanSteps)) {
    return failure<ChangeRequest>(ErrorCode::BoundedLimitExceeded, "steps",
                                  "synthesised request exceeds the supported step bound");
  }

  Diagnostics diagnostics;
  validate_request(request, diagnostics);
  if (!diagnostics.empty()) {
    return Result<ChangeRequest>(diagnostics.primary());
  }
  return success(std::move(request));
}

Result<ChangePlan> create_plan(const ChangeRequest& request, const FacilitySnapshot& snapshot,
                               const AuthorityContext& authority, PlanRevision revision,
                               FacilityRevision facility_revision,
                               std::uint64_t now_micros) {
  Diagnostics diagnostics;
  validate_request(request, diagnostics);
  if (!diagnostics.empty()) {
    return Result<ChangePlan>(diagnostics.primary());
  }
  if (!snapshot.valid()) {
    return failure<ChangePlan>(ErrorCode::StorageFailure, "facility",
                               "facility snapshot is not usable");
  }
  if (!authority.complete()) {
    return failure<ChangePlan>(ErrorCode::MissingAuthority, "authority",
                               "plan creation requires a complete authority context");
  }
  if (!revision.valid()) {
    return failure<ChangePlan>(ErrorCode::InvalidIdentity, "revision",
                               "plan revision must be at least 1");
  }
  if (request.site != snapshot.site()) {
    return failure<ChangePlan>(ErrorCode::ImpossibleCombination, request.site.to_string(),
                               "request site does not match the observed facility site");
  }
  if (request.facility_epoch != snapshot.epoch()) {
    return failure<ChangePlan>(
        ErrorCode::StaleGeneration, request.facility_epoch.to_string(),
        "request is bound to facility epoch " + request.facility_epoch.to_string() +
            " but the observed epoch is " + snapshot.epoch().to_string());
  }
  const CurrencyCheck currency =
      check_currency(request.bound_generations, snapshot.generations());
  if (!currency.current()) {
    return failure<ChangePlan>(currency.primary_code(), currency.subject_key(),
                               "request generation binding is not current: " +
                                   currency.describe());
  }

  ChangePlan plan;
  plan.id = derive_plan_id(request.id.value(), request.facility_epoch, request.revision);
  plan.request_id = request.id;
  plan.revision = revision;
  plan.state = PlanState::Draft;
  plan.kind = request.kind;
  plan.site = request.site;
  plan.intent = request.intent;
  plan.scope = request.scope;
  plan.facility_epoch = request.facility_epoch;
  plan.policy_generation = request.bound_generations.policy;
  plan.bound_generations = request.bound_generations;
  plan.request_digest = request.digest();
  plan.evidence_digest = request.evidence_digest;
  plan.facility_digest = snapshot.digest();
  plan.authority = authority;
  plan.steps = request.steps;
  std::sort(plan.steps.begin(), plan.steps.end(),
            [](const ChangeStep& a, const ChangeStep& b) { return a.id < b.id; });
  for (const ChangeStep& step : plan.steps) {
    plan.step_status.insert_or_assign(step.id, StepStatus::Pending);
  }
  plan.explained_revision = facility_revision;
  plan.created_at_micros = now_micros;
  plan.updated_at_micros = now_micros;
  plan.transition_note = "created from request " + request.id.value();

  Diagnostics plan_diagnostics;
  validate_plan(plan, plan_diagnostics);
  if (!plan_diagnostics.empty()) {
    return Result<ChangePlan>(plan_diagnostics.primary());
  }
  return success(std::move(plan));
}

PlanEvaluation evaluate_plan(const ChangePlan& plan, const FacilitySnapshot& snapshot) {
  PlanEvaluation evaluation;
  validate_plan(plan, evaluation.diagnostics);

  if (plan.site.valid() && snapshot.site().valid() && plan.site != snapshot.site()) {
    evaluation.diagnostics.add(ErrorCode::ImpossibleCombination, plan.site.to_string(),
                               "plan site does not match the observed facility site");
  }
  const CurrencyCheck currency = check_currency(plan.bound_generations, snapshot.generations());
  if (!currency.current()) {
    bool explained = true;
    for (const GenerationDelta& delta : currency.advanced) {
      const auto it = std::find_if(plan.self_advanced.begin(), plan.self_advanced.end(),
                                   [&](const GenerationDelta& recorded) {
                                     return recorded.kind == delta.kind &&
                                            recorded.current == delta.current;
                                   });
      if (it == plan.self_advanced.end()) {
        explained = false;
        break;
      }
    }
    if (!currency.regressed.empty()) explained = false;
    if (!explained) {
      evaluation.diagnostics.add(currency.primary_code(), currency.subject_key(),
                                 "generations moved underneath the plan: " + currency.describe());
    }
  }

  auto order = plan.topological_order();
  if (!order.ok()) {
    evaluation.diagnostics.add(order.error());
    return evaluation;
  }
  evaluation.order = order.value();

  for (const StepId& step_id : evaluation.order) {
    const ChangeStep* step = plan.find_step(step_id);
    if (step == nullptr) continue;
    StepReadiness readiness;
    readiness.step = step_id;
    readiness.eligible = true;
    for (const StepId& dependency : step->depends_on) {
      if (plan.status_of(dependency) != StepStatus::Verified) readiness.eligible = false;
    }
    if (readiness.eligible) {
      if (snapshot.find(step->request.asset) == nullptr) {
        evaluation.diagnostics.add(ErrorCode::UnknownIdentity, step_id.to_string(),
                                   "step targets an asset that is not in the observed facility");
        readiness.blockages.push_back("asset is not present in the facility snapshot");
      }
      for (const Predicate& predicate : step->preconditions) {
        auto satisfied = evaluate_predicate(predicate, snapshot, plan.step_status);
        if (!satisfied.ok()) {
          evaluation.diagnostics.add(satisfied.error());
          readiness.blockages.push_back(describe(satisfied.error()));
          continue;
        }
        if (!satisfied.value()) {
          evaluation.diagnostics.add(ErrorCode::PreconditionUnsatisfied, step_id.to_string(),
                                     "precondition " + predicate.describe() + " does not hold");
          readiness.blockages.push_back(predicate.describe() + " does not hold");
        }
      }
    }
    readiness.ready = readiness.eligible && readiness.blockages.empty();
    if (readiness.eligible) {
      if (readiness.ready) {
        evaluation.initially_ready.push_back(step_id);
      } else {
        evaluation.initially_blocked.push_back(step_id);
      }
    }
    evaluation.readiness.push_back(std::move(readiness));
  }

  evaluation.explanation.push_back("plan " + plan.id.value() + " revision " +
                                   plan.revision.to_string() + " state " +
                                   std::string(to_string(plan.state)));
  evaluation.explanation.push_back("intent: " + plan.intent);
  {
    std::set<std::string> domains;
    std::set<std::string> authorities;
    for (const ChangeStep& step : plan.steps) {
      domains.insert(std::string(to_string(step.owner)));
      authorities.insert(step.request.asset.value());
    }
    std::string domain_line = "domains:";
    for (const std::string& domain : domains) domain_line += " " + domain;
    evaluation.explanation.push_back(domain_line);
    std::string asset_line = "assets:";
    for (const std::string& asset : authorities) asset_line += " " + asset;
    evaluation.explanation.push_back(asset_line);
  }
  {
    std::string line = "order:";
    for (const StepId& step_id : evaluation.order) line += " " + step_id.value();
    evaluation.explanation.push_back(line);
  }
  {
    std::string line = "ready-now:";
    for (const StepId& step_id : evaluation.initially_ready) line += " " + step_id.value();
    evaluation.explanation.push_back(line);
  }
  for (const StepId& step_id : evaluation.initially_blocked) {
    for (const StepReadiness& readiness : evaluation.readiness) {
      if (readiness.step != step_id) continue;
      for (const std::string& blockage : readiness.blockages) {
        evaluation.explanation.push_back("blocked " + step_id.value() + ": " + blockage);
      }
    }
  }
  evaluation.explanation.push_back(
      "generations: " + plan.bound_generations.describe());
  evaluation.explanation.push_back(
      "facility-digest: " + plan.facility_digest.to_hex());
  return evaluation;
}

std::vector<std::string> fencing_reasons(const ChangePlan& plan,
                                         const FacilitySnapshot& snapshot,
                                         const AuthorityContext& current,
                                         FacilityRevision current_facility_revision) {
  std::vector<std::string> reasons;

  const FencingDecision decision = fence_authority(plan.authority, current);
  if (!decision.permitted) reasons.push_back(decision.describe());

  for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kGenerationKindMax); ++kind) {
    const auto typed = static_cast<GenerationKind>(kind);
    const std::uint64_t bound = plan.bound_generations.get(typed);
    const std::uint64_t observed = snapshot.generations().get(typed);
    if (bound == observed) continue;
    const auto recorded = std::find_if(plan.self_advanced.begin(), plan.self_advanced.end(),
                                       [&](const GenerationDelta& delta) {
                                         return delta.kind == typed && delta.current == observed;
                                       });
    if (observed > bound && recorded != plan.self_advanced.end()) continue;
    reasons.push_back(std::string(to_string(typed)) + " moved from " + std::to_string(bound) +
                      " to " + std::to_string(observed) +
                      (observed > bound ? " outside this plan"
                                        : " backwards; the observation is not trustworthy"));
  }

  if (plan.explained_revision.valid() &&
      current_facility_revision.value() != plan.explained_revision.value()) {
    reasons.push_back("facility revision moved from " + plan.explained_revision.to_string() +
                      " to " + current_facility_revision.to_string() +
                      " outside this plan");
  }
  return reasons;
}

std::vector<StepId> ready_steps(const ChangePlan& plan) {
  std::vector<StepId> out;
  auto order = plan.topological_order();
  if (!order.ok()) return out;
  for (const StepId& step_id : order.value()) {
    const ChangeStep* step = plan.find_step(step_id);
    if (step == nullptr) continue;
    const StepStatus status = plan.status_of(step_id);
    if (is_settled_step_status(status) || status == StepStatus::Issued ||
        status == StepStatus::Acknowledged || status == StepStatus::Observed ||
        status == StepStatus::Unresolved) {
      continue;
    }
    bool eligible = true;
    for (const StepId& dependency : step->depends_on) {
      if (plan.status_of(dependency) != StepStatus::Verified) eligible = false;
    }
    if (eligible) out.push_back(step_id);
  }
  return out;
}

}  // namespace fco
