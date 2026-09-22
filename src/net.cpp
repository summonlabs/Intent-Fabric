// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/net.hpp"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ifabric::net {

namespace {

constexpr std::array<std::uint8_t, 8> kFrameMagic = {'I', 'F', 'A', 'B', 'F', 'R', 'M', '\n'};

void put_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

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

std::uint16_t get_u16(const std::string& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset])) |
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8));
}

std::uint32_t get_u32(const std::string& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 3])) << 24);
}

std::uint64_t get_u64(const std::string& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(
                 static_cast<std::uint8_t>(bytes[offset + static_cast<std::size_t>(i)]))
             << (i * 8);
  }
  return value;
}

Error send_all(SocketHandle handle, const char* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const int chunk = static_cast<int>(std::min<std::size_t>(size - offset, 1u << 20));
#if defined(_WIN32)
    const int sent = ::send(static_cast<SOCKET>(handle), data + offset, chunk, 0);
#else
    const ssize_t sent = ::send(handle, data + offset, static_cast<std::size_t>(chunk), 0);
#endif
    if (sent <= 0) {
      return Error(ErrorCode::NetworkError, "socket send failed", std::to_string(sent));
    }
    offset += static_cast<std::size_t>(sent);
  }
  return Error();
}

Result<std::size_t> receive_some(SocketHandle handle, char* data, std::size_t size) {
  if (size == 0) {
    return std::size_t{0};
  }
  const int chunk = static_cast<int>(std::min<std::size_t>(size, 1u << 20));
#if defined(_WIN32)
  const int received = ::recv(static_cast<SOCKET>(handle), data, chunk, 0);
#else
  const ssize_t received = ::recv(handle, data, static_cast<std::size_t>(chunk), 0);
#endif
  if (received == 0) {
    return Error(ErrorCode::NetworkError, "peer closed the connection");
  }
  if (received < 0) {
    return Error(ErrorCode::NetworkError, "socket receive failed");
  }
  return static_cast<std::size_t>(received);
}

Result<std::string> receive_exactly(SocketHandle handle, std::size_t size) {
  std::string out;
  out.resize(size);
  std::size_t offset = 0;
  while (offset < size) {
    const auto received = receive_some(handle, out.data() + offset, size - offset);
    if (!received) {
      return received.error();
    }
    offset += received.value();
  }
  return out;
}

Error resolve(const std::string& host, std::uint16_t port, addrinfo** out) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  if (host.empty()) {
    hints.ai_flags = AI_PASSIVE;
  }
  const std::string service = std::to_string(port);
  const int status = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, out);
  if (status != 0) {
    return Error(ErrorCode::NetworkError, "cannot resolve address", host + ":" + service);
  }
  return Error();
}

Error describe_last_socket_error(const char* what) {
#if defined(_WIN32)
  return Error(ErrorCode::NetworkError, what, std::to_string(::WSAGetLastError()));
#else
  return Error(ErrorCode::NetworkError, what, std::strerror(errno));
#endif
}

Result<JsonValue> error_json(const Error& error) {
  JsonValue out;
  (void)out.set("code", JsonValue::string(std::string(to_string(error.code))));
  (void)out.set("message", JsonValue::string(error.message));
  (void)out.set("detail", JsonValue::string(error.detail));
  return out;
}

Error decode_error(const JsonValue& payload) {
  const JsonValue* code = payload.find("code");
  const JsonValue* message = payload.find("message");
  const JsonValue* detail = payload.find("detail");
  ErrorCode parsed = ErrorCode::Internal;
  for (std::uint16_t value = 0; value <= static_cast<std::uint16_t>(ErrorCode::Unauthorized); ++value) {
    if (to_string(static_cast<ErrorCode>(value)) ==
        (code != nullptr && code->try_string() != nullptr ? *code->try_string() : std::string())) {
      parsed = static_cast<ErrorCode>(value);
      break;
    }
  }
  return Error(parsed,
               message != nullptr && message->try_string() != nullptr ? *message->try_string()
                                                                     : std::string("server error"),
               detail != nullptr && detail->try_string() != nullptr ? *detail->try_string()
                                                                    : std::string());
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello: return "hello";
    case MessageType::HelloAck: return "hello-ack";
    case MessageType::Error: return "error";
    case MessageType::GetHead: return "get-head";
    case MessageType::HeadResult: return "head-result";
    case MessageType::GetGeneration: return "get-generation";
    case MessageType::GenerationResult: return "generation-result";
    case MessageType::ListGenerations: return "list-generations";
    case MessageType::GenerationListResult: return "generation-list";
    case MessageType::Propose: return "propose";
    case MessageType::ProposeResult: return "propose-result";
    case MessageType::Validate: return "validate";
    case MessageType::ValidateResult: return "validate-result";
    case MessageType::Commit: return "commit";
    case MessageType::CommitResult: return "commit-result";
    case MessageType::Cancel: return "cancel";
    case MessageType::CancelResult: return "cancel-result";
    case MessageType::Diff: return "diff";
    case MessageType::DiffResult: return "diff-result";
    case MessageType::Stats: return "stats";
    case MessageType::StatsResult: return "stats-result";
    case MessageType::Subscribe: return "subscribe";
    case MessageType::SubscribeResult: return "subscribe-result";
    case MessageType::Poll: return "poll";
    case MessageType::PollResult: return "poll-result";
    case MessageType::Verify: return "verify";
    case MessageType::VerifyResult: return "verify-result";
    case MessageType::Shutdown: return "shutdown";
    case MessageType::ShutdownResult: return "shutdown-result";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------
Result<std::string> frame_encode(const Frame& frame) {
  if (frame.payload.size() > kMaxFrameBytes) {
    return Error(ErrorCode::FrameTooLarge, "payload exceeds the frame bound",
                 std::to_string(frame.payload.size()));
  }
  std::string out;
  out.reserve(kFrameHeaderBytes + frame.payload.size());
  out.append(reinterpret_cast<const char*>(kFrameMagic.data()), kFrameMagic.size());
  put_u16(out, static_cast<std::uint16_t>(kProtocolVersion));
  put_u16(out, static_cast<std::uint16_t>(frame.type));
  put_u32(out, frame.flags);
  put_u64(out, frame.request_id);
  put_u32(out, static_cast<std::uint32_t>(frame.payload.size()));
  put_u32(out, crc32(frame.payload));
  const Digest digest = Sha256::hash(frame.payload);
  for (std::uint8_t byte : digest.bytes()) {
    out.push_back(static_cast<char>(byte));
  }
  put_u32(out, 0);
  put_u32(out, crc32(std::string_view(out.data(), kFrameHeaderBytes - 4)));
  out.append(frame.payload);
  return out;
}

Result<Frame> frame_decode(const std::string& bytes) {
  if (bytes.size() < kFrameHeaderBytes) {
    return Error(ErrorCode::ProtocolError, "frame is shorter than its header");
  }
  for (std::size_t i = 0; i < kFrameMagic.size(); ++i) {
    if (static_cast<std::uint8_t>(bytes[i]) != kFrameMagic[i]) {
      return Error(ErrorCode::ProtocolError, "frame magic is wrong");
    }
  }
  const std::uint32_t header_crc = get_u32(bytes, kFrameHeaderBytes - 4);
  if (header_crc != crc32(std::string_view(bytes.data(), kFrameHeaderBytes - 4))) {
    return Error(ErrorCode::ProtocolError, "frame header CRC does not match");
  }
  const std::uint16_t version = get_u16(bytes, 8);
  if (version != static_cast<std::uint16_t>(kProtocolVersion)) {
    return Error(ErrorCode::ProtocolError, "unsupported protocol version",
                 std::to_string(version));
  }
  Frame frame;
  frame.type = static_cast<MessageType>(get_u16(bytes, 10));
  frame.flags = get_u32(bytes, 12);
  frame.request_id = get_u64(bytes, 16);
  const std::uint32_t payload_bytes = get_u32(bytes, 24);
  if (payload_bytes > kMaxFrameBytes) {
    return Error(ErrorCode::FrameTooLarge, "declared payload exceeds the frame bound",
                 std::to_string(payload_bytes));
  }
  const auto expected = checked_add(static_cast<std::uint64_t>(kFrameHeaderBytes), payload_bytes);
  if (!expected || bytes.size() != expected.value()) {
    return Error(ErrorCode::ProtocolError, "frame length does not match its header");
  }
  frame.payload = bytes.substr(kFrameHeaderBytes);
  if (crc32(frame.payload) != get_u32(bytes, 28)) {
    return Error(ErrorCode::ProtocolError, "frame payload CRC does not match");
  }
  std::array<std::uint8_t, Digest::kBytes> stored{};
  for (std::size_t i = 0; i < stored.size(); ++i) {
    stored[i] = static_cast<std::uint8_t>(bytes[32 + i]);
  }
  if (Sha256::hash(frame.payload).bytes() != stored) {
    return Error(ErrorCode::ProtocolError, "frame payload digest does not match");
  }
  return frame;
}

Error send_frame(SocketHandle handle, const Frame& frame) {
  const auto encoded = frame_encode(frame);
  if (!encoded) {
    return encoded.error();
  }
  return send_all(handle, encoded.value().data(), encoded.value().size());
}

Result<Frame> receive_frame(SocketHandle handle) {
  const auto header = receive_exactly(handle, kFrameHeaderBytes);
  if (!header) {
    return header.error();
  }
  const std::uint32_t payload_bytes = get_u32(header.value(), 24);
  if (payload_bytes > kMaxFrameBytes) {
    return Error(ErrorCode::FrameTooLarge, "declared payload exceeds the frame bound",
                 std::to_string(payload_bytes));
  }
  const auto payload = receive_exactly(handle, payload_bytes);
  if (!payload) {
    return payload.error();
  }
  return frame_decode(header.value() + payload.value());
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------
Error platform_init() {
#if defined(_WIN32)
  static std::mutex mutex;
  static bool started = false;
  const std::lock_guard<std::mutex> guard(mutex);
  if (started) {
    return Error();
  }
  WSADATA data{};
  if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return Error(ErrorCode::NetworkError, "WSAStartup failed");
  }
  started = true;
#endif
  return Error();
}

void platform_shutdown() {
#if defined(_WIN32)
  ::WSACleanup();
#endif
}

Error close_socket(SocketHandle handle) noexcept {
  if (handle == kInvalidSocket) {
    return Error();
  }
#if defined(_WIN32)
  ::closesocket(static_cast<SOCKET>(handle));
#else
  ::close(handle);
#endif
  return Error();
}

Error set_no_delay(SocketHandle handle) noexcept {
  int one = 1;
#if defined(_WIN32)
  ::setsockopt(static_cast<SOCKET>(handle), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));
#else
  ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
  return Error();
}

Result<SocketHandle> listen_tcp(const std::string& host, std::uint16_t port,
                                std::uint16_t& bound_port) {
  const Error init = platform_init();
  if (!init.ok()) {
    return init;
  }
  addrinfo* addresses = nullptr;
  const Error resolved = resolve(host, port, &addresses);
  if (!resolved.ok()) {
    return resolved;
  }
  SocketHandle listener = kInvalidSocket;
  for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
    SocketHandle candidate = static_cast<SocketHandle>(
        ::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
    if (candidate == kInvalidSocket) {
      continue;
    }
    int one = 1;
#if defined(_WIN32)
    ::setsockopt(static_cast<SOCKET>(candidate), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
#if defined(_WIN32)
    if (::bind(static_cast<SOCKET>(candidate), address->ai_addr,
               static_cast<int>(address->ai_addrlen)) == 0) {
#else
    if (::bind(candidate, address->ai_addr, address->ai_addrlen) == 0) {
#endif
      listener = candidate;
      break;
    }
    (void)close_socket(candidate);
  }
  ::freeaddrinfo(addresses);
  if (listener == kInvalidSocket) {
    return describe_last_socket_error("cannot bind the listening socket");
  }
#if defined(_WIN32)
  if (::listen(static_cast<SOCKET>(listener), SOMAXCONN) != 0) {
#else
  if (::listen(listener, SOMAXCONN) != 0) {
#endif
    (void)close_socket(listener);
    return describe_last_socket_error("cannot listen on the bound socket");
  }
  sockaddr_storage bound{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(bound));
  if (::getsockname(static_cast<SOCKET>(listener), reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
#else
  socklen_t length = sizeof(bound);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
#endif
    (void)close_socket(listener);
    return describe_last_socket_error("cannot read the bound port");
  }
  if (bound.ss_family == AF_INET) {
    bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
  } else if (bound.ss_family == AF_INET6) {
    bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
  } else {
    bound_port = port;
  }
  return listener;
}

Result<SocketHandle> connect_tcp(const std::string& host, std::uint16_t port,
                                 std::uint32_t timeout_ms) {
  const Error init = platform_init();
  if (!init.ok()) {
    return init;
  }
  addrinfo* addresses = nullptr;
  const Error resolved = resolve(host, port, &addresses);
  if (!resolved.ok()) {
    return resolved;
  }
  SocketHandle connection = kInvalidSocket;
  for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
    SocketHandle candidate = static_cast<SocketHandle>(
        ::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
    if (candidate == kInvalidSocket) {
      continue;
    }
    // Non-blocking connect with a bounded wait.
#if defined(_WIN32)
    u_long nonblocking = 1;
    ::ioctlsocket(static_cast<SOCKET>(candidate), FIONBIO, &nonblocking);
    const int status = ::connect(static_cast<SOCKET>(candidate), address->ai_addr,
                                 static_cast<int>(address->ai_addrlen));
    const int last = ::WSAGetLastError();
    const bool in_progress = status != 0 && last == WSAEWOULDBLOCK;
#else
    const int flags = ::fcntl(candidate, F_GETFL, 0);
    (void)::fcntl(candidate, F_SETFL, flags | O_NONBLOCK);
    const int status = ::connect(candidate, address->ai_addr, address->ai_addrlen);
    const bool in_progress = status != 0 && errno == EINPROGRESS;
#endif
    bool connected = status == 0;
    if (!connected && in_progress) {
      fd_set write_set;
      FD_ZERO(&write_set);
#if defined(_WIN32)
      FD_SET(static_cast<SOCKET>(candidate), &write_set);
#else
      FD_SET(candidate, &write_set);
#endif
      timeval timeout{};
      timeout.tv_sec = static_cast<long>(timeout_ms / 1000u);
      timeout.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
#if defined(_WIN32)
      const int ready = ::select(0, nullptr, &write_set, nullptr, &timeout);
#else
      const int ready = ::select(candidate + 1, nullptr, &write_set, nullptr, &timeout);
#endif
      if (ready > 0) {
        int socket_error = 0;
#if defined(_WIN32)
        int length = static_cast<int>(sizeof(socket_error));
        (void)::getsockopt(static_cast<SOCKET>(candidate), SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&socket_error), &length);
#else
        socklen_t length = sizeof(socket_error);
        (void)::getsockopt(candidate, SOL_SOCKET, SO_ERROR, &socket_error, &length);
#endif
        connected = socket_error == 0;
      }
    }
    if (connected) {
#if defined(_WIN32)
      u_long blocking = 0;
      ::ioctlsocket(static_cast<SOCKET>(candidate), FIONBIO, &blocking);
#else
      const int flags2 = ::fcntl(candidate, F_GETFL, 0);
      (void)::fcntl(candidate, F_SETFL, flags2 & ~O_NONBLOCK);
#endif
      connection = candidate;
      break;
    }
    (void)close_socket(candidate);
  }
  ::freeaddrinfo(addresses);
  if (connection == kInvalidSocket) {
    return Error(ErrorCode::NetworkError, "cannot connect to the intent runtime",
                 host + ":" + std::to_string(port));
  }
  (void)set_no_delay(connection);
  return connection;
}

Result<SocketHandle> accept_tcp(SocketHandle listener, std::uint32_t timeout_ms) {
  fd_set read_set;
  FD_ZERO(&read_set);
#if defined(_WIN32)
  FD_SET(static_cast<SOCKET>(listener), &read_set);
#else
  FD_SET(listener, &read_set);
#endif
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000u);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
#if defined(_WIN32)
  const int ready = ::select(0, &read_set, nullptr, nullptr, &timeout);
#else
  const int ready = ::select(listener + 1, &read_set, nullptr, nullptr, &timeout);
#endif
  if (ready == 0) {
    return kInvalidSocket;
  }
  if (ready < 0) {
    return Error(ErrorCode::NetworkError, "select on the listening socket failed");
  }
  sockaddr_storage peer{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(peer));
  const SOCKET accepted =
      ::accept(static_cast<SOCKET>(listener), reinterpret_cast<sockaddr*>(&peer), &length);
  if (accepted == INVALID_SOCKET) {
    return Error(ErrorCode::NetworkError, "accept failed");
  }
  (void)set_no_delay(static_cast<SocketHandle>(accepted));
  return static_cast<SocketHandle>(accepted);
#else
  socklen_t length = sizeof(peer);
  const int accepted = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &length);
  if (accepted < 0) {
    return Error(ErrorCode::NetworkError, "accept failed");
  }
  (void)set_no_delay(accepted);
  return accepted;
#endif
}

JsonValue ServerStats::to_json() const {
  JsonValue out;
  (void)out.set("connections_accepted",
                JsonValue::integer(static_cast<std::int64_t>(connections_accepted)));
  (void)out.set("connections_rejected",
                JsonValue::integer(static_cast<std::int64_t>(connections_rejected)));
  (void)out.set("requests_served", JsonValue::integer(static_cast<std::int64_t>(requests_served)));
  (void)out.set("protocol_errors", JsonValue::integer(static_cast<std::int64_t>(protocol_errors)));
  (void)out.set("frames_rejected", JsonValue::integer(static_cast<std::int64_t>(frames_rejected)));
  (void)out.set("stale_fences", JsonValue::integer(static_cast<std::int64_t>(stale_fences)));
  return out;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
namespace {

struct Session {
  bool hello_done = false;
  Epoch epoch;
  IncarnationId incarnation;
  ActorId actor;
};

Error check_session(const JsonValue& request, const Session& session) {
  if (!session.hello_done) {
    return Error(ErrorCode::Unauthorized, "the first frame on a connection must be a hello");
  }
  const JsonValue* block = request.find("session");
  if (block == nullptr || !block->is_object()) {
    return Error(ErrorCode::Unauthorized, "request carries no session binding");
  }
  const JsonValue* epoch = block->find("epoch");
  const JsonValue* incarnation = block->find("incarnation");
  if (epoch == nullptr || incarnation == nullptr || epoch->try_string() == nullptr ||
      incarnation->try_string() == nullptr) {
    return Error(ErrorCode::Unauthorized, "request carries a malformed session binding");
  }
  if (*epoch->try_string() != session.epoch.str() ||
      *incarnation->try_string() != session.incarnation.str()) {
    return Error(ErrorCode::StaleEpoch,
                 "request carries a stale epoch or incarnation binding",
                 *epoch->try_string() + "/" + *incarnation->try_string());
  }
  return Error();
}

Result<std::uint64_t> require_u64(const JsonValue& request, const char* name) {
  const JsonValue* value = request.find(name);
  if (value == nullptr) {
    return Error(ErrorCode::InvalidArgument, std::string("missing member ") + name);
  }
  if (const auto* text = value->try_string()) {
    std::uint64_t out = 0;
    if (text->empty() || text->size() > 20) {
      return Error(ErrorCode::InvalidArgument, std::string("malformed member ") + name);
    }
    for (char c : *text) {
      if (c < '0' || c > '9') {
        return Error(ErrorCode::InvalidArgument, std::string("malformed member ") + name);
      }
      out = out * 10u + static_cast<std::uint64_t>(c - '0');
    }
    return out;
  }
  const auto number = value->try_int();
  if (!number.has_value() || number.value() < 0) {
    return Error(ErrorCode::InvalidArgument, std::string("malformed member ") + name);
  }
  return static_cast<std::uint64_t>(number.value());
}

Result<std::string> require_string(const JsonValue& request, const char* name) {
  const JsonValue* value = request.find(name);
  if (value == nullptr || value->try_string() == nullptr) {
    return Error(ErrorCode::InvalidArgument, std::string("missing or malformed member ") + name);
  }
  return *value->try_string();
}

JsonValue session_block(const Session& session) {
  JsonValue out;
  (void)out.set("epoch", JsonValue::string(session.epoch.str()));
  (void)out.set("incarnation", JsonValue::string(session.incarnation.str()));
  return out;
}

}  // namespace

IntentServer::~IntentServer() { (void)shutdown(); }

Result<std::unique_ptr<IntentServer>> IntentServer::start(IntentRuntime& runtime,
                                                          const std::string& host,
                                                          std::uint16_t port,
                                                          const ServerOptions& options) {
  auto server = std::unique_ptr<IntentServer>(new IntentServer());
  server->runtime_ = &runtime;
  server->options_ = options;
  std::uint16_t bound = port;
  const auto listener = listen_tcp(host, port, bound);
  if (!listener) {
    return listener.error();
  }
  server->listener_ = listener.value();
  server->port_ = bound;
  for (std::size_t i = 0; i < options.worker_threads; ++i) {
    server->workers_.emplace_back([raw = server.get()] { raw->worker_loop(); });
  }
  return server;
}

std::string IntentServer::endpoint() const { return "tcp://127.0.0.1:" + std::to_string(port_); }

ServerStats IntentServer::stats() const {
  const std::lock_guard<std::mutex> guard(stats_mutex_);
  return stats_;
}

void IntentServer::request_stop() { stopping_.store(true); }

void IntentServer::finish() {
  const std::lock_guard<std::mutex> guard(shutdown_mutex_);
  if (finished_) {
    return;
  }
  finished_ = true;
  stopping_.store(true);
  {
    const std::lock_guard<std::mutex> queue_guard(mutex_);
    for (const auto& connection : active_) {
      if (!connection->closed.load()) {
        (void)close_socket(connection->socket);
        connection->closed.store(true);
      }
    }
    queue_.clear();
  }
  available_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  (void)close_socket(listener_);
  listener_ = kInvalidSocket;
}

Error IntentServer::run() {
  if (listener_ == kInvalidSocket) {
    return Error(ErrorCode::NetworkError, "server was not started");
  }
  while (!stopping_.load()) {
    const auto accepted = accept_tcp(listener_, options_.accept_poll_ms);
    if (!accepted) {
      const std::lock_guard<std::mutex> guard(stats_mutex_);
      stats_.protocol_errors = saturating_add(stats_.protocol_errors, 1);
      continue;
    }
    if (accepted.value() == kInvalidSocket) {
      continue;
    }
    auto connection = std::make_shared<Connection>();
    connection->socket = accepted.value();
    bool rejected = false;
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      if (active_.size() >= options_.max_connections ||
          queue_.size() >= options_.max_pending_connections) {
        rejected = true;
      } else {
        active_.push_back(connection);
        queue_.push_back(connection);
      }
    }
    if (rejected) {
      (void)close_socket(connection->socket);
      connection->closed.store(true);
      const std::lock_guard<std::mutex> guard(stats_mutex_);
      stats_.connections_rejected = saturating_add(stats_.connections_rejected, 1);
      continue;
    }
    {
      const std::lock_guard<std::mutex> guard(stats_mutex_);
      stats_.connections_accepted = saturating_add(stats_.connections_accepted, 1);
    }
    available_.notify_one();
  }
  finish();
  return Error();
}

Error IntentServer::shutdown() {
  request_stop();
  finish();
  return Error();
}

void IntentServer::worker_loop() {
  for (;;) {
    std::shared_ptr<Connection> connection;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      available_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_.load()) {
          return;
        }
        continue;
      }
      connection = queue_.front();
      queue_.pop_front();
    }
    (void)serve_connection(connection);
    if (!connection->closed.exchange(true)) {
      (void)close_socket(connection->socket);
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    active_.erase(std::remove(active_.begin(), active_.end(), connection), active_.end());
  }
}

Error IntentServer::serve_connection(const std::shared_ptr<Connection>& connection) {
  Session session;
  for (;;) {
    if (stopping_.load()) {
      return Error();
    }
    const auto frame = receive_frame(connection->socket);
    if (!frame) {
      return frame.error();
    }
    const auto request = parse_json(frame->payload, JsonParseLimits{});
    if (!request) {
      JsonValue error_payload;
      (void)error_payload.set("code", JsonValue::string("ProtocolError"));
      (void)error_payload.set("message", JsonValue::string("request payload is not valid JSON"));
      (void)error_payload.set("detail", JsonValue::string(request.error().render()));
      Frame response;
      response.type = MessageType::Error;
      response.request_id = frame->request_id;
      response.payload = error_payload.dump();
      (void)send_frame(connection->socket, response);
      const std::lock_guard<std::mutex> guard(stats_mutex_);
      stats_.protocol_errors = saturating_add(stats_.protocol_errors, 1);
      continue;
    }
    JsonValue response_payload;
    MessageType response_type = MessageType::Error;
    Error failure;
    const JsonValue& body = request.value();
    switch (frame->type) {
      case MessageType::Hello: {
        const JsonValue* protocol = body.find("protocol");
        if (protocol == nullptr || !protocol->is_int() ||
            protocol->try_int().value() != static_cast<std::int64_t>(kProtocolVersion)) {
          failure = Error(ErrorCode::ProtocolError, "protocol version mismatch",
                          protocol != nullptr && protocol->is_int()
                              ? std::to_string(protocol->try_int().value())
                              : std::string("missing"));
          break;
        }
        const JsonValue* actor = body.find("actor");
        if (actor == nullptr || actor->try_string() == nullptr ||
            actor->try_string()->empty()) {
          failure = Error(ErrorCode::Unauthorized, "hello must name the acting writer");
          break;
        }
        const auto parsed_actor = ActorId::parse(*actor->try_string());
        if (!parsed_actor) {
          failure = parsed_actor.error();
          break;
        }
        session.actor = parsed_actor.value();
        session.epoch = runtime_->epoch();
        session.incarnation = runtime_->incarnation();
        session.hello_done = true;
        (void)response_payload.set("protocol",
                                   JsonValue::integer(static_cast<std::int64_t>(kProtocolVersion)));
        (void)response_payload.set("server", JsonValue::string(std::string(kProductName) + " " +
                                                               std::string(kVersionString)));
        (void)response_payload.set("domain", JsonValue::string(runtime_->domain().str()));
        (void)response_payload.set("session", session_block(session));
        (void)response_payload.set("recovered", JsonValue::boolean(runtime_->recovered()));
        if (runtime_->has_committed()) {
          const auto head = runtime_->head();
          if (head) {
            (void)response_payload.set("head", JsonValue::string(head->number.str()));
            (void)response_payload.set("content_digest", JsonValue::string(head->content.hex()));
          }
        } else {
          (void)response_payload.set("head", JsonValue::null());
        }
        response_type = MessageType::HelloAck;
        break;
      }
      case MessageType::Shutdown: {
        if (session.hello_done) {
          runtime_->request_shutdown();
          (void)response_payload.set("stopping", JsonValue::boolean(true));
          response_type = MessageType::ShutdownResult;
        } else {
          failure = Error(ErrorCode::Unauthorized, "handshake required before shutdown");
        }
        break;
      }
      default: {
        failure = check_session(body, session);
        if (!failure.ok()) {
          break;
        }
        switch (frame->type) {
          case MessageType::GetHead: {
            const auto head = runtime_->head();
            if (!head) {
              failure = head.error();
              break;
            }
            (void)response_payload.set("head", JsonValue::string(head->number.str()));
            (void)response_payload.set("content_digest", JsonValue::string(head->content.hex()));
            (void)response_payload.set("epoch", JsonValue::string(head->committed_epoch.str()));
            response_type = MessageType::HeadResult;
            break;
          }
          case MessageType::GetGeneration: {
            const auto number = require_u64(body, "generation");
            if (!number) {
              failure = number.error();
              break;
            }
            const auto parsed = GenerationNumber::from(number.value());
            if (!parsed) {
              failure = parsed.error();
              break;
            }
            const auto document = runtime_->document_at(parsed.value());
            if (!document) {
              failure = document.error();
              break;
            }
            (void)response_payload.set("generation",
                                       JsonValue::string(parsed.value().str()));
            (void)response_payload.set("document", to_json(document.value()));
            response_type = MessageType::GenerationResult;
            break;
          }
          case MessageType::ListGenerations: {
            std::size_t limit = 32;
            if (const auto wanted = require_u64(body, "limit"); wanted) {
              limit = static_cast<std::size_t>(std::min<std::uint64_t>(wanted.value(), 4096));
            }
            const auto summaries = runtime_->generations(limit);
            if (!summaries) {
              failure = summaries.error();
              break;
            }
            JsonArray items;
            for (const auto& summary : summaries.value()) {
              JsonValue entry;
              (void)entry.set("generation", JsonValue::string(summary.number.str()));
              (void)entry.set("content_digest", JsonValue::string(summary.content.hex()));
              (void)entry.set("document_digest", JsonValue::string(summary.document.hex()));
              (void)entry.set("committed_at",
                              JsonValue::string(format_timestamp(summary.committed_at)));
              (void)entry.set("writer", JsonValue::string(summary.writer.str()));
              (void)entry.set("record_bytes",
                              JsonValue::integer(static_cast<std::int64_t>(summary.record_bytes)));
              items.push_back(std::move(entry));
            }
            (void)response_payload.set("generations", JsonValue::array(std::move(items)));
            response_type = MessageType::GenerationListResult;
            break;
          }
          case MessageType::Propose: {
            const JsonValue* document = body.find("document");
            if (document == nullptr) {
              failure = Error(ErrorCode::InvalidArgument, "propose requires an intent document");
              break;
            }
            const auto parsed = from_json(*document);
            if (!parsed) {
              failure = parsed.error();
              break;
            }
            const auto actor = require_string(body, "actor");
            if (!actor) {
              failure = actor.error();
              break;
            }
            const auto parsed_actor = ActorId::parse(actor.value());
            if (!parsed_actor) {
              failure = parsed_actor.error();
              break;
            }
            if (parsed_actor.value() != session.actor) {
              failure = Error(ErrorCode::Unauthorized,
                              "proposal actor does not match the session writer");
              break;
            }
            const auto id = runtime_->propose(parsed.value(), parsed_actor.value());
            if (!id) {
              failure = id.error();
              break;
            }
            const auto report = runtime_->validate_proposal(id.value());
            (void)response_payload.set("proposal", JsonValue::string(id.value().str()));
            const auto inspected = runtime_->inspect_proposal(id.value());
            if (inspected) {
              (void)response_payload.set("state",
                                         JsonValue::string(std::string(to_string(inspected->state))));
              (void)response_payload.set("content_digest",
                                         JsonValue::string(inspected->content.hex()));
              (void)response_payload.set("validation", inspected->report.to_json());
              if (!inspected->rejection_reason.empty()) {
                (void)response_payload.set("rejection_reason",
                                           JsonValue::string(inspected->rejection_reason));
              }
            }
            response_type = MessageType::ProposeResult;
            if (!report) {
              failure = report.error();
              response_type = MessageType::Error;
              JsonValue detailed;
              (void)detailed.set("code", JsonValue::string(std::string(to_string(report.error().code))));
              (void)detailed.set("message", JsonValue::string(report.error().message));
              (void)detailed.set("detail", JsonValue::string(report.error().detail));
              const auto inspected2 = runtime_->inspect_proposal(id.value());
              if (inspected2) {
                (void)detailed.set("validation", inspected2->report.to_json());
              }
              (void)detailed.set("proposal", JsonValue::string(id.value().str()));
              (void)response_payload.set("proposal", JsonValue::string(id.value().str()));
              response_payload = detailed;
            }
            break;
          }
          case MessageType::Validate: {
            const auto proposal = require_string(body, "proposal");
            if (!proposal) {
              failure = proposal.error();
              break;
            }
            const auto parsed = ProposalId::parse(proposal.value());
            if (!parsed) {
              failure = parsed.error();
              break;
            }
            const auto report = runtime_->validate_proposal(parsed.value());
            if (!report) {
              failure = report.error();
              break;
            }
            (void)response_payload.set("validation", report.value().to_json());
            response_type = MessageType::ValidateResult;
            break;
          }
          case MessageType::Commit: {
            const auto proposal = require_string(body, "proposal");
            const auto writer = require_string(body, "writer");
            if (!proposal || !writer) {
              failure = proposal ? writer.error() : proposal.error();
              break;
            }
            const auto parsed_proposal = ProposalId::parse(proposal.value());
            const auto parsed_writer = ActorId::parse(writer.value());
            if (!parsed_proposal) {
              failure = parsed_proposal.error();
              break;
            }
            if (!parsed_writer) {
              failure = parsed_writer.error();
              break;
            }
            FenceToken token;
            token.writer = parsed_writer.value();
            token.expected_epoch = session.epoch;
            token.expected_incarnation = session.incarnation;
            if (const auto expected = require_u64(body, "expected_generation"); expected) {
              const auto parsed = GenerationNumber::from(expected.value());
              if (!parsed) {
                failure = parsed.error();
                break;
              }
              token.expected_generation = parsed.value();
            } else {
              failure = expected.error();
              break;
            }
            const auto outcome = runtime_->commit_proposal(parsed_proposal.value(), token);
            if (!outcome) {
              failure = outcome.error();
              const std::lock_guard<std::mutex> guard(stats_mutex_);
              if (failure.code == ErrorCode::StaleGeneration ||
                  failure.code == ErrorCode::StaleWriter ||
                  failure.code == ErrorCode::StaleEpoch) {
                stats_.stale_fences = saturating_add(stats_.stale_fences, 1);
              }
              break;
            }
            (void)response_payload.set("head", JsonValue::string(outcome->reference.number.str()));
            (void)response_payload.set("content_digest",
                                       JsonValue::string(outcome->reference.content.hex()));
            (void)response_payload.set("impact",
                                       JsonValue::string(std::string(to_string(outcome->impact))));
            (void)response_payload.set("diff", outcome->diff.to_json());
            response_type = MessageType::CommitResult;
            break;
          }
          case MessageType::Cancel: {
            const auto proposal = require_string(body, "proposal");
            if (!proposal) {
              failure = proposal.error();
              break;
            }
            const auto parsed = ProposalId::parse(proposal.value());
            if (!parsed) {
              failure = parsed.error();
              break;
            }
            std::string reason;
            if (const auto text = require_string(body, "reason"); text) {
              reason = text.value();
            }
            const Error cancelled = runtime_->cancel_proposal(parsed.value(), reason);
            if (!cancelled.ok()) {
              failure = cancelled;
              break;
            }
            (void)response_payload.set("cancelled", JsonValue::boolean(true));
            response_type = MessageType::CancelResult;
            break;
          }
          case MessageType::Diff: {
            const auto from = require_u64(body, "from");
            const auto to = require_u64(body, "to");
            if (!from || !to) {
              failure = from ? to.error() : from.error();
              break;
            }
            const auto from_number = GenerationNumber::from(from.value());
            const auto to_number = GenerationNumber::from(to.value());
            if (!from_number || !to_number) {
              failure = from_number ? to_number.error() : from_number.error();
              break;
            }
            const auto before = runtime_->document_at(from_number.value());
            const auto after = runtime_->document_at(to_number.value());
            if (!before) {
              failure = before.error();
              break;
            }
            if (!after) {
              failure = after.error();
              break;
            }
            (void)response_payload.set("diff",
                                       diff_documents(before.value(), after.value()).to_json());
            response_type = MessageType::DiffResult;
            break;
          }
          case MessageType::Stats: {
            (void)response_payload.set("runtime", runtime_->stats().to_json());
            (void)response_payload.set("server", stats().to_json());
            response_type = MessageType::StatsResult;
            break;
          }
          case MessageType::Subscribe: {
            const auto id = runtime_->subscribe();
            if (!id) {
              failure = id.error();
              break;
            }
            (void)response_payload.set("subscription", JsonValue::string(std::to_string(id.value())));
            response_type = MessageType::SubscribeResult;
            break;
          }
          case MessageType::Poll: {
            const auto subscription = require_u64(body, "subscription");
            if (!subscription) {
              failure = subscription.error();
              break;
            }
            const auto publication = runtime_->poll(subscription.value());
            if (!publication) {
              failure = publication.error();
              break;
            }
            if (!publication->has_value()) {
              (void)response_payload.set("publication", JsonValue::null());
            } else {
              JsonValue entry;
              (void)entry.set("generation",
                              JsonValue::string(publication->value().reference.number.str()));
              (void)entry.set("content_digest",
                              JsonValue::string(publication->value().reference.content.hex()));
              (void)entry.set("sequence",
                              JsonValue::string(std::to_string(publication->value().sequence)));
              (void)entry.set("dropped",
                              JsonValue::string(std::to_string(publication->value().dropped)));
              (void)response_payload.set("publication", std::move(entry));
            }
            response_type = MessageType::PollResult;
            break;
          }
          case MessageType::Verify: {
            (void)response_payload.set("verification", runtime_->verify_store().to_json());
            response_type = MessageType::VerifyResult;
            break;
          }
          default:
            failure = Error(ErrorCode::ProtocolError, "unsupported message type",
                            std::string(to_string(frame->type)));
            break;
        }
        break;
      }
    }
    Frame response;
    response.request_id = frame->request_id;
    if (failure.ok()) {
      response.type = response_type;
      response.payload = response_payload.dump();
    } else {
      const auto rendered = error_json(failure);
      response.type = MessageType::Error;
      response.payload = rendered.value().dump();
      const std::lock_guard<std::mutex> guard(stats_mutex_);
      stats_.protocol_errors = saturating_add(stats_.protocol_errors, 1);
    }
    const Error sent = send_frame(connection->socket, response);
    if (!sent.ok()) {
      return sent;
    }
    {
      const std::lock_guard<std::mutex> guard(stats_mutex_);
      stats_.requests_served = saturating_add(stats_.requests_served, 1);
    }
    if (frame->type == MessageType::Shutdown) {
      // A served shutdown request stops admission for the whole server; the
      // accept loop observes the flag and retires the worker pool.
      request_stop();
      return Error();
    }
  }
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
IntentClient::~IntentClient() { (void)close(); }

Result<std::unique_ptr<IntentClient>> IntentClient::connect(const std::string& host,
                                                            std::uint16_t port,
                                                            const ClientOptions& options) {
  auto socket = net::connect_tcp(host, port, options.connect_timeout_ms);
  if (!socket) {
    return socket.error();
  }
  auto client = std::unique_ptr<IntentClient>(new IntentClient());
  client->socket_ = socket.value();
  ClientOptions effective = options;
  if (effective.actor.empty()) {
    const auto actor = ActorId::from_local("ifabric-client");
    if (!actor) {
      return actor.error();
    }
    effective.actor = actor.value();
  }
  client->hello_ = JsonValue();
  {
    JsonValue request;
    (void)request.set("protocol", JsonValue::integer(static_cast<std::int64_t>(kProtocolVersion)));
    (void)request.set("client", JsonValue::string(effective.client_name));
    (void)request.set("actor", JsonValue::string(effective.actor.str()));
    const auto response = client->request(MessageType::Hello, std::move(request),
                                          MessageType::HelloAck);
    if (!response) {
      return response.error();
    }
    client->hello_ = response.value();
    const JsonValue* session = response->find("session");
    if (session == nullptr) {
      return Error(ErrorCode::ProtocolError, "server handshake carried no session binding");
    }
    const auto epoch = require_u64(*session, "epoch");
    const JsonValue* incarnation = session->find("incarnation");
    if (!epoch || incarnation == nullptr || incarnation->try_string() == nullptr) {
      return Error(ErrorCode::ProtocolError, "server handshake session binding is malformed");
    }
    const auto parsed_epoch = Epoch::from(epoch.value());
    const auto parsed_incarnation = IncarnationId::parse(*incarnation->try_string());
    if (!parsed_epoch || !parsed_incarnation) {
      return Error(ErrorCode::ProtocolError, "server handshake session binding is malformed");
    }
    client->session_epoch_ = parsed_epoch.value();
    client->session_incarnation_ = parsed_incarnation.value();
  }
  return client;
}

Result<JsonValue> IntentClient::handshake() { return hello_; }

Error IntentClient::close() {
  if (socket_ != kInvalidSocket) {
    (void)close_socket(socket_);
    socket_ = kInvalidSocket;
  }
  return Error();
}

Result<JsonValue> IntentClient::request(MessageType type, JsonValue payload, MessageType expected) {
  if (socket_ == kInvalidSocket) {
    return Error(ErrorCode::NetworkError, "client is not connected");
  }
  JsonValue body = std::move(payload);
  if (type != MessageType::Hello && type != MessageType::Shutdown) {
    JsonValue session;
    (void)session.set("epoch", JsonValue::string(session_epoch_.str()));
    (void)session.set("incarnation", JsonValue::string(session_incarnation_.str()));
    (void)body.set("session", std::move(session));
  }
  Frame frame;
  frame.type = type;
  frame.request_id = next_request_id_++;
  frame.payload = body.dump();
  const Error sent = send_frame(socket_, frame);
  if (!sent.ok()) {
    return sent;
  }
  const auto response = receive_frame(socket_);
  if (!response) {
    return response.error();
  }
  if (response->request_id != frame.request_id) {
    return Error(ErrorCode::ProtocolError, "response does not match the request identity",
                 std::to_string(response->request_id) + " != " + std::to_string(frame.request_id));
  }
  const auto decoded = parse_json(response->payload, JsonParseLimits{});
  if (!decoded) {
    return Error(ErrorCode::ProtocolError, "response payload is not valid JSON",
                 decoded.error().render());
  }
  if (response->type == MessageType::Error) {
    return decode_error(decoded.value());
  }
  if (response->type != expected) {
    return Error(ErrorCode::ProtocolError, "unexpected response type",
                 std::string(to_string(response->type)));
  }
  return decoded.value();
}

Result<JsonValue> IntentClient::call(MessageType type, const JsonValue& request_body) {
  switch (type) {
    case MessageType::GetHead: return request(type, request_body, MessageType::HeadResult);
    case MessageType::GetGeneration:
      return request(type, request_body, MessageType::GenerationResult);
    case MessageType::ListGenerations:
      return request(type, request_body, MessageType::GenerationListResult);
    case MessageType::Propose: return request(type, request_body, MessageType::ProposeResult);
    case MessageType::Validate: return request(type, request_body, MessageType::ValidateResult);
    case MessageType::Commit: return request(type, request_body, MessageType::CommitResult);
    case MessageType::Cancel: return request(type, request_body, MessageType::CancelResult);
    case MessageType::Diff: return request(type, request_body, MessageType::DiffResult);
    case MessageType::Stats: return request(type, request_body, MessageType::StatsResult);
    case MessageType::Subscribe:
      return request(type, request_body, MessageType::SubscribeResult);
    case MessageType::Poll: return request(type, request_body, MessageType::PollResult);
    case MessageType::Verify: return request(type, request_body, MessageType::VerifyResult);
    case MessageType::Shutdown:
      return request(type, request_body, MessageType::ShutdownResult);
    default:
      return Error(ErrorCode::ProtocolError, "unsupported client message type",
                   std::string(to_string(type)));
  }
}

Result<JsonValue> IntentClient::get_head() {
  JsonValue request_body;
  return call(MessageType::GetHead, request_body);
}

Result<JsonValue> IntentClient::stats() {
  JsonValue request_body;
  return call(MessageType::Stats, request_body);
}

Result<JsonValue> IntentClient::submit(IntentDocument document, const FenceToken& fence) {
  JsonValue request_body;
  (void)request_body.set("document", to_json(document));
  (void)request_body.set("actor", JsonValue::string(fence.writer.str()));
  const auto proposed = call(MessageType::Propose, request_body);
  if (!proposed) {
    return proposed.error();
  }
  const JsonValue* proposal = proposed->find("proposal");
  if (proposal == nullptr || proposal->try_string() == nullptr) {
    return Error(ErrorCode::ProtocolError, "propose response carried no proposal identity");
  }
  JsonValue commit_body;
  (void)commit_body.set("proposal", JsonValue::string(*proposal->try_string()));
  (void)commit_body.set("writer", JsonValue::string(fence.writer.str()));
  (void)commit_body.set("expected_generation",
                        JsonValue::string(fence.expected_generation.str()));
  return call(MessageType::Commit, commit_body);
}

Result<JsonValue> IntentClient::commit(const ProposalId& proposal, const FenceToken& fence) {
  JsonValue commit_body;
  (void)commit_body.set("proposal", JsonValue::string(proposal.str()));
  (void)commit_body.set("writer", JsonValue::string(fence.writer.str()));
  (void)commit_body.set("expected_generation", JsonValue::string(fence.expected_generation.str()));
  return call(MessageType::Commit, commit_body);
}

}  // namespace ifabric::net
