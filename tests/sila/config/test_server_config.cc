// Checks InMemoryServerConfig: UUID generation/uniqueness, name mutation,
// and subscription queue depth defaults/overrides.
#include <sila/config/ServerConfig.h>

#include <gtest/gtest.h>

#include <regex>
#include <set>
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

TEST(InMemoryServerConfig, ExplicitUuid) {
    InMemoryServerConfig config{"my-uuid", "TestServer"};
    EXPECT_EQ(config.uuid(), "my-uuid");
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
    InMemoryServerConfig config{"TestServer", 32};
    EXPECT_EQ(config.subscriptionQueueDepth(), 32);
}

TEST(InMemoryServerConfig, UniqueUuids) {
    std::set<std::string> uuids;
    for (int i = 0; i < 100; ++i) {
        uuids.insert(InMemoryServerConfig{"TestServer"}.uuid());
    }
    EXPECT_EQ(uuids.size(), 100);
}
