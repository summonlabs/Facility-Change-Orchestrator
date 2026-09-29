// Evidence ledger tests.
//
// Evidence is a typed claim about what a domain authority observed. src/evidence.cpp
// documents the contract this suite pins: a record that is not bound to a plan
// revision, step and attempt is refused, per-source observation sequences are
// strictly increasing and independent between (domain, incarnation) pairs,
// re-ingesting an identical record is an idempotent success(false), and reusing
// an identifier for different content is a conflict.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fco/authority.hpp"
#include "fco/digest.hpp"
#include "fco/error.hpp"
#include "fco/evidence.hpp"
#include "fco/facility.hpp"
#include "fco/strong.hpp"
#include "fco/version.hpp"
#include "test_support.hpp"

namespace {

using fco::AssetId;
using fco::AssetLifecycle;
using fco::AssetRecord;
using fco::AttemptId;
using fco::CoolingState;
using fco::ControlEpoch;
using fco::Digest;
using fco::DomainKind;
using fco::ErrorCode;
using fco::EvidenceId;
using fco::EvidenceLedger;
using fco::EvidenceOutcome;
using fco::EvidenceRecord;
using fco::GenerationKind;
using fco::GenerationSet;
using fco::IncarnationId;
using fco::LifecycleGeneration;
using fco::MaintenanceGeneration;
using fco::MaintenanceState;
using fco::NetworkState;
using fco::ObservationSequence;
using fco::PlanId;
using fco::PlanRevision;
using fco::PowerState;
using fco::Sha256;
using fco::SiteId;
using fco::StepId;
using fco::TenantId;
using fco::WorkloadState;

constexpr std::uint32_t kGenerationCount = static_cast<std::uint32_t>(fco::kGenerationKindMax);

[[nodiscard]] GenerationSet complete_generations(std::uint64_t base) {
  GenerationSet set;
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    set.set(static_cast<GenerationKind>(raw), base + raw);
  }
  return set;
}

[[nodiscard]] AssetRecord make_asset(const char* id) {
  AssetRecord record;
  record.id = AssetId(id);
  record.rack = fco::RackId("rack-a1");
  record.site = SiteId("site-alpha");
  record.lifecycle = AssetLifecycle::Active;
  record.power = PowerState::On;
  record.cooling = CoolingState::Normal;
  record.maintenance = MaintenanceState::None;
  record.network = NetworkState::Attached;
  record.workloads = WorkloadState::Drained;
  record.hardware_generation = fco::HardwareGeneration(3);
  record.firmware_generation = fco::FirmwareGeneration(7);
  record.lifecycle_generation = LifecycleGeneration(11);
  record.maintenance_generation = MaintenanceGeneration(5);
  record.capacity_generation = fco::CapacityGeneration(2);
  record.tenant = TenantId("tenant-blue");
  record.capacity_total_units = 64;
  record.capacity_reserved_units = 16;
  return record;
}

[[nodiscard]] EvidenceRecord make_record(const std::string& id, DomainKind domain,
                                         std::uint64_t incarnation, std::uint64_t sequence) {
  EvidenceRecord record;
  record.id = EvidenceId(id);
  record.plan = PlanId("plan-1");
  record.plan_revision = PlanRevision(1);
  record.step = StepId("drain-asset-a");
  record.attempt = AttemptId("attempt-1");
  record.source_domain = domain;
  record.source_incarnation = IncarnationId(incarnation);
  record.source_control_epoch = ControlEpoch(1);
  record.observation_sequence = ObservationSequence(sequence);
  record.observed_generations = complete_generations(100);
  record.content_digest = Sha256::of("content-" + id);
  record.outcome = EvidenceOutcome::EffectObserved;
  record.observed_at_micros = 1700000000000000;
  record.has_observed_asset = true;
  record.observed_asset = make_asset("asset-a");
  record.note = "observation " + id;
  return record;
}

[[nodiscard]] Digest digest_of_single(const EvidenceRecord& record) {
  EvidenceLedger ledger;
  const auto appended = ledger.append(record);
  ::fco::test::check_true(appended.ok() && appended.value(), "digest fixture append", __FILE__,
                          __LINE__);
  return ledger.digest();
}

using Mutator = void (*)(EvidenceRecord&);

struct RejectionCase {
  const char* label;
  ErrorCode code;
  bool complete;
  Mutator mutate;
};

}  // namespace

// ---------------------------------------------------------------------------
// Record validation
// ---------------------------------------------------------------------------
FCO_TEST(evidence, rejects_unbound_and_incomplete_records) {
  const std::vector<RejectionCase> cases = {
      {"unset id", ErrorCode::UnboundEvidence, false,
       [](EvidenceRecord& r) { r.id = EvidenceId{}; }},
      {"unset plan", ErrorCode::UnboundEvidence, false,
       [](EvidenceRecord& r) { r.plan = PlanId{}; }},
      {"unset plan revision", ErrorCode::UnboundEvidence, false,
       [](EvidenceRecord& r) { r.plan_revision = PlanRevision{}; }},
      {"unset step", ErrorCode::UnboundEvidence, false,
       [](EvidenceRecord& r) { r.step = StepId{}; }},
      {"unset attempt", ErrorCode::UnboundEvidence, false,
       [](EvidenceRecord& r) { r.attempt = AttemptId{}; }},
      {"unset source domain", ErrorCode::UnboundEvidence, false,
       [](EvidenceRecord& r) { r.source_domain = DomainKind::Unspecified; }},
      {"source domain out of its domain", ErrorCode::UnboundEvidence, false,
       [](EvidenceRecord& r) { r.source_domain = static_cast<DomainKind>(200); }},
      {"unset source incarnation", ErrorCode::MissingAuthority, false,
       [](EvidenceRecord& r) { r.source_incarnation = IncarnationId{}; }},
      {"unset control epoch", ErrorCode::MissingAuthority, false,
       [](EvidenceRecord& r) { r.source_control_epoch = ControlEpoch{}; }},
      {"zero observation sequence", ErrorCode::ReorderedEvidence, false,
       [](EvidenceRecord& r) { r.observation_sequence = ObservationSequence{}; }},
      {"incomplete observed generations", ErrorCode::UnboundEvidence, false,
       [](EvidenceRecord& r) { r.observed_generations.dfi = fco::DfiGeneration{}; }},
      {"invalid content digest", ErrorCode::UnboundEvidence, false,
       [](EvidenceRecord& r) { r.content_digest = Digest{}; }},
      {"unset outcome", ErrorCode::InvalidEnumValue, false,
       [](EvidenceRecord& r) { r.outcome = EvidenceOutcome::Unspecified; }},
      {"outcome outside its domain", ErrorCode::InvalidEnumValue, false,
       [](EvidenceRecord& r) { r.outcome = static_cast<EvidenceOutcome>(200); }},
      {"incomplete observed asset", ErrorCode::ImpossibleCombination, false,
       [](EvidenceRecord& r) { r.observed_asset.id = AssetId{}; }},
      {"effect observed without an observed asset", ErrorCode::UnboundEvidence, true,
       [](EvidenceRecord& r) { r.has_observed_asset = false; }},
  };

  for (const RejectionCase& entry : cases) {
    EvidenceLedger ledger;
    EvidenceRecord record = make_record("evidence-case", DomainKind::Asi, 1, 1);
    entry.mutate(record);
    FCO_CHECK_EQ(record.complete(), entry.complete);
    auto result = ledger.append(record);
    if (result.ok()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  std::string(entry.label) + ": append unexpectedly succeeded");
      continue;
    }
    ::fco::test::check_error(result.error(), entry.code, entry.label, __FILE__, __LINE__);
    FCO_CHECK_EQ(ledger.size(), std::size_t{0});
  }

  // The untouched record is accepted, so the table above is not vacuous.
  EvidenceLedger ledger;
  const EvidenceRecord good = make_record("evidence-good", DomainKind::Asi, 1, 1);
  FCO_CHECK(good.complete());
  auto appended = ledger.append(good);
  FCO_REQUIRE(appended.ok());
  FCO_CHECK(appended.value());
  FCO_CHECK_EQ(ledger.size(), std::size_t{1});
}

FCO_TEST(evidence, records_that_are_not_effect_observed_may_omit_the_asset) {
  EvidenceLedger ledger;
  for (std::uint64_t index = 1; index <= 3; ++index) {
    const std::vector<EvidenceOutcome> outcomes = {EvidenceOutcome::EffectAbsent,
                                                   EvidenceOutcome::Indeterminate,
                                                   EvidenceOutcome::ActionRejected};
    EvidenceRecord record =
        make_record("evidence-" + std::to_string(index), DomainKind::Asi, 1, index);
    record.outcome = outcomes[static_cast<std::size_t>(index - 1)];
    record.has_observed_asset = false;
    record.observed_asset = AssetRecord{};
    FCO_CHECK(record.complete());
    auto appended = ledger.append(record);
    FCO_REQUIRE(appended.ok());
    FCO_CHECK(appended.value());
  }
  FCO_CHECK_EQ(ledger.size(), std::size_t{3});
}

// ---------------------------------------------------------------------------
// Per-source sequencing
// ---------------------------------------------------------------------------
FCO_TEST(evidence, watermarks_are_independent_per_domain_and_incarnation) {
  EvidenceLedger ledger;

  auto first = ledger.append(make_record("e-asi-1-5", DomainKind::Asi, 1, 5));
  FCO_REQUIRE(first.ok());
  FCO_CHECK(first.value());
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Asi, IncarnationId(1)).value(), std::uint64_t{5});
  // The same domain under a different incarnation, and a different domain, are
  // untouched by that observation.
  FCO_CHECK(!ledger.watermark(DomainKind::Asi, IncarnationId(2)).valid());
  FCO_CHECK(!ledger.watermark(DomainKind::Power, IncarnationId(1)).valid());

  auto other_domain = ledger.append(make_record("e-power-1-1", DomainKind::Power, 1, 1));
  FCO_REQUIRE(other_domain.ok());
  FCO_CHECK(other_domain.value());
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Power, IncarnationId(1)).value(), std::uint64_t{1});
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Asi, IncarnationId(1)).value(), std::uint64_t{5});

  auto other_incarnation = ledger.append(make_record("e-asi-2-1", DomainKind::Asi, 2, 1));
  FCO_REQUIRE(other_incarnation.ok());
  FCO_CHECK(other_incarnation.value());
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Asi, IncarnationId(2)).value(), std::uint64_t{1});

  // A replay of a sequence already seen for that source is reordered.
  auto replayed = ledger.append(make_record("e-asi-1-5-again", DomainKind::Asi, 1, 5));
  FCO_CHECK_ERROR(replayed, ErrorCode::ReorderedEvidence);
  auto older = ledger.append(make_record("e-asi-1-4", DomainKind::Asi, 1, 4));
  FCO_CHECK_ERROR(older, ErrorCode::ReorderedEvidence);
  auto other_source_older = ledger.append(make_record("e-power-1-1-again", DomainKind::Power, 1, 1));
  FCO_CHECK_ERROR(other_source_older, ErrorCode::ReorderedEvidence);

  // Strictly increasing sequences are accepted and move only their own watermark.
  auto next = ledger.append(make_record("e-asi-1-6", DomainKind::Asi, 1, 6));
  FCO_REQUIRE(next.ok());
  FCO_CHECK(next.value());
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Asi, IncarnationId(1)).value(), std::uint64_t{6});
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Asi, IncarnationId(2)).value(), std::uint64_t{1});
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Power, IncarnationId(1)).value(), std::uint64_t{1});

  // A gap is a legitimate observation, not an error: the sequence only has to
  // move forward.
  auto jump = ledger.append(make_record("e-asi-1-100", DomainKind::Asi, 1, 100));
  FCO_REQUIRE(jump.ok());
  FCO_CHECK(jump.value());
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Asi, IncarnationId(1)).value(), std::uint64_t{100});
  FCO_CHECK_EQ(ledger.size(), std::size_t{5});
}

// ---------------------------------------------------------------------------
// Idempotent replay, conflicts, and the ledger digest
// ---------------------------------------------------------------------------
FCO_TEST(evidence, identical_replay_is_idempotent_and_conflicts_are_identified) {
  EvidenceLedger ledger;
  const EvidenceRecord record = make_record("evidence-1", DomainKind::Asi, 1, 7);
  auto first = ledger.append(record);
  FCO_REQUIRE(first.ok());
  FCO_CHECK(first.value());
  const Digest before = ledger.digest();
  const std::size_t size_before = ledger.size();

  auto replay = ledger.append(record);
  FCO_REQUIRE(replay.ok());
  FCO_CHECK(!replay.value());
  FCO_CHECK_EQ(ledger.size(), size_before);
  FCO_CHECK_EQ(ledger.digest(), before);
  const EvidenceRecord* stored = ledger.find(EvidenceId("evidence-1"));
  FCO_REQUIRE(stored != nullptr);
  FCO_CHECK_EQ(stored->digest(), record.digest());
  FCO_CHECK_EQ(stored->note, record.note);
  FCO_CHECK_EQ(stored->observation_sequence.value(), record.observation_sequence.value());

  EvidenceRecord conflicting = record;
  conflicting.note = "a different observation";
  auto conflict = ledger.append(conflicting);
  FCO_CHECK_ERROR(conflict, ErrorCode::DuplicateIdentity);
  FCO_CHECK_EQ(ledger.digest(), before);

  EvidenceRecord different_content = record;
  different_content.content_digest = Sha256::of("something else");
  auto content_conflict = ledger.append(different_content);
  FCO_CHECK_ERROR(content_conflict, ErrorCode::DuplicateIdentity);
  FCO_CHECK_EQ(ledger.digest(), before);

  EvidenceRecord different_outcome = record;
  different_outcome.outcome = EvidenceOutcome::Indeterminate;
  auto outcome_conflict = ledger.append(different_outcome);
  FCO_CHECK_ERROR(outcome_conflict, ErrorCode::DuplicateIdentity);
  FCO_CHECK_EQ(ledger.digest(), before);

  EvidenceRecord different_sequence = record;
  different_sequence.observation_sequence = ObservationSequence(8);
  auto sequence_conflict = ledger.append(different_sequence);
  FCO_CHECK_ERROR(sequence_conflict, ErrorCode::DuplicateIdentity);
  FCO_CHECK_EQ(ledger.digest(), before);
}

FCO_TEST(evidence, the_ledger_digest_changes_when_any_field_changes) {
  const EvidenceRecord base = make_record("evidence-1", DomainKind::Asi, 1, 7);
  const Digest base_digest = digest_of_single(base);
  FCO_CHECK(base_digest.valid());

  const std::vector<EvidenceRecord> variants = [&]() {
    std::vector<EvidenceRecord> out;
    EvidenceRecord variant = base;
    variant.id = EvidenceId("evidence-2");
    out.push_back(variant);
    variant = base;
    variant.plan = PlanId("plan-2");
    out.push_back(variant);
    variant = base;
    variant.plan_revision = PlanRevision(2);
    out.push_back(variant);
    variant = base;
    variant.step = StepId("drain-asset-b");
    out.push_back(variant);
    variant = base;
    variant.attempt = AttemptId("attempt-2");
    out.push_back(variant);
    variant = base;
    variant.source_domain = DomainKind::Power;
    out.push_back(variant);
    variant = base;
    variant.source_incarnation = IncarnationId(2);
    out.push_back(variant);
    variant = base;
    variant.source_control_epoch = ControlEpoch(2);
    out.push_back(variant);
    variant = base;
    variant.observation_sequence = ObservationSequence(8);
    out.push_back(variant);
    variant = base;
    variant.observed_generations.policy = fco::PolicyGeneration(999);
    out.push_back(variant);
    variant = base;
    variant.content_digest = Sha256::of("other content");
    out.push_back(variant);
    variant = base;
    variant.outcome = EvidenceOutcome::EffectAbsent;
    out.push_back(variant);
    variant = base;
    variant.observed_at_micros = 1;
    out.push_back(variant);
    variant = base;
    variant.has_observed_asset = false;
    variant.outcome = EvidenceOutcome::Indeterminate;
    out.push_back(variant);
    variant = base;
    variant.observed_asset.workloads = WorkloadState::Present;
    out.push_back(variant);
    variant = base;
    variant.note = "a different note";
    out.push_back(variant);
    return out;
  }();

  FCO_CHECK_EQ(variants.size(), std::size_t{16});
  for (const EvidenceRecord& variant : variants) {
    const Digest variant_digest = digest_of_single(variant);
    FCO_CHECK(variant_digest != base_digest);
  }

  // Two identical ledgers still digest identically.
  FCO_CHECK_EQ(digest_of_single(base), base_digest);
}

FCO_TEST(evidence, lookup_helpers_return_stored_records) {
  EvidenceLedger ledger;
  FCO_REQUIRE(ledger.append(make_record("e-1", DomainKind::Asi, 1, 1)).ok());
  FCO_REQUIRE(ledger.append(make_record("e-2", DomainKind::Power, 1, 1)).ok());

  EvidenceRecord second = make_record("e-3", DomainKind::Asi, 1, 2);
  second.attempt = AttemptId("attempt-2");
  second.step = StepId("isolate-asset-a");
  FCO_REQUIRE(ledger.append(second).ok());

  FCO_CHECK(ledger.find(EvidenceId("e-1")) != nullptr);
  FCO_CHECK(ledger.find(EvidenceId("absent")) == nullptr);
  FCO_CHECK_EQ(ledger.for_attempt(AttemptId("attempt-1")).size(), std::size_t{2});
  FCO_CHECK_EQ(ledger.for_attempt(AttemptId("attempt-2")).size(), std::size_t{1});
  FCO_CHECK_EQ(ledger.for_step(StepId("drain-asset-a")).size(), std::size_t{2});
  FCO_CHECK_EQ(ledger.for_step(StepId("isolate-asset-a")).size(), std::size_t{1});
  FCO_CHECK_EQ(ledger.for_step(StepId("absent")).size(), std::size_t{0});

  const Digest before_clear = ledger.digest();
  ledger.clear();
  FCO_CHECK_EQ(ledger.size(), std::size_t{0});
  FCO_CHECK(ledger.digest() != before_clear);
  FCO_CHECK(!ledger.watermark(DomainKind::Asi, IncarnationId(1)).valid());
  FCO_CHECK(ledger.find(EvidenceId("e-1")) == nullptr);
}

FCO_TEST(evidence, watermarks_are_rebuilt_from_restored_records) {
  EvidenceLedger ledger;
  ledger.restore(make_record("e-1", DomainKind::Asi, 1, 4));
  ledger.restore(make_record("e-2", DomainKind::Asi, 1, 9));
  ledger.restore(make_record("e-3", DomainKind::Asi, 2, 2));
  // restore() does not maintain watermarks; the durable decoder calls
  // rebuild_watermarks() after loading the ledger.
  FCO_CHECK(!ledger.watermark(DomainKind::Asi, IncarnationId(1)).valid());
  ledger.rebuild_watermarks();
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Asi, IncarnationId(1)).value(), std::uint64_t{9});
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Asi, IncarnationId(2)).value(), std::uint64_t{2});
  FCO_CHECK(!ledger.watermark(DomainKind::Power, IncarnationId(1)).valid());

  // A record restored without a usable source is skipped rather than counted.
  EvidenceRecord broken = make_record("e-4", DomainKind::Asi, 1, 1);
  broken.source_domain = DomainKind::Unspecified;
  ledger.restore(broken);
  ledger.rebuild_watermarks();
  FCO_CHECK_EQ(ledger.watermark(DomainKind::Asi, IncarnationId(1)).value(), std::uint64_t{9});
}

// ---------------------------------------------------------------------------
// The ledger bound
// ---------------------------------------------------------------------------
FCO_TEST(evidence, the_ledger_bound_is_enforced) {
  // The bound is a compile-time constant that the ledger checks against its own
  // size. Filling the map through restore() is the cheap way to reach it: it is
  // the entry point the durable decoder uses, so it does not enforce the bound
  // itself, and one template record is copied kMaxCollectionEntries times.
  FCO_CHECK_EQ(fco::kMaxCollectionEntries, std::uint64_t{100000});
  EvidenceLedger ledger;
  const EvidenceRecord template_record =
      make_record("filler-template", DomainKind::Asi, 1, 1);
  FCO_REQUIRE(template_record.complete());
  for (std::uint64_t index = 0; index < fco::kMaxCollectionEntries; ++index) {
    EvidenceRecord filler = template_record;
    filler.id = EvidenceId("filler-" + std::to_string(index));
    ledger.restore(filler);
  }
  FCO_CHECK_EQ(ledger.size(), static_cast<std::size_t>(fco::kMaxCollectionEntries));

  const EvidenceRecord extra = make_record("filler-overflow", DomainKind::Asi, 1, 1);
  FCO_CHECK_ERROR(ledger.append(extra), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_EQ(ledger.size(), static_cast<std::size_t>(fco::kMaxCollectionEntries));
}

FCO_TEST_MAIN
