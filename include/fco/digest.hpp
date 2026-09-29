#pragma once

// Content identity.
//
// Digest is a fixed-width SHA-256 content identity. It is used for evidence
// digests, generation-set digests, plan evidence bindings, and durable record
// integrity. It is deliberately not hashable-by-address and not comparable by
// anything other than its bytes, so that two runs that process the same content
// produce the same identity regardless of allocator or iteration order.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "fco/error.hpp"

namespace fco {

// IEEE CRC-32 (reflected, polynomial 0xEDB88320). Used as a fast structural
// integrity check on durable record frames. SHA-256 remains the content
// identity; CRC-32 is only a corruption detector.
[[nodiscard]] std::uint32_t crc32(const void* data, std::size_t length, std::uint32_t seed = 0);

class Digest {
 public:
  static constexpr std::size_t kBytes = 32;
  using Bytes = std::array<std::uint8_t, kBytes>;

  // Unset digest. valid() is false, and it is never treated as a match.
  Digest() = default;
  explicit Digest(const Bytes& bytes) : bytes_(bytes) {}

  [[nodiscard]] static Digest from_bytes(const Bytes& bytes) { return Digest(bytes); }
  [[nodiscard]] static Result<Digest> from_hex(std::string_view hex);

  // True when the digest was produced by a hashing operation rather than
  // default-constructed. An all-zero digest is never accepted as real content
  // identity, which is what keeps "missing" from becoming "matches".
  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string to_hex() const;

  friend bool operator==(const Digest& a, const Digest& b) noexcept = default;
  friend auto operator<=>(const Digest& a, const Digest& b) noexcept = default;

 private:
  Bytes bytes_{};
};

class Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = Digest::kBytes;
  static constexpr std::size_t kBlockBytes = 64;

  // The initial hash state is installed by the constructor. A defaulted
  // constructor would leave the state zero-initialised, which hashes to a
  // different value than the SHA-256 of the empty message; the known-answer
  // vectors in the test suite pin this down.
  Sha256() noexcept { reset(); }

  void reset() noexcept;

  void update(const void* data, std::size_t length) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  void update_u8(std::uint8_t value) noexcept;
  void update_u16(std::uint16_t value) noexcept;  // big-endian
  void update_u32(std::uint32_t value) noexcept;  // big-endian
  void update_u64(std::uint64_t value) noexcept;  // big-endian

  // Idempotent. Finalization never mutates observable state twice.
  [[nodiscard]] Digest finish() noexcept;

  [[nodiscard]] static Digest of(std::string_view text) noexcept;

 private:
  void process_block(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, kBlockBytes> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
  Digest result_{};
  bool finished_ = false;
};

struct DigestHash {
  [[nodiscard]] std::size_t operator()(const Digest& d) const noexcept;
};

}  // namespace fco

template <>
struct std::hash<fco::Digest> {
  [[nodiscard]] std::size_t operator()(const fco::Digest& d) const noexcept {
    return fco::DigestHash{}(d);
  }
};
