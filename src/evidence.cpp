#include "fco/evidence.hpp"

#include <array>

namespace fco {
namespace {

constexpr std::array<std::pair<EvidenceOutcome, std::string_view>, 5> kEvidenceOutcomeNames = {{
    {EvidenceOutcome::Unspecified, "unspecified"},
    {EvidenceOutcome::EffectObserved, "effect-observed"},
    {EvidenceOutcome::EffectAbsent, "effect-absent"},
    {EvidenceOutcome::Indeterminate, "indeterminate"},
    {EvidenceOutcome::ActionRejected, "action-rejected"},
}};

[[nodiscard]] std::string source_key(DomainKind domain, IncarnationId incarnation) {
  std::string key(to_string(domain));
  key += '/';
  key += incarnation.to_string();
  return key;
}

}  // namespace

std::string_view to_string(EvidenceOutcome value) noexcept {
  for (const auto& entry : kEvidenceOutcomeNames) {
    if (entry.first == value) return entry.second;
  }
  return "invalid";
}

Result<EvidenceOutcome> parse_evidence_outcome(std::string_view text) {
  for (const auto& entry : kEvidenceOutcomeNames) {
    if (entry.second == text) return success(entry.first);
  }
  return failure<EvidenceOutcome>(ErrorCode::InvalidEnumValue, std::string(text),
                                  "unrecognised evidence outcome");
}

bool EvidenceRecord::complete() const noexcept {
  return id.valid() && plan.valid() && plan_revision.valid() && step.valid() && attempt.valid() &&
         is_valid(source_domain) && source_incarnation.valid() && source_control_epoch.valid() &&
         observation_sequence.valid() && observed_generations.complete() &&
         content_digest.valid() && is_valid(outcome) &&
         (!has_observed_asset || observed_asset.complete());
}

void EvidenceRecord::hash_into(Sha256& hasher) const noexcept {
  hasher.update(id.value());
  hasher.update_u8(0);
  hasher.update(plan.value());
  hasher.update_u8(0);
  hasher.update_u64(plan_revision.value());
  hasher.update(step.value());
  hasher.update_u8(0);
  hasher.update(attempt.value());
  hasher.update_u8(0);
  hasher.update_u8(static_cast<std::uint8_t>(source_domain));
  hasher.update_u64(source_incarnation.value());
  hasher.update_u64(source_control_epoch.value());
  hasher.update_u64(observation_sequence.value());
  observed_generations.hash_into(hasher);
  hasher.update(content_digest.bytes().data(), content_digest.bytes().size());
  hasher.update_u8(static_cast<std::uint8_t>(outcome));
  hasher.update_u64(observed_at_micros);
  hasher.update_u8(has_observed_asset ? 1u : 0u);
  if (has_observed_asset) observed_asset.hash_into(hasher);
  hasher.update(note);
}

Digest EvidenceRecord::digest() const {
  Sha256 hasher;
  hasher.update("fco/evidence-record/1");
  hash_into(hasher);
  return hasher.finish();
}

std::string EvidenceRecord::describe() const {
  std::string out = id.to_string();
  out += " step=" + step.to_string();
  out += " attempt=" + attempt.to_string();
  out += " domain=" + std::string(to_string(source_domain));
  out += " outcome=" + std::string(to_string(outcome));
  out += " seq=" + observation_sequence.to_string();
  return out;
}

Result<bool> EvidenceLedger::append(const EvidenceRecord& record) {
  if (!record.id.valid()) {
    return failure<bool>(ErrorCode::UnboundEvidence, "evidence.id",
                         "evidence identifier is unset");
  }
  const std::string subject = record.id.to_string();
  if (!record.plan.valid() || !record.plan_revision.valid() || !record.step.valid() ||
      !record.attempt.valid()) {
    return failure<bool>(ErrorCode::UnboundEvidence, subject,
                         "evidence is not bound to a plan revision, step and attempt");
  }
  if (!is_valid(record.source_domain)) {
    return failure<bool>(ErrorCode::UnboundEvidence, subject,
                         "evidence does not name its source domain");
  }
  if (!record.source_incarnation.valid() || !record.source_control_epoch.valid()) {
    return failure<bool>(ErrorCode::MissingAuthority, subject,
                         "evidence does not carry its source incarnation and control epoch");
  }
  if (!record.observation_sequence.valid()) {
    return failure<bool>(ErrorCode::ReorderedEvidence, subject,
                         "evidence observation sequence must be at least 1");
  }
  if (!record.observed_generations.complete()) {
    return failure<bool>(ErrorCode::UnboundEvidence, subject,
                         "evidence does not carry a complete generation observation");
  }
  if (!record.content_digest.valid()) {
    return failure<bool>(ErrorCode::UnboundEvidence, subject,
                         "evidence does not carry a content digest");
  }
  if (!is_valid(record.outcome)) {
    return failure<bool>(ErrorCode::InvalidEnumValue, subject, "evidence outcome is unset");
  }
  if (record.has_observed_asset && !record.observed_asset.complete()) {
    return failure<bool>(ErrorCode::ImpossibleCombination, subject,
                         "evidence carries an incomplete observed asset record");
  }
  if (record.outcome == EvidenceOutcome::EffectObserved && !record.has_observed_asset) {
    return failure<bool>(ErrorCode::UnboundEvidence, subject,
                         "an observation claiming an effect carries no observed asset record");
  }

  if (!is_valid_identifier(record.id.value())) {
    return failure<bool>(ErrorCode::InvalidIdentity, subject,
                         "evidence identifier is empty, longer than 64 bytes, or contains an "
                         "unsupported character");
  }

  const auto existing = records_.find(record.id);
  if (existing != records_.end()) {
    if (existing->second.digest() == record.digest()) {
      return success(false);  // idempotent replay of an identical observation
    }
    return failure<bool>(ErrorCode::DuplicateIdentity, subject,
                         "evidence identifier was already used for different content");
  }

  const std::string key = source_key(record.source_domain, record.source_incarnation);
  const auto watermark = watermarks_.find(key);
  if (watermark != watermarks_.end() &&
      record.observation_sequence.value() <= watermark->second.value()) {
    return failure<bool>(ErrorCode::ReorderedEvidence, subject,
                         "observation sequence " + record.observation_sequence.to_string() +
                             " is not ahead of the source watermark " +
                             watermark->second.to_string() + " for " + key);
  }

  if (records_.size() >= static_cast<std::size_t>(kMaxCollectionEntries)) {
    return failure<bool>(ErrorCode::BoundedLimitExceeded, subject,
                         "evidence ledger has reached its supported bound");
  }

  watermarks_.insert_or_assign(key, record.observation_sequence);
  records_.insert_or_assign(record.id, record);
  return success(true);
}

const EvidenceRecord* EvidenceLedger::find(const EvidenceId& id) const noexcept {
  const auto it = records_.find(id);
  return it == records_.end() ? nullptr : &it->second;
}

std::vector<const EvidenceRecord*> EvidenceLedger::for_attempt(const AttemptId& attempt) const {
  std::vector<const EvidenceRecord*> out;
  for (const auto& entry : records_) {
    if (entry.second.attempt == attempt) out.push_back(&entry.second);
  }
  return out;
}

std::vector<const EvidenceRecord*> EvidenceLedger::for_step(const StepId& step) const {
  std::vector<const EvidenceRecord*> out;
  for (const auto& entry : records_) {
    if (entry.second.step == step) out.push_back(&entry.second);
  }
  return out;
}

ObservationSequence EvidenceLedger::watermark(DomainKind domain,
                                              IncarnationId incarnation) const {
  const auto it = watermarks_.find(source_key(domain, incarnation));
  return it == watermarks_.end() ? ObservationSequence{} : it->second;
}

Digest EvidenceLedger::digest() const {
  Sha256 hasher;
  hasher.update("fco/evidence-ledger/1");
  hasher.update_u64(static_cast<std::uint64_t>(records_.size()));
  for (const auto& entry : records_) {
    entry.second.hash_into(hasher);
  }
  return hasher.finish();
}

void EvidenceLedger::restore(const EvidenceRecord& record) {
  if (!record.id.valid()) return;
  records_.insert_or_assign(record.id, record);
}

void EvidenceLedger::rebuild_watermarks() {
  watermarks_.clear();
  for (const auto& entry : records_) {
    const EvidenceRecord& record = entry.second;
    if (!is_valid(record.source_domain) || !record.source_incarnation.valid()) continue;
    const std::string key = source_key(record.source_domain, record.source_incarnation);
    const auto it = watermarks_.find(key);
    if (it == watermarks_.end() || it->second.value() < record.observation_sequence.value()) {
      watermarks_.insert_or_assign(key, record.observation_sequence);
    }
  }
}

void EvidenceLedger::clear() {
  records_.clear();
  watermarks_.clear();
}

}  // namespace fco
