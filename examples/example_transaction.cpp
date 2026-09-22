// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Runs one complete intent transaction against a throwaway store:
// propose -> validate -> compare -> commit generation -> publish, then reopens
// the store to show that the committed generation survived the restart.

#include "ifabric/runtime.hpp"
#include "ifabric/synth.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

int main() {
  const auto domain = ifabric::DomainId::parse("dom.example");
  const auto actor = ifabric::ActorId::parse("actor.example");
  if (!domain || !actor) {
    return 1;
  }
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("ifabric-example-" + std::to_string(ifabric::current_process_id()) + "-" +
       std::to_string(stamp));
  std::error_code error;
  std::filesystem::remove_all(directory, error);

  auto runtime = ifabric::IntentRuntime::create(directory, domain.value());
  if (!runtime) {
    std::cerr << runtime.error().render() << "\n";
    return 1;
  }
  const auto subscription = runtime.value()->subscribe();
  if (!subscription) {
    std::cerr << subscription.error().render() << "\n";
    return 1;
  }

  ifabric::SynthOptions options;
  ifabric::IntentDocument first = ifabric::synthesize(domain.value(), options);
  first.provenance.actor = actor.value();
  first.provenance.description = "generation 1";

  const auto proposal = runtime.value()->propose(first, actor.value());
  if (!proposal) {
    std::cerr << proposal.error().render() << "\n";
    return 1;
  }
  const auto report = runtime.value()->validate_proposal(proposal.value());
  if (!report) {
    std::cerr << report.error().render() << "\n";
    return 1;
  }
  std::cout << report->render() << "\n";
  const auto diff = runtime.value()->compare_proposal(proposal.value());
  if (!diff) {
    std::cerr << diff.error().render() << "\n";
    return 1;
  }
  std::cout << "\nsemantic diff against the empty domain:\n" << diff->render() << "\n";
  const auto outcome = runtime.value()->commit_proposal(
      proposal.value(), runtime.value()->fence(actor.value()));
  if (!outcome) {
    std::cerr << outcome.error().render() << "\n";
    return 1;
  }
  std::cout << "\ncommitted generation " << outcome->reference.number.str() << " content "
            << outcome->reference.content.short_hex() << "\n";
  const auto publication = runtime.value()->poll(subscription.value());
  if (publication && publication->has_value()) {
    std::cout << "published sequence " << publication->value().sequence << " generation "
              << publication->value().reference.number.str() << "\n";
  }

  // A second generation that reduces the capacity of one fabric uplink. The
  // semantic diff classifies it as service affecting rather than merely
  // subtractive.
  ifabric::IntentDocument second = first;
  second.links.front().capacity_bps /= 2;
  second.provenance.description = "generation 2";
  const auto second_outcome = runtime.value()->submit(
      second, actor.value(), runtime.value()->fence(actor.value()));
  if (!second_outcome) {
    std::cerr << second_outcome.error().render() << "\n";
    return 1;
  }
  std::cout << "\ncommitted generation " << second_outcome->reference.number.str()
            << " worst impact " << ifabric::to_string(second_outcome->impact) << "\n";
  std::cout << second_outcome->diff.render() << "\n";

  runtime.value().reset();

  auto reopened = ifabric::IntentRuntime::open(directory);
  if (!reopened) {
    std::cerr << reopened.error().render() << "\n";
    return 1;
  }
  const auto head = reopened.value()->head();
  if (!head) {
    std::cerr << head.error().render() << "\n";
    return 1;
  }
  std::cout << "\nafter restart: head generation " << head->number.str() << " content "
            << head->content.short_hex() << " epoch " << head->committed_epoch.str() << "\n";
  std::cout << "store verification: " << reopened.value()->verify_store().render() << "\n";
  reopened.value().reset();
  std::filesystem::remove_all(directory, error);
  return 0;
}
