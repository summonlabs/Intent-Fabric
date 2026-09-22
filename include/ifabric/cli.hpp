// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Command line surface: validate, normalize, digest, generations, show, diff,
// commit, explain, verify, serve and selftest.

#ifndef IFABRIC_CLI_HPP
#define IFABRIC_CLI_HPP

#include "ifabric/runtime.hpp"

#include <ostream>

namespace ifabric {

// Returns a process exit code: 0 success, 1 operational failure, 2 usage error.
int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

// The ifabricd daemon entry point.
int run_daemon(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

inline constexpr std::string_view kCliUsage =
    "Intent Fabric - authoritative desired-state intent runtime\n"
    "\n"
    "usage: ifabric <command> [options]\n"
    "\n"
    "  validate   <file> [--json]                     layered validation of an intent file\n"
    "  normalize  <file> [--out <file>] [--json]      canonical normalized form\n"
    "  digest     <file> [--json]                     content and document identity\n"
    "  explain    <file> [--json]                     explain a rejected commit\n"
    "  schema                                         schema versions and feature registry\n"
    "  capabilities                                   capability vocabulary\n"
    "  properties                                     intent property registry\n"
    "  init       --store <dir> --domain <dom.x>      create an empty intent store\n"
    "  commit     --store <dir> --file <f> --actor <a> [--expect-generation <n>]\n"
    "  generations --store <dir> [--limit <n>] [--json]\n"
    "  show       --store <dir> [--generation <n>] [--json]\n"
    "  diff       --store <dir> --from <n> --to <n> [--json]\n"
    "  verify     --store <dir> [--json]              full store integrity walk\n"
    "  serve      --store <dir> [--host <h>] [--port <n>]\n"
    "  remote     --host <h> --port <n> <subcommand>  talk to a running ifabricd\n"
    "  version\n";

}  // namespace ifabric

#endif  // IFABRIC_CLI_HPP
