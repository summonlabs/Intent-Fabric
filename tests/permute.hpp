// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic permutation of every collection in an intent document. Used by
// the tests that prove canonicalization, conflict detection and diffing do not
// depend on declaration order.

#ifndef IFABRIC_TEST_PERMUTE_HPP
#define IFABRIC_TEST_PERMUTE_HPP

#include "ifabric/model.hpp"

#include <algorithm>

namespace ifabric_test {

inline void permute_document(ifabric::IntentDocument& document, std::uint64_t seed) {
  ifabric::DeterministicRng rng(seed);
  const auto shuffle = [&rng](auto& items) {
    for (std::size_t i = items.size(); i > 1; --i) {
      const std::size_t j = rng.next_bounded(i);
      std::swap(items[i - 1], items[j]);
    }
  };
  shuffle(document.fabrics);
  shuffle(document.sites);
  shuffle(document.pods);
  shuffle(document.racks);
  shuffle(document.devices);
  shuffle(document.ports);
  shuffle(document.links);
  shuffle(document.capability_catalog);
  shuffle(document.service_classes);
  shuffle(document.policies);
  shuffle(document.tenants);
  shuffle(document.workloads);
  shuffle(document.intents);
  shuffle(document.required_features);
  shuffle(document.optional_features);
  for (auto& device : document.devices) {
    shuffle(device.service_classes);
  }
  for (auto& port : document.ports) {
    shuffle(port.service_classes);
  }
  for (auto& link : document.links) {
    shuffle(link.service_classes);
  }
  for (auto& entry : document.capability_catalog) {
    shuffle(entry.capabilities);
    shuffle(entry.supported_roles);
  }
  for (auto& entry : document.service_classes) {
    shuffle(entry.required_capabilities);
  }
}

}  // namespace ifabric_test

#endif  // IFABRIC_TEST_PERMUTE_HPP
