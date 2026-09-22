// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/canonical.hpp"

#include <algorithm>

namespace ifabric {

namespace {

// Deterministic tie-break for elements that share an identity: compare their
// canonical serialization. Duplicate identities therefore still normalize to a
// stable order (and are then reported as DuplicateIdentity by validation).
template <class T, class KeyFn>
void sort_by(JsonValue (*serialize)(const T&), std::vector<T>& items, KeyFn key) {
  std::stable_sort(items.begin(), items.end(), [&](const T& a, const T& b) {
    const auto ka = key(a);
    const auto kb = key(b);
    if (ka != kb) {
      return ka < kb;
    }
    return serialize(a).dump() < serialize(b).dump();
  });
}

template <class T, class KeyFn>
void sort_plain(std::vector<T>& items, KeyFn key) {
  std::stable_sort(items.begin(), items.end(),
                   [&](const T& a, const T& b) { return key(a) < key(b); });
}

std::string trimmed(std::string text) { return std::string(trim_ascii(text)); }

template <class K>
void normalize_id_list(std::vector<K>& ids) {
  std::stable_sort(ids.begin(), ids.end(),
                   [](const K& a, const K& b) { return a.str() < b.str(); });
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

void normalize_string_set(std::vector<std::string>& items) {
  std::stable_sort(items.begin(), items.end());
  items.erase(std::unique(items.begin(), items.end()), items.end());
}

// Serialization shims used only for the tie-break above.
JsonValue ser_fabric(const FabricDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("display_name", JsonValue::string(v.display_name));
  (void)o.set("description", JsonValue::string(v.description));
  return o;
}
JsonValue ser_site(const SiteDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("fabric", JsonValue::string(v.fabric.str()));
  (void)o.set("display_name", JsonValue::string(v.display_name));
  (void)o.set("region", JsonValue::string(v.region));
  (void)o.set("description", JsonValue::string(v.description));
  return o;
}
JsonValue ser_pod(const PodDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("site", JsonValue::string(v.site.str()));
  (void)o.set("display_name", JsonValue::string(v.display_name));
  (void)o.set("description", JsonValue::string(v.description));
  return o;
}
JsonValue ser_rack(const RackDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("pod", JsonValue::string(v.pod.str()));
  (void)o.set("position", JsonValue::integer(static_cast<std::int64_t>(v.position)));
  (void)o.set("description", JsonValue::string(v.description));
  return o;
}
JsonValue ser_device(const DeviceDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("site", JsonValue::string(v.site.str()));
  (void)o.set("pod", JsonValue::string(v.pod.str()));
  (void)o.set("rack", JsonValue::string(v.rack.str()));
  (void)o.set("role", JsonValue::string(std::string(to_string(v.role))));
  (void)o.set("model", JsonValue::string(v.model));
  (void)o.set("admin", JsonValue::string(std::string(to_string(v.admin))));
  (void)o.set("declared_port_count", JsonValue::integer(static_cast<std::int64_t>(v.declared_port_count)));
  JsonArray classes;
  for (const auto& id : v.service_classes) {
    classes.push_back(JsonValue::string(id.str()));
  }
  (void)o.set("service_classes", JsonValue::array(std::move(classes)));
  (void)o.set("meta", meta_to_json(v.meta));
  (void)o.set("description", JsonValue::string(v.description));
  return o;
}
JsonValue ser_port(const PortDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("device", JsonValue::string(v.device.str()));
  (void)o.set("index", JsonValue::integer(static_cast<std::int64_t>(v.index)));
  (void)o.set("role", JsonValue::string(std::string(to_string(v.role))));
  (void)o.set("admin", JsonValue::string(std::string(to_string(v.admin))));
  (void)o.set("speed_bps", JsonValue::integer(static_cast<std::int64_t>(v.speed_bps)));
  (void)o.set("media", JsonValue::string(v.media));
  JsonArray classes;
  for (const auto& id : v.service_classes) {
    classes.push_back(JsonValue::string(id.str()));
  }
  (void)o.set("service_classes", JsonValue::array(std::move(classes)));
  (void)o.set("description", JsonValue::string(v.description));
  return o;
}
JsonValue ser_link(const LinkDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("endpoint_a", JsonValue::string(v.endpoint_a.str()));
  (void)o.set("endpoint_b", JsonValue::string(v.endpoint_b.str()));
  (void)o.set("kind", JsonValue::string(std::string(to_string(v.kind))));
  (void)o.set("capacity_bps", JsonValue::integer(static_cast<std::int64_t>(v.capacity_bps)));
  (void)o.set("admin", JsonValue::string(std::string(to_string(v.admin))));
  JsonArray classes;
  for (const auto& id : v.service_classes) {
    classes.push_back(JsonValue::string(id.str()));
  }
  (void)o.set("service_classes", JsonValue::array(std::move(classes)));
  (void)o.set("description", JsonValue::string(v.description));
  return o;
}
JsonValue ser_capability(const CapabilityDecl& v) {
  JsonValue o;
  (void)o.set("model", JsonValue::string(v.model));
  JsonArray caps;
  for (const auto& cap : v.capabilities) {
    caps.push_back(JsonValue::string(cap));
  }
  (void)o.set("capabilities", JsonValue::array(std::move(caps)));
  return o;
}
JsonValue ser_service_class(const ServiceClassDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("display_name", JsonValue::string(v.display_name));
  (void)o.set("priority", JsonValue::integer(static_cast<std::int64_t>(v.priority)));
  return o;
}
JsonValue ser_policy(const PolicyDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("kind", JsonValue::string(std::string(to_string(policy_kind(v)))));
  return o;
}
JsonValue ser_tenant(const TenantDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("isolation_id", JsonValue::integer(static_cast<std::int64_t>(v.isolation_id)));
  return o;
}
JsonValue ser_workload(const WorkloadDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("tenant", JsonValue::string(v.tenant.str()));
  (void)o.set("service_class", JsonValue::string(v.service_class.str()));
  return o;
}
JsonValue ser_intent(const IntentObjectDecl& v) {
  JsonValue o;
  (void)o.set("id", JsonValue::string(v.id.str()));
  (void)o.set("subject", JsonValue::string(v.subject.str()));
  (void)o.set("property", JsonValue::string(v.property));
  (void)o.set("desired", v.desired);
  return o;
}

}  // namespace

IntentDocument normalize(IntentDocument document) {
  for (auto& entry : document.fabrics) {
    entry.display_name = trimmed(entry.display_name);
    entry.description = trimmed(entry.description);
  }
  for (auto& entry : document.sites) {
    entry.display_name = trimmed(entry.display_name);
    entry.region = trimmed(entry.region);
    entry.description = trimmed(entry.description);
  }
  for (auto& entry : document.pods) {
    entry.display_name = trimmed(entry.display_name);
    entry.description = trimmed(entry.description);
  }
  for (auto& entry : document.racks) {
    entry.description = trimmed(entry.description);
  }
  for (auto& entry : document.devices) {
    entry.model = trimmed(entry.model);
    entry.description = trimmed(entry.description);
    normalize_id_list(entry.service_classes);
  }
  for (auto& entry : document.ports) {
    entry.media = trimmed(entry.media);
    entry.description = trimmed(entry.description);
    normalize_id_list(entry.service_classes);
  }
  for (auto& entry : document.links) {
    entry.description = trimmed(entry.description);
    normalize_id_list(entry.service_classes);
  }
  for (auto& entry : document.capability_catalog) {
    entry.model = trimmed(entry.model);
    normalize_string_set(entry.capabilities);
    sort_plain(entry.supported_roles,
               [](DeviceRole role) { return static_cast<int>(role); });
    entry.supported_roles.erase(
        std::unique(entry.supported_roles.begin(), entry.supported_roles.end()),
        entry.supported_roles.end());
  }
  for (auto& entry : document.service_classes) {
    entry.display_name = trimmed(entry.display_name);
    entry.description = trimmed(entry.description);
    normalize_string_set(entry.required_capabilities);
  }
  for (auto& entry : document.policies) {
    if (auto* routing = std::get_if<RoutingPolicyDecl>(&entry.body)) {
      normalize_id_list(routing->match_service_classes);
      normalize_id_list(routing->match_tenants);
      routing->description = trimmed(routing->description);
    } else if (auto* capacity = std::get_if<CapacityPolicyDecl>(&entry.body)) {
      normalize_id_list(capacity->service_classes);
      capacity->description = trimmed(capacity->description);
    } else if (auto* admin = std::get_if<AdminStatePolicyDecl>(&entry.body)) {
      admin->reason = trimmed(admin->reason);
      admin->description = trimmed(admin->description);
    } else if (auto* redundancy = std::get_if<RedundancyPolicyDecl>(&entry.body)) {
      normalize_id_list(redundancy->service_classes);
      redundancy->description = trimmed(redundancy->description);
    } else if (auto* maintenance = std::get_if<MaintenanceEligibilityPolicyDecl>(&entry.body)) {
      normalize_string_set(maintenance->upgrade_groups);
      normalize_string_set(maintenance->allowed_impact);
      maintenance->description = trimmed(maintenance->description);
    } else if (auto* isolation = std::get_if<TenantIsolationPolicyDecl>(&entry.body)) {
      normalize_string_set(isolation->exclusive_scopes);
      isolation->description = trimmed(isolation->description);
    }
  }
  for (auto& entry : document.tenants) {
    entry.display_name = trimmed(entry.display_name);
    entry.description = trimmed(entry.description);
  }
  for (auto& entry : document.workloads) {
    normalize_id_list(entry.placement);
    normalize_id_list(entry.allowed_sites);
    entry.description = trimmed(entry.description);
  }
  for (auto& entry : document.intents) {
    entry.property = trimmed(entry.property);
    entry.description = trimmed(entry.description);
  }
  document.required_features = [&] {
    std::vector<std::string> items = document.required_features;
    for (auto& item : items) {
      item = trimmed(item);
    }
    normalize_string_set(items);
    return items;
  }();
  document.optional_features = [&] {
    std::vector<std::string> items = document.optional_features;
    for (auto& item : items) {
      item = trimmed(item);
    }
    normalize_string_set(items);
    return items;
  }();
  document.provenance.description = trimmed(document.provenance.description);

  sort_by(ser_fabric, document.fabrics, [](const FabricDecl& v) { return v.id.local(); });
  sort_by(ser_site, document.sites, [](const SiteDecl& v) { return v.id.local(); });
  sort_by(ser_pod, document.pods, [](const PodDecl& v) { return v.id.local(); });
  sort_by(ser_rack, document.racks, [](const RackDecl& v) { return v.id.local(); });
  sort_by(ser_device, document.devices, [](const DeviceDecl& v) { return v.id.local(); });
  sort_by(ser_port, document.ports, [](const PortDecl& v) { return v.id.local(); });
  sort_by(ser_link, document.links, [](const LinkDecl& v) { return v.id.local(); });
  sort_by(ser_capability, document.capability_catalog,
          [](const CapabilityDecl& v) { return v.model; });
  sort_by(ser_service_class, document.service_classes,
          [](const ServiceClassDecl& v) { return v.id.local(); });
  sort_by(ser_policy, document.policies, [](const PolicyDecl& v) { return v.id.local(); });
  sort_by(ser_tenant, document.tenants, [](const TenantDecl& v) { return v.id.local(); });
  sort_by(ser_workload, document.workloads, [](const WorkloadDecl& v) { return v.id.local(); });
  sort_by(ser_intent, document.intents, [](const IntentObjectDecl& v) { return v.id.local(); });

  return document;
}

Result<CanonicalForm> canonicalize(const IntentDocument& document) {
  CanonicalForm form;
  form.normalized = normalize(document);
  if (form.normalized.declared_object_count() > kMaxObjectsPerDocument) {
    return Error(ErrorCode::LimitExceeded, "document declares too many objects",
                 std::to_string(form.normalized.declared_object_count()));
  }
  form.document_json = to_json(form.normalized);
  form.content_json = content_payload(form.normalized);
  form.document_digest = DocumentDigest::of(form.document_json.dump());
  form.content_digest = ContentDigest::of(form.content_json.dump());
  return form;
}

ContentDigest content_digest_of(const IntentDocument& document) {
  return ContentDigest::of(content_payload(normalize(document)).dump());
}

DocumentDigest document_digest_of(const IntentDocument& document) {
  return DocumentDigest::of(to_json(normalize(document)).dump());
}

}  // namespace ifabric
