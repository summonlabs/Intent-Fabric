// Intent Fabric - downstream packaging consumer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is not part of the Intent Fabric build. It is configured and
// built by cmake/RunDownstreamCheck.cmake against an installed tree, and it
// uses only the public headers and the exported CMake target.

#include <ifabric/canonical.hpp>
#include <ifabric/conflict.hpp>
#include <ifabric/synth.hpp>
#include <ifabric/validate.hpp>

#include <iostream>

int main() {
  const auto domain = ifabric::DomainId::parse("dom.downstream");
  if (!domain) {
    std::cerr << "cannot build a domain identity\n";
    return 1;
  }
  ifabric::SynthOptions options;
  options.leaves = 4;
  options.spines = 2;
  options.servers_per_leaf = 2;
  const ifabric::IntentDocument document = ifabric::synthesize(domain.value(), options);

  const auto form = ifabric::canonicalize(document);
  if (!form) {
    std::cerr << form.error().render() << "\n";
    return 1;
  }
  const ifabric::ValidationReport report =
      ifabric::validate_document(form->normalized, document.schema);
  if (!report.passed()) {
    std::cerr << report.render() << "\n";
    return 1;
  }
  const ifabric::TopologyIndex index(form->normalized);
  const std::vector<ifabric::Conflict> conflicts =
      ifabric::detect_conflicts(form->normalized, index);
  if (!conflicts.empty()) {
    std::cerr << "unexpected conflicts\n";
    return 1;
  }
  std::cout << "IntentFabric " << ifabric::kVersionString << " consumer ok"
            << " schema=" << form->normalized.schema.str()
            << " objects=" << form->normalized.declared_object_count()
            << " content=" << form->content_digest.short_hex() << "\n";
  return 0;
}
