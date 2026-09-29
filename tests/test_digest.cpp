// Tests for fco/digest.hpp: SHA-256 known-answer vectors, CRC-32, hex codec,
// digest validity, and the incremental-hashing property (one-shot hashing must
// agree with any chunking of the same bytes).

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "fco/digest.hpp"

namespace fco::test {

template <>
struct ValuePrinter<fco::Digest> {
  [[nodiscard]] static std::string print(const fco::Digest& value) { return value.to_hex(); }
};

}  // namespace fco::test

namespace {

using fco::Digest;
using fco::Sha256;

[[nodiscard]] Digest hash_bytes(const std::vector<std::uint8_t>& bytes) {
  Sha256 hasher;
  hasher.update(bytes.data(), bytes.size());
  return hasher.finish();
}

[[nodiscard]] std::string hex_of(std::string_view text) { return Sha256::of(text).to_hex(); }

[[nodiscard]] std::vector<std::uint8_t> random_bytes(std::mt19937_64& rng, std::size_t length) {
  std::uniform_int_distribution<int> byte_dist(0, 255);
  std::vector<std::uint8_t> bytes(length);
  for (std::size_t i = 0; i < length; ++i) {
    bytes[i] = static_cast<std::uint8_t>(byte_dist(rng));
  }
  return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> big_endian_u32(std::uint32_t value) {
  return {static_cast<std::uint8_t>((value >> 24) & 0xFFu),
          static_cast<std::uint8_t>((value >> 16) & 0xFFu),
          static_cast<std::uint8_t>((value >> 8) & 0xFFu),
          static_cast<std::uint8_t>(value & 0xFFu)};
}

[[nodiscard]] std::vector<std::uint8_t> big_endian_u64(std::uint64_t value) {
  std::vector<std::uint8_t> out;
  out.reserve(8);
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFull));
  }
  return out;
}

void append(std::vector<std::uint8_t>& target, const std::vector<std::uint8_t>& source) {
  target.insert(target.end(), source.begin(), source.end());
}

}  // namespace

// ---------------------------------------------------------------------------
// SHA-256 known-answer vectors (NIST FIPS 180-4 / NESSIE test vectors).
// ---------------------------------------------------------------------------
FCO_TEST(digest, sha256_known_answers) {
  FCO_CHECK_EQ(hex_of(""),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  FCO_CHECK_EQ(hex_of("abc"),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  FCO_CHECK_EQ(
      hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  FCO_CHECK_EQ(hex_of("The quick brown fox jumps over the lazy dog"),
               std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));

  // One million 'a' characters.
  const std::string million_a(1000000, 'a');
  FCO_CHECK_EQ(hex_of(million_a),
               std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

  // The same vectors through an explicitly constructed hasher, exactly as a
  // caller of the public interface would use it.
  Sha256 empty;
  empty.update("");
  FCO_CHECK_EQ(empty.finish().to_hex(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  Sha256 abc;
  abc.update("abc");
  FCO_CHECK_EQ(abc.finish().to_hex(),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

// The SHA-256 core itself (padding, block processing, length encoding) is a
// pure function of the state the hasher starts from. Hashing the published
// vectors after an explicit reset() isolates the algorithm from the way a
// freshly constructed Sha256 is initialised.
FCO_TEST(digest, sha256_algorithm_matches_known_answers_after_reset) {
  const std::string million_a(1000000, 'a');
  const std::pair<std::string, std::string> vectors[] = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {"The quick brown fox jumps over the lazy dog",
       "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
      {million_a, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
  };

  for (const auto& vector : vectors) {
    Sha256 hasher;
    hasher.reset();
    hasher.update(vector.first);
    FCO_CHECK_EQ(hasher.finish().to_hex(), vector.second);
  }
}

FCO_TEST(digest, sha256_one_shot_matches_chunked_and_bytewise) {
  const std::string message =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
      "The quick brown fox jumps over the lazy dog";

  const std::vector<std::uint8_t> bytes(message.begin(), message.end());

  Sha256 whole;
  whole.update(bytes.data(), bytes.size());

  Sha256 split;
  for (std::size_t offset = 0; offset < bytes.size(); offset += 7) {
    const std::size_t take = (bytes.size() - offset < 7) ? (bytes.size() - offset) : 7;
    split.update(bytes.data() + offset, take);
  }

  Sha256 bytewise;
  for (std::uint8_t value : bytes) bytewise.update_u8(value);

  const Digest expected = Sha256::of(message);
  FCO_CHECK_EQ(whole.finish(), expected);
  FCO_CHECK_EQ(split.finish(), expected);
  FCO_CHECK_EQ(bytewise.finish(), expected);
}

FCO_TEST(digest, sha256_finish_is_idempotent) {
  Sha256 hasher;
  hasher.update("abc");
  const Digest first = hasher.finish();
  const Digest second = hasher.finish();
  FCO_CHECK_EQ(first, second);

  // Updating a finalised hasher must not change the result.
  hasher.update("more");
  FCO_CHECK_EQ(hasher.finish(), first);

  // Block-boundary lengths exercise the padding path in both directions.
  for (std::size_t length : {std::size_t{55}, std::size_t{56}, std::size_t{57}, std::size_t{63},
                             std::size_t{64}, std::size_t{65}, std::size_t{119}, std::size_t{120}}) {
    const std::string text(length, 'x');
    Sha256 chunked;
    for (std::size_t offset = 0; offset < length; offset += 3) {
      const std::size_t take = (length - offset < 3) ? (length - offset) : 3;
      chunked.update(text.data() + offset, take);
    }
    FCO_CHECK_EQ(chunked.finish(), Sha256::of(text));
  }
}

// Regression test: a freshly constructed Sha256 must already be in the SHA-256
// initial state, so it must agree with a hasher that has been reset(). This
// caught a constructor that left the state zero-initialised, which made every
// digest produced through the public interface -- including Sha256::of(), used
// for all content identity -- something other than SHA-256.
FCO_TEST(digest, sha256_default_construction_matches_reset_state) {
  const char* const messages[] = {"", "abc", "The quick brown fox jumps over the lazy dog"};

  for (const char* message : messages) {
    Sha256 fresh;
    fresh.update(message);

    Sha256 reset_hasher;
    reset_hasher.reset();
    reset_hasher.update(message);

    FCO_CHECK_EQ(fresh.finish(), reset_hasher.finish());
  }
}

// ---------------------------------------------------------------------------
// CRC-32.
// ---------------------------------------------------------------------------
FCO_TEST(digest, crc32_known_answers) {
  const char* check = "123456789";
  FCO_CHECK_EQ(fco::crc32(check, 9), std::uint32_t{0xCBF43926u});
  const char empty[] = "";
  FCO_CHECK_EQ(fco::crc32(empty, 0), std::uint32_t{0x00000000u});
  FCO_CHECK_EQ(fco::crc32("a", 1, 0), fco::crc32("a", 1, 0));
}

// ---------------------------------------------------------------------------
// Hex codec.
// ---------------------------------------------------------------------------
FCO_TEST(digest, from_hex_accepts_both_cases_and_round_trips) {
  const std::string lower("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  std::string upper = lower;
  for (char& c : upper) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  FCO_CHECK(upper != lower);

  const auto lower_digest = Digest::from_hex(lower);
  const auto upper_digest = Digest::from_hex(upper);
  FCO_REQUIRE(lower_digest.ok());
  FCO_REQUIRE(upper_digest.ok());
  FCO_CHECK_EQ(lower_digest.value(), upper_digest.value());
  FCO_CHECK_EQ(lower_digest.value().to_hex(), lower);
  FCO_CHECK_EQ(upper_digest.value().to_hex(), lower);
  FCO_CHECK_EQ(lower_digest.value().to_hex().size(), std::size_t{64});
}

FCO_TEST(digest, from_hex_rejects_wrong_length_and_non_hex) {
  FCO_CHECK_ERROR(Digest::from_hex(""), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(Digest::from_hex(std::string(63, 'a')), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(Digest::from_hex(std::string(65, 'a')), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(Digest::from_hex(std::string(64, 'z')), fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(Digest::from_hex("g3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
                  fco::ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(Digest::from_hex("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991b7852B85 "),
                  fco::ErrorCode::MalformedInput);
}

// ---------------------------------------------------------------------------
// Validity.
// ---------------------------------------------------------------------------
FCO_TEST(digest, validity_follows_content_identity) {
  const Digest unset;
  FCO_CHECK(!unset.valid());
  FCO_CHECK_EQ(unset.to_hex(), std::string(64, '0'));

  const Digest real = Sha256::of("abc");
  FCO_CHECK(real.valid());

  const Digest other = Sha256::of("abd");
  FCO_CHECK(real != other);
  FCO_CHECK(real < other || other < real);

  // An all-zero digest is "missing", never "matches".
  const auto zeros = Digest::from_hex(std::string(64, '0'));
  FCO_REQUIRE(zeros.ok());
  FCO_CHECK(!zeros.value().valid());
  FCO_CHECK(zeros.value() == unset);
}

FCO_TEST(digest, hash_is_consistent_with_equality) {
  const Digest a = Sha256::of("abc");
  const Digest b = Sha256::of("abc");
  FCO_CHECK(a == b);
  FCO_CHECK_EQ(std::hash<Digest>{}(a), std::hash<Digest>{}(b));

  std::unordered_set<Digest> seen;
  seen.insert(a);
  seen.insert(Sha256::of("abd"));
  seen.insert(b);
  FCO_CHECK_EQ(seen.size(), std::size_t{2});
}

// ---------------------------------------------------------------------------
// Property: incremental hashing is independent of the chunking.
// ---------------------------------------------------------------------------
FCO_TEST(digest, property_chunking_does_not_change_sha256) {
  std::mt19937_64 rng(0x5EED1234ULL);
  std::uniform_int_distribution<std::size_t> length_dist(0, 300);
  std::uniform_int_distribution<std::size_t> chunk_dist(1, 17);

  for (int iteration = 0; iteration < 200; ++iteration) {
    const std::size_t length = length_dist(rng);
    const std::vector<std::uint8_t> bytes = random_bytes(rng, length);

    Sha256 one_shot;
    one_shot.update(bytes.data(), bytes.size());
    const Digest expected = one_shot.finish();

    Sha256 chunked;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      std::size_t take = chunk_dist(rng);
      if (take > bytes.size() - offset) take = bytes.size() - offset;
      chunked.update(bytes.data() + offset, take);
      offset += take;
    }
    FCO_CHECK_EQ(chunked.finish(), expected);

    Sha256 bytewise;
    for (std::uint8_t value : bytes) bytewise.update_u8(value);
    FCO_CHECK_EQ(bytewise.finish(), expected);

    // Text-view hashing of the same bytes must agree as well.
    const std::string text(bytes.begin(), bytes.end());
    FCO_CHECK_EQ(Sha256::of(text), expected);
  }
}

FCO_TEST(digest, property_integer_updates_match_big_endian_bytes) {
  std::mt19937_64 rng(0x5EED1234ULL);

  for (int iteration = 0; iteration < 200; ++iteration) {
    std::uniform_int_distribution<int> count_dist(0, 40);
    const int count = count_dist(rng);

    Sha256 incremental_u32;
    Sha256 flat_u32;
    std::vector<std::uint8_t> bytes_u32;
    for (int i = 0; i < count; ++i) {
      const std::uint32_t value = static_cast<std::uint32_t>(rng());
      incremental_u32.update_u32(value);
      append(bytes_u32, big_endian_u32(value));
    }
    flat_u32.update(bytes_u32.data(), bytes_u32.size());
    FCO_CHECK_EQ(incremental_u32.finish(), flat_u32.finish());

    Sha256 incremental_u64;
    Sha256 flat_u64;
    std::vector<std::uint8_t> bytes_u64;
    for (int i = 0; i < count; ++i) {
      const std::uint64_t value = rng();
      incremental_u64.update_u64(value);
      append(bytes_u64, big_endian_u64(value));
    }
    flat_u64.update(bytes_u64.data(), bytes_u64.size());
    FCO_CHECK_EQ(incremental_u64.finish(), flat_u64.finish());

    // Mixed-width interleaving must also match the concatenated byte image.
    Sha256 mixed;
    Sha256 mixed_flat;
    std::vector<std::uint8_t> mixed_bytes;
    for (int i = 0; i < count; ++i) {
      const std::uint8_t a = static_cast<std::uint8_t>(rng() & 0xFFull);
      const std::uint16_t b = static_cast<std::uint16_t>(rng() & 0xFFFFull);
      const std::uint32_t c = static_cast<std::uint32_t>(rng());
      const std::uint64_t d = rng();
      mixed.update_u8(a);
      mixed.update_u16(b);
      mixed.update_u32(c);
      mixed.update_u64(d);
      mixed_bytes.push_back(a);
      mixed_bytes.push_back(static_cast<std::uint8_t>((b >> 8) & 0xFFu));
      mixed_bytes.push_back(static_cast<std::uint8_t>(b & 0xFFu));
      append(mixed_bytes, big_endian_u32(c));
      append(mixed_bytes, big_endian_u64(d));
    }
    mixed_flat.update(mixed_bytes.data(), mixed_bytes.size());
    FCO_CHECK_EQ(mixed.finish(), mixed_flat.finish());
  }
}

FCO_TEST_MAIN
