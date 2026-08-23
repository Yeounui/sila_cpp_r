// Checks for FeatureRegistry: registration/lookup round-trip, the two
// documented throw paths (duplicate FQI, unknown FQI), and that
// registeredFeatureIdentifiers() returns FQIs in sorted order regardless of
// registration order.
#include <sila/server/FeatureRegistry.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

TEST(FeatureRegistry, RetrievesRegisteredFeatureDefinitionByFqi)
{
    sila2::FeatureRegistry registry;
    registry.registerFeature("org.silastandard/core/SiLAService/v1", "<Feature/>");

    EXPECT_EQ(registry.featureDefinition("org.silastandard/core/SiLAService/v1"), "<Feature/>");
}

TEST(FeatureRegistry, ThrowsOnDuplicateFqi)
{
    sila2::FeatureRegistry registry;
    registry.registerFeature("org.silastandard/core/SiLAService/v1", "<Feature/>");

    EXPECT_THROW(registry.registerFeature("org.silastandard/core/SiLAService/v1", "<Feature/>"),
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
    registry.registerFeature("org.silastandard/core/SiLAService/v1", "<Feature/>");
    registry.registerFeature("org.silastandard/core/LockController/v1", "<Feature/>");
    registry.registerFeature("org.silastandard/core/ErrorRecoveryService/v1", "<Feature/>");

    const std::vector<std::string> expected{
        "org.silastandard/core/ErrorRecoveryService/v1",
        "org.silastandard/core/LockController/v1",
        "org.silastandard/core/SiLAService/v1",
    };
    EXPECT_EQ(registry.registeredFeatureIdentifiers(), expected);
}
