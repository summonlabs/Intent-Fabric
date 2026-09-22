// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The declarative intent model. This is the canonical statement of what the
// network should be: topology expectations, routing/path policy, capacity and
// reservation constraints, administrative state, service classes,
// redundancy/failure-domain constraints and maintenance/upgrade eligibility
// hooks. It carries no device configuration syntax and no rollout mechanics --
// those belong to Configuration Fabric, the Change Planner and Rollout Fabric.

#ifndef IFABRIC_MODEL_HPP
#define IFABRIC_MODEL_HPP

#include "ifabric/json.hpp"
#include "ifabric/types.hpp"

#include <variant>

namespace ifabric {

// ---------------------------------------------------------------------------
// Schema identity and version
// ---------------------------------------------------------------------------
inline constexpr std::string_view kSchemaName = "ifabric.intent";

class SchemaVersion {
 public:
  static constexpr std::uint32_t kSupportedMajor = 1;
  static constexpr std::uint32_t kSupportedMinor = 3;
  static constexpr std::uint32_t kOldestMinor = 0;

  SchemaVersion() = default;

  static Result<SchemaVersion> make(std::uint32_t major, std::uint32_t minor) {
    if (major > 0xFFFFu || minor > 0xFFFFu) {
      return Error(ErrorCode::SchemaVersionMalformed, "schema version component out of range");
    }
    return SchemaVersion(static_cast<std::uint16_t>(major), static_cast<std::uint16_t>(minor));
  }

  static Result<SchemaVersion> parse(std::string_view text);
  static Result<SchemaVersion> current() { return make(kSupportedMajor, kSupportedMinor); }

  std::uint32_t major() const noexcept { return major_; }
  std::uint32_t minor() const noexcept { return minor_; }
  bool is_supported_major() const noexcept { return major_ == kSupportedMajor; }
  bool is_supported_minor() const noexcept {
    return minor_ >= kOldestMinor && minor_ <= kSupportedMinor;
  }
  bool is_current() const noexcept {
    return major_ == kSupportedMajor && minor_ == kSupportedMinor;
  }
  std::string str() const;

  friend bool operator==(const SchemaVersion& a, const SchemaVersion& b) noexcept = default;
  friend std::strong_ordering operator<=>(const SchemaVersion& a,
                                          const SchemaVersion& b) noexcept = default;

 private:
  SchemaVersion(std::uint16_t major, std::uint16_t minor) : major_(major), minor_(minor) {}
  std::uint16_t major_ = kSupportedMajor;
  std::uint16_t minor_ = kSupportedMinor;
};

struct FeatureInfo {
  std::string_view name;
  std::uint32_t introduced_minor;
  bool requires_opt_in;
};

const std::vector<FeatureInfo>& known_features();
bool is_known_feature(std::string_view name);
std::optional<FeatureInfo> feature_info(std::string_view name);

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------
enum class AdminState : std::uint8_t { Enabled = 0, Disabled = 1, Maintenance = 2, Draining = 3 };
enum class DeviceRole : std::uint8_t {
  Leaf = 0,
  Spine = 1,
  SuperSpine = 2,
  Border = 3,
  Compute = 4,
  Storage = 5,
  Appliance = 6
};
enum class PortRole : std::uint8_t { Fabric = 0, Uplink = 1, Access = 2, Peer = 3 };
enum class LinkKind : std::uint8_t { Fabric = 0, Uplink = 1, Peer = 2, Host = 3 };
enum class IntentMode : std::uint8_t { Enforce = 0, Advisory = 1, Reserved = 2 };
enum class FailureDomainLevel : std::uint8_t {
  Link = 0,
  Port = 1,
  Device = 2,
  Rack = 3,
  Pod = 4,
  Site = 5,
  Fabric = 6
};
enum class RoutingAction : std::uint8_t {
  ShortestPath = 0,
  ExplicitPath = 1,
  PinnedPath = 2,
  Drop = 3
};
enum class PolicyKind : std::uint8_t {
  Routing = 0,
  Capacity = 1,
  AdminState = 2,
  Redundancy = 3,
  MaintenanceEligibility = 4,
  TenantIsolation = 5
};

std::string_view to_string(AdminState value) noexcept;
std::string_view to_string(DeviceRole value) noexcept;
std::string_view to_string(PortRole value) noexcept;
std::string_view to_string(LinkKind value) noexcept;
std::string_view to_string(IntentMode value) noexcept;
std::string_view to_string(FailureDomainLevel value) noexcept;
std::string_view to_string(RoutingAction value) noexcept;
std::string_view to_string(PolicyKind value) noexcept;

Result<AdminState> parse_admin_state(std::string_view text);
Result<DeviceRole> parse_device_role(std::string_view text);
Result<PortRole> parse_port_role(std::string_view text);
Result<LinkKind> parse_link_kind(std::string_view text);
Result<IntentMode> parse_intent_mode(std::string_view text);
Result<FailureDomainLevel> parse_failure_domain_level(std::string_view text);
Result<RoutingAction> parse_routing_action(std::string_view text);
Result<PolicyKind> parse_policy_kind(std::string_view text);

Result<std::uint64_t> parse_speed_bps(std::string_view text);

// ---------------------------------------------------------------------------
// Meta map: sorted, unique, scalar-valued annotations.
// ---------------------------------------------------------------------------
using MetaMap = std::vector<std::pair<std::string, JsonValue>>;
Result<MetaMap> meta_from_json(const JsonValue& value, std::string_view path);
JsonValue meta_to_json(const MetaMap& meta);

// ---------------------------------------------------------------------------
// Topology expectations
// ---------------------------------------------------------------------------
struct FabricDecl {
  FabricId id;
  std::string display_name;
  std::string description;
};

struct SiteDecl {
  SiteId id;
  FabricId fabric;
  std::string display_name;
  std::string region;
  std::string description;
};

struct PodDecl {
  PodId id;
  SiteId site;
  std::string display_name;
  std::string description;
};

struct RackDecl {
  RackId id;
  PodId pod;
  std::uint32_t position = 0;
  std::string description;
};

struct DeviceDecl {
  DeviceId id;
  SiteId site;
  PodId pod;
  RackId rack;
  DeviceRole role = DeviceRole::Leaf;
  std::string model;
  AdminState admin = AdminState::Enabled;
  std::uint32_t declared_port_count = 0;
  std::vector<ServiceClassId> service_classes;
  MetaMap meta;
  std::string description;
};

struct PortDecl {
  PortId id;
  DeviceId device;
  std::uint32_t index = 0;
  PortRole role = PortRole::Fabric;
  AdminState admin = AdminState::Enabled;
  std::uint64_t speed_bps = 0;
  std::string media;
  std::vector<ServiceClassId> service_classes;
  std::string description;
};

struct LinkDecl {
  LinkId id;
  PortId endpoint_a;
  PortId endpoint_b;
  LinkKind kind = LinkKind::Fabric;
  std::uint64_t capacity_bps = 0;
  AdminState admin = AdminState::Enabled;
  std::vector<ServiceClassId> service_classes;
  std::string description;
};

struct CapabilityDecl {
  std::string model;
  std::vector<std::string> capabilities;
  std::uint32_t port_count = 0;
  std::uint64_t max_speed_bps = 0;
  std::vector<DeviceRole> supported_roles;
};

// ---------------------------------------------------------------------------
// Service classes
// ---------------------------------------------------------------------------
struct ServiceClassDecl {
  ServiceClassId id;
  std::string display_name;
  std::uint32_t priority = 0;                     // higher value wins
  std::uint32_t bandwidth_share_permille = 0;     // 0..1000
  std::uint64_t latency_budget_us = 0;            // 0 == unconstrained
  std::uint32_t loss_budget_ppm = 0;              // 0 == unconstrained
  bool lossless = false;
  std::vector<std::string> required_capabilities;
  std::string description;
};

// ---------------------------------------------------------------------------
// Policy objects
// ---------------------------------------------------------------------------
struct RoutingPolicyDecl {
  std::uint32_t priority = 0;
  std::vector<ServiceClassId> match_service_classes;
  std::vector<TenantId> match_tenants;
  std::string match_source;       // AnyId text, empty means "any"
  std::string match_destination;  // AnyId text, empty means "any"
  RoutingAction action = RoutingAction::ShortestPath;
  std::vector<std::string> explicit_path;  // AnyId texts, ordered hops
  std::uint32_t max_ecmp_width = 1;
  std::uint64_t min_bandwidth_bps = 0;
  std::string extends_policy;  // PolicyId text of a routing policy this one composes
  std::string description;
};

struct CapacityPolicyDecl {
  std::string scope;  // AnyId text
  std::vector<ServiceClassId> service_classes;
  std::uint64_t reserved_bps = 0;
  std::uint32_t max_utilization_permille = 1000;
  std::string description;
};

struct AdminStatePolicyDecl {
  std::string scope;  // AnyId text
  AdminState state = AdminState::Enabled;
  bool drain_first = false;
  std::string reason;
  std::string description;
};

struct RedundancyPolicyDecl {
  std::string scope;  // AnyId text
  FailureDomainLevel level = FailureDomainLevel::Device;
  std::uint32_t min_disjoint_paths = 1;
  std::vector<ServiceClassId> service_classes;
  std::string description;
};

struct MaintenanceEligibilityPolicyDecl {
  std::string scope;  // AnyId text
  std::vector<std::string> upgrade_groups;
  std::uint32_t max_concurrent_operations = 1;
  bool drain_required = true;
  bool requires_redundancy_headroom = true;
  std::vector<std::string> allowed_impact;
  std::string description;
};

struct TenantIsolationPolicyDecl {
  TenantId tenant;
  std::vector<std::string> exclusive_scopes;  // AnyId texts
  bool forbid_shared_links = true;
  std::string description;
};

using PolicyBody =
    std::variant<RoutingPolicyDecl, CapacityPolicyDecl, AdminStatePolicyDecl, RedundancyPolicyDecl,
                 MaintenanceEligibilityPolicyDecl, TenantIsolationPolicyDecl>;

struct PolicyDecl {
  PolicyId id;
  PolicyBody body;
};

PolicyKind policy_kind(const PolicyDecl& policy) noexcept;
const char* policy_body_key(const PolicyDecl& policy) noexcept;

// ---------------------------------------------------------------------------
// Tenants and workloads
// ---------------------------------------------------------------------------
struct TenantDecl {
  TenantId id;
  std::string display_name;
  std::uint32_t isolation_id = 0;
  std::string description;
};

struct WorkloadDecl {
  WorkloadId id;
  TenantId tenant;
  ServiceClassId service_class;
  std::vector<DeviceId> placement;
  std::uint64_t bandwidth_bps = 0;
  std::uint32_t replicas = 1;
  FailureDomainLevel redundancy_level = FailureDomainLevel::Device;
  std::uint32_t min_disjoint_domains = 1;
  std::vector<SiteId> allowed_sites;
  std::string description;
};

// ---------------------------------------------------------------------------
// Intent objects: declarative statements binding a subject to a desired
// property value.
// ---------------------------------------------------------------------------
struct IntentObjectDecl {
  IntentObjectId id;
  AnyId subject;
  std::string property;
  JsonValue desired;
  IntentMode mode = IntentMode::Enforce;
  std::uint32_t priority = 0;
  std::string tenant;  // TenantId text, empty means "no tenant"
  std::string description;
};

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------
struct ParentIntent {
  GenerationId generation;
  ContentDigest content;
};

struct Provenance {
  ActorId actor;
  SourceRevision source_revision;
  std::optional<ParentIntent> parent_intent;
  Timestamp created_at{};
  std::string description;
  MetaMap labels;
};

// ---------------------------------------------------------------------------
// The intent document
// ---------------------------------------------------------------------------
struct IntentDocument {
  SchemaVersion schema;
  std::vector<std::string> required_features;
  std::vector<std::string> optional_features;
  DomainId domain;
  Provenance provenance;

  std::vector<FabricDecl> fabrics;
  std::vector<SiteDecl> sites;
  std::vector<PodDecl> pods;
  std::vector<RackDecl> racks;
  std::vector<DeviceDecl> devices;
  std::vector<PortDecl> ports;
  std::vector<LinkDecl> links;
  std::vector<CapabilityDecl> capability_catalog;
  std::vector<ServiceClassDecl> service_classes;
  std::vector<PolicyDecl> policies;
  std::vector<TenantDecl> tenants;
  std::vector<WorkloadDecl> workloads;
  std::vector<IntentObjectDecl> intents;

  // Number of declared objects, used for the bounded-objects check.
  std::size_t declared_object_count() const noexcept;
};

// ---------------------------------------------------------------------------
// JSON projection
// ---------------------------------------------------------------------------
JsonValue to_json(const IntentDocument& document);
Result<IntentDocument> from_json(const JsonValue& value);

// Schema-version compatibility. Applies documented structural migrations to
// reach the current minor version. Returns the upgraded tree plus the list of
// applied migration labels.
struct SchemaUpgrade {
  JsonValue upgraded;
  std::vector<std::string> applied;
  SchemaVersion from;
};

Result<SchemaUpgrade> upgrade_to_current(const JsonValue& value);

// The semantic payload used for content identity: everything except volatile
// provenance (actor, source revision, creation time, labels, description).
JsonValue content_payload(const IntentDocument& document);

}  // namespace ifabric

#endif  // IFABRIC_MODEL_HPP
