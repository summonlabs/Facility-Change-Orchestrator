#pragma once

// Durable record codec.
//
// The encoding is explicit, bounded, and little-endian. Every primitive is read
// with an explicit field name so a rejection names the field that failed, and
// every decoder enforces: exact length, no trailing bytes, no non-canonical
// boolean, no out-of-domain enum, no unset identity, bounded strings and
// collections, reserved fields that must be zero, and an unsupported format
// version rejected before any other interpretation.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fco/digest.hpp"
#include "fco/error.hpp"
#include "fco/state.hpp"
#include "fco/version.hpp"

namespace fco {

// Encoding is total: it either produces a record the decoder will accept, or it
// reports the first bound it could not honour. After the first failure every
// further write is a no-op, so a partially encoded buffer can never be
// mistaken for a valid one.
class Writer {
 public:
  Writer() = default;

  void u8(std::uint8_t value);
  void boolean(bool value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void bytes(const void* data, std::size_t length);
  void string(std::string_view value);
  void digest(const Digest& value);
  void count(std::uint64_t value, std::string_view field,
             std::uint64_t maximum = kMaxCollectionEntries);
  void reserve(std::uint32_t value, std::string_view field);

  void fail(ErrorCode code, std::string subject, std::string message);
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] Status status() const;

  [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

 private:
  std::vector<std::uint8_t> buffer_;
  Error error_;
  bool failed_ = false;
};

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t length) noexcept : data_(data), size_(length) {}

  [[nodiscard]] Result<std::uint8_t> u8(std::string_view field);
  [[nodiscard]] Result<bool> boolean(std::string_view field);
  [[nodiscard]] Result<std::uint16_t> u16(std::string_view field);
  [[nodiscard]] Result<std::uint32_t> u32(std::string_view field);
  [[nodiscard]] Result<std::uint64_t> u64(std::string_view field);
  [[nodiscard]] Result<std::int64_t> i64(std::string_view field);
  [[nodiscard]] Result<std::string> string(std::string_view field,
                                           std::size_t maximum = kMaxStringBytes);
  [[nodiscard]] Result<Digest> digest(std::string_view field);
  [[nodiscard]] Result<std::uint64_t> count(std::string_view field,
                                            std::uint64_t maximum = kMaxCollectionEntries);
  [[nodiscard]] Result<void> reserved_u32(std::string_view field);
  [[nodiscard]] Result<void> reserved_u16(std::string_view field);
  [[nodiscard]] Result<void> reserved_u64(std::string_view field);
  [[nodiscard]] Result<void> expect_end(std::string_view field);

  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

 private:
  [[nodiscard]] Result<const std::uint8_t*> take(std::size_t length, std::string_view field);

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
};

// Format-version prefix shared by every durable payload.
void write_version_prefix(Writer& writer);
[[nodiscard]] Result<void> read_version_prefix(Reader& reader, std::string_view field);

[[nodiscard]] Status encode_state(const OrchestratorState& state, Writer& writer);
[[nodiscard]] Result<OrchestratorState> decode_state(const std::uint8_t* data, std::size_t length);

[[nodiscard]] Result<std::vector<std::uint8_t>> encode_state_bytes(const OrchestratorState& state);

}  // namespace fco
