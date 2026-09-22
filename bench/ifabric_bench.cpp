// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Benchmarks measure completed work: every timed unit is a fully parsed,
// canonicalized, validated, conflicted, diffed or durably committed intent
// generation. Enqueue or submission latency is never reported.

#include "ifabric/canonical.hpp"
#include "ifabric/conflict.hpp"
#include "ifabric/diff.hpp"
#include "ifabric/runtime.hpp"
#include "ifabric/synth.hpp"
#include "ifabric/validate.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
  std::string name;
  double seconds = 0.0;
  std::size_t iterations = 0;
  std::size_t bytes = 0;

  double per_second() const {
    return seconds > 0.0 ? static_cast<double>(iterations) / seconds : 0.0;
  }
  double bytes_per_second() const {
    return seconds > 0.0 ? static_cast<double>(bytes) / seconds : 0.0;
  }
};

ifabric::DomainId example_domain() { return ifabric::DomainId::parse("dom.bench").value(); }

template <class Body>
Measurement measure(const std::string& name, std::size_t iterations, Body body) {
  Measurement measurement;
  measurement.name = name;
  measurement.iterations = iterations;
  const auto start = Clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    measurement.bytes += body(i);
  }
  const auto finish = Clock::now();
  measurement.seconds = std::chrono::duration<double>(finish - start).count();
  return measurement;
}

void report(const Measurement& measurement) {
  std::printf("%-34s %10zu units  %9.3f s  %12.1f units/s  %10.2f MiB/s\n",
              measurement.name.c_str(), measurement.iterations, measurement.seconds,
              measurement.per_second(), measurement.bytes_per_second() / (1024.0 * 1024.0));
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t scale = 1;
  if (argc > 1) {
    scale = static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10));
    if (scale == 0) {
      scale = 1;
    }
  }

  ifabric::SynthOptions options;
  options.leaves = 8;
  options.spines = 4;
  options.servers_per_leaf = 4;
  options.seed = 20260101;
  const ifabric::IntentDocument reference = ifabric::synthesize(example_domain(), options);
  const std::string text = ifabric::to_json(reference).dump();
  std::printf("Intent Fabric %s benchmark\n", std::string(ifabric::kVersionString).c_str());
  std::printf("reference document: %zu objects, %zu canonical bytes\n\n",
              reference.declared_object_count(), text.size());
  std::fflush(stdout);

  std::vector<Measurement> measurements;

  measurements.push_back(measure("parse + model build", 20 * scale, [&](std::size_t) {
    const auto parsed = ifabric::parse_json(text, ifabric::JsonParseLimits{});
    const auto document = ifabric::from_json(parsed.value());
    return document.ok() ? text.size() : std::size_t{0};
  }));
  measurements.push_back(measure("canonicalize", 60 * scale, [&](std::size_t) {
    const auto form = ifabric::canonicalize(reference);
    return form.ok() ? form->document_json.dump().size() : std::size_t{0};
  }));
  measurements.push_back(measure("validate (all layers)", 20 * scale, [&](std::size_t) {
    const ifabric::ValidationReport report =
        ifabric::validate_document(reference, reference.schema);
    return report.passed() ? reference.declared_object_count() : std::size_t{0};
  }));
  const ifabric::IntentDocument normalized_reference = ifabric::normalize(reference);
  const ifabric::TopologyIndex reference_index(normalized_reference);
  measurements.push_back(measure("conflict detection", 40 * scale, [&](std::size_t) {
    return ifabric::detect_conflicts(normalized_reference, reference_index).size();
  }));

  ifabric::IntentDocument changed = reference;
  changed.devices.front().admin = ifabric::AdminState::Disabled;
  changed.links.front().capacity_bps /= 2;
  measurements.push_back(measure("semantic diff", 40 * scale, [&](std::size_t) {
    return ifabric::diff_documents(reference, changed).entries.size();
  }));

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "ifabric-bench-store";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  {
    auto runtime = ifabric::IntentRuntime::create(directory, example_domain());
    if (!runtime) {
      std::cerr << runtime.error().render() << "\n";
      return 1;
    }
    const auto actor = ifabric::ActorId::parse("actor.bench").value();
    const std::size_t commits = 20 * scale;
    std::size_t committed_bytes = 0;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < commits; ++i) {
      ifabric::IntentDocument document = reference;
      document.provenance.description = "benchmark generation " + std::to_string(i);
      document.devices.front().admin =
          (i % 2 == 0) ? ifabric::AdminState::Enabled : ifabric::AdminState::Draining;
      const auto outcome =
          runtime.value()->submit(document, actor, runtime.value()->fence(actor));
      if (!outcome) {
        std::cerr << "commit failed: " << outcome.error().render() << "\n";
        return 1;
      }
      committed_bytes += text.size();
    }
    const auto finish = Clock::now();
    Measurement durable;
    durable.name = "durable commit (fsync)";
    durable.iterations = commits;
    durable.bytes = committed_bytes;
    durable.seconds = std::chrono::duration<double>(finish - start).count();
    measurements.push_back(durable);
    const auto stats = runtime.value()->stats();
    std::printf("committed head generation %s after %llu commits\n\n",
                stats.head.str().c_str(),
                static_cast<unsigned long long>(stats.proposals_committed));
  }

  measurements.push_back(measure("store reopen + full verify", 4 * scale, [&](std::size_t) {
    auto runtime = ifabric::IntentRuntime::open(directory);
    if (!runtime) {
      return std::size_t{0};
    }
    const ifabric::VerificationReport report = runtime.value()->verify_store();
    return report.ok() ? report.generations_checked : std::size_t{0};
  }));

  for (const auto& measurement : measurements) {
    report(measurement);
  }
  std::filesystem::remove_all(directory, error);
  return 0;
}
