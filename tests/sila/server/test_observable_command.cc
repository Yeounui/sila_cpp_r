// Checks for ObservableCommandExecution (state machine, cancellation,
// lifetime expiration) and ObservableCommandManager (UUID generation,
// lookup, GC sweep, bulk interruption).
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>

#include <sila/error/SiLAErrorSubtypes.h>

#include <gtest/gtest.h>

#include <chrono>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>

using sila2::ObservableCommandExecution;
using sila2::ObservableCommandManager;
using State = ObservableCommandExecution::State;

// ---------------------------------------------------------------------------
// ObservableCommandExecution
// ---------------------------------------------------------------------------

TEST(ObservableCommandExecution, StartsInWaitingState) {
    ObservableCommandExecution exec{"test-uuid", std::chrono::seconds{0}};
    EXPECT_EQ(exec.state(), State::Waiting);
    EXPECT_EQ(exec.uuid(), "test-uuid");
}

TEST(ObservableCommandExecution, NormalLifecycle) {
    ObservableCommandExecution exec{"uuid-1", std::chrono::seconds{0}};
    exec.start();
    EXPECT_EQ(exec.state(), State::Running);

    exec.setProgress(0.5, std::chrono::seconds{10});
    EXPECT_DOUBLE_EQ(exec.progress(), 0.5);
    EXPECT_EQ(exec.estimatedRemaining(), std::chrono::seconds{10});

    exec.finish();
    EXPECT_EQ(exec.state(), State::FinishedSuccessfully);
}

TEST(ObservableCommandExecution, ErrorLifecycle) {
    ObservableCommandExecution exec{"uuid-2", std::chrono::seconds{0}};
    exec.start();
    exec.fail("something went wrong");
    EXPECT_EQ(exec.state(), State::FinishedWithError);
    EXPECT_EQ(exec.errorMessage(), "something went wrong");
}

TEST(ObservableCommandExecution, InvalidTransitionFromWaitingToFinished) {
    ObservableCommandExecution exec{"uuid-3", std::chrono::seconds{0}};
    EXPECT_THROW(exec.finish(), std::logic_error);
}

TEST(ObservableCommandExecution, InvalidTransitionFromWaitingToFailed) {
    ObservableCommandExecution exec{"uuid-4", std::chrono::seconds{0}};
    EXPECT_THROW(exec.fail("nope"), std::logic_error);
}

TEST(ObservableCommandExecution, InvalidTransitionDoubleStart) {
    ObservableCommandExecution exec{"uuid-5", std::chrono::seconds{0}};
    exec.start();
    EXPECT_THROW(exec.start(), std::logic_error);
}

TEST(ObservableCommandExecution, InvalidTransitionDoubleFinish) {
    ObservableCommandExecution exec{"uuid-6", std::chrono::seconds{0}};
    exec.start();
    exec.finish();
    EXPECT_THROW(exec.finish(), std::logic_error);
}

TEST(ObservableCommandExecution, CancellationFlag) {
    ObservableCommandExecution exec{"uuid-7", std::chrono::seconds{0}};
    EXPECT_FALSE(exec.isInterruptionRequested());
    exec.requestInterruption();
    EXPECT_TRUE(exec.isInterruptionRequested());
}

TEST(ObservableCommandExecution, StateToStringCoversAllValues) {
    EXPECT_EQ(ObservableCommandExecution::stateToString(State::Waiting), "Waiting");
    EXPECT_EQ(ObservableCommandExecution::stateToString(State::Running), "Running");
    EXPECT_EQ(ObservableCommandExecution::stateToString(State::FinishedSuccessfully),
              "Finished Successfully");
    EXPECT_EQ(ObservableCommandExecution::stateToString(State::FinishedWithError),
              "Finished With Error");
}

TEST(ObservableCommandExecution, ZeroLifetimeNeverExpires) {
    ObservableCommandExecution exec{"uuid-8", std::chrono::seconds{0}};
    exec.start();
    exec.finish();
    EXPECT_FALSE(exec.isExpired());
}

TEST(ObservableCommandExecution, LifetimeExpiresAfterDuration) {
    ObservableCommandExecution exec{"uuid-9", std::chrono::seconds{1}};
    exec.start();
    exec.finish();
    EXPECT_FALSE(exec.isExpired());
    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    EXPECT_TRUE(exec.isExpired());
}

TEST(ObservableCommandExecution, RunningCommandDoesNotExpire) {
    ObservableCommandExecution exec{"uuid-10", std::chrono::seconds{1}};
    exec.start();
    EXPECT_FALSE(exec.isExpired());
}

// ---------------------------------------------------------------------------
// ObservableCommandManager
// ---------------------------------------------------------------------------

TEST(ObservableCommandManager, AddAndGetRoundTrip) {
    ObservableCommandManager manager;
    auto& exec = manager.addCommand();
    EXPECT_EQ(manager.size(), 1);
    EXPECT_EQ(&manager.getCommand(exec.uuid()), &exec);
}

TEST(ObservableCommandManager, GeneratesValidUuidV4) {
    ObservableCommandManager manager;
    auto& exec = manager.addCommand();
    const std::regex uuidV4Pattern{
        "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};
    EXPECT_TRUE(std::regex_match(exec.uuid(), uuidV4Pattern))
        << "UUID does not match v4 format: " << exec.uuid();
}

TEST(ObservableCommandManager, ThrowsFrameworkErrorOnUnknownUuid) {
    ObservableCommandManager manager;
    EXPECT_THROW(manager.getCommand("nonexistent"),
                 sila2::error::FrameworkError);
}

TEST(ObservableCommandManager, RemoveExpiredSweepsFinishedCommands) {
    ObservableCommandManager manager;
    auto& shortLived = manager.addCommand(std::chrono::seconds{1});
    manager.addCommand(std::chrono::seconds{0});

    shortLived.start();
    shortLived.finish();
    EXPECT_EQ(manager.size(), 2);
    EXPECT_EQ(manager.removeExpired(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    EXPECT_EQ(manager.removeExpired(), 1);
    EXPECT_EQ(manager.size(), 1);
}

TEST(ObservableCommandManager, InterruptAllSetsFlags) {
    ObservableCommandManager manager;
    auto& exec1 = manager.addCommand();
    auto& exec2 = manager.addCommand();
    exec1.start();

    manager.interruptAll();
    EXPECT_TRUE(exec1.isInterruptionRequested());
    EXPECT_TRUE(exec2.isInterruptionRequested());
}

TEST(ObservableCommandManager, GeneratesUniqueUuids) {
    ObservableCommandManager manager;
    std::set<std::string> uuids;
    for (int i = 0; i < 100; ++i) {
        uuids.insert(manager.addCommand().uuid());
    }
    EXPECT_EQ(uuids.size(), 100);
}

TEST(ObservableCommandManager, AutoGCSweepsExpiredCommands) {
    ObservableCommandManager manager;
    auto& exec = manager.addCommand(std::chrono::seconds{1});
    exec.start();
    exec.finish();
    EXPECT_EQ(manager.size(), 1);

    manager.startAutoGC(std::chrono::seconds{1});
    EXPECT_TRUE(manager.isAutoGCRunning());

    std::this_thread::sleep_for(std::chrono::milliseconds{2500});
    EXPECT_EQ(manager.size(), 0);

    manager.stopAutoGC();
    EXPECT_FALSE(manager.isAutoGCRunning());
}

TEST(ObservableCommandManager, AutoGCStopsOnDestruction) {
    auto manager = std::make_unique<ObservableCommandManager>();
    manager->startAutoGC(std::chrono::seconds{1});
    EXPECT_TRUE(manager->isAutoGCRunning());
    manager.reset();
}

TEST(ObservableCommandManager, StartAutoGCIsIdempotent) {
    ObservableCommandManager manager;
    manager.startAutoGC(std::chrono::seconds{1});
    manager.startAutoGC(std::chrono::seconds{1});
    EXPECT_TRUE(manager.isAutoGCRunning());
    manager.stopAutoGC();
}
