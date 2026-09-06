// Unit tests for ExecutionStore (client/ExecutionStore.h): round-tripping
// PersistedExecution rows through a tab-delimited file across a simulated
// client restart (Part A p33, owner directive 2026-09-03).
#include <sila/client/ExecutionStore.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
using sila2::ExecutionStore;
using sila2::PersistedExecution;

// Unique per-test store path under gtest's temp dir, mirroring
// tests/sila/server/features/test_connection_configuration_persistence.cc's
// tempStorePath() so parallel/repeated runs never collide.
std::filesystem::path tempStorePath(const std::string& testName) {
    return std::filesystem::path{::testing::TempDir()} /
        ("sila_execution_store_" + testName + "_" + std::to_string(::getpid()) + ".tsv");
}

bool containsUuid(const std::vector<PersistedExecution>& executions, const std::string& uuid) {
    return std::any_of(executions.begin(), executions.end(),
        [&uuid](const PersistedExecution& e) { return e.executionUuid == uuid; });
}

// --- True (positive) paths --------------------------------------------------

// Two SilaClientBase instances (one per registered server) share one store
// path; the file, not either instance's memory, is the source of truth.
TEST(ExecutionStore, TwoInstancesOnOneFileDoNotClobberEachOther) {
    const auto storePath = tempStorePath("TwoInstances");
    std::filesystem::remove(storePath);

    ExecutionStore a{storePath};
    ExecutionStore b{storePath};
    a.record(PersistedExecution{"srv-a", "org.example/test/Feature/v1", "DoWork", "uuid-a", "1", "running"});
    b.record(PersistedExecution{"srv-b", "org.example/test/Feature/v1", "DoWork", "uuid-b", "2", "running"});

    EXPECT_TRUE(containsUuid(a.list(), "uuid-b"));
    EXPECT_TRUE(containsUuid(b.list(), "uuid-a"));
    a.prune("uuid-a");
    const auto remaining = b.list();
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining.front().executionUuid, "uuid-b");

    std::filesystem::remove(storePath);
}

TEST(ExecutionStore, RecordRejectsTabOrNewlineInFields) {
    const auto storePath = tempStorePath("RejectDelimiters");
    std::filesystem::remove(storePath);

    ExecutionStore store{storePath};
    EXPECT_THROW(store.record(PersistedExecution{
        "srv\t1", "org.example/test/Feature/v1", "DoWork", "uuid-1", "1", "running"}),
        std::invalid_argument);
    EXPECT_THROW(store.record(PersistedExecution{
        "srv-1", "org.example/test/Feature/v1", "Do\nWork", "uuid-1", "1", "running"}),
        std::invalid_argument);
    EXPECT_TRUE(store.list().empty());

    std::filesystem::remove(storePath);
}

TEST(ExecutionStore, RoundTripAcrossReopen) {
    const auto storePath = tempStorePath("RoundTrip");
    std::filesystem::remove(storePath);

    {
        ExecutionStore store{storePath};
        store.record(PersistedExecution{
            "srv-1", "org.example/test/Feature/v1", "DoWork", "uuid-1", "1000", "waiting"});
        store.record(PersistedExecution{
            "srv-1", "org.example/test/Feature/v1", "DoOther", "uuid-2", "1001", "running"});
    }  // simulated client shutdown

    ExecutionStore reopened{storePath};  // simulated restart
    const auto executions = reopened.list();
    ASSERT_EQ(executions.size(), 2u);
    ASSERT_TRUE(containsUuid(executions, "uuid-1"));
    ASSERT_TRUE(containsUuid(executions, "uuid-2"));
    const auto& first = *std::find_if(executions.begin(), executions.end(),
        [](const PersistedExecution& e) { return e.executionUuid == "uuid-1"; });
    EXPECT_EQ(first.serverUuid, "srv-1");
    EXPECT_EQ(first.featureFqi, "org.example/test/Feature/v1");
    EXPECT_EQ(first.commandId, "DoWork");
    EXPECT_EQ(first.issueTime, "1000");
    EXPECT_EQ(first.lastStatus, "waiting");

    std::filesystem::remove(storePath);
}

TEST(ExecutionStore, UpsertReplacesSameUuid) {
    const auto storePath = tempStorePath("Upsert");
    std::filesystem::remove(storePath);

    ExecutionStore store{storePath};
    store.record(PersistedExecution{"srv-1", "fqi", "Cmd", "aaa", "1000", "waiting"});
    // Case variant of the same UUID (Part A p90: UUIDs compared without case)
    // must update the existing row, not add a second one.
    store.record(PersistedExecution{"srv-1", "fqi", "Cmd", "AAA", "1001", "running"});

    const auto executions = store.list();
    ASSERT_EQ(executions.size(), 1u);
    EXPECT_EQ(executions.front().lastStatus, "running");

    std::filesystem::remove(storePath);
}

TEST(ExecutionStore, ListBySeverFilters) {
    const auto storePath = tempStorePath("ListByServer");
    std::filesystem::remove(storePath);

    ExecutionStore store{storePath};
    store.record(PersistedExecution{"s1", "fqi", "Cmd", "uuid-a", "1000", "waiting"});
    store.record(PersistedExecution{"s1", "fqi", "Cmd", "uuid-b", "1001", "waiting"});
    store.record(PersistedExecution{"s2", "fqi", "Cmd", "uuid-c", "1002", "waiting"});

    EXPECT_EQ(store.list("s1").size(), 2u);
    EXPECT_EQ(store.list("S1").size(), 2u);  // case-insensitive per Part A p90
    EXPECT_EQ(store.list("s2").size(), 1u);

    std::filesystem::remove(storePath);
}

// --- False (rejection) paths -------------------------------------------------

TEST(ExecutionStore, PruneRemovesTerminal) {
    const auto storePath = tempStorePath("Prune");
    std::filesystem::remove(storePath);

    {
        ExecutionStore store{storePath};
        store.record(PersistedExecution{"srv-1", "fqi", "Cmd", "x", "1000", "waiting"});
        store.prune("x");
        EXPECT_TRUE(store.list().empty());
    }

    ExecutionStore reopened{storePath};  // simulated restart
    EXPECT_TRUE(reopened.list().empty());

    std::filesystem::remove(storePath);
}

TEST(ExecutionStore, CorruptRecordThrows) {
    const auto storePath = tempStorePath("Corrupt");
    std::filesystem::remove(storePath);
    {
        std::ofstream out{storePath, std::ios::trunc};
        // Only 4 tab-separated fields; a valid record has exactly 6.
        out << "srv-1\tfqi\tCmd\tuuid-1\n";
    }

    try {
        ExecutionStore store{storePath};
        FAIL() << "expected std::runtime_error for a corrupt state file";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string{e.what()}.find(storePath.string()), std::string::npos);
    }

    std::filesystem::remove(storePath);
}

TEST(ExecutionStore, MissingFileIsEmpty) {
    const auto storePath = tempStorePath("Missing");
    std::filesystem::remove(storePath);
    ASSERT_FALSE(std::filesystem::exists(storePath));

    ExecutionStore store{storePath};
    EXPECT_TRUE(store.list().empty());
}

}  // namespace
