#pragma once

// Evidence ledger.
//
// Evidence is what a domain authority observed, not what it was asked to do. It
// is bound to the exact plan, plan revision, step and attempt it belongs to, it
// carries the generations the authority had in view when it observed, and it is
// sequenced per source so that reordered or replayed observations are rejected
// instead of quietly winning or losing a race.
//
// Re-ingesting an identical record is an idempotent replay and succeeds without
// changing state. Re-ingesting the same identifier with different content is a
// conflict, never a silent overwrite.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "fco/authority.hpp"
#include "fco/digest.hpp"
#include "fco/domain.hpp"
#include "fco/error.hpp"
#include "fco/facility.hpp"
#include "fco/model.hpp"
#include "fco/strong.hpp"

namespace fco {

enum class EvidenceOutcome : std::uint8_t {
  Unspecified = 0,
  EffectObserved = 1,
  EffectAbsent = 2,
  Indeterminate = 3,
  ActionRejected = 4,
};
inline constexpr EvidenceOutcome kEvidenceOutcomeMax = EvidenceOutcome::ActionRejected;

[[nodiscard]] constexpr bool is_valid(EvidenceOutcome value) noexcept {
  return static_cast<std::uint8_t>(value) >= 1u &&
         static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(kEvidenceOutcomeMax);
}
[[nodiscard]] std::string_view to_string(EvidenceOutcome value) noexcept;
[[nodiscard]] Result<EvidenceOutcome> parse_evidence_outcome(std::string_view text);

struct EvidenceRecord {
  EvidenceId id;
  PlanId plan;
  PlanRevision plan_revision;
  StepId step;
  AttemptId attempt;
  DomainKind source_domain = DomainKind::Unspecified;
  IncarnationId source_incarnation;
  ControlEpoch source_control_epoch;
  ObservationSequence observation_sequence;
  GenerationSet observed_generations;
  Digest content_digest;
  EvidenceOutcome outcome = EvidenceOutcome::Unspecified;
  std::uint64_t observed_at_micros = 0;
  // The asset record the reporting authority observed. An observation that
  // carries no record can confirm nothing about facility state.
  bool has_observed_asset = false;
  AssetRecord observed_asset;
  std::string note;

  [[nodiscard]] bool complete() const noexcept;
  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;
  [[nodiscard]] std::string describe() const;
};

class EvidenceLedger {
 public:
  // Returns true when a new record was stored, false when the identical record
  // was already present (idempotent replay).
  [[nodiscard]] Result<bool> append(const EvidenceRecord& record);

  [[nodiscard]] const EvidenceRecord* find(const EvidenceId& id) const noexcept;
  [[nodiscard]] std::vector<const EvidenceRecord*> for_attempt(const AttemptId& attempt) const;
  [[nodiscard]] std::vector<const EvidenceRecord*> for_step(const StepId& step) const;
  [[nodiscard]] ObservationSequence watermark(DomainKind domain, IncarnationId incarnation) const;
  [[nodiscard]] const std::map<EvidenceId, EvidenceRecord>& records() const noexcept {
    return records_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] Digest digest() const;
  void clear();

  // Inserts a record that has already been validated and integrity-checked by
  // the durable decoder. Ordering enforcement lives in append(); this entry
  // point exists because a decoded ledger is stored in identifier order rather
  // than arrival order.
  void restore(const EvidenceRecord& record);

  // Recomputes the per-source watermarks from the stored records. Called after
  // decoding durable state.
  void rebuild_watermarks();

 private:
  std::map<EvidenceId, EvidenceRecord> records_;
  std::map<std::string, ObservationSequence> watermarks_;
};

}  // namespace fco
