#pragma once

// The orchestrator.
//
// The orchestrator is the composition runtime: it owns the authoritative
// facility view, the plan ledger, the attempt history and the evidence ledger,
// and it commits every transition to durable storage before it acts on it. It
// never performs a domain action itself. It submits typed requests to the
// authority that owns the action and accepts only verified observations.
//
// Concurrency model: the orchestrator is a single-threaded component. It holds
// no locks of its own; the durable store holds one exclusive OS-level writer
// lock for the process lifetime, and domain authorities are always called
// outside every lock and never re-enter the orchestrator. There are no
// callbacks under lock, no lock ordering to invert, and no worker join path
// that can wait on state the store owns.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fco/authority.hpp"
#include "fco/digest.hpp"
#include "fco/domain.hpp"
#include "fco/error.hpp"
#include "fco/facility.hpp"
#include "fco/model.hpp"
#include "fco/planner.hpp"
#include "fco/ports.hpp"
#include "fco/state.hpp"
#include "fco/store.hpp"
#include "fco/strong.hpp"

namespace fco {

// Non-owning registry of the domain authorities this orchestrator may talk to.
class DomainRegistry {
 public:
  DomainRegistry() = default;

  void add(DomainAuthority* authority);
  void clear() noexcept { authorities_.clear(); }
  [[nodiscard]] DomainAuthority* find(DomainKind domain) const noexcept;
  [[nodiscard]] std::vector<DomainKind> domains() const;
  [[nodiscard]] std::size_t size() const noexcept { return authorities_.size(); }

 private:
  std::map<DomainKind, DomainAuthority*> authorities_;
};

struct ExecuteResult {
  bool issued = false;
  StepId step;
  AttemptId attempt;
  StepStatus step_status = StepStatus::Unspecified;
  AttemptStatus attempt_status = AttemptStatus::Unspecified;
  std::string detail;

  [[nodiscard]] std::string describe() const;
};

struct AttemptClassification {
  PlanId plan;
  StepId step;
  AttemptId attempt;
  AttemptStatus status = AttemptStatus::Unspecified;
  std::string disposition;
};

struct RecoverySummary {
  std::vector<AttemptClassification> attempts;
  std::size_t requiring_resolution = 0;
  std::size_t plans_paused = 0;
  std::size_t plans_replan_required = 0;

  [[nodiscard]] std::string describe() const;
};

struct ClosureReport {
  bool store_initialized = false;
  Revision revision;
  CommitSequence commit_sequence;
  std::uint64_t generation = 0;
  std::size_t plans_total = 0;
  std::size_t plans_completed = 0;
  std::size_t plans_terminal_unfinished = 0;
  std::size_t plans_open = 0;
  std::size_t verified_steps = 0;
  std::size_t total_steps = 0;
  std::size_t unresolved_attempts = 0;
  std::size_t non_compensable_applied = 0;
  std::size_t evidence_records = 0;
  std::size_t facility_assets = 0;
  std::vector<PlanId> open_plans;
  std::vector<std::string> notes;

  [[nodiscard]] bool closed() const noexcept { return plans_open == 0 && unresolved_attempts == 0; }
  [[nodiscard]] std::string describe() const;
};

class Orchestrator {
 public:
  Orchestrator(Orchestrator&&) noexcept;
  Orchestrator& operator=(Orchestrator&&) noexcept;
  Orchestrator(const Orchestrator&) = delete;
  Orchestrator& operator=(const Orchestrator&) = delete;
  ~Orchestrator();

  [[nodiscard]] static Result<Orchestrator> open(const std::filesystem::path& root, Clock& clock,
                                                 DomainRegistry& domains,
                                                 CommitHooks hooks = CommitHooks{});

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const OrchestratorState& state() const;
  [[nodiscard]] const RecoveryReport& store_recovery() const;
  [[nodiscard]] Clock& clock() const noexcept;

  // --- lifecycle ---------------------------------------------------------
  [[nodiscard]] Status initialize(FacilitySnapshot facility, IncarnationId incarnation,
                                  ControlEpoch control_epoch, PrincipalId actor, PolicyId policy,
                                  Digest policy_digest);

  // Replaces the observed facility. This is an observation, not an authority:
  // it never authorizes anything, and moving the observed generations forward is
  // exactly what fences plans bound to the older ones.
  [[nodiscard]] Status observe_facility(FacilitySnapshot facility, std::string note);

  // Reclassifies in-flight attempts after a restart. Never re-issues anything.
  [[nodiscard]] Result<RecoverySummary> recover();

  // Takes over authority with a strictly newer control epoch. Plans that were
  // actively executing under the superseded authority are moved to
  // ReplanRequired; plans that were merely authorized keep their binding and are
  // fenced the moment execution is attempted. Nothing is silently inherited.
  [[nodiscard]] Status assume_authority(IncarnationId incarnation, ControlEpoch control_epoch,
                                        PrincipalId actor, PolicyId policy, Digest policy_digest);

  // --- planning ----------------------------------------------------------
  [[nodiscard]] Result<ChangeRequest> synthesize(const SynthesisInput& input) const;
  [[nodiscard]] Result<PlanId> create_plan(const ChangeRequest& request);
  [[nodiscard]] Result<ChangePlan> plan(const PlanId& id) const;
  [[nodiscard]] PlanEvaluation evaluate(const PlanId& id) const;
  [[nodiscard]] Status mark_evaluated(const PlanId& id);
  [[nodiscard]] Status authorize(const PlanId& id);
  [[nodiscard]] Status replan(const PlanId& id, std::string reason);
  [[nodiscard]] Status plan_pause(const PlanId& id, std::string reason);
  [[nodiscard]] Status plan_resume(const PlanId& id);
  [[nodiscard]] Status plan_abort(const PlanId& id, std::string reason);

  // --- execution ---------------------------------------------------------
  [[nodiscard]] Result<ExecuteResult> execute_next(const PlanId& id);
  [[nodiscard]] Result<std::vector<ExecuteResult>> execute_all(const PlanId& id);
  [[nodiscard]] Status ingest_evidence(const EvidenceRecord& record);
  [[nodiscard]] Status resolve_attempt(const AttemptId& id, AttemptStatus resolution,
                                       std::string note);
  [[nodiscard]] Status rollback(const PlanId& id, std::string reason);

  // --- reporting ---------------------------------------------------------
  [[nodiscard]] ClosureReport closure() const;
  [[nodiscard]] Result<std::string> render_plan(const PlanId& id) const;
  [[nodiscard]] Result<std::string> render_evaluation(const PlanId& id) const;
  [[nodiscard]] std::string render_status() const;
  [[nodiscard]] std::string render_closure() const;

 private:
  Orchestrator();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fco
