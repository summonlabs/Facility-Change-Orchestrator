#include "fco/digest.hpp"

#include <array>
#include <cstring>

namespace fco {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = ((value & 1u) != 0u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
    }
    table[index] = value;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32Table = make_crc32_table();

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned shift) noexcept {
  return (value >> shift) | (value << (32u - shift));
}

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t length, std::uint32_t seed) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = ~seed;
  for (std::size_t i = 0; i < length; ++i) {
    crc = kCrc32Table[(crc ^ bytes[i]) & 0xffu] ^ (crc >> 8);
  }
  return ~crc;
}

Result<Digest> Digest::from_hex(std::string_view hex) {
  if (hex.size() != kBytes * 2) {
    return failure<Digest>(ErrorCode::MalformedInput, std::string(hex),
                           "digest hex must be exactly 64 characters");
  }
  Bytes bytes{};
  for (std::size_t i = 0; i < kBytes; ++i) {
    const int high = hex_value(hex[2 * i]);
    const int low = hex_value(hex[2 * i + 1]);
    if (high < 0 || low < 0) {
      return failure<Digest>(ErrorCode::MalformedInput, std::string(hex),
                             "digest hex contains a non-hexadecimal character");
    }
    bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return success(Digest(bytes));
}

bool Digest::valid() const noexcept {
  for (std::uint8_t byte : bytes_) {
    if (byte != 0) return true;
  }
  return false;
}

std::string Digest::to_hex() const {
  std::string out(kBytes * 2, '0');
  for (std::size_t i = 0; i < kBytes; ++i) {
    out[2 * i] = kHexDigits[(bytes_[i] >> 4) & 0x0fu];
    out[2 * i + 1] = kHexDigits[bytes_[i] & 0x0fu];
  }
  return out;
}

void Sha256::reset() noexcept {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  buffer_.fill(0);
  buffered_ = 0;
  total_bytes_ = 0;
  result_ = Digest{};
  finished_ = false;
}

void Sha256::process_block(const std::uint8_t* block) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           (static_cast<std::uint32_t>(block[i * 4 + 3]));
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t length) noexcept {
  if (finished_ || length == 0) return;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += length;
  std::size_t offset = 0;
  while (offset < length) {
    const std::size_t room = kBlockBytes - buffered_;
    const std::size_t take = (length - offset < room) ? (length - offset) : room;
    std::memcpy(buffer_.data() + buffered_, bytes + offset, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == kBlockBytes) {
      process_block(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Sha256::update_u8(std::uint8_t value) noexcept { update(&value, 1); }

void Sha256::update_u16(std::uint16_t value) noexcept {
  const std::uint8_t bytes[2] = {static_cast<std::uint8_t>((value >> 8) & 0xffu),
                                 static_cast<std::uint8_t>(value & 0xffu)};
  update(bytes, 2);
}

void Sha256::update_u32(std::uint32_t value) noexcept {
  const std::uint8_t bytes[4] = {static_cast<std::uint8_t>((value >> 24) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 16) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 8) & 0xffu),
                                 static_cast<std::uint8_t>(value & 0xffu)};
  update(bytes, 4);
}

void Sha256::update_u64(std::uint64_t value) noexcept {
  const std::uint8_t bytes[8] = {static_cast<std::uint8_t>((value >> 56) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 48) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 40) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 32) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 24) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 16) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 8) & 0xffu),
                                 static_cast<std::uint8_t>(value & 0xffu)};
  update(bytes, 8);
}

Digest Sha256::finish() noexcept {
  if (finished_) return result_;

  const std::uint64_t bit_length = total_bytes_ * 8ull;
  const std::uint8_t marker = 0x80u;
  update(&marker, 1);
  const std::uint8_t zero = 0x00u;
  while (buffered_ != kBlockBytes - 8) {
    update(&zero, 1);
  }
  std::uint8_t length_bytes[8];
  for (std::size_t i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>((bit_length >> (56u - (8u * i))) & 0xffull);
  }
  update(length_bytes, 8);

  Digest::Bytes out{};
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xffu);
    out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xffu);
    out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xffu);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xffu);
  }
  result_ = Digest(out);
  finished_ = true;
  return result_;
}

Digest Sha256::of(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

std::size_t DigestHash::operator()(const Digest& d) const noexcept {
  std::size_t seed = 1469598103934665603ull;
  for (std::uint8_t byte : d.bytes()) {
    seed ^= static_cast<std::size_t>(byte);
    seed *= 1099511628211ull;
  }
  return seed;
}

}  // namespace fco
