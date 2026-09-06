// Tests for DynamicCall.cc's SiLAService metadata skip (S5, SiLA 2 Part A:
// "every call of the SiLA Service Feature MUST NOT contain any SiLA Client
// Metadata"): callUnary/callServerStream must not attach an injector's
// entries when the method path targets SiLAService, but must still attach
// them for every other call.
//
// GenericDynamicTestServer (used by test_dynamic_call_e2e.cc) has no way to
// observe client_metadata() from its per-method handler callback, so this
// file runs its own minimal AsyncGenericService server that captures
// whatever metadata arrived on the most recent call. It answers every call
// with one message + OK, matching the wire shape (Read once, Write N,
// Finish) that both callUnary and callServerStream drive.
#include <sila/client/dynamic/DynamicCall.h>

#include <sila/client/MetadataInjector.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/support/async_stream.h>
#include <grpcpp/support/byte_buffer.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
using sila2::MetadataInjector;
using sila2::dynamic::callServerStream;
using sila2::dynamic::callUnary;

// A SiLAService method path and a non-SiLAService one, both plausible RPC
// names under their respective services -- the guard matches on the prefix,
// not a specific method, so the exact suffix is arbitrary.
const std::string kSiLAServiceUnaryMethod =
    "/sila2.org.silastandard.core.silaservice.v1.SiLAService/Get_ServerName";
// A second SiLAService method distinct from the first, to pin that the
// guard matches the service prefix rather than one hard-coded method name.
const std::string kSiLAServiceSecondMethod =
    "/sila2.org.silastandard.core.silaservice.v1.SiLAService/SetServerName";
const std::string kOrdinaryUnaryMethod = "/some.other.Feature/DoWork";
const std::string kOrdinaryStreamMethod = "/some.other.Feature/Subscribe";

const std::string kLockIdentifierFqi = "org.silastandard/core/LockController/v1/Metadata/LockIdentifier";
const std::string kAccessTokenFqi = "org.silastandard/core/AuthorizationService/v1/Metadata/AccessToken";

grpc::ByteBuffer bufferFromString(const std::string& value) {
    grpc::Slice slice(value);
    return grpc::ByteBuffer(&slice, 1);
}

// Any "sila-*-bin" key is one MetadataInjector::apply() could have
// attached (see MetadataHeaderKey.h); this is what a rejection case must
// find absent, rather than asserting on one specific FQI's derived key, so
// it also catches a guard that only strips some entries.
bool anySilaBinHeader(const std::multimap<std::string, std::string>& received) {
    for (const auto& [key, value] : received) {
        (void)value;
        if (key.starts_with("sila-") && key.ends_with("-bin")) {
            return true;
        }
    }
    return false;
}

// Minimal in-process AsyncGenericService server that records the client
// metadata of the most recent call and answers every call with one
// message + OK. Runs its own accept loop single-threaded: tests issue one
// call at a time and wait for it to finish before inspecting
// receivedMetadata(), so there is never a concurrent call to race.
class MetadataCapturingGenericServer {
public:
    MetadataCapturingGenericServer() {
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
        builder.RegisterAsyncGenericService(&service_);
        notificationCq_ = builder.AddCompletionQueue();
        server_ = builder.BuildAndStart();
        acceptThread_ = std::thread([this] { acceptLoop(); });
    }

    ~MetadataCapturingGenericServer() {
        server_->Shutdown();
        notificationCq_->Shutdown();
        acceptThread_.join();
    }

    MetadataCapturingGenericServer(const MetadataCapturingGenericServer&) = delete;
    MetadataCapturingGenericServer& operator=(const MetadataCapturingGenericServer&) = delete;

    std::shared_ptr<grpc::Channel> channel() const {
        return grpc::CreateChannel("127.0.0.1:" + std::to_string(port_), grpc::InsecureChannelCredentials());
    }

    std::multimap<std::string, std::string> receivedMetadata() const {
        std::lock_guard<std::mutex> lock(mu_);
        return receivedMetadata_;
    }

private:
    // Single-threaded accept-and-serve loop: RequestCall, wait for a call,
    // read its one request message, capture ctx->client_metadata(), write
    // one response message, Finish OK, repeat. No per-call thread or
    // per-method script (unlike GenericDynamicTestServer) since every test
    // here only needs one canned round trip and the metadata it arrived
    // with.
    void acceptLoop() {
        while (true) {
            auto ctx = std::make_unique<grpc::GenericServerContext>();
            auto stream = std::make_unique<grpc::GenericServerAsyncReaderWriter>(ctx.get());
            grpc::CompletionQueue callCq;
            void* acceptTag = ctx.get();
            service_.RequestCall(ctx.get(), stream.get(), &callCq, notificationCq_.get(), acceptTag);

            void* tag = nullptr;
            bool ok = false;
            if (!notificationCq_->Next(&tag, &ok) || !ok) {
                break;  // server shutting down
            }

            grpc::ByteBuffer request;
            stream->Read(&request, tag);
            if (!callCq.Next(&tag, &ok) || !ok) {
                callCq.Shutdown();
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                receivedMetadata_.clear();
                for (const auto& [key, value] : ctx->client_metadata()) {
                    receivedMetadata_.emplace(
                        std::string(key.data(), key.length()), std::string(value.data(), value.length()));
                }
            }

            grpc::ByteBuffer response = bufferFromString("ok");
            stream->Write(response, tag);
            callCq.Next(&tag, &ok);

            stream->Finish(grpc::Status::OK, tag);
            callCq.Next(&tag, &ok);
            callCq.Shutdown();
        }
    }

    int port_ = 0;
    grpc::AsyncGenericService service_;
    std::unique_ptr<grpc::ServerCompletionQueue> notificationCq_;
    std::unique_ptr<grpc::Server> server_;
    std::thread acceptThread_;

    mutable std::mutex mu_;
    std::multimap<std::string, std::string> receivedMetadata_;
};

// ---------------------------------------------------------------------------
// True (positive) paths: an ordinary (non-SiLAService) call still gets the
// injector's metadata -- the guard must not disturb it.
// ---------------------------------------------------------------------------

TEST(DynamicCallSiLAServiceMetadata, MetadataReachesANonSiLAServiceUnaryCall) {
    MetadataCapturingGenericServer server;
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "lock-value");

    grpc::ByteBuffer response;
    auto status = callUnary(
        server.channel(), kOrdinaryUnaryMethod, bufferFromString("request"), &response, &injector);

    EXPECT_TRUE(status.ok()) << status.error_message();
    const auto received = server.receivedMetadata();
    const auto it = received.find(MetadataInjector::headerKey(kLockIdentifierFqi));
    ASSERT_NE(it, received.end());
    EXPECT_EQ(it->second, "lock-value");
}

TEST(DynamicCallSiLAServiceMetadata, MetadataReachesANonSiLAServiceServerStreamCall) {
    MetadataCapturingGenericServer server;
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "lock-value");

    auto status = callServerStream(
        server.channel(), kOrdinaryStreamMethod, bufferFromString("request"),
        [](const grpc::ByteBuffer&) { return true; }, &injector);

    EXPECT_TRUE(status.ok()) << status.error_message();
    const auto received = server.receivedMetadata();
    const auto it = received.find(MetadataInjector::headerKey(kLockIdentifierFqi));
    ASSERT_NE(it, received.end());
    EXPECT_EQ(it->second, "lock-value");
}

TEST(DynamicCallSiLAServiceMetadata, EveryInjectorEntryReachesANonSiLAServiceCall) {
    MetadataCapturingGenericServer server;
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "lock-value");
    injector.set(kAccessTokenFqi, "token-value");

    grpc::ByteBuffer response;
    auto status = callUnary(
        server.channel(), kOrdinaryUnaryMethod, bufferFromString("request"), &response, &injector);

    EXPECT_TRUE(status.ok()) << status.error_message();
    const auto received = server.receivedMetadata();
    EXPECT_EQ(received.find(MetadataInjector::headerKey(kLockIdentifierFqi))->second, "lock-value");
    EXPECT_EQ(received.find(MetadataInjector::headerKey(kAccessTokenFqi))->second, "token-value");
}

// ---------------------------------------------------------------------------
// False (rejection) paths: a SiLAService call must receive no SiLA Client
// Metadata at all, even with entries seeded on the injector. Uncaught
// before this item -- the injector attached to every call.
// ---------------------------------------------------------------------------

TEST(DynamicCallSiLAServiceMetadata, NoMetadataReachesASiLAServiceUnaryCall) {
    MetadataCapturingGenericServer server;
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "lock-value");
    injector.set(kAccessTokenFqi, "token-value");

    grpc::ByteBuffer response;
    auto status = callUnary(
        server.channel(), kSiLAServiceUnaryMethod, bufferFromString("request"), &response, &injector);

    // Assert on absence, not on an error status: a guard bug that instead
    // rejected the call outright would pass an EXPECT_FALSE(status.ok())
    // check while still shipping metadata, so the call completing normally
    // is asserted alongside the absence check.
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(anySilaBinHeader(server.receivedMetadata()));
}

TEST(DynamicCallSiLAServiceMetadata, NoMetadataReachesASiLAServiceServerStreamCall) {
    MetadataCapturingGenericServer server;
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "lock-value");
    injector.set(kAccessTokenFqi, "token-value");

    auto status = callServerStream(
        server.channel(), kSiLAServiceUnaryMethod, bufferFromString("request"),
        [](const grpc::ByteBuffer&) { return true; }, &injector);

    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(anySilaBinHeader(server.receivedMetadata()));
}

TEST(DynamicCallSiLAServiceMetadata, NoMetadataReachesASecondSiLAServiceMethod) {
    // Pins that the guard matches the SiLAService prefix, not one
    // hard-coded method name.
    MetadataCapturingGenericServer server;
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "lock-value");

    grpc::ByteBuffer response;
    auto status = callUnary(
        server.channel(), kSiLAServiceSecondMethod, bufferFromString("request"), &response, &injector);

    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(anySilaBinHeader(server.receivedMetadata()));
}

}  // namespace
