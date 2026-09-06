// Integration tests for executeObservableCommand (architecture.md §5): the
// full Observable Command orchestration — execute (unary) -> subscribe
// _Info (server stream) -> fetch _Result (unary) — driven end-to-end
// against an in-process fake server so every branch of the five-step
// pipeline (ObservableCommandRunner.cc) is exercised, including the
// non-terminal-status and malformed-payload branches real servers rarely
// trigger.
#include <sila/client/dynamic/ObservableCommandRunner.h>

#include <sila/client/CommandExecutionStatus.h>
#include <sila/client/ClientConfig.h>
#include <sila/client/ExecutionStore.h>
#include <sila/client/SilaClientBase.h>
#include <sila/client/dynamic/FeatureCatalog.h>
#include <sila/server/config/TlsConfig.h>

#include "GenericDynamicTestServer.h"

#include "SiLAFramework.pb.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using sila2::CommandExecutionStatus;
using sila2::ExecutionStore;
using sila2::ExecutionUpdate;
using sila2::dynamic::executeObservableCommand;
using sila2::dynamic::FeatureCatalog;
using sila2::dynamic::ObservableCommandResult;
using sila2::dynamic::reattachObservableCommand;
using sila2::test::fromByteBuffer;
using sila2::test::GenericDynamicTestServer;
using sila2::test::ScriptedResponse;
using sila2::test::toByteBuffer;
namespace fw = sila2::org::silastandard;

struct KeyCertPem {
    std::string keyPem;
    std::string certPem;
};

// Self-signed: the certificate is its own root, so it can serve as both the
// server's leaf cert and, handed to the peer as pem_root_certs, its own CA.
// Mirrors the generateSelfSigned() convention in test_client_config_e2e.cc.
KeyCertPem generateSelfSigned() {
    const auto key = sila2::generateKey();
    const auto cert = sila2::generateCertificate(key, "SiLA2", "127.0.0.1");
    return {sila2::keyToPem(key), sila2::certificateToPem(cert)};
}

// Unique per-test store path under gtest's temp dir, mirroring
// tests/sila/client/test_execution_store.cc's tempStorePath().
std::filesystem::path tempStorePath(const std::string& testName) {
    return std::filesystem::path{::testing::TempDir()} /
        ("sila_observable_runner_store_" + testName + "_" + std::to_string(::getpid()) + ".tsv");
}

// Originator segment restored: FeatureCatalog::add() now derives the FQI from
// the FDL itself (Originator="org.silastandardtest" below) and rejects a
// caller-supplied FQI that disagrees, per S44.
constexpr char kFqi[] = "org.silastandardtest/test/ObservableRunnerFeature/v1";
constexpr char kCommandId[] = "DoWork";

// One FDL feature with a single observable command is enough to drive
// FeatureCatalog::grpcMethodName() for the execute/_Info/_Result RPC paths;
// ObservableCommandRunner never inspects the FDL-declared Parameter/Response
// field shapes, only the CommandConfirmation/ExecutionInfo/
// CommandExecutionUUID framework messages, so a minimal Command suffices.
//
// FeatureCatalog has a user-declared destructor, which suppresses the
// implicit move ctor and leaves it non-copyable/non-movable — so it can't
// be returned by value (NRVO isn't guaranteed). Tests build one on the
// stack and populate it in place instead.
void addObservableRunnerFeature(FeatureCatalog& catalog) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>ObservableRunnerFeature</Identifier>
  <DisplayName>Observable runner feature</DisplayName>
  <Description>Observable runner feature</Description>
  <Command>
    <Identifier>DoWork</Identifier>
    <DisplayName>Do work</DisplayName>
    <Description>Do work</Description>
    <Observable>Yes</Observable>
    <Parameter>
      <Identifier>Input</Identifier>
      <DisplayName>Input</DisplayName>
      <Description>Input</Description>
      <DataType><Basic>String</Basic></DataType>
    </Parameter>
    <Response>
      <Identifier>Output</Identifier>
      <DisplayName>Output</DisplayName>
      <Description>Output</Description>
      <DataType><Basic>String</Basic></DataType>
    </Response>
  </Command>
</Feature>
)xml";
    catalog.add(kFqi, kFdl);
}

std::unique_ptr<google::protobuf::Message> commandRequest(FeatureCatalog& catalog) {
    const auto* descriptor = catalog.requestDescriptor(kFqi, kCommandId);
    auto request = std::unique_ptr<google::protobuf::Message>{
        catalog.messageFactory().GetPrototype(descriptor)->New()};
    const auto* input = descriptor->FindFieldByName("Input");
    auto* wrapped = request->GetReflection()->MutableMessage(request.get(), input);
    const auto* value = wrapped->GetDescriptor()->FindFieldByName("value");
    wrapped->GetReflection()->SetString(wrapped, value, "input");
    return request;
}

grpc::ByteBuffer confirmationBuffer(const std::string& uuid) {
    fw::CommandConfirmation confirmation;
    confirmation.mutable_commandexecutionuuid()->set_value(uuid);
    return toByteBuffer(confirmation);
}

fw::ExecutionInfo makeInfo(fw::ExecutionInfo_CommandStatus status, double progress = 0.0) {
    fw::ExecutionInfo info;
    info.set_commandstatus(status);
    if (progress > 0.0) {
        info.mutable_progressinfo()->set_value(progress);
    }
    return info;
}

// Wires up execute/_Info/_Result handlers on `server` for kFqi/kCommandId.
// Tests override individual steps afterwards via server.on(...) to inject
// failures at a specific point in the pipeline.
void wireHappyPath(GenericDynamicTestServer& server, FeatureCatalog& catalog, const std::string& uuid) {
    server.on(catalog.grpcMethodName(kFqi, kCommandId), [uuid](const grpc::ByteBuffer&) {
        return ScriptedResponse{{confirmationBuffer(uuid)}, grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Info"), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_running, 0.5)),
             toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_finishedSuccessfully))},
            grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Result"), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{[] {
            grpc::Slice slice("final-result");
            return grpc::ByteBuffer(&slice, 1);
        }()}, grpc::Status::OK};
    });
}

// --- True (positive) paths --------------------------------------------------

TEST(ObservableCommandRunner, FullPipelineDeliversUpdatesThenResult) {
    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    wireHappyPath(server, catalog, "uuid-happy");

    std::vector<CommandExecutionStatus> statuses;
    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog),
        [&](const ExecutionUpdate& update) { statuses.push_back(update.status); });

    EXPECT_TRUE(result.status.ok()) << result.status.error_message();
    EXPECT_EQ(result.commandExecutionUuid, "uuid-happy");
    EXPECT_EQ(fromByteBuffer(result.result), "final-result");
    EXPECT_EQ(statuses, (std::vector<CommandExecutionStatus>{
                            CommandExecutionStatus::kRunning,
                            CommandExecutionStatus::kFinishedSuccessfully}));
}

TEST(ObservableCommandRunner, ImmediateSuccessWithNoIntermediateUpdatesFetchesResult) {
    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    server.on(catalog.grpcMethodName(kFqi, kCommandId), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{confirmationBuffer("uuid-immediate")}, grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Info"), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_finishedSuccessfully))},
            grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Result"), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{[] {
            grpc::Slice slice("done");
            return grpc::ByteBuffer(&slice, 1);
        }()}, grpc::Status::OK};
    });

    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog));

    EXPECT_TRUE(result.status.ok()) << result.status.error_message();
    EXPECT_EQ(result.commandExecutionUuid, "uuid-immediate");
    EXPECT_EQ(fromByteBuffer(result.result), "done");
}

TEST(ObservableCommandRunner, OnUpdateCallbackIsOptional) {
    // Default-constructed ExecutionUpdateCallback (the onUpdate={} default
    // argument) must not be invoked and must not crash the pipeline.
    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    wireHappyPath(server, catalog, "uuid-no-callback");

    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog));

    EXPECT_TRUE(result.status.ok()) << result.status.error_message();
    EXPECT_EQ(result.commandExecutionUuid, "uuid-no-callback");
    EXPECT_EQ(fromByteBuffer(result.result), "final-result");
}

// S36/G1 (Part A p33): a store passed to executeObservableCommand records the
// UUID on issue (before the blocking watch) and prunes it once the execution
// reaches a terminal state.
TEST(ObservableCommandRunner, PersistsOnIssueThenPrunesOnSuccess) {
    const auto storePath = tempStorePath("PersistsThenPrunes");
    std::filesystem::remove(storePath);
    ExecutionStore store{storePath};

    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    wireHappyPath(server, catalog, "uuid-persist");

    bool sawRecordedDuringRun = false;
    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog),
        [&](const ExecutionUpdate& update) {
            // The record-on-issue write happens before the _Info stream is
            // even subscribed to, so it must already be visible by the time
            // the first (kRunning) update arrives.
            if (update.status == CommandExecutionStatus::kRunning) {
                sawRecordedDuringRun = (store.list().size() == 1);
            }
        },
        /*constraintResolver=*/{}, /*injector=*/nullptr, &store, "srv-uuid");

    EXPECT_TRUE(result.status.ok()) << result.status.error_message();
    EXPECT_TRUE(sawRecordedDuringRun);
    EXPECT_TRUE(store.list().empty());  // pruned on the successful terminal status

    std::filesystem::remove(storePath);
}

// S36/G1: a successful execution whose _Result fetch fails (e.g. the
// connection dropped between _Info and _Result) must keep its persisted row,
// otherwise a restart could never recover the result the server still holds.
TEST(ObservableCommandRunner, ResultFetchFailureKeepsPersistedRow) {
    const auto storePath = tempStorePath("ResultFailKeepsRow");
    std::filesystem::remove(storePath);
    ExecutionStore store{storePath};

    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    server.on(catalog.grpcMethodName(kFqi, kCommandId), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{confirmationBuffer("uuid-keep-row")}, grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Info"), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_finishedSuccessfully))},
            grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Result"), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{}, grpc::Status(grpc::StatusCode::UNAVAILABLE, "connection lost")};
    });

    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog),
        /*onUpdate=*/{}, /*constraintResolver=*/{}, /*injector=*/nullptr, &store, "srv-uuid");

    EXPECT_FALSE(result.status.ok());
    const auto rows = store.list();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().executionUuid, "uuid-keep-row");

    std::filesystem::remove(storePath);
}

// S36/G1 production wiring: a SilaClientBase built with an execution store
// path hands its own store to the runner, so the UUID a restart would need is
// visible on disk while the command runs, and reattach through the same
// client finds it without the caller ever touching ExecutionStore.
TEST(ObservableCommandRunner, ClientBaseOverloadWiresTheConfiguredStore) {
    const auto storePath = tempStorePath("ClientBaseWiring");
    std::filesystem::remove(storePath);

    // The one test in this file routed through ClientConfig (S74, Part B
    // p74: no plaintext); every other test here reaches the server through
    // GenericDynamicTestServer::channel() directly and stays insecure.
    const auto serverPair = generateSelfSigned();
    grpc::SslServerCredentialsOptions sslOpts;
    sslOpts.pem_key_cert_pairs.push_back({serverPair.keyPem, serverPair.certPem});
    GenericDynamicTestServer server{grpc::SslServerCredentials(sslOpts)};
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    wireHappyPath(server, catalog, "uuid-wired");

    sila2::ClientConfig config;
    sila2::TlsCredentials creds;
    creds.caCertificatePem = serverPair.certPem;
    config.setTlsCredentials(creds);
    config.setExecutionStorePath(storePath);
    sila2::SilaClientBase client{"127.0.0.1", static_cast<uint16_t>(server.port()), config};
    ASSERT_NE(client.executionStore(), nullptr);

    bool recordedDuringRun = false;
    ObservableCommandResult result = executeObservableCommand(
        client, "srv-uuid", catalog, kFqi, kCommandId, *commandRequest(catalog),
        [&](const ExecutionUpdate& update) {
            if (update.status == CommandExecutionStatus::kRunning) {
                // Read through a second handle: the row must be on disk.
                recordedDuringRun = (ExecutionStore{storePath}.list("srv-uuid").size() == 1);
            }
        });
    EXPECT_TRUE(result.status.ok()) << result.status.error_message();
    EXPECT_TRUE(recordedDuringRun);
    EXPECT_TRUE(client.executionStore()->list().empty());

    ObservableCommandResult again = reattachObservableCommand(
        client, "srv-uuid", catalog, kFqi, kCommandId, "uuid-wired");
    EXPECT_TRUE(again.status.ok()) << again.status.error_message();

    std::filesystem::remove(storePath);
}

// S36/G1: reattachObservableCommand re-subscribes to an already-issued UUID
// (e.g. one read back from ExecutionStore::list() after a client restart)
// without re-initiating the command.
TEST(ObservableCommandRunner, ReattachDeliversResultForPersistedUuid) {
    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    wireHappyPath(server, catalog, "uuid-happy");

    ObservableCommandResult result = reattachObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, "uuid-happy");

    EXPECT_TRUE(result.status.ok()) << result.status.error_message();
    EXPECT_EQ(fromByteBuffer(result.result), "final-result");
}

// --- False (negative/rejection) paths ---------------------------------------

// Step 1 (ObservableCommandRunner.cc:28): execute callUnary itself fails.
TEST(ObservableCommandRunner, ExecuteFailureReturnsEmptyUuidAndFailedStatus) {
    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    server.on(catalog.grpcMethodName(kFqi, kCommandId), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{}, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "rejected")};
    });

    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog));

    EXPECT_FALSE(result.status.ok());
    EXPECT_EQ(result.status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_TRUE(result.commandExecutionUuid.empty());
    EXPECT_EQ(fromByteBuffer(result.result), "");
}

// Step 2 (ObservableCommandRunner.cc:37): CommandConfirmation bytes are
// not valid protobuf, so deserialization fails.
TEST(ObservableCommandRunner, MalformedConfirmationReturnsDeserializeFailure) {
    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    server.on(catalog.grpcMethodName(kFqi, kCommandId), [](const grpc::ByteBuffer&) {
        grpc::Slice garbage("\xFF\xFF\xFF");
        return ScriptedResponse{{grpc::ByteBuffer(&garbage, 1)}, grpc::Status::OK};
    });

    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog));

    EXPECT_FALSE(result.status.ok());
    EXPECT_TRUE(result.commandExecutionUuid.empty());
}

// Step 4 (ObservableCommandRunner.cc:93): the _Info stream reaches
// finishedWithError; the pipeline stops there without fetching _Result.
TEST(ObservableCommandRunner, InfoStreamFinishedWithErrorSkipsResultFetch) {
    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    server.on(catalog.grpcMethodName(kFqi, kCommandId), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{confirmationBuffer("uuid-error")}, grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Info"), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_finishedWithError))},
            grpc::Status::OK};
    });
    // Registering _Result would be a test bug if the pipeline actually
    // called it here; leave it unregistered so such a call fails loudly.

    std::vector<CommandExecutionStatus> statuses;
    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog),
        [&](const ExecutionUpdate& update) { statuses.push_back(update.status); });

    EXPECT_EQ(result.commandExecutionUuid, "uuid-error");
    EXPECT_EQ(fromByteBuffer(result.result), "");
    EXPECT_EQ(statuses, (std::vector<CommandExecutionStatus>{CommandExecutionStatus::kFinishedWithError}));
}

// S36/G1: a store passed in still prunes on the finishedWithError terminal
// status, not only on success (Part A p33's guarantee ends once the
// execution reaches any terminal state).
TEST(ObservableCommandRunner, PrunesOnErrorTerminal) {
    const auto storePath = tempStorePath("PrunesOnError");
    std::filesystem::remove(storePath);
    ExecutionStore store{storePath};

    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    server.on(catalog.grpcMethodName(kFqi, kCommandId), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{confirmationBuffer("uuid-prune-error")}, grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Info"), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_finishedWithError))},
            grpc::Status::OK};
    });

    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog),
        /*onUpdate=*/{}, /*constraintResolver=*/{}, /*injector=*/nullptr, &store, "srv-uuid");

    EXPECT_EQ(result.commandExecutionUuid, "uuid-prune-error");
    EXPECT_TRUE(store.list().empty());

    std::filesystem::remove(storePath);
}

// Step 4 (ObservableCommandRunner.cc:59): a malformed ExecutionInfo message
// mid-stream makes the callback return false, ending the stream before any
// terminal status is observed.
TEST(ObservableCommandRunner, InfoStreamDeserializeFailureEndsStreamEarly) {
    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    server.on(catalog.grpcMethodName(kFqi, kCommandId), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{confirmationBuffer("uuid-malformed-info")}, grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Info"), [](const grpc::ByteBuffer&) {
        grpc::Slice garbage("\xFF\xFF\xFF");
        return ScriptedResponse{{grpc::ByteBuffer(&garbage, 1)}, grpc::Status::OK};
    });

    int updateCount = 0;
    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog),
        [&](const ExecutionUpdate&) { ++updateCount; });

    // The malformed message must not reach onUpdate.
    EXPECT_EQ(updateCount, 0);
    EXPECT_EQ(result.commandExecutionUuid, "uuid-malformed-info");
    EXPECT_EQ(fromByteBuffer(result.result), "");
}

// Step 5 (ObservableCommandRunner.cc:89): the command finishes successfully
// but the _Result fetch itself fails.
TEST(ObservableCommandRunner, ResultFetchFailureReturnsFailedStatus) {
    GenericDynamicTestServer server;
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    addObservableRunnerFeature(catalog);
    server.on(catalog.grpcMethodName(kFqi, kCommandId), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{confirmationBuffer("uuid-result-fail")}, grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Info"), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_finishedSuccessfully))},
            grpc::Status::OK};
    });
    server.on(catalog.grpcMethodName(kFqi, "DoWork_Result"), [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{}, grpc::Status(grpc::StatusCode::NOT_FOUND, "result expired")};
    });

    ObservableCommandResult result = executeObservableCommand(
        server.channel(), catalog, kFqi, kCommandId, *commandRequest(catalog));

    EXPECT_FALSE(result.status.ok());
    EXPECT_EQ(result.status.error_code(), grpc::StatusCode::NOT_FOUND);
    EXPECT_EQ(result.commandExecutionUuid, "uuid-result-fail");
    EXPECT_EQ(fromByteBuffer(result.result), "");
}

}  // namespace
