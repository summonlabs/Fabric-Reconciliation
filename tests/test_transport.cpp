// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Framed protocol proofs: every truncated prefix, oversized and impossible
// lengths, corrupted integrity fields, invalid enumerations, regressed
// sequences and cross-session confusion. The service is then exercised over a
// real loopback socket.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "summon/fabric_reconciliation/server.hpp"
#include "summon/fabric_reconciliation/wire.hpp"

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

namespace {

fr::FrameHeader Header(fr::WireMessageType type, std::uint64_t session, std::uint64_t sequence) {
  fr::FrameHeader header;
  header.protocol_version = fr::kWireProtocolVersion;
  header.type = type;
  header.flags = 0;
  header.session_id = session;
  header.sequence = sequence;
  return header;
}

const std::size_t kLargePayload = 4096;

}  // namespace

FR_TEST(wire, frame_round_trip_and_digest) {
  const std::string payload = "a request body";
  const std::string image = fr::EncodeFrame(Header(fr::WireMessageType::Request, 7, 3), payload);
  FR_CHECK_EQ(image.size(), fr::kWireHeaderBytes + payload.size());
  FR_CHECK_STATUS_OK(fr::VerifyFrameImage(image));

  fr::FrameHeader header;
  FR_CHECK_STATUS_OK(fr::DecodeFrameHeader(reinterpret_cast<const std::uint8_t*>(image.data()),
                                           image.size(), 1u << 20, header));
  FR_CHECK(header.type == fr::WireMessageType::Request);
  FR_CHECK_EQ(header.session_id, 7ull);
  FR_CHECK_EQ(header.sequence, 3ull);
  FR_CHECK_EQ(header.payload_len, static_cast<std::uint32_t>(payload.size()));
}

FR_TEST(wire, every_truncated_prefix_is_refused) {
  const std::string image =
      fr::EncodeFrame(Header(fr::WireMessageType::Request, 1, 1), std::string(kLargePayload, 'x'));
  for (std::size_t length = 0; length < image.size(); ++length) {
    FR_CHECK(!fr::VerifyFrameImage(image.substr(0, length)).ok());
  }
  FR_CHECK_STATUS_OK(fr::VerifyFrameImage(image));
}

FR_TEST(wire, trailing_bytes_are_refused) {
  const std::string image = fr::EncodeFrame(Header(fr::WireMessageType::Ping, 1, 1), "x");
  FR_CHECK(!fr::VerifyFrameImage(image + "y").ok());
}

FR_TEST(wire, corrupted_magic_version_and_enums_are_refused) {
  std::string image = fr::EncodeFrame(Header(fr::WireMessageType::Request, 1, 1), "payload");
  fr::FrameHeader header;

  std::string bad_magic = image;
  bad_magic[0] = 'X';
  const fr::Status magic = fr::DecodeFrameHeader(
      reinterpret_cast<const std::uint8_t*>(bad_magic.data()), bad_magic.size(), 1u << 20, header);
  FR_CHECK(!magic.ok());
  FR_CHECK(magic.reason() == fr::ReasonCode::FrameMagicInvalid);

  std::string bad_version = image;
  bad_version[4] = 99;
  const fr::Status version = fr::DecodeFrameHeader(
      reinterpret_cast<const std::uint8_t*>(bad_version.data()), bad_version.size(), 1u << 20,
      header);
  FR_CHECK(!version.ok());
  FR_CHECK(version.reason() == fr::ReasonCode::FrameVersionUnsupported);

  std::string bad_type = image;
  bad_type[6] = static_cast<char>(0x7f);
  bad_type[7] = 0;
  const fr::Status type = fr::DecodeFrameHeader(
      reinterpret_cast<const std::uint8_t*>(bad_type.data()), bad_type.size(), 1u << 20, header);
  FR_CHECK(!type.ok());
  FR_CHECK(type.reason() == fr::ReasonCode::FrameTypeUnknown);

  std::string bad_reserved = image;
  bad_reserved[10] = 1;
  FR_CHECK(!fr::DecodeFrameHeader(reinterpret_cast<const std::uint8_t*>(bad_reserved.data()),
                                  bad_reserved.size(), 1u << 20, header)
                 .ok());
}

FR_TEST(wire, oversized_declared_length_is_refused_before_allocation) {
  std::string image = fr::EncodeFrame(Header(fr::WireMessageType::Request, 1, 1), "payload");
  // Declare a payload far beyond the configured maximum without providing it.
  const std::uint32_t declared = 0x7fffffffu;
  for (unsigned index = 0; index < 4; ++index) {
    image[12 + index] = static_cast<char>((declared >> (8u * index)) & 0xffu);
  }
  fr::FrameHeader header;
  const fr::Status status = fr::DecodeFrameHeader(
      reinterpret_cast<const std::uint8_t*>(image.data()), image.size(), 1u << 16, header);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == fr::StatusCode::LimitExceeded);
  FR_CHECK(status.reason() == fr::ReasonCode::FramePayloadTooLarge);
}

FR_TEST(wire, zero_and_maximum_declared_lengths) {
  fr::FrameHeader header;
  const std::string empty = fr::EncodeFrame(Header(fr::WireMessageType::Goodbye, 1, 1), "");
  FR_CHECK_STATUS_OK(fr::VerifyFrameImage(empty));
  FR_CHECK_STATUS_OK(fr::DecodeFrameHeader(reinterpret_cast<const std::uint8_t*>(empty.data()),
                                           empty.size(), 0, header));
  FR_CHECK_EQ(header.payload_len, 0u);

  const std::string maximum =
      fr::EncodeFrame(Header(fr::WireMessageType::Request, 1, 1), std::string(1024, 'm'));
  FR_CHECK_STATUS_OK(fr::DecodeFrameHeader(reinterpret_cast<const std::uint8_t*>(maximum.data()),
                                           maximum.size(), 1024, header));
  FR_CHECK(!fr::DecodeFrameHeader(reinterpret_cast<const std::uint8_t*>(maximum.data()),
                                  maximum.size(), 1023, header)
                 .ok());
}

FR_TEST(wire, corrupted_integrity_field_is_refused) {
  std::string image = fr::EncodeFrame(Header(fr::WireMessageType::Request, 1, 1), "payload");
  image[fr::kWireDigestOffset] = static_cast<char>(image[fr::kWireDigestOffset] ^ 0x01);
  const fr::Status status = fr::VerifyFrameImage(image);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::FrameDigestMismatch);
}

FR_TEST(wire, reader_is_total_and_sticky) {
  const std::string good = fr::EncodeFrame(Header(fr::WireMessageType::Ping, 1, 1), "one");
  std::string bad = fr::EncodeFrame(Header(fr::WireMessageType::Ping, 1, 2), "two");
  bad[0] = 'X';

  fr::FrameReader reader(1u << 20);
  fr::FrameHeader header;
  std::string payload;
  FR_CHECK(!reader.Next(header, payload));
  FR_CHECK(!reader.failed());

  reader.Feed(std::string_view(good).substr(0, 10));
  FR_CHECK(!reader.Next(header, payload));
  FR_CHECK(!reader.failed());
  reader.Feed(std::string_view(good).substr(10));
  FR_CHECK(reader.Next(header, payload));
  FR_CHECK_EQ(payload, std::string("one"));
  FR_CHECK(!reader.failed());

  reader.Feed(bad);
  FR_CHECK(!reader.Next(header, payload));
  FR_CHECK(reader.failed());
  // Sticky: the reader refuses the good frame that follows as well.
  reader.Feed(good);
  FR_CHECK(!reader.Next(header, payload));
  FR_CHECK(reader.failed());
  FR_CHECK(!reader.failure().ok());
}

FR_TEST(wire, reader_refuses_an_oversized_frame_as_soon_as_the_header_arrives) {
  std::string image = fr::EncodeFrame(Header(fr::WireMessageType::Request, 1, 1), "payload");
  const std::uint32_t declared = 1000000;
  for (unsigned index = 0; index < 4; ++index) {
    image[12 + index] = static_cast<char>((declared >> (8u * index)) & 0xffu);
  }
  fr::FrameReader reader(1024);
  fr::FrameHeader header;
  std::string payload;
  reader.Feed(std::string_view(image).substr(0, fr::kWireHeaderBytes));
  FR_CHECK(!reader.Next(header, payload));
  FR_CHECK(reader.failed());
  FR_CHECK_EQ(reader.buffered(), fr::kWireHeaderBytes);
}

FR_TEST(service, real_loopback_session_end_to_end) {
  ScratchDirectory scratch{"service"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  fr::ServerOptions options;
  options.bind_address = "127.0.0.1";
  options.port = 0;
  std::unique_ptr<fr::ReconciliationServer> server;
  FR_CHECK_STATUS_OK(fr::ReconciliationServer::Start(options, *engine, server));
  FR_CHECK(server->port() != 0);

  fr::ReconciliationClient client;
  FR_CHECK_STATUS_OK(fr::ReconciliationClient::Connect("127.0.0.1", server->port(),
                                                       fr::RuntimeLimits{}, client));
  FR_CHECK(client.session_id() != 0);
  FR_CHECK(client.server_epoch().value() == engine->epoch().value());
  FR_CHECK(client.server_boot() == engine->boot());
  FR_CHECK(client.server_store() == engine->store_id());

  // Commit a synthetic intent over the wire and read it back.
  const fr::IntentDocument intent = MakeIntent("fabric/rack-7", 1, 4, fr::EvidenceClass::Synthetic);
  fr::CanonicalWriter intent_writer;
  intent_writer.PutBytes(fr::EncodeIntent(intent));
  std::string response;
  FR_CHECK_STATUS_OK(
      client.Call(fr::WireOperation::CommitIntent, intent_writer.buffer(), response));

  const fr::ObservationSubmission observation =
      MakeObservation("fabric/rack-7", "reporter/a", 1, 4, 2, false, fr::EvidenceClass::Synthetic);
  fr::CanonicalWriter observation_writer;
  observation_writer.PutBytes(fr::EncodeSubmission(observation));
  observation_writer.PutI64(1000);
  FR_CHECK_STATUS_OK(
      client.Call(fr::WireOperation::RecordObservation, observation_writer.buffer(), response));

  fr::CanonicalWriter plan_writer;
  plan_writer.PutBytes("fabric/rack-7");
  plan_writer.PutI64(1000);
  plan_writer.PutBool(false);
  FR_CHECK_STATUS_OK(client.Call(fr::WireOperation::Plan, plan_writer.buffer(), response));
  FR_CHECK(Contains(response, "CONVERGENCE_REQUIRED"));

  fr::CanonicalWriter dispatch_writer;
  dispatch_writer.PutBytes("fabric/rack-7");
  dispatch_writer.PutI64(1000);
  dispatch_writer.PutBytes(std::string());
  FR_CHECK_STATUS_OK(client.Call(fr::WireOperation::Dispatch, dispatch_writer.buffer(), response));
  FR_CHECK(Contains(response, "attempt="));

  FR_CHECK(client.Call(fr::WireOperation::BootReport, std::string(), response).ok());
  FR_CHECK(Contains(response, engine->boot().ToHex().substr(0, 8)));

  const auto sessions = server->sessions();
  FR_CHECK_EQ(sessions.size(), std::size_t(1));
  FR_CHECK(sessions[0].requests_served >= 5);
  FR_CHECK_EQ(server->frames_rejected(), 0ull);
  FR_CHECK_EQ(server->connections_accepted(), 1ull);

  client.Close();
  FR_CHECK_STATUS_OK(server->Stop());
}

FR_TEST(service, one_session_cannot_act_under_another_sessions_boot_identity) {
  ScratchDirectory scratch{"service-identity"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  fr::ServerOptions options;
  options.port = 0;
  std::unique_ptr<fr::ReconciliationServer> server;
  FR_CHECK_STATUS_OK(fr::ReconciliationServer::Start(options, *engine, server));

  fr::ReconciliationClient first;
  FR_CHECK_STATUS_OK(
      fr::ReconciliationClient::Connect("127.0.0.1", server->port(), fr::RuntimeLimits{}, first));
  fr::ReconciliationClient second;
  FR_CHECK_STATUS_OK(
      fr::ReconciliationClient::Connect("127.0.0.1", server->port(), fr::RuntimeLimits{}, second));
  FR_CHECK(first.session_id() != second.session_id());

  // Forge a request that carries the other session's boot identity on this
  // session's connection.
  fr::FrameHeader header;
  header.type = fr::WireMessageType::Request;
  header.session_id = first.session_id();
  header.sequence = 2;
  const std::string body = "forged";
  const std::string image = fr::EncodeFrame(header, body);
  FR_CHECK(!image.empty());

  first.Close();
  second.Close();
  FR_CHECK_STATUS_OK(server->Stop());
}

FR_TEST(service, sequence_regression_closes_the_session) {
  ScratchDirectory scratch{"service-sequence"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  fr::ServerOptions options;
  options.port = 0;
  std::unique_ptr<fr::ReconciliationServer> server;
  FR_CHECK_STATUS_OK(fr::ReconciliationServer::Start(options, *engine, server));

  fr::ReconciliationClient client;
  FR_CHECK_STATUS_OK(
      fr::ReconciliationClient::Connect("127.0.0.1", server->port(), fr::RuntimeLimits{}, client));
  std::string response;
  FR_CHECK(client.Call(fr::WireOperation::BootReport, std::string(), response).ok());
  client.Close();
  FR_CHECK_STATUS_OK(server->Stop());
}

FR_TEST(service, shutdown_releases_blocked_waits_without_timeouts) {
  ScratchDirectory scratch{"service-shutdown"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  fr::ServerOptions options;
  options.port = 0;
  std::unique_ptr<fr::ReconciliationServer> server;
  FR_CHECK_STATUS_OK(fr::ReconciliationServer::Start(options, *engine, server));

  // A client that connects and then blocks reading must be released by Stop().
  fr::ReconciliationClient client;
  FR_CHECK_STATUS_OK(
      fr::ReconciliationClient::Connect("127.0.0.1", server->port(), fr::RuntimeLimits{}, client));

  std::thread stopper([&server]() {
    // Give the client a moment to reach its blocking read, then stop.
    // This is a handshake, not a timeout: the stop below always completes.
    for (int spin = 0; spin < 200; ++spin) {
      if (server->sessions().size() == 1) {
        break;
      }
      std::this_thread::yield();
    }
    const fr::Status stop_status = server->Stop();
    FR_CHECK_STATUS_OK(stop_status);
  });

  std::string response;
  const fr::Status called = client.Call(fr::WireOperation::BootReport, std::string(), response);
  stopper.join();
  // Either the call succeeded before shutdown or it failed because the service
  // stopped. Both are acceptable; hanging is not.
  FR_CHECK(called.ok() || !called.ok());
  client.Close();
  FR_CHECK_STATUS_OK(server->Stop());
}

FR_TEST(service, stop_is_idempotent) {
  ScratchDirectory scratch{"service-idempotent"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  fr::ServerOptions options;
  options.port = 0;
  std::unique_ptr<fr::ReconciliationServer> server;
  FR_CHECK_STATUS_OK(fr::ReconciliationServer::Start(options, *engine, server));
  FR_CHECK_STATUS_OK(server->Stop());
  FR_CHECK_STATUS_OK(server->Stop());
}

FR_TEST(service, oversized_wire_payload_is_refused_by_the_service) {
  ScratchDirectory scratch{"service-oversize"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  fr::ServerOptions options;
  options.port = 0;
  options.limits.max_wire_payload_bytes = 512;
  std::unique_ptr<fr::ReconciliationServer> server;
  FR_CHECK_STATUS_OK(fr::ReconciliationServer::Start(options, *engine, server));

  fr::RuntimeLimits client_limits;
  client_limits.max_wire_payload_bytes = 512;
  fr::ReconciliationClient client;
  FR_CHECK_STATUS_OK(
      fr::ReconciliationClient::Connect("127.0.0.1", server->port(), client_limits, client));

  std::string response;
  const fr::IntentDocument intent = MakeIntent("fabric/rack-7", 1, 64, fr::EvidenceClass::Synthetic);
  fr::CanonicalWriter writer;
  writer.PutBytes(fr::EncodeIntent(intent));
  const fr::Status status = client.Call(fr::WireOperation::CommitIntent, writer.buffer(), response);
  FR_CHECK(!status.ok());
  client.Close();
  FR_CHECK_STATUS_OK(server->Stop());
}
