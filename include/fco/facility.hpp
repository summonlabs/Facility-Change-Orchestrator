#pragma once

// Facility observation model.
//
// The snapshot is what the orchestrator believes about the facility at a
// specific facility epoch and generation set. It is an observation, never an
// authority: holding a snapshot does not grant permission to act, and a plan
// evaluated against one snapshot is fenced as soon as the observed generations
// move. Every field has an explicit "Unknown" state that is never treated as a
// healthy, ready, drained, or safe state.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "fco/authority.hpp"
#include "fco/digest.hpp"
#include "fco/domain.hpp"
#include "fco/error.hpp"
#include "fco/strong.hpp"
#include "fco/version.hpp"

namespace fco {

struct AssetRecord {
  AssetId id;
  RackId rack;
  SiteId site;
  AssetLifecycle lifecycle = AssetLifecycle::Unspecified;
  PowerState power = PowerState::Unspecified;
  CoolingState cooling = CoolingState::Unspecified;
  MaintenanceState maintenance = MaintenanceState::Unspecified;
  NetworkState network = NetworkState::Unspecified;
  WorkloadState workloads = WorkloadState::Unspecified;
  HardwareGeneration hardware_generation;
  FirmwareGeneration firmware_generation;
  LifecycleGeneration lifecycle_generation;
  MaintenanceGeneration maintenance_generation;
  CapacityGeneration capacity_generation;
  TenantId tenant;
  std::uint32_t capacity_total_units = 0;
  std::uint32_t capacity_reserved_units = 0;

  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] bool has_tenant() const noexcept { return tenant.valid(); }
  [[nodiscard]] std::uint64_t capacity_available_units() const noexcept;
  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;
  [[nodiscard]] std::string describe() const;
};

class FacilitySnapshot {
 public:
  FacilitySnapshot() = default;

  // Fails rather than partially constructing: a snapshot with a duplicate asset
  // id, an unset identity, an unspecified observation, or an incomplete
  // generation set is rejected in full.
  [[nodiscard]] static Result<FacilitySnapshot> create(SiteId site, FacilityEpoch epoch,
                                                       GenerationSet generations,
                                                       std::vector<AssetRecord> assets);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const SiteId& site() const noexcept { return site_; }
  [[nodiscard]] FacilityEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] const GenerationSet& generations() const noexcept { return generations_; }
  [[nodiscard]] std::size_t size() const noexcept { return assets_.size(); }

  [[nodiscard]] const AssetRecord* find(const AssetId& id) const noexcept;
  [[nodiscard]] std::vector<const AssetRecord*> assets_in_rack(const RackId& rack) const;
  [[nodiscard]] std::vector<AssetId> asset_ids() const;
  [[nodiscard]] const std::map<AssetId, AssetRecord>& assets() const noexcept { return assets_; }

  // Canonical digest: independent of insertion order, dependent on every
  // observed field.
  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;

  void apply(const AssetRecord& record);
  void set_generations(GenerationSet generations) { generations_ = generations; }
  void set_epoch(FacilityEpoch epoch) { epoch_ = epoch; }

 private:
  SiteId site_;
  FacilityEpoch epoch_;
  GenerationSet generations_;
  std::map<AssetId, AssetRecord> assets_;
};

// Evaluates one precondition. Returns an error when the predicate itself is
// malformed (unknown kind, unset asset, an expected state outside the domain of
// the referenced enum). A satisfied predicate returns true; an unknown or
// unmeasured observation returns false, never true.
[[nodiscard]] Result<bool> evaluate_predicate(const Predicate& predicate,
                                              const FacilitySnapshot& snapshot,
                                              const std::map<StepId, StepStatus>& progress);

// Evaluates an expected effect against a snapshot. Used by verification.
[[nodiscard]] Result<bool> evaluate_effect(const EffectSpec& effect,
                                           const FacilitySnapshot& snapshot);

// Applies the state transition implied by an action kind onto a record. This is
// the model used by the synthetic plant and by evidence application; it is the
// only place where an action is translated into an observed state change.
[[nodiscard]] Result<AssetRecord> project_action(const AssetRecord& before,
                                                 const ActionRequest& request);

}  // namespace fco
