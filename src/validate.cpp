// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/validate.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace ifabric {

std::string_view to_string(ValidationLayer value) noexcept {
  switch (value) {
    case ValidationLayer::SchemaCompatibility: return "schema-compatibility";
    case ValidationLayer::Syntax: return "syntax";
    case ValidationLayer::ReferentialIntegrity: return "referential-integrity";
    case ValidationLayer::CapabilityCompatibility: return "capability-compatibility";
    case ValidationLayer::TopologyFeasibility: return "topology-feasibility";
    case ValidationLayer::PolicyConflict: return "policy-conflict";
    case ValidationLayer::InvariantViolation: return "invariant-violation";
  }
  return "unknown";
}

JsonValue Diagnostic::to_json() const {
  JsonValue out;
  (void)out.set("code", JsonValue::string(std::string(to_string(code))));
  (void)out.set("layer", JsonValue::string(std::string(to_string(layer))));
  (void)out.set("severity", JsonValue::string(std::string(to_string(severity))));
  (void)out.set("path", JsonValue::string(path));
  (void)out.set("message", JsonValue::string(message));
  if (!detail.empty()) {
    (void)out.set("detail", JsonValue::string(detail));
  }
  if (!subjects.empty()) {
    JsonArray items;
    items.reserve(subjects.size());
    for (const auto& subject : subjects) {
      items.push_back(JsonValue::string(subject.str()));
    }
    (void)out.set("subjects", JsonValue::array(std::move(items)));
  }
  if (!conflict.empty()) {
    (void)out.set("conflict", JsonValue::string(conflict));
  }
  return out;
}

std::string Diagnostic::render() const {
  std::string out(to_string(severity));
  out.append(" [");
  out.append(to_string(layer));
  out.append("] ");
  out.append(std::string(to_string(code)));
  if (!path.empty()) {
    out.append(" at ");
    out.append(path);
  }
  out.append(": ");
  out.append(message);
  if (!detail.empty()) {
    out.append(" (");
    out.append(detail);
    out.append(")");
  }
  return out;
}

bool ValidationReport::passed() const noexcept {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::Error) {
      return false;
    }
  }
  return true;
}

std::size_t ValidationReport::error_count() const noexcept {
  std::size_t total = 0;
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::Error) {
      total = saturating_add(total, 1);
    }
  }
  return total;
}

std::size_t ValidationReport::warning_count() const noexcept {
  std::size_t total = 0;
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::Warning) {
      total = saturating_add(total, 1);
    }
  }
  return total;
}

JsonValue ValidationReport::to_json() const {
  JsonValue out;
  (void)out.set("parsed", JsonValue::boolean(parsed));
  (void)out.set("passed", JsonValue::boolean(passed()));
  (void)out.set("schema", JsonValue::string(declared_schema.str()));
  (void)out.set("content_digest", JsonValue::string(content_digest.hex()));
  (void)out.set("document_digest", JsonValue::string(document_digest.hex()));
  (void)out.set("error_count",
                JsonValue::integer(static_cast<std::int64_t>(error_count())));
  (void)out.set("warning_count",
                JsonValue::integer(static_cast<std::int64_t>(warning_count())));
  JsonArray migrations;
  for (const auto& item : applied_migrations) {
    migrations.push_back(JsonValue::string(item));
  }
  (void)out.set("applied_migrations", JsonValue::array(std::move(migrations)));
  JsonArray items;
  items.reserve(diagnostics.size());
  for (const auto& diagnostic : diagnostics) {
    items.push_back(diagnostic.to_json());
  }
  (void)out.set("diagnostics", JsonValue::array(std::move(items)));
  JsonArray found;
  found.reserve(conflicts.size());
  for (const auto& conflict : conflicts) {
    found.push_back(conflict.to_json());
  }
  (void)out.set("conflicts", JsonValue::array(std::move(found)));
  return out;
}

std::string ValidationReport::render() const {
  std::string out = "validation ";
  out.append(passed() ? "PASSED" : "FAILED");
  out.append(" (");
  out.append(std::to_string(error_count()));
  out.append(" error(s), ");
  out.append(std::to_string(warning_count()));
  out.append(" warning(s))");
  if (!applied_migrations.empty()) {
    out.append("\nschema migrations applied:");
    for (const auto& item : applied_migrations) {
      out.append("\n  - ");
      out.append(item);
    }
  }
  for (const auto& diagnostic : diagnostics) {
    out.append("\n  ");
    out.append(diagnostic.render());
  }
  for (const auto& conflict : conflicts) {
    out.append("\n  conflict ");
    out.append(conflict.render());
  }
  return out;
}

// ---------------------------------------------------------------------------
// Capability vocabulary and property registry
// ---------------------------------------------------------------------------
const std::vector<std::string_view>& known_capabilities() {
  static const std::vector<std::string_view> kCapabilities = {
      "l2.bridging", "l2.mlag",    "l3.routing",  "l3.vrf",     "l3.vxlan",
      "l3.evpn",     "srv6",       "mpls",        "qos.pfc",    "qos.ecn",
      "qos.remark",  "qos.shaping", "acl.stateful", "lag.static", "lag.lacp",
      "bfd",         "ptp",        "rocev2",      "telemetry.flow", "netconf",
      "gnmi",        "snmp.read",  "secure.boot", "fabric.ztp",
  };
  return kCapabilities;
}

bool is_known_capability(std::string_view name) {
  for (const auto& capability : known_capabilities()) {
    if (capability == name) {
      return true;
    }
  }
  return false;
}

const std::vector<PropertySpec>& known_properties() {
  static const std::vector<PropertySpec> kProperties = {
      {"admin_state",
       {IdClass::Device, IdClass::Port, IdClass::Link},
       PropertyValueKind::Enum,
       "admin_state"},
      {"service_classes",
       {IdClass::Device, IdClass::Port, IdClass::Link, IdClass::Workload},
       PropertyValueKind::IdList,
       "service-class"},
      {"bandwidth_reservation_bps",
       {IdClass::Link, IdClass::Device, IdClass::Site, IdClass::Pod, IdClass::Rack},
       PropertyValueKind::Integer,
       ""},
      {"path_policy",
       {IdClass::Device, IdClass::Site, IdClass::Link, IdClass::Fabric},
       PropertyValueKind::Enum,
       "routing_action"},
      {"redundancy_level",
       {IdClass::Device, IdClass::Site, IdClass::Fabric},
       PropertyValueKind::Enum,
       "failure_domain"},
      {"maintenance_window",
       {IdClass::Device, IdClass::Rack, IdClass::Pod},
       PropertyValueKind::String,
       ""},
      {"upgrade_group",
       {IdClass::Device, IdClass::Rack, IdClass::Pod},
       PropertyValueKind::String,
       ""},
      {"tenant",
       {IdClass::Device, IdClass::Port, IdClass::Workload},
       PropertyValueKind::String,
       ""},
      {"isolation_id", {IdClass::Tenant}, PropertyValueKind::Integer, ""},
  };
  return kProperties;
}

const PropertySpec* find_property(std::string_view name) {
  for (const auto& property : known_properties()) {
    if (property.name == name) {
      return &property;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
namespace {

void add(std::vector<Diagnostic>& sink, Severity severity, ValidationLayer layer, ErrorCode code,
         const std::string& path, std::string message, std::string detail = std::string()) {
  if (sink.size() >= kMaxDiagnostics) {
    return;
  }
  Diagnostic diagnostic;
  diagnostic.severity = severity;
  diagnostic.layer = layer;
  diagnostic.code = code;
  diagnostic.path = path;
  diagnostic.message = std::move(message);
  diagnostic.detail = std::move(detail);
  sink.push_back(std::move(diagnostic));
}

void add_subject(std::vector<Diagnostic>& sink, Severity severity, ValidationLayer layer,
                 ErrorCode code, const std::string& path, std::string message,
                 const std::vector<AnyId>& subjects, std::string detail = std::string()) {
  if (sink.size() >= kMaxDiagnostics) {
    return;
  }
  Diagnostic diagnostic;
  diagnostic.severity = severity;
  diagnostic.layer = layer;
  diagnostic.code = code;
  diagnostic.path = path;
  diagnostic.message = std::move(message);
  diagnostic.detail = std::move(detail);
  diagnostic.subjects = subjects;
  sink.push_back(std::move(diagnostic));
}

std::string index_path(const char* collection, std::size_t index) {
  return std::string(collection) + "[" + std::to_string(index) + "]";
}

std::string named_path(const char* collection, const std::string& id) {
  return std::string(collection) + "[" + id + "]";
}

bool is_enabled(AdminState state) { return state == AdminState::Enabled; }

// ---- Layer: schema compatibility -----------------------------------------
void validate_schema_compatibility(const IntentDocument& document, SchemaVersion declared,
                                   std::vector<Diagnostic>& sink) {
  for (const auto& feature : document.required_features) {
    const auto info = feature_info(feature);
    if (!info.has_value()) {
      add(sink, Severity::Error, ValidationLayer::SchemaCompatibility, ErrorCode::FeatureNotAvailable,
          "schema.requires",
          "document requires a feature this build does not implement: '" + feature + "'",
          "refusing to accept semantics that cannot be honoured");
      continue;
    }
    if (info->introduced_minor > declared.minor()) {
      add(sink, Severity::Error, ValidationLayer::SchemaCompatibility,
          ErrorCode::AmbiguousSemantics, "schema.requires",
          "feature '" + feature + "' was introduced in schema minor " +
              std::to_string(info->introduced_minor) + " but the document declares " +
              declared.str(),
          "the declaration is internally inconsistent");
    }
  }
  for (const auto& feature : document.optional_features) {
    const auto info = feature_info(feature);
    if (!info.has_value()) {
      add(sink, Severity::Info, ValidationLayer::SchemaCompatibility, ErrorCode::FeatureNotAvailable,
          "schema.optional", "optional feature '" + feature + "' is not implemented and is ignored");
      continue;
    }
    if (std::find(document.required_features.begin(), document.required_features.end(), feature) !=
        document.required_features.end()) {
      add(sink, Severity::Error, ValidationLayer::SchemaCompatibility,
          ErrorCode::AmbiguousSemantics, "schema",
          "feature '" + feature + "' is declared both required and optional");
    }
  }
}

// ---- Layer: syntax --------------------------------------------------------
void validate_syntax(const IntentDocument& document, std::vector<Diagnostic>& sink) {
  const std::size_t objects = document.declared_object_count();
  if (objects > kMaxObjectsPerDocument) {
    add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::LimitExceeded, "$",
        "document declares more objects than the configured bound",
        std::to_string(objects) + " > " + std::to_string(kMaxObjectsPerDocument));
  }
  if (document.domain.empty()) {
    add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::IdentityMalformed, "domain",
        "intent domain identity is required");
  }
  for (std::size_t i = 0; i < document.service_classes.size(); ++i) {
    const auto& entry = document.service_classes[i];
    const std::string path = named_path("service_classes", entry.id.str());
    if (entry.bandwidth_share_permille > 1000) {
      add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::InvalidArgument,
          path + ".bandwidth_share_permille",
          "bandwidth share must be at most 1000 permille",
          std::to_string(entry.bandwidth_share_permille));
    }
    if (entry.loss_budget_ppm > 1000000) {
      add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::InvalidArgument,
          path + ".loss_budget_ppm", "loss budget must be at most 1000000 ppm",
          std::to_string(entry.loss_budget_ppm));
    }
  }
  for (std::size_t i = 0; i < document.ports.size(); ++i) {
    const auto& entry = document.ports[i];
    const std::string path = named_path("topology.ports", entry.id.str());
    if (entry.device.empty()) {
      add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::MissingField, path + ".device",
          "port must name its device");
    }
    if (entry.device.empty()) {
      continue;
    }
  }
  for (std::size_t i = 0; i < document.ports.size(); ++i) {
    const auto& entry = document.ports[i];
    const std::string path = named_path("topology.ports", entry.id.str());
    if (entry.speed_bps == 0) {
      add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::MissingField,
          path + ".speed_bps", "port speed must be declared and non-zero",
          "an undeclared port speed is not assumed to be adequate");
    }
  }
  for (std::size_t i = 0; i < document.links.size(); ++i) {
    const auto& entry = document.links[i];
    const std::string path = named_path("topology.links", entry.id.str());
    if (entry.capacity_bps == 0) {
      add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::MissingField,
          path + ".capacity_bps", "link capacity must be declared and non-zero",
          "an undeclared link capacity is not assumed to be adequate");
    }
  }
  for (std::size_t i = 0; i < document.policies.size(); ++i) {
    const auto& policy = document.policies[i];
    const std::string path = named_path("policies", policy.id.str());
    if (const auto* routing = std::get_if<RoutingPolicyDecl>(&policy.body)) {
      if (routing->max_ecmp_width == 0) {
        add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::InvalidArgument,
            path + ".routing.max_ecmp_width", "ECMP width must be at least 1");
      }
      if (routing->action != RoutingAction::ShortestPath && routing->explicit_path.empty() &&
          routing->action != RoutingAction::Drop) {
        add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::AmbiguousSemantics,
            path + ".routing.explicit_path",
            "an explicit or pinned routing action requires a non-empty path",
            std::string(to_string(routing->action)));
      }
    } else if (const auto* capacity = std::get_if<CapacityPolicyDecl>(&policy.body)) {
      if (capacity->max_utilization_permille > 1000) {
        add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::InvalidArgument,
            path + ".capacity.max_utilization_permille",
            "maximum utilization must be at most 1000 permille");
      }
    } else if (const auto* redundancy = std::get_if<RedundancyPolicyDecl>(&policy.body)) {
      if (redundancy->min_disjoint_paths == 0) {
        add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::InvalidArgument,
            path + ".redundancy.min_disjoint_paths",
            "a redundancy requirement must ask for at least one disjoint path");
      }
    } else if (const auto* maintenance =
                   std::get_if<MaintenanceEligibilityPolicyDecl>(&policy.body)) {
      if (maintenance->max_concurrent_operations == 0) {
        add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::InvalidArgument,
            path + ".maintenance-eligibility.max_concurrent_operations",
            "maintenance concurrency must be at least 1");
      }
    }
  }
  for (std::size_t i = 0; i < document.workloads.size(); ++i) {
    const auto& entry = document.workloads[i];
    const std::string path = named_path("workloads", entry.id.str());
    if (entry.replicas == 0) {
      add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::InvalidArgument,
          path + ".replicas", "workload replicas must be at least 1");
    }
    if (entry.min_disjoint_domains == 0) {
      add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::InvalidArgument,
          path + ".min_disjoint_domains", "workload redundancy must ask for at least one domain");
    }
  }
  for (std::size_t i = 0; i < document.intents.size(); ++i) {
    const auto& entry = document.intents[i];
    const std::string path = named_path("intents", entry.id.str());
    if (entry.property.empty()) {
      add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::MissingField,
          path + ".property", "intent object must declare the property it constrains");
    }
  }
  // Service class bandwidth shares are a fabric-wide budget.
  std::uint64_t share_total = 0;
  for (const auto& entry : document.service_classes) {
    share_total += entry.bandwidth_share_permille;
  }
  if (share_total > 1000) {
    add(sink, Severity::Error, ValidationLayer::Syntax, ErrorCode::CapacityExceeded,
        "service_classes", "declared service-class bandwidth shares exceed 1000 permille",
        std::to_string(share_total));
  }
}

// ---- Layer: referential integrity ----------------------------------------
void sort_diagnostics(std::vector<Diagnostic>& sink);

void require_reference(std::vector<Diagnostic>& sink, const TopologyIndex& index,
                       const std::string& path, const AnyId& id, std::string what) {
  if (id.empty()) {
    return;
  }
  if (!index.resolves(id)) {
    add_subject(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
                ErrorCode::DanglingReference, path,
                std::move(what) + " references an object that is not declared in this generation",
                {id}, id.str());
  }
}

void validate_references(const IntentDocument& document, const TopologyIndex& index,
                         std::vector<Diagnostic>& sink) {
  for (const auto& duplicate : index.duplicates()) {
    add_subject(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
                ErrorCode::DuplicateIdentity, duplicate.first_path,
                "identity is declared more than once (" + std::to_string(duplicate.count) +
                    " times); exactly one declaration per identity is authoritative",
                {duplicate.id}, duplicate.second_path);
  }
  for (const auto& entry : document.sites) {
    require_reference(sink, index, named_path("topology.sites", entry.id.str()) + ".fabric",
                      AnyId(entry.fabric), "site fabric");
  }
  for (const auto& entry : document.pods) {
    require_reference(sink, index, named_path("topology.pods", entry.id.str()) + ".site",
                      AnyId(entry.site), "pod site");
  }
  for (const auto& entry : document.racks) {
    require_reference(sink, index, named_path("topology.racks", entry.id.str()) + ".pod",
                      AnyId(entry.pod), "rack pod");
  }
  for (const auto& entry : document.devices) {
    const std::string path = named_path("topology.devices", entry.id.str());
    require_reference(sink, index, path + ".site", AnyId(entry.site), "device site");
    require_reference(sink, index, path + ".pod", AnyId(entry.pod), "device pod");
    require_reference(sink, index, path + ".rack", AnyId(entry.rack), "device rack");
    for (const auto& id : entry.service_classes) {
      require_reference(sink, index, path + ".service_classes", AnyId(id), "device service class");
    }
  }
  for (const auto& entry : document.ports) {
    const std::string path = named_path("topology.ports", entry.id.str());
    require_reference(sink, index, path + ".device", AnyId(entry.device), "port device");
    for (const auto& id : entry.service_classes) {
      require_reference(sink, index, path + ".service_classes", AnyId(id), "port service class");
    }
  }
  for (const auto& entry : document.links) {
    const std::string path = named_path("topology.links", entry.id.str());
    require_reference(sink, index, path + ".endpoint_a", AnyId(entry.endpoint_a), "link endpoint");
    require_reference(sink, index, path + ".endpoint_b", AnyId(entry.endpoint_b), "link endpoint");
    for (const auto& id : entry.service_classes) {
      require_reference(sink, index, path + ".service_classes", AnyId(id), "link service class");
    }
  }
  for (const auto& entry : document.workloads) {
    const std::string path = named_path("workloads", entry.id.str());
    require_reference(sink, index, path + ".tenant", AnyId(entry.tenant), "workload tenant");
    require_reference(sink, index, path + ".service_class", AnyId(entry.service_class),
                      "workload service class");
    for (const auto& id : entry.placement) {
      require_reference(sink, index, path + ".placement", AnyId(id), "workload placement");
    }
    for (const auto& id : entry.allowed_sites) {
      require_reference(sink, index, path + ".allowed_sites", AnyId(id), "workload allowed site");
    }
  }
  for (const auto& entry : document.intents) {
    const std::string path = named_path("intents", entry.id.str());
    require_reference(sink, index, path + ".subject", entry.subject, "intent subject");
    if (!entry.tenant.empty()) {
      const auto tenant = AnyId::parse(entry.tenant);
      if (!tenant) {
        add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
            ErrorCode::IdentityMalformed, path + ".tenant", "intent tenant identity is malformed",
            entry.tenant);
      } else {
        require_reference(sink, index, path + ".tenant", tenant.value(), "intent tenant");
      }
    }
  }
  for (const auto& policy : document.policies) {
    const std::string path = named_path("policies", policy.id.str());
    if (const auto* routing = std::get_if<RoutingPolicyDecl>(&policy.body)) {
      if (!routing->match_source.empty()) {
        const auto id = AnyId::parse(routing->match_source);
        if (!id) {
          add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
              ErrorCode::IdentityMalformed, path + ".routing.match_source",
              "match source identity is malformed", routing->match_source);
        } else {
          require_reference(sink, index, path + ".routing.match_source", id.value(),
                            "routing match source");
        }
      }
      if (!routing->match_destination.empty()) {
        const auto id = AnyId::parse(routing->match_destination);
        if (!id) {
          add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
              ErrorCode::IdentityMalformed, path + ".routing.match_destination",
              "match destination identity is malformed", routing->match_destination);
        } else {
          require_reference(sink, index, path + ".routing.match_destination", id.value(),
                            "routing match destination");
        }
      }
      for (const auto& hop : routing->explicit_path) {
        const auto id = AnyId::parse(hop);
        if (!id) {
          add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
              ErrorCode::IdentityMalformed, path + ".routing.explicit_path",
              "explicit path hop identity is malformed", hop);
        } else {
          require_reference(sink, index, path + ".routing.explicit_path", id.value(),
                            "routing path hop");
        }
      }
      for (const auto& id : routing->match_service_classes) {
        require_reference(sink, index, path + ".routing.match_service_classes", AnyId(id),
                          "routing match service class");
      }
      for (const auto& id : routing->match_tenants) {
        require_reference(sink, index, path + ".routing.match_tenants", AnyId(id),
                          "routing match tenant");
      }
      if (!routing->extends_policy.empty()) {
        const auto id = AnyId::parse(routing->extends_policy);
        if (!id || id->klass() != IdClass::Policy) {
          add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
              ErrorCode::IdentityClassMismatch, path + ".routing.extends_policy",
              "composed policy reference must be a policy identity", routing->extends_policy);
        } else {
          require_reference(sink, index, path + ".routing.extends_policy", id.value(),
                            "composed routing policy");
          const auto* target = index.policy(id->as<IdClass::Policy>().value());
          if (target != nullptr && std::get_if<RoutingPolicyDecl>(&target->body) == nullptr) {
            add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
                ErrorCode::AmbiguousSemantics, path + ".routing.extends_policy",
                "composed policy is not a routing policy", target->id.str());
          }
        }
      }
    } else if (const auto* capacity = std::get_if<CapacityPolicyDecl>(&policy.body)) {
      const auto id = AnyId::parse(capacity->scope);
      if (!id) {
        add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
            ErrorCode::IdentityMalformed, path + ".capacity.scope",
            "capacity scope identity is malformed", capacity->scope);
      } else {
        require_reference(sink, index, path + ".capacity.scope", id.value(), "capacity scope");
      }
      for (const auto& class_id : capacity->service_classes) {
        require_reference(sink, index, path + ".capacity.service_classes", AnyId(class_id),
                          "capacity service class");
      }
    } else if (const auto* admin = std::get_if<AdminStatePolicyDecl>(&policy.body)) {
      const auto id = AnyId::parse(admin->scope);
      if (!id) {
        add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
            ErrorCode::IdentityMalformed, path + ".admin-state.scope",
            "administrative-state scope identity is malformed", admin->scope);
      } else {
        require_reference(sink, index, path + ".admin-state.scope", id.value(),
                          "administrative-state scope");
      }
    } else if (const auto* redundancy = std::get_if<RedundancyPolicyDecl>(&policy.body)) {
      const auto id = AnyId::parse(redundancy->scope);
      if (!id) {
        add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
            ErrorCode::IdentityMalformed, path + ".redundancy.scope",
            "redundancy scope identity is malformed", redundancy->scope);
      } else {
        require_reference(sink, index, path + ".redundancy.scope", id.value(), "redundancy scope");
      }
      for (const auto& class_id : redundancy->service_classes) {
        require_reference(sink, index, path + ".redundancy.service_classes", AnyId(class_id),
                          "redundancy service class");
      }
    } else if (const auto* maintenance =
                   std::get_if<MaintenanceEligibilityPolicyDecl>(&policy.body)) {
      const auto id = AnyId::parse(maintenance->scope);
      if (!id) {
        add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
            ErrorCode::IdentityMalformed, path + ".maintenance-eligibility.scope",
            "maintenance scope identity is malformed", maintenance->scope);
      } else {
        require_reference(sink, index, path + ".maintenance-eligibility.scope", id.value(),
                          "maintenance scope");
      }
    } else if (const auto* isolation = std::get_if<TenantIsolationPolicyDecl>(&policy.body)) {
      require_reference(sink, index, path + ".tenant-isolation.tenant", AnyId(isolation->tenant),
                        "isolation tenant");
      for (const auto& scope : isolation->exclusive_scopes) {
        const auto id = AnyId::parse(scope);
        if (!id) {
          add(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
              ErrorCode::IdentityMalformed, path + ".tenant-isolation.exclusive_scopes",
              "exclusive scope identity is malformed", scope);
        } else {
          require_reference(sink, index, path + ".tenant-isolation.exclusive_scopes", id.value(),
                            "exclusive scope");
        }
      }
    }
  }
  const auto cycles = detect_reference_cycles(document);
  for (const auto& cycle : cycles) {
    std::string joined;
    for (std::size_t i = 0; i < cycle.members.size(); ++i) {
      if (i != 0) {
        joined.append(" -> ");
      }
      joined.append(cycle.members[i].str());
    }
    add_subject(sink, Severity::Error, ValidationLayer::ReferentialIntegrity,
                ErrorCode::CyclicReference, "policies",
                "cyclic reference detected in the declared intent graph", cycle.members, joined);
  }
}

// ---- Layer: capability compatibility -------------------------------------
void validate_capabilities(const IntentDocument& document, const TopologyIndex& index,
                           std::vector<Diagnostic>& sink) {
  for (const auto& entry : document.service_classes) {
    const std::string path = named_path("service_classes", entry.id.str());
    for (const auto& capability : entry.required_capabilities) {
      if (!is_known_capability(capability)) {
        add(sink, Severity::Error, ValidationLayer::CapabilityCompatibility,
            ErrorCode::UnknownCapability, path + ".required_capabilities",
            "service class requires a capability this runtime cannot reason about: '" + capability +
                "'",
            "an unrecognised capability requirement is never assumed to be satisfied");
      }
    }
  }
  for (const auto& entry : document.devices) {
    const std::string path = named_path("topology.devices", entry.id.str());
    if (entry.model.empty()) {
      add(sink, Severity::Error, ValidationLayer::CapabilityCompatibility,
          ErrorCode::UnknownCapability, path + ".model",
          "device declares no model, so its capabilities are unknown",
          "unknown capabilities are not treated as satisfied");
      continue;
    }
    const CapabilityDecl* capability = index.capability_model(entry.model);
    if (capability == nullptr) {
      add(sink, Severity::Error, ValidationLayer::CapabilityCompatibility,
          ErrorCode::UnknownCapability, path + ".model",
          "device model '" + entry.model + "' is not described by the capability catalog",
          "declare the model in capability_catalog or the device's capabilities stay unknown");
      continue;
    }
    if (!capability->supported_roles.empty() &&
        std::find(capability->supported_roles.begin(), capability->supported_roles.end(),
                  entry.role) == capability->supported_roles.end()) {
      add(sink, Severity::Error, ValidationLayer::CapabilityCompatibility,
          ErrorCode::UnknownCapability, path + ".role",
          "device model '" + entry.model + "' does not declare support for role '" +
              std::string(to_string(entry.role)) + "'");
    }
    if (capability->port_count > 0 && entry.declared_port_count > capability->port_count) {
      add(sink, Severity::Error, ValidationLayer::CapabilityCompatibility,
          ErrorCode::CapacityExceeded, path + ".declared_port_count",
          "device declares more ports than its model provides",
          std::to_string(entry.declared_port_count) + " > " +
              std::to_string(capability->port_count));
    }
    std::set<std::string> owned(capability->capabilities.begin(), capability->capabilities.end());
    for (const auto& class_id : entry.service_classes) {
      const ServiceClassDecl* service_class = index.service_class(class_id);
      if (service_class == nullptr) {
        continue;
      }
      for (const auto& requirement : service_class->required_capabilities) {
        if (owned.count(requirement) == 0) {
          add(sink, Severity::Error, ValidationLayer::CapabilityCompatibility,
              ErrorCode::UnknownCapability, path + ".service_classes",
              "device model '" + entry.model + "' does not provide capability '" + requirement +
                  "' required by service class " + class_id.str(),
              "capability must be proven, not assumed");
        }
      }
    }
  }
  for (const auto& entry : document.ports) {
    const std::string path = named_path("topology.ports", entry.id.str());
    const DeviceDecl* device = index.device(entry.device);
    if (device == nullptr) {
      continue;
    }
    const CapabilityDecl* capability =
        device->model.empty() ? nullptr : index.capability_model(device->model);
    if (capability == nullptr) {
      continue;
    }
    if (capability->max_speed_bps > 0 && entry.speed_bps > capability->max_speed_bps) {
      add(sink, Severity::Error, ValidationLayer::CapabilityCompatibility,
          ErrorCode::CapacityExceeded, path + ".speed_bps",
          "port speed exceeds the maximum speed declared for the device model",
          std::to_string(entry.speed_bps) + " > " + std::to_string(capability->max_speed_bps));
    }
    std::set<std::string> owned(capability->capabilities.begin(), capability->capabilities.end());
    for (const auto& class_id : entry.service_classes) {
      const ServiceClassDecl* service_class = index.service_class(class_id);
      if (service_class == nullptr) {
        continue;
      }
      for (const auto& requirement : service_class->required_capabilities) {
        if (owned.count(requirement) == 0) {
          add(sink, Severity::Error, ValidationLayer::CapabilityCompatibility,
              ErrorCode::UnknownCapability, path + ".service_classes",
              "device model '" + device->model + "' does not provide capability '" + requirement +
                  "' required by service class " + class_id.str());
        }
      }
    }
  }
  // Capacity observations used by the conflict layer must be observable.
  for (const auto& policy : document.policies) {
    const auto* capacity = std::get_if<CapacityPolicyDecl>(&policy.body);
    if (capacity == nullptr || capacity->reserved_bps == 0) {
      continue;
    }
    const auto scope = AnyId::parse(capacity->scope);
    if (!scope) {
      continue;
    }
    const ScopeCapacity observed = scope_capacity(index, scope.value());
    if (!observed.known) {
      add(sink, Severity::Error, ValidationLayer::CapabilityCompatibility,
          ErrorCode::UnknownCapability,
          named_path("policies", policy.id.str()) + ".capacity.scope",
          "cannot observe any enabled link capacity for scope " + scope->str(),
          "a reservation cannot be checked against unobserved capacity");
    }
  }
}

// ---- Layer: topology feasibility -----------------------------------------
void validate_topology(const IntentDocument& document, const TopologyIndex& index,
                       std::vector<Diagnostic>& sink) {
  for (const auto& entry : document.ports) {
    const DeviceDecl* device = index.device(entry.device);
    if (device == nullptr) {
      continue;
    }
    const std::string path = named_path("topology.ports", entry.id.str());
    if (device->declared_port_count > 0 && entry.index >= device->declared_port_count) {
      add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
          ErrorCode::InfeasibleTopology, path + ".index",
          "port index is outside the declared port count of its device",
          std::to_string(entry.index) + " >= " + std::to_string(device->declared_port_count));
    }
  }
  std::map<std::string, std::uint32_t> port_index_owner;
  for (const auto& entry : document.ports) {
    const std::string key = entry.device.str() + "#" + std::to_string(entry.index);
    const auto it = port_index_owner.find(key);
    if (it != port_index_owner.end()) {
      add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
          ErrorCode::DuplicateIdentity, named_path("topology.ports", entry.id.str()) + ".index",
          "two ports of the same device claim the same index", key);
    } else {
      port_index_owner.emplace(key, 1);
    }
  }
  std::map<std::string, std::string> port_link_owner;
  for (const auto& entry : document.links) {
    const std::string path = named_path("topology.links", entry.id.str());
    const PortDecl* a = index.port(entry.endpoint_a);
    const PortDecl* b = index.port(entry.endpoint_b);
    if (a == nullptr || b == nullptr) {
      continue;
    }
    if (a->id == b->id) {
      add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
          ErrorCode::InfeasibleTopology, path,
          "link terminates on the same port twice", a->id.str());
      continue;
    }
    if (a->device == b->device) {
      add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
          ErrorCode::InfeasibleTopology, path,
          "link connects two ports of the same device", a->device.str());
    }
    if (a->speed_bps != b->speed_bps) {
      add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
          ErrorCode::InfeasibleTopology, path + ".endpoint_b",
          "link endpoints declare different speeds",
          std::to_string(a->speed_bps) + " vs " + std::to_string(b->speed_bps));
    }
    for (const PortDecl* port_entry : {a, b}) {
      const auto it = port_link_owner.find(port_entry->id.str());
      if (it != port_link_owner.end() && it->second != entry.id.str()) {
        add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
            ErrorCode::InfeasibleTopology, path,
            "port is already terminated by another link",
            port_entry->id.str() + " also used by " + it->second);
      } else {
        port_link_owner.emplace(port_entry->id.str(), entry.id.str());
      }
    }
    const bool host = entry.kind == LinkKind::Host;
    const bool a_access = a->role == PortRole::Access;
    const bool b_access = b->role == PortRole::Access;
    if (host && (a_access == b_access)) {
      add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
          ErrorCode::InfeasibleTopology, path,
          "a host link must terminate on exactly one access port");
    }
    if (!host && (a_access || b_access)) {
      add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
          ErrorCode::InfeasibleTopology, path,
          "a fabric link must not terminate on an access port");
    }
  }
  // Device connectivity across enabled links.
  {
    std::map<std::string, std::vector<std::string>> adjacency;
    for (const auto& device : document.devices) {
      adjacency.emplace(device.id.str(), std::vector<std::string>{});
    }
    for (const auto& entry : document.links) {
      if (entry.admin != AdminState::Enabled) {
        continue;
      }
      const PortDecl* a = index.port(entry.endpoint_a);
      const PortDecl* b = index.port(entry.endpoint_b);
      if (a == nullptr || b == nullptr) {
        continue;
      }
      adjacency[a->device.str()].push_back(b->device.str());
      adjacency[b->device.str()].push_back(a->device.str());
    }
    if (document.devices.size() > 1) {
      std::map<std::string, int> component;
      int next = 0;
      for (const auto& node : adjacency) {
        if (component.count(node.first) != 0) {
          continue;
        }
        std::vector<std::string> stack{node.first};
        component[node.first] = next;
        while (!stack.empty()) {
          const std::string current = stack.back();
          stack.pop_back();
          for (const auto& neighbour : adjacency[current]) {
            if (component.count(neighbour) == 0) {
              component[neighbour] = next;
              stack.push_back(neighbour);
            }
          }
        }
        ++next;
      }
      if (next > 1) {
        std::map<int, std::vector<std::string>> groups;
        for (const auto& pair : component) {
          groups[pair.second].push_back(pair.first);
        }
        for (auto& group : groups) {
          std::sort(group.second.begin(), group.second.end());
        }
        std::string detail;
        for (const auto& group : groups) {
          if (!detail.empty()) {
            detail.append(" | ");
          }
          detail.append("{");
          for (std::size_t i = 0; i < group.second.size(); ++i) {
            if (i != 0) {
              detail.append(",");
            }
            detail.append(group.second[i]);
          }
          detail.append("}");
        }
        add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
            ErrorCode::InfeasibleTopology, "topology",
            "the declared topology is partitioned into " + std::to_string(next) +
                " unreachable components",
            detail);
      }
    }
  }
  // Routing path feasibility.
  for (const auto& policy : document.policies) {
    const auto* routing = std::get_if<RoutingPolicyDecl>(&policy.body);
    if (routing == nullptr || routing->explicit_path.empty()) {
      continue;
    }
    const std::string path = named_path("policies", policy.id.str()) + ".routing.explicit_path";
    if (routing->explicit_path.size() > kMaxPathHops) {
      add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
          ErrorCode::LimitExceeded, path, "explicit path exceeds the maximum hop count",
          std::to_string(routing->explicit_path.size()));
      continue;
    }
    std::vector<AnyId> hops;
    bool resolvable = true;
    for (const auto& hop : routing->explicit_path) {
      const auto id = AnyId::parse(hop);
      if (!id || !index.resolves(id.value())) {
        resolvable = false;
        break;
      }
      hops.push_back(id.value());
    }
    if (!resolvable || hops.size() < 2) {
      continue;
    }
    for (std::size_t i = 1; i < hops.size(); ++i) {
      const AnyId& from = hops[i - 1];
      const AnyId& to = hops[i];
      bool adjacent = false;
      if (from.klass() == IdClass::Device && to.klass() == IdClass::Device) {
        for (const LinkDecl* link : index.links_of_device(from.as<IdClass::Device>().value())) {
          if (link->admin != AdminState::Enabled) {
            continue;
          }
          const PortDecl* a = index.port(link->endpoint_a);
          const PortDecl* b = index.port(link->endpoint_b);
          if (a == nullptr || b == nullptr) {
            continue;
          }
          const DeviceId far = a->device == from.as<IdClass::Device>().value() ? b->device : a->device;
          if (far == to.as<IdClass::Device>().value()) {
            adjacent = true;
            break;
          }
        }
      } else if (from.klass() == IdClass::Link || to.klass() == IdClass::Link) {
        adjacent = true;
      } else {
        adjacent = true;
      }
      if (!adjacent) {
        add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
            ErrorCode::InfeasibleTopology, path,
            "explicit path is not connected between consecutive hops",
            from.str() + " -> " + to.str());
      }
    }
  }
  // Workload redundancy feasibility.
  for (const auto& entry : document.workloads) {
    const std::string path = named_path("workloads", entry.id.str());
    if (!entry.placement.empty() && entry.replicas > entry.placement.size()) {
      add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
          ErrorCode::InfeasibleTopology, path + ".replicas",
          "workload asks for more replicas than declared placement devices",
          std::to_string(entry.replicas) + " > " + std::to_string(entry.placement.size()));
    }
    if (entry.min_disjoint_domains <= 1 || entry.placement.empty()) {
      continue;
    }
    std::set<std::string> domains;
    for (const auto& device_id : entry.placement) {
      const DeviceDecl* device = index.device(device_id);
      if (device == nullptr) {
        continue;
      }
      domains.insert(index.failure_domain_key(*device, entry.redundancy_level));
    }
    if (domains.size() < entry.min_disjoint_domains) {
      add(sink, Severity::Error, ValidationLayer::TopologyFeasibility,
          ErrorCode::InfeasibleTopology, path + ".min_disjoint_domains",
          "workload placement cannot satisfy the requested failure-domain spread",
          std::to_string(domains.size()) + " domain(s) < " +
              std::to_string(entry.min_disjoint_domains) + " at level '" +
              std::string(to_string(entry.redundancy_level)) + "'");
    }
  }
}

// ---- Layer: policy conflict ----------------------------------------------
void validate_policy_conflicts(const IntentDocument& document, const TopologyIndex& index,
                               std::vector<Conflict>& conflicts, std::vector<Diagnostic>& sink) {
  conflicts = detect_conflicts(document, index);
  for (const auto& conflict : conflicts) {
    Diagnostic diagnostic;
    diagnostic.severity = conflict.severity;
    diagnostic.layer = ValidationLayer::PolicyConflict;
    diagnostic.code = ErrorCode::PolicyConflict;
    diagnostic.path = conflict.path;
    diagnostic.message = conflict.summary;
    diagnostic.detail = conflict.explanation;
    diagnostic.conflict = conflict.id.str();
    for (const auto& participant : conflict.participants) {
      diagnostic.subjects.push_back(participant.id);
    }
    if (sink.size() < kMaxDiagnostics) {
      sink.push_back(std::move(diagnostic));
    }
  }
}

// ---- Layer: invariants ----------------------------------------------------
bool property_value_matches(const PropertySpec& spec, const JsonValue& value, std::string& why) {
  switch (spec.kind) {
    case PropertyValueKind::Enum: {
      const auto* text = value.try_string();
      if (text == nullptr) {
        why = "expected an enumeration string";
        return false;
      }
      if (spec.domain == "admin_state") {
        const auto parsed = parse_admin_state(*text);
        if (!parsed) {
          why = "not a known administrative state: " + *text;
          return false;
        }
        return true;
      }
      if (spec.domain == "routing_action") {
        const auto parsed = parse_routing_action(*text);
        if (!parsed) {
          why = "not a known routing action: " + *text;
          return false;
        }
        return true;
      }
      if (spec.domain == "failure_domain") {
        const auto parsed = parse_failure_domain_level(*text);
        if (!parsed) {
          why = "not a known failure-domain level: " + *text;
          return false;
        }
        return true;
      }
      return true;
    }
    case PropertyValueKind::String: {
      if (value.try_string() == nullptr) {
        why = "expected a string";
        return false;
      }
      return true;
    }
    case PropertyValueKind::Integer: {
      if (!value.is_int()) {
        why = "expected an integer";
        return false;
      }
      return true;
    }
    case PropertyValueKind::Boolean: {
      if (!value.is_bool()) {
        why = "expected a boolean";
        return false;
      }
      return true;
    }
    case PropertyValueKind::IdList: {
      const auto* items = value.try_array();
      if (items == nullptr) {
        why = "expected an array of identities";
        return false;
      }
      for (const auto& item : *items) {
        const auto* text = item.try_string();
        if (text == nullptr) {
          why = "expected an array of identity strings";
          return false;
        }
        const auto parsed = AnyId::parse(*text);
        if (!parsed) {
          why = "malformed identity in list: " + *text;
          return false;
        }
      }
      return true;
    }
  }
  why = "unsupported property kind";
  return false;
}

void validate_invariants(const IntentDocument& document, const TopologyIndex& index,
                         std::vector<Diagnostic>& sink) {
  for (const auto& entry : document.links) {
    const std::string path = named_path("topology.links", entry.id.str());
    if (entry.admin != AdminState::Enabled) {
      continue;
    }
    const PortDecl* a = index.port(entry.endpoint_a);
    const PortDecl* b = index.port(entry.endpoint_b);
    for (const PortDecl* port_entry : {a, b}) {
      if (port_entry == nullptr) {
        continue;
      }
      if (port_entry->admin != AdminState::Enabled) {
        add(sink, Severity::Error, ValidationLayer::InvariantViolation,
            ErrorCode::InvariantViolation, path,
            "an enabled link terminates on a port that is not enabled", port_entry->id.str());
      }
      const DeviceDecl* device = index.device(port_entry->device);
      if (device != nullptr && device->admin == AdminState::Disabled) {
        add(sink, Severity::Error, ValidationLayer::InvariantViolation,
            ErrorCode::InvariantViolation, path,
            "an enabled link terminates on a disabled device",
            device->id.str() + " is " + std::string(to_string(device->admin)));
      } else if (device != nullptr && device->admin != AdminState::Enabled) {
        add(sink, Severity::Warning, ValidationLayer::InvariantViolation,
            ErrorCode::InvariantViolation, path,
            "an enabled link terminates on a device that is draining or in maintenance",
            device->id.str() + " is " + std::string(to_string(device->admin)));
      }
    }
  }
  for (const auto& entry : document.ports) {
    if (entry.admin != AdminState::Enabled) {
      continue;
    }
    const DeviceDecl* device = index.device(entry.device);
    if (device != nullptr && device->admin == AdminState::Disabled) {
      add(sink, Severity::Error, ValidationLayer::InvariantViolation,
          ErrorCode::InvariantViolation, named_path("topology.ports", entry.id.str()),
          "an enabled port belongs to a disabled device", device->id.str());
    }
  }
  for (const auto& entry : document.workloads) {
    const std::string path = named_path("workloads", entry.id.str());
    for (const auto& device_id : entry.placement) {
      const DeviceDecl* device = index.device(device_id);
      if (device == nullptr) {
        continue;
      }
      if (device->admin == AdminState::Disabled) {
        add(sink, Severity::Error, ValidationLayer::InvariantViolation,
            ErrorCode::InvariantViolation, path + ".placement",
            "workload is placed on a device that is administratively disabled", device->id.str());
      } else if (device->admin != AdminState::Enabled && entry.replicas <= 1) {
        add(sink, Severity::Warning, ValidationLayer::InvariantViolation,
            ErrorCode::InvariantViolation, path + ".placement",
            "workload has a single replica on a device that is not enabled",
            device->id.str() + " is " + std::string(to_string(device->admin)));
      }
    }
  }
  for (const auto& entry : document.intents) {
    const std::string path = named_path("intents", entry.id.str());
    const PropertySpec* spec = find_property(entry.property);
    if (spec == nullptr) {
      add(sink, Severity::Error, ValidationLayer::InvariantViolation, ErrorCode::UnknownField,
          path + ".property",
          "intent constrains a property this runtime does not define: '" + entry.property + "'",
          "unknown properties are rejected rather than ignored");
      continue;
    }
    if (std::find(spec->subjects.begin(), spec->subjects.end(), entry.subject.klass()) ==
        spec->subjects.end()) {
      add(sink, Severity::Error, ValidationLayer::InvariantViolation,
          ErrorCode::AmbiguousSemantics, path + ".subject",
          "property '" + entry.property + "' cannot constrain a " +
              std::string(id_class_name(entry.subject.klass())) + " subject");
      continue;
    }
    std::string why;
    if (!property_value_matches(*spec, entry.desired, why)) {
      add(sink, Severity::Error, ValidationLayer::InvariantViolation,
          ErrorCode::JsonTypeMismatch, path + ".desired",
          "desired value for property '" + entry.property + "' is not valid", why);
    }
    if (entry.mode == IntentMode::Reserved && !entry.tenant.empty()) {
      add(sink, Severity::Warning, ValidationLayer::InvariantViolation,
          ErrorCode::AmbiguousSemantics, path,
          "a reserved intent bound to a tenant reserves capacity without enforcing it");
    }
  }
  for (const auto& policy : document.policies) {
    const auto* isolation = std::get_if<TenantIsolationPolicyDecl>(&policy.body);
    if (isolation == nullptr) {
      continue;
    }
    const TenantDecl* tenant = index.tenant(isolation->tenant);
    if (tenant == nullptr) {
      continue;
    }
    const std::string path = named_path("policies", policy.id.str()) + ".tenant-isolation";
    if (isolation->exclusive_scopes.empty()) {
      add(sink, Severity::Warning, ValidationLayer::InvariantViolation,
          ErrorCode::AmbiguousSemantics, path,
          "tenant-isolation policy declares no exclusive scope and therefore constrains nothing");
    }
  }
}

void sort_diagnostics(std::vector<Diagnostic>& sink) {
  std::sort(sink.begin(), sink.end(), [](const Diagnostic& a, const Diagnostic& b) {
    if (a.layer != b.layer) {
      return static_cast<std::uint8_t>(a.layer) < static_cast<std::uint8_t>(b.layer);
    }
    if (a.code != b.code) {
      return static_cast<std::uint16_t>(a.code) < static_cast<std::uint16_t>(b.code);
    }
    if (a.severity != b.severity) {
      return static_cast<std::uint8_t>(a.severity) > static_cast<std::uint8_t>(b.severity);
    }
    if (a.path != b.path) {
      return a.path < b.path;
    }
    if (a.message != b.message) {
      return a.message < b.message;
    }
    return a.detail < b.detail;
  });
}

}  // namespace

ValidationReport validate_document(const IntentDocument& document, SchemaVersion declared) {
  ValidationReport report;
  report.parsed = true;
  report.declared_schema = declared;
  const auto canonical = canonicalize(document);
  if (!canonical) {
    add(report.diagnostics, Severity::Error, ValidationLayer::Syntax, canonical.error().code, "$",
        canonical.error().message, canonical.error().detail);
    sort_diagnostics(report.diagnostics);
    return report;
  }
  const CanonicalForm& form = canonical.value();
  report.content_digest = form.content_digest;
  report.document_digest = form.document_digest;

  const IntentDocument& normalized = form.normalized;
  const TopologyIndex index(normalized);

  validate_schema_compatibility(normalized, declared, report.diagnostics);
  validate_syntax(normalized, report.diagnostics);
  validate_references(normalized, index, report.diagnostics);
  validate_capabilities(normalized, index, report.diagnostics);
  validate_topology(normalized, index, report.diagnostics);
  validate_policy_conflicts(normalized, index, report.conflicts, report.diagnostics);
  validate_invariants(normalized, index, report.diagnostics);

  if (report.diagnostics.size() >= kMaxDiagnostics) {
    add(report.diagnostics, Severity::Warning, ValidationLayer::InvariantViolation,
        ErrorCode::LimitExceeded, "$", "diagnostic limit reached; further diagnostics suppressed",
        std::to_string(report.diagnostics.size()));
  }
  sort_diagnostics(report.diagnostics);
  std::sort(report.conflicts.begin(), report.conflicts.end(),
            [](const Conflict& a, const Conflict& b) {
              if (a.rule != b.rule) {
                return static_cast<std::uint8_t>(a.rule) < static_cast<std::uint8_t>(b.rule);
              }
              return a.id.local() < b.id.local();
            });
  return report;
}

ValidationReport validate_text(std::string_view text, const JsonParseLimits& limits) {
  ValidationReport report;
  const auto parsed = parse_json(text, limits);
  if (!parsed) {
    add(report.diagnostics, Severity::Error, ValidationLayer::Syntax, parsed.error().code, "$",
        parsed.error().message, parsed.error().detail);
    return report;
  }
  const auto upgraded = upgrade_to_current(parsed.value());
  if (!upgraded) {
    add(report.diagnostics, Severity::Error, ValidationLayer::SchemaCompatibility,
        upgraded.error().code, "$", upgraded.error().message, upgraded.error().detail);
    return report;
  }
  report.applied_migrations = upgraded->applied;
  const auto document = from_json(upgraded->upgraded);
  if (!document) {
    add(report.diagnostics, Severity::Error, ValidationLayer::Syntax, document.error().code, "$",
        document.error().message, document.error().detail);
    return report;
  }
  ValidationReport full = validate_document(document.value(), upgraded->from);
  full.applied_migrations = upgraded->applied;
  return full;
}

}  // namespace ifabric
