#include "fco/facility.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace fco {
namespace {

template <class Enum>
[[nodiscard]] bool state_in_domain(std::uint32_t raw, Enum maximum) noexcept {
  return raw >= 1u && raw <= static_cast<std::uint32_t>(static_cast<std::uint8_t>(maximum));
}

}  // namespace

bool AssetRecord::complete() const noexcept {
  return id.valid() && rack.valid() && site.valid() && is_valid_identifier(id.value()) &&
         is_valid_identifier(rack.value()) && is_valid_identifier(site.value()) &&
         (!tenant.valid() || is_valid_identifier(tenant.value())) && is_valid(lifecycle) &&
         is_valid(power) && is_valid(cooling) && is_valid(maintenance) && is_valid(network) &&
         is_valid(workloads) && hardware_generation.valid() && firmware_generation.valid() &&
         lifecycle_generation.valid() && maintenance_generation.valid() &&
         capacity_generation.valid();
}

std::uint64_t AssetRecord::capacity_available_units() const noexcept {
  if (capacity_reserved_units > capacity_total_units) return 0;
  return static_cast<std::uint64_t>(capacity_total_units - capacity_reserved_units);
}

void AssetRecord::hash_into(Sha256& hasher) const noexcept {
  hasher.update(id.value());
  hasher.update_u8(0);
  hasher.update(rack.value());
  hasher.update_u8(0);
  hasher.update(site.value());
  hasher.update_u8(0);
  hasher.update_u8(static_cast<std::uint8_t>(lifecycle));
  hasher.update_u8(static_cast<std::uint8_t>(power));
  hasher.update_u8(static_cast<std::uint8_t>(cooling));
  hasher.update_u8(static_cast<std::uint8_t>(maintenance));
  hasher.update_u8(static_cast<std::uint8_t>(network));
  hasher.update_u8(static_cast<std::uint8_t>(workloads));
  hasher.update_u64(hardware_generation.value());
  hasher.update_u64(firmware_generation.value());
  hasher.update_u64(lifecycle_generation.value());
  hasher.update_u64(maintenance_generation.value());
  hasher.update_u64(capacity_generation.value());
  hasher.update(tenant.value());
  hasher.update_u8(0);
  hasher.update_u32(capacity_total_units);
  hasher.update_u32(capacity_reserved_units);
}

Digest AssetRecord::digest() const {
  Sha256 hasher;
  hasher.update("fco/asset-record/1");
  hash_into(hasher);
  return hasher.finish();
}

std::string AssetRecord::describe() const {
  std::string out = id.to_string();
  out += " rack=" + rack.to_string();
  out += " lifecycle=" + std::string(to_string(lifecycle));
  out += " power=" + std::string(to_string(power));
  out += " cooling=" + std::string(to_string(cooling));
  out += " maintenance=" + std::string(to_string(maintenance));
  out += " network=" + std::string(to_string(network));
  out += " workloads=" + std::string(to_string(workloads));
  out += " hw-gen=" + hardware_generation.to_string();
  out += " fw-gen=" + firmware_generation.to_string();
  out += " lifecycle-gen=" + lifecycle_generation.to_string();
  if (tenant.valid()) out += " tenant=" + tenant.to_string();
  out += " capacity=" + std::to_string(capacity_reserved_units) + "/" +
         std::to_string(capacity_total_units);
  return out;
}

Result<FacilitySnapshot> FacilitySnapshot::create(SiteId site, FacilityEpoch epoch,
                                                  GenerationSet generations,
                                                  std::vector<AssetRecord> assets) {
  Diagnostics diagnostics;
  if (!site.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "site", "snapshot site identifier is unset");
  }
  if (!epoch.valid()) {
    diagnostics.add(ErrorCode::InvalidIdentity, "facility-epoch",
                    "snapshot facility epoch is unset");
  }
  if (!generations.complete()) {
    diagnostics.add(ErrorCode::MissingAuthority, "generations",
                    "snapshot generation set is incomplete");
  }
  if (assets.empty()) {
    diagnostics.add(ErrorCode::ImpossibleCombination, "assets",
                    "a facility snapshot must contain at least one asset");
  }
  if (assets.size() > static_cast<std::size_t>(kMaxCollectionEntries)) {
    diagnostics.add(ErrorCode::BoundedLimitExceeded, "assets",
                    "asset count exceeds the supported bound");
  }

  std::set<AssetId> seen;
  for (const AssetRecord& record : assets) {
    if (!record.complete()) {
      diagnostics.add(ErrorCode::ImpossibleCombination, record.id.to_string(),
                      "asset record is incomplete: " + record.describe());
      continue;
    }
    if (!seen.insert(record.id).second) {
      diagnostics.add(ErrorCode::DuplicateIdentity, record.id.to_string(),
                      "duplicate asset identifier in snapshot");
      continue;
    }
    // Unknown is a real observation and is retained as such; it is never
    // rewritten into a healthy state here or anywhere downstream.
  }

  if (!diagnostics.empty()) {
    const Error& primary = diagnostics.primary();
    return Result<FacilitySnapshot>(primary);
  }

  FacilitySnapshot snapshot;
  snapshot.site_ = std::move(site);
  snapshot.epoch_ = epoch;
  snapshot.generations_ = generations;
  for (AssetRecord& record : assets) {
    // Capture the key before the record is moved: the key must never be read
    // from an object that has already been moved from.
    const AssetId key = record.id;
    snapshot.assets_.insert_or_assign(key, std::move(record));
  }
  for (const auto& entry : snapshot.assets_) {
    if (!(entry.first == entry.second.id)) {
      diagnostics.add(ErrorCode::InternalError, "assets",
                      "a facility asset is keyed by an identifier that is not the record's own "
                      "identifier");
      break;
    }
  }
  if (!diagnostics.empty()) {
    return Result<FacilitySnapshot>(diagnostics.primary());
  }
  return success(std::move(snapshot));
}

bool FacilitySnapshot::valid() const noexcept {
  return site_.valid() && epoch_.valid() && generations_.complete() && !assets_.empty();
}

const AssetRecord* FacilitySnapshot::find(const AssetId& id) const noexcept {
  if (!id.valid()) return nullptr;
  const auto it = assets_.find(id);
  return it == assets_.end() ? nullptr : &it->second;
}

std::vector<const AssetRecord*> FacilitySnapshot::assets_in_rack(const RackId& rack) const {
  std::vector<const AssetRecord*> out;
  if (!rack.valid()) return out;
  for (const auto& entry : assets_) {
    if (entry.second.rack == rack) out.push_back(&entry.second);
  }
  return out;
}

std::vector<AssetId> FacilitySnapshot::asset_ids() const {
  std::vector<AssetId> out;
  out.reserve(assets_.size());
  for (const auto& entry : assets_) out.push_back(entry.first);
  return out;
}

void FacilitySnapshot::hash_into(Sha256& hasher) const noexcept {
  hasher.update(site_.value());
  hasher.update_u8(0);
  hasher.update_u64(epoch_.value());
  generations_.hash_into(hasher);
  hasher.update_u64(static_cast<std::uint64_t>(assets_.size()));
  for (const auto& entry : assets_) {
    // The key is hashed as well as the record, so a snapshot whose records are
    // keyed by the wrong identifiers cannot produce a correct digest.
    hasher.update(entry.first.value());
    hasher.update_u8(0);
    entry.second.hash_into(hasher);
  }
}

Digest FacilitySnapshot::digest() const {
  Sha256 hasher;
  hasher.update("fco/facility-snapshot/1");
  hash_into(hasher);
  return hasher.finish();
}

void FacilitySnapshot::apply(const AssetRecord& record) {
  if (!record.id.valid()) return;
  assets_.insert_or_assign(record.id, record);
}

Result<bool> evaluate_predicate(const Predicate& predicate, const FacilitySnapshot& snapshot,
                                const std::map<StepId, StepStatus>& progress) {
  const std::string subject = predicate.subject_key();
  switch (predicate.kind) {
    case PredicateKind::Unspecified:
      return failure<bool>(ErrorCode::InvalidEnumValue, subject, "precondition kind is unset");

    case PredicateKind::PredecessorVerified: {
      if (!predicate.predecessor.valid()) {
        return failure<bool>(ErrorCode::MalformedInput, subject,
                             "predecessor predicate has no predecessor step");
      }
      const auto it = progress.find(predicate.predecessor);
      if (it == progress.end()) {
        return failure<bool>(ErrorCode::UnknownStep, subject,
                             "predecessor step is not part of this plan");
      }
      return success(it->second == StepStatus::Verified);
    }

    case PredicateKind::PredecessorNotVerified: {
      if (!predicate.predecessor.valid()) {
        return failure<bool>(ErrorCode::MalformedInput, subject,
                             "predecessor predicate has no predecessor step");
      }
      const auto it = progress.find(predicate.predecessor);
      if (it == progress.end()) {
        return failure<bool>(ErrorCode::UnknownStep, subject,
                             "predecessor step is not part of this plan");
      }
      return success(it->second != StepStatus::Verified);
    }

    case PredicateKind::CapacityAvailableAtLeast: {
      if (!predicate.asset.valid()) {
        return failure<bool>(ErrorCode::MalformedInput, subject,
                             "capacity predicate has no asset");
      }
      const AssetRecord* record = snapshot.find(predicate.asset);
      if (record == nullptr) {
        return failure<bool>(ErrorCode::UnknownIdentity, subject,
                             "asset is not present in the facility snapshot");
      }
      return success(record->capacity_available_units() >= predicate.expected_value);
    }

    case PredicateKind::NoTenantAssigned: {
      if (!predicate.asset.valid()) {
        return failure<bool>(ErrorCode::MalformedInput, subject, "predicate has no asset");
      }
      const AssetRecord* record = snapshot.find(predicate.asset);
      if (record == nullptr) {
        return failure<bool>(ErrorCode::UnknownIdentity, subject,
                             "asset is not present in the facility snapshot");
      }
      return success(!record->has_tenant());
    }

    case PredicateKind::AssetLifecycleIs:
    case PredicateKind::PowerStateIs:
    case PredicateKind::CoolingStateIs:
    case PredicateKind::MaintenanceStateIs:
    case PredicateKind::NetworkStateIs:
    case PredicateKind::WorkloadStateIs: {
      if (!predicate.asset.valid()) {
        return failure<bool>(ErrorCode::MalformedInput, subject,
                             "state predicate has no asset");
      }
      const AssetRecord* record = snapshot.find(predicate.asset);
      if (record == nullptr) {
        return failure<bool>(ErrorCode::UnknownIdentity, subject,
                             "asset is not present in the facility snapshot");
      }
      auto domain_ok = [&](auto maximum) {
        return state_in_domain(predicate.expected_state, maximum);
      };
      switch (predicate.kind) {
        case PredicateKind::AssetLifecycleIs: {
          if (!domain_ok(kAssetLifecycleMax)) {
            return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                                 "expected lifecycle is outside the enum domain");
          }
          // A known target state is never satisfied by Unknown.
          return success(static_cast<std::uint8_t>(record->lifecycle) ==
                         predicate.expected_state);
        }
        case PredicateKind::PowerStateIs: {
          if (!domain_ok(kPowerStateMax)) {
            return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                                 "expected power state is outside the enum domain");
          }
          return success(static_cast<std::uint8_t>(record->power) == predicate.expected_state);
        }
        case PredicateKind::CoolingStateIs: {
          if (!domain_ok(kCoolingStateMax)) {
            return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                                 "expected cooling state is outside the enum domain");
          }
          return success(static_cast<std::uint8_t>(record->cooling) == predicate.expected_state);
        }
        case PredicateKind::MaintenanceStateIs: {
          if (!domain_ok(kMaintenanceStateMax)) {
            return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                                 "expected maintenance state is outside the enum domain");
          }
          return success(static_cast<std::uint8_t>(record->maintenance) ==
                         predicate.expected_state);
        }
        case PredicateKind::NetworkStateIs: {
          if (!domain_ok(kNetworkStateMax)) {
            return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                                 "expected network state is outside the enum domain");
          }
          return success(static_cast<std::uint8_t>(record->network) == predicate.expected_state);
        }
        case PredicateKind::WorkloadStateIs: {
          if (!domain_ok(kWorkloadStateMax)) {
            return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                                 "expected workload state is outside the enum domain");
          }
          return success(static_cast<std::uint8_t>(record->workloads) ==
                         predicate.expected_state);
        }
        default:
          break;
      }
      return failure<bool>(ErrorCode::InternalError, subject, "unreachable predicate branch");
    }

    case PredicateKind::AssetLifecycleGenerationAtLeast: {
      if (!predicate.asset.valid()) {
        return failure<bool>(ErrorCode::MalformedInput, subject,
                             "generation predicate has no asset");
      }
      const AssetRecord* record = snapshot.find(predicate.asset);
      if (record == nullptr) {
        return failure<bool>(ErrorCode::UnknownIdentity, subject,
                             "asset is not present in the facility snapshot");
      }
      return success(record->lifecycle_generation.value() >= predicate.expected_value);
    }

    default:
      return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                           "precondition kind is outside the enum domain");
  }
}

Result<bool> evaluate_effect(const EffectSpec& effect, const FacilitySnapshot& snapshot) {
  const std::string subject = effect.asset.valid() ? effect.asset.to_string() : "effect";
  if (!is_valid(effect.kind)) {
    return failure<bool>(ErrorCode::InvalidEnumValue, subject, "effect kind is unset");
  }
  if (!effect.asset.valid()) {
    return failure<bool>(ErrorCode::MalformedInput, subject, "effect has no asset");
  }
  const AssetRecord* record = snapshot.find(effect.asset);
  if (record == nullptr) {
    return failure<bool>(ErrorCode::UnknownIdentity, subject,
                         "asset is not present in the facility snapshot");
  }
  const auto state = static_cast<std::uint32_t>(effect.expected_state);
  switch (effect.kind) {
    case EffectKind::WorkloadStateChange:
      if (!state_in_domain(state, kWorkloadStateMax)) {
        return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                             "expected workload state is outside the enum domain");
      }
      return success(static_cast<std::uint32_t>(record->workloads) == state);
    case EffectKind::MaintenanceStateChange:
      if (!state_in_domain(state, kMaintenanceStateMax)) {
        return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                             "expected maintenance state is outside the enum domain");
      }
      return success(static_cast<std::uint32_t>(record->maintenance) == state);
    case EffectKind::PowerStateChange:
      if (!state_in_domain(state, kPowerStateMax)) {
        return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                             "expected power state is outside the enum domain");
      }
      return success(static_cast<std::uint32_t>(record->power) == state);
    case EffectKind::CoolingStateChange:
      if (!state_in_domain(state, kCoolingStateMax)) {
        return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                             "expected cooling state is outside the enum domain");
      }
      return success(static_cast<std::uint32_t>(record->cooling) == state);
    case EffectKind::CapacityChange:
      return success(record->capacity_reserved_units ==
                     static_cast<std::uint32_t>(effect.expected_value));
    case EffectKind::NetworkStateChange:
      if (!state_in_domain(state, kNetworkStateMax)) {
        return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                             "expected network state is outside the enum domain");
      }
      return success(static_cast<std::uint32_t>(record->network) == state);
    case EffectKind::LifecycleStateChange:
      if (!state_in_domain(state, kAssetLifecycleMax)) {
        return failure<bool>(ErrorCode::InvalidEnumValue, subject,
                             "expected lifecycle state is outside the enum domain");
      }
      return success(static_cast<std::uint32_t>(record->lifecycle) == state);
    case EffectKind::HardwareGenerationChange:
      return success(record->hardware_generation.value() == effect.expected_value);
    case EffectKind::FirmwareGenerationChange:
      return success(record->firmware_generation.value() == effect.expected_value);
    case EffectKind::RestorationVerified:
      return success(record->lifecycle == AssetLifecycle::Active &&
                     record->power == PowerState::On && record->cooling == CoolingState::Normal &&
                     record->network == NetworkState::Attached &&
                     record->maintenance == MaintenanceState::None &&
                     (record->workloads == WorkloadState::Present ||
                      record->workloads == WorkloadState::Absent));
    case EffectKind::NoOpRecorded:
      return success(true);
    case EffectKind::Unspecified:
    default:
      return failure<bool>(ErrorCode::InvalidEnumValue, subject, "effect kind is unset");
  }
}

Result<AssetRecord> project_action(const AssetRecord& before, const ActionRequest& request) {
  if (!request.complete()) {
    return failure<AssetRecord>(ErrorCode::MalformedInput, before.id.to_string(),
                                "action request is incomplete");
  }
  if (before.id != request.asset) {
    return failure<AssetRecord>(ErrorCode::ImpossibleCombination, before.id.to_string(),
                                "action request targets a different asset");
  }
  // Every generation this model can advance is checked before any of them is
  // advanced. An unsigned wrap would produce a zero generation, which is an
  // unset generation, which would in turn make the expected effect of the step
  // agree with a wrapped result. Exhausted generations are therefore refused
  // outright rather than wrapped.
  constexpr std::uint64_t kGenerationCeiling = (std::numeric_limits<std::uint64_t>::max)();
  if (before.hardware_generation.value() == kGenerationCeiling ||
      before.firmware_generation.value() == kGenerationCeiling ||
      before.lifecycle_generation.value() == kGenerationCeiling ||
      before.maintenance_generation.value() == kGenerationCeiling ||
      before.capacity_generation.value() == kGenerationCeiling) {
    return failure<AssetRecord>(
        ErrorCode::BoundedLimitExceeded, before.id.to_string(),
        "an asset whose generations are exhausted cannot be changed: advancing one would wrap");
  }
  AssetRecord after = before;
  switch (request.kind) {
    case ActionKind::DrainWorkloads:
      after.workloads = WorkloadState::Drained;
      after.lifecycle = AssetLifecycle::Draining;
      after.lifecycle_generation = LifecycleGeneration(before.lifecycle_generation.value() + 1);
      break;
    case ActionKind::RestoreWorkloads:
      after.workloads = WorkloadState::Present;
      after.lifecycle = AssetLifecycle::Active;
      after.lifecycle_generation = LifecycleGeneration(before.lifecycle_generation.value() + 1);
      break;
    case ActionKind::MaintenanceIsolate:
      after.maintenance = MaintenanceState::Isolated;
      after.lifecycle = AssetLifecycle::Maintenance;
      after.maintenance_generation =
          MaintenanceGeneration(before.maintenance_generation.value() + 1);
      after.lifecycle_generation = LifecycleGeneration(before.lifecycle_generation.value() + 1);
      break;
    case ActionKind::MaintenanceRelease:
      after.maintenance = MaintenanceState::None;
      after.maintenance_generation =
          MaintenanceGeneration(before.maintenance_generation.value() + 1);
      break;
    case ActionKind::PowerDownAsset:
      after.power = PowerState::Off;
      break;
    case ActionKind::PowerUpAsset:
      after.power = PowerState::On;
      break;
    case ActionKind::CoolingIsolate:
      after.cooling = CoolingState::Isolated;
      break;
    case ActionKind::CoolingRestore:
      after.cooling = CoolingState::Normal;
      break;
    case ActionKind::CapacityReserve:
      if (request.target_value > after.capacity_available_units()) {
        return failure<AssetRecord>(ErrorCode::PreconditionUnsatisfied, before.id.to_string(),
                                    "requested reservation exceeds available capacity");
      }
      after.capacity_reserved_units = static_cast<std::uint32_t>(
          after.capacity_reserved_units + request.target_value);
      after.capacity_generation = CapacityGeneration(before.capacity_generation.value() + 1);
      break;
    case ActionKind::CapacityRelease:
      if (request.target_value > after.capacity_reserved_units) {
        return failure<AssetRecord>(ErrorCode::PreconditionUnsatisfied, before.id.to_string(),
                                    "requested release exceeds reserved capacity");
      }
      after.capacity_reserved_units =
          static_cast<std::uint32_t>(after.capacity_reserved_units - request.target_value);
      after.capacity_generation = CapacityGeneration(before.capacity_generation.value() + 1);
      break;
    case ActionKind::NetworkQuiesce:
      after.network = NetworkState::Quiesced;
      break;
    case ActionKind::NetworkRestore:
      after.network = NetworkState::Attached;
      break;
    case ActionKind::LifecycleTransition: {
      if (!state_in_domain(request.target_state, kAssetLifecycleMax)) {
        return failure<AssetRecord>(ErrorCode::InvalidEnumValue, before.id.to_string(),
                                    "lifecycle transition target is outside the enum domain");
      }
      after.lifecycle = static_cast<AssetLifecycle>(request.target_state);
      after.lifecycle_generation = LifecycleGeneration(before.lifecycle_generation.value() + 1);
      break;
    }
    case ActionKind::HardwareSwap:
      after.hardware_generation = HardwareGeneration(before.hardware_generation.value() + 1);
      after.lifecycle = AssetLifecycle::Installed;
      after.power = PowerState::Off;
      after.workloads = WorkloadState::Absent;
      after.lifecycle_generation = LifecycleGeneration(before.lifecycle_generation.value() + 1);
      break;
    case ActionKind::FirmwareStage:
      after.firmware_generation = FirmwareGeneration(before.firmware_generation.value() + 1);
      break;
    case ActionKind::Recommission:
      after.lifecycle = AssetLifecycle::Active;
      after.lifecycle_generation = LifecycleGeneration(before.lifecycle_generation.value() + 1);
      break;
    case ActionKind::Decommission:
      after.lifecycle = AssetLifecycle::Decommissioned;
      after.tenant = TenantId{};
      after.capacity_reserved_units = 0;
      after.lifecycle_generation = LifecycleGeneration(before.lifecycle_generation.value() + 1);
      break;
    case ActionKind::VerifyRestoration:
    case ActionKind::RecordNoOp:
      break;
    case ActionKind::Unspecified:
    default:
      return failure<AssetRecord>(ErrorCode::InvalidEnumValue, before.id.to_string(),
                                  "action kind is unset");
  }
  return success(after);
}

}  // namespace fco
