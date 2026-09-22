// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic conflict detection.
//
// A conflict carries a machine-readable identity derived from its rule and its
// participants, so two runs over identical inputs -- in any input order --
// produce identical conflict identities. Each conflict also carries a
// human-readable explanation naming the governing declarations, the selected
// outcome and the rejected alternatives.

#ifndef IFABRIC_CONFLICT_HPP
#define IFABRIC_CONFLICT_HPP

#include "ifabric/index.hpp"

namespace ifabric {

enum class Severity : std::uint8_t { Info = 0, Warning = 1, Error = 2 };
std::string_view to_string(Severity value) noexcept;

enum class ConflictRule : std::uint8_t {
  AdminStateDisagreement = 0,
  IntentValueDisagreement = 1,
  RoutingActionDisagreement = 2,
  CapacityOvercommit = 3,
  RedundancyUnachievable = 4,
  TenantIsolationOverlap = 5,
  MaintenanceVsAdminState = 6
};
std::string_view to_string(ConflictRule value) noexcept;

struct ConflictParticipant {
  AnyId id;
  std::string role;
  std::string value;

  friend bool operator==(const ConflictParticipant& a, const ConflictParticipant& b) noexcept {
    return a.id == b.id && a.role == b.role && a.value == b.value;
  }
  friend bool operator<(const ConflictParticipant& a, const ConflictParticipant& b) noexcept {
    if (a.id != b.id) {
      return a.id < b.id;
    }
    if (a.role != b.role) {
      return a.role < b.role;
    }
    return a.value < b.value;
  }
};

struct Conflict {
  ConflictId id;
  ConflictRule rule = ConflictRule::AdminStateDisagreement;
  Severity severity = Severity::Error;
  std::string path;
  std::string summary;
  std::string explanation;
  std::vector<ConflictParticipant> participants;   // canonically sorted
  std::vector<std::string> governing_policy;       // declarative basis
  std::vector<std::string> rejected_alternatives;
  std::vector<std::string> evidence;               // observed inputs

  JsonValue to_json() const;
  std::string render() const;
};

ConflictId make_conflict_id(ConflictRule rule, const std::vector<ConflictParticipant>& participants);

// Deterministic: the returned vector is sorted by (rule, id) and the identity
// of each conflict depends only on its rule and its sorted participants.
std::vector<Conflict> detect_conflicts(const IntentDocument& document, const TopologyIndex& index);

// Capacity observable for a scope, used by capacity conflicts and validation.
struct ScopeCapacity {
  std::uint64_t capacity_bps = 0;
  std::size_t contributing_links = 0;
  bool known = false;  // false when no enabled link contributes
};

ScopeCapacity scope_capacity(const TopologyIndex& index, const AnyId& scope);

// Achievable disjoint neighbour count for a device at a failure-domain level.
std::size_t achievable_disjoint_neighbours(const TopologyIndex& index, const DeviceDecl& device,
                                           FailureDomainLevel level);

}  // namespace ifabric

#endif  // IFABRIC_CONFLICT_HPP
