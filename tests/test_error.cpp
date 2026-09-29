// Tests for fco/error.hpp and fco/error.cpp: the ErrorCode precedence
// contract, to_string coverage, deterministic primary-error selection, error
// ordering, retryability/storage classification, and the Result/Status types.

#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fco/error.hpp"

// ---------------------------------------------------------------------------
// Compile-time statement of the documented precedence contract: lower numeric
// value runs earlier and wins.
// ---------------------------------------------------------------------------
static_assert(fco::precedence(fco::ErrorCode::Ok) == 0);
static_assert(fco::precedence(fco::ErrorCode::MalformedInput) <
              fco::precedence(fco::ErrorCode::UnknownIdentity));
static_assert(fco::precedence(fco::ErrorCode::UnknownIdentity) <
              fco::precedence(fco::ErrorCode::StaleAuthority));
static_assert(fco::precedence(fco::ErrorCode::StaleAuthority) <
              fco::precedence(fco::ErrorCode::StaleGeneration));
static_assert(fco::precedence(fco::ErrorCode::StaleGeneration) <
              fco::precedence(fco::ErrorCode::UnknownPlan));
static_assert(fco::precedence(fco::ErrorCode::UnknownPlan) <
              fco::precedence(fco::ErrorCode::UnknownAttempt));
static_assert(fco::precedence(fco::ErrorCode::UnknownAttempt) <
              fco::precedence(fco::ErrorCode::StorageFailure));
static_assert(fco::precedence(fco::ErrorCode::StorageFailure) <
              fco::precedence(fco::ErrorCode::DomainRejected));
static_assert(fco::precedence(fco::ErrorCode::DomainRejected) <
              fco::precedence(fco::ErrorCode::UsageError));
static_assert(fco::precedence(fco::ErrorCode::UsageError) <
              fco::precedence(fco::ErrorCode::InternalError));
static_assert(fco::precedence(fco::ErrorCode::MissingAuthority) <
              fco::precedence(fco::ErrorCode::AuthorityMismatch));
static_assert(fco::precedence(fco::ErrorCode::AuthorityMismatch) <
              fco::precedence(fco::ErrorCode::StaleAuthority));
static_assert(fco::precedence(fco::ErrorCode::StaleAuthority) <
              fco::precedence(fco::ErrorCode::FencedAuthority));
static_assert(fco::precedence(fco::ErrorCode::FencedAuthority) <
              fco::precedence(fco::ErrorCode::PolicyViolation));
static_assert(fco::precedence(fco::ErrorCode::PlanCycle) <
              fco::precedence(fco::ErrorCode::UnknownStep));
static_assert(fco::precedence(fco::ErrorCode::UnknownStep) <
              fco::precedence(fco::ErrorCode::MissingPredecessor));
static_assert(fco::precedence(fco::ErrorCode::MissingPredecessor) <
              fco::precedence(fco::ErrorCode::PreconditionUnsatisfied));
static_assert(fco::precedence(fco::ErrorCode::ImpossibleCombination) <
              fco::precedence(fco::ErrorCode::UnsupportedOperation));
static_assert(fco::precedence(fco::ErrorCode::StaleGeneration) <
              fco::precedence(fco::ErrorCode::GenerationRegression));
static_assert(fco::precedence(fco::ErrorCode::EvidenceDigestMismatch) <
              fco::precedence(fco::ErrorCode::UnboundEvidence));

namespace {

using fco::Error;
using fco::ErrorCode;

constexpr ErrorCode kCodes[] = {
    ErrorCode::Ok,
    ErrorCode::MalformedInput,
    ErrorCode::UnsupportedFormatVersion,
    ErrorCode::TrailingBytes,
    ErrorCode::IntegrityFailure,
    ErrorCode::ReservedFieldNonZero,
    ErrorCode::BoundedLimitExceeded,
    ErrorCode::InvalidEnumValue,
    ErrorCode::ImpossibleCombination,
    ErrorCode::UnsupportedOperation,
    ErrorCode::UnknownIdentity,
    ErrorCode::DuplicateIdentity,
    ErrorCode::InvalidIdentity,
    ErrorCode::MissingAuthority,
    ErrorCode::AuthorityMismatch,
    ErrorCode::StaleAuthority,
    ErrorCode::FencedAuthority,
    ErrorCode::PolicyViolation,
    ErrorCode::StaleGeneration,
    ErrorCode::GenerationRegression,
    ErrorCode::EvidenceDigestMismatch,
    ErrorCode::ReorderedEvidence,
    ErrorCode::MissingEvidence,
    ErrorCode::UnboundEvidence,
    ErrorCode::UnknownPlan,
    ErrorCode::PlanRevisionMismatch,
    ErrorCode::InvalidStateTransition,
    ErrorCode::PlanCycle,
    ErrorCode::UnknownStep,
    ErrorCode::MissingPredecessor,
    ErrorCode::PreconditionUnsatisfied,
    ErrorCode::SafetyClassViolation,
    ErrorCode::PointOfNoReturn,
    ErrorCode::NonReversibleStep,
    ErrorCode::VerificationFailed,
    ErrorCode::PlanNotEvaluated,
    ErrorCode::PlanNotAuthorized,
    ErrorCode::PlanTerminal,
    ErrorCode::ReplanRequired,
    ErrorCode::EmptyPlan,
    ErrorCode::UnknownAttempt,
    ErrorCode::AttemptUnresolved,
    ErrorCode::AttemptAlreadyResolved,
    ErrorCode::IdempotencyConflict,
    ErrorCode::NoExecutableStep,
    ErrorCode::StepNotIssueable,
    ErrorCode::StorageFailure,
    ErrorCode::LockUnavailable,
    ErrorCode::RecordNotFound,
    ErrorCode::CommitFailure,
    ErrorCode::AmbiguousRecovery,
    ErrorCode::PathRejected,
    ErrorCode::DomainRejected,
    ErrorCode::DomainUnavailable,
    ErrorCode::DomainIndeterminate,
    ErrorCode::UsageError,
    ErrorCode::UnknownCommand,
    ErrorCode::UnsupportedDirection,
    ErrorCode::InternalError,
};

constexpr std::string_view kNames[] = {
    "ok",
    "malformed-input",
    "unsupported-format-version",
    "trailing-bytes",
    "integrity-failure",
    "reserved-field-non-zero",
    "bounded-limit-exceeded",
    "invalid-enum-value",
    "impossible-combination",
    "unsupported-operation",
    "unknown-identity",
    "duplicate-identity",
    "invalid-identity",
    "missing-authority",
    "authority-mismatch",
    "stale-authority",
    "fenced-authority",
    "policy-violation",
    "stale-generation",
    "generation-regression",
    "evidence-digest-mismatch",
    "reordered-evidence",
    "missing-evidence",
    "unbound-evidence",
    "unknown-plan",
    "plan-revision-mismatch",
    "invalid-state-transition",
    "plan-cycle",
    "unknown-step",
    "missing-predecessor",
    "precondition-unsatisfied",
    "safety-class-violation",
    "point-of-no-return",
    "non-reversible-step",
    "verification-failed",
    "plan-not-evaluated",
    "plan-not-authorized",
    "plan-terminal",
    "replan-required",
    "empty-plan",
    "unknown-attempt",
    "attempt-unresolved",
    "attempt-already-resolved",
    "idempotency-conflict",
    "no-executable-step",
    "step-not-issueable",
    "storage-failure",
    "lock-unavailable",
    "record-not-found",
    "commit-failure",
    "ambiguous-recovery",
    "path-rejected",
    "domain-rejected",
    "domain-unavailable",
    "domain-indeterminate",
    "usage-error",
    "unknown-command",
    "unsupported-direction",
    "internal-error",
};

static_assert(std::size(kCodes) == std::size(kNames));

[[nodiscard]] Error make(ErrorCode code, std::string subject, std::string message,
                         std::uint32_t ordinal = 0) {
  return Error(code, std::move(subject), std::move(message), ordinal);
}

}  // namespace

// ---------------------------------------------------------------------------
// to_string coverage.
// ---------------------------------------------------------------------------
FCO_TEST(error, to_string_names_every_declared_code) {
  std::array<bool, 1001> declared{};
  for (std::size_t i = 0; i < std::size(kCodes); ++i) {
    const std::uint16_t raw = static_cast<std::uint16_t>(kCodes[i]);
    FCO_CHECK_EQ(fco::to_string(kCodes[i]), kNames[i]);
    FCO_CHECK(fco::to_string(kCodes[i]) != std::string_view("unnamed-error-code"));
    FCO_REQUIRE(raw < declared.size());
    FCO_CHECK(!declared[raw]);
    declared[raw] = true;
  }

  // Every value that the header does not declare must be reported as unnamed
  // rather than silently mapped onto a neighbouring code.
  for (std::uint32_t raw = 0; raw <= 1000; ++raw) {
    const std::string_view name = fco::to_string(static_cast<ErrorCode>(raw));
    if (declared[raw]) {
      FCO_CHECK(name != std::string_view("unnamed-error-code"));
    } else {
      FCO_CHECK_EQ(name, std::string_view("unnamed-error-code"));
    }
  }
}

FCO_TEST(error, precedence_is_the_numeric_value_of_the_code) {
  for (std::size_t i = 0; i < std::size(kCodes); ++i) {
    FCO_CHECK_EQ(fco::precedence(kCodes[i]), static_cast<std::uint16_t>(kCodes[i]));
  }
  FCO_CHECK_EQ(fco::precedence(ErrorCode::Ok), std::uint16_t{0});
  FCO_CHECK(fco::precedence(ErrorCode::AuthorityMismatch) <
            fco::precedence(ErrorCode::FencedAuthority));
  FCO_CHECK(fco::precedence(ErrorCode::UnknownStep) <
            fco::precedence(ErrorCode::MissingPredecessor));
  FCO_CHECK(fco::precedence(ErrorCode::InternalError) == std::uint16_t{1000});
}

// ---------------------------------------------------------------------------
// Error ordering and description.
// ---------------------------------------------------------------------------
FCO_TEST(error, error_precedes_follows_the_documented_tuple) {
  const Error malformed = make(ErrorCode::MalformedInput, "field", "text");
  const Error unknown = make(ErrorCode::UnknownIdentity, "asset", "text");
  FCO_CHECK(fco::error_precedes(malformed, unknown));
  FCO_CHECK(!fco::error_precedes(unknown, malformed));
  FCO_CHECK(!fco::error_precedes(malformed, malformed));

  const Error subject_a = make(ErrorCode::InvalidIdentity, "a", "text");
  const Error subject_b = make(ErrorCode::InvalidIdentity, "b", "text");
  FCO_CHECK(fco::error_precedes(subject_a, subject_b));
  FCO_CHECK(!fco::error_precedes(subject_b, subject_a));

  const Error ordinal_1 = make(ErrorCode::InvalidIdentity, "a", "text", 1);
  const Error ordinal_2 = make(ErrorCode::InvalidIdentity, "a", "text", 2);
  FCO_CHECK(fco::error_precedes(ordinal_1, ordinal_2));

  const Error message_a = make(ErrorCode::InvalidIdentity, "a", "alpha", 1);
  const Error message_z = make(ErrorCode::InvalidIdentity, "a", "zulu", 1);
  FCO_CHECK(fco::error_precedes(message_a, message_z));

  // Precedence outranks every other component.
  const Error later_code_early_subject = make(ErrorCode::UnknownPlan, "aaa", "aaa", 0);
  const Error earlier_code_late_subject = make(ErrorCode::MalformedInput, "zzz", "zzz", 9);
  FCO_CHECK(fco::error_precedes(earlier_code_late_subject, later_code_early_subject));
}

FCO_TEST(error, error_equal_compares_every_component) {
  const Error base = make(ErrorCode::DuplicateIdentity, "asset-1", "duplicate", 3);
  FCO_CHECK(fco::error_equal(base, base));
  FCO_CHECK(fco::error_equal(base, make(ErrorCode::DuplicateIdentity, "asset-1", "duplicate", 3)));
  FCO_CHECK(!fco::error_equal(base, make(ErrorCode::DuplicateIdentity, "asset-1", "duplicate", 4)));
  FCO_CHECK(!fco::error_equal(base, make(ErrorCode::DuplicateIdentity, "asset-2", "duplicate", 3)));
  FCO_CHECK(!fco::error_equal(base, make(ErrorCode::DuplicateIdentity, "asset-1", "other", 3)));
  FCO_CHECK(!fco::error_equal(base, make(ErrorCode::UnknownIdentity, "asset-1", "duplicate", 3)));
}

FCO_TEST(error, describe_renders_code_subject_and_message) {
  FCO_CHECK_EQ(fco::describe(make(ErrorCode::MalformedInput, "field", "bad text")),
               std::string("malformed-input [field]: bad text"));
  FCO_CHECK_EQ(fco::describe(make(ErrorCode::MalformedInput, "", "bad text")),
               std::string("malformed-input: bad text"));
  FCO_CHECK_EQ(fco::describe(make(ErrorCode::MalformedInput, "field", "")),
               std::string("malformed-input [field]"));
  FCO_CHECK_EQ(fco::describe(Error()), std::string("ok"));
}

// ---------------------------------------------------------------------------
// Diagnostics.
// ---------------------------------------------------------------------------
FCO_TEST(error, diagnostics_primary_ignores_insertion_order) {
  const ErrorCode codes[] = {ErrorCode::StaleAuthority, ErrorCode::UnknownPlan,
                             ErrorCode::MalformedInput};

  for (int rotation = 0; rotation < 3; ++rotation) {
    fco::Diagnostics diagnostics;
    for (int i = 0; i < 3; ++i) {
      const ErrorCode code = codes[(rotation + i) % 3];
      diagnostics.add(code, "subject", std::string("message for ") + std::string(fco::to_string(code)));
    }
    FCO_REQUIRE(diagnostics.size() == 3);
    FCO_CHECK_EQ(diagnostics.primary().code, ErrorCode::MalformedInput);
    FCO_CHECK(fco::error_equal(diagnostics.primary(),
                               make(ErrorCode::MalformedInput, "subject",
                                    "message for malformed-input")));
  }

  // The lowest numeric code wins even when it is added last.
  fco::Diagnostics ascending;
  ascending.add(ErrorCode::UsageError, "cli", "usage");
  ascending.add(ErrorCode::InternalError, "kernel", "invariant");
  ascending.add(ErrorCode::MalformedInput, "record", "shape");
  FCO_CHECK_EQ(ascending.primary().code, ErrorCode::MalformedInput);

  fco::Diagnostics descending;
  descending.add(ErrorCode::MalformedInput, "record", "shape");
  descending.add(ErrorCode::InternalError, "kernel", "invariant");
  descending.add(ErrorCode::UsageError, "cli", "usage");
  FCO_CHECK_EQ(descending.primary().code, ErrorCode::MalformedInput);
  FCO_CHECK(fco::error_equal(ascending.primary(), descending.primary()));
}

FCO_TEST(error, diagnostics_tie_break_is_subject_then_ordinal_then_message) {
  fco::Diagnostics by_subject;
  by_subject.add(ErrorCode::DuplicateIdentity, "b", "first added");
  by_subject.add(ErrorCode::DuplicateIdentity, "a", "second added");
  FCO_CHECK_EQ(by_subject.primary().subject, std::string("a"));

  fco::Diagnostics by_subject_then_ordinal;
  by_subject_then_ordinal.add(Error(ErrorCode::InvalidIdentity, "a", "text", 9));
  by_subject_then_ordinal.add(Error(ErrorCode::InvalidIdentity, "b", "text", 1));
  FCO_CHECK_EQ(by_subject_then_ordinal.primary().subject, std::string("a"));
  FCO_CHECK_EQ(by_subject_then_ordinal.primary().ordinal, std::uint32_t{9});

  fco::Diagnostics by_ordinal;
  by_ordinal.add(Error(ErrorCode::InvalidIdentity, "a", "later", 7));
  by_ordinal.add(Error(ErrorCode::InvalidIdentity, "a", "earlier", 2));
  FCO_CHECK_EQ(by_ordinal.primary().ordinal, std::uint32_t{2});

  fco::Diagnostics by_message;
  by_message.add(Error(ErrorCode::InvalidIdentity, "a", "zulu", 3));
  by_message.add(Error(ErrorCode::InvalidIdentity, "a", "alpha", 3));
  FCO_CHECK_EQ(by_message.primary().message, std::string("alpha"));
}

FCO_TEST(error, diagnostics_ordered_is_sorted_and_stable) {
  fco::Diagnostics diagnostics;
  diagnostics.add(ErrorCode::UnknownPlan, "plan", "c");
  diagnostics.add(ErrorCode::MalformedInput, "record", "a");
  diagnostics.add(ErrorCode::StaleAuthority, "authority", "b");
  diagnostics.add(ErrorCode::MalformedInput, "asset", "a2");

  const std::vector<Error> ordered = diagnostics.ordered();
  FCO_REQUIRE(ordered.size() == 4);
  for (std::size_t i = 1; i < ordered.size(); ++i) {
    FCO_CHECK(!fco::error_precedes(ordered[i], ordered[i - 1]));
  }
  FCO_CHECK_EQ(ordered.front().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(ordered.front().subject, std::string("asset"));
  FCO_CHECK(fco::error_equal(ordered.front(), diagnostics.primary()));

  // ordered() must not mutate the collected candidates.
  FCO_CHECK_EQ(diagnostics.all().size(), std::size_t{4});
  FCO_CHECK_EQ(diagnostics.all().front().code, ErrorCode::UnknownPlan);
}

FCO_TEST(error, diagnostics_empty_behaviour) {
  fco::Diagnostics diagnostics;
  FCO_CHECK(diagnostics.empty());
  FCO_CHECK_EQ(diagnostics.size(), std::size_t{0});
  FCO_CHECK_EQ(diagnostics.primary().code, ErrorCode::Ok);
  FCO_CHECK_EQ(diagnostics.primary().subject, std::string(""));
  FCO_CHECK(!diagnostics.try_primary().has_value());
  FCO_CHECK(diagnostics.ordered().empty());

  // Ok is not a failure condition and is never collected.
  diagnostics.add(ErrorCode::Ok, "subject", "no failure");
  FCO_CHECK(diagnostics.empty());
  FCO_CHECK_EQ(diagnostics.size(), std::size_t{0});

  diagnostics.add(ErrorCode::UnknownStep, "s-1", "no such step");
  FCO_CHECK(!diagnostics.empty());
  FCO_CHECK_EQ(diagnostics.size(), std::size_t{1});
  FCO_REQUIRE(diagnostics.try_primary().has_value());
  FCO_CHECK_EQ(diagnostics.try_primary()->code, ErrorCode::UnknownStep);

  diagnostics.clear();
  FCO_CHECK(diagnostics.empty());
  FCO_CHECK_EQ(diagnostics.primary().code, ErrorCode::Ok);
}

// ---------------------------------------------------------------------------
// Classification helpers.
// ---------------------------------------------------------------------------
FCO_TEST(error, is_retryable_covers_representative_codes) {
  const ErrorCode retryable[] = {
      ErrorCode::PreconditionUnsatisfied, ErrorCode::MissingEvidence,
      ErrorCode::DomainUnavailable,       ErrorCode::DomainIndeterminate,
      ErrorCode::LockUnavailable,         ErrorCode::AttemptUnresolved,
      ErrorCode::ReplanRequired,
  };
  for (ErrorCode code : retryable) {
    FCO_CHECK(fco::is_retryable(code));
  }

  const ErrorCode terminal[] = {
      ErrorCode::Ok,            ErrorCode::MalformedInput,
      ErrorCode::InvalidIdentity, ErrorCode::StaleAuthority,
      ErrorCode::DuplicateIdentity, ErrorCode::VerificationFailed,
      ErrorCode::StorageFailure, ErrorCode::InternalError,
  };
  for (ErrorCode code : terminal) {
    FCO_CHECK(!fco::is_retryable(code));
  }
}

FCO_TEST(error, is_storage_code_covers_the_record_decode_range) {
  // The implemented contract classifies the record-frame decode and integrity
  // codes (100..108) as storage rejections: those are the failures produced
  // when durable state cannot be read back as written.
  for (std::uint16_t raw = 100; raw <= 108; ++raw) {
    FCO_CHECK(fco::is_storage_code(static_cast<ErrorCode>(raw)));
  }
  const ErrorCode not_storage[] = {
      ErrorCode::Ok,          ErrorCode::InvalidIdentity,
      ErrorCode::UnknownPlan, ErrorCode::DomainRejected,
      ErrorCode::UsageError,  ErrorCode::InternalError,
  };
  for (ErrorCode code : not_storage) {
    FCO_CHECK(!fco::is_storage_code(code));
  }
}

// ---------------------------------------------------------------------------
// Result<T> and Status.
// ---------------------------------------------------------------------------
namespace {

fco::Result<int> parse_positive(int value) {
  if (value <= 0) {
    return fco::make_error(fco::ErrorCode::MalformedInput, "value", "value must be positive", 1);
  }
  return value;
}

fco::Result<int> parse_doubled(int value) {
  FCO_RETURN_IF_ERROR(parse_positive(value));
  return value * 2;
}

}  // namespace

FCO_TEST(error, result_carries_value_or_error) {
  auto ok_result = fco::success(std::string("payload"));
  FCO_CHECK(ok_result.ok());
  FCO_CHECK(static_cast<bool>(ok_result));
  FCO_CHECK_EQ(ok_result.value(), std::string("payload"));

  fco::Result<int> implicit_value = 7;
  FCO_CHECK(implicit_value.ok());
  FCO_CHECK_EQ(implicit_value.value(), 7);
  implicit_value.value() = 8;
  FCO_CHECK_EQ(implicit_value.value(), 8);

  auto failure = fco::failure<int>(ErrorCode::UnknownStep, "s-1", "no such step", 4);
  FCO_CHECK(!failure.ok());
  FCO_CHECK(!static_cast<bool>(failure));
  FCO_CHECK_EQ(failure.error().code, ErrorCode::UnknownStep);
  FCO_CHECK_EQ(failure.error().subject, std::string("s-1"));
  FCO_CHECK_EQ(failure.error().message, std::string("no such step"));
  FCO_CHECK_EQ(failure.error().ordinal, std::uint32_t{4});

  auto taken = fco::success(std::string("moved"));
  FCO_CHECK_EQ(taken.take(), std::string("moved"));

  const auto copy = ok_result;
  FCO_CHECK_EQ(copy.value(), std::string("payload"));
}

FCO_TEST(error, status_is_a_result_without_a_value) {
  fco::Status ok_status = fco::success();
  FCO_CHECK(ok_status.ok());
  FCO_CHECK(static_cast<bool>(ok_status));

  fco::Status bad_status = fco::failure(ErrorCode::UsageError, "cli", "bad arguments");
  FCO_CHECK(!bad_status.ok());
  FCO_CHECK_EQ(bad_status.error().code, ErrorCode::UsageError);
  FCO_CHECK_EQ(bad_status.error().subject, std::string("cli"));

  fco::Status default_status;
  FCO_CHECK(default_status.ok());

  fco::Status from_error = fco::make_error(ErrorCode::PathRejected, "path", "outside the root");
  FCO_CHECK(!from_error.ok());
  FCO_CHECK_EQ(from_error.error().code, ErrorCode::PathRejected);
}

FCO_TEST(error, return_if_error_propagates_the_primary_condition) {
  const auto good = parse_doubled(3);
  FCO_REQUIRE(good.ok());
  FCO_CHECK_EQ(good.value(), 6);

  FCO_CHECK_ERROR(parse_doubled(0), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_doubled(-5), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_positive(0), ErrorCode::MalformedInput);
}

FCO_TEST_MAIN
