#pragma once

// Facility Change Orchestrator - release identity.
//
// The values below are the only authoritative statement of the release
// identity. They are compiled into the library so that an installed artifact
// can report exactly which build it is, independently of any file on disk.

#include <cstdint>
#include <string_view>

namespace fco {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

inline constexpr std::string_view kVersionString = "1.0.0";
inline constexpr std::string_view kProjectName = "Facility Change Orchestrator";
inline constexpr std::string_view kProjectAbbreviation = "FCO";
inline constexpr std::string_view kDccpProgram = "Data Center Control Plane";
inline constexpr std::string_view kDccpTranche = "Tranche 5 - Physical Fleet Lifecycle";
inline constexpr std::string_view kDccpRepositoryNumber = "DCCP repository 40 of 72";
inline constexpr std::string_view kCopyrightNotice = "Copyright 2026 Summon Software Labs";

// Durable state format version. Bumping this value is a storage compatibility
// break: every version below is rejected as an unsupported format version
// rather than being silently upgraded.
inline constexpr std::uint16_t kStorageFormatVersion = 1;

// Bound on a single durable record payload. Records larger than this are
// rejected before any allocation proportional to the claimed size is made.
inline constexpr std::uint64_t kMaxRecordPayloadBytes = 16ull * 1024ull * 1024ull;

// Bound on any single encoded string field.
inline constexpr std::uint64_t kMaxStringBytes = 4096ull;

// Bound on collection sizes accepted from durable storage or CLI input.
inline constexpr std::uint64_t kMaxCollectionEntries = 100000ull;

// Bound on the number of steps in one plan.
inline constexpr std::uint64_t kMaxPlanSteps = 4096ull;

// Bound on the number of plans retained in one durable store.
inline constexpr std::uint64_t kMaxRetainedPlans = 4096ull;

// Bound on retained historical generations in the durable store.
inline constexpr std::uint64_t kRetainedGenerations = 8ull;

}  // namespace fco
