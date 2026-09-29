#pragma once

// Rendering: DAG, explanation, status and closure reports.
//
// Rendering never consults anything the orchestrator has not already observed.
// A rendered plan shows requested and verified state separately, because a
// requested state is not an observed state.

#include <string>
#include <vector>

#include "fco/engine.hpp"
#include "fco/model.hpp"
#include "fco/planner.hpp"
#include "fco/state.hpp"

namespace fco {

[[nodiscard]] std::string render_plan(const ChangePlan& plan);
[[nodiscard]] std::string render_evaluation(const ChangePlan& plan,
                                            const PlanEvaluation& evaluation);
[[nodiscard]] std::string render_state(const OrchestratorState& state,
                                       const RecoveryReport& recovery);
[[nodiscard]] std::string render_closure(const OrchestratorState& state,
                                         const ClosureReport& report);
[[nodiscard]] std::string render_recovery(const RecoverySummary& summary);
[[nodiscard]] std::string render_evidence(const EvidenceRecord& record);
[[nodiscard]] std::string render_facility(const FacilitySnapshot& facility);

}  // namespace fco
