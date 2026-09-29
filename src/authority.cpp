#include "fco/authority.hpp"

#include <array>

namespace fco {
namespace {

constexpr std::array<std::pair<GenerationKind, std::string_view>, 12> kGenerationKindNames = {{
    {GenerationKind::Unspecified, "unspecified"},
    {GenerationKind::FacilityEpoch, "facility-epoch"},
    {GenerationKind::Policy, "policy"},
    {GenerationKind::Dependency, "dependency"},
    {GenerationKind::Topology, "topology"},
    {GenerationKind::Capacity, "capacity"},
    {GenerationKind::Lifecycle, "lifecycle"},
    {GenerationKind::Maintenance, "maintenance"},
    {GenerationKind::Power, "power"},
    {GenerationKind::Cooling, "cooling"},
    {GenerationKind::Asi, "asi"},
    {GenerationKind::Dfi, "dfi"},
}};

}  // namespace

std::string_view to_string(GenerationKind value) noexcept {
  for (const auto& entry : kGenerationKindNames) {
    if (entry.first == value) return entry.second;
  }
  return "invalid";
}

Result<GenerationKind> parse_generation_kind(std::string_view text) {
  for (const auto& entry : kGenerationKindNames) {
    if (entry.second == text) return success(entry.first);
  }
  return failure<GenerationKind>(ErrorCode::InvalidEnumValue, std::string(text),
                                 "unrecognised generation kind");
}

bool GenerationSet::complete() const noexcept {
  return facility_epoch.valid() && policy.valid() && dependency.valid() && topology.valid() &&
         capacity.valid() && lifecycle.valid() && maintenance.valid() && power.valid() &&
         cooling.valid() && asi.valid() && dfi.valid();
}

std::uint64_t GenerationSet::get(GenerationKind kind) const noexcept {
  switch (kind) {
    case GenerationKind::FacilityEpoch:
      return facility_epoch.value();
    case GenerationKind::Policy:
      return policy.value();
    case GenerationKind::Dependency:
      return dependency.value();
    case GenerationKind::Topology:
      return topology.value();
    case GenerationKind::Capacity:
      return capacity.value();
    case GenerationKind::Lifecycle:
      return lifecycle.value();
    case GenerationKind::Maintenance:
      return maintenance.value();
    case GenerationKind::Power:
      return power.value();
    case GenerationKind::Cooling:
      return cooling.value();
    case GenerationKind::Asi:
      return asi.value();
    case GenerationKind::Dfi:
      return dfi.value();
    case GenerationKind::Unspecified:
    default:
      return 0;
  }
}

void GenerationSet::set(GenerationKind kind, std::uint64_t value) noexcept {
  switch (kind) {
    case GenerationKind::FacilityEpoch:
      facility_epoch = FacilityEpoch(value);
      break;
    case GenerationKind::Policy:
      policy = PolicyGeneration(value);
      break;
    case GenerationKind::Dependency:
      dependency = DependencyGeneration(value);
      break;
    case GenerationKind::Topology:
      topology = TopologyGeneration(value);
      break;
    case GenerationKind::Capacity:
      capacity = CapacityGeneration(value);
      break;
    case GenerationKind::Lifecycle:
      lifecycle = LifecycleGeneration(value);
      break;
    case GenerationKind::Maintenance:
      maintenance = MaintenanceGeneration(value);
      break;
    case GenerationKind::Power:
      power = PowerGeneration(value);
      break;
    case GenerationKind::Cooling:
      cooling = CoolingGeneration(value);
      break;
    case GenerationKind::Asi:
      asi = AsiGeneration(value);
      break;
    case GenerationKind::Dfi:
      dfi = DfiGeneration(value);
      break;
    case GenerationKind::Unspecified:
    default:
      break;
  }
}

void GenerationSet::hash_into(Sha256& hasher) const noexcept {
  for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kGenerationKindMax); ++kind) {
    hasher.update_u8(kind);
    hasher.update_u64(get(static_cast<GenerationKind>(kind)));
  }
}

Digest GenerationSet::digest() const {
  Sha256 hasher;
  hasher.update("fco/generation-set/1");
  hash_into(hasher);
  return hasher.finish();
}

std::string GenerationSet::describe() const {
  std::string out;
  for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kGenerationKindMax); ++kind) {
    const auto typed = static_cast<GenerationKind>(kind);
    if (!out.empty()) out += ' ';
    out += to_string(typed);
    out += '=';
    out += std::to_string(get(typed));
  }
  return out;
}

CurrencyCheck check_currency(const GenerationSet& bound, const GenerationSet& observed) {
  CurrencyCheck result;
  for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(kGenerationKindMax); ++kind) {
    const auto typed = static_cast<GenerationKind>(kind);
    const std::uint64_t a = bound.get(typed);
    const std::uint64_t b = observed.get(typed);
    if (a == b) continue;
    GenerationDelta delta;
    delta.kind = typed;
    delta.bound = a;
    delta.current = b;
    if (b > a) {
      result.advanced.push_back(delta);
    } else {
      result.regressed.push_back(delta);
    }
  }
  const bool any_advanced = !result.advanced.empty();
  const bool any_regressed = !result.regressed.empty();
  if (any_advanced && any_regressed) {
    result.outcome = CurrencyOutcome::Mixed;
  } else if (any_advanced) {
    result.outcome = CurrencyOutcome::Advanced;
  } else if (any_regressed) {
    result.outcome = CurrencyOutcome::Regressed;
  } else {
    result.outcome = CurrencyOutcome::Current;
  }
  return result;
}

ErrorCode CurrencyCheck::primary_code() const noexcept {
  switch (outcome) {
    case CurrencyOutcome::Current:
      return ErrorCode::Ok;
    case CurrencyOutcome::Advanced:
    case CurrencyOutcome::Mixed:
      return ErrorCode::StaleGeneration;
    case CurrencyOutcome::Regressed:
      return ErrorCode::GenerationRegression;
    case CurrencyOutcome::Unspecified:
    default:
      return ErrorCode::InternalError;
  }
}

std::string CurrencyCheck::subject_key() const {
  std::string out;
  for (const GenerationDelta& delta : advanced) {
    if (!out.empty()) out += ',';
    out += to_string(delta.kind);
  }
  for (const GenerationDelta& delta : regressed) {
    if (!out.empty()) out += ',';
    out += to_string(delta.kind);
    out += "(regressed)";
  }
  return out;
}

std::string CurrencyCheck::describe() const {
  if (outcome == CurrencyOutcome::Current) return "generations current";
  std::string out;
  for (const GenerationDelta& delta : advanced) {
    if (!out.empty()) out += "; ";
    out += std::string(to_string(delta.kind)) + " advanced " + std::to_string(delta.bound) + " -> " +
           std::to_string(delta.current);
  }
  for (const GenerationDelta& delta : regressed) {
    if (!out.empty()) out += "; ";
    out += std::string(to_string(delta.kind)) + " regressed " + std::to_string(delta.bound) +
           " -> " + std::to_string(delta.current);
  }
  return out;
}

GenerationKind generation_advanced_by(ActionKind action) noexcept {
  switch (action) {
    case ActionKind::DrainWorkloads:
    case ActionKind::RestoreWorkloads:
      return GenerationKind::Asi;
    case ActionKind::MaintenanceIsolate:
    case ActionKind::MaintenanceRelease:
      return GenerationKind::Maintenance;
    case ActionKind::PowerDownAsset:
    case ActionKind::PowerUpAsset:
      return GenerationKind::Power;
    case ActionKind::CoolingIsolate:
    case ActionKind::CoolingRestore:
      return GenerationKind::Cooling;
    case ActionKind::CapacityReserve:
    case ActionKind::CapacityRelease:
      return GenerationKind::Capacity;
    case ActionKind::NetworkQuiesce:
    case ActionKind::NetworkRestore:
      return GenerationKind::Dfi;
    case ActionKind::LifecycleTransition:
    case ActionKind::HardwareSwap:
    case ActionKind::FirmwareStage:
    case ActionKind::Recommission:
    case ActionKind::Decommission:
      return GenerationKind::Lifecycle;
    case ActionKind::VerifyRestoration:
    case ActionKind::RecordNoOp:
    case ActionKind::Unspecified:
    default:
      return GenerationKind::Unspecified;
  }
}

bool AuthorityContext::complete() const noexcept {
  return incarnation.valid() && control_epoch.valid() && actor.valid() && policy.valid();
}

void AuthorityContext::hash_into(Sha256& hasher) const noexcept {
  hasher.update_u64(incarnation.value());
  hasher.update_u64(control_epoch.value());
  hasher.update(actor.value());
  hasher.update_u8(0);
  hasher.update(policy.value());
  hasher.update_u8(0);
  hasher.update(policy_digest.bytes().data(), policy_digest.bytes().size());
}

Digest AuthorityContext::digest() const {
  Sha256 hasher;
  hasher.update("fco/authority-context/1");
  hash_into(hasher);
  return hasher.finish();
}

std::string AuthorityContext::describe() const {
  std::string out = "incarnation=" + incarnation.to_string();
  out += " control-epoch=" + control_epoch.to_string();
  out += " actor=" + actor.to_string();
  out += " policy=" + policy.to_string();
  return out;
}

FencingDecision FencingDecision::allow() {
  FencingDecision decision;
  decision.permitted = true;
  decision.code = ErrorCode::Ok;
  return decision;
}

FencingDecision FencingDecision::deny(ErrorCode code, std::string subject, std::string message) {
  FencingDecision decision;
  decision.permitted = false;
  decision.code = code;
  decision.subject = std::move(subject);
  decision.message = std::move(message);
  return decision;
}

std::string FencingDecision::describe() const {
  if (permitted) return "authority current";
  return std::string(fco::to_string(code)) + " [" + subject + "]: " + message;
}

FencingDecision fence_authority(const AuthorityContext& bound, const AuthorityContext& current) {
  Diagnostics diagnostics;

  if (!bound.complete()) {
    diagnostics.add(ErrorCode::MissingAuthority, "plan-authority",
                    "the plan binding does not carry a complete authority context");
  }
  if (!current.complete()) {
    diagnostics.add(ErrorCode::MissingAuthority, "current-authority",
                    "the caller does not present a complete authority context");
  }
  if (diagnostics.empty()) {
    if (bound.incarnation != current.incarnation) {
      diagnostics.add(ErrorCode::FencedAuthority, "incarnation",
                      "plan authorized under incarnation " + bound.incarnation.to_string() +
                          " but current incarnation is " + current.incarnation.to_string());
    }
    if (current.control_epoch < bound.control_epoch) {
      diagnostics.add(ErrorCode::StaleAuthority, "control-epoch",
                      "caller control epoch " + current.control_epoch.to_string() +
                          " is behind the plan binding " + bound.control_epoch.to_string());
    } else if (current.control_epoch > bound.control_epoch) {
      diagnostics.add(ErrorCode::FencedAuthority, "control-epoch",
                      "plan binding control epoch " + bound.control_epoch.to_string() +
                          " was superseded by " + current.control_epoch.to_string());
    }
    if (bound.actor != current.actor) {
      diagnostics.add(ErrorCode::AuthorityMismatch, "actor",
                      "plan was authorized by " + bound.actor.to_string() +
                          " but the caller is " + current.actor.to_string());
    }
    if (bound.policy != current.policy) {
      diagnostics.add(ErrorCode::PolicyViolation, "policy",
                      "plan bound to policy " + bound.policy.to_string() +
                          " but the active policy is " + current.policy.to_string());
    }
  }

  if (diagnostics.empty()) return FencingDecision::allow();
  const Error& primary = diagnostics.primary();
  return FencingDecision::deny(primary.code, primary.subject, primary.message);
}

}  // namespace fco
