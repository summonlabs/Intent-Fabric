// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic JSON. Object members are stored sorted by key and are unique,
// so two documents that differ only in member order produce byte-identical
// canonical serializations. The parser is strict: duplicate keys, trailing
// content, bad escapes, lone surrogates, non-finite numbers, out-of-range
// integers, over-long strings, over-deep nesting and invalid UTF-8 are all
// rejected rather than repaired.

#ifndef IFABRIC_JSON_HPP
#define IFABRIC_JSON_HPP

#include "ifabric/core.hpp"

namespace ifabric {

class JsonValue;

using JsonArray = std::vector<JsonValue>;
using JsonMember = std::pair<std::string, JsonValue>;
using JsonObject = std::vector<JsonMember>;

class JsonValue {
 public:
  enum class Kind : std::uint8_t { Null = 0, Bool = 1, Int = 2, Double = 3, String = 4, Array = 5, Object = 6 };

  JsonValue() = default;

  static JsonValue null() { return JsonValue(); }
  static JsonValue boolean(bool value);
  static JsonValue integer(std::int64_t value);
  static Result<JsonValue> number(double value);
  static JsonValue string(std::string value);
  static JsonValue array(JsonArray values);
  static Result<JsonValue> object(JsonObject members);

  Kind kind() const noexcept { return static_cast<Kind>(payload_.index()); }

  bool is_null() const noexcept { return kind() == Kind::Null; }
  bool is_bool() const noexcept { return kind() == Kind::Bool; }
  bool is_int() const noexcept { return kind() == Kind::Int; }
  bool is_double() const noexcept { return kind() == Kind::Double; }
  bool is_number() const noexcept { return is_int() || is_double(); }
  bool is_string() const noexcept { return kind() == Kind::String; }
  bool is_array() const noexcept { return kind() == Kind::Array; }
  bool is_object() const noexcept { return kind() == Kind::Object; }

  std::string_view kind_name() const noexcept;

  std::optional<bool> try_bool() const;
  std::optional<std::int64_t> try_int() const;
  std::optional<double> try_double() const;
  const std::string* try_string() const;
  const JsonArray* try_array() const;
  const JsonValue* find(std::string_view key) const;
  JsonValue* find_mut(std::string_view key);
  const JsonObject* try_object() const;
  JsonObject* object_mut();
  JsonArray* array_mut();

  // Typed accessors that report a structured error instead of throwing.
  Result<bool> require_bool(std::string_view path) const;
  Result<std::int64_t> require_int(std::string_view path) const;
  Result<std::string> require_string(std::string_view path) const;
  const JsonArray& require_array(std::string_view path) const;
  const JsonObject& require_object(std::string_view path) const;

  // Object mutation. set() keeps the member list sorted and unique (replacing
  // an existing member in place). insert() reports a duplicate key.
  Error set(std::string_view key, JsonValue value);
  Error insert(std::string_view key, JsonValue value);
  bool erase(std::string_view key);

  void push(JsonValue value);
  std::size_t size() const noexcept;

  std::string dump() const;
  std::string dump_pretty(unsigned indent = 2) const;

  friend bool operator==(const JsonValue& a, const JsonValue& b) noexcept;
  friend bool operator!=(const JsonValue& a, const JsonValue& b) noexcept { return !(a == b); }

 private:
  using Payload = std::variant<std::monostate, bool, std::int64_t, double, std::string, JsonArray,
                               JsonObject>;
  Payload payload_;
};

struct JsonParseLimits {
  std::size_t max_depth = kMaxJsonDepth;
  std::size_t max_nodes = kMaxJsonNodes;
  std::size_t max_bytes = kMaxDocumentBytes;
  std::size_t max_string_length = kMaxStringLength;
};

Result<JsonValue> parse_json(std::string_view text, const JsonParseLimits& limits = {});

std::string format_json_double(double value);
Result<double> parse_json_double(std::string_view text);

}  // namespace ifabric

#endif  // IFABRIC_JSON_HPP
