// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// JSON projection of the intent model, schema-version migration, and content
// identity.

#include "ifabric/model.hpp"

#include <algorithm>

namespace ifabric {

namespace {

std::string child_path(const std::string& base, std::string_view name) {
  std::string out = base;
  out.push_back('.');
  out.append(name);
  return out;
}

Error type_error(const std::string& path, const JsonValue& value, std::string_view expected) {
  return Error(ErrorCode::JsonTypeMismatch,
               "expected " + std::string(expected) + " at " + path + ", found " +
                   std::string(value.kind_name()));
}

bool within_limit(const std::string& text) { return text.size() <= kMaxStringLength; }

Error read_string(const JsonValue* value, const std::string& path, std::string& out) {
  if (value == nullptr) {
    return Error();
  }
  if (value->is_null()) {
    out.clear();
    return Error();
  }
  const auto* text = value->try_string();
  if (text == nullptr) {
    return type_error(path, *value, "a string");
  }
  if (!within_limit(*text)) {
    return Error(ErrorCode::LimitExceeded, "string at " + path + " exceeds the length limit");
  }
  out = *text;
  return Error();
}

Error read_bool(const JsonValue* value, const std::string& path, bool& out) {
  if (value == nullptr) {
    return Error();
  }
  const auto flag = value->try_bool();
  if (!flag.has_value()) {
    return type_error(path, *value, "a boolean");
  }
  out = *flag;
  return Error();
}

Error read_u32(const JsonValue* value, const std::string& path, std::uint32_t& out) {
  if (value == nullptr) {
    return Error();
  }
  const auto number = value->try_int();
  if (!number.has_value()) {
    return type_error(path, *value, "an integer");
  }
  if (number.value() < 0 || number.value() > 0xFFFFFFFFll) {
    return Error(ErrorCode::JsonNumberOutOfRange, "integer at " + path + " is out of range",
                 std::to_string(number.value()));
  }
  out = static_cast<std::uint32_t>(number.value());
  return Error();
}

Error read_u64(const JsonValue* value, const std::string& path, std::uint64_t& out) {
  if (value == nullptr) {
    return Error();
  }
  const auto number = value->try_int();
  if (!number.has_value()) {
    return type_error(path, *value, "an integer");
  }
  if (number.value() < 0) {
    return Error(ErrorCode::JsonNumberOutOfRange, "integer at " + path + " must not be negative",
                 std::to_string(number.value()));
  }
  out = static_cast<std::uint64_t>(number.value());
  return Error();
}

template <IdClass K>
Error read_id(const JsonValue* value, const std::string& path, BasicId<K>& out) {
  if (value == nullptr) {
    return Error();
  }
  if (value->is_null()) {
    out = BasicId<K>();
    return Error();
  }
  const auto* text = value->try_string();
  if (text == nullptr) {
    return type_error(path, *value, "an identity string");
  }
  if (text->empty()) {
    out = BasicId<K>();
    return Error();
  }
  const auto parsed = BasicId<K>::parse(*text);
  if (!parsed) {
    return Error(parsed.error().code, parsed.error().message + " at " + path,
                 parsed.error().detail);
  }
  out = parsed.value();
  return Error();
}

Error read_any_id(const JsonValue* value, const std::string& path, std::string& out) {
  if (value == nullptr) {
    return Error();
  }
  if (value->is_null()) {
    out.clear();
    return Error();
  }
  const auto* text = value->try_string();
  if (text == nullptr) {
    return type_error(path, *value, "an identity string");
  }
  if (!text->empty()) {
    const auto parsed = AnyId::parse(*text);
    if (!parsed) {
      return Error(parsed.error().code, parsed.error().message + " at " + path,
                   parsed.error().detail);
    }
  }
  out = *text;
  return Error();
}

template <class E, class Parser>
Error read_enum(const JsonValue* value, const std::string& path, E& out, Parser parser) {
  if (value == nullptr) {
    return Error();
  }
  const auto* text = value->try_string();
  if (text == nullptr) {
    return type_error(path, *value, "an enumeration string");
  }
  const auto parsed = parser(*text);
  if (!parsed) {
    return Error(parsed.error().code, parsed.error().message + " at " + path,
                 parsed.error().detail);
  }
  out = parsed.value();
  return Error();
}

template <IdClass K>
Error read_id_array(const JsonValue* value, const std::string& path, std::vector<BasicId<K>>& out) {
  if (value == nullptr) {
    return Error();
  }
  const auto* items = value->try_array();
  if (items == nullptr) {
    return type_error(path, *value, "an array of identities");
  }
  if (items->size() > kMaxCollectionMembers) {
    return Error(ErrorCode::LimitExceeded, "array at " + path + " exceeds the element limit");
  }
  out.clear();
  out.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    BasicId<K> id;
    const Error error = read_id<K>(&(*items)[i], path + "[" + std::to_string(i) + "]", id);
    if (!error.ok()) {
      return error;
    }
    out.push_back(id);
  }
  return Error();
}

Error read_string_array(const JsonValue* value, const std::string& path,
                        std::vector<std::string>& out) {
  if (value == nullptr) {
    return Error();
  }
  const auto* items = value->try_array();
  if (items == nullptr) {
    return type_error(path, *value, "an array of strings");
  }
  if (items->size() > kMaxCollectionMembers) {
    return Error(ErrorCode::LimitExceeded, "array at " + path + " exceeds the element limit");
  }
  out.clear();
  out.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const auto* text = (*items)[i].try_string();
    if (text == nullptr) {
      return type_error(path + "[" + std::to_string(i) + "]", (*items)[i], "a string");
    }
    if (!within_limit(*text)) {
      return Error(ErrorCode::LimitExceeded, "string in " + path + " exceeds the length limit");
    }
    out.push_back(*text);
  }
  return Error();
}

// Object reader that enforces "no unknown fields".
class ObjReader {
 public:
  ObjReader(const JsonValue& value, std::string path) : path_(std::move(path)) {
    members_ = value.try_object();
    if (members_ == nullptr) {
      error_ = Error(ErrorCode::JsonTypeMismatch,
                     "expected an object at " + path_ + ", found " +
                         std::string(value.kind_name()));
    }
  }

  bool ok() const { return error_.ok(); }
  const Error& error() const { return error_; }
  const std::string& path() const { return path_; }

  void set_error(Error error) {
    if (error_.ok()) {
      error_ = std::move(error);
    }
  }

  const JsonValue* take(std::string_view name) {
    if (!ok()) {
      return nullptr;
    }
    used_.emplace_back(name);
    const auto it = std::lower_bound(
        members_->begin(), members_->end(), name,
        [](const JsonMember& member, std::string_view needle) { return member.first < needle; });
    if (it == members_->end() || it->first != name) {
      return nullptr;
    }
    return &it->second;
  }

  const JsonValue* take_required(std::string_view name) {
    const JsonValue* value = take(name);
    if (!ok()) {
      return nullptr;
    }
    if (value == nullptr) {
      set_error(Error(ErrorCode::MissingField,
                      "missing required field " + child_path(path_, name)));
    }
    return value;
  }

  Error finish() const {
    if (!error_.ok()) {
      return error_;
    }
    for (const auto& member : *members_) {
      bool known = false;
      for (const auto& used : used_) {
        if (used == member.first) {
          known = true;
          break;
        }
      }
      if (!known) {
        return Error(ErrorCode::UnknownField,
                     "unknown field " + child_path(path_, member.first) +
                         " (this schema version does not define it)");
      }
    }
    return Error();
  }

 private:
  const JsonObject* members_ = nullptr;
  std::string path_;
  std::vector<std::string> used_;
  Error error_;
};

void read_into(ObjReader& reader, Error error) { reader.set_error(std::move(error)); }

const JsonValue* take_array(ObjReader& reader, std::string_view name, bool required) {
  const JsonValue* value = required ? reader.take_required(name) : reader.take(name);
  if (!reader.ok() || value == nullptr) {
    return nullptr;
  }
  if (value->try_array() == nullptr) {
    reader.set_error(type_error(child_path(reader.path(), name), *value, "an array"));
    return nullptr;
  }
  return value;
}

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------
Error parse_schema(const JsonValue& value, IntentDocument& document) {
  ObjReader reader(value, "schema");
  if (!reader.ok()) {
    return reader.error();
  }
  std::string name;
  read_into(reader, read_string(reader.take_required("name"), "schema.name", name));
  if (reader.ok() && name != kSchemaName) {
    reader.set_error(Error(ErrorCode::SchemaVersionMalformed, "unexpected schema name",
                           child_path("schema.name", name)));
  }
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  read_into(reader, read_u32(reader.take_required("major"), "schema.major", major));
  read_into(reader, read_u32(reader.take_required("minor"), "schema.minor", minor));
  if (reader.ok()) {
    const auto version = SchemaVersion::make(major, minor);
    if (!version) {
      reader.set_error(version.error());
    } else {
      document.schema = version.value();
    }
  }
  read_into(reader, read_string_array(reader.take("requires"), "schema.requires",
                                      document.required_features));
  read_into(reader, read_string_array(reader.take("optional"), "schema.optional",
                                      document.optional_features));
  const JsonValue* upgraded = reader.take("upgraded_from");
  if (upgraded != nullptr && !upgraded->is_null()) {
    const auto* text = upgraded->try_string();
    if (text == nullptr) {
      reader.set_error(type_error("schema.upgraded_from", *upgraded, "a string"));
    }
  }
  if (!reader.ok()) {
    return reader.error();
  }
  return reader.finish();
}

Error parse_provenance(const JsonValue& value, Provenance& out) {
  ObjReader reader(value, "provenance");
  if (!reader.ok()) {
    return reader.error();
  }
  read_into(reader, read_id<IdClass::Actor>(reader.take("actor"), "provenance.actor", out.actor));
  std::string revision;
  read_into(reader, read_string(reader.take("source_revision"), "provenance.source_revision",
                                revision));
  if (reader.ok() && !revision.empty()) {
    const auto parsed = SourceRevision::parse(revision);
    if (!parsed) {
      reader.set_error(parsed.error());
    } else {
      out.source_revision = parsed.value();
    }
  }
  const JsonValue* parent = reader.take("parent_intent");
  if (parent != nullptr && !parent->is_null()) {
    ObjReader parent_reader(*parent, "provenance.parent_intent");
    if (!parent_reader.ok()) {
      reader.set_error(parent_reader.error());
    } else {
      ParentIntent entry;
      read_into(parent_reader,
                read_id<IdClass::Generation>(parent_reader.take_required("generation"),
                                             "provenance.parent_intent.generation",
                                             entry.generation));
      std::string digest_text;
      read_into(parent_reader,
                read_string(parent_reader.take("content_digest"),
                            "provenance.parent_intent.content_digest", digest_text));
      if (parent_reader.ok() && !digest_text.empty()) {
        const auto digest = ContentDigest::from_hex(digest_text);
        if (!digest) {
          parent_reader.set_error(digest.error());
        } else {
          entry.content = digest.value();
        }
      }
      read_into(parent_reader, parent_reader.finish());
      if (!parent_reader.ok()) {
        reader.set_error(parent_reader.error());
      } else {
        out.parent_intent = entry;
      }
    }
  }
  std::string created_at;
  read_into(reader, read_string(reader.take("created_at"), "provenance.created_at", created_at));
  if (reader.ok() && !created_at.empty()) {
    const auto parsed = parse_timestamp(created_at);
    if (!parsed) {
      reader.set_error(parsed.error());
    } else {
      out.created_at = parsed.value();
    }
  }
  read_into(reader, read_string(reader.take("description"), "provenance.description",
                                out.description));
  const JsonValue* labels = reader.take("labels");
  if (labels != nullptr && !labels->is_null()) {
    const auto parsed = meta_from_json(*labels, "provenance.labels");
    if (!parsed) {
      reader.set_error(parsed.error());
    } else {
      out.labels = parsed.value();
    }
  }
  if (!reader.ok()) {
    return reader.error();
  }
  return reader.finish();
}

Error parse_meta_field(ObjReader& reader, std::string_view name, MetaMap& out) {
  const JsonValue* value = reader.take(name);
  if (value == nullptr || !reader.ok()) {
    return Error();
  }
  const auto parsed = meta_from_json(*value, child_path(reader.path(), name));
  if (!parsed) {
    return parsed.error();
  }
  out = parsed.value();
  return Error();
}

Error parse_fabrics(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("topology.fabrics", value, "an array");
  }
  document.fabrics.clear();
  document.fabrics.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "topology.fabrics[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    FabricDecl entry;
    read_into(reader, read_id<IdClass::Fabric>(reader.take_required("id"), path + ".id", entry.id));
    read_into(reader, read_string(reader.take("display_name"), path + ".display_name",
                                  entry.display_name));
    read_into(reader, read_string(reader.take("description"), path + ".description",
                                  entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.fabrics.push_back(std::move(entry));
  }
  return Error();
}

Error parse_sites(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("topology.sites", value, "an array");
  }
  document.sites.clear();
  document.sites.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "topology.sites[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    SiteDecl entry;
    read_into(reader, read_id<IdClass::Site>(reader.take_required("id"), path + ".id", entry.id));
    read_into(reader, read_id<IdClass::Fabric>(reader.take_required("fabric"), path + ".fabric",
                                               entry.fabric));
    read_into(reader,
              read_string(reader.take("display_name"), path + ".display_name", entry.display_name));
    read_into(reader, read_string(reader.take("region"), path + ".region", entry.region));
    read_into(reader,
              read_string(reader.take("description"), path + ".description", entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.sites.push_back(std::move(entry));
  }
  return Error();
}

Error parse_pods(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("topology.pods", value, "an array");
  }
  document.pods.clear();
  document.pods.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "topology.pods[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    PodDecl entry;
    read_into(reader, read_id<IdClass::Pod>(reader.take_required("id"), path + ".id", entry.id));
    read_into(reader,
              read_id<IdClass::Site>(reader.take_required("site"), path + ".site", entry.site));
    read_into(reader,
              read_string(reader.take("display_name"), path + ".display_name", entry.display_name));
    read_into(reader,
              read_string(reader.take("description"), path + ".description", entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.pods.push_back(std::move(entry));
  }
  return Error();
}

Error parse_racks(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("topology.racks", value, "an array");
  }
  document.racks.clear();
  document.racks.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "topology.racks[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    RackDecl entry;
    read_into(reader, read_id<IdClass::Rack>(reader.take_required("id"), path + ".id", entry.id));
    read_into(reader, read_id<IdClass::Pod>(reader.take_required("pod"), path + ".pod", entry.pod));
    read_into(reader, read_u32(reader.take("position"), path + ".position", entry.position));
    read_into(reader,
              read_string(reader.take("description"), path + ".description", entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.racks.push_back(std::move(entry));
  }
  return Error();
}

Error parse_devices(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("topology.devices", value, "an array");
  }
  document.devices.clear();
  document.devices.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "topology.devices[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    DeviceDecl entry;
    read_into(reader,
              read_id<IdClass::Device>(reader.take_required("id"), path + ".id", entry.id));
    read_into(reader,
              read_id<IdClass::Site>(reader.take_required("site"), path + ".site", entry.site));
    read_into(reader, read_id<IdClass::Pod>(reader.take("pod"), path + ".pod", entry.pod));
    read_into(reader, read_id<IdClass::Rack>(reader.take("rack"), path + ".rack", entry.rack));
    read_into(reader, read_enum(reader.take("role"), path + ".role", entry.role, parse_device_role));
    read_into(reader, read_string(reader.take("model"), path + ".model", entry.model));
    read_into(reader,
              read_enum(reader.take("admin"), path + ".admin", entry.admin, parse_admin_state));
    read_into(reader, read_u32(reader.take("declared_port_count"), path + ".declared_port_count",
                               entry.declared_port_count));
    read_into(reader, read_id_array<IdClass::ServiceClass>(reader.take("service_classes"),
                                                           path + ".service_classes",
                                                           entry.service_classes));
    read_into(reader, parse_meta_field(reader, "meta", entry.meta));
    read_into(reader,
              read_string(reader.take("description"), path + ".description", entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.devices.push_back(std::move(entry));
  }
  return Error();
}

Error parse_ports(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("topology.ports", value, "an array");
  }
  document.ports.clear();
  document.ports.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "topology.ports[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    PortDecl entry;
    read_into(reader, read_id<IdClass::Port>(reader.take_required("id"), path + ".id", entry.id));
    read_into(reader, read_id<IdClass::Device>(reader.take_required("device"), path + ".device",
                                               entry.device));
    read_into(reader, read_u32(reader.take_required("index"), path + ".index", entry.index));
    read_into(reader,
              read_enum(reader.take("role"), path + ".role", entry.role, parse_port_role));
    read_into(reader,
              read_enum(reader.take("admin"), path + ".admin", entry.admin, parse_admin_state));
    read_into(reader, read_u64(reader.take("speed_bps"), path + ".speed_bps", entry.speed_bps));
    read_into(reader, read_string(reader.take("media"), path + ".media", entry.media));
    read_into(reader, read_id_array<IdClass::ServiceClass>(reader.take("service_classes"),
                                                           path + ".service_classes",
                                                           entry.service_classes));
    read_into(reader,
              read_string(reader.take("description"), path + ".description", entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.ports.push_back(std::move(entry));
  }
  return Error();
}

Error parse_links(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("topology.links", value, "an array");
  }
  document.links.clear();
  document.links.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "topology.links[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    LinkDecl entry;
    read_into(reader, read_id<IdClass::Link>(reader.take_required("id"), path + ".id", entry.id));
    read_into(reader, read_id<IdClass::Port>(reader.take_required("endpoint_a"),
                                             path + ".endpoint_a", entry.endpoint_a));
    read_into(reader, read_id<IdClass::Port>(reader.take_required("endpoint_b"),
                                             path + ".endpoint_b", entry.endpoint_b));
    read_into(reader,
              read_enum(reader.take("kind"), path + ".kind", entry.kind, parse_link_kind));
    read_into(reader,
              read_u64(reader.take("capacity_bps"), path + ".capacity_bps", entry.capacity_bps));
    read_into(reader,
              read_enum(reader.take("admin"), path + ".admin", entry.admin, parse_admin_state));
    read_into(reader, read_id_array<IdClass::ServiceClass>(reader.take("service_classes"),
                                                           path + ".service_classes",
                                                           entry.service_classes));
    read_into(reader,
              read_string(reader.take("description"), path + ".description", entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.links.push_back(std::move(entry));
  }
  return Error();
}

Error parse_capability_catalog(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("capability_catalog", value, "an array");
  }
  document.capability_catalog.clear();
  document.capability_catalog.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "capability_catalog[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    CapabilityDecl entry;
    read_into(reader, read_string(reader.take_required("model"), path + ".model", entry.model));
    read_into(reader, read_string_array(reader.take("capabilities"), path + ".capabilities",
                                        entry.capabilities));
    read_into(reader, read_u32(reader.take("port_count"), path + ".port_count", entry.port_count));
    read_into(reader,
              read_u64(reader.take("max_speed_bps"), path + ".max_speed_bps", entry.max_speed_bps));
    const JsonValue* roles = take_array(reader, "supported_roles", false);
    if (roles != nullptr) {
      entry.supported_roles.clear();
      for (std::size_t r = 0; r < roles->try_array()->size(); ++r) {
        DeviceRole role = DeviceRole::Leaf;
        read_into(reader, read_enum(&(*roles->try_array())[r],
                                    path + ".supported_roles[" + std::to_string(r) + "]", role,
                                    parse_device_role));
        if (!reader.ok()) {
          break;
        }
        entry.supported_roles.push_back(role);
      }
    }
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.capability_catalog.push_back(std::move(entry));
  }
  return Error();
}

Error parse_service_classes(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("service_classes", value, "an array");
  }
  document.service_classes.clear();
  document.service_classes.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "service_classes[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    ServiceClassDecl entry;
    read_into(reader, read_id<IdClass::ServiceClass>(reader.take_required("id"), path + ".id",
                                                     entry.id));
    read_into(reader,
              read_string(reader.take("display_name"), path + ".display_name", entry.display_name));
    read_into(reader, read_u32(reader.take("priority"), path + ".priority", entry.priority));
    read_into(reader, read_u32(reader.take("bandwidth_share_permille"),
                               path + ".bandwidth_share_permille", entry.bandwidth_share_permille));
    read_into(reader, read_u64(reader.take("latency_budget_us"), path + ".latency_budget_us",
                               entry.latency_budget_us));
    read_into(reader,
              read_u32(reader.take("loss_budget_ppm"), path + ".loss_budget_ppm",
                       entry.loss_budget_ppm));
    read_into(reader, read_bool(reader.take("lossless"), path + ".lossless", entry.lossless));
    read_into(reader, read_string_array(reader.take("required_capabilities"),
                                        path + ".required_capabilities",
                                        entry.required_capabilities));
    read_into(reader,
              read_string(reader.take("description"), path + ".description", entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.service_classes.push_back(std::move(entry));
  }
  return Error();
}

Error parse_routing_body(const JsonValue& value, const std::string& path,
                         RoutingPolicyDecl& body) {
  ObjReader reader(value, path);
  if (!reader.ok()) {
    return reader.error();
  }
  read_into(reader, read_u32(reader.take("priority"), path + ".priority", body.priority));
  read_into(reader, read_id_array<IdClass::ServiceClass>(reader.take("match_service_classes"),
                                                         path + ".match_service_classes",
                                                         body.match_service_classes));
  read_into(reader, read_id_array<IdClass::Tenant>(reader.take("match_tenants"),
                                                   path + ".match_tenants", body.match_tenants));
  read_into(reader,
            read_any_id(reader.take("match_source"), path + ".match_source", body.match_source));
  read_into(reader, read_any_id(reader.take("match_destination"), path + ".match_destination",
                                body.match_destination));
  read_into(reader, read_enum(reader.take("action"), path + ".action", body.action,
                              parse_routing_action));
  read_into(reader, read_string_array(reader.take("explicit_path"), path + ".explicit_path",
                                      body.explicit_path));
  read_into(reader,
            read_u32(reader.take("max_ecmp_width"), path + ".max_ecmp_width", body.max_ecmp_width));
  read_into(reader, read_u64(reader.take("min_bandwidth_bps"), path + ".min_bandwidth_bps",
                             body.min_bandwidth_bps));
  read_into(reader, read_any_id(reader.take("extends_policy"), path + ".extends_policy",
                                body.extends_policy));
  read_into(reader,
            read_string(reader.take("description"), path + ".description", body.description));
  if (!reader.ok()) {
    return reader.error();
  }
  return reader.finish();
}

Error parse_capacity_body(const JsonValue& value, const std::string& path,
                          CapacityPolicyDecl& body) {
  ObjReader reader(value, path);
  if (!reader.ok()) {
    return reader.error();
  }
  read_into(reader, read_any_id(reader.take_required("scope"), path + ".scope", body.scope));
  read_into(reader, read_id_array<IdClass::ServiceClass>(reader.take("service_classes"),
                                                         path + ".service_classes",
                                                         body.service_classes));
  read_into(reader,
            read_u64(reader.take("reserved_bps"), path + ".reserved_bps", body.reserved_bps));
  read_into(reader, read_u32(reader.take("max_utilization_permille"),
                             path + ".max_utilization_permille", body.max_utilization_permille));
  read_into(reader,
            read_string(reader.take("description"), path + ".description", body.description));
  if (!reader.ok()) {
    return reader.error();
  }
  return reader.finish();
}

Error parse_admin_state_body(const JsonValue& value, const std::string& path,
                             AdminStatePolicyDecl& body) {
  ObjReader reader(value, path);
  if (!reader.ok()) {
    return reader.error();
  }
  read_into(reader, read_any_id(reader.take_required("scope"), path + ".scope", body.scope));
  read_into(reader,
            read_enum(reader.take_required("state"), path + ".state", body.state,
                      parse_admin_state));
  read_into(reader, read_bool(reader.take("drain_first"), path + ".drain_first", body.drain_first));
  read_into(reader, read_string(reader.take("reason"), path + ".reason", body.reason));
  read_into(reader,
            read_string(reader.take("description"), path + ".description", body.description));
  if (!reader.ok()) {
    return reader.error();
  }
  return reader.finish();
}

Error parse_redundancy_body(const JsonValue& value, const std::string& path,
                            RedundancyPolicyDecl& body) {
  ObjReader reader(value, path);
  if (!reader.ok()) {
    return reader.error();
  }
  read_into(reader, read_any_id(reader.take_required("scope"), path + ".scope", body.scope));
  read_into(reader, read_enum(reader.take_required("level"), path + ".level", body.level,
                              parse_failure_domain_level));
  read_into(reader, read_u32(reader.take_required("min_disjoint_paths"),
                             path + ".min_disjoint_paths", body.min_disjoint_paths));
  read_into(reader, read_id_array<IdClass::ServiceClass>(reader.take("service_classes"),
                                                         path + ".service_classes",
                                                         body.service_classes));
  read_into(reader,
            read_string(reader.take("description"), path + ".description", body.description));
  if (!reader.ok()) {
    return reader.error();
  }
  return reader.finish();
}

Error parse_maintenance_body(const JsonValue& value, const std::string& path,
                             MaintenanceEligibilityPolicyDecl& body) {
  ObjReader reader(value, path);
  if (!reader.ok()) {
    return reader.error();
  }
  read_into(reader, read_any_id(reader.take_required("scope"), path + ".scope", body.scope));
  read_into(reader, read_string_array(reader.take("upgrade_groups"), path + ".upgrade_groups",
                                      body.upgrade_groups));
  read_into(reader, read_u32(reader.take("max_concurrent_operations"),
                             path + ".max_concurrent_operations",
                             body.max_concurrent_operations));
  read_into(reader,
            read_bool(reader.take("drain_required"), path + ".drain_required",
                      body.drain_required));
  read_into(reader, read_bool(reader.take("requires_redundancy_headroom"),
                              path + ".requires_redundancy_headroom",
                              body.requires_redundancy_headroom));
  read_into(reader, read_string_array(reader.take("allowed_impact"), path + ".allowed_impact",
                                      body.allowed_impact));
  read_into(reader,
            read_string(reader.take("description"), path + ".description", body.description));
  if (!reader.ok()) {
    return reader.error();
  }
  return reader.finish();
}

Error parse_tenant_isolation_body(const JsonValue& value, const std::string& path,
                                  TenantIsolationPolicyDecl& body) {
  ObjReader reader(value, path);
  if (!reader.ok()) {
    return reader.error();
  }
  read_into(reader, read_id<IdClass::Tenant>(reader.take_required("tenant"), path + ".tenant",
                                             body.tenant));
  read_into(reader, read_string_array(reader.take("exclusive_scopes"), path + ".exclusive_scopes",
                                      body.exclusive_scopes));
  read_into(reader, read_bool(reader.take("forbid_shared_links"), path + ".forbid_shared_links",
                              body.forbid_shared_links));
  read_into(reader,
            read_string(reader.take("description"), path + ".description", body.description));
  if (!reader.ok()) {
    return reader.error();
  }
  return reader.finish();
}

Error parse_policies(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("policies", value, "an array");
  }
  document.policies.clear();
  document.policies.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "policies[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    PolicyDecl entry;
    read_into(reader, read_id<IdClass::Policy>(reader.take_required("id"), path + ".id", entry.id));
    PolicyKind kind = PolicyKind::Routing;
    read_into(reader, read_enum(reader.take_required("kind"), path + ".kind", kind,
                                parse_policy_kind));
    if (!reader.ok()) {
      return reader.error();
    }
    const std::string_view key = to_string(kind);
    const JsonValue* body = reader.take_required(key);
    if (!reader.ok()) {
      return reader.error();
    }
    const std::string body_path = path + "." + std::string(key);
    switch (kind) {
      case PolicyKind::Routing: {
        RoutingPolicyDecl parsed;
        const Error error = parse_routing_body(*body, body_path, parsed);
        if (!error.ok()) {
          return error;
        }
        entry.body = std::move(parsed);
        break;
      }
      case PolicyKind::Capacity: {
        CapacityPolicyDecl parsed;
        const Error error = parse_capacity_body(*body, body_path, parsed);
        if (!error.ok()) {
          return error;
        }
        entry.body = std::move(parsed);
        break;
      }
      case PolicyKind::AdminState: {
        AdminStatePolicyDecl parsed;
        const Error error = parse_admin_state_body(*body, body_path, parsed);
        if (!error.ok()) {
          return error;
        }
        entry.body = std::move(parsed);
        break;
      }
      case PolicyKind::Redundancy: {
        RedundancyPolicyDecl parsed;
        const Error error = parse_redundancy_body(*body, body_path, parsed);
        if (!error.ok()) {
          return error;
        }
        entry.body = std::move(parsed);
        break;
      }
      case PolicyKind::MaintenanceEligibility: {
        MaintenanceEligibilityPolicyDecl parsed;
        const Error error = parse_maintenance_body(*body, body_path, parsed);
        if (!error.ok()) {
          return error;
        }
        entry.body = std::move(parsed);
        break;
      }
      case PolicyKind::TenantIsolation: {
        TenantIsolationPolicyDecl parsed;
        const Error error = parse_tenant_isolation_body(*body, body_path, parsed);
        if (!error.ok()) {
          return error;
        }
        entry.body = std::move(parsed);
        break;
      }
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.policies.push_back(std::move(entry));
  }
  return Error();
}

Error parse_tenants(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("tenants", value, "an array");
  }
  document.tenants.clear();
  document.tenants.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "tenants[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    TenantDecl entry;
    read_into(reader, read_id<IdClass::Tenant>(reader.take_required("id"), path + ".id", entry.id));
    read_into(reader,
              read_string(reader.take("display_name"), path + ".display_name", entry.display_name));
    read_into(reader,
              read_u32(reader.take("isolation_id"), path + ".isolation_id", entry.isolation_id));
    read_into(reader,
              read_string(reader.take("description"), path + ".description", entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.tenants.push_back(std::move(entry));
  }
  return Error();
}

Error parse_workloads(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("workloads", value, "an array");
  }
  document.workloads.clear();
  document.workloads.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "workloads[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    WorkloadDecl entry;
    read_into(reader,
              read_id<IdClass::Workload>(reader.take_required("id"), path + ".id", entry.id));
    read_into(reader, read_id<IdClass::Tenant>(reader.take_required("tenant"), path + ".tenant",
                                               entry.tenant));
    read_into(reader, read_id<IdClass::ServiceClass>(reader.take_required("service_class"),
                                                     path + ".service_class", entry.service_class));
    read_into(reader, read_id_array<IdClass::Device>(reader.take("placement"), path + ".placement",
                                                     entry.placement));
    read_into(reader,
              read_u64(reader.take("bandwidth_bps"), path + ".bandwidth_bps", entry.bandwidth_bps));
    read_into(reader, read_u32(reader.take("replicas"), path + ".replicas", entry.replicas));
    read_into(reader, read_enum(reader.take("redundancy_level"), path + ".redundancy_level",
                                entry.redundancy_level, parse_failure_domain_level));
    read_into(reader, read_u32(reader.take("min_disjoint_domains"),
                               path + ".min_disjoint_domains", entry.min_disjoint_domains));
    read_into(reader, read_id_array<IdClass::Site>(reader.take("allowed_sites"),
                                                   path + ".allowed_sites", entry.allowed_sites));
    read_into(reader,
              read_string(reader.take("description"), path + ".description", entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.workloads.push_back(std::move(entry));
  }
  return Error();
}

Error parse_intents(const JsonValue& value, IntentDocument& document) {
  const auto* items = value.try_array();
  if (items == nullptr) {
    return type_error("intents", value, "an array");
  }
  document.intents.clear();
  document.intents.reserve(items->size());
  for (std::size_t i = 0; i < items->size(); ++i) {
    const std::string path = "intents[" + std::to_string(i) + "]";
    ObjReader reader((*items)[i], path);
    if (!reader.ok()) {
      return reader.error();
    }
    IntentObjectDecl entry;
    read_into(reader,
              read_id<IdClass::IntentObject>(reader.take_required("id"), path + ".id", entry.id));
    const JsonValue* subject = reader.take_required("subject");
    if (subject != nullptr && reader.ok()) {
      std::string subject_text;
      read_into(reader, read_any_id(subject, path + ".subject", subject_text));
      if (reader.ok()) {
        if (subject_text.empty()) {
          reader.set_error(Error(ErrorCode::MissingField,
                                 "intent subject must not be empty at " + path + ".subject"));
        } else {
          const auto parsed_subject = AnyId::parse(subject_text);
          if (!parsed_subject) {
            reader.set_error(parsed_subject.error());
          } else {
            entry.subject = parsed_subject.value();
          }
        }
      }
    }
    read_into(reader,
              read_string(reader.take_required("property"), path + ".property", entry.property));
    const JsonValue* desired = reader.take_required("desired");
    if (desired != nullptr && reader.ok()) {
      entry.desired = *desired;
    }
    read_into(reader, read_enum(reader.take("mode"), path + ".mode", entry.mode,
                                parse_intent_mode));
    read_into(reader, read_u32(reader.take("priority"), path + ".priority", entry.priority));
    read_into(reader, read_any_id(reader.take("tenant"), path + ".tenant", entry.tenant));
    read_into(reader,
              read_string(reader.take("description"), path + ".description", entry.description));
    if (!reader.ok()) {
      return reader.error();
    }
    const Error error = reader.finish();
    if (!error.ok()) {
      return error;
    }
    document.intents.push_back(std::move(entry));
  }
  return Error();
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------
void put(JsonValue& object, const char* key, JsonValue value) {
  (void)object.set(key, std::move(value));
}

JsonValue json_string(const std::string& text) { return JsonValue::string(text); }

JsonValue json_u64(std::uint64_t value) {
  return JsonValue::integer(static_cast<std::int64_t>(value));
}

JsonValue json_u32(std::uint32_t value) {
  return JsonValue::integer(static_cast<std::int64_t>(value));
}

template <IdClass K>
JsonValue json_id(const BasicId<K>& id) {
  if (id.empty()) {
    return JsonValue::null();
  }
  return JsonValue::string(id.str());
}

JsonValue json_any_id(const std::string& text) {
  if (text.empty()) {
    return JsonValue::null();
  }
  return JsonValue::string(text);
}

template <IdClass K>
JsonValue json_id_array(const std::vector<BasicId<K>>& ids) {
  JsonArray out;
  out.reserve(ids.size());
  for (const auto& id : ids) {
    out.push_back(JsonValue::string(id.str()));
  }
  return JsonValue::array(std::move(out));
}

JsonValue json_string_array(const std::vector<std::string>& items) {
  JsonArray out;
  out.reserve(items.size());
  for (const auto& item : items) {
    out.push_back(JsonValue::string(item));
  }
  return JsonValue::array(std::move(out));
}

JsonValue topology_json(const IntentDocument& document) {
  JsonValue topology;
  {
    JsonArray items;
    items.reserve(document.fabrics.size());
    for (const auto& entry : document.fabrics) {
      JsonValue object;
      put(object, "id", json_id(entry.id));
      put(object, "display_name", json_string(entry.display_name));
      put(object, "description", json_string(entry.description));
      items.push_back(std::move(object));
    }
    put(topology, "fabrics", JsonValue::array(std::move(items)));
  }
  {
    JsonArray items;
    items.reserve(document.sites.size());
    for (const auto& entry : document.sites) {
      JsonValue object;
      put(object, "id", json_id(entry.id));
      put(object, "fabric", json_id(entry.fabric));
      put(object, "display_name", json_string(entry.display_name));
      put(object, "region", json_string(entry.region));
      put(object, "description", json_string(entry.description));
      items.push_back(std::move(object));
    }
    put(topology, "sites", JsonValue::array(std::move(items)));
  }
  {
    JsonArray items;
    items.reserve(document.pods.size());
    for (const auto& entry : document.pods) {
      JsonValue object;
      put(object, "id", json_id(entry.id));
      put(object, "site", json_id(entry.site));
      put(object, "display_name", json_string(entry.display_name));
      put(object, "description", json_string(entry.description));
      items.push_back(std::move(object));
    }
    put(topology, "pods", JsonValue::array(std::move(items)));
  }
  {
    JsonArray items;
    items.reserve(document.racks.size());
    for (const auto& entry : document.racks) {
      JsonValue object;
      put(object, "id", json_id(entry.id));
      put(object, "pod", json_id(entry.pod));
      put(object, "position", json_u32(entry.position));
      put(object, "description", json_string(entry.description));
      items.push_back(std::move(object));
    }
    put(topology, "racks", JsonValue::array(std::move(items)));
  }
  {
    JsonArray items;
    items.reserve(document.devices.size());
    for (const auto& entry : document.devices) {
      JsonValue object;
      put(object, "id", json_id(entry.id));
      put(object, "site", json_id(entry.site));
      put(object, "pod", json_id(entry.pod));
      put(object, "rack", json_id(entry.rack));
      put(object, "role", json_string(std::string(to_string(entry.role))));
      put(object, "model", json_string(entry.model));
      put(object, "admin", json_string(std::string(to_string(entry.admin))));
      put(object, "declared_port_count", json_u32(entry.declared_port_count));
      put(object, "service_classes", json_id_array(entry.service_classes));
      put(object, "meta", meta_to_json(entry.meta));
      put(object, "description", json_string(entry.description));
      items.push_back(std::move(object));
    }
    put(topology, "devices", JsonValue::array(std::move(items)));
  }
  {
    JsonArray items;
    items.reserve(document.ports.size());
    for (const auto& entry : document.ports) {
      JsonValue object;
      put(object, "id", json_id(entry.id));
      put(object, "device", json_id(entry.device));
      put(object, "index", json_u32(entry.index));
      put(object, "role", json_string(std::string(to_string(entry.role))));
      put(object, "admin", json_string(std::string(to_string(entry.admin))));
      put(object, "speed_bps", json_u64(entry.speed_bps));
      put(object, "media", json_string(entry.media));
      put(object, "service_classes", json_id_array(entry.service_classes));
      put(object, "description", json_string(entry.description));
      items.push_back(std::move(object));
    }
    put(topology, "ports", JsonValue::array(std::move(items)));
  }
  {
    JsonArray items;
    items.reserve(document.links.size());
    for (const auto& entry : document.links) {
      JsonValue object;
      put(object, "id", json_id(entry.id));
      put(object, "endpoint_a", json_id(entry.endpoint_a));
      put(object, "endpoint_b", json_id(entry.endpoint_b));
      put(object, "kind", json_string(std::string(to_string(entry.kind))));
      put(object, "capacity_bps", json_u64(entry.capacity_bps));
      put(object, "admin", json_string(std::string(to_string(entry.admin))));
      put(object, "service_classes", json_id_array(entry.service_classes));
      put(object, "description", json_string(entry.description));
      items.push_back(std::move(object));
    }
    put(topology, "links", JsonValue::array(std::move(items)));
  }
  return topology;
}

JsonValue capability_catalog_json(const IntentDocument& document) {
  JsonArray items;
  items.reserve(document.capability_catalog.size());
  for (const auto& entry : document.capability_catalog) {
    JsonValue object;
    put(object, "model", json_string(entry.model));
    put(object, "capabilities", json_string_array(entry.capabilities));
    put(object, "port_count", json_u32(entry.port_count));
    put(object, "max_speed_bps", json_u64(entry.max_speed_bps));
    JsonArray roles;
    roles.reserve(entry.supported_roles.size());
    for (DeviceRole role : entry.supported_roles) {
      roles.push_back(JsonValue::string(std::string(to_string(role))));
    }
    put(object, "supported_roles", JsonValue::array(std::move(roles)));
    items.push_back(std::move(object));
  }
  return JsonValue::array(std::move(items));
}

JsonValue service_classes_json(const IntentDocument& document) {
  JsonArray items;
  items.reserve(document.service_classes.size());
  for (const auto& entry : document.service_classes) {
    JsonValue object;
    put(object, "id", json_id(entry.id));
    put(object, "display_name", json_string(entry.display_name));
    put(object, "priority", json_u32(entry.priority));
    put(object, "bandwidth_share_permille", json_u32(entry.bandwidth_share_permille));
    put(object, "latency_budget_us", json_u64(entry.latency_budget_us));
    put(object, "loss_budget_ppm", json_u32(entry.loss_budget_ppm));
    put(object, "lossless", JsonValue::boolean(entry.lossless));
    put(object, "required_capabilities", json_string_array(entry.required_capabilities));
    put(object, "description", json_string(entry.description));
    items.push_back(std::move(object));
  }
  return JsonValue::array(std::move(items));
}

JsonValue policies_json(const IntentDocument& document) {
  JsonArray items;
  items.reserve(document.policies.size());
  for (const auto& entry : document.policies) {
    JsonValue object;
    put(object, "id", json_id(entry.id));
    const std::string_view key = to_string(policy_kind(entry));
    put(object, "kind", json_string(std::string(key)));
    JsonValue body;
    if (const auto* routing = std::get_if<RoutingPolicyDecl>(&entry.body)) {
      put(body, "priority", json_u32(routing->priority));
      put(body, "match_service_classes", json_id_array(routing->match_service_classes));
      put(body, "match_tenants", json_id_array(routing->match_tenants));
      put(body, "match_source", json_any_id(routing->match_source));
      put(body, "match_destination", json_any_id(routing->match_destination));
      put(body, "action", json_string(std::string(to_string(routing->action))));
      put(body, "explicit_path", json_string_array(routing->explicit_path));
      put(body, "max_ecmp_width", json_u32(routing->max_ecmp_width));
      put(body, "min_bandwidth_bps", json_u64(routing->min_bandwidth_bps));
      put(body, "extends_policy", json_any_id(routing->extends_policy));
      put(body, "description", json_string(routing->description));
    } else if (const auto* capacity = std::get_if<CapacityPolicyDecl>(&entry.body)) {
      put(body, "scope", json_any_id(capacity->scope));
      put(body, "service_classes", json_id_array(capacity->service_classes));
      put(body, "reserved_bps", json_u64(capacity->reserved_bps));
      put(body, "max_utilization_permille", json_u32(capacity->max_utilization_permille));
      put(body, "description", json_string(capacity->description));
    } else if (const auto* admin = std::get_if<AdminStatePolicyDecl>(&entry.body)) {
      put(body, "scope", json_any_id(admin->scope));
      put(body, "state", json_string(std::string(to_string(admin->state))));
      put(body, "drain_first", JsonValue::boolean(admin->drain_first));
      put(body, "reason", json_string(admin->reason));
      put(body, "description", json_string(admin->description));
    } else if (const auto* redundancy = std::get_if<RedundancyPolicyDecl>(&entry.body)) {
      put(body, "scope", json_any_id(redundancy->scope));
      put(body, "level", json_string(std::string(to_string(redundancy->level))));
      put(body, "min_disjoint_paths", json_u32(redundancy->min_disjoint_paths));
      put(body, "service_classes", json_id_array(redundancy->service_classes));
      put(body, "description", json_string(redundancy->description));
    } else if (const auto* maintenance =
                   std::get_if<MaintenanceEligibilityPolicyDecl>(&entry.body)) {
      put(body, "scope", json_any_id(maintenance->scope));
      put(body, "upgrade_groups", json_string_array(maintenance->upgrade_groups));
      put(body, "max_concurrent_operations", json_u32(maintenance->max_concurrent_operations));
      put(body, "drain_required", JsonValue::boolean(maintenance->drain_required));
      put(body, "requires_redundancy_headroom",
          JsonValue::boolean(maintenance->requires_redundancy_headroom));
      put(body, "allowed_impact", json_string_array(maintenance->allowed_impact));
      put(body, "description", json_string(maintenance->description));
    } else if (const auto* isolation = std::get_if<TenantIsolationPolicyDecl>(&entry.body)) {
      put(body, "tenant", json_id(isolation->tenant));
      put(body, "exclusive_scopes", json_string_array(isolation->exclusive_scopes));
      put(body, "forbid_shared_links", JsonValue::boolean(isolation->forbid_shared_links));
      put(body, "description", json_string(isolation->description));
    }
    put(object, std::string(key).c_str(), std::move(body));
    items.push_back(std::move(object));
  }
  return JsonValue::array(std::move(items));
}

JsonValue tenants_json(const IntentDocument& document) {
  JsonArray items;
  items.reserve(document.tenants.size());
  for (const auto& entry : document.tenants) {
    JsonValue object;
    put(object, "id", json_id(entry.id));
    put(object, "display_name", json_string(entry.display_name));
    put(object, "isolation_id", json_u32(entry.isolation_id));
    put(object, "description", json_string(entry.description));
    items.push_back(std::move(object));
  }
  return JsonValue::array(std::move(items));
}

JsonValue workloads_json(const IntentDocument& document) {
  JsonArray items;
  items.reserve(document.workloads.size());
  for (const auto& entry : document.workloads) {
    JsonValue object;
    put(object, "id", json_id(entry.id));
    put(object, "tenant", json_id(entry.tenant));
    put(object, "service_class", json_id(entry.service_class));
    put(object, "placement", json_id_array(entry.placement));
    put(object, "bandwidth_bps", json_u64(entry.bandwidth_bps));
    put(object, "replicas", json_u32(entry.replicas));
    put(object, "redundancy_level", json_string(std::string(to_string(entry.redundancy_level))));
    put(object, "min_disjoint_domains", json_u32(entry.min_disjoint_domains));
    put(object, "allowed_sites", json_id_array(entry.allowed_sites));
    put(object, "description", json_string(entry.description));
    items.push_back(std::move(object));
  }
  return JsonValue::array(std::move(items));
}

JsonValue intents_json(const IntentDocument& document) {
  JsonArray items;
  items.reserve(document.intents.size());
  for (const auto& entry : document.intents) {
    JsonValue object;
    put(object, "id", json_id(entry.id));
    put(object, "subject", json_any_id(entry.subject.str()));
    put(object, "property", json_string(entry.property));
    put(object, "desired", entry.desired);
    put(object, "mode", json_string(std::string(to_string(entry.mode))));
    put(object, "priority", json_u32(entry.priority));
    put(object, "tenant", json_any_id(entry.tenant));
    put(object, "description", json_string(entry.description));
    items.push_back(std::move(object));
  }
  return JsonValue::array(std::move(items));
}

JsonValue provenance_json(const Provenance& provenance) {
  JsonValue object;
  put(object, "actor", json_id(provenance.actor));
  put(object, "source_revision", json_string(provenance.source_revision.str()));
  if (provenance.parent_intent.has_value()) {
    JsonValue parent;
    put(parent, "generation", json_id(provenance.parent_intent->generation));
    put(parent, "content_digest", json_string(provenance.parent_intent->content.hex()));
    put(object, "parent_intent", std::move(parent));
  } else {
    put(object, "parent_intent", JsonValue::null());
  }
  put(object, "created_at", json_string(format_timestamp(provenance.created_at)));
  put(object, "description", json_string(provenance.description));
  put(object, "labels", meta_to_json(provenance.labels));
  return object;
}

// ---------------------------------------------------------------------------
// Migration helpers
// ---------------------------------------------------------------------------
Error rename_field(JsonValue& object, std::string_view from, std::string_view to,
                   const std::string& path) {
  JsonValue* source = object.find_mut(from);
  if (source == nullptr) {
    return Error();
  }
  if (object.find(to) != nullptr) {
    return Error(ErrorCode::AmbiguousSemantics,
                 "both the legacy field '" + std::string(from) + "' and the current field '" +
                     std::string(to) + "' are present at " + path);
  }
  JsonValue moved = *source;
  (void)object.erase(from);
  (void)object.set(to, std::move(moved));
  return Error();
}

Result<std::uint64_t> speed_from_json(const JsonValue& value, const std::string& path) {
  if (const auto* text = value.try_string()) {
    const auto parsed = parse_speed_bps(*text);
    if (!parsed) {
      return Error(parsed.error().code, parsed.error().message + " at " + path,
                   parsed.error().detail);
    }
    return parsed.value();
  }
  const auto number = value.try_int();
  if (number.has_value() && number.value() >= 0) {
    return static_cast<std::uint64_t>(number.value());
  }
  return Error(ErrorCode::JsonTypeMismatch,
               "expected a link speed string such as \"100g\" or an integer bit rate at " + path);
}

Error migrate_1_0_to_1_1(JsonValue& tree, std::vector<std::string>& applied) {
  JsonValue* topology = tree.find_mut("topology");
  if (topology == nullptr) {
    return Error(ErrorCode::MissingField, "missing required field topology");
  }
  JsonValue* ports = topology->find_mut("ports");
  if (ports != nullptr) {
    JsonArray* items = ports->array_mut();
    if (items == nullptr) {
      return Error(ErrorCode::JsonTypeMismatch, "expected an array at topology.ports");
    }
    for (std::size_t i = 0; i < items->size(); ++i) {
      JsonValue& port = (*items)[i];
      const std::string path = "topology.ports[" + std::to_string(i) + "]";
      if (port.find("speed") == nullptr && port.find("speed_bps") == nullptr) {
        return Error(ErrorCode::MissingField,
                     "schema 1.0 requires a port speed at " + path + ".speed");
      }
      JsonValue* legacy = port.find_mut("speed");
      if (legacy != nullptr) {
        const auto bits = speed_from_json(*legacy, path + ".speed");
        if (!bits) {
          return bits.error();
        }
        (void)port.erase("speed");
        (void)port.set("speed_bps", json_u64(bits.value()));
      }
    }
  }
  JsonValue* links = topology->find_mut("links");
  if (links != nullptr) {
    JsonArray* items = links->array_mut();
    if (items == nullptr) {
      return Error(ErrorCode::JsonTypeMismatch, "expected an array at topology.links");
    }
    for (std::size_t i = 0; i < items->size(); ++i) {
      JsonValue& link = (*items)[i];
      const std::string path = "topology.links[" + std::to_string(i) + "]";
      if (link.find("bandwidth") == nullptr && link.find("capacity_bps") == nullptr) {
        return Error(ErrorCode::MissingField,
                     "schema 1.0 requires a link bandwidth at " + path + ".bandwidth");
      }
      JsonValue* legacy = link.find_mut("bandwidth");
      if (legacy != nullptr) {
        const auto bits = speed_from_json(*legacy, path + ".bandwidth");
        if (!bits) {
          return bits.error();
        }
        (void)link.erase("bandwidth");
        (void)link.set("capacity_bps", json_u64(bits.value()));
      }
    }
  }
  applied.emplace_back("1.0->1.1: port.speed -> port.speed_bps, link.bandwidth -> link.capacity_bps");
  return Error();
}

Error migrate_1_1_to_1_2(JsonValue& tree, std::vector<std::string>& applied) {
  JsonValue* policies = tree.find_mut("policies");
  if (policies != nullptr) {
    JsonArray* items = policies->array_mut();
    if (items == nullptr) {
      return Error(ErrorCode::JsonTypeMismatch, "expected an array at policies");
    }
    for (std::size_t i = 0; i < items->size(); ++i) {
      JsonValue& policy = (*items)[i];
      const JsonValue* kind = policy.find("kind");
      const auto* kind_text = kind == nullptr ? nullptr : kind->try_string();
      if (kind_text == nullptr || *kind_text != "maintenance-eligibility") {
        continue;
      }
      JsonValue* body = policy.find_mut("maintenance-eligibility");
      if (body == nullptr || !body->is_object()) {
        continue;
      }
      if (body->find("requires_redundancy_headroom") == nullptr) {
        (void)body->set("requires_redundancy_headroom", JsonValue::boolean(true));
      }
      if (body->find("upgrade_groups") == nullptr) {
        (void)body->set("upgrade_groups", JsonValue::array({}));
      }
    }
  }
  applied.emplace_back(
      "1.1->1.2: maintenance-eligibility defaults (requires_redundancy_headroom, upgrade_groups)");
  return Error();
}

Error migrate_1_2_to_1_3(JsonValue& tree, std::vector<std::string>& applied) {
  JsonValue* topology = tree.find_mut("topology");
  if (topology != nullptr) {
    JsonValue* devices = topology->find_mut("devices");
    if (devices != nullptr) {
      JsonArray* items = devices->array_mut();
      if (items == nullptr) {
        return Error(ErrorCode::JsonTypeMismatch, "expected an array at topology.devices");
      }
      for (std::size_t i = 0; i < items->size(); ++i) {
        const Error error = rename_field((*items)[i], "port_count", "declared_port_count",
                                         "topology.devices[" + std::to_string(i) + "]");
        if (!error.ok()) {
          return error;
        }
      }
    }
  }
  JsonValue* catalog = tree.find_mut("capability_catalog");
  if (catalog != nullptr) {
    JsonArray* items = catalog->array_mut();
    if (items == nullptr) {
      return Error(ErrorCode::JsonTypeMismatch, "expected an array at capability_catalog");
    }
    for (std::size_t i = 0; i < items->size(); ++i) {
      const Error error = rename_field((*items)[i], "ports", "port_count",
                                       "capability_catalog[" + std::to_string(i) + "]");
      if (!error.ok()) {
        return error;
      }
    }
  }
  applied.emplace_back(
      "1.2->1.3: device.port_count -> device.declared_port_count, "
      "capability_catalog[].ports -> capability_catalog[].port_count");
  return Error();
}

// ---------------------------------------------------------------------------
// Document parsing
// ---------------------------------------------------------------------------
Error parse_document(const JsonValue& value, IntentDocument& document) {
  if (!value.is_object()) {
    return type_error("$", value, "an object");
  }
  IntentDocument parsed;
  ObjReader root(value, "$");
  if (!root.ok()) {
    return root.error();
  }
  const JsonValue* schema = root.take_required("schema");
  if (!root.ok()) {
    return root.error();
  }
  const Error schema_error = parse_schema(*schema, parsed);
  if (!schema_error.ok()) {
    return schema_error;
  }
  if (!parsed.schema.is_supported_major()) {
    return Error(ErrorCode::UnsupportedSchemaMajor,
                 "unsupported schema major version " + std::to_string(parsed.schema.major()),
                 "this build reads " + std::to_string(SchemaVersion::kSupportedMajor) + ".x");
  }
  if (!parsed.schema.is_supported_minor()) {
    return Error(ErrorCode::UnsupportedSchemaMinor,
                 "unsupported schema minor version " + std::to_string(parsed.schema.minor()),
                 "this build reads " + std::to_string(SchemaVersion::kSupportedMajor) + "." +
                     std::to_string(SchemaVersion::kOldestMinor) + " .. " +
                     std::to_string(SchemaVersion::kSupportedMajor) + "." +
                     std::to_string(SchemaVersion::kSupportedMinor));
  }
  if (!parsed.schema.is_current()) {
    return Error(ErrorCode::UnsupportedSchemaMinor,
                 "document has not been upgraded to the current schema minor version",
                 parsed.schema.str());
  }

  std::string domain;
  read_into(root, read_string(root.take_required("domain"), "$.domain", domain));
  if (root.ok()) {
    const auto parsed_domain = DomainId::parse(domain);
    if (!parsed_domain) {
      root.set_error(parsed_domain.error());
    } else {
      parsed.domain = parsed_domain.value();
    }
  }
  const JsonValue* provenance = root.take("provenance");
  if (provenance != nullptr && !provenance->is_null()) {
    const Error error = parse_provenance(*provenance, parsed.provenance);
    if (!error.ok()) {
      return error;
    }
  }
  const JsonValue* topology = root.take_required("topology");
  if (!root.ok()) {
    return root.error();
  }
  {
    ObjReader topology_reader(*topology, "topology");
    if (!topology_reader.ok()) {
      return topology_reader.error();
    }
    const JsonValue* fabrics = take_array(topology_reader, "fabrics", true);
    if (topology_reader.ok()) {
      const Error error = parse_fabrics(*fabrics, parsed);
      if (!error.ok()) {
        return error;
      }
    }
    const JsonValue* sites = take_array(topology_reader, "sites", true);
    if (topology_reader.ok()) {
      const Error error = parse_sites(*sites, parsed);
      if (!error.ok()) {
        return error;
      }
    }
    const JsonValue* pods = take_array(topology_reader, "pods", true);
    if (topology_reader.ok()) {
      const Error error = parse_pods(*pods, parsed);
      if (!error.ok()) {
        return error;
      }
    }
    const JsonValue* racks = take_array(topology_reader, "racks", true);
    if (topology_reader.ok()) {
      const Error error = parse_racks(*racks, parsed);
      if (!error.ok()) {
        return error;
      }
    }
    const JsonValue* devices = take_array(topology_reader, "devices", true);
    if (topology_reader.ok()) {
      const Error error = parse_devices(*devices, parsed);
      if (!error.ok()) {
        return error;
      }
    }
    const JsonValue* ports = take_array(topology_reader, "ports", true);
    if (topology_reader.ok()) {
      const Error error = parse_ports(*ports, parsed);
      if (!error.ok()) {
        return error;
      }
    }
    const JsonValue* links = take_array(topology_reader, "links", true);
    if (topology_reader.ok()) {
      const Error error = parse_links(*links, parsed);
      if (!error.ok()) {
        return error;
      }
    }
    if (!topology_reader.ok()) {
      return topology_reader.error();
    }
    const Error error = topology_reader.finish();
    if (!error.ok()) {
      return error;
    }
  }
  const JsonValue* catalog = take_array(root, "capability_catalog", true);
  if (root.ok()) {
    const Error error = parse_capability_catalog(*catalog, parsed);
    if (!error.ok()) {
      return error;
    }
  }
  const JsonValue* service_classes = take_array(root, "service_classes", true);
  if (root.ok()) {
    const Error error = parse_service_classes(*service_classes, parsed);
    if (!error.ok()) {
      return error;
    }
  }
  const JsonValue* policies = take_array(root, "policies", false);
  if (root.ok() && policies != nullptr) {
    const Error error = parse_policies(*policies, parsed);
    if (!error.ok()) {
      return error;
    }
  }
  const JsonValue* tenants = take_array(root, "tenants", false);
  if (root.ok() && tenants != nullptr) {
    const Error error = parse_tenants(*tenants, parsed);
    if (!error.ok()) {
      return error;
    }
  }
  const JsonValue* workloads = take_array(root, "workloads", false);
  if (root.ok() && workloads != nullptr) {
    const Error error = parse_workloads(*workloads, parsed);
    if (!error.ok()) {
      return error;
    }
  }
  const JsonValue* intents = take_array(root, "intents", false);
  if (root.ok() && intents != nullptr) {
    const Error error = parse_intents(*intents, parsed);
    if (!error.ok()) {
      return error;
    }
  }
  if (!root.ok()) {
    return root.error();
  }
  const Error root_error = root.finish();
  if (!root_error.ok()) {
    return root_error;
  }
  document = std::move(parsed);
  return Error();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public JSON projection
// ---------------------------------------------------------------------------
JsonValue to_json(const IntentDocument& document) {
  JsonValue root;
  {
    JsonValue schema;
    put(schema, "name", json_string(std::string(kSchemaName)));
    put(schema, "major", json_u32(document.schema.major()));
    put(schema, "minor", json_u32(document.schema.minor()));
    put(schema, "requires", json_string_array(document.required_features));
    put(schema, "optional", json_string_array(document.optional_features));
    put(root, "schema", std::move(schema));
  }
  put(root, "domain", json_string(document.domain.str()));
  put(root, "provenance", provenance_json(document.provenance));
  put(root, "topology", topology_json(document));
  put(root, "capability_catalog", capability_catalog_json(document));
  put(root, "service_classes", service_classes_json(document));
  put(root, "policies", policies_json(document));
  put(root, "tenants", tenants_json(document));
  put(root, "workloads", workloads_json(document));
  put(root, "intents", intents_json(document));
  return root;
}

Result<IntentDocument> from_json(const JsonValue& value) {
  const auto upgraded = upgrade_to_current(value);
  if (!upgraded) {
    return upgraded.error();
  }
  IntentDocument document;
  const Error error = parse_document(upgraded->upgraded, document);
  if (!error.ok()) {
    return error;
  }
  return document;
}

Result<SchemaUpgrade> upgrade_to_current(const JsonValue& value) {
  if (!value.is_object()) {
    return type_error("$", value, "an object");
  }
  const JsonValue* schema = value.find("schema");
  if (schema == nullptr || !schema->is_object()) {
    return Error(ErrorCode::MissingField, "missing required field schema");
  }
  const JsonValue* name = schema->find("name");
  if (name == nullptr) {
    return Error(ErrorCode::MissingField, "missing required field schema.name");
  }
  const auto* name_text = name->try_string();
  if (name_text == nullptr) {
    return type_error("schema.name", *name, "a string");
  }
  if (*name_text != kSchemaName) {
    return Error(ErrorCode::SchemaVersionMalformed, "unexpected schema name", *name_text);
  }
  const JsonValue* major_value = schema->find("major");
  const JsonValue* minor_value = schema->find("minor");
  if (major_value == nullptr) {
    return Error(ErrorCode::MissingField, "missing required field schema.major");
  }
  if (minor_value == nullptr) {
    return Error(ErrorCode::MissingField, "missing required field schema.minor");
  }
  const auto major = major_value->try_int();
  const auto minor = minor_value->try_int();
  if (!major.has_value() || !minor.has_value()) {
    return Error(ErrorCode::SchemaVersionMalformed,
                 "schema.major and schema.minor must be integers");
  }
  if (major.value() < 0 || minor.value() < 0 || major.value() > 0xFFFF || minor.value() > 0xFFFF) {
    return Error(ErrorCode::SchemaVersionMalformed, "schema version component out of range");
  }
  const auto version = SchemaVersion::make(static_cast<std::uint32_t>(major.value()),
                                           static_cast<std::uint32_t>(minor.value()));
  if (!version) {
    return version.error();
  }
  if (!version->is_supported_major()) {
    return Error(ErrorCode::UnsupportedSchemaMajor,
                 "unsupported schema major version " + std::to_string(version->major()),
                 "this build reads " + std::to_string(SchemaVersion::kSupportedMajor) + ".x");
  }
  if (version->minor() > SchemaVersion::kSupportedMinor) {
    return Error(ErrorCode::UnsupportedSchemaMinor,
                 "unsupported schema minor version " + std::to_string(version->minor()),
                 "this build reads up to " + std::to_string(SchemaVersion::kSupportedMajor) + "." +
                     std::to_string(SchemaVersion::kSupportedMinor) +
                     "; newer minor versions may carry semantics this build cannot honour");
  }

  SchemaUpgrade upgrade;
  upgrade.from = version.value();
  upgrade.upgraded = value;
  for (std::uint32_t m = version->minor(); m < SchemaVersion::kSupportedMinor; ++m) {
    Error error;
    switch (m) {
      case 0: error = migrate_1_0_to_1_1(upgrade.upgraded, upgrade.applied); break;
      case 1: error = migrate_1_1_to_1_2(upgrade.upgraded, upgrade.applied); break;
      case 2: error = migrate_1_2_to_1_3(upgrade.upgraded, upgrade.applied); break;
      default:
        return Error(ErrorCode::Internal, "no migration registered for schema minor " +
                                              std::to_string(m));
    }
    if (!error.ok()) {
      return error;
    }
  }
  if (!version->is_current()) {
    JsonValue* schema_mut = upgrade.upgraded.find_mut("schema");
    (void)schema_mut->set("minor", json_u32(SchemaVersion::kSupportedMinor));
    (void)schema_mut->set("upgraded_from", json_string(version->str()));
  }
  return upgrade;
}

JsonValue content_payload(const IntentDocument& document) {
  JsonValue out;
  put(out, "domain", json_string(document.domain.str()));
  {
    JsonValue features;
    put(features, "required", json_string_array(document.required_features));
    put(features, "optional", json_string_array(document.optional_features));
    put(out, "features", std::move(features));
  }
  put(out, "topology", topology_json(document));
  put(out, "capability_catalog", capability_catalog_json(document));
  put(out, "service_classes", service_classes_json(document));
  put(out, "policies", policies_json(document));
  put(out, "tenants", tenants_json(document));
  put(out, "workloads", workloads_json(document));
  put(out, "intents", intents_json(document));
  return out;
}

std::size_t IntentDocument::declared_object_count() const noexcept {
  std::size_t total = 0;
  total = saturating_add(total, fabrics.size());
  total = saturating_add(total, sites.size());
  total = saturating_add(total, pods.size());
  total = saturating_add(total, racks.size());
  total = saturating_add(total, devices.size());
  total = saturating_add(total, ports.size());
  total = saturating_add(total, links.size());
  total = saturating_add(total, capability_catalog.size());
  total = saturating_add(total, service_classes.size());
  total = saturating_add(total, policies.size());
  total = saturating_add(total, tenants.size());
  total = saturating_add(total, workloads.size());
  total = saturating_add(total, intents.size());
  return total;
}

PolicyKind policy_kind(const PolicyDecl& policy) noexcept {
  return static_cast<PolicyKind>(policy.body.index());
}

const char* policy_body_key(const PolicyDecl& policy) noexcept {
  return to_string(policy_kind(policy)).data();
}

}  // namespace ifabric
