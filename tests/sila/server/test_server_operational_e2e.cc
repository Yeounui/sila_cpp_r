// End-to-end tests for the operational surface Run() turns on alongside the
// application's own gRPC services: the gRPC health-check service
// (grpc::EnableDefaultHealthCheckService), gRPC server reflection
// (grpc::reflection::InitProtoReflectionServerBuilderPlugin), and the
// LogCallback wired through Builder::setLogCallback into InterceptorChain
// and fired by dispatchToHandler() (GrpcTransport.h) on every successful
// gRPC command/property call. Audit finding 3.4g: none of the three had
// automated coverage before this file.
//
// Health and reflection are exercised over raw grpc::ByteBuffer RPCs rather
// than generated Stub classes: this vcpkg gRPC port compiles
// grpc.health.v1.Health and grpc.reflection.v1.ServerReflection's C++
// bindings straight into libgrpc++_reflection.a for the *server*-side
// plugins (InitProtoReflectionServerBuilderPlugin,
// EnableDefaultHealthCheckService) but installs no health.grpc.pb.h /
// reflection.grpc.pb.h for a *client* to include — confirmed empirically
// (no such header under vcpkg_installed/x64-linux/include, and no
// health.proto/reflection.proto source shipped to regenerate one). Adding a
// protoc codegen step for those two protos would need a CMakeLists.txt
// change, which is out of scope for this file (see caller's report). Both
// wire formats below are the stable, minimal (varint + length-delimited)
// subset documented in gRPC's health.proto / reflection.proto, so hand
// encoding/decoding sidesteps the build change for two trivial messages.
#include <sila/server/SiLAServerBase.h>

#include <sila/server/LogCallback.h>
#include <sila/server/SiLAServiceImpl.h>
#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/CredentialVerifier.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/client_unary_call.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/slice.h>
#include <grpcpp/support/sync_stream.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sila2::LogCallback;
using sila2::LogLevel;
using sila2::SiLAServerBase;

// Distinct ports per test, following test_sila_server_base_run_shutdown_e2e.cc's
// convention — picked outside every kPort* range already in use under tests/sila.
constexpr uint16_t kPortHealthCheckServing = 50290;
constexpr uint16_t kPortHealthCheckBeforeStart = 50291;
constexpr uint16_t kPortReflectionListsServices = 50292;
constexpr uint16_t kPortReflectionIncludesSiLAService = 50293;
constexpr uint16_t kPortLogCallbackDispatch = 50294;
constexpr uint16_t kPortLogCallbackNullptr = 50295;

// Full proto service names (package + service, from the .proto files under
// src/sila/common/proto/) — what gRPC reflection reports, distinct from the
// FQI strings (org.silastandard/...) used elsewhere in this codebase.
constexpr std::string_view kSiLAServiceProtoName =
    "sila2.org.silastandard.core.silaservice.v1.SiLAService";
constexpr std::string_view kAuthenticationServiceProtoName =
    "sila2.org.silastandard.core.authenticationservice.v1.AuthenticationService";

// Same pattern as test_sila_server_base_run_shutdown_e2e.cc's dialChannel.
std::shared_ptr<grpc::Channel> dialChannel(const SiLAServerBase& server, uint16_t port) {
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = server.certificatePem();
    return grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(opts));
}

// Reused from test_sila_server_base_run_shutdown_e2e.cc's pattern: only used
// to register a second real gRPC service so reflection has more than one
// service to enumerate.
struct AcceptingVerifier : sila2::auth::CredentialVerifier {
    std::optional<std::string> verify(const std::string& user, const std::string&) override {
        return user;
    }
};

struct AllowAllPolicy : sila2::auth::AccessPolicy {
    bool isAllowed(const std::string&, const std::string&) const override { return true; }
    std::vector<std::string> allowedFqis(const std::string&) const override { return {}; }
};

// ---------------------------------------------------------------------------
// Minimal hand-rolled protobuf wire codec (see file header comment for why).
// Only the two wire types (0: varint, 2: length-delimited) that
// HealthCheckResponse and ServerReflectionResponse actually use are handled.
// ---------------------------------------------------------------------------

std::string encodeVarint(uint64_t value) {
    std::string out;
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
    return out;
}

std::string encodeLengthDelimitedField(uint32_t fieldNumber, std::string_view payload) {
    std::string out = encodeVarint((fieldNumber << 3) | 2);
    out += encodeVarint(payload.size());
    out += payload;
    return out;
}

grpc::ByteBuffer toByteBuffer(const std::string& bytes) {
    grpc::Slice slice{bytes.data(), bytes.size()};
    return grpc::ByteBuffer{&slice, 1};
}

std::string fromByteBuffer(const grpc::ByteBuffer& buffer) {
    std::vector<grpc::Slice> slices;
    buffer.Dump(&slices);
    std::string out;
    for (const auto& slice : slices) {
        out.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
    }
    return out;
}

// One decoded field: `varint` for wire type 0, `bytes` for wire type 2
// (covers both length-delimited strings and embedded sub-messages).
struct ProtoField {
    uint32_t number = 0;
    uint8_t wireType = 0;
    uint64_t varint = 0;
    std::string bytes;
};

uint64_t readVarint(std::string_view data, size_t& i) {
    uint64_t result = 0;
    int shift = 0;
    while (i < data.size()) {
        auto byte = static_cast<uint8_t>(data[i++]);
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    return result;
}

std::vector<ProtoField> parseFields(std::string_view data) {
    std::vector<ProtoField> fields;
    size_t i = 0;
    while (i < data.size()) {
        uint64_t tag = readVarint(data, i);
        ProtoField field;
        field.number = static_cast<uint32_t>(tag >> 3);
        field.wireType = static_cast<uint8_t>(tag & 0x7);
        if (field.wireType == 0) {
            field.varint = readVarint(data, i);
        } else if (field.wireType == 2) {
            uint64_t len = readVarint(data, i);
            field.bytes = std::string(data.substr(i, len));
            i += len;
        } else {
            break;  // not used by either message decoded below
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

const ProtoField* findField(const std::vector<ProtoField>& fields, uint32_t number) {
    for (const auto& f : fields) {
        if (f.number == number) return &f;
    }
    return nullptr;
}

// grpc.health.v1.Health/Check — HealthCheckRequest{string service=1}; an
// empty message (field omitted, default "") asks for overall server status.
// HealthCheckResponse{ServingStatus status=1}; SERVING == 1.
constexpr uint64_t kHealthStatusServing = 1;

grpc::Status callHealthCheck(grpc::ChannelInterface* channel, grpc::ClientContext* ctx,
                              uint64_t* statusOut) {
    grpc::ByteBuffer request = toByteBuffer("");
    grpc::ByteBuffer response;
    grpc::internal::RpcMethod method("/grpc.health.v1.Health/Check",
                                     grpc::internal::RpcMethod::NORMAL_RPC);
    grpc::Status status = grpc::internal::BlockingUnaryCall<grpc::ByteBuffer, grpc::ByteBuffer>(
        channel, method, ctx, request, &response);
    if (status.ok()) {
        auto fields = parseFields(fromByteBuffer(response));
        const auto* statusField = findField(fields, 1);
        *statusOut = statusField ? statusField->varint : 0;
    }
    return status;
}

// grpc.reflection.v1.ServerReflection/ServerReflectionInfo — bidi streaming.
// Request: ServerReflectionRequest{ oneof message_request { string
// list_services = 7; } }, field explicitly present (any value, including
// "") selects the list_services case. Response:
// ServerReflectionResponse{ oneof message_response {
// ListServiceResponse list_services_response = 6; } },
// ListServiceResponse{ repeated ServiceResponse service = 1; },
// ServiceResponse{ string name = 1; }.
std::vector<std::string> listServicesViaReflection(const std::shared_ptr<grpc::Channel>& channel,
                                                    grpc::Status* statusOut) {
    grpc::ClientContext ctx;
    grpc::internal::RpcMethod method("/grpc.reflection.v1.ServerReflection/ServerReflectionInfo",
                                     grpc::internal::RpcMethod::BIDI_STREAMING);
    std::unique_ptr<grpc::ClientReaderWriter<grpc::ByteBuffer, grpc::ByteBuffer>> stream{
        grpc::internal::ClientReaderWriterFactory<grpc::ByteBuffer, grpc::ByteBuffer>::Create(
            channel.get(), method, &ctx)};

    grpc::ByteBuffer request = toByteBuffer(encodeLengthDelimitedField(7, ""));
    std::vector<std::string> names;
    if (!stream->Write(request)) {
        *statusOut = stream->Finish();
        return names;
    }
    stream->WritesDone();

    grpc::ByteBuffer response;
    if (stream->Read(&response)) {
        auto top = parseFields(fromByteBuffer(response));
        if (const auto* listResp = findField(top, 6)) {
            for (const auto& serviceField : parseFields(listResp->bytes)) {
                if (serviceField.number != 1) continue;
                // Named, not inlined into findField's argument: a temporary
                // here would be destroyed at the end of this statement,
                // leaving nameField (a pointer into it) dangling for the
                // push_back below.
                auto serviceResponseFields = parseFields(serviceField.bytes);
                const auto* nameField = findField(serviceResponseFields, 1);
                if (nameField) names.push_back(nameField->bytes);
            }
        }
    }
    *statusOut = stream->Finish();
    return names;
}

}  // namespace

// ---------------------------------------------------------------------------
// Health check, reflection, log callback — True (positive) paths
// ---------------------------------------------------------------------------

TEST(SiLAServerOperational, HealthCheckReturnsServingAfterServerStart) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .WithDiscovery(kPortHealthCheckServing)
                      .Build();
    server.Run(false);

    auto channel = dialChannel(server, kPortHealthCheckServing);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
    uint64_t servingStatus = 0;
    auto status = callHealthCheck(channel.get(), &ctx, &servingStatus);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(servingStatus, kHealthStatusServing);

    server.Shutdown();
}

TEST(SiLAServerOperational, ReflectionListsAllRegisteredFeatureServices) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .WithAuthentication(std::make_unique<AcceptingVerifier>(),
                                          std::make_unique<AllowAllPolicy>(), {})
                      .WithDiscovery(kPortReflectionListsServices)
                      .Build();
    server.Run(false);

    auto channel = dialChannel(server, kPortReflectionListsServices);
    grpc::Status status;
    auto names = listServicesViaReflection(channel, &status);

    ASSERT_TRUE(status.ok()) << status.error_message();
    // Both feature services registered above must be independently
    // enumerable — proves reflection's registration covers more than just
    // whichever service happens to be first in FeatureRegistry.
    EXPECT_NE(std::find(names.begin(), names.end(), kSiLAServiceProtoName), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), kAuthenticationServiceProtoName),
              names.end());

    server.Shutdown();
}

TEST(SiLAServerOperational, LogCallbackReceivesDispatchEventForDispatchedCommand) {
    struct Event {
        LogLevel level;
        std::string category;
        std::string message;
    };
    std::mutex mutex;
    std::vector<Event> events;

    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .setLogCallback([&](LogLevel level, std::string_view category,
                                          std::string_view message) {
                          std::lock_guard<std::mutex> lock{mutex};
                          events.push_back(Event{level, std::string{category},
                                                 std::string{message}});
                      })
                      .WithDiscovery(kPortLogCallbackDispatch)
                      .Build();
    server.Run(false);

    // Any dispatched gRPC call goes through dispatchToHandler
    // (GrpcTransport.h), which fires logEvent(..., kInfo, "dispatch", fqi)
    // once the handler returns successfully.
    auto stub = sila2::silaservice_proto::SiLAService::NewStub(
        dialChannel(server, kPortLogCallbackDispatch));
    grpc::ClientContext ctx;
    sila2::silaservice_proto::Get_ServerUUID_Parameters req;
    sila2::silaservice_proto::Get_ServerUUID_Responses resp;
    auto status = stub->Get_ServerUUID(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();

    server.Shutdown();

    std::lock_guard<std::mutex> lock{mutex};
    auto dispatched = std::find_if(events.begin(), events.end(), [](const Event& e) {
        return e.level == LogLevel::kInfo && e.category == "dispatch";
    });
    ASSERT_NE(dispatched, events.end());
    EXPECT_FALSE(dispatched->message.empty());
}

// ---------------------------------------------------------------------------
// Health check, reflection, log callback — False (negative/rejection) paths
// ---------------------------------------------------------------------------

// CAUGHT: nothing is listening on this port, so the gRPC channel layer
// itself rejects the call — not application logic, but still the only way
// a health check request can fail for a server that was never Run().
TEST(SiLAServerOperational, HealthCheckBeforeServerStartFailsWithConnectionError) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .WithDiscovery(kPortHealthCheckBeforeStart)
                      .Build();
    // server.Run() deliberately not called.

    auto channel = dialChannel(server, kPortHealthCheckBeforeStart);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{3});
    uint64_t servingStatus = 0;
    auto status = callHealthCheck(channel.get(), &ctx, &servingStatus);

    EXPECT_FALSE(status.ok());
    EXPECT_NE(status.error_code(), grpc::StatusCode::OK);
}

// UNCAUGHT: Builder::setLogCallback(nullptr) is never rejected — logCallback_
// is a plain std::function assignment with no validity check anywhere on the
// Build()/Run() path. It is harmless only because logEvent() (LogCallback.h)
// guards every call site with `if (cb) cb(...)`, not because Build() or
// setLogCallback() themselves reject a null callback.
TEST(SiLAServerOperational, LogCallbackNullptrDoesNotCrashDispatch) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .setLogCallback(nullptr)
                      .WithDiscovery(kPortLogCallbackNullptr)
                      .Build();
    server.Run(false);

    auto stub = sila2::silaservice_proto::SiLAService::NewStub(
        dialChannel(server, kPortLogCallbackNullptr));
    grpc::ClientContext ctx;
    sila2::silaservice_proto::Get_ServerUUID_Parameters req;
    sila2::silaservice_proto::Get_ServerUUID_Responses resp;

    grpc::Status status;
    EXPECT_NO_THROW(status = stub->Get_ServerUUID(&ctx, req, &resp));
    EXPECT_TRUE(status.ok()) << status.error_message();

    server.Shutdown();
}

// Not a rejection path — no upstream code path can omit SiLAService from
// FeatureRegistry (Builder::Build() registers it unconditionally). Included
// per audit finding 3.4g's explicit request: a completeness assertion that
// reflection's enumeration is not silently missing the one service every
// SiLA2 client depends on to discover the rest, distinct from the "at least
// two services" check above.
TEST(SiLAServerOperational, ReflectionIncludesSiLAServiceAmongRegisteredServices) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .WithDiscovery(kPortReflectionIncludesSiLAService)
                      .Build();
    server.Run(false);

    auto channel = dialChannel(server, kPortReflectionIncludesSiLAService);
    grpc::Status status;
    auto names = listServicesViaReflection(channel, &status);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_NE(std::find(names.begin(), names.end(), kSiLAServiceProtoName), names.end());

    server.Shutdown();
}
