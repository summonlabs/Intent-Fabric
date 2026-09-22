// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic semantic diff between intent generations. Entries are
// categorized by semantic meaning (administrative state, capacity, routing,
// redundancy, maintenance, tenancy, schema, topology, ...) and classified by
// impact, not by raw text.

#ifndef IFABRIC_DIFF_HPP
#define IFABRIC_DIFF_HPP

#include "ifabric/model.hpp"

namespace ifabric {

enum class DiffCategory : std::uint8_t {
  Schema = 0,
  Provenance = 1,
  Topology = 2,
  CapabilityCatalog = 3,
  ServiceClass = 4,
  Policy = 5,
  Tenant = 6,
  Workload = 7,
  IntentObject = 8,
  AdministrativeState = 9,
  Capacity = 10,
  Routing = 11,
  Redundancy = 12,
  Maintenance = 13,
  Tenancy = 14
};
std::string_view to_string(DiffCategory value) noexcept;

enum class Impact : std::uint8_t {
  None = 0,
  Additive = 1,
  Subtractive = 2,
  ServiceAffecting = 3,
  Disruptive = 4,
  Unknown = 5
};
std::string_view to_string(Impact value) noexcept;
Impact max_impact(Impact a, Impact b) noexcept;

struct DiffEntry {
  std::string path;
  std::string change;  // "added" | "removed" | "modified"
  DiffCategory category = DiffCategory::Topology;
  Impact impact = Impact::None;
  bool has_before = false;
  bool has_after = false;
  JsonValue before;
  JsonValue after;
  std::string summary;

  JsonValue to_json() const;
};

struct SemanticDiff {
  DocumentDigest from_document;
  DocumentDigest to_document;
  ContentDigest from_content;
  ContentDigest to_content;
  std::vector<DiffEntry> entries;
  Impact worst = Impact::None;

  bool empty() const noexcept { return entries.empty(); }
  std::size_t count(Impact impact) const;
  JsonValue to_json() const;
  std::string render() const;
};

// Entries are ordered by path, then by change, so the result is stable for
// identical inputs regardless of declaration order.
SemanticDiff diff_documents(const IntentDocument& before, const IntentDocument& after);

}  // namespace ifabric

#endif  // IFABRIC_DIFF_HPP
