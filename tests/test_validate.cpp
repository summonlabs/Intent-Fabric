// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/synth.hpp"
#include "ifabric/validate.hpp"
#include "ifabric_test.hpp"

#include <iostream>

using namespace ifabric;

namespace {

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

ValidationReport validate_synth(const SynthOptions& options) {
  const IntentDocument document = synthesize(core_domain(), options);
  return validate_text(to_json(document).dump());
}

bool has_diagnostic(const ValidationReport& report, ErrorCode code, ValidationLayer layer) {
  for (const auto& diagnostic : report.diagnostics) {
    if (diagnostic.code == code && diagnostic.layer == layer &&
        diagnostic.severity == Severity::Error) {
      return true;
    }
  }
  return false;
}

}  // namespace

IFABRIC_TEST(validate, reference_document_passes_every_layer) {
  const ValidationReport report = validate_synth(SynthOptions{});
  if (!report.passed()) {
    std::cout << report.render() << "\n";
  }
  CHECK(report.passed());
  CHECK_EQ(report.error_count(), std::size_t{0});
  CHECK(!report.content_digest.is_zero());
  CHECK(report.parsed);
}

IFABRIC_TEST(validate, duplicate_identity_is_reported) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::DuplicateIdentity;
    return options;
  }());
  CHECK(!report.passed());
  CHECK(has_diagnostic(report, ErrorCode::DuplicateIdentity, ValidationLayer::ReferentialIntegrity));
}

IFABRIC_TEST(validate, dangling_reference_is_reported) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::DanglingReference;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::DanglingReference, ValidationLayer::ReferentialIntegrity));
}

IFABRIC_TEST(validate, cyclic_reference_is_reported) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::CyclicPolicyComposition;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::CyclicReference, ValidationLayer::ReferentialIntegrity));
}

IFABRIC_TEST(validate, unknown_capability_is_reported) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::UnknownCapability;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::UnknownCapability,
                       ValidationLayer::CapabilityCompatibility));
}

IFABRIC_TEST(validate, unproven_device_capability_is_reported) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::UnknownRequiredCapability;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::UnknownCapability,
                       ValidationLayer::CapabilityCompatibility));
}

IFABRIC_TEST(validate, unknown_device_model_is_not_treated_as_satisfied) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::UnknownModel;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::UnknownCapability,
                       ValidationLayer::CapabilityCompatibility));
}

IFABRIC_TEST(validate, infeasible_port_index_is_reported) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::PortIndexOutOfRange;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::InfeasibleTopology, ValidationLayer::TopologyFeasibility));
}

IFABRIC_TEST(validate, self_loop_link_is_infeasible) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::SelfLoopLink;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::InfeasibleTopology, ValidationLayer::TopologyFeasibility));
}

IFABRIC_TEST(validate, undeclared_capacity_is_not_assumed) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::EmptyCapacity;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::MissingField, ValidationLayer::Syntax));
}

IFABRIC_TEST(validate, invariant_violation_is_reported) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::DisabledLinkWithEnabledPort;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::InvariantViolation, ValidationLayer::InvariantViolation));
}

IFABRIC_TEST(validate, unknown_property_is_rejected) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::UnknownProperty;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::UnknownField, ValidationLayer::InvariantViolation));
}

IFABRIC_TEST(validate, unknown_required_feature_is_rejected) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::UnknownFeature;
    return options;
  }());
  CHECK(has_diagnostic(report, ErrorCode::FeatureNotAvailable,
                       ValidationLayer::SchemaCompatibility));
}

IFABRIC_TEST(validate, unsupported_schema_minor_is_rejected_before_anything_else) {
  const ValidationReport report = validate_synth([] {
    SynthOptions options;
    options.defect = SynthDefect::UnsupportedMinor;
    return options;
  }());
  CHECK(!report.parsed);
  CHECK(has_diagnostic(report, ErrorCode::UnsupportedSchemaMinor,
                       ValidationLayer::SchemaCompatibility));
}

IFABRIC_TEST(validate, every_defect_is_rejected_and_none_is_accepted) {
  for (const auto& entry : synth_defect_registry()) {
    SynthOptions options;
    options.defect = entry.second;
    const ValidationReport report = validate_synth(options);
    if (entry.second == SynthDefect::None) {
      CHECK(report.passed());
    } else {
      if (report.passed()) {
        std::cout << "defect " << entry.first << " was accepted\n";
      }
      CHECK(!report.passed());
    }
  }
}

IFABRIC_TEST(validate, results_are_deterministic_for_identical_input) {
  const std::string text = to_json(synthesize(core_domain(), [] {
                                     SynthOptions options;
                                     options.defect = SynthDefect::AdminStateDisagreement;
                                     return options;
                                   }())).dump();
  const ValidationReport first = validate_text(text);
  const ValidationReport second = validate_text(text);
  CHECK_EQ(first.render(), second.render());
  CHECK_EQ(first.to_json().dump(), second.to_json().dump());
  CHECK_EQ(first.error_count(), second.error_count());
}

IFABRIC_TEST(validate, malformed_text_reports_a_syntax_error) {
  const ValidationReport report = validate_text("{ not json");
  CHECK(!report.parsed);
  CHECK(!report.passed());
  CHECK(has_diagnostic(report, ErrorCode::JsonParseError, ValidationLayer::Syntax));
}

IFABRIC_TEST(validate, huge_declared_object_counts_are_rejected) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  DeviceDecl filler = document.devices.front();
  document.devices.resize(kMaxObjectsPerDocument + 1, filler);
  const ValidationReport report = validate_document(document, document.schema);
  CHECK(!report.passed());
  CHECK(has_diagnostic(report, ErrorCode::LimitExceeded, ValidationLayer::Syntax));
}

IFABRIC_TEST(validate, intent_property_subject_mismatch_is_rejected) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  REQUIRE(!document.intents.empty());
  document.intents.front().property = "isolation_id";  // only valid for tenants
  const ValidationReport report = validate_document(document, document.schema);
  CHECK(has_diagnostic(report, ErrorCode::AmbiguousSemantics, ValidationLayer::InvariantViolation));
}

IFABRIC_TEST(validate, intent_value_kind_is_checked) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  REQUIRE(!document.intents.empty());
  document.intents.front().desired = JsonValue::integer(7);
  const ValidationReport report = validate_document(document, document.schema);
  CHECK(has_diagnostic(report, ErrorCode::JsonTypeMismatch, ValidationLayer::InvariantViolation));
}

IFABRIC_TEST(validate, partitioned_topology_is_infeasible) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  for (auto& link : document.links) {
    if (link.kind == LinkKind::Fabric) {
      link.admin = AdminState::Disabled;
    }
  }
  const ValidationReport report = validate_document(document, document.schema);
  CHECK(has_diagnostic(report, ErrorCode::InfeasibleTopology, ValidationLayer::TopologyFeasibility));
}

IFABRIC_TEST(validate, service_class_budget_is_bounded) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  document.service_classes.front().bandwidth_share_permille = 900;
  const ValidationReport report = validate_document(document, document.schema);
  CHECK(has_diagnostic(report, ErrorCode::CapacityExceeded, ValidationLayer::Syntax));
}

IFABRIC_TEST(validate, capability_vocabulary_is_closed) {
  CHECK(is_known_capability("qos.pfc"));
  CHECK(!is_known_capability("qos.magic"));
  CHECK(find_property("admin_state") != nullptr);
  CHECK(find_property("not_a_property") == nullptr);
}
