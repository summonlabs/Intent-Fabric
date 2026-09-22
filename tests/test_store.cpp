// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/store.hpp"
#include "ifabric/synth.hpp"
#include "ifabric_test.hpp"
#include "process.hpp"

#include <fstream>

using namespace ifabric;

namespace {

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

IntentDocument document_for(std::uint64_t seed) {
  SynthOptions options;
  options.seed = seed;
  options.leaves = 2 + static_cast<std::size_t>(seed % 3);
  IntentDocument document = synthesize(core_domain(), options);
  document.provenance.description = "generation " + std::to_string(seed);
  return document;
}

FenceToken fence_for(const IntentStore& store, const std::string& actor,
                     const GenerationNumber& expected) {
  FenceToken token;
  token.writer = ActorId::parse(actor).value();
  token.expected_generation = expected;
  token.expected_epoch = store.epoch();
  token.expected_incarnation = store.incarnation();
  return token;
}

Result<GenerationRef> commit_next(IntentStore& store, const IntentDocument& document,
                                  const std::string& actor, const GenerationNumber& expected) {
  const FenceToken token = fence_for(store, actor, expected);
  GenerationRecord record;
  record.domain = store.domain();
  record.generation = expected.is_genesis() ? GenerationNumber::from(1).value()
                                            : expected.next().value();
  record.writer = token.writer;
  record.committed_at = Timestamp(std::chrono::milliseconds(
      1767225600000ll + static_cast<std::int64_t>(expected.value()) * 1000));
  record.document = document;
  return store.commit(std::move(record), token);
}

std::string flip_byte(const std::string& bytes, std::size_t offset) {
  std::string out = bytes;
  if (offset < out.size()) {
    out[offset] = static_cast<char>(static_cast<unsigned char>(out[offset]) ^ 0x40u);
  }
  return out;
}

std::vector<std::filesystem::path> record_files(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> out;
  for (const auto& entry : std::filesystem::directory_iterator(directory / "records")) {
    if (entry.path().extension() == ".rec") {
      out.push_back(entry.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

IFABRIC_TEST(store, fresh_store_has_no_committed_generation) {
  ifabric_test::TempDirectory directory("store-fresh");
  auto store = IntentStore::create(directory.path(), core_domain());
  REQUIRE(store.ok());
  CHECK(!store.value()->has_committed());
  CHECK_EQ(store.value()->head_generation().is_genesis(), true);
  CHECK_ERROR_CODE(store.value()->head().error(), ErrorCode::NoCommittedGeneration);
  CHECK(!store.value()->recovered());
}

IFABRIC_TEST(store, create_refuses_to_overwrite_an_existing_store) {
  ifabric_test::TempDirectory directory("store-existing");
  auto first = IntentStore::create(directory.path(), core_domain());
  REQUIRE(first.ok());
  first.value().reset();
  CHECK_ERROR_CODE(IntentStore::create(directory.path(), core_domain()).error(), ErrorCode::StoreBusy);
}

IFABRIC_TEST(store, commit_and_reload_preserves_the_generation) {
  ifabric_test::TempDirectory directory("store-roundtrip");
  const IntentDocument document = document_for(1);
  const auto expected_content = content_digest_of(document);
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    const auto reference = commit_next(*store.value(), document, "actor.writer",
                                       GenerationNumber::genesis());
    REQUIRE(reference.ok());
    CHECK_EQ(reference->number.value(), 1ull);
    CHECK_EQ(reference->content.hex(), expected_content.hex());
  }
  {
    auto store = IntentStore::open(directory.path());
    REQUIRE(store.ok());
    CHECK(store.value()->has_committed());
    CHECK_EQ(store.value()->head_generation().value(), 1ull);
    CHECK_EQ(store.value()->head_content().hex(), expected_content.hex());
    const auto record = store.value()->load_head();
    REQUIRE(record.ok());
    CHECK_EQ(content_digest_of(record->document).hex(), expected_content.hex());
    CHECK(!store.value()->recovered());
    CHECK(store.value()->verify_all().ok());
  }
}

IFABRIC_TEST(store, restart_preserves_the_whole_lineage) {
  ifabric_test::TempDirectory directory("store-lineage");
  std::vector<std::string> digests;
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    GenerationNumber expected = GenerationNumber::genesis();
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
      const IntentDocument document = document_for(seed);
      digests.push_back(content_digest_of(document).hex());
      const auto reference = commit_next(*store.value(), document, "actor.writer", expected);
      REQUIRE(reference.ok());
      expected = reference->number;
    }
  }
  for (int restart = 0; restart < 2; ++restart) {
    auto store = IntentStore::open(directory.path());
    REQUIRE(store.ok());
    CHECK_EQ(store.value()->head_generation().value(), 5ull);
    const auto summaries = store.value()->list(0);
    REQUIRE(summaries.ok());
    CHECK_EQ(summaries->size(), std::size_t{5});
    for (std::size_t i = 0; i < summaries->size(); ++i) {
      const std::size_t generation = summaries->size() - i;
      CHECK_EQ(summaries.value()[i].number.value(), static_cast<std::uint64_t>(generation));
      CHECK_EQ(summaries.value()[i].content.hex(), digests[generation - 1]);
      const auto record = store.value()->load(summaries.value()[i].number);
      REQUIRE(record.ok());
      CHECK_EQ(content_digest_of(record->document).hex(), digests[generation - 1]);
    }
    CHECK(store.value()->verify_all().ok());
    CHECK_EQ(store.value()->verify_all().chain_links_checked, std::size_t{4});
  }
}

IFABRIC_TEST(store, epoch_advances_on_every_open) {
  ifabric_test::TempDirectory directory("store-epoch");
  Epoch first;
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    first = store.value()->epoch();
    REQUIRE(commit_next(*store.value(), document_for(1), "actor.writer",
                        GenerationNumber::genesis())
                .ok());
  }
  auto store = IntentStore::open(directory.path());
  REQUIRE(store.ok());
  CHECK_EQ(store.value()->epoch().value(), first.value() + 1);
  CHECK_EQ(store.value()->incarnation().empty(), false);
}

IFABRIC_TEST(store, stale_writer_cannot_overwrite_a_newer_generation) {
  ifabric_test::TempDirectory directory("store-stale");
  auto store = IntentStore::create(directory.path(), core_domain());
  REQUIRE(store.ok());
  REQUIRE(commit_next(*store.value(), document_for(1), "actor.writer",
                      GenerationNumber::genesis())
              .ok());
  // A writer that still believes the head is genesis is fenced out.
  const auto late = commit_next(*store.value(), document_for(2), "actor.writer",
                                GenerationNumber::genesis());
  CHECK_ERROR_CODE(late.error(), ErrorCode::StaleWriter);
  CHECK_EQ(store.value()->head_generation().value(), 1ull);
  const auto record = store.value()->load_head();
  REQUIRE(record.ok());
  CHECK_EQ(content_digest_of(record->document).hex(), content_digest_of(document_for(1)).hex());
}

IFABRIC_TEST(store, stale_epoch_is_fenced) {
  ifabric_test::TempDirectory directory("store-stale-epoch");
  auto store = IntentStore::create(directory.path(), core_domain());
  REQUIRE(store.ok());
  FenceToken stale;
  stale.writer = ActorId::parse("actor.writer").value();
  stale.expected_generation = GenerationNumber::genesis();
  stale.expected_epoch = Epoch(store.value()->epoch().value() + 5);
  stale.expected_incarnation = store.value()->incarnation();
  GenerationRecord record;
  record.domain = store.value()->domain();
  record.generation = GenerationNumber::from(1).value();
  record.writer = stale.writer;
  record.document = document_for(1);
  CHECK_ERROR_CODE(store.value()->commit(std::move(record), stale).error(), ErrorCode::StaleEpoch);

  FenceToken wrong_incarnation = stale;
  wrong_incarnation.expected_epoch = store.value()->epoch();
  wrong_incarnation.expected_incarnation = IncarnationId::generate();
  GenerationRecord second;
  second.domain = store.value()->domain();
  second.generation = GenerationNumber::from(1).value();
  second.writer = stale.writer;
  second.document = document_for(1);
  CHECK_ERROR_CODE(store.value()->commit(std::move(second), wrong_incarnation).error(),
                   ErrorCode::StaleEpoch);
  CHECK_EQ(store.value()->has_committed(), false);
}

IFABRIC_TEST(store, commit_must_name_the_fenced_writer) {
  ifabric_test::TempDirectory directory("store-writer");
  auto store = IntentStore::create(directory.path(), core_domain());
  REQUIRE(store.ok());
  FenceToken token = fence_for(*store.value(), "actor.writer", GenerationNumber::genesis());
  GenerationRecord record;
  record.domain = store.value()->domain();
  record.generation = GenerationNumber::from(1).value();
  record.writer = ActorId::parse("actor.someone-else").value();
  record.document = document_for(1);
  CHECK_ERROR_CODE(store.value()->commit(std::move(record), token).error(), ErrorCode::Unauthorized);
}

IFABRIC_TEST(store, commit_rejects_a_foreign_domain) {
  ifabric_test::TempDirectory directory("store-domain");
  auto store = IntentStore::create(directory.path(), core_domain());
  REQUIRE(store.ok());
  GenerationRecord record;
  record.domain = DomainId::parse("dom.other").value();
  record.generation = GenerationNumber::from(1).value();
  record.writer = ActorId::parse("actor.writer").value();
  record.document = document_for(1);
  CHECK_ERROR_CODE(
      store.value()->commit(std::move(record), fence_for(*store.value(), "actor.writer",
                                                         GenerationNumber::genesis())).error(),
      ErrorCode::UnknownDomain);
}

IFABRIC_TEST(store, generation_retention_bound_is_enforced) {
  ifabric_test::TempDirectory directory("store-bound");
  StoreOptions options;
  options.max_generations = 2;
  auto store = IntentStore::create(directory.path(), core_domain(), options);
  REQUIRE(store.ok());
  GenerationNumber expected = GenerationNumber::genesis();
  for (int i = 0; i < 2; ++i) {
    const auto reference = commit_next(*store.value(), document_for(static_cast<std::uint64_t>(i)),
                                       "actor.writer", expected);
    REQUIRE(reference.ok());
    expected = reference->number;
  }
  const auto rejected = commit_next(*store.value(), document_for(9), "actor.writer", expected);
  CHECK_ERROR_CODE(rejected.error(), ErrorCode::LimitExceeded);
  CHECK_EQ(store.value()->head_generation().value(), 2ull);
}

IFABRIC_TEST(store, payload_corruption_is_detected) {
  ifabric_test::TempDirectory directory("store-corrupt-payload");
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    REQUIRE(commit_next(*store.value(), document_for(1), "actor.writer",
                        GenerationNumber::genesis())
                .ok());
  }
  const auto files = record_files(directory.path());
  REQUIRE(files.size() == 1);
  const auto bytes = read_file(files.front(), 1u << 30);
  REQUIRE(bytes.ok());
  const std::string corrupted = flip_byte(bytes.value(), bytes.value().size() / 2);
  REQUIRE(write_file_atomic(files.front(), corrupted, WriteOptions{false, false}).ok());
  auto store = IntentStore::open(directory.path());
  CHECK_ERROR_CODE(store.error(), ErrorCode::StoreCorrupt);
}

IFABRIC_TEST(store, header_corruption_is_detected) {
  ifabric_test::TempDirectory directory("store-corrupt-header");
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    REQUIRE(commit_next(*store.value(), document_for(1), "actor.writer",
                        GenerationNumber::genesis())
                .ok());
  }
  const auto files = record_files(directory.path());
  REQUIRE(files.size() == 1);
  const auto bytes = read_file(files.front(), 1u << 30);
  REQUIRE(bytes.ok());
  REQUIRE(write_file_atomic(files.front(), flip_byte(bytes.value(), 20), WriteOptions{false, false})
              .ok());
  CHECK_ERROR_CODE(IntentStore::open(directory.path()).error(), ErrorCode::StoreCorrupt);
}

IFABRIC_TEST(store, truncated_record_is_detected) {
  ifabric_test::TempDirectory directory("store-truncate");
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    REQUIRE(commit_next(*store.value(), document_for(1), "actor.writer",
                        GenerationNumber::genesis())
                .ok());
  }
  const auto files = record_files(directory.path());
  REQUIRE(files.size() == 1);
  const auto bytes = read_file(files.front(), 1u << 30);
  REQUIRE(bytes.ok());
  REQUIRE(write_file_atomic(files.front(), bytes.value().substr(0, bytes.value().size() / 2),
                           WriteOptions{false, false})
              .ok());
  CHECK_ERROR_CODE(IntentStore::open(directory.path()).error(), ErrorCode::StoreCorrupt);
}

IFABRIC_TEST(store, envelope_that_lies_about_its_digest_is_detected) {
  ifabric_test::TempDirectory directory("store-lie");
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    REQUIRE(commit_next(*store.value(), document_for(1), "actor.writer",
                        GenerationNumber::genesis())
                .ok());
  }
  const auto files = record_files(directory.path());
  REQUIRE(files.size() == 1);
  const auto bytes = read_file(files.front(), 1u << 30);
  REQUIRE(bytes.ok());
  const std::string payload = bytes.value().substr(kRecordHeaderBytes);
  JsonValue envelope = parse_json(payload, JsonParseLimits{}).value();
  (void)envelope.set("content_digest", JsonValue::string(std::string(64, 'a')));
  std::string modified = bytes.value().substr(0, kRecordHeaderBytes);
  modified.append(envelope.dump());
  REQUIRE(write_file_atomic(files.front(), modified, WriteOptions{false, false}).ok());
  CHECK_ERROR_CODE(IntentStore::open(directory.path()).error(), ErrorCode::StoreCorrupt);
}

IFABRIC_TEST(store, head_pointer_corruption_recovers_from_the_backup) {
  ifabric_test::TempDirectory directory("store-head-recovery");
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    GenerationNumber expected = GenerationNumber::genesis();
    for (int i = 0; i < 2; ++i) {
      const auto reference = commit_next(*store.value(), document_for(static_cast<std::uint64_t>(i)),
                                         "actor.writer", expected);
      REQUIRE(reference.ok());
      expected = reference->number;
    }
  }
  const Path head_path = directory.child("head.json");
  const auto head_bytes = read_file(head_path, 1u << 20);
  REQUIRE(head_bytes.ok());
  REQUIRE(write_file_atomic(head_path, flip_byte(head_bytes.value(), 40), WriteOptions{false, false})
              .ok());
  auto store = IntentStore::open(directory.path());
  REQUIRE(store.ok());
  CHECK(store.value()->recovered());
  CHECK_EQ(store.value()->head_generation().value(), 1ull);
  CHECK_EQ(store.value()->recovery_note().empty(), false);
  // The recovered head pointer was rewritten, so the next open is clean.
  store.value().reset();
  auto reopened = IntentStore::open(directory.path());
  REQUIRE(reopened.ok());
  CHECK_EQ(reopened.value()->head_generation().value(), 1ull);
  CHECK_EQ(reopened.value()->verify_all().ok(), true);
}

IFABRIC_TEST(store, corrupt_head_and_backup_refuse_to_open) {
  ifabric_test::TempDirectory directory("store-head-dead");
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    REQUIRE(commit_next(*store.value(), document_for(1), "actor.writer",
                        GenerationNumber::genesis())
                .ok());
  }
  for (const char* name : {"head.json", "head.json.bak"}) {
    const Path path = directory.child(name);
    if (!file_exists(path)) {
      continue;
    }
    const auto bytes = read_file(path, 1u << 20);
    REQUIRE(bytes.ok());
    REQUIRE(write_file_atomic(path, flip_byte(bytes.value(), 30), WriteOptions{false, false}).ok());
  }
  const auto store = IntentStore::open(directory.path());
  CHECK_ERROR_CODE(store.error(), ErrorCode::StoreCorrupt);
}

IFABRIC_TEST(store, interrupted_commit_orphans_are_removed) {
  ifabric_test::TempDirectory directory("store-orphan");
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    REQUIRE(commit_next(*store.value(), document_for(1), "actor.writer",
                        GenerationNumber::genesis())
                .ok());
  }
  const Path orphan = directory.path() / "records" / "gen-0000000000000009-deadbeefdeadbeef.rec";
  REQUIRE(write_file_atomic(orphan, std::string(200, 'x'), WriteOptions{false, false}).ok());
  REQUIRE(file_exists(orphan));
  auto store = IntentStore::open(directory.path());
  REQUIRE(store.ok());
  CHECK(!file_exists(orphan));
  CHECK_EQ(store.value()->head_generation().value(), 1ull);
}

IFABRIC_TEST(store, pruned_generations_read_back_as_pruned) {
  ifabric_test::TempDirectory directory("store-prune");
  auto store = IntentStore::create(directory.path(), core_domain());
  REQUIRE(store.ok());
  GenerationNumber expected = GenerationNumber::genesis();
  for (int i = 0; i < 3; ++i) {
    const auto reference = commit_next(*store.value(), document_for(static_cast<std::uint64_t>(i)),
                                       "actor.writer", expected);
    REQUIRE(reference.ok());
    expected = reference->number;
  }
  REQUIRE(store.value()->prune_before(GenerationNumber::from(3).value()).ok());
  CHECK_ERROR_CODE(store.value()->load(GenerationNumber::from(1).value()).error(),
                   ErrorCode::GenerationPruned);
  CHECK(store.value()->load(GenerationNumber::from(3).value()).ok());
  CHECK_ERROR_CODE(store.value()->load(GenerationNumber::from(9).value()).error(),
                   ErrorCode::GenerationNotFound);
}

IFABRIC_TEST(store, only_one_writer_may_hold_the_store_lock) {
  ifabric_test::TempDirectory directory("store-lock");
  auto first = IntentStore::create(directory.path(), core_domain());
  REQUIRE(first.ok());
  CHECK_ERROR_CODE(IntentStore::open(directory.path()).error(), ErrorCode::StoreLockBusy);
  first.value().reset();
  CHECK(IntentStore::open(directory.path()).ok());
}

IFABRIC_TEST(store, missing_directory_and_descriptor_are_reported) {
  ifabric_test::TempDirectory directory("store-missing");
  CHECK_ERROR_CODE(IntentStore::open(directory.child("absent")).error(), ErrorCode::StoreNotFound);
  REQUIRE(ensure_directory(directory.child("empty")).ok());
  CHECK_ERROR_CODE(IntentStore::open(directory.child("empty")).error(), ErrorCode::StoreNotFound);
}

IFABRIC_TEST(store, unsupported_format_version_is_rejected) {
  ifabric_test::TempDirectory directory("store-format");
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
  }
  const Path descriptor = directory.child("store.json");
  JsonValue json = parse_json(read_file(descriptor, 1u << 20).value(), JsonParseLimits{}).value();
  (void)json.set("format_version", JsonValue::integer(99));
  REQUIRE(write_file_atomic(descriptor, json.dump(), WriteOptions{false, false}).ok());
  CHECK_ERROR_CODE(IntentStore::open(directory.path()).error(), ErrorCode::StoreFormatUnsupported);
}

IFABRIC_TEST(store, verification_reports_the_full_lineage) {
  ifabric_test::TempDirectory directory("store-verify");
  auto store = IntentStore::create(directory.path(), core_domain());
  REQUIRE(store.ok());
  GenerationNumber expected = GenerationNumber::genesis();
  for (int i = 0; i < 4; ++i) {
    const auto reference = commit_next(*store.value(), document_for(static_cast<std::uint64_t>(i)),
                                       "actor.writer", expected);
    REQUIRE(reference.ok());
    expected = reference->number;
  }
  const VerificationReport report = store.value()->verify_all();
  CHECK(report.ok());
  CHECK_EQ(report.generations_checked, std::size_t{4});
  CHECK_EQ(report.chain_links_checked, std::size_t{3});
  CHECK(report.bytes_checked > 0);
  CHECK_EQ(report.head_generation.value(), 4ull);
}

IFABRIC_TEST(store, repeated_open_close_cycles_stay_consistent) {
  ifabric_test::TempDirectory directory("store-cycles");
  {
    auto store = IntentStore::create(directory.path(), core_domain());
    REQUIRE(store.ok());
    REQUIRE(commit_next(*store.value(), document_for(1), "actor.writer",
                        GenerationNumber::genesis())
                .ok());
  }
  for (int cycle = 0; cycle < 6; ++cycle) {
    auto store = IntentStore::open(directory.path());
    REQUIRE(store.ok());
    CHECK_EQ(store.value()->head_generation().value(), 1ull);
    CHECK(store.value()->verify_all().ok());
  }
}

IFABRIC_TEST(store, lineage_journal_records_every_commit) {
  ifabric_test::TempDirectory directory("store-lineage-log");
  auto store = IntentStore::create(directory.path(), core_domain());
  REQUIRE(store.ok());
  GenerationNumber expected = GenerationNumber::genesis();
  for (int i = 0; i < 3; ++i) {
    const auto reference = commit_next(*store.value(), document_for(static_cast<std::uint64_t>(i)),
                                       "actor.writer", expected);
    REQUIRE(reference.ok());
    expected = reference->number;
  }
  const auto journal = read_file(directory.child("lineage.log"), 1u << 20);
  REQUIRE(journal.ok());
  std::size_t lines = 0;
  for (char c : journal.value()) {
    if (c == '\n') {
      ++lines;
    }
  }
  CHECK_EQ(lines, std::size_t{3});
  CHECK(journal.value().find("\"event\":\"commit\"") != std::string::npos);
}
