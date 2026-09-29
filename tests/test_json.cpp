// Strict JSON codec tests.
//
// Every claim here is a statement about src/json.cpp and include/fco/json.hpp:
// the codec is strict by construction, so an input that is not exactly one
// canonical JSON value is rejected with a specific, documented ErrorCode, and an
// input that is accepted round-trips through serialize() byte for byte whenever
// it is already canonical.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "fco/error.hpp"
#include "fco/json.hpp"
#include "test_support.hpp"

namespace {

using fco::ErrorCode;
using fco::json::Kind;
using fco::json::Limits;
using fco::json::Value;

[[nodiscard]] fco::Result<Value> parse_of(std::string_view text) {
  return fco::json::parse(text);
}

[[nodiscard]] std::string bytes_of(std::initializer_list<unsigned int> values) {
  std::string out;
  out.reserve(values.size());
  for (unsigned int value : values) out.push_back(static_cast<char>(value));
  return out;
}

[[nodiscard]] std::string nested_arrays(std::size_t depth) {
  std::string doc;
  doc.append(depth, '[');
  doc += '1';
  doc.append(depth, ']');
  return doc;
}

[[nodiscard]] std::string nested_objects(std::size_t depth) {
  std::string doc;
  for (std::size_t index = 0; index < depth; ++index) doc += "{\"k\":";
  doc += '1';
  doc.append(depth, '}');
  return doc;
}

// Canonical compact documents that must survive serialize(parse(x)) == x.
[[nodiscard]] const std::vector<std::string>& canonical_documents() {
  const std::string non_ascii_document = std::string(R"({"u":"caf)") + bytes_of({0xC3, 0xA9}) + R"("})";
  static const std::vector<std::string> documents = {
      "null",
      "true",
      "false",
      "0",
      "-1",
      "123456789",
      "9007199254740993",
      "1.5",
      "-2.25",
      R"("")",
      R"("a")",
      R"("a\nb\tc")",
      R"("quote\" backslash\\ slash/")",
      "[]",
      "{}",
      "[1,2,3]",
      "[[],{}]",
      R"({"a":1})",
      R"({"a":{"b":[{"c":null}]}})",
      R"({"z":1,"a":2,"m":3})",
      bytes_of({'"', 0xC3, 0xA9, '"'}),
      non_ascii_document,
  };
  return documents;
}

}  // namespace

// ---------------------------------------------------------------------------
// Round-tripping and member order
// ---------------------------------------------------------------------------
FCO_TEST(json, round_trip_nested_document) {
  const std::string text =
      R"({"name":"alpha","nested":{"list":[1,2,-3,4.5,true,false,null],"deep":{"k":"v"}},)"
      R"("empty_obj":{},"empty_arr":[],"s":"a\nb\t\"c\"\\d","u":"\u00e9\u4e2d","count":3})";
  auto parsed = fco::json::parse(text);
  FCO_REQUIRE(parsed.ok());
  const Value& root = parsed.value();
  FCO_CHECK(root.is_object());
  FCO_CHECK_EQ(root.size(), std::size_t{7});

  // Member order is the document order, not a sorted order.
  const Value::Object& members = root.members();
  FCO_CHECK_EQ(members.size(), std::size_t{7});
  FCO_CHECK_EQ(members[0].first, std::string("name"));
  FCO_CHECK_EQ(members[1].first, std::string("nested"));
  FCO_CHECK_EQ(members[2].first, std::string("empty_obj"));
  FCO_CHECK_EQ(members[3].first, std::string("empty_arr"));
  FCO_CHECK_EQ(members[4].first, std::string("s"));
  FCO_CHECK_EQ(members[5].first, std::string("u"));
  FCO_CHECK_EQ(members[6].first, std::string("count"));

  const Value* name = root.find("name");
  FCO_REQUIRE(name != nullptr);
  FCO_CHECK(name->is_string());
  FCO_CHECK_EQ(name->as_string("name").value(), std::string("alpha"));

  const Value* nested = root.find("nested");
  FCO_REQUIRE(nested != nullptr);
  const Value* list = nested->find("list");
  FCO_REQUIRE(list != nullptr);
  FCO_CHECK(list->is_array());
  FCO_CHECK_EQ(list->size(), std::size_t{7});
  const Value::Array& elements = list->elements();
  FCO_CHECK(elements[0].is_number());
  FCO_CHECK_EQ(elements[0].as_i64("e").value(), std::int64_t{1});
  FCO_CHECK_EQ(elements[1].as_i64("e").value(), std::int64_t{2});
  FCO_CHECK_EQ(elements[2].as_i64("e").value(), std::int64_t{-3});
  FCO_CHECK(!elements[3].number().integral);
  FCO_CHECK(elements[4].is_bool());
  FCO_CHECK(elements[4].as_bool("e").value());
  FCO_CHECK(elements[5].is_bool());
  FCO_CHECK(!elements[5].as_bool("e").value());
  FCO_CHECK(elements[6].is_null());

  const Value* deep = nested->find("deep");
  FCO_REQUIRE(deep != nullptr);
  const Value* deep_key = deep->find("k");
  FCO_REQUIRE(deep_key != nullptr);
  FCO_CHECK_EQ(deep_key->as_string("k").value(), std::string("v"));

  const Value* empty_object = root.find("empty_obj");
  FCO_REQUIRE(empty_object != nullptr);
  FCO_CHECK(empty_object->is_object());
  FCO_CHECK_EQ(empty_object->size(), std::size_t{0});
  const Value* empty_array = root.find("empty_arr");
  FCO_REQUIRE(empty_array != nullptr);
  FCO_CHECK(empty_array->is_array());
  FCO_CHECK_EQ(empty_array->size(), std::size_t{0});

  const Value* escaped = root.find("s");
  FCO_REQUIRE(escaped != nullptr);
  FCO_CHECK_EQ(escaped->as_string("s").value(), std::string("a\nb\t\"c\"\\d"));

  const Value* unicode = root.find("u");
  FCO_REQUIRE(unicode != nullptr);
  FCO_CHECK_EQ(unicode->as_string("u").value(), bytes_of({0xC3, 0xA9, 0xE4, 0xB8, 0xAD}));

  const Value* count = root.find("count");
  FCO_REQUIRE(count != nullptr);
  FCO_CHECK_EQ(count->as_u64("count").value(), std::uint64_t{3});

  // Serialisation is canonical: the \u escapes for non-ASCII code points are
  // emitted as their raw UTF-8 bytes, and everything else is reproduced exactly.
  const std::string canonical =
      R"({"name":"alpha","nested":{"list":[1,2,-3,4.5,true,false,null],"deep":{"k":"v"}},)"
      R"("empty_obj":{},"empty_arr":[],"s":"a\nb\t\"c\"\\d","u":"\u00e9\u4e2d","count":3})";
  std::string expected = text;
  const std::string escaped_unicode = "\\u00e9\\u4e2d";
  const std::size_t position = expected.find(escaped_unicode);
  FCO_REQUIRE(position != std::string::npos);
  const std::string raw = bytes_of({0xC3, 0xA9, 0xE4, 0xB8, 0xAD});
  expected.replace(position, escaped_unicode.size(), raw);
  FCO_CHECK_EQ(fco::json::serialize(root), expected);
}

FCO_TEST(json, member_order_is_preserved_by_serialize) {
  auto parsed = fco::json::parse(R"({"z":1,"a":2,"m":3,"A":4})");
  FCO_REQUIRE(parsed.ok());
  FCO_CHECK_EQ(fco::json::serialize(parsed.value()), std::string(R"({"z":1,"a":2,"m":3,"A":4})"));

  const Value::Object& members = parsed.value().members();
  FCO_REQUIRE(members.size() == 4);
  FCO_CHECK_EQ(members[0].first, std::string("z"));
  FCO_CHECK_EQ(members[1].first, std::string("a"));
  FCO_CHECK_EQ(members[2].first, std::string("m"));
  FCO_CHECK_EQ(members[3].first, std::string("A"));
}

FCO_TEST(json, canonical_documents_are_idempotent) {
  for (const std::string& document : canonical_documents()) {
    auto parsed = fco::json::parse(document);
    if (!parsed.ok()) {
      ::fco::test::report_failure(__FILE__, __LINE__,
                                  "canonical document rejected: " + document + " -> " +
                                      fco::describe(parsed.error()));
      continue;
    }
    const std::string again = fco::json::serialize(parsed.value());
    FCO_CHECK_EQ(again, document);
    auto reparsed = fco::json::parse(again);
    FCO_REQUIRE(reparsed.ok());
    FCO_CHECK(reparsed.value() == parsed.value());
  }
}

FCO_TEST(json, pretty_serialisation_reparses_equal) {
  auto parsed = fco::json::parse(R"({"a":[1,{"b":null}],"c":{}})");
  FCO_REQUIRE(parsed.ok());
  const std::string pretty = fco::json::serialize(parsed.value(), true);
  FCO_CHECK(!pretty.empty());
  FCO_CHECK_EQ(pretty.back(), '\n');
  FCO_CHECK(pretty.find('\n') != std::string::npos);
  FCO_CHECK(pretty.find("  ") != std::string::npos);
  auto reparsed = fco::json::parse(pretty);
  FCO_REQUIRE(reparsed.ok());
  FCO_CHECK(reparsed.value() == parsed.value());
}

// ---------------------------------------------------------------------------
// Unicode escapes
// ---------------------------------------------------------------------------
FCO_TEST(json, surrogate_pair_decodes_to_utf8_and_round_trips) {
  auto parsed = fco::json::parse(R"("\uD83D\uDE00")");
  FCO_REQUIRE(parsed.ok());
  const std::string decoded = parsed.value().as_string("s").value();
  FCO_CHECK_EQ(decoded.size(), std::size_t{4});
  FCO_CHECK_EQ(decoded, bytes_of({0xF0, 0x9F, 0x98, 0x80}));
  FCO_CHECK(fco::json::is_valid_utf8(decoded));

  // Serialisation emits the raw UTF-8 bytes, and parsing them back is stable.
  const std::string serialized = fco::json::serialize(parsed.value());
  FCO_CHECK_EQ(serialized, "\"" + decoded + "\"");
  auto again = fco::json::parse(serialized);
  FCO_REQUIRE(again.ok());
  FCO_CHECK(again.value() == parsed.value());
}

FCO_TEST(json, tab_escape_decodes_to_tab_and_is_re_escaped_as_short_form) {
  auto parsed = fco::json::parse(R"("\u0009")");
  FCO_REQUIRE(parsed.ok());
  FCO_CHECK_EQ(parsed.value().as_string("s").value(), std::string("\t"));
  // The documented re-escape for TAB is the short form \t (src/json.cpp quote()).
  FCO_CHECK_EQ(fco::json::serialize(parsed.value()), std::string(R"("\t")"));
}

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------
FCO_TEST(json, rejects_duplicate_object_keys) {
  FCO_CHECK_ERROR(parse_of(R"({"a":1,"a":2})"), ErrorCode::DuplicateIdentity);
  FCO_CHECK_ERROR(parse_of(R"({"x":{"k":1,"k":2}})"), ErrorCode::DuplicateIdentity);
  FCO_CHECK_ERROR(parse_of(R"({"":1,"":2})"), ErrorCode::DuplicateIdentity);
  // The same key at different nesting levels is not a duplicate.
  auto accepted = fco::json::parse(R"({"a":{"a":1}})");
  FCO_CHECK(accepted.ok());
}

FCO_TEST(json, rejects_trailing_content) {
  FCO_CHECK_ERROR(parse_of("1 2"), ErrorCode::TrailingBytes);
  FCO_CHECK_ERROR(parse_of("{} {}"), ErrorCode::TrailingBytes);
  FCO_CHECK_ERROR(parse_of("[1]x"), ErrorCode::TrailingBytes);
  FCO_CHECK_ERROR(parse_of("nulltrue"), ErrorCode::TrailingBytes);
  FCO_CHECK_ERROR(parse_of("\"a\"\"b\""), ErrorCode::TrailingBytes);
  FCO_CHECK_ERROR(parse_of("[1] [2]"), ErrorCode::TrailingBytes);
  // "0x10" is an accepted top-level 0 followed by the trailing bytes "x10". The
  // strict reading of include/fco/json.hpp ("no trailing content after the
  // top-level value") is TrailingBytes, and that is what the implementation
  // reports; the task brief listed MalformedInput, so this is reported as a
  // documentation deviation rather than a defect.
  FCO_CHECK_ERROR(parse_of("0x10"), ErrorCode::TrailingBytes);
}

FCO_TEST(json, rejects_malformed_numbers) {
  FCO_CHECK_ERROR(parse_of("01"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("-01"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("+1"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("1."), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(".5"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("1e"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("1e+"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("1.5e-"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("-"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("NaN"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("Infinity"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("-Infinity"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("nul"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("tru"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("1 2 3"), ErrorCode::TrailingBytes);
}

FCO_TEST(json, rejects_numbers_outside_the_representable_range) {
  FCO_CHECK_ERROR(parse_of("1e999"), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(parse_of("-1e999"), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(parse_of("1e309"), ErrorCode::BoundedLimitExceeded);
  // A finite real at the edge of the double range is accepted.
  FCO_CHECK(parse_of("1e308").ok());
  FCO_CHECK(parse_of("1.7976931348623157e308").ok());
  // An integer literal that does not fit int64 becomes a finite real.
  auto huge = fco::json::parse("184467440737095516160");
  FCO_REQUIRE(huge.ok());
  FCO_CHECK(huge.value().is_number());
  FCO_CHECK(!huge.value().number().integral);
}

FCO_TEST(json, rejects_unterminated_structures) {
  FCO_CHECK_ERROR(parse_of("\"abc"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("\"abc\\"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("[1,2"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("["), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("[1,"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(R"({"a":1)"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(R"({"a")"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(R"({"a":)"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(""), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("   "), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("[,]"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("{,}"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("{1:2}"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("[1 2]"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(R"({"a":1 "b":2})"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("}"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of("]"), ErrorCode::MalformedInput);
}

FCO_TEST(json, rejects_raw_control_characters_in_strings) {
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 'a', 0x0A, 'b', '"'})), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0x00, '"'})), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0x1F, '"'})), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 'a', 0x0D, '"'})), ErrorCode::MalformedInput);
  // 0x7F is not a JSON control byte and is legal inside a string.
  FCO_CHECK(parse_of(bytes_of({'"', 0x7F, '"'})).ok());
}

FCO_TEST(json, rejects_lone_surrogates) {
  FCO_CHECK_ERROR(parse_of(R"("\uD800")"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(R"("\uDC00")"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(R"("\uDFFF")"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(R"("\uD800x")"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(R"("\uD800\u0041")"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(R"("\uD800\uD800")"), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(R"("\uD800z")"), ErrorCode::MalformedInput);
}

FCO_TEST(json, accepts_the_whole_surrogate_pair_range) {
  // U+10000 and U+10FFFF are the extreme supplementary code points.
  auto lowest = fco::json::parse(R"("\uD800\uDC00")");
  FCO_REQUIRE(lowest.ok());
  FCO_CHECK_EQ(lowest.value().as_string("s").value(), bytes_of({0xF0, 0x90, 0x80, 0x80}));
  auto highest = fco::json::parse(R"("\uDBFF\uDFFF")");
  FCO_REQUIRE(highest.ok());
  FCO_CHECK_EQ(highest.value().as_string("s").value(), bytes_of({0xF4, 0x8F, 0xBF, 0xBF}));
}

FCO_TEST(json, rejects_invalid_utf8_inside_strings) {
  // Stray continuation byte.
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0x80, '"'})), ErrorCode::MalformedInput);
  // Overlong encodings.
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0xC0, 0xAF, '"'})), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0xE0, 0x80, 0x80, '"'})), ErrorCode::MalformedInput);
  // A surrogate encoded in UTF-8 (ED A0 80 = U+D800).
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0xED, 0xA0, 0x80, '"'})), ErrorCode::MalformedInput);
  // A code point above U+10FFFF (F4 90 80 80 and the invalid lead F5).
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0xF4, 0x90, 0x80, 0x80, '"'})),
                  ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0xF5, 0x80, 0x80, 0x80, '"'})),
                  ErrorCode::MalformedInput);
  // Truncated sequences at the end of the document.
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0xC3})), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0xE4, 0xB8})), ErrorCode::MalformedInput);
  FCO_CHECK_ERROR(parse_of(bytes_of({'"', 0xF0, 0x9F, 0x98})), ErrorCode::MalformedInput);
  // Well-formed sequences are accepted.
  FCO_CHECK(parse_of(bytes_of({'"', 0xC3, 0xA9, '"'})).ok());
  FCO_CHECK(parse_of(bytes_of({'"', 0xF0, 0x9F, 0x98, 0x80, '"'})).ok());
}

FCO_TEST(json, depth_bound_is_exactly_64) {
  auto at_bound = fco::json::parse(nested_arrays(64));
  FCO_CHECK(at_bound.ok());
  FCO_CHECK_ERROR(parse_of(nested_arrays(65)), ErrorCode::BoundedLimitExceeded);

  auto objects_at_bound = fco::json::parse(nested_objects(64));
  FCO_CHECK(objects_at_bound.ok());
  FCO_CHECK_ERROR(parse_of(nested_objects(65)), ErrorCode::BoundedLimitExceeded);

  Limits shallow;
  shallow.max_depth = 3;
  FCO_CHECK(fco::json::parse(nested_arrays(3), shallow).ok());
  FCO_CHECK_ERROR(fco::json::parse(nested_arrays(4), shallow), ErrorCode::BoundedLimitExceeded);
}

FCO_TEST(json, document_byte_bound) {
  Limits limits;
  limits.max_document_bytes = 8;
  FCO_CHECK(fco::json::parse("[1,2,3]", limits).ok());
  FCO_CHECK_ERROR(fco::json::parse("[1,2,3]  ", limits), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(fco::json::parse(std::string(9, '1'), limits), ErrorCode::BoundedLimitExceeded);
}

FCO_TEST(json, entry_count_bound) {
  Limits limits;
  limits.max_entries = 3;
  FCO_CHECK(fco::json::parse("[1,2,3]", limits).ok());
  FCO_CHECK_ERROR(fco::json::parse("[1,2,3,4]", limits), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK(fco::json::parse(R"({"a":1,"b":2,"c":3})", limits).ok());
  FCO_CHECK_ERROR(fco::json::parse(R"({"a":1,"b":2,"c":3,"d":4})", limits),
                  ErrorCode::BoundedLimitExceeded);
  // Nested containers count towards the same budget.
  FCO_CHECK_ERROR(fco::json::parse("[[1],[2],[3]]", limits), ErrorCode::BoundedLimitExceeded);
}

FCO_TEST(json, string_byte_bound) {
  Limits limits;
  limits.max_string_bytes = 4;
  FCO_CHECK(fco::json::parse(R"("abcd")", limits).ok());
  FCO_CHECK_ERROR(fco::json::parse(R"("abcde")", limits), ErrorCode::BoundedLimitExceeded);
  FCO_CHECK_ERROR(fco::json::parse(R"("\u00e9\u00e9\u00e9")", limits),
                  ErrorCode::BoundedLimitExceeded);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
FCO_TEST(json, accessors_reject_the_wrong_kind_with_the_field_as_subject) {
  auto parsed = fco::json::parse(R"({"b":true,"s":"x","n":1,"r":1.5,"a":[],"o":{},"z":null})");
  FCO_REQUIRE(parsed.ok());
  const Value& root = parsed.value();
  const Value* bool_member = root.find("b");
  const Value* string_member = root.find("s");
  const Value* number_member = root.find("n");
  const Value* real_member = root.find("r");
  const Value* array_member = root.find("a");
  const Value* object_member = root.find("o");
  const Value* null_member = root.find("z");
  FCO_REQUIRE(bool_member != nullptr && string_member != nullptr && number_member != nullptr &&
              real_member != nullptr && array_member != nullptr && object_member != nullptr &&
              null_member != nullptr);

  auto as_bool_string = string_member->as_bool("field-name");
  FCO_REQUIRE(!as_bool_string.ok());
  FCO_CHECK_EQ(as_bool_string.error().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(as_bool_string.error().subject, std::string("field-name"));

  auto as_string_number = number_member->as_string("n-field");
  FCO_REQUIRE(!as_string_number.ok());
  FCO_CHECK_EQ(as_string_number.error().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(as_string_number.error().subject, std::string("n-field"));

  auto as_u64_string = string_member->as_u64("u64-field");
  FCO_REQUIRE(!as_u64_string.ok());
  FCO_CHECK_EQ(as_u64_string.error().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(as_u64_string.error().subject, std::string("u64-field"));

  auto as_i64_string = string_member->as_i64("i64-field");
  FCO_REQUIRE(!as_i64_string.ok());
  FCO_CHECK_EQ(as_i64_string.error().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(as_i64_string.error().subject, std::string("i64-field"));

  auto as_array_object = object_member->as_array("array-field");
  FCO_REQUIRE(!as_array_object.ok());
  FCO_CHECK_EQ(as_array_object.error().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(as_array_object.error().subject, std::string("array-field"));

  auto as_object_array = array_member->as_object("object-field");
  FCO_REQUIRE(!as_object_array.ok());
  FCO_CHECK_EQ(as_object_array.error().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(as_object_array.error().subject, std::string("object-field"));

  auto as_bool_null = null_member->as_bool("null-field");
  FCO_REQUIRE(!as_bool_null.ok());
  FCO_CHECK_EQ(as_bool_null.error().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(as_bool_null.error().subject, std::string("null-field"));

  // A real is not an integer, and a negative integer is not a u64.
  auto as_u64_real = real_member->as_u64("real-field");
  FCO_REQUIRE(!as_u64_real.ok());
  FCO_CHECK_EQ(as_u64_real.error().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(as_u64_real.error().subject, std::string("real-field"));

  auto negative = fco::json::parse("-5");
  FCO_REQUIRE(negative.ok());
  auto as_u64_negative = negative.value().as_u64("negative-field");
  FCO_REQUIRE(!as_u64_negative.ok());
  FCO_CHECK_EQ(as_u64_negative.error().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(as_u64_negative.error().subject, std::string("negative-field"));

  // The same values on the right kind succeed.
  FCO_CHECK(bool_member->as_bool("b").value());
  FCO_CHECK_EQ(string_member->as_string("s").value(), std::string("x"));
  FCO_CHECK_EQ(number_member->as_u64("n").value(), std::uint64_t{1});
  FCO_CHECK_EQ(number_member->as_i64("n").value(), std::int64_t{1});
  FCO_CHECK_EQ(negative.value().as_i64("n").value(), std::int64_t{-5});
  FCO_CHECK_EQ(array_member->as_array("a").value().size(), std::size_t{0});
  FCO_CHECK_EQ(object_member->as_object("o").value().size(), std::size_t{0});
}

FCO_TEST(json, find_and_require) {
  auto parsed = fco::json::parse(R"({"present":1})");
  FCO_REQUIRE(parsed.ok());
  const Value& root = parsed.value();
  FCO_CHECK(root.find("absent") == nullptr);
  FCO_CHECK(root.find("present") != nullptr);
  // find() on a non-object returns nullptr for every key.
  auto array = fco::json::parse("[1]");
  FCO_REQUIRE(array.ok());
  FCO_CHECK(array.value().find("present") == nullptr);

  auto missing = root.require("absent", "context");
  FCO_REQUIRE(!missing.ok());
  FCO_CHECK_EQ(missing.error().code, ErrorCode::UnknownIdentity);
  FCO_CHECK_EQ(missing.error().subject, std::string("absent"));

  auto present = root.require("present", "context");
  FCO_REQUIRE(present.ok());
  FCO_CHECK_EQ(present.value().as_i64("present").value(), std::int64_t{1});

  auto wrong_kind = array.value().require("present", "context");
  FCO_REQUIRE(!wrong_kind.ok());
  FCO_CHECK_EQ(wrong_kind.error().code, ErrorCode::MalformedInput);
  FCO_CHECK_EQ(wrong_kind.error().subject, std::string("present"));
}

FCO_TEST(json, value_factories_and_equality) {
  const Value null_value = Value::make_null();
  const Value true_value = Value::make_bool(true);
  const Value string_value = Value::make_string("s");
  const Value integer = Value::make_integer(-7);
  const Value real = Value::make_real(0.5);
  const Value array = Value::make_array({integer, real});
  const Value object = Value::make_object({{"k", string_value}});

  FCO_CHECK(null_value.is_null());
  FCO_CHECK_EQ(null_value.kind(), Kind::Null);
  FCO_CHECK_EQ(true_value.kind(), Kind::Bool);
  FCO_CHECK_EQ(string_value.size(), std::size_t{1});
  FCO_CHECK_EQ(integer.number().integer, std::int64_t{-7});
  FCO_CHECK(integer.number().integral);
  FCO_CHECK(!real.number().integral);
  FCO_CHECK_EQ(array.size(), std::size_t{2});
  FCO_CHECK_EQ(object.size(), std::size_t{1});
  FCO_CHECK(!Value{}.specified());

  FCO_CHECK_EQ(fco::json::serialize(null_value), std::string("null"));
  FCO_CHECK_EQ(fco::json::serialize(true_value), std::string("true"));
  FCO_CHECK_EQ(fco::json::serialize(integer), std::string("-7"));
  FCO_CHECK_EQ(fco::json::serialize(real), std::string("0.5"));
  FCO_CHECK_EQ(fco::json::serialize(array), std::string("[-7,0.5]"));
  FCO_CHECK_EQ(fco::json::serialize(object), std::string(R"({"k":"s"})"));

  auto reparsed = fco::json::parse(fco::json::serialize(object));
  FCO_REQUIRE(reparsed.ok());
  FCO_CHECK(reparsed.value() == object);
  FCO_CHECK(!(reparsed.value() == array));
}

// ---------------------------------------------------------------------------
// UTF-8 validation and escaping helpers
// ---------------------------------------------------------------------------
FCO_TEST(json, is_valid_utf8_direct_checks) {
  FCO_CHECK(fco::json::is_valid_utf8(""));
  FCO_CHECK(fco::json::is_valid_utf8("ascii text"));
  FCO_CHECK(fco::json::is_valid_utf8(bytes_of({0x7F})));
  FCO_CHECK(fco::json::is_valid_utf8(bytes_of({0xC2, 0x80})));
  FCO_CHECK(fco::json::is_valid_utf8(bytes_of({0xC3, 0xA9})));
  FCO_CHECK(fco::json::is_valid_utf8(bytes_of({0xE4, 0xB8, 0xAD})));
  FCO_CHECK(fco::json::is_valid_utf8(bytes_of({0xF0, 0x9F, 0x98, 0x80})));
  FCO_CHECK(fco::json::is_valid_utf8(bytes_of({0xF4, 0x8F, 0xBF, 0xBF})));

  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0x80})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xBF})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xC0, 0xAF})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xC1, 0xBF})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xC3})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xC3, 0x28})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xE0, 0x80, 0x80})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xE0, 0x9F, 0xBF})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xED, 0xA0, 0x80})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xED, 0xBF, 0xBF})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xE4, 0xB8})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xF0, 0x80, 0x80, 0x80})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xF4, 0x90, 0x80, 0x80})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xF5, 0x80, 0x80, 0x80})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({0xF0, 0x9F, 0x98})));
  FCO_CHECK(!fco::json::is_valid_utf8(bytes_of({'a', 0x80, 'b'})));
}

FCO_TEST(json, quote_escapes_control_bytes_and_quotes) {
  FCO_CHECK_EQ(fco::json::quote(""), std::string("\"\""));
  FCO_CHECK_EQ(fco::json::quote("plain"), std::string("\"plain\""));
  FCO_CHECK_EQ(fco::json::quote("a\"b"), std::string(R"("a\"b")"));
  FCO_CHECK_EQ(fco::json::quote("a\\b"), std::string(R"("a\\b")"));
  FCO_CHECK_EQ(fco::json::quote("\b\f\n\r\t"), std::string(R"("\b\f\n\r\t")"));
  FCO_CHECK_EQ(fco::json::quote(bytes_of({0x00})), std::string(R"("\u0000")"));
  FCO_CHECK_EQ(fco::json::quote(bytes_of({0x01})), std::string(R"("\u0001")"));
  FCO_CHECK_EQ(fco::json::quote(bytes_of({0x1F})), std::string(R"("\u001f")"));
  // 0x7F is not a JSON control byte and passes through unchanged.
  FCO_CHECK_EQ(fco::json::quote(bytes_of({0x7F})), "\"" + bytes_of({0x7F}) + "\"");
  // Non-ASCII bytes pass through unchanged; the codec never re-encodes UTF-8.
  FCO_CHECK_EQ(fco::json::quote(bytes_of({0xC3, 0xA9})), "\"" + bytes_of({0xC3, 0xA9}) + "\"");
  // Every quoted escape re-parses to the original text.
  const std::string tricky = std::string("a\"b\\c\nd\te") + bytes_of({0x01, 0xC3, 0xA9});
  auto reparsed = fco::json::parse(fco::json::quote(tricky));
  FCO_REQUIRE(reparsed.ok());
  FCO_CHECK_EQ(reparsed.value().as_string("s").value(), tricky);
}

FCO_TEST_MAIN
