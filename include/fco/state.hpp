#pragma once

// Authoritative orchestrator state.
//
// This is the single value that a durable generation contains. It is a
// snapshot, not an event log: recovery therefore selects exactly one
// authoritative generation instead of replaying and merging partial histories.

#include <cstdint>
#include <map>
#include <string>

#include "fco/authority.hpp"
#include "fco/digest.hpp"
#include "fco/domain.hpp"
#include "fco/error.hpp"
#include "fco/evidence.hpp"
#include "fco/facility.hpp"
#include "fco/model.hpp"
#include "fco/strong.hpp"

namespace fco {

struct OrchestratorState {
  Revision revision;              // durable commit revision
  CommitSequence commit_sequence;
  FacilityEpoch facility_epoch;
  GenerationSet generations;
  FacilityRevision facility_revision;
  FacilitySnapshot facility;
  IncarnationId incarnation;
  ControlEpoch control_epoch;
  PrincipalId actor;
  PolicyId policy;
  Digest policy_digest;
  std::map<PlanId, ChangePlan> plans;
  EvidenceLedger evidence;
  std::uint64_t updated_at_micros = 0;

  [[nodiscard]] AuthorityContext authority() const;
  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] ChangePlan* find_plan(const PlanId& id) noexcept;
  [[nodiscard]] const ChangePlan* find_plan(const PlanId& id) const noexcept;

  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;
};

}  // namespace fco
