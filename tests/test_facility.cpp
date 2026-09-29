// Tests for fco/facility.hpp and fco/facility.cpp: snapshot construction and
// rejection, order-independent canonical digest, lookup helpers, precondition
// and effect evaluation for every enumerator, and the action projection model.

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "fco/facility.hpp"

namespace fco::test {

template <>
struct ValuePrinter<fco::Digest> {
  [[nodiscard]] static std::string print(const fco::Digest& value) { return value.to_hex(); }
};

}  // namespace fco::test

namespace {

using fco::ActionKind;
using fco::ActionRequest;
using fco::AssetId;
using fco::AssetLifecycle;
using fco::AssetRecord;
using fco::CoolingState;
using fco::FacilitySnapshot;
using fco::FirmwareGeneration;
using fco::HardwareGeneration;
using fco::MaintenanceState;
using fco::NetworkState;
using fco::PowerState;
using fco::Predicate;
using fco::PredicateKind;
using fco::RackId;
using fco::SiteId;
using fco::StepId;
using fco::StepStatus;
using fco::WorkloadState;

constexpr std::uint32_t kGenerationCount = static_cast<std::uint32_t>(fco::kGenerationKindMax);

using Progress = std::map<StepId, StepStatus>;

[[nodiscard]] fco::GenerationSet make_generations(std::uint64_t base) {
  fco::GenerationSet set;
  for (std::uint32_t raw = 1; raw <= kGenerationCount; ++raw) {
    set.set(static_cast<fco::GenerationKind>(raw), base + raw);
  }
  return set;
}

[[nodiscard]] AssetRecord make_record(const char* id, const char* rack) {
  AssetRecord record;
  record.id = AssetId(id);
  record.rack = RackId(rack);
  record.site = SiteId("site-1");
  record.lifecycle = AssetLifecycle::Active;
  record.power = PowerState::On;
  record.cooling = CoolingState::Normal;
  record.maintenance = MaintenanceState::None;
  record.network = NetworkState::Attached;
  record.workloads = WorkloadState::Present;
  record.hardware_generation = HardwareGeneration(2);
  record.firmware_generation = FirmwareGeneration(5);
  record.lifecycle_generation = fco::LifecycleGeneration(10);
  record.maintenance_generation = fco::MaintenanceGeneration(4);
  record.capacity_generation = fco::CapacityGeneration(6);
  record.capacity_total_units = 100;
  record.capacity_reserved_units = 0;
  return record;
}

[[nodiscard]] fco::Result<FacilitySnapshot> make_snapshot(std::vector<AssetRecord> assets) {
  return FacilitySnapshot::create(SiteId("site-1"), fco::FacilityEpoch(3), make_generations(10),
                                  std::move(assets));
}

[[nodiscard]] ActionRequest make_request(ActionKind kind, const AssetRecord& record) {
  ActionRequest request;
  request.kind = kind;
  request.owner = fco::owning_domain(kind);
  request.asset = record.id;
  request.rack = record.rack;
  request.site = record.site;
  request.hardware_generation = record.hardware_generation;
  request.firmware_generation = record.firmware_generation;
  request.lifecycle_generation = record.lifecycle_generation;
  return request;
}

[[nodiscard]] Predicate make_predicate(PredicateKind kind, const char* asset) {
  Predicate predicate;
  predicate.kind = kind;
  if (asset != nullptr) predicate.asset = AssetId(asset);
  return predicate;
}

void expect_predicate(bool expected, const char* label, const Predicate& predicate,
                      const FacilitySnapshot& snapshot, const Progress& progress) {
  const auto result = fco::evaluate_predicate(predicate, snapshot, progress);
  if (!result.ok()) {
    ::fco::test::report_failure(__FILE__, __LINE__,
                                std::string(label) + ": unexpected error " +
                                    fco::describe(result.error()));
    return;
  }
  ::fco::test::check_true(result.value() == expected,
                          (std::string(label) + ": expected " +
                           (expected ? "satisfied" : "unsatisfied"))
                              .c_str(),
                          __FILE__, __LINE__);
}

void expect_effect(bool expected, const char* label, const fco::EffectSpec& effect,
                   const FacilitySnapshot& snapshot) {
  const auto result = fco::evaluate_effect(effect, snapshot);
  if (!result.ok()) {
    ::fco::test::report_failure(__FILE__, __LINE__, std::string(label) + ": unexpected error " +
                                                          fco::describe(result.error()));
    return;
  }
  ::fco::test::check_true(result.value() == expected,
                          (std::string(label) + ": expected " +
                           (expected ? "satisfied" : "unsatisfied"))
                              .c_str(),
                          __FILE__, __LINE__);
}

[[nodiscard]] fco::EffectSpec make_effect(fco::EffectKind kind, const char* asset) {
  fco::EffectSpec effect;
  effect.kind = kind;
  if (asset != nullptr) effect.asset = AssetId(asset);
  return effect;
}

}  // namespace

// ---------------------------------------------------------------------------
// Snapshot construction.
// ---------------------------------------------------------------------------
FCO_TEST(facility, snapshot_create_accepts_complete_observed_assets) {
  const auto snapshot = make_snapshot({make_record("asset-1", "rack-1"),
                                       make_record("asset-2", "rack-1"),
                                       make_record("asset-3", "rack-2")});
  FCO_REQUIRE(snapshot.ok());
  const FacilitySnapshot& facility = snapshot.value();

  FCO_CHECK(facility.valid());
  FCO_CHECK_EQ(facility.size(), std::size_t{3});
  FCO_CHECK_EQ(facility.site().to_string(), std::string("site-1"));
  FCO_CHECK_EQ(facility.epoch().value(), std::uint64_t{3});
  FCO_CHECK(facility.generations().complete());
  FCO_CHECK(facility.digest().valid());

  const AssetRecord* first = facility.find(AssetId("asset-1"));
  FCO_REQUIRE(first != nullptr);
  FCO_CHECK_EQ(first->id.to_string(), std::string("asset-1"));
  FCO_CHECK_EQ(first->rack.to_string(), std::string("rack-1"));
  FCO_CHECK_EQ(first->lifecycle, AssetLifecycle::Active);
  FCO_CHECK_EQ(first->power, PowerState::On);
  FCO_CHECK_EQ(first->cooling, CoolingState::Normal);
  FCO_CHECK_EQ(first->maintenance, MaintenanceState::None);
  FCO_CHECK_EQ(first->network, NetworkState::Attached);
  FCO_CHECK_EQ(first->workloads, WorkloadState::Present);
  FCO_CHECK_EQ(first->hardware_generation.value(), std::uint64_t{2});
  FCO_CHECK_EQ(first->capacity_available_units(), std::uint64_t{100});
  FCO_CHECK(!first->has_tenant());

  FCO_CHECK(facility.find(AssetId("asset-9")) == nullptr);
  FCO_CHECK(facility.find(AssetId{}) == nullptr);
}

FCO_TEST(facility, snapshot_lookup_helpers) {
  const auto snapshot = make_snapshot({make_record("asset-3", "rack-2"),
                                       make_record("asset-1", "rack-1"),
                                       make_record("asset-2", "rack-1")});
  FCO_REQUIRE(snapshot.ok());
  FacilitySnapshot facility = snapshot.value();

  const std::vector<AssetId> ids = facility.asset_ids();
  FCO_REQUIRE(ids.size() == 3);
  FCO_CHECK_EQ(ids[0].to_string(), std::string("asset-1"));
  FCO_CHECK_EQ(ids[1].to_string(), std::string("asset-2"));
  FCO_CHECK_EQ(ids[2].to_string(), std::string("asset-3"));

  const std::vector<const AssetRecord*> rack_one = facility.assets_in_rack(RackId("rack-1"));
  FCO_REQUIRE(rack_one.size() == 2);
  FCO_CHECK_EQ(rack_one[0]->id.to_string(), std::string("asset-1"));
  FCO_CHECK_EQ(rack_one[1]->id.to_string(), std::string("asset-2"));

  const std::vector<const AssetRecord*> rack_two = facility.assets_in_rack(RackId("rack-2"));
  FCO_REQUIRE(rack_two.size() == 1);
  FCO_CHECK_EQ(rack_two[0]->id.to_string(), std::string("asset-3"));

  FCO_CHECK(facility.assets_in_rack(RackId("rack-9")).empty());
  FCO_CHECK(facility.assets_in_rack(RackId{}).empty());

  // apply() replaces an existing observation and inserts a new one.
  AssetRecord updated = make_record("asset-1", "rack-1");
  updated.power = PowerState::Off;
  facility.apply(updated);
  FCO_CHECK_EQ(facility.find(AssetId("asset-1"))->power, PowerState::Off);

  AssetRecord extra = make_record("asset-4", "rack-2");
  facility.apply(extra);
  FCO_CHECK_EQ(facility.size(), std::size_t{4});

  AssetRecord unusable;
  facility.apply(unusable);
  FCO_CHECK_EQ(facility.size(), std::size_t{4});
}

FCO_TEST(facility, snapshot_create_rejects_duplicate_asset_ids) {
  const auto snapshot =
      make_snapshot({make_record("asset-1", "rack-1"), make_record("asset-1", "rack-2")});
  FCO_CHECK_ERROR(snapshot, fco::ErrorCode::DuplicateIdentity);
  FCO_REQUIRE(!snapshot.ok());
  FCO_CHECK_EQ(snapshot.error().subject, std::string("asset-1"));
}

FCO_TEST(facility, snapshot_create_rejects_an_empty_asset_list) {
  const auto snapshot = make_snapshot({});
  FCO_CHECK_ERROR(snapshot, fco::ErrorCode::ImpossibleCombination);
}

FCO_TEST(facility, snapshot_create_rejects_incomplete_asset_records) {
  struct Mutation {
    const char* label;
    void (*apply)(AssetRecord&);
  };
  const Mutation mutations[] = {
      {"unset id", [](AssetRecord& r) { r.id = AssetId{}; }},
      {"unset rack", [](AssetRecord& r) { r.rack = RackId{}; }},
      {"unset site", [](AssetRecord& r) { r.site = SiteId{}; }},
      {"unset lifecycle", [](AssetRecord& r) { r.lifecycle = AssetLifecycle::Unspecified; }},
      {"unset power", [](AssetRecord& r) { r.power = PowerState::Unspecified; }},
      {"unset cooling", [](AssetRecord& r) { r.cooling = CoolingState::Unspecified; }},
      {"unset maintenance", [](AssetRecord& r) { r.maintenance = MaintenanceState::Unspecified; }},
      {"unset network", [](AssetRecord& r) { r.network = NetworkState::Unspecified; }},
      {"unset workloads", [](AssetRecord& r) { r.workloads = WorkloadState::Unspecified; }},
      {"unset hardware generation", [](AssetRecord& r) { r.hardware_generation = HardwareGeneration{}; }},
      {"unset firmware generation", [](AssetRecord& r) { r.firmware_generation = fco::FirmwareGeneration{}; }},
      {"unset lifecycle generation", [](AssetRecord& r) { r.lifecycle_generation = fco::LifecycleGeneration{}; }},
      {"unset maintenance generation", [](AssetRecord& r) { r.maintenance_generation = fco::MaintenanceGeneration{}; }},
      {"unset capacity generation", [](AssetRecord& r) { r.capacity_generation = fco::CapacityGeneration{}; }},
  };

  for (const Mutation& mutation : mutations) {
    AssetRecord broken = make_record("asset-1", "rack-1");
    mutation.apply(broken);
    FCO_CHECK(!broken.complete());

    const auto snapshot = make_snapshot({make_record("asset-2", "rack-1"), broken});
    const std::string label = std::string("incomplete record (") + mutation.label + ")";
    ::fco::test::check_true(!snapshot.ok() &&
                                snapshot.error().code == fco::ErrorCode::ImpossibleCombination,
                            (label + ": must be rejected with impossible-combination").c_str(),
                            __FILE__, __LINE__);
  }
}

FCO_TEST(facility, snapshot_create_rejects_unset_identity_epoch_and_generations) {
  const std::vector<AssetRecord> assets = {make_record("asset-1", "rack-1")};

  const auto no_site =
      FacilitySnapshot::create(SiteId{}, fco::FacilityEpoch(3), make_generations(10), assets);
  FCO_CHECK_ERROR(no_site, fco::ErrorCode::InvalidIdentity);

  const auto no_epoch =
      FacilitySnapshot::create(SiteId("site-1"), fco::FacilityEpoch{}, make_generations(10), assets);
  FCO_CHECK_ERROR(no_epoch, fco::ErrorCode::InvalidIdentity);

  fco::GenerationSet incomplete = make_generations(10);
  incomplete.set(fco::GenerationKind::Capacity, 0);
  const auto no_generations =
      FacilitySnapshot::create(SiteId("site-1"), fco::FacilityEpoch(3), incomplete, assets);
  FCO_CHECK_ERROR(no_generations, fco::ErrorCode::MissingAuthority);

  const auto valid = make_snapshot(assets);
  FCO_CHECK(valid.ok());
}

// ---------------------------------------------------------------------------
// Canonical digest.
// ---------------------------------------------------------------------------
FCO_TEST(facility, snapshot_digest_is_independent_of_asset_order) {
  const AssetRecord one = make_record("asset-1", "rack-1");
  const AssetRecord two = make_record("asset-2", "rack-1");
  const AssetRecord three = make_record("asset-3", "rack-2");

  const auto forward = make_snapshot({one, two, three});
  const auto rotated = make_snapshot({three, one, two});
  const auto reversed = make_snapshot({three, two, one});
  FCO_REQUIRE(forward.ok());
  FCO_REQUIRE(rotated.ok());
  FCO_REQUIRE(reversed.ok());

  FCO_CHECK_EQ(forward.value().digest(), rotated.value().digest());
  FCO_CHECK_EQ(forward.value().digest(), reversed.value().digest());
  FCO_CHECK_EQ(forward.value().digest(), make_snapshot({one, two, three}).value().digest());

  // Every observed field participates in the identity.
  AssetRecord changed = one;
  changed.power = PowerState::Off;
  FCO_CHECK(make_snapshot({changed, two, three}).value().digest() != forward.value().digest());

  changed = one;
  changed.cooling = CoolingState::Reduced;
  FCO_CHECK(make_snapshot({changed, two, three}).value().digest() != forward.value().digest());

  changed = one;
  changed.lifecycle = AssetLifecycle::Maintenance;
  FCO_CHECK(make_snapshot({changed, two, three}).value().digest() != forward.value().digest());

  changed = one;
  changed.network = NetworkState::Detached;
  FCO_CHECK(make_snapshot({changed, two, three}).value().digest() != forward.value().digest());

  changed = one;
  changed.maintenance = MaintenanceState::Isolated;
  FCO_CHECK(make_snapshot({changed, two, three}).value().digest() != forward.value().digest());

  changed = one;
  changed.workloads = WorkloadState::Drained;
  FCO_CHECK(make_snapshot({changed, two, three}).value().digest() != forward.value().digest());

  changed = one;
  changed.hardware_generation = HardwareGeneration(9);
  FCO_CHECK(make_snapshot({changed, two, three}).value().digest() != forward.value().digest());

  changed = one;
  changed.capacity_reserved_units = 7;
  FCO_CHECK(make_snapshot({changed, two, three}).value().digest() != forward.value().digest());

  changed = one;
  changed.tenant = fco::TenantId("tenant-1");
  FCO_CHECK(make_snapshot({changed, two, three}).value().digest() != forward.value().digest());

  const auto other_site = FacilitySnapshot::create(SiteId("site-2"), fco::FacilityEpoch(3),
                                                   make_generations(10), {one, two, three});
  FCO_REQUIRE(other_site.ok());
  FCO_CHECK(other_site.value().digest() != forward.value().digest());

  const auto other_epoch = FacilitySnapshot::create(SiteId("site-1"), fco::FacilityEpoch(4),
                                                    make_generations(10), {one, two, three});
  FCO_REQUIRE(other_epoch.ok());
  FCO_CHECK(other_epoch.value().digest() != forward.value().digest());

  const auto other_generations = FacilitySnapshot::create(SiteId("site-1"), fco::FacilityEpoch(3),
                                                          make_generations(11), {one, two, three});
  FCO_REQUIRE(other_generations.ok());
  FCO_CHECK(other_generations.value().digest() != forward.value().digest());
}

// ---------------------------------------------------------------------------
// evaluate_predicate.
// ---------------------------------------------------------------------------
FCO_TEST(facility, evaluate_predicate_for_every_kind) {
  AssetRecord first = make_record("asset-1", "rack-1");
  first.workloads = WorkloadState::Present;
  first.capacity_total_units = 100;
  first.capacity_reserved_units = 20;
  first.lifecycle_generation = fco::LifecycleGeneration(5);

  AssetRecord second = make_record("asset-2", "rack-1");
  second.tenant = fco::TenantId("tenant-1");
  second.lifecycle = AssetLifecycle::Maintenance;
  second.power = PowerState::Off;
  second.cooling = CoolingState::Isolated;
  second.maintenance = MaintenanceState::Isolated;
  second.network = NetworkState::Quiesced;
  second.workloads = WorkloadState::Drained;

  AssetRecord unknown = make_record("asset-3", "rack-2");
  unknown.lifecycle = AssetLifecycle::Unknown;
  unknown.power = PowerState::Unknown;
  unknown.cooling = CoolingState::Unknown;
  unknown.maintenance = MaintenanceState::Unknown;
  unknown.network = NetworkState::Unknown;
  unknown.workloads = WorkloadState::Unknown;

  const auto snapshot = make_snapshot({first, second, unknown});
  FCO_REQUIRE(snapshot.ok());
  const FacilitySnapshot& facility = snapshot.value();

  Progress progress;
  progress[StepId("pre-ok")] = StepStatus::Verified;
  progress[StepId("pre-fail")] = StepStatus::Failed;
  progress[StepId("pre-pending")] = StepStatus::Pending;

  Predicate predicate = make_predicate(PredicateKind::AssetLifecycleIs, "asset-1");
  predicate.expected_state = static_cast<std::uint32_t>(AssetLifecycle::Active);
  expect_predicate(true, "asset-1 lifecycle is active", predicate, facility, progress);
  predicate.expected_state = static_cast<std::uint32_t>(AssetLifecycle::Maintenance);
  expect_predicate(false, "asset-1 lifecycle is maintenance", predicate, facility, progress);

  predicate = make_predicate(PredicateKind::PowerStateIs, "asset-1");
  predicate.expected_state = static_cast<std::uint32_t>(PowerState::On);
  expect_predicate(true, "asset-1 power is on", predicate, facility, progress);
  predicate.expected_state = static_cast<std::uint32_t>(PowerState::Off);
  expect_predicate(false, "asset-1 power is off", predicate, facility, progress);

  predicate = make_predicate(PredicateKind::CoolingStateIs, "asset-1");
  predicate.expected_state = static_cast<std::uint32_t>(CoolingState::Normal);
  expect_predicate(true, "asset-1 cooling is normal", predicate, facility, progress);
  predicate.expected_state = static_cast<std::uint32_t>(CoolingState::Reduced);
  expect_predicate(false, "asset-1 cooling is reduced", predicate, facility, progress);

  predicate = make_predicate(PredicateKind::MaintenanceStateIs, "asset-1");
  predicate.expected_state = static_cast<std::uint32_t>(MaintenanceState::None);
  expect_predicate(true, "asset-1 maintenance is none", predicate, facility, progress);
  predicate.expected_state = static_cast<std::uint32_t>(MaintenanceState::Isolated);
  expect_predicate(false, "asset-1 maintenance is isolated", predicate, facility, progress);

  predicate = make_predicate(PredicateKind::NetworkStateIs, "asset-1");
  predicate.expected_state = static_cast<std::uint32_t>(NetworkState::Attached);
  expect_predicate(true, "asset-1 network is attached", predicate, facility, progress);
  predicate.expected_state = static_cast<std::uint32_t>(NetworkState::Detached);
  expect_predicate(false, "asset-1 network is detached", predicate, facility, progress);

  predicate = make_predicate(PredicateKind::WorkloadStateIs, "asset-1");
  predicate.expected_state = static_cast<std::uint32_t>(WorkloadState::Present);
  expect_predicate(true, "asset-1 workloads are present", predicate, facility, progress);
  predicate.expected_state = static_cast<std::uint32_t>(WorkloadState::Absent);
  expect_predicate(false, "asset-1 workloads are absent", predicate, facility, progress);

  predicate = make_predicate(PredicateKind::CapacityAvailableAtLeast, "asset-1");
  predicate.expected_value = 80;
  expect_predicate(true, "asset-1 has 80 available units", predicate, facility, progress);
  predicate.expected_value = 81;
  expect_predicate(false, "asset-1 has 81 available units", predicate, facility, progress);

  predicate = make_predicate(PredicateKind::NoTenantAssigned, "asset-1");
  expect_predicate(true, "asset-1 has no tenant", predicate, facility, progress);
  predicate = make_predicate(PredicateKind::NoTenantAssigned, "asset-2");
  expect_predicate(false, "asset-2 has a tenant", predicate, facility, progress);

  predicate = make_predicate(PredicateKind::AssetLifecycleGenerationAtLeast, "asset-1");
  predicate.expected_value = 5;
  expect_predicate(true, "asset-1 lifecycle generation is at least 5", predicate, facility, progress);
  predicate.expected_value = 6;
  expect_predicate(false, "asset-1 lifecycle generation is at least 6", predicate, facility, progress);

  predicate = make_predicate(PredicateKind::PredecessorVerified, nullptr);
  predicate.predecessor = StepId("pre-ok");
  expect_predicate(true, "pre-ok is verified", predicate, facility, progress);
  predicate.predecessor = StepId("pre-fail");
  expect_predicate(false, "pre-fail is verified", predicate, facility, progress);
  predicate.predecessor = StepId("pre-pending");
  expect_predicate(false, "pre-pending is verified", predicate, facility, progress);

  predicate = make_predicate(PredicateKind::PredecessorNotVerified, nullptr);
  predicate.predecessor = StepId("pre-fail");
  expect_predicate(true, "pre-fail is not verified", predicate, facility, progress);
  predicate.predecessor = StepId("pre-ok");
  expect_predicate(false, "pre-ok is not verified", predicate, facility, progress);

  // Unknown is a real observation and never satisfies a known-state predicate.
  predicate = make_predicate(PredicateKind::PowerStateIs, "asset-3");
  predicate.expected_state = static_cast<std::uint32_t>(PowerState::On);
  expect_predicate(false, "unknown power is not on", predicate, facility, progress);
  predicate = make_predicate(PredicateKind::AssetLifecycleIs, "asset-3");
  predicate.expected_state = static_cast<std::uint32_t>(AssetLifecycle::Active);
  expect_predicate(false, "unknown lifecycle is not active", predicate, facility, progress);
  predicate = make_predicate(PredicateKind::CoolingStateIs, "asset-3");
  predicate.expected_state = static_cast<std::uint32_t>(CoolingState::Normal);
  expect_predicate(false, "unknown cooling is not normal", predicate, facility, progress);
  predicate = make_predicate(PredicateKind::MaintenanceStateIs, "asset-3");
  predicate.expected_state = static_cast<std::uint32_t>(MaintenanceState::None);
  expect_predicate(false, "unknown maintenance is not none", predicate, facility, progress);
  predicate = make_predicate(PredicateKind::NetworkStateIs, "asset-3");
  predicate.expected_state = static_cast<std::uint32_t>(NetworkState::Attached);
  expect_predicate(false, "unknown network is not attached", predicate, facility, progress);
  predicate = make_predicate(PredicateKind::WorkloadStateIs, "asset-3");
  predicate.expected_state = static_cast<std::uint32_t>(WorkloadState::Absent);
  expect_predicate(false, "unknown workloads are not absent", predicate, facility, progress);
}

FCO_TEST(facility, evaluate_predicate_rejects_malformed_predicates) {
  const auto snapshot = make_snapshot({make_record("asset-1", "rack-1")});
  FCO_REQUIRE(snapshot.ok());
  const FacilitySnapshot& facility = snapshot.value();
  const Progress progress;

  Predicate predicate = make_predicate(PredicateKind::Unspecified, "asset-1");
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::InvalidEnumValue);

  predicate = make_predicate(static_cast<PredicateKind>(static_cast<std::uint8_t>(
                                 fco::kPredicateKindMax) + 1u), "asset-1");
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::InvalidEnumValue);

  predicate = make_predicate(PredicateKind::PowerStateIs, nullptr);
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::MalformedInput);

  predicate = make_predicate(PredicateKind::PowerStateIs, "asset-1");
  predicate.expected_state = 0;
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::InvalidEnumValue);
  predicate.expected_state = 5;  // one past kPowerStateMax
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::InvalidEnumValue);

  predicate = make_predicate(PredicateKind::AssetLifecycleIs, "asset-1");
  predicate.expected_state = 0;
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::InvalidEnumValue);
  predicate.expected_state = 13;  // one past kAssetLifecycleMax
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::InvalidEnumValue);

  const PredicateKind state_kinds[] = {
      PredicateKind::CoolingStateIs,     PredicateKind::MaintenanceStateIs,
      PredicateKind::NetworkStateIs,     PredicateKind::WorkloadStateIs,
  };
  for (PredicateKind kind : state_kinds) {
    predicate = make_predicate(kind, "asset-1");
    predicate.expected_state = 0;
    FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                    fco::ErrorCode::InvalidEnumValue);
    predicate.expected_state = 200;
    FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                    fco::ErrorCode::InvalidEnumValue);
  }

  predicate = make_predicate(PredicateKind::CapacityAvailableAtLeast, nullptr);
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::MalformedInput);

  predicate = make_predicate(PredicateKind::NoTenantAssigned, nullptr);
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::MalformedInput);

  predicate = make_predicate(PredicateKind::AssetLifecycleGenerationAtLeast, nullptr);
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::MalformedInput);

  predicate = make_predicate(PredicateKind::PredecessorVerified, nullptr);
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::MalformedInput);
  predicate.predecessor = StepId("not-in-progress");
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::UnknownStep);

  predicate = make_predicate(PredicateKind::PredecessorNotVerified, nullptr);
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::MalformedInput);
  predicate.predecessor = StepId("not-in-progress");
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::UnknownStep);

  // A predicate that names an asset outside the snapshot is unknown, not false.
  predicate = make_predicate(PredicateKind::PowerStateIs, "asset-9");
  predicate.expected_state = static_cast<std::uint32_t>(PowerState::On);
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::UnknownIdentity);
  predicate = make_predicate(PredicateKind::NoTenantAssigned, "asset-9");
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::UnknownIdentity);
  predicate = make_predicate(PredicateKind::CapacityAvailableAtLeast, "asset-9");
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::UnknownIdentity);

  // The kind is validated before anything else.
  predicate = make_predicate(PredicateKind::Unspecified, nullptr);
  FCO_CHECK_ERROR(fco::evaluate_predicate(predicate, facility, progress),
                  fco::ErrorCode::InvalidEnumValue);
}

// ---------------------------------------------------------------------------
// evaluate_effect.
// ---------------------------------------------------------------------------
FCO_TEST(facility, evaluate_effect_for_every_kind) {
  AssetRecord drained = make_record("asset-1", "rack-1");
  drained.lifecycle = AssetLifecycle::Draining;
  drained.power = PowerState::Off;
  drained.cooling = CoolingState::Isolated;
  drained.maintenance = MaintenanceState::Isolated;
  drained.network = NetworkState::Quiesced;
  drained.workloads = WorkloadState::Drained;
  drained.hardware_generation = HardwareGeneration(3);
  drained.firmware_generation = fco::FirmwareGeneration(7);
  drained.capacity_reserved_units = 20;

  const auto snapshot = make_snapshot({drained});
  FCO_REQUIRE(snapshot.ok());
  const FacilitySnapshot& facility = snapshot.value();

  fco::EffectSpec effect = make_effect(fco::EffectKind::WorkloadStateChange, "asset-1");
  effect.expected_state = static_cast<std::uint32_t>(WorkloadState::Drained);
  expect_effect(true, "workloads drained", effect, facility);
  effect.expected_state = static_cast<std::uint32_t>(WorkloadState::Present);
  expect_effect(false, "workloads present", effect, facility);

  effect = make_effect(fco::EffectKind::MaintenanceStateChange, "asset-1");
  effect.expected_state = static_cast<std::uint32_t>(MaintenanceState::Isolated);
  expect_effect(true, "maintenance isolated", effect, facility);
  effect.expected_state = static_cast<std::uint32_t>(MaintenanceState::None);
  expect_effect(false, "maintenance none", effect, facility);

  effect = make_effect(fco::EffectKind::PowerStateChange, "asset-1");
  effect.expected_state = static_cast<std::uint32_t>(PowerState::Off);
  expect_effect(true, "power off", effect, facility);
  effect.expected_state = static_cast<std::uint32_t>(PowerState::On);
  expect_effect(false, "power on", effect, facility);

  effect = make_effect(fco::EffectKind::CoolingStateChange, "asset-1");
  effect.expected_state = static_cast<std::uint32_t>(CoolingState::Isolated);
  expect_effect(true, "cooling isolated", effect, facility);
  effect.expected_state = static_cast<std::uint32_t>(CoolingState::Normal);
  expect_effect(false, "cooling normal", effect, facility);

  effect = make_effect(fco::EffectKind::NetworkStateChange, "asset-1");
  effect.expected_state = static_cast<std::uint32_t>(NetworkState::Quiesced);
  expect_effect(true, "network quiesced", effect, facility);
  effect.expected_state = static_cast<std::uint32_t>(NetworkState::Attached);
  expect_effect(false, "network attached", effect, facility);

  effect = make_effect(fco::EffectKind::LifecycleStateChange, "asset-1");
  effect.expected_state = static_cast<std::uint32_t>(AssetLifecycle::Draining);
  expect_effect(true, "lifecycle draining", effect, facility);
  effect.expected_state = static_cast<std::uint32_t>(AssetLifecycle::Active);
  expect_effect(false, "lifecycle active", effect, facility);

  effect = make_effect(fco::EffectKind::CapacityChange, "asset-1");
  effect.expected_value = 20;
  expect_effect(true, "capacity reservation is 20", effect, facility);
  effect.expected_value = 21;
  expect_effect(false, "capacity reservation is 21", effect, facility);

  effect = make_effect(fco::EffectKind::HardwareGenerationChange, "asset-1");
  effect.expected_value = 3;
  expect_effect(true, "hardware generation is 3", effect, facility);
  effect.expected_value = 4;
  expect_effect(false, "hardware generation is 4", effect, facility);

  effect = make_effect(fco::EffectKind::FirmwareGenerationChange, "asset-1");
  effect.expected_value = 7;
  expect_effect(true, "firmware generation is 7", effect, facility);
  effect.expected_value = 8;
  expect_effect(false, "firmware generation is 8", effect, facility);

  effect = make_effect(fco::EffectKind::NoOpRecorded, "asset-1");
  expect_effect(true, "no-op recorded", effect, facility);

  effect = make_effect(fco::EffectKind::RestorationVerified, "asset-1");
  expect_effect(false, "drained asset is not restored", effect, facility);
}

FCO_TEST(facility, restoration_verified_requires_every_healthy_observation) {
  AssetRecord restored = make_record("asset-1", "rack-1");
  const auto snapshot = make_snapshot({restored});
  FCO_REQUIRE(snapshot.ok());
  const FacilitySnapshot& facility = snapshot.value();

  const fco::EffectSpec effect = make_effect(fco::EffectKind::RestorationVerified, "asset-1");
  expect_effect(true, "fully restored asset", effect, facility);

  AssetRecord without_workloads = restored;
  without_workloads.workloads = WorkloadState::Absent;
  const auto absent = make_snapshot({without_workloads});
  FCO_REQUIRE(absent.ok());
  expect_effect(true, "restored with workloads absent", effect, absent.value());

  struct Mutation {
    const char* label;
    void (*apply)(AssetRecord&);
  };
  const Mutation mutations[] = {
      {"lifecycle not active", [](AssetRecord& r) { r.lifecycle = AssetLifecycle::Maintenance; }},
      {"power not on", [](AssetRecord& r) { r.power = PowerState::Off; }},
      {"power unknown", [](AssetRecord& r) { r.power = PowerState::Unknown; }},
      {"cooling not normal", [](AssetRecord& r) { r.cooling = CoolingState::Reduced; }},
      {"cooling unknown", [](AssetRecord& r) { r.cooling = CoolingState::Unknown; }},
      {"network not attached", [](AssetRecord& r) { r.network = NetworkState::Quiesced; }},
      {"maintenance not none", [](AssetRecord& r) { r.maintenance = MaintenanceState::Isolated; }},
      {"workloads draining", [](AssetRecord& r) { r.workloads = WorkloadState::Draining; }},
      {"workloads unknown", [](AssetRecord& r) { r.workloads = WorkloadState::Unknown; }},
  };

  for (const Mutation& mutation : mutations) {
    AssetRecord broken = restored;
    mutation.apply(broken);
    const auto broken_snapshot = make_snapshot({broken});
    FCO_REQUIRE(broken_snapshot.ok());
    const auto result = fco::evaluate_effect(effect, broken_snapshot.value());
    FCO_REQUIRE(result.ok());
    ::fco::test::check_true(!result.value(),
                            (std::string("restoration verification with ") + mutation.label +
                             " must not be satisfied")
                                .c_str(),
                            __FILE__, __LINE__);
  }
}

FCO_TEST(facility, evaluate_effect_rejects_malformed_effects) {
  const auto snapshot = make_snapshot({make_record("asset-1", "rack-1")});
  FCO_REQUIRE(snapshot.ok());
  const FacilitySnapshot& facility = snapshot.value();

  fco::EffectSpec effect = make_effect(fco::EffectKind::Unspecified, "asset-1");
  FCO_CHECK_ERROR(fco::evaluate_effect(effect, facility), fco::ErrorCode::InvalidEnumValue);

  effect = make_effect(fco::EffectKind::PowerStateChange, nullptr);
  FCO_CHECK_ERROR(fco::evaluate_effect(effect, facility), fco::ErrorCode::MalformedInput);

  effect = make_effect(fco::EffectKind::PowerStateChange, "asset-9");
  effect.expected_state = static_cast<std::uint32_t>(PowerState::On);
  FCO_CHECK_ERROR(fco::evaluate_effect(effect, facility), fco::ErrorCode::UnknownIdentity);

  const fco::EffectKind state_kinds[] = {
      fco::EffectKind::WorkloadStateChange, fco::EffectKind::MaintenanceStateChange,
      fco::EffectKind::PowerStateChange,    fco::EffectKind::CoolingStateChange,
      fco::EffectKind::NetworkStateChange,  fco::EffectKind::LifecycleStateChange,
  };
  for (fco::EffectKind kind : state_kinds) {
    effect = make_effect(kind, "asset-1");
    effect.expected_state = 0;
    FCO_CHECK_ERROR(fco::evaluate_effect(effect, facility), fco::ErrorCode::InvalidEnumValue);
    effect.expected_state = 200;
    FCO_CHECK_ERROR(fco::evaluate_effect(effect, facility), fco::ErrorCode::InvalidEnumValue);
  }
}

// ---------------------------------------------------------------------------
// project_action.
// ---------------------------------------------------------------------------
FCO_TEST(facility, project_action_rack_replacement_chain) {
  AssetRecord record = make_record("asset-1", "rack-1");
  FCO_CHECK(record.complete());
  FCO_CHECK_EQ(record.lifecycle_generation.value(), std::uint64_t{10});
  FCO_CHECK_EQ(record.maintenance_generation.value(), std::uint64_t{4});
  FCO_CHECK_EQ(record.hardware_generation.value(), std::uint64_t{2});

  const auto drain = fco::project_action(record, make_request(ActionKind::DrainWorkloads, record));
  FCO_REQUIRE(drain.ok());
  FCO_CHECK_EQ(drain.value().workloads, WorkloadState::Drained);
  FCO_CHECK_EQ(drain.value().lifecycle, AssetLifecycle::Draining);
  FCO_CHECK_EQ(drain.value().lifecycle_generation.value(), std::uint64_t{11});
  FCO_CHECK_EQ(drain.value().maintenance_generation.value(), std::uint64_t{4});

  const auto isolate =
      fco::project_action(drain.value(), make_request(ActionKind::MaintenanceIsolate, drain.value()));
  FCO_REQUIRE(isolate.ok());
  FCO_CHECK_EQ(isolate.value().maintenance, MaintenanceState::Isolated);
  FCO_CHECK_EQ(isolate.value().lifecycle, AssetLifecycle::Maintenance);
  FCO_CHECK_EQ(isolate.value().maintenance_generation.value(), std::uint64_t{5});
  FCO_CHECK_EQ(isolate.value().lifecycle_generation.value(), std::uint64_t{12});

  const auto power_down =
      fco::project_action(isolate.value(), make_request(ActionKind::PowerDownAsset, isolate.value()));
  FCO_REQUIRE(power_down.ok());
  FCO_CHECK_EQ(power_down.value().power, PowerState::Off);
  FCO_CHECK_EQ(power_down.value().lifecycle_generation.value(), std::uint64_t{12});
  FCO_CHECK_EQ(power_down.value().hardware_generation.value(), std::uint64_t{2});

  const auto cooling_isolate = fco::project_action(
      power_down.value(), make_request(ActionKind::CoolingIsolate, power_down.value()));
  FCO_REQUIRE(cooling_isolate.ok());
  FCO_CHECK_EQ(cooling_isolate.value().cooling, CoolingState::Isolated);
  FCO_CHECK_EQ(cooling_isolate.value().lifecycle_generation.value(), std::uint64_t{12});

  const auto quiesce = fco::project_action(cooling_isolate.value(),
                                           make_request(ActionKind::NetworkQuiesce,
                                                        cooling_isolate.value()));
  FCO_REQUIRE(quiesce.ok());
  FCO_CHECK_EQ(quiesce.value().network, NetworkState::Quiesced);

  const auto swap =
      fco::project_action(quiesce.value(), make_request(ActionKind::HardwareSwap, quiesce.value()));
  FCO_REQUIRE(swap.ok());
  FCO_CHECK_EQ(swap.value().hardware_generation.value(), std::uint64_t{3});
  FCO_CHECK_EQ(swap.value().lifecycle, AssetLifecycle::Installed);
  FCO_CHECK_EQ(swap.value().power, PowerState::Off);
  FCO_CHECK_EQ(swap.value().workloads, WorkloadState::Absent);
  FCO_CHECK_EQ(swap.value().lifecycle_generation.value(), std::uint64_t{13});
  FCO_CHECK_EQ(swap.value().firmware_generation.value(), std::uint64_t{5});

  const auto power_up =
      fco::project_action(swap.value(), make_request(ActionKind::PowerUpAsset, swap.value()));
  FCO_REQUIRE(power_up.ok());
  FCO_CHECK_EQ(power_up.value().power, PowerState::On);

  const auto cooling_restore = fco::project_action(
      power_up.value(), make_request(ActionKind::CoolingRestore, power_up.value()));
  FCO_REQUIRE(cooling_restore.ok());
  FCO_CHECK_EQ(cooling_restore.value().cooling, CoolingState::Normal);

  const auto network_restore = fco::project_action(
      cooling_restore.value(), make_request(ActionKind::NetworkRestore, cooling_restore.value()));
  FCO_REQUIRE(network_restore.ok());
  FCO_CHECK_EQ(network_restore.value().network, NetworkState::Attached);

  const auto recommission = fco::project_action(
      network_restore.value(), make_request(ActionKind::Recommission, network_restore.value()));
  FCO_REQUIRE(recommission.ok());
  const AssetRecord& final_record = recommission.value();
  FCO_CHECK_EQ(final_record.lifecycle, AssetLifecycle::Active);
  FCO_CHECK_EQ(final_record.lifecycle_generation.value(), std::uint64_t{14});
  FCO_CHECK_EQ(final_record.maintenance_generation.value(), std::uint64_t{5});
  FCO_CHECK_EQ(final_record.hardware_generation.value(), std::uint64_t{3});
  FCO_CHECK_EQ(final_record.firmware_generation.value(), std::uint64_t{5});
  FCO_CHECK_EQ(final_record.capacity_generation.value(), std::uint64_t{6});
  FCO_CHECK_EQ(final_record.power, PowerState::On);
  FCO_CHECK_EQ(final_record.cooling, CoolingState::Normal);
  FCO_CHECK_EQ(final_record.network, NetworkState::Attached);
  FCO_CHECK_EQ(final_record.maintenance, MaintenanceState::Isolated);
  FCO_CHECK_EQ(final_record.workloads, WorkloadState::Absent);
  FCO_CHECK_EQ(final_record.id.to_string(), std::string("asset-1"));
}

FCO_TEST(facility, project_action_rejects_mismatched_and_incomplete_requests) {
  const AssetRecord record = make_record("asset-1", "rack-1");

  ActionRequest mismatched = make_request(ActionKind::PowerDownAsset, record);
  mismatched.asset = AssetId("asset-2");
  FCO_CHECK_ERROR(fco::project_action(record, mismatched), fco::ErrorCode::ImpossibleCombination);

  ActionRequest incomplete = make_request(ActionKind::PowerDownAsset, record);
  incomplete.asset = AssetId{};
  FCO_CHECK_ERROR(fco::project_action(record, incomplete), fco::ErrorCode::MalformedInput);

  ActionRequest no_rack = make_request(ActionKind::PowerDownAsset, record);
  no_rack.rack = RackId{};
  FCO_CHECK_ERROR(fco::project_action(record, no_rack), fco::ErrorCode::MalformedInput);

  ActionRequest unset_kind = make_request(ActionKind::PowerDownAsset, record);
  unset_kind.kind = ActionKind::Unspecified;
  FCO_CHECK_ERROR(fco::project_action(record, unset_kind), fco::ErrorCode::MalformedInput);

  ActionRequest no_generation = make_request(ActionKind::PowerDownAsset, record);
  no_generation.lifecycle_generation = fco::LifecycleGeneration{};
  FCO_CHECK_ERROR(fco::project_action(record, no_generation), fco::ErrorCode::MalformedInput);
}

FCO_TEST(facility, project_action_capacity_and_lifecycle_details) {
  AssetRecord record = make_record("asset-1", "rack-1");
  record.capacity_total_units = 100;
  record.capacity_reserved_units = 10;

  ActionRequest reserve = make_request(ActionKind::CapacityReserve, record);
  reserve.target_value = 30;
  const auto reserved = fco::project_action(record, reserve);
  FCO_REQUIRE(reserved.ok());
  FCO_CHECK_EQ(reserved.value().capacity_reserved_units, std::uint32_t{40});
  FCO_CHECK_EQ(reserved.value().capacity_generation.value(), std::uint64_t{7});
  FCO_CHECK_EQ(reserved.value().capacity_available_units(), std::uint64_t{60});

  ActionRequest over_reserve = make_request(ActionKind::CapacityReserve, record);
  over_reserve.target_value = 91;
  FCO_CHECK_ERROR(fco::project_action(record, over_reserve), fco::ErrorCode::PreconditionUnsatisfied);

  ActionRequest release = make_request(ActionKind::CapacityRelease, record);
  release.target_value = 10;
  const auto released = fco::project_action(record, release);
  FCO_REQUIRE(released.ok());
  FCO_CHECK_EQ(released.value().capacity_reserved_units, std::uint32_t{0});
  FCO_CHECK_EQ(released.value().capacity_generation.value(), std::uint64_t{7});

  ActionRequest over_release = make_request(ActionKind::CapacityRelease, record);
  over_release.target_value = 11;
  FCO_CHECK_ERROR(fco::project_action(record, over_release), fco::ErrorCode::PreconditionUnsatisfied);

  ActionRequest transition = make_request(ActionKind::LifecycleTransition, record);
  transition.target_state = static_cast<std::uint32_t>(AssetLifecycle::Maintenance);
  const auto transitioned = fco::project_action(record, transition);
  FCO_REQUIRE(transitioned.ok());
  FCO_CHECK_EQ(transitioned.value().lifecycle, AssetLifecycle::Maintenance);
  FCO_CHECK_EQ(transitioned.value().lifecycle_generation.value(), std::uint64_t{11});

  transition.target_state = 0;
  FCO_CHECK_ERROR(fco::project_action(record, transition), fco::ErrorCode::InvalidEnumValue);
  transition.target_state = 13;  // one past kAssetLifecycleMax
  FCO_CHECK_ERROR(fco::project_action(record, transition), fco::ErrorCode::InvalidEnumValue);

  ActionRequest firmware = make_request(ActionKind::FirmwareStage, record);
  const auto staged = fco::project_action(record, firmware);
  FCO_REQUIRE(staged.ok());
  FCO_CHECK_EQ(staged.value().firmware_generation.value(), std::uint64_t{6});
  FCO_CHECK_EQ(staged.value().lifecycle_generation.value(), std::uint64_t{10});

  AssetRecord leased = record;
  leased.tenant = fco::TenantId("tenant-1");
  leased.capacity_reserved_units = 25;
  ActionRequest decommission = make_request(ActionKind::Decommission, leased);
  const auto decommissioned = fco::project_action(leased, decommission);
  FCO_REQUIRE(decommissioned.ok());
  FCO_CHECK_EQ(decommissioned.value().lifecycle, AssetLifecycle::Decommissioned);
  FCO_CHECK(!decommissioned.value().has_tenant());
  FCO_CHECK_EQ(decommissioned.value().capacity_reserved_units, std::uint32_t{0});
  FCO_CHECK_EQ(decommissioned.value().lifecycle_generation.value(), std::uint64_t{11});

  for (ActionKind kind : {ActionKind::VerifyRestoration, ActionKind::RecordNoOp}) {
    const auto unchanged = fco::project_action(record, make_request(kind, record));
    FCO_REQUIRE(unchanged.ok());
    FCO_CHECK_EQ(unchanged.value().digest(), record.digest());
  }
}

FCO_TEST_MAIN
