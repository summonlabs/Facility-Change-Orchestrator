#pragma once

// Domain authority ports.
//
// The orchestrator composes typed requests and typed observations. It does not
// perform power switching, cooling actuation, workload scheduling, fabric
// routing, firmware flashing, commissioning or decommissioning internals, and it
// does not decide maintenance work. Each of those is owned by the runtime behind
// one of these ports.
//
// A port is called outside every internal lock and never re-enters the
// orchestrator: submissions are pure request/response, and the orchestrator
// records what came back before doing anything else with it.

#include <cstdint>
#include <string>
#include <string_view>

#include "fco/authority.hpp"
#include "fco/digest.hpp"
#include "fco/domain.hpp"
#include "fco/error.hpp"
#include "fco/evidence.hpp"
#include "fco/facility.hpp"
#include "fco/strong.hpp"

namespace fco {

enum class SubmitOutcome : std::uint8_t {
  Unspecified = 0,
  Accepted = 1,
  Rejected = 2,
  Indeterminate = 3,
  DuplicateOfAppliedRequest = 4,
};
inline constexpr SubmitOutcome kSubmitOutcomeMax = SubmitOutcome::DuplicateOfAppliedRequest;

[[nodiscard]] constexpr bool is_valid(SubmitOutcome value) noexcept {
  return static_cast<std::uint8_t>(value) >= 1u &&
         static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(kSubmitOutcomeMax);
}
[[nodiscard]] std::string_view to_string(SubmitOutcome value) noexcept;
[[nodiscard]] Result<SubmitOutcome> parse_submit_outcome(std::string_view text);

struct ActionOutcome {
  SubmitOutcome outcome = SubmitOutcome::Unspecified;
  ObservationSequence observation_sequence;
  GenerationSet observed_generations;
  HardwareGeneration observed_hardware_generation;
  FirmwareGeneration observed_firmware_generation;
  EvidenceOutcome evidence = EvidenceOutcome::Unspecified;
  Digest content_digest;
  std::uint64_t observed_at_micros = 0;
  // The asset record the authority reports it now observes. The orchestrator
  // does not take this on trust: it verifies the report against the step's
  // expected effect before accepting it as its own observation, and an
  // acknowledgement without a report never advances the facility view.
  bool has_observed_asset = false;
  AssetRecord observed_asset;
  std::string detail;

  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] std::string describe() const;
};

class DomainAuthority {
 public:
  DomainAuthority() = default;
  virtual ~DomainAuthority() = default;
  DomainAuthority(const DomainAuthority&) = delete;
  DomainAuthority& operator=(const DomainAuthority&) = delete;
  DomainAuthority(DomainAuthority&&) = delete;
  DomainAuthority& operator=(DomainAuthority&&) = delete;

  [[nodiscard]] virtual DomainKind domain() const noexcept = 0;
  [[nodiscard]] virtual IncarnationId incarnation() const noexcept = 0;
  [[nodiscard]] virtual ControlEpoch control_epoch() const noexcept = 0;

  // Applies the request at most once per idempotency key. A repeat of a key that
  // has already been applied must report DuplicateOfAppliedRequest rather than
  // applying the effect twice.
  [[nodiscard]] virtual Result<ActionOutcome> submit(const ActionRequest& request,
                                                     const AttemptId& attempt,
                                                     const Digest& idempotency_key,
                                                     std::uint64_t now_micros) = 0;
};

class Clock {
 public:
  Clock() = default;
  virtual ~Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  Clock(Clock&&) = delete;
  Clock& operator=(Clock&&) = delete;

  [[nodiscard]] virtual std::uint64_t now_micros() const noexcept = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] std::uint64_t now_micros() const noexcept override;
};

// Deterministic clock for tests and replay. Time never advances on its own.
class ManualClock final : public Clock {
 public:
  ManualClock() = default;
  explicit ManualClock(std::uint64_t start_micros) : now_(start_micros) {}

  [[nodiscard]] std::uint64_t now_micros() const noexcept override { return now_; }
  void advance(std::uint64_t micros) noexcept { now_ += micros; }
  void set(std::uint64_t micros) noexcept { now_ = micros; }

 private:
  std::uint64_t now_ = 1000000;
};

}  // namespace fco
