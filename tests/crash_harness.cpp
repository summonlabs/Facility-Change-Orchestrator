// Crash-consistency harness.
//
// Commits one real revision of durable state and then terminates this process
// abruptly at a chosen commit stage, without unwinding, flushing, or running any
// destructor. The process test suite runs this for every stage and then proves
// that a fresh process recovers exactly one fully verified generation.

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "fco/engine.hpp"
#include "fco/ports.hpp"
#include "fco/store.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

[[noreturn]] void terminate_now() {
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), 70);
#else
  std::_Exit(70);
#endif
  std::abort();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: fco_crash_harness <root> <commit-stage>\n";
    return 2;
  }
  const std::string root = argv[1];
  auto stage = fco::parse_commit_stage(argv[2]);
  if (!stage.ok()) {
    std::cerr << "stage: " << fco::describe(stage.error()) << '\n';
    return 2;
  }

  const fco::CommitStage target = stage.value();
  fco::CommitHooks hooks;
  hooks.on_stage = [target](fco::CommitStage reached) {
    if (reached != target) return;
    std::cout << "reached " << fco::to_string(reached) << '\n';
    std::cout.flush();
    std::cerr.flush();
    terminate_now();
  };

  auto path = fco::path_from_utf8(root);
  if (!path.ok()) {
    std::cerr << "path: " << fco::describe(path.error()) << '\n';
    return 2;
  }

  fco::SystemClock clock;
  fco::DomainRegistry registry;
  auto orchestrator = fco::Orchestrator::open(path.value(), clock, registry, hooks);
  if (!orchestrator.ok()) {
    std::cerr << "open: " << fco::describe(orchestrator.error()) << '\n';
    return 3;
  }
  if (!orchestrator.value().state().initialized()) {
    std::cerr << "the store must be initialized before the crash harness runs\n";
    return 3;
  }

  const fco::FacilitySnapshot facility = orchestrator.value().state().facility;
  const auto status = orchestrator.value().observe_facility(facility, "crash harness revision");
  if (!status.ok()) {
    std::cerr << "commit: " << fco::describe(status.error()) << '\n';
    return 4;
  }
  std::cout << "committed\n";
  return 0;
}
