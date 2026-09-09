// Tests for enforceMetadataPolicy (§S5): rule (a) "SiLA Client Metadata MUST
// NOT affect the SiLAService Feature" and rule (c) "an expected SiLA Client
// Metadata that is not received MUST issue an Invalid Metadata Error". The
// third normative rule -- unexpected/undeclared metadata MUST be ignored --
// is pinned by absence: no test here, and no branch in MetadataPolicy.h,
// rejects a key that no declaration names. UndeclaredMetadataOnAnOrdinaryCall
// IsIgnored below exists specifically to fail if that branch is ever added
// back (audit.md's original S5 row asked for exactly the branch the standard
// forbids).
#include <sila/server/metadata/MetadataPolicy.h>

#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/transport/InterceptorChain.h>

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace {

using sila2::InterceptorChain;
using sila2::enforceMetadataPolicy;
using sila2::error::FrameworkError;
using sila2::kAccessTokenMetadataFqi;

const std::string kFeatureFqi = "org.test/Thing/v1";
const std::string kCommandFqi = kFeatureFqi + "/Command/Do";
const std::string kThingMetadataFqi = kFeatureFqi + "/Metadata/Thing";
// Spelled literally, not via sila2::kSiLAServiceFqi/kSiLAServiceFeatureFqi:
// this pins the wire truth (the string a client actually names) rather than
// whichever header constant happens to hold it today.
const std::string kSiLAServiceCommand =
    "org.silastandard/core/SiLAService/v1/Command/SetServerName";
const std::string kSiLAServiceFqi = "org.silastandard/core/SiLAService/v1";

// Builds a hasMetadata predicate over a fixed received set, matching what
// each transport's real predicate answers (a header-key lookup on gRPC, an
// FQI scan on cloud) without needing either transport's plumbing here.
auto receivedSet(std::set<std::string> fqis) {
    return [fqis = std::move(fqis)](const std::string& metadataFqi) {
        return fqis.count(metadataFqi) > 0;
    };
}

// --- Positive (no throw) ----------------------------------------------------

TEST(EnforceMetadataPolicy, NoDeclarationsAdmitsAnOrdinaryCall) {
    InterceptorChain chain;
    EXPECT_NO_THROW(enforceMetadataPolicy(&chain, kCommandFqi, false, receivedSet({})));
}

TEST(EnforceMetadataPolicy, UndeclaredMetadataOnAnOrdinaryCallIsIgnored) {
    // Empty table, but the call carries metadata anyway: Part A's MUST-ignore
    // rule for unexpected metadata. There is deliberately no branch in
    // MetadataPolicy.h that inspects a received key no declaration names --
    // this test is the regression guard against ever adding one back.
    InterceptorChain chain;
    EXPECT_NO_THROW(enforceMetadataPolicy(&chain, kCommandFqi, /*anyMetadataReceived=*/true,
                                          receivedSet({})));
}

TEST(EnforceMetadataPolicy, DeclaredMetadataPresentOnAnAffectedCallAdmits) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kThingMetadataFqi] = {kFeatureFqi};
    EXPECT_NO_THROW(enforceMetadataPolicy(&chain, kCommandFqi, true,
                                          receivedSet({kThingMetadataFqi})));
}

TEST(EnforceMetadataPolicy, DeclaredMetadataAbsentOnAnUnaffectedCallAdmits) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kThingMetadataFqi] = {kCommandFqi + "/Other"};
    EXPECT_NO_THROW(enforceMetadataPolicy(&chain, kCommandFqi, false, receivedSet({})));
}

TEST(EnforceMetadataPolicy, SiblingFeatureVersionIsNotAffected) {
    // FqiMatch.h's segment-boundary rule: ".../v1" must not cover ".../v10".
    InterceptorChain chain;
    chain.metadataAffectedCalls[kThingMetadataFqi] = {kFeatureFqi};
    const std::string kSiblingVersionCommand = "org.test/Thing/v10/Command/Do";
    EXPECT_NO_THROW(enforceMetadataPolicy(&chain, kSiblingVersionCommand, false, receivedSet({})));
}

TEST(EnforceMetadataPolicy, SiLAServiceCallWithoutMetadataAdmits) {
    InterceptorChain chain;
    EXPECT_NO_THROW(enforceMetadataPolicy(&chain, kSiLAServiceCommand, false, receivedSet({})));
}

TEST(EnforceMetadataPolicy, SiLAServiceIsNeverAffectedEvenWhenDeclared) {
    // A Feature that (incorrectly) declares SiLAService as affected must not
    // make it unanswerable -- the gate's early return skips the loop entirely
    // for a SiLAService target.
    InterceptorChain chain;
    chain.metadataAffectedCalls[kThingMetadataFqi] = {kSiLAServiceFqi};
    EXPECT_NO_THROW(enforceMetadataPolicy(&chain, kSiLAServiceCommand, false, receivedSet({})));
}

TEST(EnforceMetadataPolicy, AccessTokenRowIsNotEnforcedByThisGate) {
    // AuthorizationInterceptor owns presence+validity of the AccessToken
    // metadata end to end; the gate must skip this row even when the caller
    // never presented it, or Login (which issues the token) would deadlock.
    InterceptorChain chain;
    chain.metadataAffectedCalls[kAccessTokenMetadataFqi] = {kFeatureFqi};
    EXPECT_NO_THROW(enforceMetadataPolicy(&chain, kCommandFqi, false, receivedSet({})));
}

// --- Rejection ---------------------------------------------------------------

TEST(EnforceMetadataPolicy, MetadataOnTheSiLAServiceFeatureFqiIsNoMetadataAllowed) {
    // chain=nullptr ON PURPOSE: rule (a) is unconditional, a property of the
    // SiLAService Feature rather than of the server's metadata configuration.
    EXPECT_THROW(
        {
            try {
                enforceMetadataPolicy(nullptr, kSiLAServiceFqi, true, receivedSet({}));
            } catch (const FrameworkError& e) {
                EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::NoMetadataAllowed);
                throw;
            }
        },
        FrameworkError);
}

TEST(EnforceMetadataPolicy, MetadataOnASiLAServiceCommandIsNoMetadataAllowed) {
    EXPECT_THROW(
        {
            try {
                enforceMetadataPolicy(nullptr, kSiLAServiceCommand, true, receivedSet({}));
            } catch (const FrameworkError& e) {
                EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::NoMetadataAllowed);
                throw;
            }
        },
        FrameworkError);
}

TEST(EnforceMetadataPolicy, MetadataOnASiLAServicePropertyIsNoMetadataAllowed) {
    const std::string kSiLAServiceProperty = kSiLAServiceFqi + "/Property/ServerName";
    EXPECT_THROW(
        {
            try {
                enforceMetadataPolicy(nullptr, kSiLAServiceProperty, true, receivedSet({}));
            } catch (const FrameworkError& e) {
                EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::NoMetadataAllowed);
                throw;
            }
        },
        FrameworkError);
}

TEST(EnforceMetadataPolicy, MissingDeclaredMetadataOnAFeatureLevelAffectedCallIsInvalidMetadata) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kThingMetadataFqi] = {kFeatureFqi};
    EXPECT_THROW(
        {
            try {
                enforceMetadataPolicy(&chain, kCommandFqi, false, receivedSet({}));
            } catch (const FrameworkError& e) {
                EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
                EXPECT_NE(std::string{e.what()}.find(kThingMetadataFqi), std::string::npos);
                throw;
            }
        },
        FrameworkError);
}

TEST(EnforceMetadataPolicy, MissingDeclaredMetadataOnACommandLevelAffectedCallIsInvalidMetadata) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kThingMetadataFqi] = {kCommandFqi};
    EXPECT_THROW(
        {
            try {
                enforceMetadataPolicy(&chain, kCommandFqi, false, receivedSet({}));
            } catch (const FrameworkError& e) {
                EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
                EXPECT_NE(std::string{e.what()}.find(kThingMetadataFqi), std::string::npos);
                throw;
            }
        },
        FrameworkError);
}

TEST(EnforceMetadataPolicy, UnrelatedMetadataDoesNotSatisfyADeclaration) {
    // anyMetadataReceived=true (the client sent SOMETHING) must not itself
    // satisfy a per-FQMI declaration -- hasMetadata() is checked per
    // metadataFqi, not "did the client send anything at all".
    InterceptorChain chain;
    chain.metadataAffectedCalls[kThingMetadataFqi] = {kFeatureFqi};
    EXPECT_THROW(
        {
            try {
                enforceMetadataPolicy(&chain, kCommandFqi, /*anyMetadataReceived=*/true,
                                      receivedSet({"org.test/Thing/v1/Metadata/SomethingElse"}));
            } catch (const FrameworkError& e) {
                EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
                throw;
            }
        },
        FrameworkError);
}

}  // namespace
