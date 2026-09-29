// Tests for fco/strong.hpp and fco/strong.cpp: identifier syntax, strict
// decimal parsing, counter overflow behaviour, unset-value semantics, type
// separation between strong identities, deterministic identifier derivation,
// and hashability.

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "fco/strong.hpp"

namespace {

using fco::AssetId;
using fco::AttemptId;
using fco::AttemptOrdinal;
using fco::EvidenceId;
using fco::FacilityEpoch;
using fco::PlanId;
using fco::PlanRevision;
using fco::RackId;
using fco::Revision;
using fco::StepId;

template <class A, class B>
concept Comparable = requires(const A& a, const B& b) { a == b; };

// Strong types with different tags are not interchangeable: this is the
// compile-time half of the identity contract.
static_assert(Comparable<AssetId, AssetId>);
static_assert(!Comparable<AssetId, RackId>);
static_assert(!Comparable<AssetId, std::string>);
static_assert(!Comparable<FacilityEpoch, fco::LifecycleGeneration>);
static_assert(!Comparable<fco::LifecycleGeneration, fco::CapacityGeneration>);
static_assert(!std::is_invocable_v<std::equal_to<>, AssetId, RackId>);
static_assert(std::is_invocable_v<std::equal_to<>, AssetId, AssetId>);
static_assert(!std::is_convertible_v<AssetId, std::string>);
static_assert(!std::is_convertible_v<AssetId, RackId>);
static_assert(!std::is_convertible_v<std::string, AssetId>);
static_assert(!std::is_convertible_v<FacilityEpoch, std::uint64_t>);
static_assert(std::is_same_v<AssetId::tag_type, fco::AssetIdTag>);
static_assert(std::is_same_v<FacilityEpoch::value_type, std::uint64_t>);

// 66 characters: every permitted character class, deliberately longer than the
// 64-character identifier bound so that the bound itself is exercised.
constexpr std::string_view kIdentifierAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-";

}  // namespace

// ---------------------------------------------------------------------------
// Identifier syntax.
// ---------------------------------------------------------------------------
FCO_TEST(strong, identifier_syntax_accepts_the_documented_alphabet) {
  FCO_CHECK(fco::is_valid_identifier("a"));
  FCO_CHECK(fco::is_valid_identifier("asset-0001"));
  FCO_CHECK(fco::is_valid_identifier("A-Z_a.z:0-9"));
  FCO_CHECK(fco::is_valid_identifier("0123456789._:-"));
  FCO_CHECK(fco::is_valid_identifier(kIdentifierAlphabet.substr(0, 64)));

  FCO_CHECK_EQ(kIdentifierAlphabet.size(), std::size_t{66});
  FCO_CHECK(!fco::is_valid_identifier(kIdentifierAlphabet));

  const std::string sixty_four(64, 'x');
  FCO_CHECK(fco::is_valid_identifier(sixty_four));
  FCO_CHECK_EQ(sixty_four.size(), std::size_t{64});

  const auto normalized = fco::normalize_identifier("asset-0001");
  FCO_REQUIRE(normalized.ok());
  FCO_CHECK_EQ(normalized.value(), std::string("asset-0001"));

  const auto parsed = AssetId::parse("asset-0001");
  FCO_REQUIRE(parsed.ok());
  FCO_CHECK_EQ(parsed.value().to_string(), std::string("asset-0001"));
  FCO_CHECK(parsed.value().valid());
  FCO_CHECK_EQ(parsed.value().describe(), std::string("asset-id 'asset-0001'"));
}

FCO_TEST(strong, identifier_syntax_rejects_empty_long_whitespace_and_foreign_characters) {
  FCO_CHECK(!fco::is_valid_identifier(""));
  FCO_CHECK(!fco::is_valid_identifier(" asset"));
  FCO_CHECK(!fco::is_valid_identifier("asset "));
  FCO_CHECK(!fco::is_valid_identifier("ass et"));
  FCO_CHECK(!fco::is_valid_identifier("asset#1"));
  FCO_CHECK(!fco::is_valid_identifier("asset/1"));
  FCO_CHECK(!fco::is_valid_identifier("asset\n1"));
  FCO_CHECK(!fco::is_valid_identifier("\xC3\xA9"));
  FCO_CHECK(!fco::is_valid_identifier("\xFF"));

  const std::string sixty_five(65, 'x');
  FCO_CHECK(!fco::is_valid_identifier(sixty_five));

  FCO_CHECK_ERROR(fco::normalize_identifier(""), fco::ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(fco::normalize_identifier(" asset"), fco::ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(fco::normalize_identifier("asset#1"), fco::ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(fco::normalize_identifier("/"), fco::ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(fco::normalize_identifier("\xC3\xA9"), fco::ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(fco::normalize_identifier(sixty_five), fco::ErrorCode::BoundedLimitExceeded);
  // Length is rejected before the character set is inspected.
  const std::string long_and_invalid = std::string(65, '#');
  FCO_CHECK_ERROR(fco::normalize_identifier(long_and_invalid),
                  fco::ErrorCode::BoundedLimitExceeded);

  FCO_CHECK_ERROR(AssetId::parse(""), fco::ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(AssetId::parse("bad id"), fco::ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(AssetId::parse(sixty_five), fco::ErrorCode::BoundedLimitExceeded);
}

// ---------------------------------------------------------------------------
// Strict decimal parsing.
// ---------------------------------------------------------------------------
FCO_TEST(strong, parse_bounded_decimal_accepts_canonical_digits) {
  const auto zero = fco::parse_bounded_decimal("0", 0);
  FCO_REQUIRE(zero.ok());
  FCO_CHECK_EQ(zero.value(), std::uint64_t{0});

  const auto one = fco::parse_bounded_decimal("1", 1);
  FCO_REQUIRE(one.ok());
  FCO_CHECK_EQ(one.value(), std::uint64_t{1});

  const auto bound = fco::parse_bounded_decimal("100", 100);
  FCO_REQUIRE(bound.ok());
  FCO_CHECK_EQ(bound.value(), std::uint64_t{100});

  const std::string maximum_text = std::to_string(std::numeric_limits<std::uint64_t>::max());
  const auto maximum = fco::parse_bounded_decimal(maximum_text,
                                                  std::numeric_limits<std::uint64_t>::max());
  FCO_REQUIRE(maximum.ok());
  FCO_CHECK_EQ(maximum.value(), std::numeric_limits<std::uint64_t>::max());
}

FCO_TEST(strong, parse_bounded_decimal_rejects_non_canonical_forms) {
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("", 10), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("00", 10), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("01", 10), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("+1", 10), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("-1", 10), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal(" 1", 10), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("1 ", 10), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("1a", 10), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("0x10", 10), fco::ErrorCode::MalformedInput);
}

FCO_TEST(strong, parse_bounded_decimal_rejects_values_above_the_bound) {
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("101", 100), fco::ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("18446744073709551616",
                                             std::numeric_limits<std::uint64_t>::max()),
                  fco::ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(
      fco::parse_bounded_decimal("99999999999999999999999999", std::numeric_limits<std::uint64_t>::max()),
      fco::ErrorCode::BoundedLimitExceeded);
}

// Regression test: a bound smaller than ten must still reject every value
// above it. The range check for "any value above the supplied bound" once
// underflowed in unsigned arithmetic and let a single digit above such a bound
// through instead of reporting BoundedLimitExceeded.
FCO_TEST(strong, parse_bounded_decimal_rejects_single_digit_above_a_small_bound) {
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("1", 0), fco::ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("6", 5), fco::ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("9", 5), fco::ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(fco::parse_bounded_decimal("9", 8), fco::ErrorCode::BoundedLimitExceeded);

  // The boundary itself remains accepted.
  const auto at_bound = fco::parse_bounded_decimal("5", 5);
  FCO_REQUIRE(at_bound.ok());
  FCO_CHECK_EQ(at_bound.value(), std::uint64_t{5});
  const auto zero = fco::parse_bounded_decimal("0", 0);
  FCO_REQUIRE(zero.ok());
  FCO_CHECK_EQ(zero.value(), std::uint64_t{0});
}

// ---------------------------------------------------------------------------
// Counters.
// ---------------------------------------------------------------------------
FCO_TEST(strong, counter_parse_rejects_zero) {
  FCO_CHECK_ERROR(FacilityEpoch::parse("0"), fco::ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(Revision::parse("0"), fco::ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(PlanRevision::parse("0"), fco::ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(AttemptOrdinal::parse("0"), fco::ErrorCode::InvalidIdentity);

  const auto parsed = FacilityEpoch::parse("7");
  FCO_REQUIRE(parsed.ok());
  FCO_CHECK_EQ(parsed.value().value(), std::uint64_t{7});
  FCO_CHECK(parsed.value().valid());
  FCO_CHECK_EQ(parsed.value().to_string(), std::string("7"));
  FCO_CHECK_EQ(parsed.value().describe(), std::string("facility-epoch '7'"));

  FCO_CHECK_ERROR(FacilityEpoch::parse("01"), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(FacilityEpoch::parse(""), fco::ErrorCode::MalformedInput);
}

FCO_TEST(strong, incremented_is_checked_and_tag_scoped) {
  const auto incremented = FacilityEpoch(1).incremented();
  FCO_REQUIRE(incremented.ok());
  FCO_CHECK_EQ(incremented.value().value(), std::uint64_t{2});

  const auto overflow =
      FacilityEpoch(std::numeric_limits<std::uint64_t>::max()).incremented();
  FCO_CHECK_ERROR(overflow, fco::ErrorCode::BoundedLimitExceeded);

  using NarrowCounter = fco::Strong<fco::ObservationSequenceTag, std::uint8_t>;
  const auto narrow_max = NarrowCounter(static_cast<std::uint8_t>(255)).incremented();
  FCO_CHECK_ERROR(narrow_max, fco::ErrorCode::BoundedLimitExceeded);
  const auto narrow_ok = NarrowCounter(static_cast<std::uint8_t>(254)).incremented();
  FCO_REQUIRE(narrow_ok.ok());
  FCO_CHECK_EQ(narrow_ok.value().value(), static_cast<std::uint8_t>(255));

  const auto identifier = AssetId("asset-1").incremented();
  FCO_CHECK_ERROR(identifier, fco::ErrorCode::UnsupportedOperation);
}

FCO_TEST(strong, default_constructed_values_are_unset) {
  const AssetId unset_asset;
  const RackId unset_rack;
  const FacilityEpoch unset_epoch;
  const Revision unset_revision;

  FCO_CHECK(!unset_asset.valid());
  FCO_CHECK(!unset_rack.valid());
  FCO_CHECK(!unset_epoch.valid());
  FCO_CHECK(!unset_revision.valid());
  FCO_CHECK_EQ(unset_asset.to_string(), std::string(""));
  FCO_CHECK_EQ(unset_epoch.to_string(), std::string("0"));

  FCO_CHECK(AssetId("asset-1").valid());
  FCO_CHECK(FacilityEpoch(1).valid());
  FCO_CHECK(!FacilityEpoch(0).valid());
  FCO_CHECK(!AssetId("").valid());
}

FCO_TEST(strong, strong_types_with_equal_text_are_still_distinct) {
  const AssetId asset("rack-1");
  const RackId rack("rack-1");

  // Same payload text, different types: identical spelling never makes them
  // interchangeable, and the tag travels with the value.
  FCO_CHECK_EQ(asset.to_string(), rack.to_string());
  FCO_CHECK_EQ(asset.to_string(), std::string("rack-1"));
  FCO_CHECK_EQ(asset.describe(), std::string("asset-id 'rack-1'"));
  FCO_CHECK_EQ(rack.describe(), std::string("rack-id 'rack-1'"));
  FCO_CHECK(asset == AssetId("rack-1"));
  FCO_CHECK(rack == RackId("rack-1"));
}

// ---------------------------------------------------------------------------
// Derived identifiers.
// ---------------------------------------------------------------------------
FCO_TEST(strong, derive_plan_id_is_deterministic_and_input_sensitive) {
  const PlanId base = fco::derive_plan_id("request-1", FacilityEpoch(7), Revision(2));
  FCO_CHECK_EQ(base, fco::derive_plan_id("request-1", FacilityEpoch(7), Revision(2)));
  FCO_CHECK(base.valid());
  FCO_CHECK(fco::is_valid_identifier(base.value()));
  FCO_CHECK_EQ(base.value().rfind("plan-", 0), std::size_t{0});

  FCO_CHECK(base != fco::derive_plan_id("request-2", FacilityEpoch(7), Revision(2)));
  FCO_CHECK(base != fco::derive_plan_id("request-1", FacilityEpoch(8), Revision(2)));
  FCO_CHECK(base != fco::derive_plan_id("request-1", FacilityEpoch(7), Revision(3)));
}

FCO_TEST(strong, derive_attempt_id_is_deterministic_and_input_sensitive) {
  const PlanId plan = fco::derive_plan_id("request-1", FacilityEpoch(7), Revision(2));
  const PlanId other_plan = fco::derive_plan_id("request-1", FacilityEpoch(8), Revision(2));
  const StepId step("drain");
  const StepId other_step("isolate");

  const AttemptId base = fco::derive_attempt_id(plan, PlanRevision(1), step, AttemptOrdinal(1));
  FCO_CHECK_EQ(base, fco::derive_attempt_id(plan, PlanRevision(1), step, AttemptOrdinal(1)));
  FCO_CHECK(base.valid());
  FCO_CHECK(fco::is_valid_identifier(base.value()));
  FCO_CHECK_EQ(base.value().rfind("att-", 0), std::size_t{0});

  FCO_CHECK(base != fco::derive_attempt_id(other_plan, PlanRevision(1), step, AttemptOrdinal(1)));
  FCO_CHECK(base != fco::derive_attempt_id(plan, PlanRevision(2), step, AttemptOrdinal(1)));
  FCO_CHECK(base != fco::derive_attempt_id(plan, PlanRevision(1), other_step, AttemptOrdinal(1)));
  FCO_CHECK(base != fco::derive_attempt_id(plan, PlanRevision(1), step, AttemptOrdinal(2)));
}

FCO_TEST(strong, derive_evidence_id_is_deterministic_and_input_sensitive) {
  const PlanId plan = fco::derive_plan_id("request-1", FacilityEpoch(7), Revision(2));
  const PlanId other_plan = fco::derive_plan_id("request-1", FacilityEpoch(8), Revision(2));
  const StepId step("drain");
  const StepId other_step("isolate");
  const AttemptId attempt = fco::derive_attempt_id(plan, PlanRevision(1), step, AttemptOrdinal(1));
  const AttemptId other_attempt =
      fco::derive_attempt_id(plan, PlanRevision(1), other_step, AttemptOrdinal(1));

  const EvidenceId base =
      fco::derive_evidence_id(plan, step, attempt, fco::ObservationSequence(3));
  FCO_CHECK_EQ(base, fco::derive_evidence_id(plan, step, attempt, fco::ObservationSequence(3)));
  FCO_CHECK(base.valid());
  FCO_CHECK(fco::is_valid_identifier(base.value()));
  FCO_CHECK_EQ(base.value().rfind("ev-", 0), std::size_t{0});

  FCO_CHECK(base != fco::derive_evidence_id(other_plan, step, attempt, fco::ObservationSequence(3)));
  FCO_CHECK(base != fco::derive_evidence_id(plan, other_step, attempt, fco::ObservationSequence(3)));
  FCO_CHECK(
      base != fco::derive_evidence_id(plan, step, other_attempt, fco::ObservationSequence(3)));
  FCO_CHECK(base != fco::derive_evidence_id(plan, step, attempt, fco::ObservationSequence(4)));
}

// ---------------------------------------------------------------------------
// Hashing.
// ---------------------------------------------------------------------------
FCO_TEST(strong, hash_supports_associative_containers) {
  std::unordered_map<AssetId, int> counts;
  counts[AssetId("asset-1")] = 1;
  counts[AssetId("asset-2")] = 2;
  counts[AssetId("asset-1")] = 3;

  FCO_CHECK_EQ(counts.size(), std::size_t{2});
  FCO_CHECK_EQ(counts.at(AssetId("asset-1")), 3);
  FCO_CHECK_EQ(counts.at(AssetId("asset-2")), 2);
  FCO_CHECK_EQ(counts.count(AssetId("asset-3")), std::size_t{0});

  FCO_CHECK_EQ(std::hash<AssetId>{}(AssetId("asset-1")), std::hash<AssetId>{}(AssetId("asset-1")));
  FCO_CHECK_EQ(std::hash<AssetId>{}(AssetId("asset-1")),
               std::hash<std::string>{}(std::string("asset-1")));
  FCO_CHECK_EQ(std::hash<FacilityEpoch>{}(FacilityEpoch(7)),
               std::hash<std::uint64_t>{}(std::uint64_t{7}));
}

FCO_TEST_MAIN
