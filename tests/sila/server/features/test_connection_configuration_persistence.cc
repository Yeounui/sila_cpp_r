// Tests for the server-initiated connection PERSISTENCE flow added to
// ConnectionConfigurationServiceImpl: saveState()/loadState() round-tripping
// mode + persist==true clients through a tab-delimited store file, and
// connectPersistentClients() restoring state on startup. State-restoration
// assertions do not depend on the background CloudTransport connect
// succeeding (see loadState()/connectPersistentClients() in the .cc: clients_
// is populated before the connect loop runs), so every test here points
// persist clients at a dead local endpoint (127.0.0.1:1) and never touches
// the network.
#include <sila/server/features/ConnectionConfigurationServiceImpl.h>

#include <sila/common/error/SiLAErrorException.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>

#include "ConnectionConfigurationService.grpc.pb.h"
#include "SiLAFramework.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace
{
using sila2::CloudEnvelopeRouter;
using sila2::ConnectionConfigurationServiceImpl;
using sila2::FeatureRegistry;
using sila2::error::DefinedExecutionError;
using sila2::error::fromGrpcStatus;
using sila2::error::SiLAError;

namespace connconfig_proto = sila2::org::silastandard::core::connectionconfigurationservice::v1;

const std::string kInvalidSiLAClientErrorId =
    "org.silastandard/core/ConnectionConfigurationService/v1/DefinedExecutionError/InvalidSiLAClient";

// ---------------------------------------------------------------------------
// Harness — copied from test_connection_configuration_service.cc so this
// suite exercises the exact same public entry points.
// ---------------------------------------------------------------------------

grpc::Status enableMode(ConnectionConfigurationServiceImpl& svc) {
    grpc::ServerContext ctx;
    connconfig_proto::EnableServerInitiatedConnectionMode_Parameters request;
    connconfig_proto::EnableServerInitiatedConnectionMode_Responses response;
    return svc.EnableServerInitiatedConnectionMode(&ctx, &request, &response);
}

grpc::Status disableMode(ConnectionConfigurationServiceImpl& svc) {
    grpc::ServerContext ctx;
    connconfig_proto::DisableServerInitiatedConnectionMode_Parameters request;
    connconfig_proto::DisableServerInitiatedConnectionMode_Responses response;
    return svc.DisableServerInitiatedConnectionMode(&ctx, &request, &response);
}

bool getModeStatus(ConnectionConfigurationServiceImpl& svc) {
    grpc::ServerContext ctx;
    connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Parameters request;
    connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Responses response;
    svc.Get_ServerInitiatedConnectionModeStatus(&ctx, &request, &response);
    return response.serverinitiatedconnectionmodestatus().value();
}

connconfig_proto::Get_ConfiguredSiLAClients_Responses getConfiguredClients(
    ConnectionConfigurationServiceImpl& svc) {
    grpc::ServerContext ctx;
    connconfig_proto::Get_ConfiguredSiLAClients_Parameters request;
    connconfig_proto::Get_ConfiguredSiLAClients_Responses response;
    svc.Get_ConfiguredSiLAClients(&ctx, &request, &response);
    return response;
}

grpc::Status connectSiLAClient(ConnectionConfigurationServiceImpl& svc,
                                const std::string& clientName,
                                const std::string& host,
                                int64_t port,
                                bool persist) {
    grpc::ServerContext ctx;
    connconfig_proto::ConnectSiLAClient_Parameters request;
    request.mutable_clientname()->set_value(clientName);
    request.mutable_silaclienthost()->set_value(host);
    request.mutable_silaclientport()->set_value(port);
    request.mutable_persist()->set_value(persist);
    connconfig_proto::ConnectSiLAClient_Responses response;
    return svc.ConnectSiLAClient(&ctx, &request, &response);
}

grpc::Status disconnectSiLAClient(ConnectionConfigurationServiceImpl& svc,
                                   const std::string& clientName,
                                   bool remove) {
    grpc::ServerContext ctx;
    connconfig_proto::DisconnectSiLAClient_Parameters request;
    request.mutable_clientname()->set_value(clientName);
    request.mutable_remove()->set_value(remove);
    connconfig_proto::DisconnectSiLAClient_Responses response;
    return svc.DisconnectSiLAClient(&ctx, &request, &response);
}

// Finds a configured client by name in a Get_ConfiguredSiLAClients response,
// or nullptr if absent — lets tests assert presence/absence and field values
// in one lookup instead of scanning the repeated field inline.
const connconfig_proto::Get_ConfiguredSiLAClients_Responses_ConfiguredSiLAClients_Struct* findClient(
    const connconfig_proto::Get_ConfiguredSiLAClients_Responses& response,
    const std::string& clientName) {
    for (const auto& client : response.configuredsilaclients()) {
        if (client.clientname().value() == clientName) {
            return &client;
        }
    }
    return nullptr;
}

// Unique per-test store path under the system temp dir, including the test
// name and pid so parallel/repeated runs never collide on a shared file.
std::filesystem::path tempStorePath(const std::string& testName) {
    return std::filesystem::temp_directory_path() /
        ("sila_connconfig_persistence_" + testName + "_" + std::to_string(::getpid()) + ".tsv");
}

// ---------------------------------------------------------------------------
// True paths — round-trip through saveState()/loadState()
// ---------------------------------------------------------------------------

TEST(ConnectionConfigurationPersistence, RoundTripRestoresModeAndPersistClient) {
    const auto storePath = tempStorePath("RoundTrip");
    std::filesystem::remove(storePath);

    {
        FeatureRegistry registry;
        CloudEnvelopeRouter router{registry};
        ConnectionConfigurationServiceImpl svc{
            router, grpc::InsecureChannelCredentials(), nullptr, storePath};

        ASSERT_TRUE(enableMode(svc).ok());
        ASSERT_TRUE(connectSiLAClient(svc, "clientA", "127.0.0.1", 1, /*persist=*/true).ok());
    }  // svc1 destructor joins CloudTransport's background thread before svc2 opens the same file

    {
        FeatureRegistry registry;
        CloudEnvelopeRouter router{registry};
        ConnectionConfigurationServiceImpl svc{
            router, grpc::InsecureChannelCredentials(), nullptr, storePath};

        svc.connectPersistentClients();

        EXPECT_TRUE(getModeStatus(svc));
        const auto clients = getConfiguredClients(svc);
        ASSERT_EQ(clients.configuredsilaclients_size(), 1);
        const auto* clientA = findClient(clients, "clientA");
        ASSERT_NE(clientA, nullptr);
        EXPECT_EQ(clientA->silaclienthost().value(), "127.0.0.1");
        EXPECT_EQ(clientA->silaclientport().value(), 1);
    }

    std::filesystem::remove(storePath);
}

// Mode gates restart reconnection (architecture.md §3.9): a persist client
// loaded with mode==false must be restored as configured but NOT reconnected.
// The store is hand-written (mode 0 + one persist client) rather than driven
// through the service, because enabling the mode to connect a client would
// open a real CloudTransport stream. The dead endpoint 127.0.0.1:1 is the test
// oracle: connectPersistentClients() returns promptly here only because the
// mode gate skips the connect loop — without the gate this call would block on
// the outbound connect to the dead port, exactly as the mode==true RoundTrip
// path does.
TEST(ConnectionConfigurationPersistence, DisabledModeSkipsReconnectButKeepsClientConfigured) {
    const auto storePath = tempStorePath("DisabledModeSkipsReconnect");
    std::filesystem::remove(storePath);
    {
        std::ofstream out{storePath, std::ios::trunc};
        out << "mode\t0\n";
        out << "client\tclientD\t127.0.0.1\t1\n";
    }
    // Owner-only like saveState() writes it: loadState() refuses a store that
    // group or others could write (Codex review of SC32).
    std::filesystem::permissions(storePath,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);

    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, storePath};

    svc.connectPersistentClients();

    EXPECT_FALSE(getModeStatus(svc));
    const auto clients = getConfiguredClients(svc);
    const auto* clientD = findClient(clients, "clientD");
    ASSERT_NE(clientD, nullptr);
    EXPECT_EQ(clientD->silaclienthost().value(), "127.0.0.1");
    EXPECT_EQ(clientD->silaclientport().value(), 1);

    std::filesystem::remove(storePath);
}

TEST(ConnectionConfigurationPersistence, PersistFalseClientIsNotStored) {
    const auto storePath = tempStorePath("PersistFalse");
    std::filesystem::remove(storePath);

    {
        FeatureRegistry registry;
        CloudEnvelopeRouter router{registry};
        ConnectionConfigurationServiceImpl svc{
            router, grpc::InsecureChannelCredentials(), nullptr, storePath};

        ASSERT_TRUE(enableMode(svc).ok());
        ASSERT_TRUE(connectSiLAClient(svc, "ephemeral", "127.0.0.1", 1, /*persist=*/false).ok());
    }

    {
        FeatureRegistry registry;
        CloudEnvelopeRouter router{registry};
        ConnectionConfigurationServiceImpl svc{
            router, grpc::InsecureChannelCredentials(), nullptr, storePath};

        svc.connectPersistentClients();

        // Mode still round-trips — only client persistence is gated on persist==true.
        EXPECT_TRUE(getModeStatus(svc));
        const auto clients = getConfiguredClients(svc);
        EXPECT_EQ(findClient(clients, "ephemeral"), nullptr);
        EXPECT_EQ(clients.configuredsilaclients_size(), 0);
    }

    std::filesystem::remove(storePath);
}

TEST(ConnectionConfigurationPersistence, DisconnectWithRemoveTrueDeletesStoredRecord) {
    const auto storePath = tempStorePath("RemoveTrue");
    std::filesystem::remove(storePath);

    {
        FeatureRegistry registry;
        CloudEnvelopeRouter router{registry};
        ConnectionConfigurationServiceImpl svc{
            router, grpc::InsecureChannelCredentials(), nullptr, storePath};

        ASSERT_TRUE(connectSiLAClient(svc, "clientB", "127.0.0.1", 1, /*persist=*/true).ok());
        ASSERT_TRUE(disconnectSiLAClient(svc, "clientB", /*remove=*/true).ok());
    }

    {
        FeatureRegistry registry;
        CloudEnvelopeRouter router{registry};
        ConnectionConfigurationServiceImpl svc{
            router, grpc::InsecureChannelCredentials(), nullptr, storePath};

        svc.connectPersistentClients();

        EXPECT_EQ(findClient(getConfiguredClients(svc), "clientB"), nullptr);
    }

    std::filesystem::remove(storePath);
}

// Bonus: remove==false leaves the record in place, distinguishing "record
// erased" from "record kept but transport disconnected" — both call
// saveState(), only remove==true drops the client from clients_ first.
TEST(ConnectionConfigurationPersistence, DisconnectWithRemoveFalseKeepsStoredRecord) {
    const auto storePath = tempStorePath("RemoveFalse");
    std::filesystem::remove(storePath);

    {
        FeatureRegistry registry;
        CloudEnvelopeRouter router{registry};
        ConnectionConfigurationServiceImpl svc{
            router, grpc::InsecureChannelCredentials(), nullptr, storePath};

        ASSERT_TRUE(connectSiLAClient(svc, "clientC", "127.0.0.1", 1, /*persist=*/true).ok());
        ASSERT_TRUE(disconnectSiLAClient(svc, "clientC", /*remove=*/false).ok());
    }

    {
        FeatureRegistry registry;
        CloudEnvelopeRouter router{registry};
        ConnectionConfigurationServiceImpl svc{
            router, grpc::InsecureChannelCredentials(), nullptr, storePath};

        svc.connectPersistentClients();

        // Named local, not a temporary passed straight to findClient(): the
        // pointer findClient() returns points into this response, and a
        // temporary's lifetime would end at the semicolon of the line that
        // creates it — dangling before the EXPECT_EQ calls below read it.
        const auto clients = getConfiguredClients(svc);
        const auto* clientC = findClient(clients, "clientC");
        ASSERT_NE(clientC, nullptr);
        EXPECT_EQ(clientC->silaclienthost().value(), "127.0.0.1");
        EXPECT_EQ(clientC->silaclientport().value(), 1);
    }

    std::filesystem::remove(storePath);
}

// ---------------------------------------------------------------------------
// False paths — CAUGHT: loadState()/connectSiLAClient() reject the input
// ---------------------------------------------------------------------------

TEST(ConnectionConfigurationPersistence, CorruptRecordTypeThrowsRuntimeError) {
    const auto storePath = tempStorePath("CorruptRecordType");
    std::filesystem::remove(storePath);
    {
        std::ofstream out{storePath, std::ios::trunc};
        out << "bogus\tx\n";
    }
    // Owner-only like saveState() writes it: loadState() refuses a store that
    // group or others could write (Codex review of SC32).
    std::filesystem::permissions(storePath,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);

    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, storePath};

    EXPECT_THROW(svc.connectPersistentClients(), std::runtime_error);

    std::filesystem::remove(storePath);
}

TEST(ConnectionConfigurationPersistence, NonNumericPortFieldThrowsRuntimeError) {
    const auto storePath = tempStorePath("NonNumericPort");
    std::filesystem::remove(storePath);
    {
        std::ofstream out{storePath, std::ios::trunc};
        out << "client\tc\t127.0.0.1\t80x\n";
    }
    // Owner-only like saveState() writes it: loadState() refuses a store that
    // group or others could write (Codex review of SC32).
    std::filesystem::permissions(storePath,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);

    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, storePath};

    EXPECT_THROW(svc.connectPersistentClients(), std::runtime_error);

    std::filesystem::remove(storePath);
}

TEST(ConnectionConfigurationPersistence, OutOfRangePortFieldThrowsRuntimeError) {
    const auto storePath = tempStorePath("OutOfRangePort");
    std::filesystem::remove(storePath);
    {
        std::ofstream out{storePath, std::ios::trunc};
        out << "client\tc\t127.0.0.1\t70000\n";
    }
    // Owner-only like saveState() writes it: loadState() refuses a store that
    // group or others could write (Codex review of SC32).
    std::filesystem::permissions(storePath,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);

    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, storePath};

    EXPECT_THROW(svc.connectPersistentClients(), std::runtime_error);

    std::filesystem::remove(storePath);
}

// Control characters in ClientName are rejected by connectSiLAClient()'s
// storage-format integrity guard before any transport (or store write)
// happens — this is the write-side counterpart to 5/6 above, which cover
// corrupt records already on disk.
TEST(ConnectionConfigurationPersistence, ConnectWithControlCharacterInNameReturnsInvalidSiLAClient) {
    const auto storePath = tempStorePath("ControlCharName");
    std::filesystem::remove(storePath);

    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, storePath};

    const grpc::Status status = connectSiLAClient(svc, "bad\tname", "127.0.0.1", 1, true);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kInvalidSiLAClientErrorId);
    // Rejected before saveState() ever wrote the record.
    EXPECT_FALSE(std::filesystem::exists(storePath));

    std::filesystem::remove(storePath);
}

// Codex review of SC32 (2026-09-04): the default store may sit in the shared
// temp directory at a path predictable from the advertised UUID, so the file
// is written owner-only and never trusted when someone else could have
// written it.
TEST(ConnectionConfigurationPersistence, SavedStoreIsOwnerOnly) {
    const auto storePath = tempStorePath("OwnerOnly");
    std::filesystem::remove(storePath);

    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, storePath};
    ASSERT_TRUE(enableMode(svc).ok());

    const auto perms = std::filesystem::status(storePath).permissions();
    EXPECT_EQ(perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
              std::filesystem::perms::none);

    std::filesystem::remove(storePath);
}

TEST(ConnectionConfigurationPersistence, GroupOrWorldWritableStoreIsRefused) {
    const auto storePath = tempStorePath("WorldWritable");
    std::filesystem::remove(storePath);
    {
        std::ofstream out{storePath};
        out << "mode\t1\n";
    }
    // Owner-only like saveState() writes it: loadState() refuses a store that
    // group or others could write (Codex review of SC32).
    std::filesystem::permissions(storePath,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
    std::filesystem::permissions(storePath,
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_write,
                                 std::filesystem::perm_options::replace);

    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, storePath};

    EXPECT_THROW(svc.connectPersistentClients(), std::runtime_error);
    EXPECT_FALSE(getModeStatus(svc));  // nothing was loaded

    std::filesystem::remove(storePath);
}

// The per-host credential provider (SiLAServerBase's Part A p32 default)
// vetoes a target before anything is persisted, and vetoes a persisted
// target on restore when the trust configuration no longer allows it.
TEST(ConnectionConfigurationPersistence, ProviderVetoRejectsConnectBeforePersistAndFailsRestore) {
    const auto storePath = tempStorePath("ProviderVeto");
    std::filesystem::remove(storePath);

    const auto onlyPrivate = [](std::string_view host)
        -> std::shared_ptr<grpc::ChannelCredentials> {
        return host == "10.0.0.1" ? grpc::InsecureChannelCredentials() : nullptr;
    };
    {
        FeatureRegistry registry;
        CloudEnvelopeRouter router{registry};
        ConnectionConfigurationServiceImpl svc{router, onlyPrivate, nullptr, storePath};

        const grpc::Status refused = connectSiLAClient(svc, "pub", "203.0.113.5", 1, true);
        ASSERT_FALSE(refused.ok());
        const auto reconstructed = fromGrpcStatus(refused);
        ASSERT_NE(reconstructed, nullptr);
        const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
        ASSERT_NE(err, nullptr);
        EXPECT_EQ(err->errorIdentifier(), kInvalidSiLAClientErrorId);
        EXPECT_FALSE(std::filesystem::exists(storePath));  // vetoed before saveState()

        ASSERT_TRUE(enableMode(svc).ok());
        ASSERT_TRUE(connectSiLAClient(svc, "lan", "10.0.0.1", 1, true).ok());
    }
    {
        // Trust configuration changed: nothing is allowed any more, so the
        // persisted "lan" record cannot be restored -- fail loud.
        FeatureRegistry registry;
        CloudEnvelopeRouter router{registry};
        ConnectionConfigurationServiceImpl svc{
            router, [](std::string_view) { return std::shared_ptr<grpc::ChannelCredentials>{}; },
            nullptr, storePath};
        EXPECT_THROW(svc.connectPersistentClients(), std::runtime_error);
    }

    std::filesystem::remove(storePath);
}

}  // namespace
