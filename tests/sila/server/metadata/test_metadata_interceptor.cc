// Tests for MetadataExtractingInterceptor: "sila-" prefixed headers are
// copied into CallContext, everything else (wrong case, wrong prefix,
// empty input) is left out. Also covers the standard AccessToken header,
// which carries a serialized Metadata_AccessToken and must be unwrapped
// into the normalized "access-token" key rather than copied verbatim.
#include <sila/server/metadata/MetadataExtractingInterceptor.h>

#include <sila/client/MetadataInjector.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/transport/CallContext.h>

// Builds a valid/invalid wire value for the standard AccessToken header.
#include "AuthorizationService.pb.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace
{
using sila2::CallContext;
using sila2::MetadataExtractingInterceptor;

const std::string kAccessTokenMetadataFqi =
    "org.silastandard/core/AuthorizationService/v1/Metadata/AccessToken";
// Single derived constant: the dashed dialect this file used to also accept
// had no real peer (see MetadataHeaderKey.h), so it is now a rejection case
// instead of a second accepted key.
const std::string kAccessTokenStandardKey = sila2::metadataHeaderKey(kAccessTokenMetadataFqi);
const std::string kDotFoldedLegacyAccessTokenKey =
    "sila-org-silastandard-core-authorizationservice-v1-metadata-accesstoken-bin";

// Serializes a Metadata_AccessToken carrying tokenValue, matching what
// SilaClientBase actually puts on the wire for this header.
std::string serializedAccessTokenMetadata(const std::string& tokenValue) {
    sila2::org::silastandard::core::authorizationservice::v1::Metadata_AccessToken metadata;
    metadata.mutable_accesstoken()->set_value(tokenValue);
    return metadata.SerializeAsString();
}

TEST(MetadataExtractingInterceptor, SingleSilaHeaderIsExtracted) {
    std::multimap<std::string, std::string> headers{
        {"sila-client-id", "client-42"},
    };
    CallContext ctx;

    MetadataExtractingInterceptor::extract(headers, ctx);

    EXPECT_EQ(ctx.metadata("sila-client-id"), "client-42");
}

TEST(MetadataExtractingInterceptor, MultipleSilaHeadersAreAllExtracted) {
    std::multimap<std::string, std::string> headers{
        {"sila-client-id", "client-42"},
        {"sila-command-id", "cmd-1"},
        {"sila-metadata-x", "value-x"},
    };
    CallContext ctx;

    MetadataExtractingInterceptor::extract(headers, ctx);

    EXPECT_EQ(ctx.metadata("sila-client-id"), "client-42");
    EXPECT_EQ(ctx.metadata("sila-command-id"), "cmd-1");
    EXPECT_EQ(ctx.metadata("sila-metadata-x"), "value-x");
}

TEST(MetadataExtractingInterceptor, NonSilaHeadersAmongSilaHeadersAreIgnored) {
    std::multimap<std::string, std::string> headers{
        {"sila-client-id", "client-42"},
        {"authorization", "Bearer token"},
    };
    CallContext ctx;

    MetadataExtractingInterceptor::extract(headers, ctx);

    EXPECT_EQ(ctx.metadata("sila-client-id"), "client-42");
    EXPECT_FALSE(ctx.metadata("authorization").has_value());
}

TEST(MetadataExtractingInterceptor, EmptyHeadersLeavesContextMetadataEmpty) {
    std::multimap<std::string, std::string> headers;
    CallContext ctx;

    MetadataExtractingInterceptor::extract(headers, ctx);

    EXPECT_FALSE(ctx.metadata("sila-client-id").has_value());
}

TEST(MetadataExtractingInterceptor, WrongCaseOrPrefixHeadersAreNotExtracted) {
    // Prefix match is exact-case "sila-"; none of these qualify.
    std::multimap<std::string, std::string> headers{
        {"Sila-client-id", "a"},
        {"SILA-client-id", "b"},
        {"silaX-client-id", "c"},
    };
    CallContext ctx;

    MetadataExtractingInterceptor::extract(headers, ctx);

    EXPECT_FALSE(ctx.metadata("Sila-client-id").has_value());
    EXPECT_FALSE(ctx.metadata("SILA-client-id").has_value());
    EXPECT_FALSE(ctx.metadata("silaX-client-id").has_value());
}

TEST(MetadataExtractingInterceptor, BareSilaPrefixWithEmptySuffixIsExtracted) {
    std::multimap<std::string, std::string> headers{
        {"sila-", "value"},
    };
    CallContext ctx;

    MetadataExtractingInterceptor::extract(headers, ctx);

    EXPECT_EQ(ctx.metadata("sila-"), "value");
}

TEST(MetadataExtractingInterceptor, StandardAccessTokenHeaderIsUnwrappedToAccessTokenKey) {
    std::multimap<std::string, std::string> headers{
        {kAccessTokenStandardKey, serializedAccessTokenMetadata("secret-token")},
    };
    CallContext ctx;

    MetadataExtractingInterceptor::extract(headers, ctx);

    // The raw header is still kept under its own key (like any other
    // "sila-" header)...
    EXPECT_EQ(ctx.metadata(kAccessTokenStandardKey), serializedAccessTokenMetadata("secret-token"));
    // ...and the inner token string is what ends up under "access-token",
    // not the serialized bytes.
    EXPECT_EQ(ctx.metadata("access-token"), "secret-token");
}

TEST(MetadataExtractingInterceptor, ClientAndServerAgreeOnTheKey) {
    // Ties the two sides of the wire together so the derivation can never
    // drift apart again: this is the same constant MetadataInjector derives
    // client-side for the same FQI.
    EXPECT_EQ(sila2::MetadataInjector::headerKey(kAccessTokenMetadataFqi), kAccessTokenStandardKey);
}

TEST(MetadataExtractingInterceptor, DotFoldedLegacyKeyDoesNotYieldAnAccessToken) {
    // The dashed dialect had no real peer (see MetadataHeaderKey.h): even a
    // validly serialized Metadata_AccessToken under that key must not reach
    // the auth gate.
    std::multimap<std::string, std::string> headers{
        {kDotFoldedLegacyAccessTokenKey, serializedAccessTokenMetadata("secret-token")},
    };
    CallContext ctx;

    MetadataExtractingInterceptor::extract(headers, ctx);

    // The raw header is still kept under its own key (like any other
    // "sila-" header)...
    EXPECT_EQ(ctx.metadata(kDotFoldedLegacyAccessTokenKey), serializedAccessTokenMetadata("secret-token"));
    // ...but it must not be unwrapped into "access-token".
    EXPECT_FALSE(ctx.metadata("access-token").has_value());
}

TEST(MetadataExtractingInterceptor, MalformedStandardAccessTokenHeaderIsNotExposedAsAccessToken) {
    // Not a valid Metadata_AccessToken protobuf. Falling back to treating
    // this as a raw token would let unparseable/adversarial bytes reach the
    // auth gate under "access-token", so it must be dropped instead.
    std::multimap<std::string, std::string> headers{
        {kAccessTokenStandardKey, "not-a-valid-protobuf-message"},
    };
    CallContext ctx;

    MetadataExtractingInterceptor::extract(headers, ctx);

    EXPECT_FALSE(ctx.metadata("access-token").has_value());
}

}  // namespace
