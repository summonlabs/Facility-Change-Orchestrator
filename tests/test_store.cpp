// Durable store tests.
//
// These drive the real DurableStore against a real filesystem: publication
// order, strict monotonicity, the observable commit-stage sequence, recovery
// from damaged artifacts, retention, the record frame codec, and the UTF-8 path
// conversion. Corruption cases rebuild a two-generation store from scratch so
// that no case can be explained by the previous one.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "fco/codec.hpp"
#include "fco/digest.hpp"
#include "fco/error.hpp"
#include "fco/store.hpp"
#include "fco/version.hpp"
#include "test_support.hpp"

namespace {

namespace fs = std::filesystem;

using fco::AssetId;
using fco::AssetLifecycle;
using fco::AssetRecord;
using fco::CommitHooks;
using fco::CommitSequence;
using fco::CommitStage;
using fco::CoolingState;
using fco::ControlEpoch;
using fco::Digest;
using fco::DurableStore;
using fco::ErrorCode;
using fco::FacilityEpoch;
using fco::FacilityRevision;
using fco::FacilitySnapshot;
using fco::FirmwareGeneration;
using fco::GenerationKind;
using fco::GenerationSet;
using fco::HardwareGeneration;
using fco::IncarnationId;
using fco::LifecycleGeneration;
using fco::MaintenanceGeneration;
using fco::MaintenanceState;
using fco::NetworkState;
using fco::OrchestratorState;
using fco::PolicyId;
using fco::PowerState;
using fco::PrincipalId;
using fco::RecordFrame;
using fco::RecordKind;
using fco::RecoveryMode;
using fco::Revision;
using fco::Sha256;
using fco::SiteId;
using fco::TenantId;
using fco::WorkloadState;

constexpr std::size_t kFrameHeaderBytes = 24;
constexpr std::size_t kFrameTrailerBytes = 32;

class TempRoot {
 public:
  TempRoot() {
    static unsigned counter = 0;
    ++counter;
    std::ostringstream name;
    name << "fco-store-" << counter << '-'
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

constexpr std::uint32_t kGenerationCount = static_cast<std::uint32_t>(fco::kGenerationKindMax);

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

[[nodiscard]] OrchestratorState make_state(std::uint64_t revision, std::uint64_t sequence) {
  const GenerationSet generations = generations_for(4);
  auto facility = FacilitySnapshot::create(SiteId("site-alpha"), FacilityEpoch(4), generations,
                                           {make_asset("asset-a")});
  ::fco::test::check_true(facility.ok(), "fixture facility", __FILE__, __LINE__);
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

void store_u32_at(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4u; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>((value >> (8u * index)) & 0xFFu);
  }
}

void store_u16_at(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

[[nodiscard]] std::size_t count_regular_files(const fs::path& directory) {
  std::size_t count = 0;
  std::error_code code;
  fs::directory_iterator it(directory, code);
  if (code) return 0;
  const fs::directory_iterator end;
  for (; it != end; it.increment(code)) {
    if (code) break;
    if (it->is_regular_file()) ++count;
  }
  return count;
}

[[nodiscard]] fs::path records_directory(const fs::path& root) {
  return root / std::string(DurableStore::records_directory_name());
}

[[nodiscard]] fs::path record_path(const fs::path& root, std::uint64_t generation) {
  return records_directory(root) / DurableStore::state_record_name(generation);
}

[[nodiscard]] std::string mode_name(RecoveryMode mode) {
  return std::string(fco::to_string(mode));
}

// Builds a store with exactly two committed generations and closes it. The two
// state digests identify everything recovery is allowed to return.
void build_two_generations(const fs::path& root, Digest& first, Digest& second) {
  auto opened = DurableStore::open(root);
  ::fco::test::check_true(opened.ok(), "fixture store open", __FILE__, __LINE__);
  if (!opened.ok()) return;
  DurableStore store = opened.take();
  const OrchestratorState one = make_state(1, 1);
  const OrchestratorState two = make_state(2, 2);
  first = one.digest();
  second = two.digest();
  const auto first_commit = store.commit(one);
  ::fco::test::check_true(first_commit.ok(), "fixture first commit", __FILE__, __LINE__);
  const auto second_commit = store.commit(two);
  ::fco::test::check_true(second_commit.ok(), "fixture second commit", __FILE__, __LINE__);
  store.close();
}

// A recovered store must never hand back a state that fails to decode, and the
// state it does hand back must be one of the generations that was actually
// committed.
void expect_recovered_state(const DurableStore& store, const Digest& first, const Digest& second,
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
                                std::string(label) + ": load failed after recovery: " +
                                    fco::describe(loaded.error()));
    return;
  }
  const Digest digest = loaded.value().digest();
  if (digest != first && digest != second) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string(label) +
                                    ": recovered a state that was never committed");
    return;
  }
  auto reencoded = fco::encode_state_bytes(loaded.value());
  if (!reencoded.ok()) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string(label) + ": recovered a state that does not "
                                    "re-encode: " + fco::describe(reencoded.error()));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Commit, load, and monotonicity
// ---------------------------------------------------------------------------
FCO_TEST(store, open_creates_the_root_and_commits_load_back) {
  TempRoot temp;
  const fs::path nested = temp.path() / "deep" / "nested" / "store";
  FCO_CHECK(!fs::exists(nested));
  auto opened = DurableStore::open(nested);
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();

  FCO_CHECK(store.is_open());
  FCO_CHECK(store.empty_store());
  FCO_CHECK_EQ(store.generation(), std::uint64_t{0});
  FCO_CHECK_EQ(store.revision().value(), std::uint64_t{0});
  FCO_CHECK_EQ(store.commit_sequence().value(), std::uint64_t{0});
  FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("fresh-store"));
  FCO_CHECK(store.recovery().store_created);
  FCO_CHECK(!store.recovery().manifest_repaired);
  FCO_CHECK(!store.recovery().fencing_repaired);
  FCO_CHECK(fs::is_directory(nested));
  FCO_CHECK(fs::is_directory(nested / std::string(DurableStore::records_directory_name())));
  FCO_CHECK(fs::is_directory(nested / std::string(DurableStore::staging_directory_name())));
  FCO_CHECK(fs::exists(nested / std::string(DurableStore::lock_file_name())));
  // An empty store has no committed generation to load.
  FCO_CHECK_ERROR(store.load(), ErrorCode::RecordNotFound);

  const OrchestratorState first = make_state(1, 1);
  auto first_revision = store.commit(first);
  FCO_REQUIRE(first_revision.ok());
  FCO_CHECK_EQ(first_revision.value().value(), std::uint64_t{1});
  FCO_CHECK_EQ(store.revision().value(), std::uint64_t{1});
  FCO_CHECK_EQ(store.commit_sequence().value(), std::uint64_t{1});
  FCO_CHECK_EQ(store.generation(), std::uint64_t{1});
  FCO_CHECK(!store.empty_store());
  FCO_CHECK(store.fencing_digest().valid());

  auto loaded = store.load();
  FCO_REQUIRE(loaded.ok());
  FCO_CHECK_EQ(loaded.value().digest(), first.digest());
  FCO_CHECK_EQ(loaded.value().revision.value(), std::uint64_t{1});
  FCO_CHECK_EQ(loaded.value().commit_sequence.value(), std::uint64_t{1});
  FCO_CHECK_EQ(loaded.value().facility.digest(), first.facility.digest());

  // Both counters and the generation advance by exactly one per commit.
  const OrchestratorState second = make_state(2, 2);
  FCO_REQUIRE(store.commit(second).ok());
  FCO_CHECK_EQ(store.revision().value(), std::uint64_t{2});
  FCO_CHECK_EQ(store.commit_sequence().value(), std::uint64_t{2});
  FCO_CHECK_EQ(store.generation(), std::uint64_t{2});

  // The accessors agree with the durable manifest, which is what a fresh open
  // recovers from.
  store.close();
  FCO_CHECK(!store.is_open());
  FCO_CHECK_ERROR(store.load(), ErrorCode::StorageFailure);

  auto reopened = DurableStore::open(nested);
  FCO_REQUIRE(reopened.ok());
  FCO_CHECK_EQ(mode_name(reopened.value().recovery().mode), std::string("manifest-authoritative"));
  FCO_CHECK_EQ(reopened.value().recovery().generation, std::uint64_t{2});
  FCO_CHECK_EQ(reopened.value().generation(), std::uint64_t{2});
  FCO_CHECK_EQ(reopened.value().revision().value(), std::uint64_t{2});
  FCO_CHECK_EQ(reopened.value().commit_sequence().value(), std::uint64_t{2});
  FCO_CHECK(reopened.value().fencing_digest().valid());
  auto reloaded = reopened.value().load();
  FCO_REQUIRE(reloaded.ok());
  FCO_CHECK_EQ(reloaded.value().digest(), second.digest());
  FCO_CHECK_EQ(reloaded.value().revision.value(), std::uint64_t{2});
  FCO_CHECK_EQ(reloaded.value().commit_sequence.value(), std::uint64_t{2});
  FCO_CHECK_EQ(reloaded.value().facility.digest(), second.facility.digest());
}

FCO_TEST(store, commit_requires_strict_monotonicity_and_writes_nothing) {
  TempRoot temp;
  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();

  const OrchestratorState first = make_state(1, 1);
  FCO_REQUIRE(store.commit(first).ok());
  const Digest first_digest = first.digest();

  OrchestratorState jumped = make_state(3, 2);  // revision + 2
  FCO_CHECK_ERROR(store.commit(jumped), ErrorCode::GenerationRegression);
  FCO_CHECK_EQ(store.generation(), std::uint64_t{1});

  OrchestratorState repeated = make_state(1, 2);  // revision + 0
  FCO_CHECK_ERROR(store.commit(repeated), ErrorCode::GenerationRegression);
  FCO_CHECK_EQ(store.generation(), std::uint64_t{1});

  OrchestratorState wrong_sequence = make_state(2, 5);  // revision fine, sequence not
  FCO_CHECK_ERROR(store.commit(wrong_sequence), ErrorCode::GenerationRegression);
  FCO_CHECK_EQ(store.generation(), std::uint64_t{1});

  OrchestratorState backward_sequence = make_state(2, 1);
  FCO_CHECK_ERROR(store.commit(backward_sequence), ErrorCode::GenerationRegression);
  FCO_CHECK_EQ(store.generation(), std::uint64_t{1});

  // An unset state is refused before any monotonicity reasoning happens.
  OrchestratorState unset;
  FCO_CHECK_ERROR(store.commit(unset), ErrorCode::CommitFailure);
  FCO_CHECK_EQ(store.generation(), std::uint64_t{1});

  // Nothing was written: the previous generation is still the only one, and it
  // still loads with its original digest.
  FCO_CHECK_EQ(count_regular_files(records_directory(temp.path())), std::size_t{1});
  auto loaded = store.load();
  FCO_REQUIRE(loaded.ok());
  FCO_CHECK_EQ(loaded.value().digest(), first_digest);
  FCO_CHECK_EQ(store.revision().value(), std::uint64_t{1});
  FCO_CHECK_EQ(store.commit_sequence().value(), std::uint64_t{1});

  // ... and a correctly sequenced commit still succeeds afterwards.
  FCO_REQUIRE(store.commit(make_state(2, 2)).ok());
  FCO_CHECK_EQ(store.generation(), std::uint64_t{2});
}

FCO_TEST(store, commit_hooks_observe_the_documented_stage_order) {
  TempRoot temp;
  std::vector<CommitStage> stages;
  CommitHooks hooks;
  hooks.on_stage = [&stages](CommitStage stage) { stages.push_back(stage); };

  auto opened = DurableStore::open(temp.path(), hooks);
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_REQUIRE(store.commit(make_state(1, 1)).ok());

  const std::vector<CommitStage> expected = {
      CommitStage::BeforeStagingWrite,      CommitStage::AfterStagingWritten,
      CommitStage::AfterStagingFlushed,     CommitStage::AfterStagingVerified,
      CommitStage::AfterRecordPublished,    CommitStage::BeforeManifestWrite,
      CommitStage::AfterManifestFlushed,    CommitStage::AfterManifestVerified,
      CommitStage::AfterManifestPublished,  CommitStage::AfterFencingPublished,
      CommitStage::AfterRetention,          CommitStage::CommitComplete,
  };
  FCO_CHECK_EQ(stages.size(), expected.size());
  for (std::size_t index = 0; index < stages.size() && index < expected.size(); ++index) {
    FCO_CHECK_EQ(stages[index], expected[index]);
  }

  // Every commit fires the same sequence, in the same order.
  stages.clear();
  FCO_REQUIRE(store.commit(make_state(2, 2)).ok());
  FCO_CHECK_EQ(stages.size(), expected.size());
  for (std::size_t index = 0; index < stages.size() && index < expected.size(); ++index) {
    FCO_CHECK_EQ(stages[index], expected[index]);
  }

  // A hook that throws turns the commit into a CommitFailure rather than a
  // silently partial publication, and leaves the previous generation intact.
  bool fail_on_publish = false;
  std::vector<CommitStage> failing_stages;
  CommitHooks failing;
  failing.on_stage = [&failing_stages, &fail_on_publish](CommitStage stage) {
    failing_stages.push_back(stage);
    if (fail_on_publish && stage == CommitStage::AfterRecordPublished) {
      throw std::runtime_error("injected hook failure");
    }
  };
  const fs::path second_root = temp.path() / "second";
  auto failing_open = DurableStore::open(second_root, failing);
  FCO_REQUIRE(failing_open.ok());
  DurableStore failing_store = failing_open.take();
  FCO_REQUIRE(failing_store.commit(make_state(1, 1)).ok());
  const std::size_t stages_before = failing_stages.size();
  fail_on_publish = true;
  FCO_CHECK_ERROR(failing_store.commit(make_state(2, 2)), ErrorCode::CommitFailure);
  FCO_CHECK_EQ(failing_store.generation(), std::uint64_t{1});
  // The commit stopped at the throwing stage, so fewer stages fired than a
  // complete commit would fire.
  FCO_CHECK(failing_stages.size() > stages_before);
  FCO_CHECK(failing_stages.size() - stages_before < expected.size());
  auto still_there = failing_store.load();
  FCO_REQUIRE(still_there.ok());
  FCO_CHECK_EQ(still_there.value().revision.value(), std::uint64_t{1});
  // The record published by the failed commit was removed again, so exactly one
  // generation survives.
  FCO_CHECK_EQ(count_regular_files(records_directory(second_root)), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Corruption and recovery
// ---------------------------------------------------------------------------
FCO_TEST(store, recovers_a_verified_older_generation_from_a_damaged_record) {
  const std::vector<std::string> cases = {"payload flip", "truncated record", "appended byte",
                                          "non-zero flags", "bumped format version"};
  for (const std::string& label : cases) {
    TempRoot temp;
    Digest first;
    Digest second;
    build_two_generations(temp.path(), first, second);
    const fs::path record = record_path(temp.path(), 2);
    std::vector<std::uint8_t> bytes = read_file(record);
    FCO_REQUIRE(bytes.size() > kFrameHeaderBytes + kFrameTrailerBytes);
    if (label == "payload flip") {
      bytes[kFrameHeaderBytes + 10u] = static_cast<std::uint8_t>(bytes[kFrameHeaderBytes + 10u] ^ 0xFFu);
    } else if (label == "truncated record") {
      bytes.pop_back();
    } else if (label == "appended byte") {
      bytes.push_back(0u);
    } else if (label == "non-zero flags") {
      store_u32_at(bytes, 8u, 1u);
    } else {
      store_u16_at(bytes, 4u, 2u);
    }
    write_file(record, bytes);

    auto opened = DurableStore::open(temp.path());
    if (!opened.ok()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  label + ": open failed: " + fco::describe(opened.error()));
      continue;
    }
    DurableStore store = opened.take();
    FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("record-scan"));
    FCO_CHECK_EQ(store.generation(), std::uint64_t{1});
    FCO_CHECK(store.recovery().manifest_repaired);
    FCO_CHECK(store.recovery().fencing_repaired);
    FCO_CHECK(store.recovery().rejected_records >= 1);
    expect_recovered_state(store, first, second, label.c_str());
    // Observation, reported: a record-scan recovery rewrites the manifest for the
    // generation it recovered but deliberately never moves the fencing record
    // backwards, so the store's fencing_digest() still names the manifest that
    // was replaced. The accessor is faithful to the fencing record (that is the
    // digest it carries); a later commit republishes both artifacts and heals
    // the divergence. The check below pins that behaviour so a future change is
    // deliberate.
    if (store.recovery().mode == fco::RecoveryMode::RecordScan) {
      const std::vector<std::uint8_t> manifest_bytes =
          read_file(temp.path() / std::string(DurableStore::manifest_file_name()));
      fco::Sha256 hasher;
      hasher.update(manifest_bytes.data(), manifest_bytes.size());
      const Digest manifest_digest = hasher.finish();
      FCO_CHECK(manifest_digest != store.fencing_digest());
      FCO_CHECK(store.fencing_digest().valid());
    }

    // The repaired store is usable: the next commit follows the recovered
    // generation and the next open is manifest-authoritative again.
    FCO_REQUIRE(store.commit(make_state(2, 2)).ok());
    FCO_CHECK_EQ(store.generation(), std::uint64_t{2});
    store.close();
    auto again = DurableStore::open(temp.path());
    FCO_REQUIRE(again.ok());
    FCO_CHECK_EQ(mode_name(again.value().recovery().mode), std::string("manifest-authoritative"));
    expect_recovered_state(again.value(), first, second, label.c_str());
  }
}

FCO_TEST(store, recovery_rebuilds_a_truncated_manifest_from_the_fencing_record) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);

  const fs::path manifest = temp.path() / std::string(DurableStore::manifest_file_name());
  std::vector<std::uint8_t> bytes = read_file(manifest);
  FCO_REQUIRE(!bytes.empty());
  bytes.pop_back();
  write_file(manifest, bytes);

  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("fencing-authoritative"));
  FCO_CHECK_EQ(store.generation(), std::uint64_t{2});
  FCO_CHECK(store.recovery().manifest_repaired);
  expect_recovered_state(store, first, second, "truncated manifest");
}

FCO_TEST(store, recovery_trusts_a_verified_manifest_when_fencing_is_damaged) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);

  const fs::path fencing = temp.path() / std::string(DurableStore::fencing_file_name());
  std::vector<std::uint8_t> bytes = read_file(fencing);
  FCO_REQUIRE(bytes.size() > kFrameHeaderBytes + kFrameTrailerBytes);
  bytes[kFrameHeaderBytes + 4u] = static_cast<std::uint8_t>(bytes[kFrameHeaderBytes + 4u] ^ 0x01u);
  write_file(fencing, bytes);

  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("manifest-authoritative"));
  FCO_CHECK_EQ(store.generation(), std::uint64_t{2});
  FCO_CHECK(!store.recovery().manifest_repaired);
  FCO_CHECK(!store.recovery().fencing_repaired);
  expect_recovered_state(store, first, second, "damaged fencing");
}

FCO_TEST(store, recovery_scans_records_when_the_newest_record_is_not_a_frame) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);

  // Both artifacts name generation 2, but the record is not a state record at
  // all. Generation 1 still verifies, so the scan recovers it and rewrites both
  // artifacts.
  write_file(record_path(temp.path(), 2), {1u, 2u, 3u, 4u});
  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("record-scan"));
  FCO_CHECK_EQ(store.generation(), std::uint64_t{1});
  FCO_CHECK(store.recovery().manifest_repaired);
  FCO_CHECK(store.recovery().fencing_repaired);
  expect_recovered_state(store, first, second, "unreadable current record");
}

FCO_TEST(store, recovery_is_ambiguous_when_nothing_verifies) {
  TempRoot temp;
  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  const OrchestratorState only = make_state(1, 1);
  FCO_REQUIRE(store.commit(only).ok());
  store.close();

  // The only record is damaged, so the manifest names an unreadable record, the
  // fencing record names the same unreadable record and the scan finds nothing.
  // An empty store must never be invented over durable, unreadable data.
  std::vector<std::uint8_t> bytes = read_file(record_path(temp.path(), 1));
  FCO_REQUIRE(bytes.size() > kFrameHeaderBytes + kFrameTrailerBytes);
  bytes[kFrameHeaderBytes + 3u] = static_cast<std::uint8_t>(bytes[kFrameHeaderBytes + 3u] ^ 0xFFu);
  write_file(record_path(temp.path(), 1), bytes);

  auto reopened = DurableStore::open(temp.path());
  FCO_CHECK_ERROR(reopened, ErrorCode::AmbiguousRecovery);
}

FCO_TEST(store, a_missing_record_is_not_counted_as_rejected) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);
  std::error_code code;
  fs::remove(record_path(temp.path(), 2), code);
  FCO_CHECK(!fs::exists(record_path(temp.path(), 2)));

  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("record-scan"));
  FCO_CHECK_EQ(store.generation(), std::uint64_t{1});
  // rejected_records counts state record files that failed verification; a
  // missing record is reported through the notes instead.
  FCO_CHECK_EQ(store.recovery().rejected_records, std::uint64_t{0});
  FCO_CHECK(store.recovery().verified_records >= 1);
  expect_recovered_state(store, first, second, "missing current record");
}

FCO_TEST(store, recovery_fences_a_manifest_that_names_an_older_generation) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);

  // A second root is built with exactly the same first generation, so its
  // manifest is byte-identical to the manifest this root published for
  // generation 1. Installing it makes the manifest the stale artifact while the
  // fencing record and both records stay intact.
  TempRoot donor;
  auto donor_open = DurableStore::open(donor.path());
  FCO_REQUIRE(donor_open.ok());
  DurableStore donor_store = donor_open.take();
  FCO_REQUIRE(donor_store.commit(make_state(1, 1)).ok());
  donor_store.close();

  const std::vector<std::uint8_t> older_manifest =
      read_file(donor.path() / std::string(DurableStore::manifest_file_name()));
  FCO_REQUIRE(!older_manifest.empty());
  write_file(temp.path() / std::string(DurableStore::manifest_file_name()), older_manifest);

  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("fencing-authoritative"));
  FCO_CHECK_EQ(store.generation(), std::uint64_t{2});
  FCO_CHECK(store.recovery().manifest_repaired);
  expect_recovered_state(store, first, second, "stale manifest");
}

FCO_TEST(store, recovery_scans_records_when_manifest_and_fencing_are_both_damaged) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);

  const fs::path manifest = temp.path() / std::string(DurableStore::manifest_file_name());
  std::vector<std::uint8_t> manifest_bytes = read_file(manifest);
  FCO_REQUIRE(!manifest_bytes.empty());
  manifest_bytes.pop_back();
  write_file(manifest, manifest_bytes);

  const fs::path fencing = temp.path() / std::string(DurableStore::fencing_file_name());
  std::vector<std::uint8_t> fencing_bytes = read_file(fencing);
  FCO_REQUIRE(fencing_bytes.size() > kFrameHeaderBytes + kFrameTrailerBytes);
  fencing_bytes[kFrameHeaderBytes + 2u] =
      static_cast<std::uint8_t>(fencing_bytes[kFrameHeaderBytes + 2u] ^ 0x01u);
  write_file(fencing, fencing_bytes);

  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("record-scan"));
  FCO_CHECK_EQ(store.generation(), std::uint64_t{2});
  FCO_CHECK(store.recovery().manifest_repaired);
  FCO_CHECK(store.recovery().fencing_repaired);
  expect_recovered_state(store, first, second, "both artifacts damaged");
}

FCO_TEST(store, recovery_never_moves_the_manifest_backwards) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);

  // The manifest is the newest verified generation; the record it names is
  // replaced by the older record, which does not match the manifest.
  std::vector<std::uint8_t> older = read_file(record_path(temp.path(), 1));
  FCO_REQUIRE(!older.empty());
  write_file(record_path(temp.path(), 2), older);

  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK(store.generation() <= 2u);
  expect_recovered_state(store, first, second, "record replaced");
}

FCO_TEST(store, recovery_leaves_a_fence_that_is_ahead_untouched) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);

  // Rewrite the fencing record so that it publishes a generation no record
  // holds. The frame stays valid: only the payload bytes and the three
  // integrity fields are recomputed.
  const fs::path fencing = temp.path() / std::string(DurableStore::fencing_file_name());
  std::vector<std::uint8_t> bytes = read_file(fencing);
  FCO_REQUIRE(bytes.size() > kFrameHeaderBytes + kFrameTrailerBytes);
  const std::size_t generation_offset = kFrameHeaderBytes + 4u;  // version + reserved
  for (unsigned index = 0; index < 8u; ++index) {
    bytes[generation_offset + index] = static_cast<std::uint8_t>(index == 0 ? 5u : 0u);
  }
  const std::size_t payload_length = bytes.size() - kFrameHeaderBytes - kFrameTrailerBytes;
  store_u32_at(bytes, 16u, fco::crc32(bytes.data() + kFrameHeaderBytes, payload_length, 0));
  store_u32_at(bytes, 20u, fco::crc32(bytes.data(), 20u, 0));
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
  FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("manifest-authoritative"));
  FCO_CHECK_EQ(store.generation(), std::uint64_t{2});  // never 5, never 1
  expect_recovered_state(store, first, second, "fence ahead of every record");
  // A later commit publishes a real generation and moves past the fence.
  FCO_REQUIRE(store.commit(make_state(3, 3)).ok());
  FCO_CHECK_EQ(store.generation(), std::uint64_t{3});
}

FCO_TEST(store, records_directory_junk_is_ignored) {
  TempRoot temp;
  Digest first;
  Digest second;
  build_two_generations(temp.path(), first, second);
  const fs::path records = records_directory(temp.path());
  write_file(records / "notes.txt", {1u, 2u, 3u});
  write_file(records / "state-abc.fco", {1u});
  write_file(records / "state-0.fco", {1u});
  write_file(records / "state-01.fco", {1u});
  write_file(records / "state-.fco", {1u});
  write_file(records / "state-99999999999999999999999.fco", {1u});
  std::error_code code;
  fs::create_directories(records / "state-7.fco", code);

  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();
  FCO_CHECK_EQ(mode_name(store.recovery().mode), std::string("manifest-authoritative"));
  FCO_CHECK_EQ(store.generation(), std::uint64_t{2});
  expect_recovered_state(store, first, second, "junk in records/");
  FCO_CHECK(fs::exists(records / "notes.txt"));
  FCO_CHECK(fs::is_directory(records / "state-7.fco"));
  FCO_CHECK(fs::exists(record_path(temp.path(), 2)));
}

// ---------------------------------------------------------------------------
// Locking, paths and retention
// ---------------------------------------------------------------------------
FCO_TEST(store, the_writer_lock_is_exclusive_across_handles) {
  TempRoot temp;
  const fs::path root = temp.path() / "store";
  auto first = DurableStore::open(root);
  FCO_REQUIRE(first.ok());
  DurableStore store = first.take();
  FCO_REQUIRE(store.commit(make_state(1, 1)).ok());

  // Measured on Windows (MSVC 19.44, NTFS): CreateFileW with dwShareMode = 0 is
  // enforced against every other handle, including one owned by the same
  // process, so a second open of the same root is refused with
  // LockUnavailable (win32 error 32, ERROR_SHARING_VIOLATION). Cross-process
  // exclusion is proven separately by tests/test_process.cpp with real
  // processes.
  auto second = DurableStore::open(root);
  FCO_CHECK_ERROR(second, ErrorCode::LockUnavailable);
  // The refused open leaves the holder untouched and usable.
  FCO_CHECK_EQ(store.generation(), std::uint64_t{1});
  auto loaded = store.load();
  FCO_CHECK(loaded.ok());

  store.close();
  auto third = DurableStore::open(root);
  FCO_REQUIRE(third.ok());
  FCO_CHECK_EQ(third.value().generation(), std::uint64_t{1});

  // Closing is idempotent.
  third.value().close();
  third.value().close();
  FCO_CHECK(!third.value().is_open());
}

FCO_TEST(store, open_rejects_a_file_root_and_an_empty_path) {
  TempRoot temp;
  const fs::path file_root = temp.path() / "a-file";
  write_file(file_root, {1u, 2u, 3u});

  auto opened = DurableStore::open(file_root);
  FCO_CHECK_ERROR(opened, ErrorCode::PathRejected);

  auto empty = DurableStore::open(fs::path());
  FCO_CHECK_ERROR(empty, ErrorCode::PathRejected);

  // A directory below a file cannot be created, which is a storage failure
  // rather than a path rejection.
  auto under_file = DurableStore::open(file_root / "child");
  if (!under_file.ok()) {
    FCO_CHECK_EQ(under_file.error().code, ErrorCode::StorageFailure);
  }
  FCO_CHECK(fs::is_regular_file(file_root));
  FCO_CHECK(!fs::exists(file_root / "child"));
}

FCO_TEST(store, retention_keeps_the_newest_generations) {
  TempRoot temp;
  auto opened = DurableStore::open(temp.path());
  FCO_REQUIRE(opened.ok());
  DurableStore store = opened.take();

  const std::uint64_t commits = fco::kRetainedGenerations + 4u;  // 12
  for (std::uint64_t index = 1; index <= commits; ++index) {
    auto committed = store.commit(make_state(index, index));
    FCO_REQUIRE(committed.ok());
  }
  FCO_CHECK_EQ(store.generation(), commits);
  const fs::path records = records_directory(temp.path());
  const std::size_t remaining = count_regular_files(records);
  FCO_CHECK(remaining <= static_cast<std::size_t>(fco::kRetainedGenerations));
  FCO_CHECK_EQ(remaining, static_cast<std::size_t>(fco::kRetainedGenerations));
  FCO_CHECK(fs::exists(record_path(temp.path(), commits)));
  FCO_CHECK(fs::exists(record_path(temp.path(), commits - fco::kRetainedGenerations + 1u)));
  FCO_CHECK(!fs::exists(record_path(temp.path(), commits - fco::kRetainedGenerations)));
  FCO_CHECK(!fs::exists(record_path(temp.path(), 1u)));

  // The current generation still loads after the pruning.
  auto loaded = store.load();
  FCO_REQUIRE(loaded.ok());
  FCO_CHECK_EQ(loaded.value().revision.value(), commits);
}

// ---------------------------------------------------------------------------
// Record frames
// ---------------------------------------------------------------------------
FCO_TEST(store, frame_codec_round_trips_every_record_kind) {
  const std::vector<RecordKind> kinds = {RecordKind::Manifest, RecordKind::State,
                                         RecordKind::Fencing};
  for (const RecordKind kind : kinds) {
    RecordFrame frame;
    frame.kind = kind;
    frame.format_version = fco::kStorageFormatVersion;
    frame.payload = {1u, 2u, 3u, 4u, 5u};
    const std::vector<std::uint8_t> bytes = fco::encode_frame(frame);
    FCO_CHECK_EQ(bytes.size(), kFrameHeaderBytes + frame.payload.size() + kFrameTrailerBytes);
    auto decoded = fco::decode_frame(bytes.data(), bytes.size());
    FCO_REQUIRE(decoded.ok());
    FCO_CHECK_EQ(decoded.value().kind, kind);
    FCO_CHECK_EQ(decoded.value().format_version, fco::kStorageFormatVersion);
    FCO_CHECK(decoded.value().payload == frame.payload);
    FCO_CHECK(decoded.value().content_digest.valid());
    // encode_frame does not mutate the frame it is given.
    FCO_CHECK(!frame.content_digest.valid());
  }

  // An empty payload is a legal frame.
  RecordFrame empty;
  empty.kind = RecordKind::State;
  empty.payload.clear();
  const std::vector<std::uint8_t> empty_bytes = fco::encode_frame(empty);
  FCO_CHECK_EQ(empty_bytes.size(), kFrameHeaderBytes + kFrameTrailerBytes);
  auto decoded_empty = fco::decode_frame(empty_bytes.data(), empty_bytes.size());
  FCO_REQUIRE(decoded_empty.ok());
  FCO_CHECK(decoded_empty.value().payload.empty());
}

FCO_TEST(store, frame_codec_rejects_corruption) {
  RecordFrame frame;
  frame.kind = RecordKind::State;
  frame.payload = {9u, 8u, 7u, 6u};
  const std::vector<std::uint8_t> good = fco::encode_frame(frame);
  FCO_REQUIRE(good.size() == kFrameHeaderBytes + frame.payload.size() + kFrameTrailerBytes);
  FCO_CHECK(fco::decode_frame(good.data(), good.size()).ok());

  // Bad magic.
  std::vector<std::uint8_t> magic = good;
  magic[0] = static_cast<std::uint8_t>(magic[0] ^ 0x01u);
  FCO_CHECK_ERROR(fco::decode_frame(magic.data(), magic.size()), ErrorCode::MalformedInput);

  // Bad header CRC: the record kind byte is covered by the header CRC and is
  // validated only after it.
  std::vector<std::uint8_t> header_crc = good;
  header_crc[6] = static_cast<std::uint8_t>(header_crc[6] ^ 0xFFu);
  FCO_CHECK_ERROR(fco::decode_frame(header_crc.data(), header_crc.size()),
                  ErrorCode::IntegrityFailure);

  // A zeroed header CRC is an integrity failure too.
  std::vector<std::uint8_t> zeroed_crc = good;
  store_u32_at(zeroed_crc, 20u, 0u);
  FCO_CHECK_ERROR(fco::decode_frame(zeroed_crc.data(), zeroed_crc.size()),
                  ErrorCode::IntegrityFailure);

  // Bad payload CRC.
  std::vector<std::uint8_t> payload_crc = good;
  payload_crc[kFrameHeaderBytes] = static_cast<std::uint8_t>(payload_crc[kFrameHeaderBytes] ^ 0xFFu);
  FCO_CHECK_ERROR(fco::decode_frame(payload_crc.data(), payload_crc.size()),
                  ErrorCode::IntegrityFailure);

  // Bad content digest: the trailer is not covered by either CRC.
  std::vector<std::uint8_t> digest = good;
  digest.back() = static_cast<std::uint8_t>(digest.back() ^ 0x01u);
  FCO_CHECK_ERROR(fco::decode_frame(digest.data(), digest.size()), ErrorCode::IntegrityFailure);

  // Wrong total length, in both directions.
  FCO_CHECK_ERROR(fco::decode_frame(good.data(), good.size() - 1u), ErrorCode::MalformedInput);
  std::vector<std::uint8_t> longer = good;
  longer.push_back(0u);
  FCO_CHECK_ERROR(fco::decode_frame(longer.data(), longer.size()), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::decode_frame(good.data(), kFrameHeaderBytes - 1u),
                  ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(fco::decode_frame(nullptr, 0u), ErrorCode::MalformedInput);

  // An unsupported format version is rejected before anything else.
  std::vector<std::uint8_t> version = good;
  store_u16_at(version, 4u, 2u);
  FCO_CHECK_ERROR(fco::decode_frame(version.data(), version.size()),
                  ErrorCode::UnsupportedFormatVersion);

  // Non-zero reserved flags.
  std::vector<std::uint8_t> flags = good;
  store_u32_at(flags, 8u, 1u);
  FCO_CHECK_ERROR(fco::decode_frame(flags.data(), flags.size()), ErrorCode::ReservedFieldNonZero);

  // An unknown record kind, with the header CRC recomputed so that the kind
  // check is what rejects it.
  for (const std::uint16_t raw_kind : {std::uint16_t{0u}, std::uint16_t{4u},
                                       std::uint16_t{99u}}) {
    std::vector<std::uint8_t> unknown = good;
    store_u16_at(unknown, 6u, raw_kind);
    store_u32_at(unknown, 20u, fco::crc32(unknown.data(), 20u, 0));
    FCO_CHECK_ERROR(fco::decode_frame(unknown.data(), unknown.size()),
                    ErrorCode::InvalidEnumValue);
  }

  // A payload length that claims more than the supported bound is rejected
  // before the buffer is interpreted any further.
  const std::size_t claimed = static_cast<std::size_t>(fco::kMaxRecordPayloadBytes) + 1u;
  std::vector<std::uint8_t> oversized(kFrameHeaderBytes + claimed + kFrameTrailerBytes, 0u);
  oversized[0] = 'F';
  oversized[1] = 'C';
  oversized[2] = 'O';
  oversized[3] = 'J';
  store_u16_at(oversized, 4u, fco::kStorageFormatVersion);
  store_u16_at(oversized, 6u, static_cast<std::uint16_t>(RecordKind::State));
  store_u32_at(oversized, 12u, static_cast<std::uint32_t>(claimed));
  FCO_CHECK_ERROR(fco::decode_frame(oversized.data(), oversized.size()),
                  ErrorCode::BoundedLimitExceeded);

  // encode_frame is total: an unencodable frame is an empty vector, never a
  // truncated one.
  RecordFrame invalid;
  invalid.kind = RecordKind::Unspecified;
  FCO_CHECK(fco::encode_frame(invalid).empty());
  RecordFrame too_large;
  too_large.kind = RecordKind::State;
  too_large.payload.assign(claimed, 0u);
  FCO_CHECK(fco::encode_frame(too_large).empty());
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
FCO_TEST(store, utf8_path_conversion_round_trips) {
  const std::string with_spaces = "store dir/with spaces";
  auto spaces = fco::path_from_utf8(with_spaces);
  FCO_REQUIRE(spaces.ok());
  FCO_CHECK_EQ(fco::path_to_utf8(spaces.value()), with_spaces);

  const std::string non_ascii = std::string("caf") + "\xC3\xA9" + "/" + "\xE8\xB7\xAF\xE5\xBE\x84";
  auto unicode = fco::path_from_utf8(non_ascii);
  FCO_REQUIRE(unicode.ok());
  FCO_CHECK_EQ(fco::path_to_utf8(unicode.value()), non_ascii);

  FCO_CHECK_ERROR(fco::path_from_utf8(""), ErrorCode::PathRejected);
  const std::string with_nul("a\0b", 3);
  FCO_CHECK_ERROR(fco::path_from_utf8(with_nul), ErrorCode::PathRejected);
  FCO_CHECK_ERROR(fco::path_from_utf8(std::string_view(with_nul)), ErrorCode::PathRejected);
  FCO_CHECK_ERROR(fco::path_from_utf8(std::string_view("\0", 1)), ErrorCode::PathRejected);
#if defined(_WIN32)
  FCO_CHECK_ERROR(fco::path_from_utf8("\xFF\xFE"), ErrorCode::PathRejected);
#endif

  // A converted path is usable end to end.
  TempRoot temp;
  const fs::path root = temp.path() / spaces.value();
  auto opened = DurableStore::open(root);
  FCO_REQUIRE(opened.ok());
  FCO_CHECK(fs::is_directory(root));
}

FCO_TEST_MAIN
