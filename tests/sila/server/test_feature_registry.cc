// Checks for FeatureRegistry: registration/lookup round-trip, the two
// documented throw paths (duplicate FQI, unknown FQI), that
// registeredFeatureIdentifiers() returns FQIs in sorted order regardless of
// registration order, and (S46) that registerFeature rejects an FDL whose own
// root <Feature> identity disagrees with the FQI it is registered under.
#include <sila/server/FeatureRegistry.h>

#include <sila/server/SiLAServiceImpl.h>
#include <sila/server/features/AuthenticationServiceImpl.h>
#include <sila/server/features/AuthorizationConfigurationServiceImpl.h>
#include <sila/server/features/AuthorizationServiceImpl.h>
#include <sila/server/features/ConnectionConfigurationServiceImpl.h>
#include <sila/server/features/LockControllerImpl.h>
#include <sila/server/features/SimulationControllerImpl.h>
#include <sila/server/recovery/ErrorRecoveryServiceImpl.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// FeatureRegistry now derives a Feature's identity from its FDL, so a fixture
// needs a root <Feature> whose attributes and <Identifier> spell the FQI it is
// registered under.
std::string fdlFor(std::string_view originator, std::string_view category,
                   std::string_view identifier, std::string_view featureVersion) {
    return std::string{"<Feature Originator=\""} + std::string{originator} + "\" Category=\"" +
           std::string{category} + "\" FeatureVersion=\"" + std::string{featureVersion} +
           "\"><Identifier>" + std::string{identifier} + "</Identifier></Feature>";
}

}  // namespace

TEST(FeatureRegistry, RetrievesRegisteredFeatureDefinitionByFqi)
{
    sila2::FeatureRegistry registry;
    const std::string fdl = fdlFor("org.silastandard", "core", "SiLAService", "1.0");
    registry.registerFeature("org.silastandard/core/SiLAService/v1", fdl);

    EXPECT_EQ(registry.featureDefinition("org.silastandard/core/SiLAService/v1"), fdl);
}

TEST(FeatureRegistry, ThrowsOnDuplicateFqi)
{
    sila2::FeatureRegistry registry;
    registry.registerFeature("org.silastandard/core/SiLAService/v1",
                             fdlFor("org.silastandard", "core", "SiLAService", "1.0"));

    EXPECT_THROW(registry.registerFeature("org.silastandard/core/SiLAService/v1",
                                          fdlFor("org.silastandard", "core", "SiLAService", "1.0")),
                 std::invalid_argument);
}

TEST(FeatureRegistry, ThrowsOnUnregisteredFqi)
{
    sila2::FeatureRegistry registry;

    EXPECT_THROW(registry.featureDefinition("org.silastandard/core/SiLAService/v1"),
                 std::out_of_range);
}

TEST(FeatureRegistry, ListsIdentifiersInFqiSortedOrder)
{
    sila2::FeatureRegistry registry;
    registry.registerFeature("org.silastandard/core/SiLAService/v1",
                             fdlFor("org.silastandard", "core", "SiLAService", "1.0"));
    registry.registerFeature("org.silastandard/core/LockController/v1",
                             fdlFor("org.silastandard", "core", "LockController", "1.0"));
    registry.registerFeature("org.silastandard/core/ErrorRecoveryService/v1",
                             fdlFor("org.silastandard", "core", "ErrorRecoveryService", "1.0"));

    const std::vector<std::string> expected{
        "org.silastandard/core/ErrorRecoveryService/v1",
        "org.silastandard/core/LockController/v1",
        "org.silastandard/core/SiLAService/v1",
    };
    EXPECT_EQ(registry.registeredFeatureIdentifiers(), expected);
}

TEST(FeatureRegistry, RegisterServiceRoundTripsThroughRegisteredServices)
{
    sila2::FeatureRegistry registry;
    registry.registerFeature("org.silastandard/core/SiLAService/v1",
                             fdlFor("org.silastandard", "core", "SiLAService", "1.0"));
    auto service = std::make_shared<grpc::Service>();

    registry.registerService("org.silastandard/core/SiLAService/v1", service);

    const auto services = registry.registeredServices();
    ASSERT_EQ(services.size(), 1u);
    EXPECT_EQ(services[0], service.get());
}

TEST(FeatureRegistry, RegisterServiceForUnregisteredFqiIsAllowed)
{
    // Transport-level services (BinaryTransfer, the cloud connector) are real
    // gRPC services with no Feature definition behind them, so registerService
    // deliberately stopped requiring registerFeature first (ecbeb8c). This
    // test asserted the old throw and had been red ever since.
    sila2::FeatureRegistry registry;
    auto service = std::make_shared<grpc::Service>();

    registry.registerService("org.silastandard/core/SiLAService/v1", service);

    const auto services = registry.registeredServices();
    ASSERT_EQ(services.size(), 1u);
    EXPECT_EQ(services[0], service.get());
}

// ---------------------------------------------------------------------------
// S46: registerFeature checks the FQI against the identity the FDL itself
// declares (root <Feature> Originator/Category/FeatureVersion + Identifier).
// ---------------------------------------------------------------------------

// Server-side twin of test_fdl_namespace_and_identity.cc's
// ServerEmbeddedFdlIdentitiesMatchTheirFqiConstants (the CLIENT half, which
// checks the same eight pairs through the full libxml2 FdlRuntimeParser) --
// together they prove the lightweight extractor here agrees with the full
// parser on every real FDL in the tree.
TEST(FeatureRegistry, ServerEmbeddedFdlIdentitiesRegisterUnderTheirFqiConstants)
{
    // Function-local so the fdlXml references bind at test run time: at
    // namespace scope they would bind during this TU's dynamic init to
    // kFdlXml strings whose cross-TU construction order is unspecified.
    struct ServerFeature {
        std::string_view name;
        std::string_view fqi;
        const std::string& fdlXml;
    };
    const ServerFeature kServerFeatures[] = {
        {"SiLAService", sila2::kSiLAServiceFqi, sila2::silaServiceFdlXml()},
        {"AuthenticationService", sila2::kAuthenticationServiceFqi, sila2::authenticationServiceFdlXml()},
        {"AuthorizationService", sila2::kAuthorizationServiceFqi, sila2::authorizationServiceFdlXml()},
        {"AuthorizationConfigurationService", sila2::kAuthorizationConfigurationServiceFqi,
         sila2::authorizationConfigurationServiceFdlXml()},
        {"ErrorRecoveryService", sila2::kErrorRecoveryServiceFqi, sila2::errorRecoveryServiceFdlXml()},
        {"LockController", sila2::kLockControllerFqi, sila2::lockControllerFdlXml()},
        {"SimulationController", sila2::kSimulationControllerFqi, sila2::simulationControllerFdlXml()},
        {"ConnectionConfigurationService", sila2::kConnectionConfigurationServiceFqi,
         sila2::connectionConfigurationServiceFdlXml()},
    };
    for (const auto& feature : kServerFeatures) {
        SCOPED_TRACE(feature.name);
        sila2::FeatureRegistry registry;
        EXPECT_NO_THROW(registry.registerFeature(std::string{feature.fqi}, feature.fdlXml));
    }
}

TEST(FeatureRegistry, ThrowsWhenTheFdlIdentityDoesNotMatchTheFqi)
{
    {
        // Wrong Identifier.
        sila2::FeatureRegistry registry;
        EXPECT_THROW(registry.registerFeature("org.example/test/Thing/v1",
                                              fdlFor("org.example", "test", "Other", "1.0")),
                     std::invalid_argument);
        EXPECT_TRUE(registry.registeredFeatureIdentifiers().empty());
    }
    {
        // Wrong Originator.
        sila2::FeatureRegistry registry;
        EXPECT_THROW(registry.registerFeature("org.example/test/Thing/v1",
                                              fdlFor("org.other", "test", "Thing", "1.0")),
                     std::invalid_argument);
    }
    {
        // Wrong Category.
        sila2::FeatureRegistry registry;
        EXPECT_THROW(registry.registerFeature("org.example/test/Thing/v1",
                                              fdlFor("org.example", "core", "Thing", "1.0")),
                     std::invalid_argument);
    }
    {
        // Wrong major version.
        sila2::FeatureRegistry registry;
        EXPECT_THROW(registry.registerFeature("org.example/test/Thing/v1",
                                              fdlFor("org.example", "test", "Thing", "2.0")),
                     std::invalid_argument);
    }
}

TEST(FeatureRegistry, ThrowsWhenTheFdlCarriesNoFeatureIdentity)
{
    sila2::FeatureRegistry registry;
    EXPECT_THROW(registry.registerFeature("org.example/test/Thing/v1", "<Feature/>"),
                 std::invalid_argument);
}

// ---------------------------------------------------------------------------
// S60: Part A p87 -- FQI comparison (and thus uniqueness/lookup) MUST ignore
// case. definitions_ uses util::CaseInsensitiveLess (FeatureRegistry.h) so a
// case-variant FQI still resolves, while the stored key -- and therefore what
// registeredFeatureIdentifiers()/ListImplementedFeatures advertises -- stays
// canonical case.
// ---------------------------------------------------------------------------

TEST(FeatureRegistry, FeatureDefinitionLookupIgnoresFqiCase)
{
    sila2::FeatureRegistry registry;
    const std::string fdl = fdlFor("org.silastandard", "core", "SiLAService", "1.0");
    registry.registerFeature("org.silastandard/core/SiLAService/v1", fdl);

    EXPECT_EQ(registry.featureDefinition("ORG.SILASTANDARD/CORE/SILASERVICE/V1"), fdl);
}

TEST(FeatureRegistry, RejectsCaseVariantDuplicateRegistration)
{
    sila2::FeatureRegistry registry;
    registry.registerFeature("org.example/test/Thing/v1",
                             fdlFor("org.example", "test", "Thing", "1.0"));

    // The second FDL derives its own identity ("org.example/test/thing/v1",
    // passing registerFeature's derived==fqi check on its own terms) then
    // collides case-insensitively with the already-stored "Thing" entry --
    // proving uniqueness is checked without regard to case (Part A p87), not
    // just lookup.
    EXPECT_THROW(registry.registerFeature("org.example/test/thing/v1",
                                          fdlFor("org.example", "test", "thing", "1.0")),
                 std::invalid_argument);
}
