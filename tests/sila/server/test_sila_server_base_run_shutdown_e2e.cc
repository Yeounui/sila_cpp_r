// End-to-end tests for SilaServerBase::Run and SilaServerBase::Shutdown
// (architecture.md §3.1): run() assembles TLS credentials, binds the
// listening port, registers every service from FeatureRegistry, and starts
// the gRPC server (or throws if BuildAndStart fails); shutdown() interrupts
// every registered ObservableCommandManager, stops mDNS if enabled, and
// shuts down the gRPC server. Each test drives the flow through a real
// SilaServerBase — a Builder is the only way to obtain one, so False inputs
// for run() are three distinct malformed-PEM shapes rather than three
// distinct exception types: run() has exactly one throw site
// (BuildAndStart() returning nullptr), confirmed empirically before writing
// these tests (see below).
#include <sila/server/SilaServerBase.h>

#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/SilaServiceImpl.h>
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
using sila2::SilaServerBase;
using State = ObservableCommandExecution::State;

namespace errorrecovery_proto = sila2::errorrecovery_proto;
using sila2::recovery::kRecoverableErrorsPropertyId;

// Distinct ports per test so servers never contend for the same listener —
// SilaServerBase::Run only exposes port control through withDiscovery(), so
// every run()-driving test below enables discovery even when it is not
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
std::shared_ptr<grpc::Channel> dialChannel(const SilaServerBase& server, uint16_t port) {
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = server.certificatePem();
    return grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(opts));
}

// Minimal stubs reused from test_auth_session_client_e2e.cc's pattern: only
// used to prove AuthenticationService is reachable once run() registers it
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
// SilaServerBase::Run — True (positive) paths
// ---------------------------------------------------------------------------

TEST(SilaServerBaseRun, StartsListeningAndServesUnaryRpc) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(kPortRunListens)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    server.run(false);  // block=false: must return without waiting

    auto stub = sila2::silaservice_proto::SiLAService::NewStub(
        dialChannel(server, kPortRunListens));
    grpc::ClientContext ctx;
    sila2::silaservice_proto::Get_ServerUUID_Parameters req;
    sila2::silaservice_proto::Get_ServerUUID_Responses resp;
    auto status = stub->Get_ServerUUID(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.serveruuid().value(), server.serverConfig().uuid());

    server.shutdown();
}

TEST(SilaServerBaseRun, RestoresPersistentConnectionClientsBeforeListeningAndClearsThemOnShutdown) {
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

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(0)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withConnectionConfiguration(
                          storePath, grpc::InsecureChannelCredentials())
                      .build();
    auto services = server.featureRegistry().registeredServices();
    auto serviceIt = std::find_if(services.begin(), services.end(), [](grpc::Service* service) {
        return dynamic_cast<sila2::ConnectionConfigurationServiceImpl*>(service) != nullptr;
    });
    ASSERT_NE(serviceIt, services.end());
    auto* connectionService =
        dynamic_cast<sila2::ConnectionConfigurationServiceImpl*>(*serviceIt);

    server.run(false);

    auto stub = sila2::connconfig_proto::ConnectionConfigurationService::NewStub(
        dialChannel(server, server.port()));
    grpc::ClientContext context;
    sila2::connconfig_proto::Get_ConfiguredSiLAClients_Parameters request;
    sila2::connconfig_proto::Get_ConfiguredSiLAClients_Responses response;
    ASSERT_TRUE(stub->Get_ConfiguredSiLAClients(&context, request, &response).ok());
    ASSERT_EQ(response.configuredsilaclients_size(), 1);
    EXPECT_EQ(response.configuredsilaclients(0).clientname().value(), "persisted");

    server.shutdown();

    grpc::ServerContext localContext;
    response.Clear();
    EXPECT_TRUE(connectionService->Get_ConfiguredSiLAClients(
        &localContext, &request, &response).ok());
    EXPECT_EQ(response.configuredsilaclients_size(), 0);
    std::filesystem::remove(storePath);
}

TEST(SilaServerBaseRun, RegistersEveryFeatureServiceNotJustSiLAService) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withAuthentication(std::make_unique<AcceptingVerifier>(),
                                          std::make_unique<AllowAllPolicy>(),
                                          {})
                      .withDiscovery(kPortRunMultiService)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    server.run(false);

    // AuthenticationService is a second, independently-registered gRPC
    // service — reaching it proves run()'s loop over
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

    server.shutdown();
}

// Pins S8b Option A (architecture-v2.md §3.12): ErrorRecoveryService v1 is
// never advertised or served, only v2. withErrorRecovery() only registers
// the v2 gRPC service (see kErrorRecoveryServiceFqi in
// ErrorRecoveryServiceImpl.h) — there is no v1 service left to dial.
TEST(SilaServerBaseRun, AdvertisesErrorRecoveryV2OnlyNotV1) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(kPortRunErrorRecoveryAdvert)
                      .withErrorRecovery()
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    server.run(false);

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
    // it would for any other unimplemented feature (SilaServiceImpl.cc:56-58).
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

    server.shutdown();
}

TEST(SilaServerBaseRun, BlockTrueBlocksCallingThreadUntilShutdown) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(kPortRunBlocks)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    std::atomic<bool> runReturned{false};
    std::thread runner([&] {
        server.run(true);  // block=true: must not return before shutdown()
        runReturned = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    EXPECT_FALSE(runReturned.load()) << "run(true) returned before shutdown() was called";

    server.shutdown();
    runner.join();

    EXPECT_TRUE(runReturned.load()) << "run(true) never returned after shutdown()";
}

// ---------------------------------------------------------------------------
// SilaServerBase::Run — False (negative/rejection) paths
// All CAUGHT: BuildAndStart() returns nullptr for malformed TLS material,
// and run() surfaces that as std::runtime_error. Builder::build() already
// rejects empty PEM strings (see test_sila_server_base.cc), so the only way
// to reach this branch is non-empty-but-unusable PEM content.
// ---------------------------------------------------------------------------

TEST(SilaServerBaseRun, GarbagePemMaterialThrowsRuntimeError) {
    auto server = SilaServerBase::Builder()
                      .withCertificate("not-a-real-certificate-pem", "not-a-real-private-key-pem")
                      .withDiscovery(kPortRunGarbagePem)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    EXPECT_THROW(server.run(false), std::runtime_error);
}

TEST(SilaServerBaseRun, SwappedCertificateAndKeyThrowsRuntimeError) {
    // Real, individually-valid PEM material — but withCertificate is given
    // the key where the certificate belongs and vice versa.
    auto valid = SilaServerBase::Builder()
                     .withSelfSignedCertificate("localhost", "127.0.0.1")
                     .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                     .build();

    auto swapped = SilaServerBase::Builder()
                       .withCertificate(valid.privateKeyPem(), valid.certificatePem())
                       .withDiscovery(kPortRunSwappedPem)
                       .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                       .build();

    EXPECT_THROW(swapped.run(false), std::runtime_error);
}

TEST(SilaServerBaseRun, TruncatedCertificateThrowsRuntimeError) {
    auto valid = SilaServerBase::Builder()
                     .withSelfSignedCertificate("localhost", "127.0.0.1")
                     .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                     .build();
    const std::string truncatedCert = valid.certificatePem().substr(0, valid.certificatePem().size() / 2);

    auto server = SilaServerBase::Builder()
                      .withCertificate(truncatedCert, valid.privateKeyPem())
                      .withDiscovery(kPortRunTruncatedPem)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    EXPECT_THROW(server.run(false), std::runtime_error);
}

// ---------------------------------------------------------------------------
// mDNS publish-after-bind ordering (audit S31): build() must construct the
// publisher (SilaServiceImpl captures it for SetServerName) without
// advertising, and run() must advertise only after BuildAndStart has written
// the bound port back. No multicast browse test here: test_mdns_browser.cc's
// mdnsPortAvailable() skips on WSL2 (/proc/version names microsoft), so a
// browse-based assertion would never run on this host. Every case below
// reads publisher state directly instead.
// ---------------------------------------------------------------------------

TEST(SilaServerBaseRun, MdnsAdvertisesTheOsSelectedPortWhenBuiltWithPortZero) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(0)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    ASSERT_NE(server.mdnsPublisher(), nullptr);

    server.run(false);

    // Before the fix the publisher's port stayed 0 forever -- it was set at
    // construction time in build(), before BuildAndStart chose a real one.
    EXPECT_NE(server.port(), 0);
    EXPECT_EQ(server.mdnsPublisher()->port(), server.port());

    server.shutdown();
}

TEST(SilaServerBaseRun, MdnsAdvertisesTheRequestedPortWhenBuiltWithAFixedPort) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(kPortMdnsFixedPort)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    server.run(false);

    // Pins that the reordering did not change the fixed-port path, which
    // every other e2e test in this file relies on.
    EXPECT_EQ(server.port(), kPortMdnsFixedPort);
    EXPECT_EQ(server.mdnsPublisher()->port(), kPortMdnsFixedPort);

    server.shutdown();
}

TEST(SilaServerBaseRun, MdnsAdvertisedPortIsDialableAtTheMomentItIsAdvertised) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(0)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    server.run(false);

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

    server.shutdown();
}

TEST(SilaServerBaseRun, DoesNotPublishBeforeRun) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(0)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    ASSERT_NE(server.mdnsPublisher(), nullptr);

    // Direct regression guard: on the old code build() published, so this
    // would be true. Not a port check -- before run() the port reads 0
    // under both old and new code, so port alone cannot tell them apart.
    EXPECT_FALSE(server.mdnsPublisher()->isPublishing());
}

TEST(SilaServerBaseRun, FailedRunLeavesDiscoveryUnpublished) {
    auto server = SilaServerBase::Builder()
                      .withCertificate("not-a-real-certificate-pem", "not-a-real-private-key-pem")
                      .withDiscovery(kPortMdnsFailedRun)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    EXPECT_THROW(server.run(false), std::runtime_error);

    // A server that failed to bind must not be discoverable. setPort() is
    // never reached, so the port is still the one requested at build().
    EXPECT_FALSE(server.mdnsPublisher()->isPublishing());
    EXPECT_EQ(server.mdnsPublisher()->port(), kPortMdnsFailedRun);
}

// S53: Part B p75 MUST -- discovery is enabled by default and cannot be
// disabled, so a server that never calls withDiscovery() still gets a
// publisher and still starts. Formerly WithoutDiscoveryNoPublisherIsCreated
// AndRunStillStarts, which pinned the opposite (opt-in) behaviour.
TEST(SilaServerBaseRun, DiscoveryIsEnabledByDefaultWithoutWithDiscovery) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    EXPECT_NE(server.mdnsPublisher(), nullptr);
    EXPECT_NO_THROW(server.run(false));
    EXPECT_NO_THROW(server.shutdown());
}

// S53: the always-on default port, unexercised by the test above.
TEST(SilaServerBaseRun, WithoutWithDiscoveryAdvertisesTheDefaultPort) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    ASSERT_NE(server.mdnsPublisher(), nullptr);
    EXPECT_EQ(server.mdnsPublisher()->port(), 50051);
}

// ---------------------------------------------------------------------------
// Builder::withPersistentUuid (audit S30b): a caller-supplied file makes the
// server's ServerUUID survive a restart without a caller-supplied
// ServerConfig. build() alone is enough to exercise every case -- run()
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

TEST(SilaServerBaseBuilder, WithPersistentUuidCreatesFileAndUsesConformantUuid) {
    const auto path = uniquePersistentUuidPath("creates-file");
    std::filesystem::remove(path);

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withPersistentUuid(path)
                      .build();

    ASSERT_TRUE(std::filesystem::exists(path));
    std::ifstream in{path};
    std::string fileContent;
    std::getline(in, fileContent);
    EXPECT_EQ(fileContent, server.serverConfig().uuid());
    EXPECT_TRUE(std::regex_match(server.serverConfig().uuid(),
        std::regex{"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"}));

    std::filesystem::remove(path);
}

TEST(SilaServerBaseBuilder, WithPersistentUuidReusesExistingUuidAcrossBuilds) {
    const auto path = uniquePersistentUuidPath("reuses-across-builds");
    std::filesystem::remove(path);

    auto first = SilaServerBase::Builder()
                     .withSelfSignedCertificate("localhost", "127.0.0.1")
                     .withPersistentUuid(path)
                     .build();
    auto second = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withPersistentUuid(path)
                      .build();

    EXPECT_EQ(first.serverConfig().uuid(), second.serverConfig().uuid());

    std::filesystem::remove(path);
}

TEST(SilaServerBaseBuilder, WithPersistentUuidToleratesTrailingNewline) {
    const auto path = uniquePersistentUuidPath("trailing-newline");
    const std::string kUuid = "12345678-1234-1234-1234-123456789abc";
    {
        std::ofstream out{path};
        out << kUuid << "\n";
    }

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withPersistentUuid(path)
                      .build();

    EXPECT_EQ(server.serverConfig().uuid(), kUuid);

    std::filesystem::remove(path);
}

TEST(SilaServerBaseBuilder, WithPersistentUuidThrowsOnMalformedFile) {
    const auto path = uniquePersistentUuidPath("malformed");
    {
        std::ofstream out{path};
        out << "not-a-uuid";
    }

    EXPECT_THROW(SilaServerBase::Builder()
                     .withSelfSignedCertificate("localhost", "127.0.0.1")
                     .withPersistentUuid(path)
                     .build(),
                 std::runtime_error);

    // No silent overwrite: the malformed content must survive the rejection.
    std::ifstream in{path};
    std::string fileContent;
    std::getline(in, fileContent);
    EXPECT_EQ(fileContent, "not-a-uuid");

    std::filesystem::remove(path);
}

TEST(SilaServerBaseBuilder, WithPersistentUuidThrowsOnNonconformantUuid) {
    const auto path = uniquePersistentUuidPath("nonconformant");
    const std::string kUppercaseUuid = "12345678-1234-1234-1234-123456789ABC";
    {
        std::ofstream out{path};
        out << kUppercaseUuid;
    }

    EXPECT_THROW(SilaServerBase::Builder()
                     .withSelfSignedCertificate("localhost", "127.0.0.1")
                     .withPersistentUuid(path)
                     .build(),
                 std::runtime_error);

    std::ifstream in{path};
    std::string fileContent;
    std::getline(in, fileContent);
    EXPECT_EQ(fileContent, kUppercaseUuid);

    std::filesystem::remove(path);
}

TEST(SilaServerBaseBuilder, WithPersistentUuidConflictsWithWithConfig) {
    const auto path = uniquePersistentUuidPath("conflicts-with-config");

    EXPECT_THROW(SilaServerBase::Builder()
                     .withSelfSignedCertificate("localhost", "127.0.0.1")
                     .withConfig(std::make_unique<sila2::InMemoryServerConfig>("TestServer"))
                     .withPersistentUuid(path)
                     .build(),
                 std::logic_error);
}

// ---------------------------------------------------------------------------
// SilaServerBase::Shutdown — True (positive) paths
// ---------------------------------------------------------------------------

TEST(SilaServerBaseShutdown, InterruptsAllRegisteredCommandManagers) {
    ObservableCommandManager mgr;
    auto exec = mgr.addCommand();
    exec->start();
    ASSERT_FALSE(exec->isInterruptionRequested());

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(kPortShutdownInterrupts)
                      .registerCommandManager(&mgr)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    server.run(false);

    server.shutdown();

    EXPECT_TRUE(exec->isInterruptionRequested());
}

TEST(SilaServerBaseShutdown, StopsMdnsPublisherPromptlyWhenDiscoveryEnabled) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(kPortShutdownMdns)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    server.run(false);

    const auto t0 = std::chrono::steady_clock::now();
    server.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Regression guard against MdnsPublisher::shutdown() deadlocking the
    // listen thread join — mDNS's own probe/announce cycle runs in
    // milliseconds (ServerConfig's default mdnsProbeWait is 250ms), so a
    // multi-second bound leaves ample margin without masking a real hang.
    EXPECT_LT(elapsed, std::chrono::seconds{5});
}

TEST(SilaServerBaseShutdown, WithoutPriorRunDoesNotThrow) {
    // server_ is still null (run() never called); shutdown()'s `if (server_)`
    // guard must make this a no-op rather than a null-deref.
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    EXPECT_NO_THROW(server.shutdown());
}

// Pins audit 3.2m: a Subscribe_RecoverableErrors handler parks in
// Subscription::waitForNext(), on a condition variable gRPC's Shutdown()
// knows nothing about. The subscriber below is deliberately left idle — no
// error is ever published — because that is the exact state 3.2m describes;
// no deadline on grpc::Server::Shutdown() would unpark this handler, only
// cancelling the subscription (ObservablePropertyManager::shutdown()) does.
TEST(SilaServerBaseShutdown, ReturnsWithIdleRecoverableErrorsSubscriber) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(kPortShutdownIdleSubscriber)
                      .withErrorRecovery()
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    server.run(false);

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

    auto shutdownFuture = std::async(std::launch::async, [&server] { server.shutdown(); });
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
// on a worker thread instead of an explicit shutdown() call — SilaServerBase
// has a deleted move constructor and a private constructor, so it cannot be
// held in optional/unique_ptr to trigger destruction from the test thread
// directly. Pins the destructor path, which before this fix neither cancels
// nor joins before components_ dies.
TEST(SilaServerBaseShutdown, DestructorReturnsWithIdleRecoverableErrorsSubscriber) {
    std::promise<std::string> certPromise;
    std::promise<ObservablePropertyManager*> mgrPromise;
    std::promise<void> goPromise;

    auto serverFuture = std::async(std::launch::async, [&] {
        try {
            auto server = SilaServerBase::Builder()
                              .withSelfSignedCertificate("localhost", "127.0.0.1")
                              .withDiscovery(kPortDestructorIdleSubscriber)
                              .withErrorRecovery()
                              .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                              .build();
            server.run(false);
            certPromise.set_value(server.certificatePem());
            mgrPromise.set_value(server.errorRecoveryPropertyManager());
            goPromise.get_future().wait();
            // server destructs here, with no explicit shutdown() call.
        } catch (...) {
            // build()/run() failed: surface it through whichever promise
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
        // stuck inside shutdown() has not yet reached member destruction.
        propMgr->shutdown();
        serverFuture.wait();
    }
    subCtx.TryCancel();
    reader->Finish();
}

// ---------------------------------------------------------------------------
// SilaServerBase::Shutdown — False (negative/invariant-violation) paths
// shutdown() has no documented error path (architecture.md gives it none),
// so these exercise invariants shutdown() does not itself validate.
// ---------------------------------------------------------------------------

// CAUGHT (safely, by downstream idempotency): shutdown() has no re-entrancy
// guard of its own, but grpc::Server::Shutdown(), MdnsPublisher::shutdown(),
// and ObservableCommandManager::interruptAll() are each independently safe
// to call more than once, so a second call does not throw or crash.
TEST(SilaServerBaseShutdown, CalledTwiceDoesNotThrow) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(kPortShutdownTwice)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    server.run(false);
    server.shutdown();

    EXPECT_NO_THROW(server.shutdown());
}

// UNCAUGHT: interruptAll() -> requestInterruption() is an unconditional
// atomic flag set (ObservableCommandExecution.h) with no precondition on
// state, so shutdown() happily "interrupts" a command that was registered
// but never start()ed. The execution's state is left at Waiting — shutdown()
// does not force any transition.
TEST(SilaServerBaseShutdown, InterruptsCommandThatWasNeverStarted) {
    ObservableCommandManager mgr;
    auto exec = mgr.addCommand();
    ASSERT_EQ(exec->state(), State::Waiting);

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(kPortShutdownWaiting)
                      .registerCommandManager(&mgr)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    server.run(false);

    server.shutdown();

    EXPECT_TRUE(exec->isInterruptionRequested());
    EXPECT_EQ(exec->state(), State::Waiting);
}

// UNCAUGHT: same unconditional requestInterruption() also fires on a command
// that already finished before shutdown() ran — a meaningless but harmless
// no-op that nothing in the pipeline guards against.
TEST(SilaServerBaseShutdown, InterruptsCommandThatAlreadyFinished) {
    ObservableCommandManager mgr;
    auto exec = mgr.addCommand();
    exec->start();
    exec->finish();
    ASSERT_EQ(exec->state(), State::FinishedSuccessfully);

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withDiscovery(kPortShutdownFinished)
                      .registerCommandManager(&mgr)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    server.run(false);

    server.shutdown();

    EXPECT_TRUE(exec->isInterruptionRequested());
}
