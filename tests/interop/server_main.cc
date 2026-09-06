// server_main.cc — standalone SiLA2 server exercising AuthenticationTest-v1_0
// and BinaryTransferTest-v1_0 (tests/interop/CMakeLists.txt), for validation
// suites and interop testing against a real client.
#include "AuthenticationTestServiceAdapter.h"
#include "BinaryTransferTestServiceAdapter.h"
#include "meta/AuthenticationTestMeta.h"
#include "meta/BinaryTransferTestMeta.h"

#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <fstream>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/auth/DenyByDefaultAccessPolicy.h>
#include <sila/server/config/ServerConfig.h>
#include <sila/server/SiLAServerBase.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/ResponseSink.h>

#include <SiLAFramework.pb.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

std::atomic<bool> gTerminateRequested{false};

// SIGTERM must not kill the process outright — audit 4.4a needs main() to
// return so SiLAServerBase destructs without an explicit Shutdown() call.
// relaxed is enough: the only cross-thread requirement is that the polling
// loop below eventually observes this flag, not ordering vs. other state.
void handleSigterm(int /*signum*/) {
    gTerminateRequested.store(true, std::memory_order_relaxed);
}

}  // namespace

namespace {

// AuthenticationTest FDL (third_party/sila_base/.../AuthenticationTest-v1_0.sila.xml):
// "username: 'test', password: 'test'".
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

// Protected FQIs: AuthenticationTest exercises the access-token boundary.
// BinaryTransferTest stays public because sila2-python's observable command
// follow-up RPCs do not carry metadata from command initiation.
std::vector<std::string> interopProtectedFqis() {
    // SiLAService is NOT protected: the SiLA 2 spec requires feature discovery
    // to be accessible pre-auth so clients can enumerate features before login.
    return {
        "org.silastandard/core/AuthorizationConfigurationService/v1",
        "org.silastandard/core/ErrorRecoveryService/v2",
        "org.silastandard/core/BinaryUpload/v1",
        "org.silastandard/core/BinaryDownload/v1",
        "org.silastandard/test/AuthenticationTest/v1",
    };
}

// Shared state for a single EchoBinariesObservably execution, keyed by
// command execution UUID (echoStates below). The worker thread writes
// echoedBinaries/jointBinary/done under mu and notifies cv; the _Intermediate
// and _Result RPC handlers read them under the same mutex.
struct EchoBinariesState {
    std::vector<std::string> inputBinaries;
    std::vector<std::string> echoedBinaries;
    std::string jointBinary;
    std::mutex mu;
    std::condition_variable cv;
    std::size_t echoedCount = 0;
    bool done = false;
};

struct ServerArgs {
    std::uint16_t port = 50052;
    std::string certOut;
    std::string hostname = "localhost";
    std::string ip = "127.0.0.1";
};

ServerArgs parseArgs(int argc, char* argv[]) {
    ServerArgs args;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string{argv[i]} == "--port") {
            args.port = static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
        } else if (std::string{argv[i]} == "--cert-out") {
            args.certOut = argv[i + 1];
        } else if (std::string{argv[i]} == "--hostname") {
            args.hostname = argv[i + 1];
        } else if (std::string{argv[i]} == "--ip") {
            args.ip = argv[i + 1];
        }
    }
    return args;
}

}  // namespace

int main(int argc, char* argv[]) {
    namespace auth_proto = sila2::org::silastandard::test::authenticationtest::v1;
    namespace binary_proto = sila2::org::silastandard::test::binarytransfertest::v1;
    namespace fw = sila2::org::silastandard;

    const auto args = parseArgs(argc, argv);
    const std::uint16_t port = args.port;

    sila2::ObservableCommandManager cmdManager;
    std::mutex echoStatesMu;
    std::unordered_map<std::string, std::shared_ptr<EchoBinariesState>> echoStates;

    sila2::SiLAServerBase::Builder builder;
    builder.WithSelfSignedCertificate(args.hostname, args.ip)
        .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("InteropTestServer"))
        .WithBinaryTransfer()
        .WithAuthentication(
            std::make_unique<TestCredentialVerifier>(),
            std::make_unique<sila2::auth::DenyByDefaultAccessPolicy>(
                interopProtectedFqis()),
            interopProtectedFqis());

    auto authAdapter =
        std::make_shared<sila2::generated::authenticationtest::AuthenticationTestServiceAdapter>(
            builder.chain());

    // The authorization interceptor (§3.11) already rejected the call before
    // the handler runs if the token was missing or invalid, so reaching here
    // means the token check already passed — nothing left to verify.
    authAdapter->onRequiresToken = [](
            const auth_proto::RequiresToken_Parameters& /*req*/,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<auth_proto::RequiresToken_Responses>& sink) {
        auth_proto::RequiresToken_Responses resp;
        sink.send(resp);
        sink.finish();
    };

    authAdapter->onRequiresTokenForBinaryUpload = [](
            const auth_proto::RequiresTokenForBinaryUpload_Parameters& /*req*/,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<auth_proto::RequiresTokenForBinaryUpload_Responses>& sink) {
        auth_proto::RequiresTokenForBinaryUpload_Responses resp;
        sink.send(resp);
        sink.finish();
    };

    auto binaryAdapter =
        std::make_shared<sila2::generated::binarytransfertest::BinaryTransferTestServiceAdapter>(
            builder.chain());

    binaryAdapter->onEchoBinaryValue = [](
            const binary_proto::EchoBinaryValue_Parameters& req,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<binary_proto::EchoBinaryValue_Responses>& sink) {
        binary_proto::EchoBinaryValue_Responses resp;
        *resp.mutable_receivedvalue() = req.binaryvalue();
        sink.send(resp);
        sink.finish();
    };

    binaryAdapter->onEchoBinariesObservably = [&cmdManager, &echoStatesMu, &echoStates](
            const binary_proto::EchoBinariesObservably_Parameters& req,
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

        // Detached worker thread: captures the shared_ptr by value (not a
        // raw reference) so it keeps the execution alive on its own,
        // regardless of when this thread finishes relative to cmdManager's
        // GC sweep; state is kept alive the same way by its own shared_ptr copy.
        std::thread([exec, state] {
            for (const auto& binary : state->inputBinaries) {
                // FDL: "echoes them individually as intermediate responses
                // with a delay of 1 second".
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
            // Same zero-means-unset rule as ShakeControllerImpl.cc's
            // onShakeForTime: skip the field rather than send Duration{0},
            // which the wire would read as "already expired".
            confirmation.mutable_lifetimeofexecution()->set_seconds(exec->lifetime().count());
        }
        sink.send(confirmation);
        sink.finish();
    };

    binaryAdapter->onEchoBinariesObservablyInfo = [&cmdManager](
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
                // Same zero-means-unset rule as ShakeControllerImpl.cc's
                // onShakeForTimeInfo.
                info.mutable_updatedlifetimeofexecution()->set_seconds(exec->lifetime().count());
            }
            // Same send-only-on-change policy as ShakeControllerImpl.cc's
            // onShakeForTimeInfo, which this handler cites as its rule -- one
            // envelope per 200ms poll was the cadence defect fixed there
            // (SC9 review).
            if (!lastSent || lastSent->SerializeAsString() != info.SerializeAsString()) {
                sink.send(info);
                lastSent = info;
            }

            const bool isFinished =
                state == sila2::ObservableCommandExecution::State::FinishedSuccessfully ||
                state == sila2::ObservableCommandExecution::State::FinishedWithError;
            if (ctx.isCancelled()) {
                // Same rule as ShakeControllerImpl.cc's onShakeForTimeInfo:
                // direct gRPC cannot tell an explicit _Info cancel apart from
                // the connection dying, so this must never interrupt the run
                // (architecture-v2.md:286, §3.3 ②) -- just stop sending.
                break;
            }
            if (isFinished) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        sink.finish();
    };

    binaryAdapter->onEchoBinariesObservablyIntermediate = [&cmdManager, &echoStatesMu, &echoStates](
            const fw::CommandExecutionUUID& req,
            sila2::CallContext& ctx,
            sila2::ResponseSink<binary_proto::EchoBinariesObservably_IntermediateResponses>& sink) {
        std::shared_ptr<EchoBinariesState> state;
        {
            std::lock_guard<std::mutex> lock(echoStatesMu);
            const auto it = echoStates.find(req.value());
            if (it != echoStates.end()) {
                state = it->second;
            }
        }
        if (!state) {
            // Unknown UUID: let ObservableCommandManager raise the standard
            // InvalidCommandExecutionUuid framework error instead of
            // duplicating that policy here.
            cmdManager.getCommand(req.value());
            sink.finish();
            return;
        }

        std::size_t sent = 0;
        std::unique_lock<std::mutex> lock(state->mu);
        while (true) {
            // Poll rather than wait indefinitely so a client-initiated
            // cancellation (ctx.isCancelled()) is noticed even though it
            // never signals state->cv itself.
            state->cv.wait_for(lock, std::chrono::milliseconds{200}, [&] {
                return state->echoedCount > sent || state->done;
            });

            while (sent < state->echoedCount) {
                binary_proto::EchoBinariesObservably_IntermediateResponses resp;
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

    binaryAdapter->onEchoBinariesObservablyResult = [&cmdManager, &echoStatesMu, &echoStates](
            const fw::CommandExecutionUUID& req,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<binary_proto::EchoBinariesObservably_Responses>& sink) {
        // Validates the UUID and raises InvalidCommandExecutionUuid if unknown.
        auto exec = cmdManager.getCommand(req.value());
        switch (exec->state()) {
        case sila2::ObservableCommandExecution::State::Waiting:
        case sila2::ObservableCommandExecution::State::Running:
            // Part A p51: _Result before completion MUST return a Command
            // Execution Not Finished Error, not block the serving thread.
            throw sila2::error::FrameworkError{
                sila2::error::FrameworkError::FrameworkErrorType::CommandExecutionNotFinished,
                "EchoBinariesObservably has not finished yet"};
        case sila2::ObservableCommandExecution::State::FinishedWithError:
            // Part A p51 (S68): a failed execution MUST return its error, not
            // an empty OK response. Defensive: this worker never calls
            // exec->fail(), so this branch is unreachable in practice.
            throw sila2::error::UndefinedExecutionError{exec->errorMessage()};
        case sila2::ObservableCommandExecution::State::FinishedSuccessfully: {
            std::shared_ptr<EchoBinariesState> state;
            {
                std::lock_guard<std::mutex> lock(echoStatesMu);
                state = echoStates.at(req.value());
            }
            binary_proto::EchoBinariesObservably_Responses resp;
            {
                // FinishedSuccessfully is only reached after the worker set
                // jointBinary and called exec->finish(), so no wait is
                // needed here -- just take the lock for the read.
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
            const binary_proto::Get_BinaryValueDirectly_Parameters& /*req*/,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<binary_proto::Get_BinaryValueDirectly_Responses>& sink) {
        binary_proto::Get_BinaryValueDirectly_Responses resp;
        resp.mutable_binaryvaluedirectly()->set_value("SiLA2_Test_String_Value");
        sink.send(resp);
        sink.finish();
    };

    binaryAdapter->onGetBinaryValueDownload = [](
            const binary_proto::Get_BinaryValueDownload_Parameters& /*req*/,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<binary_proto::Get_BinaryValueDownload_Responses>& sink) {
        // Built once and reused: the FDL fixes this string's content, so
        // regenerating it per-call would just repeat 100,000 appends for no
        // benefit.
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

        binary_proto::Get_BinaryValueDownload_Responses resp;
        resp.mutable_binaryvaluedownload()->set_value(largeString);
        sink.send(resp);
        sink.finish();
    };

    binaryAdapter->onEchoBinaryAndMetadataString = [](
            const binary_proto::EchoBinaryAndMetadataString_Parameters& req,
            sila2::CallContext& ctx,
            sila2::ResponseSink<binary_proto::EchoBinaryAndMetadataString_Responses>& sink) {
        binary_proto::EchoBinaryAndMetadataString_Responses resp;
        *resp.mutable_binary() = req.binary();

        // Header key derived by metadataHeaderKey() from the Metadata's FQI.
        const std::optional<std::string> metadataBytes = ctx.metadata(
            sila2::metadataHeaderKey("org.silastandard/test/BinaryTransferTest/v1/Metadata/String"));
        if (metadataBytes.has_value()) {
            binary_proto::Metadata_String metadata;
            if (metadata.ParseFromString(*metadataBytes)) {
                *resp.mutable_stringmetadata() = metadata.string();
            }
        }

        sink.send(resp);
        sink.finish();
    };

    binaryAdapter->onGetFCPAffectedByMetadataString = [](
            const binary_proto::Get_FCPAffectedByMetadata_String_Parameters& /*req*/,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<binary_proto::Get_FCPAffectedByMetadata_String_Responses>& sink) {
        binary_proto::Get_FCPAffectedByMetadata_String_Responses resp;
        resp.add_affectedcalls()->set_value(
            "org.silastandard/test/BinaryTransferTest/v1/Command/EchoBinaryAndMetadataString");
        sink.send(resp);
        sink.finish();
    };

    auto server = builder
        .AddFeature("org.silastandard/test/AuthenticationTest/v1",
                    std::string{sila2::generated::authenticationtest::kFdlXml},
                    authAdapter)
        .AddFeature("org.silastandard/test/BinaryTransferTest/v1",
                    std::string{sila2::generated::binarytransfertest::kFdlXml},
                    binaryAdapter)
        .WithDiscovery(port)
        .Build();

    if (!args.certOut.empty()) {
        std::ofstream f{args.certOut};
        f << server.certificatePem();
    }

    std::cout << "SiLA2 interop server listening on port " << port << "\n";
    std::signal(SIGTERM, handleSigterm);
    server.Run(false);
    while (!gTerminateRequested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // No server.Shutdown() here — audit 4.4a exercises destruction while the
    // server is still live, matching a kill/crash scenario, not a clean exit.
    return 0;
}
