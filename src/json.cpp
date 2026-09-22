// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ifabric {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
JsonValue JsonValue::boolean(bool value) {
  JsonValue out;
  out.payload_ = value;
  return out;
}

JsonValue JsonValue::integer(std::int64_t value) {
  JsonValue out;
  out.payload_ = value;
  return out;
}

Result<JsonValue> JsonValue::number(double value) {
  if (!std::isfinite(value)) {
    return Error(ErrorCode::JsonNumberOutOfRange, "JSON numbers must be finite");
  }
  JsonValue out;
  out.payload_ = value;
  return out;
}

JsonValue JsonValue::string(std::string value) {
  JsonValue out;
  out.payload_ = std::move(value);
  return out;
}

JsonValue JsonValue::array(JsonArray values) {
  JsonValue out;
  out.payload_ = std::move(values);
  return out;
}

Result<JsonValue> JsonValue::object(JsonObject members) {
  std::sort(members.begin(), members.end(),
            [](const JsonMember& a, const JsonMember& b) { return a.first < b.first; });
  for (std::size_t i = 1; i < members.size(); ++i) {
    if (members[i - 1].first == members[i].first) {
      return Error(ErrorCode::JsonDuplicateKey, "duplicate object member", members[i].first);
    }
  }
  JsonValue out;
  out.payload_ = std::move(members);
  return out;
}

std::string_view JsonValue::kind_name() const noexcept {
  switch (kind()) {
    case Kind::Null: return "null";
    case Kind::Bool: return "boolean";
    case Kind::Int: return "integer";
    case Kind::Double: return "number";
    case Kind::String: return "string";
    case Kind::Array: return "array";
    case Kind::Object: return "object";
  }
  return "unknown";
}

std::optional<bool> JsonValue::try_bool() const {
  if (const auto* value = std::get_if<bool>(&payload_)) {
    return *value;
  }
  return std::nullopt;
}

std::optional<std::int64_t> JsonValue::try_int() const {
  if (const auto* value = std::get_if<std::int64_t>(&payload_)) {
    return *value;
  }
  return std::nullopt;
}

std::optional<double> JsonValue::try_double() const {
  if (const auto* value = std::get_if<double>(&payload_)) {
    return *value;
  }
  if (const auto* value = std::get_if<std::int64_t>(&payload_)) {
    return static_cast<double>(*value);
  }
  return std::nullopt;
}

const std::string* JsonValue::try_string() const { return std::get_if<std::string>(&payload_); }

const JsonArray* JsonValue::try_array() const { return std::get_if<JsonArray>(&payload_); }

const JsonObject* JsonValue::try_object() const { return std::get_if<JsonObject>(&payload_); }

const JsonValue* JsonValue::find(std::string_view key) const {
  const auto* members = try_object();
  if (members == nullptr) {
    return nullptr;
  }
  const auto it = std::lower_bound(
      members->begin(), members->end(), key,
      [](const JsonMember& member, std::string_view needle) { return member.first < needle; });
  if (it == members->end() || it->first != key) {
    return nullptr;
  }
  return &it->second;
}

JsonValue* JsonValue::find_mut(std::string_view key) {
  auto* members = std::get_if<JsonObject>(&payload_);
  if (members == nullptr) {
    return nullptr;
  }
  const auto it = std::lower_bound(
      members->begin(), members->end(), key,
      [](const JsonMember& member, std::string_view needle) { return member.first < needle; });
  if (it == members->end() || it->first != key) {
    return nullptr;
  }
  return &it->second;
}

JsonObject* JsonValue::object_mut() { return std::get_if<JsonObject>(&payload_); }

JsonArray* JsonValue::array_mut() { return std::get_if<JsonArray>(&payload_); }

Result<bool> JsonValue::require_bool(std::string_view path) const {
  const auto value = try_bool();
  if (!value.has_value()) {
    return Error(ErrorCode::JsonTypeMismatch,
                 "expected a boolean at " + std::string(path) + ", found " +
                     std::string(kind_name()));
  }
  return *value;
}

Result<std::int64_t> JsonValue::require_int(std::string_view path) const {
  const auto value = try_int();
  if (!value.has_value()) {
    return Error(ErrorCode::JsonTypeMismatch,
                 "expected an integer at " + std::string(path) + ", found " +
                     std::string(kind_name()));
  }
  return *value;
}

Result<std::string> JsonValue::require_string(std::string_view path) const {
  const auto* value = try_string();
  if (value == nullptr) {
    return Error(ErrorCode::JsonTypeMismatch,
                 "expected a string at " + std::string(path) + ", found " +
                     std::string(kind_name()));
  }
  return *value;
}

const JsonArray& JsonValue::require_array(std::string_view path) const {
  const auto* value = try_array();
  if (value == nullptr) {
    detail::require_failed(ErrorCode::JsonTypeMismatch,
                           "expected an array at " + std::string(path) + ", found " +
                               std::string(kind_name()));
  }
  return *value;
}

const JsonObject& JsonValue::require_object(std::string_view path) const {
  const auto* value = try_object();
  if (value == nullptr) {
    detail::require_failed(ErrorCode::JsonTypeMismatch,
                           "expected an object at " + std::string(path) + ", found " +
                               std::string(kind_name()));
  }
  return *value;
}

Error JsonValue::set(std::string_view key, JsonValue value) {
  auto* members = std::get_if<JsonObject>(&payload_);
  if (members == nullptr) {
    payload_ = JsonObject{};
    members = std::get_if<JsonObject>(&payload_);
  }
  const auto it = std::lower_bound(
      members->begin(), members->end(), key,
      [](const JsonMember& member, std::string_view needle) { return member.first < needle; });
  if (it != members->end() && it->first == key) {
    it->second = std::move(value);
    return Error();
  }
  members->insert(it, JsonMember(std::string(key), std::move(value)));
  return Error();
}

Error JsonValue::insert(std::string_view key, JsonValue value) {
  auto* members = std::get_if<JsonObject>(&payload_);
  if (members == nullptr) {
    payload_ = JsonObject{};
    members = std::get_if<JsonObject>(&payload_);
  }
  const auto it = std::lower_bound(
      members->begin(), members->end(), key,
      [](const JsonMember& member, std::string_view needle) { return member.first < needle; });
  if (it != members->end() && it->first == key) {
    return Error(ErrorCode::JsonDuplicateKey, "duplicate object member", std::string(key));
  }
  members->insert(it, JsonMember(std::string(key), std::move(value)));
  return Error();
}

bool JsonValue::erase(std::string_view key) {
  auto* members = std::get_if<JsonObject>(&payload_);
  if (members == nullptr) {
    return false;
  }
  const auto it = std::lower_bound(
      members->begin(), members->end(), key,
      [](const JsonMember& member, std::string_view needle) { return member.first < needle; });
  if (it == members->end() || it->first != key) {
    return false;
  }
  members->erase(it);
  return true;
}

void JsonValue::push(JsonValue value) {
  auto* items = std::get_if<JsonArray>(&payload_);
  if (items == nullptr) {
    payload_ = JsonArray{};
    items = std::get_if<JsonArray>(&payload_);
  }
  items->push_back(std::move(value));
}

std::size_t JsonValue::size() const noexcept {
  if (const auto* items = try_array()) {
    return items->size();
  }
  if (const auto* members = try_object()) {
    return members->size();
  }
  if (const auto* text = try_string()) {
    return text->size();
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Canonical serialization
// ---------------------------------------------------------------------------
namespace {

void append_escaped(std::string& out, std::string_view text) {
  out.push_back('"');
  for (char ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out.append("\\\""); continue;
      case '\\': out.append("\\\\"); continue;
      case '\b': out.append("\\b"); continue;
      case '\f': out.append("\\f"); continue;
      case '\n': out.append("\\n"); continue;
      case '\r': out.append("\\r"); continue;
      case '\t': out.append("\\t"); continue;
      default: break;
    }
    if (byte < 0x20u) {
      static constexpr char kDigits[] = "0123456789abcdef";
      out.append("\\u00");
      out.push_back(kDigits[(byte >> 4) & 0x0Fu]);
      out.push_back(kDigits[byte & 0x0Fu]);
      continue;
    }
    out.push_back(ch);
  }
  out.push_back('"');
}

void dump_into(const JsonValue& value, std::string& out, unsigned indent, unsigned depth) {
  const bool pretty = indent > 0;
  switch (value.kind()) {
    case JsonValue::Kind::Null: out.append("null"); return;
    case JsonValue::Kind::Bool:
      out.append(value.try_bool().value() ? "true" : "false");
      return;
    case JsonValue::Kind::Int:
      out.append(std::to_string(value.try_int().value()));
      return;
    case JsonValue::Kind::Double:
      out.append(format_json_double(value.try_double().value()));
      return;
    case JsonValue::Kind::String:
      append_escaped(out, *value.try_string());
      return;
    case JsonValue::Kind::Array: {
      const auto& items = *value.try_array();
      if (items.empty()) {
        out.append("[]");
        return;
      }
      out.push_back('[');
      for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        if (pretty) {
          out.push_back('\n');
          out.append(static_cast<std::size_t>(depth + 1) * indent, ' ');
        }
        dump_into(items[i], out, indent, depth + 1);
      }
      if (pretty) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(depth) * indent, ' ');
      }
      out.push_back(']');
      return;
    }
    case JsonValue::Kind::Object: {
      const auto& members = *value.try_object();
      if (members.empty()) {
        out.append("{}");
        return;
      }
      out.push_back('{');
      for (std::size_t i = 0; i < members.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        if (pretty) {
          out.push_back('\n');
          out.append(static_cast<std::size_t>(depth + 1) * indent, ' ');
        }
        append_escaped(out, members[i].first);
        out.push_back(':');
        if (pretty) {
          out.push_back(' ');
        }
        dump_into(members[i].second, out, indent, depth + 1);
      }
      if (pretty) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(depth) * indent, ' ');
      }
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

std::string JsonValue::dump() const {
  std::string out;
  dump_into(*this, out, 0, 0);
  return out;
}

std::string JsonValue::dump_pretty(unsigned indent) const {
  std::string out;
  dump_into(*this, out, indent == 0 ? 2u : indent, 0);
  return out;
}

bool operator==(const JsonValue& a, const JsonValue& b) noexcept {
  if (a.kind() != b.kind()) {
    return false;
  }
  switch (a.kind()) {
    case JsonValue::Kind::Null: return true;
    case JsonValue::Kind::Bool: return a.try_bool() == b.try_bool();
    case JsonValue::Kind::Int: return a.try_int() == b.try_int();
    case JsonValue::Kind::Double: {
      const double left = a.try_double().value();
      const double right = b.try_double().value();
      return left == right;
    }
    case JsonValue::Kind::String: return *a.try_string() == *b.try_string();
    case JsonValue::Kind::Array: return *a.try_array() == *b.try_array();
    case JsonValue::Kind::Object: return *a.try_object() == *b.try_object();
  }
  return false;
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------
std::string format_json_double(double value) {
  if (!std::isfinite(value)) {
    return "null";
  }
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                    std::chars_format::general);
  if (result.ec != std::errc()) {
    return "null";
  }
  std::string out(buffer, result.ptr);
  const bool has_marker = out.find_first_of(".eE") != std::string::npos;
  if (!has_marker) {
    out.append(".0");
  }
  return out;
}

Result<double> parse_json_double(std::string_view text) {
  if (text.empty() || text.size() >= 64) {
    return Error(ErrorCode::JsonNumberOutOfRange, "number literal is empty or too long",
                 std::string(text));
  }
  char buffer[64];
  std::memcpy(buffer, text.data(), text.size());
  buffer[text.size()] = '\0';
  char* end = nullptr;
  const double value = std::strtod(buffer, &end);
  if (end != buffer + text.size()) {
    return Error(ErrorCode::JsonNumberOutOfRange, "number literal was not fully consumed",
                 std::string(text));
  }
  if (!std::isfinite(value)) {
    return Error(ErrorCode::JsonNumberOutOfRange, "number literal is not finite",
                 std::string(text));
  }
  return value;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
namespace {

class Parser {
 public:
  Parser(std::string_view text, const JsonParseLimits& limits)
      : text_(text), limits_(limits) {}

  Result<JsonValue> run() {
    if (text_.size() > limits_.max_bytes) {
      return Error(ErrorCode::JsonSizeExceeded, "JSON document exceeds the byte limit",
                   std::to_string(text_.size()));
    }
    if (!is_valid_utf8(text_)) {
      return Error(ErrorCode::JsonEncodingError, "JSON document is not valid UTF-8");
    }
    skip_whitespace();
    auto value = parse_value(0);
    if (!value) {
      return value.error();
    }
    skip_whitespace();
    if (cursor_ != text_.size()) {
      return Error(ErrorCode::JsonTrailingContent, "unexpected content after the JSON document",
                   std::string(text_.substr(cursor_, 32)));
    }
    return value;
  }

 private:
  void skip_whitespace() {
    while (cursor_ < text_.size()) {
      const char c = text_[cursor_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++cursor_;
        continue;
      }
      break;
    }
  }

  Result<JsonValue> parse_value(std::size_t depth) {
    if (depth > limits_.max_depth) {
      return Error(ErrorCode::JsonDepthExceeded, "JSON nesting depth exceeds the limit",
                   std::to_string(depth));
    }
    if (++nodes_ > limits_.max_nodes) {
      return Error(ErrorCode::JsonSizeExceeded, "JSON node count exceeds the limit");
    }
    if (cursor_ >= text_.size()) {
      return Error(ErrorCode::JsonParseError, "unexpected end of JSON input");
    }
    switch (text_[cursor_]) {
      case '{': return parse_object(depth);
      case '[': return parse_array(depth);
      case '"': {
        auto text = parse_string();
        if (!text) {
          return text.error();
        }
        return JsonValue::string(std::move(text).release());
      }
      case 't':
        if (text_.substr(cursor_, 4) == "true") {
          cursor_ += 4;
          return JsonValue::boolean(true);
        }
        return Error(ErrorCode::JsonParseError, "invalid literal", std::string(text_.substr(cursor_, 4)));
      case 'f':
        if (text_.substr(cursor_, 5) == "false") {
          cursor_ += 5;
          return JsonValue::boolean(false);
        }
        return Error(ErrorCode::JsonParseError, "invalid literal", std::string(text_.substr(cursor_, 5)));
      case 'n':
        if (text_.substr(cursor_, 4) == "null") {
          cursor_ += 4;
          return JsonValue::null();
        }
        return Error(ErrorCode::JsonParseError, "invalid literal", std::string(text_.substr(cursor_, 4)));
      default:
        return parse_number();
    }
  }

  Result<JsonValue> parse_object(std::size_t depth) {
    ++cursor_;  // '{'
    JsonObject members;
    skip_whitespace();
    if (cursor_ < text_.size() && text_[cursor_] == '}') {
      ++cursor_;
      return JsonValue::object(std::move(members));
    }
    for (;;) {
      skip_whitespace();
      if (cursor_ >= text_.size() || text_[cursor_] != '"') {
        return Error(ErrorCode::JsonParseError, "expected an object member name");
      }
      auto key = parse_string();
      if (!key) {
        return key.error();
      }
      skip_whitespace();
      if (cursor_ >= text_.size() || text_[cursor_] != ':') {
        return Error(ErrorCode::JsonParseError, "expected ':' after an object member name");
      }
      ++cursor_;
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value) {
        return value.error();
      }
      const auto it = std::lower_bound(
          members.begin(), members.end(), key.value(),
          [](const JsonMember& member, const std::string& needle) { return member.first < needle; });
      if (it != members.end() && it->first == key.value()) {
        return Error(ErrorCode::JsonDuplicateKey, "duplicate object member", key.value());
      }
      members.insert(it, JsonMember(std::move(key).release(), std::move(value).release()));
      if (members.size() > kMaxCollectionMembers) {
        return Error(ErrorCode::LimitExceeded, "object member count exceeds the limit");
      }
      skip_whitespace();
      if (cursor_ >= text_.size()) {
        return Error(ErrorCode::JsonParseError, "unterminated object");
      }
      if (text_[cursor_] == ',') {
        ++cursor_;
        continue;
      }
      if (text_[cursor_] == '}') {
        ++cursor_;
        return JsonValue::object(std::move(members));
      }
      return Error(ErrorCode::JsonParseError, "expected ',' or '}' in an object");
    }
  }

  Result<JsonValue> parse_array(std::size_t depth) {
    ++cursor_;  // '['
    JsonArray items;
    skip_whitespace();
    if (cursor_ < text_.size() && text_[cursor_] == ']') {
      ++cursor_;
      return JsonValue::array(std::move(items));
    }
    for (;;) {
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value) {
        return value.error();
      }
      items.push_back(std::move(value).release());
      if (items.size() > kMaxCollectionMembers) {
        return Error(ErrorCode::LimitExceeded, "array element count exceeds the limit");
      }
      skip_whitespace();
      if (cursor_ >= text_.size()) {
        return Error(ErrorCode::JsonParseError, "unterminated array");
      }
      if (text_[cursor_] == ',') {
        ++cursor_;
        continue;
      }
      if (text_[cursor_] == ']') {
        ++cursor_;
        return JsonValue::array(std::move(items));
      }
      return Error(ErrorCode::JsonParseError, "expected ',' or ']' in an array");
    }
  }

  Result<std::string> parse_string() {
    ++cursor_;  // opening quote
    std::string out;
    for (;;) {
      if (cursor_ >= text_.size()) {
        return Error(ErrorCode::JsonParseError, "unterminated string");
      }
      const char c = text_[cursor_];
      if (c == '"') {
        ++cursor_;
        return out;
      }
      if (static_cast<unsigned char>(c) < 0x20u) {
        return Error(ErrorCode::JsonParseError, "unescaped control character in a string");
      }
      if (c != '\\') {
        out.push_back(c);
        ++cursor_;
        if (out.size() > limits_.max_string_length) {
          return Error(ErrorCode::LimitExceeded, "string exceeds the length limit");
        }
        continue;
      }
      ++cursor_;
      if (cursor_ >= text_.size()) {
        return Error(ErrorCode::JsonParseError, "unterminated escape sequence");
      }
      const char escape = text_[cursor_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          auto first = parse_hex4();
          if (!first) {
            return first.error();
          }
          std::uint32_t codepoint = first.value();
          if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
            if (cursor_ + 1 >= text_.size() || text_[cursor_] != '\\' || text_[cursor_ + 1] != 'u') {
              return Error(ErrorCode::JsonEncodingError,
                           "high surrogate is not followed by a low surrogate escape");
            }
            cursor_ += 2;
            auto second = parse_hex4();
            if (!second) {
              return second.error();
            }
            const std::uint32_t low = second.value();
            if (low < 0xDC00u || low > 0xDFFFu) {
              return Error(ErrorCode::JsonEncodingError, "invalid low surrogate in escape sequence");
            }
            codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
          } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
            return Error(ErrorCode::JsonEncodingError, "lone low surrogate in escape sequence");
          }
          append_utf8(out, codepoint);
          break;
        }
        default:
          return Error(ErrorCode::JsonParseError, "invalid escape sequence",
                       std::string(1, escape));
      }
      if (out.size() > limits_.max_string_length) {
        return Error(ErrorCode::LimitExceeded, "string exceeds the length limit");
      }
    }
  }

  Result<std::uint32_t> parse_hex4() {
    if (cursor_ + 4 > text_.size()) {
      return Error(ErrorCode::JsonParseError, "truncated \\u escape sequence");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[cursor_ + static_cast<std::size_t>(i)];
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return Error(ErrorCode::JsonParseError, "invalid hex digit in \\u escape sequence");
      }
      value = (value << 4) | digit;
    }
    cursor_ += 4;
    return value;
  }

  static void append_utf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint < 0x80u) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800u) {
      out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint < 0x10000u) {
      out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
  }

  Result<JsonValue> parse_number() {
    const std::size_t start = cursor_;
    if (cursor_ < text_.size() && text_[cursor_] == '-') {
      ++cursor_;
    }
    if (cursor_ >= text_.size() || text_[cursor_] < '0' || text_[cursor_] > '9') {
      return Error(ErrorCode::JsonParseError, "invalid number literal",
                   std::string(text_.substr(start, 16)));
    }
    if (text_[cursor_] == '0') {
      ++cursor_;
    } else {
      while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') {
        ++cursor_;
      }
    }
    bool is_integer = true;
    if (cursor_ < text_.size() && text_[cursor_] == '.') {
      is_integer = false;
      ++cursor_;
      if (cursor_ >= text_.size() || text_[cursor_] < '0' || text_[cursor_] > '9') {
        return Error(ErrorCode::JsonParseError, "number literal has no fraction digits");
      }
      while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') {
        ++cursor_;
      }
    }
    if (cursor_ < text_.size() && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
      is_integer = false;
      ++cursor_;
      if (cursor_ < text_.size() && (text_[cursor_] == '+' || text_[cursor_] == '-')) {
        ++cursor_;
      }
      if (cursor_ >= text_.size() || text_[cursor_] < '0' || text_[cursor_] > '9') {
        return Error(ErrorCode::JsonParseError, "number literal has no exponent digits");
      }
      while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') {
        ++cursor_;
      }
    }
    const std::string_view literal = text_.substr(start, cursor_ - start);
    if (is_integer) {
      std::int64_t out = 0;
      const auto result = std::from_chars(literal.data(), literal.data() + literal.size(), out);
      if (result.ec == std::errc::result_out_of_range) {
        return Error(ErrorCode::JsonNumberOutOfRange,
                     "integer literal does not fit in a signed 64-bit integer",
                     std::string(literal));
      }
      if (result.ec != std::errc() || result.ptr != literal.data() + literal.size()) {
        return Error(ErrorCode::JsonParseError, "invalid integer literal", std::string(literal));
      }
      return JsonValue::integer(out);
    }
    auto value = parse_json_double(literal);
    if (!value) {
      return value.error();
    }
    return JsonValue::number(value.value());
  }

  std::string_view text_;
  JsonParseLimits limits_;
  std::size_t cursor_ = 0;
  std::size_t nodes_ = 0;
};

}  // namespace

Result<JsonValue> parse_json(std::string_view text, const JsonParseLimits& limits) {
  Parser parser(text, limits);
  return parser.run();
}

}  // namespace ifabric
