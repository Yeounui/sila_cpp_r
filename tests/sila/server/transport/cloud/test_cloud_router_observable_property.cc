// End-to-end tests for CloudEnvelopeRouter::route()'s kObservablePropertySubscription
// branch: the one path that owns a real ObservablePropertyManager subscription
// and a background pump thread, instead of a one-shot dispatchTo(). Before this
// file, registerObservableProperty was never called in any test, so this whole
// branch — subscribe, the auth gate, the pump loop, and cancellation via
// onCancellation — had zero coverage (audit finding 2.2m).
//
// Batch B1 adds coverage for the duplicate-requestUUID guard (2.2n) and the
// concurrent-pump cap (3.2q): both now live in the shared
// CloudEnvelopeRouter::startPump() used by all four long-running envelope
// kinds, not only this one, so the cap tests below also exercise an
// ObservableCommandExecutionInfoSubscription pump.
#include "CloudRouterTestHarness.h"

#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/property/ObservablePropertyManager.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/server/transport/cloud/ActiveCallRegistry.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>

#include <gtest/gtest.h>

#include <any>
#include <chrono>
#include <string>
#include <thread>

namespace {

namespace cloud = sila2::org::silastandard;
using cloud_test::CloudRouterFixture;

const std::string kObservableFqi = "org.test/Feature/ObservableProperty/v1";
const std::string kPropertyId = "TestObservableProperty";

// Every publish() in this file enqueues a std::string, so every subscription
// here shares this serializer: any_cast<std::string> is the concrete failure
// CloudEnvelopeRouter.cc's pump-loop catch(std::exception) is documented to
// guard against (a wrong-typed value throws std::bad_any_cast).
std::string serializeString(const std::any& value) {
    return std::any_cast<std::string>(value);
}

class CloudRouterObservableProperty : public CloudRouterFixture {
protected:
    sila2::FeatureRegistry registry_;
    sila2::ObservablePropertyManager manager_;
};

// ---------------------------------------------------------------------------
// Positive (True) paths
// ---------------------------------------------------------------------------

TEST_F(CloudRouterObservableProperty, SubscribeThenPublishDeliversValueThroughWriter) {
    sila2::CloudEnvelopeRouter router{registry_};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-obs-1");
    sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kObservableFqi);
    router.route(sub, *writer_, writer_, calls_);

    manager_.publish(kPropertyId, std::string{"value-1"});

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-obs-1");
    ASSERT_TRUE(resp.has_observablepropertyvalue());
    EXPECT_EQ(resp.observablepropertyvalue().value(), "value-1");
}

TEST_F(CloudRouterObservableProperty, SubscribeThenMultiplePublishesDeliversAllInOrder) {
    sila2::CloudEnvelopeRouter router{registry_};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-obs-2");
    sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kObservableFqi);
    router.route(sub, *writer_, writer_, calls_);

    manager_.publish(kPropertyId, std::string{"v1"});
    manager_.publish(kPropertyId, std::string{"v2"});
    manager_.publish(kPropertyId, std::string{"v3"});

    // The queue is FIFO (Subscription::waitForNext pops from the front), so
    // the pump must write them back in the order they were published.
    for (const std::string& expected : {"v1", "v2", "v3"}) {
        auto resp = popResponse();
        EXPECT_EQ(resp.requestuuid(), "req-obs-2");
        ASSERT_TRUE(resp.has_observablepropertyvalue());
        EXPECT_EQ(resp.observablepropertyvalue().value(), expected);
    }
}

TEST_F(CloudRouterObservableProperty, CancelStopsFurtherPublishesFromBeingDelivered) {
    sila2::CloudEnvelopeRouter router{registry_};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-obs-3");
    sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kObservableFqi);
    router.route(sub, *writer_, writer_, calls_);

    // Confirm the subscription is actually live before cancelling it.
    manager_.publish(kPropertyId, std::string{"before-cancel"});
    auto resp = popResponse();
    ASSERT_TRUE(resp.has_observablepropertyvalue());
    EXPECT_EQ(resp.observablepropertyvalue().value(), "before-cancel");

    cloud::SiLAClientMessage cancel;
    cancel.set_requestuuid("req-obs-3");
    cancel.mutable_cancelobservablepropertysubscription();
    router.route(cancel, *writer_, writer_, calls_);

    // route() for the cancel message runs requestCancellation() synchronously
    // (ActiveCallRegistry::cancel), which fires onCancellation ->
    // ObservablePropertyManager::unsubscribe() before this line, so the
    // publish below has nothing left to enqueue to.
    manager_.publish(kPropertyId, std::string{"after-cancel"});
    EXPECT_TRUE(noResponse());
    EXPECT_EQ(manager_.subscriberCount(kPropertyId), 0u);
}

// ---------------------------------------------------------------------------
// Negative (False) paths
// ---------------------------------------------------------------------------

// CAUGHT: fqi is not in observableProps_ (only kObservableFqi was registered
// there), so route() falls back to a one-shot dispatchTo() over
// propertyHandlers_ instead of opening a subscription (CloudEnvelopeRouter.cc
// lines 429-434).
TEST_F(CloudRouterObservableProperty, SubscribeToUnknownFqiFallsBackToPropertyHandler) {
    sila2::CloudEnvelopeRouter router{registry_};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);
    const std::string oneShotFqi = "org.test/Feature/UnregisteredObservable/v1";
    router.registerPropertyHandler(oneShotFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_observablepropertyvalue()->set_value("one-shot-value");
            w.write(resp);
        });

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-obs-4");
    sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(oneShotFqi);
    router.route(sub, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-obs-4");
    ASSERT_TRUE(resp.has_observablepropertyvalue());
    EXPECT_EQ(resp.observablepropertyvalue().value(), "one-shot-value");

    // No pump was ever spawned for this fqi, so the manager's subscriber set
    // for the registered observable property is untouched.
    EXPECT_EQ(manager_.subscriberCount(kPropertyId), 0u);
}

// CAUGHT: the auth gate at CloudEnvelopeRouter.cc line 444 runs before
// entry.mgr->subscribe(), so a rejected subscription never touches the
// ObservablePropertyManager at all (subscriberCount stays 0).
TEST_F(CloudRouterObservableProperty, SubscribeRejectedByAuthNeverSubscribesToManager) {
    sila2::auth::AuthTokenStore tokenStore;
    auto isProtected = [](const std::string&) { return true; };
    sila2::auth::AuthorizationInterceptor authInterceptor{tokenStore, isProtected};
    sila2::InterceptorChain chain;
    chain.auth = &authInterceptor;
    sila2::CloudEnvelopeRouter router{registry_, &chain};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-obs-5");
    // No "access-token" metadata is set, so AuthorizationInterceptor rejects.
    sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kObservableFqi);
    router.route(sub, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-obs-5");
    ASSERT_TRUE(resp.has_propertyerror());
    ASSERT_TRUE(resp.propertyerror().has_frameworkerror());
    EXPECT_EQ(resp.propertyerror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
    EXPECT_EQ(manager_.subscriberCount(kPropertyId), 0u);
}

// UNCAUGHT: cancelling a requestUUID nothing ever subscribed under is a
// silent no-op (ActiveCallRegistry::cancel's find() returns nullptr, so
// requestCancellation() is simply never called) — no crash, no response,
// no validation that the UUID was ever real.
TEST_F(CloudRouterObservableProperty, CancelWithUnknownRequestUuidDoesNotCrash) {
    sila2::CloudEnvelopeRouter router{registry_};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);

    cloud::SiLAClientMessage cancel;
    cancel.set_requestuuid("never-subscribed-uuid");
    cancel.mutable_cancelobservablepropertysubscription();
    router.route(cancel, *writer_, writer_, calls_);

    EXPECT_TRUE(noResponse());
}

// UNCAUGHT until this batch (S2): a pump body throwing (here, serializeString's
// any_cast<std::string> hitting a wrong-typed publish) used to be reported as a
// FrameworkError -- the deleted `Invalid` sentinel falling through to proto's
// default 0, COMMAND_EXECUTION_NOT_ACCEPTED. On direct gRPC the identical
// escape becomes an UndefinedExecutionError (ErrorTransmitInterceptor.h:28-32),
// so the two transports disagreed about what kind of failure the client saw.
// This is the one test in this batch that would have caught that defect.
TEST_F(CloudRouterObservableProperty, PumpBodyThrowIsReportedAsUndefinedExecutionError) {
    sila2::CloudEnvelopeRouter router{registry_};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-obs-throw");
    sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kObservableFqi);
    router.route(sub, *writer_, writer_, calls_);

    // Wrong-typed publish: serializeString's any_cast<std::string> throws
    // std::bad_any_cast inside the pump body.
    manager_.publish(kPropertyId, 42);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-obs-throw");
    ASSERT_TRUE(resp.has_propertyerror());
    EXPECT_TRUE(resp.propertyerror().has_undefinedexecutionerror());
    EXPECT_FALSE(resp.propertyerror().has_frameworkerror());
    EXPECT_NE(resp.propertyerror().undefinedexecutionerror().message().find("bad any_cast"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// Admission control (audit 2.2n / 3.2q) — both live in the shared
// CloudEnvelopeRouter::startPump(), so the property branch is what these
// tests drive, but the guarantees apply to all four pump kinds.
// ---------------------------------------------------------------------------

// CAUGHT (2.2n): a second subscription under a requestUUID that already has a
// live pump is rejected, and the FIRST pump keeps running and stays reachable
// by Cancel. Without the rejection undoing the second subscribe(), the first
// pump would silently lose its requestUUID slot in ActiveCallRegistry and
// become uncancellable — an orphaned, uncancellable subscription.
TEST_F(CloudRouterObservableProperty, DuplicateRequestUuidSubscriptionIsRejectedAndFirstPumpSurvives) {
    sila2::CloudEnvelopeRouter router{registry_};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-dup");
    sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kObservableFqi);
    router.route(sub, *writer_, writer_, calls_);
    router.route(sub, *writer_, writer_, calls_);  // same requestUUID

    auto rejected = popResponse();
    ASSERT_TRUE(rejected.has_propertyerror());
    ASSERT_TRUE(rejected.propertyerror().has_frameworkerror());
    EXPECT_EQ(rejected.propertyerror().frameworkerror().message(),
              "requestUUID already has a live subscription: req-dup");
    // The rejected subscribe() must be undone, or the manager keeps a
    // subscriber nothing will ever drain (the §2.1h sibling leak). This check
    // runs right after the second route() returns rather than polling: the
    // rejection path calls ctx->requestCancellation() synchronously on the
    // calling thread, which fires the unsubscribe before route() returns.
    EXPECT_EQ(manager_.subscriberCount(kPropertyId), 1u);

    // The survivor is the FIRST pump, and it is still reachable by Cancel.
    manager_.publish(kPropertyId, std::string{"after-reject"});
    auto value = popResponse();
    ASSERT_TRUE(value.has_observablepropertyvalue());
    EXPECT_EQ(value.observablepropertyvalue().value(), "after-reject");

    cloud::SiLAClientMessage cancel;
    cancel.set_requestuuid("req-dup");
    cancel.mutable_cancelobservablepropertysubscription();
    router.route(cancel, *writer_, writer_, calls_);
    EXPECT_EQ(manager_.subscriberCount(kPropertyId), 0u);
}

// ---------------------------------------------------------------------------
// 2.2o: the unary sibling of the DuplicateRequestUuidSubscriptionIsRejected...
// case above — the same guard, reached from dispatchTo instead of startPump.
// ---------------------------------------------------------------------------

// CAUGHT (2.2o): a unary call reusing a requestUUID that already has a live
// pump is rejected, and the pump stays reachable by Cancel. Pre-fix, the
// unary call's CallRegistryGuard unconditionally erased the pump's registry
// entry on return, so Cancel found nothing and the subscription streamed on
// with no way to stop it.
TEST_F(CloudRouterObservableProperty, UnaryCallReusingLivePumpRequestUuidIsRejectedAndPumpStaysCancellable) {
    sila2::CloudEnvelopeRouter router{registry_};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);
    const std::string kUnaryFqi = "org.test/Feature/ObservableProperty/v1/Property/UnaryRead";
    router.registerPropertyHandler(kUnaryFqi,
        [](const std::string&, sila2::CallContext&, sila2::StreamWriteSerializer& w,
           const std::string& uuid) {
            cloud::SiLAServerMessage m;
            m.set_requestuuid(uuid);
            m.mutable_unobservablepropertyvalue()->set_value("unary");
            w.write(m);
        });

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-dup");
    sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kObservableFqi);
    router.route(sub, *writer_, writer_, calls_);

    // Confirm the pump is actually running before the unary call collides.
    manager_.publish(kPropertyId, std::string{"live"});
    auto liveResp = popResponse();
    ASSERT_TRUE(liveResp.has_observablepropertyvalue());
    EXPECT_EQ(liveResp.observablepropertyvalue().value(), "live");

    cloud::SiLAClientMessage unary;
    unary.set_requestuuid("req-dup");
    unary.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(kUnaryFqi);
    router.route(unary, *writer_, writer_, calls_);

    auto rejected = popResponse();
    ASSERT_TRUE(rejected.has_propertyerror());
    ASSERT_TRUE(rejected.propertyerror().has_frameworkerror());
    EXPECT_EQ(rejected.propertyerror().frameworkerror().message(),
              "requestUUID already has a live subscription: req-dup");

    // The pump is still there and still delivering.
    manager_.publish(kPropertyId, std::string{"still-here"});
    auto stillLive = popResponse();
    ASSERT_TRUE(stillLive.has_observablepropertyvalue());
    EXPECT_EQ(stillLive.observablepropertyvalue().value(), "still-here");

    cloud::SiLAClientMessage cancel;
    cancel.set_requestuuid("req-dup");
    cancel.mutable_cancelobservablepropertysubscription();
    router.route(cancel, *writer_, writer_, calls_);
    // THE regression: pre-fix, the unary call's CallRegistryGuard erased the
    // pump's registry entry, so Cancel's find() found nothing and the
    // subscription streamed on uncancellable.
    EXPECT_EQ(manager_.subscriberCount(kPropertyId), 0u);
}

// CAUGHT (2.2o): sequential reuse after completion stays legal —
// CallRegistryGuard removes synchronously on return, and the guard reaps
// before it looks, so a finished-but-unreaped pump never poisons a later
// unrelated unary call under the same requestUUID.
TEST_F(CloudRouterObservableProperty, UnaryRequestUuidReuseAfterCompletionIsAccepted) {
    sila2::CloudEnvelopeRouter router{registry_};
    const std::string kUnaryFqi = "org.test/Feature/ObservableProperty/v1/Property/UnaryRead";
    router.registerPropertyHandler(kUnaryFqi,
        [](const std::string&, sila2::CallContext&, sila2::StreamWriteSerializer& w,
           const std::string& uuid) {
            cloud::SiLAServerMessage m;
            m.set_requestuuid(uuid);
            m.mutable_unobservablepropertyvalue()->set_value("unary");
            w.write(m);
        });

    for (int attempt = 0; attempt < 2; ++attempt) {
        cloud::SiLAClientMessage unary;
        unary.set_requestuuid("req-seq");
        unary.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(kUnaryFqi);
        router.route(unary, *writer_, writer_, calls_);

        auto resp = popResponse();
        ASSERT_TRUE(resp.has_unobservablepropertyvalue());
        EXPECT_EQ(resp.unobservablepropertyvalue().value(), "unary");
        EXPECT_FALSE(resp.has_propertyerror());
    }
}

// CAUGHT (3.2q): beyond the configured cap, a subscription is rejected with a
// FrameworkError instead of spawning an unbounded pump. Observed through the
// manager's subscriber count, not by introspecting threads.
TEST_F(CloudRouterObservableProperty, SubscriptionFloodIsBoundedByPumpCap) {
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {},
                                      std::chrono::seconds{0}, /*maxConcurrentSubscriptions=*/2};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);
    auto subscribe = [&](const std::string& reqUuid) {
        cloud::SiLAClientMessage sub;
        sub.set_requestuuid(reqUuid);
        sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kObservableFqi);
        router.route(sub, *writer_, writer_, calls_);
    };
    subscribe("req-cap-1");
    subscribe("req-cap-2");
    subscribe("req-cap-3");

    auto rejected = popResponse();
    ASSERT_TRUE(rejected.has_propertyerror());
    ASSERT_TRUE(rejected.propertyerror().has_frameworkerror());
    EXPECT_EQ(rejected.propertyerror().frameworkerror().message(),
              "too many concurrent cloud subscriptions (max 2)");
    // A rejected subscription unsubscribes itself, so exactly the admitted
    // pumps remain.
    EXPECT_EQ(manager_.subscriberCount(kPropertyId), 2u);

    // The cap bounds LIVE pumps, not lifetime totals: cancel one and a new
    // subscription is admitted once the freed pump has been reaped. Reaping
    // happens at the next spawn, so poll rather than assume it already ran.
    cloud::SiLAClientMessage cancel;
    cancel.set_requestuuid("req-cap-1");
    cancel.mutable_cancelobservablepropertysubscription();
    router.route(cancel, *writer_, writer_, calls_);

    bool admitted = false;
    for (int attempt = 0; attempt < 40 && !admitted; ++attempt) {
        subscribe("req-cap-4");
        // A rejection writes an envelope; an admission writes nothing.
        admitted = noResponse(std::chrono::milliseconds{50});
        if (!admitted) {
            popResponse();  // drain the rejection
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }
    EXPECT_TRUE(admitted) << "a cancelled subscription never freed its cap slot";
    EXPECT_EQ(manager_.subscriberCount(kPropertyId), 2u);
}

// CAUGHT (3.2q): the cap is shared across pump kinds — an
// ExecutionInfoSubscription pump must be refused once the cap is already
// spent by property pumps, proving startPump's cap is not property-only.
TEST_F(CloudRouterObservableProperty, ExecutionInfoSubscriptionSharesThePropertyPumpCap) {
    // Declared locally (not manager_, the fixture's ObservablePropertyManager)
    // to avoid shadowing while still exercising the command-side pump kind.
    sila2::ObservableCommandManager manager;
    sila2::CloudEnvelopeRouter router{registry_, nullptr, nullptr, {&manager},
                                      std::chrono::seconds{0}, /*maxConcurrentSubscriptions=*/2};
    router.registerObservableProperty(kObservableFqi, kPropertyId, &manager_, serializeString);

    auto subscribe = [&](const std::string& reqUuid) {
        cloud::SiLAClientMessage sub;
        sub.set_requestuuid(reqUuid);
        sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kObservableFqi);
        router.route(sub, *writer_, writer_, calls_);
    };
    subscribe("req-b3-1");
    subscribe("req-b3-2");

    auto exec = manager.addCommand(std::chrono::seconds{300});
    exec->start();

    cloud::SiLAClientMessage info;
    info.set_requestuuid("req-b3-info");
    info.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(exec->uuid());
    router.route(info, *writer_, writer_, calls_);

    auto rejected = popResponse();
    ASSERT_TRUE(rejected.has_commanderror());
    ASSERT_TRUE(rejected.commanderror().has_frameworkerror());
    EXPECT_EQ(rejected.commanderror().frameworkerror().message(),
              "too many concurrent cloud subscriptions (max 2)");
}

}  // namespace
