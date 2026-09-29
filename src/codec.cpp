#include "fco/codec.hpp"

#include <cstring>
#include <limits>

namespace fco {
namespace {

constexpr std::uint64_t kCounterMax = (std::numeric_limits<std::uint64_t>::max)();

template <class Id>
[[nodiscard]] Result<Id> read_id(Reader& reader, std::string_view field) {
  auto text = reader.string(field);
  if (!text.ok()) return text.error();
  return Id::parse(text.value());
}

template <class Counter>
[[nodiscard]] Result<Counter> read_counter(Reader& reader, std::string_view field) {
  auto raw = reader.u64(field);
  if (!raw.ok()) return raw.error();
  if (raw.value() == 0) {
    return failure<Counter>(ErrorCode::InvalidIdentity, std::string(field),
                            "counter must be at least 1");
  }
  return success(Counter(raw.value()));
}

template <class Enum>
[[nodiscard]] Result<Enum> read_enum(Reader& reader, std::string_view field) {
  auto raw = reader.u8(field);
  if (!raw.ok()) return raw.error();
  return decode_enum<Enum>(raw.value(), field);
}

// Every identifier written to a durable record is validated against the exact
// syntax the decoder enforces. Without this the encoder could publish a
// generation that can never be loaded again, which is a durable hazard rather
// than a caller mistake: the commit would succeed and the recovery would fail.
template <class Id>
void write_id(Writer& writer, const Id& value) {
  if (writer.failed()) return;
  if (!is_valid_identifier(value.value())) {
    writer.fail(ErrorCode::InvalidIdentity, std::string(Id::tag_type::kind_name),
                "identifier '" + value.value() +
                    "' is empty, longer than 64 bytes, or contains a character the decoder "
                    "rejects");
    return;
  }
  writer.string(value.value());
}

template <class Counter>
void write_counter(Writer& writer, const Counter& value) {
  writer.u64(value.value());
}

template <class Enum>
void write_enum(Writer& writer, Enum value) {
  writer.u8(static_cast<std::uint8_t>(value));
}

void encode_generation_set(const GenerationSet& generations, Writer& writer) {
  for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kGenerationKindMax); ++kind) {
    writer.u64(generations.get(static_cast<GenerationKind>(kind)));
  }
}

Result<GenerationSet> decode_generation_set(Reader& reader, std::string_view field) {
  GenerationSet generations;
  for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kGenerationKindMax); ++kind) {
    auto raw = reader.u64(field);
    if (!raw.ok()) return raw.error();
    if (raw.value() == 0) {
      return failure<GenerationSet>(ErrorCode::InvalidIdentity, std::string(field),
                                    "generation must be at least 1");
    }
    generations.set(static_cast<GenerationKind>(kind), raw.value());
  }
  return success(generations);
}

void encode_authority(const AuthorityContext& authority, Writer& writer) {
  write_counter(writer, authority.incarnation);
  write_counter(writer, authority.control_epoch);
  write_id(writer, authority.actor);
  write_id(writer, authority.policy);
  writer.digest(authority.policy_digest);
}

Result<AuthorityContext> decode_authority(Reader& reader, std::string_view field) {
  AuthorityContext authority;
  auto incarnation = read_counter<IncarnationId>(reader, field);
  if (!incarnation.ok()) return incarnation.error();
  auto epoch = read_counter<ControlEpoch>(reader, field);
  if (!epoch.ok()) return epoch.error();
  auto actor = read_id<PrincipalId>(reader, field);
  if (!actor.ok()) return actor.error();
  auto policy = read_id<PolicyId>(reader, field);
  if (!policy.ok()) return policy.error();
  auto digest = reader.digest(field);
  if (!digest.ok()) return digest.error();
  authority.incarnation = incarnation.value();
  authority.control_epoch = epoch.value();
  authority.actor = actor.value();
  authority.policy = policy.value();
  authority.policy_digest = digest.value();
  return success(authority);
}

void encode_asset(const AssetRecord& asset, Writer& writer) {
  write_id(writer, asset.id);
  write_id(writer, asset.rack);
  write_id(writer, asset.site);
  write_enum(writer, asset.lifecycle);
  write_enum(writer, asset.power);
  write_enum(writer, asset.cooling);
  write_enum(writer, asset.maintenance);
  write_enum(writer, asset.network);
  write_enum(writer, asset.workloads);
  write_counter(writer, asset.hardware_generation);
  write_counter(writer, asset.firmware_generation);
  write_counter(writer, asset.lifecycle_generation);
  write_counter(writer, asset.maintenance_generation);
  write_counter(writer, asset.capacity_generation);
  writer.string(asset.tenant.value());
  writer.u32(asset.capacity_total_units);
  writer.u32(asset.capacity_reserved_units);
}

Result<AssetRecord> decode_asset(Reader& reader, std::string_view field) {
  AssetRecord asset;
  auto id = read_id<AssetId>(reader, field);
  if (!id.ok()) return id.error();
  auto rack = read_id<RackId>(reader, field);
  if (!rack.ok()) return rack.error();
  auto site = read_id<SiteId>(reader, field);
  if (!site.ok()) return site.error();
  auto lifecycle = read_enum<AssetLifecycle>(reader, field);
  if (!lifecycle.ok()) return lifecycle.error();
  auto power = read_enum<PowerState>(reader, field);
  if (!power.ok()) return power.error();
  auto cooling = read_enum<CoolingState>(reader, field);
  if (!cooling.ok()) return cooling.error();
  auto maintenance = read_enum<MaintenanceState>(reader, field);
  if (!maintenance.ok()) return maintenance.error();
  auto network = read_enum<NetworkState>(reader, field);
  if (!network.ok()) return network.error();
  auto workloads = read_enum<WorkloadState>(reader, field);
  if (!workloads.ok()) return workloads.error();
  auto hardware = read_counter<HardwareGeneration>(reader, field);
  if (!hardware.ok()) return hardware.error();
  auto firmware = read_counter<FirmwareGeneration>(reader, field);
  if (!firmware.ok()) return firmware.error();
  auto lifecycle_generation = read_counter<LifecycleGeneration>(reader, field);
  if (!lifecycle_generation.ok()) return lifecycle_generation.error();
  auto maintenance_generation = read_counter<MaintenanceGeneration>(reader, field);
  if (!maintenance_generation.ok()) return maintenance_generation.error();
  auto capacity_generation = read_counter<CapacityGeneration>(reader, field);
  if (!capacity_generation.ok()) return capacity_generation.error();
  auto tenant = reader.string(field);
  if (!tenant.ok()) return tenant.error();
  auto total = reader.u32(field);
  if (!total.ok()) return total.error();
  auto reserved = reader.u32(field);
  if (!reserved.ok()) return reserved.error();
  if (reserved.value() > total.value()) {
    return failure<AssetRecord>(ErrorCode::ImpossibleCombination, std::string(field),
                                "reserved capacity exceeds total capacity");
  }
  if (!tenant.value().empty()) {
    auto tenant_id = TenantId::parse(tenant.value());
    if (!tenant_id.ok()) return tenant_id.error();
    asset.tenant = tenant_id.value();
  }

  asset.id = id.value();
  asset.rack = rack.value();
  asset.site = site.value();
  asset.lifecycle = lifecycle.value();
  asset.power = power.value();
  asset.cooling = cooling.value();
  asset.maintenance = maintenance.value();
  asset.network = network.value();
  asset.workloads = workloads.value();
  asset.hardware_generation = hardware.value();
  asset.firmware_generation = firmware.value();
  asset.lifecycle_generation = lifecycle_generation.value();
  asset.maintenance_generation = maintenance_generation.value();
  asset.capacity_generation = capacity_generation.value();
  asset.capacity_total_units = total.value();
  asset.capacity_reserved_units = reserved.value();
  return success(asset);
}

void encode_action_request(const ActionRequest& request, Writer& writer) {
  write_enum(writer, request.kind);
  write_enum(writer, request.owner);
  write_id(writer, request.asset);
  write_id(writer, request.rack);
  write_id(writer, request.site);
  write_counter(writer, request.hardware_generation);
  write_counter(writer, request.firmware_generation);
  write_counter(writer, request.lifecycle_generation);
  writer.u32(request.target_state);
  writer.u64(request.target_value);
}

Result<ActionRequest> decode_action_request(Reader& reader, std::string_view field) {
  ActionRequest request;
  auto kind = read_enum<ActionKind>(reader, field);
  if (!kind.ok()) return kind.error();
  auto owner = read_enum<DomainKind>(reader, field);
  if (!owner.ok()) return owner.error();
  auto asset = read_id<AssetId>(reader, field);
  if (!asset.ok()) return asset.error();
  auto rack = read_id<RackId>(reader, field);
  if (!rack.ok()) return rack.error();
  auto site = read_id<SiteId>(reader, field);
  if (!site.ok()) return site.error();
  auto hardware = read_counter<HardwareGeneration>(reader, field);
  if (!hardware.ok()) return hardware.error();
  auto firmware = read_counter<FirmwareGeneration>(reader, field);
  if (!firmware.ok()) return firmware.error();
  auto lifecycle = read_counter<LifecycleGeneration>(reader, field);
  if (!lifecycle.ok()) return lifecycle.error();
  auto target_state = reader.u32(field);
  if (!target_state.ok()) return target_state.error();
  auto target_value = reader.u64(field);
  if (!target_value.ok()) return target_value.error();
  request.kind = kind.value();
  request.owner = owner.value();
  request.asset = asset.value();
  request.rack = rack.value();
  request.site = site.value();
  request.hardware_generation = hardware.value();
  request.firmware_generation = firmware.value();
  request.lifecycle_generation = lifecycle.value();
  request.target_state = target_state.value();
  request.target_value = target_value.value();
  return success(request);
}

void encode_predicate(const Predicate& predicate, Writer& writer) {
  write_enum(writer, predicate.kind);
  // A predicate names either an asset or a predecessor, never both, so these two
  // identifiers are legitimately optional and are written as plain strings. The
  // decoder parses whichever ones are present.
  writer.string(predicate.asset.value());
  writer.string(predicate.predecessor.value());
  writer.u32(predicate.expected_state);
  writer.u64(predicate.expected_value);
}

Result<Predicate> decode_predicate(Reader& reader, std::string_view field) {
  Predicate predicate;
  auto kind = read_enum<PredicateKind>(reader, field);
  if (!kind.ok()) return kind.error();
  auto asset = reader.string(field);
  if (!asset.ok()) return asset.error();
  auto predecessor = reader.string(field);
  if (!predecessor.ok()) return predecessor.error();
  auto state = reader.u32(field);
  if (!state.ok()) return state.error();
  auto value = reader.u64(field);
  if (!value.ok()) return value.error();
  if (!asset.value().empty()) {
    auto parsed = AssetId::parse(asset.value());
    if (!parsed.ok()) return parsed.error();
    predicate.asset = parsed.value();
  }
  if (!predecessor.value().empty()) {
    auto parsed = StepId::parse(predecessor.value());
    if (!parsed.ok()) return parsed.error();
    predicate.predecessor = parsed.value();
  }
  predicate.kind = kind.value();
  predicate.expected_state = state.value();
  predicate.expected_value = value.value();
  return success(predicate);
}

void encode_effect(const EffectSpec& effect, Writer& writer) {
  write_enum(writer, effect.kind);
  write_id(writer, effect.asset);
  writer.u32(effect.expected_state);
  writer.u64(effect.expected_value);
}

Result<EffectSpec> decode_effect(Reader& reader, std::string_view field) {
  EffectSpec effect;
  auto kind = read_enum<EffectKind>(reader, field);
  if (!kind.ok()) return kind.error();
  auto asset = read_id<AssetId>(reader, field);
  if (!asset.ok()) return asset.error();
  auto state = reader.u32(field);
  if (!state.ok()) return state.error();
  auto value = reader.u64(field);
  if (!value.ok()) return value.error();
  effect.kind = kind.value();
  effect.asset = asset.value();
  effect.expected_state = state.value();
  effect.expected_value = value.value();
  return success(effect);
}

void encode_step(const ChangeStep& step, Writer& writer) {
  write_id(writer, step.id);
  write_enum(writer, step.owner);
  write_enum(writer, step.action);
  write_enum(writer, step.safety);
  write_enum(writer, step.reversibility);
  write_enum(writer, step.verification);
  writer.boolean(step.point_of_no_return);
  encode_action_request(step.request, writer);
  writer.count(static_cast<std::uint64_t>(step.preconditions.size()), "preconditions");
  for (const Predicate& predicate : step.preconditions) encode_predicate(predicate, writer);
  writer.count(static_cast<std::uint64_t>(step.depends_on.size()), "depends_on");
  for (const StepId& dependency : step.depends_on) write_id(writer, dependency);
  encode_effect(step.expected_effect, writer);
  writer.boolean(step.compensation.has_value());
  if (step.compensation.has_value()) encode_action_request(*step.compensation, writer);
  writer.string(step.rationale);
}

Result<ChangeStep> decode_step(Reader& reader, std::string_view field) {
  ChangeStep step;
  auto id = read_id<StepId>(reader, field);
  if (!id.ok()) return id.error();
  auto owner = read_enum<DomainKind>(reader, field);
  if (!owner.ok()) return owner.error();
  auto action = read_enum<ActionKind>(reader, field);
  if (!action.ok()) return action.error();
  auto safety = read_enum<SafetyClass>(reader, field);
  if (!safety.ok()) return safety.error();
  auto reversibility = read_enum<Reversibility>(reader, field);
  if (!reversibility.ok()) return reversibility.error();
  auto verification = read_enum<VerificationMode>(reader, field);
  if (!verification.ok()) return verification.error();
  auto point_of_no_return = reader.boolean(field);
  if (!point_of_no_return.ok()) return point_of_no_return.error();
  auto request = decode_action_request(reader, field);
  if (!request.ok()) return request.error();
  auto precondition_count = reader.count(field);
  if (!precondition_count.ok()) return precondition_count.error();
  std::vector<Predicate> preconditions;
  preconditions.reserve(static_cast<std::size_t>(precondition_count.value()));
  for (std::uint64_t index = 0; index < precondition_count.value(); ++index) {
    auto predicate = decode_predicate(reader, field);
    if (!predicate.ok()) return predicate.error();
    preconditions.push_back(predicate.take());
  }
  auto dependency_count = reader.count(field);
  if (!dependency_count.ok()) return dependency_count.error();
  std::vector<StepId> dependencies;
  dependencies.reserve(static_cast<std::size_t>(dependency_count.value()));
  for (std::uint64_t index = 0; index < dependency_count.value(); ++index) {
    auto dependency = read_id<StepId>(reader, field);
    if (!dependency.ok()) return dependency.error();
    dependencies.push_back(dependency.take());
  }
  auto effect = decode_effect(reader, field);
  if (!effect.ok()) return effect.error();
  auto has_compensation = reader.boolean(field);
  if (!has_compensation.ok()) return has_compensation.error();
  std::optional<ActionRequest> compensation;
  if (has_compensation.value()) {
    auto parsed = decode_action_request(reader, field);
    if (!parsed.ok()) return parsed.error();
    compensation = parsed.take();
  }
  auto rationale = reader.string(field);
  if (!rationale.ok()) return rationale.error();

  step.id = id.value();
  step.owner = owner.value();
  step.action = action.value();
  step.safety = safety.value();
  step.reversibility = reversibility.value();
  step.verification = verification.value();
  step.point_of_no_return = point_of_no_return.value();
  step.request = request.take();
  step.preconditions = std::move(preconditions);
  step.depends_on = std::move(dependencies);
  step.expected_effect = effect.take();
  step.compensation = std::move(compensation);
  step.rationale = rationale.take();
  return success(std::move(step));
}

void encode_attempt(const AttemptRecord& attempt, Writer& writer) {
  write_id(writer, attempt.id);
  write_id(writer, attempt.plan);
  write_counter(writer, attempt.plan_revision);
  write_id(writer, attempt.step);
  write_counter(writer, attempt.ordinal);
  write_enum(writer, attempt.status);
  writer.digest(attempt.intent_digest);
  writer.digest(attempt.idempotency_key);
  encode_generation_set(attempt.bound_generations, writer);
  encode_authority(attempt.bound_authority, writer);
  write_counter(writer, attempt.last_observation_sequence);
  writer.digest(attempt.last_evidence);
  writer.u64(attempt.issued_at_micros);
  writer.u64(attempt.updated_at_micros);
  writer.string(attempt.note);
}

Result<AttemptRecord> decode_attempt(Reader& reader, std::string_view field) {
  AttemptRecord attempt;
  auto id = read_id<AttemptId>(reader, field);
  if (!id.ok()) return id.error();
  auto plan = read_id<PlanId>(reader, field);
  if (!plan.ok()) return plan.error();
  auto revision = read_counter<PlanRevision>(reader, field);
  if (!revision.ok()) return revision.error();
  auto step = read_id<StepId>(reader, field);
  if (!step.ok()) return step.error();
  auto ordinal = read_counter<AttemptOrdinal>(reader, field);
  if (!ordinal.ok()) return ordinal.error();
  auto status = read_enum<AttemptStatus>(reader, field);
  if (!status.ok()) return status.error();
  auto intent = reader.digest(field);
  if (!intent.ok()) return intent.error();
  auto key = reader.digest(field);
  if (!key.ok()) return key.error();
  auto generations = decode_generation_set(reader, field);
  if (!generations.ok()) return generations.error();
  auto authority = decode_authority(reader, field);
  if (!authority.ok()) return authority.error();
  auto sequence = read_counter<ObservationSequence>(reader, field);
  if (!sequence.ok()) return sequence.error();
  auto evidence = reader.digest(field);
  if (!evidence.ok()) return evidence.error();
  auto issued = reader.u64(field);
  if (!issued.ok()) return issued.error();
  auto updated = reader.u64(field);
  if (!updated.ok()) return updated.error();
  auto note = reader.string(field);
  if (!note.ok()) return note.error();

  attempt.id = id.value();
  attempt.plan = plan.value();
  attempt.plan_revision = revision.value();
  attempt.step = step.value();
  attempt.ordinal = ordinal.value();
  attempt.status = status.value();
  attempt.intent_digest = intent.value();
  attempt.idempotency_key = key.value();
  attempt.bound_generations = generations.take();
  attempt.bound_authority = authority.take();
  attempt.last_observation_sequence = sequence.value();
  attempt.last_evidence = evidence.value();
  attempt.issued_at_micros = issued.value();
  attempt.updated_at_micros = updated.value();
  attempt.note = note.take();
  return success(std::move(attempt));
}

void encode_scope(const PlanScope& scope, Writer& writer) {
  write_id(writer, scope.site);
  writer.string(scope.rack.value());
  writer.count(static_cast<std::uint64_t>(scope.assets.size()), "scope.assets");
  for (const AssetId& asset : scope.assets) write_id(writer, asset);
}

Result<PlanScope> decode_scope(Reader& reader, std::string_view field) {
  PlanScope scope;
  auto site = read_id<SiteId>(reader, field);
  if (!site.ok()) return site.error();
  auto rack = reader.string(field);
  if (!rack.ok()) return rack.error();
  auto count = reader.count(field);
  if (!count.ok()) return count.error();
  std::vector<AssetId> assets;
  assets.reserve(static_cast<std::size_t>(count.value()));
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    auto asset = read_id<AssetId>(reader, field);
    if (!asset.ok()) return asset.error();
    assets.push_back(asset.take());
  }
  scope.site = site.value();
  if (!rack.value().empty()) {
    auto parsed = RackId::parse(rack.value());
    if (!parsed.ok()) return parsed.error();
    scope.rack = parsed.value();
  }
  scope.assets = std::move(assets);
  return success(std::move(scope));
}

void encode_evidence(const EvidenceRecord& record, Writer& writer) {
  write_id(writer, record.id);
  write_id(writer, record.plan);
  write_counter(writer, record.plan_revision);
  write_id(writer, record.step);
  write_id(writer, record.attempt);
  write_enum(writer, record.source_domain);
  write_counter(writer, record.source_incarnation);
  write_counter(writer, record.source_control_epoch);
  write_counter(writer, record.observation_sequence);
  encode_generation_set(record.observed_generations, writer);
  writer.digest(record.content_digest);
  write_enum(writer, record.outcome);
  writer.u64(record.observed_at_micros);
  writer.boolean(record.has_observed_asset);
  if (record.has_observed_asset) encode_asset(record.observed_asset, writer);
  writer.string(record.note);
}

Result<EvidenceRecord> decode_evidence(Reader& reader, std::string_view field) {
  EvidenceRecord record;
  auto id = read_id<EvidenceId>(reader, field);
  if (!id.ok()) return id.error();
  auto plan = read_id<PlanId>(reader, field);
  if (!plan.ok()) return plan.error();
  auto revision = read_counter<PlanRevision>(reader, field);
  if (!revision.ok()) return revision.error();
  auto step = read_id<StepId>(reader, field);
  if (!step.ok()) return step.error();
  auto attempt = read_id<AttemptId>(reader, field);
  if (!attempt.ok()) return attempt.error();
  auto domain = read_enum<DomainKind>(reader, field);
  if (!domain.ok()) return domain.error();
  auto incarnation = read_counter<IncarnationId>(reader, field);
  if (!incarnation.ok()) return incarnation.error();
  auto epoch = read_counter<ControlEpoch>(reader, field);
  if (!epoch.ok()) return epoch.error();
  auto sequence = read_counter<ObservationSequence>(reader, field);
  if (!sequence.ok()) return sequence.error();
  auto generations = decode_generation_set(reader, field);
  if (!generations.ok()) return generations.error();
  auto content = reader.digest(field);
  if (!content.ok()) return content.error();
  auto outcome = read_enum<EvidenceOutcome>(reader, field);
  if (!outcome.ok()) return outcome.error();
  auto observed = reader.u64(field);
  if (!observed.ok()) return observed.error();
  auto has_asset = reader.boolean(field);
  if (!has_asset.ok()) return has_asset.error();
  AssetRecord observed_asset;
  if (has_asset.value()) {
    auto asset = decode_asset(reader, field);
    if (!asset.ok()) return asset.error();
    observed_asset = asset.take();
  }
  auto note = reader.string(field);
  if (!note.ok()) return note.error();

  record.has_observed_asset = has_asset.value();
  record.observed_asset = std::move(observed_asset);
  record.id = id.value();
  record.plan = plan.value();
  record.plan_revision = revision.value();
  record.step = step.value();
  record.attempt = attempt.value();
  record.source_domain = domain.value();
  record.source_incarnation = incarnation.value();
  record.source_control_epoch = epoch.value();
  record.observation_sequence = sequence.value();
  record.observed_generations = generations.take();
  record.content_digest = content.value();
  record.outcome = outcome.value();
  record.observed_at_micros = observed.value();
  record.note = note.take();
  return success(std::move(record));
}

void encode_plan(const ChangePlan& plan, Writer& writer) {
  write_id(writer, plan.id);
  write_id(writer, plan.request_id);
  write_counter(writer, plan.revision);
  write_enum(writer, plan.state);
  write_id(writer, plan.kind);
  write_id(writer, plan.site);
  writer.string(plan.intent);
  encode_scope(plan.scope, writer);
  write_counter(writer, plan.facility_epoch);
  write_counter(writer, plan.policy_generation);
  encode_generation_set(plan.bound_generations, writer);
  writer.digest(plan.request_digest);
  writer.digest(plan.evidence_digest);
  writer.digest(plan.facility_digest);
  encode_authority(plan.authority, writer);
  writer.count(static_cast<std::uint64_t>(plan.steps.size()), "plan.steps", kMaxPlanSteps);
  for (const ChangeStep& step : plan.steps) encode_step(step, writer);
  writer.count(static_cast<std::uint64_t>(plan.step_status.size()), "plan.step_status");
  for (const auto& entry : plan.step_status) {
    write_id(writer, entry.first);
    write_enum(writer, entry.second);
  }
  writer.count(static_cast<std::uint64_t>(plan.attempts.size()), "plan.attempts");
  for (const AttemptRecord& attempt : plan.attempts) encode_attempt(attempt, writer);
  writer.count(static_cast<std::uint64_t>(plan.self_advanced.size()), "plan.self_advanced");
  for (const GenerationDelta& delta : plan.self_advanced) {
    write_enum(writer, delta.kind);
    writer.u64(delta.bound);
    writer.u64(delta.current);
  }
  write_counter(writer, plan.explained_revision);
  writer.string(plan.transition_note);
  writer.u64(plan.created_at_micros);
  writer.u64(plan.updated_at_micros);
}

Result<ChangePlan> decode_plan(Reader& reader, std::string_view field) {
  ChangePlan plan;
  auto id = read_id<PlanId>(reader, field);
  if (!id.ok()) return id.error();
  auto request_id = read_id<ChangeRequestId>(reader, field);
  if (!request_id.ok()) return request_id.error();
  auto revision = read_counter<PlanRevision>(reader, field);
  if (!revision.ok()) return revision.error();
  auto state = read_enum<PlanState>(reader, field);
  if (!state.ok()) return state.error();
  auto kind = read_id<ChangeKind>(reader, field);
  if (!kind.ok()) return kind.error();
  auto site = read_id<SiteId>(reader, field);
  if (!site.ok()) return site.error();
  auto intent = reader.string(field);
  if (!intent.ok()) return intent.error();
  auto scope = decode_scope(reader, field);
  if (!scope.ok()) return scope.error();
  auto epoch = read_counter<FacilityEpoch>(reader, field);
  if (!epoch.ok()) return epoch.error();
  auto policy_generation = read_counter<PolicyGeneration>(reader, field);
  if (!policy_generation.ok()) return policy_generation.error();
  auto generations = decode_generation_set(reader, field);
  if (!generations.ok()) return generations.error();
  auto request_digest = reader.digest(field);
  if (!request_digest.ok()) return request_digest.error();
  auto evidence_digest = reader.digest(field);
  if (!evidence_digest.ok()) return evidence_digest.error();
  auto facility_digest = reader.digest(field);
  if (!facility_digest.ok()) return facility_digest.error();
  auto authority = decode_authority(reader, field);
  if (!authority.ok()) return authority.error();

  auto step_count = reader.count(field, kMaxPlanSteps);
  if (!step_count.ok()) return step_count.error();
  std::vector<ChangeStep> steps;
  steps.reserve(static_cast<std::size_t>(step_count.value()));
  for (std::uint64_t index = 0; index < step_count.value(); ++index) {
    auto step = decode_step(reader, field);
    if (!step.ok()) return step.error();
    steps.push_back(step.take());
  }

  auto status_count = reader.count(field);
  if (!status_count.ok()) return status_count.error();
  std::map<StepId, StepStatus> statuses;
  for (std::uint64_t index = 0; index < status_count.value(); ++index) {
    auto step_id = read_id<StepId>(reader, field);
    if (!step_id.ok()) return step_id.error();
    auto status = read_enum<StepStatus>(reader, field);
    if (!status.ok()) return status.error();
    if (!statuses.insert_or_assign(step_id.value(), status.value()).second) {
      return failure<ChangePlan>(ErrorCode::DuplicateIdentity, step_id.value().value(),
                                 "step status recorded twice");
    }
  }

  auto attempt_count = reader.count(field);
  if (!attempt_count.ok()) return attempt_count.error();
  std::vector<AttemptRecord> attempts;
  attempts.reserve(static_cast<std::size_t>(attempt_count.value()));
  for (std::uint64_t index = 0; index < attempt_count.value(); ++index) {
    auto attempt = decode_attempt(reader, field);
    if (!attempt.ok()) return attempt.error();
    attempts.push_back(attempt.take());
  }

  auto advanced_count = reader.count(field);
  if (!advanced_count.ok()) return advanced_count.error();
  std::vector<GenerationDelta> advanced;
  advanced.reserve(static_cast<std::size_t>(advanced_count.value()));
  for (std::uint64_t index = 0; index < advanced_count.value(); ++index) {
    GenerationDelta delta;
    auto advanced_kind = read_enum<GenerationKind>(reader, field);
    if (!advanced_kind.ok()) return advanced_kind.error();
    auto bound = reader.u64(field);
    if (!bound.ok()) return bound.error();
    auto current = reader.u64(field);
    if (!current.ok()) return current.error();
    delta.kind = advanced_kind.value();
    delta.bound = bound.value();
    delta.current = current.value();
    advanced.push_back(delta);
  }

  auto explained = read_counter<FacilityRevision>(reader, field);
  if (!explained.ok()) return explained.error();
  auto note = reader.string(field);
  if (!note.ok()) return note.error();
  auto created = reader.u64(field);
  if (!created.ok()) return created.error();
  auto updated = reader.u64(field);
  if (!updated.ok()) return updated.error();

  plan.id = id.value();
  plan.request_id = request_id.value();
  plan.revision = revision.value();
  plan.state = state.value();
  plan.kind = kind.value();
  plan.site = site.value();
  plan.intent = intent.take();
  plan.scope = scope.take();
  plan.facility_epoch = epoch.value();
  plan.policy_generation = policy_generation.value();
  plan.bound_generations = generations.take();
  plan.request_digest = request_digest.value();
  plan.evidence_digest = evidence_digest.value();
  plan.facility_digest = facility_digest.value();
  plan.authority = authority.take();
  plan.steps = std::move(steps);
  plan.step_status = std::move(statuses);
  plan.attempts = std::move(attempts);
  plan.self_advanced = std::move(advanced);
  plan.explained_revision = explained.value();
  plan.transition_note = note.take();
  plan.created_at_micros = created.value();
  plan.updated_at_micros = updated.value();
  return success(std::move(plan));
}

}  // namespace

void Writer::fail(ErrorCode code, std::string subject, std::string message) {
  if (failed_) return;
  failed_ = true;
  error_ = Error(code, std::move(subject), std::move(message));
}

Status Writer::status() const {
  if (!failed_) return success();
  return Status(error_);
}

void Writer::u8(std::uint8_t value) {
  if (failed_) return;
  buffer_.push_back(value);
}

void Writer::boolean(bool value) {
  if (failed_) return;
  buffer_.push_back(value ? 1u : 0u);
}

void Writer::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>(value & 0xffu));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void Writer::u32(std::uint32_t value) {
  for (unsigned shift = 0; shift < 32u; shift += 8u) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void Writer::u64(std::uint64_t value) {
  for (unsigned shift = 0; shift < 64u; shift += 8u) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void Writer::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void Writer::bytes(const void* data, std::size_t length) {
  if (failed_) return;
  const auto* pointer = static_cast<const std::uint8_t*>(data);
  if (length == 0) return;
  if (pointer == nullptr) {
    fail(ErrorCode::InternalError, "bytes", "null buffer with non-zero length");
    return;
  }
  buffer_.insert(buffer_.end(), pointer, pointer + length);
}

void Writer::string(std::string_view value) {
  if (failed_) return;
  if (value.size() > kMaxStringBytes) {
    fail(ErrorCode::BoundedLimitExceeded, "string",
         "string field exceeds the supported bound of " + std::to_string(kMaxStringBytes) +
             " bytes");
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  bytes(value.data(), value.size());
}

void Writer::digest(const Digest& value) {
  bytes(value.bytes().data(), value.bytes().size());
}

void Writer::count(std::uint64_t value, std::string_view field, std::uint64_t maximum) {
  if (failed_) return;
  if (value > maximum) {
    fail(ErrorCode::BoundedLimitExceeded, std::string(field),
         "collection exceeds the supported bound");
    return;
  }
  u64(value);
}

void Writer::reserve(std::uint32_t value, std::string_view field) {
  if (failed_) return;
  if (value != 0u) {
    fail(ErrorCode::ReservedFieldNonZero, std::string(field),
         "reserved field must be zero when written");
    return;
  }
  u32(0u);
}

Result<const std::uint8_t*> Reader::take(std::size_t length, std::string_view field) {
  if (length > size_ - offset_) {
    return failure<const std::uint8_t*>(ErrorCode::MalformedInput, std::string(field),
                                        "record ended before the field was complete");
  }
  const std::uint8_t* pointer = data_ + offset_;
  offset_ += length;
  return success(pointer);
}

Result<std::uint8_t> Reader::u8(std::string_view field) {
  auto pointer = take(1, field);
  if (!pointer.ok()) return pointer.error();
  return success(*pointer.value());
}

Result<bool> Reader::boolean(std::string_view field) {
  auto value = u8(field);
  if (!value.ok()) return value.error();
  if (value.value() > 1u) {
    return failure<bool>(ErrorCode::MalformedInput, std::string(field),
                         "boolean field must be 0 or 1");
  }
  return success(value.value() == 1u);
}

Result<std::uint16_t> Reader::u16(std::string_view field) {
  auto pointer = take(2, field);
  if (!pointer.ok()) return pointer.error();
  const std::uint8_t* bytes = pointer.value();
  return success(static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                            (static_cast<std::uint16_t>(bytes[1]) << 8)));
}

Result<std::uint32_t> Reader::u32(std::string_view field) {
  auto pointer = take(4, field);
  if (!pointer.ok()) return pointer.error();
  const std::uint8_t* bytes = pointer.value();
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4u; ++index) {
    value |= static_cast<std::uint32_t>(bytes[index]) << (8u * index);
  }
  return success(value);
}

Result<std::uint64_t> Reader::u64(std::string_view field) {
  auto pointer = take(8, field);
  if (!pointer.ok()) return pointer.error();
  const std::uint8_t* bytes = pointer.value();
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8u; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (8u * index);
  }
  return success(value);
}

Result<std::int64_t> Reader::i64(std::string_view field) {
  auto value = u64(field);
  if (!value.ok()) return value.error();
  return success(static_cast<std::int64_t>(value.value()));
}

Result<std::string> Reader::string(std::string_view field, std::size_t maximum) {
  auto length = u32(field);
  if (!length.ok()) return length.error();
  if (static_cast<std::uint64_t>(length.value()) > static_cast<std::uint64_t>(maximum)) {
    return failure<std::string>(ErrorCode::BoundedLimitExceeded, std::string(field),
                                "string length exceeds the supported bound");
  }
  auto pointer = take(length.value(), field);
  if (!pointer.ok()) return pointer.error();
  return success(std::string(reinterpret_cast<const char*>(pointer.value()), length.value()));
}

Result<Digest> Reader::digest(std::string_view field) {
  auto pointer = take(Digest::kBytes, field);
  if (!pointer.ok()) return pointer.error();
  Digest::Bytes bytes{};
  for (std::size_t index = 0; index < Digest::kBytes; ++index) {
    bytes[index] = pointer.value()[index];
  }
  return success(Digest(bytes));
}

Result<std::uint64_t> Reader::count(std::string_view field, std::uint64_t maximum) {
  auto value = u64(field);
  if (!value.ok()) return value.error();
  if (value.value() > maximum) {
    return failure<std::uint64_t>(ErrorCode::BoundedLimitExceeded, std::string(field),
                                  "collection count exceeds the supported bound");
  }
  if (value.value() > static_cast<std::uint64_t>(remaining())) {
    return failure<std::uint64_t>(ErrorCode::MalformedInput, std::string(field),
                                  "collection count is larger than the remaining record");
  }
  return value;
}

Result<void> Reader::reserved_u32(std::string_view field) {
  auto value = u32(field);
  if (!value.ok()) return value.error();
  if (value.value() != 0u) {
    return failure<>(ErrorCode::ReservedFieldNonZero, std::string(field),
                     "reserved field must be zero");
  }
  return success();
}

Result<void> Reader::reserved_u16(std::string_view field) {
  auto value = u16(field);
  if (!value.ok()) return value.error();
  if (value.value() != 0u) {
    return failure<>(ErrorCode::ReservedFieldNonZero, std::string(field),
                     "reserved field must be zero");
  }
  return success();
}

Result<void> Reader::reserved_u64(std::string_view field) {
  auto value = u64(field);
  if (!value.ok()) return value.error();
  if (value.value() != 0ull) {
    return failure<>(ErrorCode::ReservedFieldNonZero, std::string(field),
                     "reserved field must be zero");
  }
  return success();
}

Result<void> Reader::expect_end(std::string_view field) {
  if (remaining() != 0) {
    return failure<>(ErrorCode::TrailingBytes, std::string(field),
                     std::to_string(remaining()) + " trailing byte(s) after the record payload");
  }
  return success();
}

void write_version_prefix(Writer& writer) {
  writer.u16(kStorageFormatVersion);
  writer.u16(0u);  // reserved, must be zero
}

Result<void> read_version_prefix(Reader& reader, std::string_view field) {
  auto version = reader.u16(field);
  if (!version.ok()) return version.error();
  if (version.value() != kStorageFormatVersion) {
    return failure<>(ErrorCode::UnsupportedFormatVersion, std::string(field),
                     "record format version " + std::to_string(version.value()) +
                         " is not supported (expected " +
                         std::to_string(kStorageFormatVersion) + ")");
  }
  return reader.reserved_u16(field);
}

Status encode_state(const OrchestratorState& state, Writer& writer) {
  write_version_prefix(writer);

  writer.u64(state.revision.value());
  writer.u64(state.commit_sequence.value());
  writer.u64(state.facility_epoch.value());
  encode_generation_set(state.generations, writer);
  writer.u64(state.facility_revision.value());

  write_id(writer, state.facility.site());
  writer.u64(state.facility.epoch().value());
  encode_generation_set(state.facility.generations(), writer);
  writer.count(static_cast<std::uint64_t>(state.facility.size()), "facility.assets");
  for (const auto& entry : state.facility.assets()) {
    encode_asset(entry.second, writer);
  }

  writer.count(static_cast<std::uint64_t>(state.evidence.size()), "evidence.records");
  for (const auto& entry : state.evidence.records()) {
    encode_evidence(entry.second, writer);
  }

  writer.count(static_cast<std::uint64_t>(state.plans.size()), "state.plans",
               kMaxRetainedPlans);
  for (const auto& entry : state.plans) {
    encode_plan(entry.second, writer);
  }

  write_counter(writer, state.incarnation);
  write_counter(writer, state.control_epoch);
  write_id(writer, state.actor);
  write_id(writer, state.policy);
  writer.digest(state.policy_digest);
  writer.u64(state.updated_at_micros);
  writer.reserve(0u, "state.reserved");
  return writer.status();
}

Result<std::vector<std::uint8_t>> encode_state_bytes(const OrchestratorState& state) {
  Writer writer;
  const Status status = encode_state(state, writer);
  if (!status.ok()) return status.error();
  if (writer.size() > kMaxRecordPayloadBytes) {
    return failure<std::vector<std::uint8_t>>(
        ErrorCode::BoundedLimitExceeded, "state",
        "encoded state exceeds the maximum record payload size");
  }
  return success(writer.buffer());
}

Result<OrchestratorState> decode_state(const std::uint8_t* data, std::size_t length) {
  if (data == nullptr) {
    return failure<OrchestratorState>(ErrorCode::MalformedInput, "state", "null record buffer");
  }
  Reader reader(data, length);
  auto version = read_version_prefix(reader, "state.version");
  if (!version.ok()) return version.error();

  OrchestratorState state;
  auto revision = reader.u64("state.revision");
  if (!revision.ok()) return revision.error();
  auto commit_sequence = reader.u64("state.commit_sequence");
  if (!commit_sequence.ok()) return commit_sequence.error();
  auto epoch = reader.u64("state.facility_epoch");
  if (!epoch.ok()) return epoch.error();
  auto generations = decode_generation_set(reader, "state.generations");
  if (!generations.ok()) return generations.error();
  auto facility_revision = reader.u64("state.facility_revision");
  if (!facility_revision.ok()) return facility_revision.error();

  state.revision = Revision(revision.value());
  state.commit_sequence = CommitSequence(commit_sequence.value());
  state.facility_epoch = FacilityEpoch(epoch.value());
  state.generations = generations.take();
  state.facility_revision = FacilityRevision(facility_revision.value());
  if (!state.revision.valid() || !state.commit_sequence.valid() || !state.facility_epoch.valid() ||
      !state.facility_revision.valid()) {
    return failure<OrchestratorState>(ErrorCode::InvalidIdentity, "state",
                                      "durable state carries a zero counter");
  }

  auto site = read_id<SiteId>(reader, "state.facility.site");
  if (!site.ok()) return site.error();
  auto facility_epoch = reader.u64("state.facility.epoch");
  if (!facility_epoch.ok()) return facility_epoch.error();
  auto facility_generations = decode_generation_set(reader, "state.facility.generations");
  if (!facility_generations.ok()) return facility_generations.error();
  auto asset_count = reader.count("state.facility.assets");
  if (!asset_count.ok()) return asset_count.error();
  std::vector<AssetRecord> assets;
  assets.reserve(static_cast<std::size_t>(asset_count.value()));
  for (std::uint64_t index = 0; index < asset_count.value(); ++index) {
    auto asset = decode_asset(reader, "state.facility.asset");
    if (!asset.ok()) return asset.error();
    assets.push_back(asset.take());
  }
  auto facility = FacilitySnapshot::create(site.value(), FacilityEpoch(facility_epoch.value()),
                                           facility_generations.take(), std::move(assets));
  if (!facility.ok()) return facility.error();
  state.facility = facility.take();

  auto evidence_count = reader.count("state.evidence.records");
  if (!evidence_count.ok()) return evidence_count.error();
  for (std::uint64_t index = 0; index < evidence_count.value(); ++index) {
    auto record = decode_evidence(reader, "state.evidence.record");
    if (!record.ok()) return record.error();
    state.evidence.restore(record.value());
  }
  if (state.evidence.size() != static_cast<std::size_t>(evidence_count.value())) {
    return failure<OrchestratorState>(ErrorCode::DuplicateIdentity, "state.evidence",
                                      "durable evidence ledger contains a duplicate identifier");
  }
  state.evidence.rebuild_watermarks();

  auto plan_count = reader.count("state.plans", kMaxRetainedPlans);
  if (!plan_count.ok()) return plan_count.error();
  for (std::uint64_t index = 0; index < plan_count.value(); ++index) {
    auto plan = decode_plan(reader, "state.plan");
    if (!plan.ok()) return plan.error();
    // The identifier is captured before the plan is moved. Evaluating
    // plan.value().id as the key of an insert_or_assign whose mapped value is
    // plan.take() would bind the key to the very object the move empties, which
    // would silently key the map with an empty identifier.
    ChangePlan decoded_plan = plan.take();
    const PlanId decoded_id = decoded_plan.id;
    if (!state.plans.insert_or_assign(decoded_id, std::move(decoded_plan)).second) {
      return failure<OrchestratorState>(ErrorCode::DuplicateIdentity, "state.plans",
                                        "durable state contains a duplicate plan identifier");
    }
  }

  auto incarnation = read_counter<IncarnationId>(reader, "state.incarnation");
  if (!incarnation.ok()) return incarnation.error();
  auto control_epoch = read_counter<ControlEpoch>(reader, "state.control_epoch");
  if (!control_epoch.ok()) return control_epoch.error();
  auto actor = read_id<PrincipalId>(reader, "state.actor");
  if (!actor.ok()) return actor.error();
  auto policy = read_id<PolicyId>(reader, "state.policy");
  if (!policy.ok()) return policy.error();
  auto policy_digest = reader.digest("state.policy_digest");
  if (!policy_digest.ok()) return policy_digest.error();
  auto updated = reader.u64("state.updated_at_micros");
  if (!updated.ok()) return updated.error();
  auto reserved = reader.reserved_u32("state.reserved");
  if (!reserved.ok()) return reserved.error();
  auto end = reader.expect_end("state");
  if (!end.ok()) return end.error();

  state.incarnation = incarnation.value();
  state.control_epoch = control_epoch.value();
  state.actor = actor.value();
  state.policy = policy.value();
  state.policy_digest = policy_digest.value();
  state.updated_at_micros = updated.value();

  for (const auto& entry : state.plans) {
    if (!(entry.first == entry.second.id)) {
      return failure<OrchestratorState>(
          ErrorCode::ImpossibleCombination, "state.plans",
          "a durable plan is keyed by an identifier that is not the plan's own identifier");
    }
  }

  if (state.facility.generations().digest() != state.generations.digest()) {
    return failure<OrchestratorState>(ErrorCode::ImpossibleCombination, "state.generations",
                                      "facility snapshot generations disagree with the "
                                      "authoritative generation set");
  }
  if (state.facility.epoch() != state.facility_epoch) {
    return failure<OrchestratorState>(ErrorCode::ImpossibleCombination, "state.facility_epoch",
                                      "facility snapshot epoch disagrees with the authoritative "
                                      "facility epoch");
  }
  return success(std::move(state));
}

}  // namespace fco
