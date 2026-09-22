// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deliberate attempts to break the runtime: partial writes, truncated and
// tampered serialization, restart in the middle of a mutation, cross-process
// lock contention, cancellation racing a commit, repeated start/stop cycles,
// invalid sizes and resource bounds.

#include "ifabric/net.hpp"
#include "ifabric/runtime.hpp"
#include "ifabric/synth.hpp"
#include "ifabric_test.hpp"
#include "process.hpp"

#include <atomic>
#include <thread>

using namespace ifabric;

namespace {

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

IntentDocument document_for(std::uint64_t seed) {
  SynthOptions options;
  options.seed = seed;
  options.leaves = 2 + static_cast<std::size_t>(seed % 3);
  IntentDocument document = synthesize(core_domain(), options);
  document.provenance.description = "hardening generation " + std::to_string(seed);
  return document;
}

ActorId writer(std::string_view name) { return ActorId::parse(std::string(name)).value(); }

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

Error commit_once(IntentRuntime& runtime, const IntentDocument& document,
                  const std::string& actor) {
  const ActorId id = writer(actor);
  const auto outcome = runtime.submit(document, id, runtime.fence(id));
  return outcome.ok() ? Error() : outcome.error();
}

}  // namespace

IFABRIC_TEST(hardening, head_pointer_cannot_escape_the_store_directory) {
  ifabric_test::TempDirectory directory("hardening-escape");
  const std::filesystem::path store_path = directory.child("store");
  {
    auto store = IntentStore::create(store_path, core_domain());
    REQUIRE(store.ok());
    GenerationRecord record;
    record.domain = core_domain();
    record.generation = GenerationNumber::from(1).value();
    record.writer = writer("actor.hardening");
    record.document = document_for(1);
    FenceToken token;
    token.writer = record.writer;
    token.expected_generation = GenerationNumber::genesis();
    token.expected_epoch = store.value()->epoch();
    token.expected_incarnation = store.value()->incarnation();
    REQUIRE(store.value()->commit(std::move(record), token).ok());
  }
  const Path head_path = store_path / "head.json";
  JsonValue head = parse_json(read_file(head_path, 1u << 20).value(), JsonParseLimits{}).value();
  (void)head.set("record_file", JsonValue::string("../../../etc/passwd"));
  (void)head.erase("head_digest");
  (void)head.set("head_digest", JsonValue::string(Sha256::hash(head.dump()).hex()));
  REQUIRE(write_file_atomic(head_path, head.dump(), WriteOptions{false, false}).ok());
  CHECK_ERROR_CODE(IntentStore::open(store_path).error(), ErrorCode::StoreCorrupt);
}

IFABRIC_TEST(hardening, head_pointer_with_a_future_format_version_is_refused) {
  ifabric_test::TempDirectory directory("hardening-head-format");
  const std::filesystem::path store_path = directory.child("store");
  {
    auto store = IntentStore::create(store_path, core_domain());
    REQUIRE(store.ok());
    GenerationRecord record;
    record.domain = core_domain();
    record.generation = GenerationNumber::from(1).value();
    record.writer = writer("actor.hardening");
    record.document = document_for(1);
    FenceToken token;
    token.writer = record.writer;
    token.expected_generation = GenerationNumber::genesis();
    token.expected_epoch = store.value()->epoch();
    token.expected_incarnation = store.value()->incarnation();
    REQUIRE(store.value()->commit(std::move(record), token).ok());
  }
  const Path head_path = store_path / "head.json";
  JsonValue head = parse_json(read_file(head_path, 1u << 20).value(), JsonParseLimits{}).value();
  (void)head.set("format_version", JsonValue::integer(99));
  REQUIRE(write_file_atomic(head_path, head.dump(), WriteOptions{false, false}).ok());
  // Either head pointer candidates failed, so the store refuses to open. The
  // specific reason is preserved in the error detail for an operator.
  const Error refused = IntentStore::open(store_path).error();
  CHECK_EQ(refused.code, ErrorCode::StoreCorrupt);
  CHECK(refused.detail.find("StoreFormatUnsupported") != std::string::npos);
}

IFABRIC_TEST(hardening, record_with_a_lying_header_length_is_refused) {
  ifabric_test::TempDirectory directory("hardening-record-length");
  const std::filesystem::path store_path = directory.child("store");
  {
    auto runtime = IntentRuntime::create(store_path, core_domain());
    REQUIRE(runtime.ok());
    REQUIRE(commit_once(*runtime.value(), document_for(1), "actor.hardening").ok());
  }
  const auto files = record_files(store_path);
  REQUIRE(files.size() == 1);
  auto bytes = read_file(files.front(), 1u << 30).value();
  const std::uint64_t huge = 1ull << 40;
  for (int i = 0; i < 8; ++i) {
    bytes[24 + i] = static_cast<char>((huge >> (i * 8)) & 0xFFu);
  }
  const std::uint32_t header_crc = crc32(std::string_view(bytes.data(), 124));
  for (int i = 0; i < 4; ++i) {
    bytes[124 + i] = static_cast<char>((header_crc >> (i * 8)) & 0xFFu);
  }
  REQUIRE(write_file_atomic(files.front(), bytes, WriteOptions{false, false}).ok());
  CHECK_ERROR_CODE(IntentStore::open(store_path).error(), ErrorCode::StoreCorrupt);
}

IFABRIC_TEST(hardening, record_that_is_not_an_envelope_is_refused) {
  ifabric_test::TempDirectory directory("hardening-envelope");
  const std::filesystem::path store_path = directory.child("store");
  {
    auto runtime = IntentRuntime::create(store_path, core_domain());
    REQUIRE(runtime.ok());
    REQUIRE(commit_once(*runtime.value(), document_for(1), "actor.hardening").ok());
  }
  const auto files = record_files(store_path);
  REQUIRE(files.size() == 1);
  auto bytes = read_file(files.front(), 1u << 30).value();
  const std::string replacement = "{\"hello\":\"world\"}";
  bytes.resize(kRecordHeaderBytes);
  const std::uint64_t payload_bytes = replacement.size();
  for (int i = 0; i < 8; ++i) {
    bytes[24 + i] = static_cast<char>((payload_bytes >> (i * 8)) & 0xFFu);
  }
  const std::uint32_t payload_crc = crc32(replacement);
  for (int i = 0; i < 4; ++i) {
    bytes[32 + i] = static_cast<char>((payload_crc >> (i * 8)) & 0xFFu);
  }
  const Digest digest = Sha256::hash(replacement);
  for (std::size_t i = 0; i < digest.bytes().size(); ++i) {
    bytes[40 + i] = static_cast<char>(digest.bytes()[i]);
  }
  const std::uint32_t header_crc = crc32(std::string_view(bytes.data(), 124));
  for (int i = 0; i < 4; ++i) {
    bytes[124 + i] = static_cast<char>((header_crc >> (i * 8)) & 0xFFu);
  }
  bytes.append(replacement);
  REQUIRE(write_file_atomic(files.front(), bytes, WriteOptions{false, false}).ok());
  CHECK_ERROR_CODE(IntentStore::open(store_path).error(), ErrorCode::StoreCorrupt);
}

IFABRIC_TEST(hardening, record_deleted_after_the_head_was_replaced_recovers_backwards) {
  ifabric_test::TempDirectory directory("hardening-interrupted");
  const std::filesystem::path store_path = directory.child("store");
  {
    auto runtime = IntentRuntime::create(store_path, core_domain());
    REQUIRE(runtime.ok());
    REQUIRE(commit_once(*runtime.value(), document_for(1), "actor.hardening").ok());
    REQUIRE(commit_once(*runtime.value(), document_for(2), "actor.hardening").ok());
  }
  // Simulate a crash between writing the record and replacing the head pointer:
  // the newest record vanishes while the head pointer backup still names the
  // previous generation.
  const auto files = record_files(store_path);
  REQUIRE(files.size() == 2);
  const Path head_path = store_path / "head.json";
  auto head_bytes = read_file(head_path, 1u << 20).value();
  head_bytes[30] = static_cast<char>(head_bytes[30] ^ 0x11);
  REQUIRE(write_file_atomic(head_path, head_bytes, WriteOptions{false, false}).ok());
  REQUIRE(remove_file(files.back()).ok());
  auto runtime = IntentRuntime::open(store_path);
  REQUIRE(runtime.ok());
  CHECK(runtime.value()->recovered());
  CHECK_EQ(runtime.value()->head().value().number.value(), 1ull);
  CHECK(runtime.value()->verify_store().ok());
}

IFABRIC_TEST(hardening, second_process_cannot_take_the_store_lock) {
  ifabric_test::TempDirectory directory("hardening-lock");
  const std::string store_path = directory.child("store").string();
  std::uint16_t port = 0;
  {
    const auto listener = net::listen_tcp("127.0.0.1", 0, port);
    REQUIRE(listener.ok());
    (void)net::close_socket(listener.value());
  }
  REQUIRE(port != 0);
  {
    auto created = IntentRuntime::create(store_path, core_domain());
    REQUIRE(created.ok());
  }
  auto daemon = ifabric_test::spawn_process(
      IFABRICD_PATH, {"--store", store_path, "--port", std::to_string(port)},
      directory.child("daemon.log"));
  REQUIRE(daemon.ok());
  REQUIRE(ifabric_test::wait_for_port("127.0.0.1", port));
  const auto contender =
      ifabric_test::run_process(IFABRIC_CLI_PATH, {"generations", "--store", store_path});
  CHECK_EQ(contender.exit_code, 1);
  CHECK(contender.output.find("StoreLockBusy") != std::string::npos);
  daemon.value().terminate();
  (void)daemon.value().wait();
  const auto after =
      ifabric_test::run_process(IFABRIC_CLI_PATH, {"generations", "--store", store_path});
  CHECK_EQ(after.exit_code, 0);
}

IFABRIC_TEST(hardening, cancellation_racing_commit_never_publishes_twice) {
  ifabric_test::TempDirectory directory("hardening-cancel-race");
  auto runtime = IntentRuntime::create(directory.path(), core_domain());
  REQUIRE(runtime.ok());
  const ActorId actor = writer("actor.hardening");
  std::size_t committed = 0;
  std::size_t cancelled = 0;
  for (int round = 0; round < 40; ++round) {
    const FenceToken token = runtime.value()->fence(actor);
    const auto proposal =
        runtime.value()->propose(document_for(static_cast<std::uint64_t>(round) + 1), actor);
    REQUIRE(proposal.ok());
    REQUIRE(runtime.value()->validate_proposal(proposal.value()).ok());
    std::atomic<bool> cancel_won{false};
    std::atomic<bool> commit_won{false};
    std::thread committer([&] {
      if (runtime.value()->commit_proposal(proposal.value(), token).ok()) {
        commit_won.store(true);
      }
    });
    std::thread canceller([&] {
      if (runtime.value()->cancel_proposal(proposal.value(), "racing cancellation").ok()) {
        cancel_won.store(true);
      }
    });
    committer.join();
    canceller.join();
    if (commit_won.load()) {
      ++committed;
      CHECK_EQ(cancel_won.load(), false);
      const auto inspected = runtime.value()->inspect_proposal(proposal.value());
      REQUIRE(inspected.ok());
      CHECK_EQ(inspected->state, ProposalState::Committed);
    } else {
      ++cancelled;
      CHECK(cancel_won.load());
    }
  }
  CHECK_EQ(committed + cancelled, std::size_t{40});
  if (committed == 0) {
    // Every cancellation won: the domain still has no authoritative generation.
    CHECK_ERROR_CODE(runtime.value()->head().error(), ErrorCode::NoCommittedGeneration);
  } else {
    CHECK_EQ(runtime.value()->head().value().number.value(),
             static_cast<std::uint64_t>(committed));
  }
  CHECK_EQ(runtime.value()->generations(0).value().size(), committed);
  CHECK(runtime.value()->verify_store().ok());
}

IFABRIC_TEST(hardening, repeated_open_commit_close_cycles_stay_consistent) {
  ifabric_test::TempDirectory directory("hardening-cycles");
  for (int cycle = 0; cycle < 16; ++cycle) {
    auto runtime = IntentRuntime::open(directory.path());
    if (!runtime) {
      auto created = IntentRuntime::create(directory.path(), core_domain());
      REQUIRE(created.ok());
      runtime = std::move(created);
    }
    REQUIRE(commit_once(*runtime.value(), document_for(static_cast<std::uint64_t>(cycle) + 1),
                        "actor.hardening")
                .ok());
    const VerificationReport report = runtime.value()->verify_store();
    CHECK(report.ok());
    CHECK_EQ(report.generations_checked, static_cast<std::size_t>(cycle + 1));
    runtime.value().reset();
  }
  auto final = IntentRuntime::open(directory.path());
  REQUIRE(final.ok());
  CHECK_EQ(final.value()->head().value().number.value(), 16ull);
  CHECK(final.value()->verify_store().ok());
}

IFABRIC_TEST(hardening, repeated_server_start_stop_is_clean) {
  ifabric_test::TempDirectory directory("hardening-server-cycles");
  auto runtime = IntentRuntime::create(directory.child("store"), core_domain());
  REQUIRE(runtime.ok());
  for (int cycle = 0; cycle < 8; ++cycle) {
    net::ServerOptions options;
    options.worker_threads = 2;
    auto server = net::IntentServer::start(*runtime.value(), "127.0.0.1", 0, options);
    REQUIRE(server.ok());
    const std::uint16_t port = server.value()->port();
    std::thread accept([&] { (void)server.value()->run(); });
    net::ClientOptions client_options;
    client_options.actor = writer("actor.hardening");
    auto client = net::IntentClient::connect("127.0.0.1", port, client_options);
    REQUIRE(client.ok());
    const auto head = client.value()->get_head();
    CHECK_ERROR_CODE(head.error(), ErrorCode::NoCommittedGeneration);
    (void)client.value()->close();
    REQUIRE(server.value()->shutdown().ok());
    accept.join();
    CHECK_EQ(server.value()->stats().connections_accepted, 1ull);
  }
}

IFABRIC_TEST(hardening, out_of_range_numbers_are_refused) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  JsonValue json = to_json(document);
  JsonValue& port = (*json.find_mut("topology")->find_mut("ports")->array_mut())[0];
  (void)port.set("speed_bps", JsonValue::integer(-1));
  CHECK_ERROR_CODE(from_json(json).error(), ErrorCode::JsonNumberOutOfRange);
  CHECK_ERROR_CODE(parse_json("18446744073709551615", JsonParseLimits{}).error(),
                   ErrorCode::JsonNumberOutOfRange);
}

IFABRIC_TEST(hardening, oversized_identity_names_are_refused) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  JsonValue json = to_json(document);
  JsonValue& device = (*json.find_mut("topology")->find_mut("devices")->array_mut())[0];
  (void)device.set("id", JsonValue::string("dev." + std::string(400, 'x')));
  CHECK_ERROR_CODE(from_json(json).error(), ErrorCode::IdentityMalformed);
}

IFABRIC_TEST(hardening, non_ascii_identity_names_are_refused) {
  IntentDocument document = synthesize(core_domain(), SynthOptions{});
  JsonValue json = to_json(document);
  JsonValue& device = (*json.find_mut("topology")->find_mut("devices")->array_mut())[0];
  (void)device.set("id", JsonValue::string("dev.leaf\xC3\xA9"));
  CHECK_ERROR_CODE(from_json(json).error(), ErrorCode::IdentityMalformed);
}

IFABRIC_TEST(hardening, commit_after_shutdown_is_refused_and_leaves_no_generation) {
  ifabric_test::TempDirectory directory("hardening-shutdown");
  auto runtime = IntentRuntime::create(directory.path(), core_domain());
  REQUIRE(runtime.ok());
  const ActorId actor = writer("actor.hardening");
  const auto proposal = runtime.value()->propose(document_for(1), actor);
  REQUIRE(proposal.ok());
  REQUIRE(runtime.value()->validate_proposal(proposal.value()).ok());
  const FenceToken token = runtime.value()->fence(actor);
  runtime.value()->request_shutdown();
  const auto outcome = runtime.value()->commit_proposal(proposal.value(), token);
  CHECK_ERROR_CODE(outcome.error(), ErrorCode::Cancelled);
  CHECK(!runtime.value()->has_committed());
  runtime.value().reset();
  auto reopened = IntentRuntime::open(directory.path());
  REQUIRE(reopened.ok());
  CHECK(!reopened.value()->has_committed());
}

IFABRIC_TEST(hardening, garbage_files_in_the_record_directory_are_ignored) {
  ifabric_test::TempDirectory directory("hardening-garbage");
  const std::filesystem::path store_path = directory.child("store");
  {
    auto runtime = IntentRuntime::create(store_path, core_domain());
    REQUIRE(runtime.ok());
    REQUIRE(commit_once(*runtime.value(), document_for(1), "actor.hardening").ok());
  }
  REQUIRE(write_file_atomic(store_path / "records" / "notes.txt", "hello",
                            WriteOptions{false, false})
              .ok());
  REQUIRE(write_file_atomic(store_path / "records" / "gen-notanumber-x.rec", "junk",
                            WriteOptions{false, false})
              .ok());
  auto runtime = IntentRuntime::open(store_path);
  REQUIRE(runtime.ok());
  CHECK_EQ(runtime.value()->head().value().number.value(), 1ull);
  CHECK(runtime.value()->verify_store().ok());
}

IFABRIC_TEST(hardening, empty_record_file_is_reported_not_ignored) {
  ifabric_test::TempDirectory directory("hardening-empty-record");
  const std::filesystem::path store_path = directory.child("store");
  {
    auto runtime = IntentRuntime::create(store_path, core_domain());
    REQUIRE(runtime.ok());
    REQUIRE(commit_once(*runtime.value(), document_for(1), "actor.hardening").ok());
  }
  const auto files = record_files(store_path);
  REQUIRE(files.size() == 1);
  REQUIRE(write_file_atomic(files.front(), std::string(), WriteOptions{false, false}).ok());
  CHECK_ERROR_CODE(IntentStore::open(store_path).error(), ErrorCode::StoreCorrupt);
}

IFABRIC_TEST(hardening, subscriber_limit_is_enforced_and_released) {
  ifabric_test::TempDirectory directory("hardening-subscribers");
  IntentRuntime::Options options;
  options.max_subscribers = 3;
  auto runtime = IntentRuntime::create(directory.path(), core_domain(), options);
  REQUIRE(runtime.ok());
  std::vector<std::uint64_t> ids;
  for (int i = 0; i < 3; ++i) {
    const auto id = runtime.value()->subscribe();
    REQUIRE(id.ok());
    ids.push_back(id.value());
  }
  CHECK_ERROR_CODE(runtime.value()->subscribe().error(), ErrorCode::ServerBusy);
  REQUIRE(runtime.value()->unsubscribe(ids.front()).ok());
  CHECK(runtime.value()->subscribe().ok());
}

IFABRIC_TEST(hardening, proposal_identities_are_unique_under_load) {
  ifabric_test::TempDirectory directory("hardening-proposal-ids");
  auto runtime = IntentRuntime::create(directory.path(), core_domain());
  REQUIRE(runtime.ok());
  std::vector<std::string> ids;
  for (int i = 0; i < 200; ++i) {
    const auto id = runtime.value()->propose(document_for(static_cast<std::uint64_t>(i) + 1),
                                             writer("actor.hardening"));
    REQUIRE(id.ok());
    ids.push_back(id.value().str());
  }
  std::sort(ids.begin(), ids.end());
  CHECK_EQ(std::unique(ids.begin(), ids.end()), ids.end());
}
