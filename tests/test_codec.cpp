// Durable record codec tests.
//
// These pin the encoding contract of src/codec.cpp: a fully populated
// OrchestratorState survives encode -> decode field for field, encoding is a
// pure function of the state, the Writer fails instead of truncating, and the
// decoder rejects every structural violation with its documented ErrorCode. A
// successfully decoded record is always re-encodable to the exact bytes it was
// decoded from (the encoding is canonical), which is what makes "a byte flip
// either fails or yields a genuinely different, still-valid state" checkable.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fco/codec.hpp"
#include "fco/error.hpp"
#include "fco/state.hpp"
#include "fco/version.hpp"
#include "test_support.hpp"

namespace {

using fco::ActionKind;
using fco::ActionRequest;
using fco::AssetId;
using fco::AssetLifecycle;
using fco::AssetRecord;
using fco::AttemptId;
using fco::AttemptOrdinal;
using fco::AttemptRecord;
using fco::AttemptStatus;
using fco::ChangeKind;
using fco::ChangePlan;
using fco::ChangeRequestId;
using fco::ChangeStep;
using fco::CommitSequence;
using fco::ControlEpoch;
using fco::CoolingState;
using fco::Digest;
using fco::DomainKind;
using fco::EffectKind;
using fco::EffectSpec;
using fco::ErrorCode;
using fco::EvidenceId;
using fco::EvidenceOutcome;
using fco::EvidenceRecord;
using fco::FacilityEpoch;
using fco::FacilityRevision;
using fco::FacilitySnapshot;
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
using fco::ObservationSequence;
using fco::OrchestratorState;
using fco::PlanId;
using fco::PlanRevision;
using fco::PlanScope;
using fco::PlanState;
using fco::PolicyGeneration;
using fco::PolicyId;
using fco::PowerState;
using fco::Predicate;
using fco::PredicateKind;
using fco::PrincipalId;
using fco::Reader;
using fco::Revision;
using fco::SafetyClass;
using fco::Sha256;
using fco::SiteId;
using fco::StepId;
using fco::StepStatus;
using fco::TenantId;
using fco::VerificationMode;
using fco::WorkloadState;
using fco::Writer;

constexpr std::uint32_t kGenerationCount = static_cast<std::uint32_t>(fco::kGenerationKindMax);

[[nodiscard]] GenerationSet make_generations(std::uint64_t base) {
  GenerationSet set;
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    set.set(static_cast<GenerationKind>(raw), base + raw);
  }
  return set;
}

[[nodiscard]] AssetRecord make_record(const char* id, const char* rack) {
  AssetRecord record;
  record.id = AssetId(id);
  record.rack = fco::RackId(rack);
  record.site = SiteId("site-alpha");
  record.lifecycle = AssetLifecycle::Active;
  record.power = PowerState::On;
  record.cooling = CoolingState::Normal;
  record.maintenance = MaintenanceState::None;
  record.network = NetworkState::Attached;
  record.workloads = WorkloadState::Present;
  record.hardware_generation = HardwareGeneration(3);
  record.firmware_generation = FirmwareGeneration(7);
  record.lifecycle_generation = LifecycleGeneration(11);
  record.maintenance_generation = MaintenanceGeneration(5);
  record.capacity_generation = fco::CapacityGeneration(2);
  record.tenant = TenantId("tenant-blue");
  record.capacity_total_units = 64;
  record.capacity_reserved_units = 16;
  return record;
}

[[nodiscard]] ActionRequest make_request(ActionKind kind, const AssetRecord& record) {
  ActionRequest request;
  request.kind = kind;
  request.owner = fco::owning_domain(kind);
  request.asset = record.id;
  request.rack = record.rack;
  request.site = record.site;
  request.hardware_generation = record.hardware_generation;
  request.firmware_generation = record.firmware_generation;
  request.lifecycle_generation = record.lifecycle_generation;
  request.target_state = static_cast<std::uint32_t>(record.power);
  request.target_value = 4;
  return request;
}

[[nodiscard]] ChangeStep make_drain_step(const AssetRecord& record) {
  ChangeStep step;
  step.id = StepId("drain-asset-a");
  step.action = ActionKind::DrainWorkloads;
  step.owner = fco::owning_domain(step.action);
  step.safety = SafetyClass::Standard;
  step.reversibility = fco::Reversibility::Compensable;
  step.verification = VerificationMode::Required;
  step.point_of_no_return = false;
  step.request = make_request(step.action, record);
  step.compensation = make_request(ActionKind::RestoreWorkloads, record);
  Predicate predicate;
  predicate.kind = PredicateKind::WorkloadStateIs;
  predicate.asset = record.id;
  predicate.expected_state = static_cast<std::uint32_t>(WorkloadState::Drained);
  predicate.expected_value = 0;
  step.preconditions.push_back(predicate);
  EffectSpec effect;
  effect.kind = EffectKind::WorkloadStateChange;
  effect.asset = record.id;
  effect.expected_state = static_cast<std::uint32_t>(WorkloadState::Drained);
  step.expected_effect = effect;
  step.rationale = "workloads must leave before physical work";
  return step;
}

[[nodiscard]] ChangeStep make_swap_step(const AssetRecord& record) {
  ChangeStep step;
  step.id = StepId("swap-asset-b");
  step.action = ActionKind::HardwareSwap;
  step.owner = fco::owning_domain(step.action);
  step.safety = SafetyClass::Critical;
  step.reversibility = fco::Reversibility::NonReversible;
  step.verification = VerificationMode::Required;
  step.point_of_no_return = true;
  step.request = make_request(step.action, record);
  Predicate predecessor;
  predecessor.kind = PredicateKind::PredecessorVerified;
  predecessor.predecessor = StepId("drain-asset-a");
  step.preconditions.push_back(predecessor);
  Predicate power;
  power.kind = PredicateKind::PowerStateIs;
  power.asset = record.id;
  power.expected_state = static_cast<std::uint32_t>(PowerState::Off);
  step.preconditions.push_back(power);
  step.depends_on.push_back(StepId("drain-asset-a"));
  EffectSpec effect;
  effect.kind = EffectKind::HardwareGenerationChange;
  effect.asset = record.id;
  effect.expected_value = record.hardware_generation.value() + 1u;
  step.expected_effect = effect;
  step.rationale = "physical replacement is a point of no return";
  return step;
}

[[nodiscard]] AttemptRecord make_attempt(const PlanId& plan, const GenerationSet& generations) {
  AttemptRecord attempt;
  attempt.id = AttemptId("attempt-1");
  attempt.plan = plan;
  attempt.plan_revision = PlanRevision(1);
  attempt.step = StepId("drain-asset-a");
  attempt.ordinal = AttemptOrdinal(1);
  attempt.status = AttemptStatus::Verified;
  attempt.intent_digest = Sha256::of("intent-1");
  attempt.idempotency_key = Sha256::of("idempotency-1");
  attempt.bound_generations = generations;
  attempt.bound_authority.incarnation = IncarnationId(1);
  attempt.bound_authority.control_epoch = ControlEpoch(2);
  attempt.bound_authority.actor = PrincipalId("principal-ops");
  attempt.bound_authority.policy = PolicyId("policy-change-v1");
  attempt.bound_authority.policy_digest = Sha256::of("policy-change-v1");
  attempt.last_observation_sequence = ObservationSequence(4);
  attempt.last_evidence = Sha256::of("evidence-1");
  attempt.issued_at_micros = 1700000000000010;
  attempt.updated_at_micros = 1700000000000020;
  attempt.note = "issued under the drain step";
  return attempt;
}

[[nodiscard]] ChangePlan make_plan(const GenerationSet& generations,
                                   const FacilitySnapshot& facility, const AssetRecord& asset_a,
                                   const AssetRecord& asset_b) {
  ChangePlan plan;
  plan.id = PlanId("plan-1");
  plan.request_id = ChangeRequestId("request-1");
  plan.revision = PlanRevision(1);
  plan.state = PlanState::Executing;
  plan.kind = ChangeKind("rack-replacement");
  plan.site = facility.site();
  plan.intent = "replace one node in rack a1";
  plan.scope.site = facility.site();
  plan.scope.rack = fco::RackId("rack-a1");
  plan.scope.assets = {asset_a.id, asset_b.id};
  plan.facility_epoch = facility.epoch();
  plan.policy_generation = generations.policy;
  plan.bound_generations = generations;
  plan.request_digest = Sha256::of("request-1");
  plan.evidence_digest = Sha256::of("facility-observation");
  plan.facility_digest = facility.digest();
  plan.authority.incarnation = IncarnationId(1);
  plan.authority.control_epoch = ControlEpoch(2);
  plan.authority.actor = PrincipalId("principal-ops");
  plan.authority.policy = PolicyId("policy-change-v1");
  plan.authority.policy_digest = Sha256::of("policy-change-v1");
  plan.steps.push_back(make_drain_step(asset_a));
  plan.steps.push_back(make_swap_step(asset_b));
  plan.step_status.insert_or_assign(StepId("drain-asset-a"), StepStatus::Verified);
  plan.step_status.insert_or_assign(StepId("swap-asset-b"), StepStatus::Pending);
  plan.attempts.push_back(make_attempt(plan.id, generations));
  GenerationDelta delta;
  delta.kind = GenerationKind::Asi;
  delta.bound = generations.get(GenerationKind::Asi);
  delta.current = delta.bound + 1u;
  plan.self_advanced.push_back(delta);
  plan.explained_revision = FacilityRevision(9);
  plan.transition_note = "created from request request-1";
  plan.created_at_micros = 1700000000000030;
  plan.updated_at_micros = 1700000000000040;
  return plan;
}

[[nodiscard]] OrchestratorState make_full_state() {
  const GenerationSet generations = make_generations(10);
  const AssetRecord asset_a = make_record("asset-a", "rack-a1");
  const AssetRecord asset_b = make_record("asset-b", "rack-a2");
  auto facility = FacilitySnapshot::create(SiteId("site-alpha"), generations.facility_epoch,
                                           generations, {asset_a, asset_b});
  ::fco::test::check_true(facility.ok(), "fixture facility snapshot", __FILE__, __LINE__);

  OrchestratorState state;
  state.revision = Revision(5);
  state.commit_sequence = CommitSequence(6);
  state.facility_epoch = generations.facility_epoch;
  state.generations = generations;
  state.facility_revision = FacilityRevision(9);
  if (facility.ok()) state.facility = facility.value();
  state.incarnation = IncarnationId(1);
  state.control_epoch = ControlEpoch(2);
  state.actor = PrincipalId("principal-ops");
  state.policy = PolicyId("policy-change-v1");
  state.policy_digest = Sha256::of("policy-change-v1");
  state.updated_at_micros = 1700000000000050;

  const ChangePlan plan = make_plan(generations, state.facility, asset_a, asset_b);
  state.plans.insert_or_assign(plan.id, plan);

  EvidenceRecord evidence;
  evidence.id = EvidenceId("evidence-1");
  evidence.plan = plan.id;
  evidence.plan_revision = PlanRevision(1);
  evidence.step = StepId("drain-asset-a");
  evidence.attempt = AttemptId("attempt-1");
  evidence.source_domain = DomainKind::Asi;
  evidence.source_incarnation = IncarnationId(1);
  evidence.source_control_epoch = ControlEpoch(2);
  evidence.observation_sequence = ObservationSequence(4);
  evidence.observed_generations = generations;
  evidence.content_digest = Sha256::of("observation-1");
  evidence.outcome = EvidenceOutcome::EffectObserved;
  evidence.observed_at_micros = 1700000000000060;
  evidence.has_observed_asset = true;
  evidence.observed_asset = asset_a;
  evidence.observed_asset.workloads = WorkloadState::Drained;
  evidence.note = "the asi authority observed the drained workload state";
  const auto appended = state.evidence.append(evidence);
  ::fco::test::check_true(appended.ok() && appended.value(), "fixture evidence append", __FILE__,
                          __LINE__);
  return state;
}

// The fixed layout prefix of encode_state(): version (u16) + reserved (u16) +
// revision, commit sequence, facility epoch (u64 each) + the eleven-entry
// authoritative generation set (u64 each) + facility revision (u64).
constexpr std::size_t kStatePrefixBytes = 2 + 2 + 8 + 8 + 8 + (8 * 11) + 8;

[[nodiscard]] std::uint32_t load_u32_at(const std::vector<std::uint8_t>& bytes,
                                        std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4u; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + index]) << (8u * index);
  }
  return value;
}

[[nodiscard]] std::uint64_t load_u64_at(const std::vector<std::uint8_t>& bytes,
                                        std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8u; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index]) << (8u * index);
  }
  return value;
}

void store_u32_at(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4u; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>((value >> (8u * index)) & 0xFFu);
  }
}

void store_u64_at(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned index = 0; index < 8u; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>((value >> (8u * index)) & 0xFFu);
  }
}

// Decodes the fixture and reports a failure when it does not decode at all.
[[nodiscard]] fco::Result<OrchestratorState> decode_bytes(const std::vector<std::uint8_t>& bytes) {
  return fco::decode_state(bytes.data(), bytes.size());
}

// Offset of the facility site string length field, which sits immediately after
// the fixed state prefix.
[[nodiscard]] std::size_t site_length_offset() { return kStatePrefixBytes; }

// Offset of the facility asset collection count: after the site string, the
// facility epoch and the facility generation set.
[[nodiscard]] std::size_t asset_count_offset(std::size_t site_bytes) {
  return kStatePrefixBytes + 4 + site_bytes + 8 + (8 * 11);
}

}  // namespace

// ---------------------------------------------------------------------------
// Round-trip and determinism
// ---------------------------------------------------------------------------
FCO_TEST(codec, round_trip_preserves_every_field) {
  const OrchestratorState original = make_full_state();
  auto encoded = fco::encode_state_bytes(original);
  FCO_REQUIRE(encoded.ok());
  auto decoded = decode_bytes(encoded.value());
  FCO_REQUIRE(decoded.ok());
  const OrchestratorState& round = decoded.value();

  FCO_CHECK_EQ(round.digest(), original.digest());
  FCO_CHECK_EQ(round.revision.value(), original.revision.value());
  FCO_CHECK_EQ(round.commit_sequence.value(), original.commit_sequence.value());
  FCO_CHECK_EQ(round.facility_epoch.value(), original.facility_epoch.value());
  FCO_CHECK_EQ(round.facility_revision.value(), original.facility_revision.value());
  FCO_CHECK_EQ(round.incarnation.value(), original.incarnation.value());
  FCO_CHECK_EQ(round.control_epoch.value(), original.control_epoch.value());
  FCO_CHECK_EQ(round.actor.value(), original.actor.value());
  FCO_CHECK_EQ(round.policy.value(), original.policy.value());
  FCO_CHECK_EQ(round.policy_digest, original.policy_digest);
  FCO_CHECK_EQ(round.updated_at_micros, original.updated_at_micros);
  FCO_CHECK_EQ(round.generations.digest(), original.generations.digest());
  FCO_CHECK_EQ(round.authority().digest(), original.authority().digest());

  // Facility
  FCO_CHECK_EQ(round.facility.digest(), original.facility.digest());
  FCO_CHECK_EQ(round.facility.site().value(), std::string("site-alpha"));
  FCO_CHECK_EQ(round.facility.epoch().value(), original.facility.epoch().value());
  FCO_CHECK_EQ(round.facility.size(), original.facility.size());
  const AssetRecord* round_asset = round.facility.find(AssetId("asset-a"));
  const AssetRecord* original_asset = original.facility.find(AssetId("asset-a"));
  FCO_REQUIRE(round_asset != nullptr);
  FCO_REQUIRE(original_asset != nullptr);
  FCO_CHECK_EQ(round_asset->digest(), original_asset->digest());
  FCO_CHECK_EQ(round_asset->rack.value(), std::string("rack-a1"));
  FCO_CHECK_EQ(round_asset->tenant.value(), std::string("tenant-blue"));
  FCO_CHECK_EQ(round_asset->capacity_total_units, std::uint32_t{64});
  FCO_CHECK_EQ(round_asset->capacity_reserved_units, std::uint32_t{16});
  FCO_CHECK_EQ(round_asset->hardware_generation.value(), std::uint64_t{3});
  FCO_CHECK_EQ(round_asset->firmware_generation.value(), std::uint64_t{7});
  FCO_CHECK_EQ(round_asset->maintenance_generation.value(), std::uint64_t{5});
  FCO_CHECK_EQ(round_asset->capacity_generation.value(), std::uint64_t{2});

  // Plans
  FCO_CHECK_EQ(round.plans.size(), original.plans.size());
  for (const auto& entry : original.plans) {
    const ChangePlan* other = round.find_plan(entry.first);
    FCO_REQUIRE(other != nullptr);
    FCO_CHECK_EQ(other->digest(), entry.second.digest());
    FCO_CHECK_EQ(other->request_id.value(), entry.second.request_id.value());
    FCO_CHECK_EQ(other->revision.value(), entry.second.revision.value());
    FCO_CHECK_EQ(other->state, entry.second.state);
    FCO_CHECK_EQ(other->kind.value(), entry.second.kind.value());
    FCO_CHECK_EQ(other->intent, entry.second.intent);
    FCO_CHECK_EQ(other->facility_epoch.value(), entry.second.facility_epoch.value());
    FCO_CHECK_EQ(other->policy_generation.value(), entry.second.policy_generation.value());
    FCO_CHECK_EQ(other->request_digest, entry.second.request_digest);
    FCO_CHECK_EQ(other->evidence_digest, entry.second.evidence_digest);
    FCO_CHECK_EQ(other->facility_digest, entry.second.facility_digest);
    FCO_CHECK_EQ(other->authority.digest(), entry.second.authority.digest());
    FCO_CHECK_EQ(other->explained_revision.value(), entry.second.explained_revision.value());
    FCO_CHECK_EQ(other->transition_note, entry.second.transition_note);
    FCO_CHECK_EQ(other->created_at_micros, entry.second.created_at_micros);
    FCO_CHECK_EQ(other->updated_at_micros, entry.second.updated_at_micros);
    FCO_CHECK_EQ(other->scope.rack.value(), entry.second.scope.rack.value());
    FCO_CHECK_EQ(other->scope.assets.size(), entry.second.scope.assets.size());

    FCO_CHECK_EQ(other->steps.size(), entry.second.steps.size());
    for (std::size_t index = 0; index < entry.second.steps.size(); ++index) {
      const ChangeStep& expected_step = entry.second.steps[index];
      const ChangeStep& actual_step = other->steps[index];
      FCO_CHECK_EQ(actual_step.digest(), expected_step.digest());
      // rationale is deliberately not hashed by ChangeStep::hash_into.
      FCO_CHECK_EQ(actual_step.rationale, expected_step.rationale);
      FCO_CHECK_EQ(actual_step.point_of_no_return, expected_step.point_of_no_return);
      FCO_CHECK_EQ(actual_step.preconditions.size(), expected_step.preconditions.size());
      FCO_CHECK_EQ(actual_step.depends_on.size(), expected_step.depends_on.size());
      FCO_CHECK_EQ(actual_step.compensation.has_value(), expected_step.compensation.has_value());
    }

    FCO_CHECK_EQ(other->step_status.size(), entry.second.step_status.size());
    for (const auto& status : entry.second.step_status) {
      const auto found = other->step_status.find(status.first);
      FCO_REQUIRE(found != other->step_status.end());
      FCO_CHECK_EQ(found->second, status.second);
    }

    FCO_CHECK_EQ(other->attempts.size(), entry.second.attempts.size());
    for (std::size_t index = 0; index < entry.second.attempts.size(); ++index) {
      const AttemptRecord& expected_attempt = entry.second.attempts[index];
      const AttemptRecord& actual_attempt = other->attempts[index];
      FCO_CHECK_EQ(actual_attempt.digest(), expected_attempt.digest());
      // note is deliberately not hashed by AttemptRecord::hash_into.
      FCO_CHECK_EQ(actual_attempt.note, expected_attempt.note);
      FCO_CHECK_EQ(actual_attempt.last_observation_sequence.value(),
                   expected_attempt.last_observation_sequence.value());
      FCO_CHECK_EQ(actual_attempt.bound_authority.digest(),
                   expected_attempt.bound_authority.digest());
      FCO_CHECK_EQ(actual_attempt.bound_generations.digest(),
                   expected_attempt.bound_generations.digest());
    }

    FCO_CHECK_EQ(other->self_advanced.size(), entry.second.self_advanced.size());
    for (std::size_t index = 0; index < entry.second.self_advanced.size(); ++index) {
      FCO_CHECK_EQ(other->self_advanced[index].kind, entry.second.self_advanced[index].kind);
      FCO_CHECK_EQ(other->self_advanced[index].bound, entry.second.self_advanced[index].bound);
      FCO_CHECK_EQ(other->self_advanced[index].current, entry.second.self_advanced[index].current);
    }
  }

  // Evidence
  FCO_CHECK_EQ(round.evidence.size(), original.evidence.size());
  FCO_CHECK_EQ(round.evidence.digest(), original.evidence.digest());
  const EvidenceRecord* round_evidence = round.evidence.find(EvidenceId("evidence-1"));
  FCO_REQUIRE(round_evidence != nullptr);
  FCO_CHECK_EQ(round_evidence->digest(),
               original.evidence.find(EvidenceId("evidence-1"))->digest());
  FCO_CHECK_EQ(round_evidence->note,
               original.evidence.find(EvidenceId("evidence-1"))->note);
  FCO_CHECK_EQ(round_evidence->outcome, EvidenceOutcome::EffectObserved);
  FCO_CHECK(round_evidence->has_observed_asset);
  FCO_CHECK_EQ(round_evidence->observed_asset.digest(),
               original.evidence.find(EvidenceId("evidence-1"))->observed_asset.digest());
  FCO_CHECK_EQ(round_evidence->observed_generations.digest(),
               original.evidence.find(EvidenceId("evidence-1"))->observed_generations.digest());
}

FCO_TEST(codec, encoding_is_deterministic_and_canonical) {
  const OrchestratorState state = make_full_state();
  auto first = fco::encode_state_bytes(state);
  auto second = fco::encode_state_bytes(state);
  FCO_REQUIRE(first.ok());
  FCO_REQUIRE(second.ok());
  FCO_CHECK(first.value() == second.value());

  auto decoded = decode_bytes(first.value());
  FCO_REQUIRE(decoded.ok());
  auto reencoded = fco::encode_state_bytes(decoded.value());
  FCO_REQUIRE(reencoded.ok());
  FCO_CHECK(reencoded.value() == first.value());
}

// ---------------------------------------------------------------------------
// Writer bounds
// ---------------------------------------------------------------------------
FCO_TEST(codec, writer_fails_rather_than_truncating) {
  Writer writer;
  const std::string maximum_string(static_cast<std::size_t>(fco::kMaxStringBytes), 'x');
  writer.string(maximum_string);
  FCO_CHECK(writer.status().ok());

  const std::string too_long(static_cast<std::size_t>(fco::kMaxStringBytes) + 1u, 'y');
  writer.string(too_long);
  FCO_CHECK(writer.failed());
  FCO_CHECK(!writer.status().ok());
  FCO_CHECK_ERROR(writer.status(), ErrorCode::BoundedLimitExceeded);

  // After the first failure every further write is a no-op, so a partially
  // encoded buffer can never be mistaken for a valid record.
  const std::size_t size_at_failure = writer.size();
  writer.u64(1u);
  writer.string("short");
  writer.boolean(true);
  FCO_CHECK_EQ(writer.size(), size_at_failure);
  FCO_CHECK_EQ(writer.status().error().code, ErrorCode::BoundedLimitExceeded);

  Writer counts;
  counts.count(fco::kMaxCollectionEntries, "entries");
  FCO_CHECK(counts.status().ok());
  counts.count(fco::kMaxCollectionEntries + 1u, "entries");
  FCO_CHECK_ERROR(counts.status(), ErrorCode::BoundedLimitExceeded);

  Writer reserved;
  reserved.reserve(0u, "reserved");
  FCO_CHECK(reserved.status().ok());
  Writer bad_reserved;
  bad_reserved.reserve(1u, "reserved");
  FCO_CHECK_ERROR(bad_reserved.status(), ErrorCode::ReservedFieldNonZero);

  // The whole-state encoder surfaces the same bound: an over-long plan intent
  // fails the encode instead of writing a truncated string.
  OrchestratorState state = make_full_state();
  ChangePlan* plan = state.find_plan(PlanId("plan-1"));
  FCO_REQUIRE(plan != nullptr);
  plan->intent = std::string(static_cast<std::size_t>(fco::kMaxStringBytes), 'i');
  FCO_CHECK(fco::encode_state_bytes(state).ok());
  plan->intent = std::string(static_cast<std::size_t>(fco::kMaxStringBytes) + 1u, 'i');
  FCO_CHECK_ERROR(fco::encode_state_bytes(state), ErrorCode::BoundedLimitExceeded);
}

// ---------------------------------------------------------------------------
// Decoder rejections
// ---------------------------------------------------------------------------
FCO_TEST(codec, rejects_an_empty_buffer) {
  const std::vector<std::uint8_t> empty;
  FCO_CHECK_ERROR(fco::decode_state(empty.data(), 0), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::decode_state(nullptr, 0), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::decode_state(nullptr, 64), ErrorCode::MalformedInput);
}

FCO_TEST(codec, rejects_every_truncation) {
  auto encoded = fco::encode_state_bytes(make_full_state());
  FCO_REQUIRE(encoded.ok());
  const std::vector<std::uint8_t>& bytes = encoded.value();
  FCO_CHECK(bytes.size() > 64);

  for (std::size_t length = 1; length < bytes.size(); ++length) {
    auto decoded = fco::decode_state(bytes.data(), length);
    if (decoded.ok()) {
      ::fco::test::report_failure(
          __FILE__, __LINE__,
          "a record truncated to " + std::to_string(length) + " of " +
              std::to_string(bytes.size()) + " bytes decoded successfully");
    }
  }
  // The untruncated record still decodes: the loop above is not vacuous.
  auto full = fco::decode_state(bytes.data(), bytes.size());
  FCO_CHECK(full.ok());
}

FCO_TEST(codec, rejects_trailing_bytes) {
  auto encoded = fco::encode_state_bytes(make_full_state());
  FCO_REQUIRE(encoded.ok());
  std::vector<std::uint8_t> extended = encoded.value();
  extended.push_back(0u);
  FCO_CHECK_ERROR(fco::decode_state(extended.data(), extended.size()), ErrorCode::TrailingBytes);
  extended.back() = 0xFFu;
  FCO_CHECK_ERROR(fco::decode_state(extended.data(), extended.size()), ErrorCode::TrailingBytes);
}

FCO_TEST(codec, rejects_an_unsupported_format_version) {
  auto encoded = fco::encode_state_bytes(make_full_state());
  FCO_REQUIRE(encoded.ok());

  std::vector<std::uint8_t> bumped = encoded.value();
  bumped[0] = 2u;
  bumped[1] = 0u;
  FCO_CHECK_ERROR(fco::decode_state(bumped.data(), bumped.size()),
                  ErrorCode::UnsupportedFormatVersion);

  std::vector<std::uint8_t> zeroed = encoded.value();
  zeroed[0] = 0u;
  zeroed[1] = 0u;
  FCO_CHECK_ERROR(fco::decode_state(zeroed.data(), zeroed.size()),
                  ErrorCode::UnsupportedFormatVersion);

  // The version is rejected before the reserved field is even read.
  std::vector<std::uint8_t> both = encoded.value();
  both[0] = 3u;
  both[2] = 9u;
  FCO_CHECK_ERROR(fco::decode_state(both.data(), both.size()),
                  ErrorCode::UnsupportedFormatVersion);
}

FCO_TEST(codec, rejects_a_non_zero_reserved_field) {
  auto encoded = fco::encode_state_bytes(make_full_state());
  FCO_REQUIRE(encoded.ok());
  std::vector<std::uint8_t> patched = encoded.value();
  const std::size_t reserved_offset = patched.size() - 4u;
  FCO_CHECK_EQ(load_u32_at(patched, reserved_offset), std::uint32_t{0});
  store_u32_at(patched, reserved_offset, 1u);
  FCO_CHECK_ERROR(fco::decode_state(patched.data(), patched.size()),
                  ErrorCode::ReservedFieldNonZero);

  // The other reserved field is the u16 immediately after the format version.
  std::vector<std::uint8_t> version_reserved = encoded.value();
  version_reserved[2] = 1u;
  version_reserved[3] = 0u;
  FCO_CHECK_ERROR(fco::decode_state(version_reserved.data(), version_reserved.size()),
                  ErrorCode::ReservedFieldNonZero);
}

FCO_TEST(codec, rejects_a_zero_counter) {
  const OrchestratorState valid = make_full_state();

  OrchestratorState zero_revision = valid;
  zero_revision.revision = Revision{};
  auto encoded_revision = fco::encode_state_bytes(zero_revision);
  FCO_REQUIRE(encoded_revision.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_revision.value()), ErrorCode::InvalidIdentity);

  OrchestratorState zero_sequence = valid;
  zero_sequence.commit_sequence = CommitSequence{};
  auto encoded_sequence = fco::encode_state_bytes(zero_sequence);
  FCO_REQUIRE(encoded_sequence.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_sequence.value()), ErrorCode::InvalidIdentity);

  OrchestratorState zero_epoch = valid;
  zero_epoch.facility_epoch = FacilityEpoch{};
  auto encoded_epoch = fco::encode_state_bytes(zero_epoch);
  FCO_REQUIRE(encoded_epoch.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_epoch.value()), ErrorCode::InvalidIdentity);

  OrchestratorState zero_facility_revision = valid;
  zero_facility_revision.facility_revision = FacilityRevision{};
  auto encoded_facility_revision = fco::encode_state_bytes(zero_facility_revision);
  FCO_REQUIRE(encoded_facility_revision.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_facility_revision.value()), ErrorCode::InvalidIdentity);

  OrchestratorState zero_incarnation = valid;
  zero_incarnation.incarnation = IncarnationId{};
  auto encoded_incarnation = fco::encode_state_bytes(zero_incarnation);
  FCO_REQUIRE(encoded_incarnation.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_incarnation.value()), ErrorCode::InvalidIdentity);

  OrchestratorState zero_plan_revision = valid;
  ChangePlan* plan = zero_plan_revision.find_plan(PlanId("plan-1"));
  FCO_REQUIRE(plan != nullptr);
  plan->revision = PlanRevision{};
  auto encoded_plan_revision = fco::encode_state_bytes(zero_plan_revision);
  FCO_REQUIRE(encoded_plan_revision.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_plan_revision.value()), ErrorCode::InvalidIdentity);

  OrchestratorState zero_ordinal = valid;
  ChangePlan* ordinal_plan = zero_ordinal.find_plan(PlanId("plan-1"));
  FCO_REQUIRE(ordinal_plan != nullptr);
  FCO_REQUIRE(!ordinal_plan->attempts.empty());
  ordinal_plan->attempts[0].ordinal = AttemptOrdinal{};
  auto encoded_ordinal = fco::encode_state_bytes(zero_ordinal);
  FCO_REQUIRE(encoded_ordinal.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_ordinal.value()), ErrorCode::InvalidIdentity);

  // A zero counter patched directly into the revision field behaves identically.
  auto encoded_valid = fco::encode_state_bytes(valid);
  FCO_REQUIRE(encoded_valid.ok());
  std::vector<std::uint8_t> patched = encoded_valid.value();
  store_u64_at(patched, 4, 0u);
  FCO_CHECK_ERROR(fco::decode_state(patched.data(), patched.size()), ErrorCode::InvalidIdentity);
}

FCO_TEST(codec, rejects_an_enum_outside_its_domain) {
  const OrchestratorState valid = make_full_state();

  OrchestratorState unset_action = valid;
  ChangePlan* action_plan = unset_action.find_plan(PlanId("plan-1"));
  FCO_REQUIRE(action_plan != nullptr);
  FCO_REQUIRE(!action_plan->steps.empty());
  action_plan->steps[0].action = ActionKind::Unspecified;
  auto encoded_action = fco::encode_state_bytes(unset_action);
  FCO_REQUIRE(encoded_action.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_action.value()), ErrorCode::InvalidEnumValue);

  OrchestratorState unset_lifecycle = valid;
  const AssetRecord* record = unset_lifecycle.facility.find(AssetId("asset-a"));
  FCO_REQUIRE(record != nullptr);
  AssetRecord patched_record = *record;
  patched_record.lifecycle = AssetLifecycle::Unspecified;
  unset_lifecycle.facility.apply(patched_record);
  auto encoded_lifecycle = fco::encode_state_bytes(unset_lifecycle);
  FCO_REQUIRE(encoded_lifecycle.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_lifecycle.value()), ErrorCode::InvalidEnumValue);

  OrchestratorState out_of_domain = valid;
  const AssetRecord* other = out_of_domain.facility.find(AssetId("asset-b"));
  FCO_REQUIRE(other != nullptr);
  AssetRecord out_of_domain_record = *other;
  out_of_domain_record.cooling = static_cast<CoolingState>(200);
  out_of_domain.facility.apply(out_of_domain_record);
  auto encoded_out_of_domain = fco::encode_state_bytes(out_of_domain);
  FCO_REQUIRE(encoded_out_of_domain.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_out_of_domain.value()), ErrorCode::InvalidEnumValue);

  OrchestratorState unset_plan_state = valid;
  ChangePlan* state_plan = unset_plan_state.find_plan(PlanId("plan-1"));
  FCO_REQUIRE(state_plan != nullptr);
  state_plan->state = PlanState::Unspecified;
  auto encoded_plan_state = fco::encode_state_bytes(unset_plan_state);
  FCO_REQUIRE(encoded_plan_state.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_plan_state.value()), ErrorCode::InvalidEnumValue);

  // decode_enum is the single gate every enum field passes through.
  FCO_CHECK_ERROR(fco::decode_enum<AssetLifecycle>(0u, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<AssetLifecycle>(13u, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<AssetLifecycle>(255u, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK(fco::decode_enum<AssetLifecycle>(12u, "field").ok());
  FCO_CHECK_ERROR(fco::decode_enum<GenerationKind>(0u, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<GenerationKind>(12u, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<EvidenceOutcome>(5u, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<AttemptStatus>(0u, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<StepStatus>(12u, "field"), ErrorCode::InvalidEnumValue);
}

FCO_TEST(codec, rejects_a_non_canonical_boolean) {
  const std::uint8_t two = 2u;
  Reader reader(&two, 1);
  FCO_CHECK_ERROR(reader.boolean("flag"), ErrorCode::MalformedInput);

  const std::uint8_t zero = 0u;
  Reader zero_reader(&zero, 1);
  auto flag = zero_reader.boolean("flag");
  FCO_REQUIRE(flag.ok());
  FCO_CHECK(!flag.value());

  const std::uint8_t one = 1u;
  Reader one_reader(&one, 1);
  auto set = one_reader.boolean("flag");
  FCO_REQUIRE(set.ok());
  FCO_CHECK(set.value());

  // A boolean field that the record ends in the middle of is MalformedInput too.
  Reader empty(nullptr, 0);
  FCO_CHECK_ERROR(empty.boolean("flag"), ErrorCode::MalformedInput);
}

FCO_TEST(codec, rejects_a_count_larger_than_the_record) {
  auto encoded = fco::encode_state_bytes(make_full_state());
  FCO_REQUIRE(encoded.ok());
  const std::vector<std::uint8_t>& bytes = encoded.value();
  const std::size_t count_offset = asset_count_offset(10u);  // "site-alpha" is ten bytes
  FCO_CHECK_EQ(load_u32_at(bytes, site_length_offset()), std::uint32_t{10});
  FCO_CHECK_EQ(load_u64_at(bytes, count_offset), std::uint64_t{2});

  std::vector<std::uint8_t> huge_count = bytes;
  store_u64_at(huge_count, count_offset, fco::kMaxCollectionEntries);
  FCO_CHECK_ERROR(fco::decode_state(huge_count.data(), huge_count.size()),
                  ErrorCode::MalformedInput);

  std::vector<std::uint8_t> above_bound = bytes;
  store_u64_at(above_bound, count_offset, fco::kMaxCollectionEntries + 1u);
  FCO_CHECK_ERROR(fco::decode_state(above_bound.data(), above_bound.size()),
                  ErrorCode::BoundedLimitExceeded);

  // Reader::count is the single gate for every collection count.
  std::vector<std::uint8_t> eight(8u, 0u);
  store_u64_at(eight, 0u, 100u);
  Reader small(eight.data(), eight.size());
  FCO_CHECK_ERROR(small.count("count"), ErrorCode::MalformedInput);
  store_u64_at(eight, 0u, fco::kMaxCollectionEntries + 1u);
  Reader over(eight.data(), eight.size());
  FCO_CHECK_ERROR(over.count("count"), ErrorCode::BoundedLimitExceeded);
}

FCO_TEST(codec, rejects_a_string_length_above_the_bound) {
  auto encoded = fco::encode_state_bytes(make_full_state());
  FCO_REQUIRE(encoded.ok());
  const std::vector<std::uint8_t>& bytes = encoded.value();
  FCO_CHECK_EQ(load_u32_at(bytes, site_length_offset()), std::uint32_t{10});

  std::vector<std::uint8_t> patched = bytes;
  store_u32_at(patched, site_length_offset(),
               static_cast<std::uint32_t>(fco::kMaxStringBytes + 1u));
  FCO_CHECK_ERROR(fco::decode_state(patched.data(), patched.size()),
                  ErrorCode::BoundedLimitExceeded);

  // A length inside the string bound but past the end of the record is
  // MalformedInput: the bound is not what rejects it, the record length is.
  FCO_REQUIRE(bytes.size() + 8u <= static_cast<std::size_t>(fco::kMaxStringBytes));
  std::vector<std::uint8_t> truncated_length = bytes;
  store_u32_at(truncated_length, site_length_offset(),
               static_cast<std::uint32_t>(bytes.size() + 8u));
  FCO_CHECK_ERROR(fco::decode_state(truncated_length.data(), truncated_length.size()),
                  ErrorCode::MalformedInput);

  std::vector<std::uint8_t> eight(8u, 0u);
  store_u32_at(eight, 0u, 100u);  // a length the record cannot possibly hold
  Reader reader(eight.data(), eight.size());
  FCO_CHECK_ERROR(reader.string("field"), ErrorCode::MalformedInput);
}

FCO_TEST(codec, byte_flips_are_either_rejected_or_canonical) {
  auto encoded = fco::encode_state_bytes(make_full_state());
  FCO_REQUIRE(encoded.ok());
  const std::vector<std::uint8_t>& bytes = encoded.value();

  std::size_t examined = 0;
  for (std::size_t offset = 0; offset < bytes.size(); offset += 97u) {
    ++examined;
    std::vector<std::uint8_t> flipped = bytes;
    flipped[offset] = static_cast<std::uint8_t>(flipped[offset] ^ 0x5Au);
    auto decoded = fco::decode_state(flipped.data(), flipped.size());
    if (!decoded.ok()) continue;  // rejected: the documented outcome
    auto reencoded = fco::encode_state_bytes(decoded.value());
    if (!reencoded.ok()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "a decoded byte flip at offset " + std::to_string(offset) +
                                      " could not be re-encoded");
      continue;
    }
    if (reencoded.value() != flipped) {
      ::fco::test::report_failure(
          __FILE__, __LINE__,
          "a byte flip at offset " + std::to_string(offset) +
              " decoded into a state that does not re-encode to the flipped bytes: it is "
              "neither a rejection nor a canonical state");
    }
  }
  FCO_CHECK(examined >= 3);
}

// ---------------------------------------------------------------------------
// Internal consistency
// ---------------------------------------------------------------------------
FCO_TEST(codec, rejects_disagreeing_facility_generations_and_epoch) {
  const OrchestratorState valid = make_full_state();

  OrchestratorState other_generations = valid;
  other_generations.facility.set_generations(make_generations(200));
  auto encoded_generations = fco::encode_state_bytes(other_generations);
  FCO_REQUIRE(encoded_generations.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_generations.value()), ErrorCode::ImpossibleCombination);

  OrchestratorState other_epoch = valid;
  other_epoch.facility.set_epoch(FacilityEpoch(99));
  auto encoded_epoch = fco::encode_state_bytes(other_epoch);
  FCO_REQUIRE(encoded_epoch.ok());
  FCO_CHECK_ERROR(decode_bytes(encoded_epoch.value()), ErrorCode::ImpossibleCombination);

  // Control: a state whose facility agrees with the authoritative values decodes.
  OrchestratorState agreeing = valid;
  agreeing.facility.set_epoch(agreeing.facility_epoch);
  agreeing.facility.set_generations(agreeing.generations);
  auto encoded_agreeing = fco::encode_state_bytes(agreeing);
  FCO_REQUIRE(encoded_agreeing.ok());
  FCO_CHECK(decode_bytes(encoded_agreeing.value()).ok());
}

FCO_TEST(codec, reader_helpers_enforce_their_contract) {
  std::vector<std::uint8_t> buffer(4u, 0u);
  Reader zero_reserved(buffer.data(), buffer.size());
  FCO_CHECK(zero_reserved.reserved_u32("reserved").ok());

  store_u32_at(buffer, 0u, 7u);
  Reader non_zero_reserved(buffer.data(), buffer.size());
  FCO_CHECK_ERROR(non_zero_reserved.reserved_u32("reserved"), ErrorCode::ReservedFieldNonZero);

  std::vector<std::uint8_t> trailing(5u, 0u);
  Reader ends(trailing.data(), trailing.size());
  FCO_REQUIRE(ends.u32("u32").ok());
  FCO_CHECK_ERROR(ends.expect_end("record"), ErrorCode::TrailingBytes);

  Reader exact(trailing.data(), 4u);
  FCO_REQUIRE(exact.u32("u32").ok());
  FCO_CHECK(exact.expect_end("record").ok());

  Reader short_buffer(trailing.data(), 2u);
  FCO_CHECK_ERROR(short_buffer.u32("u32"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(short_buffer.u64("u64"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(short_buffer.string("string"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(short_buffer.digest("digest"), ErrorCode::MalformedInput);

  // The version prefix reader rejects a bumped version and a non-zero reserved
  // half-word in that order.
  Writer writer;
  fco::write_version_prefix(writer);
  FCO_REQUIRE(writer.status().ok());
  FCO_CHECK_EQ(writer.size(), std::size_t{4});
  Reader prefix(writer.buffer().data(), writer.buffer().size());
  FCO_CHECK(fco::read_version_prefix(prefix, "version").ok());

  std::vector<std::uint8_t> bumped = writer.buffer();
  bumped[0] = 9u;
  Reader bumped_reader(bumped.data(), bumped.size());
  FCO_CHECK_ERROR(fco::read_version_prefix(bumped_reader, "version"),
                  ErrorCode::UnsupportedFormatVersion);
}

FCO_TEST_MAIN
