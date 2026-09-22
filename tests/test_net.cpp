// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Transport tests. The in-process server still uses real TCP loopback sockets;
// the process tests start and kill actual ifabricd processes.

#include "ifabric/net.hpp"
#include "ifabric/synth.hpp"
#include "ifabric_test.hpp"
#include "process.hpp"

#include <iostream>
#include <thread>

using namespace ifabric;
using namespace ifabric::net;

namespace {

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

IntentDocument document_for(std::uint64_t seed) {
  SynthOptions options;
  options.seed = seed;
  options.leaves = 2 + static_cast<std::size_t>(seed % 3);
  IntentDocument document = synthesize(core_domain(), options);
  document.provenance.description = "network generation " + std::to_string(seed);
  return document;
}

std::uint16_t pick_free_port() {
  std::uint16_t bound = 0;
  const auto listener = listen_tcp("127.0.0.1", 0, bound);
  if (listener) {
    (void)close_socket(listener.value());
  }
  return bound;
}

struct ServerFixture {
  ifabric_test::TempDirectory directory;
  std::unique_ptr<IntentRuntime> runtime;
  std::unique_ptr<IntentServer> server;
  std::thread accept_thread;

  explicit ServerFixture(const std::string& label) : directory(label) {
    auto opened = IntentRuntime::create(directory.path(), core_domain());
    if (!opened) {
      return;
    }
    runtime = std::move(opened.value());
    ServerOptions options;
    options.worker_threads = 2;
    auto started = IntentServer::start(*runtime, "127.0.0.1", 0, options);
    if (!started) {
      return;
    }
    server = std::move(started.value());
    accept_thread = std::thread([this] { (void)server->run(); });
  }

  ~ServerFixture() {
    if (server) {
      (void)server->shutdown();
    }
    if (accept_thread.joinable()) {
      accept_thread.join();
    }
  }

  ServerFixture(const ServerFixture&) = delete;
  ServerFixture& operator=(const ServerFixture&) = delete;
};

ActorId writer(std::string_view name) { return ActorId::parse(std::string(name)).value(); }

}  // namespace

IFABRIC_TEST(net, frame_round_trip_and_integrity) {
  Frame frame;
  frame.type = MessageType::Propose;
  frame.request_id = 42;
  frame.flags = 1;
  frame.payload = "{\"a\":1}";
  const auto encoded = frame_encode(frame);
  REQUIRE(encoded.ok());
  const auto decoded = frame_decode(encoded.value());
  REQUIRE(decoded.ok());
  CHECK_EQ(decoded->request_id, 42ull);
  CHECK_EQ(decoded->type, MessageType::Propose);
  CHECK_EQ(decoded->payload, std::string("{\"a\":1}"));

  std::string corrupted = encoded.value();
  corrupted[corrupted.size() - 1] = static_cast<char>(corrupted.back() ^ 0x20);
  CHECK_ERROR_CODE(frame_decode(corrupted).error(), ErrorCode::ProtocolError);

  std::string bad_header = encoded.value();
  bad_header[20] = static_cast<char>(bad_header[20] ^ 0x01);
  CHECK_ERROR_CODE(frame_decode(bad_header).error(), ErrorCode::ProtocolError);

  std::string bad_magic = encoded.value();
  bad_magic[0] = 'X';
  CHECK_ERROR_CODE(frame_decode(bad_magic).error(), ErrorCode::ProtocolError);

  CHECK_ERROR_CODE(frame_decode(encoded.value().substr(0, 10)).error(), ErrorCode::ProtocolError);
  CHECK_ERROR_CODE(frame_decode(encoded.value() + "extra").error(), ErrorCode::ProtocolError);
}

IFABRIC_TEST(net, frame_payload_bound_is_enforced) {
  Frame frame;
  frame.type = MessageType::Propose;
  frame.payload.assign(kMaxFrameBytes + 1, 'x');
  CHECK_ERROR_CODE(frame_encode(frame).error(), ErrorCode::FrameTooLarge);
}

IFABRIC_TEST(net, client_server_end_to_end_over_loopback) {
  ServerFixture fixture("net-loopback");
  REQUIRE(fixture.server != nullptr);
  ClientOptions options;
  options.actor = writer("actor.remote");
  auto client = IntentClient::connect("127.0.0.1", fixture.server->port(), options);
  REQUIRE(client.ok());
  const auto hello = client.value()->handshake();
  REQUIRE(hello.ok());
  CHECK(hello->find("session") != nullptr);
  CHECK(hello->find("domain") != nullptr);

  const auto empty_head = client.value()->get_head();
  CHECK_ERROR_CODE(empty_head.error(), ErrorCode::NoCommittedGeneration);

  FenceToken token;
  token.writer = options.actor;
  token.expected_generation = GenerationNumber::genesis();
  token.expected_epoch = client.value()->session_epoch();
  token.expected_incarnation = client.value()->session_incarnation();
  const auto outcome = client.value()->submit(document_for(1), token);
  REQUIRE(outcome.ok());
  CHECK_EQ(*outcome->find("head")->try_string(), std::string("1"));

  const auto head = client.value()->get_head();
  REQUIRE(head.ok());
  CHECK_EQ(*head->find("head")->try_string(), std::string("1"));

  const auto stats = client.value()->stats();
  REQUIRE(stats.ok());
  CHECK(stats->find("runtime") != nullptr);

  const auto verification = client.value()->call(MessageType::Verify, JsonValue());
  REQUIRE(verification.ok());
  (void)client.value()->close();
}

IFABRIC_TEST(net, stale_expected_generation_is_rejected_over_the_wire) {
  ServerFixture fixture("net-stale");
  REQUIRE(fixture.server != nullptr);
  ClientOptions options;
  options.actor = writer("actor.remote");
  auto first = IntentClient::connect("127.0.0.1", fixture.server->port(), options);
  auto second = IntentClient::connect("127.0.0.1", fixture.server->port(), options);
  REQUIRE(first.ok());
  REQUIRE(second.ok());
  FenceToken token;
  token.writer = options.actor;
  token.expected_generation = GenerationNumber::genesis();
  token.expected_epoch = first.value()->session_epoch();
  token.expected_incarnation = first.value()->session_incarnation();
  REQUIRE(first.value()->submit(document_for(1), token).ok());
  const auto late = second.value()->submit(document_for(2), token);
  CHECK_ERROR_CODE(late.error(), ErrorCode::StaleGeneration);
  CHECK_EQ(fixture.runtime->head().value().number.value(), 1ull);
}

IFABRIC_TEST(net, unhandled_first_frame_is_rejected) {
  ServerFixture fixture("net-handshake");
  REQUIRE(fixture.server != nullptr);
  const auto socket = connect_tcp("127.0.0.1", fixture.server->port(), 2000);
  REQUIRE(socket.ok());
  Frame frame;
  frame.type = MessageType::GetHead;
  frame.request_id = 1;
  frame.payload = "{}";
  REQUIRE(send_frame(socket.value(), frame).ok());
  const auto response = receive_frame(socket.value());
  REQUIRE(response.ok());
  CHECK_EQ(response->type, MessageType::Error);
  const auto payload = parse_json(response->payload, JsonParseLimits{});
  REQUIRE(payload.ok());
  CHECK_EQ(*payload->find("code")->try_string(), std::string("Unauthorized"));
  (void)close_socket(socket.value());
}

IFABRIC_TEST(net, wrong_protocol_version_is_rejected) {
  ServerFixture fixture("net-protocol");
  REQUIRE(fixture.server != nullptr);
  const auto socket = connect_tcp("127.0.0.1", fixture.server->port(), 2000);
  REQUIRE(socket.ok());
  JsonValue body;
  (void)body.set("protocol", JsonValue::integer(99));
  (void)body.set("actor", JsonValue::string("actor.remote"));
  Frame frame;
  frame.type = MessageType::Hello;
  frame.request_id = 7;
  frame.payload = body.dump();
  REQUIRE(send_frame(socket.value(), frame).ok());
  const auto response = receive_frame(socket.value());
  REQUIRE(response.ok());
  CHECK_EQ(response->type, MessageType::Error);
  (void)close_socket(socket.value());
}

IFABRIC_TEST(net, oversized_declared_frame_is_refused_before_allocation) {
  ServerFixture fixture("net-oversize");
  REQUIRE(fixture.server != nullptr);
  const auto socket = connect_tcp("127.0.0.1", fixture.server->port(), 2000);
  REQUIRE(socket.ok());
  Frame frame;
  frame.type = MessageType::Hello;
  frame.request_id = 3;
  frame.payload = "{}";
  auto encoded = frame_encode(frame);
  REQUIRE(encoded.ok());
  // Patch the declared payload length beyond the frame bound.
  std::string hostile = encoded.value();
  const std::uint32_t huge = static_cast<std::uint32_t>(kMaxFrameBytes + 1);
  for (int i = 0; i < 4; ++i) {
    hostile[24 + i] = static_cast<char>((huge >> (i * 8)) & 0xFFu);
  }
  // Recompute the header CRC so that only the length check can reject it.
  const std::uint32_t crc = crc32(std::string_view(hostile.data(), kFrameHeaderBytes - 4));
  for (int i = 0; i < 4; ++i) {
    hostile[kFrameHeaderBytes - 4 + static_cast<std::size_t>(i)] =
        static_cast<char>((crc >> (i * 8)) & 0xFFu);
  }
  CHECK_ERROR_CODE(frame_decode(hostile).error(), ErrorCode::FrameTooLarge);
  Frame header_only;
  (void)close_socket(socket.value());
}

IFABRIC_TEST(net, shutdown_is_real_and_idempotent) {
  ifabric_test::TempDirectory directory("net-shutdown");
  auto runtime = IntentRuntime::create(directory.path(), core_domain());
  REQUIRE(runtime.ok());
  ServerOptions options;
  options.worker_threads = 2;
  auto server = IntentServer::start(*runtime.value(), "127.0.0.1", 0, options);
  REQUIRE(server.ok());
  std::thread accept([&] { (void)server.value()->run(); });
  ClientOptions client_options;
  client_options.actor = writer("actor.remote");
  auto client = IntentClient::connect("127.0.0.1", server.value()->port(), client_options);
  REQUIRE(client.ok());
  REQUIRE(server.value()->shutdown().ok());
  REQUIRE(server.value()->shutdown().ok());
  accept.join();
  CHECK_EQ(server.value()->stats().connections_accepted, 1ull);
}

// ---------------------------------------------------------------------------
// Independent process tests
// ---------------------------------------------------------------------------
IFABRIC_TEST(net, daemon_survives_kill_and_restart_with_fresh_incarnation_fencing) {
  ifabric_test::TempDirectory directory("net-daemon");
  const std::string store_path = directory.child("store").string();
  const std::uint16_t port = pick_free_port();
  REQUIRE(port != 0);

  const IntentDocument first = document_for(1);
  const IntentDocument second = document_for(2);

  // The daemon serves an existing store; create it here and release the lock
  // before the first process starts.
  {
    auto created = IntentRuntime::create(store_path, core_domain());
    REQUIRE(created.ok());
  }

  Epoch first_epoch;
  IncarnationId first_incarnation;
  std::string first_proposal_text;
  {
    auto spawned = ifabric_test::spawn_process(IFABRICD_PATH,
                                       {"--store", store_path, "--port", std::to_string(port)},
                                       directory.child("daemon-1.log"));
    REQUIRE(spawned.ok());
    ifabric_test::ChildProcess daemon = std::move(spawned.value());
    REQUIRE(ifabric_test::wait_for_port("127.0.0.1", port));
    ClientOptions options;
    options.actor = writer("actor.remote");
    auto client = IntentClient::connect("127.0.0.1", port, options);
    REQUIRE(client.ok());
    first_epoch = client.value()->session_epoch();
    first_incarnation = client.value()->session_incarnation();
    FenceToken token;
    token.writer = options.actor;
    token.expected_generation = GenerationNumber::genesis();
    token.expected_epoch = first_epoch;
    token.expected_incarnation = first_incarnation;
    const auto outcome = client.value()->submit(first, token);
    if (!outcome) {
      std::cout << outcome.error().render() << "\n" << daemon.output() << "\n";
    }
    REQUIRE(outcome.ok());
    // Capture a proposal identity that must not survive the restart.
    JsonValue propose_body;
    (void)propose_body.set("document", to_json(second));
    (void)propose_body.set("actor", JsonValue::string(options.actor.str()));
    const auto proposed = client.value()->call(MessageType::Propose, propose_body);
    REQUIRE(proposed.ok());
    first_proposal_text = *proposed->find("proposal")->try_string();
    // Hard kill: no graceful shutdown, no flush, no cleanup.
    daemon.terminate();
    (void)daemon.wait();
  }

  {
    auto spawned = ifabric_test::spawn_process(IFABRICD_PATH,
                                       {"--store", store_path, "--port", std::to_string(port)},
                                       directory.child("daemon-2.log"));
    REQUIRE(spawned.ok());
    ifabric_test::ChildProcess daemon = std::move(spawned.value());
    REQUIRE(ifabric_test::wait_for_port("127.0.0.1", port));
    ClientOptions options;
    options.actor = writer("actor.remote");
    auto client = IntentClient::connect("127.0.0.1", port, options);
    REQUIRE(client.ok());
    CHECK(client.value()->session_epoch().value() > first_epoch.value());
    CHECK_NE(client.value()->session_incarnation().str(), first_incarnation.str());

    const auto head = client.value()->get_head();
    REQUIRE(head.ok());
    CHECK_EQ(*head->find("head")->try_string(), std::string("1"));

    const auto verification = client.value()->call(MessageType::Verify, JsonValue());
    REQUIRE(verification.ok());
    CHECK_EQ(*verification->find("verification")->find("ok")->try_bool(), true);

    // A proposal from the previous incarnation is gone.
    JsonValue validate_body;
    (void)validate_body.set("proposal", JsonValue::string(first_proposal_text));
    CHECK_ERROR_CODE(client.value()->call(MessageType::Validate, validate_body).error(),
                     ErrorCode::ProposalNotFound);

    // The restarted process fences a writer that still believes the head is
    // genesis, even though the fence carries a fresh session binding.
    FenceToken stale;
    stale.writer = options.actor;
    stale.expected_generation = GenerationNumber::genesis();
    stale.expected_epoch = client.value()->session_epoch();
    stale.expected_incarnation = client.value()->session_incarnation();
    CHECK_ERROR_CODE(client.value()->submit(second, stale).error(), ErrorCode::StaleGeneration);

    FenceToken current;
    current.writer = options.actor;
    current.expected_generation = GenerationNumber::from(1).value();
    current.expected_epoch = client.value()->session_epoch();
    current.expected_incarnation = client.value()->session_incarnation();
    const auto outcome = client.value()->submit(second, current);
    REQUIRE(outcome.ok());
    CHECK_EQ(*outcome->find("head")->try_string(), std::string("2"));

    JsonValue shutdown_body;
    const auto stopped = client.value()->call(MessageType::Shutdown, shutdown_body);
    REQUIRE(stopped.ok());
    CHECK_EQ(client.value()->session_epoch().value() > first_epoch.value(), true);
    (void)client.value()->close();
    (void)daemon.wait();
  }
}
