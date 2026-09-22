// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/cli.hpp"

#include "ifabric/net.hpp"

#include <fstream>
#include <iostream>
#include <map>

namespace ifabric {

namespace {

struct Arguments {
  std::string command;
  std::vector<std::string> positional;
  std::map<std::string, std::string> options;
  std::vector<std::string> flags;
  std::string error;

  bool has(const std::string& name) const { return options.count(name) != 0; }
  std::string get(const std::string& name, const std::string& fallback = std::string()) const {
    const auto it = options.find(name);
    return it == options.end() ? fallback : it->second;
  }
  bool flag(const std::string& name) const {
    for (const auto& item : flags) {
      if (item == name) {
        return true;
      }
    }
    return false;
  }
};

Arguments parse_arguments(const std::vector<std::string>& args) {
  Arguments out;
  std::size_t index = 0;
  if (!args.empty()) {
    out.command = args[0];
    index = 1;
  }
  static const std::vector<std::string> kValueOptions = {
      "--store", "--file", "--actor", "--domain", "--out", "--from", "--to",
      "--limit", "--generation", "--host", "--port", "--expect-generation", "--seed",
      "--proposal", "--client"};
  static const std::vector<std::string> kFlags = {"--json", "--strict"};
  for (; index < args.size(); ++index) {
    const std::string& token = args[index];
    bool consumed = false;
    for (const auto& option : kValueOptions) {
      if (token == option) {
        if (index + 1 >= args.size()) {
          out.error = "option " + option + " requires a value";
          return out;
        }
        out.options[option] = args[index + 1];
        ++index;
        consumed = true;
        break;
      }
      if (token.size() > option.size() + 1 && token.compare(0, option.size(), option) == 0 &&
          token[option.size()] == '=') {
        out.options[option] = token.substr(option.size() + 1);
        consumed = true;
        break;
      }
    }
    if (consumed) {
      continue;
    }
    bool is_flag = false;
    for (const auto& flag_name : kFlags) {
      if (token == flag_name) {
        out.flags.push_back(token);
        is_flag = true;
        break;
      }
    }
    if (is_flag) {
      continue;
    }
    if (!token.empty() && token[0] == '-') {
      out.error = "unknown option " + token;
      return out;
    }
    out.positional.push_back(token);
  }
  return out;
}

Result<std::string> read_text_file(const std::string& path) {
  const auto text = read_file(Path(path), kMaxDocumentBytes);
  if (!text) {
    return text.error();
  }
  return text.value();
}

void print_json(std::ostream& out, const JsonValue& value) { out << value.dump_pretty(2) << "\n"; }

int emit_report(const ValidationReport& report, bool as_json, std::ostream& out) {
  if (as_json) {
    print_json(out, report.to_json());
  } else {
    out << report.render() << "\n";
  }
  return report.passed() ? 0 : 1;
}

std::string require_option(const Arguments& args, const std::string& name, std::ostream& err) {
  if (!args.has(name)) {
    err << "error: --" << name.substr(2) << " is required\n";
    return std::string();
  }
  return args.get(name);
}

Result<std::unique_ptr<IntentRuntime>> open_runtime(const std::string& directory) {
  return IntentRuntime::open(Path(directory));
}

int command_validate(const Arguments& args, std::ostream& out, std::ostream& err) {
  if (args.positional.empty()) {
    err << "error: validate requires an intent file\n";
    return 2;
  }
  const auto text = read_text_file(args.positional.front());
  if (!text) {
    err << "error: " << text.error().render() << "\n";
    return 1;
  }
  const ValidationReport report = validate_text(text.value());
  return emit_report(report, args.flag("--json"), out);
}

int command_normalize(const Arguments& args, std::ostream& out, std::ostream& err) {
  if (args.positional.empty()) {
    err << "error: normalize requires an intent file\n";
    return 2;
  }
  const auto text = read_text_file(args.positional.front());
  if (!text) {
    err << "error: " << text.error().render() << "\n";
    return 1;
  }
  const auto parsed = parse_json(text.value(), JsonParseLimits{});
  if (!parsed) {
    err << "error: " << parsed.error().render() << "\n";
    return 1;
  }
  const auto upgraded = upgrade_to_current(parsed.value());
  if (!upgraded) {
    err << "error: " << upgraded.error().render() << "\n";
    return 1;
  }
  const auto document = from_json(upgraded->upgraded);
  if (!document) {
    err << "error: " << document.error().render() << "\n";
    return 1;
  }
  const auto canonical = canonicalize(document.value());
  if (!canonical) {
    err << "error: " << canonical.error().render() << "\n";
    return 1;
  }
  const std::string rendered =
      args.flag("--json")
          ? [&] {
              JsonValue envelope;
              (void)envelope.set("content_digest",
                                 JsonValue::string(canonical->content_digest.hex()));
              (void)envelope.set("document_digest",
                                 JsonValue::string(canonical->document_digest.hex()));
              (void)envelope.set("document", canonical->document_json);
              return envelope.dump_pretty(2);
            }()
          : canonical->document_json.dump_pretty(2);
  if (args.has("--out")) {
    const Path target(args.get("--out"));
    const Error error = write_file_atomic(target, rendered + "\n", WriteOptions{true, false});
    if (!error.ok()) {
      err << "error: " << error.render() << "\n";
      return 1;
    }
    out << "wrote " << target.string() << "\n";
  } else {
    out << rendered << "\n";
  }
  return 0;
}

int command_digest(const Arguments& args, std::ostream& out, std::ostream& err) {
  if (args.positional.empty()) {
    err << "error: digest requires an intent file\n";
    return 2;
  }
  const auto text = read_text_file(args.positional.front());
  if (!text) {
    err << "error: " << text.error().render() << "\n";
    return 1;
  }
  const auto parsed = parse_json(text.value(), JsonParseLimits{});
  if (!parsed) {
    err << "error: " << parsed.error().render() << "\n";
    return 1;
  }
  const auto upgraded = upgrade_to_current(parsed.value());
  if (!upgraded) {
    err << "error: " << upgraded.error().render() << "\n";
    return 1;
  }
  const auto document = from_json(upgraded->upgraded);
  if (!document) {
    err << "error: " << document.error().render() << "\n";
    return 1;
  }
  const auto canonical = canonicalize(document.value());
  if (!canonical) {
    err << "error: " << canonical.error().render() << "\n";
    return 1;
  }
  if (args.flag("--json")) {
    JsonValue envelope;
    (void)envelope.set("content_digest", JsonValue::string(canonical->content_digest.hex()));
    (void)envelope.set("document_digest", JsonValue::string(canonical->document_digest.hex()));
    (void)envelope.set("schema", JsonValue::string(canonical->normalized.schema.str()));
    (void)envelope.set("domain", JsonValue::string(canonical->normalized.domain.str()));
    (void)envelope.set("objects", JsonValue::integer(static_cast<std::int64_t>(
                                       canonical->normalized.declared_object_count())));
    print_json(out, envelope);
  } else {
    out << "content  " << canonical->content_digest.hex() << "\n";
    out << "document " << canonical->document_digest.hex() << "\n";
    out << "schema   " << canonical->normalized.schema.str() << "\n";
    out << "domain   " << canonical->normalized.domain.str() << "\n";
    out << "objects  " << canonical->normalized.declared_object_count() << "\n";
  }
  return 0;
}

int command_explain(const Arguments& args, std::ostream& out, std::ostream& err) {
  if (args.positional.empty()) {
    err << "error: explain requires an intent file\n";
    return 2;
  }
  const auto text = read_text_file(args.positional.front());
  if (!text) {
    err << "error: " << text.error().render() << "\n";
    return 1;
  }
  const ValidationReport report = validate_text(text.value());
  if (args.flag("--json")) {
    JsonValue envelope;
    (void)envelope.set("committed", JsonValue::boolean(false));
    (void)envelope.set("reason", JsonValue::string(report.passed()
                                                       ? "the proposal would be accepted"
                                                       : "the proposal would be rejected"));
    (void)envelope.set("report", report.to_json());
    print_json(out, envelope);
  } else {
    out << (report.passed() ? "this proposal would be committed\n"
                            : "this proposal would be REJECTED\n");
    out << report.render() << "\n";
    if (!report.passed()) {
      out << "\nrejection is deterministic: identical input always yields these "
             "diagnostics and conflict identities.\n";
    }
  }
  return report.passed() ? 0 : 1;
}

int command_schema(std::ostream& out) {
  out << "schema name: " << kSchemaName << "\n";
  out << "supported majors: " << SchemaVersion::kSupportedMajor << "\n";
  out << "supported minors: " << SchemaVersion::kOldestMinor << " .. "
      << SchemaVersion::kSupportedMinor << "\n";
  out << "current version: " << SchemaVersion::kSupportedMajor << "."
      << SchemaVersion::kSupportedMinor << "\n";
  out << "\nfeatures:\n";
  for (const auto& feature : known_features()) {
    out << "  " << feature.name << "  introduced=" << SchemaVersion::kSupportedMajor << "."
        << feature.introduced_minor << (feature.requires_opt_in ? "  optional-only" : "") << "\n";
  }
  out << "\nmigrations:\n"
      << "  1.0 -> 1.1  port.speed -> port.speed_bps, link.bandwidth -> link.capacity_bps\n"
      << "  1.1 -> 1.2  maintenance-eligibility defaults\n"
      << "  1.2 -> 1.3  port-count field renames\n";
  return 0;
}

int command_capabilities(std::ostream& out) {
  for (const auto& capability : known_capabilities()) {
    out << capability << "\n";
  }
  return 0;
}

int command_properties(std::ostream& out) {
  for (const auto& property : known_properties()) {
    out << property.name << "  subjects=";
    for (std::size_t i = 0; i < property.subjects.size(); ++i) {
      if (i != 0) {
        out << ",";
      }
      out << id_class_name(property.subjects[i]);
    }
    if (!property.domain.empty()) {
      out << "  enum=" << property.domain;
    }
    out << "\n";
  }
  return 0;
}

int command_init(const Arguments& args, std::ostream& out, std::ostream& err) {
  const std::string directory = require_option(args, "--store", err);
  const std::string domain = require_option(args, "--domain", err);
  if (directory.empty() || domain.empty()) {
    return 2;
  }
  const auto parsed = DomainId::parse(domain);
  if (!parsed) {
    err << "error: " << parsed.error().render() << "\n";
    return 2;
  }
  const auto runtime = IntentRuntime::create(Path(directory), parsed.value());
  if (!runtime) {
    err << "error: " << runtime.error().render() << "\n";
    return 1;
  }
  out << "created intent store " << directory << " for domain " << domain << "\n";
  return 0;
}

int command_commit(const Arguments& args, std::ostream& out, std::ostream& err) {
  const std::string directory = require_option(args, "--store", err);
  const std::string file = require_option(args, "--file", err);
  const std::string actor = require_option(args, "--actor", err);
  if (directory.empty() || file.empty() || actor.empty()) {
    return 2;
  }
  const auto parsed_actor = ActorId::parse(actor);
  if (!parsed_actor) {
    err << "error: " << parsed_actor.error().render() << "\n";
    return 2;
  }
  const auto text = read_text_file(file);
  if (!text) {
    err << "error: " << text.error().render() << "\n";
    return 1;
  }
  const auto runtime = open_runtime(directory);
  if (!runtime) {
    err << "error: " << runtime.error().render() << "\n";
    return 1;
  }
  const auto parsed = parse_json(text.value(), JsonParseLimits{});
  if (!parsed) {
    err << "error: " << parsed.error().render() << "\n";
    return 1;
  }
  const auto document = from_json(parsed.value());
  if (!document) {
    err << "error: " << document.error().render() << "\n";
    return 1;
  }
  FenceToken token = runtime.value()->fence(parsed_actor.value());
  if (args.has("--expect-generation")) {
    const auto expected = GenerationNumber::from(
        std::strtoull(args.get("--expect-generation").c_str(), nullptr, 10));
    if (!expected) {
      err << "error: " << expected.error().render() << "\n";
      return 2;
    }
    token.expected_generation = expected.value();
  }
  const auto proposal = runtime.value()->propose(document.value(), parsed_actor.value());
  if (!proposal) {
    err << "error: " << proposal.error().render() << "\n";
    return 1;
  }
  const auto report = runtime.value()->validate_proposal(proposal.value());
  if (!report) {
    err << "commit REJECTED: " << report.error().render() << "\n";
    const auto inspected = runtime.value()->inspect_proposal(proposal.value());
    if (inspected) {
      if (args.flag("--json")) {
        print_json(out, inspected->to_json());
      } else {
        out << inspected->report.render() << "\n";
      }
    }
    return 1;
  }
  const auto outcome = runtime.value()->commit_proposal(proposal.value(), token);
  if (!outcome) {
    err << "commit FAILED: " << outcome.error().render() << "\n";
    return 1;
  }
  if (args.flag("--json")) {
    JsonValue envelope;
    (void)envelope.set("generation", JsonValue::string(outcome->reference.number.str()));
    (void)envelope.set("content_digest", JsonValue::string(outcome->reference.content.hex()));
    (void)envelope.set("impact", JsonValue::string(std::string(to_string(outcome->impact))));
    (void)envelope.set("diff", outcome->diff.to_json());
    print_json(out, envelope);
  } else {
    out << "committed generation " << outcome->reference.number.str() << " content "
        << outcome->reference.content.short_hex() << " impact "
        << to_string(outcome->impact) << "\n";
    out << outcome->diff.render() << "\n";
  }
  return 0;
}

int command_generations(const Arguments& args, std::ostream& out, std::ostream& err) {
  const std::string directory = require_option(args, "--store", err);
  if (directory.empty()) {
    return 2;
  }
  const auto runtime = open_runtime(directory);
  if (!runtime) {
    err << "error: " << runtime.error().render() << "\n";
    return 1;
  }
  std::size_t limit = 0;
  if (args.has("--limit")) {
    limit = static_cast<std::size_t>(std::strtoull(args.get("--limit").c_str(), nullptr, 10));
  }
  const auto summaries = runtime.value()->generations(limit);
  if (!summaries) {
    err << "error: " << summaries.error().render() << "\n";
    return 1;
  }
  if (args.flag("--json")) {
    JsonArray items;
    for (const auto& summary : summaries.value()) {
      JsonValue entry;
      (void)entry.set("generation", JsonValue::string(summary.number.str()));
      (void)entry.set("content_digest", JsonValue::string(summary.content.hex()));
      (void)entry.set("document_digest", JsonValue::string(summary.document.hex()));
      (void)entry.set("committed_at", JsonValue::string(format_timestamp(summary.committed_at)));
      (void)entry.set("writer", JsonValue::string(summary.writer.str()));
      (void)entry.set("epoch", JsonValue::string(summary.epoch.str()));
      (void)entry.set("record_bytes",
                      JsonValue::integer(static_cast<std::int64_t>(summary.record_bytes)));
      items.push_back(std::move(entry));
    }
    print_json(out, JsonValue::array(std::move(items)));
    return 0;
  }
  if (summaries.value().empty()) {
    out << "no committed generations\n";
    return 0;
  }
  out << "generation  content            writer                committed_at\n";
  for (const auto& summary : summaries.value()) {
    out << "  " << summary.number.str() << "        " << summary.content.short_hex() << "       "
        << summary.writer.str() << "   " << format_timestamp(summary.committed_at) << "\n";
  }
  return 0;
}

int command_show(const Arguments& args, std::ostream& out, std::ostream& err) {
  const std::string directory = require_option(args, "--store", err);
  if (directory.empty()) {
    return 2;
  }
  const auto runtime = open_runtime(directory);
  if (!runtime) {
    err << "error: " << runtime.error().render() << "\n";
    return 1;
  }
  GenerationNumber number = GenerationNumber::genesis();
  if (args.has("--generation")) {
    const auto parsed = GenerationNumber::from(
        std::strtoull(args.get("--generation").c_str(), nullptr, 10));
    if (!parsed) {
      err << "error: " << parsed.error().render() << "\n";
      return 2;
    }
    number = parsed.value();
  } else {
    const auto head = runtime.value()->head();
    if (!head) {
      err << "error: " << head.error().render() << "\n";
      return 1;
    }
    number = head->number;
  }
  const auto document = runtime.value()->document_at(number);
  if (!document) {
    err << "error: " << document.error().render() << "\n";
    return 1;
  }
  if (args.flag("--json")) {
    print_json(out, to_json(document.value()));
  } else {
    out << to_json(document.value()).dump_pretty(2) << "\n";
  }
  return 0;
}

int command_diff(const Arguments& args, std::ostream& out, std::ostream& err) {
  const std::string directory = require_option(args, "--store", err);
  const std::string from = require_option(args, "--from", err);
  const std::string to = require_option(args, "--to", err);
  if (directory.empty() || from.empty() || to.empty()) {
    return 2;
  }
  const auto runtime = open_runtime(directory);
  if (!runtime) {
    err << "error: " << runtime.error().render() << "\n";
    return 1;
  }
  const auto from_number = GenerationNumber::from(std::strtoull(from.c_str(), nullptr, 10));
  const auto to_number = GenerationNumber::from(std::strtoull(to.c_str(), nullptr, 10));
  if (!from_number || !to_number) {
    err << "error: --from and --to must be committed generation numbers\n";
    return 2;
  }
  const auto before = runtime.value()->document_at(from_number.value());
  const auto after = runtime.value()->document_at(to_number.value());
  if (!before) {
    err << "error: " << before.error().render() << "\n";
    return 1;
  }
  if (!after) {
    err << "error: " << after.error().render() << "\n";
    return 1;
  }
  const SemanticDiff diff = diff_documents(before.value(), after.value());
  if (args.flag("--json")) {
    print_json(out, diff.to_json());
  } else {
    out << diff.render() << "\n";
  }
  return 0;
}

int command_verify(const Arguments& args, std::ostream& out, std::ostream& err) {
  const std::string directory = require_option(args, "--store", err);
  if (directory.empty()) {
    return 2;
  }
  const auto runtime = open_runtime(directory);
  if (!runtime) {
    err << "error: " << runtime.error().render() << "\n";
    return 1;
  }
  const VerificationReport report = runtime.value()->verify_store();
  if (args.flag("--json")) {
    print_json(out, report.to_json());
  } else {
    out << report.render() << "\n";
  }
  return report.ok() ? 0 : 1;
}

int command_serve(const Arguments& args, std::ostream& out, std::ostream& err) {
  const std::string directory = require_option(args, "--store", err);
  if (directory.empty()) {
    return 2;
  }
  const std::string host = args.get("--host", "127.0.0.1");
  const std::uint16_t port = static_cast<std::uint16_t>(
      std::strtoul(args.get("--port", std::to_string(net::kDefaultPort)).c_str(), nullptr, 10));
  const auto runtime = open_runtime(directory);
  if (!runtime) {
    err << "error: " << runtime.error().render() << "\n";
    return 1;
  }
  const auto server = net::IntentServer::start(*runtime.value(), host, port);
  if (!server) {
    err << "error: " << server.error().render() << "\n";
    return 1;
  }
  out << "ifabricd listening on " << server.value()->endpoint() << " domain "
      << runtime.value()->domain().str() << " epoch " << runtime.value()->epoch().str()
      << " incarnation " << runtime.value()->incarnation().str() << "\n";
  out.flush();
  const Error error = server.value()->run();
  (void)server.value()->shutdown();
  if (!error.ok()) {
    err << "error: " << error.render() << "\n";
    return 1;
  }
  return 0;
}

int command_remote(const Arguments& args, std::ostream& out, std::ostream& err) {
  const std::string host = args.get("--host", "127.0.0.1");
  const std::uint16_t port = static_cast<std::uint16_t>(
      std::strtoul(args.get("--port", std::to_string(net::kDefaultPort)).c_str(), nullptr, 10));
  net::ClientOptions options;
  if (args.has("--actor")) {
    const auto actor = ActorId::parse(args.get("--actor"));
    if (!actor) {
      err << "error: " << actor.error().render() << "\n";
      return 2;
    }
    options.actor = actor.value();
  }
  const auto client = net::IntentClient::connect(host, port, options);
  if (!client) {
    err << "error: " << client.error().render() << "\n";
    return 1;
  }
  const std::string subcommand = args.positional.empty() ? "head" : args.positional.front();
  if (subcommand == "head") {
    const auto head = client.value()->get_head();
    if (!head) {
      err << "error: " << head.error().render() << "\n";
      return 1;
    }
    print_json(out, head.value());
    return 0;
  }
  if (subcommand == "stats") {
    const auto stats = client.value()->stats();
    if (!stats) {
      err << "error: " << stats.error().render() << "\n";
      return 1;
    }
    print_json(out, stats.value());
    return 0;
  }
  if (subcommand == "generations") {
    JsonValue request_body;
    (void)request_body.set("limit", JsonValue::integer(64));
    const auto list = client.value()->call(net::MessageType::ListGenerations, request_body);
    if (!list) {
      err << "error: " << list.error().render() << "\n";
      return 1;
    }
    print_json(out, list.value());
    return 0;
  }
  if (subcommand == "verify") {
    JsonValue request_body;
    const auto report = client.value()->call(net::MessageType::Verify, request_body);
    if (!report) {
      err << "error: " << report.error().render() << "\n";
      return 1;
    }
    print_json(out, report.value());
    return 0;
  }
  if (subcommand == "commit") {
    const std::string file = require_option(args, "--file", err);
    if (file.empty()) {
      return 2;
    }
    const auto text = read_text_file(file);
    if (!text) {
      err << "error: " << text.error().render() << "\n";
      return 1;
    }
    const auto document = [&]() -> Result<IntentDocument> {
      const auto parsed = parse_json(text.value(), JsonParseLimits{});
      if (!parsed) {
        return parsed.error();
      }
      return from_json(parsed.value());
    }();
    if (!document) {
      err << "error: " << document.error().render() << "\n";
      return 1;
    }
    JsonValue session;
    const JsonValue* hello_session = client.value()->hello().find("session");
    if (hello_session != nullptr) {
      session = *hello_session;
    }
    FenceToken token;
    token.writer = options.actor;
    token.expected_epoch = client.value()->session_epoch();
    token.expected_incarnation = client.value()->session_incarnation();
    if (const JsonValue* head = client.value()->hello().find("head");
        head != nullptr && head->try_string() != nullptr) {
      const auto parsed = GenerationNumber::from(std::strtoull(head->try_string()->c_str(), nullptr, 10));
      token.expected_generation = parsed ? parsed.value() : GenerationNumber::genesis();
    }
    const auto outcome = client.value()->submit(document.value(), token);
    if (!outcome) {
      err << "commit FAILED: " << outcome.error().render() << "\n";
      return 1;
    }
    print_json(out, outcome.value());
    return 0;
  }
  if (subcommand == "shutdown") {
    JsonValue request_body;
    const auto response = client.value()->call(net::MessageType::Shutdown, request_body);
    if (!response) {
      err << "error: " << response.error().render() << "\n";
      return 1;
    }
    print_json(out, response.value());
    return 0;
  }
  err << "error: unknown remote subcommand '" << subcommand << "'\n";
  return 2;
}

}  // namespace

int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  const Arguments parsed = parse_arguments(args);
  if (!parsed.error.empty()) {
    err << "error: " << parsed.error << "\n" << kCliUsage;
    return 2;
  }
  if (parsed.command.empty() || parsed.command == "help" || parsed.command == "--help" ||
      parsed.command == "-h") {
    out << kCliUsage;
    return parsed.command.empty() ? 2 : 0;
  }
  if (parsed.command == "version") {
    out << kProductName << " " << kVersionString << " (" << kVendorName << ")\n";
    return 0;
  }
  if (parsed.command == "validate") {
    return command_validate(parsed, out, err);
  }
  if (parsed.command == "normalize") {
    return command_normalize(parsed, out, err);
  }
  if (parsed.command == "digest") {
    return command_digest(parsed, out, err);
  }
  if (parsed.command == "explain") {
    return command_explain(parsed, out, err);
  }
  if (parsed.command == "schema") {
    return command_schema(out);
  }
  if (parsed.command == "capabilities") {
    return command_capabilities(out);
  }
  if (parsed.command == "properties") {
    return command_properties(out);
  }
  if (parsed.command == "init") {
    return command_init(parsed, out, err);
  }
  if (parsed.command == "commit") {
    return command_commit(parsed, out, err);
  }
  if (parsed.command == "generations") {
    return command_generations(parsed, out, err);
  }
  if (parsed.command == "show") {
    return command_show(parsed, out, err);
  }
  if (parsed.command == "diff") {
    return command_diff(parsed, out, err);
  }
  if (parsed.command == "verify") {
    return command_verify(parsed, out, err);
  }
  if (parsed.command == "serve") {
    return command_serve(parsed, out, err);
  }
  if (parsed.command == "remote") {
    return command_remote(parsed, out, err);
  }
  err << "error: unknown command '" << parsed.command << "'\n" << kCliUsage;
  return 2;
}

int run_daemon(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  std::vector<std::string> forwarded;
  forwarded.emplace_back("serve");
  for (const auto& arg : args) {
    forwarded.push_back(arg);
  }
  return run_cli(forwarded, out, err);
}

}  // namespace ifabric
