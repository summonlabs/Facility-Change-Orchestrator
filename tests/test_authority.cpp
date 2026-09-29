// Tests for fco/authority.hpp and fco/authority.cpp: generation sets and their
// digest, currency checking in both directions, authority fencing and the
// documented ErrorCode precedence when several denials hold at once.

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fco/authority.hpp"

// The fencing precedence is a numeric contract: the lower code wins.
static_assert(fco::precedence(fco::ErrorCode::MissingAuthority) <
              fco::precedence(fco::ErrorCode::AuthorityMismatch));
static_assert(fco::precedence(fco::ErrorCode::AuthorityMismatch) <
              fco::precedence(fco::ErrorCode::StaleAuthority));
static_assert(fco::precedence(fco::ErrorCode::StaleAuthority) <
              fco::precedence(fco::ErrorCode::FencedAuthority));
static_assert(fco::precedence(fco::ErrorCode::FencedAuthority) <
              fco::precedence(fco::ErrorCode::PolicyViolation));

namespace fco::test {

template <>
struct ValuePrinter<fco::Digest> {
  [[nodiscard]] static std::string print(const fco::Digest& value) { return value.to_hex(); }
};

}  // namespace fco::test

namespace {

using fco::ActionKind;
using fco::AuthorityContext;
using fco::ControlEpoch;
using fco::CurrencyOutcome;
using fco::FencingDecision;
using fco::GenerationKind;
using fco::GenerationSet;
using fco::IncarnationId;
using fco::PolicyId;
using fco::PrincipalId;

// The generation domain is derived from the enum itself so that adding a
// generation kind extends the test instead of invalidating it.
constexpr std::uint32_t kGenerationCount = static_cast<std::uint32_t>(fco::kGenerationKindMax);

[[nodiscard]] GenerationSet make_generations(std::uint64_t base) {
  GenerationSet set;
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    set.set(static_cast<GenerationKind>(raw), base + raw);
  }
  return set;
}

[[nodiscard]] AuthorityContext make_context() {
  AuthorityContext context;
  context.incarnation = IncarnationId(7);
  context.control_epoch = ControlEpoch(11);
  context.actor = PrincipalId("operator-1");
  context.policy = PolicyId("policy-1");
  context.policy_digest = fco::Sha256::of("policy-1");
  return context;
}

using Mutator = void (*)(AuthorityContext&);

void advance_control_epoch(AuthorityContext& context) { context.control_epoch = ControlEpoch(12); }
void regress_control_epoch(AuthorityContext& context) { context.control_epoch = ControlEpoch(10); }
void change_incarnation(AuthorityContext& context) { context.incarnation = IncarnationId(8); }
void change_actor(AuthorityContext& context) { context.actor = PrincipalId("operator-2"); }
void change_policy(AuthorityContext& context) { context.policy = PolicyId("policy-2"); }
void clear_actor(AuthorityContext& context) { context.actor = PrincipalId{}; }
void clear_policy(AuthorityContext& context) { context.policy = PolicyId{}; }

[[nodiscard]] FencingDecision fence_with(Mutator first, Mutator second) {
  const AuthorityContext bound = make_context();
  AuthorityContext current = make_context();
  if (first != nullptr) first(current);
  if (second != nullptr) second(current);
  return fco::fence_authority(bound, current);
}

}  // namespace

// ---------------------------------------------------------------------------
// GenerationKind services.
// ---------------------------------------------------------------------------
FCO_TEST(authority, generation_kind_domain_is_closed_and_named) {
  const std::uint32_t max_raw = static_cast<std::uint32_t>(fco::kGenerationKindMax);
  FCO_CHECK(!fco::is_valid(static_cast<GenerationKind>(0)));
  FCO_CHECK(fco::is_valid(static_cast<GenerationKind>(max_raw)));
  FCO_CHECK(!fco::is_valid(static_cast<GenerationKind>(max_raw + 1u)));
  FCO_CHECK(max_raw >= 9u);  // facility epoch plus the observed generation kinds

  for (std::uint32_t raw = 1; raw <= max_raw; ++raw) {
    const auto kind = static_cast<GenerationKind>(raw);
    const std::string_view name = fco::to_string(kind);
    const auto parsed = fco::parse_generation_kind(name);
    const std::string label = "generation kind " + std::to_string(raw);
    fco::test::check_true(name != std::string_view("invalid"),
                          (label + ": to_string must name the kind").c_str(), __FILE__, __LINE__);
    fco::test::check_true(parsed.ok() && parsed.value() == kind,
                          (label + ": parse must round-trip '" + std::string(name) + "'").c_str(),
                          __FILE__, __LINE__);
  }

  FCO_CHECK_ERROR(fco::parse_generation_kind("not-a-kind"), fco::ErrorCode::InvalidEnumValue);
  FCO_CHECK_EQ(fco::to_string(static_cast<GenerationKind>(0)), std::string_view("unspecified"));
  FCO_CHECK_EQ(fco::to_string(static_cast<GenerationKind>(max_raw + 1u)),
               std::string_view("invalid"));
}

// ---------------------------------------------------------------------------
// GenerationSet.
// ---------------------------------------------------------------------------
FCO_TEST(authority, generation_set_round_trips_every_kind) {
  GenerationSet set;
  FCO_CHECK(!set.complete());

  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    const auto kind = static_cast<GenerationKind>(raw);
    FCO_CHECK_EQ(set.get(kind), std::uint64_t{0});
    set.set(kind, 100 + raw);
    FCO_CHECK_EQ(set.get(kind), std::uint64_t{100 + raw});
  }
  FCO_CHECK(set.complete());

  // Unspecified is not a real generation: it reads as zero and writes nothing.
  FCO_CHECK_EQ(set.get(GenerationKind::Unspecified), std::uint64_t{0});
  const fco::Digest before = set.digest();
  set.set(GenerationKind::Unspecified, 55);
  FCO_CHECK_EQ(set.get(GenerationKind::Unspecified), std::uint64_t{0});
  FCO_CHECK_EQ(set.digest(), before);
}

FCO_TEST(authority, generation_set_is_incomplete_when_any_kind_is_unset) {
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    GenerationSet set = make_generations(10);
    FCO_REQUIRE(set.complete());
    set.set(static_cast<GenerationKind>(raw), 0);
    fco::test::check_true(!set.complete(),
                          ("generation set without kind " + std::to_string(raw) +
                           " must be incomplete").c_str(),
                          __FILE__, __LINE__);
  }
  FCO_CHECK(make_generations(10).complete());
}

FCO_TEST(authority, generation_set_digest_tracks_every_generation) {
  const GenerationSet base = make_generations(10);
  FCO_REQUIRE(base.complete());
  FCO_CHECK(base.digest().valid());
  FCO_CHECK_EQ(base.digest(), make_generations(10).digest());

  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    const auto kind = static_cast<GenerationKind>(raw);
    GenerationSet changed = base;
    changed.set(kind, base.get(kind) + 1);
    fco::test::check_true(changed.digest() != base.digest(),
                          (std::string("changing ") + std::string(fco::to_string(kind)) +
                           " must change the digest").c_str(),
                          __FILE__, __LINE__);
  }

  // Two identical sets are identical identities even when built in a different
  // order.
  GenerationSet reversed;
  for (std::uint32_t offset = 0; offset < kGenerationCount; ++offset) {
    const std::uint32_t raw = kGenerationCount - offset;
    reversed.set(static_cast<GenerationKind>(raw), 10 + raw);
  }
  FCO_CHECK_EQ(reversed.digest(), base.digest());
}

FCO_TEST(authority, generation_set_describe_lists_every_kind) {
  const std::string description = make_generations(10).describe();
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    const auto kind = static_cast<GenerationKind>(raw);
    const std::string name(fco::to_string(kind));
    fco::test::check_true(description.find(name) != std::string::npos,
                          ("describe must mention " + name).c_str(), __FILE__, __LINE__);
    fco::test::check_true(description.find(name + "=" + std::to_string(10 + raw)) != std::string::npos,
                          ("describe must state the value of " + name).c_str(), __FILE__, __LINE__);
  }
}

// ---------------------------------------------------------------------------
// Currency.
// ---------------------------------------------------------------------------
FCO_TEST(authority, check_currency_reports_both_directions) {
  const GenerationSet bound = make_generations(10);

  const fco::CurrencyCheck current = fco::check_currency(bound, bound);
  FCO_CHECK_EQ(current.outcome, CurrencyOutcome::Current);
  FCO_CHECK(current.current());
  FCO_CHECK(current.advanced.empty());
  FCO_CHECK(current.regressed.empty());
  FCO_CHECK_EQ(current.primary_code(), fco::ErrorCode::Ok);
  FCO_CHECK_EQ(current.describe(), std::string("generations current"));
  FCO_CHECK_EQ(current.subject_key(), std::string(""));

  GenerationSet observed = bound;
  observed.set(GenerationKind::Capacity, bound.get(GenerationKind::Capacity) + 3);
  const fco::CurrencyCheck advanced = fco::check_currency(bound, observed);
  FCO_CHECK_EQ(advanced.outcome, CurrencyOutcome::Advanced);
  FCO_CHECK(!advanced.current());
  FCO_REQUIRE(advanced.advanced.size() == 1);
  FCO_CHECK(advanced.regressed.empty());
  FCO_CHECK_EQ(advanced.advanced.front().kind, GenerationKind::Capacity);
  FCO_CHECK_EQ(advanced.advanced.front().bound, bound.get(GenerationKind::Capacity));
  FCO_CHECK_EQ(advanced.advanced.front().current, bound.get(GenerationKind::Capacity) + 3);
  FCO_CHECK_EQ(advanced.primary_code(), fco::ErrorCode::StaleGeneration);
  FCO_CHECK_EQ(advanced.subject_key(), std::string("capacity"));
  FCO_CHECK(advanced.describe().find("capacity advanced") != std::string::npos);

  observed = bound;
  observed.set(GenerationKind::Policy, bound.get(GenerationKind::Policy) - 1);
  const fco::CurrencyCheck regressed = fco::check_currency(bound, observed);
  FCO_CHECK_EQ(regressed.outcome, CurrencyOutcome::Regressed);
  FCO_CHECK(!regressed.current());
  FCO_CHECK(regressed.advanced.empty());
  FCO_REQUIRE(regressed.regressed.size() == 1);
  FCO_CHECK_EQ(regressed.regressed.front().kind, GenerationKind::Policy);
  FCO_CHECK_EQ(regressed.regressed.front().bound, bound.get(GenerationKind::Policy));
  FCO_CHECK_EQ(regressed.regressed.front().current, bound.get(GenerationKind::Policy) - 1);
  FCO_CHECK_EQ(regressed.primary_code(), fco::ErrorCode::GenerationRegression);
  FCO_CHECK_EQ(regressed.subject_key(), std::string("policy(regressed)"));
  FCO_CHECK(regressed.describe().find("policy regressed") != std::string::npos);

  observed = bound;
  observed.set(GenerationKind::Capacity, bound.get(GenerationKind::Capacity) + 1);
  observed.set(GenerationKind::Policy, bound.get(GenerationKind::Policy) - 1);
  const fco::CurrencyCheck mixed = fco::check_currency(bound, observed);
  FCO_CHECK_EQ(mixed.outcome, CurrencyOutcome::Mixed);
  FCO_CHECK(!mixed.current());
  FCO_REQUIRE(mixed.advanced.size() == 1);
  FCO_REQUIRE(mixed.regressed.size() == 1);
  FCO_CHECK_EQ(mixed.advanced.front().kind, GenerationKind::Capacity);
  FCO_CHECK_EQ(mixed.regressed.front().kind, GenerationKind::Policy);
  FCO_CHECK_EQ(mixed.primary_code(), fco::ErrorCode::StaleGeneration);
  FCO_CHECK_EQ(mixed.subject_key(), std::string("capacity,policy(regressed)"));
}

FCO_TEST(authority, check_currency_deltas_follow_kind_order) {
  const GenerationSet bound = make_generations(10);
  GenerationSet observed = bound;
  observed.set(GenerationKind::Topology, bound.get(GenerationKind::Topology) + 1);
  observed.set(GenerationKind::Policy, bound.get(GenerationKind::Policy) + 1);
  observed.set(GenerationKind::Asi, bound.get(GenerationKind::Asi) + 1);

  const fco::CurrencyCheck check = fco::check_currency(bound, observed);
  FCO_CHECK_EQ(check.outcome, CurrencyOutcome::Advanced);
  FCO_REQUIRE(check.advanced.size() == 3);
  FCO_CHECK_EQ(check.advanced[0].kind, GenerationKind::Policy);
  FCO_CHECK_EQ(check.advanced[1].kind, GenerationKind::Topology);
  FCO_CHECK_EQ(check.advanced[2].kind, GenerationKind::Asi);
  FCO_CHECK_EQ(check.subject_key(), std::string("policy,topology,asi"));
}

FCO_TEST(authority, currency_primary_code_maps_from_the_outcome) {
  fco::CurrencyCheck unset;
  FCO_CHECK_EQ(unset.outcome, CurrencyOutcome::Unspecified);
  FCO_CHECK(!unset.current());
  FCO_CHECK_EQ(unset.primary_code(), fco::ErrorCode::InternalError);
  FCO_CHECK_EQ(unset.describe(), std::string(""));
}

// ---------------------------------------------------------------------------
// Authority fencing.
// ---------------------------------------------------------------------------
// Regression test: an allowed fencing decision carries permitted == true and
// code == Ok, and describe() renders it as "authority current". This caught
// allow() returning a default-constructed decision, whose permitted member is
// false, which made a permitted decision indistinguishable from a denial.
FCO_TEST(authority, fence_authority_permits_an_identical_context) {
  const AuthorityContext context = make_context();
  const FencingDecision decision = fco::fence_authority(context, context);
  FCO_CHECK_EQ(decision.code, fco::ErrorCode::Ok);
  FCO_CHECK(decision.permitted);
  FCO_CHECK_EQ(decision.code, fco::ErrorCode::Ok);
  FCO_CHECK_EQ(decision.describe(), std::string("authority current"));

  const FencingDecision allowed = FencingDecision::allow();
  FCO_CHECK(allowed.permitted);
  FCO_CHECK_EQ(allowed.code, fco::ErrorCode::Ok);

  const FencingDecision denied =
      FencingDecision::deny(fco::ErrorCode::StaleAuthority, "control-epoch", "behind");
  FCO_CHECK(!denied.permitted);
  FCO_CHECK_EQ(denied.describe(), std::string("stale-authority [control-epoch]: behind"));
}

FCO_TEST(authority, generation_advanced_by_is_defined_for_every_action) {
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::DrainWorkloads), GenerationKind::Asi);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::RestoreWorkloads), GenerationKind::Asi);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::MaintenanceIsolate),
               GenerationKind::Maintenance);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::MaintenanceRelease),
               GenerationKind::Maintenance);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::PowerDownAsset), GenerationKind::Power);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::PowerUpAsset), GenerationKind::Power);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::CoolingIsolate), GenerationKind::Cooling);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::CoolingRestore), GenerationKind::Cooling);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::CapacityReserve), GenerationKind::Capacity);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::CapacityRelease), GenerationKind::Capacity);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::NetworkQuiesce), GenerationKind::Dfi);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::NetworkRestore), GenerationKind::Dfi);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::LifecycleTransition),
               GenerationKind::Lifecycle);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::HardwareSwap), GenerationKind::Lifecycle);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::FirmwareStage), GenerationKind::Lifecycle);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::Recommission), GenerationKind::Lifecycle);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::Decommission), GenerationKind::Lifecycle);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::VerifyRestoration),
               GenerationKind::Unspecified);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::RecordNoOp), GenerationKind::Unspecified);
  FCO_CHECK_EQ(fco::generation_advanced_by(ActionKind::Unspecified), GenerationKind::Unspecified);

  const std::uint32_t max_raw = static_cast<std::uint32_t>(fco::kActionKindMax);
  for (std::uint32_t raw = 1; raw <= max_raw; ++raw) {
    const GenerationKind advanced =
        fco::generation_advanced_by(static_cast<ActionKind>(raw));
    fco::test::check_true(advanced == GenerationKind::Unspecified || fco::is_valid(advanced),
                          ("action " + std::to_string(raw) +
                           " must map to a real generation kind").c_str(),
                          __FILE__, __LINE__);
  }
}

FCO_TEST(authority, fence_authority_denies_each_condition) {
  const FencingDecision incarnation = fence_with(change_incarnation, nullptr);
  FCO_CHECK(!incarnation.permitted);
  FCO_CHECK_EQ(incarnation.code, fco::ErrorCode::FencedAuthority);
  FCO_CHECK_EQ(incarnation.subject, std::string("incarnation"));

  const FencingDecision advanced_epoch = fence_with(advance_control_epoch, nullptr);
  FCO_CHECK(!advanced_epoch.permitted);
  FCO_CHECK_EQ(advanced_epoch.code, fco::ErrorCode::FencedAuthority);
  FCO_CHECK_EQ(advanced_epoch.subject, std::string("control-epoch"));

  const FencingDecision regressed_epoch = fence_with(regress_control_epoch, nullptr);
  FCO_CHECK(!regressed_epoch.permitted);
  FCO_CHECK_EQ(regressed_epoch.code, fco::ErrorCode::StaleAuthority);
  FCO_CHECK_EQ(regressed_epoch.subject, std::string("control-epoch"));

  const FencingDecision actor = fence_with(change_actor, nullptr);
  FCO_CHECK(!actor.permitted);
  FCO_CHECK_EQ(actor.code, fco::ErrorCode::AuthorityMismatch);
  FCO_CHECK_EQ(actor.subject, std::string("actor"));

  const FencingDecision policy = fence_with(change_policy, nullptr);
  FCO_CHECK(!policy.permitted);
  FCO_CHECK_EQ(policy.code, fco::ErrorCode::PolicyViolation);
  FCO_CHECK_EQ(policy.subject, std::string("policy"));
}

FCO_TEST(authority, fence_authority_requires_complete_contexts) {
  const AuthorityContext complete = make_context();
  FCO_REQUIRE(complete.complete());

  const FencingDecision missing_current = fence_with(clear_actor, nullptr);
  FCO_CHECK(!missing_current.permitted);
  FCO_CHECK_EQ(missing_current.code, fco::ErrorCode::MissingAuthority);
  FCO_CHECK_EQ(missing_current.subject, std::string("current-authority"));

  const FencingDecision missing_current_policy = fence_with(clear_policy, nullptr);
  FCO_CHECK_EQ(missing_current_policy.code, fco::ErrorCode::MissingAuthority);
  FCO_CHECK_EQ(missing_current_policy.subject, std::string("current-authority"));

  AuthorityContext incomplete_bound = make_context();
  incomplete_bound.incarnation = IncarnationId{};
  const FencingDecision missing_bound = fco::fence_authority(incomplete_bound, complete);
  FCO_CHECK(!missing_bound.permitted);
  FCO_CHECK_EQ(missing_bound.code, fco::ErrorCode::MissingAuthority);
  FCO_CHECK_EQ(missing_bound.subject, std::string("plan-authority"));

  // Both sides incomplete: the documented tie-break is the subject key.
  const FencingDecision missing_both = fco::fence_authority(incomplete_bound, AuthorityContext{});
  FCO_CHECK_EQ(missing_both.code, fco::ErrorCode::MissingAuthority);
  FCO_CHECK_EQ(missing_both.subject, std::string("current-authority"));

  // An incomplete context outranks every other denial: the completeness check
  // runs first and the remaining comparisons are not reached.
  const FencingDecision incomplete_and_mismatched = fence_with(clear_actor, change_policy);
  FCO_CHECK_EQ(incomplete_and_mismatched.code, fco::ErrorCode::MissingAuthority);
}

FCO_TEST(authority, fence_authority_precedence_when_conditions_combine) {
  // The header numbers AuthorityMismatch=301, StaleAuthority=302,
  // FencedAuthority=303, PolicyViolation=304, and lower numbers win. An actor
  // mismatch therefore outranks a superseded control epoch.
  FCO_CHECK_EQ(fence_with(advance_control_epoch, change_actor).code,
               fco::ErrorCode::AuthorityMismatch);
  FCO_CHECK_EQ(fence_with(change_actor, change_policy).code,
               fco::ErrorCode::AuthorityMismatch);
  FCO_CHECK_EQ(fence_with(regress_control_epoch, change_actor).code,
               fco::ErrorCode::AuthorityMismatch);
  FCO_CHECK_EQ(fence_with(change_incarnation, change_actor).code,
               fco::ErrorCode::AuthorityMismatch);

  // Among the remaining denials, FencedAuthority(303) precedes
  // PolicyViolation(304), and StaleAuthority(302) precedes it as well.
  FCO_CHECK_EQ(fence_with(change_incarnation, change_policy).code,
               fco::ErrorCode::FencedAuthority);
  FCO_CHECK_EQ(fence_with(advance_control_epoch, change_policy).code,
               fco::ErrorCode::FencedAuthority);
  FCO_CHECK_EQ(fence_with(regress_control_epoch, change_policy).code,
               fco::ErrorCode::StaleAuthority);

  // Two denials with the same code are separated by their subject key.
  const FencingDecision same_code = fence_with(change_incarnation, advance_control_epoch);
  FCO_CHECK_EQ(same_code.code, fco::ErrorCode::FencedAuthority);
  FCO_CHECK_EQ(same_code.subject, std::string("control-epoch"));
}

// ---------------------------------------------------------------------------
// AuthorityContext.
// ---------------------------------------------------------------------------
FCO_TEST(authority, authority_context_completeness_digest_and_describe) {
  const AuthorityContext context = make_context();
  FCO_CHECK(context.complete());
  FCO_CHECK(context.incarnation.valid());
  FCO_CHECK(context.control_epoch.valid());
  FCO_CHECK(context.actor.valid());
  FCO_CHECK(context.policy.valid());

  FCO_CHECK_EQ(context.digest(), make_context().digest());
  FCO_CHECK(context.digest().valid());

  AuthorityContext changed = context;
  changed.incarnation = IncarnationId(8);
  FCO_CHECK(changed.digest() != context.digest());

  changed = context;
  changed.control_epoch = ControlEpoch(12);
  FCO_CHECK(changed.digest() != context.digest());

  changed = context;
  changed.actor = PrincipalId("operator-2");
  FCO_CHECK(changed.digest() != context.digest());

  changed = context;
  changed.policy = PolicyId("policy-2");
  FCO_CHECK(changed.digest() != context.digest());

  changed = context;
  changed.policy_digest = fco::Sha256::of("policy-2");
  FCO_CHECK(changed.digest() != context.digest());

  const std::string description = context.describe();
  FCO_CHECK(description.find("incarnation=7") != std::string::npos);
  FCO_CHECK(description.find("control-epoch=11") != std::string::npos);
  FCO_CHECK(description.find("actor=operator-1") != std::string::npos);
  FCO_CHECK(description.find("policy=policy-1") != std::string::npos);

  AuthorityContext incomplete = context;
  incomplete.incarnation = IncarnationId{};
  FCO_CHECK(!incomplete.complete());

  incomplete = context;
  incomplete.control_epoch = ControlEpoch{};
  FCO_CHECK(!incomplete.complete());

  incomplete = context;
  incomplete.actor = PrincipalId{};
  FCO_CHECK(!incomplete.complete());

  incomplete = context;
  incomplete.policy = PolicyId{};
  FCO_CHECK(!incomplete.complete());

  // The completeness contract covers the identity fields above; the policy
  // digest is carried and hashed but is not part of complete().
  AuthorityContext without_policy_digest = context;
  without_policy_digest.policy_digest = fco::Digest{};
  FCO_CHECK(!without_policy_digest.policy_digest.valid());
  FCO_CHECK(without_policy_digest.complete());
}

FCO_TEST_MAIN
