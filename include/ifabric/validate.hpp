// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Layered validation. A proposal only becomes eligible for commit when every
// layer passes:
//
//   schema compatibility -> syntax -> referential integrity ->
//   capability compatibility -> topology feasibility ->
//   policy conflict -> invariant violation
//
// Unknown facts are never treated as satisfied: an unresolvable device model,
// an unobserved link capacity or an unrecognised intent property is reported as
// an error, not silently accepted.

#ifndef IFABRIC_VALIDATE_HPP
#define IFABRIC_VALIDATE_HPP

#include "ifabric/canonical.hpp"
#include "ifabric/conflict.hpp"

namespace ifabric {

enum class ValidationLayer : std::uint8_t {
  SchemaCompatibility = 0,
  Syntax = 1,
  ReferentialIntegrity = 2,
  CapabilityCompatibility = 3,
  TopologyFeasibility = 4,
  PolicyConflict = 5,
  InvariantViolation = 6
};
std::string_view to_string(ValidationLayer value) noexcept;

struct Diagnostic {
  ErrorCode code = ErrorCode::Ok;
  ValidationLayer layer = ValidationLayer::Syntax;
  Severity severity = Severity::Error;
  std::string path;
  std::string message;
  std::string detail;
  std::vector<AnyId> subjects;
  std::string conflict;  // ConflictId text when the diagnostic came from a conflict

  JsonValue to_json() const;
  std::string render() const;
};

struct ValidationReport {
  bool parsed = false;
  SchemaVersion declared_schema;
  std::vector<std::string> applied_migrations;
  ContentDigest content_digest;
  DocumentDigest document_digest;
  std::vector<Diagnostic> diagnostics;
  std::vector<Conflict> conflicts;

  bool passed() const noexcept;
  std::size_t error_count() const noexcept;
  std::size_t warning_count() const noexcept;
  std::string render() const;
  JsonValue to_json() const;
};

// Validates a document whose schema version has already been normalized.
ValidationReport validate_document(const IntentDocument& document, SchemaVersion declared);

// Full pipeline over raw intent text: parse, migrate, normalize, validate.
ValidationReport validate_text(std::string_view text, const JsonParseLimits& limits = {});

// The capability vocabulary the runtime reasons about. A service class may only
// require capabilities from this vocabulary; an unknown requirement is an
// error, never an assumption.
const std::vector<std::string_view>& known_capabilities();
bool is_known_capability(std::string_view name);

// Declarative property registry for intent objects.
enum class PropertyValueKind : std::uint8_t { Enum, String, Integer, Boolean, IdList };
struct PropertySpec {
  std::string_view name;
  std::vector<IdClass> subjects;
  PropertyValueKind kind;
  std::string_view domain;  // enum domain name for Enum kinds ("admin_state", ...)
};
const std::vector<PropertySpec>& known_properties();
const PropertySpec* find_property(std::string_view name);

}  // namespace ifabric

#endif  // IFABRIC_VALIDATE_HPP
