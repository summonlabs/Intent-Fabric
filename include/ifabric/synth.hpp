// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic synthetic intent generator. It produces a fully valid leaf and
// spine topology by default, and can inject a single named defect so that the
// adversarial test matrix is reproducible from a seed.

#ifndef IFABRIC_SYNTH_HPP
#define IFABRIC_SYNTH_HPP

#include "ifabric/model.hpp"

namespace ifabric {

enum class SynthDefect : std::uint8_t {
  None = 0,
  DuplicateIdentity,
  DanglingReference,
  CyclicPolicyComposition,
  ImpossibleRedundancy,
  UnknownCapability,
  UnknownProperty,
  CapacityOvercommit,
  RoutingDisagreement,
  AdminStateDisagreement,
  PortIndexOutOfRange,
  UnknownModel,
  UnsupportedMinor,
  UnknownFeature,
  SelfLoopLink,
  DisabledLinkWithEnabledPort,
  UnknownRequiredCapability,
  EmptyCapacity
};

std::string_view to_string(SynthDefect defect) noexcept;
Result<SynthDefect> parse_synth_defect(std::string_view text);
const std::vector<std::pair<std::string_view, SynthDefect>>& synth_defect_registry();

struct SynthOptions {
  std::size_t leaves = 4;
  std::size_t spines = 2;
  std::size_t servers_per_leaf = 2;
  bool include_policies = true;
  bool include_tenants = true;
  bool include_workloads = true;
  bool include_intents = true;
  SynthDefect defect = SynthDefect::None;
  std::uint64_t seed = 1;
  std::string actor = "actor.synth";
  std::string source_revision = "synth:1";
};

// Builds a document. Every identity is derived from the index, so two calls
// with the same options produce byte-identical canonical forms.
IntentDocument synthesize(const DomainId& domain, const SynthOptions& options);

// Declares the object count the generator will produce, so that callers can
// bound a synthetic document before materializing it.
std::size_t synth_declared_objects(const SynthOptions& options);

}  // namespace ifabric

#endif  // IFABRIC_SYNTH_HPP
