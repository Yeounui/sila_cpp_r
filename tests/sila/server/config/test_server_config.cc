// Checks InMemoryServerConfig: UUID generation/uniqueness, name mutation,
// and subscription queue depth defaults/overrides.
#include <sila/server/config/ServerConfig.h>

#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <stdexcept>
#include <string>

using sila2::InMemoryServerConfig;
using sila2::ServerConfig;

TEST(InMemoryServerConfig, AutoUuid) {
    InMemoryServerConfig config{"TestServer"};
    const std::regex uuidV4Pattern{
        "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};
    EXPECT_TRUE(std::regex_match(config.uuid(), uuidV4Pattern))
        << "UUID does not match v4 format: " << config.uuid();
}

TEST(InMemoryServerConfig, NameGetter) {
    InMemoryServerConfig config{"MyServer"};
    EXPECT_EQ(config.name(), "MyServer");
}

TEST(InMemoryServerConfig, SetName) {
    InMemoryServerConfig config{"Old"};
    config.setName("New");
    EXPECT_EQ(config.name(), "New");
}

TEST(InMemoryServerConfig, DefaultQueueDepth) {
    InMemoryServerConfig config{"TestServer"};
    EXPECT_EQ(config.subscriptionQueueDepth(), 16);
}

TEST(InMemoryServerConfig, ExplicitQueueDepth) {
    InMemoryServerConfig config{"TestServer", {}, {.subscriptionQueueDepth = 32}};
    EXPECT_EQ(config.subscriptionQueueDepth(), 32);
}

TEST(InMemoryServerConfig, UniqueUuids) {
    std::set<std::string> uuids;
    for (int i = 0; i < 100; ++i) {
        uuids.insert(InMemoryServerConfig{"TestServer"}.uuid());
    }
    EXPECT_EQ(uuids.size(), 100);
}

// ---------------------------------------------------------------------------
// S29 — Identity NSDMI defaults must be FDL-Pattern-conformant.
// ---------------------------------------------------------------------------

TEST(InMemoryServerConfig, DefaultIdentityServerTypeMatchesFdlPattern) {
    InMemoryServerConfig config{"TestServer"};
    EXPECT_EQ(config.serverType(), "SiLAServer");
    // SiLAService-v1_0.sila.xml:126.
    EXPECT_TRUE(std::regex_match(config.serverType(), std::regex{"^[A-Z][a-zA-Z0-9]*$"}));
}

TEST(InMemoryServerConfig, DefaultIdentityVersionMatchesFdlPattern) {
    InMemoryServerConfig config{"TestServer"};
    EXPECT_EQ(config.version(), "0.1.0");
    // SiLAService-v1_0.sila.xml:177.
    EXPECT_TRUE(std::regex_match(config.version(),
        std::regex{R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*))?(_[_a-zA-Z0-9]+)?$)"}));
}

TEST(InMemoryServerConfig, DefaultIdentityVendorUrlMatchesFdlPattern) {
    InMemoryServerConfig config{"TestServer"};
    // SiLAService-v1_0.sila.xml:197.
    EXPECT_TRUE(std::regex_match(config.vendorUrl(), std::regex{"^https?://.+$"}));
    EXPECT_FALSE(config.vendorUrl().empty());
}

TEST(InMemoryServerConfig, DefaultIdentityLeavesDescriptionEmpty) {
    InMemoryServerConfig config{"TestServer"};
    // ServerDescription carries no <Constrained> wrapper (SiLAService-v1_0.sila.xml:152-160),
    // so it must not get a placeholder default the way the other three do.
    EXPECT_TRUE(config.description().empty());
}

TEST(InMemoryServerConfig, ExplicitIdentityOverridesDefaults) {
    InMemoryServerConfig config{"TestServer",
        ServerConfig::Identity{"Shaker", "A shaker", "2.1", "http://vendor.example"}};
    EXPECT_EQ(config.serverType(), "Shaker");
    EXPECT_EQ(config.description(), "A shaker");
    EXPECT_EQ(config.version(), "2.1");
    // Bare "http://" is accepted: the FDL Pattern is https?, not https-only.
    EXPECT_EQ(config.vendorUrl(), "http://vendor.example");
}

// ---------------------------------------------------------------------------
// S37 — authorizationProviderUuid_ must default to the server's own uuid_.
// ---------------------------------------------------------------------------

TEST(InMemoryServerConfig, DefaultAuthorizationProviderIsOwnUuid) {
    InMemoryServerConfig config{"TestServer"};
    EXPECT_EQ(config.authorizationProviderUuid(), config.uuid());
}

TEST(InMemoryServerConfig, DefaultAuthorizationProviderMatchesFdlUuidConstraint) {
    InMemoryServerConfig config{"TestServer"};
    const std::string provider = config.authorizationProviderUuid();
    // AuthorizationConfigurationService-v1_0.sila.xml:20-21 -- Length 36 and
    // the lowercase-hex UUID Pattern, asserted separately to mirror the two
    // FDL <Constraints> children.
    EXPECT_EQ(provider.size(), 36u);
    EXPECT_TRUE(std::regex_match(provider,
        std::regex{"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"}));
}

TEST(InMemoryServerConfig, ExplicitUuidCtorDefaultsProviderToThatUuid) {
    const std::string kUuid = "12345678-1234-1234-1234-123456789abc";
    InMemoryServerConfig config{kUuid, "TestServer"};
    EXPECT_EQ(config.authorizationProviderUuid(), kUuid);
}

TEST(InMemoryServerConfig, DefaultAuthorizationProviderIsNeverEmpty) {
    InMemoryServerConfig config{"TestServer"};
    EXPECT_FALSE(config.authorizationProviderUuid().empty());
}

TEST(InMemoryServerConfig, SetAuthorizationProviderUuidOverridesDefault) {
    InMemoryServerConfig config{"TestServer"};
    const std::string provider = "11111111-2222-3333-4444-555555555555";
    config.setAuthorizationProviderUuid(provider);
    EXPECT_EQ(config.authorizationProviderUuid(), provider);
    EXPECT_NE(config.authorizationProviderUuid(), config.uuid());
}

// ---------------------------------------------------------------------------
// S30a — the explicit-UUID constructor must reject values that violate the
// SiLAService ServerUUID constraint (SiLAService-v1_0.sila.xml:144-147),
// since Get_ServerUUID returns the stored value verbatim.
// ---------------------------------------------------------------------------

TEST(InMemoryServerConfig, AutoUuidSatisfiesFdlServerUuidConstraint) {
    InMemoryServerConfig config{"TestServer"};
    EXPECT_EQ(config.uuid().size(), 36u);
    EXPECT_TRUE(std::regex_match(config.uuid(),
        std::regex{"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"}));
}

TEST(InMemoryServerConfig, ExplicitUuidAcceptsFdlConformantValue) {
    InMemoryServerConfig config{"12345678-1234-1234-1234-123456789abc", "TestServer"};
    EXPECT_EQ(config.uuid(), "12345678-1234-1234-1234-123456789abc");
}

TEST(InMemoryServerConfig, ExplicitUuidAcceptsNonV4ConformantValue) {
    // The FDL Pattern constrains shape only, not RFC 4122 version/variant
    // bits -- tightening to the v4 pattern (see AutoUuid above) would reject
    // a conformant peer's UUID.
    InMemoryServerConfig config{"00000000-0000-0000-0000-000000000000", "TestServer"};
    EXPECT_EQ(config.uuid(), "00000000-0000-0000-0000-000000000000");
}

TEST(InMemoryServerConfig, ExplicitUuidRejectsNonUuidString) {
    // The exact value audit S30 named as the defect: previously accepted and
    // advertised verbatim over Get_ServerUUID.
    EXPECT_THROW((InMemoryServerConfig{"my-uuid", "TestServer"}), std::invalid_argument);
}

TEST(InMemoryServerConfig, ExplicitUuidRejectsUppercaseHex) {
    // 36 characters -- only the Pattern check can reject this.
    EXPECT_THROW((InMemoryServerConfig{"12345678-1234-1234-1234-123456789ABC", "TestServer"}),
        std::invalid_argument);
}

TEST(InMemoryServerConfig, ExplicitUuidRejectsWrongLength) {
    // 35 characters, all lowercase hex -- rejected by both checks.
    EXPECT_THROW((InMemoryServerConfig{"12345678-1234-1234-1234-123456789ab", "TestServer"}),
        std::invalid_argument);
}

TEST(InMemoryServerConfig, ExplicitUuidRejectsEmptyString) {
    EXPECT_THROW((InMemoryServerConfig{"", "TestServer"}), std::invalid_argument);
}
