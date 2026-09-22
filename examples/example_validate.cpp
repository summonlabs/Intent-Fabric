// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Validates an intent file supplied on the command line, or a synthetic
// reference document when no path is given, and prints the canonical identity.

#include "ifabric/io.hpp"
#include "ifabric/synth.hpp"
#include "ifabric/validate.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  std::string text;
  if (argc > 1) {
    const auto file = ifabric::read_file(ifabric::Path(argv[1]), ifabric::kMaxDocumentBytes);
    if (!file) {
      std::cerr << file.error().render() << "\n";
      return 1;
    }
    text = file.value();
  } else {
    const auto domain = ifabric::DomainId::parse("dom.example");
    if (!domain) {
      return 1;
    }
    text = ifabric::to_json(ifabric::synthesize(domain.value(), ifabric::SynthOptions{})).dump();
  }
  const ifabric::ValidationReport report = ifabric::validate_text(text);
  std::cout << report.render() << "\n\n";
  std::cout << "content identity  " << report.content_digest.hex() << "\n";
  std::cout << "document identity " << report.document_digest.hex() << "\n";
  return report.passed() ? 0 : 1;
}
