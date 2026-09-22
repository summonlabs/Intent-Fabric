// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/conflict.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace ifabric {

std::string_view to_string(Severity value) noexcept {
  switch (value) {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
  }
  return "unknown";
}

std::string_view to_string(ConflictRule value) noexcept {
  switch (value) {
    case ConflictRule::AdminStateDisagreement: return "admin-state-disagreement";
    case ConflictRule::IntentValueDisagreement: return "intent-value-disagreement";
    case ConflictRule::RoutingActionDisagreement: return "routing-action-disagreement";
    case ConflictRule::CapacityOvercommit: return "capacity-overcommit";
    case ConflictRule::RedundancyUnachievable: return "redundancy-unachievable";
    case ConflictRule::TenantIsolationOverlap: return "tenant-isolation-overlap";
    case ConflictRule::MaintenanceVsAdminState: return "maintenance-vs-admin-state";
  }
  return "unknown";
}

namespace {

// Sorts participants into canonical order and derives the machine-readable
// identity from that canonical order, so the identity never depends on the
// order in which a rule happened to discover its participants.
void finalize(Conflict& conflict) {
  std::sort(conflict.participants.begin(), conflict.participants.end());
  conflict.participants.erase(
      std::unique(conflict.participants.begin(), conflict.participants.end()),
      conflict.participants.end());
  conflict.id = make_conflict_id(conflict.rule, conflict.participants);
}

}  // namespace

ConflictId make_conflict_id(ConflictRule rule, const std::vector<ConflictParticipant>& participants) {
  std::vector<ConflictParticipant> sorted = participants;
  std::sort(sorted.begin(), sorted.end());
  std::string seed(to_string(rule));
  seed.push_back('\n');
  for (const auto& participant : sorted) {
    seed.append(participant.role);
    seed.push_back('=');
    seed.append(participant.value);
    seed.push_back('@');
    seed.append(participant.id.str());
    seed.push_back('\n');
  }
  const Digest digest = Sha256::hash(seed);
  const auto id = ConflictId::from_local(to_hex(digest.bytes().data(), 8));
  if (!id) {
    return ConflictId();
  }
  return id.value();
}

JsonValue Conflict::to_json() const {
  JsonValue out;
  (void)out.set("id", JsonValue::string(id.str()));
  (void)out.set("rule", JsonValue::string(std::string(to_string(rule))));
  (void)out.set("severity", JsonValue::string(std::string(to_string(severity))));
  (void)out.set("path", JsonValue::string(path));
  (void)out.set("summary", JsonValue::string(summary));
  (void)out.set("explanation", JsonValue::string(explanation));
  JsonArray people;
  people.reserve(participants.size());
  for (const auto& participant : participants) {
    JsonValue entry;
    (void)entry.set("id", JsonValue::string(participant.id.str()));
    (void)entry.set("role", JsonValue::string(participant.role));
    (void)entry.set("value", JsonValue::string(participant.value));
    people.push_back(std::move(entry));
  }
  (void)out.set("participants", JsonValue::array(std::move(people)));
  JsonArray policy;
  for (const auto& item : governing_policy) {
    policy.push_back(JsonValue::string(item));
  }
  (void)out.set("governing_policy", JsonValue::array(std::move(policy)));
  JsonArray rejected;
  for (const auto& item : rejected_alternatives) {
    rejected.push_back(JsonValue::string(item));
  }
  (void)out.set("rejected_alternatives", JsonValue::array(std::move(rejected)));
  JsonArray observed;
  for (const auto& item : evidence) {
    observed.push_back(JsonValue::string(item));
  }
  (void)out.set("evidence", JsonValue::array(std::move(observed)));
  return out;
}

std::string Conflict::render() const {
  std::string out = id.str();
  out.append(" [");
  out.append(to_string(rule));
  out.append("/");
  out.append(to_string(severity));
  out.append("] ");
  out.append(summary);
  out.append("\n  explanation: ");
  out.append(explanation);
  if (!participants.empty()) {
    out.append("\n  participants:");
    for (const auto& participant : participants) {
      out.append("\n    - ");
      out.append(participant.id.str());
      out.append(" (");
      out.append(participant.role);
      out.append("): ");
      out.append(participant.value);
    }
  }
  if (!governing_policy.empty()) {
    out.append("\n  governing policy:");
    for (const auto& item : governing_policy) {
      out.append("\n    - ");
      out.append(item);
    }
  }
  if (!rejected_alternatives.empty()) {
    out.append("\n  rejected alternatives:");
    for (const auto& item : rejected_alternatives) {
      out.append("\n    - ");
      out.append(item);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Capacity observables
// ---------------------------------------------------------------------------
ScopeCapacity scope_capacity(const TopologyIndex& index, const AnyId& scope) {
  ScopeCapacity out;
  std::vector<const LinkDecl*> links;
  if (!index.links_of_scope(scope, links)) {
    return out;
  }
  for (const LinkDecl* link : links) {
    if (link->admin != AdminState::Enabled) {
      continue;
    }
    if (link->capacity_bps == 0) {
      continue;
    }
    const auto sum = checked_add(out.capacity_bps, link->capacity_bps);
    if (!sum) {
      out.capacity_bps = (std::numeric_limits<std::uint64_t>::max)();
      out.contributing_links = saturating_add(out.contributing_links, 1);
      out.known = true;
      continue;
    }
    out.capacity_bps = sum.value();
    out.contributing_links = saturating_add(out.contributing_links, 1);
  }
  out.known = out.contributing_links > 0;
  return out;
}

std::size_t achievable_disjoint_neighbours(const TopologyIndex& index, const DeviceDecl& device,
                                           FailureDomainLevel level) {
  std::set<std::string> neighbours;
  for (const LinkDecl* link : index.links_of_device(device.id)) {
    if (link->admin != AdminState::Enabled) {
      continue;
    }
    const PortDecl* a = index.port(link->endpoint_a);
    const PortDecl* b = index.port(link->endpoint_b);
    if (a == nullptr || b == nullptr) {
      continue;
    }
    const PortDecl* far = a->device == device.id ? b : a;
    const DeviceDecl* other = index.device(far->device);
    if (other == nullptr || other->id == device.id) {
      continue;
    }
    switch (level) {
      case FailureDomainLevel::Link:
      case FailureDomainLevel::Port: neighbours.insert(link->id.str()); break;
      default: neighbours.insert(index.failure_domain_key(*other, level)); break;
    }
  }
  return neighbours.size();
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------
namespace {

struct Claim {
  AnyId source;      // policy or intent that made the claim
  std::string role;  // "policy" | "intent"
  std::string value;
};

void sort_conflicts(std::vector<Conflict>& conflicts) {
  std::sort(conflicts.begin(), conflicts.end(), [](const Conflict& a, const Conflict& b) {
    if (a.rule != b.rule) {
      return static_cast<std::uint8_t>(a.rule) < static_cast<std::uint8_t>(b.rule);
    }
    return a.id.local() < b.id.local();
  });
}

std::string json_compact(const JsonValue& value) { return value.dump(); }

std::string describe_admin_state(AdminState state) { return std::string(to_string(state)); }

AdminState admin_state_from_json(const JsonValue& value, bool& ok) {
  ok = false;
  const auto* text = value.try_string();
  if (text == nullptr) {
    return AdminState::Enabled;
  }
  const auto parsed = parse_admin_state(*text);
  if (!parsed) {
    return AdminState::Enabled;
  }
  ok = true;
  return parsed.value();
}

}  // namespace

std::vector<Conflict> detect_conflicts(const IntentDocument& document, const TopologyIndex& index) {
  std::vector<Conflict> conflicts;

  // -------------------------------------------------------------------------
  // Rule 1 - administrative-state disagreement, and
  // Rule 7 - maintenance eligibility versus a pinned administrative state.
  // -------------------------------------------------------------------------
  {
    std::map<std::string, std::vector<Claim>> by_subject;
    std::map<std::string, AnyId> subject_ids;

    for (const auto& policy : document.policies) {
      const auto* admin = std::get_if<AdminStatePolicyDecl>(&policy.body);
      if (admin == nullptr) {
        continue;
      }
      const auto scope = AnyId::parse(admin->scope);
      if (!scope) {
        continue;
      }
      std::vector<const DeviceDecl*> devices;
      std::vector<const LinkDecl*> links;
      index.devices_of_scope(scope.value(), devices);
      index.links_of_scope(scope.value(), links);
      const std::string value = describe_admin_state(admin->state);
      for (const DeviceDecl* device : devices) {
        const std::string key = device->id.str();
        subject_ids.emplace(key, AnyId(device->id));
        by_subject[key].push_back(Claim{AnyId(policy.id), "policy", value});
      }
      for (const LinkDecl* link : links) {
        const std::string key = link->id.str();
        subject_ids.emplace(key, AnyId(link->id));
        by_subject[key].push_back(Claim{AnyId(policy.id), "policy", value});
      }
    }

    for (const auto& intent : document.intents) {
      if (intent.property != "admin_state" || intent.mode != IntentMode::Enforce) {
        continue;
      }
      bool ok = false;
      const AdminState state = admin_state_from_json(intent.desired, ok);
      if (!ok) {
        continue;
      }
      const std::string key = intent.subject.str();
      subject_ids.emplace(key, intent.subject);
      by_subject[key].push_back(Claim{AnyId(intent.id), "intent", describe_admin_state(state)});
    }

    for (auto& entry : by_subject) {
      std::vector<Claim>& claims = entry.second;
      std::sort(claims.begin(), claims.end(), [](const Claim& a, const Claim& b) {
        if (a.value != b.value) {
          return a.value < b.value;
        }
        if (a.source != b.source) {
          return a.source < b.source;
        }
        return a.role < b.role;
      });
      claims.erase(std::unique(claims.begin(), claims.end(),
                               [](const Claim& a, const Claim& b) {
                                 return a.value == b.value && a.source == b.source;
                               }),
                   claims.end());
      const std::string first_value = claims.front().value;
      const Claim& winner = claims.front();
      for (std::size_t i = 1; i < claims.size(); ++i) {
        if (claims[i].value == first_value) {
          continue;
        }
        Conflict conflict;
        conflict.rule = ConflictRule::AdminStateDisagreement;
        conflict.severity = Severity::Error;
        conflict.path = entry.first;
        conflict.summary = "conflicting administrative state for " + entry.first;
        conflict.explanation =
            "Two declarations that both resolve to " + entry.first +
            " require different administrative states. Intent Fabric refuses to choose: the "
            "authoritative desired state for a subject is ambiguous, so the commit is rejected.";
        conflict.participants = {
            ConflictParticipant{winner.source, winner.role, winner.value},
            ConflictParticipant{claims[i].source, claims[i].role, claims[i].value},
            ConflictParticipant{subject_ids[entry.first], "subject", entry.first}};
        conflict.governing_policy = {"deterministic-policy:admin-state-consistency"};
        conflict.rejected_alternatives = {"resolve to '" + winner.value + "'",
                                          "resolve to '" + claims[i].value + "'"};
        conflict.evidence = {"subject=" + entry.first, "claims=" + std::to_string(claims.size())};
        finalize(conflict);
        conflicts.push_back(std::move(conflict));
      }
    }
  }

  // -------------------------------------------------------------------------
  // Rule 2 - two enforce intents disagree about the same (subject, property).
  // -------------------------------------------------------------------------
  {
    std::map<std::string, std::vector<const IntentObjectDecl*>> groups;
    for (const auto& intent : document.intents) {
      if (intent.mode != IntentMode::Enforce) {
        continue;
      }
      groups[intent.subject.str() + "|" + intent.property].push_back(&intent);
    }
    for (auto& entry : groups) {
      std::vector<const IntentObjectDecl*>& intents = entry.second;
      std::sort(intents.begin(), intents.end(),
                [](const IntentObjectDecl* a, const IntentObjectDecl* b) {
                  const std::string da = json_compact(a->desired);
                  const std::string db = json_compact(b->desired);
                  if (da != db) {
                    return da < db;
                  }
                  return a->id < b->id;
                });
      if (intents.size() < 2 || json_compact(intents.front()->desired) ==
                                   json_compact(intents.back()->desired)) {
        continue;
      }
      const IntentObjectDecl* left = intents.front();
      const IntentObjectDecl* right = intents.back();
      Conflict conflict;
      conflict.rule = ConflictRule::IntentValueDisagreement;
      conflict.severity = Severity::Error;
      conflict.path = "intents";
      conflict.summary = "conflicting intent values for " + left->subject.str() + "." +
                         left->property;
      conflict.explanation =
          "Two enforced intent objects declare different desired values for the same subject and "
          "property. The runtime cannot satisfy both, so the proposal is rejected.";
      conflict.participants = {
          ConflictParticipant{AnyId(left->id), "intent", json_compact(left->desired)},
          ConflictParticipant{AnyId(right->id), "intent", json_compact(right->desired)},
          ConflictParticipant{left->subject, "subject", left->property}};
      conflict.governing_policy = {"deterministic-policy:single-desired-value"};
      conflict.rejected_alternatives = {"prefer " + left->id.str(),
                                        "prefer " + right->id.str()};
      conflict.evidence = {"property=" + left->property};
      finalize(conflict);
      conflicts.push_back(std::move(conflict));
    }
  }

  // -------------------------------------------------------------------------
  // Rule 3 - routing policies with the same precedence and overlapping match
  // sets that select different actions.
  // -------------------------------------------------------------------------
  {
    struct RoutingEntry {
      const PolicyDecl* policy;
      const RoutingPolicyDecl* body;
    };
    std::vector<RoutingEntry> entries;
    for (const auto& policy : document.policies) {
      if (const auto* body = std::get_if<RoutingPolicyDecl>(&policy.body)) {
        entries.push_back(RoutingEntry{&policy, body});
      }
    }
    std::sort(entries.begin(), entries.end(), [](const RoutingEntry& a, const RoutingEntry& b) {
      return a.policy->id < b.policy->id;
    });
    for (std::size_t i = 0; i < entries.size(); ++i) {
      for (std::size_t j = i + 1; j < entries.size(); ++j) {
        const RoutingPolicyDecl& a = *entries[i].body;
        const RoutingPolicyDecl& b = *entries[j].body;
        if (a.priority != b.priority) {
          continue;
        }
        if (a.action == b.action) {
          continue;
        }
        const bool class_overlap = a.match_service_classes.empty() || b.match_service_classes.empty()
                                       ? true
                                       : std::any_of(a.match_service_classes.begin(),
                                                     a.match_service_classes.end(),
                                                     [&](const ServiceClassId& id) {
                                                       return std::find(
                                                                  b.match_service_classes.begin(),
                                                                  b.match_service_classes.end(),
                                                                  id) !=
                                                              b.match_service_classes.end();
                                                     });
        const bool tenant_overlap = a.match_tenants.empty() || b.match_tenants.empty()
                                        ? true
                                        : std::any_of(a.match_tenants.begin(), a.match_tenants.end(),
                                                      [&](const TenantId& id) {
                                                        return std::find(b.match_tenants.begin(),
                                                                         b.match_tenants.end(),
                                                                         id) !=
                                                               b.match_tenants.end();
                                                      });
        const bool source_overlap = a.match_source.empty() || b.match_source.empty() ||
                                    a.match_source == b.match_source;
        const bool destination_overlap = a.match_destination.empty() || b.match_destination.empty() ||
                                         a.match_destination == b.match_destination;
        if (!class_overlap || !tenant_overlap || !source_overlap || !destination_overlap) {
          continue;
        }
        Conflict conflict;
        conflict.rule = ConflictRule::RoutingActionDisagreement;
        conflict.severity = Severity::Error;
        conflict.path = "policies";
        conflict.summary = "conflicting routing actions at precedence " + std::to_string(a.priority);
        conflict.explanation =
            "Two routing policies share the same precedence and their match sets overlap, but "
            "they select different actions (" + std::string(to_string(a.action)) + " versus " +
            std::string(to_string(b.action)) +
            "). Precedence does not disambiguate them, so the proposal is rejected.";
        conflict.participants = {
            ConflictParticipant{AnyId(entries[i].policy->id), "policy",
                                std::string(to_string(a.action))},
            ConflictParticipant{AnyId(entries[j].policy->id), "policy",
                                std::string(to_string(b.action))}};
        conflict.governing_policy = {"deterministic-policy:routing-precedence-total-order"};
        conflict.rejected_alternatives = {"apply " + entries[i].policy->id.str(),
                                          "apply " + entries[j].policy->id.str()};
        conflict.evidence = {"priority=" + std::to_string(a.priority)};
        finalize(conflict);
        conflicts.push_back(std::move(conflict));
      }
    }
  }

  // -------------------------------------------------------------------------
  // Rule 4 - reservations that exceed the observable capacity of a scope.
  // -------------------------------------------------------------------------
  {
    struct Reservation {
      const PolicyDecl* policy;
      const CapacityPolicyDecl* body;
      AnyId scope;
    };
    std::map<std::string, std::vector<Reservation>> by_scope;
    for (const auto& policy : document.policies) {
      const auto* body = std::get_if<CapacityPolicyDecl>(&policy.body);
      if (body == nullptr || body->reserved_bps == 0) {
        continue;
      }
      const auto scope = AnyId::parse(body->scope);
      if (!scope) {
        continue;
      }
      by_scope[scope->str()].push_back(Reservation{&policy, body, scope.value()});
    }
    for (auto& entry : by_scope) {
      std::vector<Reservation>& reservations = entry.second;
      std::sort(reservations.begin(), reservations.end(),
                [](const Reservation& a, const Reservation& b) {
                  return a.policy->id < b.policy->id;
                });
      const ScopeCapacity capacity = scope_capacity(index, reservations.front().scope);
      if (!capacity.known) {
        continue;  // no observable capacity: reported by the capability layer instead
      }
      std::uint64_t total = 0;
      bool overflow = false;
      for (const Reservation& reservation : reservations) {
        const auto sum = checked_add(total, reservation.body->reserved_bps);
        if (!sum) {
          overflow = true;
          break;
        }
        total = sum.value();
      }
      if (!overflow && total <= capacity.capacity_bps) {
        continue;
      }
      Conflict conflict;
      conflict.rule = ConflictRule::CapacityOvercommit;
      conflict.severity = Severity::Error;
      conflict.path = "policies";
      conflict.summary = "capacity reservations overcommit " + entry.first;
      conflict.explanation =
          "The sum of declared reservations on " + entry.first + " exceeds the capacity observable "
          "from the declared topology (" + std::to_string(capacity.capacity_bps) + " bps from " +
          std::to_string(capacity.contributing_links) +
          " enabled link(s)). Intent Fabric does not assume unobserved headroom.";
      conflict.participants.reserve(reservations.size() + 1);
      for (const Reservation& reservation : reservations) {
        conflict.participants.push_back(
            ConflictParticipant{AnyId(reservation.policy->id), "policy",
                                "reserved_bps=" + std::to_string(reservation.body->reserved_bps)});
      }
      conflict.participants.push_back(ConflictParticipant{
          reservations.front().scope, "scope", "capacity_bps=" + std::to_string(capacity.capacity_bps)});
      conflict.governing_policy = {"deterministic-policy:reservation-within-observed-capacity"};
      conflict.rejected_alternatives = {"admit the reservation set as declared",
                                        "silently clamp reservations to capacity"};
      conflict.evidence = {"observed_capacity_bps=" + std::to_string(capacity.capacity_bps),
                           "declared_reserved_bps=" +
                               (overflow ? std::string("overflow") : std::to_string(total))};
      finalize(conflict);
      conflicts.push_back(std::move(conflict));
    }
  }

  // -------------------------------------------------------------------------
  // Rule 5 - a redundancy requirement the declared topology cannot deliver.
  // -------------------------------------------------------------------------
  {
    for (const auto& policy : document.policies) {
      const auto* body = std::get_if<RedundancyPolicyDecl>(&policy.body);
      if (body == nullptr) {
        continue;
      }
      const auto scope = AnyId::parse(body->scope);
      if (!scope) {
        continue;
      }
      std::vector<const DeviceDecl*> devices;
      if (!index.devices_of_scope(scope.value(), devices) || devices.empty()) {
        continue;
      }
      std::size_t worst = (std::numeric_limits<std::size_t>::max)();
      const DeviceDecl* worst_device = nullptr;
      for (const DeviceDecl* device : devices) {
        const std::size_t count = achievable_disjoint_neighbours(index, *device, body->level);
        if (count < worst) {
          worst = count;
          worst_device = device;
        }
      }
      if (worst_device == nullptr || worst >= body->min_disjoint_paths) {
        continue;
      }
      Conflict conflict;
      conflict.rule = ConflictRule::RedundancyUnachievable;
      conflict.severity = Severity::Error;
      conflict.path = "policies";
      conflict.summary = "redundancy requirement is unachievable for " + scope->str();
      conflict.explanation =
          "The redundancy policy requires " + std::to_string(body->min_disjoint_paths) +
          " disjoint path(s) at failure-domain level '" + std::string(to_string(body->level)) +
          "', but the declared topology provides at most " + std::to_string(worst) +
          " for " + worst_device->id.str() + ". No policy may assume redundancy the declared "
          "topology does not provide.";
      conflict.participants = {
          ConflictParticipant{AnyId(policy.id), "policy",
                              "min_disjoint_paths=" + std::to_string(body->min_disjoint_paths)},
          ConflictParticipant{scope.value(), "scope",
                              "achievable=" + std::to_string(worst)},
          ConflictParticipant{AnyId(worst_device->id), "worst-device",
                              "level=" + std::string(to_string(body->level))}};
      conflict.governing_policy = {"deterministic-policy:no-unproven-redundancy"};
      conflict.rejected_alternatives = {"assume the requirement is met",
                                        "ignore the unreachable failure domain"};
      conflict.evidence = {"required=" + std::to_string(body->min_disjoint_paths),
                           "achievable=" + std::to_string(worst)};
      finalize(conflict);
      conflicts.push_back(std::move(conflict));
    }
  }

  // -------------------------------------------------------------------------
  // Rule 6 - two tenants claiming the same exclusive scope.
  // -------------------------------------------------------------------------
  {
    std::map<std::string, std::vector<const PolicyDecl*>> by_scope;
    for (const auto& policy : document.policies) {
      const auto* body = std::get_if<TenantIsolationPolicyDecl>(&policy.body);
      if (body == nullptr) {
        continue;
      }
      for (const auto& scope : body->exclusive_scopes) {
        by_scope[scope].push_back(&policy);
      }
    }
    for (auto& entry : by_scope) {
      std::vector<const PolicyDecl*>& policies = entry.second;
      std::sort(policies.begin(), policies.end(),
                [](const PolicyDecl* a, const PolicyDecl* b) { return a->id < b->id; });
      if (policies.size() < 2) {
        continue;
      }
      std::set<std::string> tenants;
      for (const PolicyDecl* policy : policies) {
        const auto* body = std::get_if<TenantIsolationPolicyDecl>(&policy->body);
        tenants.insert(body->tenant.str());
      }
      if (tenants.size() < 2) {
        continue;
      }
      Conflict conflict;
      conflict.rule = ConflictRule::TenantIsolationOverlap;
      conflict.severity = Severity::Error;
      conflict.path = "policies";
      conflict.summary = "tenants claim the same exclusive scope " + entry.first;
      conflict.explanation =
          "Two tenant-isolation policies grant exclusivity over the same scope to different "
          "tenants. Exclusive ownership cannot be held by more than one tenant.";
      conflict.participants.reserve(policies.size());
      for (const PolicyDecl* policy : policies) {
        const auto* body = std::get_if<TenantIsolationPolicyDecl>(&policy->body);
        conflict.participants.push_back(
            ConflictParticipant{AnyId(policy->id), "policy", "tenant=" + body->tenant.str()});
      }
      const auto scope_id = AnyId::parse(entry.first);
      if (scope_id) {
        conflict.participants.push_back(
            ConflictParticipant{scope_id.value(), "scope", "exclusive=true"});
      }
      conflict.governing_policy = {"deterministic-policy:single-exclusive-owner"};
      conflict.rejected_alternatives = {"share the scope", "grant exclusivity to the first claim"};
      conflict.evidence = {"claimants=" + std::to_string(policies.size())};
      finalize(conflict);
      conflicts.push_back(std::move(conflict));
    }
  }

  // -------------------------------------------------------------------------
  // Rule 7 - maintenance eligibility that requires a drain while an enforced
  // administrative state pins the same object enabled without draining.
  // -------------------------------------------------------------------------
  {
    struct Maintenance {
      const PolicyDecl* policy;
      const MaintenanceEligibilityPolicyDecl* body;
      AnyId scope;
    };
    std::vector<Maintenance> maintenance;
    for (const auto& policy : document.policies) {
      const auto* body = std::get_if<MaintenanceEligibilityPolicyDecl>(&policy.body);
      if (body == nullptr || !body->drain_required) {
        continue;
      }
      const auto scope = AnyId::parse(body->scope);
      if (!scope) {
        continue;
      }
      maintenance.push_back(Maintenance{&policy, body, scope.value()});
    }
    if (!maintenance.empty()) {
      std::map<std::string, const PolicyDecl*> pinned_enabled;
      for (const auto& policy : document.policies) {
        const auto* body = std::get_if<AdminStatePolicyDecl>(&policy.body);
        if (body == nullptr || body->state != AdminState::Enabled || body->drain_first) {
          continue;
        }
        const auto scope = AnyId::parse(body->scope);
        if (!scope) {
          continue;
        }
        std::vector<const DeviceDecl*> devices;
        index.devices_of_scope(scope.value(), devices);
        for (const DeviceDecl* device : devices) {
          pinned_enabled.emplace(device->id.str(), &policy);
        }
      }
      for (const Maintenance& entry : maintenance) {
        std::vector<const DeviceDecl*> devices;
        index.devices_of_scope(entry.scope, devices);
        for (const DeviceDecl* device : devices) {
          const auto it = pinned_enabled.find(device->id.str());
          if (it == pinned_enabled.end()) {
            continue;
          }
          Conflict conflict;
          conflict.rule = ConflictRule::MaintenanceVsAdminState;
          conflict.severity = Severity::Error;
          conflict.path = "policies";
          conflict.summary = "maintenance drain conflicts with a pinned enabled state on " +
                             device->id.str();
          conflict.explanation =
              "The maintenance-eligibility policy for " + entry.scope.str() +
              " requires a drain before maintenance, but an administrative-state policy pins " +
              device->id.str() +
              " to enabled without draining. A device cannot be both drainable and pinned up.";
          conflict.participants = {
              ConflictParticipant{AnyId(entry.policy->id), "policy", "drain_required=true"},
              ConflictParticipant{AnyId(it->second->id), "policy",
                                  "admin_state=enabled,drain_first=false"},
              ConflictParticipant{AnyId(device->id), "subject", "admin_state=enabled"}};
          conflict.governing_policy = {"deterministic-policy:maintenance-drain-precondition"};
          conflict.rejected_alternatives = {"grant maintenance eligibility",
                                            "silently ignore the drain requirement"};
          conflict.evidence = {"scope=" + entry.scope.str()};
          finalize(conflict);
          conflicts.push_back(std::move(conflict));
        }
      }
    }
  }

  sort_conflicts(conflicts);
  return conflicts;
}

}  // namespace ifabric
