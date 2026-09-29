#include "fco/sim.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fco {
namespace {

// Byte key of a domain inside the plant's per-domain tables.
[[nodiscard]] constexpr std::uint8_t domain_index(DomainKind domain) noexcept {
  return static_cast<std::uint8_t>(domain);
}

// An unset incarnation and incarnation 0 share one bucket: the plant never
// invents an incarnation, it only sequences under the one it was told about.
[[nodiscard]] constexpr std::uint64_t incarnation_index(IncarnationId incarnation) noexcept {
  return incarnation.valid() ? incarnation.value() : 0u;
}

// content_digest construction, exactly as documented in the header:
//   SHA-256("fco/simulated-observation/1" || u8(domain) || record.digest().bytes())
// record.digest() is the canonical fco/asset-record/1 encoding from
// src/facility.cpp, so the result changes whenever the resulting record changes,
// and two domains producing the same record still differ.
[[nodiscard]] Digest simulated_content_digest(DomainKind domain, const AssetRecord& record) {
  const Digest record_digest = record.digest();
  Sha256 hasher;
  hasher.update("fco/simulated-observation/1");
  hasher.update_u8(domain_index(domain));
  hasher.update(record_digest.bytes().data(), record_digest.bytes().size());
  return hasher.finish();
}

[[nodiscard]] std::string asset_subject(const ActionRequest& request) {
  return request.asset.valid() ? request.asset.to_string() : std::string("asset");
}

[[nodiscard]] std::string domain_name(DomainKind domain) {
  return std::string(to_string(domain));
}

}  // namespace

// ---------------------------------------------------------------------------
// SimulatedPlant
// ---------------------------------------------------------------------------

SimulatedPlant::SimulatedPlant(FacilitySnapshot initial)
    : snapshot_(std::move(initial)), generations_(snapshot_.generations()) {}

const FacilitySnapshot& SimulatedPlant::snapshot() const noexcept { return snapshot_; }

GenerationSet SimulatedPlant::generations() const noexcept { return generations_; }

FacilityRevision SimulatedPlant::revision() const noexcept { return revision_; }

std::uint64_t SimulatedPlant::applied_actions() const noexcept { return applied_actions_; }

bool SimulatedPlant::has_applied(const Digest& idempotency_key) const noexcept {
  return applied_.find(idempotency_key) != applied_.end();
}

Result<ActionOutcome> SimulatedPlant::apply(const ActionRequest& request, const AttemptId& attempt,
                                            const Digest& idempotency_key,
                                            std::uint64_t now_micros) {
  const std::string subject = asset_subject(request);

  // Validation follows the documented ErrorCode precedence: input shape first,
  // then identity, then authority, then the idempotency conflict.
  if (!idempotency_key.valid()) {
    return failure<ActionOutcome>(
        ErrorCode::MalformedInput, "idempotency-key",
        "an unset idempotency key cannot be deduplicated; no action was applied");
  }
  if (!is_valid(request.kind)) {
    return failure<ActionOutcome>(ErrorCode::InvalidEnumValue, subject,
                                  "action request carries no action kind");
  }
  if (!request.asset.valid()) {
    return failure<ActionOutcome>(ErrorCode::UnknownIdentity, "asset",
                                  "action request carries no asset identity");
  }
  const AssetRecord* current = snapshot_.find(request.asset);
  if (current == nullptr) {
    return failure<ActionOutcome>(ErrorCode::UnknownIdentity, subject,
                                  "asset is not present in the simulated plant");
  }
  if (!is_valid(request.owner) || owning_domain(request.kind) != request.owner) {
    return failure<ActionOutcome>(
        ErrorCode::AuthorityMismatch, subject,
        "action '" + std::string(to_string(request.kind)) + "' is owned by '" +
            std::string(to_string(owning_domain(request.kind))) + "' but the request names '" +
            std::string(to_string(request.owner)) + "'");
  }

  // A recorded application is a fact, so it is answered from the record before
  // the unreliable channel is modelled: no duplicate walks the fault path.
  const Digest request_digest = request.digest();
  const auto recorded = applied_.find(idempotency_key);
  if (recorded != applied_.end()) {
    if (recorded->second.request_digest != request_digest) {
      return failure<ActionOutcome>(
          ErrorCode::IdempotencyConflict, subject,
          "idempotency key was already applied to a different request; refusing to report the "
          "original observation as the answer to this one");
    }
    ActionOutcome replay = recorded->second.outcome;
    replay.outcome = SubmitOutcome::DuplicateOfAppliedRequest;
    replay.evidence = EvidenceOutcome::EffectObserved;
    replay.detail = std::string(kSimulatedProvenance) +
                    " duplicate of an applied request; the effect was applied exactly once at " +
                    std::to_string(replay.observed_at_micros) + " micros, replayed by attempt '" +
                    attempt.to_string() + "'";
    return success(std::move(replay));
  }

  DomainFault& fault = faults_[domain_index(request.owner)];

  if (fault.has_pending_failure) {
    const SubmitOutcome injected = fault.pending_outcome;
    std::string injected_detail = fault.pending_detail;
    fault.has_pending_failure = false;
    fault.pending_outcome = SubmitOutcome::Unspecified;
    fault.pending_detail.clear();
    if (!is_valid(injected)) {
      return failure<ActionOutcome>(
          ErrorCode::InvalidEnumValue, subject,
          "injected submit outcome is outside the SubmitOutcome domain; injection consumed: " +
              injected_detail);
    }
    if (injected == SubmitOutcome::Accepted ||
        injected == SubmitOutcome::DuplicateOfAppliedRequest) {
      return failure<ActionOutcome>(
          ErrorCode::ImpossibleCombination, subject,
          std::string(kSimulatedProvenance) + " fail_next injection for '" +
              domain_name(request.owner) + "' is not meaningful (" +
              std::string(to_string(injected)) + " with nothing applied); injection consumed: " +
              injected_detail);
    }
    const EvidenceOutcome evidence = injected == SubmitOutcome::Rejected
                                         ? EvidenceOutcome::ActionRejected
                                         : EvidenceOutcome::Indeterminate;
    return success(unapplied_outcome(injected, evidence, std::move(injected_detail), now_micros,
                                     current));
  }

  if (fault.stalled) {
    return success(unapplied_outcome(
        SubmitOutcome::Indeterminate, EvidenceOutcome::Indeterminate,
        std::string(kSimulatedProvenance) + " domain '" + domain_name(request.owner) +
            "' is stalled: the request was transmitted and no response was received; nothing was "
            "applied",
        now_micros, current));
  }

  Result<AssetRecord> projected = project_action(*current, request);
  if (!projected.ok()) return projected.error();
  const AssetRecord after = projected.take();

  snapshot_.apply(after);
  const GenerationKind advanced = generation_advanced_by(request.kind);
  if (is_valid(advanced)) {
    generations_.set(advanced, generations_.get(advanced) + 1u);
  }
  snapshot_.set_generations(generations_);
  revision_ = FacilityRevision(revision_.value() + 1u);
  ++applied_actions_;

  ActionOutcome outcome;
  outcome.outcome = SubmitOutcome::Accepted;
  outcome.observation_sequence = advance_sequence(request.owner);
  outcome.observed_generations = generations_;
  outcome.observed_hardware_generation = after.hardware_generation;
  outcome.observed_firmware_generation = after.firmware_generation;
  outcome.evidence = EvidenceOutcome::EffectObserved;
  outcome.content_digest = simulated_content_digest(request.owner, after);
  outcome.observed_at_micros = now_micros;
  outcome.has_observed_asset = true;
  outcome.observed_asset = after;
  outcome.detail = std::string(kSimulatedProvenance) + " application of '" +
                   std::string(to_string(request.kind)) + "' on '" + request.asset.to_string() +
                   "' via '" + domain_name(request.owner) + "' for attempt '" +
                   attempt.to_string() + "'";

  applied_.insert_or_assign(idempotency_key,
                            AppliedEntry{request_digest, request.owner,
                                         incarnation_for(request.owner), outcome});

  if (fault.observation_lag) {
    // Acknowledged, not yet observed: the effect really happened and the record
    // above is the observation the engine will collect when it replays the key,
    // but this response carries no sequence, no content digest and no record.
    ActionOutcome lagged = outcome;
    lagged.evidence = EvidenceOutcome::Indeterminate;
    lagged.observation_sequence = ObservationSequence{};
    lagged.content_digest = Digest{};
    lagged.has_observed_asset = false;
    lagged.observed_asset = AssetRecord{};
    lagged.detail = std::string(kSimulatedProvenance) + " domain '" + domain_name(request.owner) +
                    "' acknowledged the request but has not yet observed the effect; replay the "
                    "idempotency key for the recorded observation";
    return success(std::move(lagged));
  }

  return success(std::move(outcome));
}

void SimulatedPlant::fail_next(DomainKind domain, SubmitOutcome outcome, std::string detail) {
  if (!is_valid(domain)) return;  // a fault that cannot be addressed is not recorded
  DomainFault& fault = faults_[domain_index(domain)];
  fault.has_pending_failure = true;
  fault.pending_outcome = outcome;
  fault.pending_detail = std::move(detail);
}

void SimulatedPlant::stall_domain(DomainKind domain, bool stalled) {
  if (!is_valid(domain)) return;
  faults_[domain_index(domain)].stalled = stalled;
}

void SimulatedPlant::set_observation_lag(DomainKind domain, bool lag) {
  if (!is_valid(domain)) return;
  faults_[domain_index(domain)].observation_lag = lag;
}

void SimulatedPlant::seed_observation_sequence(DomainKind domain, std::uint64_t next) {
  // The watermark the next advance moves past. A next value below 1 is ignored,
  // and so is a seed that would move the watermark backwards: the plant only
  // ever reports a sequence it has not reported before.
  if (!is_valid(domain) || next == 0) return;
  const std::uint64_t watermark = next - 1u;
  if (watermark == 0) return;  // the next reported sequence is already at least 1
  const auto key = sequence_key(domain);
  const auto found = sequences_.find(key);
  const std::uint64_t current =
      (found == sequences_.end() || !found->second.valid()) ? 0u : found->second.value();
  if (watermark > current) sequences_.insert_or_assign(key, ObservationSequence(watermark));
}

std::uint64_t SimulatedPlant::next_observation_sequence(DomainKind domain) const noexcept {
  if (!is_valid(domain)) return 0;
  const auto found = sequences_.find(sequence_key(domain));
  if (found == sequences_.end() || !found->second.valid()) return 1;
  return found->second.value() + 1u;
}

void SimulatedPlant::force_generation(GenerationKind kind, std::uint64_t value) {
  // An authority generation move, not a facility change: the facility revision
  // and the applied-action count are untouched. A zero value or an unspecified
  // kind is ignored so the generation set always stays complete and valid.
  if (!is_valid(kind) || value == 0) return;
  generations_.set(kind, value);
  snapshot_.set_generations(generations_);
}

void SimulatedPlant::force_facility_change(const AssetRecord& record) {
  // A real change that no plan caused: it advances the facility revision. It
  // touches neither generations nor the idempotency records, because no action
  // was applied through an authority.
  if (!record.complete()) return;
  snapshot_.apply(record);
  revision_ = FacilityRevision(revision_.value() + 1u);
}

std::string SimulatedPlant::describe() const {
  std::string out;
  out += "[";
  out += std::string(kSimulatedProvenance);
  out += "] simulated plant site='";
  out += snapshot_.site().to_string();
  out += "' epoch=";
  out += snapshot_.epoch().to_string();
  out += " assets=";
  out += std::to_string(snapshot_.size());
  out += " revision=";
  out += revision_.to_string();
  out += " applied-actions=";
  out += std::to_string(applied_actions_);
  out += "\n  generations: ";
  out += generations_.describe();
  for (const auto& entry : snapshot_.assets()) {
    out += "\n  asset ";
    out += entry.second.describe();
  }
  for (const auto& entry : faults_) {
    const DomainFault& fault = entry.second;
    if (!fault.stalled && !fault.observation_lag && !fault.has_pending_failure) continue;
    out += "\n  fault ";
    out += domain_name(static_cast<DomainKind>(entry.first));
    if (fault.stalled) out += " stalled";
    if (fault.observation_lag) out += " observation-lag";
    if (fault.has_pending_failure) {
      out += " fail-next=";
      out += to_string(fault.pending_outcome);
      out += " detail='";
      out += fault.pending_detail;
      out += "'";
    }
  }
  for (const auto& entry : sequences_) {
    out += "\n  sequence ";
    out += domain_name(static_cast<DomainKind>(entry.first.first));
    out += "/incarnation=";
    out += std::to_string(entry.first.second);
    out += " -> ";
    out += entry.second.to_string();
  }
  for (const auto& entry : applied_) {
    out += "\n  applied-key ";
    out += entry.first.to_hex();
    out += " domain=";
    out += domain_name(entry.second.domain);
    out += " incarnation=";
    out += entry.second.incarnation.valid() ? entry.second.incarnation.to_string()
                                            : std::string("unset");
    out += " sequence=";
    out += entry.second.outcome.observation_sequence.to_string();
    out += " asset=";
    out += entry.second.outcome.observed_asset.id.to_string();
  }
  return out;
}

void SimulatedPlant::note_incarnation(DomainKind domain, IncarnationId incarnation) {
  if (!is_valid(domain)) return;
  incarnations_.insert_or_assign(domain_index(domain), incarnation);
}

IncarnationId SimulatedPlant::incarnation_for(DomainKind domain) const noexcept {
  const auto found = incarnations_.find(domain_index(domain));
  return found == incarnations_.end() ? IncarnationId{} : found->second;
}

std::pair<std::uint8_t, std::uint64_t> SimulatedPlant::sequence_key(
    DomainKind domain) const noexcept {
  return std::make_pair(domain_index(domain), incarnation_index(incarnation_for(domain)));
}

ObservationSequence SimulatedPlant::advance_sequence(DomainKind domain) {
  const auto key = sequence_key(domain);
  const auto found = sequences_.find(key);
  const std::uint64_t next =
      (found == sequences_.end() || !found->second.valid()) ? 1u : found->second.value() + 1u;
  const ObservationSequence sequence(next);
  sequences_.insert_or_assign(key, sequence);
  return sequence;
}

ActionOutcome SimulatedPlant::unapplied_outcome(SubmitOutcome outcome, EvidenceOutcome evidence,
                                                std::string detail, std::uint64_t now_micros,
                                                const AssetRecord* current) const {
  ActionOutcome result;
  result.outcome = outcome;
  result.evidence = evidence;
  result.observed_generations = generations_;
  if (current != nullptr) {
    result.observed_hardware_generation = current->hardware_generation;
    result.observed_firmware_generation = current->firmware_generation;
  }
  result.observed_at_micros = now_micros;
  // Nothing was applied and nothing was observed: no sequence, no content
  // digest, and no observed asset record.
  result.has_observed_asset = false;
  result.detail = std::move(detail);
  return result;
}

// ---------------------------------------------------------------------------
// SimulatedAuthority
// ---------------------------------------------------------------------------

SimulatedAuthority::SimulatedAuthority(DomainKind domain, SimulatedPlant& plant,
                                       IncarnationId incarnation, ControlEpoch control_epoch)
    : domain_(domain), plant_(&plant), incarnation_(incarnation), control_epoch_(control_epoch) {
  // Register the (domain, incarnation) pair so the plant's observation sequence
  // is per domain and per incarnation, and a new incarnation starts at 1.
  plant.note_incarnation(domain, incarnation);
}

DomainKind SimulatedAuthority::domain() const noexcept { return domain_; }

IncarnationId SimulatedAuthority::incarnation() const noexcept { return incarnation_; }

ControlEpoch SimulatedAuthority::control_epoch() const noexcept { return control_epoch_; }

Result<ActionOutcome> SimulatedAuthority::submit(const ActionRequest& request,
                                                 const AttemptId& attempt,
                                                 const Digest& idempotency_key,
                                                 std::uint64_t now_micros) {
  if (plant_ == nullptr) {
    return failure<ActionOutcome>(ErrorCode::InternalError, "simulated-authority",
                                  "simulated authority is not bound to a plant");
  }
  if (!is_valid(domain_)) {
    return failure<ActionOutcome>(ErrorCode::MissingAuthority, "simulated-authority",
                                  "simulated authority has no valid domain");
  }
  if (request.owner != domain_ ||
      (is_valid(request.kind) && owning_domain(request.kind) != domain_)) {
    return failure<ActionOutcome>(
        ErrorCode::AuthorityMismatch, asset_subject(request),
        "simulated authority for '" + domain_name(domain_) + "' was asked for '" +
            std::string(to_string(request.kind)) + "' owned by '" +
            std::string(to_string(request.owner)) + "'");
  }
  return plant_->apply(request, attempt, idempotency_key, now_micros);
}

// ---------------------------------------------------------------------------
// SimulatedAuthoritySet
// ---------------------------------------------------------------------------

SimulatedAuthoritySet::SimulatedAuthoritySet(SimulatedPlant& plant, IncarnationId incarnation,
                                             ControlEpoch epoch) {
  const std::size_t count = static_cast<std::size_t>(static_cast<std::uint8_t>(kDomainKindMax));
  authorities_.reserve(count);
  for (std::uint8_t raw = 1; raw <= static_cast<std::uint8_t>(kDomainKindMax); ++raw) {
    const auto domain = static_cast<DomainKind>(raw);
    authorities_.push_back(std::make_unique<SimulatedAuthority>(domain, plant, incarnation, epoch));
  }
}

DomainAuthority* SimulatedAuthoritySet::find(DomainKind domain) noexcept {
  if (!is_valid(domain)) return nullptr;
  const std::size_t index = static_cast<std::size_t>(domain_index(domain)) - 1u;
  return index < authorities_.size() ? authorities_[index].get() : nullptr;
}

const DomainAuthority* SimulatedAuthoritySet::find(DomainKind domain) const noexcept {
  if (!is_valid(domain)) return nullptr;
  const std::size_t index = static_cast<std::size_t>(domain_index(domain)) - 1u;
  return index < authorities_.size() ? authorities_[index].get() : nullptr;
}

std::size_t SimulatedAuthoritySet::size() const noexcept { return authorities_.size(); }

// ---------------------------------------------------------------------------
// Deterministic example facility
// ---------------------------------------------------------------------------

FacilitySnapshot make_example_facility(SiteId site, FacilityEpoch epoch,
                                       GenerationSet generations) {
  constexpr std::uint32_t kCapacityTotalUnits = 64;
  constexpr std::uint32_t kCapacityReservedUnits = 16;
  constexpr std::uint64_t kHardwareGeneration = 3;
  constexpr std::uint64_t kFirmwareGeneration = 7;
  constexpr std::uint64_t kLifecycleGeneration = 2;
  constexpr std::uint64_t kMaintenanceGeneration = 1;
  constexpr std::uint64_t kCapacityGeneration = 1;

  constexpr std::array<std::string_view, 3> kRacks = {"rack-a1", "rack-a2", "rack-a3"};
  constexpr std::array<std::string_view, 3> kNodes = {"node-1", "node-2", "node-3"};

  std::vector<AssetRecord> assets;
  assets.reserve(kRacks.size() * kNodes.size());
  for (const std::string_view rack : kRacks) {
    for (const std::string_view node : kNodes) {
      AssetRecord record;
      record.id = AssetId(std::string(rack) + "-" + std::string(node));
      record.rack = RackId(std::string(rack));
      record.site = site;
      record.lifecycle = AssetLifecycle::Active;
      record.power = PowerState::On;
      record.cooling = CoolingState::Normal;
      record.maintenance = MaintenanceState::None;
      record.network = NetworkState::Attached;
      record.workloads = WorkloadState::Present;
      record.hardware_generation = HardwareGeneration(kHardwareGeneration);
      record.firmware_generation = FirmwareGeneration(kFirmwareGeneration);
      record.lifecycle_generation = LifecycleGeneration(kLifecycleGeneration);
      record.maintenance_generation = MaintenanceGeneration(kMaintenanceGeneration);
      record.capacity_generation = CapacityGeneration(kCapacityGeneration);
      record.tenant = TenantId(std::string("tenant-blue"));
      record.capacity_total_units = kCapacityTotalUnits;
      record.capacity_reserved_units = kCapacityReservedUnits;
      assets.push_back(std::move(record));
    }
  }

  Result<FacilitySnapshot> created = FacilitySnapshot::create(site, epoch, generations, std::move(assets));
  if (!created.ok()) {
    // The caller supplied a site, epoch or generation set that cannot describe a
    // facility; the deterministic assets alone cannot repair it, so the caller
    // receives a default (invalid) snapshot instead of a partial one.
    return FacilitySnapshot{};
  }
  return created.take();
}

}  // namespace fco
