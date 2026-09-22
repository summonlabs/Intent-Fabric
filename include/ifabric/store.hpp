// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Versioned, integrity-checked persistence for committed intent generations,
// lineage and metadata.
//
// Layout of a store directory:
//
//   store.json    descriptor (format, format version, domain, store identity)
//   head.json     authoritative head pointer for the single committed generation
//   head.json.bak previous good head pointer, used for conservative recovery
//   lock          exclusive process-scoped lock
//   records/      immutable generation records, one file per committed generation
//   lineage.log   append-only lineage journal
//
// Every record is verified against a header CRC, a payload CRC, a payload
// SHA-256 and the content digest recomputed from the embedded intent document.
// Recovery never accepts an unverified record: if neither the head pointer nor
// its backup validates, opening the store fails with StoreCorrupt.

#ifndef IFABRIC_STORE_HPP
#define IFABRIC_STORE_HPP

#include "ifabric/canonical.hpp"
#include "ifabric/io.hpp"

namespace ifabric {

inline constexpr std::size_t kRecordHeaderBytes = 128;
inline constexpr std::uint32_t kStoreFormatVersion = 1;
inline constexpr std::string_view kStoreFormatName = "ifabric.store";
inline constexpr std::string_view kHeadFormatName = "ifabric.head";
inline constexpr std::string_view kGenerationFormatName = "ifabric.generation";
inline constexpr std::string_view kLineageFormatName = "ifabric.lineage";

struct GenerationRecord {
  DomainId domain;
  GenerationNumber generation;
  Epoch epoch;
  IncarnationId incarnation;
  ActorId writer;
  Timestamp committed_at{};
  ContentDigest content_digest;
  DocumentDigest document_digest;
  LineageDigest lineage_digest;
  HeadDigest head_digest;
  HeadDigest parent_head;
  PayloadDigest payload;
  std::uint32_t payload_crc = 0;
  std::optional<GenerationRef> parent;
  Provenance provenance;
  IntentDocument document;
  Path record_path;
  std::uint64_t record_bytes = 0;

  GenerationRef reference() const {
    GenerationRef out;
    out.number = generation;
    out.content = content_digest;
    out.committed_epoch = epoch;
    out.committed_incarnation = incarnation;
    return out;
  }
};

struct GenerationSummary {
  GenerationNumber number;
  ContentDigest content;
  DocumentDigest document;
  Timestamp committed_at{};
  ActorId writer;
  Epoch epoch;
  std::uint64_t record_bytes = 0;
};

struct VerificationReport {
  DomainId domain;
  GenerationNumber head_generation;
  std::size_t generations_checked = 0;
  std::uint64_t bytes_checked = 0;
  std::size_t chain_links_checked = 0;
  std::vector<std::string> problems;

  bool ok() const noexcept { return problems.empty(); }

  JsonValue to_json() const;
  std::string render() const;
};

struct StoreOptions {
  std::size_t max_generations = kMaxGenerationHistory;
  bool fsync = true;
  bool prune_orphans = true;
};

class IntentStore {
 public:
  ~IntentStore();
  IntentStore(const IntentStore&) = delete;
  IntentStore& operator=(const IntentStore&) = delete;

  // Creates a new store. Fails with StoreBusy when the directory already
  // contains a descriptor.
  static Result<std::unique_ptr<IntentStore>> create(const Path& directory,
                                                     const DomainId& domain,
                                                     const StoreOptions& options = {});
  // Opens an existing store, taking the exclusive lock and bumping the epoch.
  static Result<std::unique_ptr<IntentStore>> open(const Path& directory,
                                                   const StoreOptions& options = {});

  const DomainId& domain() const noexcept { return domain_; }
  const Path& directory() const noexcept { return directory_; }
  Epoch epoch() const noexcept { return epoch_; }
  const IncarnationId& incarnation() const noexcept { return incarnation_; }
  bool recovered() const noexcept { return recovered_; }
  const std::string& recovery_note() const noexcept { return recovery_note_; }

  bool has_committed() const noexcept { return !head_generation_.is_genesis(); }
  HeadDigest head_digest() const noexcept { return head_digest_; }
  GenerationNumber head_generation() const noexcept { return head_generation_; }
  ContentDigest head_content() const noexcept { return head_content_; }
  Result<GenerationRef> head() const;
  Result<std::string> head_json() const;

  Result<GenerationRecord> load(const GenerationNumber& number) const;
  Result<GenerationRecord> load_head() const;
  Result<std::vector<GenerationSummary>> list(std::size_t limit) const;
  VerificationReport verify_all() const;

  // Atomic from the caller's point of view: the record is durable before the
  // head pointer is replaced. A failed commit leaves the authoritative head
  // untouched.
  Result<GenerationRef> commit(GenerationRecord record, const FenceToken& fence);

  // Explicit retention operation. Pruned generations read back as
  // GenerationPruned, never as a stale authoritative state.
  Error prune_before(const GenerationNumber& keep_from);

  // Serializes the canonical envelope for a record (exposed for tooling and
  // tests, since it is the durable payload format).
  static Result<std::string> encode_record(const GenerationRecord& record,
                                           const LineageDigest& lineage);
  static Result<GenerationRecord> decode_record(const std::string& bytes);

  static LineageDigest next_lineage(const LineageDigest& parent, const DomainId& domain,
                                    const GenerationNumber& generation,
                                    const ContentDigest& content);
  static LineageDigest genesis_lineage(const DomainId& domain);

 private:
  IntentStore() = default;

  struct HeadState {
    bool present = false;
    GenerationNumber generation;
    ContentDigest content;
    DocumentDigest document;
    LineageDigest lineage;
    HeadDigest head_digest;
    Path record_path;
    std::uint64_t record_bytes = 0;
    PayloadDigest payload;
    std::uint32_t payload_crc = 0;
    Epoch epoch;
    IncarnationId incarnation;
    ActorId writer;
    Timestamp committed_at{};
    std::uint64_t parent_generation = 0;
    ContentDigest parent_content;
    HeadDigest parent_head;
    JsonValue raw;
  };

  Result<HeadState> parse_head(const std::string& bytes) const;
  Result<HeadState> validate_head(const HeadState& state) const;
  Result<std::string> build_head_json(const GenerationRecord& record) const;
  Result<Path> record_path_for(const GenerationNumber& number) const;
  Error read_record_file(const Path& path, GenerationRecord& out) const;
  Error append_lineage(const GenerationRecord& record);

  Path directory_;
  Path records_directory_;
  Path head_path_;
  Path lineage_path_;
  StoreOptions options_;
  DomainId domain_;
  std::string store_id_;
  Epoch epoch_;
  IncarnationId incarnation_;
  bool recovered_ = false;
  std::string recovery_note_;
  HeadState head_;
  GenerationNumber head_generation_;
  ContentDigest head_content_;
  HeadDigest head_digest_;
  LineageDigest lineage_;
  std::unique_ptr<FileLock> lock_;
  std::size_t retained_generations_ = 0;
};

}  // namespace ifabric

#endif  // IFABRIC_STORE_HPP
