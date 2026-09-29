#include "fco/error.hpp"

#include <algorithm>
#include <sstream>

namespace fco {
namespace {

struct CodeName {
  ErrorCode code;
  std::string_view name;
};

constexpr CodeName kCodeNames[] = {
    {ErrorCode::Ok, "ok"},
    {ErrorCode::MalformedInput, "malformed-input"},
    {ErrorCode::UnsupportedFormatVersion, "unsupported-format-version"},
    {ErrorCode::TrailingBytes, "trailing-bytes"},
    {ErrorCode::IntegrityFailure, "integrity-failure"},
    {ErrorCode::ReservedFieldNonZero, "reserved-field-non-zero"},
    {ErrorCode::BoundedLimitExceeded, "bounded-limit-exceeded"},
    {ErrorCode::InvalidEnumValue, "invalid-enum-value"},
    {ErrorCode::ImpossibleCombination, "impossible-combination"},
    {ErrorCode::UnsupportedOperation, "unsupported-operation"},
    {ErrorCode::UnknownIdentity, "unknown-identity"},
    {ErrorCode::DuplicateIdentity, "duplicate-identity"},
    {ErrorCode::InvalidIdentity, "invalid-identity"},
    {ErrorCode::MissingAuthority, "missing-authority"},
    {ErrorCode::AuthorityMismatch, "authority-mismatch"},
    {ErrorCode::StaleAuthority, "stale-authority"},
    {ErrorCode::FencedAuthority, "fenced-authority"},
    {ErrorCode::PolicyViolation, "policy-violation"},
    {ErrorCode::StaleGeneration, "stale-generation"},
    {ErrorCode::GenerationRegression, "generation-regression"},
    {ErrorCode::EvidenceDigestMismatch, "evidence-digest-mismatch"},
    {ErrorCode::ReorderedEvidence, "reordered-evidence"},
    {ErrorCode::MissingEvidence, "missing-evidence"},
    {ErrorCode::UnboundEvidence, "unbound-evidence"},
    {ErrorCode::UnknownPlan, "unknown-plan"},
    {ErrorCode::PlanRevisionMismatch, "plan-revision-mismatch"},
    {ErrorCode::InvalidStateTransition, "invalid-state-transition"},
    {ErrorCode::PlanCycle, "plan-cycle"},
    {ErrorCode::UnknownStep, "unknown-step"},
    {ErrorCode::MissingPredecessor, "missing-predecessor"},
    {ErrorCode::PreconditionUnsatisfied, "precondition-unsatisfied"},
    {ErrorCode::SafetyClassViolation, "safety-class-violation"},
    {ErrorCode::PointOfNoReturn, "point-of-no-return"},
    {ErrorCode::NonReversibleStep, "non-reversible-step"},
    {ErrorCode::VerificationFailed, "verification-failed"},
    {ErrorCode::PlanNotEvaluated, "plan-not-evaluated"},
    {ErrorCode::PlanNotAuthorized, "plan-not-authorized"},
    {ErrorCode::PlanTerminal, "plan-terminal"},
    {ErrorCode::ReplanRequired, "replan-required"},
    {ErrorCode::EmptyPlan, "empty-plan"},
    {ErrorCode::UnknownAttempt, "unknown-attempt"},
    {ErrorCode::AttemptUnresolved, "attempt-unresolved"},
    {ErrorCode::AttemptAlreadyResolved, "attempt-already-resolved"},
    {ErrorCode::IdempotencyConflict, "idempotency-conflict"},
    {ErrorCode::NoExecutableStep, "no-executable-step"},
    {ErrorCode::StepNotIssueable, "step-not-issueable"},
    {ErrorCode::StorageFailure, "storage-failure"},
    {ErrorCode::LockUnavailable, "lock-unavailable"},
    {ErrorCode::RecordNotFound, "record-not-found"},
    {ErrorCode::CommitFailure, "commit-failure"},
    {ErrorCode::AmbiguousRecovery, "ambiguous-recovery"},
    {ErrorCode::PathRejected, "path-rejected"},
    {ErrorCode::DomainRejected, "domain-rejected"},
    {ErrorCode::DomainUnavailable, "domain-unavailable"},
    {ErrorCode::DomainIndeterminate, "domain-indeterminate"},
    {ErrorCode::UsageError, "usage-error"},
    {ErrorCode::UnknownCommand, "unknown-command"},
    {ErrorCode::UnsupportedDirection, "unsupported-direction"},
    {ErrorCode::InternalError, "internal-error"},
};

}  // namespace

std::string_view to_string(ErrorCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) return entry.name;
  }
  return "unnamed-error-code";
}

bool is_retryable(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::PreconditionUnsatisfied:
    case ErrorCode::MissingEvidence:
    case ErrorCode::DomainUnavailable:
    case ErrorCode::DomainIndeterminate:
    case ErrorCode::LockUnavailable:
    case ErrorCode::AttemptUnresolved:
    case ErrorCode::ReplanRequired:
      return true;
    default:
      return false;
  }
}

bool is_storage_code(ErrorCode code) noexcept {
  const std::uint16_t value = static_cast<std::uint16_t>(code);
  return value >= 100 && value <= 108;
}

bool error_precedes(const Error& a, const Error& b) noexcept {
  if (a.code != b.code) return precedence(a.code) < precedence(b.code);
  if (a.subject != b.subject) return a.subject < b.subject;
  if (a.ordinal != b.ordinal) return a.ordinal < b.ordinal;
  return a.message < b.message;
}

bool error_equal(const Error& a, const Error& b) noexcept {
  return a.code == b.code && a.subject == b.subject && a.ordinal == b.ordinal &&
         a.message == b.message;
}

std::string describe(const Error& e) {
  std::ostringstream out;
  out << to_string(e.code);
  if (!e.subject.empty()) out << " [" << e.subject << ']';
  if (!e.message.empty()) out << ": " << e.message;
  return out.str();
}

void Diagnostics::add(Error e) {
  if (e.code == ErrorCode::Ok) return;
  errors_.push_back(std::move(e));
}

void Diagnostics::add(ErrorCode code, std::string subject, std::string message,
                      std::uint32_t ordinal) {
  add(Error(code, std::move(subject), std::move(message), ordinal));
}

const Error& Diagnostics::primary() const {
  static const Error kNone{};
  const Error* best = nullptr;
  for (const Error& candidate : errors_) {
    if (best == nullptr || error_precedes(candidate, *best)) best = &candidate;
  }
  return best == nullptr ? kNone : *best;
}

std::optional<Error> Diagnostics::try_primary() const {
  if (errors_.empty()) return std::nullopt;
  return primary();
}

std::vector<Error> Diagnostics::ordered() const {
  std::vector<Error> copy = errors_;
  std::stable_sort(copy.begin(), copy.end(),
                   [](const Error& a, const Error& b) { return error_precedes(a, b); });
  return copy;
}

}  // namespace fco
