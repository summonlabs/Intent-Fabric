// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/canonical.hpp"
#include "ifabric/diff.hpp"
#include "ifabric/synth.hpp"
#include "ifabric_test.hpp"
#include "permute.hpp"

using namespace ifabric;

namespace {

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

const DiffEntry* find_entry(const SemanticDiff& diff, const std::string& path) {
  for (const auto& entry : diff.entries) {
    if (entry.path == path) {
      return &entry;
    }
  }
  return nullptr;
}

const DiffEntry* find_containing(const SemanticDiff& diff, const std::string& fragment) {
  for (const auto& entry : diff.entries) {
    if (entry.path.find(fragment) != std::string::npos) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

IFABRIC_TEST(diff, identical_documents_have_an_empty_diff) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const SemanticDiff diff = diff_documents(document, document);
  CHECK(diff.empty());
  CHECK_EQ(diff.worst, Impact::None);
  CHECK_EQ(diff.from_content.hex(), diff.to_content.hex());
  CHECK_EQ(diff.entries.size(), std::size_t{0});
  CHECK_EQ(diff.render().find("0 change(s)") != std::string::npos, true);
}

IFABRIC_TEST(diff, semantically_equivalent_documents_have_an_empty_diff) {
  const IntentDocument reference = synthesize(core_domain(), SynthOptions{});
  IntentDocument shuffled = reference;
  ifabric_test::permute_document(shuffled, 7);
  IntentDocument renamed = shuffled;
  renamed.provenance.description = "narrated differently";
  const SemanticDiff diff = diff_documents(reference, renamed);
  // Only the provenance differs, and provenance carries no semantic impact.
  CHECK_EQ(diff.worst, Impact::None);
  for (const auto& entry : diff.entries) {
    CHECK_EQ(entry.category, DiffCategory::Provenance);
  }
}

IFABRIC_TEST(diff, administrative_state_change_is_disruptive) {
  IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument after = before;
  after.devices.front().admin = AdminState::Disabled;
  const SemanticDiff diff = diff_documents(before, after);
  const DiffEntry* entry = find_containing(diff, ".admin");
  REQUIRE(entry != nullptr);
  CHECK_EQ(entry->category, DiffCategory::AdministrativeState);
  CHECK_EQ(entry->impact, Impact::Disruptive);
  CHECK_EQ(entry->change, std::string("modified"));
  CHECK_EQ(diff.worst, Impact::Disruptive);
}

IFABRIC_TEST(diff, administrative_state_to_maintenance_is_service_affecting) {
  IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument after = before;
  after.devices.front().admin = AdminState::Maintenance;
  const SemanticDiff diff = diff_documents(before, after);
  const DiffEntry* entry = find_containing(diff, ".admin");
  REQUIRE(entry != nullptr);
  CHECK_EQ(entry->impact, Impact::ServiceAffecting);
}

IFABRIC_TEST(diff, added_and_removed_objects_are_additive_and_subtractive) {
  const IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument after = before;
  after.devices.erase(after.devices.begin());
  DeviceDecl extra;
  extra.id = DeviceId::from_local("extra-leaf").value();
  extra.site = SiteId::from_local("a").value();
  extra.pod = PodId::from_local("a1").value();
  extra.rack = RackId::from_local("a1").value();
  extra.model = "acme.tor-32";
  extra.declared_port_count = 32;
  after.devices.push_back(extra);
  const SemanticDiff diff = diff_documents(before, after);
  const DiffEntry* removed = find_containing(diff, ".devices[dev.leaf-0]");
  REQUIRE(removed != nullptr);
  CHECK_EQ(removed->change, std::string("removed"));
  CHECK_EQ(removed->impact, Impact::Subtractive);
  const DiffEntry* added = find_containing(diff, ".devices[dev.extra-leaf]");
  REQUIRE(added != nullptr);
  CHECK_EQ(added->change, std::string("added"));
  CHECK_EQ(added->impact, Impact::Additive);
}

IFABRIC_TEST(diff, capacity_reduction_is_service_affecting_and_growth_is_additive) {
  const IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument reduced = before;
  reduced.links.front().capacity_bps = 100000000000ull;
  const SemanticDiff shrink = diff_documents(before, reduced);
  const DiffEntry* entry = find_containing(shrink, ".capacity_bps");
  REQUIRE(entry != nullptr);
  CHECK_EQ(entry->category, DiffCategory::Capacity);
  CHECK_EQ(entry->impact, Impact::ServiceAffecting);
  const SemanticDiff grow = diff_documents(reduced, before);
  const DiffEntry* grown = find_containing(grow, ".capacity_bps");
  REQUIRE(grown != nullptr);
  CHECK_EQ(grown->impact, Impact::Additive);
}

IFABRIC_TEST(diff, routing_and_redundancy_changes_are_categorised) {
  const IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument after = before;
  for (auto& policy : after.policies) {
    if (auto* body = std::get_if<RoutingPolicyDecl>(&policy.body)) {
      body->action = RoutingAction::Drop;
      body->explicit_path.clear();
      body->match_service_classes.clear();
      body->priority = 5;
    }
    if (auto* body = std::get_if<RedundancyPolicyDecl>(&policy.body)) {
      body->min_disjoint_paths = 1;
    }
  }
  const SemanticDiff diff = diff_documents(before, after);
  const DiffEntry* routing = find_containing(diff, ".routing.");
  REQUIRE(routing != nullptr);
  CHECK_EQ(routing->category, DiffCategory::Routing);
  CHECK_EQ(routing->impact, Impact::ServiceAffecting);
  const DiffEntry* redundancy = find_containing(diff, ".redundancy.min_disjoint_paths");
  REQUIRE(redundancy != nullptr);
  CHECK_EQ(redundancy->category, DiffCategory::Redundancy);
  CHECK_EQ(redundancy->impact, Impact::ServiceAffecting);
}

IFABRIC_TEST(diff, maintenance_and_tenancy_changes_are_categorised) {
  const IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument after = before;
  for (auto& policy : after.policies) {
    if (auto* body = std::get_if<MaintenanceEligibilityPolicyDecl>(&policy.body)) {
      body->max_concurrent_operations = 3;
    }
    if (auto* body = std::get_if<TenantIsolationPolicyDecl>(&policy.body)) {
      body->forbid_shared_links = false;
    }
  }
  const SemanticDiff diff = diff_documents(before, after);
  const DiffEntry* maintenance = find_containing(diff, ".maintenance-eligibility.");
  REQUIRE(maintenance != nullptr);
  CHECK_EQ(maintenance->category, DiffCategory::Maintenance);
  CHECK_EQ(maintenance->impact, Impact::ServiceAffecting);
  const DiffEntry* tenancy = find_containing(diff, ".tenant-isolation.");
  REQUIRE(tenancy != nullptr);
  CHECK_EQ(tenancy->category, DiffCategory::Tenancy);
}

IFABRIC_TEST(diff, schema_change_is_reported_as_unknown_impact) {
  IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument after = before;
  after.schema = SchemaVersion::make(1, 2).value();
  const SemanticDiff diff = diff_documents(before, after);
  const DiffEntry* entry = find_entry(diff, "$.schema.minor");
  REQUIRE(entry != nullptr);
  CHECK_EQ(entry->category, DiffCategory::Schema);
  CHECK_EQ(entry->impact, Impact::Unknown);
  CHECK_EQ(diff.worst, Impact::Unknown);
}

IFABRIC_TEST(diff, entries_are_ordered_by_path) {
  const IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument after = before;
  after.devices.front().admin = AdminState::Disabled;
  after.links.front().capacity_bps = 100000000000ull;
  after.intents.front().desired = JsonValue::string("disabled");
  const SemanticDiff diff = diff_documents(before, after);
  REQUIRE(diff.entries.size() >= 3);
  for (std::size_t i = 1; i < diff.entries.size(); ++i) {
    CHECK(!(diff.entries[i].path < diff.entries[i - 1].path));
  }
}

IFABRIC_TEST(diff, is_deterministic_under_permutation) {
  const IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument after = before;
  after.devices.front().admin = AdminState::Disabled;
  after.links.front().capacity_bps = 100000000000ull;
  const SemanticDiff baseline = diff_documents(before, after);
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    IntentDocument left = before;
    IntentDocument right = after;
    ifabric_test::permute_document(left, seed);
    ifabric_test::permute_document(right, seed * 31);
    const SemanticDiff again = diff_documents(left, right);
    CHECK_EQ(again.to_json().dump(), baseline.to_json().dump());
    CHECK_EQ(again.render(), baseline.render());
  }
}

IFABRIC_TEST(diff, reports_content_identities_of_both_sides) {
  const IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument after = before;
  after.devices.front().admin = AdminState::Disabled;
  const SemanticDiff diff = diff_documents(before, after);
  CHECK_EQ(diff.from_content.hex(), content_digest_of(before).hex());
  CHECK_EQ(diff.to_content.hex(), content_digest_of(after).hex());
  CHECK_NE(diff.from_content.hex(), diff.to_content.hex());
  CHECK_EQ(diff.from_document.hex(), document_digest_of(before).hex());
  // Diffing backwards undoes the picture.
  const SemanticDiff reverse = diff_documents(after, before);
  CHECK_EQ(reverse.from_content.hex(), diff.to_content.hex());
  CHECK_EQ(reverse.to_content.hex(), diff.from_content.hex());
}

IFABRIC_TEST(diff, json_projection_carries_impact_and_counts) {
  IntentDocument before = synthesize(core_domain(), SynthOptions{});
  IntentDocument after = before;
  after.devices.front().admin = AdminState::Disabled;
  const SemanticDiff diff = diff_documents(before, after);
  const JsonValue json = diff.to_json();
  CHECK(json.find("changes") != nullptr);
  CHECK_EQ(*json.find("worst_impact")->try_string(), std::string("disruptive"));
  CHECK_EQ(diff.count(Impact::Disruptive), std::size_t{1});
}
