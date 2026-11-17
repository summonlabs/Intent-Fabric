// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/canonical.hpp"
#include "ifabric/index.hpp"
#include "ifabric/synth.hpp"
#include "ifabric_test.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

using namespace ifabric;

namespace {

// CPU time used so far by the calling thread, in milliseconds. Unlike elapsed
// time it does not grow while the machine is busy with other work.
double thread_cpu_ms() {
#if defined(_WIN32)
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (::GetThreadTimes(::GetCurrentThread(), &created, &exited, &kernel, &user) == 0) {
    return 0.0;
  }
  const auto ticks = [](const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) | value.dwLowDateTime;
  };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#else
  timespec now{};
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) * 1000.0 + static_cast<double>(now.tv_nsec) / 1.0e6;
#endif
}

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

// Deterministic permutation of every collection in the document, used to prove
// that input order does not influence the normalized form.
void permute(IntentDocument& document, std::uint64_t seed) {
  DeterministicRng rng(seed);
  const auto shuffle = [&rng](auto& items) {
    for (std::size_t i = items.size(); i > 1; --i) {
      const std::size_t j = rng.next_bounded(i);
      std::swap(items[i - 1], items[j]);
    }
  };
  shuffle(document.fabrics);
  shuffle(document.sites);
  shuffle(document.pods);
  shuffle(document.racks);
  shuffle(document.devices);
  shuffle(document.ports);
  shuffle(document.links);
  shuffle(document.capability_catalog);
  shuffle(document.service_classes);
  shuffle(document.policies);
  shuffle(document.tenants);
  shuffle(document.workloads);
  shuffle(document.intents);
  shuffle(document.required_features);
  shuffle(document.optional_features);
  for (auto& device : document.devices) {
    shuffle(device.service_classes);
  }
  for (auto& port : document.ports) {
    shuffle(port.service_classes);
  }
  for (auto& link : document.links) {
    shuffle(link.service_classes);
  }
  for (auto& entry : document.capability_catalog) {
    shuffle(entry.capabilities);
    shuffle(entry.supported_roles);
  }
  for (auto& entry : document.service_classes) {
    shuffle(entry.required_capabilities);
  }
}

}  // namespace

IFABRIC_TEST(canonical, normalization_is_idempotent) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const IntentDocument once = normalize(document);
  const IntentDocument twice = normalize(once);
  CHECK_EQ(to_json(once).dump(), to_json(twice).dump());
  const IntentDocument thrice = normalize(twice);
  CHECK_EQ(to_json(once).dump(), to_json(thrice).dump());
}

IFABRIC_TEST(canonical, normalization_is_order_independent) {
  const IntentDocument reference = synthesize(core_domain(), SynthOptions{});
  const auto base = canonicalize(reference);
  REQUIRE(base.ok());
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    IntentDocument shuffled = reference;
    permute(shuffled, seed);
    const auto form = canonicalize(shuffled);
    REQUIRE(form.ok());
    CHECK_EQ(form->document_digest.hex(), base->document_digest.hex());
    CHECK_EQ(form->content_digest.hex(), base->content_digest.hex());
    CHECK_EQ(form->document_json.dump(), base->document_json.dump());
  }
}

IFABRIC_TEST(canonical, set_valued_fields_collapse_duplicates) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  REQUIRE(!document.devices.empty());
  const ServiceClassId spare = document.service_classes.front().id;
  document.devices.front().service_classes.push_back(spare);
  document.devices.front().service_classes.push_back(spare);
  document.required_features.push_back("topology.multi-site");
  const IntentDocument normalized = normalize(document);
  const auto occurrences = std::count(normalized.devices.front().service_classes.begin(),
                                      normalized.devices.front().service_classes.end(), spare);
  CHECK_EQ(occurrences, std::ptrdiff_t{1});
  const auto feature_occurrences =
      std::count(normalized.required_features.begin(), normalized.required_features.end(),
                 std::string("topology.multi-site"));
  CHECK_EQ(feature_occurrences, std::ptrdiff_t{1});
}

IFABRIC_TEST(canonical, duplicate_identities_still_normalize_stably) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const DeviceDecl copy = document.devices.front();
  DeviceDecl variant = copy;
  variant.description = "a different declaration for the same identity";
  document.devices.push_back(copy);
  document.devices.push_back(variant);
  const IntentDocument left = normalize(document);
  IntentDocument reordered = document;
  std::reverse(reordered.devices.begin(), reordered.devices.end());
  const IntentDocument right = normalize(reordered);
  CHECK_EQ(to_json(left).dump(), to_json(right).dump());
  const TopologyIndex index(left);
  CHECK_EQ(index.duplicates().size(), std::size_t{1});
  CHECK_EQ(index.duplicates().front().count, std::size_t{3});
}

IFABRIC_TEST(canonical, whitespace_is_trimmed_but_meaning_is_not_altered) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  document.fabrics.front().display_name = "  Core Fabric \t";
  document.provenance.description = "\n  padded  \n";
  const IntentDocument normalized = normalize(document);
  CHECK_EQ(normalized.fabrics.front().display_name, std::string("Core Fabric"));
  CHECK_EQ(normalized.provenance.description, std::string("padded"));
}

IFABRIC_TEST(canonical, content_identity_ignores_provenance_and_schema_minor) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const auto reference = canonicalize(document);
  REQUIRE(reference.ok());
  IntentDocument other = document;
  other.provenance.actor = ActorId::parse("actor.someone-else").value();
  other.provenance.description = "same desired state, different narrator";
  other.schema = SchemaVersion::make(1, 2).value();
  const auto form = canonicalize(other);
  REQUIRE(form.ok());
  CHECK_EQ(form->content_digest.hex(), reference->content_digest.hex());
  CHECK_NE(form->document_digest.hex(), reference->document_digest.hex());
}

IFABRIC_TEST(canonical, declared_object_bound_is_enforced) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  // Duplicate one device far beyond the configured bound without allocating a
  // real document: the bound check reads the declared counts.
  DeviceDecl filler = document.devices.front();
  document.devices.resize(kMaxObjectsPerDocument + 1, filler);
  const auto form = canonicalize(document);
  CHECK_ERROR_CODE(form.error(), ErrorCode::LimitExceeded);
}

IFABRIC_TEST(canonical, declared_object_bound_is_checked_before_normalizing) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  // Every filler device has the same id, so normalizing them would order the
  // ties by serializing devices: millions of serializations for this document.
  DeviceDecl filler = document.devices.front();
  document.devices.resize(kMaxObjectsPerDocument + 1, filler);
  const double before = thread_cpu_ms();
  const auto form = canonicalize(document);
  const double used = thread_cpu_ms() - before;
  CHECK_ERROR_CODE(form.error(), ErrorCode::LimitExceeded);
  // The bound refuses the document from its declared counts, before any of the
  // work it bounds. Refused only after normalization, this document cost about
  // 10 s of CPU in Release and minutes in Debug.
  CHECK(used < 1000.0);
}

IFABRIC_TEST(canonical, digest_helpers_agree_with_canonicalize) {
  const IntentDocument document = synthesize(core_domain(), SynthOptions{});
  const auto form = canonicalize(document);
  REQUIRE(form.ok());
  CHECK_EQ(content_digest_of(document).hex(), form->content_digest.hex());
  CHECK_EQ(document_digest_of(document).hex(), form->document_digest.hex());
}

IFABRIC_TEST(canonical, empty_document_normalizes_and_digests) {
  IntentDocument document;
  document.domain = core_domain();
  const auto form = canonicalize(document);
  REQUIRE(form.ok());
  CHECK_EQ(form->normalized.declared_object_count(), std::size_t{0});
  CHECK(!form->content_digest.is_zero());
  const IntentDocument again = normalize(form->normalized);
  CHECK_EQ(to_json(again).dump(), form->document_json.dump());
}
