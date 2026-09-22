// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/index.hpp"

#include <algorithm>
#include <map>

namespace ifabric {

namespace {

template <class T, class KeyFn>
void build_map(std::unordered_map<std::string, std::size_t>& out, const std::vector<T>& items,
               KeyFn key) {
  out.reserve(items.size() * 2 + 1);
  for (std::size_t i = 0; i < items.size(); ++i) {
    out.emplace(key(items[i]), i);
  }
}

template <class T, class KeyFn>
void collect_duplicates(const std::vector<T>& items, KeyFn key, std::string_view prefix,
                        std::vector<DuplicateIdentity>& out) {
  std::map<std::string, std::pair<std::size_t, std::size_t>> seen;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const std::string id = key(items[i]);
    auto it = seen.find(id);
    if (it == seen.end()) {
      seen.emplace(id, std::make_pair(i, std::size_t{1}));
      continue;
    }
    it->second.second += 1;
  }
  for (const auto& entry : seen) {
    if (entry.second.second <= 1) {
      continue;
    }
    DuplicateIdentity duplicate;
    const auto parsed = AnyId::parse(entry.first);
    if (!parsed) {
      continue;
    }
    duplicate.id = parsed.value();
    duplicate.count = entry.second.second;
    duplicate.first_path = std::string(prefix) + "[" + entry.first + "]";
    duplicate.second_path =
        std::string(prefix) + "[" + entry.first + "] (declared " +
        std::to_string(entry.second.second) + " times)";
    out.push_back(std::move(duplicate));
  }
}

}  // namespace

TopologyIndex::TopologyIndex(const IntentDocument& document) : document_(&document) {
  build_map(fabrics_, document.fabrics, [](const FabricDecl& v) { return v.id.str(); });
  build_map(sites_, document.sites, [](const SiteDecl& v) { return v.id.str(); });
  build_map(pods_, document.pods, [](const PodDecl& v) { return v.id.str(); });
  build_map(racks_, document.racks, [](const RackDecl& v) { return v.id.str(); });
  build_map(devices_, document.devices, [](const DeviceDecl& v) { return v.id.str(); });
  build_map(ports_, document.ports, [](const PortDecl& v) { return v.id.str(); });
  build_map(links_, document.links, [](const LinkDecl& v) { return v.id.str(); });
  build_map(capabilities_, document.capability_catalog,
            [](const CapabilityDecl& v) { return v.model; });
  build_map(service_classes_, document.service_classes,
            [](const ServiceClassDecl& v) { return v.id.str(); });
  build_map(policies_, document.policies, [](const PolicyDecl& v) { return v.id.str(); });
  build_map(tenants_, document.tenants, [](const TenantDecl& v) { return v.id.str(); });
  build_map(workloads_, document.workloads, [](const WorkloadDecl& v) { return v.id.str(); });

  for (std::size_t i = 0; i < document.ports.size(); ++i) {
    ports_by_device_[document.ports[i].device.str()].push_back(i);
  }
  for (std::size_t i = 0; i < document.links.size(); ++i) {
    const LinkDecl& link = document.links[i];
    const PortDecl* a = port(link.endpoint_a);
    const PortDecl* b = port(link.endpoint_b);
    if (a != nullptr) {
      links_by_device_[a->device.str()].push_back(i);
      link_by_port_.emplace(a->id.str(), i);
    }
    if (b != nullptr) {
      links_by_device_[b->device.str()].push_back(i);
      link_by_port_.emplace(b->id.str(), i);
    }
  }
  for (auto& entry : ports_by_device_) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  for (auto& entry : links_by_device_) {
    std::sort(entry.second.begin(), entry.second.end());
    entry.second.erase(std::unique(entry.second.begin(), entry.second.end()), entry.second.end());
  }

  collect_duplicates(document.fabrics, [](const FabricDecl& v) { return v.id.str(); },
                     "topology.fabrics", duplicates_);
  collect_duplicates(document.sites, [](const SiteDecl& v) { return v.id.str(); },
                     "topology.sites", duplicates_);
  collect_duplicates(document.pods, [](const PodDecl& v) { return v.id.str(); }, "topology.pods",
                     duplicates_);
  collect_duplicates(document.racks, [](const RackDecl& v) { return v.id.str(); }, "topology.racks",
                     duplicates_);
  collect_duplicates(document.devices, [](const DeviceDecl& v) { return v.id.str(); },
                     "topology.devices", duplicates_);
  collect_duplicates(document.ports, [](const PortDecl& v) { return v.id.str(); }, "topology.ports",
                     duplicates_);
  collect_duplicates(document.links, [](const LinkDecl& v) { return v.id.str(); }, "topology.links",
                     duplicates_);
  collect_duplicates(document.capability_catalog, [](const CapabilityDecl& v) { return v.model; },
                     "capability_catalog", duplicates_);
  collect_duplicates(document.service_classes, [](const ServiceClassDecl& v) { return v.id.str(); },
                     "service_classes", duplicates_);
  collect_duplicates(document.policies, [](const PolicyDecl& v) { return v.id.str(); }, "policies",
                     duplicates_);
  collect_duplicates(document.tenants, [](const TenantDecl& v) { return v.id.str(); }, "tenants",
                     duplicates_);
  collect_duplicates(document.workloads, [](const WorkloadDecl& v) { return v.id.str(); },
                     "workloads", duplicates_);
  collect_duplicates(document.intents, [](const IntentObjectDecl& v) { return v.id.str(); },
                     "intents", duplicates_);
  std::sort(duplicates_.begin(), duplicates_.end(),
            [](const DuplicateIdentity& a, const DuplicateIdentity& b) { return a.id < b.id; });
}

const FabricDecl* TopologyIndex::fabric(const FabricId& id) const {
  const auto it = fabrics_.find(id.str());
  return it == fabrics_.end() ? nullptr : &document_->fabrics[it->second];
}
const SiteDecl* TopologyIndex::site(const SiteId& id) const {
  const auto it = sites_.find(id.str());
  return it == sites_.end() ? nullptr : &document_->sites[it->second];
}
const PodDecl* TopologyIndex::pod(const PodId& id) const {
  const auto it = pods_.find(id.str());
  return it == pods_.end() ? nullptr : &document_->pods[it->second];
}
const RackDecl* TopologyIndex::rack(const RackId& id) const {
  const auto it = racks_.find(id.str());
  return it == racks_.end() ? nullptr : &document_->racks[it->second];
}
const DeviceDecl* TopologyIndex::device(const DeviceId& id) const {
  const auto it = devices_.find(id.str());
  return it == devices_.end() ? nullptr : &document_->devices[it->second];
}
const PortDecl* TopologyIndex::port(const PortId& id) const {
  const auto it = ports_.find(id.str());
  return it == ports_.end() ? nullptr : &document_->ports[it->second];
}
const LinkDecl* TopologyIndex::link(const LinkId& id) const {
  const auto it = links_.find(id.str());
  return it == links_.end() ? nullptr : &document_->links[it->second];
}
const CapabilityDecl* TopologyIndex::capability_model(const std::string& model) const {
  const auto it = capabilities_.find(model);
  return it == capabilities_.end() ? nullptr : &document_->capability_catalog[it->second];
}
const ServiceClassDecl* TopologyIndex::service_class(const ServiceClassId& id) const {
  const auto it = service_classes_.find(id.str());
  return it == service_classes_.end() ? nullptr : &document_->service_classes[it->second];
}
const PolicyDecl* TopologyIndex::policy(const PolicyId& id) const {
  const auto it = policies_.find(id.str());
  return it == policies_.end() ? nullptr : &document_->policies[it->second];
}
const TenantDecl* TopologyIndex::tenant(const TenantId& id) const {
  const auto it = tenants_.find(id.str());
  return it == tenants_.end() ? nullptr : &document_->tenants[it->second];
}
const WorkloadDecl* TopologyIndex::workload(const WorkloadId& id) const {
  const auto it = workloads_.find(id.str());
  return it == workloads_.end() ? nullptr : &document_->workloads[it->second];
}

bool TopologyIndex::resolves(const AnyId& id) const {
  switch (id.klass()) {
    case IdClass::Fabric: return fabrics_.count(id.str()) != 0;
    case IdClass::Site: return sites_.count(id.str()) != 0;
    case IdClass::Pod: return pods_.count(id.str()) != 0;
    case IdClass::Rack: return racks_.count(id.str()) != 0;
    case IdClass::Device: return devices_.count(id.str()) != 0;
    case IdClass::Port: return ports_.count(id.str()) != 0;
    case IdClass::Link: return links_.count(id.str()) != 0;
    case IdClass::ServiceClass: return service_classes_.count(id.str()) != 0;
    case IdClass::Policy: return policies_.count(id.str()) != 0;
    case IdClass::Tenant: return tenants_.count(id.str()) != 0;
    case IdClass::Workload: return workloads_.count(id.str()) != 0;
    case IdClass::IntentObject: return true;
    case IdClass::Domain:
      return !document_->domain.empty() && document_->domain.str() == id.str();
    case IdClass::Conflict:
    case IdClass::Actor:
    case IdClass::Proposal:
    case IdClass::Generation:
    case IdClass::Count: return false;
  }
  return false;
}

std::string TopologyIndex::describe(const AnyId& id) const {
  if (!resolves(id)) {
    return id.str() + " (unresolved " + std::string(id_class_name(id.klass())) + ")";
  }
  return id.str();
}

std::vector<const DeviceDecl*> TopologyIndex::devices_in_site(const SiteId& id) const {
  std::vector<const DeviceDecl*> out;
  for (const auto& device : document_->devices) {
    if (device.site == id) {
      out.push_back(&device);
    }
  }
  return out;
}

std::vector<const DeviceDecl*> TopologyIndex::devices_in_pod(const PodId& id) const {
  std::vector<const DeviceDecl*> out;
  for (const auto& device : document_->devices) {
    if (device.pod == id) {
      out.push_back(&device);
    }
  }
  return out;
}

std::vector<const DeviceDecl*> TopologyIndex::devices_in_rack(const RackId& id) const {
  std::vector<const DeviceDecl*> out;
  for (const auto& device : document_->devices) {
    if (device.rack == id) {
      out.push_back(&device);
    }
  }
  return out;
}

std::vector<const PortDecl*> TopologyIndex::ports_of_device(const DeviceId& id) const {
  std::vector<const PortDecl*> out;
  const auto it = ports_by_device_.find(id.str());
  if (it == ports_by_device_.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (std::size_t index : it->second) {
    out.push_back(&document_->ports[index]);
  }
  return out;
}

std::vector<const LinkDecl*> TopologyIndex::links_of_device(const DeviceId& id) const {
  std::vector<const LinkDecl*> out;
  const auto it = links_by_device_.find(id.str());
  if (it == links_by_device_.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (std::size_t index : it->second) {
    out.push_back(&document_->links[index]);
  }
  return out;
}

const LinkDecl* TopologyIndex::link_of_port(const PortId& id) const {
  const auto it = link_by_port_.find(id.str());
  return it == link_by_port_.end() ? nullptr : &document_->links[it->second];
}

bool TopologyIndex::is_scope_capable(const AnyId& scope) const {
  switch (scope.klass()) {
    case IdClass::Fabric:
    case IdClass::Site:
    case IdClass::Pod:
    case IdClass::Rack:
    case IdClass::Device:
    case IdClass::Link:
    case IdClass::Port:
    case IdClass::ServiceClass:
    case IdClass::Tenant: return true;
    default: return false;
  }
}

bool TopologyIndex::devices_of_scope(const AnyId& scope, std::vector<const DeviceDecl*>& out) const {
  out.clear();
  switch (scope.klass()) {
    case IdClass::Fabric: {
      const auto parsed = scope.as<IdClass::Fabric>();
      if (!parsed) {
        return false;
      }
      for (const auto& site : document_->sites) {
        if (site.fabric == parsed.value()) {
          for (const auto* device : devices_in_site(site.id)) {
            out.push_back(device);
          }
        }
      }
      return true;
    }
    case IdClass::Site: {
      const auto parsed = scope.as<IdClass::Site>();
      if (!parsed) {
        return false;
      }
      out = devices_in_site(parsed.value());
      return true;
    }
    case IdClass::Pod: {
      const auto parsed = scope.as<IdClass::Pod>();
      if (!parsed) {
        return false;
      }
      out = devices_in_pod(parsed.value());
      return true;
    }
    case IdClass::Rack: {
      const auto parsed = scope.as<IdClass::Rack>();
      if (!parsed) {
        return false;
      }
      out = devices_in_rack(parsed.value());
      return true;
    }
    case IdClass::Device: {
      const auto parsed = scope.as<IdClass::Device>();
      if (!parsed) {
        return false;
      }
      if (const DeviceDecl* device = this->device(parsed.value())) {
        out.push_back(device);
      }
      return true;
    }
    case IdClass::Port: {
      const auto parsed = scope.as<IdClass::Port>();
      if (!parsed) {
        return false;
      }
      if (const PortDecl* port_entry = port(parsed.value())) {
        if (const DeviceDecl* device_entry = device(port_entry->device)) {
          out.push_back(device_entry);
        }
      }
      return true;
    }
    case IdClass::Link: {
      const auto parsed = scope.as<IdClass::Link>();
      if (!parsed) {
        return false;
      }
      if (const LinkDecl* link_entry = link(parsed.value())) {
        if (const PortDecl* a = port(link_entry->endpoint_a)) {
          if (const DeviceDecl* device_entry = device(a->device)) {
            out.push_back(device_entry);
          }
        }
        if (const PortDecl* b = port(link_entry->endpoint_b)) {
          if (const DeviceDecl* device_entry = device(b->device)) {
            if (out.empty() || out.front()->id != device_entry->id) {
              out.push_back(device_entry);
            }
          }
        }
      }
      return true;
    }
    case IdClass::ServiceClass: {
      const auto parsed = scope.as<IdClass::ServiceClass>();
      if (!parsed) {
        return false;
      }
      for (const auto& device_entry : document_->devices) {
        if (std::find(device_entry.service_classes.begin(), device_entry.service_classes.end(),
                      parsed.value()) != device_entry.service_classes.end()) {
          out.push_back(&device_entry);
        }
      }
      return true;
    }
    case IdClass::Tenant: {
      const auto parsed = scope.as<IdClass::Tenant>();
      if (!parsed) {
        return false;
      }
      for (const auto& workload : document_->workloads) {
        if (workload.tenant != parsed.value()) {
          continue;
        }
        for (const auto& device_id : workload.placement) {
          if (const DeviceDecl* device_entry = this->device(device_id)) {
            out.push_back(device_entry);
          }
        }
      }
      std::sort(out.begin(), out.end(),
                [](const DeviceDecl* a, const DeviceDecl* b) { return a->id < b->id; });
      out.erase(std::unique(out.begin(), out.end()), out.end());
      return true;
    }
    default: return false;
  }
}

bool TopologyIndex::links_of_scope(const AnyId& scope, std::vector<const LinkDecl*>& out) const {
  out.clear();
  std::vector<const DeviceDecl*> devices;
  if (!devices_of_scope(scope, devices)) {
    if (scope.klass() == IdClass::Link) {
      const auto parsed = scope.as<IdClass::Link>();
      if (parsed) {
        if (const LinkDecl* entry = link(parsed.value())) {
          out.push_back(entry);
        }
        return true;
      }
    }
    return false;
  }
  if (scope.klass() == IdClass::Link) {
    const auto parsed = scope.as<IdClass::Link>();
    if (parsed) {
      if (const LinkDecl* entry = link(parsed.value())) {
        out.push_back(entry);
      }
      return true;
    }
  }
  for (const DeviceDecl* device : devices) {
    for (const LinkDecl* entry : links_of_device(device->id)) {
      out.push_back(entry);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const LinkDecl* a, const LinkDecl* b) { return a->id < b->id; });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return true;
}

std::string TopologyIndex::failure_domain_key(const DeviceDecl& device,
                                              FailureDomainLevel level) const {
  switch (level) {
    case FailureDomainLevel::Link:
    case FailureDomainLevel::Port:
    case FailureDomainLevel::Device: return device.id.str();
    case FailureDomainLevel::Rack:
      return device.rack.empty() ? device.id.str() + "#norack" : device.rack.str();
    case FailureDomainLevel::Pod:
      return device.pod.empty() ? device.id.str() + "#nopod" : device.pod.str();
    case FailureDomainLevel::Site: return device.site.str();
    case FailureDomainLevel::Fabric: {
      const SiteDecl* site_entry = site(device.site);
      return site_entry == nullptr ? std::string("#unknown-fabric") : site_entry->fabric.str();
    }
  }
  return device.id.str();
}

// ---------------------------------------------------------------------------
// Reference graph and cycle detection
// ---------------------------------------------------------------------------
namespace {

struct Graph {
  std::vector<AnyId> nodes;
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::size_t>> edges;

  std::size_t intern(const AnyId& id) {
    const auto it = index.find(id.str());
    if (it != index.end()) {
      return it->second;
    }
    const std::size_t position = nodes.size();
    index.emplace(id.str(), position);
    nodes.push_back(id);
    edges.emplace_back();
    return position;
  }
};

void add_edge(Graph& graph, const AnyId& from, const AnyId& to) {
  const std::size_t a = graph.intern(from);
  const std::size_t b = graph.intern(to);
  graph.edges[a].push_back(b);
}

}  // namespace

std::vector<ReferenceCycle> detect_reference_cycles(const IntentDocument& document) {
  Graph graph;
  const auto as_any = [](const auto& id) { return AnyId(id); };

  for (const auto& device : document.devices) {
    if (!device.rack.empty()) {
      add_edge(graph, as_any(device.id), as_any(device.rack));
    }
    if (!device.pod.empty()) {
      add_edge(graph, as_any(device.id), as_any(device.pod));
    }
    if (!device.site.empty()) {
      add_edge(graph, as_any(device.id), as_any(device.site));
    }
  }
  for (const auto& rack : document.racks) {
    if (!rack.pod.empty()) {
      add_edge(graph, as_any(rack.id), as_any(rack.pod));
    }
  }
  for (const auto& pod : document.pods) {
    if (!pod.site.empty()) {
      add_edge(graph, as_any(pod.id), as_any(pod.site));
    }
  }
  for (const auto& site : document.sites) {
    if (!site.fabric.empty()) {
      add_edge(graph, as_any(site.id), as_any(site.fabric));
    }
  }
  for (const auto& port : document.ports) {
    if (!port.device.empty()) {
      add_edge(graph, as_any(port.id), as_any(port.device));
    }
  }
  for (const auto& link : document.links) {
    if (!link.endpoint_a.empty()) {
      add_edge(graph, as_any(link.id), as_any(link.endpoint_a));
    }
    if (!link.endpoint_b.empty()) {
      add_edge(graph, as_any(link.id), as_any(link.endpoint_b));
    }
  }
  for (const auto& workload : document.workloads) {
    if (!workload.tenant.empty()) {
      add_edge(graph, as_any(workload.id), as_any(workload.tenant));
    }
    for (const auto& device : workload.placement) {
      add_edge(graph, as_any(workload.id), as_any(device));
    }
  }
  for (const auto& policy : document.policies) {
    if (const auto* routing = std::get_if<RoutingPolicyDecl>(&policy.body)) {
      if (!routing->extends_policy.empty()) {
        const auto target = AnyId::parse(routing->extends_policy);
        if (target) {
          add_edge(graph, as_any(policy.id), target.value());
        }
      }
    }
    if (const auto* isolation = std::get_if<TenantIsolationPolicyDecl>(&policy.body)) {
      if (!isolation->tenant.empty()) {
        add_edge(graph, as_any(policy.id), as_any(isolation->tenant));
      }
      for (const auto& scope : isolation->exclusive_scopes) {
        const auto target = AnyId::parse(scope);
        if (target) {
          add_edge(graph, as_any(policy.id), target.value());
        }
      }
    }
  }
  for (const auto& intent : document.intents) {
    if (!intent.subject.empty()) {
      add_edge(graph, as_any(intent.id), intent.subject);
    }
    if (!intent.tenant.empty()) {
      const auto target = AnyId::parse(intent.tenant);
      if (target) {
        add_edge(graph, as_any(intent.id), target.value());
      }
    }
  }

  for (auto& list : graph.edges) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  }

  // Iterative Tarjan strongly-connected-components; any component with more
  // than one member, or a self loop, is a reference cycle.
  const std::size_t n = graph.nodes.size();
  std::vector<std::size_t> index_of(n, std::size_t(-1));
  std::vector<std::size_t> low(n, 0);
  std::vector<bool> on_stack(n, false);
  std::vector<std::size_t> stack;
  std::vector<std::vector<std::size_t>> components;
  std::size_t next_index = 0;

  struct Frame {
    std::size_t node;
    std::size_t cursor;
  };

  for (std::size_t root = 0; root < n; ++root) {
    if (index_of[root] != std::size_t(-1)) {
      continue;
    }
    std::vector<Frame> frames;
    frames.push_back(Frame{root, 0});
    index_of[root] = next_index;
    low[root] = next_index;
    ++next_index;
    stack.push_back(root);
    on_stack[root] = true;
    while (!frames.empty()) {
      Frame& frame = frames.back();
      if (frame.cursor < graph.edges[frame.node].size()) {
        const std::size_t next = graph.edges[frame.node][frame.cursor++];
        if (index_of[next] == std::size_t(-1)) {
          index_of[next] = next_index;
          low[next] = next_index;
          ++next_index;
          stack.push_back(next);
          on_stack[next] = true;
          frames.push_back(Frame{next, 0});
        } else if (on_stack[next]) {
          low[frame.node] = std::min(low[frame.node], index_of[next]);
        }
        continue;
      }
      const std::size_t node = frame.node;
      frames.pop_back();
      if (low[node] == index_of[node]) {
        std::vector<std::size_t> component;
        for (;;) {
          const std::size_t member = stack.back();
          stack.pop_back();
          on_stack[member] = false;
          component.push_back(member);
          if (member == node) {
            break;
          }
        }
        components.push_back(std::move(component));
      }
      if (!frames.empty()) {
        low[frames.back().node] = std::min(low[frames.back().node], low[node]);
      }
    }
  }

  std::vector<ReferenceCycle> cycles;
  for (const auto& component : components) {
    bool self_loop = false;
    if (component.size() == 1) {
      const std::size_t node = component.front();
      for (std::size_t next : graph.edges[node]) {
        if (next == node) {
          self_loop = true;
          break;
        }
      }
      if (!self_loop) {
        continue;
      }
    }
    ReferenceCycle cycle;
    cycle.members.reserve(component.size());
    for (std::size_t member : component) {
      cycle.members.push_back(graph.nodes[member]);
    }
    std::sort(cycle.members.begin(), cycle.members.end());
    cycles.push_back(std::move(cycle));
  }
  std::sort(cycles.begin(), cycles.end(), [](const ReferenceCycle& a, const ReferenceCycle& b) {
    const std::size_t count = std::min(a.members.size(), b.members.size());
    for (std::size_t i = 0; i < count; ++i) {
      if (a.members[i] != b.members[i]) {
        return a.members[i] < b.members[i];
      }
    }
    return a.members.size() < b.members.size();
  });
  return cycles;
}

}  // namespace ifabric
