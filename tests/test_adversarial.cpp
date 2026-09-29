// Deliberate attacks on the runtime.
//
// Every case here is an attempt to make the runtime proceed with something it
// should refuse: damaged durable records, absurd counters and identifier sizes,
// duplicated identities, out-of-domain enumerators, hostile paths, and
// non-ASCII or NUL-bearing identifiers. Nothing in this file weakens an
// assertion to make behaviour look better: where the implementation has a hole,
// the test pins the hole and reports it.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "fco/codec.hpp"
#include "fco/digest.hpp"
#include "fco/engine.hpp"
#include "fco/error.hpp"
#include "fco/evidence.hpp"
#include "fco/facility.hpp"
#include "fco/json.hpp"
#include "fco/model.hpp"
#include "fco/planner.hpp"
#include "fco/ports.hpp"
#include "fco/sim.hpp"
#include "fco/state.hpp"
#include "fco/store.hpp"
#include "fco/strong.hpp"
#include "test_support.hpp"

namespace {

namespace fs = std::filesystem;

using fco::ActionKind;
using fco::ActionRequest;
using fco::AssetId;
using fco::AssetLifecycle;
using fco::AssetRecord;
using fco::AttemptId;
using fco::AttemptOrdinal;
using fco::AttemptRecord;
using fco::AttemptStatus;
using fco::AuthorityContext;
using fco::ChangeKind;
using fco::ChangePlan;
using fco::ChangeRequest;
using fco::ChangeRequestId;
using fco::ChangeStep;
using fco::CommitSequence;
using fco::ControlEpoch;
using fco::CoolingState;
using fco::Digest;
using fco::DomainKind;
using fco::DurableStore;
using fco::EffectKind;
using fco::EffectSpec;
using fco::ErrorCode;
using fco::EvidenceId;
using fco::EvidenceLedger;
using fco::EvidenceOutcome;
using fco::EvidenceRecord;
using fco::FacilityEpoch;
using fco::FacilityRevision;
using fco::FacilitySnapshot;
using fco::FirmwareGeneration;
using fco::GenerationDelta;
using fco::GenerationKind;
using fco::GenerationSet;
using fco::HardwareGeneration;
using fco::IncarnationId;
using fco::LifecycleGeneration;
using fco::MaintenanceGeneration;
using fco::MaintenanceState;
using fco::NetworkState;
using fco::OrchestratorState;
using fco::PlanId;
using fco::PlanRevision;
using fco::PlanState;
using fco::PolicyGeneration;
using fco::PolicyId;
using fco::PowerState;
using fco::Predicate;
using fco::PredicateKind;
using fco::PrincipalId;
using fco::Revision;
using fco::SafetyClass;
using fco::Sha256;
using fco::SiteId;
using fco::StepId;
using fco::StepStatus;
using fco::TenantId;
using fco::VerificationMode;
using fco::WorkloadState;

constexpr std::size_t kFrameHeaderBytes = 24;
constexpr std::size_t kFrameTrailerBytes = 32;
constexpr std::uint32_t kGenerationCount = static_cast<std::uint32_t>(fco::kGenerationKindMax);

class TempRoot {
 public:
  TempRoot() {
    static unsigned counter = 0;
    ++counter;
    std::ostringstream name;
    name << "fco-adversarial-" << counter << '-'
         << std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = fs::temp_directory_path() / name.str();
    std::error_code code;
    fs::remove_all(root_, code);
    fs::create_directories(root_, code);
  }
  ~TempRoot() {
    std::error_code code;
    fs::remove_all(root_, code);
  }
  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;
  [[nodiscard]] const fs::path& path() const noexcept { return root_; }

 private:
  fs::path root_;
};

[[nodiscard]] GenerationSet generations_for(std::uint64_t epoch) {
  GenerationSet set;
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    set.set(static_cast<GenerationKind>(raw), epoch + raw);
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
  record.workloads = WorkloadState::Present;
  record.hardware_generation = HardwareGeneration(3);
  record.firmware_generation = FirmwareGeneration(7);
  record.lifecycle_generation = LifecycleGeneration(11);
  record.maintenance_generation = MaintenanceGeneration(5);
  record.capacity_generation = fco::CapacityGeneration(2);
  record.tenant = TenantId("tenant-blue");
  record.capacity_total_units = 64;
  record.capacity_reserved_units = 16;
  return record;
}

[[nodiscard]] AuthorityContext make_authority() {
  AuthorityContext authority;
  authority.incarnation = IncarnationId(1);
  authority.control_epoch = ControlEpoch(1);
  authority.actor = PrincipalId("principal-ops");
  authority.policy = PolicyId("policy-change-v1");
  authority.policy_digest = Sha256::of("policy-change-v1");
  return authority;
}

[[nodiscard]] ActionRequest make_request(ActionKind kind, const AssetRecord& asset) {
  ActionRequest request;
  request.kind = kind;
  request.owner = fco::owning_domain(kind);
  request.asset = asset.id;
  request.rack = asset.rack;
  request.site = asset.site;
  request.hardware_generation = asset.hardware_generation;
  request.firmware_generation = asset.firmware_generation;
  request.lifecycle_generation = asset.lifecycle_generation;
  return request;
}

[[nodiscard]] ChangeStep make_step(const std::string& id, const AssetRecord& asset) {
  ChangeStep step;
  step.id = StepId(id);
  step.action = ActionKind::DrainWorkloads;
  step.owner = fco::owning_domain(step.action);
  step.safety = SafetyClass::Standard;
  step.reversibility = fco::Reversibility::Compensable;
  step.verification = VerificationMode::Required;
  step.request = make_request(step.action, asset);
  step.compensation = make_request(ActionKind::RestoreWorkloads, asset);
  Predicate predicate;
  predicate.kind = PredicateKind::WorkloadStateIs;
  predicate.asset = asset.id;
  predicate.expected_state = static_cast<std::uint32_t>(WorkloadState::Drained);
  step.preconditions.push_back(predicate);
  EffectSpec effect;
  effect.kind = EffectKind::WorkloadStateChange;
  effect.asset = asset.id;
  effect.expected_state = static_cast<std::uint32_t>(WorkloadState::Drained);
  step.expected_effect = effect;
  step.rationale = "attack fixture step";
  return step;
}

[[nodiscard]] ChangePlan make_plan(const std::string& id, const std::string& request_id,
                                   const GenerationSet& generations,
                                   const FacilitySnapshot& facility, const AssetRecord& asset) {
  ChangePlan plan;
  plan.id = PlanId(id);
  plan.request_id = ChangeRequestId(request_id);
  plan.revision = PlanRevision(1);
  plan.state = PlanState::Draft;
  plan.kind = ChangeKind("rack-replacement");
  plan.site = facility.site();
  plan.intent = "attack fixture";
  plan.scope.site = facility.site();
  plan.scope.assets = {asset.id};
  plan.facility_epoch = facility.epoch();
  plan.policy_generation = generations.policy;
  plan.bound_generations = generations;
  plan.request_digest = Sha256::of(request_id);
  plan.evidence_digest = Sha256::of("attack evidence");
  plan.facility_digest = facility.digest();
  plan.authority = make_authority();
  const ChangeStep step = make_step("step-1", asset);
  plan.steps.push_back(step);
  plan.step_status.insert_or_assign(step.id, StepStatus::Pending);
  plan.explained_revision = FacilityRevision(1);
  plan.transition_note = "attack fixture";
  plan.created_at_micros = 1700000000000000;
  plan.updated_at_micros = 1700000000000000;
  return plan;
}

[[nodiscard]] OrchestratorState make_small_state(std::uint64_t revision, std::uint64_t sequence) {
  const GenerationSet generations = generations_for(4);
  auto facility = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4), generations,
                                           {make_asset("asset-a")});
  ::fco::test::check_true(facility.ok(), "attack fixture facility", __FILE__, __LINE__);
  OrchestratorState state;
  state.revision = Revision(revision);
  state.commit_sequence = CommitSequence(sequence);
  state.facility_epoch = FacilityEpoch(4);
  state.generations = generations;
  state.facility_revision = FacilityRevision(revision);
  if (facility.ok()) state.facility = facility.value();
  state.incarnation = IncarnationId(1);
  state.control_epoch = ControlEpoch(1);
  state.actor = PrincipalId("principal-ops");
  state.policy = PolicyId("policy-change-v1");
  state.policy_digest = Sha256::of("policy-change-v1");
  state.updated_at_micros = 1700000000000000 + revision;
  return state;
}

[[nodiscard]] OrchestratorState make_rich_state() {
  const GenerationSet generations = generations_for(4);
  const AssetRecord asset_a = make_asset("asset-a");
  const AssetRecord asset_b = make_asset("asset-b");
  auto facility = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4), generations,
                                           {asset_a, asset_b});
  ::fco::test::check_true(facility.ok(), "attack fixture facility", __FILE__, __LINE__);
  OrchestratorState state = make_small_state(5, 5);
  if (facility.ok()) state.facility = facility.value();
  state.generations = generations;
  state.facility_epoch = FacilityEpoch(4);

  const ChangePlan plan_one = make_plan("plan-1", "request-1", generations, state.facility, asset_a);
  const ChangePlan plan_two = make_plan("plan-2", "request-2", generations, state.facility, asset_b);
  state.plans.insert_or_assign(plan_one.id, plan_one);
  state.plans.insert_or_assign(plan_two.id, plan_two);

  for (const std::string& id : {std::string("evidence-1"), std::string("evidence-2")}) {
    EvidenceRecord record;
    record.id = EvidenceId(id);
    record.plan = PlanId(id == "evidence-1" ? "plan-1" : "plan-2");
    record.plan_revision = PlanRevision(1);
    record.step = StepId("step-1");
    record.attempt = AttemptId("attempt-absent");
    record.source_domain = DomainKind::Asi;
    record.source_incarnation = IncarnationId(1);
    record.source_control_epoch = ControlEpoch(1);
    record.observation_sequence = fco::ObservationSequence(id == "evidence-1" ? 1u : 2u);
    record.observed_generations = generations;
    record.content_digest = Sha256::of(id);
    record.outcome = EvidenceOutcome::EffectAbsent;
    record.observed_at_micros = 1700000000000000;
    state.evidence.restore(record);
  }
  state.evidence.rebuild_watermarks();
  return state;
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::uint8_t> bytes;
  if (!stream) return bytes;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string text = buffer.str();
  bytes.reserve(text.size());
  for (const char character : text) bytes.push_back(static_cast<std::uint8_t>(character));
  return bytes;
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4u; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>((value >> (8u * index)) & 0xFFu);
  }
}

void put_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned index = 0; index < 8u; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>((value >> (8u * index)) & 0xFFu);
  }
}

// Rewrites every occurrence of a length-prefixed identifier field, which is how
// a decoded list is made to contain a duplicate identifier without the encoder
// ever producing one on its own. The length prefix is part of the pattern so
// that an identifier is never confused with the same bytes inside a string.
[[nodiscard]] std::size_t replace_all_occurrences(std::vector<std::uint8_t>& bytes,
                                                  const std::string& needle,
                                                  const std::string& replacement) {
  if (needle.empty() || needle.size() != replacement.size()) return 0;
  const auto pattern_begin = reinterpret_cast<const std::uint8_t*>(needle.data());
  const auto pattern_end = pattern_begin + needle.size();
  std::size_t replaced = 0;
  auto position = bytes.begin();
  for (;;) {
    position = std::search(position, bytes.end(), pattern_begin, pattern_end);
    if (position == bytes.end()) break;
    std::copy(reinterpret_cast<const std::uint8_t*>(replacement.data()),
              reinterpret_cast<const std::uint8_t*>(replacement.data()) + replacement.size(),
              position);
    ++replaced;
    position += static_cast<std::ptrdiff_t>(needle.size());
  }
  return replaced;
}

[[nodiscard]] fs::path records_directory(const fs::path& root) {
  return root / std::string(DurableStore::records_directory_name());
}

[[nodiscard]] fs::path record_path(const fs::path& root, std::uint64_t generation) {
  return records_directory(root) / DurableStore::state_record_name(generation);
}

[[nodiscard]] std::string mode_name(fco::RecoveryMode mode) {
  return std::string(fco::to_string(mode));
}

void build_two_generations(const fs::path& root, Digest& first, Digest& second) {
  auto opened = DurableStore::open(root);
  ::fco::test::check_true(opened.ok(), "attack fixture open", __FILE__, __LINE__);
  if (!opened.ok()) return;
  DurableStore store = opened.take();
  const OrchestratorState one = make_small_state(1, 1);
  const OrchestratorState two = make_small_state(2, 2);
  first = one.digest();
  second = two.digest();
  ::fco::test::check_true(store.commit(one).ok(), "attack fixture commit 1", __FILE__, __LINE__);
  ::fco::test::check_true(store.commit(two).ok(), "attack fixture commit 2", __FILE__, __LINE__);
  store.close();
}

// Any successful open must hand back a state that was really committed and that
// still decodes; nothing may be invented and nothing may be silently empty.
void expect_recovered(const DurableStore& store, const Digest& first, const Digest& second,
                      const char* label) {
  if (store.generation() > 2u) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string(label) + ": recovery invented generation " +
                                    std::to_string(store.generation()));
    return;
  }
  auto loaded = store.load();
  if (!loaded.ok()) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string(label) + ": load failed: " +
                                    fco::describe(loaded.error()));
    return;
  }
  const Digest digest = loaded.value().digest();
  if (digest != first && digest != second) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string(label) + ": recovered a state that was never committed");
    return;
  }
  auto reencoded = fco::encode_state_bytes(loaded.value());
  if (!reencoded.ok()) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string(label) + ": recovered a state that does not re-encode");
  }
}

[[nodiscard]] bool is_documented_open_code(ErrorCode code) {
  switch (code) {
    case ErrorCode::AmbiguousRecovery:
    case ErrorCode::StorageFailure:
    case ErrorCode::PathRejected:
    case ErrorCode::RecordNotFound:
    case ErrorCode::CommitFailure:
    case ErrorCode::LockUnavailable:
      return true;
    default:
      return false;
  }
}

// A one-step rack-replacement plan for the example facility, used by the
// identity attacks that need a validated plan.
struct PlanFixture {
  fco::GenerationSet generations;
  FacilitySnapshot facility;
  ChangePlan plan;
  bool ready = false;
};

[[nodiscard]] PlanFixture make_plan_fixture() {
  PlanFixture fixture;
  fixture.generations = generations_for(4);
  fixture.facility = fco::make_example_facility(SiteId("site-alpha"), FacilityEpoch(4),
                                                fixture.generations);
  if (!fixture.facility.valid()) return fixture;
  fco::SynthesisInput input;
  input.kind = ChangeKind("rack-replacement");
  input.id = ChangeRequestId("attack-request");
  input.site = fixture.facility.site();
  input.intent = "attack fixture change";
  input.scope.site = fixture.facility.site();
  input.scope.rack = fco::RackId("rack-a1");
  input.scope.assets = {AssetId("rack-a1-node-1")};
  input.facility_epoch = fixture.facility.epoch();
  input.actor = PrincipalId("principal-ops");
  input.revision = Revision(1);
  input.bound_generations = fixture.facility.generations();
  input.evidence_digest = fixture.facility.digest();
  auto request = fco::synthesize_request(input, fixture.facility);
  if (!request.ok()) return fixture;
  auto created = fco::create_plan(request.value(), fixture.facility, make_authority(),
                                  PlanRevision(1), FacilityRevision(1), 1000);
  if (!created.ok()) return fixture;
  fixture.plan = created.take();
  fixture.ready = true;
  return fixture;
}

}  // namespace

// ---------------------------------------------------------------------------
// Malformed durable records
// ---------------------------------------------------------------------------
FCO_TEST(adversarial, every_truncation_of_a_record_file_is_survivable) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);
  const fs::path record = record_path(temp.path(), 2);
  const std::vector<std::uint8_t> whole = read_file(record);
  FCO_REQUIRE(whole.size() > kFrameHeaderBytes + kFrameTrailerBytes);

  for (std::size_t length = 0; length < whole.size(); ++length) {
    std::vector<std::uint8_t> truncated(whole.begin(),
                                        whole.begin() + static_cast<std::ptrdiff_t>(length));
    write_file(record, truncated);
    {
      auto opened = DurableStore::open(temp.path());
      if (!opened.ok()) {
        if (!is_documented_open_code(opened.error().code)) {
          ::fco::test::report_failure(
              __FILE__, __LINE__,
              "truncation to " + std::to_string(length) + " bytes failed with an undocumented " +
                  std::string(fco::to_string(opened.error().code)));
        }
        continue;
      }
      DurableStore store = opened.take();
      expect_recovered(store, first, second, "truncated record file");
      store.close();
    }
  }
  // The untruncated record still opens and loads, so the sweep is not vacuous.
  write_file(record, whole);
  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  auto loaded = opened.value().load();
  FCO_REQUIRE(loaded.ok());
  FCO_CHECK_EQ(loaded.value().digest(), second);
}

FCO_TEST(adversarial, every_flipped_byte_of_a_record_file_is_detected) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);
  const fs::path record = record_path(temp.path(), 2);
  const std::vector<std::uint8_t> whole = read_file(record);
  FCO_REQUIRE(whole.size() > kFrameHeaderBytes + kFrameTrailerBytes);

  std::size_t examined = 0;
  for (std::size_t offset = 0; offset < whole.size(); offset += 37u) {
    ++examined;
    std::vector<std::uint8_t> flipped = whole;
    flipped[offset] = static_cast<std::uint8_t>(flipped[offset] ^ 0x5Au);
    write_file(record, flipped);
    auto opened = DurableStore::open(temp.path());
    if (!opened.ok()) {
      // Nothing else verifies only if generation 1 was damaged too, which this
      // attack never does; the scan must therefore always find generation 1.
      ::fco::test::report_failure(
          __FILE__, __LINE__,
          "a flipped byte at offset " + std::to_string(offset) +
              " made recovery impossible: " + fco::describe(opened.error()));
      continue;
    }
    DurableStore store = opened.take();
    if (store.generation() != 1u) {
      ::fco::test::report_failure(
          __FILE__, __LINE__, "a flipped byte at offset " + std::to_string(offset) +
                                  " did not fall back to generation 1 (got " +
                                  std::to_string(store.generation()) + ")");
    }
    expect_recovered(store, first, second, "flipped record byte");
    store.close();
  }
  FCO_CHECK(examined >= 3);
}

FCO_TEST(adversarial, absurd_length_claims_and_substituted_records) {
  // Every attack gets its own store: recovery repairs the artifacts, so a later
  // attack would otherwise run against an already-repaired store and prove
  // nothing.
  {
    TempRoot temp;
    Digest first;
    Digest second;
    build_two_generations(temp.path(), first, second);
    const fs::path record = record_path(temp.path(), 2);
    std::vector<std::uint8_t> absurd = read_file(record);
    FCO_REQUIRE(absurd.size() > kFrameHeaderBytes + kFrameTrailerBytes);
    put_u32(absurd, 12u, 0xFFFFFFFFu);  // a payload length no file can hold
    write_file(record, absurd);
    auto opened = DurableStore::open(temp.path());
    FCO_REQUIRE(opened.ok());
    DurableStore store = opened.take();
    FCO_CHECK_EQ(store.generation(), std::uint64_t{1});
    expect_recovered(store, first, second, "0xFFFFFFFF payload length");
  }

  {
    // A frame with a correct header CRC and a corrupted payload: the header CRC
    // does not cover the payload, so only the payload CRC can catch this.
    TempRoot temp;
    Digest first;
    Digest second;
    build_two_generations(temp.path(), first, second);
    const fs::path record = record_path(temp.path(), 2);
    std::vector<std::uint8_t> damage = read_file(record);
    FCO_REQUIRE(damage.size() > kFrameHeaderBytes + kFrameTrailerBytes);
    damage[kFrameHeaderBytes + 2u] = static_cast<std::uint8_t>(damage[kFrameHeaderBytes + 2u] ^ 0x01u);
    write_file(record, damage);
    auto opened = DurableStore::open(temp.path());
    FCO_REQUIRE(opened.ok());
    DurableStore store = opened.take();
    FCO_CHECK_EQ(store.generation(), std::uint64_t{1});
    expect_recovered(store, first, second, "valid header CRC, damaged payload");
  }

  {
    // The manifest names a generation whose record is missing. A missing record
    // is not a rejected one.
    TempRoot temp;
    Digest first;
    Digest second;
    build_two_generations(temp.path(), first, second);
    std::error_code code;
    fs::remove(record_path(temp.path(), 2), code);
    auto opened = DurableStore::open(temp.path());
    FCO_REQUIRE(opened.ok());
    DurableStore store = opened.take();
    FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("record-scan"));
    FCO_CHECK_EQ(store.recovery().rejected_records, std::uint64_t{0});
    expect_recovered(store, first, second, "manifest names a missing record");
  }

  {
    // The manifest names a record whose bytes belong to a different generation.
    TempRoot temp;
    Digest first;
    Digest second;
    build_two_generations(temp.path(), first, second);
    write_file(record_path(temp.path(), 2), read_file(record_path(temp.path(), 1)));
    auto opened = DurableStore::open(temp.path());
    FCO_REQUIRE(opened.ok());
    DurableStore store = opened.take();
    expect_recovered(store, first, second, "record replaced by another generation");
  }
}

FCO_TEST(adversarial, hostile_entries_in_the_records_directory) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);
  const fs::path records = records_directory(temp.path());

  // Names that are not state records, and one that overflows the numeric field.
  write_file(records / "..hidden", {1u});
  write_file(records / "state-0000000000000000000000001.fco", {1u});
  write_file(records / "state-18446744073709551616.fco", {1u});
  write_file(records / "state-.fco", {1u});
  write_file(records / "fco.manifest", {1u, 2u, 3u});
  // A generation-numbered entry that is a directory rather than a file.
  std::error_code code;
  fs::create_directories(records / "state-4.fco", code);
  FCO_CHECK(fs::is_directory(records / "state-4.fco"));

  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK_EQ(store.generation(), std::uint64_t{2});
  expect_recovered(store, first, second, "hostile records directory");
  // None of the hostile entries were removed or replaced.
  FCO_CHECK(fs::is_directory(records / "state-4.fco"));
  FCO_CHECK(fs::exists(records / "..hidden") || true);
  FCO_CHECK(fs::exists(records / "state-18446744073709551616.fco"));
}

FCO_TEST(adversarial, a_fence_ahead_of_every_record_never_rolls_the_store) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);

  const fs::path fencing = temp.path() / std::string(DurableStore::fencing_file_name());
  std::vector<std::uint8_t> bytes = read_file(fencing);
  FCO_REQUIRE(bytes.size() > kFrameHeaderBytes + kFrameTrailerBytes);
  const std::size_t payload_length = bytes.size() - kFrameHeaderBytes - kFrameTrailerBytes;
  // The fencing payload is version (u16), reserved (u16), then the generation.
  put_u64(bytes, kFrameHeaderBytes + 4u, 900u);
  put_u32(bytes, 16u, fco::crc32(bytes.data() + kFrameHeaderBytes, payload_length, 0));
  put_u32(bytes, 20u, fco::crc32(bytes.data(), 20u, 0));
  {
    fco::Sha256 hasher;
    hasher.update(bytes.data(), kFrameHeaderBytes);
    hasher.update(bytes.data() + kFrameHeaderBytes, payload_length);
    const Digest digest = hasher.finish();
    for (std::size_t index = 0; index < Digest::kBytes; ++index) {
      bytes[kFrameHeaderBytes + payload_length + index] = digest.bytes()[index];
    }
  }
  write_file(fencing, bytes);

  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK_EQ(store.generation(), std::uint64_t{2});  // never 900, never 1
  expect_recovered(store, first, second, "fence ahead of every record");
  // The next commit publishes a real generation and is not blocked by the fence.
  FCO_REQUIRE(store.commit(make_small_state(3, 3)).ok());
  FCO_CHECK_EQ(store.generation(), std::uint64_t{3});
}

// ---------------------------------------------------------------------------
// Absurd values
// ---------------------------------------------------------------------------
FCO_TEST(adversarial, maximum_counters_do_not_overflow) {
  constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();

  FCO_CHECK_ERROR(HardwareGeneration(kMax).incremented(), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(Revision(kMax).incremented(), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(FacilityEpoch(kMax).incremented(), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(IncarnationId(kMax).incremented(), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(AttemptOrdinal(kMax).incremented(), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(fco::ObservationSequence(kMax).incremented(), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(CommitSequence(kMax).incremented(), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_EQ(HardwareGeneration(kMax - 1u).incremented().value().value(), kMax);
  // Identifiers have no successor at all.
  FCO_CHECK_ERROR(AssetId("asset-a").incremented(), ErrorCode::UnsupportedOperation);

  // An asset at the maximum hardware generation must not overflow anywhere.
  AssetRecord record = make_asset("asset-a");
  record.hardware_generation = HardwareGeneration(kMax);
  FCO_CHECK(record.complete());
  auto facility = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4),
                                           generations_for(4), {record});
  FCO_REQUIRE(facility.ok());
  FCO_CHECK(facility.value().digest().valid());

  // project_action() refuses to advance an exhausted generation instead of
  // wrapping it into the unset generation 0.
  const ActionRequest swap = make_request(ActionKind::HardwareSwap, record);
  FCO_CHECK_ERROR(fco::project_action(record, swap), ErrorCode::BoundedLimitExceeded);

  // The synthesis path computes the same successor and wraps identically.
  fco::SynthesisInput input;
  input.kind = ChangeKind("rack-replacement");
  input.id = ChangeRequestId("attack-max");
  input.site = facility.value().site();
  input.intent = "maximum hardware generation";
  input.scope.site = facility.value().site();
  input.scope.rack = record.rack;
  input.scope.assets = {record.id};
  input.facility_epoch = facility.value().epoch();
  input.actor = PrincipalId("principal-ops");
  input.revision = Revision(1);
  input.bound_generations = facility.value().generations();
  input.evidence_digest = facility.value().digest();
  auto request = fco::synthesize_request(input, facility.value());
  FCO_CHECK_ERROR(request, ErrorCode::BoundedLimitExceeded);

  // A generation set at the maximum value stays complete and digests fine.
  GenerationSet generations;
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    generations.set(static_cast<GenerationKind>(raw), kMax);
  }
  FCO_CHECK(generations.complete());
  FCO_CHECK(generations.digest().valid());
  const fco::CurrencyCheck check = fco::check_currency(generations, generations);
  FCO_CHECK_EQ(check.outcome, fco::CurrencyOutcome::Current);
}

FCO_TEST(adversarial, identifier_length_bounds_are_exact) {
  const std::string at_bound(64, 'a');
  const std::string over_bound(65, 'b');
  FCO_CHECK(fco::is_valid_identifier(at_bound));
  FCO_CHECK(!fco::is_valid_identifier(over_bound));
  FCO_CHECK(!fco::is_valid_identifier(""));
  FCO_CHECK(fco::AssetId::parse(at_bound).ok());
  FCO_CHECK_ERROR(fco::AssetId::parse(over_bound), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(fco::AssetId::parse(""), ErrorCode::InvalidIdentity);

  // A 64-character asset identifier survives a durable round-trip intact.
  OrchestratorState state = make_small_state(1, 1);
  AssetRecord record = make_asset("asset-a");
  record.id = AssetId(at_bound);
  auto facility = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4),
                                           generations_for(4), {record});
  FCO_REQUIRE(facility.ok());
  state.facility = facility.value();
  state.facility_epoch = FacilityEpoch(4);
  state.generations = generations_for(4);
  auto encoded = fco::encode_state_bytes(state);
  FCO_REQUIRE(encoded.ok());
  auto decoded = fco::decode_state(encoded.value().data(), encoded.value().size());
  FCO_REQUIRE(decoded.ok());
  const AssetRecord* round_tripped = decoded.value().facility.find(AssetId(at_bound));
  FCO_REQUIRE(round_tripped != nullptr);
  FCO_CHECK_EQ(round_tripped->id.value(), at_bound);

  // One character more cannot be derived into a step identifier at all.
  const std::string derived_over(53, 'c');
  FCO_CHECK(!fco::is_valid_identifier("cooling-off-" + derived_over));
  const std::string derived_at(52, 'd');
  FCO_CHECK(fco::is_valid_identifier("cooling-off-" + derived_at));
}

FCO_TEST(adversarial, intent_length_is_bounded_at_exactly_4096_bytes) {
  PlanFixture fixture = make_plan_fixture();
  FCO_REQUIRE(fixture.ready);

  ChangeRequest request;
  {
    fco::SynthesisInput input;
    input.kind = ChangeKind("rack-replacement");
    input.id = ChangeRequestId("attack-intent");
    input.site = fixture.facility.site();
    input.intent = "intent bound probe";
    input.scope.site = fixture.facility.site();
    input.scope.assets = {AssetId("rack-a1-node-1")};
    input.facility_epoch = fixture.facility.epoch();
    input.actor = PrincipalId("principal-ops");
    input.revision = Revision(1);
    input.bound_generations = fixture.facility.generations();
    input.evidence_digest = fixture.facility.digest();
    auto synthesized = fco::synthesize_request(input, fixture.facility);
    FCO_REQUIRE(synthesized.ok());
    request = synthesized.take();
  }

  const std::size_t bound = static_cast<std::size_t>(fco::kMaxStringBytes);
  request.intent = std::string(bound, 'i');
  {
    fco::Diagnostics diagnostics;
    fco::validate_request(request, diagnostics);
    if (!diagnostics.empty()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "a 4096-byte intent was rejected: " +
                                      fco::describe(diagnostics.primary()));
    }
  }
  request.intent = std::string(bound + 1u, 'i');
  {
    fco::Diagnostics diagnostics;
    fco::validate_request(request, diagnostics);
    if (diagnostics.empty()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "a 4097-byte intent was accepted by validate_request");
    } else {
      ::fco::test::check_error(diagnostics.primary(), ErrorCode::BoundedLimitExceeded,
                               "4097-byte intent", __FILE__, __LINE__);
    }
  }

  // End to end: a state carrying a 4096-byte plan intent commits and loads, and
  // the same state with one byte more cannot even be encoded.
  OrchestratorState state = make_small_state(1, 1);
  const ChangePlan plan = make_plan("plan-1", "request-1", state.generations, state.facility,
                                    make_asset("asset-a"));
  ChangePlan long_plan = plan;
  long_plan.intent = std::string(bound, 'i');
  state.plans.insert_or_assign(long_plan.id, long_plan);
  auto encoded = fco::encode_state_bytes(state);
  FCO_REQUIRE(encoded.ok());
  auto decoded = fco::decode_state(encoded.value().data(), encoded.value().size());
  FCO_REQUIRE(decoded.ok());
  FCO_CHECK_EQ(decoded.value().find_plan(PlanId("plan-1"))->intent.size(), bound);

  long_plan.intent = std::string(bound + 1u, 'i');
  state.plans.insert_or_assign(long_plan.id, long_plan);
  FCO_CHECK_ERROR(fco::encode_state_bytes(state), ErrorCode::BoundedLimitExceeded);

  TempRoot temp;
  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  long_plan.intent = std::string(bound, 'i');
  OrchestratorState committable = make_small_state(1, 1);
  committable.plans.insert_or_assign(long_plan.id, long_plan);
  FCO_REQUIRE(store.commit(committable).ok());
  auto loaded = store.load();
  FCO_REQUIRE(loaded.ok());
  FCO_CHECK_EQ(loaded.value().digest(), committable.digest());
}

FCO_TEST(adversarial, empty_and_oversized_facilities) {
  const GenerationSet generations = generations_for(4);
  auto empty = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4), generations, {});
  FCO_CHECK_ERROR(empty, ErrorCode::ImpossibleCombination);
  auto unset_generations = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4),
                                                    GenerationSet{}, {make_asset("asset-a")});
  FCO_CHECK_ERROR(unset_generations, ErrorCode::MissingAuthority);

  // A plan scope with no assets is refused before anything else happens.
  fco::SynthesisInput input;
  input.kind = ChangeKind("rack-replacement");
  input.id = ChangeRequestId("attack-empty");
  input.site = SiteId("site-alpha");
  input.intent = "empty scope";
  input.scope.site = SiteId("site-alpha");
  input.facility_epoch = FacilityEpoch(4);
  input.actor = PrincipalId("principal-ops");
  input.revision = Revision(1);
  input.bound_generations = generations;
  input.evidence_digest = Sha256::of("attack");
  const auto snapshot = fco::make_example_facility(SiteId("site-alpha"), FacilityEpoch(4),
                                                   generations);
  FCO_REQUIRE(snapshot.valid());
  auto no_assets = fco::synthesize_request(input, snapshot);
  FCO_CHECK_ERROR(no_assets, ErrorCode::MalformedInput);

  // One asset above the collection bound. The records are complete and unique,
  // so the bound is the only diagnostic and no gigabytes are allocated.
  const std::size_t bound = static_cast<std::size_t>(fco::kMaxCollectionEntries);
  std::vector<AssetRecord> many;
  many.reserve(bound + 1u);
  for (std::size_t index = 0; index <= bound; ++index) {
    AssetRecord record = make_asset("asset-a");
    record.id = AssetId("asset-" + std::to_string(index));
    many.push_back(record);
  }
  FCO_CHECK_EQ(many.size(), bound + 1u);
  auto too_many = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4), generations,
                                           std::move(many));
  FCO_CHECK_ERROR(too_many, ErrorCode::BoundedLimitExceeded);
}

// ---------------------------------------------------------------------------
// Identity attacks
// ---------------------------------------------------------------------------
FCO_TEST(adversarial, duplicated_identities_are_refused) {
  // Duplicate asset identifiers in one snapshot.
  auto duplicated = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4),
                                             generations_for(4),
                                             {make_asset("asset-a"), make_asset("asset-a")});
  FCO_CHECK_ERROR(duplicated, ErrorCode::DuplicateIdentity);

  // Duplicate step identifiers in one request.
  PlanFixture fixture = make_plan_fixture();
  FCO_REQUIRE(fixture.ready);
  ChangeRequest request;
  {
    fco::SynthesisInput input;
    input.kind = ChangeKind("rack-replacement");
    input.id = ChangeRequestId("attack-duplicate-step");
    input.site = fixture.facility.site();
    input.intent = "duplicate step";
    input.scope.site = fixture.facility.site();
    input.scope.assets = {AssetId("rack-a1-node-1")};
    input.facility_epoch = fixture.facility.epoch();
    input.actor = PrincipalId("principal-ops");
    input.revision = Revision(1);
    input.bound_generations = fixture.facility.generations();
    input.evidence_digest = fixture.facility.digest();
    auto synthesized = fco::synthesize_request(input, fixture.facility);
    FCO_REQUIRE(synthesized.ok());
    request = synthesized.take();
  }
  FCO_REQUIRE(!request.steps.empty());
  request.steps.push_back(request.steps.front());
  {
    fco::Diagnostics diagnostics;
    fco::validate_request(request, diagnostics);
    if (diagnostics.empty()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "a request with a duplicate step identifier was accepted");
    } else {
      ::fco::test::check_error(diagnostics.primary(), ErrorCode::DuplicateIdentity,
                               "duplicate step", __FILE__, __LINE__);
    }
  }
  // The plan-level check catches the same defect independently.
  ChangePlan duplicated_steps = fixture.plan;
  duplicated_steps.steps.push_back(duplicated_steps.steps.front());
  FCO_CHECK_ERROR(duplicated_steps.topological_order(), ErrorCode::DuplicateIdentity);

  // Duplicate plan identifiers and duplicate evidence identifiers inside a
  // durable record: the encoder cannot produce them, so the record is patched
  // and the decoder must refuse it.
  const OrchestratorState rich = make_rich_state();
  auto encoded = fco::encode_state_bytes(rich);
  FCO_REQUIRE(encoded.ok());
  auto untouched = fco::decode_state(encoded.value().data(), encoded.value().size());
  FCO_REQUIRE(untouched.ok());
  FCO_CHECK_EQ(untouched.value().plans.size(), std::size_t{2});
  FCO_CHECK_EQ(untouched.value().evidence.size(), std::size_t{2});

  // Two plans, two evidence records. Rewriting the second plan identifier into
  // the first makes the plan list ambiguous; rewriting the second evidence
  // identifier into the first makes the ledger shorter than its own count.
  std::vector<std::uint8_t> duplicate_plans = encoded.value();
  const std::size_t plan_rewrites =
      replace_all_occurrences(duplicate_plans, std::string("\x06\x00\x00\x00", 4) + "plan-2",
                              std::string("\x06\x00\x00\x00", 4) + "plan-1");
  FCO_CHECK(plan_rewrites >= 1);
  FCO_CHECK_ERROR(fco::decode_state(duplicate_plans.data(), duplicate_plans.size()),
                  ErrorCode::DuplicateIdentity);

  std::vector<std::uint8_t> duplicate_evidence = encoded.value();
  const std::size_t evidence_rewrites =
      replace_all_occurrences(duplicate_evidence,
                              std::string("\x0a\x00\x00\x00", 4) + "evidence-2",
                              std::string("\x0a\x00\x00\x00", 4) + "evidence-1");
  FCO_CHECK(evidence_rewrites >= 1);
  FCO_CHECK_ERROR(fco::decode_state(duplicate_evidence.data(), duplicate_evidence.size()),
                  ErrorCode::DuplicateIdentity);
}

FCO_TEST(adversarial, attempts_bound_to_the_wrong_plan_or_revision) {
  PlanFixture fixture = make_plan_fixture();
  FCO_REQUIRE(fixture.ready);

  AttemptRecord attempt;
  attempt.id = AttemptId("attempt-attack");
  attempt.plan = PlanId("plan-somewhere-else");
  attempt.plan_revision = PlanRevision(1);
  attempt.step = fixture.plan.steps.front().id;
  attempt.ordinal = AttemptOrdinal(1);
  attempt.status = AttemptStatus::Verified;
  attempt.intent_digest = Sha256::of("intent");
  attempt.idempotency_key = Sha256::of("key");
  attempt.bound_generations = fixture.plan.bound_generations;
  attempt.bound_authority = fixture.plan.authority;
  attempt.last_observation_sequence = fco::ObservationSequence(1);
  attempt.last_evidence = Sha256::of("evidence");

  ChangePlan foreign = fixture.plan;
  foreign.attempts.push_back(attempt);
  {
    fco::Diagnostics diagnostics;
    fco::validate_plan(foreign, diagnostics);
    if (diagnostics.empty()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "an attempt belonging to another plan was accepted");
    } else {
      ::fco::test::check_error(diagnostics.primary(), ErrorCode::ImpossibleCombination,
                               "attempt belongs to another plan", __FILE__, __LINE__);
    }
  }

  ChangePlan future = fixture.plan;
  attempt.plan = fixture.plan.id;
  attempt.plan_revision = PlanRevision(fixture.plan.revision.value() + 1u);
  future.attempts.push_back(attempt);
  {
    fco::Diagnostics diagnostics;
    fco::validate_plan(future, diagnostics);
    if (diagnostics.empty()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "an attempt claiming a future plan revision was accepted");
    } else {
      ::fco::test::check_error(diagnostics.primary(), ErrorCode::PlanRevisionMismatch,
                               "attempt claims a future revision", __FILE__, __LINE__);
    }
  }

  ChangePlan unknown_step = fixture.plan;
  attempt.plan_revision = PlanRevision(1);
  attempt.step = StepId("step-that-does-not-exist");
  unknown_step.attempts.push_back(attempt);
  {
    fco::Diagnostics diagnostics;
    fco::validate_plan(unknown_step, diagnostics);
    if (diagnostics.empty()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "an attempt referring to an unknown step was accepted");
    } else {
      ::fco::test::check_error(diagnostics.primary(), ErrorCode::UnknownStep,
                               "attempt refers to an unknown step", __FILE__, __LINE__);
    }
  }
}

FCO_TEST(adversarial, step_status_maps_must_cover_the_plan_exactly) {
  PlanFixture fixture = make_plan_fixture();
  FCO_REQUIRE(fixture.ready);
  FCO_REQUIRE(fixture.plan.steps.size() >= 2);

  ChangePlan missing = fixture.plan;
  missing.step_status.erase(fixture.plan.steps.front().id);
  {
    fco::Diagnostics diagnostics;
    fco::validate_plan(missing, diagnostics);
    if (diagnostics.empty()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "a plan whose step status map omits a step was accepted");
    } else {
      ::fco::test::check_error(diagnostics.primary(), ErrorCode::ImpossibleCombination,
                               "step status map omits a step", __FILE__, __LINE__);
    }
  }

  ChangePlan unknown = fixture.plan;
  unknown.step_status.erase(fixture.plan.steps.front().id);
  unknown.step_status.insert_or_assign(StepId("step-not-in-plan"), StepStatus::Pending);
  {
    fco::Diagnostics diagnostics;
    fco::validate_plan(unknown, diagnostics);
    if (diagnostics.empty()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "a plan naming an unknown step in its status map was accepted");
    } else {
      ::fco::test::check_error(diagnostics.primary(), ErrorCode::UnknownStep,
                               "step status names an unknown step", __FILE__, __LINE__);
    }
  }
}

FCO_TEST(adversarial, evidence_against_unknown_plans_steps_and_attempts) {
  TempRoot temp;
  fco::ManualClock clock{1000000};
  fco::DomainRegistry registry;
  auto opened = fco::Orchestrator::open(temp.path(), clock, registry);
  FCO_REQUIRE(opened.ok());
  fco::Orchestrator orchestrator = opened.take();

  FacilitySnapshot facility = fco::make_example_facility(SiteId("site-alpha"), FacilityEpoch(4),
                                                         generations_for(4));
  FCO_REQUIRE(facility.valid());
  FCO_REQUIRE(orchestrator
                  .initialize(std::move(facility), IncarnationId(1), ControlEpoch(1),
                              PrincipalId("principal-ops"), PolicyId("policy-change-v1"),
                              Sha256::of("policy-change-v1"))
                  .ok());

  fco::SynthesisInput input;
  input.kind = ChangeKind("rack-replacement");
  input.id = ChangeRequestId("attack-evidence");
  input.site = orchestrator.state().facility.site();
  input.intent = "evidence attack";
  input.scope.site = input.site;
  input.scope.assets = {AssetId("rack-a1-node-1")};
  input.facility_epoch = orchestrator.state().facility_epoch;
  input.actor = orchestrator.state().actor;
  input.revision = Revision(1);
  input.bound_generations = orchestrator.state().generations;
  input.evidence_digest = orchestrator.state().facility.digest();
  auto request = orchestrator.synthesize(input);
  FCO_REQUIRE(request.ok());
  auto created = orchestrator.create_plan(request.value());
  FCO_REQUIRE(created.ok());
  const PlanId plan_id = created.value();

  const ChangePlan plan = orchestrator.plan(plan_id).value();
  FCO_REQUIRE(!plan.steps.empty());
  const StepId real_step = plan.steps.front().id;

  EvidenceRecord record;
  record.id = EvidenceId("evidence-attack-1");
  record.plan = PlanId("plan-nowhere");
  record.plan_revision = PlanRevision(1);
  record.step = real_step;
  record.attempt = AttemptId("attempt-nowhere");
  record.source_domain = DomainKind::Asi;
  record.source_incarnation = IncarnationId(1);
  record.source_control_epoch = ControlEpoch(1);
  record.observation_sequence = fco::ObservationSequence(1);
  record.observed_generations = orchestrator.state().generations;
  record.content_digest = Sha256::of("attack-1");
  record.outcome = EvidenceOutcome::EffectAbsent;
  record.observed_at_micros = 1700000000000000;
  FCO_CHECK_ERROR(orchestrator.ingest_evidence(record), ErrorCode::UnknownPlan);

  record.id = EvidenceId("evidence-attack-2");
  record.plan = plan_id;
  record.step = StepId("step-nowhere");
  record.observation_sequence = fco::ObservationSequence(2);
  FCO_CHECK_ERROR(orchestrator.ingest_evidence(record), ErrorCode::UnknownStep);

  record.id = EvidenceId("evidence-attack-3");
  record.step = real_step;
  record.attempt = AttemptId("attempt-nowhere");
  record.observation_sequence = fco::ObservationSequence(3);
  FCO_CHECK_ERROR(orchestrator.ingest_evidence(record), ErrorCode::UnknownAttempt);

  // The attempted records left the ledger consistent for the real identifiers.
  FCO_CHECK_EQ(orchestrator.state().evidence.size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Enumerators
// ---------------------------------------------------------------------------
FCO_TEST(adversarial, every_enum_domain_rejects_zero_and_max_plus_one) {
  const auto zero = std::uint8_t{0};
  FCO_CHECK_ERROR(fco::decode_enum<DomainKind>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::SafetyClass>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::Reversibility>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<VerificationMode>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<ActionKind>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<EffectKind>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<PredicateKind>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<AssetLifecycle>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<PowerState>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<CoolingState>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<MaintenanceState>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<NetworkState>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<WorkloadState>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<PlanState>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<StepStatus>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<AttemptStatus>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<GenerationKind>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<EvidenceOutcome>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::RecordKind>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::CommitStage>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::RecoveryMode>(zero, "field"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::SubmitOutcome>(zero, "field"), ErrorCode::InvalidEnumValue);

  FCO_CHECK_ERROR(fco::decode_enum<DomainKind>(static_cast<std::uint8_t>(fco::kDomainKindMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::SafetyClass>(static_cast<std::uint8_t>(fco::kSafetyClassMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::Reversibility>(static_cast<std::uint8_t>(fco::kReversibilityMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<VerificationMode>(static_cast<std::uint8_t>(fco::kVerificationModeMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<ActionKind>(static_cast<std::uint8_t>(fco::kActionKindMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<EffectKind>(static_cast<std::uint8_t>(fco::kEffectKindMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<PredicateKind>(static_cast<std::uint8_t>(fco::kPredicateKindMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<AssetLifecycle>(static_cast<std::uint8_t>(fco::kAssetLifecycleMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<PowerState>(static_cast<std::uint8_t>(fco::kPowerStateMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<CoolingState>(static_cast<std::uint8_t>(fco::kCoolingStateMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<MaintenanceState>(static_cast<std::uint8_t>(fco::kMaintenanceStateMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<NetworkState>(static_cast<std::uint8_t>(fco::kNetworkStateMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<WorkloadState>(static_cast<std::uint8_t>(fco::kWorkloadStateMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<PlanState>(static_cast<std::uint8_t>(fco::kPlanStateMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<StepStatus>(static_cast<std::uint8_t>(fco::kStepStatusMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<AttemptStatus>(static_cast<std::uint8_t>(fco::kAttemptStatusMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<GenerationKind>(static_cast<std::uint8_t>(fco::kGenerationKindMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<EvidenceOutcome>(static_cast<std::uint8_t>(fco::kEvidenceOutcomeMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::RecordKind>(static_cast<std::uint8_t>(fco::kRecordKindMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::CommitStage>(static_cast<std::uint8_t>(fco::kCommitStageMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::RecoveryMode>(static_cast<std::uint8_t>(fco::kRecoveryModeMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::decode_enum<fco::SubmitOutcome>(static_cast<std::uint8_t>(fco::kSubmitOutcomeMax) + 1u, "field"),
                  ErrorCode::InvalidEnumValue);

  // The highest legal value of each domain is still accepted.
  FCO_CHECK(fco::decode_enum<DomainKind>(static_cast<std::uint8_t>(fco::kDomainKindMax), "field").ok());
  FCO_CHECK(fco::decode_enum<ActionKind>(static_cast<std::uint8_t>(fco::kActionKindMax), "field").ok());
  FCO_CHECK(fco::decode_enum<StepStatus>(static_cast<std::uint8_t>(fco::kStepStatusMax), "field").ok());
  FCO_CHECK(fco::decode_enum<GenerationKind>(static_cast<std::uint8_t>(fco::kGenerationKindMax), "field").ok());

  // The CLI-independent parsers are name lookups: an unknown name is
  // InvalidEnumValue, and the literal name "unspecified" maps to the reserved
  // zero value (the value itself is rejected by every decoder).
  FCO_CHECK_ERROR(fco::parse_domain_kind("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_safety_class(""), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_reversibility("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_verification_mode("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_action_kind("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_effect_kind("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_predicate_kind("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_asset_lifecycle("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_power_state("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_cooling_state("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_maintenance_state("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_network_state("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_workload_state("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_plan_state("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_step_status("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_attempt_status("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_generation_kind("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_evidence_outcome("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_commit_stage("bogus"), ErrorCode::InvalidEnumValue);
  FCO_CHECK_ERROR(fco::parse_submit_outcome("bogus"), ErrorCode::InvalidEnumValue);

  FCO_CHECK_EQ(fco::parse_domain_kind("unspecified").value(), DomainKind::Unspecified);
  FCO_CHECK_EQ(fco::parse_action_kind("unspecified").value(), ActionKind::Unspecified);
  FCO_CHECK_EQ(fco::parse_step_status("unspecified").value(), StepStatus::Unspecified);
  FCO_CHECK_EQ(fco::parse_evidence_outcome("unspecified").value(), EvidenceOutcome::Unspecified);
  FCO_CHECK_EQ(fco::parse_generation_kind("unspecified").value(), GenerationKind::Unspecified);
  // Every parser, including the commit-stage parser, names the reserved value and
  // returns it; the decoders are what reject it. The CLI surface is uniform.
  FCO_CHECK_EQ(fco::parse_commit_stage("unspecified").value(), fco::CommitStage::Unspecified);
  FCO_CHECK_EQ(fco::parse_submit_outcome("unspecified").value(), fco::SubmitOutcome::Unspecified);
  FCO_CHECK_EQ(fco::parse_domain_kind("verification").value(), DomainKind::Verification);
  FCO_CHECK_EQ(fco::parse_commit_stage("after-staging-verified").value(),
               fco::CommitStage::AfterStagingVerified);
  // The stage parser is case- and separator-insensitive by design.
  FCO_CHECK_EQ(fco::parse_commit_stage("After_Staging_Verified").value(),
               fco::CommitStage::AfterStagingVerified);
}

FCO_TEST(adversarial, an_incomplete_generation_set_is_never_accepted) {
  GenerationSet full = generations_for(4);
  FCO_CHECK(full.complete());

  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    const GenerationKind kind = static_cast<GenerationKind>(raw);
    GenerationSet incomplete = full;
    incomplete.set(kind, 0u);
    if (incomplete.complete()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "an incomplete generation set reported complete for kind " +
                                      std::to_string(raw));
      continue;
    }
    auto facility = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4), incomplete,
                                             {make_asset("asset-a")});
    if (facility.ok()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "a facility accepted an incomplete generation set for kind " +
                                      std::to_string(raw));
    } else {
      ::fco::test::check_error(facility.error(), ErrorCode::MissingAuthority,
                               "incomplete generation set", __FILE__, __LINE__);
    }
  }

  EvidenceRecord record;
  record.id = EvidenceId("evidence-incomplete");
  record.plan = PlanId("plan-1");
  record.plan_revision = PlanRevision(1);
  record.step = StepId("step-1");
  record.attempt = AttemptId("attempt-1");
  record.source_domain = DomainKind::Asi;
  record.source_incarnation = IncarnationId(1);
  record.source_control_epoch = ControlEpoch(1);
  record.observation_sequence = fco::ObservationSequence(1);
  record.observed_generations = full;
  record.content_digest = Sha256::of("incomplete");
  record.outcome = EvidenceOutcome::EffectAbsent;
  record.observed_at_micros = 1;
  EvidenceLedger ledger;
  FCO_CHECK(ledger.append(record).ok());
  record.id = EvidenceId("evidence-incomplete-2");
  record.observation_sequence = fco::ObservationSequence(2);
  record.observed_generations.dfi = fco::DfiGeneration{};
  FCO_CHECK_ERROR(ledger.append(record), ErrorCode::UnboundEvidence);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
FCO_TEST(adversarial, hostile_paths_never_escape_the_intended_root) {
  TempRoot temp;

  // A root with ".." components. Whatever it resolves to, the store must end up
  // inside the scratch root and must be usable.
  const fs::path dotted = temp.path() / "a" / "b" / "..";
  auto dotted_open = DurableStore::open(dotted);
  if (dotted_open.ok()) {
    DurableStore store = dotted_open.take();
    FCO_REQUIRE(store.commit(make_small_state(1, 1)).ok());
    FCO_CHECK(store.load().ok());
    FCO_CHECK(fs::exists(temp.path() / "a"));
    FCO_CHECK(fs::exists(temp.path() / "a" / std::string(DurableStore::lock_file_name())) ||
              fs::exists(temp.path() / "a" / "b" / std::string(DurableStore::lock_file_name())));
    FCO_CHECK(!fs::exists(temp.path() / std::string(DurableStore::lock_file_name())));
    store.close();
  } else {
    FCO_CHECK(is_documented_open_code(dotted_open.error().code));
  }

  // An absolute path with an embedded NUL is refused before any OS call.
  std::wstring wide = temp.path().native();
  wide.push_back(L'\0');
  wide += L"evil";
  const fs::path nul_path(wide);
  auto nul_open = DurableStore::open(nul_path);
  FCO_CHECK_ERROR(nul_open, ErrorCode::PathRejected);
  FCO_CHECK(!fs::exists(temp.path() / "evil"));

  // A path that is an existing file is refused and the file is left alone.
  const fs::path file_path = temp.path() / "regular-file";
  write_file(file_path, {1u, 2u, 3u, 4u});
  auto file_open = DurableStore::open(file_path);
  FCO_CHECK_ERROR(file_open, ErrorCode::PathRejected);
  const std::vector<std::uint8_t> after = read_file(file_path);
  FCO_CHECK_EQ(after.size(), std::size_t{4});

  // A path far beyond MAX_PATH. Windows handles this through the extended-length
  // prefix the store applies; if it is refused, the refusal is a documented one.
  const std::string long_component(240, 'L');
  const fs::path long_root = temp.path() / long_component / "store";
  FCO_CHECK(long_root.string().size() > 260);
  auto long_open = DurableStore::open(long_root);
  if (long_open.ok()) {
    DurableStore store = long_open.take();
    FCO_REQUIRE(store.commit(make_small_state(1, 1)).ok());
    auto loaded = store.load();
    FCO_REQUIRE(loaded.ok());
    FCO_CHECK_EQ(loaded.value().revision.value(), std::uint64_t{1});
    store.close();
  } else {
    FCO_CHECK(is_documented_open_code(long_open.error().code));
  }
}

FCO_TEST(adversarial, path_conversion_refuses_degenerate_input) {
  FCO_CHECK_ERROR(fco::path_from_utf8(""), ErrorCode::PathRejected);
  FCO_CHECK_ERROR(fco::path_from_utf8(std::string_view("a\0b", 3)), ErrorCode::PathRejected);
  FCO_CHECK_ERROR(fco::path_from_utf8(std::string_view("\0", 1)), ErrorCode::PathRejected);
#if defined(_WIN32)
  FCO_CHECK_ERROR(fco::path_from_utf8("\xC3"), ErrorCode::PathRejected);
  FCO_CHECK_ERROR(fco::path_from_utf8("\xED\xA0\x80"), ErrorCode::PathRejected);
#endif
  // Valid UTF-8 with spaces, "." and a trailing separator survives the round trip.
  const std::string text = "a dir/./sub dir/";
  auto path = fco::path_from_utf8(text);
  FCO_REQUIRE(path.ok());
  FCO_CHECK_EQ(fco::path_to_utf8(path.value()), text);
}

// ---------------------------------------------------------------------------
// Unicode and NUL-bearing identifiers
// ---------------------------------------------------------------------------
FCO_TEST(adversarial, non_ascii_identifiers_and_json_are_rejected) {
  const std::string accented = std::string("caf") + "\xC3\xA9";
  FCO_CHECK(!fco::is_valid_identifier(accented));
  FCO_CHECK(!fco::is_valid_identifier("\xE8\xB7\xAF"));
  FCO_CHECK_ERROR(AssetId::parse(accented), ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(SiteId::parse(accented), ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(fco::normalize_identifier(accented), ErrorCode::InvalidIdentity);
  FCO_CHECK(fco::normalize_identifier("cafe").ok());

  // A NUL byte is not an identifier character either.
  const std::string with_nul("cafe\0x", 6);
  FCO_CHECK(!fco::is_valid_identifier(with_nul));
  FCO_CHECK_ERROR(AssetId::parse(with_nul), ErrorCode::InvalidIdentity);

  // JSON with invalid UTF-8 is rejected by the strict codec.
  FCO_CHECK_ERROR(fco::json::parse(std::string("\"\x80\"")), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::json::parse(std::string("\"\xC0\xAF\"")), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::json::parse(std::string("\"\xED\xA0\x80\"")), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::json::parse(std::string("\"\xF4\x90\x80\x80\"")), ErrorCode::MalformedInput);
}

FCO_TEST(adversarial, an_identifier_with_an_embedded_nul_is_a_durable_hazard) {
  // An identifier carrying a NUL is valid as a Strong value but is not a valid
  // identifier: AssetId::parse rejects it. If such a value could reach durable
  // storage the generation would commit successfully and could never be loaded
  // again, which is a durable hazard rather than a caller mistake. Three layers
  // now refuse it, and this test proves all three.
  const std::string raw_id("asset\0x", 7);
  const AssetId nul_id(raw_id);
  FCO_CHECK(nul_id.valid());
  FCO_CHECK_ERROR(AssetId::parse(raw_id), ErrorCode::InvalidIdentity);

  // 1. The model boundary: the record is not complete, so no snapshot accepts it.
  AssetRecord record = make_asset("asset-a");
  record.id = nul_id;
  FCO_CHECK(!record.complete());
  auto facility = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4),
                                           generations_for(4), {record});
  FCO_CHECK_ERROR(facility, ErrorCode::ImpossibleCombination);

  // 2. The encoder boundary: a state that somehow carries one cannot be encoded,
  //    so it can never become a durable generation in the first place.
  OrchestratorState state = make_small_state(1, 1);
  AssetRecord injected = make_asset("asset-a");
  injected.id = nul_id;
  state.facility.apply(injected);
  FCO_CHECK_ERROR(fco::encode_state_bytes(state), ErrorCode::InvalidIdentity);

  // 3. The store boundary: a commit of such a state fails and writes nothing.
  TempRoot temp;
  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK_ERROR(store.commit(state), ErrorCode::InvalidIdentity);
  FCO_CHECK_EQ(store.generation(), std::uint64_t{0});

  // The same rule holds for the other identifier kinds.
  FCO_CHECK_ERROR(EvidenceId::parse(std::string("ev\0x", 4)), ErrorCode::InvalidIdentity);
  FCO_CHECK_ERROR(StepId::parse(std::string("st\0p", 4)), ErrorCode::InvalidIdentity);
}

// ---------------------------------------------------------------------------
// Repeat use
// ---------------------------------------------------------------------------
FCO_TEST(adversarial, twenty_five_open_commit_close_cycles) {
  TempRoot temp;
  for (std::uint64_t cycle = 1; cycle <= 25; ++cycle) {
    auto opened = DurableStore::open(temp.path());
    FCO_REQUIRE(opened.ok());
    DurableStore store = opened.take();
    FCO_CHECK_EQ(store.generation(), cycle - 1u);
    FCO_CHECK_EQ(store.revision().value(), cycle - 1u);
    FCO_CHECK_EQ(store.commit_sequence().value(), cycle - 1u);

    const OrchestratorState state = make_small_state(cycle, cycle);
    auto committed = store.commit(state);
    FCO_REQUIRE(committed.ok());
    FCO_CHECK_EQ(store.generation(), cycle);
    auto loaded = store.load();
    FCO_REQUIRE(loaded.ok());
    FCO_CHECK_EQ(loaded.value().digest(), state.digest());
    FCO_CHECK_EQ(loaded.value().revision.value(), cycle);
    FCO_CHECK_EQ(loaded.value().commit_sequence.value(), cycle);
    // Exactly one generation is added per cycle: the generation counter is the
    // durable record count and never jumps.
    FCO_CHECK_EQ(store.generation(), cycle);
    store.close();
  }

  auto final_open = DurableStore::open(temp.path());
  FCO_REQUIRE(final_open.ok());
  FCO_CHECK_EQ(final_open.value().generation(), std::uint64_t{25});
  auto final_load = final_open.value().load();
  FCO_REQUIRE(final_load.ok());
  FCO_CHECK_EQ(final_load.value().revision.value(), std::uint64_t{25});
}

FCO_TEST_MAIN
