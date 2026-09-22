// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// ifabricd serves one intent domain over the framed transport. It is a real OS
// process: the process-level tests start it, kill it and restart it to prove
// fresh-incarnation fencing.

#include "ifabric/cli.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return ifabric::run_daemon(args, std::cout, std::cerr);
}
