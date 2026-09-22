// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// End-to-end command line tests. Every case runs the real ifabric binary as a
// separate OS process.

#include "ifabric/io.hpp"
#include "ifabric/synth.hpp"
#include "ifabric_test.hpp"
#include "process.hpp"

#include <iostream>

using namespace ifabric;

namespace {

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

std::string write_document(const std::filesystem::path& path, const IntentDocument& document) {
  const JsonValue json = to_json(document);
  const Error error =
      write_file_atomic(path, json.dump_pretty(2) + "\n", WriteOptions{false, false});
  if (!error.ok()) {
    ifabric_test::report_failure(__FILE__, __LINE__,
                                 "cannot write fixture " + path.string() + ": " + error.render());
  }
  return path.string();
}

ifabric_test::ProcessResult cli(const std::vector<std::string>& args) {
  return ifabric_test::run_process(IFABRIC_CLI_PATH, args);
}

}  // namespace

IFABRIC_TEST(cli, version_reports_the_product_identity) {
  const auto result = cli({"version"});
  CHECK_EQ(result.exit_code, 0);
  CHECK(result.output.find("Intent Fabric 1.0.0") != std::string::npos);
  CHECK(result.output.find("Summon Software Labs") != std::string::npos);
}

IFABRIC_TEST(cli, schema_capabilities_and_properties_are_listed) {
  const auto schema = cli({"schema"});
  CHECK_EQ(schema.exit_code, 0);
  CHECK(schema.output.find("ifabric.intent") != std::string::npos);
  CHECK(schema.output.find("1.3") != std::string::npos);
  CHECK(schema.output.find("1.0 -> 1.1") != std::string::npos);
  const auto capabilities = cli({"capabilities"});
  CHECK_EQ(capabilities.exit_code, 0);
  CHECK(capabilities.output.find("qos.pfc") != std::string::npos);
  const auto properties = cli({"properties"});
  CHECK_EQ(properties.exit_code, 0);
  CHECK(properties.output.find("admin_state") != std::string::npos);
}

IFABRIC_TEST(cli, unknown_command_is_a_usage_error) {
  const auto result = cli({"not-a-command"});
  CHECK_EQ(result.exit_code, 2);
  CHECK(result.output.find("unknown command") != std::string::npos);
}

IFABRIC_TEST(cli, missing_file_is_an_operational_failure) {
  const auto result = cli({"validate", "definitely-missing-file.json"});
  CHECK_EQ(result.exit_code, 1);
}

IFABRIC_TEST(cli, validate_normalize_digest_and_explain) {
  ifabric_test::TempDirectory directory("cli-basics");
  const std::string path =
      write_document(directory.child("intent.json"), synthesize(core_domain(), SynthOptions{}));

  const auto valid = cli({"validate", path});
  CHECK_EQ(valid.exit_code, 0);
  CHECK(valid.output.find("validation PASSED") != std::string::npos);

  const auto json = cli({"validate", path, "--json"});
  CHECK_EQ(json.exit_code, 0);
  const auto parsed = parse_json(json.output, JsonParseLimits{});
  REQUIRE(parsed.ok());
  CHECK_EQ(*parsed->find("passed")->try_bool(), true);
  CHECK_EQ(*parsed->find("error_count")->try_int(), 0ll);

  const std::string normalized = directory.child("normalized.json").string();
  const auto normalize = cli({"normalize", path, "--out", normalized});
  CHECK_EQ(normalize.exit_code, 0);
  CHECK(file_exists(Path(normalized)));
  const auto revalidated = cli({"validate", normalized});
  CHECK_EQ(revalidated.exit_code, 0);

  const auto digest = cli({"digest", path});
  CHECK_EQ(digest.exit_code, 0);
  CHECK(digest.output.find("content  ") != std::string::npos);
  CHECK(digest.output.find("document ") != std::string::npos);

  const auto explained = cli({"explain", path});
  CHECK_EQ(explained.exit_code, 0);
  CHECK(explained.output.find("would be committed") != std::string::npos);

  SynthOptions defective;
  defective.defect = SynthDefect::UnknownCapability;
  const std::string bad_path =
      write_document(directory.child("bad.json"), synthesize(core_domain(), defective));
  const auto rejected = cli({"validate", bad_path, "--json"});
  CHECK_EQ(rejected.exit_code, 1);
  const auto rejected_json = parse_json(rejected.output, JsonParseLimits{});
  REQUIRE(rejected_json.ok());
  CHECK_EQ(*rejected_json->find("passed")->try_bool(), false);
  const auto explanation = cli({"explain", bad_path});
  CHECK_EQ(explanation.exit_code, 1);
  CHECK(explanation.output.find("REJECTED") != std::string::npos);
}

IFABRIC_TEST(cli, commit_generations_show_diff_and_verify) {
  ifabric_test::TempDirectory directory("cli-lifecycle");
  const std::string store = directory.child("store").string();

  const auto init = cli({"init", "--store", store, "--domain", "dom.core"});
  CHECK_EQ(init.exit_code, 0);

  const IntentDocument first = synthesize(core_domain(), SynthOptions{});
  const std::string first_path = write_document(directory.child("gen1.json"), first);
  const auto commit_first =
      cli({"commit", "--store", store, "--file", first_path, "--actor", "actor.cli"});
  if (commit_first.exit_code != 0) {
    std::cout << commit_first.output << "\n";
  }
  CHECK_EQ(commit_first.exit_code, 0);
  CHECK(commit_first.output.find("committed generation 1") != std::string::npos);

  IntentDocument second = first;
  second.intents.front().desired = JsonValue::string("disabled");
  const std::string second_path = write_document(directory.child("gen2.json"), second);
  const auto commit_second =
      cli({"commit", "--store", store, "--file", second_path, "--actor", "actor.cli"});
  CHECK_EQ(commit_second.exit_code, 0);
  CHECK(commit_second.output.find("committed generation 2") != std::string::npos);

  const auto generations = cli({"generations", "--store", store});
  CHECK_EQ(generations.exit_code, 0);
  CHECK(generations.output.find("generation  content") != std::string::npos);

  const auto shown = cli({"show", "--store", store, "--generation", "1", "--json"});
  CHECK_EQ(shown.exit_code, 0);
  const auto shown_json = parse_json(shown.output, JsonParseLimits{});
  REQUIRE(shown_json.ok());
  CHECK(shown_json->find("topology") != nullptr);

  const auto diff = cli({"diff", "--store", store, "--from", "1", "--to", "2", "--json"});
  CHECK_EQ(diff.exit_code, 0);
  const auto diff_json = parse_json(diff.output, JsonParseLimits{});
  REQUIRE(diff_json.ok());
  CHECK_EQ(*diff_json->find("worst_impact")->try_string(), std::string("disruptive"));

  const auto verified = cli({"verify", "--store", store});
  CHECK_EQ(verified.exit_code, 0);
  CHECK(verified.output.find("store OK") != std::string::npos);

  const auto stale = cli({"commit", "--store", store, "--file", second_path, "--actor",
                          "actor.cli", "--expect-generation", "1"});
  CHECK_EQ(stale.exit_code, 1);
  CHECK(stale.output.find("StaleGeneration") != std::string::npos);

  const auto after = cli({"generations", "--store", store, "--json"});
  CHECK_EQ(after.exit_code, 0);
  const auto after_json = parse_json(after.output, JsonParseLimits{});
  REQUIRE(after_json.ok());
  CHECK_EQ(after_json->size(), std::size_t{2});
}

IFABRIC_TEST(cli, normalize_is_byte_stable_across_repeated_runs) {
  ifabric_test::TempDirectory directory("cli-stability");
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const std::string first = write_document(directory.child("a.json"), document);
  const std::string first_out = directory.child("a.out.json").string();
  const std::string second_out = directory.child("b.out.json").string();
  CHECK_EQ(cli({"normalize", first, "--out", first_out}).exit_code, 0);
  const auto first_bytes = read_file(Path(first_out), 1u << 24);
  REQUIRE(first_bytes.ok());
  const auto parsed = parse_json(first_bytes.value(), JsonParseLimits{});
  REQUIRE(parsed.ok());
  const auto reloaded = from_json(parsed.value());
  REQUIRE(reloaded.ok());
  const std::string second = write_document(directory.child("b.json"), reloaded.value());
  CHECK_EQ(cli({"normalize", second, "--out", second_out}).exit_code, 0);
  const auto second_bytes = read_file(Path(second_out), 1u << 24);
  REQUIRE(second_bytes.ok());
  CHECK_EQ(first_bytes.value(), second_bytes.value());
}
