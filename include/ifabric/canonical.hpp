// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Canonical normalization and content identity.
//
// normalize() is a total function: for any document it produces a stable
// normalized form. Semantically equivalent input -- different member order,
// different collection order, omitted fields that carry their default value,
// duplicated entries inside set-valued fields, differently authored but
// compatible schema minors -- normalizes to the same form and the same content
// identity.

#ifndef IFABRIC_CANONICAL_HPP
#define IFABRIC_CANONICAL_HPP

#include "ifabric/model.hpp"

namespace ifabric {

struct CanonicalForm {
  IntentDocument normalized;
  JsonValue document_json;   // full canonical document (schema + provenance + content)
  JsonValue content_json;    // semantic payload only
  DocumentDigest document_digest;
  ContentDigest content_digest;
};

// Stable, total normalization. Never fails; deterministic ordering is imposed
// on every collection, including collections that contain duplicate identities.
IntentDocument normalize(IntentDocument document);

// Normalize, then compute both digests. Fails when the document exceeds the
// declared-object bound.
Result<CanonicalForm> canonicalize(const IntentDocument& document);

// Convenience wrappers used by the store and the diff engine.
ContentDigest content_digest_of(const IntentDocument& document);
DocumentDigest document_digest_of(const IntentDocument& document);

}  // namespace ifabric

#endif  // IFABRIC_CANONICAL_HPP
