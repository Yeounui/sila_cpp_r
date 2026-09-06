// Tests for ServerRegistry: registration/upsert, mDNS delegation, lookup,
// and connection-state callback dispatch.
#include <sila/client/ClientConfig.h>
#include <sila/client/ServerRegistry.h>
#include <sila/client/SilaClientBase.h>

#include <gtest/gtest.h>
#include <grpcpp/client_context.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
using sila2::ClientConfig;
using sila2::ConnectionState;
using sila2::ServerRegistry;

ServerRegistry makeInsecureRegistry() {
    ClientConfig config;
    // No allowInsecure(): every target is a private-range IP, so
    // channelCredentials() takes the Part B p75 untrusted-TLS path (S74).
    return ServerRegistry{std::move(config)};
}

ClientConfig insecureConfig() {
    ClientConfig config;
    // No allowInsecure(): every target is a private-range IP, so
    // channelCredentials() takes the Part B p75 untrusted-TLS path (S74).
    return config;
}

// Unique per-test store path under the system temp dir, including the test
// name and pid so parallel/repeated runs never collide on a shared file —
// mirrors tempStorePath() in
// tests/sila/server/features/test_connection_configuration_persistence.cc.
std::filesystem::path tempStorePath(const std::string& testName) {
    return std::filesystem::temp_directory_path() /
        ("sila_server_registry_" + testName + "_" + std::to_string(::getpid()) + ".tsv");
}

// --- True (positive) paths --------------------------------------------------

TEST(ServerRegistry, RegisterServerThenFindByUuidReturnsEntry) {
    auto registry = makeInsecureRegistry();

    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");
    auto entry = registry.findByUuid("uuid-1");

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->uuid, "uuid-1");
    EXPECT_EQ(entry->host, "192.168.1.10");
    EXPECT_EQ(entry->port, 50051);
    EXPECT_EQ(entry->serverName, "MyServer");
}

TEST(ServerRegistry, RegisterServerWithExistingUuidUpdatesHostAndPort) {
    // Re-bind after reboot (§3.7): same uuid, new address.
    auto registry = makeInsecureRegistry();
    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");

    registry.registerServer("uuid-1", "192.168.1.99", 60051, "MyServerRenamed");
    auto entry = registry.findByUuid("uuid-1");

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->host, "192.168.1.99");
    EXPECT_EQ(entry->port, 60051);
    EXPECT_EQ(entry->serverName, "MyServerRenamed");
}

TEST(ServerRegistry, RegisterServerAddsEntryToAllServers) {
    auto registry = makeInsecureRegistry();

    registry.registerServer("uuid-2", "10.0.0.5", 50052, "DiscoveredServer");
    const std::vector<ServerRegistry::ServerEntry> all = registry.allServers();

    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].uuid, "uuid-2");
    EXPECT_EQ(all[0].serverName, "DiscoveredServer");
}

TEST(ServerRegistry, UpdateStateInvokesCallbackWithUuidAndState) {
    auto registry = makeInsecureRegistry();
    registry.registerServer("uuid-3", "10.0.0.1", 50051, "Server3");

    std::string capturedUuid;
    ConnectionState capturedState{ConnectionState::kDisconnected};
    registry.setConnectionStateCallback([&](const std::string& uuid, ConnectionState state) {
        capturedUuid = uuid;
        capturedState = state;
    });

    registry.updateState("uuid-3", ConnectionState::kConnected);

    EXPECT_EQ(capturedUuid, "uuid-3");
    EXPECT_EQ(capturedState, ConnectionState::kConnected);
}

TEST(ServerRegistry, FindByUuidIgnoresCase) {
    // Part A p90: UUID string comparison MUST ignore case, so a case-variant
    // lookup must still find the entry registered under the lower-case uuid.
    auto registry = makeInsecureRegistry();
    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");

    auto entry = registry.findByUuid("UUID-1");

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->host, "192.168.1.10");
}

// --- False (negative/edge) paths --------------------------------------------

TEST(ServerRegistry, FindByUuidWithUnknownUuidReturnsNullptr) {
    auto registry = makeInsecureRegistry();
    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");

    EXPECT_FALSE(registry.findByUuid("no-such-uuid").has_value());
}

TEST(ServerRegistry, RemoveServerWithUnknownUuidIsNoOp) {
    auto registry = makeInsecureRegistry();
    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");

    registry.removeServer("no-such-uuid");

    EXPECT_TRUE(registry.findByUuid("uuid-1").has_value());
    EXPECT_EQ(registry.allServers().size(), 1u);
}

TEST(ServerRegistry, UpdateStateWithUnknownUuidIsSilentNoOp) {
    auto registry = makeInsecureRegistry();
    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");

    bool callbackInvoked = false;
    registry.setConnectionStateCallback(
        [&](const std::string&, ConnectionState) { callbackInvoked = true; });

    registry.updateState("no-such-uuid", ConnectionState::kConnected);

    EXPECT_FALSE(callbackInvoked);
    // The known entry's state must be untouched by the no-op call.
    auto entry = registry.findByUuid("uuid-1");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->state, ConnectionState::kConnecting);
}

TEST(ServerRegistry, UpdateStateDisconnectedKeepsEntry) {
    auto registry = makeInsecureRegistry();
    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");

    registry.updateState("uuid-1", ConnectionState::kDisconnected);
    auto entry = registry.findByUuid("uuid-1");

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->state, ConnectionState::kDisconnected);
}

// --- SilaClientBase wiring (True) --------------------------------------------

TEST(ServerRegistry, RegisterServerCreatesNonNullClient) {
    auto registry = makeInsecureRegistry();

    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");
    auto entry = registry.findByUuid("uuid-1");

    ASSERT_TRUE(entry.has_value());
    EXPECT_NE(entry->client, nullptr);
}

TEST(ServerRegistry, RegisterServerEntryChannelReturnsNonNullChannel) {
    auto registry = makeInsecureRegistry();

    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");
    auto entry = registry.findByUuid("uuid-1");

    ASSERT_TRUE(entry.has_value());
    EXPECT_NE(entry->client->channel(), nullptr);
}

TEST(ServerRegistry, ReregisterServerCreatesNewClientWithNewChannel) {
    // Re-bind after reboot (§3.7) must replace the client, not mutate it in
    // place: SilaClientBase is non-movable, so the old channel would
    // otherwise stay bound to the stale address.
    auto registry = makeInsecureRegistry();
    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");
    auto before = registry.findByUuid("uuid-1");
    ASSERT_TRUE(before.has_value());

    registry.registerServer("uuid-1", "192.168.1.99", 60051, "MyServerRenamed");
    auto after = registry.findByUuid("uuid-1");

    ASSERT_TRUE(after.has_value());
    EXPECT_NE(before->client, after->client);
    EXPECT_NE(before->client->channel(), after->client->channel());
}

// --- SilaClientBase wiring (False) -------------------------------------------

TEST(ServerRegistry, BareServerEntryHasNullClient) {
    ServerRegistry::ServerEntry entry;

    EXPECT_EQ(entry.client, nullptr);
}

TEST(ServerRegistry, RemoveServerAfterRegisterLeavesAllServersEmpty) {
    auto registry = makeInsecureRegistry();
    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");

    registry.removeServer("uuid-1");

    EXPECT_TRUE(registry.allServers().empty());
    EXPECT_FALSE(registry.findByUuid("uuid-1").has_value());
}

TEST(ServerRegistry, RegisterServerWithLockIdentifierPropagatesToClientConfig) {
    ClientConfig config;
    // No allowInsecure(): every target is a private-range IP, so
    // channelCredentials() takes the Part B p75 untrusted-TLS path (S74).
    config.setLockIdentifier("lock-42");
    ServerRegistry registry{config};

    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");
    auto entry = registry.findByUuid("uuid-1");

    ASSERT_TRUE(entry.has_value());
    ASSERT_NE(entry->client, nullptr);
    ASSERT_TRUE(entry->client->config().lockIdentifier().has_value());
    EXPECT_EQ(*entry->client->config().lockIdentifier(), "lock-42");

    // Smoke-test: the injector must have picked up the lock metadata during
    // construction and apply it without throwing. grpc::ClientContext keeps
    // the metadata it receives private, so this is the closest observable
    // check short of a live RPC.
    grpc::ClientContext ctx;
    EXPECT_NO_THROW(entry->client->metadataInjector().apply(ctx));
}

// --- Persistence (True) ------------------------------------------------------

TEST(ServerRegistry, PersistsAcrossReopenAsDisconnected) {
    const auto storePath = tempStorePath("PersistsAcrossReopen");
    std::filesystem::remove(storePath);

    {
        ServerRegistry registryA{insecureConfig(), storePath};
        registryA.registerServer("uuid-1", "192.168.1.10", 50051, "ServerOne");
        registryA.registerServer("uuid-2", "10.0.0.5", 50052, "ServerTwo");
    }  // registryA destructs; the store file must already hold both entries.

    ServerRegistry registryB{insecureConfig(), storePath};
    const auto all = registryB.allServers();

    ASSERT_EQ(all.size(), 2u);
    for (const auto& entry : all) {
        // Loaded entries have never connected in this process, so they must
        // come back kDisconnected with no live client (S36/G2 semantics).
        EXPECT_EQ(entry.state, ConnectionState::kDisconnected);
        EXPECT_EQ(entry.client, nullptr);
    }
    auto one = registryB.findByUuid("uuid-1");
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->host, "192.168.1.10");
    EXPECT_EQ(one->port, 50051);
    EXPECT_EQ(one->serverName, "ServerOne");
    auto two = registryB.findByUuid("uuid-2");
    ASSERT_TRUE(two.has_value());
    EXPECT_EQ(two->host, "10.0.0.5");
    EXPECT_EQ(two->port, 50052);
    EXPECT_EQ(two->serverName, "ServerTwo");

    std::filesystem::remove(storePath);
}

TEST(ServerRegistry, PersistingRegistryRejectsTabOrNewlineInFields) {
    const auto storePath = tempStorePath("RejectDelimiters");
    std::filesystem::remove(storePath);

    ServerRegistry registry{insecureConfig(), storePath};
    EXPECT_THROW(registry.registerServer("uuid-1", "10.0.0.5", 50051, "Server\tOne"),
                 std::invalid_argument);
    EXPECT_THROW(registry.registerServer("uuid\n1", "10.0.0.5", 50051, "ServerOne"),
                 std::invalid_argument);
    EXPECT_TRUE(registry.allServers().empty());
    // A non-persisting registry keeps accepting such names (pre-existing behaviour).
    ServerRegistry plain{insecureConfig()};
    EXPECT_NO_THROW(plain.registerServer("uuid-1", "10.0.0.5", 50051, "Server\tOne"));

    std::filesystem::remove(storePath);
}

TEST(ServerRegistry, RemovePersists) {
    const auto storePath = tempStorePath("RemovePersists");
    std::filesystem::remove(storePath);

    {
        ServerRegistry registryA{insecureConfig(), storePath};
        registryA.registerServer("uuid-1", "192.168.1.10", 50051, "ServerOne");
        registryA.registerServer("uuid-2", "10.0.0.5", 50052, "ServerTwo");
        registryA.removeServer("uuid-1");
    }

    ServerRegistry registryB{insecureConfig(), storePath};
    const auto all = registryB.allServers();

    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].uuid, "uuid-2");
    EXPECT_FALSE(registryB.findByUuid("uuid-1").has_value());

    std::filesystem::remove(storePath);
}

TEST(ServerRegistry, CaseInsensitiveUuidSurvives) {
    // Part A p90: UUID keys compare without case; that must hold across the
    // save/load round trip too, not just within one live map.
    const auto storePath = tempStorePath("CaseInsensitiveUuid");
    std::filesystem::remove(storePath);

    {
        ServerRegistry registryA{insecureConfig(), storePath};
        registryA.registerServer("ABC-uuid", "192.168.1.10", 50051, "ServerOne");
    }

    ServerRegistry registryB{insecureConfig(), storePath};
    auto entry = registryB.findByUuid("abc-uuid");

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->host, "192.168.1.10");

    std::filesystem::remove(storePath);
}

// --- Persistence (False) -----------------------------------------------------

TEST(ServerRegistry, CorruptStoreThrows) {
    const auto storePath = tempStorePath("CorruptStore");
    std::filesystem::remove(storePath);
    {
        std::ofstream out{storePath, std::ios::trunc};
        // Port field "80x" fails the all-digits guard.
        out << "uuid-1\t192.168.1.10\t80x\tMyServer\n";
    }

    try {
        ServerRegistry registry{insecureConfig(), storePath};
        FAIL() << "expected ServerRegistry construction to throw on a corrupt store";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string{e.what()}.find(storePath.string()), std::string::npos);
    }

    std::filesystem::remove(storePath);
}

TEST(ServerRegistry, MissingFileIsEmpty) {
    const auto storePath = tempStorePath("MissingFile");
    std::filesystem::remove(storePath);  // guarantee it does not exist.

    ServerRegistry registry{insecureConfig(), storePath};

    EXPECT_TRUE(registry.allServers().empty());
}

TEST(ServerRegistry, NoPathNoFile) {
    // Default (empty) storePath keeps persistence off — the pre-existing
    // in-memory-only behaviour every other test in this file relies on.
    // saveState() is gated on `!storePath_.empty()`, so with no path supplied
    // registerServer must never touch the filesystem: the working directory's
    // entry count before and after must match exactly.
    std::size_t entriesBefore = 0;
    for (const auto& _ : std::filesystem::directory_iterator{std::filesystem::current_path()}) {
        (void)_;
        ++entriesBefore;
    }

    auto registry = makeInsecureRegistry();
    registry.registerServer("uuid-1", "192.168.1.10", 50051, "MyServer");

    std::size_t entriesAfter = 0;
    for (const auto& _ : std::filesystem::directory_iterator{std::filesystem::current_path()}) {
        (void)_;
        ++entriesAfter;
    }
    EXPECT_EQ(entriesBefore, entriesAfter);
}

}  // namespace
