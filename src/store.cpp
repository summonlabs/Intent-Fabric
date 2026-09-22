// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ifabric {

namespace {

constexpr std::array<std::uint8_t, 8> kRecordMagic = {'I', 'F', 'G', 'E', 'N', '1', '\r', '\n'};

void put_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
  out.push_back(static_cast<char>((value >> 16) & 0xFFu));
  out.push_back(static_cast<char>((value >> 24) & 0xFFu));
}

void put_u64(std::string& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((value >> (i * 8)) & 0xFFu));
  }
}

std::uint32_t read_u32(const std::string& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 3])) << 24);
}

std::uint64_t read_u64(const std::string& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(
                 static_cast<std::uint8_t>(bytes[offset + static_cast<std::size_t>(i)]))
             << (i * 8);
  }
  return value;
}

std::string record_file_name(const GenerationNumber& number, const PayloadDigest& payload) {
  return "gen-" + number.padded() + "-" + payload.short_hex() + ".rec";
}

bool is_lower_hex(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) {
      return false;
    }
  }
  return true;
}

// A record file name is exactly gen-<16 hex>-<16 hex>.rec. Anything else in the
// record directory is debris and is never treated as authority.
Result<GenerationNumber> parse_record_file_name(std::string_view name) {
  if (name.size() != 41 || name.compare(0, 4, "gen-") != 0 ||
      name.compare(20, 1, "-") != 0 || name.compare(37, 4, ".rec") != 0) {
    return Error(ErrorCode::InvalidArgument, "not a generation record file name",
                 std::string(name));
  }
  const std::string_view digits = name.substr(4, 16);
  const std::string_view payload = name.substr(21, 16);
  if (!is_lower_hex(digits) || !is_lower_hex(payload)) {
    return Error(ErrorCode::InvalidArgument, "not a generation record file name",
                 std::string(name));
  }
  std::uint64_t value = 0;
  for (char c : digits) {
    value = value * 16u + static_cast<std::uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  }
  return GenerationNumber::from(value);
}

std::string join_problems(const std::vector<std::string>& problems) {
  std::string out;
  for (std::size_t i = 0; i < problems.size(); ++i) {
    if (i != 0) {
      out.append("; ");
    }
    out.append(problems[i]);
  }
  return out;
}

Result<std::uint64_t> parse_decimal_u64(std::string_view text) {
  if (text.empty() || text.size() > 20) {
    return Error(ErrorCode::StoreCorrupt, "malformed numeric field", std::string(text));
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return Error(ErrorCode::StoreCorrupt, "malformed numeric field", std::string(text));
    }
    value = value * 10u + static_cast<std::uint64_t>(c - '0');
  }
  return value;
}

Result<std::string> string_member(const JsonValue& object, const char* name, bool required) {
  const JsonValue* value = object.find(name);
  if (value == nullptr || value->is_null()) {
    if (required) {
      return Error(ErrorCode::StoreCorrupt, std::string("missing member ") + name);
    }
    return std::string();
  }
  const auto* text = value->try_string();
  if (text == nullptr) {
    return Error(ErrorCode::StoreCorrupt, std::string("member ") + name + " must be a string");
  }
  return *text;
}

}  // namespace

JsonValue VerificationReport::to_json() const {
  JsonValue out;
  (void)out.set("domain", JsonValue::string(domain.str()));
  (void)out.set("head_generation", JsonValue::string(head_generation.str()));
  (void)out.set("ok", JsonValue::boolean(ok()));
  (void)out.set("generations_checked",
                JsonValue::integer(static_cast<std::int64_t>(generations_checked)));
  (void)out.set("bytes_checked", JsonValue::integer(static_cast<std::int64_t>(bytes_checked)));
  (void)out.set("chain_links_checked",
                JsonValue::integer(static_cast<std::int64_t>(chain_links_checked)));
  JsonArray items;
  for (const auto& problem : problems) {
    items.push_back(JsonValue::string(problem));
  }
  (void)out.set("problems", JsonValue::array(std::move(items)));
  return out;
}

std::string VerificationReport::render() const {
  std::string out = ok() ? "store OK" : "store INTEGRITY FAILURE";
  out.append(" domain=");
  out.append(domain.str());
  out.append(" head=");
  out.append(head_generation.str());
  out.append(" generations=");
  out.append(std::to_string(generations_checked));
  out.append(" bytes=");
  out.append(std::to_string(bytes_checked));
  out.append(" chain_links=");
  out.append(std::to_string(chain_links_checked));
  for (const auto& problem : problems) {
    out.append("\n  - ");
    out.append(problem);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Lineage
// ---------------------------------------------------------------------------
LineageDigest IntentStore::genesis_lineage(const DomainId& domain) {
  std::string seed(kLineageFormatName);
  seed.push_back('|');
  seed.append(domain.str());
  seed.append("|genesis");
  return LineageDigest::of(seed);
}

LineageDigest IntentStore::next_lineage(const LineageDigest& parent, const DomainId& domain,
                                        const GenerationNumber& generation,
                                        const ContentDigest& content) {
  std::string seed(kLineageFormatName);
  seed.push_back('|');
  seed.append(domain.str());
  seed.push_back('|');
  seed.append(parent.hex());
  seed.push_back('|');
  seed.append(generation.str());
  seed.push_back('|');
  seed.append(content.hex());
  return LineageDigest::of(seed);
}

// ---------------------------------------------------------------------------
// Record payload encoding / decoding
// ---------------------------------------------------------------------------
Result<std::string> IntentStore::encode_record(const GenerationRecord& record,
                                               const LineageDigest& lineage) {
  JsonValue envelope;
  (void)envelope.set("format", JsonValue::string(std::string(kGenerationFormatName)));
  (void)envelope.set("format_version",
                     JsonValue::integer(static_cast<std::int64_t>(kStoreFormatVersion)));
  (void)envelope.set("domain", JsonValue::string(record.domain.str()));
  (void)envelope.set("generation", JsonValue::string(record.generation.str()));
  (void)envelope.set("epoch", JsonValue::string(record.epoch.str()));
  (void)envelope.set("incarnation", JsonValue::string(record.incarnation.str()));
  (void)envelope.set("writer", JsonValue::string(record.writer.str()));
  (void)envelope.set("committed_at", JsonValue::string(format_timestamp(record.committed_at)));
  (void)envelope.set("content_digest", JsonValue::string(record.content_digest.hex()));
  (void)envelope.set("document_digest", JsonValue::string(record.document_digest.hex()));
  (void)envelope.set("lineage_digest", JsonValue::string(lineage.hex()));
  if (record.parent.has_value()) {
    JsonValue parent;
    (void)parent.set("generation", JsonValue::string(record.parent->number.str()));
    (void)parent.set("content_digest", JsonValue::string(record.parent->content.hex()));
    (void)parent.set("head_digest", JsonValue::string(record.parent_head.hex()));
    (void)envelope.set("parent", std::move(parent));
  } else {
    (void)envelope.set("parent", JsonValue::null());
  }
  (void)envelope.set("document", to_json(record.document));
  return envelope.dump();
}

Result<GenerationRecord> IntentStore::decode_record(const std::string& bytes) {
  const auto parsed = parse_json(bytes, JsonParseLimits{});
  if (!parsed) {
    return Error(ErrorCode::StoreCorrupt, "generation payload is not valid JSON",
                 parsed.error().render());
  }
  const JsonValue& envelope = parsed.value();
  if (!envelope.is_object()) {
    return Error(ErrorCode::StoreCorrupt, "generation payload is not an object");
  }
  const auto format = string_member(envelope, "format", true);
  if (!format) {
    return format.error();
  }
  if (format.value() != kGenerationFormatName) {
    return Error(ErrorCode::StoreFormatUnsupported, "generation payload has an unknown format",
                 format.value());
  }
  const JsonValue* version = envelope.find("format_version");
  const auto version_value = version == nullptr ? std::optional<std::int64_t>() : version->try_int();
  if (!version_value.has_value() ||
      version_value.value() != static_cast<std::int64_t>(kStoreFormatVersion)) {
    return Error(ErrorCode::StoreFormatUnsupported, "unsupported generation format version");
  }
  const auto domain_text = string_member(envelope, "domain", true);
  if (!domain_text) {
    return domain_text.error();
  }
  const auto parsed_domain = DomainId::parse(domain_text.value());
  if (!parsed_domain) {
    return parsed_domain.error();
  }
  const auto generation_text = string_member(envelope, "generation", true);
  if (!generation_text) {
    return generation_text.error();
  }
  const auto generation_value = parse_decimal_u64(generation_text.value());
  if (!generation_value) {
    return generation_value.error();
  }
  const auto parsed_generation = GenerationNumber::from(generation_value.value());
  if (!parsed_generation) {
    return parsed_generation.error();
  }
  const JsonValue* document = envelope.find("document");
  if (document == nullptr) {
    return Error(ErrorCode::StoreCorrupt, "generation payload has no embedded document");
  }
  const auto document_parsed = from_json(*document);
  if (!document_parsed) {
    return Error(ErrorCode::StoreCorrupt,
                 "embedded intent document is not valid: " + document_parsed.error().render());
  }
  GenerationRecord record;
  record.domain = parsed_domain.value();
  record.generation = parsed_generation.value();
  record.document = document_parsed.value();
  const auto canonical = canonicalize(record.document);
  if (!canonical) {
    return canonical.error();
  }
  record.content_digest = canonical->content_digest;
  record.document_digest = canonical->document_digest;
  record.provenance = record.document.provenance;

  // The envelope must not disagree with the document it carries.
  const auto claimed_content = string_member(envelope, "content_digest", true);
  if (!claimed_content) {
    return claimed_content.error();
  }
  const auto parsed_content = ContentDigest::from_hex(claimed_content.value());
  if (!parsed_content) {
    return parsed_content.error();
  }
  if (parsed_content.value() != record.content_digest) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "record content digest does not match the embedded intent document",
                 claimed_content.value() + " != " + record.content_digest.hex());
  }
  const auto claimed_document = string_member(envelope, "document_digest", true);
  if (!claimed_document) {
    return claimed_document.error();
  }
  const auto parsed_document = DocumentDigest::from_hex(claimed_document.value());
  if (!parsed_document) {
    return parsed_document.error();
  }
  if (parsed_document.value() != record.document_digest) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "record document digest does not match the embedded intent document",
                 claimed_document.value() + " != " + record.document_digest.hex());
  }

  const auto epoch_text = string_member(envelope, "epoch", false);
  if (epoch_text.ok() && !epoch_text.value().empty()) {
    const auto value = parse_decimal_u64(epoch_text.value());
    if (!value) {
      return value.error();
    }
    const auto parsed_epoch = Epoch::from(value.value());
    if (!parsed_epoch) {
      return parsed_epoch.error();
    }
    record.epoch = parsed_epoch.value();
  }
  const auto incarnation_text = string_member(envelope, "incarnation", false);
  if (incarnation_text.ok() && !incarnation_text.value().empty()) {
    const auto parsed_incarnation = IncarnationId::parse(incarnation_text.value());
    if (!parsed_incarnation) {
      return parsed_incarnation.error();
    }
    record.incarnation = parsed_incarnation.value();
  }
  const auto writer_text = string_member(envelope, "writer", false);
  if (writer_text.ok() && !writer_text.value().empty()) {
    const auto parsed_writer = ActorId::parse(writer_text.value());
    if (!parsed_writer) {
      return parsed_writer.error();
    }
    record.writer = parsed_writer.value();
  }
  const auto committed_text = string_member(envelope, "committed_at", true);
  if (!committed_text) {
    return committed_text.error();
  }
  const auto parsed_time = parse_timestamp(committed_text.value());
  if (!parsed_time) {
    return parsed_time.error();
  }
  record.committed_at = parsed_time.value();
  const auto lineage_text = string_member(envelope, "lineage_digest", true);
  if (!lineage_text) {
    return lineage_text.error();
  }
  const auto parsed_lineage = LineageDigest::from_hex(lineage_text.value());
  if (!parsed_lineage) {
    return parsed_lineage.error();
  }
  record.lineage_digest = parsed_lineage.value();

  const JsonValue* parent = envelope.find("parent");
  if (parent != nullptr && parent->is_object()) {
    const auto parent_generation = string_member(*parent, "generation", true);
    const auto parent_content = string_member(*parent, "content_digest", true);
    const auto parent_head = string_member(*parent, "head_digest", true);
    if (!parent_generation || !parent_content || !parent_head) {
      return Error(ErrorCode::StoreCorrupt, "generation payload has a malformed parent reference");
    }
    const auto number_value = parse_decimal_u64(parent_generation.value());
    const auto content_value = ContentDigest::from_hex(parent_content.value());
    if (!number_value || !content_value) {
      return Error(ErrorCode::StoreCorrupt, "generation payload has a malformed parent reference");
    }
    const auto number = GenerationNumber::from(number_value.value());
    if (!number) {
      return number.error();
    }
    const auto parent_head_digest = HeadDigest::from_hex(parent_head.value());
    if (!parent_head_digest) {
      return parent_head_digest.error();
    }
    GenerationRef reference;
    reference.number = number.value();
    reference.content = content_value.value();
    reference.committed_epoch = record.epoch;
    reference.committed_incarnation = record.incarnation;
    record.parent = reference;
    record.parent_head = parent_head_digest.value();
  }
  return record;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
IntentStore::~IntentStore() = default;

Result<std::unique_ptr<IntentStore>> IntentStore::create(const Path& directory,
                                                         const DomainId& domain,
                                                         const StoreOptions& options) {
  Error error = ensure_directory(directory);
  if (!error.ok()) {
    return error;
  }
  const Path descriptor = directory / "store.json";
  if (file_exists(descriptor)) {
    return Error(ErrorCode::StoreBusy, "a store already exists in this directory",
                 directory.string());
  }
  auto store = std::unique_ptr<IntentStore>(new IntentStore());
  store->directory_ = directory;
  store->records_directory_ = directory / "records";
  store->head_path_ = directory / "head.json";
  store->lineage_path_ = directory / "lineage.log";
  store->options_ = options;
  store->domain_ = domain;
  store->store_id_ = random_hex_128();
  store->epoch_ = Epoch(1);
  store->incarnation_ = IncarnationId::generate();
  error = ensure_directory(store->records_directory_);
  if (!error.ok()) {
    return error;
  }
  auto lock = FileLock::acquire(directory / "lock");
  if (!lock) {
    return lock.error();
  }
  store->lock_ = std::move(lock.value());

  JsonValue descriptor_json;
  (void)descriptor_json.set("format", JsonValue::string(std::string(kStoreFormatName)));
  (void)descriptor_json.set("format_version",
                            JsonValue::integer(static_cast<std::int64_t>(kStoreFormatVersion)));
  (void)descriptor_json.set("domain", JsonValue::string(domain.str()));
  (void)descriptor_json.set("store_id", JsonValue::string(store->store_id_));
  (void)descriptor_json.set("created_at",
                            JsonValue::string(format_timestamp(SystemClock().now())));
  const WriteOptions write{options.fsync, false};
  error = write_file_atomic(descriptor, descriptor_json.dump(), write);
  if (!error.ok()) {
    return error;
  }
  store->lineage_ = genesis_lineage(domain);
  store->head_generation_ = GenerationNumber::genesis();
  return store;
}

// ---------------------------------------------------------------------------
// Head pointer
// ---------------------------------------------------------------------------
Result<IntentStore::HeadState> IntentStore::parse_head(const std::string& bytes) const {
  const auto parsed = parse_json(bytes, JsonParseLimits{});
  if (!parsed) {
    return Error(ErrorCode::StoreCorrupt, "head pointer is not valid JSON",
                 parsed.error().render());
  }
  const JsonValue& head = parsed.value();
  if (!head.is_object()) {
    return Error(ErrorCode::StoreCorrupt, "head pointer is not an object");
  }
  const auto format = string_member(head, "format", true);
  if (!format) {
    return format.error();
  }
  if (format.value() != kHeadFormatName) {
    return Error(ErrorCode::StoreFormatUnsupported, "head pointer has an unknown format");
  }
  const JsonValue* version = head.find("format_version");
  const auto version_value = version == nullptr ? std::optional<std::int64_t>() : version->try_int();
  if (!version_value.has_value() ||
      version_value.value() != static_cast<std::int64_t>(kStoreFormatVersion)) {
    return Error(ErrorCode::StoreFormatUnsupported, "unsupported head pointer format version");
  }
  const auto digest_text = string_member(head, "head_digest", true);
  if (!digest_text) {
    return digest_text.error();
  }
  JsonValue copy = head;
  (void)copy.erase("head_digest");
  const Digest computed = Sha256::hash(copy.dump());
  if (computed.hex() != digest_text.value()) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "head pointer integrity digest does not match its contents",
                 digest_text.value() + " != " + computed.hex());
  }
  HeadState state;
  state.raw = head;
  const auto domain_text = string_member(head, "domain", true);
  if (!domain_text) {
    return domain_text.error();
  }
  if (domain_text.value() != domain_.str()) {
    return Error(ErrorCode::UnknownDomain, "head pointer belongs to a different intent domain",
                 domain_text.value());
  }
  const auto generation_text = string_member(head, "generation", true);
  if (!generation_text) {
    return generation_text.error();
  }
  const auto generation_value = parse_decimal_u64(generation_text.value());
  if (!generation_value) {
    return generation_value.error();
  }
  const auto generation = GenerationNumber::from(generation_value.value());
  if (!generation) {
    return generation.error();
  }
  state.generation = generation.value();
  const auto content_text = string_member(head, "content_digest", true);
  const auto document_text = string_member(head, "document_digest", true);
  const auto lineage_text = string_member(head, "lineage_digest", true);
  const auto payload_text = string_member(head, "payload_digest", true);
  const auto record_file = string_member(head, "record_file", true);
  if (!content_text || !document_text || !lineage_text || !payload_text || !record_file) {
    return Error(ErrorCode::StoreCorrupt, "head pointer is missing mandatory members");
  }
  const auto content = ContentDigest::from_hex(content_text.value());
  const auto document = DocumentDigest::from_hex(document_text.value());
  const auto lineage = LineageDigest::from_hex(lineage_text.value());
  const auto payload = PayloadDigest::from_hex(payload_text.value());
  if (!content || !document || !lineage || !payload) {
    return Error(ErrorCode::StoreCorrupt, "head pointer carries a malformed digest");
  }
  state.content = content.value();
  state.document = document.value();
  state.lineage = lineage.value();
  state.payload = payload.value();
  if (record_file.value().find("..") != std::string::npos ||
      record_file.value().find('/') != std::string::npos ||
      record_file.value().find('\\') != std::string::npos) {
    return Error(ErrorCode::StoreCorrupt, "head pointer record path escapes the store directory",
                 record_file.value());
  }
  state.record_path = directory_ / "records" / record_file.value();
  const JsonValue* record_bytes = head.find("record_bytes");
  const auto bytes_value =
      record_bytes == nullptr ? std::optional<std::int64_t>() : record_bytes->try_int();
  if (!bytes_value.has_value() || bytes_value.value() < 0) {
    return Error(ErrorCode::StoreCorrupt, "head pointer has no valid record byte count");
  }
  state.record_bytes = static_cast<std::uint64_t>(bytes_value.value());
  const JsonValue* payload_crc = head.find("payload_crc32");
  const auto crc_value = payload_crc == nullptr ? std::optional<std::int64_t>() : payload_crc->try_int();
  if (!crc_value.has_value() || crc_value.value() < 0) {
    return Error(ErrorCode::StoreCorrupt, "head pointer has no valid payload CRC");
  }
  state.payload_crc = static_cast<std::uint32_t>(crc_value.value());
  const auto epoch_text = string_member(head, "epoch", true);
  const auto incarnation_text = string_member(head, "incarnation", true);
  const auto writer_text = string_member(head, "writer", true);
  const auto committed_text = string_member(head, "committed_at", true);
  if (!epoch_text || !incarnation_text || !writer_text || !committed_text) {
    return Error(ErrorCode::StoreCorrupt, "head pointer is missing provenance members");
  }
  const auto epoch_value = parse_decimal_u64(epoch_text.value());
  if (!epoch_value) {
    return epoch_value.error();
  }
  const auto epoch = Epoch::from(epoch_value.value());
  if (!epoch) {
    return epoch.error();
  }
  state.epoch = epoch.value();
  const auto incarnation = IncarnationId::parse(incarnation_text.value());
  if (!incarnation) {
    return incarnation.error();
  }
  state.incarnation = incarnation.value();
  if (!writer_text.value().empty()) {
    const auto writer = ActorId::parse(writer_text.value());
    if (!writer) {
      return writer.error();
    }
    state.writer = writer.value();
  }
  const auto committed = parse_timestamp(committed_text.value());
  if (!committed) {
    return committed.error();
  }
  state.committed_at = committed.value();
  const auto parent_generation_text = string_member(head, "parent_generation", true);
  if (!parent_generation_text) {
    return parent_generation_text.error();
  }
  const auto parent_generation = parse_decimal_u64(parent_generation_text.value());
  if (!parent_generation) {
    return parent_generation.error();
  }
  state.parent_generation = parent_generation.value();
  if (state.parent_generation != 0) {
    const auto parent_content_text = string_member(head, "parent_content_digest", true);
    const auto parent_head_text = string_member(head, "parent_head_digest", true);
    if (!parent_content_text || !parent_head_text) {
      return Error(ErrorCode::StoreCorrupt, "head pointer is missing its parent reference");
    }
    const auto parent_content = ContentDigest::from_hex(parent_content_text.value());
    const auto parent_head = HeadDigest::from_hex(parent_head_text.value());
    if (!parent_content || !parent_head) {
      return Error(ErrorCode::StoreCorrupt, "head pointer parent reference is malformed");
    }
    state.parent_content = parent_content.value();
    state.parent_head = parent_head.value();
  }
  const auto parsed_head_digest = HeadDigest::from_hex(digest_text.value());
  if (!parsed_head_digest) {
    return parsed_head_digest.error();
  }
  state.head_digest = parsed_head_digest.value();
  state.present = true;
  return state;
}

Result<IntentStore::HeadState> IntentStore::validate_head(const HeadState& state) const {
  if (!file_exists(state.record_path)) {
    return Error(ErrorCode::StoreNotFound, "head pointer references a missing generation record",
                 state.record_path.string());
  }
  const auto size = ifabric::file_size(state.record_path);
  if (!size) {
    return size.error();
  }
  if (size.value() != state.record_bytes) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "generation record size does not match the head pointer",
                 std::to_string(size.value()) + " != " + std::to_string(state.record_bytes));
  }
  GenerationRecord record;
  const Error error = read_record_file(state.record_path, record);
  if (!error.ok()) {
    return error;
  }
  if (record.generation != state.generation) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "generation record number does not match the head pointer",
                 record.generation.str() + " != " + state.generation.str());
  }
  if (record.content_digest != state.content) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "generation content digest does not match the head pointer");
  }
  if (record.document_digest != state.document) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "generation document digest does not match the head pointer");
  }
  if (record.lineage_digest != state.lineage) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "generation lineage digest does not match the head pointer");
  }
  if (record.head_digest != state.head_digest) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "generation head digest does not match the head pointer");
  }
  if (state.parent_generation != 0) {
    if (!record.parent.has_value() || record.parent->number.value() != state.parent_generation) {
      return Error(ErrorCode::StoreIntegrityMismatch,
                   "generation parent reference does not match the head pointer");
    }
  }
  // Chain check against the retained parent record when it is still present.
  if (state.parent_generation != 0) {
    const auto parent_number = GenerationNumber::from(state.parent_generation);
    if (parent_number) {
      const auto parent_path = record_path_for(parent_number.value());
      if (parent_path.ok() && file_exists(parent_path.value())) {
        GenerationRecord parent;
        const Error parent_error = read_record_file(parent_path.value(), parent);
        if (!parent_error.ok()) {
          return parent_error;
        }
        if (parent.head_digest != state.parent_head) {
          return Error(ErrorCode::StoreIntegrityMismatch,
                       "generation chain is broken: parent head digest does not match",
                       parent.head_digest.hex() + " != " + state.parent_head.hex());
        }
        if (parent.content_digest != state.parent_content) {
          return Error(ErrorCode::StoreIntegrityMismatch,
                       "generation chain is broken: parent content digest does not match");
        }
      }
    }
  }
  return state;
}

// ---------------------------------------------------------------------------
// Record files
// ---------------------------------------------------------------------------
Error IntentStore::read_record_file(const Path& path, GenerationRecord& out) const {
  const auto bytes = read_file(path, kRecordHeaderBytes + kMaxRecordBytes);
  if (!bytes) {
    return bytes.error().code == ErrorCode::LimitExceeded
               ? Error(ErrorCode::LimitExceeded, "generation record exceeds the byte bound",
                       path.string())
               : bytes.error();
  }
  const std::string& raw = bytes.value();
  if (raw.size() < kRecordHeaderBytes) {
    return Error(ErrorCode::StoreCorrupt, "generation record is truncated before its header",
                 path.string());
  }
  for (std::size_t i = 0; i < kRecordMagic.size(); ++i) {
    if (static_cast<std::uint8_t>(raw[i]) != kRecordMagic[i]) {
      return Error(ErrorCode::StoreFormatUnsupported, "generation record magic is wrong",
                   path.string());
    }
  }
  const std::uint32_t format_version = read_u32(raw, 8);
  if (format_version != kStoreFormatVersion) {
    return Error(ErrorCode::StoreFormatUnsupported, "unsupported generation record version",
                 std::to_string(format_version));
  }
  const std::uint32_t header_bytes = read_u32(raw, 12);
  if (header_bytes != kRecordHeaderBytes) {
    return Error(ErrorCode::StoreCorrupt, "generation record header size is wrong",
                 std::to_string(header_bytes));
  }
  const std::uint32_t stored_header_crc = read_u32(raw, 124);
  const std::uint32_t computed_header_crc = crc32(std::string_view(raw.data(), 124));
  if (stored_header_crc != computed_header_crc) {
    return Error(ErrorCode::StoreCorrupt, "generation record header CRC does not match",
                 path.string());
  }
  const std::uint64_t generation_value = read_u64(raw, 16);
  const std::uint64_t payload_bytes = read_u64(raw, 24);
  const std::uint32_t stored_payload_crc = read_u32(raw, 32);
  const auto expected_size = checked_add(static_cast<std::uint64_t>(kRecordHeaderBytes), payload_bytes);
  if (!expected_size) {
    return expected_size.error();
  }
  if (static_cast<std::uint64_t>(raw.size()) != expected_size.value()) {
    return Error(ErrorCode::StoreCorrupt, "generation record length does not match its header",
                 std::to_string(raw.size()) + " != " + std::to_string(expected_size.value()));
  }
  const std::string payload = raw.substr(kRecordHeaderBytes);
  if (crc32(payload) != stored_payload_crc) {
    return Error(ErrorCode::StoreIntegrityMismatch, "generation record payload CRC does not match",
                 path.string());
  }
  std::array<std::uint8_t, Digest::kBytes> stored_digest{};
  for (std::size_t i = 0; i < stored_digest.size(); ++i) {
    stored_digest[i] = static_cast<std::uint8_t>(raw[40 + i]);
  }
  const Digest computed_digest = Sha256::hash(payload);
  if (computed_digest.bytes() != stored_digest) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "generation record payload digest does not match", path.string());
  }
  const auto decoded = decode_record(payload);
  if (!decoded) {
    return decoded.error();
  }
  out = decoded.value();
  if (out.generation.value() != generation_value) {
    return Error(ErrorCode::StoreIntegrityMismatch,
                 "generation record number does not match its header",
                 out.generation.str() + " != " + std::to_string(generation_value));
  }
  if (out.domain != domain_) {
    return Error(ErrorCode::UnknownDomain,
                 "generation record belongs to a different intent domain", out.domain.str());
  }
  HeadDigest head_digest;
  const auto parsed_head = HeadDigest::from_bytes(
      reinterpret_cast<const std::uint8_t*>(raw.data()) + 72, Digest::kBytes);
  if (!parsed_head) {
    return parsed_head.error();
  }
  out.head_digest = parsed_head.value();
  out.record_path = path;
  out.record_bytes = static_cast<std::uint64_t>(raw.size());
  return Error();
}

Result<Path> IntentStore::record_path_for(const GenerationNumber& number) const {
  const auto entries = list_files(records_directory_, ".rec");
  if (!entries) {
    return entries.error();
  }
  const std::string prefix = "gen-" + number.padded() + "-";
  for (const auto& entry : entries.value()) {
    const std::string name = entry.filename().string();
    if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
      return entry;
    }
  }
  if (number.value() > head_generation_.value()) {
    return Error(ErrorCode::GenerationNotFound, "generation has never been committed",
                 number.str());
  }
  return Error(ErrorCode::GenerationPruned, "generation is no longer retained", number.str());
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IntentStore>> IntentStore::open(const Path& directory,
                                                       const StoreOptions& options) {
  if (!directory_exists(directory)) {
    return Error(ErrorCode::StoreNotFound, "store directory does not exist", directory.string());
  }
  const Path descriptor = directory / "store.json";
  if (!file_exists(descriptor)) {
    return Error(ErrorCode::StoreNotFound, "store descriptor is missing", descriptor.string());
  }
  const auto descriptor_bytes = read_file(descriptor, 1024u * 1024u);
  if (!descriptor_bytes) {
    return descriptor_bytes.error();
  }
  const auto descriptor_json = parse_json(descriptor_bytes.value(), JsonParseLimits{});
  if (!descriptor_json) {
    return Error(ErrorCode::StoreCorrupt, "store descriptor is not valid JSON",
                 descriptor_json.error().render());
  }
  const auto format = string_member(descriptor_json.value(), "format", true);
  if (!format) {
    return format.error();
  }
  if (format.value() != kStoreFormatName) {
    return Error(ErrorCode::StoreFormatUnsupported, "store descriptor has an unknown format");
  }
  const JsonValue* version = descriptor_json->find("format_version");
  const auto version_value = version == nullptr ? std::optional<std::int64_t>() : version->try_int();
  if (!version_value.has_value() ||
      version_value.value() != static_cast<std::int64_t>(kStoreFormatVersion)) {
    return Error(ErrorCode::StoreFormatUnsupported, "unsupported store format version",
                 version_value.has_value() ? std::to_string(version_value.value()) : "missing");
  }
  const auto domain_text = string_member(descriptor_json.value(), "domain", true);
  if (!domain_text) {
    return domain_text.error();
  }
  const auto parsed_domain = DomainId::parse(domain_text.value());
  if (!parsed_domain) {
    return parsed_domain.error();
  }

  auto store = std::unique_ptr<IntentStore>(new IntentStore());
  store->directory_ = directory;
  store->records_directory_ = directory / "records";
  store->head_path_ = directory / "head.json";
  store->lineage_path_ = directory / "lineage.log";
  store->options_ = options;
  store->domain_ = parsed_domain.value();
  const JsonValue* store_id = descriptor_json->find("store_id");
  store->store_id_ =
      store_id == nullptr || store_id->try_string() == nullptr ? "unknown" : *store_id->try_string();

  auto lock = FileLock::acquire(directory / "lock");
  if (!lock) {
    return lock.error();
  }
  store->lock_ = std::move(lock.value());
  Error error = ensure_directory(store->records_directory_);
  if (!error.ok()) {
    return error;
  }

  std::vector<std::string> problems;
  std::optional<HeadState> primary;
  std::optional<HeadState> backup;
  const bool primary_present = file_exists(store->head_path_);
  const bool backup_present = file_exists(Path(store->head_path_.string() + ".bak"));

  const auto load_candidate = [&](const Path& path) -> std::optional<HeadState> {
    const auto bytes = read_file(path, kMaxRecordBytes + 1024u * 1024u);
    if (!bytes.ok()) {
      problems.push_back(path.filename().string() + ": " + bytes.error().render());
      return std::nullopt;
    }
    const auto parsed = store->parse_head(bytes.value());
    if (!parsed.ok()) {
      problems.push_back(path.filename().string() + ": " + parsed.error().render());
      return std::nullopt;
    }
    const auto validated = store->validate_head(parsed.value());
    if (!validated.ok()) {
      problems.push_back(path.filename().string() + ": " + validated.error().render());
      return std::nullopt;
    }
    return validated.value();
  };

  if (primary_present) {
    primary = load_candidate(store->head_path_);
  }
  if (backup_present) {
    backup = load_candidate(Path(store->head_path_.string() + ".bak"));
  }

  if (!primary.has_value() && !backup.has_value()) {
    if (primary_present || backup_present) {
      return Error(ErrorCode::StoreCorrupt,
                   "no head pointer could be verified; refusing to guess a committed generation",
                   join_problems(problems));
    }
    store->lineage_ = genesis_lineage(store->domain_);
    store->epoch_ = Epoch(1);
    store->incarnation_ = IncarnationId::generate();
    store->head_generation_ = GenerationNumber::genesis();
    return store;
  }

  HeadState chosen = primary.has_value() ? primary.value() : backup.value();
  bool used_backup = !primary.has_value();
  if (primary.has_value() && backup.has_value() &&
      backup->generation.value() > primary->generation.value()) {
    chosen = backup.value();
    used_backup = true;
  }
  store->head_ = chosen;
  store->head_generation_ = chosen.generation;
  store->head_content_ = chosen.content;
  store->head_digest_ = chosen.head_digest;
  store->lineage_ = chosen.lineage;
  if (used_backup) {
    store->recovered_ = true;
    store->recovery_note_ = "recovered head generation " + chosen.generation.str() +
                            " from the head pointer backup (" + join_problems(problems) + ")";
    const WriteOptions write{options.fsync, false};
    const Error rewrite = write_file_atomic(store->head_path_, chosen.raw.dump(), write);
    if (!rewrite.ok()) {
      return rewrite;
    }
  }
  const auto next_epoch = chosen.epoch.next();
  if (!next_epoch) {
    return next_epoch.error();
  }
  store->epoch_ = next_epoch.value();
  store->incarnation_ = IncarnationId::generate();

  const auto entries = list_files(store->records_directory_, ".rec");
  if (!entries) {
    return entries.error();
  }
  for (const auto& entry : entries.value()) {
    const auto number = parse_record_file_name(entry.filename().string());
    if (!number) {
      // A file that cannot be a generation record can never be authority; it is
      // debris from an abandoned tool or a partial copy.
      if (options.prune_orphans) {
        (void)remove_file(entry);
      }
      continue;
    }
    if (number->value() > store->head_generation_.value()) {
      if (options.prune_orphans) {
        (void)remove_file(entry);
      }
      continue;
    }
    store->retained_generations_ = saturating_add(store->retained_generations_, 1);
  }
  return store;
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------
Result<GenerationRef> IntentStore::head() const {
  if (head_generation_.is_genesis()) {
    return Error(ErrorCode::NoCommittedGeneration, "this intent domain has no committed generation");
  }
  return GenerationRef{head_generation_, head_content_, head_.epoch, head_.incarnation};
}

Result<std::string> IntentStore::head_json() const {
  if (!head_.present) {
    return Error(ErrorCode::NoCommittedGeneration, "this intent domain has no committed generation");
  }
  return head_.raw.dump();
}

Result<GenerationRecord> IntentStore::load(const GenerationNumber& number) const {
  if (number.is_genesis()) {
    return Error(ErrorCode::GenerationNotFound, "generation zero is not a committed generation");
  }
  if (number.value() > head_generation_.value()) {
    return Error(ErrorCode::GenerationNotFound, "generation is ahead of the committed head",
                 number.str());
  }
  const auto path = record_path_for(number);
  if (!path) {
    return path.error();
  }
  GenerationRecord record;
  const Error error = read_record_file(path.value(), record);
  if (!error.ok()) {
    return error;
  }
  return record;
}

Result<GenerationRecord> IntentStore::load_head() const {
  if (!head_.present) {
    return Error(ErrorCode::NoCommittedGeneration, "this intent domain has no committed generation");
  }
  return load(head_generation_);
}

Result<std::vector<GenerationSummary>> IntentStore::list(std::size_t limit) const {
  std::vector<GenerationSummary> out;
  const auto entries = list_files(records_directory_, ".rec");
  if (!entries) {
    return entries.error();
  }
  for (const auto& entry : entries.value()) {
    const auto number = parse_record_file_name(entry.filename().string());
    if (!number) {
      continue;
    }
    if (number->value() > head_generation_.value()) {
      continue;
    }
    const auto size = ifabric::file_size(entry);
    GenerationSummary summary;
    summary.number = number.value();
    summary.record_bytes = size.ok() ? size.value() : 0;
    if (number->value() == head_generation_.value()) {
      summary.content = head_content_;
      summary.document = head_.document;
      summary.committed_at = head_.committed_at;
      summary.writer = head_.writer;
      summary.epoch = head_.epoch;
    } else {
      GenerationRecord record;
      const Error error = read_record_file(entry, record);
      if (!error.ok()) {
        return error;
      }
      summary.content = record.content_digest;
      summary.document = record.document_digest;
      summary.committed_at = record.committed_at;
      summary.writer = record.writer;
      summary.epoch = record.epoch;
    }
    out.push_back(std::move(summary));
  }
  std::sort(out.begin(), out.end(), [](const GenerationSummary& a, const GenerationSummary& b) {
    return b.number.value() < a.number.value();
  });
  if (limit != 0 && out.size() > limit) {
    out.resize(limit);
  }
  return out;
}

VerificationReport IntentStore::verify_all() const {
  VerificationReport report;
  report.domain = domain_;
  report.head_generation = head_generation_;
  const auto entries = list_files(records_directory_, ".rec");
  if (!entries) {
    report.problems.push_back(entries.error().render());
    return report;
  }
  std::vector<GenerationRecord> records;
  for (const auto& entry : entries.value()) {
    GenerationRecord record;
    const Error error = read_record_file(entry, record);
    if (!error.ok()) {
      report.problems.push_back(entry.filename().string() + ": " + error.render());
      continue;
    }
    report.generations_checked = saturating_add(report.generations_checked, 1);
    report.bytes_checked = saturating_add(report.bytes_checked,
                                          static_cast<std::size_t>(record.record_bytes));
    records.push_back(std::move(record));
  }
  std::sort(records.begin(), records.end(),
            [](const GenerationRecord& a, const GenerationRecord& b) {
              return a.generation.value() < b.generation.value();
            });
  for (std::size_t i = 1; i < records.size(); ++i) {
    if (records[i].generation.value() != records[i - 1].generation.value() + 1) {
      continue;  // a pruned generation breaks the chain by design
    }
    if (!records[i].parent.has_value()) {
      report.problems.push_back("generation " + records[i].generation.str() +
                                " has no parent reference");
      continue;
    }
    if (records[i].parent->number.value() != records[i - 1].generation.value()) {
      report.problems.push_back("generation " + records[i].generation.str() +
                                " names the wrong parent generation");
      continue;
    }
    if (records[i].parent->content != records[i - 1].content_digest) {
      report.problems.push_back("generation " + records[i].generation.str() +
                                " parent content digest does not match");
      continue;
    }
    report.chain_links_checked = saturating_add(report.chain_links_checked, 1);
  }
  if (head_.present) {
    bool head_found = false;
    for (const auto& record : records) {
      if (record.generation == head_generation_) {
        head_found = true;
        if (record.head_digest != head_.head_digest) {
          report.problems.push_back("head generation record digest does not match the head pointer");
        }
        break;
      }
    }
    if (!head_found) {
      report.problems.push_back("the head generation record is missing from the store");
    }
  }
  return report;
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------
Result<std::string> IntentStore::build_head_json(const GenerationRecord& record) const {
  JsonValue head;
  (void)head.set("format", JsonValue::string(std::string(kHeadFormatName)));
  (void)head.set("format_version",
                 JsonValue::integer(static_cast<std::int64_t>(kStoreFormatVersion)));
  (void)head.set("domain", JsonValue::string(domain_.str()));
  (void)head.set("generation", JsonValue::string(record.generation.str()));
  (void)head.set("content_digest", JsonValue::string(record.content_digest.hex()));
  (void)head.set("document_digest", JsonValue::string(record.document_digest.hex()));
  (void)head.set("lineage_digest", JsonValue::string(record.lineage_digest.hex()));
  (void)head.set("record_file", JsonValue::string(record.record_path.filename().string()));
  (void)head.set("record_bytes", JsonValue::integer(static_cast<std::int64_t>(record.record_bytes)));
  (void)head.set("payload_digest", JsonValue::string(record.payload.hex()));
  (void)head.set("payload_crc32",
                 JsonValue::integer(static_cast<std::int64_t>(record.payload_crc)));
  (void)head.set("epoch", JsonValue::string(record.epoch.str()));
  (void)head.set("incarnation", JsonValue::string(record.incarnation.str()));
  (void)head.set("writer", JsonValue::string(record.writer.str()));
  (void)head.set("committed_at", JsonValue::string(format_timestamp(record.committed_at)));
  (void)head.set("parent_generation",
                 JsonValue::string(record.parent.has_value() ? record.parent->number.str() : "0"));
  (void)head.set("parent_content_digest",
                 JsonValue::string(record.parent.has_value() ? record.parent->content.hex()
                                                             : std::string(64, '0')));
  (void)head.set("parent_head_digest",
                 JsonValue::string(record.parent_head.hex()));
  const std::string digest = Sha256::hash(head.dump()).hex();
  (void)head.set("head_digest", JsonValue::string(digest));
  return head.dump();
}

Error IntentStore::append_lineage(const GenerationRecord& record) {
  JsonValue line;
  (void)line.set("event", JsonValue::string("commit"));
  (void)line.set("generation", JsonValue::string(record.generation.str()));
  (void)line.set("content_digest", JsonValue::string(record.content_digest.hex()));
  (void)line.set("lineage_digest", JsonValue::string(record.lineage_digest.hex()));
  (void)line.set("head_digest", JsonValue::string(record.head_digest.hex()));
  (void)line.set("epoch", JsonValue::string(record.epoch.str()));
  (void)line.set("incarnation", JsonValue::string(record.incarnation.str()));
  (void)line.set("writer", JsonValue::string(record.writer.str()));
  (void)line.set("committed_at", JsonValue::string(format_timestamp(record.committed_at)));
  std::string payload = line.dump();
  payload.push_back('\n');
#if defined(_WIN32)
  FILE* stream = nullptr;
  if (::_wfopen_s(&stream, lineage_path_.c_str(), L"ab") != 0) {
    stream = nullptr;
  }
#else
  FILE* stream = std::fopen(lineage_path_.c_str(), "ab");
#endif
  if (stream == nullptr) {
    return Error(ErrorCode::IoError, "cannot append to the lineage journal",
                 lineage_path_.string());
  }
  const std::size_t written = std::fwrite(payload.data(), 1, payload.size(), stream);
  (void)std::fflush(stream);
  if (options_.fsync) {
    (void)sync_directory(directory_);
  }
  if (std::fclose(stream) != 0 || written != payload.size()) {
    return Error(ErrorCode::IoError, "lineage journal append failed", lineage_path_.string());
  }
  return Error();
}

Result<GenerationRef> IntentStore::commit(GenerationRecord record, const FenceToken& fence) {
  if (fence.writer.empty()) {
    return Error(ErrorCode::Unauthorized, "a commit must name the writer that produced it");
  }
  if (fence.expected_generation != head_generation_) {
    return Error(ErrorCode::StaleWriter,
                 "the writer expected a different head generation than the committed one",
                 "expected " + fence.expected_generation.str() + ", committed " +
                     head_generation_.str());
  }
  if (fence.expected_epoch != epoch_) {
    return Error(ErrorCode::StaleEpoch,
                 "the writer belongs to a previous store epoch",
                 "expected " + fence.expected_epoch.str() + ", current " + epoch_.str());
  }
  if (fence.expected_incarnation != incarnation_) {
    return Error(ErrorCode::StaleEpoch,
                 "the writer belongs to a previous store incarnation",
                 "expected " + fence.expected_incarnation.str() + ", current " +
                     incarnation_.str());
  }
  if (record.domain != domain_) {
    return Error(ErrorCode::UnknownDomain, "commit targets a different intent domain",
                 record.domain.str());
  }
  if (record.writer != fence.writer) {
    return Error(ErrorCode::Unauthorized, "the commit writer does not match the fence token");
  }
  const auto expected_next = head_generation_.is_genesis()
                                 ? GenerationNumber::from(1)
                                 : head_generation_.next();
  if (!expected_next) {
    return expected_next.error();
  }
  if (record.generation != expected_next.value()) {
    return Error(ErrorCode::StaleWriter, "the commit does not advance the head by exactly one",
                 "generation " + record.generation.str() + " vs expected " +
                     expected_next.value().str());
  }
  if (retained_generations_ >= options_.max_generations) {
    return Error(ErrorCode::LimitExceeded,
                 "the store has reached its configured generation retention bound",
                 std::to_string(options_.max_generations));
  }
  const auto canonical = canonicalize(record.document);
  if (!canonical) {
    return canonical.error();
  }
  record.content_digest = canonical->content_digest;
  record.document_digest = canonical->document_digest;
  record.document = canonical->normalized;
  record.provenance = record.document.provenance;
  record.epoch = epoch_;
  record.incarnation = incarnation_;
  record.lineage_digest = next_lineage(lineage_, domain_, record.generation, record.content_digest);
  if (head_.present) {
    record.parent = GenerationRef{head_generation_, head_content_, head_.epoch, head_.incarnation};
    record.parent_head = head_digest_;
  } else {
    record.parent.reset();
    record.parent_head = HeadDigest();
  }
  if (record.committed_at == Timestamp{}) {
    record.committed_at = SystemClock().now();
  }

  const auto payload = encode_record(record, record.lineage_digest);
  if (!payload) {
    return payload.error();
  }
  record.payload = PayloadDigest::of(payload.value());
  record.payload_crc = crc32(payload.value());
  record.record_path = records_directory_ / record_file_name(record.generation, record.payload);
  const auto total_bytes =
      checked_add(static_cast<std::uint64_t>(kRecordHeaderBytes), payload.value().size());
  if (!total_bytes) {
    return total_bytes.error();
  }
  record.record_bytes = total_bytes.value();

  // The head pointer determines this generation's head digest. It is computed
  // before the record is written because it is stored in the record header and
  // is therefore covered by the header integrity check.
  const auto head_json = build_head_json(record);
  if (!head_json) {
    return head_json.error();
  }
  const auto parsed_head = parse_head(head_json.value());
  if (!parsed_head) {
    return parsed_head.error();
  }
  record.head_digest = parsed_head.value().head_digest;

  std::string record_bytes;
  record_bytes.reserve(kRecordHeaderBytes + payload.value().size());
  record_bytes.append(reinterpret_cast<const char*>(kRecordMagic.data()), kRecordMagic.size());
  put_u32(record_bytes, kStoreFormatVersion);
  put_u32(record_bytes, static_cast<std::uint32_t>(kRecordHeaderBytes));
  put_u64(record_bytes, record.generation.value());
  put_u64(record_bytes, static_cast<std::uint64_t>(payload.value().size()));
  put_u32(record_bytes, record.payload_crc);
  put_u32(record_bytes, 0);
  for (std::uint8_t byte : record.payload.digest().bytes()) {
    record_bytes.push_back(static_cast<char>(byte));
  }
  for (std::uint8_t byte : record.head_digest.digest().bytes()) {
    record_bytes.push_back(static_cast<char>(byte));
  }
  for (int i = 0; i < 20; ++i) {
    record_bytes.push_back('\0');
  }
  put_u32(record_bytes, crc32(std::string_view(record_bytes.data(), 124)));

  // Re-parse the head pointer once the record length is final so that the
  // digest written into the header is exactly the digest of the head pointer
  // that will be installed.
  const auto final_head = build_head_json(record);
  if (!final_head) {
    return final_head.error();
  }
  const auto final_parsed = parse_head(final_head.value());
  if (!final_parsed) {
    return final_parsed.error();
  }
  if (final_parsed.value().head_digest != record.head_digest) {
    return Error(ErrorCode::Internal, "head digest changed while finalizing the commit");
  }
  record_bytes.append(payload.value());

  const WriteOptions write{options_.fsync, false};
  const Error write_error = write_file_atomic(record.record_path, record_bytes, write);
  if (!write_error.ok()) {
    return write_error;
  }
  const Error lineage_error = append_lineage(record);
  if (!lineage_error.ok()) {
    return lineage_error;
  }
  const WriteOptions head_write{options_.fsync, true};
  const Error head_error = write_file_atomic(head_path_, final_head.value(), head_write);
  if (!head_error.ok()) {
    return head_error;
  }
  head_ = final_parsed.value();
  head_generation_ = record.generation;
  head_content_ = record.content_digest;
  head_digest_ = record.head_digest;
  lineage_ = record.lineage_digest;
  retained_generations_ = saturating_add(retained_generations_, 1);
  return record.reference();
}

Error IntentStore::prune_before(const GenerationNumber& keep_from) {
  const auto entries = list_files(records_directory_, ".rec");
  if (!entries) {
    return entries.error();
  }
  std::size_t removed = 0;
  for (const auto& entry : entries.value()) {
    const auto number = parse_record_file_name(entry.filename().string());
    if (!number || number->value() >= keep_from.value()) {
      continue;
    }
    (void)remove_file(entry);
    removed = saturating_add(removed, 1);
  }
  if (removed > 0 && retained_generations_ >= removed) {
    retained_generations_ -= removed;
  } else {
    retained_generations_ = 0;
  }
  return Error();
}

}  // namespace ifabric
