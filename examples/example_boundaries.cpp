// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Prints the systems boundary this runtime implements, the schema versions and
// features it understands, and the vocabulary it reasons about.

#include "ifabric/validate.hpp"

#include <iostream>

int main() {
  std::cout << ifabric::kProductName << " " << ifabric::kVersionString << " ("
            << ifabric::kVendorName << ")\n\n";
  std::cout << "owns:\n"
            << "  - the canonical declarative statement of desired network state\n"
            << "  - validated, normalized, generation-bound intent artifacts\n\n";
  std::cout << "does not own:\n"
            << "  - device configuration rendering or distribution (Configuration Fabric)\n"
            << "  - transition sequencing (Change Planner)\n"
            << "  - staged deployment (Rollout Fabric)\n"
            << "  - maintenance scheduling (Maintenance Fabric)\n"
            << "  - actual-vs-intended observation (Network Drift Observatory)\n\n";
  std::cout << "schema " << ifabric::kSchemaName << " supports "
            << ifabric::SchemaVersion::kSupportedMajor << "."
            << ifabric::SchemaVersion::kOldestMinor << " .. "
            << ifabric::SchemaVersion::kSupportedMajor << "."
            << ifabric::SchemaVersion::kSupportedMinor << "\n";
  std::cout << "features:\n";
  for (const auto& feature : ifabric::known_features()) {
    std::cout << "  " << feature.name << " (major " << ifabric::SchemaVersion::kSupportedMajor
              << "." << feature.introduced_minor << ")\n";
  }
  std::cout << "\ncapabilities the runtime reasons about:\n  ";
  for (const auto& capability : ifabric::known_capabilities()) {
    std::cout << capability << " ";
  }
  std::cout << "\n\nintent properties:\n";
  for (const auto& property : ifabric::known_properties()) {
    std::cout << "  " << property.name << " -> ";
    for (std::size_t i = 0; i < property.subjects.size(); ++i) {
      if (i != 0) {
        std::cout << ", ";
      }
      std::cout << ifabric::id_class_name(property.subjects[i]);
    }
    std::cout << "\n";
  }
  return 0;
}
