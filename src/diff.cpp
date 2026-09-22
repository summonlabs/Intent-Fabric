// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/diff.hpp"

#include "ifabric/canonical.hpp"

#include <algorithm>
#include <map>

namespace ifabric {

std::string_view to_string(DiffCategory value) noexcept {
  switch (value) {
    case DiffCategory::Schema: return "schema";
    case DiffCategory::Provenance: return "provenance";
    case DiffCategory::Topology: return "topology";
    case DiffCategory::CapabilityCatalog: return "capability-catalog";
    case DiffCategory::ServiceClass: return "service-class";
    case DiffCategory::Policy: return "policy";
    case DiffCategory::Tenant: return "tenant";
    case DiffCategory::Workload: return "workload";
    case DiffCategory::IntentObject: return "intent-object";
    case DiffCategory::AdministrativeState: return "administrative-state";
    case DiffCategory::Capacity: return "capacity";
    case DiffCategory::Routing: return "routing";
    case DiffCategory::Redundancy: return "redundancy";
    case DiffCategory::Maintenance: return "maintenance";
    case DiffCategory::Tenancy: return "tenancy";
  }
  return "unknown";
}

std::string_view to_string(Impact value) noexcept {
  switch (value) {
    case Impact::None: return "none";
    case Impact::Additive: return "additive";
    case Impact::Subtractive: return "subtractive";
    case Impact::ServiceAffecting: return "service-affecting";
    case Impact::Disruptive: return "disruptive";
    case Impact::Unknown: return "unknown";
  }
  return "unknown";
}

Impact max_impact(Impact a, Impact b) noexcept {
  return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a : b;
}

JsonValue DiffEntry::to_json() const {
  JsonValue out;
  (void)out.set("path", JsonValue::string(path));
  (void)out.set("change", JsonValue::string(change));
  (void)out.set("category", JsonValue::string(std::string(to_string(category))));
  (void)out.set("impact", JsonValue::string(std::string(to_string(impact))));
  (void)out.set("summary", JsonValue::string(summary));
  if (has_before) {
    (void)out.set("before", before);
  }
  if (has_after) {
    (void)out.set("after", after);
  }
  return out;
}

std::size_t SemanticDiff::count(Impact impact) const {
  std::size_t total = 0;
  for (const auto& entry : entries) {
    if (entry.impact == impact) {
      total = saturating_add(total, 1);
    }
  }
  return total;
}

JsonValue SemanticDiff::to_json() const {
  JsonValue out;
  (void)out.set("from_document", JsonValue::string(from_document.hex()));
  (void)out.set("to_document", JsonValue::string(to_document.hex()));
  (void)out.set("from_content", JsonValue::string(from_content.hex()));
  (void)out.set("to_content", JsonValue::string(to_content.hex()));
  (void)out.set("worst_impact", JsonValue::string(std::string(to_string(worst))));
  (void)out.set("change_count", JsonValue::integer(static_cast<std::int64_t>(entries.size())));
  JsonArray items;
  items.reserve(entries.size());
  for (const auto& entry : entries) {
    items.push_back(entry.to_json());
  }
  (void)out.set("changes", JsonValue::array(std::move(items)));
  return out;
}

std::string SemanticDiff::render() const {
  std::string out = "content ";
  out.append(from_content.short_hex());
  out.append(" -> ");
  out.append(to_content.short_hex());
  out.append("\nworst impact: ");
  out.append(to_string(worst));
  out.append("  (");
  out.append(std::to_string(entries.size()));
  out.append(" change(s))");
  for (const auto& entry : entries) {
    out.push_back('\n');
    out.append("  [");
    out.append(to_string(entry.impact));
    out.append("] ");
    out.append(entry.change);
    out.push_back(' ');
    out.append(entry.category == DiffCategory::Topology ? std::string()
                                                        : std::string(to_string(entry.category)) + " ");
    out.append(entry.path);
    if (!entry.summary.empty()) {
      out.append(": ");
      out.append(entry.summary);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------
namespace {

bool starts_with(const std::string& text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

DiffCategory categorize(const std::string& path) {
  const std::string tail =
      starts_with(path, "$.") ? path.substr(2) : path;
  if (starts_with(tail, "schema") || starts_with(tail, "features")) {
    return DiffCategory::Schema;
  }
  if (starts_with(tail, "provenance")) {
    return DiffCategory::Provenance;
  }
  if (starts_with(tail, "capability_catalog")) {
    return DiffCategory::CapabilityCatalog;
  }
  if (starts_with(tail, "service_classes")) {
    return DiffCategory::ServiceClass;
  }
  if (starts_with(tail, "tenants")) {
    return DiffCategory::Tenant;
  }
  if (starts_with(tail, "workloads")) {
    return DiffCategory::Workload;
  }
  if (starts_with(tail, "intents")) {
    return DiffCategory::IntentObject;
  }
  if (starts_with(tail, "policies")) {
    if (contains(path, ".routing")) return DiffCategory::Routing;
    if (contains(path, ".capacity")) return DiffCategory::Capacity;
    if (contains(path, ".admin-state")) return DiffCategory::AdministrativeState;
    if (contains(path, ".redundancy")) return DiffCategory::Redundancy;
    if (contains(path, ".maintenance-eligibility")) return DiffCategory::Maintenance;
    if (contains(path, ".tenant-isolation")) return DiffCategory::Tenancy;
    return DiffCategory::Policy;
  }
  if (contains(path, ".admin")) {
    return DiffCategory::AdministrativeState;
  }
  if (contains(path, ".capacity_bps") || contains(path, ".speed_bps") ||
      contains(path, ".reserved_bps") || contains(path, ".bandwidth_bps")) {
    return DiffCategory::Capacity;
  }
  return DiffCategory::Topology;
}

Impact impact_for_admin_state(const JsonValue& value) {
  const auto* text = value.try_string();
  if (text == nullptr) {
    return Impact::Unknown;
  }
  if (*text == "enabled") {
    return Impact::Additive;
  }
  if (*text == "disabled") {
    return Impact::Disruptive;
  }
  if (*text == "maintenance" || *text == "draining") {
    return Impact::ServiceAffecting;
  }
  return Impact::Unknown;
}

Impact impact_for_numeric_change(const JsonValue& before, const JsonValue& after) {
  const auto left = before.try_int();
  const auto right = after.try_int();
  if (!left.has_value() || !right.has_value()) {
    return Impact::Unknown;
  }
  if (right.value() == left.value()) {
    return Impact::None;
  }
  return right.value() > left.value() ? Impact::Additive : Impact::ServiceAffecting;
}

Impact classify(const std::string& path, const std::string& change, const JsonValue& before,
                const JsonValue& after) {
  const DiffCategory category = categorize(path);
  if (category == DiffCategory::Provenance) {
    return Impact::None;
  }
  if (change == "added") {
    return Impact::Additive;
  }
  if (change == "removed") {
    if (category == DiffCategory::AdministrativeState) {
      return Impact::Disruptive;
    }
    return Impact::Subtractive;
  }
  // A declared schema version change has no derivable semantic impact; the
  // caller must decide. Feature-list edits are ordinary additions or removals
  // and have already been classified above.
  if (category == DiffCategory::Schema) {
    return Impact::Unknown;
  }
  switch (category) {
    case DiffCategory::AdministrativeState: return impact_for_admin_state(after);
    case DiffCategory::Capacity: return impact_for_numeric_change(before, after);
    case DiffCategory::Redundancy: return impact_for_numeric_change(before, after);
    case DiffCategory::ServiceClass:
      if (contains(path, ".priority") || contains(path, ".bandwidth_share_permille")) {
        return Impact::ServiceAffecting;
      }
      return Impact::Additive;
    case DiffCategory::Routing:
    case DiffCategory::Maintenance:
    case DiffCategory::Tenancy: return Impact::ServiceAffecting;
    case DiffCategory::IntentObject: {
      if (contains(path, "admin_state")) {
        return impact_for_admin_state(after);
      }
      return Impact::ServiceAffecting;
    }
    case DiffCategory::Topology:
    case DiffCategory::Workload:
    case DiffCategory::Tenant:
    case DiffCategory::Policy:
      return Impact::Subtractive;
    default: return Impact::Unknown;
  }
}

std::string summarize(const JsonValue& before, const JsonValue& after, bool has_before,
                      bool has_after) {
  if (has_before && has_after) {
    return before.dump() + " -> " + after.dump();
  }
  if (has_after) {
    return after.dump();
  }
  return before.dump();
}

bool has_identity(const JsonValue& value, std::string& id) {
  const auto* object = value.try_object();
  if (object == nullptr) {
    return false;
  }
  const JsonValue* id_value = value.find("id");
  if (id_value == nullptr) {
    return false;
  }
  const auto* text = id_value->try_string();
  if (text == nullptr || text->empty()) {
    return false;
  }
  id = *text;
  return true;
}

void emit(std::vector<DiffEntry>& entries, const std::string& path, const std::string& change,
          bool has_before, const JsonValue& before, bool has_after, const JsonValue& after) {
  DiffEntry entry;
  entry.path = path;
  entry.change = change;
  entry.category = categorize(path);
  entry.impact = classify(path, change, before, after);
  entry.has_before = has_before;
  entry.has_after = has_after;
  if (has_before) {
    entry.before = before;
  }
  if (has_after) {
    entry.after = after;
  }
  entry.summary = summarize(before, after, has_before, has_after);
  entries.push_back(std::move(entry));
}

void diff_values(const JsonValue& before, const JsonValue& after, const std::string& path,
                 std::vector<DiffEntry>& entries);

void diff_objects(const JsonValue& before, const JsonValue& after, const std::string& path,
                  std::vector<DiffEntry>& entries) {
  const JsonObject& left = *before.try_object();
  const JsonObject& right = *after.try_object();
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left.size() || j < right.size()) {
    if (j >= right.size() || (i < left.size() && left[i].first < right[j].first)) {
      const std::string child = path + "." + left[i].first;
      if (left[i].first == "upgraded_from") {
        ++i;
        continue;
      }
      emit(entries, child, "removed", true, left[i].second, false, JsonValue());
      ++i;
      continue;
    }
    if (i >= left.size() || right[j].first < left[i].first) {
      const std::string child = path + "." + right[j].first;
      if (right[j].first == "upgraded_from") {
        ++j;
        continue;
      }
      emit(entries, child, "added", false, JsonValue(), true, right[j].second);
      ++j;
      continue;
    }
    const std::string child = path + "." + left[i].first;
    if (left[i].first != "upgraded_from") {
      diff_values(left[i].second, right[j].second, child, entries);
    }
    ++i;
    ++j;
  }
}

void diff_arrays(const JsonValue& before, const JsonValue& after, const std::string& path,
                 std::vector<DiffEntry>& entries) {
  const JsonArray& left = *before.try_array();
  const JsonArray& right = *after.try_array();
  bool keyed = !left.empty() && !right.empty();
  for (const auto& item : left) {
    std::string ignored;
    if (!has_identity(item, ignored)) {
      keyed = false;
      break;
    }
  }
  if (keyed) {
    for (const auto& item : right) {
      std::string ignored;
      if (!has_identity(item, ignored)) {
        keyed = false;
        break;
      }
    }
  }
  if (keyed) {
    std::map<std::string, const JsonValue*> left_by_id;
    std::map<std::string, const JsonValue*> right_by_id;
    for (const auto& item : left) {
      std::string id;
      (void)has_identity(item, id);
      left_by_id.emplace(id, &item);
    }
    for (const auto& item : right) {
      std::string id;
      (void)has_identity(item, id);
      right_by_id.emplace(id, &item);
    }
    auto it_left = left_by_id.begin();
    auto it_right = right_by_id.begin();
    while (it_left != left_by_id.end() || it_right != right_by_id.end()) {
      if (it_right == right_by_id.end() ||
          (it_left != left_by_id.end() && it_left->first < it_right->first)) {
        emit(entries, path + "[" + it_left->first + "]", "removed", true, *it_left->second, false,
             JsonValue());
        ++it_left;
        continue;
      }
      if (it_left == left_by_id.end() || it_right->first < it_left->first) {
        emit(entries, path + "[" + it_right->first + "]", "added", false, JsonValue(), true,
             *it_right->second);
        ++it_right;
        continue;
      }
      diff_values(*it_left->second, *it_right->second, path + "[" + it_left->first + "]", entries);
      ++it_left;
      ++it_right;
    }
    return;
  }
  const std::size_t maximum = std::max(left.size(), right.size());
  for (std::size_t index = 0; index < maximum; ++index) {
    const std::string child = path + "[" + std::to_string(index) + "]";
    if (index >= left.size()) {
      emit(entries, child, "added", false, JsonValue(), true, right[index]);
      continue;
    }
    if (index >= right.size()) {
      emit(entries, child, "removed", true, left[index], false, JsonValue());
      continue;
    }
    diff_values(left[index], right[index], child, entries);
  }
}

void diff_values(const JsonValue& before, const JsonValue& after, const std::string& path,
                 std::vector<DiffEntry>& entries) {
  if (before == after) {
    return;
  }
  if (before.is_object() && after.is_object()) {
    diff_objects(before, after, path, entries);
    return;
  }
  if (before.is_array() && after.is_array()) {
    diff_arrays(before, after, path, entries);
    return;
  }
  emit(entries, path, "modified", true, before, true, after);
}

}  // namespace

SemanticDiff diff_documents(const IntentDocument& before, const IntentDocument& after) {
  SemanticDiff out;
  const IntentDocument normalized_before = normalize(before);
  const IntentDocument normalized_after = normalize(after);
  const JsonValue left = to_json(normalized_before);
  const JsonValue right = to_json(normalized_after);
  out.from_document = DocumentDigest::of(left.dump());
  out.to_document = DocumentDigest::of(right.dump());
  out.from_content = ContentDigest::of(content_payload(normalized_before).dump());
  out.to_content = ContentDigest::of(content_payload(normalized_after).dump());
  diff_values(left, right, "$", out.entries);

  // Intent-object paths are enriched with the constrained property so that an
  // entry is self-describing and its semantic impact can be classified without
  // consulting the document that produced it.
  for (auto& entry : out.entries) {
    const std::string marker = ".intents[";
    const std::size_t at = entry.path.find(marker);
    if (at == std::string::npos) {
      continue;
    }
    const std::size_t close = entry.path.find(']', at);
    if (close == std::string::npos) {
      continue;
    }
    if (entry.path.substr(close + 1) != ".desired") {
      continue;
    }
    const std::string id_text =
        entry.path.substr(at + marker.size(), close - at - marker.size());
    const auto id = IntentObjectId::parse(id_text);
    if (!id) {
      continue;
    }
    std::string property;
    for (const auto& intent : normalized_after.intents) {
      if (intent.id == id.value()) {
        property = intent.property;
        break;
      }
    }
    if (property.empty()) {
      continue;
    }
    entry.path.append(".");
    entry.path.append(property);
    entry.category = categorize(entry.path);
    entry.impact = classify(entry.path, entry.change, entry.before, entry.after);
  }

  std::sort(out.entries.begin(), out.entries.end(), [](const DiffEntry& a, const DiffEntry& b) {
    if (a.path != b.path) {
      return a.path < b.path;
    }
    if (a.change != b.change) {
      return a.change < b.change;
    }
    return a.summary < b.summary;
  });
  for (const auto& entry : out.entries) {
    out.worst = max_impact(out.worst, entry.impact);
  }
  return out;
}

}  // namespace ifabric
