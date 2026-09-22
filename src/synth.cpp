// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/synth.hpp"

#include <algorithm>

namespace ifabric {

namespace {

struct DefectRow {
  std::string_view name;
  SynthDefect value;
};

const std::vector<std::pair<std::string_view, SynthDefect>>& rows() {
  static const std::vector<std::pair<std::string_view, SynthDefect>> kRows = {
      {"none", SynthDefect::None},
      {"duplicate-identity", SynthDefect::DuplicateIdentity},
      {"dangling-reference", SynthDefect::DanglingReference},
      {"cyclic-policy-composition", SynthDefect::CyclicPolicyComposition},
      {"impossible-redundancy", SynthDefect::ImpossibleRedundancy},
      {"unknown-capability", SynthDefect::UnknownCapability},
      {"unknown-property", SynthDefect::UnknownProperty},
      {"capacity-overcommit", SynthDefect::CapacityOvercommit},
      {"routing-disagreement", SynthDefect::RoutingDisagreement},
      {"admin-state-disagreement", SynthDefect::AdminStateDisagreement},
      {"port-index-out-of-range", SynthDefect::PortIndexOutOfRange},
      {"unknown-model", SynthDefect::UnknownModel},
      {"unsupported-minor", SynthDefect::UnsupportedMinor},
      {"unknown-feature", SynthDefect::UnknownFeature},
      {"self-loop-link", SynthDefect::SelfLoopLink},
      {"disabled-link-with-enabled-port", SynthDefect::DisabledLinkWithEnabledPort},
      {"unknown-required-capability", SynthDefect::UnknownRequiredCapability},
      {"empty-capacity", SynthDefect::EmptyCapacity},
  };
  return kRows;
}

template <IdClass K>
BasicId<K> id_of(const std::string& local) {
  const auto parsed = BasicId<K>::from_local(local);
  return parsed.ok() ? parsed.value() : BasicId<K>();
}

std::string leaf_name(std::size_t index) { return "leaf-" + std::to_string(index); }
std::string spine_name(std::size_t index) { return "spine-" + std::to_string(index); }
std::string server_name(std::size_t leaf, std::size_t slot) {
  return "srv-" + std::to_string(leaf) + "-" + std::to_string(slot);
}

std::uint64_t gbps(std::uint64_t value) { return value * 1000000000ull; }

}  // namespace

std::string_view to_string(SynthDefect defect) noexcept {
  for (const auto& row : rows()) {
    if (row.second == defect) {
      return row.first;
    }
  }
  return "unknown";
}

Result<SynthDefect> parse_synth_defect(std::string_view text) {
  for (const auto& row : rows()) {
    if (row.first == text) {
      return row.second;
    }
  }
  return Error(ErrorCode::UnknownField, "unknown synthetic defect name", std::string(text));
}

const std::vector<std::pair<std::string_view, SynthDefect>>& synth_defect_registry() {
  return rows();
}

namespace {

// Some defects need a substrate to be expressible: a routing collision needs
// the baseline policy set, a capacity overcommit needs the baseline link.
// A leaf-and-spine fabric needs at least as many leaves as spines, otherwise a
// spine would hang off a single leaf and no redundancy requirement could hold.
std::size_t effective_spines(const SynthOptions& options) {
  const std::size_t capped = options.spines == 0 ? 1 : options.spines;
  return options.leaves == 0 ? capped : std::min(capped, options.leaves);
}

bool defect_needs_intents(SynthDefect defect) {
  return defect == SynthDefect::UnknownProperty;
}

bool defect_needs_policies(SynthDefect defect) {
  switch (defect) {
    case SynthDefect::RoutingDisagreement:
    case SynthDefect::AdminStateDisagreement:
    case SynthDefect::CapacityOvercommit:
    case SynthDefect::ImpossibleRedundancy: return true;
    default: return false;
  }
}

}  // namespace

std::size_t synth_declared_objects(const SynthOptions& options) {
  const std::size_t leaves = options.leaves;
  const std::size_t spines = effective_spines(options);
  const std::size_t servers = leaves * options.servers_per_leaf;
  std::size_t total = 0;
  total += 1;  // fabric
  total += 1;  // site
  total += 1;  // pod
  total += 3;  // racks
  total += leaves + spines + servers;
  total += (2 + 2 * options.servers_per_leaf) * leaves;
  total += leaves * 2;
  total += servers * 2;
  total += 2 * leaves + servers * 2;
  total += 3;  // capability catalog
  total += 3;  // service classes
  const bool policies = options.include_policies || defect_needs_policies(options.defect);
  if (policies) {
    total += 5;
  }
  if (options.include_tenants) {
    total += 4;  // two tenants and their isolation policies
  }
  if (options.include_workloads && options.include_tenants && leaves > 0 &&
      options.servers_per_leaf > 0) {
    total += 1;
  }
  const bool intents = options.include_intents || defect_needs_intents(options.defect);
  if (intents && leaves > 0) {
    total += 4;
  }
  switch (options.defect) {
    case SynthDefect::DuplicateIdentity:
    case SynthDefect::ImpossibleRedundancy:
    case SynthDefect::UnknownCapability:
    case SynthDefect::UnknownRequiredCapability:
    case SynthDefect::CapacityOvercommit:
    case SynthDefect::RoutingDisagreement: total += 1; break;
    case SynthDefect::AdminStateDisagreement: total += 2; break;
    case SynthDefect::CyclicPolicyComposition: total += 2; break;
    default: break;
  }
  return total;
}

IntentDocument synthesize(const DomainId& domain, const SynthOptions& options) {
  IntentDocument document;
  document.schema = SchemaVersion::make(SchemaVersion::kSupportedMajor,
                                        SchemaVersion::kSupportedMinor).value();
  document.domain = domain;
  const std::size_t spines = effective_spines(options);
  document.required_features = {"topology.multi-site", "routing.explicit-path",
                                "capacity.reservation", "redundancy.disjoint-paths",
                                "tenancy.isolation", "maintenance.upgrade-groups"};
  document.optional_features = {"telemetry.flow-counters"};

  document.provenance.actor = id_of<IdClass::Actor>(options.actor.empty() ? "actor.synth"
                                                                          : options.actor);
  const auto revision = SourceRevision::parse(options.source_revision);
  if (revision) {
    document.provenance.source_revision = revision.value();
  }
  document.provenance.created_at =
      Timestamp(std::chrono::milliseconds(static_cast<std::int64_t>(1767225600000ull)));
  document.provenance.description = "synthetic reference intent";
  document.provenance.labels.emplace_back("generator", JsonValue::string("ifabric.synth"));
  document.provenance.labels.emplace_back("seed", JsonValue::integer(
                                                       static_cast<std::int64_t>(options.seed)));

  FabricDecl fabric;
  fabric.id = id_of<IdClass::Fabric>("core");
  fabric.display_name = "Core Fabric";
  fabric.description = "synthetic core fabric";
  document.fabrics.push_back(fabric);

  SiteDecl site;
  site.id = id_of<IdClass::Site>("a");
  site.fabric = fabric.id;
  site.display_name = "Site A";
  site.region = "region-1";
  document.sites.push_back(site);

  PodDecl pod;
  pod.id = id_of<IdClass::Pod>("a1");
  pod.site = site.id;
  pod.display_name = "Pod A1";
  document.pods.push_back(pod);

  RackDecl rack_a;
  rack_a.id = id_of<IdClass::Rack>("a1");
  rack_a.pod = pod.id;
  rack_a.position = 1;
  document.racks.push_back(rack_a);

  RackDecl rack_b;
  rack_b.id = id_of<IdClass::Rack>("a2");
  rack_b.pod = pod.id;
  rack_b.position = 2;
  document.racks.push_back(rack_b);

  RackDecl rack_hosts;
  rack_hosts.id = id_of<IdClass::Rack>("h1");
  rack_hosts.pod = pod.id;
  rack_hosts.position = 9;
  document.racks.push_back(rack_hosts);

  CapabilityDecl tor;
  tor.model = "acme.tor-32";
  tor.capabilities = {"bfd", "l2.bridging", "l3.routing", "lag.lacp", "qos.ecn", "qos.pfc",
                      "qos.remark", "telemetry.flow"};
  tor.port_count = 32;
  tor.max_speed_bps = gbps(400);
  tor.supported_roles = {DeviceRole::Leaf};
  document.capability_catalog.push_back(tor);

  CapabilityDecl spine;
  spine.model = "acme.spine-128";
  spine.capabilities = {"bfd", "l2.bridging", "l3.evpn", "l3.routing", "l3.vxlan", "lag.lacp",
                        "qos.ecn", "qos.pfc", "qos.remark", "telemetry.flow"};
  spine.port_count = 128;
  spine.max_speed_bps = gbps(400);
  spine.supported_roles = {DeviceRole::Spine};
  document.capability_catalog.push_back(spine);

  CapabilityDecl server;
  server.model = "acme.srv-2";
  server.capabilities = {"l2.bridging", "rocev2"};
  server.port_count = 2;
  server.max_speed_bps = gbps(100);
  server.supported_roles = {DeviceRole::Compute};
  document.capability_catalog.push_back(server);

  ServiceClassDecl gold;
  gold.id = id_of<IdClass::ServiceClass>("gold");
  gold.display_name = "Gold";
  gold.priority = 300;
  gold.bandwidth_share_permille = 400;
  gold.latency_budget_us = 50;
  gold.lossless = true;
  gold.required_capabilities = {"qos.ecn", "qos.pfc"};
  document.service_classes.push_back(gold);

  ServiceClassDecl silver;
  silver.id = id_of<IdClass::ServiceClass>("silver");
  silver.display_name = "Silver";
  silver.priority = 200;
  silver.bandwidth_share_permille = 300;
  silver.latency_budget_us = 500;
  silver.required_capabilities = {"l2.bridging"};
  document.service_classes.push_back(silver);

  ServiceClassDecl best_effort;
  best_effort.id = id_of<IdClass::ServiceClass>("best-effort");
  best_effort.display_name = "Best Effort";
  best_effort.priority = 100;
  best_effort.bandwidth_share_permille = 300;
  document.service_classes.push_back(best_effort);

  const ServiceClassId gold_id = gold.id;
  const ServiceClassId silver_id = silver.id;
  const ServiceClassId best_id = best_effort.id;
  const std::vector<ServiceClassId> fabric_classes = {best_id, gold_id, silver_id};
  const std::vector<ServiceClassId> server_classes = {best_id, silver_id};

  for (std::size_t i = 0; i < options.leaves; ++i) {
    DeviceDecl leaf;
    leaf.id = id_of<IdClass::Device>(leaf_name(i));
    leaf.site = site.id;
    leaf.pod = pod.id;
    leaf.rack = (i % 2 == 0) ? rack_a.id : rack_b.id;
    leaf.role = DeviceRole::Leaf;
    leaf.model = "acme.tor-32";
    leaf.admin = AdminState::Enabled;
    leaf.declared_port_count = 32;
    leaf.service_classes = fabric_classes;
    leaf.description = "synthetic leaf";
    document.devices.push_back(leaf);
  }
  for (std::size_t i = 0; i < spines; ++i) {
    DeviceDecl spine_device;
    spine_device.id = id_of<IdClass::Device>(spine_name(i));
    spine_device.site = site.id;
    spine_device.pod = pod.id;
    spine_device.rack = (i % 2 == 0) ? rack_a.id : rack_b.id;
    spine_device.role = DeviceRole::Spine;
    spine_device.model = "acme.spine-128";
    spine_device.admin = AdminState::Enabled;
    spine_device.declared_port_count = 128;
    spine_device.service_classes = fabric_classes;
    spine_device.description = "synthetic spine";
    document.devices.push_back(spine_device);
  }
  for (std::size_t i = 0; i < options.leaves; ++i) {
    for (std::size_t slot = 0; slot < options.servers_per_leaf; ++slot) {
      DeviceDecl host;
      host.id = id_of<IdClass::Device>(server_name(i, slot));
      host.site = site.id;
      host.pod = pod.id;
      host.rack = rack_hosts.id;
      host.role = DeviceRole::Compute;
      host.model = "acme.srv-2";
      host.admin = AdminState::Enabled;
      host.declared_port_count = 2;
      host.service_classes = server_classes;
      host.description = "synthetic server";
      document.devices.push_back(host);
    }
  }

  for (std::size_t i = 0; i < options.leaves; ++i) {
    for (std::size_t uplink = 0; uplink < 2; ++uplink) {
      PortDecl port;
      port.id = id_of<IdClass::Port>(leaf_name(i) + "-p" + std::to_string(uplink));
      port.device = id_of<IdClass::Device>(leaf_name(i));
      port.index = static_cast<std::uint32_t>(uplink);
      port.role = PortRole::Uplink;
      port.admin = AdminState::Enabled;
      port.speed_bps = gbps(400);
      port.media = "optical-400g";
      port.service_classes = fabric_classes;
      document.ports.push_back(port);
    }
    for (std::size_t slot = 0; slot < 2 * options.servers_per_leaf; ++slot) {
      PortDecl port;
      port.id = id_of<IdClass::Port>(leaf_name(i) + "-h" + std::to_string(slot));
      port.device = id_of<IdClass::Device>(leaf_name(i));
      port.index = static_cast<std::uint32_t>(2 + slot);
      port.role = PortRole::Access;
      port.admin = AdminState::Enabled;
      port.speed_bps = gbps(100);
      port.media = "copper-100g";
      port.service_classes = server_classes;
      document.ports.push_back(port);
    }
  }
  std::vector<std::size_t> spine_port_cursor(spines, 0);
  for (std::size_t leaf_index = 0; leaf_index < options.leaves; ++leaf_index) {
    for (std::size_t uplink = 0; uplink < 2; ++uplink) {
      const std::size_t spine_index =
          spines == 0 ? 0 : (leaf_index + uplink) % spines;
      const std::size_t peer = spine_port_cursor[spine_index]++;
      PortDecl port;
      port.id = id_of<IdClass::Port>(spine_name(spine_index) + "-p" + std::to_string(peer));
      port.device = id_of<IdClass::Device>(spine_name(spine_index));
      port.index = static_cast<std::uint32_t>(peer);
      port.role = PortRole::Fabric;
      port.admin = AdminState::Enabled;
      port.speed_bps = gbps(400);
      port.media = "optical-400g";
      port.service_classes = fabric_classes;
      document.ports.push_back(port);
    }
  }
  for (std::size_t i = 0; i < options.leaves; ++i) {
    for (std::size_t slot = 0; slot < options.servers_per_leaf; ++slot) {
      for (std::size_t nic = 0; nic < 2; ++nic) {
        PortDecl port;
        port.id = id_of<IdClass::Port>(server_name(i, slot) + "-p" + std::to_string(nic));
        port.device = id_of<IdClass::Device>(server_name(i, slot));
        port.index = static_cast<std::uint32_t>(nic);
        port.role = PortRole::Peer;
        port.admin = AdminState::Enabled;
        port.speed_bps = gbps(100);
        port.media = "copper-100g";
        port.service_classes = server_classes;
        document.ports.push_back(port);
      }
    }
  }

  {
    std::vector<std::size_t> spine_link_cursor(spines, 0);
    for (std::size_t leaf_index = 0; leaf_index < options.leaves; ++leaf_index) {
      for (std::size_t uplink = 0; uplink < 2; ++uplink) {
        const std::size_t spine_index =
            spines == 0 ? 0 : (leaf_index + uplink) % spines;
        const std::size_t peer = spine_link_cursor[spine_index]++;
        LinkDecl link;
        link.id = id_of<IdClass::Link>("l" + std::to_string(leaf_index) + "-s" +
                                       std::to_string(spine_index));
        link.endpoint_a =
            id_of<IdClass::Port>(leaf_name(leaf_index) + "-p" + std::to_string(uplink));
        link.endpoint_b =
            id_of<IdClass::Port>(spine_name(spine_index) + "-p" + std::to_string(peer));
        link.kind = LinkKind::Fabric;
        link.capacity_bps = gbps(400);
        link.admin = AdminState::Enabled;
        link.service_classes = fabric_classes;
        document.links.push_back(link);
      }
    }
  }
  for (std::size_t i = 0; i < options.leaves; ++i) {
    for (std::size_t slot = 0; slot < options.servers_per_leaf; ++slot) {
      for (std::size_t nic = 0; nic < 2; ++nic) {
        const std::size_t far_leaf = nic == 0 ? i : (i + 1) % options.leaves;
        const std::size_t far_slot = nic == 0 ? (2 * slot) : (2 * slot + 1);
        LinkDecl link;
        link.id = id_of<IdClass::Link>("h" + std::to_string(i) + "-" + std::to_string(slot) + "-" +
                                       std::to_string(nic));
        link.endpoint_a =
            id_of<IdClass::Port>(server_name(i, slot) + "-p" + std::to_string(nic));
        link.endpoint_b = id_of<IdClass::Port>(leaf_name(far_leaf) + "-h" +
                                               std::to_string(far_slot));
        link.kind = LinkKind::Host;
        link.capacity_bps = gbps(100);
        link.admin = AdminState::Enabled;
        link.service_classes = server_classes;
        document.links.push_back(link);
      }
    }
  }

  const bool policies = options.include_policies || defect_needs_policies(options.defect);
  if (policies) {
    PolicyDecl routing_gold;
    routing_gold.id = id_of<IdClass::Policy>("routing-gold");
    RoutingPolicyDecl gold_body;
    gold_body.priority = 100;
    gold_body.match_service_classes = {gold_id};
    gold_body.action = RoutingAction::ShortestPath;
    gold_body.max_ecmp_width = 4;
    gold_body.description = "gold traffic takes the shortest path with ECMP";
    routing_gold.body = gold_body;
    document.policies.push_back(routing_gold);

    PolicyDecl routing_edge;
    routing_edge.id = id_of<IdClass::Policy>("routing-edge");
    RoutingPolicyDecl edge_body;
    edge_body.priority = 200;
    edge_body.match_service_classes = {silver_id};
    edge_body.action = RoutingAction::ExplicitPath;
    edge_body.explicit_path = {id_of<IdClass::Device>(leaf_name(0)).str(),
                               id_of<IdClass::Device>(spine_name(0)).str()};
    edge_body.max_ecmp_width = 1;
    edge_body.description = "silver traffic is pinned to the first leaf and spine";
    routing_edge.body = edge_body;
    document.policies.push_back(routing_edge);

    PolicyDecl capacity;
    capacity.id = id_of<IdClass::Policy>("reserve-gold");
    CapacityPolicyDecl capacity_body;
    capacity_body.scope = id_of<IdClass::Link>("l0-s0").str();
    capacity_body.service_classes = {gold_id};
    capacity_body.reserved_bps = gbps(100);
    capacity_body.max_utilization_permille = 800;
    capacity_body.description = "reserve gold bandwidth on the first uplink";
    capacity.body = capacity_body;
    document.policies.push_back(capacity);

    PolicyDecl redundancy;
    redundancy.id = id_of<IdClass::Policy>("fabric-redundancy");
    RedundancyPolicyDecl redundancy_body;
    redundancy_body.scope = fabric.id.str();
    redundancy_body.level = FailureDomainLevel::Device;
    redundancy_body.min_disjoint_paths = (options.leaves >= 2 && spines >= 2) ? 2 : 1;
    redundancy_body.service_classes = {gold_id};
    redundancy_body.description = "every device keeps two disjoint neighbours";
    redundancy.body = redundancy_body;
    document.policies.push_back(redundancy);

    PolicyDecl maintenance;
    maintenance.id = id_of<IdClass::Policy>("maintenance-wave");
    MaintenanceEligibilityPolicyDecl maintenance_body;
    maintenance_body.scope = rack_a.id.str();
    maintenance_body.upgrade_groups = {};
    maintenance_body.max_concurrent_operations = 1;
    maintenance_body.drain_required = false;
    maintenance_body.requires_redundancy_headroom = true;
    maintenance_body.allowed_impact = {"additive"};
    maintenance_body.description = "rack a1 upgrades one device at a time";
    maintenance.body = maintenance_body;
    document.policies.push_back(maintenance);
  }

  if (options.include_tenants) {
    TenantDecl tenant_a;
    tenant_a.id = id_of<IdClass::Tenant>("app-a");
    tenant_a.display_name = "Application A";
    tenant_a.isolation_id = 100;
    document.tenants.push_back(tenant_a);

    TenantDecl tenant_b;
    tenant_b.id = id_of<IdClass::Tenant>("app-b");
    tenant_b.display_name = "Application B";
    tenant_b.isolation_id = 200;
    document.tenants.push_back(tenant_b);

    PolicyDecl isolation_a;
    isolation_a.id = id_of<IdClass::Policy>("isolation-a");
    TenantIsolationPolicyDecl isolation_a_body;
    isolation_a_body.tenant = tenant_a.id;
    isolation_a_body.exclusive_scopes = {rack_a.id.str()};
    isolation_a_body.forbid_shared_links = true;
    isolation_a.body = isolation_a_body;
    document.policies.push_back(isolation_a);

    PolicyDecl isolation_b;
    isolation_b.id = id_of<IdClass::Policy>("isolation-b");
    TenantIsolationPolicyDecl isolation_b_body;
    isolation_b_body.tenant = tenant_b.id;
    isolation_b_body.exclusive_scopes = {rack_b.id.str()};
    isolation_b_body.forbid_shared_links = true;
    isolation_b.body = isolation_b_body;
    document.policies.push_back(isolation_b);
  }

  if (options.include_workloads && options.include_tenants && options.leaves > 0 &&
      options.servers_per_leaf > 0) {
    WorkloadDecl workload;
    workload.id = id_of<IdClass::Workload>("tenant-app-a");
    workload.tenant = id_of<IdClass::Tenant>("app-a");
    workload.service_class = gold_id;
    for (std::size_t slot = 0; slot < options.servers_per_leaf; ++slot) {
      workload.placement.push_back(id_of<IdClass::Device>(server_name(0, slot)));
    }
    workload.bandwidth_bps = gbps(40);
    workload.replicas = static_cast<std::uint32_t>(std::min<std::size_t>(
        options.servers_per_leaf, static_cast<std::size_t>(2)));
    workload.redundancy_level = FailureDomainLevel::Device;
    workload.min_disjoint_domains = workload.replicas;
    workload.description = "synthetic tenant workload";
    document.workloads.push_back(workload);
  }

  const bool intents = options.include_intents || defect_needs_intents(options.defect);
  if (intents && options.leaves > 0) {
    IntentObjectDecl admin;
    admin.id = id_of<IdClass::IntentObject>("leaf0-admin");
    admin.subject = AnyId(id_of<IdClass::Device>(leaf_name(0)));
    admin.property = "admin_state";
    admin.desired = JsonValue::string("enabled");
    admin.mode = IntentMode::Enforce;
    admin.priority = 100;
    admin.description = "first leaf stays administratively enabled";
    document.intents.push_back(admin);

    IntentObjectDecl classes;
    classes.id = id_of<IdClass::IntentObject>("leaf0-classes");
    classes.subject = AnyId(id_of<IdClass::Device>(leaf_name(0)));
    classes.property = "service_classes";
    classes.desired = JsonValue::array({JsonValue::string(gold_id.str()),
                                        JsonValue::string(silver_id.str())});
    classes.mode = IntentMode::Enforce;
    classes.priority = 100;
    document.intents.push_back(classes);

    IntentObjectDecl upgrade;
    upgrade.id = id_of<IdClass::IntentObject>("rackh1-upgrade");
    upgrade.subject = AnyId(rack_hosts.id);
    upgrade.property = "upgrade_group";
    upgrade.desired = JsonValue::string("wave-1");
    upgrade.mode = IntentMode::Enforce;
    upgrade.priority = 50;
    document.intents.push_back(upgrade);

    IntentObjectDecl reserved;
    reserved.id = id_of<IdClass::IntentObject>("leaf1-reserved");
    reserved.subject = AnyId(id_of<IdClass::Device>(leaf_name(options.leaves > 1 ? 1 : 0)));
    reserved.property = "admin_state";
    reserved.desired = JsonValue::string("enabled");
    reserved.mode = IntentMode::Reserved;
    reserved.priority = 10;
    if (options.include_tenants) {
      reserved.tenant = id_of<IdClass::Tenant>("app-b").str();
    }
    document.intents.push_back(reserved);
  }

  // ---------------------------------------------------------------------
  // Defect injection. Exactly one defect per call.
  // ---------------------------------------------------------------------
  switch (options.defect) {
    case SynthDefect::None:
      break;
    case SynthDefect::DuplicateIdentity:
      if (!document.devices.empty()) {
        document.devices.push_back(document.devices.front());
      }
      break;
    case SynthDefect::DanglingReference:
      if (!document.devices.empty()) {
        document.devices.front().rack = id_of<IdClass::Rack>("missing-rack");
      }
      break;
    case SynthDefect::CyclicPolicyComposition: {
      PolicyDecl left;
      left.id = id_of<IdClass::Policy>("cycle-left");
      RoutingPolicyDecl left_body;
      left_body.action = RoutingAction::ShortestPath;
      left_body.max_ecmp_width = 1;
      left_body.extends_policy = id_of<IdClass::Policy>("cycle-right").str();
      left.body = left_body;
      PolicyDecl right;
      right.id = id_of<IdClass::Policy>("cycle-right");
      RoutingPolicyDecl right_body;
      right_body.action = RoutingAction::ShortestPath;
      right_body.max_ecmp_width = 1;
      right_body.extends_policy = id_of<IdClass::Policy>("cycle-left").str();
      right.body = right_body;
      document.policies.push_back(left);
      document.policies.push_back(right);
      break;
    }
    case SynthDefect::ImpossibleRedundancy: {
      PolicyDecl policy;
      policy.id = id_of<IdClass::Policy>("impossible-redundancy");
      RedundancyPolicyDecl body;
      body.scope = id_of<IdClass::Fabric>("core").str();
      body.level = FailureDomainLevel::Site;
      body.min_disjoint_paths = 9;
      policy.body = body;
      document.policies.push_back(policy);
      break;
    }
    case SynthDefect::UnknownCapability: {
      ServiceClassDecl entry;
      entry.id = id_of<IdClass::ServiceClass>("platinum");
      entry.display_name = "Platinum";
      entry.priority = 400;
      entry.required_capabilities = {"vendor.quantum-fabric"};
      document.service_classes.push_back(entry);
      break;
    }
    case SynthDefect::UnknownRequiredCapability: {
      ServiceClassDecl entry;
      entry.id = id_of<IdClass::ServiceClass>("bronze");
      entry.display_name = "Bronze";
      entry.priority = 50;
      entry.required_capabilities = {"qos.pfc"};
      document.service_classes.push_back(entry);
      ServiceClassId bronze = entry.id;
      for (auto& device : document.devices) {
        if (device.role == DeviceRole::Compute) {
          device.service_classes.push_back(bronze);
        }
      }
      break;
    }
    case SynthDefect::UnknownProperty:
      if (!document.intents.empty()) {
        document.intents.front().property = "vendor.magic-knob";
      }
      break;
    case SynthDefect::CapacityOvercommit: {
      PolicyDecl policy;
      policy.id = id_of<IdClass::Policy>("overcommit");
      CapacityPolicyDecl body;
      body.scope = id_of<IdClass::Link>("l0-s0").str();
      body.reserved_bps = gbps(900);
      policy.body = body;
      document.policies.push_back(policy);
      break;
    }
    case SynthDefect::RoutingDisagreement: {
      PolicyDecl policy;
      policy.id = id_of<IdClass::Policy>("routing-collision");
      RoutingPolicyDecl body;
      body.priority = 100;
      body.match_service_classes = {gold_id};
      body.action = RoutingAction::Drop;
      body.max_ecmp_width = 1;
      policy.body = body;
      document.policies.push_back(policy);
      break;
    }
    case SynthDefect::AdminStateDisagreement: {
      PolicyDecl fabric_policy;
      fabric_policy.id = id_of<IdClass::Policy>("disable-fabric");
      AdminStatePolicyDecl fabric_body;
      fabric_body.scope = fabric.id.str();
      fabric_body.state = AdminState::Disabled;
      fabric_policy.body = fabric_body;
      document.policies.push_back(fabric_policy);

      PolicyDecl rack_policy;
      rack_policy.id = id_of<IdClass::Policy>("enable-rack-a1");
      AdminStatePolicyDecl rack_body;
      rack_body.scope = rack_a.id.str();
      rack_body.state = AdminState::Enabled;
      rack_body.drain_first = true;
      rack_policy.body = rack_body;
      document.policies.push_back(rack_policy);
      break;
    }
    case SynthDefect::PortIndexOutOfRange:
      if (!document.ports.empty()) {
        document.ports.front().index = 4096;
      }
      break;
    case SynthDefect::UnknownModel:
      if (!document.devices.empty()) {
        document.devices.front().model = "acme.unknown-platform";
      }
      break;
    case SynthDefect::UnsupportedMinor:
      document.schema = SchemaVersion::make(SchemaVersion::kSupportedMajor,
                                            SchemaVersion::kSupportedMinor + 7)
                            .value();
      break;
    case SynthDefect::UnknownFeature:
      document.required_features.push_back("vendor.quantum-teleport");
      break;
    case SynthDefect::SelfLoopLink:
      if (!document.links.empty()) {
        document.links.front().endpoint_b = document.links.front().endpoint_a;
      }
      break;
    case SynthDefect::DisabledLinkWithEnabledPort:
      if (!document.links.empty()) {
        document.links.front().admin = AdminState::Enabled;
        const std::string endpoint = document.links.front().endpoint_a.str();
        for (auto& port : document.ports) {
          if (port.id.str() == endpoint) {
            port.admin = AdminState::Disabled;
          }
        }
      }
      break;
    case SynthDefect::EmptyCapacity:
      if (!document.links.empty()) {
        document.links.front().capacity_bps = 0;
      }
      break;
  }
  return document;
}

}  // namespace ifabric
