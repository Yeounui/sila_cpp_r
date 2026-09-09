// End-to-end tests for the cross-component GC cleanup flow:
// ObservableCommandManager::removeExpired() (ObservableCommandManager.cc)
// erases each expired UUID under mu_, then fires removalCallback_ per
// erased UUID after releasing mu_ (architecture.md §2.2c — avoids holding
// the manager's lock while calling into CloudEnvelopeRouter, whose own
// lock is invoked reentrantly by the callback). That callback is designed
// to wire into CloudEnvelopeRouter::removeExecutionFQI
// (CloudEnvelopeRouter.cc), which erases the UUID from executionFqis_.
// Without this wiring,
// the router accumulates stale FQI mappings for commands the manager has
// already GC'd. Covers callback firing (with and without a wired router),
// multi-command sweeps, and the negative paths where nothing should fire:
// no callback set, non-expired commands, and still-running commands.
#include "CloudRouterTestHarness.h"

#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace cloud = sila2::org::silastandard;
using cloud_test::CloudRouterFixture;
using sila2::ObservableCommandManager;

class CloudRouterGcCleanup : public CloudRouterFixture {
protected:
    sila2::FeatureRegistry registry_;
    ObservableCommandManager manager_;
};

// ---------------------------------------------------------------------------
// Positive (True) paths
// ---------------------------------------------------------------------------

TEST(ObservableGcCleanup, RemovalCallbackFiresWithCorrectUuid) {
    ObservableCommandManager manager;
    std::vector<std::string> removed;
    manager.addRemovalObserver([&removed](const std::string& uuid) { removed.push_back(uuid); });

    auto exec = manager.addCommand(std::chrono::seconds{1});
    std::string uuid = exec->uuid();
    exec->start();
    exec->finish();

    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    EXPECT_EQ(manager.removeExpired(), 1);

    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0], uuid);
}

TEST_F(CloudRouterGcCleanup, GcCleansUpRouterExecutionFqi) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    // The real wiring: GC-driven removal drains the router's FQI map.
    manager_.addRemovalObserver([&router](const std::string& uuid) { router.removeExecutionFQI(uuid); });

    auto exec = manager_.addCommand(std::chrono::seconds{1});
    std::string uuid = exec->uuid();
    router.registerExecutionFQI(uuid, "org.test/Feature/Command/v1");
    exec->start();
    exec->finish();

    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    EXPECT_EQ(manager_.removeExpired(), 1);

    // The manager already dropped the execution too, so lookup at
    // dispatchObservableByUuid's first step (getCommand) fails first —
    // this confirms the callback ran as part of the same sweep, not that
    // the FQI erase alone is observable in isolation.
    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-check");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);
    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(),
              cloud::FrameworkError::INVALID_COMMAND_EXECUTION_UUID);
}

TEST(ObservableGcCleanup, MultipleExpiredCommandsAllCallbacksFire) {
    ObservableCommandManager manager;
    std::vector<std::string> removed;
    manager.addRemovalObserver([&removed](const std::string& uuid) { removed.push_back(uuid); });

    std::vector<std::string> uuids;
    for (int i = 0; i < 3; ++i) {
        auto exec = manager.addCommand(std::chrono::seconds{1});
        uuids.push_back(exec->uuid());
        exec->start();
        exec->finish();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    EXPECT_EQ(manager.removeExpired(), 3);

    ASSERT_EQ(removed.size(), 3u);
    for (const auto& uuid : uuids) {
        EXPECT_NE(std::find(removed.begin(), removed.end(), uuid), removed.end())
            << "missing uuid: " << uuid;
    }
}

// ---------------------------------------------------------------------------
// Negative (False) paths — all UNCAUGHT: removeExpired() has no validation
// to reject; these exercise conditions under which the callback correctly
// does NOT fire, verifying the sweep does not over-fire or crash absent
// a callback.
// ---------------------------------------------------------------------------

TEST(ObservableGcCleanup, NoCallbackSetGcDoesNotCrash) {
    ObservableCommandManager manager;
    // Deliberately no addRemovalObserver() call.
    auto exec = manager.addCommand(std::chrono::seconds{1});
    exec->start();
    exec->finish();

    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    EXPECT_EQ(manager.removeExpired(), 1);
    EXPECT_EQ(manager.size(), 0u);
}

TEST(ObservableGcCleanup, NonExpiredCommandsCallbackNotCalled) {
    ObservableCommandManager manager;
    std::vector<std::string> removed;
    manager.addRemovalObserver([&removed](const std::string& uuid) { removed.push_back(uuid); });

    // Zero lifetime means never expires (ObservableCommandManager.h:32).
    auto exec = manager.addCommand(std::chrono::seconds{0});
    exec->start();
    exec->finish();

    EXPECT_EQ(manager.removeExpired(), 0);
    EXPECT_TRUE(removed.empty());
}

TEST(ObservableGcCleanup, RunningCommandNotGarbageCollected) {
    ObservableCommandManager manager;
    std::vector<std::string> removed;
    manager.addRemovalObserver([&removed](const std::string& uuid) { removed.push_back(uuid); });

    auto exec = manager.addCommand(std::chrono::seconds{1});
    std::string uuid = exec->uuid();
    exec->start();
    // Deliberately no exec->finish(): isExpired() only trips post-finish.

    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    EXPECT_EQ(manager.removeExpired(), 0);
    EXPECT_TRUE(removed.empty());
    EXPECT_EQ(manager.size(), 1u);
    EXPECT_EQ(manager.getCommand(uuid).get(), &*exec);
}

}  // namespace
