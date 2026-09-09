// End-to-end tests for CloudEnvelopeRouter::route()'s Observable Command
// paths: ExecutionInfoSubscription, IntermediateResponseSubscription, and
// GetResponse. Covers the observableCommands_ == nullptr gate, unknown UUID,
// missing executionFQI registration, and missing handler paths.
//
// Batch B1 moved all three of these paths off the receive-loop thread and
// onto CloudEnvelopeRouter::startPump()'s pump threads (audit 1.2g/1.2h), so
// this file also covers: ExecutionInfoSubscription now streams every
// transition to the terminal state instead of a one-shot snapshot; a
// blocking _Result handler no longer stalls other cloud dispatch on the same
// stream; Cancel reaches a handler that is still running, on both the
// _Intermediate and ExecutionInfoSubscription paths; and a duplicate
// requestUUID is rejected rather than silently orphaning the first pump.
#include "CloudRouterTestHarness.h"

#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace {

namespace cloud = sila2::org::silastandard;
using cloud_test::CloudRouterFixture;

// Registers both suffix handlers ("_Intermediate", "_Result") for fqi on
// router, each echoing a distinguishable literal so tests can tell which
// handler fired.
void registerEchoHandlers(sila2::CloudEnvelopeRouter& router, const std::string& fqi) {
    router.registerCommandHandler(fqi + "_Intermediate",
        [](const std::string&, sila2::CallContext&, sila2::StreamWriteSerializer& w,
           const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_observablecommandintermediateresponse()->set_response("intermediate-data");
            w.write(resp);
        });
    router.registerCommandHandler(fqi + "_Result",
        [](const std::string&, sila2::CallContext&, sila2::StreamWriteSerializer& w,
           const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_observablecommandresponse()->set_response("result-data");
            w.write(resp);
        });
}

class CloudRouterObservable : public CloudRouterFixture {
protected:
    sila2::FeatureRegistry registry_;
    sila2::ObservableCommandManager manager_;
};

// ---------------------------------------------------------------------------
// Positive (True) paths
// ---------------------------------------------------------------------------

// Renamed from ExecutionInfoSubscriptionReturnsRunningState (audit 1.2h): the
// old name pinned a one-shot snapshot as the whole contract, which is exactly
// what let the router write once and stop instead of streaming. It does not
// *fail* pre-fix — it under-specifies — so this flip asserts the transitions
// a spec-compliant client can never observe today: a progress change, then
// the terminal state, then the pump exiting instead of idling against the cap.
TEST_F(CloudRouterObservable, ExecutionInfoSubscriptionStreamsUntilTerminalState) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    std::string uuid = exec->uuid();
    exec->start();
    exec->setProgress(0.5, std::chrono::seconds{30});

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-1");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    EXPECT_EQ(resp.requestuuid(), "req-1");
    ASSERT_TRUE(resp.has_observablecommandexecutioninfo());
    const auto& info = resp.observablecommandexecutioninfo();
    EXPECT_EQ(info.commandexecutionuuid().value(), uuid);
    EXPECT_EQ(info.executioninfo().commandstatus(), cloud::ExecutionInfo::running);
    EXPECT_DOUBLE_EQ(info.executioninfo().progressinfo().value(), 0.5);
    EXPECT_EQ(info.executioninfo().estimatedremainingtime().seconds(), 30);

    exec->setProgress(0.9, std::chrono::seconds{5});
    auto progressed = popResponse();
    ASSERT_TRUE(progressed.has_observablecommandexecutioninfo());
    EXPECT_DOUBLE_EQ(
        progressed.observablecommandexecutioninfo().executioninfo().progressinfo().value(), 0.9);

    exec->finish();
    auto terminal = popResponse();
    ASSERT_TRUE(terminal.has_observablecommandexecutioninfo());
    EXPECT_EQ(terminal.observablecommandexecutioninfo().executioninfo().commandstatus(),
              cloud::ExecutionInfo::finishedSuccessfully);
    // The pump exits at the terminal state instead of idling against the cap.
    // 400ms exceeds the 200ms poll interval, so a pump that failed to exit
    // would still be alive to observe this window as silence only by luck —
    // this margin makes that false pass exceedingly unlikely.
    EXPECT_TRUE(noResponse(std::chrono::milliseconds{400}));
}

// The handler now runs on its own pump thread (audit 1.2g) instead of inline
// on the receive loop; popResponse()'s 2s timeout covers the thread hop.
TEST_F(CloudRouterObservable, IntermediateResponseSubscriptionDispatchesToIntermediateHandler) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    std::string uuid = exec->uuid();
    exec->start();
    router.registerExecutionFQI(uuid, "org.test/Feature/Command/v1");
    registerEchoHandlers(router, "org.test/Feature/Command/v1");

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-2");
    msg.mutable_observablecommandintermediateresponsesubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    EXPECT_EQ(resp.requestuuid(), "req-2");
    ASSERT_TRUE(resp.has_observablecommandintermediateresponse());
    EXPECT_EQ(resp.observablecommandintermediateresponse().response(), "intermediate-data");
}

// Same as above: dispatch is off the receive loop (audit 1.2g); popResponse()'s
// 2s timeout covers the thread hop.
TEST_F(CloudRouterObservable, GetResponseDispatchesToResultHandler) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    std::string uuid = exec->uuid();
    exec->start();
    exec->finish();
    router.registerExecutionFQI(uuid, "org.test/Feature/Command/v1");
    registerEchoHandlers(router, "org.test/Feature/Command/v1");

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-3");
    msg.mutable_observablecommandgetresponse()
       ->mutable_commandexecutionuuid()->set_value(uuid);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    EXPECT_EQ(resp.requestuuid(), "req-3");
    ASSERT_TRUE(resp.has_observablecommandresponse());
    EXPECT_EQ(resp.observablecommandresponse().response(), "result-data");
}

TEST_F(CloudRouterObservable, ExecutionInfoSubscriptionReturnsFinishedSuccessfullyState) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    std::string uuid = exec->uuid();
    exec->start();
    exec->finish();

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-4");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_observablecommandexecutioninfo());
    EXPECT_EQ(resp.observablecommandexecutioninfo().executioninfo().commandstatus(),
              cloud::ExecutionInfo::finishedSuccessfully);
    // A pump subscribed to an already-terminal execution writes the terminal
    // snapshot once and exits — it does not keep polling against the cap.
    EXPECT_TRUE(noResponse(std::chrono::milliseconds{400}));
}

// S17: a registered "<fqi>_Info" handler is preferred over the router's
// synthesized pump. estimatedRemainingTime==4242 is unreachable from
// ObservableCommandExecution::estimatedRemaining() on a fresh execution (0),
// so this is unambiguous proof the handler ran rather than the synthesizer.
TEST_F(CloudRouterObservable, InfoSubscriptionPrefersARegisteredInfoHandler) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    const std::string uuid = exec->uuid();
    exec->start();
    router.registerExecutionFQI(uuid, "org.test/Feature/Command/v1");
    router.registerCommandHandler("org.test/Feature/Command/v1_Info",
        [](const std::string&, sila2::CallContext&, sila2::StreamWriteSerializer& w,
           const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            auto* body = resp.mutable_observablecommandexecutioninfo();
            body->mutable_executioninfo()->mutable_estimatedremainingtime()->set_seconds(4242);
            w.write(resp);
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-info-handler");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);
    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_observablecommandexecutioninfo());
    EXPECT_EQ(resp.observablecommandexecutioninfo().executioninfo().estimatedremainingtime().seconds(),
              4242);
}

// REJECTION-shaped: no execution FQI is registered, so the fallback
// synthesizer runs (not a rejection of the subscription itself -- it still
// answers, proving the fallback stays reachable). Fails pre-fix, where
// buildExecutionInfo set progressInfo/estimatedRemainingTime unconditionally
// for every state, including Waiting.
TEST_F(CloudRouterObservable, FallbackSynthesizedInfoOmitsProgressWhileWaiting) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    const std::string uuid = exec->uuid();
    // Deliberately no exec->start() (state stays Waiting) and no
    // registerExecutionFQI (so the fallback pump is the only possible
    // producer).

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-waiting-fallback");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);
    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_observablecommandexecutioninfo());
    const auto& info = resp.observablecommandexecutioninfo().executioninfo();
    EXPECT_EQ(info.commandstatus(), cloud::ExecutionInfo::waiting);
    EXPECT_FALSE(info.has_progressinfo());
    EXPECT_FALSE(info.has_estimatedremainingtime());
}

// ---------------------------------------------------------------------------
// Negative (False) paths — all CAUGHT: the router explicitly detects and
// rejects each of these before reaching a handler.
// ---------------------------------------------------------------------------

TEST_F(CloudRouterObservable, ExecutionInfoSubscriptionWithoutObservableCommandsReturnsCommandError) {
    sila2::CloudEnvelopeRouter router{registry_};  // no ObservableCommandManager

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-5");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value("any-uuid");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().message(), "observable commands not configured");
}

TEST_F(CloudRouterObservable, IntermediateResponseSubscriptionWithoutObservableCommandsReturnsCommandError) {
    sila2::CloudEnvelopeRouter router{registry_};  // no ObservableCommandManager

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-6");
    msg.mutable_observablecommandintermediateresponsesubscription()
       ->mutable_commandexecutionuuid()->set_value("any-uuid");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().message(), "observable commands not configured");
}

TEST_F(CloudRouterObservable, GetResponseWithoutObservableCommandsReturnsCommandError) {
    sila2::CloudEnvelopeRouter router{registry_};  // no ObservableCommandManager

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-6b");
    msg.mutable_observablecommandgetresponse()
       ->mutable_commandexecutionuuid()->set_value("any-uuid");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().message(), "observable commands not configured");
}

TEST_F(CloudRouterObservable, ExecutionInfoSubscriptionWithUnknownUuidReturnsFrameworkError) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-7");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value("never-registered-uuid");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(),
              cloud::FrameworkError::INVALID_COMMAND_EXECUTION_UUID);
}

TEST_F(CloudRouterObservable, IntermediateResponseSubscriptionWithoutFqiRegistrationReturnsCommandError) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    std::string uuid = exec->uuid();
    exec->start();
    // Deliberately no router.registerExecutionFQI(uuid, ...) call.

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-8");
    msg.mutable_observablecommandintermediateresponsesubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().message(),
              "no command FQI registered for execution: " + uuid);
}

TEST_F(CloudRouterObservable, GetResponseWithRegisteredFqiButNoHandlerReturnsCommandError) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    std::string uuid = exec->uuid();
    exec->start();
    exec->finish();
    router.registerExecutionFQI(uuid, "org.test/Feature/Command/v1");
    // Deliberately no router.registerCommandHandler(..."_Result", ...) call.

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-9");
    msg.mutable_observablecommandgetresponse()
       ->mutable_commandexecutionuuid()->set_value(uuid);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().message(),
              "no handler registered for: org.test/Feature/Command/v1_Result");
}

// ---------------------------------------------------------------------------
// New regression tests (batch B1: audit 1.2g, 1.2h, 2.2n)
// ---------------------------------------------------------------------------

// Before 1.2g, this _Result handler ran inline on the receive loop, so
// route() never returned while it blocked — freezing every other cloud
// dispatch on the same stream, including the property read below.
TEST_F(CloudRouterObservable, BlockingResultHandlerDoesNotStallOtherCloudDispatch) {
    // Declared before router: the promise must outlive the router's
    // destructor, which joins the still-running handler thread.
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    const std::string uuid = exec->uuid();
    exec->start();
    exec->finish();
    router.registerExecutionFQI(uuid, "org.test/Feature/Command/v1");
    router.registerCommandHandler("org.test/Feature/Command/v1_Result",
        [releaseFuture](const std::string&, sila2::CallContext&,
                        sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            // Bounded so a regression fails the test instead of hanging ctest.
            releaseFuture.wait_for(std::chrono::seconds{5});
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_observablecommandresponse()->set_response("result-data");
            w.write(resp);
        });
    router.registerPropertyHandler("org.test/Feature/Property/v1",
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablepropertyvalue()->set_value("still-alive");
            w.write(resp);
        });

    cloud::SiLAClientMessage getResp;
    getResp.set_requestuuid("req-blocking");
    getResp.mutable_observablecommandgetresponse()->mutable_commandexecutionuuid()->set_value(uuid);
    auto routed = std::async(std::launch::async,
                             [&] { router.route(getResp, *writer_, writer_, calls_); });

    const bool returnedPromptly =
        routed.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    EXPECT_TRUE(returnedPromptly) << "route() blocked on the _Result handler";

    if (returnedPromptly) {
        // The handler stays blocked while this property read goes through:
        // that ordering is the regression under test, so the handler is
        // released only after the property response has been popped. If an
        // ASSERT or popResponse timeout exits early, ~router joins the
        // handler against its own 5s wait_for bound — a bounded stall, not
        // a hang.
        cloud::SiLAClientMessage read;
        read.set_requestuuid("req-other");
        read.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(
            "org.test/Feature/Property/v1");
        router.route(read, *writer_, writer_, calls_);
        auto other = popResponse();
        EXPECT_EQ(other.requestuuid(), "req-other");
        ASSERT_TRUE(other.has_unobservablepropertyvalue());
    }

    // On the regression path route() is still stuck inside the handler, so
    // the release must come before routed.wait() to unstick it promptly.
    release.set_value();
    routed.wait();
    if (returnedPromptly) {
        auto result = popResponse();
        EXPECT_EQ(result.requestuuid(), "req-blocking");
        ASSERT_TRUE(result.has_observablecommandresponse());
    }
}

// Before 1.2g, the receive loop ran this handler inline, so the Cancel
// envelope meant to stop it sat queued behind the very handler it exists to
// interrupt — a dispatch that could never unblock itself. The handler now
// runs on its own pump thread, so Cancel reaches ActiveCallRegistry::cancel()
// while the handler is still polling.
TEST_F(CloudRouterObservable, CancelIntermediateSubscriptionUnblocksItsOwnHandler) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    const std::string uuid = exec->uuid();
    exec->start();
    router.registerExecutionFQI(uuid, "org.test/Feature/Command/v1");

    auto sawCancel = std::make_shared<std::atomic<bool>>(false);
    router.registerCommandHandler("org.test/Feature/Command/v1_Intermediate",
        [sawCancel](const std::string&, sila2::CallContext& ctx,
                    sila2::StreamWriteSerializer&, const std::string&) {
            // 250 * 20ms = 5s bound: an unfixed build (Cancel queued behind
            // this handler on the receive loop) fails here instead of
            // hanging ctest forever.
            for (int i = 0; i < 250 && !ctx.isCancelled(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
            sawCancel->store(ctx.isCancelled());
        });

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-cancel-me");
    sub.mutable_observablecommandintermediateresponsesubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);
    router.route(sub, *writer_, writer_, calls_);

    cloud::SiLAClientMessage cancel;
    cancel.set_requestuuid("req-cancel-me");
    cancel.mutable_cancelobservablecommandintermediateresponsesubscription();
    router.route(cancel, *writer_, writer_, calls_);

    bool cancelled = false;
    for (int attempt = 0; attempt < 100 && !cancelled; ++attempt) {
        cancelled = sawCancel->load();
        if (!cancelled) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }
    EXPECT_TRUE(cancelled);
}

TEST_F(CloudRouterObservable, CancelExecutionInfoSubscriptionStopsOnlyTheStream) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    const std::string uuid = exec->uuid();
    exec->start();

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-cancel-info");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);
    router.route(msg, *writer_, writer_, calls_);
    popResponse();  // first snapshot

    EXPECT_FALSE(exec->isInterruptionRequested());

    cloud::SiLAClientMessage cancel;
    cancel.set_requestuuid("req-cancel-info");
    cancel.mutable_cancelobservablecommandexecutioninfosubscription();
    router.route(cancel, *writer_, writer_, calls_);

    exec->setProgress(0.7, std::chrono::seconds{10});
    EXPECT_TRUE(noResponse(std::chrono::milliseconds{400}));
    EXPECT_FALSE(exec->isInterruptionRequested());
}

// REJECTION: a pump that interrupted on every exit path (not just the
// cancelled one) would flip this. The terminal exit must never call
// requestInterruption(), or shutdown accounting would misreport a command
// that finished on its own as one the client cancelled.
TEST_F(CloudRouterObservable, TerminalExecutionInfoExitDoesNotInterrupt) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    std::string uuid = exec->uuid();
    exec->start();

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-terminal-no-interrupt");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);
    router.route(msg, *writer_, writer_, calls_);
    popResponse();  // first snapshot

    exec->finish();
    auto terminal = popResponse();
    ASSERT_TRUE(terminal.has_observablecommandexecutioninfo());
    EXPECT_EQ(terminal.observablecommandexecutioninfo().executioninfo().commandstatus(),
              cloud::ExecutionInfo::finishedSuccessfully);

    EXPECT_FALSE(exec->isInterruptionRequested());
}

// Before 1.2h/2.2n, a second ExecutionInfoSubscription under the same
// requestUUID silently orphaned the first pump instead of being rejected —
// the command-side sibling of the property branch's duplicate guard.
TEST_F(CloudRouterObservable, DuplicateRequestUuidExecutionInfoSubscriptionIsRejected) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    const std::string uuid = exec->uuid();
    exec->start();

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-dup");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);
    router.route(msg, *writer_, writer_, calls_);
    popResponse();  // first snapshot from the surviving pump

    router.route(msg, *writer_, writer_, calls_);  // same requestUUID again
    auto rejected = popResponse();
    ASSERT_TRUE(rejected.has_commanderror());
    ASSERT_TRUE(rejected.commanderror().has_frameworkerror());
    EXPECT_EQ(rejected.commanderror().frameworkerror().message(),
              "requestUUID already has a live subscription: req-dup");

    // The surviving pump is the FIRST one, and it is still reachable by Cancel.
    cloud::SiLAClientMessage cancel;
    cancel.set_requestuuid("req-dup");
    cancel.mutable_cancelobservablecommandexecutioninfosubscription();
    router.route(cancel, *writer_, writer_, calls_);

    exec->setProgress(0.6, std::chrono::seconds{20});
    EXPECT_TRUE(noResponse(std::chrono::milliseconds{400}));
}

// REJECTION: rejectDuplicateRequestUUID() never touches the surviving pump
// or its execution — it only writes an error envelope for the second
// requestUUID — so it must never reach requestInterruption() on exec.
TEST_F(CloudRouterObservable, RejectedDuplicateInfoSubscriptionDoesNotInterruptTheLivePump) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    const std::string uuid = exec->uuid();
    exec->start();

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-dup-no-interrupt");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);
    router.route(msg, *writer_, writer_, calls_);
    popResponse();  // first snapshot from the surviving pump

    router.route(msg, *writer_, writer_, calls_);  // same requestUUID again
    auto rejected = popResponse();
    ASSERT_TRUE(rejected.has_commanderror());

    EXPECT_FALSE(exec->isInterruptionRequested());
}

// Connection loss ends the subscription but not the execution.
TEST_F(CloudRouterObservable, ConnectionLostDoesNotInterruptTheExecution) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager_}};
    auto exec = manager_.addCommand(std::chrono::seconds{300});
    const std::string uuid = exec->uuid();
    exec->start();

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-connection-lost");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(uuid);
    router.route(msg, *writer_, writer_, calls_);
    popResponse();  // first snapshot

    calls_.cancelAll();  // production: CloudTransport::receiveLoop on Connection loss, no Cancel* envelope involved

    // Give a wrongly-interrupting pump every chance to flip the flag.
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    EXPECT_FALSE(exec->isInterruptionRequested());

    // The pump stopped sending once cancelled, even though the execution is
    // still alive and progressing -- no reconnect/re-subscribe happened here.
    exec->setProgress(0.7, std::chrono::seconds{10});
    EXPECT_TRUE(noResponse(std::chrono::milliseconds{400}));
}

}  // namespace
