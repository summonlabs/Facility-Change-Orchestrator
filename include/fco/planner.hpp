#pragma once

// Evaluation and replanning.
//
// Evaluation answers the core question for one request against one facility
// snapshot: which ordered plan is valid now, which domain authorities must act,
// which preconditions and verification gates apply, and which steps cannot run
// yet. Evaluation never authorizes and never mutates the world.

#include <cstdint>
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

struct StepReadiness {
  StepId step;
  bool eligible = false;  // every predecessor is verified
  bool ready = false;     // eligible and every precondition holds right now
  std::vector<std::string> blockages;
};

struct PlanEvaluation {
  Diagnostics diagnostics;
  std::vector<StepId> order;
  std::vector<StepReadiness> readiness;
  std::vector<StepId> initially_ready;
  std::vector<StepId> initially_blocked;
  std::vector<std::string> explanation;

  [[nodiscard]] bool accepted() const noexcept { return diagnostics.empty(); }
};

// Builds a Draft plan from a request. The request is fully validated first; a
// request with any defect yields its documented primary error and no plan.
[[nodiscard]] Result<ChangePlan> create_plan(const ChangeRequest& request,
                                             const FacilitySnapshot& snapshot,
                                             const AuthorityContext& authority,
                                             PlanRevision revision,
                                             FacilityRevision facility_revision,
                                             std::uint64_t now_micros);

// Pure evaluation. Reports the deterministic execution order, the readiness of
// every step against this snapshot, and one diagnostic per currently
// unsatisfiable precondition of an eligible step.
[[nodiscard]] PlanEvaluation evaluate_plan(const ChangePlan& plan,
                                           const FacilitySnapshot& snapshot);

// Reasons the plan may no longer proceed. An empty vector means the plan is
// still valid. Recorded self-advances are accepted only at the exact value this
// plan produced; anything else is fenced.
[[nodiscard]] std::vector<std::string> fencing_reasons(const ChangePlan& plan,
                                                       const FacilitySnapshot& snapshot,
                                                       const AuthorityContext& current,
                                                       FacilityRevision current_facility_revision);

// Steps that are eligible and ready to be issued, in deterministic order.
[[nodiscard]] std::vector<StepId> ready_steps(const ChangePlan& plan);

// --------------------------------------------------------------------------
// Change-kind templates. A request may name a recognised change kind and leave
// its steps empty; the planner then synthesises the domain step chain from the
// observed facility state. Synthesis is still a proposal: the resulting request
// passes exactly the same validation as a hand-written one.
// --------------------------------------------------------------------------
struct SynthesisInput {
  ChangeKind kind;
  ChangeRequestId id;
  SiteId site;
  std::string intent;
  PlanScope scope;
  FacilityEpoch facility_epoch;
  PrincipalId actor;
  Revision revision;
  GenerationSet bound_generations;
  Digest evidence_digest;
  std::uint32_t capacity_units = 0;
};

[[nodiscard]] bool is_known_change_kind(std::string_view kind) noexcept;
[[nodiscard]] std::vector<std::string> known_change_kinds();
[[nodiscard]] Result<ChangeRequest> synthesize_request(const SynthesisInput& input,
                                                       const FacilitySnapshot& snapshot);

}  // namespace fco
