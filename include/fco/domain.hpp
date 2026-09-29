#pragma once

// Domain vocabulary: the typed language this runtime speaks to the domain
// authorities it composes.
//
// Every enumerator domain reserves the value 0 for "Unspecified". A zero value
// is rejected by every decoder and every validation path, so a truncated or
// zeroed durable record can never be read as a real domain state. "Unknown" is
// modelled as an explicit, non-zero enumerator: it is a real observation, and it
// is never treated as healthy, ready, safe, or permitted.

#include <cstdint>
#include <string>
#include <string_view>

#include "fco/digest.hpp"
#include "fco/error.hpp"
#include "fco/strong.hpp"

namespace fco {

// --------------------------------------------------------------------------
// Owning domain authority
// --------------------------------------------------------------------------
enum class DomainKind : std::uint8_t {
  Unspecified = 0,
  PhysicalAsset = 1,
  Power = 2,
  Cooling = 3,
  Capacity = 4,
  Network = 5,
  Lifecycle = 6,
  Maintenance = 7,
  Asi = 8,
  Dfi = 9,
  Verification = 10,
};
inline constexpr DomainKind kDomainKindMax = DomainKind::Verification;

// --------------------------------------------------------------------------
// Change classification
// --------------------------------------------------------------------------
enum class SafetyClass : std::uint8_t {
  Unspecified = 0,
  Advisory = 1,
  Standard = 2,
  Restricted = 3,
  Critical = 4,
};
inline constexpr SafetyClass kSafetyClassMax = SafetyClass::Critical;

enum class Reversibility : std::uint8_t {
  Unspecified = 0,
  Reversible = 1,
  Compensable = 2,
  NonReversible = 3,
};
inline constexpr Reversibility kReversibilityMax = Reversibility::NonReversible;

enum class VerificationMode : std::uint8_t {
  Unspecified = 0,
  Required = 1,
  NotRequired = 2,
};
inline constexpr VerificationMode kVerificationModeMax = VerificationMode::NotRequired;

// --------------------------------------------------------------------------
// Requested action
// --------------------------------------------------------------------------
enum class ActionKind : std::uint8_t {
  Unspecified = 0,
  DrainWorkloads = 1,
  RestoreWorkloads = 2,
  MaintenanceIsolate = 3,
  MaintenanceRelease = 4,
  PowerDownAsset = 5,
  PowerUpAsset = 6,
  CoolingIsolate = 7,
  CoolingRestore = 8,
  CapacityReserve = 9,
  CapacityRelease = 10,
  NetworkQuiesce = 11,
  NetworkRestore = 12,
  LifecycleTransition = 13,
  HardwareSwap = 14,
  FirmwareStage = 15,
  Recommission = 16,
  Decommission = 17,
  VerifyRestoration = 18,
  RecordNoOp = 19,
};
inline constexpr ActionKind kActionKindMax = ActionKind::RecordNoOp;

enum class EffectKind : std::uint8_t {
  Unspecified = 0,
  WorkloadStateChange = 1,
  MaintenanceStateChange = 2,
  PowerStateChange = 3,
  CoolingStateChange = 4,
  CapacityChange = 5,
  NetworkStateChange = 6,
  LifecycleStateChange = 7,
  HardwareGenerationChange = 8,
  FirmwareGenerationChange = 9,
  RestorationVerified = 10,
  NoOpRecorded = 11,
};
inline constexpr EffectKind kEffectKindMax = EffectKind::NoOpRecorded;

// --------------------------------------------------------------------------
// Preconditions
// --------------------------------------------------------------------------
enum class PredicateKind : std::uint8_t {
  Unspecified = 0,
  AssetLifecycleIs = 1,
  PowerStateIs = 2,
  CoolingStateIs = 3,
  MaintenanceStateIs = 4,
  NetworkStateIs = 5,
  WorkloadStateIs = 6,
  CapacityAvailableAtLeast = 7,
  PredecessorVerified = 8,
  PredecessorNotVerified = 9,
  AssetLifecycleGenerationAtLeast = 10,
  NoTenantAssigned = 11,
};
inline constexpr PredicateKind kPredicateKindMax = PredicateKind::NoTenantAssigned;

// --------------------------------------------------------------------------
// Facility observation vocabulary
// --------------------------------------------------------------------------
enum class AssetLifecycle : std::uint8_t {
  Unspecified = 0,
  Unknown = 1,
  Discovered = 2,
  Commissioning = 3,
  Installed = 4,
  Active = 5,
  Draining = 6,
  Maintenance = 7,
  Drained = 8,
  Decommissioning = 9,
  Decommissioned = 10,
  Failed = 11,
  Retired = 12,
};
inline constexpr AssetLifecycle kAssetLifecycleMax = AssetLifecycle::Retired;

enum class PowerState : std::uint8_t {
  Unspecified = 0,
  Unknown = 1,
  Off = 2,
  On = 3,
  Cycling = 4,
};
inline constexpr PowerState kPowerStateMax = PowerState::Cycling;

enum class CoolingState : std::uint8_t {
  Unspecified = 0,
  Unknown = 1,
  Normal = 2,
  Reduced = 3,
  Isolated = 4,
  Failed = 5,
};
inline constexpr CoolingState kCoolingStateMax = CoolingState::Failed;

enum class MaintenanceState : std::uint8_t {
  Unspecified = 0,
  Unknown = 1,
  None = 2,
  Scheduled = 3,
  Isolated = 4,
};
inline constexpr MaintenanceState kMaintenanceStateMax = MaintenanceState::Isolated;

enum class NetworkState : std::uint8_t {
  Unspecified = 0,
  Unknown = 1,
  Attached = 2,
  Quiesced = 3,
  Detached = 4,
};
inline constexpr NetworkState kNetworkStateMax = NetworkState::Detached;

enum class WorkloadState : std::uint8_t {
  Unspecified = 0,
  Unknown = 1,
  Present = 2,
  Draining = 3,
  Drained = 4,
  Absent = 5,
};
inline constexpr WorkloadState kWorkloadStateMax = WorkloadState::Absent;

// --------------------------------------------------------------------------
// Plan lifecycle
// --------------------------------------------------------------------------
enum class PlanState : std::uint8_t {
  Unspecified = 0,
  Draft = 1,
  Evaluated = 2,
  Authorized = 3,
  Executing = 4,
  Paused = 5,
  PartiallyApplied = 6,
  Verifying = 7,
  Completed = 8,
  Failed = 9,
  RollingBack = 10,
  RolledBack = 11,
  ReplanRequired = 12,
  Cancelled = 13,
};
inline constexpr PlanState kPlanStateMax = PlanState::Cancelled;

enum class StepStatus : std::uint8_t {
  Unspecified = 0,
  Pending = 1,
  Ready = 2,
  Issued = 3,
  Acknowledged = 4,
  Observed = 5,
  Verified = 6,
  Failed = 7,
  Compensated = 8,
  Blocked = 9,
  Unresolved = 10,
  NonCompensable = 11,
};
inline constexpr StepStatus kStepStatusMax = StepStatus::NonCompensable;

enum class AttemptStatus : std::uint8_t {
  Unspecified = 0,
  NotIssued = 1,
  PossiblyIssued = 2,
  Acknowledged = 3,
  Observed = 4,
  Verified = 5,
  Failed = 6,
  Compensated = 7,
  Unresolved = 8,
  Rejected = 9,
  ResolutionRequired = 10,
};
inline constexpr AttemptStatus kAttemptStatusMax = AttemptStatus::ResolutionRequired;

// --------------------------------------------------------------------------
// Enum services. is_valid() rejects the reserved zero value and any value
// outside the defined domain; decoders fail rather than clamping.
// --------------------------------------------------------------------------
#define FCO_ENUM_SERVICES(Tag, Name, MaxValue)                                    \
  [[nodiscard]] constexpr bool is_valid(Name value) noexcept {                    \
    return static_cast<std::uint8_t>(value) >= 1u &&                              \
           static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(MaxValue); \
  }                                                                               \
  [[nodiscard]] std::string_view to_string(Name value) noexcept;                  \
  [[nodiscard]] Result<Name> parse_##Tag(std::string_view text);

FCO_ENUM_SERVICES(domain_kind, DomainKind, kDomainKindMax)
FCO_ENUM_SERVICES(safety_class, SafetyClass, kSafetyClassMax)
FCO_ENUM_SERVICES(reversibility, Reversibility, kReversibilityMax)
FCO_ENUM_SERVICES(verification_mode, VerificationMode, kVerificationModeMax)
FCO_ENUM_SERVICES(action_kind, ActionKind, kActionKindMax)
FCO_ENUM_SERVICES(effect_kind, EffectKind, kEffectKindMax)
FCO_ENUM_SERVICES(predicate_kind, PredicateKind, kPredicateKindMax)
FCO_ENUM_SERVICES(asset_lifecycle, AssetLifecycle, kAssetLifecycleMax)
FCO_ENUM_SERVICES(power_state, PowerState, kPowerStateMax)
FCO_ENUM_SERVICES(cooling_state, CoolingState, kCoolingStateMax)
FCO_ENUM_SERVICES(maintenance_state, MaintenanceState, kMaintenanceStateMax)
FCO_ENUM_SERVICES(network_state, NetworkState, kNetworkStateMax)
FCO_ENUM_SERVICES(workload_state, WorkloadState, kWorkloadStateMax)
FCO_ENUM_SERVICES(plan_state, PlanState, kPlanStateMax)
FCO_ENUM_SERVICES(step_status, StepStatus, kStepStatusMax)
FCO_ENUM_SERVICES(attempt_status, AttemptStatus, kAttemptStatusMax)

#undef FCO_ENUM_SERVICES

// Generic decode helper used by the durable record decoder: an unknown byte is
// an InvalidEnumValue rejection, never a clamp to a neighbouring value.
template <class Enum>
[[nodiscard]] Result<Enum> decode_enum(std::uint8_t raw, std::string_view field) {
  const auto value = static_cast<Enum>(raw);
  if (!is_valid(value)) {
    return failure<Enum>(ErrorCode::InvalidEnumValue, std::string(field),
                         "encoded value " + std::to_string(raw) + " is outside the enum domain");
  }
  return success(value);
}

// The domain authority that must own each action kind. Composing a request for
// an action through the wrong authority is rejected during plan validation.
[[nodiscard]] DomainKind owning_domain(ActionKind action) noexcept;

// True when the action is idempotent by construction: re-issuing it cannot
// compound a physical effect.
[[nodiscard]] bool is_idempotent_action(ActionKind action) noexcept;

// True when the action is destructive and must never be blindly replayed after
// an unresolved outcome.
[[nodiscard]] bool is_destructive_action(ActionKind action) noexcept;

// --------------------------------------------------------------------------
// Typed action request handed to a domain authority. The orchestrator never
// performs the action itself; it describes it precisely and the owning runtime
// decides how.
// --------------------------------------------------------------------------
struct ActionRequest {
  ActionKind kind = ActionKind::Unspecified;
  DomainKind owner = DomainKind::Unspecified;
  AssetId asset;
  RackId rack;
  SiteId site;
  HardwareGeneration hardware_generation;
  FirmwareGeneration firmware_generation;
  LifecycleGeneration lifecycle_generation;
  std::uint32_t target_state = 0;  // interpreted per action kind
  std::uint64_t target_value = 0;  // interpreted per action kind

  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] std::string describe() const;
  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;
};

// A precondition evaluated against the facility snapshot and live plan
// progress. Predicates are evaluated, never assumed.
struct Predicate {
  PredicateKind kind = PredicateKind::Unspecified;
  AssetId asset;
  StepId predecessor;
  std::uint32_t expected_state = 0;
  std::uint64_t expected_value = 0;

  [[nodiscard]] std::string subject_key() const;
  [[nodiscard]] std::string describe() const;
  void hash_into(Sha256& hasher) const noexcept;
};

// The effect a step claims it will produce. Verification compares observed
// evidence against this specification; acknowledgement alone never satisfies it.
struct EffectSpec {
  EffectKind kind = EffectKind::Unspecified;
  AssetId asset;
  std::uint32_t expected_state = 0;
  std::uint64_t expected_value = 0;

  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] std::string describe() const;
  void hash_into(Sha256& hasher) const noexcept;
};

}  // namespace fco
