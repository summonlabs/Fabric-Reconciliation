// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/server.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "net.hpp"
#include "wire_ops.hpp"

namespace summon {
namespace fabric_reconciliation {
namespace {

using wireops::Request;
using wireops::Response;

struct SessionState {
  net::Socket socket;
  /// Releases this session's blocked read deterministically. A blocking recv is
  /// not guaranteed to return when the socket is shut down on every supported
  /// platform, so shutdown never depends on that behaviour.
  net::WakeupChannel wake;
  std::uint64_t session_id{0};
  BootId client_boot;
  CoordinatorEpoch handshake_epoch;
  std::string peer;
  std::atomic<std::uint64_t> requests{0};
  std::atomic<std::uint64_t> rejected{0};
  std::atomic<bool> live{false};
};

std::string TextResponse(const std::string& body) { return body; }

Response MakeError(const Status& status) {
  Response response;
  response.code = status.code();
  response.reason = status.reason();
  response.detail = status.detail();
  return response;
}

Response FromStatus(const Status& status, std::string body) {
  Response response;
  response.code = status.code();
  response.reason = status.reason();
  response.detail = status.detail();
  response.body = std::move(body);
  return response;
}

}  // namespace

struct ReconciliationServer::Impl {
  ReconciliationEngine* engine{nullptr};
  ServerOptions options{};
  net::Socket listener;
  net::WakeupChannel wake;
  std::thread accept_thread;
  std::vector<std::thread> workers;
  mutable std::mutex mutex;
  std::vector<std::shared_ptr<SessionState>> sessions;
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> accepted{0};
  std::atomic<std::uint64_t> rejected{0};
  std::atomic<std::uint64_t> next_session{1};

  void ServeSession(const std::shared_ptr<SessionState>& session);
  /// Ends a session and closes its connection so the peer observes the end
  /// immediately instead of waiting for a read that will never complete.
  static void CloseSession(const std::shared_ptr<SessionState>& session) noexcept;
  Response Handle(const Request& request, SessionState& session);
};

namespace {

FrameHeader MakeHeader(WireMessageType type, std::uint64_t session_id, std::uint64_t sequence) {
  FrameHeader header;
  header.protocol_version = kWireProtocolVersion;
  header.type = type;
  header.flags = 0;
  header.session_id = session_id;
  header.sequence = sequence;
  return header;
}

Status SendFrame(const net::Socket& socket, const FrameHeader& header, std::string_view payload) {
  const std::string image = EncodeFrame(header, payload);
  return net::SendAll(socket, image.data(), image.size());
}

/// Why a receive attempt ended without producing a frame. The distinction
/// matters: a protocol violation is answered with an error frame before the
/// connection is closed, while a released or peer-closed session is not.
enum class ReceiveOutcome {
  Frame,
  PeerClosed,
  Released,
  ProtocolError,
};

ReceiveOutcome ReceiveFrame(const std::shared_ptr<SessionState>& session, FrameReader& reader,
                            FrameHeader& header, std::string& payload) {
  const net::Socket& socket = session->socket;
  for (;;) {
    if (reader.failed()) {
      return ReceiveOutcome::ProtocolError;
    }
    if (reader.Next(header, payload)) {
      return ReceiveOutcome::Frame;
    }
    if (reader.failed()) {
      return ReceiveOutcome::ProtocolError;
    }
    bool ready = false;
    const Status waited = net::WaitReadable(socket, session->wake, ready);
    if (!waited.ok()) {
      // The session was released by shutdown, or the peer went away.
      return ReceiveOutcome::Released;
    }
    if (!ready) {
      continue;
    }
    char buffer[4096];
    std::size_t received = 0;
    const Status status = net::Receive(socket, buffer, sizeof(buffer), received);
    if (!status.ok() || received == 0) {
      return ReceiveOutcome::PeerClosed;
    }
    reader.Feed(std::string_view(buffer, received));
  }
}

}  // namespace

Response ReconciliationServer::Impl::Handle(const Request& request, SessionState& session) {
  // Every request is bound to the identity the session established.
  if (!(request.client_boot == session.client_boot)) {
    return MakeError(Status(StatusCode::StaleAuthority, ReasonCode::SessionBootMismatch,
                            "request carries a boot identity this session did not establish"));
  }
  const CoordinatorEpoch live = engine->epoch();
  if (!(request.epoch == live)) {
    return MakeError(Status(StatusCode::StaleAuthority, ReasonCode::SessionEpochMismatch,
                            "request carries an epoch that is no longer current"));
  }

  switch (request.operation) {
    case WireOperation::ActivePolicy: {
      const auto policy = engine->ActivePolicy();
      if (!policy.ok()) {
        return MakeError(policy.status());
      }
      return FromStatus(Status::Ok(), EncodePolicy(*policy));
    }
    case WireOperation::PutPolicy: {
      CanonicalReader reader(request.body);
      bool allow_same_version = false;
      std::string policy_bytes;
      if (!reader.ReadBool(allow_same_version) || !reader.ReadBytes(policy_bytes) ||
          !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed PutPolicy request"));
      }
      ReconciliationPolicy policy;
      if (!DecodePolicyExact(policy_bytes, policy, options.limits)) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "policy payload failed validation"));
      }
      const Status status = engine->PutPolicy(policy, allow_same_version);
      return FromStatus(status, status.ok() ? "ok" : std::string());
    }
    case WireOperation::CommitIntent: {
      CanonicalReader reader(request.body);
      std::string intent_bytes;
      if (!reader.ReadBytes(intent_bytes) || !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed CommitIntent request"));
      }
      IntentDocument document;
      if (!DecodeIntentExact(intent_bytes, document, options.limits)) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "intent payload failed validation"));
      }
      IntentCommitResult result;
      const Status status = engine->CommitIntent(document, result);
      CanonicalWriter writer;
      writer.PutBool(result.duplicate);
      writer.PutU64(result.generation.value());
      writer.PutDigest(result.digest);
      return FromStatus(status, writer.ok() ? writer.buffer() : std::string());
    }
    case WireOperation::RecordObservation: {
      CanonicalReader reader(request.body);
      std::string submission_bytes;
      std::int64_t stamp = 0;
      if (!reader.ReadBytes(submission_bytes) || !reader.ReadI64(stamp) || !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed RecordObservation request"));
      }
      ObservationSubmission submission;
      if (!DecodeSubmissionExact(submission_bytes, submission, options.limits)) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "observation payload failed validation"));
      }
      ObservationCommitResult result;
      const Status status = engine->RecordObservationAt(submission, stamp, result);
      CanonicalWriter writer;
      writer.PutBool(result.duplicate);
      writer.PutBytes(EncodeObservation(result.stamped));
      return FromStatus(status, writer.ok() ? writer.buffer() : std::string());
    }
    case WireOperation::Classify: {
      CanonicalReader reader(request.body);
      std::string scope_text;
      std::int64_t now = 0;
      if (!reader.ReadBytes(scope_text) || !reader.ReadI64(now) || !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed Classify request"));
      }
      const auto scope = ScopeId::TryParse(scope_text);
      if (!scope.has_value()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "invalid scope identity"));
      }
      const auto report = engine->Classify(*scope, now);
      if (!report.ok()) {
        return MakeError(report.status());
      }
      return FromStatus(Status::Ok(), RenderClassificationText(*report));
    }
    case WireOperation::Plan: {
      CanonicalReader reader(request.body);
      std::string scope_text;
      std::int64_t now = 0;
      bool dry_run = false;
      if (!reader.ReadBytes(scope_text) || !reader.ReadI64(now) || !reader.ReadBool(dry_run) ||
          !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed Plan request"));
      }
      const auto scope = ScopeId::TryParse(scope_text);
      if (!scope.has_value()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "invalid scope identity"));
      }
      PlanRequest plan_request;
      plan_request.scope = *scope;
      plan_request.now_unix_ms = now;
      plan_request.dry_run = dry_run;
      const auto plan = engine->Plan(plan_request);
      if (!plan.ok()) {
        return MakeError(plan.status());
      }
      return FromStatus(Status::Ok(), RenderPlanText(*plan));
    }
    case WireOperation::Dispatch: {
      CanonicalReader reader(request.body);
      std::string scope_text;
      std::int64_t now = 0;
      std::string expected_plan_id;
      if (!reader.ReadBytes(scope_text) || !reader.ReadI64(now) ||
          !reader.ReadBytes(expected_plan_id) || !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed Dispatch request"));
      }
      const auto scope = ScopeId::TryParse(scope_text);
      if (!scope.has_value()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "invalid scope identity"));
      }
      PlanRequest plan_request;
      plan_request.scope = *scope;
      plan_request.now_unix_ms = now;
      const auto plan = engine->Plan(plan_request);
      if (!plan.ok()) {
        return MakeError(plan.status());
      }
      if (!expected_plan_id.empty() && expected_plan_id != plan->plan_id) {
        return MakeError(Status(StatusCode::StaleAuthority, ReasonCode::AuthorityIntentGenerationMismatch,
                                "the plan changed since the caller observed it"));
      }
      const auto dispatched = engine->Dispatch(*plan);
      if (!dispatched.ok()) {
        return MakeError(dispatched.status());
      }
      std::ostringstream out;
      out << "plan=" << plan->plan_id << " verdict=" << ToText(plan->verdict)
          << " issued=" << dispatched->intents.size()
          << " already-issued=" << dispatched->already_issued
          << " fenced=" << dispatched->fenced << "\n";
      for (const ActionIntent& action : dispatched->intents) {
        out << "attempt=" << action.attempt.value() << " key="
            << ToHex(action.idempotency_key) << " subject=" << action.subject.str()
            << " action=" << ToText(action.action) << "\n";
      }
      return FromStatus(Status::Ok(), out.str());
    }
    case WireOperation::Complete: {
      CanonicalReader reader(request.body);
      std::uint64_t attempt = 0;
      std::uint64_t epoch = 0;
      BootId boot;
      Sha256Digest key{};
      std::uint8_t target = 0;
      std::int64_t at = 0;
      std::uint16_t reason = 0;
      if (!reader.ReadU64(attempt) || !reader.ReadU64(epoch) ||
          !wireops::GetBoot(reader, boot) || !reader.ReadDigest(key) ||
          !reader.ReadU8(target) || !reader.ReadI64(at) || !reader.ReadU16(reason) ||
          !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed Complete request"));
      }
      if (target > static_cast<std::uint8_t>(AttemptState::Interrupted) ||
          reason > static_cast<std::uint16_t>(ReasonCode::RestartLeasesNotRestored)) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "completion carries an out-of-domain enumeration"));
      }
      CompletionRequest completion;
      completion.attempt = AttemptId(attempt);
      completion.epoch = CoordinatorEpoch(epoch);
      completion.boot = boot;
      completion.idempotency_key = key;
      completion.target = static_cast<AttemptState>(target);
      completion.at_unix_ms = at;
      completion.reason = static_cast<ReasonCode>(reason);
      ReconciliationOutcome outcome;
      const Status status = engine->Complete(completion, outcome);
      std::ostringstream out;
      out << "attempt=" << outcome.attempt.value() << " state=" << ToText(outcome.terminal_state)
          << " reason=" << ToText(outcome.reason)
          << " verified=" << (outcome.verified ? "yes" : "no")
          << " ambiguous=" << (outcome.ambiguous ? "yes" : "no") << "\n";
      return FromStatus(status, out.str());
    }
    case WireOperation::Verify: {
      CanonicalReader reader(request.body);
      std::string scope_text;
      std::string subject_text;
      Sha256Digest key{};
      std::string observation_text;
      std::uint64_t generation = 0;
      std::uint64_t epoch = 0;
      std::string verifier_text;
      std::string applier_text;
      std::int64_t at = 0;
      bool confirms = false;
      if (!reader.ReadBytes(scope_text) || !reader.ReadBytes(subject_text) ||
          !reader.ReadDigest(key) || !reader.ReadBytes(observation_text) ||
          !reader.ReadU64(generation) || !reader.ReadU64(epoch) ||
          !reader.ReadBytes(verifier_text) || !reader.ReadBytes(applier_text) ||
          !reader.ReadI64(at) || !reader.ReadBool(confirms) || !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed Verify request"));
      }
      const auto scope = ScopeId::TryParse(scope_text);
      const auto subject = SubjectId::TryParse(subject_text);
      const auto observation = ObservationId::TryParse(observation_text);
      const auto verifier = ReporterId::TryParse(verifier_text);
      const auto applier = ReporterId::TryParse(applier_text);
      if (!scope.has_value() || !subject.has_value() || !observation.has_value() ||
          !verifier.has_value() || !applier.has_value()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "verification carries an invalid identity"));
      }
      VerificationEvidence evidence;
      evidence.scope = *scope;
      evidence.subject = *subject;
      evidence.idempotency_key = key;
      evidence.observation = *observation;
      evidence.generation = ObservationGeneration(generation);
      evidence.received_epoch = CoordinatorEpoch(epoch);
      evidence.verifier = *verifier;
      evidence.applier = *applier;
      evidence.received_unix_ms = at;
      evidence.confirms_effect = confirms;
      ReconciliationOutcome outcome;
      const Status status = engine->Verify(evidence, outcome);
      std::ostringstream out;
      out << "attempt=" << outcome.attempt.value() << " state=" << ToText(outcome.terminal_state)
          << " verified=" << (outcome.verified ? "yes" : "no") << "\n";
      return FromStatus(status, out.str());
    }
    case WireOperation::PutFence: {
      CanonicalReader reader(request.body);
      std::string fence_bytes;
      if (!reader.ReadBytes(fence_bytes) || !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed PutFence request"));
      }
      CanonicalReader fence_reader(fence_bytes);
      FenceEntry fence;
      if (!DecodeFence(fence_reader, fence) || !fence_reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "fence payload failed validation"));
      }
      const Status status = engine->PutFence(fence);
      return FromStatus(status, status.ok() ? "ok" : std::string());
    }
    case WireOperation::ClearFence: {
      CanonicalReader reader(request.body);
      std::string id_text;
      if (!reader.ReadBytes(id_text) || !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed ClearFence request"));
      }
      const auto id = FenceId::TryParse(id_text);
      if (!id.has_value()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "invalid fence identity"));
      }
      const Status status = engine->ClearFence(*id);
      return FromStatus(status, status.ok() ? "ok" : std::string());
    }
    case WireOperation::KnownScopes: {
      const auto scopes = engine->KnownScopes();
      if (!scopes.ok()) {
        return MakeError(scopes.status());
      }
      std::ostringstream out;
      for (const ScopeId& scope : *scopes) {
        out << scope.str() << "\n";
      }
      return FromStatus(Status::Ok(), out.str());
    }
    case WireOperation::Attempts: {
      CanonicalReader reader(request.body);
      std::string scope_text;
      if (!reader.ReadBytes(scope_text) || !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed Attempts request"));
      }
      ScopeId scope;
      if (!scope_text.empty()) {
        const auto parsed = ScopeId::TryParse(scope_text);
        if (!parsed.has_value()) {
          return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                  "invalid scope identity"));
        }
        scope = *parsed;
      }
      const auto attempts = engine->Attempts(scope);
      if (!attempts.ok()) {
        return MakeError(attempts.status());
      }
      std::ostringstream out;
      for (const AttemptRecord& attempt : *attempts) {
        out << RenderAttemptText(attempt);
      }
      return FromStatus(Status::Ok(), out.str());
    }
    case WireOperation::Outcomes: {
      CanonicalReader reader(request.body);
      std::string scope_text;
      if (!reader.ReadBytes(scope_text) || !reader.AtEnd()) {
        return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                "malformed Outcomes request"));
      }
      ScopeId scope;
      if (!scope_text.empty()) {
        const auto parsed = ScopeId::TryParse(scope_text);
        if (!parsed.has_value()) {
          return MakeError(Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                                  "invalid scope identity"));
        }
        scope = *parsed;
      }
      const auto outcomes = engine->Outcomes(scope);
      if (!outcomes.ok()) {
        return MakeError(outcomes.status());
      }
      std::ostringstream out;
      for (const ReconciliationOutcome& outcome : *outcomes) {
        out << "attempt=" << outcome.attempt.value() << " subject=" << outcome.subject.str()
            << " state=" << ToText(outcome.terminal_state)
            << " verified=" << (outcome.verified ? "yes" : "no")
            << " ambiguous=" << (outcome.ambiguous ? "yes" : "no")
            << " key=" << ToHex(outcome.idempotency_key) << "\n";
      }
      return FromStatus(Status::Ok(), out.str());
    }
    case WireOperation::BootReport:
      return FromStatus(Status::Ok(), RenderBootReportText(engine->boot_report()));
    case WireOperation::Compact: {
      const Status status = engine->Compact();
      return FromStatus(status, status.ok() ? "ok" : std::string());
    }
    case WireOperation::Invalid:
    default:
      return MakeError(Status(StatusCode::Unsupported, ReasonCode::FrameTypeUnknown,
                              "unsupported operation"));
  }
}

void ReconciliationServer::Impl::CloseSession(
    const std::shared_ptr<SessionState>& session) noexcept {
  session->live.store(false);
  // The session thread owns its connection: shutting it down and closing it
  // here is what makes the end of a session observable at the other end.
  session->socket.Shutdown();
  session->socket.Close();
}

void ReconciliationServer::Impl::ServeSession(const std::shared_ptr<SessionState>& session) {
  FrameReader reader(static_cast<std::uint32_t>(options.limits.max_wire_payload_bytes));
  const net::Socket& socket = session->socket;
  std::uint64_t incoming_sequence = 1;
  std::uint64_t outgoing_sequence = 1;

  // Handshake. A frame that is not a well-formed Hello ends the session.
  FrameHeader header;
  std::string payload;
  const ReceiveOutcome handshake = ReceiveFrame(session, reader, header, payload);
  if (handshake == ReceiveOutcome::ProtocolError) {
    ++rejected;
    (void)SendFrame(socket, MakeHeader(WireMessageType::Error, 0, outgoing_sequence),
                    wireops::EncodeResponse(MakeError(
                        Status(StatusCode::Rejected, ReasonCode::FrameTypeUnknown,
                               "the opening frame is not a decodable handshake frame"))));
  }
  if (handshake != ReceiveOutcome::Frame) {
    CloseSession(session);
    return;
  }
  if (header.type != WireMessageType::Hello || header.sequence != incoming_sequence ||
      header.session_id != 0) {
    ++rejected;
    CloseSession(session);
    return;
  }
  BootId client_boot;
  std::string nonce;
  if (!wireops::DecodeHello(payload, client_boot, nonce)) {
    ++rejected;
    CloseSession(session);
    return;
  }
  const std::uint64_t session_id = next_session.fetch_add(1);
  {
    std::lock_guard<std::mutex> guard(mutex);
    session->session_id = session_id;
    session->client_boot = client_boot;
    session->handshake_epoch = engine->epoch();
  }
  wireops::Welcome welcome;
  welcome.session_id = session_id;
  welcome.epoch = engine->epoch();
  welcome.server_boot = engine->boot();
  welcome.store = engine->store_id();
  welcome.nonce = nonce;
  welcome.max_payload = static_cast<std::uint32_t>(options.limits.max_wire_payload_bytes);
  const std::string welcome_payload = wireops::EncodeWelcome(welcome);
  if (welcome_payload.empty() ||
      !SendFrame(socket, MakeHeader(WireMessageType::Welcome, 0, 1), welcome_payload).ok()) {
    CloseSession(session);
    return;
  }
  session->live.store(true);

  std::uint64_t expected_incoming = 2;
  for (;;) {
    const ReceiveOutcome outcome = ReceiveFrame(session, reader, header, payload);
    if (outcome == ReceiveOutcome::ProtocolError) {
      // The stream can no longer be framed, so it is answered once and closed
      // rather than resynchronised.
      ++rejected;
      (void)SendFrame(socket, MakeHeader(WireMessageType::Error, session_id, outgoing_sequence++),
                      wireops::EncodeResponse(MakeError(Status(
                          StatusCode::Rejected, ReasonCode::FrameTypeUnknown,
                          "the frame stream violated the protocol and cannot be trusted"))));
      break;
    }
    if (outcome != ReceiveOutcome::Frame) {
      break;
    }
    if (header.session_id != session_id) {
      ++rejected;
      (void)SendFrame(socket,
                      MakeHeader(WireMessageType::Error, session_id, outgoing_sequence++),
                      wireops::EncodeResponse(MakeError(Status(
                          StatusCode::StaleAuthority, ReasonCode::SessionIdentityMismatch,
                          "frame carries a session identity that is not this session's"))));
      continue;
    }
    if (header.type == WireMessageType::Goodbye) {
      break;
    }
    if (header.sequence != expected_incoming) {
      ++rejected;
      (void)SendFrame(socket,
                      MakeHeader(WireMessageType::Error, session_id, outgoing_sequence++),
                      wireops::EncodeResponse(MakeError(Status(
                          StatusCode::ConflictState, ReasonCode::SessionSequenceRegression,
                          "frame sequence is not contiguous"))));
      // A regressed or replayed sequence is a hard session failure: the stream
      // can no longer be trusted, so the session is closed rather than resynced.
      break;
    }
    ++expected_incoming;
    if (header.type == WireMessageType::Ping) {
      if (!SendFrame(socket, MakeHeader(WireMessageType::Pong, session_id, outgoing_sequence++),
                     std::string())
               .ok()) {
        break;
      }
      continue;
    }
    if (header.type != WireMessageType::Request) {
      ++rejected;
      (void)SendFrame(socket,
                      MakeHeader(WireMessageType::Error, session_id, outgoing_sequence++),
                      wireops::EncodeResponse(MakeError(Status(
                          StatusCode::Rejected, ReasonCode::FrameTypeUnknown,
                          "only request frames are accepted after the handshake"))));
      continue;
    }
    Request request;
    if (!wireops::DecodeRequest(payload, request)) {
      ++rejected;
      (void)SendFrame(socket,
                      MakeHeader(WireMessageType::Error, session_id, outgoing_sequence++),
                      wireops::EncodeResponse(MakeError(Status(
                          StatusCode::Rejected, ReasonCode::FrameTypeUnknown,
                          "request payload failed decoding"))));
      continue;
    }
    session->requests.fetch_add(1);
    // The engine is called with no server lock held: the ownership audit in
    // docs/CONCURRENCY-AUDIT.md records this as a hard rule.
    const Response response = Handle(request, *session);
    if (!SendFrame(socket, MakeHeader(WireMessageType::Response, session_id, outgoing_sequence++),
                   wireops::EncodeResponse(response))
             .ok()) {
      break;
    }
  }
  CloseSession(session);
}

ReconciliationServer::~ReconciliationServer() {
  if (impl_) {
    (void)Stop();
  }
}

Status ReconciliationServer::Start(const ServerOptions& options, ReconciliationEngine& engine,
                                   std::unique_ptr<ReconciliationServer>& out) {
  auto server = std::unique_ptr<ReconciliationServer>(new ReconciliationServer());
  server->impl_ = std::make_unique<Impl>();
  Impl& impl = *server->impl_;
  impl.engine = &engine;
  impl.options = options;
  impl.stopping.store(false);

  const Status created = net::WakeupChannel::Create(impl.wake);
  if (!created.ok()) {
    return created;
  }
  std::uint16_t port = 0;
  const Status listening = net::Listen(options.bind_address, options.port, impl.listener, port);
  if (!listening.ok()) {
    impl.wake.Close();
    return listening;
  }
  server->port_ = port;

  impl.accept_thread = std::thread([&impl]() {
    while (!impl.stopping.load()) {
      bool ready = false;
      const Status waited = net::WaitReadable(impl.listener, impl.wake, ready);
      if (!waited.ok()) {
        break;
      }
      if (!ready) {
        continue;
      }
      net::Socket connection;
      const Status accepted = net::Accept(impl.listener, connection);
      if (!accepted.ok()) {
        if (impl.stopping.load()) {
          break;
        }
        continue;
      }
      std::size_t active = 0;
      {
        std::lock_guard<std::mutex> guard(impl.mutex);
        active = impl.sessions.size();
      }
      if (active >= impl.options.max_sessions) {
        ++impl.rejected;
        connection.Close();
        continue;
      }
      auto session = std::make_shared<SessionState>();
      session->socket = std::move(connection);
      if (!net::WakeupChannel::Create(session->wake).ok()) {
        ++impl.rejected;
        session->socket.Close();
        continue;
      }
      session->peer = net::DescribePeer(session->socket);
      {
        std::lock_guard<std::mutex> guard(impl.mutex);
        impl.sessions.push_back(session);
      }
      ++impl.accepted;
      impl.workers.emplace_back([&impl, session]() { impl.ServeSession(session); });
      if (impl.options.single_session) {
        break;
      }
    }
  });

  out = std::move(server);
  return Status::Ok();
}

Status ReconciliationServer::Stop() {
  if (!impl_) {
    return Status::Ok();
  }
  Impl& impl = *impl_;
  const bool already = impl.stopping.exchange(true);
  if (!already) {
    // Release the accept wait first, then every established connection. Both
    // actions are non-blocking, so no thread is ever joined while it still
    // holds a resource another thread is waiting for.
    impl.wake.Signal();
    impl.listener.Shutdown();
    std::vector<std::shared_ptr<SessionState>> live;
    {
      std::lock_guard<std::mutex> guard(impl.mutex);
      live = impl.sessions;
    }
    for (const auto& session : live) {
      // Reading is released through the session's own wake channel, which is
      // deterministic on every supported platform. Shutting the socket down as
      // well releases a thread blocked in a write, which does honour shutdown.
      session->wake.Signal();
      session->socket.Shutdown();
    }
  }
  if (impl.accept_thread.joinable()) {
    impl.accept_thread.join();
  }
  for (std::thread& worker : impl.workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  impl.workers.clear();
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    for (const auto& session : impl.sessions) {
      session->socket.Close();
    }
    impl.sessions.clear();
  }
  impl.listener.Close();
  impl.wake.Close();
  return Status::Ok();
}

std::string ReconciliationServer::endpoint() const {
  if (!impl_) {
    return std::string();
  }
  return impl_->options.bind_address + ":" + std::to_string(port_);
}

std::vector<SessionSnapshot> ReconciliationServer::sessions() const {
  std::vector<SessionSnapshot> snapshots;
  if (!impl_) {
    return snapshots;
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  for (const auto& session : impl_->sessions) {
    SessionSnapshot snapshot;
    snapshot.session_id = session->session_id;
    snapshot.client_boot = session->client_boot;
    snapshot.handshake_epoch = session->handshake_epoch;
    snapshot.requests_served = session->requests.load();
    snapshot.rejected_frames = session->rejected.load();
    snapshots.push_back(snapshot);
  }
  return snapshots;
}

std::uint64_t ReconciliationServer::connections_accepted() const noexcept {
  return impl_ ? impl_->accepted.load() : 0;
}

std::uint64_t ReconciliationServer::frames_rejected() const noexcept {
  return impl_ ? impl_->rejected.load() : 0;
}

}  // namespace fabric_reconciliation
}  // namespace summon
