// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/canonical.hpp"
#include "ifabric/conflict.hpp"
#include "ifabric/synth.hpp"
#include "ifabric_test.hpp"
#include "permute.hpp"

#include <iostream>

using namespace ifabric;

namespace {

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

IntentDocument synth_document(const SynthOptions& options) {
  return normalize(synthesize(core_domain(), options));
}

std::vector<Conflict> conflicts_of(const IntentDocument& document) {
  const TopologyIndex index(document);
  return detect_conflicts(document, index);
}

std::size_t count_rule(const std::vector<Conflict>& conflicts, ConflictRule rule) {
  std::size_t total = 0;
  for (const auto& conflict : conflicts) {
    if (conflict.rule == rule) {
      ++total;
    }
  }
  return total;
}

}  // namespace

IFABRIC_TEST(conflict, reference_document_has_no_conflicts) {
  const std::vector<Conflict> conflicts = conflicts_of(synth_document(SynthOptions{}));
  if (!conflicts.empty()) {
    for (const auto& conflict : conflicts) {
      std::cout << conflict.render() << "\n";
    }
  }
  CHECK(conflicts.empty());
}

IFABRIC_TEST(conflict, administrative_state_disagreement) {
  SynthOptions options;
  options.defect = SynthDefect::AdminStateDisagreement;
  const std::vector<Conflict> conflicts = conflicts_of(synth_document(options));
  CHECK_EQ(count_rule(conflicts, ConflictRule::AdminStateDisagreement) > 0, true);
  for (const auto& conflict : conflicts) {
    if (conflict.rule != ConflictRule::AdminStateDisagreement) {
      continue;
    }
    CHECK(!conflict.id.empty());
    CHECK_EQ(conflict.explanation.empty(), false);
    CHECK(!conflict.governing_policy.empty());
    CHECK(!conflict.rejected_alternatives.empty());
    CHECK(conflict.participants.size() >= 2);
  }
}

IFABRIC_TEST(conflict, intent_value_disagreement) {
  IntentDocument document = synth_document(SynthOptions{});
  REQUIRE(!document.intents.empty());
  IntentObjectDecl conflicting = document.intents.front();
  conflicting.id = IntentObjectId::from_local("conflicting-value").value();
  conflicting.desired = JsonValue::string("disabled");
  document.intents.push_back(conflicting);
  const std::vector<Conflict> conflicts = conflicts_of(normalize(document));
  CHECK_EQ(count_rule(conflicts, ConflictRule::IntentValueDisagreement), std::size_t{1});
}

IFABRIC_TEST(conflict, routing_action_disagreement) {
  SynthOptions options;
  options.defect = SynthDefect::RoutingDisagreement;
  const std::vector<Conflict> conflicts = conflicts_of(synth_document(options));
  CHECK_EQ(count_rule(conflicts, ConflictRule::RoutingActionDisagreement), std::size_t{1});
}

IFABRIC_TEST(conflict, capacity_overcommit) {
  SynthOptions options;
  options.defect = SynthDefect::CapacityOvercommit;
  const std::vector<Conflict> conflicts = conflicts_of(synth_document(options));
  CHECK_EQ(count_rule(conflicts, ConflictRule::CapacityOvercommit), std::size_t{1});
}

IFABRIC_TEST(conflict, redundancy_unachievable) {
  SynthOptions options;
  options.defect = SynthDefect::ImpossibleRedundancy;
  const std::vector<Conflict> conflicts = conflicts_of(synth_document(options));
  CHECK_EQ(count_rule(conflicts, ConflictRule::RedundancyUnachievable), std::size_t{1});
}

IFABRIC_TEST(conflict, tenant_isolation_overlap) {
  IntentDocument document = synth_document(SynthOptions{});
  const auto* isolation = [&]() -> const TenantIsolationPolicyDecl* {
    for (const auto& policy : document.policies) {
      if (const auto* body = std::get_if<TenantIsolationPolicyDecl>(&policy.body)) {
        return body;
      }
    }
    return nullptr;
  }();
  REQUIRE(isolation != nullptr);
  const std::string scope = isolation->exclusive_scopes.front();
  PolicyDecl extra;
  extra.id = PolicyId::from_local("isolation-clash").value();
  TenantIsolationPolicyDecl body;
  body.tenant = TenantId::from_local("app-b").value();
  body.exclusive_scopes = {scope};
  extra.body = body;
  document.policies.push_back(extra);
  const std::vector<Conflict> conflicts = conflicts_of(normalize(document));
  CHECK_EQ(count_rule(conflicts, ConflictRule::TenantIsolationOverlap), std::size_t{1});
}

IFABRIC_TEST(conflict, maintenance_drain_versus_pinned_state) {
  IntentDocument document = synth_document(SynthOptions{});
  PolicyDecl pinned;
  pinned.id = PolicyId::from_local("pin-enabled").value();
  AdminStatePolicyDecl pinned_body;
  pinned_body.scope = FabricId::from_local("core").value().str();
  pinned_body.state = AdminState::Enabled;
  pinned_body.drain_first = false;
  pinned.body = pinned_body;
  document.policies.push_back(pinned);
  for (auto& policy : document.policies) {
    if (auto* body = std::get_if<MaintenanceEligibilityPolicyDecl>(&policy.body)) {
      body->drain_required = true;
    }
  }
  const std::vector<Conflict> conflicts = conflicts_of(normalize(document));
  CHECK(count_rule(conflicts, ConflictRule::MaintenanceVsAdminState) > 0);
}

IFABRIC_TEST(conflict, identities_depend_only_on_rule_and_participants) {
  const std::vector<ConflictParticipant> left = {
      ConflictParticipant{AnyId(DeviceId::from_local("d1").value()), "left", "enabled"},
      ConflictParticipant{AnyId(PolicyId::from_local("p1").value()), "right", "disabled"}};
  const std::vector<ConflictParticipant> right = {left[1], left[0]};
  const ConflictId first = make_conflict_id(ConflictRule::AdminStateDisagreement, left);
  const ConflictId second = make_conflict_id(ConflictRule::AdminStateDisagreement, right);
  CHECK(!first.empty());
  CHECK_EQ(first.str(), second.str());
  const ConflictId other = make_conflict_id(ConflictRule::CapacityOvercommit, left);
  CHECK_NE(first.str(), other.str());
  const std::vector<ConflictParticipant> changed = {
      ConflictParticipant{left[0].id, left[0].role, "maintenance"}, left[1]};
  CHECK_NE(first.str(), make_conflict_id(ConflictRule::AdminStateDisagreement, changed).str());
}

IFABRIC_TEST(conflict, detection_is_deterministic_under_permutation) {
  SynthOptions options;
  options.defect = SynthDefect::AdminStateDisagreement;
  const IntentDocument reference = normalize(synthesize(core_domain(), options));
  const std::vector<Conflict> baseline = conflicts_of(reference);
  REQUIRE(!baseline.empty());
  for (std::uint64_t seed = 1; seed <= 16; ++seed) {
    IntentDocument shuffled = reference;
    ifabric_test::permute_document(shuffled, seed);
    const std::vector<Conflict> again = conflicts_of(normalize(shuffled));
    REQUIRE(again.size() == baseline.size());
    for (std::size_t i = 0; i < again.size(); ++i) {
      CHECK_EQ(again[i].id.str(), baseline[i].id.str());
      CHECK_EQ(again[i].render(), baseline[i].render());
      CHECK_EQ(again[i].to_json().dump(), baseline[i].to_json().dump());
    }
  }
}

IFABRIC_TEST(conflict, repeated_detection_is_byte_identical) {
  SynthOptions options;
  options.defect = SynthDefect::RoutingDisagreement;
  const IntentDocument document = synth_document(options);
  const std::vector<Conflict> first = conflicts_of(document);
  const std::vector<Conflict> second = conflicts_of(document);
  REQUIRE(first.size() == second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    CHECK_EQ(first[i].to_json().dump(), second[i].to_json().dump());
  }
}

IFABRIC_TEST(conflict, participants_are_sorted_and_unique) {
  SynthOptions options;
  options.defect = SynthDefect::CapacityOvercommit;
  const std::vector<Conflict> conflicts = conflicts_of(synth_document(options));
  REQUIRE(!conflicts.empty());
  for (const auto& conflict : conflicts) {
    for (std::size_t i = 1; i < conflict.participants.size(); ++i) {
      CHECK(!(conflict.participants[i] < conflict.participants[i - 1]));
    }
  }
}

IFABRIC_TEST(conflict, unknown_capacity_produces_no_false_conflict) {
  SynthOptions options;
  options.defect = SynthDefect::EmptyCapacity;
  const std::vector<Conflict> conflicts = conflicts_of(synth_document(options));
  // An unobserved capacity must not be treated as infinite nor as zero: the
  // capability layer reports it, the conflict layer stays silent.
  CHECK_EQ(count_rule(conflicts, ConflictRule::CapacityOvercommit), std::size_t{0});
}
