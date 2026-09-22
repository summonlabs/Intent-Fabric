// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Strongly typed domain identities. Intent Fabric never passes a fabric, a
// device, a generation, an epoch or an incarnation around as a bare string or
// integer: every one of them is a distinct type whose parser validates the
// class prefix, so a FabricId can never be silently used where a DeviceId is
// required.

#ifndef IFABRIC_TYPES_HPP
#define IFABRIC_TYPES_HPP

#include "ifabric/core.hpp"

#include <compare>

namespace ifabric {

// ---------------------------------------------------------------------------
// Identity classes
// ---------------------------------------------------------------------------
enum class IdClass : std::uint8_t {
  Domain = 0,
  Fabric = 1,
  Site = 2,
  Pod = 3,
  Rack = 4,
  Device = 5,
  Port = 6,
  Link = 7,
  Policy = 8,
  IntentObject = 9,
  Tenant = 10,
  Workload = 11,
  ServiceClass = 12,
  Conflict = 13,
  Actor = 14,
  Proposal = 15,
  Generation = 16,
  Count = 17
};

inline constexpr std::size_t kIdClassCount = static_cast<std::size_t>(IdClass::Count);

std::string_view id_class_prefix(IdClass klass) noexcept;
std::string_view id_class_name(IdClass klass) noexcept;
std::optional<IdClass> id_class_from_prefix(std::string_view prefix) noexcept;
std::optional<IdClass> id_class_from_name(std::string_view name) noexcept;

// ---------------------------------------------------------------------------
// BasicId<K> - a validated, class-tagged identity.
//
// Text form is "<prefix>.<local>", e.g. "dev.leaf-a1". The local part must be
// a valid identity name: 1..96 characters of [A-Za-z0-9._-], starting with an
// alphanumeric character.
// ---------------------------------------------------------------------------
template <IdClass K>
class BasicId {
 public:
  static constexpr IdClass id_class = K;

  BasicId() = default;

  static Result<BasicId> from_local(std::string_view local) {
    if (!is_valid_identity_name(local)) {
      return Error(ErrorCode::IdentityMalformed,
                   std::string("invalid ") + std::string(id_class_name(K)) + " identity name",
                   std::string(local));
    }
    return BasicId(std::string(local));
  }

  static Result<BasicId> parse(std::string_view text) {
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos) {
      return Error(ErrorCode::IdentityMalformed, "identity is missing its class prefix",
                   std::string(text));
    }
    const std::string_view prefix = text.substr(0, dot);
    const std::string_view local = text.substr(dot + 1);
    const auto found = id_class_from_prefix(prefix);
    if (!found.has_value()) {
      return Error(ErrorCode::IdentityMalformed, "unknown identity class prefix",
                   std::string(text));
    }
    if (*found != K) {
      return Error(ErrorCode::IdentityClassMismatch,
                   std::string("expected a ") + std::string(id_class_name(K)) + " identity",
                   std::string(text));
    }
    return from_local(local);
  }

  const std::string& local() const noexcept { return local_; }
  bool empty() const noexcept { return local_.empty(); }

  std::string str() const {
    std::string out(id_class_prefix(K));
    out.push_back('.');
    out.append(local_);
    return out;
  }

  friend bool operator==(const BasicId& a, const BasicId& b) noexcept = default;
  friend std::strong_ordering operator<=>(const BasicId& a, const BasicId& b) noexcept = default;

 private:
  explicit BasicId(std::string local) : local_(std::move(local)) {}
  std::string local_;
};

using DomainId = BasicId<IdClass::Domain>;
using FabricId = BasicId<IdClass::Fabric>;
using SiteId = BasicId<IdClass::Site>;
using PodId = BasicId<IdClass::Pod>;
using RackId = BasicId<IdClass::Rack>;
using DeviceId = BasicId<IdClass::Device>;
using PortId = BasicId<IdClass::Port>;
using LinkId = BasicId<IdClass::Link>;
using PolicyId = BasicId<IdClass::Policy>;
using IntentObjectId = BasicId<IdClass::IntentObject>;
using TenantId = BasicId<IdClass::Tenant>;
using WorkloadId = BasicId<IdClass::Workload>;
using ServiceClassId = BasicId<IdClass::ServiceClass>;
using ConflictId = BasicId<IdClass::Conflict>;
using ActorId = BasicId<IdClass::Actor>;
using ProposalId = BasicId<IdClass::Proposal>;
using GenerationId = BasicId<IdClass::Generation>;

// ---------------------------------------------------------------------------
// AnyId - class-tagged identity used where the class is only known at runtime
// (references, conflict participants, diff subjects). Ordering is by class and
// then by local part, which makes every collection keyed by AnyId canonical by
// construction.
// ---------------------------------------------------------------------------
class AnyId {
 public:
  AnyId() = default;
  AnyId(IdClass klass, std::string local) : klass_(klass), local_(std::move(local)) {}

  template <IdClass K>
  explicit AnyId(const BasicId<K>& id) : klass_(K), local_(id.local()) {}  // NOLINT

  static Result<AnyId> parse(std::string_view text);

  IdClass klass() const noexcept { return klass_; }
  const std::string& local() const noexcept { return local_; }
  bool empty() const noexcept { return local_.empty(); }

  std::string str() const {
    std::string out(id_class_prefix(klass_));
    out.push_back('.');
    out.append(local_);
    return out;
  }

  template <IdClass K>
  Result<BasicId<K>> as() const {
    if (klass_ != K) {
      return Error(ErrorCode::IdentityClassMismatch,
                   std::string("identity is a ") + std::string(id_class_name(klass_)) +
                       ", not a " + std::string(id_class_name(K)),
                   str());
    }
    return BasicId<K>::parse(str());
  }

  friend bool operator==(const AnyId& a, const AnyId& b) noexcept {
    return a.klass_ == b.klass_ && a.local_ == b.local_;
  }
  friend bool operator!=(const AnyId& a, const AnyId& b) noexcept { return !(a == b); }
  friend bool operator<(const AnyId& a, const AnyId& b) noexcept {
    if (a.klass_ != b.klass_) {
      return static_cast<std::uint8_t>(a.klass_) < static_cast<std::uint8_t>(b.klass_);
    }
    return a.local_ < b.local_;
  }

 private:
  IdClass klass_ = IdClass::Domain;
  std::string local_;
};

// ---------------------------------------------------------------------------
// Generation numbers, epochs, incarnations.
//
// A generation number is never zero once committed: generation 0 means "no
// committed generation". Epochs are bumped every time a runtime instance takes
// authority over a store, so a writer that survived a restart can be fenced.
// ---------------------------------------------------------------------------
class GenerationNumber {
 public:
  static constexpr std::uint64_t kMax = 0x00FFFFFFFFFFFFFEull;

  GenerationNumber() = default;

  static Result<GenerationNumber> from(std::uint64_t value) {
    if (value > kMax) {
      return Error(ErrorCode::InvalidArgument, "generation number out of range",
                   std::to_string(value));
    }
    return GenerationNumber(value);
  }
  static GenerationNumber genesis() noexcept { return GenerationNumber(0); }

  bool is_genesis() const noexcept { return value_ == 0; }
  std::uint64_t value() const noexcept { return value_; }

  Result<GenerationNumber> next() const {
    if (value_ >= kMax) {
      return Error(ErrorCode::CheckedArithmeticOverflow, "generation counter exhausted");
    }
    return GenerationNumber(value_ + 1);
  }

  std::string str() const { return std::to_string(value_); }
  std::string padded() const;

  friend bool operator==(const GenerationNumber& a, const GenerationNumber& b) noexcept = default;
  friend std::strong_ordering operator<=>(const GenerationNumber& a,
                                          const GenerationNumber& b) noexcept = default;

 private:
  explicit GenerationNumber(std::uint64_t value) : value_(value) {}
  std::uint64_t value_ = 0;
};

class Epoch {
 public:
  static constexpr std::uint64_t kMax = 0xFFFFFFFFFFFFFEull;

  Epoch() = default;
  explicit Epoch(std::uint64_t value) : value_(value) {}

  static Result<Epoch> from(std::uint64_t value) {
    if (value > kMax) {
      return Error(ErrorCode::InvalidArgument, "epoch out of range", std::to_string(value));
    }
    return Epoch(value);
  }

  std::uint64_t value() const noexcept { return value_; }
  bool is_zero() const noexcept { return value_ == 0; }

  Result<Epoch> next() const {
    if (value_ >= kMax) {
      return Error(ErrorCode::CheckedArithmeticOverflow, "epoch counter exhausted");
    }
    return Epoch(value_ + 1);
  }

  std::string str() const { return std::to_string(value_); }

  friend bool operator==(const Epoch& a, const Epoch& b) noexcept = default;
  friend std::strong_ordering operator<=>(const Epoch& a, const Epoch& b) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

class IncarnationId {
 public:
  IncarnationId() = default;

  static IncarnationId generate() { return IncarnationId(random_hex_128()); }

  static Result<IncarnationId> parse(std::string_view text) {
    if (text.size() != 32) {
      return Error(ErrorCode::IdentityMalformed, "incarnation id must be 32 hex characters",
                   std::string(text));
    }
    for (char c : text) {
      const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      if (!hex) {
        return Error(ErrorCode::IdentityMalformed, "incarnation id must be lower-case hex",
                     std::string(text));
      }
    }
    return IncarnationId(std::string(text));
  }

  const std::string& str() const noexcept { return text_; }
  bool empty() const noexcept { return text_.empty(); }

  friend bool operator==(const IncarnationId& a, const IncarnationId& b) noexcept = default;
  friend std::strong_ordering operator<=>(const IncarnationId& a,
                                          const IncarnationId& b) noexcept = default;

 private:
  explicit IncarnationId(std::string text) : text_(std::move(text)) {}
  std::string text_;
};

// ---------------------------------------------------------------------------
// Strong digests. Content identity, document identity, lineage identity and
// record payload identity are distinct types so they can never be swapped by
// accident.
// ---------------------------------------------------------------------------
struct ContentDigestTag {};
struct DocumentDigestTag {};
struct LineageDigestTag {};
struct HeadDigestTag {};
struct PayloadDigestTag {};

template <class Tag>
class StrongDigest {
 public:
  StrongDigest() = default;
  explicit StrongDigest(const Digest& digest) : digest_(digest) {}

  static StrongDigest of(std::string_view bytes) { return StrongDigest(Sha256::hash(bytes)); }
  static Result<StrongDigest> from_bytes(const std::uint8_t* data, std::size_t size) {
    const auto digest = Digest::from_bytes(data, size);
    if (!digest) {
      return digest.error();
    }
    return StrongDigest(digest.value());
  }
  static Result<StrongDigest> from_hex(std::string_view hex) {
    const auto parsed = Digest::from_hex(hex);
    if (!parsed) {
      return parsed.error();
    }
    return StrongDigest(*parsed);
  }

  const Digest& digest() const noexcept { return digest_; }
  std::string hex() const { return digest_.hex(); }
  std::string short_hex() const { return digest_.short_hex(); }
  bool is_zero() const noexcept { return digest_.is_zero(); }

  friend bool operator==(const StrongDigest& a, const StrongDigest& b) noexcept {
    return a.digest_ == b.digest_;
  }
  friend bool operator!=(const StrongDigest& a, const StrongDigest& b) noexcept {
    return !(a == b);
  }
  friend bool operator<(const StrongDigest& a, const StrongDigest& b) noexcept {
    return a.digest_ < b.digest_;
  }

 private:
  Digest digest_;
};

using ContentDigest = StrongDigest<ContentDigestTag>;
using DocumentDigest = StrongDigest<DocumentDigestTag>;
using LineageDigest = StrongDigest<LineageDigestTag>;
using HeadDigest = StrongDigest<HeadDigestTag>;
using PayloadDigest = StrongDigest<PayloadDigestTag>;

// ---------------------------------------------------------------------------
// Generation reference - the generation-bound handle handed to downstream
// runtimes. A consumer that holds a GenerationRef can tell whether it is stale
// (older than head) or superseded (same number, different content).
// ---------------------------------------------------------------------------
struct GenerationRef {
  GenerationNumber number;
  ContentDigest content;
  Epoch committed_epoch;
  IncarnationId committed_incarnation;

  bool valid() const noexcept { return !number.is_genesis() && !content.is_zero(); }

  friend bool operator==(const GenerationRef& a, const GenerationRef& b) noexcept {
    return a.number == b.number && a.content == b.content &&
           a.committed_epoch == b.committed_epoch &&
           a.committed_incarnation == b.committed_incarnation;
  }
};

// ---------------------------------------------------------------------------
// Fence token - everything a writer must prove before it may mutate
// authoritative state.
// ---------------------------------------------------------------------------
struct FenceToken {
  GenerationNumber expected_generation;  // head the writer believes is current
  Epoch expected_epoch;
  IncarnationId expected_incarnation;
  ActorId writer;

  bool has_expected_generation() const noexcept { return true; }

  std::string render() const {
    std::string out = "gen=";
    out.append(expected_generation.str());
    out.append(" epoch=");
    out.append(expected_epoch.str());
    out.append(" incarnation=");
    out.append(expected_incarnation.empty() ? std::string("<none>") : expected_incarnation.str());
    out.append(" writer=");
    out.append(writer.empty() ? std::string("<none>") : writer.str());
    return out;
  }
};

// ---------------------------------------------------------------------------
// Source revision - provenance reference supplied by an upstream system.
// ---------------------------------------------------------------------------
class SourceRevision {
 public:
  SourceRevision() = default;

  static Result<SourceRevision> parse(std::string_view text) {
    const std::string_view trimmed = trim_ascii(text);
    if (trimmed.empty()) {
      return Error(ErrorCode::InvalidArgument, "source revision must not be empty");
    }
    if (trimmed.size() > 256u) {
      return Error(ErrorCode::LimitExceeded, "source revision exceeds 256 characters");
    }
    if (!is_valid_utf8(trimmed)) {
      return Error(ErrorCode::JsonEncodingError, "source revision is not valid UTF-8");
    }
    return SourceRevision(std::string(trimmed));
  }

  const std::string& str() const noexcept { return text_; }
  bool empty() const noexcept { return text_.empty(); }

  friend bool operator==(const SourceRevision& a, const SourceRevision& b) noexcept = default;
  friend std::strong_ordering operator<=>(const SourceRevision& a,
                                          const SourceRevision& b) noexcept = default;

 private:
  explicit SourceRevision(std::string text) : text_(std::move(text)) {}
  std::string text_;
};

}  // namespace ifabric

#endif  // IFABRIC_TYPES_HPP
