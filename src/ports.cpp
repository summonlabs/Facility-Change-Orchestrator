#include "fco/ports.hpp"

#include <array>
#include <chrono>

namespace fco {
namespace {

constexpr std::array<std::pair<SubmitOutcome, std::string_view>, 5> kSubmitOutcomeNames = {{
    {SubmitOutcome::Unspecified, "unspecified"},
    {SubmitOutcome::Accepted, "accepted"},
    {SubmitOutcome::Rejected, "rejected"},
    {SubmitOutcome::Indeterminate, "indeterminate"},
    {SubmitOutcome::DuplicateOfAppliedRequest, "duplicate-of-applied-request"},
}};

}  // namespace

std::string_view to_string(SubmitOutcome value) noexcept {
  for (const auto& entry : kSubmitOutcomeNames) {
    if (entry.first == value) return entry.second;
  }
  return "invalid";
}

Result<SubmitOutcome> parse_submit_outcome(std::string_view text) {
  for (const auto& entry : kSubmitOutcomeNames) {
    if (entry.second == text) return success(entry.first);
  }
  return failure<SubmitOutcome>(ErrorCode::InvalidEnumValue, std::string(text),
                                "unrecognised submit outcome");
}

bool ActionOutcome::complete() const noexcept {
  return is_valid(outcome) && is_valid(evidence);
}

std::string ActionOutcome::describe() const {
  std::string out(to_string(outcome));
  out += " evidence=";
  out += to_string(evidence);
  if (observation_sequence.valid()) out += " seq=" + observation_sequence.to_string();
  if (has_observed_asset) out += " observed=" + observed_asset.id.to_string();
  if (!detail.empty()) out += " (" + detail + ")";
  return out;
}

std::uint64_t SystemClock::now_micros() const noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
  return micros > 0 ? static_cast<std::uint64_t>(micros) : 0ull;
}

}  // namespace fco
