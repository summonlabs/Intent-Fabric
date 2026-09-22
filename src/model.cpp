// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/model.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace ifabric {

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------
namespace {

template <class E, std::size_t N>
Result<E> parse_enum(std::string_view text,
                     const std::array<std::pair<std::string_view, E>, N>& table,
                     std::string_view what) {
  for (const auto& row : table) {
    if (row.first == text) {
      return row.second;
    }
  }
  return Error(ErrorCode::UnknownField, "unknown " + std::string(what) + " value",
               std::string(text));
}

template <class E, std::size_t N>
std::string_view enum_text(E value, const std::array<std::pair<std::string_view, E>, N>& table) {
  for (const auto& row : table) {
    if (row.second == value) {
      return row.first;
    }
  }
  return "?";
}

constexpr std::array<std::pair<std::string_view, AdminState>, 4> kAdminStates = {{
    {"enabled", AdminState::Enabled},
    {"disabled", AdminState::Disabled},
    {"maintenance", AdminState::Maintenance},
    {"draining", AdminState::Draining},
}};

constexpr std::array<std::pair<std::string_view, DeviceRole>, 7> kDeviceRoles = {{
    {"leaf", DeviceRole::Leaf},
    {"spine", DeviceRole::Spine},
    {"super-spine", DeviceRole::SuperSpine},
    {"border", DeviceRole::Border},
    {"compute", DeviceRole::Compute},
    {"storage", DeviceRole::Storage},
    {"appliance", DeviceRole::Appliance},
}};

constexpr std::array<std::pair<std::string_view, PortRole>, 4> kPortRoles = {{
    {"fabric", PortRole::Fabric},
    {"uplink", PortRole::Uplink},
    {"access", PortRole::Access},
    {"peer", PortRole::Peer},
}};

constexpr std::array<std::pair<std::string_view, LinkKind>, 4> kLinkKinds = {{
    {"fabric", LinkKind::Fabric},
    {"uplink", LinkKind::Uplink},
    {"peer", LinkKind::Peer},
    {"host", LinkKind::Host},
}};

constexpr std::array<std::pair<std::string_view, IntentMode>, 3> kIntentModes = {{
    {"enforce", IntentMode::Enforce},
    {"advisory", IntentMode::Advisory},
    {"reserved", IntentMode::Reserved},
}};

constexpr std::array<std::pair<std::string_view, FailureDomainLevel>, 7> kFailureDomains = {{
    {"link", FailureDomainLevel::Link},
    {"port", FailureDomainLevel::Port},
    {"device", FailureDomainLevel::Device},
    {"rack", FailureDomainLevel::Rack},
    {"pod", FailureDomainLevel::Pod},
    {"site", FailureDomainLevel::Site},
    {"fabric", FailureDomainLevel::Fabric},
}};

constexpr std::array<std::pair<std::string_view, RoutingAction>, 4> kRoutingActions = {{
    {"shortest-path", RoutingAction::ShortestPath},
    {"explicit-path", RoutingAction::ExplicitPath},
    {"pinned-path", RoutingAction::PinnedPath},
    {"drop", RoutingAction::Drop},
}};

constexpr std::array<std::pair<std::string_view, PolicyKind>, 6> kPolicyKinds = {{
    {"routing", PolicyKind::Routing},
    {"capacity", PolicyKind::Capacity},
    {"admin-state", PolicyKind::AdminState},
    {"redundancy", PolicyKind::Redundancy},
    {"maintenance-eligibility", PolicyKind::MaintenanceEligibility},
    {"tenant-isolation", PolicyKind::TenantIsolation},
}};

}  // namespace

std::string_view to_string(AdminState value) noexcept { return enum_text(value, kAdminStates); }
std::string_view to_string(DeviceRole value) noexcept { return enum_text(value, kDeviceRoles); }
std::string_view to_string(PortRole value) noexcept { return enum_text(value, kPortRoles); }
std::string_view to_string(LinkKind value) noexcept { return enum_text(value, kLinkKinds); }
std::string_view to_string(IntentMode value) noexcept { return enum_text(value, kIntentModes); }
std::string_view to_string(FailureDomainLevel value) noexcept {
  return enum_text(value, kFailureDomains);
}
std::string_view to_string(RoutingAction value) noexcept {
  return enum_text(value, kRoutingActions);
}
std::string_view to_string(PolicyKind value) noexcept { return enum_text(value, kPolicyKinds); }

Result<AdminState> parse_admin_state(std::string_view text) {
  return parse_enum(text, kAdminStates, "administrative state");
}
Result<DeviceRole> parse_device_role(std::string_view text) {
  return parse_enum(text, kDeviceRoles, "device role");
}
Result<PortRole> parse_port_role(std::string_view text) {
  return parse_enum(text, kPortRoles, "port role");
}
Result<LinkKind> parse_link_kind(std::string_view text) {
  return parse_enum(text, kLinkKinds, "link kind");
}
Result<IntentMode> parse_intent_mode(std::string_view text) {
  return parse_enum(text, kIntentModes, "intent mode");
}
Result<FailureDomainLevel> parse_failure_domain_level(std::string_view text) {
  return parse_enum(text, kFailureDomains, "failure domain level");
}
Result<RoutingAction> parse_routing_action(std::string_view text) {
  return parse_enum(text, kRoutingActions, "routing action");
}
Result<PolicyKind> parse_policy_kind(std::string_view text) {
  return parse_enum(text, kPolicyKinds, "policy kind");
}

Result<std::uint64_t> parse_speed_bps(std::string_view text) {
  std::string_view body = trim_ascii(text);
  if (body.empty()) {
    return Error(ErrorCode::UnknownField, "empty link speed");
  }
  std::uint64_t multiplier = 1;
  if (body.back() == 'g' || body.back() == 'G') {
    multiplier = 1000000000ull;
    body.remove_suffix(1);
  } else if (body.back() == 'm' || body.back() == 'M') {
    multiplier = 1000000ull;
    body.remove_suffix(1);
  }
  if (body.empty() || body.size() > 18) {
    return Error(ErrorCode::UnknownField, "malformed link speed", std::string(text));
  }
  std::uint64_t value = 0;
  for (char c : body) {
    if (c < '0' || c > '9') {
      return Error(ErrorCode::UnknownField, "malformed link speed", std::string(text));
    }
    value = value * 10ull + static_cast<std::uint64_t>(c - '0');
    if (value > 1000000000000ull) {
      return Error(ErrorCode::UnknownField, "link speed is out of range", std::string(text));
    }
  }
  const auto scaled = checked_mul(value, multiplier);
  if (!scaled) {
    return scaled.error();
  }
  return scaled.value();
}

// ---------------------------------------------------------------------------
// Schema version and features
// ---------------------------------------------------------------------------
Result<SchemaVersion> SchemaVersion::parse(std::string_view text) {
  const std::size_t dot = text.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= text.size()) {
    return Error(ErrorCode::SchemaVersionMalformed, "schema version must be <major>.<minor>",
                 std::string(text));
  }
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  for (std::size_t i = 0; i < dot; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return Error(ErrorCode::SchemaVersionMalformed, "schema major version must be numeric",
                   std::string(text));
    }
    major = major * 10u + static_cast<std::uint32_t>(c - '0');
    if (major > 0xFFFFu) {
      return Error(ErrorCode::SchemaVersionMalformed, "schema major version out of range",
                   std::string(text));
    }
  }
  for (std::size_t i = dot + 1; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return Error(ErrorCode::SchemaVersionMalformed, "schema minor version must be numeric",
                   std::string(text));
    }
    minor = minor * 10u + static_cast<std::uint32_t>(c - '0');
    if (minor > 0xFFFFu) {
      return Error(ErrorCode::SchemaVersionMalformed, "schema minor version out of range",
                   std::string(text));
    }
  }
  return make(major, minor);
}

std::string SchemaVersion::str() const {
  return std::to_string(major_) + "." + std::to_string(minor_);
}

const std::vector<FeatureInfo>& known_features() {
  static const std::vector<FeatureInfo> kFeatures = {
      {"topology.multi-site", 0, false},
      {"topology.placement", 0, false},
      {"routing.explicit-path", 0, false},
      {"routing.path-pinning", 1, false},
      {"capacity.reservation", 0, false},
      {"redundancy.disjoint-paths", 0, false},
      {"tenancy.isolation", 0, false},
      {"maintenance.upgrade-groups", 2, false},
      {"maintenance.redundancy-headroom", 2, false},
      {"telemetry.flow-counters", 3, true},
  };
  return kFeatures;
}

bool is_known_feature(std::string_view name) {
  for (const auto& feature : known_features()) {
    if (feature.name == name) {
      return true;
    }
  }
  return false;
}

std::optional<FeatureInfo> feature_info(std::string_view name) {
  for (const auto& feature : known_features()) {
    if (feature.name == name) {
      return feature;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Meta maps
// ---------------------------------------------------------------------------
Result<MetaMap> meta_from_json(const JsonValue& value, std::string_view path) {
  const auto* members = value.try_object();
  if (members == nullptr) {
    return Error(ErrorCode::JsonTypeMismatch,
                 "expected an object at " + std::string(path) + ", found " +
                     std::string(value.kind_name()));
  }
  if (members->size() > kMaxMetaEntries) {
    return Error(ErrorCode::LimitExceeded,
                 "meta map at " + std::string(path) + " exceeds the entry limit");
  }
  MetaMap out;
  out.reserve(members->size());
  for (const auto& member : *members) {
    if (member.first.empty() || member.first.size() > kMaxStringLength) {
      return Error(ErrorCode::LimitExceeded,
                   "meta key at " + std::string(path) + " has an invalid length", member.first);
    }
    if (!member.second.is_string() && !member.second.is_bool() && !member.second.is_number()) {
      return Error(ErrorCode::JsonTypeMismatch,
                   "meta value at " + std::string(path) + "." + member.first +
                       " must be a scalar (string, integer, number or boolean)");
    }
    out.emplace_back(member.first, member.second);
  }
  return out;
}

JsonValue meta_to_json(const MetaMap& meta) {
  // An empty meta map is an empty object, never null: a null would not survive
  // a round trip through the strict parser.
  JsonValue out = JsonValue::object({}).value();
  for (const auto& entry : meta) {
    (void)out.set(entry.first, entry.second);
  }
  return out;
}

}  // namespace ifabric
