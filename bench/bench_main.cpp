// Benchmark harness.
//
// Methodology, stated here because the numbers are meaningless without it:
//   * Every measurement is of COMPLETED operations. Nothing here times work that
//     was merely submitted or enqueued.
//   * Durable operations include their full durable cost: encoding, staging,
//     flushing, read-back verification, atomic publication, manifest
//     publication, fencing advance and retention. The clock surrounds the whole
//     commit, not the in-memory part of it.
//   * Each configuration is warmed up before it is measured, and the measured
//     loop contains only the operation under test.
//   * Provenance is labelled explicitly: results that depend on the simulated
//     domain authorities are SYNTHETIC, because no physical data-center hardware
//     is involved. The durable cost measured here is REAL: it is real filesystem
//     I/O through the real store.
//   * Timings come from std::chrono::steady_clock on this host and are not
//     comparable across machines.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "fco/engine.hpp"
#include "fco/planner.hpp"
#include "fco/sim.hpp"
#include "fco/store.hpp"

namespace {

namespace fs = std::filesystem;

class TempRoot {
 public:
  TempRoot() {
    static unsigned counter = 0;
    ++counter;
    std::ostringstream name;
    name << "fco-bench-" << counter << '-' << std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count();
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

struct Summary {
  std::string name;
  std::string provenance;
  std::uint64_t operations = 0;
  double total_seconds = 0.0;
  double p50_micros = 0.0;
  double p99_micros = 0.0;
  double max_micros = 0.0;

  [[nodiscard]] double ops_per_second() const {
    return total_seconds > 0.0 ? static_cast<double>(operations) / total_seconds : 0.0;
  }
};

[[nodiscard]] double percentile(const std::vector<double>& sorted, double fraction) {
  if (sorted.empty()) return 0.0;
  const double position = fraction * static_cast<double>(sorted.size() - 1);
  const std::size_t index = static_cast<std::size_t>(position);
  return sorted[std::min(index, sorted.size() - 1)];
}

[[nodiscard]] Summary summarize(std::string name, std::string provenance,
                                std::vector<double> samples, double total_seconds) {
  Summary summary;
  summary.name = std::move(name);
  summary.provenance = std::move(provenance);
  summary.operations = static_cast<std::uint64_t>(samples.size());
  summary.total_seconds = total_seconds;
  std::sort(samples.begin(), samples.end());
  summary.p50_micros = percentile(samples, 0.50);
  summary.p99_micros = percentile(samples, 0.99);
  summary.max_micros = samples.empty() ? 0.0 : samples.back();
  return summary;
}

void print(const Summary& summary) {
  std::cout << summary.name << " [" << summary.provenance << "]\n";
  std::cout << "  operations:      " << summary.operations << '\n';
  std::cout << "  wall seconds:    " << summary.total_seconds << '\n';
  std::cout << "  completed ops/s: " << summary.ops_per_second() << '\n';
  std::cout << "  latency p50 us:  " << summary.p50_micros << '\n';
  std::cout << "  latency p99 us:  " << summary.p99_micros << '\n';
  std::cout << "  latency max us:  " << summary.max_micros << '\n';
}

[[nodiscard]] fco::GenerationSet generations_for(std::uint64_t epoch) {
  fco::GenerationSet generations;
  generations.facility_epoch = fco::FacilityEpoch(epoch);
  generations.policy = fco::PolicyGeneration(2);
  generations.dependency = fco::DependencyGeneration(5);
  generations.topology = fco::TopologyGeneration(7);
  generations.capacity = fco::CapacityGeneration(3);
  generations.lifecycle = fco::LifecycleGeneration(4);
  generations.maintenance = fco::MaintenanceGeneration(6);
  generations.power = fco::PowerGeneration(8);
  generations.cooling = fco::CoolingGeneration(9);
  generations.asi = fco::AsiGeneration(11);
  generations.dfi = fco::DfiGeneration(12);
  return generations;
}

[[nodiscard]] Summary bench_commit(const fs::path& root, std::uint64_t operations) {
  TempRoot temp;
  auto store = fco::DurableStore::open(temp.path());
  if (!store.ok()) {
    std::cerr << "commit bench: " << fco::describe(store.error()) << '\n';
    return Summary{"durable commit", "REAL filesystem durability", 0, 0.0, 0.0, 0.0, 0.0};
  }
  (void)root;

  fco::OrchestratorState state;
  state.facility = fco::make_example_facility(fco::SiteId("site-alpha"), fco::FacilityEpoch(4),
                                              generations_for(4));
  state.facility_epoch = state.facility.epoch();
  state.generations = state.facility.generations();
  state.facility_revision = fco::FacilityRevision(1);
  state.incarnation = fco::IncarnationId(1);
  state.control_epoch = fco::ControlEpoch(1);
  state.actor = fco::PrincipalId("principal-facility-ops");
  state.policy = fco::PolicyId("policy-change-control-v1");
  state.policy_digest = fco::Sha256::of("policy-change-control-v1");

  constexpr std::uint64_t warmup = 8;
  for (std::uint64_t index = 0; index < warmup; ++index) {
    state.revision = fco::Revision(store.value().revision().value() + 1);
    state.commit_sequence = fco::CommitSequence(store.value().commit_sequence().value() + 1);
    state.updated_at_micros = index;
    auto committed = store.value().commit(state);
    if (!committed.ok()) {
      std::cerr << "commit bench warmup: " << fco::describe(committed.error()) << '\n';
      break;
    }
  }

  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(operations));
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < operations; ++index) {
    state.revision = fco::Revision(store.value().revision().value() + 1);
    state.commit_sequence = fco::CommitSequence(store.value().commit_sequence().value() + 1);
    state.updated_at_micros = warmup + index;
    const auto begin = std::chrono::steady_clock::now();
    auto committed = store.value().commit(state);
    const auto end = std::chrono::steady_clock::now();
    if (!committed.ok()) {
      std::cerr << "commit bench: " << fco::describe(committed.error()) << '\n';
      break;
    }
    samples.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
  }
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - start).count();
  return summarize("durable commit (encode, stage, flush, verify, publish, manifest, fencing, "
                   "retention)",
                   "REAL filesystem durability",
                   std::move(samples), seconds);
}

[[nodiscard]] Summary bench_plan(std::uint64_t operations) {
  TempRoot temp;
  fco::ManualClock clock(1000000);
  fco::DomainRegistry registry;
  auto orchestrator = fco::Orchestrator::open(temp.path(), clock, registry);
  if (!orchestrator.ok()) {
    std::cerr << "plan bench: " << fco::describe(orchestrator.error()) << '\n';
    return Summary{"plan lifecycle", "SYNTHETIC plant, REAL durable store", 0, 0.0, 0.0, 0.0, 0.0};
  }
  fco::FacilitySnapshot facility = fco::make_example_facility(
      fco::SiteId("site-alpha"), fco::FacilityEpoch(4), generations_for(4));
  auto initialized = orchestrator.value().initialize(
      std::move(facility), fco::IncarnationId(1), fco::ControlEpoch(1),
      fco::PrincipalId("principal-facility-ops"), fco::PolicyId("policy-change-control-v1"),
      fco::Sha256::of("policy-change-control-v1"));
  if (!initialized.ok()) {
    std::cerr << "plan bench init: " << fco::describe(initialized.error()) << '\n';
    return Summary{"plan lifecycle", "SYNTHETIC plant, REAL durable store", 0, 0.0, 0.0, 0.0, 0.0};
  }

  fco::SimulatedPlant plant(orchestrator.value().state().facility);
  fco::SimulatedAuthoritySet authorities(plant, fco::IncarnationId(1), fco::ControlEpoch(1));
  for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(fco::kDomainKindMax); ++kind) {
    fco::DomainAuthority* authority = authorities.find(static_cast<fco::DomainKind>(kind));
    if (authority != nullptr) registry.add(authority);
  }

  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(operations));
  std::uint64_t completed = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < operations; ++index) {
    fco::SynthesisInput input;
    input.kind = fco::ChangeKind("power-maintenance");
    input.id = fco::ChangeRequestId("req-bench-" + std::to_string(index));
    input.site = orchestrator.value().state().facility.site();
    input.intent = "benchmark lifecycle";
    input.scope.site = input.site;
    input.scope.rack = fco::RackId("rack-a1");
    input.scope.assets.push_back(fco::AssetId("rack-a1-node-1"));
    input.facility_epoch = orchestrator.value().state().facility_epoch;
    input.actor = orchestrator.value().state().actor;
    input.revision = fco::Revision(1);
    input.bound_generations = orchestrator.value().state().generations;
    input.evidence_digest = orchestrator.value().state().facility.digest();

    const auto begin = std::chrono::steady_clock::now();
    auto request = orchestrator.value().synthesize(input);
    if (!request.ok()) break;
    auto plan_id = orchestrator.value().create_plan(request.value());
    if (!plan_id.ok()) break;
    if (!orchestrator.value().mark_evaluated(plan_id.value()).ok()) break;
    if (!orchestrator.value().authorize(plan_id.value()).ok()) break;
    auto executed = orchestrator.value().execute_all(plan_id.value());
    if (!executed.ok()) break;
    const auto plan = orchestrator.value().plan(plan_id.value());
    if (!plan.ok()) break;
    if (plan.value().state != fco::PlanState::Completed) break;
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
    ++completed;
  }
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - start).count();
  (void)completed;
  return summarize("plan lifecycle (synthesise, create, evaluate, authorize, execute to "
                   "completion, verify)",
                   "SYNTHETIC plant, REAL durable store",
                   std::move(samples), seconds);
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t commit_operations = 300;
  std::uint64_t plan_operations = 20;
  if (argc > 1) {
    const auto parsed = std::strtoull(argv[1], nullptr, 10);
    if (parsed > 0) commit_operations = parsed;
  }
  if (argc > 2) {
    const auto parsed = std::strtoull(argv[2], nullptr, 10);
    if (parsed > 0) plan_operations = parsed;
  }

  std::cout << "Facility Change Orchestrator benchmark harness\n";
  std::cout << "host: " << (sizeof(void*) == 8 ? "64-bit" : "32-bit") << " process\n";
  std::cout << "methodology: completed operations only; durable cost included; warmed up "
               "before measurement; steady_clock; single run; not comparable across hosts.\n";
  std::cout << "provenance: the domain outcomes are SYNTHETIC (no physical hardware); the "
               "durable filesystem cost is REAL.\n\n";

  const Summary commit = bench_commit(fs::path(), commit_operations);
  print(commit);
  std::cout << '\n';
  const Summary plan = bench_plan(plan_operations);
  print(plan);
  return 0;
}
