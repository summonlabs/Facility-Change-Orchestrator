#pragma once

// Strict, deterministic JSON.
//
// This is a deliberately small codec used for the command line surface and for
// fixtures. It is strict by construction:
//   - no trailing content after the top-level value;
//   - no duplicate object keys (rejected, not last-wins);
//   - no comments, no single quotes, no unquoted keys, no trailing commas;
//   - no NaN/Infinity literals, no leading zeros, no leading '+', no bare '.',
//     no '1.' and no exponent without digits;
//   - strings must be valid UTF-8 without lone surrogates, unescaped control
//     characters, or invalid \u escapes;
//   - depth, string length, and entry count are bounded and exceeding a bound is
//     an error rather than a truncation.
//
// Object key order is preserved exactly as written and is part of the value, so
// serialisation is a pure function of the value.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fco/error.hpp"

namespace fco::json {

enum class Kind : std::uint8_t {
  Unspecified = 0,
  Null = 1,
  Bool = 2,
  Number = 3,
  String = 4,
  Array = 5,
  Object = 6,
};

struct Number {
  std::int64_t integer = 0;
  double real = 0.0;
  bool integral = true;
};

struct Limits {
  std::size_t max_depth = 64;
  std::size_t max_string_bytes = 1u << 20;
  std::size_t max_entries = 200000;
  std::size_t max_document_bytes = 8u << 20;
};

class Value {
 public:
  using Array = std::vector<Value>;
  using Member = std::pair<std::string, Value>;
  using Object = std::vector<Member>;

  Value() = default;

  [[nodiscard]] static Value make_null();
  [[nodiscard]] static Value make_bool(bool value);
  [[nodiscard]] static Value make_string(std::string value);
  [[nodiscard]] static Value make_array(Array value);
  [[nodiscard]] static Value make_object(Object value);
  [[nodiscard]] static Value make_integer(std::int64_t value);
  [[nodiscard]] static Value make_real(double value);

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return kind_ == Kind::Bool; }
  [[nodiscard]] bool is_number() const noexcept { return kind_ == Kind::Number; }
  [[nodiscard]] bool is_string() const noexcept { return kind_ == Kind::String; }
  [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::Array; }
  [[nodiscard]] bool is_object() const noexcept { return kind_ == Kind::Object; }
  [[nodiscard]] bool specified() const noexcept { return kind_ != Kind::Unspecified; }

  [[nodiscard]] std::size_t size() const noexcept;

  [[nodiscard]] Result<bool> as_bool(std::string_view field) const;
  [[nodiscard]] Result<std::string> as_string(std::string_view field) const;
  [[nodiscard]] Result<std::uint64_t> as_u64(std::string_view field) const;
  [[nodiscard]] Result<std::int64_t> as_i64(std::string_view field) const;
  // Returns a copy: fco::Result<T> is a std::variant<T, Error> and the standard
  // library forbids reference alternatives, so Result<const Array&> cannot be
  // instantiated (MSVC: C2625, illegal union member).
  [[nodiscard]] Result<Array> as_array(std::string_view field) const;
  [[nodiscard]] Result<Object> as_object(std::string_view field) const;

  // Object member lookup. Returns nullptr for a non-object or an absent key.
  [[nodiscard]] const Value* find(std::string_view key) const noexcept;
  // Returns a copy; see the note on as_array above.
  [[nodiscard]] Result<Value> require(std::string_view key,
                                      std::string_view context) const;

  [[nodiscard]] const Array& elements() const noexcept { return array_; }
  [[nodiscard]] const Object& members() const noexcept { return object_; }
  [[nodiscard]] const Number& number() const noexcept { return number_; }

  friend bool operator==(const Value& a, const Value& b) noexcept;

 private:
  Kind kind_ = Kind::Unspecified;
  bool bool_ = false;
  Number number_{};
  std::string string_;
  Array array_;
  Object object_;
};

// Parses a complete JSON document. Any trailing byte after the top-level value
// is a TrailingBytes rejection.
[[nodiscard]] Result<Value> parse(std::string_view text, const Limits& limits = Limits{});

// Canonical serialisation. When `pretty` is true the output is indented with
// two spaces and terminated by a newline; when false it is compact and has no
// trailing newline. In both cases member order is preserved.
[[nodiscard]] std::string serialize(const Value& value, bool pretty = false);

// Escapes a single string as a JSON string literal, including the quotes.
[[nodiscard]] std::string quote(std::string_view text);

// True when the byte sequence is valid UTF-8 with no lone surrogate encodings.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

}  // namespace fco::json
