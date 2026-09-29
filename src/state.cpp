#include "fco/state.hpp"

namespace fco {

AuthorityContext OrchestratorState::authority() const {
  AuthorityContext context;
  context.incarnation = incarnation;
  context.control_epoch = control_epoch;
  context.actor = actor;
  context.policy = policy;
  context.policy_digest = policy_digest;
  return context;
}

bool OrchestratorState::initialized() const noexcept {
  return revision.valid() && facility_epoch.valid() && generations.complete() &&
         facility.valid() && incarnation.valid() && control_epoch.valid() && actor.valid() &&
         policy.valid();
}

ChangePlan* OrchestratorState::find_plan(const PlanId& id) noexcept {
  const auto it = plans.find(id);
  return it == plans.end() ? nullptr : &it->second;
}

const ChangePlan* OrchestratorState::find_plan(const PlanId& id) const noexcept {
  const auto it = plans.find(id);
  return it == plans.end() ? nullptr : &it->second;
}

void OrchestratorState::hash_into(Sha256& hasher) const noexcept {
  hasher.update_u64(revision.value());
  hasher.update_u64(commit_sequence.value());
  hasher.update_u64(facility_epoch.value());
  generations.hash_into(hasher);
  hasher.update_u64(facility_revision.value());
  facility.hash_into(hasher);
  hasher.update_u64(incarnation.value());
  hasher.update_u64(control_epoch.value());
  hasher.update(actor.value());
  hasher.update_u8(0);
  hasher.update(policy.value());
  hasher.update_u8(0);
  hasher.update(policy_digest.bytes().data(), policy_digest.bytes().size());
  hasher.update_u64(static_cast<std::uint64_t>(plans.size()));
  for (const auto& entry : plans) {
    // The key is hashed as well as the value, so a plan that is keyed by the
    // wrong identifier cannot produce the same state digest as a correct one.
    hasher.update(entry.first.value());
    hasher.update_u8(0);
    entry.second.hash_into(hasher);
  }
  {
    const Digest evidence_digest = evidence.digest();
    hasher.update(evidence_digest.bytes().data(), evidence_digest.bytes().size());
  }
  hasher.update_u64(updated_at_micros);
}

Digest OrchestratorState::digest() const {
  Sha256 hasher;
  hasher.update("fco/orchestrator-state/1");
  hash_into(hasher);
  return hasher.finish();
}

}  // namespace fco
