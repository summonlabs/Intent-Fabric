// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic property and randomised state-machine tests. Every case is
// reproducible from its seed.

#include "ifabric/canonical.hpp"
#include "ifabric/conflict.hpp"
#include "ifabric/synth.hpp"
#include "ifabric/validate.hpp"
#include "ifabric_test.hpp"
#include "permute.hpp"

#include <iostream>

using namespace ifabric;

namespace {

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

SynthOptions random_options(DeterministicRng& rng) {
  SynthOptions options;
  options.leaves = 2 + rng.next_bounded(5);
  options.spines = 2 + rng.next_bounded(2);
  options.servers_per_leaf = 1 + rng.next_bounded(3);
  options.include_policies = rng.next_bool();
  options.include_tenants = rng.next_bool();
  options.include_workloads = rng.next_bool();
  options.include_intents = rng.next_bool();
  options.seed = rng.next_u64();
  return options;
}

std::string replace_first(std::string text, const std::string& needle, const std::string& value) {
  const std::size_t position = text.find(needle);
  if (position == std::string::npos) {
    return text;
  }
  return text.substr(0, position) + value + text.substr(position + needle.size());
}

}  // namespace

IFABRIC_TEST(property, randomised_valid_documents_always_validate) {
  DeterministicRng rng(20260101);
  for (int iteration = 0; iteration < 40; ++iteration) {
    SynthOptions options = random_options(rng);
    options.defect = SynthDefect::None;
    const IntentDocument document = synthesize(core_domain(), options);
    const ValidationReport report = validate_document(document, document.schema);
    if (!report.passed()) {
      std::cout << "iteration " << iteration << " seed " << options.seed << " rejected\n"
                << report.render() << "\n";
    }
    CHECK(report.passed());
  }
}

IFABRIC_TEST(property, randomised_defect_documents_always_fail) {
  DeterministicRng rng(777);
  for (int iteration = 0; iteration < 60; ++iteration) {
    SynthOptions options = random_options(rng);
    const auto& registry = synth_defect_registry();
    options.defect = registry[1 + rng.next_bounded(registry.size() - 1)].second;
    const IntentDocument document = synthesize(core_domain(), options);
    const ValidationReport report = validate_text(to_json(document).dump());
    if (report.passed()) {
      std::cout << "defect " << to_string(options.defect) << " seed " << options.seed
                << " was accepted\n";
    }
    CHECK(!report.passed());
  }
}

IFABRIC_TEST(property, canonical_identity_is_permutation_invariant) {
  DeterministicRng rng(4242);
  for (int iteration = 0; iteration < 12; ++iteration) {
    SynthOptions options = random_options(rng);
    options.defect = SynthDefect::None;
    const IntentDocument reference = synthesize(core_domain(), options);
    const auto base = canonicalize(reference);
    REQUIRE(base.ok());
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
      IntentDocument shuffled = reference;
      ifabric_test::permute_document(shuffled, seed * 1013 + iteration);
      const auto form = canonicalize(shuffled);
      REQUIRE(form.ok());
      CHECK_EQ(form->content_digest.hex(), base->content_digest.hex());
      CHECK_EQ(form->document_digest.hex(), base->document_digest.hex());
    }
  }
}

IFABRIC_TEST(property, conflict_identity_is_permutation_invariant) {
  DeterministicRng rng(31337);
  for (int iteration = 0; iteration < 10; ++iteration) {
    SynthOptions options = random_options(rng);
    options.defect = SynthDefect::AdminStateDisagreement;
    const IntentDocument reference = normalize(synthesize(core_domain(), options));
    const TopologyIndex reference_index(reference);
    const std::vector<Conflict> baseline = detect_conflicts(reference, reference_index);
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
      IntentDocument shuffled = reference;
      ifabric_test::permute_document(shuffled, seed * 7919 + iteration);
      const IntentDocument normalised = normalize(std::move(shuffled));
      const TopologyIndex index(normalised);
      const std::vector<Conflict> again = detect_conflicts(normalised, index);
      REQUIRE(again.size() == baseline.size());
      for (std::size_t i = 0; i < again.size(); ++i) {
        CHECK_EQ(again[i].id.str(), baseline[i].id.str());
      }
    }
  }
}

IFABRIC_TEST(property, truncated_documents_are_never_accepted) {
  const std::string text = to_json(synthesize(core_domain(), SynthOptions{})).dump();
  REQUIRE(text.size() > 100);
  for (std::size_t cut = 1; cut < text.size(); cut += 37) {
    const ValidationReport report = validate_text(text.substr(0, cut));
    if (report.passed()) {
      std::cout << "truncation at " << cut << " was accepted\n";
    }
    CHECK(!report.passed());
  }
}

IFABRIC_TEST(property, hostile_byte_mutations_never_crash) {
  const std::string text = to_json(synthesize(core_domain(), SynthOptions{})).dump();
  DeterministicRng rng(909);
  const char hostile[] = {'{', '}', '[', ']', '"', '\\', ',', ':', 'x', '0', '\n', '\t', 0x7F};
  std::size_t rejected = 0;
  for (int iteration = 0; iteration < 300; ++iteration) {
    std::string mutated = text;
    const std::size_t position = rng.next_bounded(mutated.size());
    mutated[position] = hostile[rng.next_bounded(sizeof(hostile))];
    const ValidationReport report = validate_text(mutated);
    if (!report.passed()) {
      ++rejected;
    }
  }
  // Structural noise is overwhelmingly rejected; the point of the property is
  // that every mutation is answered, never crashes, and never silently rewrites
  // an invalid document into a valid one.
  CHECK(rejected > 250);
}

IFABRIC_TEST(property, wrong_identity_class_in_a_reference_is_rejected) {
  const std::string text = to_json(synthesize(core_domain(), SynthOptions{})).dump();
  const std::string mutated = replace_first(text, "\"dev.leaf-0\"", "\"pol.leaf-0\"");
  CHECK_NE(mutated, text);
  const ValidationReport report = validate_text(mutated);
  CHECK(!report.passed());
}

IFABRIC_TEST(property, removing_a_device_model_breaks_capability_proof) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  document.capability_catalog.erase(document.capability_catalog.begin());
  const ValidationReport report = validate_document(document, document.schema);
  CHECK(!report.passed());
  bool capability_error = false;
  for (const auto& diagnostic : report.diagnostics) {
    if (diagnostic.code == ErrorCode::UnknownCapability) {
      capability_error = true;
    }
  }
  CHECK(capability_error);
}

IFABRIC_TEST(property, huge_but_bounded_topologies_stay_within_the_object_bound) {
  SynthOptions options;
  options.leaves = 64;
  options.spines = 4;
  options.servers_per_leaf = 8;
  const IntentDocument document = synthesize(core_domain(), options);
  CHECK(document.declared_object_count() <= kMaxObjectsPerDocument);
  const ValidationReport report = validate_document(document, document.schema);
  if (!report.passed()) {
    std::cout << report.render().substr(0, 4000) << "\n";
  }
  CHECK(report.passed());
  CHECK_EQ(document.declared_object_count(), synth_declared_objects(options));
}

IFABRIC_TEST(property, canonical_form_survives_repeated_round_trips) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const auto initial = canonicalize(document);
  REQUIRE(initial.ok());
  JsonValue json = initial->document_json;
  for (int cycle = 0; cycle < 8; ++cycle) {
    const auto parsed = from_json(json);
    REQUIRE(parsed.ok());
    const auto form = canonicalize(parsed.value());
    REQUIRE(form.ok());
    const std::string before = json.dump();
    json = form->document_json;
    CHECK_EQ(json.dump(), before);
  }
}

IFABRIC_TEST(property, adversarial_cyclic_references_are_always_detected) {
  for (int length = 2; length <= 6; ++length) {
    IntentDocument document = synthesize(core_domain(), SynthOptions{});
    for (int index = 0; index < length; ++index) {
      PolicyDecl policy;
      policy.id = PolicyId::from_local("cycle-" + std::to_string(index)).value();
      RoutingPolicyDecl body;
      body.action = RoutingAction::ShortestPath;
      body.max_ecmp_width = 1;
      body.extends_policy =
          PolicyId::from_local("cycle-" + std::to_string((index + 1) % length)).value().str();
      policy.body = body;
      document.policies.push_back(policy);
    }
    const ValidationReport report = validate_document(normalize(document), document.schema);
    CHECK(!report.passed());
    bool cyclic = false;
    for (const auto& diagnostic : report.diagnostics) {
      if (diagnostic.code == ErrorCode::CyclicReference) {
        cyclic = true;
      }
    }
    CHECK(cyclic);
  }
}

IFABRIC_TEST(property, duplicate_identity_flood_is_reported_once_per_identity) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const DeviceDecl sample = document.devices.front();
  for (int copy = 0; copy < 5; ++copy) {
    document.devices.push_back(sample);
  }
  const ValidationReport report = validate_document(normalize(document), document.schema);
  CHECK(!report.passed());
  std::size_t duplicates = 0;
  for (const auto& diagnostic : report.diagnostics) {
    if (diagnostic.code == ErrorCode::DuplicateIdentity) {
      ++duplicates;
    }
  }
  CHECK_EQ(duplicates, std::size_t{1});
}

IFABRIC_TEST(property, impossible_constraints_are_always_infeasible) {
  DeterministicRng rng(5150);
  for (int iteration = 0; iteration < 12; ++iteration) {
    IntentDocument document = synthesize(core_domain(), SynthOptions{});
    PolicyDecl policy;
    policy.id = PolicyId::from_local("impossible-" + std::to_string(iteration)).value();
    RedundancyPolicyDecl body;
    body.scope = FabricId::from_local("core").value().str();
    body.level = FailureDomainLevel::Site;
    body.min_disjoint_paths = static_cast<std::uint32_t>(2 + rng.next_bounded(64));
    policy.body = body;
    document.policies.push_back(policy);
    const ValidationReport report = validate_document(normalize(document), document.schema);
    CHECK(!report.passed());
  }
}
