// Tests for fco/domain.hpp and fco/domain.cpp: enum domain services (zero is
// always invalid, max is valid, max+1 is invalid, names round-trip through the
// parsers), the owning-domain map, idempotency/destructiveness classification,
// ActionRequest, Predicate and EffectSpec identity behaviour.

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fco/domain.hpp"

namespace fco::test {

template <>
struct ValuePrinter<fco::Digest> {
  [[nodiscard]] static std::string print(const fco::Digest& value) { return value.to_hex(); }
};

}  // namespace fco::test

namespace {

using fco::ActionKind;
using fco::ActionRequest;
using fco::AssetId;
using fco::DomainKind;
using fco::EffectKind;
using fco::EffectSpec;
using fco::LifecycleGeneration;
using fco::Predicate;
using fco::PredicateKind;
using fco::RackId;
using fco::Reversibility;
using fco::SafetyClass;
using fco::SiteId;
using fco::VerificationMode;

// Every enum domain must reject the reserved zero value, accept its documented
// maximum, reject the value above it, and round-trip every valid enumerator
// through to_string/parse_*. decode_enum must agree with is_valid exactly.
template <class Enum, class ToStringFn, class ParseFn>
void verify_enum_domain(const std::string& name, std::uint8_t maximum, ToStringFn to_string_fn,
                        ParseFn parse_fn) {
  const auto as_enum = [](std::uint32_t raw) { return static_cast<Enum>(static_cast<std::uint8_t>(raw)); };
  const std::uint32_t max_raw = static_cast<std::uint32_t>(maximum);

  fco::test::check_true(!fco::is_valid(as_enum(0)),
                        (name + ": is_valid(0) must be false").c_str(), __FILE__, __LINE__);
  fco::test::check_true(fco::is_valid(as_enum(max_raw)),
                        (name + ": is_valid(max) must be true").c_str(), __FILE__, __LINE__);
  fco::test::check_true(!fco::is_valid(as_enum(max_raw + 1u)),
                        (name + ": is_valid(max+1) must be false").c_str(), __FILE__, __LINE__);

  for (std::uint32_t raw = 1; raw <= max_raw; ++raw) {
    const Enum value = as_enum(raw);
    const std::string_view text = to_string_fn(value);
    const std::string label = name + " enumerator " + std::to_string(raw);

    fco::test::check_true(text != std::string_view("invalid"),
                          (label + ": to_string must name the enumerator").c_str(), __FILE__,
                          __LINE__);

    const auto parsed = parse_fn(text);
    fco::test::check_true(parsed.ok() && parsed.value() == value,
                          (label + ": parse_* must round-trip '" + std::string(text) + "'").c_str(),
                          __FILE__, __LINE__);
  }

  const auto unknown_name = parse_fn("not-an-enumerator");
  fco::test::check_true(!unknown_name.ok() &&
                            unknown_name.error().code == fco::ErrorCode::InvalidEnumValue,
                        (name + ": an unknown name must be invalid-enum-value").c_str(), __FILE__,
                        __LINE__);

  fco::test::check_true(to_string_fn(as_enum(max_raw + 1u)) == std::string_view("invalid"),
                        (name + ": an out-of-domain value has no name").c_str(), __FILE__, __LINE__);

  for (std::uint32_t raw = 0; raw <= max_raw + 1u; ++raw) {
    const auto decoded = fco::decode_enum<Enum>(static_cast<std::uint8_t>(raw), name);
    const bool should_decode = raw >= 1u && raw <= max_raw;
    const std::string label = name + ": decode_enum(" + std::to_string(raw) + ")";
    fco::test::check_true(decoded.ok() == should_decode, label.c_str(), __FILE__, __LINE__);
    if (!decoded.ok()) {
      fco::test::check_true(decoded.error().code == fco::ErrorCode::InvalidEnumValue,
                            (label + " must report invalid-enum-value").c_str(), __FILE__,
                            __LINE__);
    } else {
      fco::test::check_true(decoded.value() == as_enum(raw),
                            (label + " must return the decoded value").c_str(), __FILE__, __LINE__);
    }
  }
}

#define FCO_VERIFY_ENUM(EnumType, MaxExpr, ParseFn)                       \
  verify_enum_domain<fco::EnumType>(                                      \
      #EnumType, static_cast<std::uint8_t>(MaxExpr),                      \
      [](fco::EnumType value) { return fco::to_string(value); },          \
      [](std::string_view text) { return fco::ParseFn(text); })

[[nodiscard]] ActionRequest make_request(ActionKind kind, const char* asset) {
  ActionRequest request;
  request.kind = kind;
  request.owner = fco::owning_domain(kind);
  request.asset = AssetId(asset);
  request.rack = RackId("rack-1");
  request.site = SiteId("site-1");
  request.lifecycle_generation = LifecycleGeneration(4);
  return request;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enum services.
// ---------------------------------------------------------------------------
FCO_TEST(domain, every_enum_domain_is_closed_and_named) {
  FCO_VERIFY_ENUM(DomainKind, fco::kDomainKindMax, parse_domain_kind);
  FCO_VERIFY_ENUM(SafetyClass, fco::kSafetyClassMax, parse_safety_class);
  FCO_VERIFY_ENUM(Reversibility, fco::kReversibilityMax, parse_reversibility);
  FCO_VERIFY_ENUM(VerificationMode, fco::kVerificationModeMax, parse_verification_mode);
  FCO_VERIFY_ENUM(ActionKind, fco::kActionKindMax, parse_action_kind);
  FCO_VERIFY_ENUM(EffectKind, fco::kEffectKindMax, parse_effect_kind);
  FCO_VERIFY_ENUM(PredicateKind, fco::kPredicateKindMax, parse_predicate_kind);
  FCO_VERIFY_ENUM(AssetLifecycle, fco::kAssetLifecycleMax, parse_asset_lifecycle);
  FCO_VERIFY_ENUM(PowerState, fco::kPowerStateMax, parse_power_state);
  FCO_VERIFY_ENUM(CoolingState, fco::kCoolingStateMax, parse_cooling_state);
  FCO_VERIFY_ENUM(MaintenanceState, fco::kMaintenanceStateMax, parse_maintenance_state);
  FCO_VERIFY_ENUM(NetworkState, fco::kNetworkStateMax, parse_network_state);
  FCO_VERIFY_ENUM(WorkloadState, fco::kWorkloadStateMax, parse_workload_state);
  FCO_VERIFY_ENUM(PlanState, fco::kPlanStateMax, parse_plan_state);
  FCO_VERIFY_ENUM(StepStatus, fco::kStepStatusMax, parse_step_status);
  FCO_VERIFY_ENUM(AttemptStatus, fco::kAttemptStatusMax, parse_attempt_status);
}

FCO_TEST(domain, zero_is_never_a_valid_enum_value) {
  FCO_CHECK(!fco::is_valid(static_cast<DomainKind>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<SafetyClass>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<Reversibility>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<VerificationMode>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<ActionKind>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<EffectKind>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<PredicateKind>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<fco::AssetLifecycle>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<fco::PowerState>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<fco::CoolingState>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<fco::MaintenanceState>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<fco::NetworkState>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<fco::WorkloadState>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<fco::PlanState>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<fco::StepStatus>(0)));
  FCO_CHECK(!fco::is_valid(static_cast<fco::AttemptStatus>(0)));

  FCO_CHECK_EQ(fco::to_string(static_cast<fco::PowerState>(0)), std::string_view("unspecified"));
  FCO_CHECK_EQ(fco::to_string(static_cast<fco::PlanState>(0)), std::string_view("unspecified"));
  FCO_CHECK_EQ(fco::to_string(static_cast<fco::AssetLifecycle>(0)), std::string_view("unspecified"));
}

FCO_TEST(domain, decode_enum_rejects_zero_and_out_of_domain) {
  FCO_CHECK_ERROR(fco::decode_enum<fco::PlanState>(0, "plan-state"), fco::ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::PlanState>(14, "plan-state"),
                  fco::ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::PlanState>(255, "plan-state"),
                  fco::ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<ActionKind>(0, "action-kind"), fco::ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<ActionKind>(20, "action-kind"),
                  fco::ErrorCode::InvalidEnumValue);

  const auto decoded = fco::decode_enum<fco::PlanState>(13, "plan-state");
  FCO_REQUIRE(decoded.ok());
  FCO_CHECK_EQ(decoded.value(), fco::PlanState::Cancelled);
  FCO_CHECK_EQ(decoded.value(), fco::kPlanStateMax);

  const auto zero_power = fco::decode_enum<fco::PowerState>(0, "power");
  FCO_REQUIRE(!zero_power.ok());
  FCO_CHECK_EQ(zero_power.error().subject, std::string("power"));
}

// ---------------------------------------------------------------------------
// Owning domain map.
// ---------------------------------------------------------------------------
FCO_TEST(domain, owning_domain_is_defined_and_stable_for_every_action) {
  const std::uint32_t max_raw = static_cast<std::uint32_t>(fco::kActionKindMax);
  for (std::uint32_t raw = 1; raw <= max_raw; ++raw) {
    const auto kind = static_cast<ActionKind>(raw);
    const DomainKind owner = fco::owning_domain(kind);
    const std::string label = "action " + std::to_string(raw);
    fco::test::check_true(fco::is_valid(owner),
                          (label + " must map to a real owning domain").c_str(), __FILE__,
                          __LINE__);
    fco::test::check_true(fco::owning_domain(kind) == owner,
                          (label + " mapping must be stable").c_str(), __FILE__, __LINE__);
  }

  FCO_CHECK_EQ(fco::owning_domain(ActionKind::Unspecified), DomainKind::Unspecified);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::DrainWorkloads), DomainKind::Asi);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::RestoreWorkloads), DomainKind::Asi);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::MaintenanceIsolate), DomainKind::Maintenance);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::MaintenanceRelease), DomainKind::Maintenance);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::PowerDownAsset), DomainKind::Power);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::PowerUpAsset), DomainKind::Power);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::CoolingIsolate), DomainKind::Cooling);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::CoolingRestore), DomainKind::Cooling);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::CapacityReserve), DomainKind::Capacity);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::CapacityRelease), DomainKind::Capacity);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::NetworkQuiesce), DomainKind::Dfi);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::NetworkRestore), DomainKind::Dfi);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::LifecycleTransition), DomainKind::Lifecycle);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::HardwareSwap), DomainKind::Lifecycle);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::FirmwareStage), DomainKind::Lifecycle);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::Recommission), DomainKind::Lifecycle);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::Decommission), DomainKind::Lifecycle);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::VerifyRestoration), DomainKind::Verification);
  FCO_CHECK_EQ(fco::owning_domain(ActionKind::RecordNoOp), DomainKind::PhysicalAsset);
}

FCO_TEST(domain, idempotency_and_destructiveness_are_documented_per_action) {
  const ActionKind idempotent[] = {
      ActionKind::DrainWorkloads,     ActionKind::MaintenanceIsolate,
      ActionKind::MaintenanceRelease, ActionKind::PowerDownAsset,
      ActionKind::PowerUpAsset,       ActionKind::CoolingIsolate,
      ActionKind::CoolingRestore,     ActionKind::CapacityReserve,
      ActionKind::CapacityRelease,    ActionKind::NetworkQuiesce,
      ActionKind::NetworkRestore,     ActionKind::VerifyRestoration,
      ActionKind::RecordNoOp,
  };
  const ActionKind destructive[] = {
      ActionKind::HardwareSwap, ActionKind::FirmwareStage, ActionKind::Decommission,
      ActionKind::PowerDownAsset, ActionKind::CoolingIsolate,
  };

  const auto contains = [](const ActionKind* begin, const ActionKind* end, ActionKind value) {
    for (const ActionKind* it = begin; it != end; ++it) {
      if (*it == value) return true;
    }
    return false;
  };

  const std::uint32_t max_raw = static_cast<std::uint32_t>(fco::kActionKindMax);
  for (std::uint32_t raw = 0; raw <= max_raw; ++raw) {
    const auto kind = static_cast<ActionKind>(raw);
    const bool expected_idempotent = contains(idempotent, idempotent + std::size(idempotent), kind);
    const bool expected_destructive =
        contains(destructive, destructive + std::size(destructive), kind);
    const std::string label = "action " + std::to_string(raw);
    fco::test::check_true(fco::is_idempotent_action(kind) == expected_idempotent,
                          (label + ": idempotency classification").c_str(), __FILE__, __LINE__);
    fco::test::check_true(fco::is_destructive_action(kind) == expected_destructive,
                          (label + ": destructiveness classification").c_str(), __FILE__, __LINE__);
  }

  FCO_CHECK(fco::is_idempotent_action(ActionKind::VerifyRestoration));
  FCO_CHECK(fco::is_destructive_action(ActionKind::Decommission));
  FCO_CHECK(!fco::is_idempotent_action(ActionKind::Decommission));
  FCO_CHECK(fco::is_destructive_action(ActionKind::HardwareSwap));
  FCO_CHECK(!fco::is_idempotent_action(ActionKind::HardwareSwap));
  FCO_CHECK(!fco::is_destructive_action(ActionKind::RecordNoOp));
  FCO_CHECK(fco::is_idempotent_action(ActionKind::RecordNoOp));
  FCO_CHECK(!fco::is_idempotent_action(ActionKind::Unspecified));
  FCO_CHECK(!fco::is_destructive_action(ActionKind::Unspecified));
}

// ---------------------------------------------------------------------------
// ActionRequest.
// ---------------------------------------------------------------------------
FCO_TEST(domain, action_request_completeness_is_field_by_field) {
  ActionRequest request;
  FCO_CHECK(!request.complete());

  request.kind = ActionKind::PowerDownAsset;
  FCO_CHECK(!request.complete());

  request.owner = fco::owning_domain(request.kind);
  FCO_CHECK(!request.complete());

  request.asset = AssetId("asset-1");
  FCO_CHECK(!request.complete());

  request.rack = RackId("rack-1");
  FCO_CHECK(!request.complete());

  request.site = SiteId("site-1");
  FCO_CHECK(!request.complete());

  request.lifecycle_generation = LifecycleGeneration(1);
  FCO_CHECK(request.complete());

  request.kind = ActionKind::Unspecified;
  FCO_CHECK(!request.complete());
  request.kind = ActionKind::PowerDownAsset;

  request.owner = DomainKind::Unspecified;
  FCO_CHECK(!request.complete());
  request.owner = DomainKind::Power;

  request.lifecycle_generation = LifecycleGeneration(0);
  FCO_CHECK(!request.complete());
}

FCO_TEST(domain, action_request_describe_and_digest) {
  const ActionRequest base = make_request(ActionKind::PowerDownAsset, "asset-1");
  FCO_REQUIRE(base.complete());

  const std::string description = base.describe();
  FCO_CHECK(description.find("asset-1") != std::string::npos);
  FCO_CHECK(description.find("power-down-asset") != std::string::npos);
  FCO_CHECK(description.find("rack-1") != std::string::npos);

  FCO_CHECK_EQ(base.digest(), make_request(ActionKind::PowerDownAsset, "asset-1").digest());

  ActionRequest other = base;
  other.hardware_generation = fco::HardwareGeneration(3);
  FCO_CHECK(base.digest() != other.digest());

  other = base;
  other.firmware_generation = fco::FirmwareGeneration(3);
  FCO_CHECK(base.digest() != other.digest());

  other = base;
  other.lifecycle_generation = LifecycleGeneration(base.lifecycle_generation.value() + 1);
  FCO_CHECK(base.digest() != other.digest());

  other = base;
  other.kind = ActionKind::PowerUpAsset;
  FCO_CHECK(base.digest() != other.digest());

  other = base;
  other.owner = DomainKind::Lifecycle;
  FCO_CHECK(base.digest() != other.digest());

  other = base;
  other.asset = AssetId("asset-2");
  FCO_CHECK(base.digest() != other.digest());

  other = base;
  other.rack = RackId("rack-2");
  FCO_CHECK(base.digest() != other.digest());

  other = base;
  other.site = SiteId("site-2");
  FCO_CHECK(base.digest() != other.digest());

  other = base;
  other.target_state = 7;
  FCO_CHECK(base.digest() != other.digest());

  other = base;
  other.target_value = 7;
  FCO_CHECK(base.digest() != other.digest());

  FCO_CHECK(base.digest().valid());
}

// ---------------------------------------------------------------------------
// Predicate.
// ---------------------------------------------------------------------------
FCO_TEST(domain, predicate_subject_key_is_stable_and_kind_specific) {
  Predicate base;
  base.kind = PredicateKind::PowerStateIs;
  base.asset = AssetId("asset-1");
  base.predecessor = fco::StepId("drain");
  base.expected_state = 3;
  base.expected_value = 0;

  const std::string key = base.subject_key();
  FCO_CHECK_EQ(base.subject_key(), key);
  FCO_CHECK(key.find("power-state-is") != std::string::npos);
  FCO_CHECK(key.find("asset-1") != std::string::npos);

  Predicate other = base;
  other.asset = AssetId("asset-2");
  FCO_CHECK(base.subject_key() != other.subject_key());

  other = base;
  other.predecessor = fco::StepId("isolate");
  FCO_CHECK(base.subject_key() != other.subject_key());

  other = base;
  other.expected_state = 4;
  FCO_CHECK(base.subject_key() != other.subject_key());

  other = base;
  other.expected_value = 9;
  FCO_CHECK(base.subject_key() != other.subject_key());

  const std::uint32_t max_raw = static_cast<std::uint32_t>(fco::kPredicateKindMax);
  for (std::uint32_t raw = 1; raw <= max_raw; ++raw) {
    Predicate kinded = base;
    kinded.kind = static_cast<PredicateKind>(raw);
    fco::test::check_true(kinded.subject_key() != base.subject_key() ||
                              kinded.kind == PredicateKind::PowerStateIs,
                          ("predicate kind " + std::to_string(raw) + " must key differently").c_str(),
                          __FILE__, __LINE__);
  }

  FCO_CHECK(base.describe().find("asset-1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// EffectSpec.
// ---------------------------------------------------------------------------
FCO_TEST(domain, effect_spec_completeness_and_describe) {
  EffectSpec effect;
  FCO_CHECK(!effect.complete());

  effect.kind = EffectKind::PowerStateChange;
  FCO_CHECK(!effect.complete());

  effect.asset = AssetId("asset-1");
  FCO_CHECK(effect.complete());

  effect.kind = EffectKind::Unspecified;
  FCO_CHECK(!effect.complete());
  effect.kind = EffectKind::PowerStateChange;

  effect.expected_state = 3;
  effect.expected_value = 1;
  const std::string description = effect.describe();
  FCO_CHECK(description.find("power-state-change") != std::string::npos);
  FCO_CHECK(description.find("asset-1") != std::string::npos);
  FCO_CHECK(description.find("expecting=3") != std::string::npos);
}

FCO_TEST_MAIN
