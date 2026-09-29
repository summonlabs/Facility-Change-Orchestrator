// Out-of-tree consumer of the installed Facility Change Orchestrator package.
//
// This program is built only against the installed prefix through
// find_package(fco CONFIG REQUIRED) and the imported target fco::fco. It
// exercises the installed artifact rather than the in-tree target: it reads the
// compiled-in release identity, hashes with the installed digest implementation,
// and drives a complete plan lifecycle through the installed orchestrator and
// the installed durable store.

#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include "fco/engine.hpp"
#include "fco/planner.hpp"
#include "fco/sim.hpp"
#include "fco/store.hpp"
#include "fco/version.hpp"

namespace {

namespace fs = std::filesystem;

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

}  // namespace

int main() {
  int failures = 0;
  auto expect = [&](bool condition, const std::string& what) {
    if (!condition) {
      std::cerr << "consumer FAIL: " << what << '\n';
      ++failures;
    }
  };

  std::cout << "consumer linked against " << fco::kProjectName << ' ' << fco::kVersionString
            << " (" << fco::kDccpRepositoryNumber << ")\n";
  expect(fco::kVersionString == std::string_view("1.0.0"), "installed version identity");
  expect(fco::kStorageFormatVersion == 1, "installed storage format version");

  // Real digest known-answer vector, so the installed binary is genuinely the
  // one under test rather than any other library with the same symbols.
  expect(fco::Sha256::of("abc").to_hex() ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "installed SHA-256 implementation");

  const fs::path root = fs::temp_directory_path() /
                        ("fco-consumer-" + std::to_string(std::chrono::steady_clock::now()
                                                              .time_since_epoch()
                                                              .count()));
  std::error_code code;
  fs::remove_all(root, code);

  {
    fco::ManualClock clock(1000000);
    fco::DomainRegistry registry;
    auto orchestrator = fco::Orchestrator::open(root, clock, registry);
    expect(orchestrator.ok(), "open durable store through the installed package");
    if (!orchestrator.ok()) {
      fs::remove_all(root, code);
      return 1;
    }

    auto facility = fco::make_example_facility(fco::SiteId("site-alpha"), fco::FacilityEpoch(4),
                                               generations_for(4));
    auto initialized = orchestrator.value().initialize(
        std::move(facility), fco::IncarnationId(1), fco::ControlEpoch(1),
        fco::PrincipalId("principal-facility-ops"), fco::PolicyId("policy-change-control-v1"),
        fco::Sha256::of("policy-change-control-v1"));
    expect(initialized.ok(), "initialize durable state");

    fco::SimulatedPlant plant(orchestrator.value().state().facility);
    fco::SimulatedAuthoritySet authorities(plant, fco::IncarnationId(1), fco::ControlEpoch(1));
    for (std::uint8_t kind = 1; kind <= static_cast<std::uint8_t>(fco::kDomainKindMax); ++kind) {
      fco::DomainAuthority* authority = authorities.find(static_cast<fco::DomainKind>(kind));
      if (authority != nullptr) registry.add(authority);
    }

    fco::SynthesisInput input;
    input.kind = fco::ChangeKind("rack-replacement");
    input.id = fco::ChangeRequestId("req-consumer");
    input.site = orchestrator.value().state().facility.site();
    input.intent = "consumer smoke change";
    input.scope.site = input.site;
    input.scope.rack = fco::RackId("rack-a1");
    input.scope.assets.push_back(fco::AssetId("rack-a1-node-1"));
    input.facility_epoch = orchestrator.value().state().facility_epoch;
    input.actor = orchestrator.value().state().actor;
    input.revision = fco::Revision(1);
    input.bound_generations = orchestrator.value().state().generations;
    input.evidence_digest = orchestrator.value().state().facility.digest();

    auto request = orchestrator.value().synthesize(input);
    expect(request.ok(), "synthesise a change request");
    if (request.ok()) {
      expect(request.value().steps.size() == 13, "synthesised step count");
      auto plan_id = orchestrator.value().create_plan(request.value());
      expect(plan_id.ok(), "create a plan");
      if (plan_id.ok()) {
        const fco::PlanEvaluation evaluation = orchestrator.value().evaluate(plan_id.value());
        expect(evaluation.accepted(), "dry evaluation is accepted");
        expect(orchestrator.value().mark_evaluated(plan_id.value()).ok(), "mark evaluated");
        expect(orchestrator.value().authorize(plan_id.value()).ok(), "authorize");
        auto executed = orchestrator.value().execute_all(plan_id.value());
        expect(executed.ok(), "execute every step");
        auto plan = orchestrator.value().plan(plan_id.value());
        expect(plan.ok() && plan.value().state == fco::PlanState::Completed,
               "plan reaches Completed");
        expect(plan.ok() && plan.value().unresolved_attempt_count() == 0,
               "no unresolved attempts remain");
      }
    }

    const fco::ClosureReport closure = orchestrator.value().closure();
    expect(closure.closed(), "closure reports a closed store");
    std::cout << "consumer closure: " << closure.describe() << '\n';
  }

  // Reopen in a fresh orchestrator to prove the installed store round-trips.
  {
    fco::ManualClock clock(2000000);
    fco::DomainRegistry registry;
    auto orchestrator = fco::Orchestrator::open(root, clock, registry);
    expect(orchestrator.ok(), "reopen the durable store");
    if (orchestrator.ok()) {
      expect(orchestrator.value().state().initialized(), "recovered state is initialized");
      expect(orchestrator.value().closure().plans_completed == 1,
             "the completed plan survived the restart");
    }
  }

  fs::remove_all(root, code);

  if (failures != 0) {
    std::cerr << "consumer: " << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "consumer: the installed artifact passed every exercise\n";
  return 0;
}
