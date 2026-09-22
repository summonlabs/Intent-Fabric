// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Framed transport over real TCP sockets, plus the ifabricd request/response
// protocol. Every frame carries a length bound, a CRC-32 and a SHA-256 so a
// truncated, corrupted or oversized frame is rejected before it is interpreted.
//
// Distributed behaviour claimed by this repository (independent processes,
// stale-writer fencing across a restart, fresh-incarnation fencing) is proven
// through this transport in the integration tests; nothing here is a mock.

#ifndef IFABRIC_NET_HPP
#define IFABRIC_NET_HPP

#include "ifabric/runtime.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace ifabric::net {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderBytes = 72;
inline constexpr std::uint16_t kDefaultPort = 7920;

enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  Error = 3,
  GetHead = 10,
  HeadResult = 11,
  GetGeneration = 12,
  GenerationResult = 13,
  ListGenerations = 14,
  GenerationListResult = 15,
  Propose = 20,
  ProposeResult = 21,
  Validate = 22,
  ValidateResult = 23,
  Commit = 24,
  CommitResult = 25,
  Cancel = 26,
  CancelResult = 27,
  Diff = 28,
  DiffResult = 29,
  Stats = 30,
  StatsResult = 31,
  Subscribe = 32,
  SubscribeResult = 33,
  Poll = 34,
  PollResult = 35,
  Verify = 36,
  VerifyResult = 37,
  Shutdown = 40,
  ShutdownResult = 41
};

std::string_view to_string(MessageType type) noexcept;

struct Frame {
  MessageType type = MessageType::Hello;
  std::uint32_t flags = 0;
  std::uint64_t request_id = 0;
  std::string payload;
};

// Cross-platform socket handle.
#if defined(_WIN32)
using SocketHandle = std::uintptr_t;
inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~std::uintptr_t{0});
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

Error platform_init();
void platform_shutdown();
Error close_socket(SocketHandle handle) noexcept;
Error set_no_delay(SocketHandle handle) noexcept;

// A listening socket bound to host:port. Passing port 0 binds an ephemeral
// port; the resolved port is reported back.
Result<SocketHandle> listen_tcp(const std::string& host, std::uint16_t port,
                                std::uint16_t& bound_port);
Result<SocketHandle> connect_tcp(const std::string& host, std::uint16_t port,
                                 std::uint32_t timeout_ms);
// Waits up to timeout_ms for an inbound connection; returns kInvalidSocket on
// timeout so that an accept loop can observe a stop flag.
Result<SocketHandle> accept_tcp(SocketHandle listener, std::uint32_t timeout_ms);

Result<std::string> frame_encode(const Frame& frame);
Result<Frame> frame_decode(const std::string& bytes);

Error send_frame(SocketHandle handle, const Frame& frame);
Result<Frame> receive_frame(SocketHandle handle);

struct ServerStats {
  std::uint64_t connections_accepted = 0;
  std::uint64_t connections_rejected = 0;
  std::uint64_t requests_served = 0;
  std::uint64_t protocol_errors = 0;
  std::uint64_t frames_rejected = 0;
  std::uint64_t stale_fences = 0;

  JsonValue to_json() const;
};

struct ServerOptions {
  std::size_t worker_threads = 4;
  std::size_t max_connections = kMaxConnections;
  std::size_t max_pending_connections = 64;
  std::uint32_t accept_poll_ms = 50;
};

class IntentServer {
 public:
  ~IntentServer();
  IntentServer(const IntentServer&) = delete;
  IntentServer& operator=(const IntentServer&) = delete;

  static Result<std::unique_ptr<IntentServer>> start(IntentRuntime& runtime,
                                                     const std::string& host,
                                                     std::uint16_t port,
                                                     const ServerOptions& options = {});

  std::uint16_t port() const noexcept { return port_; }
  std::string endpoint() const;
  ServerStats stats() const;
  void request_stop();
  bool stopping() const noexcept { return stopping_.load(); }
  // Runs the accept loop and worker pool until the session is stopped. Returns
  // once every worker thread has been joined.
  Error run();
  // Stops and joins; safe to call more than once.
  Error shutdown();

 private:
  IntentServer() = default;

  struct Connection {
    SocketHandle socket = kInvalidSocket;
    std::atomic<bool> closed{false};
  };

  void worker_loop();
  Error serve_connection(const std::shared_ptr<Connection>& connection);
  void finish();

  IntentRuntime* runtime_ = nullptr;
  ServerOptions options_;
  SocketHandle listener_ = kInvalidSocket;
  std::uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> joined_{false};
  mutable std::mutex mutex_;
  std::condition_variable available_;
  std::deque<std::shared_ptr<Connection>> queue_;
  std::vector<std::shared_ptr<Connection>> active_;
  std::vector<std::thread> workers_;
  std::mutex shutdown_mutex_;
  bool finished_ = false;
  mutable std::mutex stats_mutex_;
  ServerStats stats_;
};

struct ClientOptions {
  std::uint32_t connect_timeout_ms = 5000;
  std::string client_name = "ifabric-client";
  ActorId actor;
};

class IntentClient {
 public:
  ~IntentClient();
  IntentClient(const IntentClient&) = delete;
  IntentClient& operator=(const IntentClient&) = delete;

  static Result<std::unique_ptr<IntentClient>> connect(const std::string& host, std::uint16_t port,
                                                       const ClientOptions& options = {});

  // Sends a request and waits for the matching response. Errors reported by the
  // server are returned as Error with a machine-readable code.
  Result<JsonValue> call(MessageType type, const JsonValue& request);
  Result<JsonValue> handshake();
  Error close();

  const JsonValue& hello() const noexcept { return hello_; }
  Epoch session_epoch() const noexcept { return session_epoch_; }
  const IncarnationId& session_incarnation() const noexcept { return session_incarnation_; }

  // Convenience helpers used by the CLI and the process-level tests.
  Result<JsonValue> get_head();
  Result<JsonValue> submit(IntentDocument document, const FenceToken& fence);
  Result<JsonValue> commit(const ProposalId& proposal, const FenceToken& fence);
  Result<JsonValue> stats();

 private:
  IntentClient() = default;
  Result<JsonValue> request(MessageType type, JsonValue payload, MessageType expected);

  SocketHandle socket_ = kInvalidSocket;
  std::uint64_t next_request_id_ = 1;
  JsonValue hello_;
  Epoch session_epoch_;
  IncarnationId session_incarnation_;
};

}  // namespace ifabric::net

#endif  // IFABRIC_NET_HPP
