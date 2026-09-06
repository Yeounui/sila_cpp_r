// Checks RecoverableErrorGate's blocking select/abort/timeout flows,
// unknown-uuid/unknown-option error paths, releaseAll shutdown, and
// raiseAndWait()'s FDL structural validation (S6, S35, S39).
#include <sila/server/recovery/RecoverableErrorGate.h>
#include <sila/server/property/ObservablePropertyManager.h>
#include <sila/common/error/SiLAErrorSubtypes.h>

#include <gtest/gtest.h>

#include <any>
#include <atomic>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using sila2::ObservablePropertyManager;
using sila2::recovery::ContinuationOption;
using sila2::recovery::RecoverableError;
using sila2::recovery::RecoverableErrorGate;
using sila2::recovery::RecoveryChoice;

namespace {
constexpr const char* kErrorIdentifier =
    "org.example/test/Shaker/v1/DefinedExecutionError/Stalled";
constexpr const char* kCommandIdentifier =
    "org.example/test/Shaker/v1/Command/ShakeForTime";

// S39 made raiseAndWait() enforce the FDL CommandExecutionUUID constraint
// (Length 36 + lowercase-hex Pattern, ErrorRecoveryService-v2_0.sila.xml
// :204-207) before publishing anything, so the old "uuid-1"-style
// placeholders this file used are now rejected outright. One named constant
// per scenario keeps that sweep readable instead of ~20 inline literals.
constexpr const char* kUuidNormalSelection = "11111111-1111-1111-1111-111111111111";
constexpr const char* kUuidAbort = "22222222-2222-2222-2222-222222222222";
constexpr const char* kUuidGlobalTimeout = "33333333-3333-3333-3333-333333333333";
constexpr const char* kUuidA = "44444444-4444-4444-4444-444444444444";
constexpr const char* kUuidServerBoundOnly = "45454545-4545-4545-4545-454545454545";
constexpr const char* kUuidB = "46464646-4646-4646-4646-464646464646";
constexpr const char* kUuidC = "47474747-4747-4747-4747-474747474747";
constexpr const char* kUuidD = "48484848-4848-4848-4848-484848484848";
constexpr const char* kUuidUnknownOption = "55555555-5555-5555-5555-555555555555";
constexpr const char* kUuidReleaseAll = "66666666-6666-6666-6666-666666666666";
constexpr const char* kUuidDuplicate = "77777777-7777-7777-7777-777777777777";
constexpr const char* kUuidIdempotent = "88888888-8888-8888-8888-888888888888";
constexpr const char* kUuidConflict = "99999999-9999-9999-9999-999999999999";
constexpr const char* kUuidAbortRetry = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
constexpr const char* kUuidFull = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
constexpr const char* kUuidEmptyOptions = "cccccccc-cccc-cccc-cccc-cccccccccccc";
constexpr const char* kUuidDupOption = "dddddddd-dddd-dddd-dddd-dddddddddddd";
constexpr const char* kUuidTwoDefaults = "eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee";
constexpr const char* kUuidMissingCmdId = "ffffffff-ffff-ffff-ffff-ffffffffffff";
constexpr const char* kUuidCanonical = "01010101-0101-0101-0101-010101010101";
constexpr const char* kUuidMaxLengthChars = "02020202-0202-0202-0202-020202020202";
constexpr const char* kUuidZeroAutoSelect = "03030303-0303-0303-0303-030303030303";
constexpr const char* kUuidMalformedFqi = "04040404-0404-0404-0404-040404040404";
constexpr const char* kUuidValidationProbe = "05050505-0505-0505-0505-050505050505";
constexpr const char* kUuidCategoryCase = "06060606-0606-0606-0606-060606060606";
}  // namespace

TEST(RecoverableErrorGate, NormalSelection) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidNormalSelection,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            .continuationOptions = {
                {"retry", false, std::chrono::seconds{0}},
                {"skip", false, std::chrono::seconds{0}},
            },
        });
    }};

    // Small sleep to let raiseAndWait() block on the CV.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.selectOption(kUuidNormalSelection, "retry", std::any{42});
    worker.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "retry");
    EXPECT_EQ(std::any_cast<int>(result->inputData), 42);
}

TEST(RecoverableErrorGate, Abort) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    std::optional<RecoveryChoice> result{RecoveryChoice{"placeholder", {}}};
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidAbort,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.abort(kUuidAbort);
    worker.join();

    EXPECT_FALSE(result.has_value());
}

TEST(RecoverableErrorGate, GlobalTimeout) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager, std::chrono::seconds{1}};

    const auto start = std::chrono::steady_clock::now();
    const auto result = gate.raiseAndWait({
        .commandExecutionUuid = kUuidGlobalTimeout,
        .errorIdentifier = kErrorIdentifier,
        .commandIdentifier = kCommandIdentifier,
        .errorMessage = "Something broke",
        .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
    });
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result.has_value());
    EXPECT_GE(elapsed, std::chrono::seconds{1});
}

// --- S35: AutomaticSelectionTimeout is the CLIENT's job, not the gate's --
// (ErrorRecoveryService-v2_0.sila.xml :264-268 vs :136-139). Replaces the old
// AutoExecuteDefault test, which asserted exactly the behaviour removed here.

TEST(RecoverableErrorGate, AutomaticSelectionTimeoutDoesNotResolveOnServer) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};  // defaultTimeout_ == 0 -- FDL-indefinite.

    std::atomic<bool> finished{false};
    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidA,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            .continuationOptions = {
                {"retry", false, std::chrono::seconds{0}},
                {"skip", true, std::chrono::seconds{1}},
            },
        });
        finished = true;
    }};

    // 1.5x the flagged option's automaticSelectionTimeout: if the server
    // still auto-selected on its own (the pre-S35 behaviour), the worker
    // would already be done by now.
    std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    EXPECT_FALSE(finished.load());

    gate.selectOption(kUuidA, "retry");
    worker.join();

    ASSERT_TRUE(result.has_value());
    // The client's choice, not the flagged default "skip" -- proves no
    // server-side selection fired.
    EXPECT_EQ(result->optionIdentifier, "retry");
}

TEST(RecoverableErrorGate, ServerErrorHandlingTimeoutIsTheOnlyServerBound) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager, std::chrono::seconds{1}};

    const auto start = std::chrono::steady_clock::now();
    const auto result = gate.raiseAndWait({
        .commandExecutionUuid = kUuidServerBoundOnly,
        .errorIdentifier = kErrorIdentifier,
        .commandIdentifier = kCommandIdentifier,
        .errorMessage = "Something broke",
        .continuationOptions = {
            {"retry", false, std::chrono::seconds{0}},
            {"skip", true, std::chrono::seconds{5}},
        },
    });
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // FDL :136-139 -- unrecoverable error, not the default option.
    EXPECT_FALSE(result.has_value());
    // The server waited its ErrorHandlingTimeout.
    EXPECT_GE(elapsed, std::chrono::seconds{1});
    // The longer client timeout did not extend it -- pins that the deleted
    // std::min combination is gone.
    EXPECT_LT(elapsed, std::chrono::seconds{5});
}

TEST(RecoverableErrorGate, ClientSelectionAfterAutomaticSelectionWindowStillWins) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager, std::chrono::seconds{5}};

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidB,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            .continuationOptions = {
                {"retry", false, std::chrono::seconds{0}},
                {"skip", true, std::chrono::seconds{1}},
            },
        });
    }};

    // Past the option's 1s automaticSelectionTimeout but still inside the
    // server's 5s ErrorHandlingTimeout: a late client choice must still be
    // honoured. Before S35 the gate would already have auto-resolved to
    // "skip" at t=1s and this selectOption() would then throw
    // InvalidCommandExecutionUUID off the resolved_ tombstone.
    std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    gate.selectOption(kUuidB, "retry");
    worker.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "retry");
}

TEST(RecoverableErrorGate, ZeroErrorHandlingTimeoutWaitsIndefinitely) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};  // ErrorHandlingTimeout 0.

    std::atomic<bool> finished{false};
    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidC,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            .continuationOptions = {
                {"retry", false, std::chrono::seconds{0}},
                {"skip", true, std::chrono::seconds{1}},
            },
        });
        finished = true;
    }};

    // FDL :139 -- "A value of zero specifies an indefinite time": the
    // option's own timeout must not substitute for the absent server bound.
    std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    EXPECT_FALSE(finished.load());

    gate.abort(kUuidC);
    worker.join();
    EXPECT_FALSE(result.has_value());
}

TEST(RecoverableErrorGate, AbortInsideAutomaticSelectionWindowReturnsNullopt) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    std::optional<RecoveryChoice> result{RecoveryChoice{"placeholder", {}}};
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidD,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            .continuationOptions = {
                {"retry", false, std::chrono::seconds{0}},
                {"skip", true, std::chrono::seconds{2}},
            },
        });
    }};

    // Well inside the option's 2s automaticSelectionTimeout: abort beats it,
    // so no fabricated default choice can appear.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    gate.abort(kUuidD);
    worker.join();

    EXPECT_FALSE(result.has_value());
    // The entry is gone, resolved as aborted.
    EXPECT_THROW(gate.selectOption(kUuidD, "skip"), sila2::error::DefinedExecutionError);
}

TEST(RecoverableErrorGate, UnknownUuidInSelectOption) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    EXPECT_THROW(gate.selectOption("no-such-uuid", "retry"),
                 sila2::error::FrameworkError);
}

TEST(RecoverableErrorGate, UnknownOptionInSelectOption) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidUnknownOption,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    // Pins the FDL-declared identifier (audit S8a): the gate must throw this
    // exact string, not just the DefinedExecutionError type, since
    // ErrorRecoveryServiceImpl no longer remaps it before it reaches the wire.
    try {
        gate.selectOption(kUuidUnknownOption, "does-not-exist");
        FAIL() << "expected DefinedExecutionError";
    } catch (const sila2::error::DefinedExecutionError& e) {
        EXPECT_EQ(e.errorIdentifier(),
                  "org.silastandard/core/ErrorRecoveryService/v2/DefinedExecutionError/UnknownContinuationOption");
    }

    // The failed selectOption() left raiseAndWait() still blocked; abort it
    // so the worker thread can finish and be joined.
    gate.abort(kUuidUnknownOption);
    worker.join();
}

TEST(RecoverableErrorGate, ReleaseAllReturnsNullopt) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    std::optional<RecoveryChoice> result{RecoveryChoice{"placeholder", {}}};
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidReleaseAll,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            // S35: a default-flagged option with a live automaticSelectionTimeout
            // is included on purpose -- shutdown must never resolve to it.
            .continuationOptions = {
                {"retry", false, std::chrono::seconds{0}},
                {"skip", true, std::chrono::seconds{1}},
            },
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.releaseAll();
    worker.join();

    EXPECT_FALSE(result.has_value());
}

TEST(RecoverableErrorGate, DuplicateUuidThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    std::optional<RecoveryChoice> result;
    std::thread worker1{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidDuplicate,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "error 1",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidDuplicate,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "error 2",
                     .continuationOptions = {{"skip", false, std::chrono::seconds{0}}},
                 }),
                 sila2::error::FrameworkError);

    gate.selectOption(kUuidDuplicate, "retry");
    worker1.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "retry");
}

TEST(RecoverableErrorGate, SelectOptionRetryIdempotent) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidIdempotent,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "error",
            .continuationOptions = {
                {"retry", false, std::chrono::seconds{0}},
                {"skip", false, std::chrono::seconds{0}},
            },
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.selectOption(kUuidIdempotent, "retry");
    worker.join();

    // Same choice again after resolution is a no-op.
    EXPECT_NO_THROW(gate.selectOption(kUuidIdempotent, "retry"));
}

TEST(RecoverableErrorGate, SelectOptionRetryConflictThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidConflict,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "error",
            .continuationOptions = {
                {"retry", false, std::chrono::seconds{0}},
                {"skip", false, std::chrono::seconds{0}},
            },
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.selectOption(kUuidConflict, "retry");
    worker.join();

    // Pins the FDL-declared identifier (audit S8a): RecoveryAlreadyResolved is
    // not FDL-declared, so an already-resolved-with-a-different-option retry
    // must surface as InvalidCommandExecutionUUID instead.
    try {
        gate.selectOption(kUuidConflict, "skip");
        FAIL() << "expected DefinedExecutionError";
    } catch (const sila2::error::DefinedExecutionError& e) {
        EXPECT_EQ(e.errorIdentifier(),
                  "org.silastandard/core/ErrorRecoveryService/v2/DefinedExecutionError/InvalidCommandExecutionUUID");
    }
}

TEST(RecoverableErrorGate, AbortRetryIdempotent) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidAbortRetry,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "error",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.selectOption(kUuidAbortRetry, "retry");
    worker.join();

    // Abort on an already-resolved uuid is a no-op.
    EXPECT_NO_THROW(gate.abort(kUuidAbortRetry));
}

// --- S6: raiseAndWait() FDL structural validation -------------------------

TEST(RecoverableErrorGate, RaiseStampsErrorTimeAndPreservesIdentity) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    auto sub = propertyManager.subscribe(sila2::recovery::kRecoverableErrorsPropertyId);

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidFull,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "Something broke",
            .continuationOptions = {
                {"retry", true, std::chrono::seconds{30}, "Retry the command", "none"},
                {"skip", false, std::chrono::seconds{0}, "Skip the step", "none"},
            },
        });
    }};

    // Drain the empty snapshot seeded at gate construction. The raise's publish
    // happens inside raiseAndWait() before it blocks and is the next message,
    // carrying the stamped errorTime for this error.
    auto initial = sub->waitForNext();
    ASSERT_TRUE(initial.has_value());
    ASSERT_TRUE(std::any_cast<const std::vector<RecoverableError>&>(*initial).empty());

    auto published = sub->waitForNext();
    ASSERT_TRUE(published.has_value());
    const auto& errors = std::any_cast<const std::vector<RecoverableError>&>(*published);
    ASSERT_EQ(errors.size(), 1u);
    // gmtime_r stamps UTC, so timezone is always {0, 0} (BasicTypes.h
    // timestampFromSystemClock) and the year is whatever "now" is, which by
    // construction is well past 2026.
    EXPECT_GE(errors[0].errorTime.year, 2026u);
    EXPECT_EQ(errors[0].errorTime.timezone.hours, 0);
    EXPECT_EQ(errors[0].errorTime.timezone.minutes, 0u);

    gate.selectOption(kUuidFull, "skip");
    worker.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "skip");
}

TEST(RecoverableErrorGate, EmptyContinuationOptionsThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    // FDL MinimalElementCount 1 (ErrorRecoveryService-v2_0.sila.xml:244).
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidEmptyOptions,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {},
                 }),
                 std::invalid_argument);

    // Proves validation runs before try_emplace: the same UUID must still be
    // free afterward, so a subsequent well-formed raise succeeds.
    std::optional<RecoveryChoice> result;
    std::exception_ptr raised;
    std::thread worker{[&] {
        try {
            result = gate.raiseAndWait({
                .commandExecutionUuid = kUuidEmptyOptions,
                .errorIdentifier = kErrorIdentifier,
                .commandIdentifier = kCommandIdentifier,
                .errorMessage = "m",
                .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
            });
        } catch (...) {
            // A throw from this well-formed raise would otherwise escape the
            // thread and std::terminate the whole binary before any assert.
            raised = std::current_exception();
        }
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.abort(kUuidEmptyOptions);
    worker.join();
    EXPECT_FALSE(raised);
    // abort() resolves the wait with nullopt; a value would mean something
    // other than this test's abort answered the raise.
    EXPECT_FALSE(result.has_value());
}

TEST(RecoverableErrorGate, DuplicateOptionIdentifierThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    // FDL :299 -- ContinuationOption.Identifier must be unique within the item.
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidDupOption,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {
                         {"Retry", false, std::chrono::seconds{0}},
                         {"Retry", false, std::chrono::seconds{0}},
                     },
                 }),
                 std::invalid_argument);

    std::optional<RecoveryChoice> result;
    std::exception_ptr raised;
    std::thread worker{[&] {
        try {
            result = gate.raiseAndWait({
                .commandExecutionUuid = kUuidDupOption,
                .errorIdentifier = kErrorIdentifier,
                .commandIdentifier = kCommandIdentifier,
                .errorMessage = "m",
                .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
            });
        } catch (...) {
            raised = std::current_exception();
        }
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.abort(kUuidDupOption);
    worker.join();
    EXPECT_FALSE(raised);
    EXPECT_FALSE(result.has_value());
}

TEST(RecoverableErrorGate, TwoDefaultOptionsThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    // FDL :250 -- DefaultOption names exactly one ContinuationOption.
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidTwoDefaults,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {
                         {"retry", true, std::chrono::seconds{0}},
                         {"skip", true, std::chrono::seconds{0}},
                     },
                 }),
                 std::invalid_argument);

    std::optional<RecoveryChoice> result;
    std::exception_ptr raised;
    std::thread worker{[&] {
        try {
            result = gate.raiseAndWait({
                .commandExecutionUuid = kUuidTwoDefaults,
                .errorIdentifier = kErrorIdentifier,
                .commandIdentifier = kCommandIdentifier,
                .errorMessage = "m",
                .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
            });
        } catch (...) {
            raised = std::current_exception();
        }
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.abort(kUuidTwoDefaults);
    worker.join();
    EXPECT_FALSE(raised);
    EXPECT_FALSE(result.has_value());
}

TEST(RecoverableErrorGate, MissingCommandIdentifierThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidMissingCmdId,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = "",
                     .errorMessage = "m",
                     .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
                 }),
                 std::invalid_argument);

    std::optional<RecoveryChoice> result;
    std::exception_ptr raised;
    std::thread worker{[&] {
        try {
            result = gate.raiseAndWait({
                .commandExecutionUuid = kUuidMissingCmdId,
                .errorIdentifier = kErrorIdentifier,
                .commandIdentifier = kCommandIdentifier,
                .errorMessage = "m",
                .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
            });
        } catch (...) {
            raised = std::current_exception();
        }
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.abort(kUuidMissingCmdId);
    worker.join();
    EXPECT_FALSE(raised);
    EXPECT_FALSE(result.has_value());
}

// --- S39: FDL constraint validation (length/pattern/range) ----------------

TEST(RecoverableErrorGate, CanonicalIdentifiersAccepted) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    const std::string maxLengthErrorId(255, 'x');
    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidCanonical,
            .errorIdentifier = maxLengthErrorId,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "m",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.selectOption(kUuidCanonical, "retry");
    worker.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "retry");
}

TEST(RecoverableErrorGate, ErrorIdentifierAtMaxLengthCountsCharactersNotBytes) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};
    auto sub = propertyManager.subscribe(sila2::recovery::kRecoverableErrorsPropertyId);

    // 255 repetitions of the two-byte UTF-8 sequence U+00E9 (510 bytes, 255
    // characters) -- pins the S22/S11 character-counting convention
    // (Constraints.cc characterCount()); a byte-counting check would reject
    // this at 510.
    std::string errorId;
    for (int i = 0; i < 255; ++i) {
        errorId += "é";
    }

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidMaxLengthChars,
            .errorIdentifier = errorId,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "m",
            .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
        });
    }};

    // Drain the empty snapshot seeded at gate construction; the raise's publish
    // is the next message.
    auto initial = sub->waitForNext();
    ASSERT_TRUE(initial.has_value());
    ASSERT_TRUE(std::any_cast<const std::vector<RecoverableError>&>(*initial).empty());

    auto published = sub->waitForNext();
    ASSERT_TRUE(published.has_value());
    const auto& errors = std::any_cast<const std::vector<RecoverableError>&>(*published);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].errorIdentifier, errorId);

    gate.selectOption(kUuidMaxLengthChars, "retry");
    worker.join();
    ASSERT_TRUE(result.has_value());
}

TEST(RecoverableErrorGate, ZeroAutomaticSelectionTimeoutAccepted) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    std::optional<RecoveryChoice> result;
    std::thread worker{[&] {
        result = gate.raiseAndWait({
            .commandExecutionUuid = kUuidZeroAutoSelect,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = "m",
            // FDL :267's "no automatic selection" -- and the MinimalInclusive
            // 0 boundary (FDL :339-340).
            .continuationOptions = {{"retry", true, std::chrono::seconds{0}}},
        });
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.selectOption(kUuidZeroAutoSelect, "retry");
    worker.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "retry");
}

TEST(RecoverableErrorGate, NonUuidCommandExecutionUuidThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    // The exact placeholder shape the old tests used.
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = "uuid-1",
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
                 }),
                 std::invalid_argument);

    // Uppercase hex: FDL :206 is [0-9a-f].
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA",
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
                 }),
                 std::invalid_argument);

    // 35 characters: the Length 36 check.
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa",
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
                 }),
                 std::invalid_argument);
}

TEST(RecoverableErrorGate, MalformedCommandIdentifierThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidMalformedFqi,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = "not-an-fqi",
                     .errorMessage = "m",
                     .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
                 }),
                 std::invalid_argument);

    // A Feature FQI is not a CommandIdentifier -- this is the case
    // types::checkFullyQualifiedIdentifier() would have wrongly ACCEPTED.
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidMalformedFqi,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = "org.example/test/Shaker/v1",
                     .errorMessage = "m",
                     .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
                 }),
                 std::invalid_argument);

    // The inverse: the case that helper would have wrongly REJECTED, pinning
    // why the local pattern exists. Run on a worker thread since a valid
    // raise blocks until resolved.
    std::optional<RecoveryChoice> result;
    std::exception_ptr raised;
    std::thread worker{[&] {
        try {
            result = gate.raiseAndWait({
                .commandExecutionUuid = kUuidMalformedFqi,
                .errorIdentifier = kErrorIdentifier,
                .commandIdentifier = "org.example/test/Shaker/v1/Command/ShakeForTime",
                .errorMessage = "m",
                .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
            });
        } catch (...) {
            raised = std::current_exception();
        }
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.abort(kUuidMalformedFqi);
    worker.join();
    EXPECT_FALSE(raised);
}

TEST(RecoverableErrorGate, ErrorIdentifierOverMaxLengthThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    // FDL :174-176.
    const std::string overMaxLengthErrorId(256, 'x');
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidValidationProbe,
                     .errorIdentifier = overMaxLengthErrorId,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
                 }),
                 std::invalid_argument);
}

TEST(RecoverableErrorGate, NegativeAutomaticSelectionTimeoutThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    // FDL :339-340 MinimalInclusive 0; std::chrono::seconds is signed, so
    // this is reachable.
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidValidationProbe,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {{"retry", true, std::chrono::seconds{-1}}},
                 }),
                 std::invalid_argument);
}

TEST(RecoverableErrorGate, AutomaticSelectionTimeoutOnNonDefaultOptionThrows) {
    ObservablePropertyManager propertyManager;
    RecoverableErrorGate gate{propertyManager};

    // The wire carries one timeout, taken from the flagged option only
    // (ErrorRecoveryServiceImpl.cc fillRecoverableErrorsResponse), so a
    // nonzero timeout on "retry" here would be silently dropped rather than
    // rejected.
    EXPECT_THROW(gate.raiseAndWait({
                     .commandExecutionUuid = kUuidValidationProbe,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = kCommandIdentifier,
                     .errorMessage = "m",
                     .continuationOptions = {
                         {"retry", false, std::chrono::seconds{5}},
                         {"skip", true, std::chrono::seconds{0}},
                     },
                 }),
                 std::invalid_argument);
}

// S45: Part A p87 requires FQI VALUES to be compared without regard to case,
// so kCommandIdentifierFqiPattern folds the value before matching --
// mixed-case Category/Identifier/Command segments are now accepted.
TEST(RecoverableErrorGate, MixedCaseCommandIdentifierAccepted) {
    ObservablePropertyManager propertyManager;
    // The default timeout (0) blocks until resolved (FDL :139); use a finite
    // one so this test terminates once the now-accepted raise is admitted.
    RecoverableErrorGate gate{propertyManager, std::chrono::seconds{1}};

    // All-upper Category, Identifier and Command name fold to a valid
    // six-segment CommandIdentifier; the raise is admitted (no throw) and
    // then times out unresolved, rather than being rejected as malformed.
    std::optional<RecoveryChoice> result;
    EXPECT_NO_THROW(result = gate.raiseAndWait({
                     .commandExecutionUuid = kUuidCategoryCase,
                     .errorIdentifier = kErrorIdentifier,
                     .commandIdentifier = "ORG.EXAMPLE/Test/Shaker/V1/COMMAND/ShakeForTime",
                     .errorMessage = "m",
                     .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
                 }));
    EXPECT_FALSE(result.has_value());
}
