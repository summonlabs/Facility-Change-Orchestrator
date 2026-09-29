#pragma once

// Strongly typed identities and counters.
//
// Every externally meaningful value in this runtime is carried by a distinct
// type. AssetId and RackId are not interchangeable, LifecycleGeneration and
// CapacityGeneration are not interchangeable, and neither is implicitly
// convertible to an integer or a string. Confusion between them is therefore a
// compile error rather than a runtime defect.
//
// A default-constructed strong value is *unset*: valid() is false. Unset values
// are rejected at every boundary (parse, decode, lookup, plan binding) and are
// never silently substituted with zero, an empty string, or a neighbouring
// value.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "fco/error.hpp"

namespace fco {

#define FCO_DECLARE_TAG(TagType, Text)            \
  struct TagType {                                \
    static constexpr std::string_view kind_name = Text; \
  }

// --- identifiers -----------------------------------------------------------
FCO_DECLARE_TAG(AssetIdTag, "asset-id");
FCO_DECLARE_TAG(RackIdTag, "rack-id");
FCO_DECLARE_TAG(SiteIdTag, "site-id");
FCO_DECLARE_TAG(TenantIdTag, "tenant-id");
FCO_DECLARE_TAG(StepIdTag, "step-id");
FCO_DECLARE_TAG(PlanIdTag, "plan-id");
FCO_DECLARE_TAG(ChangeRequestIdTag, "change-request-id");
FCO_DECLARE_TAG(AttemptIdTag, "attempt-id");
FCO_DECLARE_TAG(EvidenceIdTag, "evidence-id");
FCO_DECLARE_TAG(PrincipalIdTag, "principal-id");
FCO_DECLARE_TAG(PolicyIdTag, "policy-id");
FCO_DECLARE_TAG(ChangeKindTag, "change-kind");
FCO_DECLARE_TAG(ResolutionNoteTag, "resolution-note");

// --- counters, generations, epochs ----------------------------------------
FCO_DECLARE_TAG(FacilityEpochTag, "facility-epoch");
FCO_DECLARE_TAG(ControlEpochTag, "control-epoch");
FCO_DECLARE_TAG(IncarnationIdTag, "incarnation-id");
FCO_DECLARE_TAG(LifecycleGenerationTag, "lifecycle-generation");
FCO_DECLARE_TAG(HardwareGenerationTag, "hardware-generation");
FCO_DECLARE_TAG(FirmwareGenerationTag, "firmware-generation");
FCO_DECLARE_TAG(PolicyGenerationTag, "policy-generation");
FCO_DECLARE_TAG(DependencyGenerationTag, "dependency-generation");
FCO_DECLARE_TAG(CapacityGenerationTag, "capacity-generation");
FCO_DECLARE_TAG(TopologyGenerationTag, "topology-generation");
FCO_DECLARE_TAG(MaintenanceGenerationTag, "maintenance-generation");
FCO_DECLARE_TAG(PowerGenerationTag, "power-generation");
FCO_DECLARE_TAG(CoolingGenerationTag, "cooling-generation");
FCO_DECLARE_TAG(AsiGenerationTag, "asi-generation");
FCO_DECLARE_TAG(DfiGenerationTag, "dfi-generation");
FCO_DECLARE_TAG(FacilityRevisionTag, "facility-revision");
FCO_DECLARE_TAG(RevisionTag, "revision");
FCO_DECLARE_TAG(PlanRevisionTag, "plan-revision");
FCO_DECLARE_TAG(CommitSequenceTag, "commit-sequence");
FCO_DECLARE_TAG(ObservationSequenceTag, "observation-sequence");
FCO_DECLARE_TAG(AttemptOrdinalTag, "attempt-ordinal");

#undef FCO_DECLARE_TAG

// Identifier syntax: 1..64 characters from [A-Za-z0-9._:-].
[[nodiscard]] bool is_valid_identifier(std::string_view text) noexcept;

// Canonical identifier normalisation used before validation. Rejects leading or
// trailing whitespace, empty text, and text longer than the bound. No case
// folding is performed: identifiers are case-sensitive and byte-exact.
[[nodiscard]] Result<std::string> normalize_identifier(std::string_view text);

// Strict decimal parsing. Rejects an empty string, a sign, whitespace, leading
// zeros (except the single character "0"), non-digits, and any value above the
// supplied bound.
[[nodiscard]] Result<std::uint64_t> parse_bounded_decimal(std::string_view text,
                                                          std::uint64_t maximum);

template <class Tag, class T>
class Strong {
 public:
  using tag_type = Tag;
  using value_type = T;

  // Unset. Valid for every T so that strong values can live in containers and
  // aggregate records; valid() is false and every boundary rejects it.
  Strong() = default;
  explicit Strong(T value) : value_(std::move(value)) {}

  [[nodiscard]] const T& value() const noexcept { return value_; }
  [[nodiscard]] T& value() noexcept { return value_; }

  [[nodiscard]] bool valid() const noexcept {
    if constexpr (std::is_integral_v<T>) {
      return value_ != T{0};
    } else {
      return !value_.empty();
    }
  }

  [[nodiscard]] std::string to_string() const {
    if constexpr (std::is_integral_v<T>) {
      return std::to_string(value_);
    } else {
      return value_;
    }
  }

  [[nodiscard]] std::string describe() const {
    std::string out(tag_type::kind_name);
    out += " '";
    out += to_string();
    out += '\'';
    return out;
  }

  [[nodiscard]] static Result<Strong> parse(std::string_view text) {
    if constexpr (std::is_integral_v<T>) {
      auto parsed = parse_bounded_decimal(
          text, static_cast<std::uint64_t>(std::numeric_limits<T>::max()));
      if (!parsed.ok()) return parsed.error();
      const std::uint64_t value = parsed.value();
      if (value == 0) {
        return failure<Strong>(ErrorCode::InvalidIdentity, std::string(text),
                               std::string(tag_type::kind_name) + " must be at least 1");
      }
      return success(Strong(static_cast<T>(value)));
    } else {
      auto normalized = normalize_identifier(text);
      if (!normalized.ok()) return normalized.error();
      return success(Strong(std::move(normalized.value())));
    }
  }

  // Checked successor. Overflow is a BoundedLimitExceeded error rather than a
  // silent wrap, because a wrapped generation would look like a stale one.
  [[nodiscard]] Result<Strong> incremented() const {
    if constexpr (std::is_integral_v<T>) {
      if (value_ == std::numeric_limits<T>::max()) {
        return failure<Strong>(ErrorCode::BoundedLimitExceeded, describe(),
                               "counter would overflow");
      }
      return success(Strong(static_cast<T>(value_ + T{1})));
    } else {
      return failure<Strong>(ErrorCode::UnsupportedOperation, describe(),
                             "identifiers have no successor");
    }
  }

  friend bool operator==(const Strong&, const Strong&) = default;
  friend auto operator<=>(const Strong&, const Strong&) = default;

 private:
  T value_{};
};

template <class Tag>
using StringId = Strong<Tag, std::string>;

template <class Tag>
using Counter = Strong<Tag, std::uint64_t>;

using AssetId = StringId<AssetIdTag>;
using RackId = StringId<RackIdTag>;
using SiteId = StringId<SiteIdTag>;
using TenantId = StringId<TenantIdTag>;
using StepId = StringId<StepIdTag>;
using PlanId = StringId<PlanIdTag>;
using ChangeRequestId = StringId<ChangeRequestIdTag>;
using AttemptId = StringId<AttemptIdTag>;
using EvidenceId = StringId<EvidenceIdTag>;
using PrincipalId = StringId<PrincipalIdTag>;
using PolicyId = StringId<PolicyIdTag>;
using ChangeKind = StringId<ChangeKindTag>;
using ResolutionNote = StringId<ResolutionNoteTag>;

using FacilityEpoch = Counter<FacilityEpochTag>;
using ControlEpoch = Counter<ControlEpochTag>;
using IncarnationId = Counter<IncarnationIdTag>;
using LifecycleGeneration = Counter<LifecycleGenerationTag>;
using HardwareGeneration = Counter<HardwareGenerationTag>;
using FirmwareGeneration = Counter<FirmwareGenerationTag>;
using PolicyGeneration = Counter<PolicyGenerationTag>;
using DependencyGeneration = Counter<DependencyGenerationTag>;
using CapacityGeneration = Counter<CapacityGenerationTag>;
using TopologyGeneration = Counter<TopologyGenerationTag>;
using MaintenanceGeneration = Counter<MaintenanceGenerationTag>;
using PowerGeneration = Counter<PowerGenerationTag>;
using CoolingGeneration = Counter<CoolingGenerationTag>;
using AsiGeneration = Counter<AsiGenerationTag>;
using DfiGeneration = Counter<DfiGenerationTag>;
using FacilityRevision = Counter<FacilityRevisionTag>;
using Revision = Counter<RevisionTag>;
using PlanRevision = Counter<PlanRevisionTag>;
using CommitSequence = Counter<CommitSequenceTag>;
using ObservationSequence = Counter<ObservationSequenceTag>;
using AttemptOrdinal = Counter<AttemptOrdinalTag>;

// Deterministic synthetic identifier generation. Identifiers produced by the
// runtime are content-addressed: the same logical entity always yields the same
// identifier, so replays and restarts do not invent new identities.
[[nodiscard]] PlanId derive_plan_id(std::string_view request_id, FacilityEpoch epoch,
                                    Revision request_revision);
[[nodiscard]] AttemptId derive_attempt_id(const PlanId& plan, PlanRevision revision,
                                          const StepId& step, AttemptOrdinal ordinal);
[[nodiscard]] EvidenceId derive_evidence_id(const PlanId& plan, const StepId& step,
                                            const AttemptId& attempt,
                                            ObservationSequence sequence);

template <class Tag, class T>
struct StrongHash {
  [[nodiscard]] std::size_t operator()(const Strong<Tag, T>& value) const noexcept {
    if constexpr (std::is_integral_v<T>) {
      return std::hash<T>{}(value.value());
    } else {
      return std::hash<T>{}(value.value());
    }
  }
};

}  // namespace fco

namespace std {
template <class Tag, class T>
struct hash<fco::Strong<Tag, T>> {
  [[nodiscard]] size_t operator()(const fco::Strong<Tag, T>& value) const noexcept {
    return fco::StrongHash<Tag, T>{}(value);
  }
};
}  // namespace std
