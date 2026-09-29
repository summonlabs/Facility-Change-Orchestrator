#include "fco/strong.hpp"

#include "fco/digest.hpp"

namespace fco {
namespace {

[[nodiscard]] bool is_identifier_char(char c) noexcept {
  const bool digit = c >= '0' && c <= '9';
  const bool upper = c >= 'A' && c <= 'Z';
  const bool lower = c >= 'a' && c <= 'z';
  return digit || upper || lower || c == '.' || c == '_' || c == ':' || c == '-';
}

[[nodiscard]] std::string hex_prefix(const Digest& digest, std::size_t characters) {
  const std::string hex = digest.to_hex();
  return hex.substr(0, characters);
}

}  // namespace

bool is_valid_identifier(std::string_view text) noexcept {
  if (text.empty() || text.size() > 64) return false;
  for (char c : text) {
    if (!is_identifier_char(c)) return false;
  }
  return true;
}

Result<std::string> normalize_identifier(std::string_view text) {
  if (text.empty()) {
    return failure<std::string>(ErrorCode::InvalidIdentity, std::string(text),
                                "identifier must not be empty");
  }
  if (text.size() > 64) {
    return failure<std::string>(ErrorCode::BoundedLimitExceeded, std::string(text),
                                "identifier must not exceed 64 characters");
  }
  for (char c : text) {
    if (!is_identifier_char(c)) {
      return failure<std::string>(
          ErrorCode::InvalidIdentity, std::string(text),
          std::string("identifier contains an unsupported character '") + c + "'");
    }
  }
  return success(std::string(text));
}

Result<std::uint64_t> parse_bounded_decimal(std::string_view text, std::uint64_t maximum) {
  if (text.empty()) {
    return failure<std::uint64_t>(ErrorCode::MalformedInput, std::string(text),
                                  "decimal value must not be empty");
  }
  if (text.size() > 1 && text.front() == '0') {
    return failure<std::uint64_t>(ErrorCode::MalformedInput, std::string(text),
                                  "decimal value must not carry leading zeros");
  }
  std::uint64_t accumulator = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return failure<std::uint64_t>(ErrorCode::MalformedInput, std::string(text),
                                    "decimal value must contain only digits 0-9");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    // Checked in two steps: the multiplication first, then the addition. Doing
    // it as a single "(maximum - digit) / 10" test underflows when digit is
    // greater than maximum, which would accept a value above the bound.
    if (accumulator > maximum / 10ull) {
      return failure<std::uint64_t>(ErrorCode::BoundedLimitExceeded, std::string(text),
                                    "decimal value exceeds the supported range");
    }
    accumulator *= 10ull;
    if (digit > maximum - accumulator) {
      return failure<std::uint64_t>(ErrorCode::BoundedLimitExceeded, std::string(text),
                                    "decimal value exceeds the supported range");
    }
    accumulator += digit;
  }
  return success(accumulator);
}

PlanId derive_plan_id(std::string_view request_id, FacilityEpoch epoch,
                      Revision request_revision) {
  Sha256 hasher;
  hasher.update("fco/plan-id/1");
  hasher.update_u8(static_cast<std::uint8_t>(0));
  hasher.update(request_id);
  hasher.update_u8(static_cast<std::uint8_t>(0));
  hasher.update_u64(epoch.value());
  hasher.update_u64(request_revision.value());
  return PlanId("plan-" + hex_prefix(hasher.finish(), 20));
}

AttemptId derive_attempt_id(const PlanId& plan, PlanRevision revision, const StepId& step,
                            AttemptOrdinal ordinal) {
  Sha256 hasher;
  hasher.update("fco/attempt-id/1");
  hasher.update_u8(static_cast<std::uint8_t>(0));
  hasher.update(plan.value());
  hasher.update_u8(static_cast<std::uint8_t>(0));
  hasher.update_u64(revision.value());
  hasher.update_u8(static_cast<std::uint8_t>(0));
  hasher.update(step.value());
  hasher.update_u8(static_cast<std::uint8_t>(0));
  hasher.update_u64(ordinal.value());
  return AttemptId("att-" + hex_prefix(hasher.finish(), 20));
}

EvidenceId derive_evidence_id(const PlanId& plan, const StepId& step, const AttemptId& attempt,
                              ObservationSequence sequence) {
  Sha256 hasher;
  hasher.update("fco/evidence-id/1");
  hasher.update_u8(static_cast<std::uint8_t>(0));
  hasher.update(plan.value());
  hasher.update_u8(static_cast<std::uint8_t>(0));
  hasher.update(step.value());
  hasher.update_u8(static_cast<std::uint8_t>(0));
  hasher.update(attempt.value());
  hasher.update_u8(static_cast<std::uint8_t>(0));
  hasher.update_u64(sequence.value());
  return EvidenceId("ev-" + hex_prefix(hasher.finish(), 20));
}

}  // namespace fco
