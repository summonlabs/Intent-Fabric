// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/canonical.hpp"
#include "ifabric/synth.hpp"
#include "ifabric_test.hpp"

using namespace ifabric;

namespace {

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

JsonValue& first_device(JsonValue& document) {
  return (*document.find_mut("topology")->find_mut("devices")->array_mut())[0];
}

// Rewrites a current-schema document into the shape an older minor used.
JsonValue to_legacy_schema(JsonValue document, std::uint32_t minor) {
  const JsonValue* old_schema = document.find("schema");
  JsonValue required_features_json =
      old_schema != nullptr && old_schema->find("requires") != nullptr
          ? *old_schema->find("requires")
          : JsonValue::array({});
  JsonValue optional = old_schema != nullptr && old_schema->find("optional") != nullptr
                           ? *old_schema->find("optional")
                           : JsonValue::array({});
  JsonValue schema;
  (void)schema.set("name", JsonValue::string(std::string(kSchemaName)));
  (void)schema.set("major", JsonValue::integer(1));
  (void)schema.set("minor", JsonValue::integer(static_cast<std::int64_t>(minor)));
  (void)schema.set("requires", std::move(required_features_json));
  (void)schema.set("optional", std::move(optional));
  (void)document.set("schema", std::move(schema));

  JsonValue* topology = document.find_mut("topology");
  if (minor < 3) {
    if (JsonValue* devices = topology->find_mut("devices"); devices != nullptr) {
      for (auto& device : *devices->array_mut()) {
        if (JsonValue* count = device.find_mut("declared_port_count"); count != nullptr) {
          JsonValue moved = *count;
          (void)device.erase("declared_port_count");
          (void)device.set("port_count", std::move(moved));
        }
      }
    }
    if (JsonValue* catalog = document.find_mut("capability_catalog"); catalog != nullptr) {
      for (auto& entry : *catalog->array_mut()) {
        if (JsonValue* count = entry.find_mut("port_count"); count != nullptr) {
          JsonValue moved = *count;
          (void)entry.erase("port_count");
          (void)entry.set("ports", std::move(moved));
        }
      }
    }
  }
  if (minor < 2) {
    if (JsonValue* policies = document.find_mut("policies"); policies != nullptr) {
      for (auto& policy : *policies->array_mut()) {
        JsonValue* body = policy.find_mut("maintenance-eligibility");
        if (body == nullptr) {
          continue;
        }
        (void)body->erase("requires_redundancy_headroom");
        (void)body->erase("upgrade_groups");
      }
    }
  }
  if (minor < 1) {
    if (JsonValue* ports = topology->find_mut("ports"); ports != nullptr) {
      for (auto& port : *ports->array_mut()) {
        if (JsonValue* speed = port.find_mut("speed_bps"); speed != nullptr) {
          const auto value = speed->try_int();
          const std::string text =
              value.has_value() ? std::to_string(value.value() / 1000000000ll) + "g" : "0g";
          (void)port.erase("speed_bps");
          (void)port.set("speed", JsonValue::string(text));
        }
      }
    }
    if (JsonValue* links = topology->find_mut("links"); links != nullptr) {
      for (auto& link : *links->array_mut()) {
        if (JsonValue* capacity = link.find_mut("capacity_bps"); capacity != nullptr) {
          const auto value = capacity->try_int();
          const std::string text =
              value.has_value() ? std::to_string(value.value() / 1000000000ll) + "g" : "0g";
          (void)link.erase("capacity_bps");
          (void)link.set("bandwidth", JsonValue::string(text));
        }
      }
    }
  }
  return document;
}

}  // namespace

IFABRIC_TEST(model, schema_version_parsing) {
  const auto parsed = SchemaVersion::parse("1.3");
  REQUIRE(parsed.ok());
  CHECK_EQ(parsed.value().major(), 1u);
  CHECK_EQ(parsed.value().minor(), 3u);
  CHECK_EQ(parsed.value().str(), std::string("1.3"));
  CHECK_ERROR_CODE(SchemaVersion::parse("1").error(), ErrorCode::SchemaVersionMalformed);
  CHECK_ERROR_CODE(SchemaVersion::parse("1.").error(), ErrorCode::SchemaVersionMalformed);
  CHECK_ERROR_CODE(SchemaVersion::parse("x.1").error(), ErrorCode::SchemaVersionMalformed);
  CHECK_ERROR_CODE(SchemaVersion::parse(".1").error(), ErrorCode::SchemaVersionMalformed);
  CHECK(SchemaVersion::parse("1.0").value().is_supported_minor());
  CHECK(!SchemaVersion::parse("1.99").value().is_supported_minor());
  CHECK(!SchemaVersion::parse("2.0").value().is_supported_major());
}

IFABRIC_TEST(model, feature_registry_is_explicit) {
  CHECK(is_known_feature("routing.path-pinning"));
  CHECK(!is_known_feature("vendor.unobtainium"));
  CHECK_EQ(feature_info("maintenance.upgrade-groups").value().introduced_minor, 2u);
  CHECK(!known_features().empty());
}

IFABRIC_TEST(model, rejects_unknown_major_and_minor) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  JsonValue json = to_json(document);
  (void)json.find_mut("schema")->set("major", JsonValue::integer(2));
  CHECK_ERROR_CODE(upgrade_to_current(json).error(), ErrorCode::UnsupportedSchemaMajor);
  (void)json.find_mut("schema")->set("major", JsonValue::integer(1));
  (void)json.find_mut("schema")->set("minor", JsonValue::integer(9));
  CHECK_ERROR_CODE(upgrade_to_current(json).error(), ErrorCode::UnsupportedSchemaMinor);
}

IFABRIC_TEST(model, rejects_unknown_schema_name) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  JsonValue json = to_json(document);
  (void)json.find_mut("schema")->set("name", JsonValue::string("other.schema"));
  CHECK_ERROR_CODE(from_json(json).error(), ErrorCode::SchemaVersionMalformed);
}

IFABRIC_TEST(model, migrates_older_minors_to_the_same_content) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const JsonValue current = to_json(document);
  const auto reference = canonicalize(document);
  REQUIRE(reference.ok());

  for (std::uint32_t minor : {0u, 1u, 2u}) {
    const JsonValue legacy = to_legacy_schema(current, minor);
    const auto upgraded = upgrade_to_current(legacy);
    REQUIRE(upgraded.ok());
    CHECK_EQ(upgraded.value().applied.size(), std::size_t{3 - minor});
    const auto migrated = from_json(upgraded.value().upgraded);
    REQUIRE(migrated.ok());
    const auto canonical = canonicalize(migrated.value());
    REQUIRE(canonical.ok());
    CHECK_EQ(canonical->content_digest.hex(), reference->content_digest.hex());
  }
}

IFABRIC_TEST(model, current_minor_needs_no_migration) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const auto upgraded = upgrade_to_current(to_json(document));
  REQUIRE(upgraded.ok());
  CHECK(upgraded.value().applied.empty());
}

IFABRIC_TEST(model, ambiguous_legacy_and_current_field_is_rejected) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  JsonValue legacy = to_legacy_schema(to_json(document), 2);
  (void)first_device(legacy).set("declared_port_count", JsonValue::integer(32));
  CHECK_ERROR_CODE(upgrade_to_current(legacy).error(), ErrorCode::AmbiguousSemantics);
}

IFABRIC_TEST(model, legacy_speed_without_unit_is_accepted_as_bits) {
  JsonValue document = to_legacy_schema(to_json(synthesize(core_domain(), SynthOptions{})), 0);
  JsonValue& port = (*document.find_mut("topology")->find_mut("ports")->array_mut())[0];
  (void)port.set("speed", JsonValue::integer(400000000000ll));
  const auto upgraded = upgrade_to_current(document);
  REQUIRE(upgraded.ok());
  const auto parsed = from_json(upgraded.value().upgraded);
  REQUIRE(parsed.ok());
  CHECK_EQ(parsed.value().ports.front().speed_bps, 400000000000ull);
}

IFABRIC_TEST(model, malformed_legacy_speed_is_rejected) {
  JsonValue document = to_legacy_schema(to_json(synthesize(core_domain(), SynthOptions{})), 0);
  JsonValue& port = (*document.find_mut("topology")->find_mut("ports")->array_mut())[0];
  (void)port.set("speed", JsonValue::string("fast"));
  CHECK_ERROR_CODE(upgrade_to_current(document).error(), ErrorCode::UnknownField);
}

IFABRIC_TEST(model, unknown_fields_are_rejected) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  JsonValue json = to_json(document);
  (void)json.set("vendor_extension", JsonValue::boolean(true));
  CHECK_ERROR_CODE(from_json(json).error(), ErrorCode::UnknownField);
  JsonValue nested = to_json(document);
  (void)first_device(nested).set("purpose", JsonValue::string("mystery"));
  CHECK_ERROR_CODE(from_json(nested).error(), ErrorCode::UnknownField);
}

IFABRIC_TEST(model, missing_required_fields_are_rejected) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  JsonValue json = to_json(document);
  (void)first_device(json).erase("id");
  const Error error = from_json(json).error();
  CHECK_EQ(std::string(to_string(error.code)), std::string("MissingField"));
}

IFABRIC_TEST(model, json_round_trip_preserves_both_digests) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const auto canonical = canonicalize(document);
  REQUIRE(canonical.ok());
  const auto reloaded = from_json(canonical->document_json);
  REQUIRE(reloaded.ok());
  const auto again = canonicalize(reloaded.value());
  REQUIRE(again.ok());
  CHECK_EQ(again->content_digest.hex(), canonical->content_digest.hex());
  CHECK_EQ(again->document_digest.hex(), canonical->document_digest.hex());
}

IFABRIC_TEST(model, provenance_is_excluded_from_content_identity) {
  IntentDocument left = synthesize(core_domain(), SynthOptions{});
  IntentDocument right = left;
  right.provenance.actor = ActorId::parse("actor.other").value();
  right.provenance.created_at += std::chrono::hours(5);
  right.provenance.labels.clear();
  right.provenance.description = "a different author, a different moment";
  const auto revision = SourceRevision::parse("git:deadbeef");
  right.provenance.source_revision = revision.value();
  const auto left_form = canonicalize(left);
  const auto right_form = canonicalize(right);
  REQUIRE(left_form.ok());
  REQUIRE(right_form.ok());
  CHECK_EQ(left_form->content_digest.hex(), right_form->content_digest.hex());
  CHECK_NE(left_form->document_digest.hex(), right_form->document_digest.hex());
}

IFABRIC_TEST(model, defaults_are_materialized) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  JsonValue json = to_json(document);
  (void)json.erase("policies");
  (void)json.erase("tenants");
  (void)json.erase("workloads");
  (void)json.erase("intents");
  const auto parsed = from_json(json);
  REQUIRE(parsed.ok());
  CHECK(parsed.value().policies.empty());
  CHECK(parsed.value().tenants.empty());
  const auto form = canonicalize(parsed.value());
  REQUIRE(form.ok());
  CHECK(form->document_json.find("policies") != nullptr);
  CHECK(form->document_json.find("policies")->is_array());
  CHECK_EQ(form->document_json.find("policies")->size(), std::size_t{0});
}

IFABRIC_TEST(model, declared_object_count_covers_every_collection) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  std::size_t expected = document.fabrics.size() + document.sites.size() + document.pods.size() +
                         document.racks.size() + document.devices.size() + document.ports.size() +
                         document.links.size() + document.capability_catalog.size() +
                         document.service_classes.size() + document.policies.size() +
                         document.tenants.size() + document.workloads.size() +
                         document.intents.size();
  CHECK_EQ(document.declared_object_count(), expected);
  CHECK_EQ(synth_declared_objects(SynthOptions{}), expected);
}
