// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Lookup index over a normalized intent document: identity resolution,
// containment resolution, adjacency, scope expansion, duplicate detection and
// the reference graph used for cycle detection.

#ifndef IFABRIC_INDEX_HPP
#define IFABRIC_INDEX_HPP

#include "ifabric/model.hpp"

#include <unordered_map>

namespace ifabric {

struct DuplicateIdentity {
  AnyId id;
  std::size_t count = 0;
  std::string first_path;
  std::string second_path;
};

class TopologyIndex {
 public:
  explicit TopologyIndex(const IntentDocument& document);

  const IntentDocument& document() const noexcept { return *document_; }

  const FabricDecl* fabric(const FabricId& id) const;
  const SiteDecl* site(const SiteId& id) const;
  const PodDecl* pod(const PodId& id) const;
  const RackDecl* rack(const RackId& id) const;
  const DeviceDecl* device(const DeviceId& id) const;
  const PortDecl* port(const PortId& id) const;
  const LinkDecl* link(const LinkId& id) const;
  const CapabilityDecl* capability_model(const std::string& model) const;
  const ServiceClassDecl* service_class(const ServiceClassId& id) const;
  const PolicyDecl* policy(const PolicyId& id) const;
  const TenantDecl* tenant(const TenantId& id) const;
  const WorkloadDecl* workload(const WorkloadId& id) const;

  // Resolves an AnyId to a human-readable description, or reports that it does
  // not resolve. Unknown references are never treated as satisfied.
  bool resolves(const AnyId& id) const;
  std::string describe(const AnyId& id) const;

  std::vector<const DeviceDecl*> devices_in_site(const SiteId& id) const;
  std::vector<const DeviceDecl*> devices_in_pod(const PodId& id) const;
  std::vector<const DeviceDecl*> devices_in_rack(const RackId& id) const;
  std::vector<const PortDecl*> ports_of_device(const DeviceId& id) const;
  std::vector<const LinkDecl*> links_of_device(const DeviceId& id) const;
  const LinkDecl* link_of_port(const PortId& id) const;

  // Expands a policy scope (fabric/site/pod/rack/device/link/service-class/
  // tenant) into the devices it covers. Returns false when the scope is not a
  // scope-capable identity.
  bool devices_of_scope(const AnyId& scope, std::vector<const DeviceDecl*>& out) const;
  bool links_of_scope(const AnyId& scope, std::vector<const LinkDecl*>& out) const;
  bool is_scope_capable(const AnyId& scope) const;

  const std::vector<DuplicateIdentity>& duplicates() const noexcept { return duplicates_; }

  // The containment level a device belongs to, for failure-domain arithmetic.
  std::string failure_domain_key(const DeviceDecl& device, FailureDomainLevel level) const;

 private:
  const IntentDocument* document_;
  std::unordered_map<std::string, std::size_t> fabrics_;
  std::unordered_map<std::string, std::size_t> sites_;
  std::unordered_map<std::string, std::size_t> pods_;
  std::unordered_map<std::string, std::size_t> racks_;
  std::unordered_map<std::string, std::size_t> devices_;
  std::unordered_map<std::string, std::size_t> ports_;
  std::unordered_map<std::string, std::size_t> links_;
  std::unordered_map<std::string, std::size_t> capabilities_;
  std::unordered_map<std::string, std::size_t> service_classes_;
  std::unordered_map<std::string, std::size_t> policies_;
  std::unordered_map<std::string, std::size_t> tenants_;
  std::unordered_map<std::string, std::size_t> workloads_;
  std::unordered_map<std::string, std::vector<std::size_t>> ports_by_device_;
  std::unordered_map<std::string, std::vector<std::size_t>> links_by_device_;
  std::unordered_map<std::string, std::size_t> link_by_port_;
  std::vector<DuplicateIdentity> duplicates_;
};

// Deterministic cycle detection over the document's reference graph. Each
// reported cycle is a sorted, rotated-to-minimum list of identities so the
// result does not depend on traversal order.
struct ReferenceCycle {
  std::vector<AnyId> members;
};

std::vector<ReferenceCycle> detect_reference_cycles(const IntentDocument& document);

}  // namespace ifabric

#endif  // IFABRIC_INDEX_HPP
