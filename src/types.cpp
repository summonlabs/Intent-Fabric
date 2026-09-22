// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/types.hpp"

#include <cstdio>

namespace ifabric {

namespace {

struct IdClassRow {
  std::string_view prefix;
  std::string_view name;
};

constexpr std::array<IdClassRow, kIdClassCount> kIdClasses = {{
    {"dom", "domain"},
    {"fab", "fabric"},
    {"site", "site"},
    {"pod", "pod"},
    {"rack", "rack"},
    {"dev", "device"},
    {"port", "port"},
    {"link", "link"},
    {"pol", "policy"},
    {"int", "intent"},
    {"ten", "tenant"},
    {"wl", "workload"},
    {"svc", "service-class"},
    {"cfl", "conflict"},
    {"actor", "actor"},
    {"prop", "proposal"},
    {"gen", "generation"},
}};

}  // namespace

std::string_view id_class_prefix(IdClass klass) noexcept {
  const auto index = static_cast<std::size_t>(klass);
  if (index >= kIdClassCount) {
    return "?";
  }
  return kIdClasses[index].prefix;
}

std::string_view id_class_name(IdClass klass) noexcept {
  const auto index = static_cast<std::size_t>(klass);
  if (index >= kIdClassCount) {
    return "unknown";
  }
  return kIdClasses[index].name;
}

std::optional<IdClass> id_class_from_prefix(std::string_view prefix) noexcept {
  for (std::size_t i = 0; i < kIdClassCount; ++i) {
    if (kIdClasses[i].prefix == prefix) {
      return static_cast<IdClass>(i);
    }
  }
  return std::nullopt;
}

std::optional<IdClass> id_class_from_name(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kIdClassCount; ++i) {
    if (kIdClasses[i].name == name) {
      return static_cast<IdClass>(i);
    }
  }
  return std::nullopt;
}

Result<AnyId> AnyId::parse(std::string_view text) {
  const std::size_t dot = text.find('.');
  if (dot == std::string_view::npos) {
    return Error(ErrorCode::IdentityMalformed, "identity is missing its class prefix",
                 std::string(text));
  }
  const std::string_view prefix = text.substr(0, dot);
  const std::string_view local = text.substr(dot + 1);
  const auto klass = id_class_from_prefix(prefix);
  if (!klass.has_value()) {
    return Error(ErrorCode::IdentityMalformed, "unknown identity class prefix", std::string(text));
  }
  if (!is_valid_identity_name(local)) {
    return Error(ErrorCode::IdentityMalformed, "invalid identity name", std::string(text));
  }
  return AnyId(*klass, std::string(local));
}

std::string GenerationNumber::padded() const {
  char buffer[24];
  const int written = std::snprintf(buffer, sizeof(buffer), "%016llx",
                                    static_cast<unsigned long long>(value_));
  if (written <= 0) {
    return std::string("0000000000000000");
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

}  // namespace ifabric
