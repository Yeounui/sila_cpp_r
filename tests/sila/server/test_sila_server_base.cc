// Checks for SilaServerBase::Builder: Feature registration delegates to
// FeatureRegistry (including its duplicate-FQI throw), withSelfSignedCertificate
// produces PEM material while withCertificate stores caller-supplied PEM
// verbatim without parsing it, build() refuses to run without either, build()
// refuses to fabricate a volatile UUID when neither withConfig nor
// withPersistentUuid supplies an identity, and withAuthentication rejects
// null or asymmetric protected FQIs.
#include <sila/server/SilaServerBase.h>

#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <sila/server/config/TlsConfig.h>
#include <sila/server/features/ConnectionConfigurationServiceImpl.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>

// The dual-transport cloud harness (CloudRouterTestHarness.h) is what boots a
// real cloud stream for WithLockRegistersCloudHandlersForLockServer below --
// it lives with the cloud test suite, so it is reached with a relative path
// rather than duplicated here.
#include "transport/cloud/CloudRouterTestHarness.h"

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// FeatureRegistry (S46) now checks that an FDL's own root <Feature> identity
// spells the FQI it is registered under, so this fixture needs a matching
// four-segment FQI (org(.sub)*/Category/Name/vN) and Originator/Category/
// FeatureVersion/Identifier that derive to it.
const std::string kTestFeatureFqi = "org.example/test/TestFeature/v1";
const std::string kTestFeatureFdl =
    R"(<Feature Originator="org.example" Category="test" FeatureVersion="1.0">)"
    R"(<Identifier>TestFeature</Identifier></Feature>)";

// Minimal stubs: only withAuthentication's null-guard behavior is under
// test here, not verify()/isAllowed() decision logic.
struct StubVerifier : sila2::auth::CredentialVerifier {
    std::optional<std::string> verify(const std::string&, const std::string&) override {
        return "test";
    }
};

struct StubPolicy : sila2::auth::AccessPolicy {
    bool isAllowed(const std::string&, const std::string&) const override { return true; }
    std::vector<std::string> allowedFqis(const std::string&) const override { return {}; }
};

class ConfigWithOverriddenUuid final : public sila2::InMemoryServerConfig {
public:
    explicit ConfigWithOverriddenUuid(std::string uuid)
        : InMemoryServerConfig{"SiLA Server"}, uuid_{std::move(uuid)} {}

    std::string uuid() const override { return uuid_; }

private:
    std::string uuid_;
};

// gtest's ASSERT_* macros return out of the enclosing test function on
// failure, skipping any plain cleanup statements written after them -- this
// keeps every default-store test's temp files removed on both the pass and
// the assertion-failure path.
struct ScopedFileRemover {
    std::vector<std::filesystem::path> paths;
    ~ScopedFileRemover() {
        for (const auto& path : paths) std::filesystem::remove(path);
    }
};

// A self-signed certificate can serve as its own trust anchor when handed to
// withMutualTls -- the same substitution test_mtls_switch_e2e.cc's
// generateSelfSigned() relies on, since this codebase has no separate
// "sign this with a CA key" API (TlsConfig.h only offers self-signed leaves).
std::string generateCaCertPem() {
    const auto key = sila2::generateKey();
    const auto cert = sila2::generateCertificate(key, "SiLA2", "127.0.0.1");
    return sila2::certificateToPem(cert);
}

namespace connconfig_proto = sila2::org::silastandard::core::connectionconfigurationservice::v1;

}  // namespace

TEST(SilaServerBaseBuilder, RegistersFeaturesIntoTheAssembledRegistry)
{
    const auto server = sila2::SilaServerBase::Builder()
                             .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                             .addFeature(kTestFeatureFqi, kTestFeatureFdl)
                             .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                             .build();

    EXPECT_EQ(server.featureRegistry().featureDefinition(kTestFeatureFqi), kTestFeatureFdl);
}

TEST(SilaServerBaseBuilder, RejectsDuplicateFeatureRegistration)
{
    sila2::SilaServerBase::Builder builder;
    builder.addFeature(kTestFeatureFqi, kTestFeatureFdl);

    EXPECT_THROW(builder.addFeature(kTestFeatureFqi, kTestFeatureFdl),
                 std::invalid_argument);
}

TEST(SilaServerBaseBuilder, GeneratesASelfSignedCertificate)
{
    const auto server = sila2::SilaServerBase::Builder()
                             .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                             .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                             .build();

    // The certificate/key content itself is TlsConfig's responsibility and is
    // already covered by test_tls_config.cc; this only checks that the
    // Builder wired generation through to PEM output.
    EXPECT_EQ(server.certificatePem().rfind("-----BEGIN CERTIFICATE-----", 0), 0u);
    EXPECT_EQ(server.privateKeyPem().rfind("-----BEGIN PRIVATE KEY-----", 0), 0u);
}

TEST(SilaServerBaseBuilder, UsesSuppliedCertificateVerbatim)
{
    const auto server = sila2::SilaServerBase::Builder()
                             .withCertificate("fake-cert-pem", "fake-key-pem")
                             .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                             .build();

    EXPECT_EQ(server.certificatePem(), "fake-cert-pem");
    EXPECT_EQ(server.privateKeyPem(), "fake-key-pem");
}

TEST(SilaServerBaseBuilder, RequiresTlsBeforeBuild)
{
    sila2::SilaServerBase::Builder builder;

    EXPECT_THROW(builder.build(), std::logic_error);
}

TEST(SilaServerBaseBuilder, BuildWithoutIdentitySourceThrows)
{
    // TLS is satisfied, but neither withConfig nor withPersistentUuid supplies a
    // UUID. build() must refuse rather than mint a volatile one that changes on
    // the next boot (SiLAService-v1_0.sila.xml:134-137).
    sila2::SilaServerBase::Builder builder;
    builder.withSelfSignedCertificate("SiLA2", "127.0.0.1");

    EXPECT_THROW(builder.build(), std::logic_error);
}

// S29 end to end: an InMemoryServerConfig built from just a name takes the
// default Identity, which must already satisfy the three FDL Patterns, not
// just be a non-empty string.
// S66: Part B p77 SHOULD -- "By default this name SHOULD be equal to the SiLA
// Server Type". withPersistentUuid (not withConfig) is the path that lets the
// name default rather than being supplied explicitly, so it is used here to
// exercise the literal build() actually assigns.
TEST(SilaServerBaseBuilder, DefaultServerNameEqualsDefaultServerType)
{
    const auto path = std::filesystem::temp_directory_path() /
                       "sila2-test-persistent-uuid-default-name.txt";
    std::filesystem::remove(path);

    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withPersistentUuid(path)
                      .build();

    // The rejection half: this must no longer read "SiLA Server" (the old
    // literal), which also does not equal ServerType ("SiLAServer").
    EXPECT_EQ(server.serverConfig().name(), "SiLAServer");
    EXPECT_EQ(server.serverConfig().name(), server.serverConfig().serverType());

    std::filesystem::remove(path);
}

TEST(SilaServerBaseBuilder, DefaultIdentityIsFdlCompliant)
{
    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    const auto& cfg = server.serverConfig();

    EXPECT_TRUE(std::regex_match(cfg.serverType(), std::regex{"^[A-Z][a-zA-Z0-9]*$"}));
    EXPECT_TRUE(std::regex_match(cfg.version(),
        std::regex{R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*))?(_[_a-zA-Z0-9]+)?$)"}));
    EXPECT_TRUE(std::regex_match(cfg.vendorUrl(), std::regex{"^https?://.+$"}));
}

TEST(SilaServerBaseBuilder, BuildRejectsLowercaseServerType)
{
    sila2::SilaServerBase::Builder builder;
    builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
        .withConfig(std::make_unique<sila2::InMemoryServerConfig>("S",
            sila2::ServerConfig::Identity{"shaker", "", "0.1.0", "https://vendor.example"}));

    // Caught, not uncaught: build() is the declared rejection point for a
    // non-conformant Identity, per S29's chosen option (A+C).
    EXPECT_THROW(builder.build(), std::logic_error);
}

TEST(SilaServerBaseBuilder, BuildRejectsEmptyServerType)
{
    sila2::SilaServerBase::Builder builder;
    builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
        .withConfig(std::make_unique<sila2::InMemoryServerConfig>("S",
            sila2::ServerConfig::Identity{"", "", "0.1.0", "https://vendor.example"}));

    // Pins that the NSDMI default only applies to a defaulted field: an
    // explicitly emptied serverType is still refused, so the defect cannot
    // be re-introduced by a caller that overrides it with "".
    EXPECT_THROW(builder.build(), std::logic_error);
}

TEST(SilaServerBaseBuilder, BuildRejectsOverlongServerType)
{
    sila2::SilaServerBase::Builder builder;
    builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
        .withConfig(std::make_unique<sila2::InMemoryServerConfig>("S",
            sila2::ServerConfig::Identity{std::string(256, 'A'), "", "0.1.0",
                                           "https://vendor.example"}));

    EXPECT_THROW(builder.build(), std::logic_error);
}

TEST(SilaServerBaseBuilder, BuildRejectsNonconformantCustomConfigUuid)
{
    for (const std::string& uuid : {
             std::string{"not-a-uuid"},
             std::string{"AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"}}) {
        sila2::SilaServerBase::Builder builder;
        builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
            .withConfig(std::make_unique<ConfigWithOverriddenUuid>(uuid));

        EXPECT_THROW(builder.build(), std::logic_error);
    }
}

// Audit #6: ServerName's MaximalLength 255 (SiLAService-v1_0.sila.xml:107) was
// enforced only on the SetServerName command, so an initial name injected via
// withConfig bypassed it. The Identity is kept FDL-conformant here so the name
// length is the only variable under test.
const sila2::ServerConfig::Identity kValidIdentity{
    "Shaker", "", "0.1.0", "https://vendor.example"};

// Part A p32 (SHALL support the Server-Initiated Connection Method) and p80
// (SHALL implement the Feature): every SiLA Server declaring SiLA 2 Version
// >= "1.1" (MdnsPublisher kSilaVersion is unconditionally "1.1") registers
// this Feature, and server-initiated support is on by default even when
// withConnectionConfiguration was never called -- build() derives working
// defaults (see the Default* tests below).
TEST(SilaServerBaseBuilder, RegistersConnectionConfigurationServiceUnconditionally)
{
    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    EXPECT_FALSE(server.featureRegistry().featureDefinition(
        std::string{sila2::kConnectionConfigurationServiceFqi}).empty());
}

TEST(SilaServerBaseBuilder, RegistersConnectionConfigurationService)
{
    const auto storePath = std::filesystem::temp_directory_path() /
                           "sila-connection-configuration-builder-test.state";
    std::filesystem::remove(storePath);
    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withConnectionConfiguration(
                          storePath, grpc::InsecureChannelCredentials())
                      .build();

    EXPECT_FALSE(server.featureRegistry().featureDefinition(
        std::string{sila2::kConnectionConfigurationServiceFqi}).empty());
}

// Rejection half of withConnectionConfiguration: an explicit override still
// requires both arguments, even though build() now derives its own defaults
// when the call is skipped entirely (SilaServerBase.cc:593-604 unchanged).
TEST(SilaServerBaseBuilder, WithConnectionConfigurationRejectsEmptyPathAndNullCredentials)
{
    {
        sila2::SilaServerBase::Builder builder;
        EXPECT_THROW(builder.withConnectionConfiguration(
                         std::filesystem::path{}, grpc::InsecureChannelCredentials()),
                     std::invalid_argument);
    }
    {
        sila2::SilaServerBase::Builder builder;
        EXPECT_THROW(builder.withConnectionConfiguration(
                         std::filesystem::temp_directory_path() /
                             "sila-connection-configuration-rejects-null-creds.state",
                         nullptr),
                     std::invalid_argument);
    }
}

TEST(SilaServerBaseBuilder, BuildAcceptsMaximalLengthServerName)
{
    // Exactly 255 code points -- the boundary the constraint admits.
    const std::string name(255, 'a');
    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>(name, kValidIdentity))
                      .build();
    EXPECT_EQ(server.serverConfig().name(), name);
}

TEST(SilaServerBaseBuilder, BuildRejectsOverlongServerName)
{
    // One over the boundary. Rejected at build(), the initial name's only seam.
    sila2::SilaServerBase::Builder builder;
    builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
        .withConfig(std::make_unique<sila2::InMemoryServerConfig>(
            std::string(256, 'a'), kValidIdentity));

    EXPECT_THROW(builder.build(), std::logic_error);
}

TEST(SilaServerBaseBuilder, BuildRejectsMalformedServerVersion)
{
    {
        // Major with no minor -- SiLAService-v1_0.sila.xml:177 requires both.
        sila2::SilaServerBase::Builder builder;
        builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
            .withConfig(std::make_unique<sila2::InMemoryServerConfig>("S",
                sila2::ServerConfig::Identity{"Shaker", "", "1", "https://vendor.example"}));
        EXPECT_THROW(builder.build(), std::logic_error);
    }
    {
        // Leading zero: the FDL alternation (0|[1-9][0-9]*) rejects "01", a
        // naive \d+\.\d+ pattern would wrongly accept it.
        sila2::SilaServerBase::Builder builder;
        builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
            .withConfig(std::make_unique<sila2::InMemoryServerConfig>("S",
                sila2::ServerConfig::Identity{"Shaker", "", "01.0", "https://vendor.example"}));
        EXPECT_THROW(builder.build(), std::logic_error);
    }
}

TEST(SilaServerBaseBuilder, BuildRejectsNonHttpVendorUrl)
{
    {
        sila2::SilaServerBase::Builder builder;
        builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
            .withConfig(std::make_unique<sila2::InMemoryServerConfig>("S",
                sila2::ServerConfig::Identity{"Shaker", "", "0.1.0", "ftp://vendor.example"}));
        EXPECT_THROW(builder.build(), std::logic_error);
    }
    {
        // Scheme present but empty authority: the FDL's trailing .+ rejects it.
        sila2::SilaServerBase::Builder builder;
        builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
            .withConfig(std::make_unique<sila2::InMemoryServerConfig>("S",
                sila2::ServerConfig::Identity{"Shaker", "", "0.1.0", "https://"}));
        EXPECT_THROW(builder.build(), std::logic_error);
    }
}

TEST(SilaServerBaseBuilder, RejectsAuthenticationWithNullVerifier)
{
    sila2::SilaServerBase::Builder builder;

    EXPECT_THROW(builder.withAuthentication(nullptr, std::make_unique<StubPolicy>(), {}),
                 std::invalid_argument);
}

TEST(SilaServerBaseBuilder, RejectsAuthenticationWithNullPolicy)
{
    sila2::SilaServerBase::Builder builder;

    EXPECT_THROW(builder.withAuthentication(std::make_unique<StubVerifier>(), nullptr, {}),
                 std::invalid_argument);
}

TEST(SilaServerBaseBuilder, AcceptsCommandAndPropertyProtectedFqis)
{
    // The codegen granularity upgrade makes the gRPC path gate at Command/Property
    // granularity, symmetric with cloud, so a sub-feature entry now enforces
    // correctly instead of under-enforcing -- the builder accepts it.
    for (const auto& fqi : {"org.example/TestFeature/v1/Command/DoThing",
                            "org.example/TestFeature/v1/Property/Status"}) {
        sila2::SilaServerBase::Builder builder;
        EXPECT_NO_THROW(builder.withAuthentication(std::make_unique<StubVerifier>(),
                                                    std::make_unique<StubPolicy>(), {fqi}));
    }
}

TEST(SilaServerBaseBuilder, RejectsMetadataProtectedFqis)
{
    // A Metadata FQI gates only CreateBinary while chunk upload/delete fall back
    // to the coarser BinaryUpload FQI, so it would enforce only partially --
    // refused until the binary-metadata lifecycle is defined.
    sila2::SilaServerBase::Builder builder;
    EXPECT_THROW(builder.withAuthentication(
                     std::make_unique<StubVerifier>(), std::make_unique<StubPolicy>(),
                     {"org.example/TestFeature/v1/Metadata/AccessToken"}),
                 std::invalid_argument);
}

TEST(SilaServerBaseBuilder, AcceptsSymmetricProtectedFqis)
{
    for (const auto& fqi : {"org.example/TestFeature/v1", "org.example",
                            "org.example/TestFeature/v1/Command/Upload/Parameter/Payload"}) {
        sila2::SilaServerBase::Builder builder;
        EXPECT_NO_THROW(builder.withAuthentication(std::make_unique<StubVerifier>(),
                                                    std::make_unique<StubPolicy>(), {fqi}));
    }
}

// Part A permits Command and Property granularity in an affected list, so
// withMetadata accepts feature- and item-level entries alike.
TEST(SilaServerBaseBuilder, WithMetadataAcceptsFeatureAndItemLevelAffectedCalls)
{
    sila2::SilaServerBase::Builder builder;
    EXPECT_NO_THROW(builder.withMetadata(
        "org.test/Gate/v1/Metadata/Thing",
        {"org.test/Gate/v1", "org.test/Gate/v1/Command/A", "org.test/Gate/v1/Property/P"}));
}

TEST(SilaServerBaseBuilder, WithMetadataRejectsSiLAServiceAffectedCalls)
{
    {
        sila2::SilaServerBase::Builder builder;
        EXPECT_THROW(builder.withMetadata("org.test/Gate/v1/Metadata/Thing",
                                          {"org.silastandard/core/SiLAService/v1"}),
                     std::invalid_argument);
    }
    {
        // The Command-granularity form too, proving fqiCovers (not ==) is
        // behind the check.
        sila2::SilaServerBase::Builder builder;
        EXPECT_THROW(
            builder.withMetadata(
                "org.test/Gate/v1/Metadata/Thing",
                {"org.silastandard/core/SiLAService/v1/Command/SetServerName"}),
            std::invalid_argument);
    }
    {
        // A lookalike Feature FQI must NOT be caught by the same check.
        sila2::SilaServerBase::Builder builder;
        EXPECT_NO_THROW(builder.withMetadata("org.test/Gate/v1/Metadata/Thing",
                                             {"org.silastandard/core/SiLAServiceExtra/v1"}));
    }
}

// ---------------------------------------------------------------------------
// S32: Builder::withLock() -- opt-in per the owner ruling (Q1). Positive
// cases model themselves on the withMetadata cases above (builder.chain()
// inspected before build(), stable across the move -- SilaServerBase.h's
// doc comment on chain()); the cloud case needs a real gRPC round trip, so
// it alone uses the CloudRouterTestHarness fixture.
// ---------------------------------------------------------------------------

TEST(SilaServerBaseBuilder, WithLockRegistersTheLockControllerFeatureAndService)
{
    const std::string lockFqi = "org.silastandard/core/LockController/v1";
    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withLock()
                      .addFeature(kTestFeatureFqi, kTestFeatureFdl)
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    const auto ids = server.featureRegistry().registeredFeatureIdentifiers();
    EXPECT_NE(std::find(ids.begin(), ids.end(), lockFqi), ids.end());
    // Non-empty, not just present: pins that the FDL actually landed, not
    // merely the FQI (registerFeature and registerService are separate calls).
    EXPECT_FALSE(server.featureRegistry().featureDefinition(lockFqi).empty());
}

TEST(SilaServerBaseBuilder, WithLockDeclaresTheLockIdentifierMetadataAffectingOtherFeatures)
{
    sila2::SilaServerBase::Builder builder;
    builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
        .withLock()
        .addFeature(kTestFeatureFqi, kTestFeatureFdl);
    const auto* chain = builder.chain();
    builder.withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"));
    auto server = builder.build();

    const auto it = chain->metadataAffectedCalls.find(sila2::kLockIdentifierMetadataFqi);
    ASSERT_NE(it, chain->metadataAffectedCalls.end());
    EXPECT_NE(std::find(it->second.begin(), it->second.end(), kTestFeatureFqi),
              it->second.end());
    EXPECT_TRUE(static_cast<bool>(chain->lockGate));
}

class SilaServerBaseLockCloudRegistration : public cloud_test::CloudRouterFixture {};

TEST_F(SilaServerBaseLockCloudRegistration, WithLockRegistersCloudHandlersForLockServer)
{
    namespace cloud = sila2::org::silastandard;
    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withLock()
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-lock-cloud-1");
    msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(
        "org.silastandard/core/LockController/v1/Command/LockServer");
    server.cloudRouter()->route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    // The no-handler fallback would report a commanderror naming "no handler
    // registered"; LockServer succeeding with default parameters (empty
    // LockIdentifier, Timeout=0 -- the server is not yet locked) is proof the
    // CLOUD handler is wired, not merely the gRPC one this file's other tests
    // never exercise directly.
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
}

TEST_F(SilaServerBaseLockCloudRegistration,
       ConnectionConfigurationRegistersCloudHandlersAfterRouterConstruction)
{
    namespace cloud = sila2::org::silastandard;
    const auto storePath = std::filesystem::temp_directory_path() /
                           "sila-connection-configuration-cloud-test.state";
    std::filesystem::remove(storePath);
    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withConnectionConfiguration(
                          storePath, grpc::InsecureChannelCredentials())
                      .build();

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-connection-configuration-cloud-1");
    msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(
        "org.silastandard/core/ConnectionConfigurationService/v1/Command/"
        "EnableServerInitiatedConnectionMode");
    server.cloudRouter()->route(msg, *writer_, writer_, calls_);

    ASSERT_TRUE(popResponse().has_unobservablecommandresponse());
    std::filesystem::remove(storePath);
}

// Companion to the configured cloud test above: Part A p32 (SHALL support)
// means a server built without withConnectionConfiguration must still enable
// server-initiated mode, not merely advertise the Feature. Enable succeeds,
// the property reads back true, and build() has written the default store
// file next to the withPersistentUuid path (owner ruling 2026-09-04).
TEST_F(SilaServerBaseLockCloudRegistration,
       DefaultBuildEnablesServerInitiatedModeOverCloud)
{
    namespace cloud = sila2::org::silastandard;
    const auto uuidPath = std::filesystem::temp_directory_path() /
                           "sila-connection-configuration-default-uuid.txt";
    std::filesystem::remove(uuidPath);
    const auto connectionsPath =
        std::filesystem::path{uuidPath.string() + ".connections"};
    std::filesystem::remove(connectionsPath);
    ScopedFileRemover remover{{uuidPath, connectionsPath}};

    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withPersistentUuid(uuidPath)
                      .build();

    cloud::SiLAClientMessage enableMsg;
    enableMsg.set_requestuuid("req-connection-configuration-default-enable-1");
    enableMsg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(
        "org.silastandard/core/ConnectionConfigurationService/v1/Command/"
        "EnableServerInitiatedConnectionMode");
    server.cloudRouter()->route(enableMsg, *writer_, writer_, calls_);
    const auto enableResp = popResponse();
    ASSERT_TRUE(enableResp.has_unobservablecommandresponse());

    cloud::SiLAClientMessage statusMsg;
    statusMsg.set_requestuuid("req-connection-configuration-default-status-1");
    statusMsg.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(
        "org.silastandard/core/ConnectionConfigurationService/v1/Property/"
        "ServerInitiatedConnectionModeStatus");
    server.cloudRouter()->route(statusMsg, *writer_, writer_, calls_);
    const auto statusResp = popResponse();
    ASSERT_TRUE(statusResp.has_unobservablepropertyvalue());
    connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Responses statusValue;
    ASSERT_TRUE(statusValue.ParseFromString(statusResp.unobservablepropertyvalue().value()));
    EXPECT_TRUE(statusValue.serverinitiatedconnectionmodestatus().value());

    // Owner default (1): store = next to the withPersistentUuid file.
    EXPECT_TRUE(std::filesystem::exists(connectionsPath));
}

// Without a withPersistentUuid path, build() falls back to the temp
// directory keyed by the server's own UUID (owner default (1), second half).
TEST_F(SilaServerBaseLockCloudRegistration,
       DefaultStoreFallsBackToTempDirWithoutPersistentUuid)
{
    namespace cloud = sila2::org::silastandard;
    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .build();
    const auto connectionsPath = std::filesystem::temp_directory_path() /
        ("sila2-connections-" + server.serverConfig().uuid());
    std::filesystem::remove(connectionsPath);
    ScopedFileRemover remover{{connectionsPath}};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-connection-configuration-default-tempdir-1");
    msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(
        "org.silastandard/core/ConnectionConfigurationService/v1/Command/"
        "EnableServerInitiatedConnectionMode");
    server.cloudRouter()->route(msg, *writer_, writer_, calls_);
    ASSERT_TRUE(popResponse().has_unobservablecommandresponse());

    EXPECT_TRUE(std::filesystem::exists(connectionsPath));
}

// Owner default (2): outbound credentials present the server's own identity.
// A real outbound stream is out of scope here (that needs a live
// CloudClientListener, see notes_for_reviewer on the run/shutdown e2e file);
// the observable part of credential construction is that build() and Enable
// both succeed, with and without withMutualTls supplying a peer CA.
TEST_F(SilaServerBaseLockCloudRegistration,
       DefaultOutboundCredentialsPresentServerIdentity)
{
    namespace cloud = sila2::org::silastandard;
    const auto enableOverCloud = [this](sila2::SilaServerBase& server, const char* requestUuid) {
        cloud::SiLAClientMessage msg;
        msg.set_requestuuid(requestUuid);
        msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(
            "org.silastandard/core/ConnectionConfigurationService/v1/Command/"
            "EnableServerInitiatedConnectionMode");
        server.cloudRouter()->route(msg, *writer_, writer_, calls_);
        return popResponse().has_unobservablecommandresponse();
    };

    // No withMutualTls -- defaultOutboundCredentials() accepts an untrusted
    // peer certificate for private-range targets only (Part B p75); see
    // DefaultOutboundCredentialsFollowPartBPrivateIpRule for the policy.
    {
        const auto uuidPath = std::filesystem::temp_directory_path() /
                               "sila-connection-configuration-default-creds-notls.txt";
        std::filesystem::remove(uuidPath);
        const auto connectionsPath =
            std::filesystem::path{uuidPath.string() + ".connections"};
        std::filesystem::remove(connectionsPath);
        ScopedFileRemover remover{{uuidPath, connectionsPath}};

        auto server = sila2::SilaServerBase::Builder()
                          .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                          .withPersistentUuid(uuidPath)
                          .build();
        EXPECT_TRUE(enableOverCloud(server, "req-default-creds-notls-1"));
    }
    // withMutualTls set -- defaultOutboundCredentials() takes the
    // grpc::SslCredentials branch, verifying the peer against caCertPem_.
    {
        const auto uuidPath = std::filesystem::temp_directory_path() /
                               "sila-connection-configuration-default-creds-mtls.txt";
        std::filesystem::remove(uuidPath);
        const auto connectionsPath =
            std::filesystem::path{uuidPath.string() + ".connections"};
        std::filesystem::remove(connectionsPath);
        ScopedFileRemover remover{{uuidPath, connectionsPath}};

        auto server = sila2::SilaServerBase::Builder()
                          .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                          .withMutualTls(generateCaCertPem())
                          .withPersistentUuid(uuidPath)
                          .build();
        EXPECT_TRUE(enableOverCloud(server, "req-default-creds-mtls-1"));
    }
}

// Part B p74/p75 on the outbound leg (Codex review of SC32, 2026-09-04): the
// default provider hands out credentials for a private-range target without a
// CA, for any target with a CA, and refuses everything else so
// ConnectSiLAClient cannot make the server dial an arbitrary endpoint.
TEST(SilaServerBaseBuilder, DefaultOutboundCredentialsFollowPartBPrivateIpRule)
{
    const auto key = sila2::generateKey();
    const auto cert = sila2::certificateToPem(sila2::generateCertificate(key, "SiLA2", "127.0.0.1"));
    const auto keyPem = sila2::keyToPem(key);

    const auto zeroConfig = sila2::SilaServerBase::Builder::defaultOutboundCredentials(cert, keyPem, "");
    EXPECT_NE(zeroConfig("10.0.0.1"), nullptr);
    EXPECT_NE(zeroConfig("[fd00::1]"), nullptr);
    EXPECT_EQ(zeroConfig("203.0.113.5"), nullptr);
    EXPECT_EQ(zeroConfig("127.0.0.1"), nullptr);   // loopback is not private (S69)
    EXPECT_EQ(zeroConfig("client.local."), nullptr);

    const auto trusted = sila2::SilaServerBase::Builder::defaultOutboundCredentials(cert, keyPem, cert);
    EXPECT_NE(trusted("203.0.113.5"), nullptr);
    EXPECT_NE(trusted("client.local."), nullptr);
}

// End to end over the cloud router: a default server refuses ConnectSiLAClient
// to a public host with InvalidSiLAClient and persists nothing.
TEST_F(SilaServerBaseLockCloudRegistration, DefaultServerRefusesConnectToPublicHost)
{
    namespace cloud = sila2::org::silastandard;
    const auto uuidPath = std::filesystem::temp_directory_path() /
                           "sila-connection-configuration-default-public-host.txt";
    const auto connectionsPath = std::filesystem::path{uuidPath.string() + ".connections"};
    std::filesystem::remove(uuidPath);
    std::filesystem::remove(connectionsPath);
    ScopedFileRemover remover{{uuidPath, connectionsPath}};

    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("SiLA2", "127.0.0.1")
                      .withPersistentUuid(uuidPath)
                      .build();

    connconfig_proto::ConnectSiLAClient_Parameters params;
    params.mutable_clientname()->set_value("pub");
    params.mutable_silaclienthost()->set_value("203.0.113.5");
    params.mutable_silaclientport()->set_value(50052);
    params.mutable_persist()->set_value(true);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-default-public-host-1");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid(
        "org.silastandard/core/ConnectionConfigurationService/v1/Command/ConnectSiLAClient");
    exec->mutable_commandparameter()->set_parameters(params.SerializeAsString());
    server.cloudRouter()->route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_definedexecutionerror());
    EXPECT_NE(resp.commanderror().definedexecutionerror().erroridentifier().find("InvalidSiLAClient"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(connectionsPath));
}

TEST(SilaServerBaseBuilder, DefaultBuildDoesNotRegisterLockController)
{
    sila2::SilaServerBase::Builder builder;
    builder.withSelfSignedCertificate("SiLA2", "127.0.0.1");
    const auto* chain = builder.chain();
    builder.withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"));
    auto server = builder.build();

    const std::string lockFqi = "org.silastandard/core/LockController/v1";
    const auto ids = server.featureRegistry().registeredFeatureIdentifiers();
    EXPECT_EQ(std::find(ids.begin(), ids.end(), lockFqi), ids.end());
    EXPECT_EQ(chain->metadataAffectedCalls.find(sila2::kLockIdentifierMetadataFqi),
              chain->metadataAffectedCalls.end());
    EXPECT_FALSE(static_cast<bool>(chain->lockGate));
}

TEST(SilaServerBaseBuilder, LockAffectedCallsExcludeSiLAServiceAndLockControllerItself)
{
    sila2::SilaServerBase::Builder builder;
    builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
        .withLock()
        .withAuthentication(std::make_unique<StubVerifier>(), std::make_unique<StubPolicy>(), {});
    const auto* chain = builder.chain();
    builder.withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"));
    auto server = builder.build();

    const auto it = chain->metadataAffectedCalls.find(sila2::kLockIdentifierMetadataFqi);
    ASSERT_NE(it, chain->metadataAffectedCalls.end());
    const auto& affected = it->second;
    EXPECT_EQ(std::find(affected.begin(), affected.end(),
                        "org.silastandard/core/SiLAService/v1"), affected.end());
    EXPECT_EQ(std::find(affected.begin(), affected.end(),
                        "org.silastandard/core/LockController/v1"), affected.end());
    // Pins that the exclusion list is exactly two, not a broader carve-out
    // someone added later -- a built-in Feature that is neither of the two
    // excluded ones must still be covered.
    EXPECT_NE(std::find(affected.begin(), affected.end(),
                        "org.silastandard/core/AuthenticationService/v1"), affected.end());
}

TEST(SilaServerBaseBuilder, LockAffectedCallsExcludeBinaryTransferServices)
{
    sila2::SilaServerBase::Builder builder;
    builder.withSelfSignedCertificate("SiLA2", "127.0.0.1")
        .withLock()
        .withBinaryTransfer();
    const auto* chain = builder.chain();
    builder.withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"));
    auto server = builder.build();

    // Documents the deliberate consequence of FeatureRegistry.cc's "Allow
    // transport-level services ... without a Feature definition": BinaryUpload
    // and BinaryDownload are registerService'd, not registerFeature'd, so they
    // never enter registeredFeatureFqis and this exclusion needs no extra code
    // -- pinned here so a future 'fix' does not add them while S26 is open.
    const auto it = chain->metadataAffectedCalls.find(sila2::kLockIdentifierMetadataFqi);
    ASSERT_NE(it, chain->metadataAffectedCalls.end());
    const auto& affected = it->second;
    EXPECT_EQ(std::find(affected.begin(), affected.end(),
                        "org.silastandard/core/BinaryUpload/v1"), affected.end());
    EXPECT_EQ(std::find(affected.begin(), affected.end(),
                        "org.silastandard/core/BinaryDownload/v1"), affected.end());
}
