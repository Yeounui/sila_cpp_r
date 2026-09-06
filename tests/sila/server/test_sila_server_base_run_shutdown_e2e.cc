// End-to-end tests for SiLAServerBase::Run and SiLAServerBase::Shutdown
// (architecture.md §3.1): Run() assembles TLS credentials, binds the
// listening port, registers every service from FeatureRegistry, and starts
// the gRPC server (or throws if BuildAndStart fails); Shutdown() interrupts
// every registered ObservableCommandManager, stops mDNS if enabled, and
// shuts down the gRPC server. Each test drives the flow through a real
// SiLAServerBase — a Builder is the only way to obtain one, so False inputs
// for Run() are three distinct malformed-PEM shapes rather than three
// distinct exception types: Run() has exactly one throw site
// (BuildAndStart() returning nullptr), confirmed empirically before writing
// these tests (see below).
#include <sila/server/SiLAServerBase.h>

#include <sila/common/error/SiLAErrorException.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/SiLAServiceImpl.h>
#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/discovery/MdnsPublisher.h>
#include <sila/server/features/AuthenticationServiceImpl.h>
#include <sila/server/features/ConnectionConfigurationServiceImpl.h>
#include <sila/server/recovery/ErrorRecoveryServiceImpl.h>
#include <sila/server/recovery/RecoverableErrorGate.h>
#include <sila/server/property/ObservablePropertyManager.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using sila2::ObservableCommandExecution;
using sila2::ObservableCommandManager;
using sila2::ObservablePropertyManager;
using sila2::SiLAServerBase;
using State = ObservableCommandExecution::State;

namespace errorrecovery_proto = sila2::errorrecovery_proto;
using sila2::recovery::kRecoverableErrorsPropertyId;

// Distinct ports per test so servers never contend for the same listener —
// SiLAServerBase::Run only exposes port control through WithDiscovery(), so
// every Run()-driving test below enables discovery even when it is not
// otherwise under test.
constexpr uint16_t kPortRunListens = 50250;
constexpr uint16_t kPortRunMultiService = 50251;
constexpr uint16_t kPortRunErrorRecoveryAdvert = 50263;
constexpr uint16_t kPortRunBlocks = 50252;
constexpr uint16_t kPortRunGarbagePem = 50253;
constexpr uint16_t kPortRunSwappedPem = 50254;
constexpr uint16_t kPortRunTruncatedPem = 50255;
constexpr uint16_t kPortShutdownInterrupts = 50256;
constexpr uint16_t kPortShutdownMdns = 50257;
constexpr uint16_t kPortShutdownTwice = 50258;
constexpr uint16_t kPortShutdownWaiting = 50259;
constexpr uint16_t kPortShutdownFinished = 50260;
constexpr uint16_t kPortShutdownIdleSubscriber = 50261;
constexpr uint16_t kPortDestructorIdleSubscriber = 50262;
// Not 50264/50265: test_error_recovery_service_e2e.cc already binds 50264.
constexpr uint16_t kPortMdnsFixedPort = 50266;
constexpr uint16_t kPortMdnsFailedRun = 50267;

// Real local channel dialed against server's own self-signed certificate —
// same pattern as tests/interop/test_interop.cc's channel().
std::shared_ptr<grpc::Channel> dialChannel(const SiLAServerBase& server, uint16_t port) {
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = server.certificatePem();
    return grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(opts));
}

// Minimal stubs reused from test_auth_session_client_e2e.cc's pattern: only
// used to prove AuthenticationService is reachable once Run() registers it
// alongside SiLAService, not to exercise auth decision logic.
struct AcceptingVerifier : sila2::auth::CredentialVerifier {
    std::optional<std::string> verify(const std::string& user, const std::string&) override {
        return user;
    }
};

struct AllowAllPolicy : sila2::auth::AccessPolicy {
    bool isAllowed(const std::string&, const std::string&) const override { return true; }
    std::vector<std::string> allowedFqis(const std::string&) const override { return {}; }
};

}  // namespace

// ---------------------------------------------------------------------------
// SiLAServerBase::Run — True (positive) paths
// ---------------------------------------------------------------------------

TEST(SiLAServerBaseRun, StartsListeningAndServesUnaryRpc) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(kPortRunListens)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    server.Run(false);  // block=false: must return without waiting

    auto stub = sila2::silaservice_proto::SiLAService::NewStub(
        dialChannel(server, kPortRunListens));
    grpc::ClientContext ctx;
    sila2::silaservice_proto::Get_ServerUUID_Parameters req;
    sila2::silaservice_proto::Get_ServerUUID_Responses resp;
    auto status = stub->Get_ServerUUID(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.serveruuid().value(), server.serverConfig().uuid());

    server.Shutdown();
}

TEST(SiLAServerBaseRun, RestoresPersistentConnectionClientsBeforeListeningAndClearsThemOnShutdown) {
    const auto storePath = std::filesystem::temp_directory_path() /
                           "sila-connection-configuration-run-test.state";
    std::filesystem::remove(storePath);
    {
        std::ofstream state{storePath};
        state << "mode\t0\nclient\tpersisted\t127.0.0.1\t1\n";
    }
    // Owner-only like saveState() writes it: loadState() refuses a store that
    // group or others could write (Codex review of SC32).
    std::filesystem::permissions(storePath,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);

    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(0)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .WithConnectionConfiguration(
                          storePath, grpc::InsecureChannelCredentials())
                      .Build();
    auto services = server.featureRegistry().registeredServices();
    auto serviceIt = std::find_if(services.begin(), services.end(), [](grpc::Service* service) {
        return dynamic_cast<sila2::ConnectionConfigurationServiceImpl*>(service) != nullptr;
    });
    ASSERT_NE(serviceIt, services.end());
    auto* connectionService =
        dynamic_cast<sila2::ConnectionConfigurationServiceImpl*>(*serviceIt);

    server.Run(false);

    auto stub = sila2::connconfig_proto::ConnectionConfigurationService::NewStub(
        dialChannel(server, server.port()));
    grpc::ClientContext context;
    sila2::connconfig_proto::Get_ConfiguredSiLAClients_Parameters request;
    sila2::connconfig_proto::Get_ConfiguredSiLAClients_Responses response;
    ASSERT_TRUE(stub->Get_ConfiguredSiLAClients(&context, request, &response).ok());
    ASSERT_EQ(response.configuredsilaclients_size(), 1);
    EXPECT_EQ(response.configuredsilaclients(0).clientname().value(), "persisted");

    server.Shutdown();

    grpc::ServerContext localContext;
    response.Clear();
    EXPECT_TRUE(connectionService->Get_ConfiguredSiLAClients(
        &localContext, &request, &response).ok());
    EXPECT_EQ(response.configuredsilaclients_size(), 0);
    std::filesystem::remove(storePath);
}

TEST(SiLAServerBaseRun, RegistersEveryFeatureServiceNotJustSiLAService) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithAuthentication(std::make_unique<AcceptingVerifier>(),
                                          std::make_unique<AllowAllPolicy>(),
                                          {})
                      .WithDiscovery(kPortRunMultiService)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    server.Run(false);

    // AuthenticationService is a second, independently-registered gRPC
    // service — reaching it proves Run()'s loop over
    // featureRegistry_.registeredServices() covers more than just the
    // mandatory SiLAService.
    auto stub = sila2::auth_proto::AuthenticationService::NewStub(
        dialChannel(server, kPortRunMultiService));
    grpc::ClientContext ctx;
    sila2::auth_proto::Login_Parameters req;
    req.mutable_useridentification()->set_value("alice");
    req.mutable_password()->set_value("anything");
    req.mutable_requestedserver()->set_value(server.serverConfig().uuid());
    sila2::auth_proto::Login_Responses resp;
    auto status = stub->Login(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(resp.accesstoken().value().empty());

    server.Shutdown();
}

// Pins S8b Option A (architecture-v2.md §3.12): ErrorRecoveryService v1 is
// never advertised or served, only v2. WithErrorRecovery() only registers
// the v2 gRPC service (see kErrorRecoveryServiceFqi in
// ErrorRecoveryServiceImpl.h) — there is no v1 service left to dial.
TEST(SiLAServerBaseRun, AdvertisesErrorRecoveryV2OnlyNotV1) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(kPortRunErrorRecoveryAdvert)
                      .WithErrorRecovery()
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();
    server.Run(false);

    auto stub = sila2::silaservice_proto::SiLAService::NewStub(
        dialChannel(server, kPortRunErrorRecoveryAdvert));

    grpc::ClientContext listCtx;
    sila2::silaservice_proto::Get_ImplementedFeatures_Parameters listRequest;
    sila2::silaservice_proto::Get_ImplementedFeatures_Responses listResponse;
    ASSERT_TRUE(stub->Get_ImplementedFeatures(&listCtx, listRequest, &listResponse).ok());

    std::vector<std::string> fqis;
    for (const auto& feature : listResponse.implementedfeatures()) {
        fqis.push_back(feature.value());
    }
    EXPECT_NE(std::find(fqis.begin(), fqis.end(), std::string{sila2::kErrorRecoveryServiceFqi}),
              fqis.end());
    EXPECT_EQ(std::find(fqis.begin(), fqis.end(), "org.silastandard/core/ErrorRecoveryService/v1"),
              fqis.end());

    // GetFeatureDefinition on the unadvertised v1 FQI must fail the same way
    // it would for any other unimplemented feature (SiLAServiceImpl.cc:56-58).
    grpc::ClientContext defCtx;
    sila2::silaservice_proto::GetFeatureDefinition_Parameters defRequest;
    defRequest.mutable_featureidentifier()->set_value("org.silastandard/core/ErrorRecoveryService/v1");
    sila2::silaservice_proto::GetFeatureDefinition_Responses defResponse;
    const grpc::Status defStatus = stub->GetFeatureDefinition(&defCtx, defRequest, &defResponse);

    const auto reconstructed = sila2::error::fromGrpcStatus(defStatus);
    ASSERT_NE(reconstructed, nullptr);
    const auto* definedError = dynamic_cast<const sila2::error::DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(definedError, nullptr);
    EXPECT_EQ(definedError->errorIdentifier(),
              "org.silastandard/core/SiLAService/v1/DefinedExecutionError/UnimplementedFeature");

    server.Shutdown();
}

TEST(SiLAServerBaseRun, BlockTrueBlocksCallingThreadUntilShutdown) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(kPortRunBlocks)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    std::atomic<bool> runReturned{false};
    std::thread runner([&] {
        server.Run(true);  // block=true: must not return before Shutdown()
        runReturned = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    EXPECT_FALSE(runReturned.load()) << "Run(true) returned before Shutdown() was called";

    server.Shutdown();
    runner.join();

    EXPECT_TRUE(runReturned.load()) << "Run(true) never returned after Shutdown()";
}

// ---------------------------------------------------------------------------
// SiLAServerBase::Run — False (negative/rejection) paths
// All CAUGHT: BuildAndStart() returns nullptr for malformed TLS material,
// and Run() surfaces that as std::runtime_error. Builder::Build() already
// rejects empty PEM strings (see test_sila_server_base.cc), so the only way
// to reach this branch is non-empty-but-unusable PEM content.
// ---------------------------------------------------------------------------

TEST(SiLAServerBaseRun, GarbagePemMaterialThrowsRuntimeError) {
    auto server = SiLAServerBase::Builder()
                      .WithCertificate("not-a-real-certificate-pem", "not-a-real-private-key-pem")
                      .WithDiscovery(kPortRunGarbagePem)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    EXPECT_THROW(server.Run(false), std::runtime_error);
}

TEST(SiLAServerBaseRun, SwappedCertificateAndKeyThrowsRuntimeError) {
    // Real, individually-valid PEM material — but WithCertificate is given
    // the key where the certificate belongs and vice versa.
    auto valid = SiLAServerBase::Builder()
                     .WithSelfSignedCertificate("localhost", "127.0.0.1")
                     .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                     .Build();

    auto swapped = SiLAServerBase::Builder()
                       .WithCertificate(valid.privateKeyPem(), valid.certificatePem())
                       .WithDiscovery(kPortRunSwappedPem)
                       .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                       .Build();

    EXPECT_THROW(swapped.Run(false), std::runtime_error);
}

TEST(SiLAServerBaseRun, TruncatedCertificateThrowsRuntimeError) {
    auto valid = SiLAServerBase::Builder()
                     .WithSelfSignedCertificate("localhost", "127.0.0.1")
                     .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                     .Build();
    const std::string truncatedCert = valid.certificatePem().substr(0, valid.certificatePem().size() / 2);

    auto server = SiLAServerBase::Builder()
                      .WithCertificate(truncatedCert, valid.privateKeyPem())
                      .WithDiscovery(kPortRunTruncatedPem)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    EXPECT_THROW(server.Run(false), std::runtime_error);
}

// ---------------------------------------------------------------------------
// mDNS publish-after-bind ordering (audit S31): Build() must construct the
// publisher (SiLAServiceImpl captures it for SetServerName) without
// advertising, and Run() must advertise only after BuildAndStart has written
// the bound port back. No multicast browse test here: test_mdns_browser.cc's
// mdnsPortAvailable() skips on WSL2 (/proc/version names microsoft), so a
// browse-based assertion would never run on this host. Every case below
// reads publisher state directly instead.
// ---------------------------------------------------------------------------

TEST(SiLAServerBaseRun, MdnsAdvertisesTheOsSelectedPortWhenBuiltWithPortZero) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(0)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();
    ASSERT_NE(server.mdnsPublisher(), nullptr);

    server.Run(false);

    // Before the fix the publisher's port stayed 0 forever -- it was set at
    // construction time in Build(), before BuildAndStart chose a real one.
    EXPECT_NE(server.port(), 0);
    EXPECT_EQ(server.mdnsPublisher()->port(), server.port());

    server.Shutdown();
}

TEST(SiLAServerBaseRun, MdnsAdvertisesTheRequestedPortWhenBuiltWithAFixedPort) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(kPortMdnsFixedPort)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    server.Run(false);

    // Pins that the reordering did not change the fixed-port path, which
    // every other e2e test in this file relies on.
    EXPECT_EQ(server.port(), kPortMdnsFixedPort);
    EXPECT_EQ(server.mdnsPublisher()->port(), kPortMdnsFixedPort);

    server.Shutdown();
}

TEST(SiLAServerBaseRun, MdnsAdvertisedPortIsDialableAtTheMomentItIsAdvertised) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(0)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    server.Run(false);

    // The ordering assertion in its strongest form: the number the SRV
    // record advertises accepts a real TLS connection right now.
    const uint16_t advertised = server.mdnsPublisher()->port();
    auto stub = sila2::silaservice_proto::SiLAService::NewStub(dialChannel(server, advertised));
    grpc::ClientContext ctx;
    sila2::silaservice_proto::Get_ServerUUID_Parameters req;
    sila2::silaservice_proto::Get_ServerUUID_Responses resp;
    auto status = stub->Get_ServerUUID(&ctx, req, &resp);

    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(server.mdnsPublisher()->isPublishing());

    server.Shutdown();
}

TEST(SiLAServerBaseRun, DoesNotPublishBeforeRun) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(0)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();
    ASSERT_NE(server.mdnsPublisher(), nullptr);

    // Direct regression guard: on the old code Build() published, so this
    // would be true. Not a port check -- before Run() the port reads 0
    // under both old and new code, so port alone cannot tell them apart.
    EXPECT_FALSE(server.mdnsPublisher()->isPublishing());
}

TEST(SiLAServerBaseRun, FailedRunLeavesDiscoveryUnpublished) {
    auto server = SiLAServerBase::Builder()
                      .WithCertificate("not-a-real-certificate-pem", "not-a-real-private-key-pem")
                      .WithDiscovery(kPortMdnsFailedRun)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    EXPECT_THROW(server.Run(false), std::runtime_error);

    // A server that failed to bind must not be discoverable. setPort() is
    // never reached, so the port is still the one requested at Build().
    EXPECT_FALSE(server.mdnsPublisher()->isPublishing());
    EXPECT_EQ(server.mdnsPublisher()->port(), kPortMdnsFailedRun);
}

// S53: Part B p75 MUST -- discovery is enabled by default and cannot be
// disabled, so a server that never calls WithDiscovery() still gets a
// publisher and still starts. Formerly WithoutDiscoveryNoPublisherIsCreated
// AndRunStillStarts, which pinned the opposite (opt-in) behaviour.
TEST(SiLAServerBaseRun, DiscoveryIsEnabledByDefaultWithoutWithDiscovery) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    EXPECT_NE(server.mdnsPublisher(), nullptr);
    EXPECT_NO_THROW(server.Run(false));
    EXPECT_NO_THROW(server.Shutdown());
}

// S53: the always-on default port, unexercised by the test above.
TEST(SiLAServerBaseRun, WithoutWithDiscoveryAdvertisesTheDefaultPort) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    ASSERT_NE(server.mdnsPublisher(), nullptr);
    EXPECT_EQ(server.mdnsPublisher()->port(), 50051);
}

// ---------------------------------------------------------------------------
// Builder::WithPersistentUuid (audit S30b): a caller-supplied file makes the
// server's ServerUUID survive a restart without a caller-supplied
// ServerConfig. Build() alone is enough to exercise every case -- Run()
// would only add an unrelated port to manage.
// ---------------------------------------------------------------------------

namespace {

// Unique path per test so a run left over from a previous failed run cannot
// leak a stale UUID or malformed content into the next one.
std::filesystem::path uniquePersistentUuidPath(const std::string& label) {
    return std::filesystem::temp_directory_path() /
           ("sila2-test-persistent-uuid-" + label + ".txt");
}

}  // namespace

TEST(SiLAServerBaseBuilder, WithPersistentUuidCreatesFileAndUsesConformantUuid) {
    const auto path = uniquePersistentUuidPath("creates-file");
    std::filesystem::remove(path);

    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithPersistentUuid(path)
                      .Build();

    ASSERT_TRUE(std::filesystem::exists(path));
    std::ifstream in{path};
    std::string fileContent;
    std::getline(in, fileContent);
    EXPECT_EQ(fileContent, server.serverConfig().uuid());
    EXPECT_TRUE(std::regex_match(server.serverConfig().uuid(),
        std::regex{"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"}));

    std::filesystem::remove(path);
}

TEST(SiLAServerBaseBuilder, WithPersistentUuidReusesExistingUuidAcrossBuilds) {
    const auto path = uniquePersistentUuidPath("reuses-across-builds");
    std::filesystem::remove(path);

    auto first = SiLAServerBase::Builder()
                     .WithSelfSignedCertificate("localhost", "127.0.0.1")
                     .WithPersistentUuid(path)
                     .Build();
    auto second = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithPersistentUuid(path)
                      .Build();

    EXPECT_EQ(first.serverConfig().uuid(), second.serverConfig().uuid());

    std::filesystem::remove(path);
}

TEST(SiLAServerBaseBuilder, WithPersistentUuidToleratesTrailingNewline) {
    const auto path = uniquePersistentUuidPath("trailing-newline");
    const std::string kUuid = "12345678-1234-1234-1234-123456789abc";
    {
        std::ofstream out{path};
        out << kUuid << "\n";
    }

    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithPersistentUuid(path)
                      .Build();

    EXPECT_EQ(server.serverConfig().uuid(), kUuid);

    std::filesystem::remove(path);
}

TEST(SiLAServerBaseBuilder, WithPersistentUuidThrowsOnMalformedFile) {
    const auto path = uniquePersistentUuidPath("malformed");
    {
        std::ofstream out{path};
        out << "not-a-uuid";
    }

    EXPECT_THROW(SiLAServerBase::Builder()
                     .WithSelfSignedCertificate("localhost", "127.0.0.1")
                     .WithPersistentUuid(path)
                     .Build(),
                 std::runtime_error);

    // No silent overwrite: the malformed content must survive the rejection.
    std::ifstream in{path};
    std::string fileContent;
    std::getline(in, fileContent);
    EXPECT_EQ(fileContent, "not-a-uuid");

    std::filesystem::remove(path);
}

TEST(SiLAServerBaseBuilder, WithPersistentUuidThrowsOnNonconformantUuid) {
    const auto path = uniquePersistentUuidPath("nonconformant");
    const std::string kUppercaseUuid = "12345678-1234-1234-1234-123456789ABC";
    {
        std::ofstream out{path};
        out << kUppercaseUuid;
    }

    EXPECT_THROW(SiLAServerBase::Builder()
                     .WithSelfSignedCertificate("localhost", "127.0.0.1")
                     .WithPersistentUuid(path)
                     .Build(),
                 std::runtime_error);

    std::ifstream in{path};
    std::string fileContent;
    std::getline(in, fileContent);
    EXPECT_EQ(fileContent, kUppercaseUuid);

    std::filesystem::remove(path);
}

TEST(SiLAServerBaseBuilder, WithPersistentUuidConflictsWithWithConfig) {
    const auto path = uniquePersistentUuidPath("conflicts-with-config");

    EXPECT_THROW(SiLAServerBase::Builder()
                     .WithSelfSignedCertificate("localhost", "127.0.0.1")
                     .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("TestServer"))
                     .WithPersistentUuid(path)
                     .Build(),
                 std::logic_error);
}

// ---------------------------------------------------------------------------
// SiLAServerBase::Shutdown — True (positive) paths
// ---------------------------------------------------------------------------

TEST(SiLAServerBaseShutdown, InterruptsAllRegisteredCommandManagers) {
    ObservableCommandManager mgr;
    auto exec = mgr.addCommand();
    exec->start();
    ASSERT_FALSE(exec->isInterruptionRequested());

    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(kPortShutdownInterrupts)
                      .RegisterCommandManager(&mgr)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();
    server.Run(false);

    server.Shutdown();

    EXPECT_TRUE(exec->isInterruptionRequested());
}

TEST(SiLAServerBaseShutdown, StopsMdnsPublisherPromptlyWhenDiscoveryEnabled) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(kPortShutdownMdns)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();
    server.Run(false);

    const auto t0 = std::chrono::steady_clock::now();
    server.Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Regression guard against MdnsPublisher::shutdown() deadlocking the
    // listen thread join — mDNS's own probe/announce cycle runs in
    // milliseconds (ServerConfig's default mdnsProbeWait is 250ms), so a
    // multi-second bound leaves ample margin without masking a real hang.
    EXPECT_LT(elapsed, std::chrono::seconds{5});
}

TEST(SiLAServerBaseShutdown, WithoutPriorRunDoesNotThrow) {
    // server_ is still null (Run() never called); Shutdown()'s `if (server_)`
    // guard must make this a no-op rather than a null-deref.
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();

    EXPECT_NO_THROW(server.Shutdown());
}

// Pins audit 3.2m: a Subscribe_RecoverableErrors handler parks in
// Subscription::waitForNext(), on a condition variable gRPC's Shutdown()
// knows nothing about. The subscriber below is deliberately left idle — no
// error is ever published — because that is the exact state 3.2m describes;
// no deadline on grpc::Server::Shutdown() would unpark this handler, only
// cancelling the subscription (ObservablePropertyManager::shutdown()) does.
TEST(SiLAServerBaseShutdown, ReturnsWithIdleRecoverableErrorsSubscriber) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(kPortShutdownIdleSubscriber)
                      .WithErrorRecovery()
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();
    server.Run(false);

    auto stub = errorrecovery_proto::ErrorRecoveryService::NewStub(
        dialChannel(server, kPortShutdownIdleSubscriber));
    grpc::ClientContext subCtx;
    errorrecovery_proto::Subscribe_RecoverableErrors_Parameters request;
    auto reader = stub->Subscribe_RecoverableErrors(&subCtx, request);

    auto* propMgr = server.errorRecoveryPropertyManager();
    ASSERT_NE(propMgr, nullptr);

    // Registration happens on the gRPC handler thread, so this bounded spin
    // is the deterministic wait for it to have reached its park — not a
    // bare sleep standing in for synchronisation.
    for (int attempt = 0; attempt < 200 && propMgr->subscriberCount(kRecoverableErrorsPropertyId) == 0;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_EQ(propMgr->subscriberCount(kRecoverableErrorsPropertyId), 1u);

    auto shutdownFuture = std::async(std::launch::async, [&server] { server.Shutdown(); });
    const bool returned = shutdownFuture.wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    EXPECT_TRUE(returned);
    if (!returned) {
        // Rescue so a regression fails this test instead of hanging ctest.
        // Note this is the ONLY lever that works: subCtx.TryCancel() would not
        // wake a handler parked on Subscription::cv_, which is the finding.
        propMgr->shutdown();
        shutdownFuture.wait();
    }
    subCtx.TryCancel();
    reader->Finish();
}

// Same idle-subscriber setup as above, but drives destruction by scope exit
// on a worker thread instead of an explicit Shutdown() call — SiLAServerBase
// has a deleted move constructor and a private constructor, so it cannot be
// held in optional/unique_ptr to trigger destruction from the test thread
// directly. Pins the destructor path, which before this fix neither cancels
// nor joins before components_ dies.
TEST(SiLAServerBaseShutdown, DestructorReturnsWithIdleRecoverableErrorsSubscriber) {
    std::promise<std::string> certPromise;
    std::promise<ObservablePropertyManager*> mgrPromise;
    std::promise<void> goPromise;

    auto serverFuture = std::async(std::launch::async, [&] {
        try {
            auto server = SiLAServerBase::Builder()
                              .WithSelfSignedCertificate("localhost", "127.0.0.1")
                              .WithDiscovery(kPortDestructorIdleSubscriber)
                              .WithErrorRecovery()
                              .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                              .Build();
            server.Run(false);
            certPromise.set_value(server.certificatePem());
            mgrPromise.set_value(server.errorRecoveryPropertyManager());
            goPromise.get_future().wait();
            // server destructs here, with no explicit Shutdown() call.
        } catch (...) {
            // Build()/Run() failed: surface it through whichever promise
            // downstream is still waiting on instead of leaving certPromise
            // (and its get_future().get()) parked forever.
            certPromise.set_exception(std::current_exception());
            mgrPromise.set_exception(std::current_exception());
        }
    });

    // Releases the worker parked on goPromise.get_future().wait() on every
    // exit from this test body -- including the unwind through a thrown
    // certPromise/mgrPromise .get() below -- so serverFuture's destructor
    // never blocks. Must be declared before the first wait on a worker-fed
    // promise.
    bool releasedGoPromise = false;
    struct GoPromiseGuard {
        std::promise<void>& promise;
        bool& released;
        ~GoPromiseGuard() {
            if (!released) {
                released = true;
                promise.set_value();
            }
        }
    } goPromiseGuard{goPromise, releasedGoPromise};

    const std::string cert = certPromise.get_future().get();
    auto* propMgr = mgrPromise.get_future().get();
    EXPECT_NE(propMgr, nullptr);
    if (propMgr == nullptr) return;

    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = cert;
    auto channel = grpc::CreateChannel(
        "localhost:" + std::to_string(kPortDestructorIdleSubscriber), grpc::SslCredentials(opts));
    auto stub = errorrecovery_proto::ErrorRecoveryService::NewStub(channel);
    grpc::ClientContext subCtx;
    errorrecovery_proto::Subscribe_RecoverableErrors_Parameters request;
    auto reader = stub->Subscribe_RecoverableErrors(&subCtx, request);

    for (int attempt = 0; attempt < 200 && propMgr->subscriberCount(kRecoverableErrorsPropertyId) == 0;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    EXPECT_EQ(propMgr->subscriberCount(kRecoverableErrorsPropertyId), 1u);

    releasedGoPromise = true;
    goPromise.set_value();
    const bool returned = serverFuture.wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    EXPECT_TRUE(returned);
    if (!returned) {
        // The manager pointer stays valid during the rescue: a destructor
        // stuck inside Shutdown() has not yet reached member destruction.
        propMgr->shutdown();
        serverFuture.wait();
    }
    subCtx.TryCancel();
    reader->Finish();
}

// ---------------------------------------------------------------------------
// SiLAServerBase::Shutdown — False (negative/invariant-violation) paths
// Shutdown() has no documented error path (architecture.md gives it none),
// so these exercise invariants Shutdown() does not itself validate.
// ---------------------------------------------------------------------------

// CAUGHT (safely, by downstream idempotency): Shutdown() has no re-entrancy
// guard of its own, but grpc::Server::Shutdown(), MdnsPublisher::shutdown(),
// and ObservableCommandManager::interruptAll() are each independently safe
// to call more than once, so a second call does not throw or crash.
TEST(SiLAServerBaseShutdown, CalledTwiceDoesNotThrow) {
    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(kPortShutdownTwice)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();
    server.Run(false);
    server.Shutdown();

    EXPECT_NO_THROW(server.Shutdown());
}

// UNCAUGHT: interruptAll() -> requestInterruption() is an unconditional
// atomic flag set (ObservableCommandExecution.h) with no precondition on
// state, so Shutdown() happily "interrupts" a command that was registered
// but never start()ed. The execution's state is left at Waiting — Shutdown()
// does not force any transition.
TEST(SiLAServerBaseShutdown, InterruptsCommandThatWasNeverStarted) {
    ObservableCommandManager mgr;
    auto exec = mgr.addCommand();
    ASSERT_EQ(exec->state(), State::Waiting);

    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(kPortShutdownWaiting)
                      .RegisterCommandManager(&mgr)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();
    server.Run(false);

    server.Shutdown();

    EXPECT_TRUE(exec->isInterruptionRequested());
    EXPECT_EQ(exec->state(), State::Waiting);
}

// UNCAUGHT: same unconditional requestInterruption() also fires on a command
// that already finished before Shutdown() ran — a meaningless but harmless
// no-op that nothing in the pipeline guards against.
TEST(SiLAServerBaseShutdown, InterruptsCommandThatAlreadyFinished) {
    ObservableCommandManager mgr;
    auto exec = mgr.addCommand();
    exec->start();
    exec->finish();
    ASSERT_EQ(exec->state(), State::FinishedSuccessfully);

    auto server = SiLAServerBase::Builder()
                      .WithSelfSignedCertificate("localhost", "127.0.0.1")
                      .WithDiscovery(kPortShutdownFinished)
                      .RegisterCommandManager(&mgr)
                      .WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .Build();
    server.Run(false);

    server.Shutdown();

    EXPECT_TRUE(exec->isInterruptionRequested());
}
