#include "fco/store.hpp"

// Durable, single-writer, crash-consistent state store (implementation).
//
// Frame layout (little-endian, 24-byte header):
//   0  magic 'F','C','O','J'
//   4  u16 format_version   (must equal kStorageFormatVersion)
//   6  u16 record_kind      (RecordKind; 0 and out-of-domain rejected)
//   8  u32 flags            (must be 0)
//  12  u32 payload_length   (must be <= kMaxRecordPayloadBytes)
//  16  u32 payload_crc32    (crc32 over the payload, seed 0)
//  20  u32 header_crc32     (crc32 over bytes [0,20) with [20,24) treated as zero)
//  24  payload
//  24+payload_length  content_digest = SHA-256 over the 24 header bytes (final
//                     form, including the real header_crc32) followed by the
//                     payload bytes.
// A file is a frame only when its length is exactly 24 + payload_length + 32.
//
// Durability barriers:
//   * Windows: MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
//     is the durability barrier for a publication; there is no portable
//     directory-fsync equivalent and none is attempted. FlushFileBuffers is
//     used on staged files before they are published.
//   * POSIX: the staged file is fsync()ed before rename(), and the containing
//     directory is fsync()ed after rename() so the new name survives a crash.
//
// Commit order (observable through CommitHooks):
//   encode -> stage state record -> flush -> read back and verify -> publish ->
//   stage/verify/publish manifest -> publish fencing -> prune -> publish the new
//   in-memory revision.
//
// Failure rule: if a state record was published but the manifest was not, the
// published record is deleted before commit() returns the error. Otherwise a
// generation that no manifest names would sit in records/ and could later be
// mistaken for authoritative during a record scan.
//
// Fencing is monotonic: it is only ever advanced. Recovery repairs the manifest
// from the fencing record, never the other way around, and it never rewrites the
// fencing record backwards to a generation it already moved past.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fco {
namespace {

// ---------------------------------------------------------------------------
// Frame constants and little-endian helpers
// ---------------------------------------------------------------------------
constexpr std::size_t kHeaderBytes = 24;
constexpr std::size_t kTrailerBytes = Digest::kBytes;
constexpr std::size_t kFrameOverheadBytes = kHeaderBytes + kTrailerBytes;
constexpr std::size_t kOffsetFormatVersion = 4;
constexpr std::size_t kOffsetRecordKind = 6;
constexpr std::size_t kOffsetFlags = 8;
constexpr std::size_t kOffsetPayloadLength = 12;
constexpr std::size_t kOffsetPayloadCrc = 16;
constexpr std::size_t kOffsetHeaderCrc = 20;
constexpr std::uint64_t kMaxRecordFileBytes =
    kMaxRecordPayloadBytes + static_cast<std::uint64_t>(kFrameOverheadBytes);
constexpr std::array<std::uint8_t, 4> kFrameMagic{{'F', 'C', 'O', 'J'}};
constexpr std::string_view kStagedManifestName = "manifest.tmp";
constexpr std::string_view kStagedFencingName = "fencing.tmp";

[[nodiscard]] std::uint16_t load_u16(const std::uint8_t* data) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    static_cast<std::uint16_t>(
                                        static_cast<std::uint16_t>(data[1]) << 8));
}

[[nodiscard]] std::uint32_t load_u32(const std::uint8_t* data) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4u; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (8u * index);
  }
  return value;
}

void store_u16(std::uint8_t* data, std::uint16_t value) noexcept {
  data[0] = static_cast<std::uint8_t>(value & 0xffu);
  data[1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
}

void store_u32(std::uint8_t* data, std::uint32_t value) noexcept {
  for (unsigned index = 0; index < 4u; ++index) {
    data[index] = static_cast<std::uint8_t>((value >> (8u * index)) & 0xffu);
  }
}

[[nodiscard]] std::filesystem::path named_path(std::string_view name) {
  return std::filesystem::path(std::string(name));
}

using MaybeBytes = std::optional<std::vector<std::uint8_t>>;
using MaybeDigest = std::optional<Digest>;

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------
[[nodiscard]] bool path_has_embedded_nul(const std::filesystem::path& path) {
#if defined(_WIN32)
  return path.native().find(L'\0') != std::wstring::npos;
#else
  return path.native().find('\0') != std::string::npos;
#endif
}

#if defined(_WIN32)
[[nodiscard]] bool has_extended_prefix(const std::wstring& text) {
  return text.size() >= 4 && text[0] == L'\\' && text[1] == L'\\' && text[2] == L'?' &&
         text[3] == L'\\';
}

[[nodiscard]] std::wstring trim_trailing_separators(std::wstring text) {
  while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) {
    text.pop_back();
  }
  return text;
}

// Absolute drive-letter paths are handed to the OS with the \\?\ extended-length
// prefix so that paths beyond MAX_PATH work. UNC paths use \\?\UNC\... and any
// other path (relative, or already prefixed) is passed through unchanged.
[[nodiscard]] std::wstring native_io_path(const std::filesystem::path& path) {
  const std::wstring native = path.native();
  if (has_extended_prefix(native)) return native;
  std::error_code ec;
  std::filesystem::path resolved = std::filesystem::absolute(path, ec);
  if (ec) resolved = path;
  resolved.make_preferred();
  const std::wstring text = trim_trailing_separators(resolved.native());
  const bool drive_absolute =
      text.size() >= 2 && text[1] == L':' &&
      ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z'));
  if (drive_absolute) {
    std::wstring out = L"\\\\?\\";
    out += text;
    return out;
  }
  const bool unc_absolute = text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\';
  if (unc_absolute) {
    std::wstring out = L"\\\\?\\UNC\\";
    out += text.substr(2);
    return out;
  }
  return text;
}
#endif

// ---------------------------------------------------------------------------
// Native file handle
// ---------------------------------------------------------------------------
class NativeFile {
 public:
  NativeFile() = default;
  NativeFile(const NativeFile&) = delete;
  NativeFile& operator=(const NativeFile&) = delete;

  NativeFile(NativeFile&& other) noexcept { move_from(other); }

  NativeFile& operator=(NativeFile&& other) noexcept {
    if (this != &other) {
      reset();
      move_from(other);
    }
    return *this;
  }

  ~NativeFile() { reset(); }

  [[nodiscard]] bool valid() const noexcept {
#if defined(_WIN32)
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return fd_ >= 0;
#endif
  }

  void reset() noexcept {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
#endif
  }

#if defined(_WIN32)
  void set(HANDLE handle) noexcept {
    reset();
    handle_ = handle;
  }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
#else
  void set(int fd) noexcept {
    reset();
    fd_ = fd;
  }
  [[nodiscard]] int get() const noexcept { return fd_; }
#endif

 private:
  void move_from(NativeFile& other) noexcept {
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = INVALID_HANDLE_VALUE;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
  }

#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

// ---------------------------------------------------------------------------
// File primitives
// ---------------------------------------------------------------------------
[[nodiscard]] Result<MaybeBytes> read_whole_file(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::wstring native = native_io_path(path);
  HANDLE handle = ::CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return success(MaybeBytes{});
    }
    return failure<MaybeBytes>(ErrorCode::StorageFailure, path_to_utf8(path),
                               "cannot open the file for reading (win32 error " +
                                   std::to_string(code) + ")");
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) == 0) {
    const DWORD code = ::GetLastError();
    ::CloseHandle(handle);
    return failure<MaybeBytes>(ErrorCode::StorageFailure, path_to_utf8(path),
                               "cannot determine the file size (win32 error " +
                                   std::to_string(code) + ")");
  }
  if (size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > kMaxRecordFileBytes) {
    ::CloseHandle(handle);
    return failure<MaybeBytes>(ErrorCode::BoundedLimitExceeded, path_to_utf8(path),
                               "the file is larger than the maximum durable record size");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const DWORD chunk = static_cast<DWORD>(
        (std::min)(remaining, static_cast<std::size_t>(1u << 20)));
    DWORD read = 0;
    if (::ReadFile(handle, bytes.data() + offset, chunk, &read, nullptr) == 0) {
      const DWORD code = ::GetLastError();
      ::CloseHandle(handle);
      return failure<MaybeBytes>(ErrorCode::StorageFailure, path_to_utf8(path),
                                 "cannot read the file (win32 error " +
                                     std::to_string(code) + ")");
    }
    if (read == 0) break;
    offset += static_cast<std::size_t>(read);
  }
  ::CloseHandle(handle);
  bytes.resize(offset);
  return success(MaybeBytes{std::move(bytes)});
#else
  const std::string native = path.native();
  const int fd = ::open(native.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return success(MaybeBytes{});
    return failure<MaybeBytes>(
        ErrorCode::StorageFailure, path_to_utf8(path),
        "cannot open the file for reading (" +
            std::error_code(errno, std::generic_category()).message() + ")");
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    const int code = errno;
    ::close(fd);
    return failure<MaybeBytes>(
        ErrorCode::StorageFailure, path_to_utf8(path),
        "cannot stat the file (" +
            std::error_code(code, std::generic_category()).message() + ")");
  }
  if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<std::uint64_t>(info.st_size) > kMaxRecordFileBytes) {
    ::close(fd);
    return failure<MaybeBytes>(ErrorCode::BoundedLimitExceeded, path_to_utf8(path),
                               "the file is not a readable durable record");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t bytes_read = ::read(fd, bytes.data() + offset, bytes.size() - offset);
    if (bytes_read < 0) {
      if (errno == EINTR) continue;
      const int code = errno;
      ::close(fd);
      return failure<MaybeBytes>(
          ErrorCode::StorageFailure, path_to_utf8(path),
          "cannot read the file (" +
              std::error_code(code, std::generic_category()).message() + ")");
    }
    if (bytes_read == 0) break;
    offset += static_cast<std::size_t>(bytes_read);
  }
  ::close(fd);
  bytes.resize(offset);
  return success(MaybeBytes{std::move(bytes)});
#endif
}

[[nodiscard]] Status write_all(NativeFile& file, const std::uint8_t* data,
                               std::size_t length, const std::filesystem::path& subject) {
#if defined(_WIN32)
  std::size_t offset = 0;
  while (offset < length) {
    const std::size_t remaining = length - offset;
    const DWORD chunk =
        static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>(1u << 20)));
    DWORD written = 0;
    if (::WriteFile(file.get(), data + offset, chunk, &written, nullptr) == 0) {
      const DWORD code = ::GetLastError();
      return Status(Error(ErrorCode::StorageFailure, path_to_utf8(subject),
                          "cannot write the file (win32 error " + std::to_string(code) + ")"));
    }
    if (written == 0) {
      return Status(Error(ErrorCode::StorageFailure, path_to_utf8(subject),
                          "the file accepted no bytes before the write completed"));
    }
    offset += static_cast<std::size_t>(written);
  }
  return success();
#else
  std::size_t offset = 0;
  while (offset < length) {
    const ssize_t written = ::write(file.get(), data + offset, length - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      const int code = errno;
      return Status(Error(ErrorCode::StorageFailure, path_to_utf8(subject),
                          "cannot write the file (" +
                              std::error_code(code, std::generic_category()).message() + ")"));
    }
    if (written == 0) {
      return Status(Error(ErrorCode::StorageFailure, path_to_utf8(subject),
                          "the file accepted no bytes before the write completed"));
    }
    offset += static_cast<std::size_t>(written);
  }
  return success();
#endif
}

[[nodiscard]] Status flush_file(NativeFile& file, const std::filesystem::path& subject) {
#if defined(_WIN32)
  if (::FlushFileBuffers(file.get()) == 0) {
    const DWORD code = ::GetLastError();
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(subject),
                        "cannot flush the file to stable storage (win32 error " +
                            std::to_string(code) + ")"));
  }
  return success();
#else
  if (::fsync(file.get()) != 0) {
    const int code = errno;
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(subject),
                        "cannot flush the file to stable storage (" +
                            std::error_code(code, std::generic_category()).message() + ")"));
  }
  return success();
#endif
}

// Creates (truncating) a file and writes the whole buffer without flushing it.
// The caller decides when the durability barrier happens, which is what makes
// the AfterStagingWritten / AfterStagingFlushed stages distinguishable.
[[nodiscard]] Status begin_staged_write(const std::filesystem::path& path,
                                        const std::uint8_t* data, std::size_t length,
                                        NativeFile& out) {
  out.reset();
#if defined(_WIN32)
  const std::wstring native = native_io_path(path);
  HANDLE handle = ::CreateFileW(native.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(path),
                        "cannot create the staged file (win32 error " +
                            std::to_string(code) + ")"));
  }
  out.set(handle);
#else
  const std::string native = path.native();
  const int fd = ::open(native.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    const int code = errno;
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(path),
                        "cannot create the staged file (" +
                            std::error_code(code, std::generic_category()).message() + ")"));
  }
  out.set(fd);
#endif
  const Status written = write_all(out, data, length, path);
  if (!written.ok()) {
    out.reset();
    return written;
  }
  return success();
}

[[nodiscard]] Status write_whole_file(const std::filesystem::path& path,
                                      const std::vector<std::uint8_t>& bytes) {
  NativeFile file;
  FCO_RETURN_IF_ERROR(begin_staged_write(path, bytes.data(), bytes.size(), file));
  FCO_RETURN_IF_ERROR(flush_file(file, path));
  file.reset();
  return success();
}

#if !defined(_WIN32)
// POSIX only: fsync the directory that holds a freshly renamed file so the new
// name itself is durable. Windows has no equivalent and uses
// MOVEFILE_WRITE_THROUGH as its publication barrier instead.
[[nodiscard]] Status sync_directory(const std::filesystem::path& directory) {
  const std::string native = directory.native();
#if defined(O_DIRECTORY)
  const int fd = ::open(native.c_str(), O_RDONLY | O_DIRECTORY);
#else
  const int fd = ::open(native.c_str(), O_RDONLY);
#endif
  if (fd < 0) {
    // Directory fsync is a best-effort strengthening on platforms that allow
    // opening a directory; the rename itself already happened.
    return success();
  }
  const int result = ::fsync(fd);
  const int code = errno;
  ::close(fd);
  if (result != 0 && code != EINVAL && code != ENOTSUP) {
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(directory),
                        "cannot flush the containing directory (" +
                            std::error_code(code, std::generic_category()).message() + ")"));
  }
  return success();
}
#endif

[[nodiscard]] Status publish_file(const std::filesystem::path& from,
                                  const std::filesystem::path& to) {
#if defined(_WIN32)
  const std::wstring native_from = native_io_path(from);
  const std::wstring native_to = native_io_path(to);
  if (::MoveFileExW(native_from.c_str(), native_to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const DWORD code = ::GetLastError();
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(to),
                        "cannot publish the staged file (win32 error " +
                            std::to_string(code) + ")"));
  }
  return success();
#else
  const std::string native_from = from.native();
  const std::string native_to = to.native();
  if (::rename(native_from.c_str(), native_to.c_str()) != 0) {
    const int code = errno;
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(to),
                        "cannot publish the staged file (" +
                            std::error_code(code, std::generic_category()).message() + ")"));
  }
  return sync_directory(to.parent_path());
#endif
}

[[nodiscard]] Status remove_file(const std::filesystem::path& path, bool* removed) {
  if (removed != nullptr) *removed = false;
  std::error_code ec;
  const bool existed = std::filesystem::remove(path, ec);
  if (ec) {
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(path),
                        "cannot remove the file: " + ec.message()));
  }
  if (removed != nullptr) *removed = existed;
  return success();
}

void clear_directory(const std::filesystem::path& directory) {
  std::error_code ec;
  std::filesystem::directory_iterator it(directory, ec);
  if (ec) return;
  const std::filesystem::directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) return;
    std::error_code remove_ec;
    std::filesystem::remove_all(it->path(), remove_ec);
  }
}

[[nodiscard]] Result<std::vector<std::filesystem::path>> list_directory(
    const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> entries;
  std::error_code ec;
  std::filesystem::directory_iterator it(directory, ec);
  if (ec) {
    return failure<std::vector<std::filesystem::path>>(
        ErrorCode::StorageFailure, path_to_utf8(directory),
        "cannot enumerate the directory: " + ec.message());
  }
  const std::filesystem::directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      return failure<std::vector<std::filesystem::path>>(
          ErrorCode::StorageFailure, path_to_utf8(directory),
          "cannot enumerate the directory: " + ec.message());
    }
    entries.push_back(it->path());
  }
  return success(std::move(entries));
}

[[nodiscard]] Result<MaybeDigest> digest_of_file(const std::filesystem::path& path) {
  auto bytes = read_whole_file(path);
  if (!bytes.ok()) return bytes.error();
  if (!bytes.value().has_value()) return success(MaybeDigest{});
  Sha256 hasher;
  const std::vector<std::uint8_t>& data = *bytes.value();
  hasher.update(data.data(), data.size());
  return success(MaybeDigest{hasher.finish()});
}

// ---------------------------------------------------------------------------
// Durable payload codecs (manifest / fencing)
// ---------------------------------------------------------------------------
[[nodiscard]] Status encode_manifest_payload(const Manifest& manifest,
                                             std::vector<std::uint8_t>& out) {
  if (manifest.format_version != kStorageFormatVersion) {
    return Status(Error(ErrorCode::UnsupportedFormatVersion, "manifest.format_version",
                        "manifest format version " +
                            std::to_string(manifest.format_version) +
                            " is not supported (expected " +
                            std::to_string(kStorageFormatVersion) + ")"));
  }
  if (manifest.generation == 0) {
    return Status(Error(ErrorCode::InvalidIdentity, "manifest.generation",
                        "the manifest must name a non-zero generation"));
  }
  if (!manifest.record_digest.valid()) {
    return Status(Error(ErrorCode::InvalidIdentity, "manifest.record_digest",
                        "the manifest must carry the content digest of the record it names"));
  }
  if (!manifest.revision.valid() || !manifest.commit_sequence.valid()) {
    return Status(Error(ErrorCode::InvalidIdentity, "manifest",
                        "the manifest must carry a non-zero revision and commit sequence"));
  }
  Writer writer;
  write_version_prefix(writer);
  writer.u64(manifest.generation);
  writer.u64(manifest.payload_bytes);
  writer.u32(manifest.payload_crc32);
  writer.digest(manifest.record_digest);
  writer.u64(manifest.commit_sequence.value());
  writer.u64(manifest.revision.value());
  writer.u64(manifest.created_at_micros);
  writer.reserve(0u, "manifest.reserved");
  FCO_RETURN_IF_ERROR(writer.status());
  if (static_cast<std::uint64_t>(writer.size()) > kMaxRecordPayloadBytes) {
    return Status(Error(ErrorCode::BoundedLimitExceeded, "manifest",
                        "the encoded manifest exceeds the maximum record payload size"));
  }
  out = writer.buffer();
  return success();
}

[[nodiscard]] Result<Manifest> decode_manifest_payload(const std::uint8_t* data,
                                                       std::size_t length) {
  if (data == nullptr) {
    return failure<Manifest>(ErrorCode::MalformedInput, "manifest", "null manifest buffer");
  }
  Reader reader(data, length);
  auto version = reader.u16("manifest.format_version");
  if (!version.ok()) return version.error();
  auto reserved_version = reader.reserved_u16("manifest.reserved_version");
  if (!reserved_version.ok()) return reserved_version.error();
  if (version.value() != kStorageFormatVersion) {
    return failure<Manifest>(ErrorCode::UnsupportedFormatVersion, "manifest.format_version",
                             "manifest format version " + std::to_string(version.value()) +
                                 " is not supported (expected " +
                                 std::to_string(kStorageFormatVersion) + ")");
  }
  auto generation = reader.u64("manifest.generation");
  if (!generation.ok()) return generation.error();
  auto payload_bytes = reader.u64("manifest.payload_bytes");
  if (!payload_bytes.ok()) return payload_bytes.error();
  auto payload_crc32 = reader.u32("manifest.payload_crc32");
  if (!payload_crc32.ok()) return payload_crc32.error();
  auto record_digest = reader.digest("manifest.record_digest");
  if (!record_digest.ok()) return record_digest.error();
  auto commit_sequence = reader.u64("manifest.commit_sequence");
  if (!commit_sequence.ok()) return commit_sequence.error();
  auto revision = reader.u64("manifest.revision");
  if (!revision.ok()) return revision.error();
  auto created_at = reader.u64("manifest.created_at_micros");
  if (!created_at.ok()) return created_at.error();
  auto reserved = reader.reserved_u32("manifest.reserved");
  if (!reserved.ok()) return reserved.error();
  auto end = reader.expect_end("manifest");
  if (!end.ok()) return end.error();

  if (generation.value() == 0) {
    return failure<Manifest>(ErrorCode::InvalidIdentity, "manifest.generation",
                             "the manifest does not name a generation");
  }
  if (!record_digest.value().valid()) {
    return failure<Manifest>(ErrorCode::InvalidIdentity, "manifest.record_digest",
                             "the manifest does not carry a content digest");
  }

  Manifest manifest;
  manifest.format_version = version.value();
  manifest.generation = generation.value();
  manifest.payload_bytes = payload_bytes.value();
  manifest.payload_crc32 = payload_crc32.value();
  manifest.record_digest = record_digest.value();
  manifest.commit_sequence = CommitSequence(commit_sequence.value());
  manifest.revision = Revision(revision.value());
  manifest.created_at_micros = created_at.value();
  return success(manifest);
}

[[nodiscard]] Status encode_fencing_payload(const FencingRecord& fencing,
                                            std::vector<std::uint8_t>& out) {
  if (fencing.format_version != kStorageFormatVersion) {
    return Status(Error(ErrorCode::UnsupportedFormatVersion, "fencing.format_version",
                        "fencing format version " +
                            std::to_string(fencing.format_version) +
                            " is not supported (expected " +
                            std::to_string(kStorageFormatVersion) + ")"));
  }
  if (fencing.published_generation == 0) {
    return Status(Error(ErrorCode::InvalidIdentity, "fencing.published_generation",
                        "the fencing record must publish a non-zero generation"));
  }
  if (!fencing.manifest_digest.valid()) {
    return Status(Error(ErrorCode::InvalidIdentity, "fencing.manifest_digest",
                        "the fencing record must carry the digest of the manifest it fences"));
  }
  Writer writer;
  write_version_prefix(writer);
  writer.u64(fencing.published_generation);
  writer.u64(fencing.commit_sequence.value());
  writer.digest(fencing.manifest_digest);
  writer.reserve(0u, "fencing.reserved");
  FCO_RETURN_IF_ERROR(writer.status());
  if (static_cast<std::uint64_t>(writer.size()) > kMaxRecordPayloadBytes) {
    return Status(Error(ErrorCode::BoundedLimitExceeded, "fencing",
                        "the encoded fencing record exceeds the maximum record payload size"));
  }
  out = writer.buffer();
  return success();
}

[[nodiscard]] Result<FencingRecord> decode_fencing_payload(const std::uint8_t* data,
                                                           std::size_t length) {
  if (data == nullptr) {
    return failure<FencingRecord>(ErrorCode::MalformedInput, "fencing", "null fencing buffer");
  }
  Reader reader(data, length);
  auto version = reader.u16("fencing.format_version");
  if (!version.ok()) return version.error();
  auto reserved_version = reader.reserved_u16("fencing.reserved_version");
  if (!reserved_version.ok()) return reserved_version.error();
  if (version.value() != kStorageFormatVersion) {
    return failure<FencingRecord>(ErrorCode::UnsupportedFormatVersion, "fencing.format_version",
                                  "fencing format version " + std::to_string(version.value()) +
                                      " is not supported (expected " +
                                      std::to_string(kStorageFormatVersion) + ")");
  }
  auto generation = reader.u64("fencing.published_generation");
  if (!generation.ok()) return generation.error();
  auto commit_sequence = reader.u64("fencing.commit_sequence");
  if (!commit_sequence.ok()) return commit_sequence.error();
  auto manifest_digest = reader.digest("fencing.manifest_digest");
  if (!manifest_digest.ok()) return manifest_digest.error();
  auto reserved = reader.reserved_u32("fencing.reserved");
  if (!reserved.ok()) return reserved.error();
  auto end = reader.expect_end("fencing");
  if (!end.ok()) return end.error();

  if (generation.value() == 0) {
    return failure<FencingRecord>(ErrorCode::InvalidIdentity, "fencing.published_generation",
                                  "the fencing record does not publish a generation");
  }
  if (!manifest_digest.value().valid()) {
    return failure<FencingRecord>(ErrorCode::InvalidIdentity, "fencing.manifest_digest",
                                  "the fencing record does not carry a manifest digest");
  }

  FencingRecord fencing;
  fencing.format_version = version.value();
  fencing.published_generation = generation.value();
  fencing.commit_sequence = CommitSequence(commit_sequence.value());
  fencing.manifest_digest = manifest_digest.value();
  return success(fencing);
}

// ---------------------------------------------------------------------------
// Record files
// ---------------------------------------------------------------------------
[[nodiscard]] Result<RecordFrame> decode_record_file(const std::vector<std::uint8_t>& bytes,
                                                     RecordKind expected_kind) {
  auto frame = decode_frame(bytes.data(), bytes.size());
  if (!frame.ok()) return frame.error();
  if (frame.value().kind != expected_kind) {
    return failure<RecordFrame>(ErrorCode::MalformedInput, "frame.record_kind",
                                "record frame carries kind '" +
                                    std::string(to_string(frame.value().kind)) +
                                    "' where '" + std::string(to_string(expected_kind)) +
                                    "' was expected");
  }
  return frame;
}

[[nodiscard]] std::vector<std::uint8_t> frame_bytes(RecordKind kind,
                                                    const std::vector<std::uint8_t>& payload) {
  RecordFrame frame;
  frame.kind = kind;
  frame.format_version = kStorageFormatVersion;
  frame.payload = payload;
  return encode_frame(frame);
}

// Re-opens a staged file, decodes it, and proves byte for byte that it is
// exactly the frame that was written: same payload bytes, same content digest,
// and a file length of exactly 24 + payload + 32.
[[nodiscard]] Status verify_staged_frame(const std::filesystem::path& staged,
                                         RecordKind expected_kind,
                                         const std::vector<std::uint8_t>& expected_payload,
                                         Digest& out_digest) {
  auto bytes = read_whole_file(staged);
  if (!bytes.ok()) return Status(bytes.error());
  if (!bytes.value().has_value()) {
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(staged),
                        "the staged file disappeared before it could be verified"));
  }
  const std::vector<std::uint8_t>& data = *bytes.value();
  auto decoded = decode_record_file(data, expected_kind);
  if (!decoded.ok()) return Status(decoded.error());
  const RecordFrame& frame = decoded.value();
  if (frame.format_version != kStorageFormatVersion) {
    return Status(Error(ErrorCode::UnsupportedFormatVersion, "frame.format_version",
                        "the staged record was written with an unsupported format version"));
  }
  if (frame.payload != expected_payload) {
    return Status(Error(ErrorCode::IntegrityFailure, "frame.payload",
                        "the payload read back from the staged file differs from what was "
                        "written"));
  }
  // The content digest of the frame that was written covers the final header and
  // the payload, so it is recomputable from the expected payload alone.
  Sha256 hasher;
  hasher.update(data.data(), kHeaderBytes);
  hasher.update(expected_payload.data(), expected_payload.size());
  const Digest expected_digest = hasher.finish();
  if (frame.content_digest != expected_digest) {
    return Status(Error(ErrorCode::IntegrityFailure, "frame.content_digest",
                        "the content digest read back from the staged file does not match the "
                        "record that was written"));
  }
  out_digest = frame.content_digest;
  return success();
}

struct StateRecord {
  std::uint64_t generation = 0;
  RecordFrame frame;
  OrchestratorState state;
};

// Reads a state record file and fully verifies it: frame integrity, record kind,
// and a payload that decodes as durable state.
[[nodiscard]] Result<StateRecord> read_state_record(const std::filesystem::path& path,
                                                    std::uint64_t generation) {
  auto bytes = read_whole_file(path);
  if (!bytes.ok()) return bytes.error();
  if (!bytes.value().has_value()) {
    return failure<StateRecord>(ErrorCode::RecordNotFound, path_to_utf8(path),
                                "the record file does not exist");
  }
  const std::vector<std::uint8_t>& data = *bytes.value();
  auto decoded = decode_record_file(data, RecordKind::State);
  if (!decoded.ok()) return decoded.error();
  RecordFrame frame = decoded.take();
  auto decoded_state = decode_state(frame.payload.data(), frame.payload.size());
  if (!decoded_state.ok()) return decoded_state.error();
  StateRecord record;
  record.generation = generation;
  record.state = decoded_state.take();
  record.frame = std::move(frame);
  return success(std::move(record));
}

// The manifest is only authoritative when every value it carries agrees with the
// record it names.
[[nodiscard]] Status check_manifest_matches_record(const Manifest& manifest,
                                                   const StateRecord& record) {
  const std::vector<std::uint8_t>& payload = record.frame.payload;
  if (static_cast<std::uint64_t>(payload.size()) != manifest.payload_bytes) {
    return Status(Error(ErrorCode::IntegrityFailure, "manifest.payload_bytes",
                        "the manifest claims " + std::to_string(manifest.payload_bytes) +
                            " payload bytes but the record holds " +
                            std::to_string(payload.size())));
  }
  if (crc32(payload.data(), payload.size(), 0) != manifest.payload_crc32) {
    return Status(Error(ErrorCode::IntegrityFailure, "manifest.payload_crc32",
                        "the payload CRC-32 in the manifest does not match the record payload"));
  }
  if (record.frame.content_digest != manifest.record_digest) {
    return Status(Error(ErrorCode::IntegrityFailure, "manifest.record_digest",
                        "the record digest in the manifest does not match the record content "
                        "digest"));
  }
  if (record.state.revision != manifest.revision) {
    return Status(Error(ErrorCode::IntegrityFailure, "manifest.revision",
                        "the manifest names revision " + manifest.revision.to_string() +
                            " but the record holds revision " +
                            record.state.revision.to_string()));
  }
  if (record.state.commit_sequence != manifest.commit_sequence) {
    return Status(Error(ErrorCode::IntegrityFailure, "manifest.commit_sequence",
                        "the manifest names commit sequence " +
                            manifest.commit_sequence.to_string() +
                            " but the record holds commit sequence " +
                            record.state.commit_sequence.to_string()));
  }
  return success();
}

[[nodiscard]] Manifest manifest_from_record(const StateRecord& record) {
  Manifest manifest;
  manifest.format_version = kStorageFormatVersion;
  manifest.generation = record.generation;
  manifest.payload_bytes = static_cast<std::uint64_t>(record.frame.payload.size());
  manifest.payload_crc32 = crc32(record.frame.payload.data(), record.frame.payload.size(), 0);
  manifest.record_digest = record.frame.content_digest;
  manifest.commit_sequence = record.state.commit_sequence;
  manifest.revision = record.state.revision;
  manifest.created_at_micros = record.state.updated_at_micros;
  return manifest;
}

[[nodiscard]] Status publish_payload(const std::filesystem::path& io_root,
                                     std::string_view staged_name, RecordKind kind,
                                     const std::vector<std::uint8_t>& payload,
                                     const std::filesystem::path& final_path) {
  const std::vector<std::uint8_t> bytes = frame_bytes(kind, payload);
  if (bytes.empty()) {
    return Status(Error(ErrorCode::InternalError, std::string(to_string(kind)),
                        "the record frame could not be encoded"));
  }
  const std::filesystem::path staged =
      io_root / named_path(DurableStore::staging_directory_name()) / named_path(staged_name);
  FCO_RETURN_IF_ERROR(write_whole_file(staged, bytes));
  Digest digest;
  FCO_RETURN_IF_ERROR(verify_staged_frame(staged, kind, payload, digest));
  FCO_RETURN_IF_ERROR(publish_file(staged, final_path));
  return success();
}

[[nodiscard]] Status publish_manifest_record(const std::filesystem::path& io_root,
                                             const std::filesystem::path& manifest_path,
                                             const Manifest& manifest) {
  std::vector<std::uint8_t> payload;
  FCO_RETURN_IF_ERROR(encode_manifest_payload(manifest, payload));
  return publish_payload(io_root, kStagedManifestName, RecordKind::Manifest, payload,
                         manifest_path);
}

[[nodiscard]] Status publish_fencing_record(const std::filesystem::path& io_root,
                                            const std::filesystem::path& fencing_path,
                                            const FencingRecord& fencing) {
  std::vector<std::uint8_t> payload;
  FCO_RETURN_IF_ERROR(encode_fencing_payload(fencing, payload));
  return publish_payload(io_root, kStagedFencingName, RecordKind::Fencing, payload,
                         fencing_path);
}

[[nodiscard]] std::optional<std::uint64_t> parse_state_record_name(std::string_view name) {
  constexpr std::string_view prefix = "state-";
  constexpr std::string_view suffix = ".fco";
  if (name.size() <= prefix.size() + suffix.size()) return std::nullopt;
  if (name.substr(0, prefix.size()) != prefix) return std::nullopt;
  if (name.substr(name.size() - suffix.size()) != suffix) return std::nullopt;
  const std::string_view digits =
      name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  if (digits.empty() || digits.size() > 20) return std::nullopt;
  if (digits.size() > 1 && digits.front() == '0') return std::nullopt;
  constexpr std::uint64_t kUint64Max = (std::numeric_limits<std::uint64_t>::max)();
  std::uint64_t value = 0;
  for (const char character : digits) {
    if (character < '0' || character > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (kUint64Max - digit) / 10ull) return std::nullopt;
    value = (value * 10ull) + digit;
  }
  if (value == 0) return std::nullopt;
  return value;
}

// Retains the newest kRetainedGenerations generations and removes the rest. The
// newest generation itself is never pruned, and an orphan record newer than the
// newest generation is left alone rather than underflowing the age comparison.
[[nodiscard]] Result<std::uint64_t> prune_records(const std::filesystem::path& records_dir,
                                                  std::uint64_t newest) {
  auto entries = list_directory(records_dir);
  if (!entries.ok()) return entries.error();
  std::uint64_t pruned = 0;
  for (const std::filesystem::path& entry : entries.value()) {
    const std::optional<std::uint64_t> generation =
        parse_state_record_name(path_to_utf8(entry.filename()));
    if (!generation.has_value()) continue;
    const std::uint64_t value = *generation;
    if (value >= newest) continue;
    if (newest - value < kRetainedGenerations) continue;
    std::error_code ec;
    if (std::filesystem::remove(entry, ec)) ++pruned;
  }
  return success(pruned);
}

// ---------------------------------------------------------------------------
// Commit hooks
// ---------------------------------------------------------------------------
// A hook is observability, but it is also the crash-injection point used by the
// crash-consistency harness. A hook that throws is therefore treated as an
// interrupted commit rather than as a silent no-op: the commit stops at that
// stage, and the cleanup rule above removes any record that was published
// without its manifest.
[[nodiscard]] Status fire_stage(const CommitHooks& hooks, CommitStage stage) {
  if (!hooks.on_stage) return success();
  const std::string label(to_string(stage));
  try {
    hooks.on_stage(stage);
  } catch (const std::exception& error) {
    return Status(Error(ErrorCode::CommitFailure, label,
                        "the commit hook for stage '" + label +
                            "' threw: " + std::string(error.what())));
  } catch (...) {
    return Status(Error(ErrorCode::CommitFailure, label,
                        "the commit hook for stage '" + label +
                            "' threw a non-standard exception"));
  }
  return success();
}

[[nodiscard]] Error map_commit_error(const Error& error, std::string_view stage) {
  if (error.code == ErrorCode::StorageFailure) {
    return Error(ErrorCode::CommitFailure, std::string(stage), error.message);
  }
  return error;
}

[[nodiscard]] Status commit_io(const Status& status, std::string_view stage) {
  if (status.ok()) return status;
  return Status(map_commit_error(status.error(), stage));
}

[[nodiscard]] bool equals_stage_name(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    char a = left[index];
    char b = right[index];
    if (a == '_') a = '-';
    if (b == '_') b = '-';
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enum names
// ---------------------------------------------------------------------------
std::string_view to_string(RecordKind value) noexcept {
  switch (value) {
    case RecordKind::Unspecified:
      return "unspecified";
    case RecordKind::Manifest:
      return "manifest";
    case RecordKind::State:
      return "state";
    case RecordKind::Fencing:
      return "fencing";
  }
  return "unknown-record-kind";
}

std::string_view to_string(CommitStage value) noexcept {
  switch (value) {
    case CommitStage::Unspecified:
      return "unspecified";
    case CommitStage::BeforeStagingWrite:
      return "before-staging-write";
    case CommitStage::AfterStagingWritten:
      return "after-staging-written";
    case CommitStage::AfterStagingFlushed:
      return "after-staging-flushed";
    case CommitStage::AfterStagingVerified:
      return "after-staging-verified";
    case CommitStage::AfterRecordPublished:
      return "after-record-published";
    case CommitStage::BeforeManifestWrite:
      return "before-manifest-write";
    case CommitStage::AfterManifestFlushed:
      return "after-manifest-flushed";
    case CommitStage::AfterManifestVerified:
      return "after-manifest-verified";
    case CommitStage::AfterManifestPublished:
      return "after-manifest-published";
    case CommitStage::AfterFencingPublished:
      return "after-fencing-published";
    case CommitStage::AfterRetention:
      return "after-retention";
    case CommitStage::CommitComplete:
      return "commit-complete";
  }
  return "unknown-commit-stage";
}

Result<CommitStage> parse_commit_stage(std::string_view text) {
  // The reserved value is named here exactly as it is in every other parse_*
  // helper, so the CLI surface is uniform: parsing "unspecified" yields the
  // reserved value and the caller decides whether it is acceptable.
  if (text == "unspecified") return success(CommitStage::Unspecified);
  for (std::uint8_t raw = 1; raw <= static_cast<std::uint8_t>(kCommitStageMax); ++raw) {
    const CommitStage stage = static_cast<CommitStage>(raw);
    if (equals_stage_name(text, to_string(stage))) return success(stage);
  }
  return failure<CommitStage>(ErrorCode::InvalidEnumValue, std::string(text),
                              "unknown commit stage name");
}

std::string_view to_string(RecoveryMode value) noexcept {
  switch (value) {
    case RecoveryMode::Unspecified:
      return "unspecified";
    case RecoveryMode::FreshStore:
      return "fresh-store";
    case RecoveryMode::ManifestAuthoritative:
      return "manifest-authoritative";
    case RecoveryMode::FencingAuthoritative:
      return "fencing-authoritative";
    case RecoveryMode::RecordScan:
      return "record-scan";
  }
  return "unknown-recovery-mode";
}

std::string RecoveryReport::describe() const {
  std::string out = "mode=";
  out += to_string(mode);
  out += " generation=";
  out += std::to_string(generation);
  out += " verified=";
  out += std::to_string(verified_records);
  out += " rejected=";
  out += std::to_string(rejected_records);
  out += " pruned=";
  out += std::to_string(pruned_records);
  out += " manifest_repaired=";
  out += manifest_repaired ? "true" : "false";
  out += " fencing_repaired=";
  out += fencing_repaired ? "true" : "false";
  out += " store_created=";
  out += store_created ? "true" : "false";
  for (const std::string& note : notes) {
    out += "; ";
    out += note;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
Result<std::filesystem::path> path_from_utf8(std::string_view text) {
  if (text.empty()) {
    return failure<std::filesystem::path>(ErrorCode::PathRejected, "path", "the path is empty");
  }
  if (text.find('\0') != std::string_view::npos) {
    return failure<std::filesystem::path>(ErrorCode::PathRejected, "path",
                                          "the path contains an embedded NUL");
  }
  constexpr std::size_t kIntMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());
#if defined(_WIN32)
  if (text.size() > kIntMax) {
    return failure<std::filesystem::path>(ErrorCode::PathRejected, "path",
                                          "the path is too long to convert");
  }
  const int length = static_cast<int>(text.size());
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
  if (needed <= 0) {
    return failure<std::filesystem::path>(ErrorCode::PathRejected, "path",
                                          "the path is not valid UTF-8");
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length,
                                            wide.data(), needed);
  if (written != needed) {
    return failure<std::filesystem::path>(ErrorCode::PathRejected, "path",
                                          "the path is not valid UTF-8");
  }
  return success(std::filesystem::path(wide));
#else
  static_cast<void>(kIntMax);
  const std::u8string encoded(reinterpret_cast<const char8_t*>(text.data()), text.size());
  return success(std::filesystem::path(encoded));
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::wstring& wide = path.native();
  if (wide.empty()) return std::string();
  if (wide.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return std::string();
  }
  const int length = static_cast<int>(wide.size());
  const int needed =
      ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string out(static_cast<std::size_t>(needed), '\0');
  const int written =
      ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, out.data(), needed, nullptr, nullptr);
  if (written != needed) return std::string();
  return out;
#else
  return path.native();
#endif
}

// ---------------------------------------------------------------------------
// Frame codec
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> encode_frame(const RecordFrame& frame) {
  // Encoding is total: an unencodable frame yields an empty vector, which is
  // never a valid frame because every frame is at least 24 + 32 bytes long.
  std::vector<std::uint8_t> out;
  if (!is_valid(frame.kind)) return out;
  if (static_cast<std::uint64_t>(frame.payload.size()) > kMaxRecordPayloadBytes) return out;

  const std::size_t payload_length = frame.payload.size();
  out.resize(kFrameOverheadBytes + payload_length);
  std::memcpy(out.data(), kFrameMagic.data(), kFrameMagic.size());
  store_u16(out.data() + kOffsetFormatVersion, frame.format_version);
  store_u16(out.data() + kOffsetRecordKind, static_cast<std::uint16_t>(frame.kind));
  store_u32(out.data() + kOffsetFlags, 0u);
  store_u32(out.data() + kOffsetPayloadLength, static_cast<std::uint32_t>(payload_length));
  store_u32(out.data() + kOffsetPayloadCrc, crc32(frame.payload.data(), payload_length, 0));
  store_u32(out.data() + kOffsetHeaderCrc, 0u);
  store_u32(out.data() + kOffsetHeaderCrc, crc32(out.data(), kOffsetHeaderCrc, 0));
  if (payload_length != 0) {
    std::memcpy(out.data() + kHeaderBytes, frame.payload.data(), payload_length);
  }

  Sha256 hasher;
  hasher.update(out.data(), kHeaderBytes);
  hasher.update(frame.payload.data(), payload_length);
  const Digest content_digest = hasher.finish();
  std::memcpy(out.data() + kHeaderBytes + payload_length, content_digest.bytes().data(),
              Digest::kBytes);
  return out;
}

Result<RecordFrame> decode_frame(const std::uint8_t* data, std::size_t length) {
  if (data == nullptr) {
    return failure<RecordFrame>(ErrorCode::MalformedInput, "frame", "null record buffer");
  }
  if (length < kHeaderBytes) {
    return failure<RecordFrame>(ErrorCode::MalformedInput, "frame",
                                "the record is shorter than the 24-byte frame header");
  }
  if (std::memcmp(data, kFrameMagic.data(), kFrameMagic.size()) != 0) {
    return failure<RecordFrame>(ErrorCode::MalformedInput, "frame.magic",
                                "the record does not begin with the FCOJ magic bytes");
  }
  // Precedence follows the documented ErrorCode ordering: the format version is
  // rejected before any other interpretation, then the reserved flags, then the
  // claimed payload length.
  const std::uint16_t format_version = load_u16(data + kOffsetFormatVersion);
  if (format_version != kStorageFormatVersion) {
    return failure<RecordFrame>(ErrorCode::UnsupportedFormatVersion, "frame.format_version",
                                "record format version " + std::to_string(format_version) +
                                    " is not supported (expected " +
                                    std::to_string(kStorageFormatVersion) + ")");
  }
  const std::uint32_t flags = load_u32(data + kOffsetFlags);
  if (flags != 0u) {
    return failure<RecordFrame>(ErrorCode::ReservedFieldNonZero, "frame.flags",
                                "record flags are reserved and must be zero");
  }
  const std::uint32_t payload_length = load_u32(data + kOffsetPayloadLength);
  const std::uint64_t expected_length =
      static_cast<std::uint64_t>(kHeaderBytes) + static_cast<std::uint64_t>(payload_length) +
      static_cast<std::uint64_t>(kTrailerBytes);
  if (static_cast<std::uint64_t>(length) != expected_length) {
    return failure<RecordFrame>(
        ErrorCode::MalformedInput, "frame.length",
        "the record is " + std::to_string(length) + " bytes but the header claims a " +
            std::to_string(expected_length) + "-byte record");
  }
  if (static_cast<std::uint64_t>(payload_length) > kMaxRecordPayloadBytes) {
    return failure<RecordFrame>(ErrorCode::BoundedLimitExceeded, "frame.payload_length",
                                "the record payload exceeds the supported bound");
  }
  const std::uint32_t header_crc = load_u32(data + kOffsetHeaderCrc);
  const std::uint32_t computed_header_crc = crc32(data, kOffsetHeaderCrc, 0);
  if (header_crc != computed_header_crc) {
    return failure<RecordFrame>(ErrorCode::IntegrityFailure, "frame.header_crc32",
                                "the record header CRC-32 does not match the header bytes");
  }
  const std::uint16_t kind_raw = load_u16(data + kOffsetRecordKind);
  const RecordKind kind = static_cast<RecordKind>(kind_raw);
  if (!is_valid(kind)) {
    return failure<RecordFrame>(ErrorCode::InvalidEnumValue, "frame.record_kind",
                                "record kind " + std::to_string(kind_raw) +
                                    " is outside the record kind domain");
  }
  const std::uint8_t* payload = data + kHeaderBytes;
  const std::uint32_t payload_crc = load_u32(data + kOffsetPayloadCrc);
  if (crc32(payload, payload_length, 0) != payload_crc) {
    return failure<RecordFrame>(ErrorCode::IntegrityFailure, "frame.payload_crc32",
                                "the record payload CRC-32 does not match the payload bytes");
  }
  Digest stored_digest;
  {
    Digest::Bytes bytes{};
    const std::uint8_t* trailer = data + kHeaderBytes + payload_length;
    for (std::size_t index = 0; index < Digest::kBytes; ++index) {
      bytes[index] = trailer[index];
    }
    stored_digest = Digest::from_bytes(bytes);
  }
  Sha256 hasher;
  hasher.update(data, kHeaderBytes);
  hasher.update(payload, payload_length);
  if (hasher.finish() != stored_digest) {
    return failure<RecordFrame>(ErrorCode::IntegrityFailure, "frame.content_digest",
                                "the record content digest does not match the recorded bytes");
  }

  RecordFrame frame;
  frame.kind = kind;
  frame.format_version = format_version;
  frame.payload.assign(payload, payload + payload_length);
  frame.content_digest = stored_digest;
  return success(std::move(frame));
}

// ---------------------------------------------------------------------------
// DurableStore::Impl
// ---------------------------------------------------------------------------
struct DurableStore::Impl {
  std::filesystem::path root;     // exactly the path the caller supplied
  std::filesystem::path io_root;  // OS-level root (extended-length on Windows)
  CommitHooks hooks;
  RecoveryReport report;
  Revision revision;
  CommitSequence commit_sequence;
  std::uint64_t generation = 0;
  // Digest carried by the current fencing record: SHA-256 over the published
  // manifest file bytes, i.e. the digest that fences the manifest.
  Digest fencing_digest;
  bool open = false;
  NativeFile lock;

  [[nodiscard]] std::filesystem::path lock_path() const {
    return io_root / named_path(DurableStore::lock_file_name());
  }
  [[nodiscard]] std::filesystem::path manifest_path() const {
    return io_root / named_path(DurableStore::manifest_file_name());
  }
  [[nodiscard]] std::filesystem::path fencing_path() const {
    return io_root / named_path(DurableStore::fencing_file_name());
  }
  [[nodiscard]] std::filesystem::path records_dir() const {
    return io_root / named_path(DurableStore::records_directory_name());
  }
  [[nodiscard]] std::filesystem::path staging_dir() const {
    return io_root / named_path(DurableStore::staging_directory_name());
  }

  [[nodiscard]] Status acquire_lock();
  [[nodiscard]] Status recover();
};

// The exclusive writer lock is taken before anything is read, so two processes
// can never both believe they are the writer. Windows uses a share mode of zero;
// POSIX uses a non-blocking flock.
Status DurableStore::Impl::acquire_lock() {
  const std::filesystem::path path = lock_path();
#if defined(_WIN32)
  const std::wstring native = native_io_path(path);
  HANDLE handle = ::CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED) {
      return Status(Error(ErrorCode::LockUnavailable, path_to_utf8(path),
                          "another process holds the exclusive writer lock (win32 error " +
                              std::to_string(code) + ")"));
    }
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(path),
                        "cannot open the writer lock file (win32 error " +
                            std::to_string(code) + ")"));
  }
  lock.set(handle);
  return success();
#else
  const std::string native = path.native();
  const int fd = ::open(native.c_str(), O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    const int code = errno;
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(path),
                        "cannot open the writer lock file (" +
                            std::error_code(code, std::generic_category()).message() + ")"));
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int code = errno;
    ::close(fd);
    if (code == EWOULDBLOCK || code == EAGAIN || code == EACCES) {
      return Status(Error(ErrorCode::LockUnavailable, path_to_utf8(path),
                          "another process holds the exclusive writer lock (" +
                              std::error_code(code, std::generic_category()).message() + ")"));
    }
    return Status(Error(ErrorCode::StorageFailure, path_to_utf8(path),
                        "cannot acquire the writer lock (" +
                            std::error_code(code, std::generic_category()).message() + ")"));
  }
  lock.set(fd);
  return success();
#endif
}

// Recovery selects exactly one authoritative generation and never merges:
//   1. the manifest, when it decodes and the record it names verifies in full;
//   2. the fencing record, when it decodes and the record it publishes verifies;
//   3. otherwise the highest generation in records/ whose record verifies.
// A fencing record that publishes a generation ahead of the manifest wins,
// because the manifest is then the stale artifact. When nothing verifies while
// durable artifacts exist, open() fails with AmbiguousRecovery rather than
// silently starting empty over unreadable data.
Status DurableStore::Impl::recover() {
  report = RecoveryReport{};
  report.notes.clear();

  std::map<std::string, std::string> record_failures;      // file name -> reason
  std::map<std::string, std::uint64_t> reason_counts;      // aggregated reasons
  std::map<std::string, std::uint64_t> artifact_failures;  // reason -> count
  std::set<std::string> missing_records;
  std::set<std::string> verified_names;
  std::uint64_t verified = 0;
  std::vector<std::string> flow_notes;

  const auto examine = [&](const std::filesystem::path& path, std::uint64_t generation,
                           StateRecord& out) -> bool {
    auto record = read_state_record(path, generation);
    const std::string name = path_to_utf8(path.filename());
    if (record.ok()) {
      out = record.take();
      if (verified_names.insert(name).second) ++verified;
      return true;
    }
    if (record.error().code == ErrorCode::RecordNotFound) {
      missing_records.insert(name);
      return false;
    }
    record_failures.emplace(name, describe(record.error()));
    return false;
  };

  // ---- manifest -----------------------------------------------------------
  std::optional<Manifest> manifest;
  std::optional<StateRecord> manifest_record;
  bool manifest_present = false;
  {
    auto bytes = read_whole_file(manifest_path());
    if (!bytes.ok()) {
      ++artifact_failures["manifest: " + describe(bytes.error())];
    } else if (bytes.value().has_value()) {
      manifest_present = true;
      auto frame = decode_record_file(*bytes.value(), RecordKind::Manifest);
      if (!frame.ok()) {
        ++artifact_failures["manifest: " + describe(frame.error())];
      } else {
        const std::vector<std::uint8_t>& payload = frame.value().payload;
        auto decoded = decode_manifest_payload(payload.data(), payload.size());
        if (!decoded.ok()) {
          ++artifact_failures["manifest: " + describe(decoded.error())];
        } else {
          manifest = decoded.take();
        }
      }
    }
  }
  if (manifest.has_value()) {
    StateRecord record;
    if (examine(records_dir() / named_path(DurableStore::state_record_name(manifest->generation)),
                manifest->generation, record)) {
      const Status consistent = check_manifest_matches_record(*manifest, record);
      if (!consistent.ok()) {
        ++artifact_failures["manifest: " + describe(consistent.error())];
      } else {
        manifest_record = std::move(record);
      }
    }
  }

  // ---- fencing ------------------------------------------------------------
  std::optional<FencingRecord> fencing;
  std::optional<StateRecord> fencing_record;
  bool fencing_present = false;
  {
    auto bytes = read_whole_file(fencing_path());
    if (!bytes.ok()) {
      ++artifact_failures["fencing: " + describe(bytes.error())];
    } else if (bytes.value().has_value()) {
      fencing_present = true;
      auto frame = decode_record_file(*bytes.value(), RecordKind::Fencing);
      if (!frame.ok()) {
        ++artifact_failures["fencing: " + describe(frame.error())];
      } else {
        const std::vector<std::uint8_t>& payload = frame.value().payload;
        auto decoded = decode_fencing_payload(payload.data(), payload.size());
        if (!decoded.ok()) {
          ++artifact_failures["fencing: " + describe(decoded.error())];
        } else {
          fencing = decoded.take();
        }
      }
    }
  }
  if (fencing.has_value()) {
    StateRecord record;
    if (examine(records_dir() / named_path(DurableStore::state_record_name(
                     fencing->published_generation)),
                fencing->published_generation, record)) {
      fencing_record = std::move(record);
    }
  }

  // ---- records/ -----------------------------------------------------------
  std::vector<std::pair<std::uint64_t, std::filesystem::path>> records;
  bool records_present = false;
  {
    auto entries = list_directory(records_dir());
    if (!entries.ok()) return Status(entries.error());
    for (const std::filesystem::path& entry : entries.value()) {
      records_present = true;
      const std::string name = path_to_utf8(entry.filename());
      const std::optional<std::uint64_t> parsed_generation = parse_state_record_name(name);
      if (!parsed_generation.has_value()) {
        record_failures.emplace(name, "file name is not a state record name");
        continue;
      }
      records.emplace_back(*parsed_generation, entry);
    }
    std::sort(records.begin(), records.end(),
              [](const std::pair<std::uint64_t, std::filesystem::path>& left,
                 const std::pair<std::uint64_t, std::filesystem::path>& right) {
                return left.first > right.first;
              });
  }

  // ---- select the single authoritative generation --------------------------
  std::optional<StateRecord> recovered;
  RecoveryMode mode = RecoveryMode::Unspecified;
  bool manifest_repaired = false;
  bool fencing_repaired = false;
  bool store_created = false;
  std::string ambiguous_detail;

  if (manifest_record.has_value()) {
    recovered = std::move(manifest_record);
    mode = RecoveryMode::ManifestAuthoritative;
    if (fencing_record.has_value() &&
        fencing_record->generation > recovered->generation) {
      const std::uint64_t fenced = fencing_record->generation;
      const std::uint64_t stale = recovered->generation;
      recovered = std::move(fencing_record);
      mode = RecoveryMode::FencingAuthoritative;
      manifest_repaired = true;
      flow_notes.push_back("fencing publishes generation " + std::to_string(fenced) +
                           " ahead of manifest generation " + std::to_string(stale) +
                           "; the manifest is the stale artifact and was repaired");
    }
  } else if (fencing_record.has_value()) {
    recovered = std::move(fencing_record);
    mode = RecoveryMode::FencingAuthoritative;
    manifest_repaired = true;
    flow_notes.push_back(manifest_present
                             ? "the manifest did not verify; it was rebuilt from the fencing "
                               "record for generation " +
                                   std::to_string(recovered->generation)
                             : "no manifest was present; it was built from the fencing record "
                               "for generation " +
                                   std::to_string(recovered->generation));
  } else {
    // Full scan: the highest generation whose record verifies wins, and every
    // record that fails verification is counted and aggregated by reason.
    std::optional<StateRecord> best;
    for (const auto& entry : records) {
      StateRecord record;
      if (!examine(entry.second, entry.first, record)) continue;
      if (!best.has_value()) best = std::move(record);
    }
    if (best.has_value()) {
      recovered = std::move(best);
      mode = RecoveryMode::RecordScan;
      manifest_repaired = true;
      fencing_repaired = true;
      flow_notes.push_back("neither the manifest nor the fencing record was authoritative; "
                           "generation " +
                           std::to_string(recovered->generation) +
                           " was recovered by scanning records/ and both artifacts were rewritten");
    } else if (!records_present && !manifest_present && !fencing_present) {
      mode = RecoveryMode::FreshStore;
      store_created = true;
    } else {
      mode = RecoveryMode::Unspecified;
      ambiguous_detail = "no authoritative generation could be recovered: ";
      ambiguous_detail += std::to_string(record_failures.size());
      ambiguous_detail += " record(s) rejected";
      if (!missing_records.empty()) {
        ambiguous_detail += ", " + std::to_string(missing_records.size()) + " record(s) missing";
      }
      std::uint64_t artifacts = 0;
      for (const auto& entry : artifact_failures) artifacts += entry.second;
      if (artifacts != 0) {
        ambiguous_detail += ", " + std::to_string(artifacts) + " artifact(s) rejected";
      }
    }
  }

  // ---- report -------------------------------------------------------------
  for (const auto& entry : record_failures) ++reason_counts[entry.second];
  report.notes.clear();
  for (const auto& entry : reason_counts) {
    report.notes.push_back("rejected " + std::to_string(entry.second) + " record(s): " +
                           entry.first);
  }
  for (const auto& entry : artifact_failures) {
    report.notes.push_back("rejected " + std::to_string(entry.second) + " artifact(s): " +
                           entry.first);
  }
  if (!missing_records.empty()) {
    report.notes.push_back(std::to_string(missing_records.size()) +
                           " named record file(s) are missing");
  }
  report.verified_records = verified;
  report.rejected_records = static_cast<std::uint64_t>(record_failures.size());
  report.generation = recovered.has_value() ? recovered->generation : 0;
  report.mode = mode;
  report.manifest_repaired = manifest_repaired;
  report.fencing_repaired = fencing_repaired;
  report.store_created = store_created;

  if (mode == RecoveryMode::Unspecified) {
    std::string message = std::move(ambiguous_detail);
    for (const std::string& note : report.notes) {
      message += "; ";
      message += note;
    }
    report.notes.insert(report.notes.end(), flow_notes.begin(), flow_notes.end());
    return Status(Error(ErrorCode::AmbiguousRecovery, "recovery", message));
  }

  // ---- write the authoritative artifacts forward --------------------------
  if (recovered.has_value()) {
    if (manifest_repaired) {
      const Manifest repaired = manifest_from_record(*recovered);
      const Status published = publish_manifest_record(io_root, manifest_path(), repaired);
      if (!published.ok()) {
        report.notes.insert(report.notes.end(), flow_notes.begin(), flow_notes.end());
        return published;
      }
      flow_notes.push_back("manifest rewritten for generation " +
                           std::to_string(recovered->generation));
    }
    auto manifest_file_digest = digest_of_file(manifest_path());
    if (!manifest_file_digest.ok()) {
      report.notes.insert(report.notes.end(), flow_notes.begin(), flow_notes.end());
      return Status(manifest_file_digest.error());
    }
    if (!manifest_file_digest.value().has_value()) {
      report.notes.insert(report.notes.end(), flow_notes.begin(), flow_notes.end());
      return Status(Error(ErrorCode::StorageFailure, path_to_utf8(manifest_path()),
                          "the manifest is missing after recovery"));
    }
    const Digest current_manifest_digest = *manifest_file_digest.value();

    bool write_fencing = false;
    if (!fencing.has_value()) {
      write_fencing = true;
    } else if (fencing->published_generation > recovered->generation) {
      // Fencing is only ever advanced. Moving it backwards to the recovered
      // generation would erase the fence that already moved past it, so the
      // existing record is left in place.
      write_fencing = false;
      flow_notes.push_back(
          "fencing publishes generation " +
          std::to_string(fencing->published_generation) +
          " which is ahead of the recovered generation " +
          std::to_string(recovered->generation) +
          "; the fencing record was left untouched rather than moved backwards");
    } else {
      write_fencing = fencing->published_generation < recovered->generation ||
                      fencing_repaired || manifest_repaired ||
                      fencing->manifest_digest != current_manifest_digest;
    }
    if (write_fencing) {
      FencingRecord advanced;
      advanced.format_version = kStorageFormatVersion;
      advanced.published_generation = recovered->generation;
      advanced.commit_sequence = recovered->state.commit_sequence;
      advanced.manifest_digest = current_manifest_digest;
      const Status published = publish_fencing_record(io_root, fencing_path(), advanced);
      if (!published.ok()) {
        report.notes.insert(report.notes.end(), flow_notes.begin(), flow_notes.end());
        return published;
      }
      fencing_digest = advanced.manifest_digest;
      flow_notes.push_back("fencing advanced to generation " +
                           std::to_string(recovered->generation));
    } else if (fencing.has_value()) {
      fencing_digest = fencing->manifest_digest;
    }

    revision = recovered->state.revision;
    commit_sequence = recovered->state.commit_sequence;
    generation = recovered->generation;
  } else {
    revision = Revision{};
    commit_sequence = CommitSequence{};
    generation = 0;
    fencing_digest = Digest{};
  }

  // ---- retention ----------------------------------------------------------
  if (records_present) {
    auto pruned = prune_records(records_dir(), generation);
    if (!pruned.ok()) {
      report.notes.insert(report.notes.end(), flow_notes.begin(), flow_notes.end());
      return Status(pruned.error());
    }
    report.pruned_records += pruned.value();
  }

  report.notes.insert(report.notes.end(), flow_notes.begin(), flow_notes.end());
  return success();
}

// ---------------------------------------------------------------------------
// DurableStore
// ---------------------------------------------------------------------------
DurableStore::DurableStore() = default;
DurableStore::~DurableStore() = default;
DurableStore::DurableStore(DurableStore&&) noexcept = default;
DurableStore& DurableStore::operator=(DurableStore&&) noexcept = default;

Result<DurableStore> DurableStore::open(const std::filesystem::path& root, CommitHooks hooks) {
  if (root.empty()) {
    return failure<DurableStore>(ErrorCode::PathRejected, "root", "the store root path is empty");
  }
  if (path_has_embedded_nul(root)) {
    return failure<DurableStore>(ErrorCode::PathRejected, "root",
                                 "the store root path contains an embedded NUL");
  }

  DurableStore store;
  store.impl_ = std::make_unique<Impl>();
  Impl& impl = *store.impl_;
  impl.root = root;
  impl.hooks = std::move(hooks);
  impl.io_root = std::filesystem::path();
#if defined(_WIN32)
  impl.io_root = std::filesystem::path(native_io_path(root));
#else
  impl.io_root = root;
#endif

  std::error_code ec;
  const std::filesystem::file_status root_status = std::filesystem::status(impl.io_root, ec);
  if (!ec && std::filesystem::exists(root_status) && !std::filesystem::is_directory(root_status)) {
    return failure<DurableStore>(ErrorCode::PathRejected, path_to_utf8(root),
                                 "the store root exists and is not a directory");
  }
  ec.clear();
  std::filesystem::create_directories(impl.io_root, ec);
  if (ec) {
    return failure<DurableStore>(ErrorCode::StorageFailure, path_to_utf8(root),
                                 "cannot create the store root directory: " + ec.message());
  }
  ec.clear();
  std::filesystem::create_directories(impl.records_dir(), ec);
  if (ec) {
    return failure<DurableStore>(ErrorCode::StorageFailure, path_to_utf8(impl.records_dir()),
                                 "cannot create the records directory: " + ec.message());
  }
  ec.clear();
  std::filesystem::create_directories(impl.staging_dir(), ec);
  if (ec) {
    return failure<DurableStore>(ErrorCode::StorageFailure, path_to_utf8(impl.staging_dir()),
                                 "cannot create the staging directory: " + ec.message());
  }

  // The writer lock is taken before anything on disk is read.
  FCO_RETURN_IF_ERROR(impl.acquire_lock());

  // Staging is transient by construction; nothing in it is ever authoritative.
  clear_directory(impl.staging_dir());

  FCO_RETURN_IF_ERROR(impl.recover());

  impl.open = true;
  return success(std::move(store));
}

bool DurableStore::is_open() const noexcept { return impl_ != nullptr && impl_->open; }

const std::filesystem::path& DurableStore::root() const noexcept {
  static const std::filesystem::path kEmpty;
  return impl_ != nullptr ? impl_->root : kEmpty;
}

const RecoveryReport& DurableStore::recovery() const noexcept {
  static const RecoveryReport kEmpty;
  return impl_ != nullptr ? impl_->report : kEmpty;
}

Revision DurableStore::revision() const noexcept {
  return impl_ != nullptr ? impl_->revision : Revision{};
}

CommitSequence DurableStore::commit_sequence() const noexcept {
  return impl_ != nullptr ? impl_->commit_sequence : CommitSequence{};
}

std::uint64_t DurableStore::generation() const noexcept {
  return impl_ != nullptr ? impl_->generation : 0;
}

const Digest& DurableStore::fencing_digest() const noexcept {
  static const Digest kUnset;
  return impl_ != nullptr ? impl_->fencing_digest : kUnset;
}

bool DurableStore::empty_store() const noexcept { return generation() == 0; }

void DurableStore::close() noexcept {
  if (impl_ == nullptr) return;
  impl_->lock.reset();
  impl_->open = false;
}

Result<OrchestratorState> DurableStore::load() const {
  if (!is_open()) {
    return failure<OrchestratorState>(ErrorCode::StorageFailure, "store",
                                      "the store is not open");
  }
  const Impl& impl = *impl_;
  if (impl.generation == 0) {
    return failure<OrchestratorState>(ErrorCode::RecordNotFound, "store",
                                      "the store has no committed generation");
  }
  auto manifest_bytes = read_whole_file(impl.manifest_path());
  if (!manifest_bytes.ok()) return manifest_bytes.error();
  if (!manifest_bytes.value().has_value()) {
    return failure<OrchestratorState>(ErrorCode::StorageFailure, path_to_utf8(impl.manifest_path()),
                                      "the manifest is missing");
  }
  auto frame = decode_record_file(*manifest_bytes.value(), RecordKind::Manifest);
  if (!frame.ok()) return frame.error();
  const std::vector<std::uint8_t>& manifest_payload = frame.value().payload;
  auto manifest = decode_manifest_payload(manifest_payload.data(), manifest_payload.size());
  if (!manifest.ok()) return manifest.error();
  if (manifest.value().generation != impl.generation) {
    return failure<OrchestratorState>(
        ErrorCode::IntegrityFailure, "manifest.generation",
        "the manifest names generation " + std::to_string(manifest.value().generation) +
            " but the store recovered generation " + std::to_string(impl.generation));
  }
  auto record = read_state_record(
      impl.records_dir() / named_path(state_record_name(manifest.value().generation)),
      manifest.value().generation);
  if (!record.ok()) return record.error();
  const Status consistent = check_manifest_matches_record(manifest.value(), record.value());
  if (!consistent.ok()) return consistent.error();
  if (record.value().state.revision != impl.revision ||
      record.value().state.commit_sequence != impl.commit_sequence) {
    return failure<OrchestratorState>(
        ErrorCode::IntegrityFailure, "state",
        "the durable state revision " + record.value().state.revision.to_string() +
            " / commit sequence " + record.value().state.commit_sequence.to_string() +
            " disagree with the open store revision " + impl.revision.to_string() +
            " / commit sequence " + impl.commit_sequence.to_string());
  }
  return success(record.value().state);
}

Result<Revision> DurableStore::commit(const OrchestratorState& state) {
  if (!is_open()) {
    return failure<Revision>(ErrorCode::CommitFailure, "store", "the store is not open");
  }
  Impl& impl = *impl_;
  if (!state.initialized()) {
    return failure<Revision>(ErrorCode::CommitFailure, "state",
                             "the state is not initialized; refusing to commit an unset state");
  }

  // Strict monotonicity: a commit may only follow the durable revision and
  // commit sequence by exactly one step, for both counters independently.
  const std::uint64_t expected_revision = impl.revision.value() + 1ull;
  if (state.revision.value() != expected_revision) {
    return failure<Revision>(
        ErrorCode::GenerationRegression, "state.revision",
        "state revision " + std::to_string(state.revision.value()) +
            " does not follow durable revision " + std::to_string(impl.revision.value()) +
            " (expected " + std::to_string(expected_revision) + ")");
  }
  const std::uint64_t expected_sequence = impl.commit_sequence.value() + 1ull;
  if (state.commit_sequence.value() != expected_sequence) {
    return failure<Revision>(
        ErrorCode::GenerationRegression, "state.commit_sequence",
        "state commit sequence " + std::to_string(state.commit_sequence.value()) +
            " does not follow durable commit sequence " +
            std::to_string(impl.commit_sequence.value()) + " (expected " +
            std::to_string(expected_sequence) + ")");
  }

  // Encoding happens before anything is written: a Writer failure aborts the
  // commit with the encoder's own error and leaves the store untouched.
  Writer writer;
  const Status encoded = encode_state(state, writer);
  if (!encoded.ok()) return encoded.error();
  if (static_cast<std::uint64_t>(writer.size()) > kMaxRecordPayloadBytes) {
    return failure<Revision>(ErrorCode::BoundedLimitExceeded, "state",
                             "the encoded state exceeds the maximum record payload size");
  }
  const std::vector<std::uint8_t> payload = writer.buffer();

  const std::uint64_t next_generation = impl.generation + 1ull;
  const std::filesystem::path staged_record =
      impl.staging_dir() / named_path(state_record_name(next_generation) + ".tmp");
  const std::filesystem::path final_record =
      impl.records_dir() / named_path(state_record_name(next_generation));
  const std::filesystem::path staged_manifest =
      impl.staging_dir() / named_path(kStagedManifestName);
  const std::filesystem::path staged_fencing =
      impl.staging_dir() / named_path(kStagedFencingName);
  const std::filesystem::path final_manifest = impl.manifest_path();
  const std::filesystem::path final_fencing = impl.fencing_path();

  bool record_published = false;
  bool manifest_published = false;

  Result<Revision> outcome = [&]() -> Result<Revision> {
    const std::vector<std::uint8_t> record_bytes = frame_bytes(RecordKind::State, payload);
    if (record_bytes.empty()) {
      return failure<Revision>(ErrorCode::InternalError, "frame",
                               "the state record frame could not be encoded");
    }

    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::BeforeStagingWrite));
    NativeFile staged_handle;
    FCO_RETURN_IF_ERROR(commit_io(
        begin_staged_write(staged_record, record_bytes.data(), record_bytes.size(), staged_handle),
        "record staging"));
    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::AfterStagingWritten));
    FCO_RETURN_IF_ERROR(commit_io(flush_file(staged_handle, staged_record), "record staging"));
    staged_handle.reset();
    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::AfterStagingFlushed));
    Digest record_digest;
    FCO_RETURN_IF_ERROR(commit_io(
        verify_staged_frame(staged_record, RecordKind::State, payload, record_digest),
        "record staging"));
    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::AfterStagingVerified));
    FCO_RETURN_IF_ERROR(commit_io(publish_file(staged_record, final_record),
                                  "record publication"));
    record_published = true;
    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::AfterRecordPublished));

    Manifest manifest;
    manifest.format_version = kStorageFormatVersion;
    manifest.generation = next_generation;
    manifest.payload_bytes = static_cast<std::uint64_t>(payload.size());
    manifest.payload_crc32 = crc32(payload.data(), payload.size(), 0);
    manifest.record_digest = record_digest;
    manifest.commit_sequence = state.commit_sequence;
    manifest.revision = state.revision;
    manifest.created_at_micros = state.updated_at_micros;
    std::vector<std::uint8_t> manifest_payload;
    FCO_RETURN_IF_ERROR(encode_manifest_payload(manifest, manifest_payload));
    const std::vector<std::uint8_t> manifest_bytes =
        frame_bytes(RecordKind::Manifest, manifest_payload);
    if (manifest_bytes.empty()) {
      return failure<Revision>(ErrorCode::InternalError, "frame",
                               "the manifest frame could not be encoded");
    }

    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::BeforeManifestWrite));
    FCO_RETURN_IF_ERROR(
        commit_io(write_whole_file(staged_manifest, manifest_bytes), "manifest staging"));
    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::AfterManifestFlushed));
    Digest manifest_frame_digest;
    FCO_RETURN_IF_ERROR(commit_io(verify_staged_frame(staged_manifest, RecordKind::Manifest,
                                                      manifest_payload, manifest_frame_digest),
                                  "manifest staging"));
    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::AfterManifestVerified));
    FCO_RETURN_IF_ERROR(commit_io(publish_file(staged_manifest, final_manifest),
                                  "manifest publication"));
    manifest_published = true;
    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::AfterManifestPublished));

    // The fencing record binds the manifest file that was actually published,
    // not the buffer that was staged for it.
    auto manifest_file_digest = digest_of_file(final_manifest);
    if (!manifest_file_digest.ok()) {
      return map_commit_error(manifest_file_digest.error(), "fencing staging");
    }
    if (!manifest_file_digest.value().has_value()) {
      return failure<Revision>(ErrorCode::CommitFailure, "fencing staging",
                               "the published manifest could not be read back");
    }
    FencingRecord fencing;
    fencing.format_version = kStorageFormatVersion;
    fencing.published_generation = next_generation;
    fencing.commit_sequence = state.commit_sequence;
    fencing.manifest_digest = *manifest_file_digest.value();
    std::vector<std::uint8_t> fencing_payload;
    FCO_RETURN_IF_ERROR(encode_fencing_payload(fencing, fencing_payload));
    const std::vector<std::uint8_t> fencing_bytes =
        frame_bytes(RecordKind::Fencing, fencing_payload);
    if (fencing_bytes.empty()) {
      return failure<Revision>(ErrorCode::InternalError, "frame",
                               "the fencing frame could not be encoded");
    }
    FCO_RETURN_IF_ERROR(
        commit_io(write_whole_file(staged_fencing, fencing_bytes), "fencing staging"));
    Digest fencing_frame_digest;
    FCO_RETURN_IF_ERROR(commit_io(verify_staged_frame(staged_fencing, RecordKind::Fencing,
                                                      fencing_payload, fencing_frame_digest),
                                  "fencing staging"));
    FCO_RETURN_IF_ERROR(commit_io(publish_file(staged_fencing, final_fencing),
                                  "fencing publication"));
    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::AfterFencingPublished));

    auto pruned = prune_records(impl.records_dir(), next_generation);
    if (!pruned.ok()) return map_commit_error(pruned.error(), "retention");
    impl.report.pruned_records += pruned.value();
    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::AfterRetention));

    impl.generation = next_generation;
    impl.revision = state.revision;
    impl.commit_sequence = state.commit_sequence;
    impl.fencing_digest = fencing.manifest_digest;

    FCO_RETURN_IF_ERROR(fire_stage(impl.hooks, CommitStage::CommitComplete));
    return success(state.revision);
  }();

  if (!outcome.ok()) {
    // A published record that no manifest names would be an orphan generation:
    // recovery would have to guess between it and the last manifest. It is
    // removed here so the store falls back to exactly one authoritative
    // generation.
    if (record_published && !manifest_published) {
      bool removed = false;
      (void)remove_file(final_record, &removed);
    }
    bool removed = false;
    (void)remove_file(staged_record, &removed);
    (void)remove_file(staged_manifest, &removed);
    (void)remove_file(staged_fencing, &removed);
  }
  return outcome;
}

std::string_view DurableStore::lock_file_name() noexcept { return "fco.lock"; }
std::string_view DurableStore::manifest_file_name() noexcept { return "fco.manifest"; }
std::string_view DurableStore::fencing_file_name() noexcept { return "fco.fencing"; }
std::string_view DurableStore::records_directory_name() noexcept { return "records"; }
std::string_view DurableStore::staging_directory_name() noexcept { return "staging"; }

std::string DurableStore::state_record_name(std::uint64_t generation) {
  return "state-" + std::to_string(generation) + ".fco";
}

}  // namespace fco
