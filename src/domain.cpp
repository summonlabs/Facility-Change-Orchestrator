#include "fco/domain.hpp"

#include <array>

namespace fco {
namespace {

template <class Enum, std::size_t N>
[[nodiscard]] std::string_view lookup(const std::array<std::pair<Enum, std::string_view>, N>& table,
                                      Enum value) noexcept {
  for (const auto& entry : table) {
    if (entry.first == value) return entry.second;
  }
  return "invalid";
}

template <class Enum, std::size_t N>
[[nodiscard]] Result<Enum> parse_with(
    const std::array<std::pair<Enum, std::string_view>, N>& table, std::string_view text) {
  for (const auto& entry : table) {
    if (entry.second == text) return success(entry.first);
  }
  return failure<Enum>(ErrorCode::InvalidEnumValue, std::string(text),
                       "unrecognised enumerator name");
}

constexpr std::array<std::pair<DomainKind, std::string_view>, 11> kDomainKindNames = {{
    {DomainKind::Unspecified, "unspecified"},
    {DomainKind::PhysicalAsset, "physical-asset"},
    {DomainKind::Power, "power"},
    {DomainKind::Cooling, "cooling"},
    {DomainKind::Capacity, "capacity"},
    {DomainKind::Network, "network"},
    {DomainKind::Lifecycle, "lifecycle"},
    {DomainKind::Maintenance, "maintenance"},
    {DomainKind::Asi, "asi"},
    {DomainKind::Dfi, "dfi"},
    {DomainKind::Verification, "verification"},
}};

constexpr std::array<std::pair<SafetyClass, std::string_view>, 5> kSafetyClassNames = {{
    {SafetyClass::Unspecified, "unspecified"},
    {SafetyClass::Advisory, "advisory"},
    {SafetyClass::Standard, "standard"},
    {SafetyClass::Restricted, "restricted"},
    {SafetyClass::Critical, "critical"},
}};

constexpr std::array<std::pair<Reversibility, std::string_view>, 4> kReversibilityNames = {{
    {Reversibility::Unspecified, "unspecified"},
    {Reversibility::Reversible, "reversible"},
    {Reversibility::Compensable, "compensable"},
    {Reversibility::NonReversible, "non-reversible"},
}};

constexpr std::array<std::pair<VerificationMode, std::string_view>, 3> kVerificationModeNames = {{
    {VerificationMode::Unspecified, "unspecified"},
    {VerificationMode::Required, "required"},
    {VerificationMode::NotRequired, "not-required"},
}};

constexpr std::array<std::pair<ActionKind, std::string_view>, 20> kActionKindNames = {{
    {ActionKind::Unspecified, "unspecified"},
    {ActionKind::DrainWorkloads, "drain-workloads"},
    {ActionKind::RestoreWorkloads, "restore-workloads"},
    {ActionKind::MaintenanceIsolate, "maintenance-isolate"},
    {ActionKind::MaintenanceRelease, "maintenance-release"},
    {ActionKind::PowerDownAsset, "power-down-asset"},
    {ActionKind::PowerUpAsset, "power-up-asset"},
    {ActionKind::CoolingIsolate, "cooling-isolate"},
    {ActionKind::CoolingRestore, "cooling-restore"},
    {ActionKind::CapacityReserve, "capacity-reserve"},
    {ActionKind::CapacityRelease, "capacity-release"},
    {ActionKind::NetworkQuiesce, "network-quiesce"},
    {ActionKind::NetworkRestore, "network-restore"},
    {ActionKind::LifecycleTransition, "lifecycle-transition"},
    {ActionKind::HardwareSwap, "hardware-swap"},
    {ActionKind::FirmwareStage, "firmware-stage"},
    {ActionKind::Recommission, "recommission"},
    {ActionKind::Decommission, "decommission"},
    {ActionKind::VerifyRestoration, "verify-restoration"},
    {ActionKind::RecordNoOp, "record-no-op"},
}};

constexpr std::array<std::pair<EffectKind, std::string_view>, 12> kEffectKindNames = {{
    {EffectKind::Unspecified, "unspecified"},
    {EffectKind::WorkloadStateChange, "workload-state-change"},
    {EffectKind::MaintenanceStateChange, "maintenance-state-change"},
    {EffectKind::PowerStateChange, "power-state-change"},
    {EffectKind::CoolingStateChange, "cooling-state-change"},
    {EffectKind::CapacityChange, "capacity-change"},
    {EffectKind::NetworkStateChange, "network-state-change"},
    {EffectKind::LifecycleStateChange, "lifecycle-state-change"},
    {EffectKind::HardwareGenerationChange, "hardware-generation-change"},
    {EffectKind::FirmwareGenerationChange, "firmware-generation-change"},
    {EffectKind::RestorationVerified, "restoration-verified"},
    {EffectKind::NoOpRecorded, "no-op-recorded"},
}};

constexpr std::array<std::pair<PredicateKind, std::string_view>, 12> kPredicateKindNames = {{
    {PredicateKind::Unspecified, "unspecified"},
    {PredicateKind::AssetLifecycleIs, "asset-lifecycle-is"},
    {PredicateKind::PowerStateIs, "power-state-is"},
    {PredicateKind::CoolingStateIs, "cooling-state-is"},
    {PredicateKind::MaintenanceStateIs, "maintenance-state-is"},
    {PredicateKind::NetworkStateIs, "network-state-is"},
    {PredicateKind::WorkloadStateIs, "workload-state-is"},
    {PredicateKind::CapacityAvailableAtLeast, "capacity-available-at-least"},
    {PredicateKind::PredecessorVerified, "predecessor-verified"},
    {PredicateKind::PredecessorNotVerified, "predecessor-not-verified"},
    {PredicateKind::AssetLifecycleGenerationAtLeast, "asset-lifecycle-generation-at-least"},
    {PredicateKind::NoTenantAssigned, "no-tenant-assigned"},
}};

constexpr std::array<std::pair<AssetLifecycle, std::string_view>, 13> kAssetLifecycleNames = {{
    {AssetLifecycle::Unspecified, "unspecified"},
    {AssetLifecycle::Unknown, "unknown"},
    {AssetLifecycle::Discovered, "discovered"},
    {AssetLifecycle::Commissioning, "commissioning"},
    {AssetLifecycle::Installed, "installed"},
    {AssetLifecycle::Active, "active"},
    {AssetLifecycle::Draining, "draining"},
    {AssetLifecycle::Maintenance, "maintenance"},
    {AssetLifecycle::Drained, "drained"},
    {AssetLifecycle::Decommissioning, "decommissioning"},
    {AssetLifecycle::Decommissioned, "decommissioned"},
    {AssetLifecycle::Failed, "failed"},
    {AssetLifecycle::Retired, "retired"},
}};

constexpr std::array<std::pair<PowerState, std::string_view>, 5> kPowerStateNames = {{
    {PowerState::Unspecified, "unspecified"},
    {PowerState::Unknown, "unknown"},
    {PowerState::Off, "off"},
    {PowerState::On, "on"},
    {PowerState::Cycling, "cycling"},
}};

constexpr std::array<std::pair<CoolingState, std::string_view>, 6> kCoolingStateNames = {{
    {CoolingState::Unspecified, "unspecified"},
    {CoolingState::Unknown, "unknown"},
    {CoolingState::Normal, "normal"},
    {CoolingState::Reduced, "reduced"},
    {CoolingState::Isolated, "isolated"},
    {CoolingState::Failed, "failed"},
}};

constexpr std::array<std::pair<MaintenanceState, std::string_view>, 5> kMaintenanceStateNames = {{
    {MaintenanceState::Unspecified, "unspecified"},
    {MaintenanceState::Unknown, "unknown"},
    {MaintenanceState::None, "none"},
    {MaintenanceState::Scheduled, "scheduled"},
    {MaintenanceState::Isolated, "isolated"},
}};

constexpr std::array<std::pair<NetworkState, std::string_view>, 5> kNetworkStateNames = {{
    {NetworkState::Unspecified, "unspecified"},
    {NetworkState::Unknown, "unknown"},
    {NetworkState::Attached, "attached"},
    {NetworkState::Quiesced, "quiesced"},
    {NetworkState::Detached, "detached"},
}};

constexpr std::array<std::pair<WorkloadState, std::string_view>, 6> kWorkloadStateNames = {{
    {WorkloadState::Unspecified, "unspecified"},
    {WorkloadState::Unknown, "unknown"},
    {WorkloadState::Present, "present"},
    {WorkloadState::Draining, "draining"},
    {WorkloadState::Drained, "drained"},
    {WorkloadState::Absent, "absent"},
}};

constexpr std::array<std::pair<PlanState, std::string_view>, 14> kPlanStateNames = {{
    {PlanState::Unspecified, "unspecified"},
    {PlanState::Draft, "draft"},
    {PlanState::Evaluated, "evaluated"},
    {PlanState::Authorized, "authorized"},
    {PlanState::Executing, "executing"},
    {PlanState::Paused, "paused"},
    {PlanState::PartiallyApplied, "partially-applied"},
    {PlanState::Verifying, "verifying"},
    {PlanState::Completed, "completed"},
    {PlanState::Failed, "failed"},
    {PlanState::RollingBack, "rolling-back"},
    {PlanState::RolledBack, "rolled-back"},
    {PlanState::ReplanRequired, "replan-required"},
    {PlanState::Cancelled, "cancelled"},
}};

constexpr std::array<std::pair<StepStatus, std::string_view>, 12> kStepStatusNames = {{
    {StepStatus::Unspecified, "unspecified"},
    {StepStatus::Pending, "pending"},
    {StepStatus::Ready, "ready"},
    {StepStatus::Issued, "issued"},
    {StepStatus::Acknowledged, "acknowledged"},
    {StepStatus::Observed, "observed"},
    {StepStatus::Verified, "verified"},
    {StepStatus::Failed, "failed"},
    {StepStatus::Compensated, "compensated"},
    {StepStatus::Blocked, "blocked"},
    {StepStatus::Unresolved, "unresolved"},
    {StepStatus::NonCompensable, "non-compensable"},
}};

constexpr std::array<std::pair<AttemptStatus, std::string_view>, 11> kAttemptStatusNames = {{
    {AttemptStatus::Unspecified, "unspecified"},
    {AttemptStatus::NotIssued, "not-issued"},
    {AttemptStatus::PossiblyIssued, "possibly-issued"},
    {AttemptStatus::Acknowledged, "acknowledged"},
    {AttemptStatus::Observed, "observed"},
    {AttemptStatus::Verified, "verified"},
    {AttemptStatus::Failed, "failed"},
    {AttemptStatus::Compensated, "compensated"},
    {AttemptStatus::Unresolved, "unresolved"},
    {AttemptStatus::Rejected, "rejected"},
    {AttemptStatus::ResolutionRequired, "resolution-required"},
}};

}  // namespace

std::string_view to_string(DomainKind value) noexcept {
  return lookup<DomainKind>(kDomainKindNames, value);
}
std::string_view to_string(SafetyClass value) noexcept {
  return lookup<SafetyClass>(kSafetyClassNames, value);
}
std::string_view to_string(Reversibility value) noexcept {
  return lookup<Reversibility>(kReversibilityNames, value);
}
std::string_view to_string(VerificationMode value) noexcept {
  return lookup<VerificationMode>(kVerificationModeNames, value);
}
std::string_view to_string(ActionKind value) noexcept {
  return lookup<ActionKind>(kActionKindNames, value);
}
std::string_view to_string(EffectKind value) noexcept {
  return lookup<EffectKind>(kEffectKindNames, value);
}
std::string_view to_string(PredicateKind value) noexcept {
  return lookup<PredicateKind>(kPredicateKindNames, value);
}
std::string_view to_string(AssetLifecycle value) noexcept {
  return lookup<AssetLifecycle>(kAssetLifecycleNames, value);
}
std::string_view to_string(PowerState value) noexcept {
  return lookup<PowerState>(kPowerStateNames, value);
}
std::string_view to_string(CoolingState value) noexcept {
  return lookup<CoolingState>(kCoolingStateNames, value);
}
std::string_view to_string(MaintenanceState value) noexcept {
  return lookup<MaintenanceState>(kMaintenanceStateNames, value);
}
std::string_view to_string(NetworkState value) noexcept {
  return lookup<NetworkState>(kNetworkStateNames, value);
}
std::string_view to_string(WorkloadState value) noexcept {
  return lookup<WorkloadState>(kWorkloadStateNames, value);
}
std::string_view to_string(PlanState value) noexcept {
  return lookup<PlanState>(kPlanStateNames, value);
}
std::string_view to_string(StepStatus value) noexcept {
  return lookup<StepStatus>(kStepStatusNames, value);
}
std::string_view to_string(AttemptStatus value) noexcept {
  return lookup<AttemptStatus>(kAttemptStatusNames, value);
}

Result<DomainKind> parse_domain_kind(std::string_view text) {
  return parse_with<DomainKind>(kDomainKindNames, text);
}
Result<SafetyClass> parse_safety_class(std::string_view text) {
  return parse_with<SafetyClass>(kSafetyClassNames, text);
}
Result<Reversibility> parse_reversibility(std::string_view text) {
  return parse_with<Reversibility>(kReversibilityNames, text);
}
Result<VerificationMode> parse_verification_mode(std::string_view text) {
  return parse_with<VerificationMode>(kVerificationModeNames, text);
}
Result<ActionKind> parse_action_kind(std::string_view text) {
  return parse_with<ActionKind>(kActionKindNames, text);
}
Result<EffectKind> parse_effect_kind(std::string_view text) {
  return parse_with<EffectKind>(kEffectKindNames, text);
}
Result<PredicateKind> parse_predicate_kind(std::string_view text) {
  return parse_with<PredicateKind>(kPredicateKindNames, text);
}
Result<AssetLifecycle> parse_asset_lifecycle(std::string_view text) {
  return parse_with<AssetLifecycle>(kAssetLifecycleNames, text);
}
Result<PowerState> parse_power_state(std::string_view text) {
  return parse_with<PowerState>(kPowerStateNames, text);
}
Result<CoolingState> parse_cooling_state(std::string_view text) {
  return parse_with<CoolingState>(kCoolingStateNames, text);
}
Result<MaintenanceState> parse_maintenance_state(std::string_view text) {
  return parse_with<MaintenanceState>(kMaintenanceStateNames, text);
}
Result<NetworkState> parse_network_state(std::string_view text) {
  return parse_with<NetworkState>(kNetworkStateNames, text);
}
Result<WorkloadState> parse_workload_state(std::string_view text) {
  return parse_with<WorkloadState>(kWorkloadStateNames, text);
}
Result<PlanState> parse_plan_state(std::string_view text) {
  return parse_with<PlanState>(kPlanStateNames, text);
}
Result<StepStatus> parse_step_status(std::string_view text) {
  return parse_with<StepStatus>(kStepStatusNames, text);
}
Result<AttemptStatus> parse_attempt_status(std::string_view text) {
  return parse_with<AttemptStatus>(kAttemptStatusNames, text);
}

DomainKind owning_domain(ActionKind action) noexcept {
  switch (action) {
    case ActionKind::DrainWorkloads:
    case ActionKind::RestoreWorkloads:
      return DomainKind::Asi;
    case ActionKind::MaintenanceIsolate:
    case ActionKind::MaintenanceRelease:
      return DomainKind::Maintenance;
    case ActionKind::PowerDownAsset:
    case ActionKind::PowerUpAsset:
      return DomainKind::Power;
    case ActionKind::CoolingIsolate:
    case ActionKind::CoolingRestore:
      return DomainKind::Cooling;
    case ActionKind::CapacityReserve:
    case ActionKind::CapacityRelease:
      return DomainKind::Capacity;
    case ActionKind::NetworkQuiesce:
    case ActionKind::NetworkRestore:
      return DomainKind::Dfi;
    case ActionKind::LifecycleTransition:
    case ActionKind::HardwareSwap:
    case ActionKind::FirmwareStage:
    case ActionKind::Recommission:
    case ActionKind::Decommission:
      return DomainKind::Lifecycle;
    case ActionKind::VerifyRestoration:
      return DomainKind::Verification;
    case ActionKind::RecordNoOp:
      return DomainKind::PhysicalAsset;
    case ActionKind::Unspecified:
    default:
      return DomainKind::Unspecified;
  }
}

bool is_idempotent_action(ActionKind action) noexcept {
  switch (action) {
    case ActionKind::DrainWorkloads:
    case ActionKind::MaintenanceIsolate:
    case ActionKind::MaintenanceRelease:
    case ActionKind::PowerDownAsset:
    case ActionKind::PowerUpAsset:
    case ActionKind::CoolingIsolate:
    case ActionKind::CoolingRestore:
    case ActionKind::CapacityReserve:
    case ActionKind::CapacityRelease:
    case ActionKind::NetworkQuiesce:
    case ActionKind::NetworkRestore:
    case ActionKind::VerifyRestoration:
    case ActionKind::RecordNoOp:
      return true;
    case ActionKind::RestoreWorkloads:
    case ActionKind::LifecycleTransition:
    case ActionKind::HardwareSwap:
    case ActionKind::FirmwareStage:
    case ActionKind::Recommission:
    case ActionKind::Decommission:
    case ActionKind::Unspecified:
    default:
      return false;
  }
}

bool is_destructive_action(ActionKind action) noexcept {
  switch (action) {
    case ActionKind::HardwareSwap:
    case ActionKind::FirmwareStage:
    case ActionKind::Decommission:
    case ActionKind::PowerDownAsset:
    case ActionKind::CoolingIsolate:
      return true;
    default:
      return false;
  }
}

bool ActionRequest::complete() const noexcept {
  return is_valid(kind) && is_valid(owner) && asset.valid() && rack.valid() && site.valid() &&
         lifecycle_generation.valid();
}

std::string ActionRequest::describe() const {
  std::string out(to_string(kind));
  out += " via ";
  out += to_string(owner);
  out += " on ";
  out += asset.describe();
  if (rack.valid()) {
    out += " in ";
    out += rack.describe();
  }
  if (target_state != 0) {
    out += " target=" + std::to_string(target_state);
  }
  if (target_value != 0) {
    out += " value=" + std::to_string(target_value);
  }
  return out;
}

void ActionRequest::hash_into(Sha256& hasher) const noexcept {
  hasher.update_u8(static_cast<std::uint8_t>(kind));
  hasher.update_u8(static_cast<std::uint8_t>(owner));
  hasher.update(asset.value());
  hasher.update_u8(0);
  hasher.update(rack.value());
  hasher.update_u8(0);
  hasher.update(site.value());
  hasher.update_u8(0);
  hasher.update_u64(hardware_generation.value());
  hasher.update_u64(firmware_generation.value());
  hasher.update_u64(lifecycle_generation.value());
  hasher.update_u32(target_state);
  hasher.update_u64(target_value);
}

Digest ActionRequest::digest() const {
  Sha256 hasher;
  hasher.update("fco/action-request/1");
  hash_into(hasher);
  return hasher.finish();
}

std::string Predicate::subject_key() const {
  std::string key(to_string(kind));
  key += '/';
  key += asset.value();
  key += '/';
  key += predecessor.value();
  key += '/';
  key += std::to_string(expected_state);
  key += '/';
  key += std::to_string(expected_value);
  return key;
}

std::string Predicate::describe() const {
  std::string out(to_string(kind));
  if (asset.valid()) {
    out += " on ";
    out += asset.describe();
  }
  if (predecessor.valid()) {
    out += " of ";
    out += predecessor.describe();
  }
  if (expected_state != 0) {
    out += " expecting=" + std::to_string(expected_state);
  }
  if (expected_value != 0) {
    out += " at-least=" + std::to_string(expected_value);
  }
  return out;
}

void Predicate::hash_into(Sha256& hasher) const noexcept {
  hasher.update_u8(static_cast<std::uint8_t>(kind));
  hasher.update(asset.value());
  hasher.update_u8(0);
  hasher.update(predecessor.value());
  hasher.update_u8(0);
  hasher.update_u32(expected_state);
  hasher.update_u64(expected_value);
}

bool EffectSpec::complete() const noexcept {
  return is_valid(kind) && asset.valid();
}

std::string EffectSpec::describe() const {
  std::string out(to_string(kind));
  if (asset.valid()) {
    out += " on ";
    out += asset.describe();
  }
  if (expected_state != 0) {
    out += " expecting=" + std::to_string(expected_state);
  }
  if (expected_value != 0) {
    out += " value=" + std::to_string(expected_value);
  }
  return out;
}

void EffectSpec::hash_into(Sha256& hasher) const noexcept {
  hasher.update_u8(static_cast<std::uint8_t>(kind));
  hasher.update(asset.value());
  hasher.update_u8(0);
  hasher.update_u32(expected_state);
  hasher.update_u64(expected_value);
}

}  // namespace fco
