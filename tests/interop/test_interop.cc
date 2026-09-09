// test_interop.cc — integration tests for the SiLA2 interop server (§7).
// Three groups: InteropAuth, InteropBinaryTransfer, InteropDynamicEquivalence.
// All tests share a single in-process server via static-local initialization.
#include "AuthenticationTestServiceAdapter.h"
#include "BinaryTransferTestServiceAdapter.h"
#include "meta/AuthenticationTestMeta.h"
#include "meta/BinaryTransferTestMeta.h"

#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <sila/client/dynamic/DynamicCall.h>
#include <sila/client/dynamic/FeatureCatalog.h>
#include <sila/client/MetadataInjector.h>
#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/config/ServerConfig.h>
#include <sila/server/features/AuthenticationServiceImpl.h>
#include <sila/server/SilaServerBase.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/ResponseSink.h>

#include <SiLABinaryTransfer.grpc.pb.h>
#include <SiLAFramework.pb.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

namespace auth_svc = sila2::org::silastandard::core::authenticationservice::v1;
namespace auth_test = sila2::org::silastandard::test::authenticationtest::v1;
namespace binary_test = sila2::org::silastandard::test::binarytransfertest::v1;
namespace fw = sila2::org::silastandard;

// 0 = let the OS choose; the real port is read back from server().port() after
// run(). withDiscovery is currently the only way to set the listen port, so
// this harness advertises port 0 over mDNS — inert here because no test
// browses for it (see the follow-up note on Builder::withPort).
constexpr uint16_t kPort = 0;

// --- Server-side helpers (copied from server_main.cc) ----------------------

class TestCredentialVerifier final : public sila2::auth::CredentialVerifier {
public:
    std::optional<std::string> verify(
        const std::string& userIdentification,
        const std::string& password) override {
        if (userIdentification == "test" && password == "test") {
            return userIdentification;
        }
        return std::nullopt;
    }
};

// AuthenticationTest RPCs require a token; everything else is open.
class InteropAccessPolicy final : public sila2::auth::AccessPolicy {
public:
    bool isAllowed(const std::string& user, const std::string& fqi) const override {
        if (user.empty() && fqi.find("AuthenticationTest") != std::string::npos) {
            return false;
        }
        return true;
    }
    std::vector<std::string> allowedFqis(const std::string& user) const override {
        if (user.empty()) return {};
        // S14: BinaryUpload/v1 is now in the protectedFqis list passed to
        // withAuthentication() below, so a logged-in token must also be
        // scoped to it or DeleteBinary/UploadChunk calls that attach the
        // token (e.g. CreateBinaryThenDeleteThenGetInfoFails) get rejected
        // as InvalidAccessToken instead of exercising what they test.
        return {"org.silastandard/test/AuthenticationTest/v1",
                "org.silastandard/core/BinaryUpload/v1",
                "org.silastandard/core/BinaryDownload/v1"};
    }
};

struct EchoBinariesState {
    std::vector<std::string> inputBinaries;
    std::vector<std::string> echoedBinaries;
    std::string jointBinary;
    std::mutex mu;
    std::condition_variable cv;
    std::size_t echoedCount = 0;
    bool done = false;
};

// --- Server and channel singletons ----------------------------------------

sila2::SilaServerBase& server() {
    static auto s = [] {
        static sila2::ObservableCommandManager cmdManager;
        static std::mutex echoStatesMu;
        static std::unordered_map<std::string, std::shared_ptr<EchoBinariesState>> echoStates;

        sila2::SilaServerBase::Builder builder;
        builder.withSelfSignedCertificate("localhost", "127.0.0.1")
            .withConfig(std::make_unique<sila2::InMemoryServerConfig>("InteropTestServer"))
            .withBinaryTransfer()
            .withAuthentication(
                std::make_unique<TestCredentialVerifier>(),
                std::make_unique<InteropAccessPolicy>(),
                // S14: this in-process server is what the tests below actually
                // run against (not server_main.cc's own interopProtectedFqis(),
                // a separate subprocess entry point) — BinaryUpload/v1 must be
                // listed here too, or UploadDeleteBinaryWithoutTokenIsRejected
                // and friends silently exercise an unprotected feature.
                // BinaryDownload/v1 matches server_main.cc:77-78 so the two
                // interop server configs do not diverge (SC11 review).
                {"org.silastandard/test/AuthenticationTest/v1",
                 "org.silastandard/core/BinaryUpload/v1",
                 "org.silastandard/core/BinaryDownload/v1"});

        auto authAdapter =
            std::make_shared<sila2::generated::authenticationtest::AuthenticationTestServiceAdapter>(
                builder.chain());

        authAdapter->onRequiresToken = [](
                const auth_test::RequiresToken_Parameters& /*req*/,
                sila2::CallContext& /*ctx*/,
                sila2::ResponseSink<auth_test::RequiresToken_Responses>& sink) {
            auth_test::RequiresToken_Responses resp;
            sink.send(resp);
            sink.finish();
        };

        authAdapter->onRequiresTokenForBinaryUpload = [](
                const auth_test::RequiresTokenForBinaryUpload_Parameters& /*req*/,
                sila2::CallContext& /*ctx*/,
                sila2::ResponseSink<auth_test::RequiresTokenForBinaryUpload_Responses>& sink) {
            auth_test::RequiresTokenForBinaryUpload_Responses resp;
            sink.send(resp);
            sink.finish();
        };

        auto binaryAdapter =
            std::make_shared<sila2::generated::binarytransfertest::BinaryTransferTestServiceAdapter>();

        binaryAdapter->onEchoBinaryValue = [](
                const binary_test::EchoBinaryValue_Parameters& req,
                sila2::CallContext& /*ctx*/,
                sila2::ResponseSink<binary_test::EchoBinaryValue_Responses>& sink) {
            binary_test::EchoBinaryValue_Responses resp;
            *resp.mutable_receivedvalue() = req.binaryvalue();
            sink.send(resp);
            sink.finish();
        };

        binaryAdapter->onEchoBinariesObservably = [](
                const binary_test::EchoBinariesObservably_Parameters& req,
                sila2::CallContext& /*ctx*/,
                sila2::ResponseSink<fw::CommandConfirmation>& sink) {
            auto exec = cmdManager.addCommand(std::chrono::seconds{60});

            auto state = std::make_shared<EchoBinariesState>();
            for (const auto& binary : req.binaries()) {
                state->inputBinaries.push_back(binary.value());
            }

            {
                std::lock_guard<std::mutex> lock(echoStatesMu);
                echoStates[exec->uuid()] = state;
            }

            exec->start();

            // Captures the shared_ptr by value, not a raw reference — see
            // server_main.cc's onEchoBinariesObservably for the same pattern.
            std::thread([exec, state] {
                for (const auto& binary : state->inputBinaries) {
                    std::this_thread::sleep_for(std::chrono::seconds{1});
                    {
                        std::lock_guard<std::mutex> lock(state->mu);
                        state->echoedBinaries.push_back(binary);
                        state->jointBinary += binary;
                        ++state->echoedCount;
                    }
                    state->cv.notify_all();
                }
                {
                    std::lock_guard<std::mutex> lock(state->mu);
                    state->done = true;
                }
                state->cv.notify_all();
                exec->finish();
            }).detach();

            fw::CommandConfirmation confirmation;
            confirmation.mutable_commandexecutionuuid()->set_value(exec->uuid());
            if (exec->lifetime() > std::chrono::seconds{0}) {
                // Same zero-means-unset rule as server_main.cc's twin
                // handler (S18): skip the field rather than send Duration{0},
                // which the wire would read as "already expired".
                confirmation.mutable_lifetimeofexecution()->set_seconds(exec->lifetime().count());
            }
            sink.send(confirmation);
            sink.finish();
        };

        binaryAdapter->onEchoBinariesObservablyInfo = [](
                const fw::CommandExecutionUUID& req,
                sila2::CallContext& ctx,
                sila2::ResponseSink<fw::ExecutionInfo>& sink) {
            auto exec = cmdManager.getCommand(req.value());
            std::optional<fw::ExecutionInfo> lastSent;

            while (true) {
                const auto state = exec->state();

                fw::ExecutionInfo info;
                switch (state) {
                case sila2::ObservableCommandExecution::State::Waiting:
                    info.set_commandstatus(fw::ExecutionInfo_CommandStatus_waiting);
                    break;
                case sila2::ObservableCommandExecution::State::Running:
                    info.set_commandstatus(fw::ExecutionInfo_CommandStatus_running);
                    info.mutable_progressinfo()->set_value(exec->progress());
                    break;
                case sila2::ObservableCommandExecution::State::FinishedSuccessfully:
                    info.set_commandstatus(fw::ExecutionInfo_CommandStatus_finishedSuccessfully);
                    break;
                case sila2::ObservableCommandExecution::State::FinishedWithError:
                    info.set_commandstatus(fw::ExecutionInfo_CommandStatus_finishedWithError);
                    break;
                }
                if (exec->lifetime() > std::chrono::seconds{0}) {
                    // Same zero-means-unset rule as server_main.cc's twin
                    // handler (S18).
                    info.mutable_updatedlifetimeofexecution()->set_seconds(exec->lifetime().count());
                }
                // Same send-only-on-change dedup as server_main.cc's twin
                // handler, kept in lockstep (SC9 review).
                if (!lastSent || lastSent->SerializeAsString() != info.SerializeAsString()) {
                    sink.send(info);
                    lastSent = info;
                }

                const bool isFinished =
                    state == sila2::ObservableCommandExecution::State::FinishedSuccessfully ||
                    state == sila2::ObservableCommandExecution::State::FinishedWithError;
                if (ctx.isCancelled()) {
                    // Same rule as server_main.cc's onEchoBinariesObservablyInfo,
                    // this handler's byte-for-byte twin: direct gRPC cannot tell
                    // an explicit _Info cancel apart from the connection dying,
                    // so this must never interrupt the run (architecture-v2.md:286,
                    // §3.3 ②) -- just stop sending.
                    break;
                }
                if (isFinished) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
            }
            sink.finish();
        };

        binaryAdapter->onEchoBinariesObservablyIntermediate = [](
                const fw::CommandExecutionUUID& req,
                sila2::CallContext& ctx,
                sila2::ResponseSink<binary_test::EchoBinariesObservably_IntermediateResponses>& sink) {
            std::shared_ptr<EchoBinariesState> state;
            {
                std::lock_guard<std::mutex> lock(echoStatesMu);
                const auto it = echoStates.find(req.value());
                if (it != echoStates.end()) {
                    state = it->second;
                }
            }
            if (!state) {
                cmdManager.getCommand(req.value());
                sink.finish();
                return;
            }

            std::size_t sent = 0;
            std::unique_lock<std::mutex> lock(state->mu);
            while (true) {
                state->cv.wait_for(lock, std::chrono::milliseconds{200}, [&] {
                    return state->echoedCount > sent || state->done;
                });

                while (sent < state->echoedCount) {
                    binary_test::EchoBinariesObservably_IntermediateResponses resp;
                    resp.mutable_binary()->set_value(state->echoedBinaries[sent]);
                    lock.unlock();
                    sink.send(resp);
                    lock.lock();
                    ++sent;
                }

                if ((state->done && sent >= state->echoedCount) || ctx.isCancelled()) {
                    break;
                }
            }
            sink.finish();
        };

        binaryAdapter->onEchoBinariesObservablyResult = [](
                const fw::CommandExecutionUUID& req,
                sila2::CallContext& /*ctx*/,
                sila2::ResponseSink<binary_test::EchoBinariesObservably_Responses>& sink) {
            // Same state switch as server_main.cc's onEchoBinariesObservablyResult.
            auto exec = cmdManager.getCommand(req.value());
            switch (exec->state()) {
            case sila2::ObservableCommandExecution::State::Waiting:
            case sila2::ObservableCommandExecution::State::Running:
                // Part A p51: _Result before completion MUST return a
                // Command Execution Not Finished Error, not block the
                // serving thread.
                throw sila2::error::FrameworkError{
                    sila2::error::FrameworkError::FrameworkErrorType::CommandExecutionNotFinished,
                    "EchoBinariesObservably has not finished yet"};
            case sila2::ObservableCommandExecution::State::FinishedWithError:
                // Part A p51 (S68): a failed execution MUST return its
                // error, not an empty OK response. Defensive: this worker
                // never calls exec->fail(), so this branch is unreachable
                // in practice.
                throw sila2::error::UndefinedExecutionError{exec->errorMessage()};
            case sila2::ObservableCommandExecution::State::FinishedSuccessfully: {
                std::shared_ptr<EchoBinariesState> state;
                {
                    std::lock_guard<std::mutex> lock(echoStatesMu);
                    state = echoStates.at(req.value());
                }
                binary_test::EchoBinariesObservably_Responses resp;
                {
                    std::lock_guard<std::mutex> lock(state->mu);
                    resp.mutable_jointbinary()->set_value(state->jointBinary);
                }
                sink.send(resp);
                sink.finish();
                break;
            }
            }
        };

        binaryAdapter->onGetBinaryValueDirectly = [](
                const binary_test::Get_BinaryValueDirectly_Parameters& /*req*/,
                sila2::CallContext& /*ctx*/,
                sila2::ResponseSink<binary_test::Get_BinaryValueDirectly_Responses>& sink) {
            binary_test::Get_BinaryValueDirectly_Responses resp;
            resp.mutable_binaryvaluedirectly()->set_value("SiLA2_Test_String_Value");
            sink.send(resp);
            sink.finish();
        };

        binaryAdapter->onGetBinaryValueDownload = [](
                const binary_test::Get_BinaryValueDownload_Parameters& /*req*/,
                sila2::CallContext& /*ctx*/,
                sila2::ResponseSink<binary_test::Get_BinaryValueDownload_Responses>& sink) {
            static const std::string largeString = [] {
                constexpr char kChunk[] =
                    "A_slightly_longer_SiLA2_Test_String_Value_used_to_demonstrate_the_binary_download";
                constexpr std::size_t kRepeats = 100000;
                std::string value;
                value.reserve((sizeof(kChunk) - 1) * kRepeats);
                for (std::size_t i = 0; i < kRepeats; ++i) {
                    value += kChunk;
                }
                return value;
            }();

            binary_test::Get_BinaryValueDownload_Responses resp;
            resp.mutable_binaryvaluedownload()->set_value(largeString);
            sink.send(resp);
            sink.finish();
        };

        binaryAdapter->onEchoBinaryAndMetadataString = [](
                const binary_test::EchoBinaryAndMetadataString_Parameters& req,
                sila2::CallContext& ctx,
                sila2::ResponseSink<binary_test::EchoBinaryAndMetadataString_Responses>& sink) {
            binary_test::EchoBinaryAndMetadataString_Responses resp;
            *resp.mutable_binary() = req.binary();

            // Header key derived by metadataHeaderKey() from the Metadata's FQI.
            const std::optional<std::string> metadataBytes = ctx.metadata(
                sila2::metadataHeaderKey("org.silastandard/test/BinaryTransferTest/v1/Metadata/String"));
            if (metadataBytes.has_value()) {
                binary_test::Metadata_String metadata;
                if (metadata.ParseFromString(*metadataBytes)) {
                    *resp.mutable_stringmetadata() = metadata.string();
                }
            }

            sink.send(resp);
            sink.finish();
        };

        binaryAdapter->onGetFCPAffectedByMetadataString = [](
                const binary_test::Get_FCPAffectedByMetadata_String_Parameters& /*req*/,
                sila2::CallContext& /*ctx*/,
                sila2::ResponseSink<binary_test::Get_FCPAffectedByMetadata_String_Responses>& sink) {
            binary_test::Get_FCPAffectedByMetadata_String_Responses resp;
            resp.add_affectedcalls()->set_value(
                "org.silastandard/test/BinaryTransferTest/v1/Command/EchoBinaryAndMetadataString");
            sink.send(resp);
            sink.finish();
        };

        builder.addFeature("org.silastandard/test/AuthenticationTest/v1",
                           std::string{sila2::generated::authenticationtest::kFdlXml},
                           authAdapter)
            .addFeature("org.silastandard/test/BinaryTransferTest/v1",
                        std::string{sila2::generated::binarytransfertest::kFdlXml},
                        binaryAdapter)
            .withDiscovery(kPort);

        return builder.build();
    }();
    return s;
}

std::shared_ptr<grpc::Channel>& channel() {
    static auto ch = [] {
        server().run(false);
        grpc::SslCredentialsOptions opts;
        opts.pem_root_certs = server().certificatePem();
        grpc::ChannelArguments args;
        args.SetMaxReceiveMessageSize(sila2::kMaxReceiveMessageSizeBytes);
        return grpc::CreateCustomChannel(
            "localhost:" + std::to_string(server().port()),
            grpc::SslCredentials(opts), args);
    }();
    return ch;
}

// --- Login helper ---------------------------------------------------------

std::string login(const std::string& user, const std::string& password) {
    auto stub = auth_svc::AuthenticationService::NewStub(channel());
    grpc::ClientContext ctx;
    auth_svc::Login_Parameters req;
    req.mutable_useridentification()->set_value(user);
    req.mutable_password()->set_value(password);
    req.mutable_requestedserver()->set_value(server().serverConfig().uuid());
    auth_svc::Login_Responses resp;
    auto status = stub->Login(&ctx, req, &resp);
    if (!status.ok()) {
        throw std::runtime_error(status.error_message());
    }
    return resp.accesstoken().value();
}

// ===========================================================================
// Group 1: InteropAuth (§3.11)
// ===========================================================================

// --- True (positive) paths ---

TEST(InteropAuth, LoginThenCallProtectedRpc) {
    const std::string token = login("test", "test");
    auto stub = auth_test::AuthenticationTest::NewStub(channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("access-token", token);
    auth_test::RequiresToken_Parameters req;
    auth_test::RequiresToken_Responses resp;
    auto status = stub->RequiresToken(&ctx, req, &resp);
    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(InteropAuth, LoginThenCallProtectedBinaryRpc) {
    const std::string token = login("test", "test");
    auto stub = auth_test::AuthenticationTest::NewStub(channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("access-token", token);
    auth_test::RequiresTokenForBinaryUpload_Parameters req;
    req.mutable_binarytoupload()->set_value("");
    auth_test::RequiresTokenForBinaryUpload_Responses resp;
    auto status = stub->RequiresTokenForBinaryUpload(&ctx, req, &resp);
    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(InteropAuth, ValidTokenMultipleCallsSucceed) {
    const std::string token = login("test", "test");
    auto stub = auth_test::AuthenticationTest::NewStub(channel());

    {
        grpc::ClientContext ctx;
        ctx.AddMetadata("access-token", token);
        auth_test::RequiresToken_Parameters req;
        auth_test::RequiresToken_Responses resp;
        auto status = stub->RequiresToken(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << "first call: " << status.error_message();
    }
    {
        grpc::ClientContext ctx;
        ctx.AddMetadata("access-token", token);
        auth_test::RequiresToken_Parameters req;
        auth_test::RequiresToken_Responses resp;
        auto status = stub->RequiresToken(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << "second call: " << status.error_message();
    }
}

// --- False (negative/rejection) paths ---

TEST(InteropAuth, CallWithoutTokenReturnsError) {
    auto stub = auth_test::AuthenticationTest::NewStub(channel());
    grpc::ClientContext ctx;
    auth_test::RequiresToken_Parameters req;
    auth_test::RequiresToken_Responses resp;
    auto status = stub->RequiresToken(&ctx, req, &resp);
    EXPECT_FALSE(status.ok());
}

TEST(InteropAuth, CallWithInvalidTokenReturnsError) {
    auto stub = auth_test::AuthenticationTest::NewStub(channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("access-token", "bogus_token_12345");
    auth_test::RequiresToken_Parameters req;
    auth_test::RequiresToken_Responses resp;
    auto status = stub->RequiresToken(&ctx, req, &resp);
    EXPECT_FALSE(status.ok());
}

TEST(InteropAuth, LoginWithWrongPasswordFails) {
    EXPECT_THROW(login("test", "wrong"), std::runtime_error);
}

// ===========================================================================
// Group 2: InteropBinaryTransfer (§4.6)
// ===========================================================================

// --- True (positive) paths ---

TEST(InteropBinaryTransfer, GetBinaryValueDirectlyReturnsFixedString) {
    auto stub = binary_test::BinaryTransferTest::NewStub(channel());
    grpc::ClientContext ctx;
    binary_test::Get_BinaryValueDirectly_Parameters req;
    binary_test::Get_BinaryValueDirectly_Responses resp;
    auto status = stub->Get_BinaryValueDirectly(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.binaryvaluedirectly().value(), "SiLA2_Test_String_Value");
}

TEST(InteropBinaryTransfer, EchoBinaryValueSmallInline) {
    auto stub = binary_test::BinaryTransferTest::NewStub(channel());
    grpc::ClientContext ctx;
    binary_test::EchoBinaryValue_Parameters req;
    req.mutable_binaryvalue()->set_value("hello");
    binary_test::EchoBinaryValue_Responses resp;
    auto status = stub->EchoBinaryValue(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.receivedvalue().value(), "hello");
}

TEST(InteropBinaryTransfer, GetBinaryValueDownloadReturnsLargePayload) {
    auto stub = binary_test::BinaryTransferTest::NewStub(channel());
    grpc::ClientContext ctx;
    binary_test::Get_BinaryValueDownload_Parameters req;
    binary_test::Get_BinaryValueDownload_Responses resp;
    auto status = stub->Get_BinaryValueDownload(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_GT(resp.binaryvaluedownload().value().size(), 1000000u);
}

// End-to-end proof that MetadataInjector's derived key (client side) and the
// server's onEchoBinaryAndMetadataString handler (also keyed via
// metadataHeaderKey(), see the lambda above) agree across the wire.
TEST(InteropBinaryTransfer, EchoBinaryAndMetadataStringReturnsInjectedMetadata) {
    binary_test::Metadata_String metadata;
    metadata.mutable_string()->set_value("hello");
    sila2::MetadataInjector injector;
    injector.set("org.silastandard/test/BinaryTransferTest/v1/Metadata/String",
                 metadata.SerializeAsString());

    auto stub = binary_test::BinaryTransferTest::NewStub(channel());
    grpc::ClientContext ctx;
    injector.apply(ctx);
    binary_test::EchoBinaryAndMetadataString_Parameters req;
    req.mutable_binary()->set_value("payload");
    binary_test::EchoBinaryAndMetadataString_Responses resp;
    auto status = stub->EchoBinaryAndMetadataString(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.stringmetadata().value(), "hello");
}

// --- False (negative/rejection) paths ---

TEST(InteropBinaryTransfer, EchoBinaryAndMetadataStringWithoutMetadataReturnsEmptyString) {
    // Not required metadata (see S5): a call without it still succeeds.
    auto stub = binary_test::BinaryTransferTest::NewStub(channel());
    grpc::ClientContext ctx;
    binary_test::EchoBinaryAndMetadataString_Parameters req;
    req.mutable_binary()->set_value("payload");
    binary_test::EchoBinaryAndMetadataString_Responses resp;
    auto status = stub->EchoBinaryAndMetadataString(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.stringmetadata().value(), "");
}

TEST(InteropBinaryTransfer, GetBinaryInfoInvalidUuidReturnsError) {
    // BinaryDownload/v1 is now in the in-process protectedFqis list (SC11
    // review) -- attach a token so this keeps failing for the invalid UUID,
    // not for the missing token.
    const std::string token = login("test", "test");
    auto stub = fw::BinaryDownload::NewStub(channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("access-token", token);
    fw::GetBinaryInfoRequest req;
    req.set_binarytransferuuid("00000000-0000-0000-0000-000000000000");
    fw::GetBinaryInfoResponse resp;
    auto status = stub->GetBinaryInfo(&ctx, req, &resp);
    EXPECT_FALSE(status.ok());
}

TEST(InteropBinaryTransfer, DeleteBinaryInvalidUuidReturnsError) {
    // S14: BinaryUpload/v1 is in the in-process server()'s protectedFqis
    // list above -- the server these tests run against (server_main.cc:77 is
    // its subprocess twin) -- so DeleteBinary now gates on a token too
    // (BinaryUploadService.cc). Attach one so this keeps failing for
    // INVALID_BINARY_TRANSFER_UUID, the reason it was written for, not for
    // the new auth reason.
    const std::string token = login("test", "test");
    auto stub = fw::BinaryUpload::NewStub(channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("access-token", token);
    fw::DeleteBinaryRequest req;
    req.set_binarytransferuuid("00000000-0000-0000-0000-000000000000");
    fw::DeleteBinaryResponse resp;
    auto status = stub->DeleteBinary(&ctx, req, &resp);
    EXPECT_FALSE(status.ok());
}

TEST(InteropBinaryTransfer, CreateBinaryThenDeleteThenGetInfoFails) {
    auto uploadStub = fw::BinaryUpload::NewStub(channel());
    auto downloadStub = fw::BinaryDownload::NewStub(channel());

    // CreateBinary to obtain a valid UUID.
    std::string uuid;
    {
        grpc::ClientContext ctx;
        fw::CreateBinaryRequest req;
        // S13: CreateBinary now requires parameterIdentifier to name a
        // Command Parameter of a Feature registered on this server. Use a
        // real, unprotected FQI (the in-process server()'s protectedFqis
        // list does not cover BinaryTransferTest) rather than the default-empty
        // field this test used to send.
        req.set_parameteridentifier(
            "org.silastandard/test/BinaryTransferTest/v1/Command/EchoBinaryValue/Parameter/BinaryValue");
        fw::CreateBinaryResponse resp;
        auto status = uploadStub->CreateBinary(&ctx, req, &resp);
        ASSERT_TRUE(status.ok()) << status.error_message();
        uuid = resp.binarytransferuuid();
        ASSERT_FALSE(uuid.empty());
    }

    // DeleteBinary with the valid UUID.
    {
        // S14: BinaryUpload/v1 is in the in-process server()'s protectedFqis
        // list above (not server_main.cc's -- that subprocess twin never
        // serves these tests), so DeleteBinary now gates on a token
        // (BinaryUploadService.cc), the same as CreateBinary already did.
        const std::string token = login("test", "test");
        grpc::ClientContext ctx;
        ctx.AddMetadata("access-token", token);
        fw::DeleteBinaryRequest req;
        req.set_binarytransferuuid(uuid);
        fw::DeleteBinaryResponse resp;
        auto status = uploadStub->DeleteBinary(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
    }

    // GetBinaryInfo on the deleted UUID should fail. Tokened for the same
    // reason as the DeleteBinary block above, now that BinaryDownload/v1 is
    // protected here too (SC11 review).
    {
        const std::string token = login("test", "test");
        grpc::ClientContext ctx;
        ctx.AddMetadata("access-token", token);
        fw::GetBinaryInfoRequest req;
        req.set_binarytransferuuid(uuid);
        fw::GetBinaryInfoResponse resp;
        auto status = downloadStub->GetBinaryInfo(&ctx, req, &resp);
        EXPECT_FALSE(status.ok());
    }
}

// S13: a parameterIdentifier that names no Feature registered on this server
// (or is not a fully qualified identifier at all) is rejected before a slot
// is ever created.
TEST(InteropBinaryTransfer, CreateBinaryWithBogusParameterIdentifierReturnsError) {
    auto uploadStub = fw::BinaryUpload::NewStub(channel());

    grpc::ClientContext ctx;
    fw::CreateBinaryRequest req;
    req.set_parameteridentifier("not-an-fqi");
    fw::CreateBinaryResponse resp;
    auto status = uploadStub->CreateBinary(&ctx, req, &resp);

    EXPECT_FALSE(status.ok());
}

// S14: BinaryUpload/v1 is protected by the in-process server() these tests
// run against (and by server_main.cc's subprocess twin) but, until this item,
// DeleteBinary's own dispatchToHandler call hardcoded chain=nullptr
// (BinaryUploadService.cc) and never checked it — the RPC the earlier audit
// round missed.
TEST(InteropBinaryTransfer, UploadDeleteBinaryWithoutTokenIsRejected) {
    auto uploadStub = fw::BinaryUpload::NewStub(channel());

    // CreateBinary via an unprotected FQI (the in-process protectedFqis list
    // does not cover BinaryTransferTest), no token needed — same as
    // CreateBinaryThenDeleteThenGetInfoFails above.
    std::string uuid;
    {
        grpc::ClientContext ctx;
        fw::CreateBinaryRequest req;
        req.set_parameteridentifier(
            "org.silastandard/test/BinaryTransferTest/v1/Command/EchoBinaryValue/Parameter/BinaryValue");
        fw::CreateBinaryResponse resp;
        auto status = uploadStub->CreateBinary(&ctx, req, &resp);
        ASSERT_TRUE(status.ok()) << status.error_message();
        uuid = resp.binarytransferuuid();
        ASSERT_FALSE(uuid.empty());
    }

    grpc::ClientContext ctx;  // no access-token metadata at all
    fw::DeleteBinaryRequest req;
    req.set_binarytransferuuid(uuid);
    fw::DeleteBinaryResponse resp;
    auto status = uploadStub->DeleteBinary(&ctx, req, &resp);

    // Pin the rejection reason like the gRPC twin
    // (test_grpc_transport_auth_e2e.cc's UploadDeleteBinaryWithoutTokenIsRejected):
    // a bare EXPECT_FALSE would stay green if DeleteBinary started failing for
    // an unrelated reason (e.g. INVALID_BINARY_TRANSFER_UUID on a swept slot).
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = sila2::error::fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), sila2::error::SilaError::ErrorType::FrameworkError);
    const auto* err = dynamic_cast<const sila2::error::FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(),
              sila2::error::FrameworkError::FrameworkErrorType::InvalidMetadata);
}

// ===========================================================================
// Group 3: InteropDynamicEquivalence (§4.3)
// ===========================================================================

constexpr char kBinaryTransferTestFqi[] =
    "org.silastandard/test/BinaryTransferTest/v1";

sila2::dynamic::FeatureCatalog& catalog() {
    // FeatureCatalog has a user-declared dtor → implicit move suppressed.
    // Heap-allocate to avoid copy/move; leak is fine for test process.
    static auto* cat = [] {
        auto* c = new sila2::dynamic::FeatureCatalog{
            *google::protobuf::DescriptorPool::generated_pool()};
        c->add(kBinaryTransferTestFqi,
               std::string{sila2::generated::binarytransfertest::kFdlXml});
        return c;
    }();
    return *cat;
}

// --- True (positive) paths ---

TEST(InteropDynamicEquivalence, StaticAndDynamicGetBinaryValueDirectlyMatch) {
    // Static call.
    auto stub = binary_test::BinaryTransferTest::NewStub(channel());
    grpc::ClientContext staticCtx;
    binary_test::Get_BinaryValueDirectly_Parameters staticReq;
    binary_test::Get_BinaryValueDirectly_Responses staticResp;
    auto staticStatus = stub->Get_BinaryValueDirectly(&staticCtx, staticReq, &staticResp);
    ASSERT_TRUE(staticStatus.ok()) << staticStatus.error_message();
    const std::string staticSerialized = staticResp.SerializeAsString();

    // Dynamic call.
    auto& cat = catalog();
    const std::string method = cat.grpcMethodName(
        kBinaryTransferTestFqi, "Get_BinaryValueDirectly");
    const auto* reqDesc = cat.requestDescriptor(
        kBinaryTransferTestFqi, "Get_BinaryValueDirectly");
    std::unique_ptr<google::protobuf::Message> dynReq{
        cat.messageFactory().GetPrototype(reqDesc)->New()};

    grpc::ByteBuffer reqBuf;
    {
        const std::string serialized = dynReq->SerializeAsString();
        grpc::Slice slice(serialized);
        reqBuf = grpc::ByteBuffer(&slice, 1);
    }

    grpc::ByteBuffer respBuf;
    auto dynStatus = sila2::dynamic::callUnary(
        channel(), method, reqBuf, &respBuf);
    ASSERT_TRUE(dynStatus.ok()) << dynStatus.error_message();

    std::string dynSerialized;
    std::vector<grpc::Slice> slices;
    respBuf.Dump(&slices);
    for (const auto& s : slices) {
        dynSerialized.append(reinterpret_cast<const char*>(s.begin()), s.size());
    }

    EXPECT_EQ(staticSerialized, dynSerialized);
}

TEST(InteropDynamicEquivalence, StaticAndDynamicEchoBinaryValueMatch) {
    // Static call.
    auto stub = binary_test::BinaryTransferTest::NewStub(channel());
    grpc::ClientContext staticCtx;
    binary_test::EchoBinaryValue_Parameters staticReq;
    staticReq.mutable_binaryvalue()->set_value("dynamic_test");
    binary_test::EchoBinaryValue_Responses staticResp;
    auto staticStatus = stub->EchoBinaryValue(&staticCtx, staticReq, &staticResp);
    ASSERT_TRUE(staticStatus.ok()) << staticStatus.error_message();
    const std::string staticSerialized = staticResp.SerializeAsString();

    // Dynamic call with the same input.
    auto& cat = catalog();
    const std::string method = cat.grpcMethodName(
        kBinaryTransferTestFqi, "EchoBinaryValue");
    const auto* reqDesc = cat.requestDescriptor(
        kBinaryTransferTestFqi, "EchoBinaryValue");
    std::unique_ptr<google::protobuf::Message> dynReq{
        cat.messageFactory().GetPrototype(reqDesc)->New()};

    // Set the BinaryValue field — field 1, containing a Binary with value.
    const auto* binaryField = reqDesc->FindFieldByName("BinaryValue");
    ASSERT_NE(binaryField, nullptr);
    const auto* binaryDesc = binaryField->message_type();
    auto* binaryMsg = dynReq->GetReflection()->MutableMessage(
        dynReq.get(), binaryField);
    const auto* valueField = binaryDesc->FindFieldByName("value");
    ASSERT_NE(valueField, nullptr);
    binaryMsg->GetReflection()->SetString(binaryMsg, valueField, "dynamic_test");

    grpc::ByteBuffer reqBuf;
    {
        const std::string serialized = dynReq->SerializeAsString();
        grpc::Slice slice(serialized);
        reqBuf = grpc::ByteBuffer(&slice, 1);
    }

    grpc::ByteBuffer respBuf;
    auto dynStatus = sila2::dynamic::callUnary(
        channel(), method, reqBuf, &respBuf);
    ASSERT_TRUE(dynStatus.ok()) << dynStatus.error_message();

    std::string dynSerialized;
    std::vector<grpc::Slice> slices;
    respBuf.Dump(&slices);
    for (const auto& s : slices) {
        dynSerialized.append(reinterpret_cast<const char*>(s.begin()), s.size());
    }

    EXPECT_EQ(staticSerialized, dynSerialized);
}

TEST(InteropDynamicEquivalence, DynamicCallGetBinaryValueDirectlySucceeds) {
    auto& cat = catalog();
    const std::string method = cat.grpcMethodName(
        kBinaryTransferTestFqi, "Get_BinaryValueDirectly");
    const auto* reqDesc = cat.requestDescriptor(
        kBinaryTransferTestFqi, "Get_BinaryValueDirectly");
    const auto* respDesc = cat.responseDescriptor(
        kBinaryTransferTestFqi, "Get_BinaryValueDirectly");
    std::unique_ptr<google::protobuf::Message> dynReq{
        cat.messageFactory().GetPrototype(reqDesc)->New()};

    grpc::ByteBuffer reqBuf;
    {
        const std::string serialized = dynReq->SerializeAsString();
        grpc::Slice slice(serialized);
        reqBuf = grpc::ByteBuffer(&slice, 1);
    }

    grpc::ByteBuffer respBuf;
    auto status = sila2::dynamic::callUnary(
        channel(), method, reqBuf, &respBuf);
    ASSERT_TRUE(status.ok()) << status.error_message();

    std::unique_ptr<google::protobuf::Message> dynResp{
        cat.messageFactory().GetPrototype(respDesc)->New()};
    std::string raw;
    std::vector<grpc::Slice> slices;
    respBuf.Dump(&slices);
    for (const auto& s : slices) {
        raw.append(reinterpret_cast<const char*>(s.begin()), s.size());
    }
    EXPECT_TRUE(dynResp->ParseFromString(raw));
}

// --- False (negative/rejection) paths ---

TEST(InteropDynamicEquivalence, DynamicCallWithWrongMethodFails) {
    grpc::ByteBuffer emptyReq;
    grpc::ByteBuffer respBuf;
    auto status = sila2::dynamic::callUnary(
        channel(),
        "/sila2.org.silastandard.test.binarytransfertest.v1.BinaryTransferTest/NonExistentRpc",
        emptyReq, &respBuf);
    EXPECT_FALSE(status.ok());
}

TEST(InteropDynamicEquivalence, FeatureCatalogAddMalformedFdlThrows) {
    sila2::dynamic::FeatureCatalog cat{
        *google::protobuf::DescriptorPool::generated_pool()};
    EXPECT_THROW(
        cat.add("org.silastandard/test/Bogus/v1", "<this is not valid xml"),
        std::invalid_argument);
}

TEST(InteropDynamicEquivalence, FeatureCatalogLookupUnknownRpcThrows) {
    // grpcMethodName does not validate the RPC name (string concatenation only),
    // but requestDescriptor routes through findMethod which does.
    auto& cat = catalog();
    EXPECT_THROW(
        (void)cat.requestDescriptor(kBinaryTransferTestFqi, "CompletelyFakeRpc"),
        std::invalid_argument);
}

// ===========================================================================
// Group 4: InteropServerLimits (1.4g, 2.1j)
// ===========================================================================

TEST(InteropServerLimits, BoundPortIsReportedAfterRun) {
    channel();  // forces the one-time server().run(false)
    EXPECT_NE(server().port(), 0);
}

TEST(InteropServerLimits, RequestOverGrpcDefaultButUnderCapIsAccepted) {
    // 6 MB is above gRPC's implicit 4 MB receive default and below the
    // explicit 16 MB cap, so this fails if SetMaxReceiveMessageSize is lost.
    constexpr std::size_t kPayloadBytes = 6 * 1024 * 1024;
    auto stub = binary_test::BinaryTransferTest::NewStub(channel());
    grpc::ClientContext ctx;
    binary_test::EchoBinaryValue_Parameters req;
    req.mutable_binaryvalue()->set_value(std::string(kPayloadBytes, 'x'));
    binary_test::EchoBinaryValue_Responses resp;
    auto status = stub->EchoBinaryValue(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.receivedvalue().value().size(), kPayloadBytes);
}

TEST(InteropServerLimits, RequestOverCapIsRejected) {
    auto stub = binary_test::BinaryTransferTest::NewStub(channel());
    grpc::ClientContext ctx;
    binary_test::EchoBinaryValue_Parameters req;
    req.mutable_binaryvalue()->set_value(
        std::string(sila2::kMaxReceiveMessageSizeBytes + 1024, 'x'));
    binary_test::EchoBinaryValue_Responses resp;
    auto status = stub->EchoBinaryValue(&ctx, req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED)
        << status.error_code() << ": " << status.error_message();
}

}  // namespace
