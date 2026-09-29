#include "fco/json.hpp"

// ---------------------------------------------------------------------------
// Strict, deterministic JSON codec.
//
// Parsing is a single left-to-right pass. The first violation wins and every
// rejection carries the stable subject "json" together with a message naming
// the byte offset at which the violation was detected, for example
//   unexpected character '}' at byte 12
// Nothing here consults the locale: numbers go through std::from_chars and
// std::to_chars only, and no function throws.
//
// Numbers. An integer literal that fits in std::int64_t is stored as an
// integral Number. Every other valid JSON number (a fraction and/or exponent,
// or an integer literal that does not fit in std::int64_t) is stored as a real
// and is rejected with BoundedLimitExceeded when the converted value would be
// infinite or NaN.
//
// Strings. Raw bytes below 0x20 are rejected, and every non-ASCII sequence is
// validated as strict UTF-8 (no overlongs, no surrogate encodings, nothing
// above U+10FFFF). \u escapes are decoded and re-encoded as UTF-8; a high
// surrogate must be followed immediately by a matching low surrogate, and a
// lone low surrogate is rejected. \u escapes for code points 0x00..0x1F are
// legal JSON and decode to their UTF-8 bytes.
//
// Interface deviation (reported): fco::Result<T> is a std::variant<T, Error>,
// and the standard library forbids reference alternatives, so
// Result<const Array&>, Result<const Object&> and Result<const Value&> cannot
// be instantiated at all (MSVC: C2625 "illegal union member; type ... is
// reference type"). The three affected accessors therefore return values
// instead of references; see include/fco/json.hpp.
// ---------------------------------------------------------------------------

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fco::json {
namespace {

constexpr std::string_view kSubject = "json";

[[nodiscard]] Error json_error(ErrorCode code, std::string message) {
  return make_error(code, std::string(kSubject), std::move(message));
}

[[nodiscard]] Error limit_error(std::string message) {
  return json_error(ErrorCode::BoundedLimitExceeded, std::move(message));
}

[[nodiscard]] std::string byte_suffix(std::size_t offset) {
  return " at byte " + std::to_string(offset);
}

// Printable ASCII is shown as itself; everything else becomes a \xNN escape so
// that messages stay on one line and are identical on every platform.
[[nodiscard]] std::string describe_byte(unsigned char byte) {
  if (byte >= 0x20u && byte < 0x7Fu) {
    return std::string(1, static_cast<char>(byte));
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\\x";
  out.push_back(kHex[(byte >> 4) & 0x0Fu]);
  out.push_back(kHex[byte & 0x0Fu]);
  return out;
}

[[nodiscard]] std::string_view kind_name(Kind kind) noexcept {
  switch (kind) {
    case Kind::Unspecified:
      return "unspecified";
    case Kind::Null:
      return "null";
    case Kind::Bool:
      return "a boolean";
    case Kind::Number:
      return "a number";
    case Kind::String:
      return "a string";
    case Kind::Array:
      return "an array";
    case Kind::Object:
      return "an object";
  }
  return "unspecified";
}

[[nodiscard]] Error wrong_kind(std::string_view field, std::string_view expected, Kind actual) {
  return make_error(ErrorCode::MalformedInput, std::string(field),
                    "field '" + std::string(field) + "' must be " + std::string(expected) +
                        " but is " + std::string(kind_name(actual)));
}

[[nodiscard]] bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] bool is_continuation(unsigned char byte) noexcept {
  return (byte & 0xC0u) == 0x80u;
}

// Length in bytes of the strict UTF-8 sequence starting at text[offset], or 0
// when the sequence is invalid: bad lead byte, truncated sequence, bad
// continuation byte, overlong encoding, encoded surrogate, or a code point
// above U+10FFFF.
[[nodiscard]] std::size_t utf8_sequence_length(std::string_view text, std::size_t offset) noexcept {
  const std::size_t size = text.size();
  const auto byte_at = [&text](std::size_t index) {
    return static_cast<unsigned char>(text[index]);
  };
  const unsigned char lead = byte_at(offset);
  if (lead < 0x80u) {
    return 1;
  }
  if (lead >= 0xC2u && lead <= 0xDFu) {
    if (offset + 1 >= size || !is_continuation(byte_at(offset + 1))) {
      return 0;
    }
    return 2;
  }
  if (lead >= 0xE0u && lead <= 0xEFu) {
    if (offset + 2 >= size) {
      return 0;
    }
    const unsigned char second = byte_at(offset + 1);
    if (!is_continuation(byte_at(offset + 2))) {
      return 0;
    }
    if (lead == 0xE0u && (second < 0xA0u || second > 0xBFu)) {
      return 0;  // overlong
    }
    if (lead == 0xEDu && (second < 0x80u || second > 0x9Fu)) {
      return 0;  // encoded surrogate U+D800..U+DFFF
    }
    return is_continuation(second) ? 3 : 0;
  }
  if (lead >= 0xF0u && lead <= 0xF4u) {
    if (offset + 3 >= size) {
      return 0;
    }
    const unsigned char second = byte_at(offset + 1);
    if (!is_continuation(byte_at(offset + 2)) || !is_continuation(byte_at(offset + 3))) {
      return 0;
    }
    if (lead == 0xF0u && (second < 0x90u || second > 0xBFu)) {
      return 0;  // overlong
    }
    if (lead == 0xF4u && (second < 0x80u || second > 0x8Fu)) {
      return 0;  // above U+10FFFF
    }
    return is_continuation(second) ? 4 : 0;
  }
  return 0;
}

// Encodes a scalar value that has already been checked to be below U+110000
// and outside the surrogate range.
void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point < 0x80u) {
    out.push_back(static_cast<char>(code_point));
    return;
  }
  if (code_point < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    return;
  }
  if (code_point < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    return;
  }
  out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
  out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
}

// Index of the first member (in document order) whose key repeats an earlier
// key, or members.size() when every key is distinct.
[[nodiscard]] std::size_t first_duplicate_member(const Value::Object& members) {
  constexpr std::size_t kQuadraticThreshold = 12;
  const std::size_t count = members.size();
  if (count < 2) {
    return count;
  }
  if (count <= kQuadraticThreshold) {
    for (std::size_t i = 1; i < count; ++i) {
      for (std::size_t j = 0; j < i; ++j) {
        if (members[i].first == members[j].first) {
          return i;
        }
      }
    }
    return count;
  }
  std::unordered_set<std::string_view> seen;
  seen.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (!seen.insert(std::string_view(members[i].first)).second) {
      return i;
    }
  }
  return count;
}

class Parser {
 public:
  Parser(std::string_view text, const Limits& limits) noexcept : text_(text), limits_(limits) {}

  [[nodiscard]] Result<Value> document() {
    if (text_.size() > limits_.max_document_bytes) {
      return limit_error("document of " + std::to_string(text_.size()) +
                         " bytes exceeds the limit of " +
                         std::to_string(limits_.max_document_bytes) + " bytes" +
                         byte_suffix(limits_.max_document_bytes));
    }
    skip_whitespace();
    if (at_end()) {
      return json_error(ErrorCode::MalformedInput,
                        "document contains no JSON value" + byte_suffix(pos_));
    }
    Result<Value> value = parse_value(1);
    if (!value.ok()) {
      return value.error();
    }
    skip_whitespace();
    if (!at_end()) {
      return json_error(ErrorCode::TrailingBytes,
                        "trailing content after the top-level value" + byte_suffix(pos_));
    }
    return value;
  }

 private:
  [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }

  void skip_whitespace() noexcept {
    while (!at_end()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  [[nodiscard]] Error unexpected_character(std::size_t offset, std::string_view expected) const {
    std::string message = "unexpected character '";
    message += describe_byte(static_cast<unsigned char>(text_[offset]));
    message += '\'';
    if (!expected.empty()) {
      message += "; expected ";
      message += expected;
    }
    message += byte_suffix(offset);
    return json_error(ErrorCode::MalformedInput, std::move(message));
  }

  [[nodiscard]] Error unterminated(std::string_view what, std::size_t opener) const {
    return json_error(ErrorCode::MalformedInput,
                      "unterminated " + std::string(what) + " starting at byte " +
                          std::to_string(opener));
  }

  [[nodiscard]] Result<Value> parse_value(std::size_t depth) {
    if (at_end()) {
      return json_error(ErrorCode::MalformedInput,
                        "unexpected end of input" + byte_suffix(pos_));
    }
    const char c = text_[pos_];
    switch (c) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"': {
        Result<std::string> text = parse_string();
        if (!text.ok()) {
          return text.error();
        }
        return Value::make_string(text.take());
      }
      case 't':
        return parse_literal("true", Value::make_bool(true));
      case 'f':
        return parse_literal("false", Value::make_bool(false));
      case 'n':
        return parse_literal("null", Value::make_null());
      default:
        break;
    }
    if (c == '-' || is_digit(c)) {
      return parse_number();
    }
    return unexpected_character(pos_, std::string_view{});
  }

  [[nodiscard]] Result<Value> parse_literal(std::string_view literal, Value value) {
    const std::size_t start = pos_;
    if (text_.compare(pos_, literal.size(), literal) != 0) {
      return json_error(ErrorCode::MalformedInput,
                        "invalid literal; expected '" + std::string(literal) + "'" +
                            byte_suffix(start));
    }
    pos_ += literal.size();
    return value;
  }

  [[nodiscard]] Result<Value> parse_number() {
    const std::size_t start = pos_;
    if (text_[pos_] == '-') {
      ++pos_;
    }
    if (at_end()) {
      return json_error(ErrorCode::MalformedInput, "incomplete number" + byte_suffix(start));
    }
    if (text_[pos_] == '0') {
      ++pos_;
      if (!at_end() && is_digit(text_[pos_])) {
        return json_error(ErrorCode::MalformedInput,
                          "number must not carry leading zeros" + byte_suffix(start));
      }
    } else if (is_digit(text_[pos_])) {
      while (!at_end() && is_digit(text_[pos_])) {
        ++pos_;
      }
    } else {
      return unexpected_character(pos_, "a digit");
    }

    bool integral_literal = true;
    if (!at_end() && text_[pos_] == '.') {
      integral_literal = false;
      ++pos_;
      if (at_end() || !is_digit(text_[pos_])) {
        return json_error(ErrorCode::MalformedInput,
                          "expected a digit after the decimal point" + byte_suffix(pos_));
      }
      while (!at_end() && is_digit(text_[pos_])) {
        ++pos_;
      }
    }
    if (!at_end() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      integral_literal = false;
      ++pos_;
      if (!at_end() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (at_end() || !is_digit(text_[pos_])) {
        return json_error(ErrorCode::MalformedInput,
                          "expected a digit in the exponent" + byte_suffix(pos_));
      }
      while (!at_end() && is_digit(text_[pos_])) {
        ++pos_;
      }
    }
    return convert_number(text_.substr(start, pos_ - start), integral_literal, start);
  }

  [[nodiscard]] Result<Value> convert_number(std::string_view token, bool integral_literal,
                                             std::size_t offset) {
    if (integral_literal) {
      std::int64_t integer = 0;
      const std::from_chars_result result =
          std::from_chars(token.data(), token.data() + token.size(), integer);
      if (result.ec == std::errc() && result.ptr == token.data() + token.size()) {
        return Value::make_integer(integer);
      }
      if (result.ec != std::errc::result_out_of_range) {
        return json_error(ErrorCode::MalformedInput,
                          "invalid number '" + std::string(token) + "'" + byte_suffix(offset));
      }
      // An integer literal that does not fit in int64 is stored as a real.
    }
    double real = 0.0;
    const std::from_chars_result result = std::from_chars(
        token.data(), token.data() + token.size(), real, std::chars_format::general);
    if (result.ec == std::errc::result_out_of_range) {
      return limit_error("number '" + std::string(token) +
                         "' is outside the representable range" + byte_suffix(offset));
    }
    if (result.ec != std::errc() || result.ptr != token.data() + token.size()) {
      return json_error(ErrorCode::MalformedInput,
                        "invalid number '" + std::string(token) + "'" + byte_suffix(offset));
    }
    if (!std::isfinite(real)) {
      return limit_error("number '" + std::string(token) + "' is not finite" +
                         byte_suffix(offset));
    }
    return Value::make_real(real);
  }

  [[nodiscard]] Result<std::string> parse_string() {
    const std::size_t opener = pos_;
    ++pos_;  // the opening quote
    std::string out;
    for (;;) {
      if (at_end()) {
        return failure<std::string>(ErrorCode::MalformedInput, std::string(kSubject),
                                    "unterminated string starting at byte " +
                                        std::to_string(opener));
      }
      const unsigned char byte = static_cast<unsigned char>(text_[pos_]);
      if (byte == '"') {
        ++pos_;
        break;
      }
      if (byte == '\\') {
        const Status escaped = parse_escape(out);
        if (!escaped.ok()) {
          return escaped.error();
        }
      } else if (byte < 0x20u) {
        return failure<std::string>(ErrorCode::MalformedInput, std::string(kSubject),
                                    "unescaped control character" + byte_suffix(pos_));
      } else if (byte < 0x80u) {
        const std::size_t run_start = pos_;
        while (!at_end()) {
          const unsigned char current = static_cast<unsigned char>(text_[pos_]);
          if (current < 0x20u || current >= 0x80u || current == '"' || current == '\\') {
            break;
          }
          ++pos_;
        }
        out.append(text_.data() + run_start, pos_ - run_start);
      } else {
        const std::size_t length = utf8_sequence_length(text_, pos_);
        if (length == 0) {
          return failure<std::string>(ErrorCode::MalformedInput, std::string(kSubject),
                                      "invalid UTF-8 sequence" + byte_suffix(pos_));
        }
        out.append(text_.data() + pos_, length);
        pos_ += length;
      }
      if (out.size() > limits_.max_string_bytes) {
        return failure<std::string>(ErrorCode::BoundedLimitExceeded, std::string(kSubject),
                                    "string exceeds the limit of " +
                                        std::to_string(limits_.max_string_bytes) + " bytes" +
                                        byte_suffix(opener));
      }
    }
    return success(std::move(out));
  }

  [[nodiscard]] Status parse_escape(std::string& out) {
    const std::size_t escape_at = pos_;
    ++pos_;  // the backslash
    if (at_end()) {
      return failure(ErrorCode::MalformedInput, std::string(kSubject),
                     "unterminated escape sequence" + byte_suffix(escape_at));
    }
    const char c = text_[pos_];
    switch (c) {
      case '"':
        out.push_back('"');
        ++pos_;
        return success();
      case '\\':
        out.push_back('\\');
        ++pos_;
        return success();
      case '/':
        out.push_back('/');
        ++pos_;
        return success();
      case 'b':
        out.push_back('\b');
        ++pos_;
        return success();
      case 'f':
        out.push_back('\f');
        ++pos_;
        return success();
      case 'n':
        out.push_back('\n');
        ++pos_;
        return success();
      case 'r':
        out.push_back('\r');
        ++pos_;
        return success();
      case 't':
        out.push_back('\t');
        ++pos_;
        return success();
      case 'u':
        return parse_unicode_escape(out, escape_at);
      default:
        return failure(ErrorCode::MalformedInput, std::string(kSubject),
                       "invalid escape sequence '\\" +
                           describe_byte(static_cast<unsigned char>(c)) + "'" +
                           byte_suffix(escape_at));
    }
  }

  [[nodiscard]] Status parse_unicode_escape(std::string& out, std::size_t escape_at) {
    ++pos_;  // the 'u'
    std::uint32_t code_point = 0;
    if (!read_hex4(code_point)) {
      return failure(ErrorCode::MalformedInput, std::string(kSubject),
                     "invalid \\u escape" + byte_suffix(escape_at));
    }
    if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
      // A high surrogate is only legal when a matching low surrogate follows
      // immediately; together they encode one supplementary code point.
      if (pos_ + 6 <= text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
        const std::size_t low_at = pos_;
        pos_ += 2;
        std::uint32_t low = 0;
        if (read_hex4(low) && low >= 0xDC00u && low <= 0xDFFFu) {
          const std::uint32_t combined =
              0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
          append_utf8(out, combined);
          return success();
        }
        pos_ = low_at;
      }
      return failure(ErrorCode::MalformedInput, std::string(kSubject),
                     "unpaired high surrogate" + byte_suffix(escape_at));
    }
    if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
      return failure(ErrorCode::MalformedInput, std::string(kSubject),
                     "lone low surrogate" + byte_suffix(escape_at));
    }
    append_utf8(out, code_point);
    return success();
  }

  [[nodiscard]] bool read_hex4(std::uint32_t& value) noexcept {
    if (pos_ + 4 > text_.size()) {
      return false;
    }
    std::uint32_t result = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_ + i]);
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a') + 10u;
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A') + 10u;
      } else {
        return false;
      }
      result = (result << 4) | digit;
    }
    pos_ += 4;
    value = result;
    return true;
  }

  [[nodiscard]] Result<Value> parse_array(std::size_t depth) {
    const std::size_t opener = pos_;
    if (depth > limits_.max_depth) {
      return limit_error("nesting depth exceeds the limit of " +
                         std::to_string(limits_.max_depth) + byte_suffix(opener));
    }
    ++pos_;  // '['
    Value::Array elements;
    skip_whitespace();
    if (!at_end() && text_[pos_] == ']') {
      ++pos_;
      return Value::make_array(std::move(elements));
    }
    for (;;) {
      skip_whitespace();
      Result<Value> element = parse_value(depth + 1);
      if (!element.ok()) {
        return element.error();
      }
      if (entries_ >= limits_.max_entries) {
        return limit_error("entry count exceeds the limit of " +
                           std::to_string(limits_.max_entries) + byte_suffix(pos_));
      }
      ++entries_;
      elements.push_back(element.take());
      skip_whitespace();
      if (at_end()) {
        return unterminated("array", opener);
      }
      const char c = text_[pos_];
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == ']') {
        ++pos_;
        break;
      }
      return unexpected_character(pos_, "',' or ']'");
    }
    return Value::make_array(std::move(elements));
  }

  [[nodiscard]] Result<Value> parse_object(std::size_t depth) {
    const std::size_t opener = pos_;
    if (depth > limits_.max_depth) {
      return limit_error("nesting depth exceeds the limit of " +
                         std::to_string(limits_.max_depth) + byte_suffix(opener));
    }
    ++pos_;  // '{'
    Value::Object members;
    std::vector<std::size_t> key_offsets;
    skip_whitespace();
    if (!at_end() && text_[pos_] == '}') {
      ++pos_;
      return Value::make_object(std::move(members));
    }
    for (;;) {
      skip_whitespace();
      if (at_end() || text_[pos_] != '"') {
        if (at_end()) {
          return unterminated("object", opener);
        }
        return unexpected_character(pos_, "a string key");
      }
      const std::size_t key_at = pos_;
      Result<std::string> key = parse_string();
      if (!key.ok()) {
        return key.error();
      }
      skip_whitespace();
      if (at_end()) {
        return unterminated("object", opener);
      }
      if (text_[pos_] != ':') {
        return unexpected_character(pos_, "':' after an object key");
      }
      ++pos_;  // ':'
      skip_whitespace();
      Result<Value> member = parse_value(depth + 1);
      if (!member.ok()) {
        return member.error();
      }
      if (entries_ >= limits_.max_entries) {
        return limit_error("entry count exceeds the limit of " +
                           std::to_string(limits_.max_entries) + byte_suffix(pos_));
      }
      ++entries_;
      members.emplace_back(key.take(), member.take());
      key_offsets.push_back(key_at);
      skip_whitespace();
      if (at_end()) {
        return unterminated("object", opener);
      }
      const char c = text_[pos_];
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == '}') {
        ++pos_;
        break;
      }
      return unexpected_character(pos_, "',' or '}'");
    }
    const std::size_t duplicate = first_duplicate_member(members);
    if (duplicate < members.size()) {
      return failure<Value>(ErrorCode::DuplicateIdentity, std::string(kSubject),
                            "duplicate object key " + quote(members[duplicate].first) +
                                byte_suffix(key_offsets[duplicate]));
    }
    return Value::make_object(std::move(members));
  }

  std::string_view text_;
  const Limits& limits_;
  std::size_t pos_ = 0;
  std::size_t entries_ = 0;
};

void write_number(std::string& out, const Number& number) {
  if (number.integral) {
    out += std::to_string(number.integer);
    return;
  }
  if (!std::isfinite(number.real)) {
    // A non-finite real cannot be written as JSON; emit a null so that the
    // output is always a valid document.
    out += "null";
    return;
  }
  std::array<char, 64> buffer{};
  const std::to_chars_result result = std::to_chars(buffer.data(),
                                                    buffer.data() + buffer.size(),
                                                    number.real, std::chars_format::general);
  if (result.ec == std::errc()) {
    out.append(buffer.data(), result.ptr);
    return;
  }
  out += '0';  // unreachable for a finite double in 64 bytes
}

void write_indent(std::string& out, std::size_t depth) {
  out.append(depth * 2, ' ');
}

void write_value(std::string& out, const Value& value, bool pretty, std::size_t depth) {
  switch (value.kind()) {
    case Kind::Unspecified:
    case Kind::Null:
      out += "null";
      return;
    case Kind::Bool:
      out += value.as_bool(std::string_view{}).value() ? "true" : "false";
      return;
    case Kind::Number:
      write_number(out, value.number());
      return;
    case Kind::String:
      out += quote(value.as_string(std::string_view{}).value());
      return;
    case Kind::Array: {
      const Value::Array& elements = value.elements();
      if (elements.empty()) {
        out += "[]";
        return;
      }
      out += '[';
      for (std::size_t i = 0; i < elements.size(); ++i) {
        if (i != 0) {
          out += ',';
        }
        if (pretty) {
          out += '\n';
          write_indent(out, depth + 1);
        }
        write_value(out, elements[i], pretty, depth + 1);
      }
      if (pretty) {
        out += '\n';
        write_indent(out, depth);
      }
      out += ']';
      return;
    }
    case Kind::Object: {
      const Value::Object& members = value.members();
      if (members.empty()) {
        out += "{}";
        return;
      }
      out += '{';
      for (std::size_t i = 0; i < members.size(); ++i) {
        if (i != 0) {
          out += ',';
        }
        if (pretty) {
          out += '\n';
          write_indent(out, depth + 1);
        }
        out += quote(members[i].first);
        out += pretty ? ": " : ":";
        write_value(out, members[i].second, pretty, depth + 1);
      }
      if (pretty) {
        out += '\n';
        write_indent(out, depth);
      }
      out += '}';
      return;
    }
  }
  out += "null";
}

}  // namespace

Result<Value> parse(std::string_view text, const Limits& limits) {
  Parser parser(text, limits);
  return parser.document();
}

Value Value::make_null() {
  Value value;
  value.kind_ = Kind::Null;
  return value;
}

Value Value::make_bool(bool value) {
  Value result;
  result.kind_ = Kind::Bool;
  result.bool_ = value;
  return result;
}

Value Value::make_string(std::string value) {
  Value result;
  result.kind_ = Kind::String;
  result.string_ = std::move(value);
  return result;
}

Value Value::make_array(Array value) {
  Value result;
  result.kind_ = Kind::Array;
  result.array_ = std::move(value);
  return result;
}

Value Value::make_object(Object value) {
  Value result;
  result.kind_ = Kind::Object;
  result.object_ = std::move(value);
  return result;
}

Value Value::make_integer(std::int64_t value) {
  Value result;
  result.kind_ = Kind::Number;
  result.number_.integer = value;
  // The real field mirrors the integer so that callers which ignore the
  // integral flag still observe a consistent number.
  result.number_.real = static_cast<double>(value);
  result.number_.integral = true;
  return result;
}

Value Value::make_real(double value) {
  Value result;
  result.kind_ = Kind::Number;
  result.number_.integer = 0;
  result.number_.real = value;
  result.number_.integral = false;
  return result;
}

std::size_t Value::size() const noexcept {
  switch (kind_) {
    case Kind::String:
      return string_.size();
    case Kind::Array:
      return array_.size();
    case Kind::Object:
      return object_.size();
    case Kind::Unspecified:
    case Kind::Null:
    case Kind::Bool:
    case Kind::Number:
      return 0;
  }
  return 0;
}

Result<bool> Value::as_bool(std::string_view field) const {
  if (kind_ != Kind::Bool) {
    return wrong_kind(field, "a boolean", kind_);
  }
  return success(bool_);
}

Result<std::string> Value::as_string(std::string_view field) const {
  if (kind_ != Kind::String) {
    return wrong_kind(field, "a string", kind_);
  }
  return success(string_);
}

Result<std::uint64_t> Value::as_u64(std::string_view field) const {
  if (kind_ != Kind::Number) {
    return wrong_kind(field, "a number", kind_);
  }
  if (!number_.integral) {
    return failure<std::uint64_t>(ErrorCode::MalformedInput, std::string(field),
                                  "field '" + std::string(field) +
                                      "' must be an integral number but is a real");
  }
  if (number_.integer < 0) {
    return failure<std::uint64_t>(ErrorCode::MalformedInput, std::string(field),
                                  "field '" + std::string(field) +
                                      "' must not be negative");
  }
  return success(static_cast<std::uint64_t>(number_.integer));
}

Result<std::int64_t> Value::as_i64(std::string_view field) const {
  if (kind_ != Kind::Number) {
    return wrong_kind(field, "a number", kind_);
  }
  if (!number_.integral) {
    return failure<std::int64_t>(ErrorCode::MalformedInput, std::string(field),
                                 "field '" + std::string(field) +
                                     "' must be an integral number but is a real");
  }
  return success(number_.integer);
}

Result<Value::Array> Value::as_array(std::string_view field) const {
  if (kind_ != Kind::Array) {
    return wrong_kind(field, "an array", kind_);
  }
  return success(array_);
}

Result<Value::Object> Value::as_object(std::string_view field) const {
  if (kind_ != Kind::Object) {
    return wrong_kind(field, "an object", kind_);
  }
  return success(object_);
}

const Value* Value::find(std::string_view key) const noexcept {
  if (kind_ != Kind::Object) {
    return nullptr;
  }
  for (const Member& member : object_) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

Result<Value> Value::require(std::string_view key, std::string_view context) const {
  if (kind_ != Kind::Object) {
    return failure<Value>(ErrorCode::MalformedInput, std::string(key),
                          "required field '" + std::string(key) + "' cannot be read from " +
                              std::string(kind_name(kind_)) +
                              (context.empty() ? "" : " in " + std::string(context)));
  }
  const Value* member = find(key);
  if (member == nullptr) {
    return failure<Value>(ErrorCode::UnknownIdentity, std::string(key),
                          "missing required field '" + std::string(key) + "'" +
                              (context.empty() ? "" : " in " + std::string(context)));
  }
  return success(*member);
}

bool operator==(const Value& a, const Value& b) noexcept {
  if (a.kind_ != b.kind_) {
    return false;
  }
  switch (a.kind_) {
    case Kind::Unspecified:
    case Kind::Null:
      return true;
    case Kind::Bool:
      return a.bool_ == b.bool_;
    case Kind::Number:
      if (a.number_.integral != b.number_.integral) {
        return false;
      }
      return a.number_.integral ? a.number_.integer == b.number_.integer
                                : a.number_.real == b.number_.real;
    case Kind::String:
      return a.string_ == b.string_;
    case Kind::Array:
      return a.array_ == b.array_;
    case Kind::Object:
      return a.object_ == b.object_;
  }
  return false;
}

std::string serialize(const Value& value, bool pretty) {
  std::string out;
  write_value(out, value, pretty, 0);
  if (pretty) {
    out += '\n';
  }
  return out;
}

std::string quote(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const char raw : text) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte < 0x20u) {
          constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(byte >> 4) & 0x0Fu]);
          out.push_back(kHex[byte & 0x0Fu]);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned char byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80u) {
      ++index;
      continue;
    }
    const std::size_t length = utf8_sequence_length(text, index);
    if (length == 0) {
      return false;
    }
    index += length;
  }
  return true;
}

}  // namespace fco::json
