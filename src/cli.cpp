#include "fco/cli.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "fco/digest.hpp"
#include "fco/engine.hpp"
#include "fco/json.hpp"
#include "fco/planner.hpp"
#include "fco/render.hpp"
#include "fco/sim.hpp"

namespace fco::cli {
namespace {

using fco::json::Value;

constexpr std::string_view kPlantBanner =
    "plant: SYNTHETIC - domain actions are applied by the in-process simulated plant; no "
    "physical data-center hardware is involved.";

constexpr std::string_view kDefaultActor = "principal-facility-ops";
constexpr std::string_view kDefaultPolicy = "policy-change-control-v1";
constexpr std::string_view kDefaultSite = "site-alpha";
constexpr std::string_view kDefaultRack = "rack-a1";
constexpr std::uint64_t kDefaultEpoch = 4;

struct Arguments {
  std::vector<std::string> positional;
  std::map<std::string, std::string> flags;

  [[nodiscard]] bool has(const std::string& name) const { return flags.find(name) != flags.end(); }
  [[nodiscard]] std::string get(const std::string& name, std::string fallback = {}) const {
    const auto it = flags.find(name);
    return it == flags.end() ? fallback : it->second;
  }
};

// Every option any command accepts. An option outside this set is a usage error
// rather than something silently ignored, so a typo cannot quietly change what a
// command was asked to do.
constexpr std::string_view kKnownOptions[] = {
    "root",     "site",   "epoch",     "facility", "actor",      "policy",     "incarnation",
    "control-epoch", "file", "out",    "kind",     "request-id", "assets",     "rack",
    "plan",     "reason", "note",      "attempt",  "resolution", "apply",      "intent"};

[[nodiscard]] bool is_known_option(const std::string& name) noexcept {
  for (std::string_view known : kKnownOptions) {
    if (known == name) return true;
  }
  return false;
}

[[nodiscard]] Result<Arguments> parse_arguments(const std::vector<std::string>& arguments) {
  Arguments parsed;
  std::size_t index = 0;
  while (index < arguments.size()) {
    const std::string token = arguments[index];
    if (token.size() >= 2 && token[0] == '-' && token[1] == '-') {
      const std::string name = token.substr(2);
      if (name.empty()) {
        return failure<Arguments>(ErrorCode::UsageError, "--", "empty option name");
      }
      if (parsed.flags.find(name) != parsed.flags.end()) {
        return failure<Arguments>(ErrorCode::UsageError, name, "option was supplied twice");
      }
      if (!is_known_option(name)) {
        return failure<Arguments>(ErrorCode::UsageError, name, "unknown option");
      }
      // An option takes the following token as its value unless that token is
      // itself an option or there is no following token; in that case the option
      // is a switch whose value is "true". The rule is stated in the usage text
      // so a mistyped option value fails loudly instead of being guessed.
      const bool has_value =
          index + 1 < arguments.size() && !(arguments[index + 1].size() >= 2 &&
                                            arguments[index + 1][0] == '-' &&
                                            arguments[index + 1][1] == '-');
      parsed.flags.insert_or_assign(name, has_value ? arguments[index + 1] : std::string("true"));
      index += has_value ? 2u : 1u;
      continue;
    }
    parsed.positional.push_back(token);
    ++index;
  }
  return success(std::move(parsed));
}

[[nodiscard]] Result<std::string> require_string(const Arguments& arguments,
                                                 const std::string& name) {
  if (!arguments.has(name)) {
    return failure<std::string>(ErrorCode::UsageError, name, "required option is missing");
  }
  const std::string value = arguments.get(name);
  if (value.empty()) {
    return failure<std::string>(ErrorCode::UsageError, name, "option must not be empty");
  }
  return success(value);
}

template <class Strong>
[[nodiscard]] Result<Strong> require_strong(const Arguments& arguments, const std::string& name) {
  auto text = require_string(arguments, name);
  if (!text.ok()) return text.error();
  return Strong::parse(text.value());
}

[[nodiscard]] Result<std::uint64_t> parse_u64(const std::string& text, const std::string& subject,
                                              std::uint64_t minimum, std::uint64_t maximum) {
  auto parsed = parse_bounded_decimal(text, maximum);
  if (!parsed.ok()) return parsed.error();
  if (parsed.value() < minimum) {
    return failure<std::uint64_t>(ErrorCode::BoundedLimitExceeded, subject,
                                  "value is below the supported minimum");
  }
  return parsed;
}

[[nodiscard]] Result<std::string> json_string(const Value& object, const std::string& name) {
  const Value* member = object.find(name);
  if (member == nullptr) {
    return failure<std::string>(ErrorCode::MalformedInput, name, "field is missing");
  }
  return member->as_string(name);
}

[[nodiscard]] Result<std::uint64_t> json_u64(const Value& object, const std::string& name) {
  const Value* member = object.find(name);
  if (member == nullptr) {
    return failure<std::uint64_t>(ErrorCode::MalformedInput, name, "field is missing");
  }
  return member->as_u64(name);
}

[[nodiscard]] Result<std::uint64_t> json_u64_or(const Value& object, const std::string& name,
                                                std::uint64_t fallback) {
  const Value* member = object.find(name);
  if (member == nullptr) return success(fallback);
  return member->as_u64(name);
}

template <class Strong>
[[nodiscard]] Result<Strong> json_id(const Value& object, const std::string& name) {
  auto text = json_string(object, name);
  if (!text.ok()) return text.error();
  return Strong::parse(text.value());
}

template <class Counter>
[[nodiscard]] Result<Counter> json_counter(const Value& object, const std::string& name) {
  auto number = json_u64(object, name);
  if (!number.ok()) return number.error();
  if (number.value() == 0) {
    return failure<Counter>(ErrorCode::InvalidIdentity, name, "counter must be at least 1");
  }
  return success(Counter(number.value()));
}

template <class Enum>
[[nodiscard]] Result<Enum> json_enum(const Value& object, const std::string& name,
                                     Result<Enum> (*parser)(std::string_view)) {
  auto text = json_string(object, name);
  if (!text.ok()) return text.error();
  auto parsed = parser(text.value());
  if (!parsed.ok()) return parsed.error();
  if (!is_valid(parsed.value())) {
    return failure<Enum>(ErrorCode::InvalidEnumValue, name, "value is outside its enum domain");
  }
  return parsed;
}

[[nodiscard]] Result<GenerationSet> parse_generations(const Value& value) {
  auto object = value.as_object("generations");
  if (!object.ok()) return object.error();
  GenerationSet generations;
  for (const auto& member : object.value()) {
    auto kind = parse_generation_kind(member.first);
    if (!kind.ok()) {
      return failure<GenerationSet>(ErrorCode::InvalidEnumValue, member.first,
                                    "unknown generation kind");
    }
    if (!is_valid(kind.value())) {
      return failure<GenerationSet>(ErrorCode::InvalidEnumValue, member.first,
                                    "generation kind is unset");
    }
    auto number = member.second.as_u64(member.first);
    if (!number.ok()) return number.error();
    if (number.value() == 0) {
      return failure<GenerationSet>(ErrorCode::InvalidIdentity, member.first,
                                    "generation must be at least 1");
    }
    generations.set(kind.value(), number.value());
  }
  if (!generations.complete()) {
    return failure<GenerationSet>(ErrorCode::MissingAuthority, "generations",
                                  "generation set is incomplete");
  }
  return success(generations);
}

[[nodiscard]] Value write_generations(const GenerationSet& generations) {
  Value::Object members;
  for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kGenerationKindMax); ++kind) {
    const auto typed = static_cast<GenerationKind>(kind);
    members.emplace_back(std::string(to_string(typed)),
                         Value::make_integer(static_cast<std::int64_t>(generations.get(typed))));
  }
  return Value::make_object(std::move(members));
}

[[nodiscard]] Result<AssetRecord> parse_asset(const Value& value) {
  AssetRecord asset;
  auto id = json_id<AssetId>(value, "id");
  if (!id.ok()) return id.error();
  asset.id = id.value();
  auto rack = json_id<RackId>(value, "rack");
  if (!rack.ok()) return rack.error();
  asset.rack = rack.value();
  auto site = json_id<SiteId>(value, "site");
  if (!site.ok()) return site.error();
  asset.site = site.value();

  auto lifecycle = json_enum<AssetLifecycle>(value, "lifecycle", parse_asset_lifecycle);
  if (!lifecycle.ok()) return lifecycle.error();
  asset.lifecycle = lifecycle.value();
  auto power = json_enum<PowerState>(value, "power", parse_power_state);
  if (!power.ok()) return power.error();
  asset.power = power.value();
  auto cooling = json_enum<CoolingState>(value, "cooling", parse_cooling_state);
  if (!cooling.ok()) return cooling.error();
  asset.cooling = cooling.value();
  auto maintenance = json_enum<MaintenanceState>(value, "maintenance", parse_maintenance_state);
  if (!maintenance.ok()) return maintenance.error();
  asset.maintenance = maintenance.value();
  auto network = json_enum<NetworkState>(value, "network", parse_network_state);
  if (!network.ok()) return network.error();
  asset.network = network.value();
  auto workloads = json_enum<WorkloadState>(value, "workloads", parse_workload_state);
  if (!workloads.ok()) return workloads.error();
  asset.workloads = workloads.value();

  auto hardware = json_counter<HardwareGeneration>(value, "hardware-generation");
  if (!hardware.ok()) return hardware.error();
  asset.hardware_generation = hardware.value();
  auto firmware = json_counter<FirmwareGeneration>(value, "firmware-generation");
  if (!firmware.ok()) return firmware.error();
  asset.firmware_generation = firmware.value();
  auto lifecycle_generation = json_counter<LifecycleGeneration>(value, "lifecycle-generation");
  if (!lifecycle_generation.ok()) return lifecycle_generation.error();
  asset.lifecycle_generation = lifecycle_generation.value();
  auto maintenance_generation =
      json_counter<MaintenanceGeneration>(value, "maintenance-generation");
  if (!maintenance_generation.ok()) return maintenance_generation.error();
  asset.maintenance_generation = maintenance_generation.value();
  auto capacity_generation = json_counter<CapacityGeneration>(value, "capacity-generation");
  if (!capacity_generation.ok()) return capacity_generation.error();
  asset.capacity_generation = capacity_generation.value();

  if (value.find("tenant") != nullptr) {
    auto tenant = json_id<TenantId>(value, "tenant");
    if (!tenant.ok()) return tenant.error();
    asset.tenant = tenant.value();
  }
  auto total = json_u64_or(value, "capacity-total", 0);
  if (!total.ok()) return total.error();
  auto reserved = json_u64_or(value, "capacity-reserved", 0);
  if (!reserved.ok()) return reserved.error();
  if (total.value() > 0xFFFFFFFFull || reserved.value() > 0xFFFFFFFFull) {
    return failure<AssetRecord>(ErrorCode::BoundedLimitExceeded, "capacity",
                                "capacity exceeds the supported range");
  }
  if (reserved.value() > total.value()) {
    return failure<AssetRecord>(ErrorCode::ImpossibleCombination, "capacity-reserved",
                                "reserved capacity exceeds total capacity");
  }
  asset.capacity_total_units = static_cast<std::uint32_t>(total.value());
  asset.capacity_reserved_units = static_cast<std::uint32_t>(reserved.value());
  return success(std::move(asset));
}

[[nodiscard]] Value write_asset(const AssetRecord& asset) {
  Value::Object members;
  members.emplace_back("id", Value::make_string(asset.id.value()));
  members.emplace_back("rack", Value::make_string(asset.rack.value()));
  members.emplace_back("site", Value::make_string(asset.site.value()));
  members.emplace_back("lifecycle", Value::make_string(std::string(to_string(asset.lifecycle))));
  members.emplace_back("power", Value::make_string(std::string(to_string(asset.power))));
  members.emplace_back("cooling", Value::make_string(std::string(to_string(asset.cooling))));
  members.emplace_back("maintenance",
                       Value::make_string(std::string(to_string(asset.maintenance))));
  members.emplace_back("network", Value::make_string(std::string(to_string(asset.network))));
  members.emplace_back("workloads", Value::make_string(std::string(to_string(asset.workloads))));
  members.emplace_back(
      "hardware-generation",
      Value::make_integer(static_cast<std::int64_t>(asset.hardware_generation.value())));
  members.emplace_back(
      "firmware-generation",
      Value::make_integer(static_cast<std::int64_t>(asset.firmware_generation.value())));
  members.emplace_back(
      "lifecycle-generation",
      Value::make_integer(static_cast<std::int64_t>(asset.lifecycle_generation.value())));
  members.emplace_back(
      "maintenance-generation",
      Value::make_integer(static_cast<std::int64_t>(asset.maintenance_generation.value())));
  members.emplace_back(
      "capacity-generation",
      Value::make_integer(static_cast<std::int64_t>(asset.capacity_generation.value())));
  if (asset.tenant.valid()) {
    members.emplace_back("tenant", Value::make_string(asset.tenant.value()));
  }
  members.emplace_back("capacity-total", Value::make_integer(asset.capacity_total_units));
  members.emplace_back("capacity-reserved", Value::make_integer(asset.capacity_reserved_units));
  return Value::make_object(std::move(members));
}

[[nodiscard]] Result<FacilitySnapshot> parse_facility(const Value& value) {
  auto site = json_id<SiteId>(value, "site");
  if (!site.ok()) return site.error();
  auto epoch = json_counter<FacilityEpoch>(value, "epoch");
  if (!epoch.ok()) return epoch.error();
  const Value* generations_value = value.find("generations");
  if (generations_value == nullptr) {
    return failure<FacilitySnapshot>(ErrorCode::MissingAuthority, "generations",
                                     "facility has no generation set");
  }
  auto generations = parse_generations(*generations_value);
  if (!generations.ok()) return generations.error();
  const Value* assets_value = value.find("assets");
  if (assets_value == nullptr) {
    return failure<FacilitySnapshot>(ErrorCode::MalformedInput, "assets",
                                     "facility has no asset list");
  }
  auto assets_array = assets_value->as_array("assets");
  if (!assets_array.ok()) return assets_array.error();
  std::vector<AssetRecord> assets;
  assets.reserve(assets_array.value().size());
  for (const Value& element : assets_array.value()) {
    auto asset = parse_asset(element);
    if (!asset.ok()) return asset.error();
    assets.push_back(asset.take());
  }
  return FacilitySnapshot::create(site.value(), epoch.value(), generations.value(),
                                  std::move(assets));
}

[[nodiscard]] Value write_facility(const FacilitySnapshot& facility) {
  Value::Array assets;
  for (const auto& entry : facility.assets()) assets.push_back(write_asset(entry.second));
  Value::Object members;
  members.emplace_back("site", Value::make_string(facility.site().value()));
  members.emplace_back("epoch",
                       Value::make_integer(static_cast<std::int64_t>(facility.epoch().value())));
  members.emplace_back("generations", write_generations(facility.generations()));
  members.emplace_back("assets", Value::make_array(std::move(assets)));
  return Value::make_object(std::move(members));
}

[[nodiscard]] Result<PlanScope> parse_scope(const Value& value) {
  PlanScope scope;
  auto site = json_id<SiteId>(value, "site");
  if (!site.ok()) return site.error();
  scope.site = site.value();
  if (value.find("rack") != nullptr) {
    auto rack = json_id<RackId>(value, "rack");
    if (!rack.ok()) return rack.error();
    scope.rack = rack.value();
  }
  if (const Value* assets = value.find("assets"); assets != nullptr) {
    auto array = assets->as_array("assets");
    if (!array.ok()) return array.error();
    for (const Value& element : array.value()) {
      auto text = element.as_string("assets");
      if (!text.ok()) return text.error();
      auto parsed = AssetId::parse(text.value());
      if (!parsed.ok()) return parsed.error();
      scope.assets.push_back(parsed.value());
    }
  }
  return success(std::move(scope));
}

[[nodiscard]] Result<ChangeRequest> parse_request(const Value& value) {
  ChangeRequest request;
  auto id = json_id<ChangeRequestId>(value, "id");
  if (!id.ok()) return id.error();
  request.id = id.value();
  auto kind = json_id<ChangeKind>(value, "kind");
  if (!kind.ok()) return kind.error();
  request.kind = kind.value();
  auto site = json_id<SiteId>(value, "site");
  if (!site.ok()) return site.error();
  request.site = site.value();
  if (value.find("intent") != nullptr) {
    auto intent = json_string(value, "intent");
    if (!intent.ok()) return intent.error();
    request.intent = intent.value();
  }
  if (value.find("actor") != nullptr) {
    auto actor = json_id<PrincipalId>(value, "actor");
    if (!actor.ok()) return actor.error();
    request.actor = actor.value();
  }
  if (value.find("revision") != nullptr) {
    auto revision = json_counter<Revision>(value, "revision");
    if (!revision.ok()) return revision.error();
    request.revision = revision.value();
  }
  if (value.find("evidence-digest") != nullptr) {
    auto digest_text = json_string(value, "evidence-digest");
    if (!digest_text.ok()) return digest_text.error();
    auto digest = Digest::from_hex(digest_text.value());
    if (!digest.ok()) return digest.error();
    request.evidence_digest = digest.value();
  }
  const Value* scope = value.find("scope");
  if (scope == nullptr) {
    return failure<ChangeRequest>(ErrorCode::MalformedInput, "scope", "request has no scope");
  }
  auto parsed_scope = parse_scope(*scope);
  if (!parsed_scope.ok()) return parsed_scope.error();
  request.scope = parsed_scope.take();
  if (const Value* epoch = value.find("facility-epoch"); epoch != nullptr) {
    auto number = epoch->as_u64("facility-epoch");
    if (!number.ok()) return number.error();
    if (number.value() == 0) {
      return failure<ChangeRequest>(ErrorCode::InvalidIdentity, "facility-epoch",
                                    "facility epoch must be at least 1");
    }
    request.facility_epoch = FacilityEpoch(number.value());
  }
  if (const Value* generations = value.find("generations"); generations != nullptr) {
    auto parsed = parse_generations(*generations);
    if (!parsed.ok()) return parsed.error();
    request.bound_generations = parsed.value();
  }
  return success(std::move(request));
}

[[nodiscard]] Value write_request(const ChangeRequest& request) {
  Value::Array assets;
  for (const AssetId& asset : request.scope.assets) {
    assets.push_back(Value::make_string(asset.value()));
  }
  Value::Object scope;
  scope.emplace_back("site", Value::make_string(request.scope.site.value()));
  if (request.scope.rack.valid()) {
    scope.emplace_back("rack", Value::make_string(request.scope.rack.value()));
  }
  scope.emplace_back("assets", Value::make_array(std::move(assets)));

  Value::Object members;
  members.emplace_back("id", Value::make_string(request.id.value()));
  members.emplace_back("kind", Value::make_string(request.kind.value()));
  members.emplace_back("site", Value::make_string(request.site.value()));
  members.emplace_back("intent", Value::make_string(request.intent));
  members.emplace_back("actor", Value::make_string(request.actor.value()));
  members.emplace_back("revision",
                       Value::make_integer(static_cast<std::int64_t>(request.revision.value())));
  members.emplace_back("evidence-digest", Value::make_string(request.evidence_digest.to_hex()));
  members.emplace_back(
      "facility-epoch",
      Value::make_integer(static_cast<std::int64_t>(request.facility_epoch.value())));
  members.emplace_back("generations", write_generations(request.bound_generations));
  members.emplace_back("scope", Value::make_object(std::move(scope)));
  return Value::make_object(std::move(members));
}

[[nodiscard]] Result<EvidenceRecord> parse_evidence(const Value& value) {
  EvidenceRecord record;
  auto id = json_id<EvidenceId>(value, "id");
  if (!id.ok()) return id.error();
  record.id = id.value();
  auto plan = json_id<PlanId>(value, "plan");
  if (!plan.ok()) return plan.error();
  record.plan = plan.value();
  auto revision = json_counter<PlanRevision>(value, "plan-revision");
  if (!revision.ok()) return revision.error();
  record.plan_revision = revision.value();
  auto step = json_id<StepId>(value, "step");
  if (!step.ok()) return step.error();
  record.step = step.value();
  auto attempt = json_id<AttemptId>(value, "attempt");
  if (!attempt.ok()) return attempt.error();
  record.attempt = attempt.value();

  auto domain = json_enum<DomainKind>(value, "source-domain", parse_domain_kind);
  if (!domain.ok()) return domain.error();
  record.source_domain = domain.value();
  auto incarnation = json_counter<IncarnationId>(value, "source-incarnation");
  if (!incarnation.ok()) return incarnation.error();
  record.source_incarnation = incarnation.value();
  auto control_epoch = json_counter<ControlEpoch>(value, "source-control-epoch");
  if (!control_epoch.ok()) return control_epoch.error();
  record.source_control_epoch = control_epoch.value();
  auto sequence = json_counter<ObservationSequence>(value, "observation-sequence");
  if (!sequence.ok()) return sequence.error();
  record.observation_sequence = sequence.value();

  const Value* generations = value.find("observed-generations");
  if (generations == nullptr) {
    return failure<EvidenceRecord>(ErrorCode::MalformedInput, "observed-generations",
                                   "field is missing");
  }
  auto parsed_generations = parse_generations(*generations);
  if (!parsed_generations.ok()) return parsed_generations.error();
  record.observed_generations = parsed_generations.value();

  auto digest = json_string(value, "content-digest");
  if (!digest.ok()) return digest.error();
  auto parsed_digest = Digest::from_hex(digest.value());
  if (!parsed_digest.ok()) return parsed_digest.error();
  record.content_digest = parsed_digest.value();

  auto outcome = json_enum<EvidenceOutcome>(value, "outcome", parse_evidence_outcome);
  if (!outcome.ok()) return outcome.error();
  record.outcome = outcome.value();

  auto observed_at = json_u64_or(value, "observed-at", 0);
  if (!observed_at.ok()) return observed_at.error();
  record.observed_at_micros = observed_at.value();

  if (const Value* asset = value.find("observed-asset"); asset != nullptr) {
    auto parsed = parse_asset(*asset);
    if (!parsed.ok()) return parsed.error();
    record.has_observed_asset = true;
    record.observed_asset = parsed.take();
  }
  if (const Value* note = value.find("note"); note != nullptr) {
    auto text = note->as_string("note");
    if (!text.ok()) return text.error();
    record.note = text.value();
  }
  return success(std::move(record));
}

[[nodiscard]] Result<std::string> read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return failure<std::string>(ErrorCode::RecordNotFound, path, "cannot open file for reading");
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  if (stream.bad()) {
    return failure<std::string>(ErrorCode::StorageFailure, path, "read failed");
  }
  return success(buffer.str());
}

[[nodiscard]] Status write_text_file(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return failure<>(ErrorCode::StorageFailure, path, "cannot open file for writing");
  }
  stream << text;
  stream.flush();
  if (!stream) {
    return failure<>(ErrorCode::StorageFailure, path, "write failed");
  }
  return success();
}

[[nodiscard]] Result<Value> read_json_file(const std::string& path) {
  auto text = read_text_file(path);
  if (!text.ok()) return text.error();
  return json::parse(text.value());
}

[[nodiscard]] std::string join(const std::vector<std::string>& parts,
                                 const std::string& separator) {
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) out += separator;
    out += part;
  }
  return out;
}

[[nodiscard]] std::vector<std::string> split_list(const std::string& text) {
  std::vector<std::string> out;
  std::string current;
  for (char c : text) {
    if (c == ',') {
      if (!current.empty()) out.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

struct Session {
  SystemClock clock;
  DomainRegistry registry;
  std::unique_ptr<SimulatedPlant> plant;
  std::unique_ptr<SimulatedAuthoritySet> authorities;
  std::optional<Orchestrator> orchestrator;

  [[nodiscard]] Orchestrator& get() { return *orchestrator; }

  [[nodiscard]] Status attach_plant() {
    if (!orchestrator->state().initialized()) {
      return failure<>(ErrorCode::StorageFailure, "store",
                       "the store is not initialized; run 'fco init --root <dir>' first");
    }
    const OrchestratorState& state = orchestrator->state();
    plant = std::make_unique<SimulatedPlant>(state.facility);
    authorities = std::make_unique<SimulatedAuthoritySet>(*plant, state.incarnation,
                                                          state.control_epoch);
    for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kDomainKindMax); ++kind) {
      const auto domain = static_cast<DomainKind>(kind);
      DomainAuthority* authority = authorities->find(domain);
      if (authority == nullptr) continue;
      registry.add(authority);
      const ObservationSequence watermark = state.evidence.watermark(domain, state.incarnation);
      plant->seed_observation_sequence(domain,
                                       watermark.valid() ? watermark.value() + 1u : 1u);
    }
    return success();
  }
};

[[nodiscard]] GenerationSet default_generations(std::uint64_t epoch) {
  GenerationSet generations;
  generations.facility_epoch = FacilityEpoch(epoch);
  generations.policy = PolicyGeneration(2);
  generations.dependency = DependencyGeneration(5);
  generations.topology = TopologyGeneration(7);
  generations.capacity = CapacityGeneration(3);
  generations.lifecycle = LifecycleGeneration(4);
  generations.maintenance = MaintenanceGeneration(6);
  generations.power = PowerGeneration(8);
  generations.cooling = CoolingGeneration(9);
  generations.asi = AsiGeneration(11);
  generations.dfi = DfiGeneration(12);
  return generations;
}

[[nodiscard]] std::string format_error(const Error& error) {
  std::string out = "error: ";
  out += to_string(error.code);
  if (!error.subject.empty()) out += " [" + error.subject + "]";
  if (!error.message.empty()) out += ": " + error.message;
  return out;
}

[[nodiscard]] int exit_code_for(ErrorCode code) {
  switch (code) {
    case ErrorCode::UsageError:
    case ErrorCode::UnknownCommand:
      return kExitUsage;
    case ErrorCode::LockUnavailable:
      return kExitLocked;
    case ErrorCode::StorageFailure:
    case ErrorCode::CommitFailure:
    case ErrorCode::AmbiguousRecovery:
    case ErrorCode::RecordNotFound:
      return kExitStorage;
    case ErrorCode::AttemptUnresolved:
      return kExitUnresolved;
    default:
      return kExitFailure;
  }
}

[[nodiscard]] Status open_session(const Arguments& arguments, Session& session) {
  auto root = require_string(arguments, "root");
  if (!root.ok()) return root.error();
  auto path = path_from_utf8(root.value());
  if (!path.ok()) return path.error();
  auto orchestrator = Orchestrator::open(path.value(), session.clock, session.registry);
  if (!orchestrator.ok()) return orchestrator.error();
  session.orchestrator = orchestrator.take();
  return success();
}

[[nodiscard]] Result<PlanId> require_plan(const Arguments& arguments) {
  return require_strong<PlanId>(arguments, "plan");
}

[[nodiscard]] Status initialize_store(Session& session, const Arguments& arguments,
                                      std::ostringstream& out) {
  const std::string site_name = arguments.get("site", std::string(kDefaultSite));
  auto site = SiteId::parse(site_name);
  if (!site.ok()) return site.error();

  std::uint64_t epoch_value = kDefaultEpoch;
  if (arguments.has("epoch")) {
    auto parsed = parse_u64(arguments.get("epoch"), "epoch", 1, 0xFFFFFFFFull);
    if (!parsed.ok()) return parsed.error();
    epoch_value = parsed.value();
  }

  FacilitySnapshot facility;
  if (arguments.has("facility")) {
    auto document = read_json_file(arguments.get("facility"));
    if (!document.ok()) return document.error();
    auto parsed = parse_facility(document.value());
    if (!parsed.ok()) return parsed.error();
    facility = parsed.take();
  } else {
    auto built = make_example_facility(site.value(), FacilityEpoch(epoch_value),
                                       default_generations(epoch_value));
    if (!built.valid()) {
      return failure<>(ErrorCode::InternalError, "facility",
                       "the built-in example facility is not usable");
    }
    facility = std::move(built);
  }

  const std::string actor_name = arguments.get("actor", std::string(kDefaultActor));
  auto actor = PrincipalId::parse(actor_name);
  if (!actor.ok()) return actor.error();
  const std::string policy_name = arguments.get("policy", std::string(kDefaultPolicy));
  auto policy = PolicyId::parse(policy_name);
  if (!policy.ok()) return policy.error();

  std::uint64_t incarnation_value = 1;
  if (arguments.has("incarnation")) {
    auto parsed = parse_u64(arguments.get("incarnation"), "incarnation", 1, 0xFFFFFFFFull);
    if (!parsed.ok()) return parsed.error();
    incarnation_value = parsed.value();
  }
  std::uint64_t control_epoch_value = 1;
  if (arguments.has("control-epoch")) {
    auto parsed = parse_u64(arguments.get("control-epoch"), "control-epoch", 1, 0xFFFFFFFFull);
    if (!parsed.ok()) return parsed.error();
    control_epoch_value = parsed.value();
  }

  const Status initialized =
      session.get().initialize(std::move(facility), IncarnationId(incarnation_value),
                               ControlEpoch(control_epoch_value), actor.value(), policy.value(),
                               Sha256::of(policy_name));
  if (!initialized.ok()) return initialized;

  out << "initialized store at " << arguments.get("root") << '\n';
  out << "  site: " << site_name << " epoch: " << epoch_value << '\n';
  out << "  actor: " << actor_name << " policy: " << policy_name << '\n';
  out << "  " << kPlantBanner << '\n';
  return success();
}

[[nodiscard]] Status synthesize_into(Session& session, ChangeRequest& request) {
  SynthesisInput input;
  input.kind = request.kind;
  input.id = request.id;
  input.site = request.site;
  input.intent = request.intent;
  input.scope = request.scope;
  input.facility_epoch = request.facility_epoch.valid() ? request.facility_epoch
                                                        : session.get().state().facility_epoch;
  input.actor = request.actor.valid() ? request.actor : session.get().state().actor;
  input.revision = request.revision.valid() ? request.revision : Revision(1);
  input.bound_generations = request.bound_generations.complete()
                                ? request.bound_generations
                                : session.get().state().generations;
  input.evidence_digest = request.evidence_digest.valid()
                              ? request.evidence_digest
                              : session.get().state().facility.digest();
  auto synthesized = session.get().synthesize(input);
  if (!synthesized.ok()) return synthesized.error();
  request = synthesized.take();
  return success();
}

[[nodiscard]] Status normalize_request(Session& session, ChangeRequest& request) {
  if (!request.facility_epoch.valid()) request.facility_epoch = session.get().state().facility_epoch;
  if (!request.bound_generations.complete()) {
    request.bound_generations = session.get().state().generations;
  }
  if (!request.actor.valid()) request.actor = session.get().state().actor;
  if (!request.revision.valid()) request.revision = Revision(1);
  if (!request.evidence_digest.valid()) request.evidence_digest = session.get().state().facility.digest();
  if (request.intent.empty()) request.intent = "requested change " + request.kind.value();
  return success();
}

[[nodiscard]] Status dispatch(const Arguments& arguments, Session& session,
                              std::ostringstream& out) {
  const std::vector<std::string>& words = arguments.positional;
  const std::string group = words.empty() ? std::string() : words[0];
  const std::string action = words.size() > 1 ? words[1] : std::string();

  if (group == "version") {
    out << kProjectName << ' ' << kVersionString << '\n';
    out << "  " << kDccpProgram << " / " << kDccpTranche << '\n';
    out << "  " << kDccpRepositoryNumber << '\n';
    out << "  storage format version: " << kStorageFormatVersion << '\n';
    out << "  " << kCopyrightNotice << '\n';
    return success();
  }
  if (group == "help") {
    out << usage();
    return success();
  }

  FCO_RETURN_IF_ERROR(open_session(arguments, session));

  if (group == "init") {
    if (session.get().state().initialized()) {
      return failure<>(ErrorCode::ImpossibleCombination, arguments.get("root"),
                       "the store already holds an authoritative generation");
    }
    return initialize_store(session, arguments, out);
  }

  if (group == "facility") {
    if (action == "show") {
      if (!session.get().state().initialized()) {
        return failure<>(ErrorCode::StorageFailure, "store", "the store is not initialized");
      }
      out << render_facility(session.get().state().facility);
      return success();
    }
    if (action == "observe") {
      auto file = require_string(arguments, "file");
      if (!file.ok()) return file.error();
      auto document = read_json_file(file.value());
      if (!document.ok()) return document.error();
      auto facility = parse_facility(document.value());
      if (!facility.ok()) return facility.error();
      FCO_RETURN_IF_ERROR(session.get().observe_facility(
          facility.take(), arguments.get("note", "operator supplied observation")));
      out << "facility observation accepted; the observed generations now bind every plan\n";
      out << render_facility(session.get().state().facility);
      return success();
    }
    if (action == "export") {
      if (!session.get().state().initialized()) {
        return failure<>(ErrorCode::StorageFailure, "store", "the store is not initialized");
      }
      auto path = require_string(arguments, "out");
      if (!path.ok()) return path.error();
      const std::string text =
          json::serialize(write_facility(session.get().state().facility), true) + "\n";
      FCO_RETURN_IF_ERROR(write_text_file(path.value(), text));
      out << "wrote " << path.value() << '\n';
      return success();
    }
    return failure<>(ErrorCode::UnknownCommand, action, "unknown facility subcommand");
  }

  if (group == "request") {
    if (action != "synth") {
      return failure<>(ErrorCode::UnknownCommand, action, "unknown request subcommand");
    }
    if (!session.get().state().initialized()) {
      return failure<>(ErrorCode::StorageFailure, "store", "the store is not initialized");
    }
    auto kind = require_strong<ChangeKind>(arguments, "kind");
    if (!kind.ok()) return kind.error();
    if (!is_known_change_kind(kind.value().value())) {
      return failure<>(ErrorCode::InvalidEnumValue, kind.value().value(),
                       "unknown change kind; known kinds: " +
                           join(known_change_kinds(), ", "));
    }
    auto request_id = require_strong<ChangeRequestId>(arguments, "request-id");
    if (!request_id.ok()) return request_id.error();
    auto site = require_strong<SiteId>(arguments, "site");
    if (!site.ok()) return site.error();
    const std::string assets_text = arguments.get("assets");
    if (assets_text.empty()) {
      return failure<>(ErrorCode::UsageError, "assets",
                       "at least one asset identifier is required");
    }

    PlanScope scope;
    scope.site = site.value();
    if (arguments.has("rack")) {
      auto rack = RackId::parse(arguments.get("rack"));
      if (!rack.ok()) return rack.error();
      scope.rack = rack.value();
    }
    for (const std::string& text : split_list(assets_text)) {
      auto asset = AssetId::parse(text);
      if (!asset.ok()) return asset.error();
      scope.assets.push_back(asset.value());
    }

    SynthesisInput input;
    input.kind = kind.value();
    input.id = request_id.value();
    input.site = site.value();
    input.intent = arguments.get("intent", "synthesised " + kind.value().value());
    input.scope = scope;
    input.facility_epoch = session.get().state().facility_epoch;
    input.actor = session.get().state().actor;
    input.revision = Revision(1);
    input.bound_generations = session.get().state().generations;
    input.evidence_digest = session.get().state().facility.digest();

    auto synthesized = session.get().synthesize(input);
    if (!synthesized.ok()) return synthesized.error();
    const std::string text = json::serialize(write_request(synthesized.value()), true) + "\n";
    if (arguments.has("out")) {
      FCO_RETURN_IF_ERROR(write_text_file(arguments.get("out"), text));
      out << "wrote " << arguments.get("out") << '\n';
    } else {
      out << text;
    }
    return success();
  }

  if (group == "plan") {
    if (!session.get().state().initialized()) {
      return failure<>(ErrorCode::StorageFailure, "store", "the store is not initialized");
    }
    if (action == "list") {
      if (session.get().state().plans.empty()) {
        out << "no plans\n";
        return success();
      }
      for (const auto& entry : session.get().state().plans) {
        out << entry.second.describe() << '\n';
      }
      return success();
    }
    if (action == "create") {
      auto file = require_string(arguments, "file");
      if (!file.ok()) return file.error();
      auto document = read_json_file(file.value());
      if (!document.ok()) return document.error();
      auto parsed = parse_request(document.value());
      if (!parsed.ok()) return parsed.error();
      ChangeRequest request = parsed.take();
      FCO_RETURN_IF_ERROR(normalize_request(session, request));
      if (request.steps.empty()) {
        FCO_RETURN_IF_ERROR(synthesize_into(session, request));
      }
      auto id = session.get().create_plan(request);
      if (!id.ok()) return id.error();
      const ChangePlan plan = session.get().plan(id.value()).value();
      out << "plan " << id.value().value() << '\n';
      out << "  revision: " << plan.revision.to_string() << " state: " << to_string(plan.state)
          << '\n';
      out << "  steps: " << plan.steps.size() << '\n';
      out << "  " << kPlantBanner << '\n';
      out << "next: fco plan evaluate --root " << arguments.get("root") << " --plan "
          << id.value().value() << " --apply\n";
      return success();
    }

    auto plan_id = require_plan(arguments);
    if (!plan_id.ok()) return plan_id.error();

    if (action == "show") {
      auto plan = session.get().plan(plan_id.value());
      if (!plan.ok()) return plan.error();
      out << plan.value().describe() << '\n';
      out << "  intent: " << plan.value().intent << '\n';
      out << "  note: " << plan.value().transition_note << '\n';
      return success();
    }
    if (action == "render") {
      auto text = session.get().render_plan(plan_id.value());
      if (!text.ok()) return text.error();
      out << text.value();
      return success();
    }
    if (action == "evaluate") {
      auto text = session.get().render_evaluation(plan_id.value());
      if (!text.ok()) return text.error();
      out << text.value();
      if (arguments.has("apply")) {
        FCO_RETURN_IF_ERROR(session.get().mark_evaluated(plan_id.value()));
        out << "plan " << plan_id.value().value() << " is now evaluated\n";
      }
      return success();
    }
    if (action == "authorize") {
      FCO_RETURN_IF_ERROR(session.get().authorize(plan_id.value()));
      out << "plan " << plan_id.value().value() << " authorized under the current authority "
          << "context; generation bindings remain in force\n";
      return success();
    }
    if (action == "pause") {
      FCO_RETURN_IF_ERROR(
          session.get().plan_pause(plan_id.value(), arguments.get("reason", "paused by operator")));
      out << "plan paused\n";
      return success();
    }
    if (action == "resume") {
      FCO_RETURN_IF_ERROR(session.get().plan_resume(plan_id.value()));
      out << "plan resumed\n";
      return success();
    }
    if (action == "abort") {
      FCO_RETURN_IF_ERROR(
          session.get().plan_abort(plan_id.value(), arguments.get("reason", "aborted by operator")));
      out << "plan cancelled; no effect was undone by this command\n";
      return success();
    }
    if (action == "replan") {
      FCO_RETURN_IF_ERROR(session.get().replan(
          plan_id.value(), arguments.get("reason", "replanned by operator")));
      auto plan = session.get().plan(plan_id.value());
      if (!plan.ok()) return plan.error();
      out << "plan replanned to revision " << plan.value().revision.to_string()
          << "; verified steps were preserved and the binding was re-based on the observed "
          << "facility\n";
      out << plan.value().describe() << '\n';
      return success();
    }
    if (action == "rollback") {
      FCO_RETURN_IF_ERROR(session.get().rollback(
          plan_id.value(), arguments.get("reason", "rolled back by operator")));
      out << "rollback complete\n";
      return success();
    }
    return failure<>(ErrorCode::UnknownCommand, action, "unknown plan subcommand");
  }

  if (group == "execute") {
    if (!session.get().state().initialized()) {
      return failure<>(ErrorCode::StorageFailure, "store", "the store is not initialized");
    }
    FCO_RETURN_IF_ERROR(session.attach_plant());
    out << kPlantBanner << '\n';
    auto plan_id = require_plan(arguments);
    if (!plan_id.ok()) return plan_id.error();

    if (action == "next") {
      auto result = session.get().execute_next(plan_id.value());
      if (!result.ok()) return result.error();
      out << "issued " << result.value().describe() << '\n';
      return success();
    }
    if (action == "all") {
      auto results = session.get().execute_all(plan_id.value());
      if (!results.ok()) return results.error();
      for (const ExecuteResult& result : results.value()) {
        out << "issued " << result.describe() << '\n';
      }
      auto plan = session.get().plan(plan_id.value());
      if (!plan.ok()) return plan.error();
      out << plan.value().describe() << '\n';
      out << "  note: " << plan.value().transition_note << '\n';
      return success();
    }
    return failure<>(ErrorCode::UnknownCommand, action, "unknown execute subcommand");
  }

  if (group == "evidence") {
    if (action != "ingest") {
      return failure<>(ErrorCode::UnknownCommand, action, "unknown evidence subcommand");
    }
    auto file = require_string(arguments, "file");
    if (!file.ok()) return file.error();
    auto document = read_json_file(file.value());
    if (!document.ok()) return document.error();
    auto record = parse_evidence(document.value());
    if (!record.ok()) return record.error();
    const EvidenceId id = record.value().id;
    FCO_RETURN_IF_ERROR(session.get().ingest_evidence(record.value()));
    out << "evidence " << id.value() << " ingested and verified against the expected effect\n";
    return success();
  }

  if (group == "attempt") {
    if (action != "resolve") {
      return failure<>(ErrorCode::UnknownCommand, action, "unknown attempt subcommand");
    }
    auto attempt = require_strong<AttemptId>(arguments, "attempt");
    if (!attempt.ok()) return attempt.error();
    auto resolution_text = require_string(arguments, "resolution");
    if (!resolution_text.ok()) return resolution_text.error();
    auto resolution = parse_attempt_status(resolution_text.value());
    if (!resolution.ok()) return resolution.error();
    FCO_RETURN_IF_ERROR(session.get().resolve_attempt(attempt.value(), resolution.value(),
                                                      arguments.get("note")));
    out << "attempt " << attempt.value().value() << " resolved as " << to_string(resolution.value())
        << '\n';
    return success();
  }

  if (group == "recover") {
    if (!session.get().state().initialized()) {
      return failure<>(ErrorCode::StorageFailure, "store", "the store is not initialized");
    }
    out << "store recovery: " << session.get().store_recovery().describe() << '\n';
    auto summary = session.get().recover();
    if (!summary.ok()) return summary.error();
    out << render_recovery(summary.value());
    out << kPlantBanner << '\n';
    return success();
  }

  if (group == "status") {
    out << session.get().render_status();
    return success();
  }

  if (group == "closure") {
    out << session.get().render_closure();
    return success();
  }

  if (group == "example") {
    if (action != "rack-replacement") {
      return failure<>(ErrorCode::UnknownCommand, action,
                       "unknown example; the available example is rack-replacement");
    }
    if (!session.get().state().initialized()) {
      FCO_RETURN_IF_ERROR(initialize_store(session, arguments, out));
    }
    FCO_RETURN_IF_ERROR(session.attach_plant());
    out << kPlantBanner << '\n';

    const std::string rack_name = arguments.get("rack", std::string(kDefaultRack));
    auto rack = RackId::parse(rack_name);
    if (!rack.ok()) return rack.error();
    PlanScope scope;
    scope.site = session.get().state().facility.site();
    scope.rack = rack.value();
    for (const auto& entry : session.get().state().facility.assets()) {
      if (entry.second.rack == rack.value()) scope.assets.push_back(entry.first);
    }
    if (scope.assets.empty()) {
      return failure<>(ErrorCode::UnknownIdentity, rack_name,
                       "no asset is observed in that rack");
    }
    if (arguments.has("assets")) {
      scope.assets.clear();
      for (const std::string& text : split_list(arguments.get("assets"))) {
        auto asset = AssetId::parse(text);
        if (!asset.ok()) return asset.error();
        scope.assets.push_back(asset.value());
      }
    }
    std::sort(scope.assets.begin(), scope.assets.end());

    out << "example: rack replacement of " << rack_name << " covering "
        << scope.assets.size() << " asset(s)\n";
    out << render_facility(session.get().state().facility);

    SynthesisInput input;
    input.kind = ChangeKind("rack-replacement");
    input.id = ChangeRequestId("req-rack-replacement");
    input.site = scope.site;
    input.intent = "replace every node in " + rack_name +
                   " with verified drain, isolation, power, cooling, fabric and lifecycle "
                   "coordination";
    input.scope = scope;
    input.facility_epoch = session.get().state().facility_epoch;
    input.actor = session.get().state().actor;
    input.revision = Revision(1);
    input.bound_generations = session.get().state().generations;
    input.evidence_digest = session.get().state().facility.digest();

    auto request = session.get().synthesize(input);
    if (!request.ok()) return request.error();
    out << "synthesised " << request.value().describe() << '\n';

    auto plan_id = session.get().create_plan(request.value());
    if (!plan_id.ok()) return plan_id.error();
    out << "plan " << plan_id.value().value() << " created\n";

    auto evaluation = session.get().render_evaluation(plan_id.value());
    if (!evaluation.ok()) return evaluation.error();
    out << evaluation.value();

    FCO_RETURN_IF_ERROR(session.get().mark_evaluated(plan_id.value()));
    FCO_RETURN_IF_ERROR(session.get().authorize(plan_id.value()));
    out << "plan authorized\n";

    auto results = session.get().execute_all(plan_id.value());
    if (!results.ok()) return results.error();
    for (const ExecuteResult& result : results.value()) {
      out << "  issued " << result.describe() << '\n';
    }
    auto plan = session.get().plan(plan_id.value());
    if (!plan.ok()) return plan.error();
    out << plan.value().describe() << '\n';
    out << "  note: " << plan.value().transition_note << '\n';
    out << session.get().render_closure();
    return success();
  }

  return failure<>(ErrorCode::UnknownCommand, group, "unknown command group");
}

}  // namespace

std::string usage() {
  std::ostringstream out;
  out << kProjectName << ' ' << kVersionString << " (" << kDccpRepositoryNumber << ")\n";
  out << "usage: fco <group> <action> [options]\n\n";
  out << "common options:\n";
  out << "  --root DIR            durable store root (required except for version/help)\n";
  out << "  --name VALUE takes the next token as its value; --name alone is a switch whose\n";
  out << "  value is \"true\" (for example: fco plan evaluate --root DIR --plan ID --apply)\n\n";
  out << "commands:\n";
  out << "  version\n";
  out << "  help\n";
  out << "  init --root DIR [--site S] [--epoch N] [--facility FILE] [--actor A] [--policy P]\n";
  out << "  facility show --root DIR\n";
  out << "  facility observe --root DIR --file FACILITY.json [--note TEXT]\n";
  out << "  facility export --root DIR --out FILE\n";
  out << "  request synth --root DIR --kind KIND --site S [--rack R] --assets a,b [--out FILE]\n";
  out << "  plan create --root DIR --file REQUEST.json\n";
  out << "  plan list --root DIR\n";
  out << "  plan show --root DIR --plan ID\n";
  out << "  plan render --root DIR --plan ID\n";
  out << "  plan evaluate --root DIR --plan ID [--apply]\n";
  out << "  plan authorize --root DIR --plan ID\n";
  out << "  plan pause --root DIR --plan ID [--reason TEXT]\n";
  out << "  plan resume --root DIR --plan ID\n";
  out << "  plan abort --root DIR --plan ID [--reason TEXT]\n";
  out << "  plan replan --root DIR --plan ID [--reason TEXT]\n";
  out << "  plan rollback --root DIR --plan ID [--reason TEXT]\n";
  out << "  execute next --root DIR --plan ID\n";
  out << "  execute all --root DIR --plan ID\n";
  out << "  evidence ingest --root DIR --file EVIDENCE.json\n";
  out << "  attempt resolve --root DIR --attempt ID --resolution STATUS [--note TEXT]\n";
  out << "  recover --root DIR\n";
  out << "  status --root DIR\n";
  out << "  closure --root DIR\n";
  out << "  example rack-replacement --root DIR [--rack R] [--assets a,b]\n\n";
  out << "known change kinds: " << join(known_change_kinds(), ", ") << "\n";
  out << "exit codes: 0 success, 1 failure, 2 usage, 3 store locked, 4 storage, 5 unresolved\n";
  return out.str();
}

int run(const std::vector<std::string>& arguments, std::string& output, std::string& error_output) {
  output.clear();
  error_output.clear();

  auto parsed = parse_arguments(arguments);
  if (!parsed.ok()) {
    error_output = format_error(parsed.error()) + "\n" + usage();
    return kExitUsage;
  }
  const Arguments& args = parsed.value();
  if (args.positional.empty()) {
    output = usage();
    return kExitUsage;
  }

  Session session;
  std::ostringstream out;
  const Status status = dispatch(args, session, out);
  output = out.str();
  if (!status.ok()) {
    error_output = format_error(status.error());
    return exit_code_for(status.error().code);
  }
  return kExitSuccess;
}

}  // namespace fco::cli
