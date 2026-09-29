#pragma once

// Error model.
//
// Validation precedence is deterministic and is derived from the numeric value
// of ErrorCode: the lower the numeric value, the earlier the condition is
// evaluated and the higher its precedence when several conditions hold at the
// same time. The numbering is therefore part of the observable contract and is
// covered by a static assertion block at the bottom of this header.
//
// Within one ErrorCode the primary error is chosen by the deterministic tuple
// (subject, ordinal, message). The subject is a stable, canonical key such as a
// step id or a field path, never an address, iteration index, or hash of a
// pointer. This is what makes the same invalid request resolve to the same
// primary error regardless of incidental map ordering or thread scheduling.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace fco {

enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // --- 100..199: input shape, encoding, integrity -------------------------
  MalformedInput = 100,
  UnsupportedFormatVersion = 101,
  TrailingBytes = 102,
  IntegrityFailure = 103,
  ReservedFieldNonZero = 104,
  BoundedLimitExceeded = 105,
  InvalidEnumValue = 106,
  ImpossibleCombination = 107,
  UnsupportedOperation = 108,

  // --- 200..299: identity --------------------------------------------------
  UnknownIdentity = 200,
  DuplicateIdentity = 201,
  InvalidIdentity = 202,

  // --- 300..399: authority / epochs / incarnation --------------------------
  MissingAuthority = 300,
  AuthorityMismatch = 301,
  StaleAuthority = 302,
  FencedAuthority = 303,
  PolicyViolation = 304,

  // --- 400..499: generations and evidence currency -------------------------
  StaleGeneration = 400,
  GenerationRegression = 401,
  EvidenceDigestMismatch = 402,
  ReorderedEvidence = 403,
  MissingEvidence = 404,
  UnboundEvidence = 405,

  // --- 500..599: plan, state machine, preconditions ------------------------
  UnknownPlan = 500,
  PlanRevisionMismatch = 501,
  InvalidStateTransition = 502,
  PlanCycle = 503,
  UnknownStep = 504,
  MissingPredecessor = 505,
  PreconditionUnsatisfied = 506,
  SafetyClassViolation = 507,
  PointOfNoReturn = 508,
  NonReversibleStep = 509,
  VerificationFailed = 510,
  PlanNotEvaluated = 511,
  PlanNotAuthorized = 512,
  PlanTerminal = 513,
  ReplanRequired = 514,
  EmptyPlan = 515,

  // --- 600..699: attempts and idempotency ----------------------------------
  UnknownAttempt = 600,
  AttemptUnresolved = 601,
  AttemptAlreadyResolved = 602,
  IdempotencyConflict = 603,
  NoExecutableStep = 604,
  StepNotIssueable = 605,

  // --- 700..799: durable storage and locking -------------------------------
  StorageFailure = 700,
  LockUnavailable = 701,
  RecordNotFound = 702,
  CommitFailure = 703,
  AmbiguousRecovery = 704,
  PathRejected = 705,

  // --- 800..899: domain authority ports ------------------------------------
  DomainRejected = 800,
  DomainUnavailable = 801,
  DomainIndeterminate = 802,

  // --- 900..999: invocation surface ----------------------------------------
  UsageError = 900,
  UnknownCommand = 901,
  UnsupportedDirection = 902,

  // --- 1000+: internal invariant breaches ----------------------------------
  InternalError = 1000,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

// Documented evaluation precedence; lower runs earlier and wins.
[[nodiscard]] constexpr std::uint16_t precedence(ErrorCode code) noexcept {
  return static_cast<std::uint16_t>(code);
}

// True when the code denotes a condition that can be retried after the caller
// changes something the orchestrator does not own (facility state, authority).
[[nodiscard]] bool is_retryable(ErrorCode code) noexcept;

// True when the code denotes a condition that indicates durable state was
// rejected rather than a caller mistake.
[[nodiscard]] bool is_storage_code(ErrorCode code) noexcept;

struct Error {
  ErrorCode code = ErrorCode::Ok;
  std::string subject;  // canonical, deterministic ordering key
  std::uint32_t ordinal = 0;
  std::string message;

  Error() = default;
  Error(ErrorCode c, std::string subject_key, std::string text, std::uint32_t ord = 0)
      : code(c), subject(std::move(subject_key)), ordinal(ord), message(std::move(text)) {}
};

[[nodiscard]] bool error_precedes(const Error& a, const Error& b) noexcept;
[[nodiscard]] bool error_equal(const Error& a, const Error& b) noexcept;
[[nodiscard]] std::string describe(const Error& e);

// Deterministic primary-error selection over a set of candidate conditions.
class Diagnostics {
 public:
  Diagnostics() = default;

  void add(Error e);
  void add(ErrorCode code, std::string subject, std::string message, std::uint32_t ordinal = 0);

  [[nodiscard]] bool empty() const noexcept { return errors_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return errors_.size(); }

  [[nodiscard]] const Error& primary() const;
  [[nodiscard]] std::optional<Error> try_primary() const;

  // Candidates ordered by the documented precedence tuple.
  [[nodiscard]] const std::vector<Error>& all() const noexcept { return errors_; }
  [[nodiscard]] std::vector<Error> ordered() const;

  void clear() noexcept { errors_.clear(); }

 private:
  std::vector<Error> errors_;
};

template <class T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : storage_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  Result(const Result&) = default;
  Result(Result&&) noexcept = default;
  Result& operator=(const Result&) = default;
  Result& operator=(Result&&) noexcept = default;
  ~Result() = default;

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  // Precondition: ok(). Violating it is a programming error, not a fallback.
  [[nodiscard]] const T& value() const { return std::get<T>(storage_); }
  [[nodiscard]] T& value() { return std::get<T>(storage_); }
  [[nodiscard]] T take() { return std::get<T>(std::move(storage_)); }
  [[nodiscard]] const Error& error() const { return std::get<Error>(storage_); }

 private:
  std::variant<T, Error> storage_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Error& error() const { return *error_; }

 private:
  std::optional<Error> error_;
};

using Status = Result<void>;

template <class T>
[[nodiscard]] Result<std::decay_t<T>> success(T&& value) {
  return Result<std::decay_t<T>>(std::forward<T>(value));
}

[[nodiscard]] inline Status success() { return Status(); }

[[nodiscard]] inline Error make_error(ErrorCode code, std::string subject, std::string message,
                                      std::uint32_t ordinal = 0) {
  return Error(code, std::move(subject), std::move(message), ordinal);
}

template <class T = void>
[[nodiscard]] auto failure(ErrorCode code, std::string subject, std::string message,
                           std::uint32_t ordinal = 0) {
  if constexpr (std::is_void_v<T>) {
    return Status(make_error(code, std::move(subject), std::move(message), ordinal));
  } else {
    return Result<T>(make_error(code, std::move(subject), std::move(message), ordinal));
  }
}

// Propagate an error out of a function returning Result<T>.
#define FCO_RETURN_IF_ERROR(expr)                        \
  do {                                                   \
    const auto fco_status_ = (expr);                     \
    if (!fco_status_.ok()) return fco_status_.error();   \
  } while (false)

// ---------------------------------------------------------------------------
// Precedence contract. These assertions are the machine-checked statement that
// the numeric ordering of ErrorCode is the documented evaluation order.
// ---------------------------------------------------------------------------
static_assert(precedence(ErrorCode::MalformedInput) < precedence(ErrorCode::UnknownIdentity));
static_assert(precedence(ErrorCode::UnknownIdentity) < precedence(ErrorCode::StaleAuthority));
static_assert(precedence(ErrorCode::StaleAuthority) < precedence(ErrorCode::StaleGeneration));
static_assert(precedence(ErrorCode::StaleGeneration) < precedence(ErrorCode::UnknownPlan));
static_assert(precedence(ErrorCode::UnknownPlan) < precedence(ErrorCode::UnknownAttempt));
static_assert(precedence(ErrorCode::UnknownAttempt) < precedence(ErrorCode::StorageFailure));
static_assert(precedence(ErrorCode::StorageFailure) < precedence(ErrorCode::DomainRejected));
static_assert(precedence(ErrorCode::DomainRejected) < precedence(ErrorCode::UsageError));
static_assert(precedence(ErrorCode::UsageError) < precedence(ErrorCode::InternalError));
static_assert(precedence(ErrorCode::Ok) == 0);

}  // namespace fco
