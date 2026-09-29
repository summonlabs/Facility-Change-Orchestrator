#pragma once

// Durable, single-writer, crash-consistent state store.
//
// Layout under the store root:
//   fco.lock              exclusive OS-level writer lock (held for the process lifetime)
//   fco.manifest          the publication point: names the authoritative generation
//   fco.fencing           the highest published generation; advanced only after publication
//   records/state-<gen>.fco   integrity-checked state records
//   staging/              transient staging area, never authoritative
//
// Commit order is fixed and observable through CommitHooks:
//   stage -> flush -> read back and verify -> atomically publish the record ->
//   stage/verify/publish the manifest -> advance the fencing record -> prune.
// A crash at any point leaves either the previous generation or the new one as
// the single authoritative generation; partially committed states are never
// merged.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fco/codec.hpp"
#include "fco/digest.hpp"
#include "fco/error.hpp"
#include "fco/state.hpp"
#include "fco/strong.hpp"
#include "fco/version.hpp"

namespace fco {

enum class RecordKind : std::uint16_t {
  Unspecified = 0,
  Manifest = 1,
  State = 2,
  Fencing = 3,
};
inline constexpr RecordKind kRecordKindMax = RecordKind::Fencing;

[[nodiscard]] constexpr bool is_valid(RecordKind value) noexcept {
  return static_cast<std::uint16_t>(value) >= 1u &&
         static_cast<std::uint16_t>(value) <= static_cast<std::uint16_t>(kRecordKindMax);
}
[[nodiscard]] std::string_view to_string(RecordKind value) noexcept;

enum class CommitStage : std::uint8_t {
  Unspecified = 0,
  BeforeStagingWrite = 1,
  AfterStagingWritten = 2,
  AfterStagingFlushed = 3,
  AfterStagingVerified = 4,
  AfterRecordPublished = 5,
  BeforeManifestWrite = 6,
  AfterManifestFlushed = 7,
  AfterManifestVerified = 8,
  AfterManifestPublished = 9,
  AfterFencingPublished = 10,
  AfterRetention = 11,
  CommitComplete = 12,
};
inline constexpr CommitStage kCommitStageMax = CommitStage::CommitComplete;

[[nodiscard]] constexpr bool is_valid(CommitStage value) noexcept {
  return static_cast<std::uint8_t>(value) >= 1u &&
         static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(kCommitStageMax);
}
[[nodiscard]] std::string_view to_string(CommitStage value) noexcept;
[[nodiscard]] Result<CommitStage> parse_commit_stage(std::string_view text);

// Called after each commit stage completes. Used by the crash-consistency
// harness to terminate a real process at a meaningful commit point, and
// available for observability. The hook must not call back into the store.
struct CommitHooks {
  std::function<void(CommitStage)> on_stage;
};

enum class RecoveryMode : std::uint8_t {
  Unspecified = 0,
  FreshStore = 1,
  ManifestAuthoritative = 2,
  FencingAuthoritative = 3,
  RecordScan = 4,
};
inline constexpr RecoveryMode kRecoveryModeMax = RecoveryMode::RecordScan;

[[nodiscard]] constexpr bool is_valid(RecoveryMode value) noexcept {
  return static_cast<std::uint8_t>(value) >= 1u &&
         static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(kRecoveryModeMax);
}
[[nodiscard]] std::string_view to_string(RecoveryMode value) noexcept;

struct RecoveryReport {
  RecoveryMode mode = RecoveryMode::Unspecified;
  std::uint64_t generation = 0;
  std::uint64_t verified_records = 0;
  std::uint64_t rejected_records = 0;
  std::uint64_t pruned_records = 0;
  bool manifest_repaired = false;
  bool fencing_repaired = false;
  bool store_created = false;
  std::vector<std::string> notes;

  [[nodiscard]] std::string describe() const;
};

struct Manifest {
  std::uint16_t format_version = kStorageFormatVersion;
  std::uint64_t generation = 0;
  std::uint64_t payload_bytes = 0;
  std::uint32_t payload_crc32 = 0;
  Digest record_digest;
  CommitSequence commit_sequence;
  Revision revision;
  std::uint64_t created_at_micros = 0;
};

struct FencingRecord {
  std::uint16_t format_version = kStorageFormatVersion;
  std::uint64_t published_generation = 0;
  CommitSequence commit_sequence;
  Digest manifest_digest;
};

// Converts a UTF-8 path string to a filesystem path without locale dependence.
[[nodiscard]] Result<std::filesystem::path> path_from_utf8(std::string_view text);
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

struct RecordFrame {
  RecordKind kind = RecordKind::Unspecified;
  std::uint16_t format_version = kStorageFormatVersion;
  std::vector<std::uint8_t> payload;
  Digest content_digest;
};

[[nodiscard]] std::vector<std::uint8_t> encode_frame(const RecordFrame& frame);
[[nodiscard]] Result<RecordFrame> decode_frame(const std::uint8_t* data, std::size_t length);

class DurableStore {
 public:
  DurableStore();
  ~DurableStore();
  DurableStore(DurableStore&&) noexcept;
  DurableStore& operator=(DurableStore&&) noexcept;
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;

  // Acquires the exclusive writer lock before reading anything. Fails with
  // LockUnavailable when another process holds it.
  [[nodiscard]] static Result<DurableStore> open(const std::filesystem::path& root,
                                                 CommitHooks hooks = CommitHooks{});

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const std::filesystem::path& root() const noexcept;
  [[nodiscard]] const RecoveryReport& recovery() const noexcept;
  [[nodiscard]] Revision revision() const noexcept;
  [[nodiscard]] CommitSequence commit_sequence() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] const Digest& fencing_digest() const noexcept;
  [[nodiscard]] bool empty_store() const noexcept;

  [[nodiscard]] Result<OrchestratorState> load() const;
  [[nodiscard]] Result<Revision> commit(const OrchestratorState& state);

  // Releases the writer lock and closes the store. Safe to call repeatedly.
  void close() noexcept;

  [[nodiscard]] static std::string_view lock_file_name() noexcept;
  [[nodiscard]] static std::string_view manifest_file_name() noexcept;
  [[nodiscard]] static std::string_view fencing_file_name() noexcept;
  [[nodiscard]] static std::string_view records_directory_name() noexcept;
  [[nodiscard]] static std::string_view staging_directory_name() noexcept;
  [[nodiscard]] static std::string state_record_name(std::uint64_t generation);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fco
