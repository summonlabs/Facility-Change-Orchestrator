#pragma once

// Authority and generation currency.
//
// Two independent things must be true before this runtime is allowed to move
// anything: the caller must still hold live authority (same incarnation, same
// control epoch, same actor), and every generation the request was planned
// against must still be current. Neither is inferred. A superseded control
// epoch is fenced, never silently inherited, and a plan bound to an older
// generation is fenced rather than re-interpreted against the new one.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fco/digest.hpp"
#include "fco/domain.hpp"
#include "fco/error.hpp"
#include "fco/strong.hpp"

namespace fco {

enum class GenerationKind : std::uint8_t {
  Unspecified = 0,
  FacilityEpoch = 1,
  Policy = 2,
  Dependency = 3,
  Topology = 4,
  Capacity = 5,
  Lifecycle = 6,
  Maintenance = 7,
  Power = 8,
  Cooling = 9,
  Asi = 10,
  Dfi = 11,
};
inline constexpr GenerationKind kGenerationKindMax = GenerationKind::Dfi;

[[nodiscard]] constexpr bool is_valid(GenerationKind value) noexcept {
  return static_cast<std::uint8_t>(value) >= 1u &&
         static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(kGenerationKindMax);
}
[[nodiscard]] std::string_view to_string(GenerationKind value) noexcept;
[[nodiscard]] Result<GenerationKind> parse_generation_kind(std::string_view text);

// The complete set of generations a plan (or a piece of evidence) is bound to.
struct GenerationSet {
  FacilityEpoch facility_epoch;
  PolicyGeneration policy;
  DependencyGeneration dependency;
  TopologyGeneration topology;
  CapacityGeneration capacity;
  LifecycleGeneration lifecycle;
  MaintenanceGeneration maintenance;
  PowerGeneration power;
  CoolingGeneration cooling;
  AsiGeneration asi;
  DfiGeneration dfi;

  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] std::uint64_t get(GenerationKind kind) const noexcept;
  void set(GenerationKind kind, std::uint64_t value) noexcept;
  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;
  [[nodiscard]] std::string describe() const;
};

enum class CurrencyOutcome : std::uint8_t {
  Unspecified = 0,
  Current = 1,
  Advanced = 2,
  Regressed = 3,
  Mixed = 4,
};

struct GenerationDelta {
  GenerationKind kind = GenerationKind::Unspecified;
  std::uint64_t bound = 0;
  std::uint64_t current = 0;
};

// Result of comparing a stored binding against an observed state. Both
// directions are reported: a generation that moved ahead means the binding is
// stale, and a generation that moved backwards means the observer is not
// trustworthy. Neither is treated as "no change".
struct CurrencyCheck {
  CurrencyOutcome outcome = CurrencyOutcome::Unspecified;
  std::vector<GenerationDelta> advanced;
  std::vector<GenerationDelta> regressed;

  [[nodiscard]] bool current() const noexcept { return outcome == CurrencyOutcome::Current; }
  [[nodiscard]] ErrorCode primary_code() const noexcept;
  [[nodiscard]] std::string subject_key() const;
  [[nodiscard]] std::string describe() const;
};

[[nodiscard]] CurrencyCheck check_currency(const GenerationSet& bound,
                                           const GenerationSet& observed);

// Who is acting, under which incarnation and control epoch, under which policy.
struct AuthorityContext {
  IncarnationId incarnation;
  ControlEpoch control_epoch;
  PrincipalId actor;
  PolicyId policy;
  Digest policy_digest;

  [[nodiscard]] bool complete() const noexcept;
  void hash_into(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest digest() const;
  [[nodiscard]] std::string describe() const;
};

// The generation an action legitimately advances when its effect is verified.
// A plan's own verified effects explain an advance of that generation; any other
// advance fences the remaining steps as stale. Verification and no-op steps
// advance nothing and are reported as GenerationKind::Unspecified.
[[nodiscard]] GenerationKind generation_advanced_by(ActionKind action) noexcept;

struct FencingDecision {
  bool permitted = false;
  ErrorCode code = ErrorCode::Ok;
  std::string subject;
  std::string message;

  [[nodiscard]] static FencingDecision allow();
  [[nodiscard]] static FencingDecision deny(ErrorCode code, std::string subject,
                                            std::string message);
  [[nodiscard]] std::string describe() const;
};

// Decides whether an action bound to `bound` may proceed while `current`
// holds. When several conditions hold at once the primary condition is chosen
// by the documented ErrorCode precedence, not by evaluation order.
[[nodiscard]] FencingDecision fence_authority(const AuthorityContext& bound,
                                              const AuthorityContext& current);

}  // namespace fco
