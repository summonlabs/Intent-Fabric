// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/core.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <ctime>
#include <mutex>
#include <random>
#include <stdexcept>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ifabric {

namespace detail {
void result_missing_value(const char* type_name, const Error& error) {
  throw std::logic_error(std::string("ifabric::Result<") +
                         (type_name == nullptr ? "?" : type_name) +
                         "> accessed without a value; carried error: " + error.render());
}

void require_failed(ErrorCode code, std::string message) {
  throw std::logic_error(std::string("ifabric::JsonValue accessor failed: ") +
                         std::string(to_string(code)) + ": " + message);
}
}  // namespace detail

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------
std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::LimitExceeded: return "LimitExceeded";
    case ErrorCode::CheckedArithmeticOverflow: return "CheckedArithmeticOverflow";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::Internal: return "Internal";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::IdentityMalformed: return "IdentityMalformed";
    case ErrorCode::IdentityClassMismatch: return "IdentityClassMismatch";
    case ErrorCode::DuplicateIdentity: return "DuplicateIdentity";
    case ErrorCode::JsonParseError: return "JsonParseError";
    case ErrorCode::JsonDepthExceeded: return "JsonDepthExceeded";
    case ErrorCode::JsonSizeExceeded: return "JsonSizeExceeded";
    case ErrorCode::JsonTrailingContent: return "JsonTrailingContent";
    case ErrorCode::JsonDuplicateKey: return "JsonDuplicateKey";
    case ErrorCode::JsonTypeMismatch: return "JsonTypeMismatch";
    case ErrorCode::JsonNumberOutOfRange: return "JsonNumberOutOfRange";
    case ErrorCode::JsonEncodingError: return "JsonEncodingError";
    case ErrorCode::SchemaVersionMalformed: return "SchemaVersionMalformed";
    case ErrorCode::UnsupportedSchemaMajor: return "UnsupportedSchemaMajor";
    case ErrorCode::UnsupportedSchemaMinor: return "UnsupportedSchemaMinor";
    case ErrorCode::FeatureNotAvailable: return "FeatureNotAvailable";
    case ErrorCode::UnknownField: return "UnknownField";
    case ErrorCode::MissingField: return "MissingField";
    case ErrorCode::AmbiguousSemantics: return "AmbiguousSemantics";
    case ErrorCode::DanglingReference: return "DanglingReference";
    case ErrorCode::CyclicReference: return "CyclicReference";
    case ErrorCode::UnknownCapability: return "UnknownCapability";
    case ErrorCode::InfeasibleTopology: return "InfeasibleTopology";
    case ErrorCode::PolicyConflict: return "PolicyConflict";
    case ErrorCode::InvariantViolation: return "InvariantViolation";
    case ErrorCode::CapacityExceeded: return "CapacityExceeded";
    case ErrorCode::StoreNotFound: return "StoreNotFound";
    case ErrorCode::StoreFormatUnsupported: return "StoreFormatUnsupported";
    case ErrorCode::StoreCorrupt: return "StoreCorrupt";
    case ErrorCode::StoreIntegrityMismatch: return "StoreIntegrityMismatch";
    case ErrorCode::StoreHeadMissing: return "StoreHeadMissing";
    case ErrorCode::StoreLockBusy: return "StoreLockBusy";
    case ErrorCode::StoreBusy: return "StoreBusy";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::AtomicReplaceFailed: return "AtomicReplaceFailed";
    case ErrorCode::StaleGeneration: return "StaleGeneration";
    case ErrorCode::StaleWriter: return "StaleWriter";
    case ErrorCode::StaleEpoch: return "StaleEpoch";
    case ErrorCode::GenerationNotFound: return "GenerationNotFound";
    case ErrorCode::GenerationPruned: return "GenerationPruned";
    case ErrorCode::GenerationNotCommitted: return "GenerationNotCommitted";
    case ErrorCode::NoCommittedGeneration: return "NoCommittedGeneration";
    case ErrorCode::UnknownDomain: return "UnknownDomain";
    case ErrorCode::ProposalNotFound: return "ProposalNotFound";
    case ErrorCode::ProposalNotValidated: return "ProposalNotValidated";
    case ErrorCode::ProposalRejected: return "ProposalRejected";
    case ErrorCode::EmptyDiffRejected: return "EmptyDiffRejected";
    case ErrorCode::ProposalLimitExceeded: return "ProposalLimitExceeded";
    case ErrorCode::NetworkError: return "NetworkError";
    case ErrorCode::ProtocolError: return "ProtocolError";
    case ErrorCode::FrameTooLarge: return "FrameTooLarge";
    case ErrorCode::ServerBusy: return "ServerBusy";
    case ErrorCode::Unauthorized: return "Unauthorized";
  }
  return "UnknownErrorCode";
}

std::string Error::render() const {
  std::string out(to_string(code));
  out.append(": ");
  out.append(message);
  if (!detail.empty()) {
    out.append(" [");
    out.append(detail);
    out.append("]");
  }
  return out;
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------
namespace {

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::array<std::uint32_t, 8> kSha256Initial = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                                          0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                                          0x1f83d9abu, 0x5be0cd19u};

inline std::uint32_t rotr(std::uint32_t value, unsigned shift) noexcept {
  return (value >> shift) | (value << (32u - shift));
}

}  // namespace

Sha256::Sha256() noexcept : state_(kSha256Initial) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t size) noexcept {
  if (data == nullptr || size == 0) {
    return;
  }
  bit_length_ += static_cast<std::uint64_t>(size) * 8u;
  std::size_t offset = 0;
  if (buffered_ > 0) {
    const std::size_t need = 64 - buffered_;
    const std::size_t take = size < need ? size : need;
    std::memcpy(buffer_.data() + buffered_, data, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (offset + 64 <= size) {
    compress(data + offset);
    offset += 64;
  }
  if (offset < size) {
    const std::size_t remaining = size - offset;
    std::memcpy(buffer_.data(), data + offset, remaining);
    buffered_ = remaining;
  }
}

void Sha256::update(std::string_view data) noexcept {
  update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

Digest Sha256::finish() noexcept {
  const std::uint64_t total_bits = bit_length_;
  std::uint8_t tail[72];
  std::size_t tail_len = 0;
  tail[tail_len++] = 0x80u;
  // Pad with zeros until the total length is congruent to 56 mod 64, then the
  // 64-bit big-endian bit count. Everything is fed through the streaming
  // update so that the running state is never discarded.
  const std::size_t consumed = buffered_ + 1;
  const std::size_t zeros = (consumed % 64 <= 56) ? (56 - (consumed % 64)) : (120 - (consumed % 64));
  for (std::size_t i = 0; i < zeros; ++i) {
    tail[tail_len++] = 0x00u;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    tail[tail_len++] = static_cast<std::uint8_t>((total_bits >> ((7 - i) * 8)) & 0xFFu);
  }
  update(tail, tail_len);

  Digest digest;
  std::array<std::uint8_t, Digest::kBytes> out{};
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFu);
    out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFu);
    out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFu);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
  }
  return Digest::from_bytes(out.data(), out.size()).value();
}

Digest Sha256::hash(std::string_view data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

Result<Digest> Digest::from_bytes(const std::uint8_t* data, std::size_t size) noexcept {
  if (data == nullptr || size != kBytes) {
    return Error(ErrorCode::InvalidArgument, "digest requires exactly 32 bytes");
  }
  Digest digest;
  std::memcpy(digest.bytes_.data(), data, kBytes);
  return digest;
}

Result<Digest> Digest::from_hex(std::string_view hex) {
  const auto raw = ::ifabric::from_hex(hex);
  if (!raw) {
    return raw.error();
  }
  if (raw->size() != kBytes) {
    return Error(ErrorCode::InvalidArgument, "digest hex must encode exactly 32 bytes");
  }
  Digest digest;
  std::memcpy(digest.bytes_.data(), raw->data(), kBytes);
  return digest;
}

bool Digest::is_zero() const noexcept {
  for (std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::string Digest::hex() const { return to_hex(bytes_.data(), bytes_.size()); }

std::string Digest::short_hex() const { return to_hex(bytes_.data(), 8); }

std::uint32_t crc32(std::string_view bytes) noexcept {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> out{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
      }
      out[i] = crc;
    }
    return out;
  }();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (char ch : bytes) {
    const auto index = static_cast<std::size_t>((crc ^ static_cast<std::uint32_t>(
                                                          static_cast<std::uint8_t>(ch))) &
                                                0xFFu);
    crc = table[index] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------
Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) {
  if (a > (std::numeric_limits<std::uint64_t>::max)() - b) {
    return Error(ErrorCode::CheckedArithmeticOverflow, "unsigned addition overflow");
  }
  return a + b;
}

Result<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) {
  if (a != 0 && b > (std::numeric_limits<std::uint64_t>::max)() / a) {
    return Error(ErrorCode::CheckedArithmeticOverflow, "unsigned multiplication overflow");
  }
  return a * b;
}

Result<std::size_t> checked_size_add(std::size_t a, std::size_t b) {
  if (a > (std::numeric_limits<std::size_t>::max)() - b) {
    return Error(ErrorCode::CheckedArithmeticOverflow, "size addition overflow");
  }
  return a + b;
}

Result<std::size_t> checked_size_mul(std::size_t a, std::size_t b) {
  if (a != 0 && b > (std::numeric_limits<std::size_t>::max)() / a) {
    return Error(ErrorCode::CheckedArithmeticOverflow, "size multiplication overflow");
  }
  return a * b;
}

Result<std::uint32_t> narrow_u32(std::uint64_t value) {
  if (value > 0xFFFFFFFFull) {
    return Error(ErrorCode::CheckedArithmeticOverflow, "value does not fit in 32 bits");
  }
  return static_cast<std::uint32_t>(value);
}

Result<std::int64_t> to_i64(std::uint64_t value) {
  if (value > 0x7FFFFFFFFFFFFFFFull) {
    return Error(ErrorCode::CheckedArithmeticOverflow,
                 "value does not fit in a signed 64-bit integer");
  }
  return static_cast<std::int64_t>(value);
}

std::size_t saturating_add(std::size_t a, std::size_t b) noexcept {
  const std::size_t max = (std::numeric_limits<std::size_t>::max)();
  if (a > max - b) {
    return max;
  }
  return a + b;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
std::string format_timestamp(Timestamp ts) {
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(ts.time_since_epoch());
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(millis);
  auto remainder = millis - seconds;
  if (remainder.count() < 0) {
    remainder += std::chrono::seconds(1);
    seconds -= std::chrono::seconds(1);
  }
  const auto raw = static_cast<std::time_t>(seconds.count());
  std::tm utc{};
#if defined(_WIN32)
  if (::gmtime_s(&utc, &raw) != 0) {
    return "1970-01-01T00:00:00.000Z";
  }
#else
  if (::gmtime_r(&raw, &utc) == nullptr) {
    return "1970-01-01T00:00:00.000Z";
  }
#endif
  char buffer[40];
  const int written =
      std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                    utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                    utc.tm_sec, static_cast<int>(remainder.count()));
  if (written <= 0) {
    return "1970-01-01T00:00:00.000Z";
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

Result<Timestamp> parse_timestamp(std::string_view text) {
  if (text.size() < 20) {
    return Error(ErrorCode::InvalidArgument, "timestamp must be RFC3339 UTC with milliseconds",
                 std::string(text));
  }
  const auto digit = [&](std::size_t index) -> Result<int> {
    if (index >= text.size()) {
      return Error(ErrorCode::InvalidArgument, "truncated timestamp");
    }
    const char c = text[index];
    if (c < '0' || c > '9') {
      return Error(ErrorCode::InvalidArgument, "timestamp contains a non-digit", std::string(text));
    }
    return c - '0';
  };
  const auto number = [&](std::size_t index, std::size_t count) -> Result<int> {
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const auto d = digit(index + i);
      if (!d) {
        return d.error();
      }
      value = value * 10 + d.value();
    }
    return value;
  };

  const auto year = number(0, 4);
  const auto month = number(5, 2);
  const auto day = number(8, 2);
  const auto hour = number(11, 2);
  const auto minute = number(14, 2);
  const auto second = number(17, 2);
  const Result<int>* parts[] = {&year, &month, &day, &hour, &minute, &second};
  for (const Result<int>* part : parts) {
    if (!part->ok()) {
      return part->error();
    }
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
    return Error(ErrorCode::InvalidArgument, "timestamp is not in RFC3339 UTC form",
                 std::string(text));
  }
  int millis = 0;
  std::size_t cursor = 19;
  if (text.size() > 19 && text[19] == '.') {
    const auto frac = number(20, 3);
    if (!frac) {
      return frac.error();
    }
    millis = frac.value();
    cursor = 23;
  }
  if (cursor >= text.size() || text[cursor] != 'Z') {
    return Error(ErrorCode::InvalidArgument, "timestamp must end with Z (UTC)", std::string(text));
  }
  if (month.value() < 1 || month.value() > 12 || day.value() < 1 || day.value() > 31 ||
      hour.value() > 23 || minute.value() > 59 || second.value() > 60) {
    return Error(ErrorCode::InvalidArgument, "timestamp field out of range", std::string(text));
  }
  std::tm utc{};
  utc.tm_year = year.value() - 1900;
  utc.tm_mon = month.value() - 1;
  utc.tm_mday = day.value();
  utc.tm_hour = hour.value();
  utc.tm_min = minute.value();
  utc.tm_sec = second.value();
#if defined(_WIN32)
  const std::time_t raw = _mkgmtime64(&utc);
#else
  const std::time_t raw = ::timegm(&utc);
#endif
  if (raw == static_cast<std::time_t>(-1)) {
    return Error(ErrorCode::InvalidArgument, "timestamp is not representable", std::string(text));
  }
  return Timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::seconds(raw)) +
                   std::chrono::milliseconds(millis));
}

Timestamp SystemClock::now() const {
  return std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------
std::string to_lower_ascii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out[i * 2] = kDigits[(data[i] >> 4) & 0x0Fu];
    out[i * 2 + 1] = kDigits[data[i] & 0x0Fu];
  }
  return out;
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return Error(ErrorCode::InvalidArgument, "hex string must have an even length");
  }
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<std::uint8_t> out(hex.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int high = nibble(hex[i * 2]);
    const int low = nibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return Error(ErrorCode::InvalidArgument, "hex string contains a non-hex character");
    }
    out[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return out;
}

std::string_view trim_ascii(std::string_view text) noexcept {
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  };
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(text[begin])) {
    ++begin;
  }
  while (end > begin && is_space(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t i = 0;
  const std::size_t size = text.size();
  while (i < size) {
    const auto byte = static_cast<std::uint8_t>(text[i]);
    std::size_t extra = 0;
    std::uint32_t codepoint = 0;
    if (byte < 0x80u) {
      i += 1;
      continue;
    }
    if ((byte & 0xE0u) == 0xC0u) {
      extra = 1;
      codepoint = byte & 0x1Fu;
    } else if ((byte & 0xF0u) == 0xE0u) {
      extra = 2;
      codepoint = byte & 0x0Fu;
    } else if ((byte & 0xF8u) == 0xF0u) {
      extra = 3;
      codepoint = byte & 0x07u;
    } else {
      return false;
    }
    if (i + extra >= size) {
      return false;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto cont = static_cast<std::uint8_t>(text[i + k]);
      if ((cont & 0xC0u) != 0x80u) {
        return false;
      }
      codepoint = (codepoint << 6) | (cont & 0x3Fu);
    }
    if (extra == 1 && codepoint < 0x80u) return false;
    if (extra == 2 && codepoint < 0x800u) return false;
    if (extra == 3 && codepoint < 0x10000u) return false;
    if (codepoint > 0x10FFFFu) return false;
    if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) return false;
    i += extra + 1;
  }
  return true;
}

bool is_valid_identity_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > kMaxIdentityLength) {
    return false;
  }
  const auto is_alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  };
  if (!is_alnum(name.front())) {
    return false;
  }
  for (char c : name) {
    if (is_alnum(c) || c == '.' || c == '_' || c == '-') {
      continue;
    }
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Randomness
// ---------------------------------------------------------------------------
std::uint64_t random_u64() {
  static std::mutex mutex;
  static std::mt19937_64 engine = [] {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(),
                       device()};
    return std::mt19937_64(seed);
  }();
  const std::lock_guard<std::mutex> guard(mutex);
  return engine();
}

std::string random_hex_128() {
  const std::uint64_t high = random_u64();
  const std::uint64_t low = random_u64();
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[i] = static_cast<std::uint8_t>((high >> (i * 8)) & 0xFFu);
    bytes[i + 8] = static_cast<std::uint8_t>((low >> (i * 8)) & 0xFFu);
  }
  return to_hex(bytes.data(), bytes.size());
}

std::uint32_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::_getpid());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

std::uint64_t DeterministicRng::next_u64() noexcept {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

std::size_t DeterministicRng::next_bounded(std::size_t bound) noexcept {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::size_t>(next_u64() % static_cast<std::uint64_t>(bound));
}

std::int64_t DeterministicRng::next_range(std::int64_t lo, std::int64_t hi) noexcept {
  if (hi <= lo) {
    return lo;
  }
  const auto span = static_cast<std::uint64_t>(hi - lo) + 1ull;
  return lo + static_cast<std::int64_t>(next_u64() % span);
}

}  // namespace ifabric
