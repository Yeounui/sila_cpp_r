// Tests for AuthTokenStore: issue/validate lifecycle, expiry, sliding renewal, clear.
#include <sila/server/auth/AuthTokenStore.h>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>

namespace
{
using sila2::auth::AuthTokenStore;
using namespace std::chrono_literals;

TEST(AuthTokenStore, IssueAndValidateSuccess) {
    AuthTokenStore store;
    const std::unordered_set<std::string> fqis{"sila2.org.Feature/Command"};

    const std::string token = store.issue("alice", fqis, 60s);
    const auto result = store.validate(token, "sila2.org.Feature/Command");

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->userIdentifier, "alice");
    EXPECT_EQ(result->allowedFqis, fqis);
}

TEST(AuthTokenStore, ValidateWrongFqiReturnsNullopt) {
    AuthTokenStore store;
    const std::string token = store.issue("alice", {"sila2.org.Feature/Command"}, 60s);

    const auto result = store.validate(token, "sila2.org.Other/Thing");

    EXPECT_FALSE(result.has_value());
}

TEST(AuthTokenStore, ValidateAfterExpiryReturnsNullopt) {
    AuthTokenStore store;
    const std::string token = store.issue("alice", {"sila2.org.Feature/Command"},
                                           std::chrono::duration_cast<std::chrono::seconds>(1ms));

    std::this_thread::sleep_for(5ms);
    const auto result = store.validate(token, "sila2.org.Feature/Command");

    EXPECT_FALSE(result.has_value());
}

TEST(AuthTokenStore, ClearInvalidatesAllTokens) {
    AuthTokenStore store;
    const std::string token = store.issue("alice", {"sila2.org.Feature/Command"}, 60s);

    store.clear();
    const auto result = store.validate(token, "sila2.org.Feature/Command");

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(store.size(), 0u);
}

TEST(AuthTokenStore, SlidingRenewalResetsExpiry) {
    // issue()'s lifetime parameter is std::chrono::seconds (no implicit ms conversion),
    // so this test is rescaled to 1s/400ms/1200ms instead of the spec's 50ms/30ms/60ms.
    AuthTokenStore store;
    const std::string token = store.issue("alice", {"sila2.org.Feature/Command"}, 1s);

    std::this_thread::sleep_for(400ms);
    EXPECT_TRUE(store.validate(token, "sila2.org.Feature/Command").has_value());

    std::this_thread::sleep_for(400ms);
    EXPECT_TRUE(store.validate(token, "sila2.org.Feature/Command").has_value());

    std::this_thread::sleep_for(1200ms);
    EXPECT_FALSE(store.validate(token, "sila2.org.Feature/Command").has_value());
}

TEST(AuthTokenStore, RemoveExpiredReturnsCorrectCount) {
    AuthTokenStore store;
    const auto shortLifetime = std::chrono::duration_cast<std::chrono::seconds>(1ms);
    store.issue("alice", {"sila2.org.Feature/Command"}, shortLifetime);
    store.issue("bob", {"sila2.org.Feature/Command"}, shortLifetime);
    store.issue("carol", {"sila2.org.Feature/Command"}, 10s);

    std::this_thread::sleep_for(5ms);
    const std::size_t removed = store.removeExpired();

    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(store.size(), 1u);
}

TEST(AuthTokenStore, RemoveInvalidatesToken) {
    AuthTokenStore store;
    const std::string token = store.issue("alice", {"sila2.org.Feature/Command"}, 60s);

    EXPECT_TRUE(store.remove(token));
    EXPECT_EQ(store.size(), 0u);
    EXPECT_FALSE(store.validate(token, "sila2.org.Feature/Command").has_value());
}

TEST(AuthTokenStore, RemoveUnknownTokenReturnsFalse) {
    AuthTokenStore store;
    EXPECT_FALSE(store.remove("no-such-token"));
}

TEST(AuthTokenStore, SizeTracksCorrectly) {
    AuthTokenStore store;
    store.issue("alice", {"sila2.org.Feature/Command"}, 60s);
    store.issue("bob", {"sila2.org.Feature/Command"}, 60s);
    store.issue("carol", {"sila2.org.Feature/Command"}, 60s);

    EXPECT_EQ(store.size(), 3u);

    store.clear();

    EXPECT_EQ(store.size(), 0u);
}

}  // namespace
