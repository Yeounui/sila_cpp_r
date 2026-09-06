// Integration test: RecoverableErrorGate::raiseAndWait() publishes the
// current set of pending RecoverableError structures to
// ObservablePropertyManager subscribers of kRecoverableErrorsPropertyId
// (RecoverableErrorGate.h) — once after inserting a new pending error
// (RecoverableErrorGate.cc) and once after removing a resolved one.
// test_recoverable_error_gate.cc only checks raiseAndWait()'s return value;
// this file follows the data all the way to the subscriber.
#include <sila/server/recovery/RecoverableErrorGate.h>
#include <sila/server/property/ObservablePropertyManager.h>
#include <sila/common/error/SiLAErrorSubtypes.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <any>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using sila2::ObservablePropertyManager;
using sila2::Subscription;
using sila2::recovery::ContinuationOption;
using sila2::recovery::RecoverableError;
using sila2::recovery::RecoverableErrorGate;
using sila2::recovery::RecoveryChoice;

namespace {

using sila2::recovery::kRecoverableErrorsPropertyId;

constexpr const char* kErrorIdentifier =
    "org.example/test/Shaker/v1/DefinedExecutionError/Stalled";
constexpr const char* kCommandIdentifier =
    "org.example/test/Shaker/v1/Command/ShakeForTime";

// S39 made raiseAndWait() enforce the FDL CommandExecutionUUID constraint
// (Length 36 + lowercase-hex Pattern) before publishing anything, so the old
// "uuid-1"-style placeholders this file used are now rejected outright.
constexpr const char* kUuidRaisePublishes = "10101010-1010-1010-1010-101010101010";
constexpr const char* kUuidSelectPublishesEmpty = "20202020-2020-2020-2020-202020202020";
constexpr const char* kUuidConcurrentA = "30303030-3030-3030-3030-303030303030";
constexpr const char* kUuidConcurrentB = "31313131-3131-3131-3131-313131313131";
constexpr const char* kUuidFullStruct = "40404040-4040-4040-4040-404040404040";
constexpr const char* kUuidNoSubscribers = "50505050-5050-5050-5050-505050505050";
constexpr const char* kUuidCancelledSubscriber = "60606060-6060-6060-6060-606060606060";
constexpr const char* kUuidReleaseAllCleanup = "70707070-7070-7070-7070-707070707070";
constexpr const char* kUuidInvalid = "80808080-8080-8080-8080-808080808080";
constexpr const char* kUuidAfterInvalid = "81818181-8181-8181-8181-818181818181";
constexpr const char* kUuidLatePreexisting = "11111111-1111-1111-1111-111111111111";
constexpr const char* kUuidServerTimeout = "90909090-9090-9090-9090-909090909090";
constexpr const char* kUuidConstraintViolationFollowup = "91919191-9191-9191-9191-919191919191";

// Reads from sub until a published vector's size matches expectedSize.
// Publishes happen in order, so earlier reads may see an in-between size
// (e.g. 1 error while a second raiseAndWait() is still inserting) — this
// drains those before the size the test actually cares about. Bounded to 20
// reads so a broken publish path fails the test instead of hanging forever.
std::vector<RecoverableError> waitForMessageCount(Subscription& sub, std::size_t expectedSize) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto value = sub.waitForNext();
        if (!value.has_value()) {
            ADD_FAILURE() << "subscription cancelled while waiting for "
                          << expectedSize << " messages";
            return {};
        }
        auto errors = std::any_cast<std::vector<RecoverableError>>(*value);
        if (errors.size() == expectedSize) {
            return errors;
        }
    }
    ADD_FAILURE() << "did not observe a publish with " << expectedSize
                  << " messages within 20 reads";
    return {};
}

}  // namespace

// --- True paths -------------------------------------------------------

TEST(RecoverableErrorGatePropertyIntegration, RaisePublishesErrorToSubscriber) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    auto sub = propertyManager.subscribe(kRecoverableErrorsPropertyId);

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidRaisePublishes,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    // A leading empty snapshot (the property's initial current value, seeded at
    // gate construction) precedes the raise; waitForMessageCount drains it and
    // returns the 1-error publish.
    auto errors = waitForMessageCount(*sub, 1);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].errorMessage, "Something broke");

    gate.abort(kUuidRaisePublishes);
    worker.join();
}

// The reported bug (finding #4): an error raised BEFORE a client subscribes
// must still reach that client. The late subscriber here is created strictly
// after the raise's publish has landed (confirmed via syncSub) and with no
// further publish -- it must replay the current pending error immediately,
// otherwise a client that opens its subscription to discover why execution
// stalled would never see the very error it needs to recover.
TEST(RecoverableErrorGatePropertyIntegration, LateSubscriberSeesPreexistingError) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    // Gates the late subscribe below on the publish having landed, so the test
    // is deterministic and never races the worker into a hang.
    auto syncSub = propertyManager.subscribe(kRecoverableErrorsPropertyId);

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidLatePreexisting,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Raised before subscribe",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    // Drains syncSub's leading empty snapshot and returns the raise's publish,
    // confirming the error is now the property's current value.
    auto landed = waitForMessageCount(*syncSub, 1);
    ASSERT_EQ(landed.size(), 1u);

    // A subscriber arriving after the raise, with no further publish, replays
    // the current value -- the pending error -- as its initial message (not the
    // empty seed: the manager retains only the most recent publish).
    auto lateSub = propertyManager.subscribe(kRecoverableErrorsPropertyId);
    auto value = lateSub->waitForNext();
    ASSERT_TRUE(value.has_value());
    auto errors = std::any_cast<std::vector<RecoverableError>>(*value);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].errorMessage, "Raised before subscribe");

    gate.abort(kUuidLatePreexisting);
    worker.join();
}

TEST(RecoverableErrorGatePropertyIntegration, SelectOptionPublishesEmptyList) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    auto sub = propertyManager.subscribe(kRecoverableErrorsPropertyId);

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidSelectPublishesEmpty,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Pump failure",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    // Drains the leading empty snapshot (seeded at construction) and returns
    // the raise's 1-error publish.
    auto firstErrors = waitForMessageCount(*sub, 1);
    EXPECT_EQ(firstErrors.size(), 1u);

    gate.selectOption(kUuidSelectPublishesEmpty, "retry");
    worker.join();

    auto secondValue = sub->waitForNext();
    ASSERT_TRUE(secondValue.has_value());
    EXPECT_TRUE(std::any_cast<std::vector<RecoverableError>>(*secondValue).empty());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "retry");
}

TEST(RecoverableErrorGatePropertyIntegration, TwoConcurrentErrorsBothVisible) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    auto sub = propertyManager.subscribe(kRecoverableErrorsPropertyId);

    std::optional<RecoveryChoice> result1;
    std::optional<RecoveryChoice> result2;
    std::thread worker1{[&] {
        result1 = gate.raiseAndWait({
            .commandExecutionUuid = kUuidConcurrentA,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Sensor fault",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};
    // Let worker1's insert+publish land before worker2 starts, so the two
    // inserts are observably ordered rather than racing on pending_.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    std::thread worker2{[&] {
        result2 = gate.raiseAndWait({
            .commandExecutionUuid = kUuidConcurrentB,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Valve stuck",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    auto bothErrors = waitForMessageCount(*sub, 2);
    auto hasMessage = [&bothErrors](const std::string& message) {
        return std::any_of(bothErrors.begin(), bothErrors.end(),
                            [&message](const RecoverableError& error) {
                                return error.errorMessage == message;
                            });
    };
    EXPECT_TRUE(hasMessage("Sensor fault"));
    EXPECT_TRUE(hasMessage("Valve stuck"));

    gate.selectOption(kUuidConcurrentA, "retry");
    worker1.join();

    auto oneError = waitForMessageCount(*sub, 1);
    ASSERT_EQ(oneError.size(), 1u);
    EXPECT_EQ(oneError[0].errorMessage, "Valve stuck");

    gate.abort(kUuidConcurrentB);
    worker2.join();

    auto zeroErrors = waitForMessageCount(*sub, 0);
    EXPECT_TRUE(zeroErrors.empty());

    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1->optionIdentifier, "retry");
    EXPECT_FALSE(result2.has_value());
}

TEST(RecoverableErrorGatePropertyIntegration, PublishedErrorCarriesEveryFdlElement) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    auto sub = propertyManager.subscribe(kRecoverableErrorsPropertyId);

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidFullStruct,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            .continuationOptions = {
                {"retry", true, std::chrono::seconds{30}, "Retry the command", "none"},
                {"skip", false, std::chrono::seconds{0}, "Skip the step", "none"},
            },
        });
    }};

    // Drains the leading empty snapshot (seeded at construction); the next
    // publish carries the fully-populated error.
    auto errors = waitForMessageCount(*sub, 1);
    ASSERT_EQ(errors.size(), 1u);
    const RecoverableError& error = errors[0];

    EXPECT_EQ(error.commandExecutionUuid, kUuidFullStruct);
    EXPECT_EQ(error.errorIdentifier, kErrorIdentifier);
    EXPECT_EQ(error.commandIdentifier, kCommandIdentifier);
    EXPECT_EQ(error.errorMessage, "Something broke");
    EXPECT_NE(error.errorTime.year, 0u);
    ASSERT_EQ(error.continuationOptions.size(), 2u);
    EXPECT_EQ(error.continuationOptions[0].description, "Retry the command");
    EXPECT_EQ(error.continuationOptions[0].requiredInputData, "none");
    EXPECT_EQ(error.continuationOptions[1].description, "Skip the step");
    EXPECT_EQ(error.continuationOptions[1].requiredInputData, "none");

    gate.selectOption(kUuidFullStruct, "skip");
    worker.join();
    ASSERT_TRUE(result.has_value());
}

// --- S35: ErrorHandlingTimeout is the server's only wait bound ----------

TEST(RecoverableErrorGatePropertyIntegration, ServerTimeoutRemovesErrorFromPublishedList) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager, std::chrono::seconds{1}};
    auto sub = propertyManager.subscribe(kRecoverableErrorsPropertyId);

    // A default-flagged option with a longer client-side window: the
    // server's ErrorHandlingTimeout alone (1s) must still win (S35 deleted
    // the std::min combination with automaticSelectionTimeout).
    const auto result = gate.raiseAndWait({
        .commandExecutionUuid = kUuidServerTimeout,
        .errorIdentifier = kErrorIdentifier,
        .commandIdentifier = kCommandIdentifier,
        .errorMessage = "Timed out",
        .continuationOptions = {{"retry", true, std::chrono::seconds{5}}},
    });

    EXPECT_FALSE(result.has_value());

    auto firstSnapshot = waitForMessageCount(*sub, 1);
    ASSERT_EQ(firstSnapshot.size(), 1u);
    EXPECT_EQ(firstSnapshot[0].commandExecutionUuid, kUuidServerTimeout);

    // FDL :29-33 -- "removing the current error from the list of the
    // 'Recoverable Errors' property" -- pins that the cleanup publish still
    // happens on the nullopt/ErrorHandlingTimeout path, which before S35 was
    // reached only via abort()/releaseAll().
    auto finalSnapshot = waitForMessageCount(*sub, 0);
    EXPECT_TRUE(finalSnapshot.empty());
}

// --- False paths --------------------------------------------------------

TEST(RecoverableErrorGatePropertyIntegration, NoSubscribersDoesNotCrash) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    // Deliberately no subscribe() call — publishPendingErrors() publishes to
    // a property id with zero subscribers, which ObservablePropertyManager::
    // publish() handles by finding nothing in subscribers_ and returning
    // early. CAUGHT: the manager already tolerates this; gate's own flow
    // still has to complete normally.

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidNoSubscribers,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Silent failure",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.selectOption(kUuidNoSubscribers, "retry");
    worker.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "retry");
}

TEST(RecoverableErrorGatePropertyIntegration, CancelledSubscriberDoesNotAffectGate) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    auto sub = propertyManager.subscribe(kRecoverableErrorsPropertyId);
    sub->cancel();
    // CAUGHT: Subscription::waitForNext() checks cancelled_ before looking at
    // the queue, so a cancelled subscriber never observes values enqueued
    // after cancellation — publish() itself does not check cancellation
    // before enqueue(), so this also exercises that publish() keeps working
    // against a dead subscriber without the gate noticing or failing.

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidCancelledSubscriber,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Cancelled-subscriber failure",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.selectOption(kUuidCancelledSubscriber, "retry");
    worker.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "retry");
    EXPECT_FALSE(sub->waitForNext().has_value());
}

TEST(RecoverableErrorGatePropertyIntegration, ReleaseAllReturnsNulloptAndPublishesCleanup) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    auto sub = propertyManager.subscribe(kRecoverableErrorsPropertyId);

    std::optional<RecoveryChoice> result{RecoveryChoice{"placeholder", {}}};
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidReleaseAllCleanup,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Fatal fault",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    // Drains the leading empty snapshot (seeded at construction) and returns
    // the raise's 1-error publish.
    auto firstErrors = waitForMessageCount(*sub, 1);
    EXPECT_EQ(firstErrors.size(), 1u);

    gate.releaseAll();
    worker.join();

    EXPECT_FALSE(result.has_value());

    // CAUGHT: releaseAll() itself does not call publishPendingErrors() —
    // this cleanup publish comes from the woken raiseAndWait() erasing its
    // own entry after resolved_ wakes it.
    auto secondValue = sub->waitForNext();
    ASSERT_TRUE(secondValue.has_value());
    EXPECT_TRUE(std::any_cast<std::vector<RecoverableError>>(*secondValue).empty());
}

TEST(RecoverableErrorGatePropertyIntegration, InvalidErrorPublishesNothing) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    auto sub = propertyManager.subscribe(kRecoverableErrorsPropertyId);

    // FDL MinimalElementCount 1 — this must throw before publishPendingErrors()
    // ever runs, so the subscriber's queue stays untouched by it.
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidInvalid,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {},
                 }),
                 std::invalid_argument);

    // The subscriber's queue holds one message so far: the empty snapshot seeded
    // at construction. If the rejected raise had published (an empty vector, or
    // one with a half-built entry), it would land right after that snapshot,
    // before the valid raise below -- so draining the snapshot and finding the
    // valid error next proves the invalid raise left the queue untouched.
    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidAfterInvalid,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "m",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    auto initial = sub->waitForNext();
    ASSERT_TRUE(initial.has_value());
    EXPECT_TRUE(std::any_cast<std::vector<RecoverableError>>(*initial).empty());

    auto value = sub->waitForNext();
    ASSERT_TRUE(value.has_value());
    auto errors = std::any_cast<std::vector<RecoverableError>>(*value);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].commandExecutionUuid, kUuidAfterInvalid);

    gate.abort(kUuidAfterInvalid);
    worker.join();
}

// --- S39: FDL constraint validation ---------------------------------------

TEST(RecoverableErrorGatePropertyIntegration, ConstraintViolationPublishesNothing) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    auto sub = propertyManager.subscribe(kRecoverableErrorsPropertyId);

    // The only defect is a malformed commandExecutionUuid (S39) -- the guard
    // sits above RecoverableErrorGate.cc's publishPendingErrors(), so nothing
    // reaches the subscriber's queue.
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = "not-a-uuid",
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
                 }),
                 std::invalid_argument);

    // A subsequent well-formed raise on the same gate publishes normally,
    // proving the throw left no partial state in pending_.
    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidConstraintViolationFollowup,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "m",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    // Drain the empty snapshot seeded at construction; the well-formed raise is
    // the next message, and nothing from the rejected raise sits between them.
    auto initial = sub->waitForNext();
    ASSERT_TRUE(initial.has_value());
    EXPECT_TRUE(std::any_cast<std::vector<RecoverableError>>(*initial).empty());

    auto value = sub->waitForNext();
    ASSERT_TRUE(value.has_value());
    auto errors = std::any_cast<std::vector<RecoverableError>>(*value);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].commandExecutionUuid, kUuidConstraintViolationFollowup);

    gate.abort(kUuidConstraintViolationFollowup);
    worker.join();
}
