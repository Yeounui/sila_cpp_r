// Integration tests for CloudEnvelopeRouter::route()'s command/property
// dispatch, cancel, and metadata paths (architecture.md §3.9). Each test
// drives route() with a real SiLAClientMessage through a real gRPC stream
// (via CloudRouterFixture) and inspects the SiLAServerMessage(s) it writes.
#include "CloudRouterTestHarness.h"

#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/server/transport/cloud/ActiveCallRegistry.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>

#include "AuthorizationService.pb.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

namespace cloud = sila2::org::silastandard;
namespace authzproto = sila2::org::silastandard::core::authorizationservice::v1;

// Adapts the shared harness fixture to this file's test suite name.
class CloudRouterDispatch : public cloud_test::CloudRouterFixture {};

const std::string kCommandFqi = "org.test/Feature/Command/v1";
const std::string kPropertyFqi = "org.test/Feature/Property/v1";
const std::string kMetadataFqi = "org.test/Feature/Metadata/v1";

// Must match CloudEnvelopeRouter.cc's private kAccessTokenMetadataFqi — the
// wire key makeCloudCallContext() recognizes and unwraps into the
// "access-token" raw key AuthorizationInterceptor reads.
const std::string kAccessTokenMetadataFqi =
    "org.silastandard/core/AuthorizationService/v1/Metadata/AccessToken";

// --- Positive (True) paths ------------------------------------------------

TEST_F(CloudRouterDispatch, UnobservableCommandWithHandlerWritesResponse) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string& params, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablecommandresponse()->set_response(params);
            w.write(resp);
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-1");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid(kCommandFqi);
    exec->mutable_commandparameter()->set_parameters("hello");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-1");
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
    EXPECT_EQ(resp.unobservablecommandresponse().response(), "hello");
}

TEST_F(CloudRouterDispatch, UnobservablePropertyWithHandlerWritesResponse) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerPropertyHandler(kPropertyFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablepropertyvalue()->set_value("prop-value");
            w.write(resp);
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-2");
    msg.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(kPropertyFqi);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-2");
    ASSERT_TRUE(resp.has_unobservablepropertyvalue());
    EXPECT_EQ(resp.unobservablepropertyvalue().value(), "prop-value");
}

TEST_F(CloudRouterDispatch, ObservableCommandInitiationWithHandlerWritesConfirmation) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string& params, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_observablecommandconfirmation()
                ->mutable_commandconfirmation()
                ->mutable_commandexecutionuuid()->set_value(params);
            w.write(resp);
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-init-1");
    auto* init = msg.mutable_observablecommandinitiation();
    init->set_fullyqualifiedcommandid(kCommandFqi);
    init->mutable_commandparameter()->set_parameters("exec-uuid-1");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-init-1");
    ASSERT_TRUE(resp.has_observablecommandconfirmation());
    EXPECT_EQ(resp.observablecommandconfirmation().commandconfirmation()
                  .commandexecutionuuid().value(),
              "exec-uuid-1");
}

TEST_F(CloudRouterDispatch, ObservablePropertySubscriptionWithHandlerWritesValue) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerPropertyHandler(kPropertyFqi,
        [](const std::string&, sila2::CallContext& ctx,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_observablepropertyvalue()->set_value(
                ctx.metadata(kMetadataFqi).value_or("<missing>"));
            w.write(resp);
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-sub-1");
    auto* sub = msg.mutable_observablepropertysubscription();
    sub->set_fullyqualifiedpropertyid(kPropertyFqi);
    auto* md = sub->add_metadata();
    md->set_fullyqualifiedmetadataid(kMetadataFqi);
    md->set_value("sub-meta-value");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-sub-1");
    ASSERT_TRUE(resp.has_observablepropertyvalue());
    EXPECT_EQ(resp.observablepropertyvalue().value(), "sub-meta-value");
}

TEST_F(CloudRouterDispatch, MetadataFromEnvelopeReachesCallContext) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext& ctx,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablecommandresponse()->set_response(
                ctx.metadata(kMetadataFqi).value_or("<missing>"));
            w.write(resp);
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-3");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid(kCommandFqi);
    auto* md = exec->mutable_commandparameter()->add_metadata();
    md->set_fullyqualifiedmetadataid(kMetadataFqi);
    md->set_value("meta-value");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
    EXPECT_EQ(resp.unobservablecommandresponse().response(), "meta-value");
}

TEST_F(CloudRouterDispatch, CancelMarksActiveCallContextCancelled) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    auto ctx = std::make_shared<sila2::CallContext>();
    calls_.add("req-4", ctx);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-4");
    msg.mutable_cancelobservablecommandexecutioninfosubscription();

    router.route(msg, *writer_, writer_, calls_);

    EXPECT_TRUE(ctx->isCancelled());
    EXPECT_TRUE(noResponse());
}

TEST_F(CloudRouterDispatch, MetadataRequestForKnownFqiReturnsAffectedCalls) {
    // The router no longer owns its own table (registerMetadataAffectedCalls
    // is deleted) — kMetadataRequest answers from chain->metadataAffectedCalls,
    // the same table the admission gate reads.
    sila2::InterceptorChain chain;
    chain.metadataAffectedCalls[kMetadataFqi] = {kCommandFqi, kPropertyFqi};
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-5");
    msg.mutable_metadatarequest()->set_fullyqualifiedmetadataid(kMetadataFqi);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_getfcpaffectedbymetadataresponse());
    const auto& affected = resp.getfcpaffectedbymetadataresponse();
    ASSERT_EQ(affected.affectedcalls_size(), 2);
    EXPECT_EQ(affected.affectedcalls(0), kCommandFqi);
    EXPECT_EQ(affected.affectedcalls(1), kPropertyFqi);
}

// Also the chain==nullptr case (the router's default construction, used by
// most tests above): kMetadataRequest's `if (chain_)` guard answers empty
// instead of dereferencing a null chain.
TEST_F(CloudRouterDispatch, MetadataRequestForUnknownFqiReturnsEmptyList) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-6");
    msg.mutable_metadatarequest()->set_fullyqualifiedmetadataid("org.test/Feature/NoSuchMetadata/v1");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_getfcpaffectedbymetadataresponse());
    EXPECT_EQ(resp.getfcpaffectedbymetadataresponse().affectedcalls_size(), 0);
}

// --- Negative (False) paths -------------------------------------------------
// All five are CAUGHT: the router already detects and reports each case.

TEST_F(CloudRouterDispatch, CommandDispatchWithNoHandlerReturnsFrameworkError) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-7");
    msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(kCommandFqi);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-7");
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    // Pins the value as chosen (SiLAErrorSubtypes.h's deleted `Invalid`
    // sentinel used to reach this same wire value only by falling through to
    // proto's default 0) rather than inherited by accident.
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(),
              cloud::FrameworkError::COMMAND_EXECUTION_NOT_ACCEPTED);
}

TEST_F(CloudRouterDispatch, PropertyDispatchWithNoHandlerReturnsFrameworkError) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-8");
    msg.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(kPropertyFqi);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-8");
    ASSERT_TRUE(resp.has_propertyerror());
    ASSERT_TRUE(resp.propertyerror().has_frameworkerror());
    EXPECT_EQ(resp.propertyerror().frameworkerror().errortype(),
              cloud::FrameworkError::COMMAND_EXECUTION_NOT_ACCEPTED);
}

TEST_F(CloudRouterDispatch, CommandDispatchRejectedByAuthReturnsCommandError) {
    sila2::FeatureRegistry registry;
    sila2::auth::AuthTokenStore tokenStore;
    auto isProtected = [](const std::string&) { return true; };
    sila2::auth::AuthorizationInterceptor authInterceptor{tokenStore, isProtected};
    sila2::InterceptorChain chain;
    chain.auth = &authInterceptor;
    bool loggedAuthWarning = false;
    chain.logCallback = [&loggedAuthWarning](sila2::LogLevel level,
                                             std::string_view category,
                                             std::string_view message) {
        loggedAuthWarning = level == sila2::LogLevel::kWarning && category == "auth" &&
                            message == "access denied: " + kCommandFqi;
    };
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when auth rejects the call";
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-9");
    // No "access-token" metadata is set, so AuthorizationInterceptor rejects.
    msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(kCommandFqi);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-9");
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
    EXPECT_TRUE(loggedAuthWarning);
}

TEST_F(CloudRouterDispatch, PropertyDispatchRejectedByAuthReturnsPropertyError) {
    sila2::FeatureRegistry registry;
    sila2::auth::AuthTokenStore tokenStore;
    auto isProtected = [](const std::string&) { return true; };
    sila2::auth::AuthorizationInterceptor authInterceptor{tokenStore, isProtected};
    sila2::InterceptorChain chain;
    chain.auth = &authInterceptor;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerPropertyHandler(kPropertyFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when auth rejects the call";
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-10");
    // No "access-token" metadata is set, so AuthorizationInterceptor rejects.
    msg.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(kPropertyFqi);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-10");
    ASSERT_TRUE(resp.has_propertyerror());
    ASSERT_TRUE(resp.propertyerror().has_frameworkerror());
    EXPECT_EQ(resp.propertyerror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
}

TEST_F(CloudRouterDispatch, ObservableCommandInitiationWithNoHandlerReturnsFrameworkError) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-12");
    msg.mutable_observablecommandinitiation()->set_fullyqualifiedcommandid(kCommandFqi);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-12");
    ASSERT_TRUE(resp.has_commanderror());
    EXPECT_TRUE(resp.commanderror().has_frameworkerror());
}

TEST_F(CloudRouterDispatch, ObservablePropertySubscriptionWithNoHandlerReturnsFrameworkError) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-13");
    msg.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kPropertyFqi);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-13");
    ASSERT_TRUE(resp.has_propertyerror());
    EXPECT_TRUE(resp.propertyerror().has_frameworkerror());
}

TEST_F(CloudRouterDispatch, ObservableCommandInitiationRejectedByAuthReturnsCommandError) {
    sila2::FeatureRegistry registry;
    sila2::auth::AuthTokenStore tokenStore;
    auto isProtected = [](const std::string&) { return true; };
    sila2::auth::AuthorizationInterceptor authInterceptor{tokenStore, isProtected};
    sila2::InterceptorChain chain;
    chain.auth = &authInterceptor;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when auth rejects the call";
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-14");
    // No "access-token" metadata is set, so AuthorizationInterceptor rejects.
    msg.mutable_observablecommandinitiation()->set_fullyqualifiedcommandid(kCommandFqi);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-14");
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
}

TEST_F(CloudRouterDispatch, ObservablePropertySubscriptionRejectedByAuthReturnsPropertyError) {
    sila2::FeatureRegistry registry;
    sila2::auth::AuthTokenStore tokenStore;
    auto isProtected = [](const std::string&) { return true; };
    sila2::auth::AuthorizationInterceptor authInterceptor{tokenStore, isProtected};
    sila2::InterceptorChain chain;
    chain.auth = &authInterceptor;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerPropertyHandler(kPropertyFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when auth rejects the call";
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-15");
    // No "access-token" metadata is set, so AuthorizationInterceptor rejects.
    msg.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kPropertyFqi);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-15");
    ASSERT_TRUE(resp.has_propertyerror());
    ASSERT_TRUE(resp.propertyerror().has_frameworkerror());
    EXPECT_EQ(resp.propertyerror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
}

// Every other auth-rejection test above omits the "access-token" metadata
// entirely, so all of them take the FrameworkError/InvalidMetadata branch in
// AuthorizationInterceptor::intercept (:22-27). This test supplies a token
// the store never issued, so store_.validate() returns nullopt and the
// DefinedExecutionError branch (:35-37) fires instead — the only branch of
// the auth gate no cloud test exercised before this one.
TEST_F(CloudRouterDispatch, ProtectedFqiWithInvalidTokenReturnsInvalidAccessTokenDee) {
    sila2::FeatureRegistry registry;
    sila2::auth::AuthTokenStore tokenStore;
    auto isProtected = [](const std::string&) { return true; };
    sila2::auth::AuthorizationInterceptor authInterceptor{tokenStore, isProtected};
    sila2::InterceptorChain chain;
    chain.auth = &authInterceptor;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when auth rejects the call";
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-16");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid(kCommandFqi);
    // Wire format for AccessToken metadata is the serialized Metadata_AccessToken
    // message, not the bare token (CloudEnvelopeRouter.cc:13-15) — and the token
    // value itself is never issued by tokenStore, so validate() fails.
    authzproto::Metadata_AccessToken wrapper;
    wrapper.mutable_accesstoken()->set_value("token-the-store-never-issued");
    auto* md = exec->mutable_commandparameter()->add_metadata();
    md->set_fullyqualifiedmetadataid(kAccessTokenMetadataFqi);
    md->set_value(wrapper.SerializeAsString());

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-16");
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_definedexecutionerror());
    EXPECT_EQ(resp.commanderror().definedexecutionerror().erroridentifier(),
              "org.silastandard/core/AuthorizationService/v1/DefinedExecutionError/InvalidAccessToken");
}

// --- SiLA Client Metadata admission gate: observable-property-subscription
// seam (§S5) ---------------------------------------------------------------
// The kObservablePropertySubscription branch never funnels through
// dispatchTo when a codegen'd Subscribe_ handler (registerObservablePropertyHandler)
// or a manager entry is registered -- it builds its own CallContext and runs
// its own auth gate, so the metadata gate has to be called there too
// (CloudEnvelopeRouter.cc's route()). These tests exercise that branch
// directly by registering a stream handler, which the dispatchTo-fallback
// tests above (no observable registration at all) never reach.

TEST_F(CloudRouterDispatch, ObservablePropertySubscriptionWithNoDeclarationsSubscribes) {
    // Empty chain.metadataAffectedCalls: an ordinary (undeclared) metadata
    // entry on the subscription must be silently ignored (Part A MUST-ignore),
    // not rejected -- there is no undeclared-key branch anywhere in the gate.
    sila2::InterceptorChain chain;
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerObservablePropertyHandler(kPropertyFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_observablepropertyvalue()->set_value("pumped");
            w.write(resp);
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-md-1");
    auto* sub = msg.mutable_observablepropertysubscription();
    sub->set_fullyqualifiedpropertyid(kPropertyFqi);
    auto* md = sub->add_metadata();
    md->set_fullyqualifiedmetadataid("org.test/Feature/Metadata/Undeclared");
    md->set_value("anything");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_observablepropertyvalue());
    EXPECT_EQ(resp.observablepropertyvalue().value(), "pumped");
}

TEST_F(CloudRouterDispatch, ObservablePropertySubscriptionWithRequiredMetadataPresentSubscribes) {
    sila2::InterceptorChain chain;
    chain.metadataAffectedCalls[kMetadataFqi] = {kPropertyFqi};
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerObservablePropertyHandler(kPropertyFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_observablepropertyvalue()->set_value("pumped");
            w.write(resp);
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-md-2");
    auto* sub = msg.mutable_observablepropertysubscription();
    sub->set_fullyqualifiedpropertyid(kPropertyFqi);
    auto* md = sub->add_metadata();
    md->set_fullyqualifiedmetadataid(kMetadataFqi);
    md->set_value("meta-value");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_observablepropertyvalue());
    EXPECT_EQ(resp.observablepropertyvalue().value(), "pumped");
}

// dispatchTo's property arm (kUnobservablePropertyRead) -- Part A names
// "Reading an affected Property" separately from a subscription.
TEST_F(CloudRouterDispatch, UnobservablePropertyReadWithRequiredMetadataPresentSucceeds) {
    sila2::InterceptorChain chain;
    chain.metadataAffectedCalls[kMetadataFqi] = {kPropertyFqi};
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerPropertyHandler(kPropertyFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablepropertyvalue()->set_value("read-ok");
            w.write(resp);
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-md-3");
    auto* read = msg.mutable_unobservablepropertyread();
    read->set_fullyqualifiedpropertyid(kPropertyFqi);
    auto* md = read->add_metadata();
    md->set_fullyqualifiedmetadataid(kMetadataFqi);
    md->set_value("meta-value");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_unobservablepropertyvalue());
    EXPECT_EQ(resp.unobservablepropertyvalue().value(), "read-ok");
}

TEST_F(CloudRouterDispatch, ObservablePropertySubscriptionMissingRequiredMetadataIsInvalidMetadata) {
    sila2::InterceptorChain chain;
    chain.metadataAffectedCalls[kMetadataFqi] = {kPropertyFqi};
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerObservablePropertyHandler(kPropertyFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when the metadata gate rejects the call";
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-md-4");
    msg.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(kPropertyFqi);
    // No metadata attached at all.

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_propertyerror());
    ASSERT_TRUE(resp.propertyerror().has_frameworkerror());
    EXPECT_EQ(resp.propertyerror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
    EXPECT_NE(resp.propertyerror().frameworkerror().message().find(kMetadataFqi), std::string::npos);
}

TEST_F(CloudRouterDispatch, UnobservablePropertyReadMissingRequiredMetadataIsInvalidMetadata) {
    sila2::InterceptorChain chain;
    chain.metadataAffectedCalls[kMetadataFqi] = {kPropertyFqi};
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerPropertyHandler(kPropertyFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when the metadata gate rejects the call";
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-md-5");
    msg.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(kPropertyFqi);
    // No metadata attached at all.

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_propertyerror());
    ASSERT_TRUE(resp.propertyerror().has_frameworkerror());
    EXPECT_EQ(resp.propertyerror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
}

// Rule (a) reaching the seam that bypasses dispatchTo: a SiLAService property
// subscription carrying any metadata must be refused before the stream
// handler ever runs, matching dispatchTo's own SiLAService behavior.
TEST_F(CloudRouterDispatch, ObservablePropertySubscriptionOnSiLAServiceWithMetadataIsNoMetadataAllowed) {
    const std::string kSiLAServicePropertyFqi =
        "org.silastandard/core/SiLAService/v1/Property/ServerName";
    sila2::InterceptorChain chain;
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerObservablePropertyHandler(kSiLAServicePropertyFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when the metadata gate rejects the call";
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-md-6");
    auto* sub = msg.mutable_observablepropertysubscription();
    sub->set_fullyqualifiedpropertyid(kSiLAServicePropertyFqi);
    auto* md = sub->add_metadata();
    md->set_fullyqualifiedmetadataid(kMetadataFqi);
    md->set_value("anything");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_propertyerror());
    ASSERT_TRUE(resp.propertyerror().has_frameworkerror());
    EXPECT_EQ(resp.propertyerror().frameworkerror().errortype(),
              cloud::FrameworkError::NO_METADATA_ALLOWED);
}

TEST_F(CloudRouterDispatch, MessageNotSetWritesNoResponse) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-11");
    // No oneof field set: message_case() == MESSAGE_NOT_SET.

    router.route(msg, *writer_, writer_, calls_);

    EXPECT_TRUE(noResponse());
}

// ---------------------------------------------------------------------------
// S60: Part A p87 -- FQI comparison MUST ignore case. commandHandlers_ uses
// util::CaseInsensitiveLess (CloudEnvelopeRouter.h) so a case-variant wire FQI
// still finds the handler registered under the canonical-case FQI.
// ---------------------------------------------------------------------------

TEST_F(CloudRouterDispatch, CommandDispatchIgnoresFqiCase) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string& params, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablecommandresponse()->set_response(params);
            w.write(resp);
        });

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-12");
    auto* exec = msg.mutable_unobservablecommandexecution();
    // Uppercase variant of kCommandFqi -- the handler was registered under the
    // lowercase form above.
    exec->set_fullyqualifiedcommandid("ORG.TEST/FEATURE/COMMAND/V1");
    exec->mutable_commandparameter()->set_parameters("hello");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-12");
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
    EXPECT_EQ(resp.unobservablecommandresponse().response(), "hello");
}

TEST_F(CloudRouterDispatch, CommandDispatchWithUnknownFqiStillReturnsFrameworkError) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {});

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-13");
    // Genuinely unregistered FQI, not a case variant of kCommandFqi -- proves
    // case folding did not turn every unmatched lookup into a false hit.
    msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(
        "org.test/Feature/NoSuchCommand/v1");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-13");
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(),
              cloud::FrameworkError::COMMAND_EXECUTION_NOT_ACCEPTED);
}

}  // namespace
