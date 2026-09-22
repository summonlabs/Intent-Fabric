// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef IFABRIC_CORE_HPP
#define IFABRIC_CORE_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

namespace ifabric {

// ---------------------------------------------------------------------------
// Product identity
// ---------------------------------------------------------------------------
inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";
inline constexpr std::string_view kProductName = "Intent Fabric";
inline constexpr std::string_view kVendorName = "Summon Software Labs";

// ---------------------------------------------------------------------------
// Resource bounds.
//
// Every externally derived size (file length, frame length, declared object
// count, collection length, string length) is checked against these bounds
// before it is used to size a buffer or a container, and all arithmetic on
// those sizes goes through the checked helpers declared here.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxDocumentBytes = 256u * 1024u * 1024u;  // 256 MiB
inline constexpr std::size_t kMaxObjectsPerDocument = 200000u;
inline constexpr std::size_t kMaxJsonDepth = 64u;
inline constexpr std::size_t kMaxJsonNodes = 4000000u;
inline constexpr std::size_t kMaxCollectionMembers = 100000u;
inline constexpr std::size_t kMaxStringLength = 4096u;
inline constexpr std::size_t kMaxDiagnostics = 20000u;
inline constexpr std::size_t kMaxPathHops = 1024u;
inline constexpr std::size_t kMaxFrameBytes = 64u * 1024u * 1024u;  // 64 MiB
inline constexpr std::size_t kMaxRecordBytes = 256u * 1024u * 1024u;  // 256 MiB
inline constexpr std::size_t kMaxGenerationHistory = 100000u;
inline constexpr std::size_t kMaxSubscribers = 4096u;
inline constexpr std::size_t kMaxSubscriberQueue = 1024u;
inline constexpr std::size_t kMaxConnections = 256u;
inline constexpr std::size_t kMaxPendingProposals = 1024u;
inline constexpr std::size_t kMaxIdentityLength = 96u;
inline constexpr std::size_t kMaxMetaEntries = 256u;

// ---------------------------------------------------------------------------
// Error model
// ---------------------------------------------------------------------------
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // Generic argument / limit problems
  InvalidArgument = 1,
  LimitExceeded,
  CheckedArithmeticOverflow,
  Unsupported,
  Internal,
  Cancelled,

  // Identity
  IdentityMalformed,
  IdentityClassMismatch,
  DuplicateIdentity,

  // JSON / serialization
  JsonParseError,
  JsonDepthExceeded,
  JsonSizeExceeded,
  JsonTrailingContent,
  JsonDuplicateKey,
  JsonTypeMismatch,
  JsonNumberOutOfRange,
  JsonEncodingError,

  // Schema / version evolution
  SchemaVersionMalformed,
  UnsupportedSchemaMajor,
  UnsupportedSchemaMinor,
  FeatureNotAvailable,
  UnknownField,
  MissingField,
  AmbiguousSemantics,

  // Model / semantic validation
  DanglingReference,
  CyclicReference,
  UnknownCapability,
  InfeasibleTopology,
  PolicyConflict,
  InvariantViolation,
  CapacityExceeded,

  // Store
  StoreNotFound,
  StoreFormatUnsupported,
  StoreCorrupt,
  StoreIntegrityMismatch,
  StoreHeadMissing,
  StoreLockBusy,
  StoreBusy,
  IoError,
  AtomicReplaceFailed,

  // Generation / authority fencing
  StaleGeneration,
  StaleWriter,
  StaleEpoch,
  GenerationNotFound,
  GenerationPruned,
  GenerationNotCommitted,
  NoCommittedGeneration,
  UnknownDomain,

  // Transactions
  ProposalNotFound,
  ProposalNotValidated,
  ProposalRejected,
  EmptyDiffRejected,
  ProposalLimitExceeded,

  // Transport
  NetworkError,
  ProtocolError,
  FrameTooLarge,
  ServerBusy,
  Unauthorized
};

std::string_view to_string(ErrorCode code) noexcept;

struct Error {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  std::string detail;

  Error() = default;
  explicit Error(ErrorCode c) : code(c), message(std::string(to_string(c))) {}
  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
  Error(ErrorCode c, std::string msg, std::string det)
      : code(c), message(std::move(msg)), detail(std::move(det)) {}

  bool ok() const noexcept { return code == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }

  std::string render() const;
};

inline Error make_error(ErrorCode code, std::string message) {
  return Error(code, std::move(message));
}
inline Error make_error(ErrorCode code, std::string message, std::string detail) {
  return Error(code, std::move(message), std::move(detail));
}

namespace detail {
// Programmer-error escapes, mirroring std::get/std::optional::value. They are
// never used for input-dependent failures, which are always reported as Error.
[[noreturn]] void result_missing_value(const char* type_name, const Error& error);
[[noreturn]] void require_failed(ErrorCode code, std::string message);
}  // namespace detail

// ---------------------------------------------------------------------------
// Result<T> - value-or-error. T must be movable.
// ---------------------------------------------------------------------------
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)), error_() {}        // NOLINT(google-explicit-constructor)
  Result(Error error) : value_(), error_(std::move(error)) {}    // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return ok(); }

  const T& value() const { return require(); }
  T& value() { return require(); }
  T&& release() && { return std::move(require()); }

  const T& operator*() const { return require(); }
  T& operator*() { return require(); }
  const T* operator->() const { return &require(); }
  T* operator->() { return &require(); }

  const Error& error() const noexcept { return error_; }

  template <class U>
  T value_or(U&& fallback) const {
    return ok() ? require() : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  T& require() {
    if (!value_.has_value()) {
      detail::result_missing_value(typeid(T).name(), error_);
    }
    return *value_;
  }
  const T& require() const {
    if (!value_.has_value()) {
      detail::result_missing_value(typeid(T).name(), error_);
    }
    return *value_;
  }

  std::optional<T> value_;
  Error error_;
};

// ---------------------------------------------------------------------------
// SHA-256 and digests
// ---------------------------------------------------------------------------
class Digest {
 public:
  static constexpr std::size_t kBytes = 32;

  Digest() noexcept = default;

  static Result<Digest> from_hex(std::string_view hex);
  static Result<Digest> from_bytes(const std::uint8_t* data, std::size_t size) noexcept;

  bool is_zero() const noexcept;
  const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }

  std::string hex() const;
  std::string short_hex() const;  // first 8 bytes, 16 hex characters

  friend bool operator==(const Digest& a, const Digest& b) noexcept {
    return a.bytes_ == b.bytes_;
  }
  friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
  friend bool operator<(const Digest& a, const Digest& b) noexcept { return a.bytes_ < b.bytes_; }

 private:
  std::array<std::uint8_t, kBytes> bytes_{};
};

class Sha256 {
 public:
  Sha256() noexcept;
  void update(const std::uint8_t* data, std::size_t size) noexcept;
  void update(std::string_view data) noexcept;
  Digest finish() noexcept;

  static Digest hash(std::string_view data) noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t bit_length_ = 0;
  std::size_t buffered_ = 0;
};

std::uint32_t crc32(std::string_view bytes) noexcept;

// ---------------------------------------------------------------------------
// Checked arithmetic over externally derived sizes
// ---------------------------------------------------------------------------
Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b);
Result<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b);
Result<std::size_t> checked_size_add(std::size_t a, std::size_t b);
Result<std::size_t> checked_size_mul(std::size_t a, std::size_t b);
Result<std::uint32_t> narrow_u32(std::uint64_t value);
Result<std::int64_t> to_i64(std::uint64_t value);
std::size_t saturating_add(std::size_t a, std::size_t b) noexcept;

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
using Timestamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;

std::string format_timestamp(Timestamp ts);
Result<Timestamp> parse_timestamp(std::string_view text);

class Clock {
 public:
  virtual ~Clock() = default;
  virtual Timestamp now() const = 0;
};

class SystemClock final : public Clock {
 public:
  Timestamp now() const override;
};

class FixedClock final : public Clock {
 public:
  explicit FixedClock(Timestamp fixed) : fixed_(fixed) {}
  Timestamp now() const override { return fixed_; }
  void set(Timestamp value) noexcept { fixed_ = value; }
  void advance(std::chrono::milliseconds delta) noexcept { fixed_ += delta; }

 private:
  Timestamp fixed_;
};

// ---------------------------------------------------------------------------
// Text / encoding helpers
// ---------------------------------------------------------------------------
bool is_valid_utf8(std::string_view text) noexcept;
std::string to_lower_ascii(std::string_view text);
std::string to_hex(const std::uint8_t* data, std::size_t size);
Result<std::vector<std::uint8_t>> from_hex(std::string_view hex);
std::string_view trim_ascii(std::string_view text) noexcept;
bool is_valid_identity_name(std::string_view name) noexcept;

// ---------------------------------------------------------------------------
// Randomness
// ---------------------------------------------------------------------------
std::string random_hex_128();
std::uint64_t random_u64();
std::uint32_t current_process_id() noexcept;

// Deterministic PRNG used by reproducible synthetic generators and randomized
// property tests (splitmix64).
class DeterministicRng {
 public:
  explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next_u64() noexcept;
  std::uint32_t next_u32() noexcept { return static_cast<std::uint32_t>(next_u64() >> 32); }
  std::size_t next_bounded(std::size_t bound) noexcept;
  std::int64_t next_range(std::int64_t lo, std::int64_t hi) noexcept;
  bool next_bool() noexcept { return (next_u64() >> 63) != 0; }

  template <class T>
  const T& pick(const std::vector<T>& items) {
    return items[next_bounded(items.size())];
  }

  std::uint64_t state() const noexcept { return state_; }

 private:
  std::uint64_t state_;
};

}  // namespace ifabric

#endif  // IFABRIC_CORE_HPP
