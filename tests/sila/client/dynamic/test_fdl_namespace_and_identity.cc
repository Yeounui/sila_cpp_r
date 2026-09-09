// Tests for two runtime FDL guarantees that sit next to each other because
// both concern a Feature's *identity*, not its shape:
//
//   S41 -- the runtime parser (FdlRuntimeParser.cc) must accept FDL whose
//   elements spell the SiLA namespace with an explicit prefix
//   ("<sila:Feature>"), not only the default-namespace form every in-tree
//   fixture happens to use. a2cbdfe's libxml2 rewrite already matches
//   elements by namespace href + local name (isSilaElement(), :262-266), so
//   this file pins that behaviour with a regression test rather than
//   changing the parser.
//
//   S44 -- FeatureCatalog::add() must reject a caller-supplied FQI that
//   disagrees with the identity the FDL itself declares (Originator,
//   Category, Identifier, and the major FeatureVersion). The guard lives
//   entirely in FeatureCatalog.cc; this file exercises it black-box through
//   add(), and pins every server-embedded FDL against its FQI constant.
#include <sila/client/dynamic/FdlIR.h>
#include <sila/client/dynamic/FdlRuntimeParser.h>
#include <sila/client/dynamic/FeatureCatalog.h>

#include <sila/server/SilaServiceImpl.h>
#include <sila/server/features/AuthenticationServiceImpl.h>
#include <sila/server/features/AuthorizationConfigurationServiceImpl.h>
#include <sila/server/features/AuthorizationServiceImpl.h>
#include <sila/server/features/ConnectionConfigurationServiceImpl.h>
#include <sila/server/features/LockControllerImpl.h>
#include <sila/server/features/SimulationControllerImpl.h>
#include <sila/server/recovery/ErrorRecoveryServiceImpl.h>

#include <google/protobuf/descriptor.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using sila2::dynamic::Feature;
using sila2::dynamic::FeatureCatalog;
using sila2::dynamic::parseFdl;

// --- S41 fixtures ------------------------------------------------------

// Default-namespaced: the spelling every in-tree FDL fixture already uses.
constexpr char kBareFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.example" Category="tests">
  <Identifier>Thing</Identifier>
  <DisplayName>Thing</DisplayName>
  <Description>Thing feature</Description>
  <Property>
    <Identifier>Value</Identifier>
    <DisplayName>Value</DisplayName>
    <Description>Value</Description>
    <Observable>No</Observable>
    <DataType><Basic>String</Basic></DataType>
  </Property>
</Feature>
)xml";

// Same document, every element carrying an explicit "sila:" prefix bound to
// the same namespace URI -- the spelling the normative fdl2proto.xsl:4,13
// itself uses. FeatureVersion/Originator/Category stay unprefixed:
// FeatureDefinition.xsd:2 sets attributeFormDefault="unqualified", so a
// prefixed attribute is not the same, conformant thing.
constexpr char kPrefixedFdl[] = R"xml(
<sila:Feature xmlns:sila="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.example" Category="tests">
  <sila:Identifier>Thing</sila:Identifier>
  <sila:DisplayName>Thing</sila:DisplayName>
  <sila:Description>Thing feature</sila:Description>
  <sila:Property>
    <sila:Identifier>Value</sila:Identifier>
    <sila:DisplayName>Value</sila:DisplayName>
    <sila:Description>Value</sila:Description>
    <sila:Observable>No</sila:Observable>
    <sila:DataType><sila:Basic>String</sila:Basic></sila:DataType>
  </sila:Property>
</sila:Feature>
)xml";

// Root element in an unrelated namespace: not FDL under any prefix.
constexpr char kWrongNamespaceFdl[] = R"xml(
<Feature xmlns="http://example.com/not-sila" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.example" Category="tests">
  <Identifier>Thing</Identifier>
  <DisplayName>Thing</DisplayName>
  <Description>Thing feature</Description>
</Feature>
)xml";

// --- S41: prefixed FDL parses like its default-namespaced twin ---------

TEST(FdlNamespace, PrefixedFeatureParsesLikeTheBareForm) {
    const Feature bare = parseFdl(kBareFdl);
    const Feature prefixed = parseFdl(kPrefixedFdl);

    EXPECT_EQ(prefixed.identifier, bare.identifier);
    EXPECT_EQ(prefixed.originator, bare.originator);
    EXPECT_EQ(prefixed.category, bare.category);
    EXPECT_EQ(prefixed.featureVersion, bare.featureVersion);
    ASSERT_EQ(prefixed.properties.size(), bare.properties.size());
    EXPECT_EQ(prefixed.properties[0].identifier, bare.properties[0].identifier);
    EXPECT_FALSE(prefixed.properties[0].observable);
}

TEST(FdlNamespace, WrongNamespaceRootIsRejected) {
    EXPECT_THROW(parseFdl(kWrongNamespaceFdl), std::invalid_argument);
}

// --- S44 fixtures --------------------------------------------------------

// Originator/tests/Thing/v1 is this FDL's derived identity: FeatureVersion's
// major segment, not the full "1.0", per fdl2proto.xsl:16-25.
constexpr char kIdentityFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.example" Category="tests">
  <Identifier>Thing</Identifier>
  <DisplayName>Thing</DisplayName>
  <Description>Thing feature</Description>
</Feature>
)xml";
constexpr char kIdentityFqi[] = "org.example/tests/Thing/v1";

// No Category attribute at all -- valid FDL (see
// tests/examples/fdl/sila_base/valid-fdl/MissingCategory.sila.xml), and the
// derived FQI substitutes "none" per fdl2proto.xsl:16-23's fallback.
constexpr char kMissingCategoryFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.example">
  <Identifier>Thing</Identifier>
  <DisplayName>Thing</DisplayName>
  <Description>Thing feature</Description>
</Feature>
)xml";

// FeatureVersion carries a minor revision; the derived FQI still truncates
// to the major part, so a Feature may be revised without re-keying its
// catalog entry.
constexpr char kMinorVersionFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.3" Originator="org.example" Category="tests">
  <Identifier>Thing</Identifier>
  <DisplayName>Thing</DisplayName>
  <Description>Thing feature</Description>
</Feature>
)xml";

// --- S44: FeatureCatalog::add() accepts a matching FQI ------------------

TEST(FeatureCatalogIdentity, CatalogAcceptsAMatchingFqi) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    catalog.add(kIdentityFqi, kIdentityFdl);

    ASSERT_EQ(catalog.fqis().size(), 1u);
    EXPECT_EQ(catalog.fqis()[0], kIdentityFqi);
}

TEST(FeatureCatalogIdentity, CatalogAcceptsAnOmittedCategoryAsNone) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    EXPECT_NO_THROW(catalog.add("org.example/none/Thing/v1", kMissingCategoryFdl));
}

TEST(FeatureCatalogIdentity, CatalogAcceptsAMinorVersionInTheFdlAgainstAMajorVersionInTheFqi) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    EXPECT_NO_THROW(catalog.add(kIdentityFqi, kMinorVersionFdl));
}

// --- S44: FeatureCatalog::add() rejects a disagreeing FQI ----------------

TEST(FeatureCatalogIdentity, CatalogRejectsAWrongIdentifier) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    EXPECT_THROW(catalog.add("org.example/tests/Other/v1", kIdentityFdl), std::invalid_argument);
}

TEST(FeatureCatalogIdentity, CatalogRejectsAWrongOriginator) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    EXPECT_THROW(catalog.add("org.other/tests/Thing/v1", kIdentityFdl), std::invalid_argument);
}

TEST(FeatureCatalogIdentity, CatalogRejectsAWrongCategory) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    EXPECT_THROW(catalog.add("org.example/core/Thing/v1", kIdentityFdl), std::invalid_argument);
}

TEST(FeatureCatalogIdentity, CatalogRejectsAWrongMajorVersion) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    EXPECT_THROW(catalog.add("org.example/tests/Thing/v2", kIdentityFdl), std::invalid_argument);
}

// The exact shape of the bug this item's own evidence found at
// test_dynamic_pipeline.cc:250/295 before it was corrected: the version
// segment spelled as the raw FeatureVersion ("1.0") instead of "v1".
TEST(FeatureCatalogIdentity, CatalogRejectsAVersionSegmentSpelledAsAFeatureVersion) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    try {
        catalog.add("org.example/tests/Thing/1.0", kIdentityFdl);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        const std::string what{e.what()};
        // The message must name both FQIs -- the one the FDL implies and the
        // one the caller supplied -- so a mismatch is diagnosable without a
        // debugger.
        EXPECT_NE(what.find(kIdentityFqi), std::string::npos);
        EXPECT_NE(what.find("org.example/tests/Thing/1.0"), std::string::npos);
    }
}

TEST(FeatureCatalogIdentity, ARejectedFeatureLeavesTheCatalogEmpty) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    EXPECT_THROW(catalog.add("org.example/tests/Other/v1", kIdentityFdl), std::invalid_argument);

    // The guard runs before DescriptorBuilder/BuildFile, so a rejected
    // Feature must never reach pool_/entries_/fqis_.
    EXPECT_TRUE(catalog.fqis().empty());
    EXPECT_THROW(catalog.grpcMethodName("org.example/tests/Other/v1", "Get_Value"),
                 std::out_of_range);
}

// --- S44: every server-embedded FDL agrees with its FQI constant --------

// Not a registration-time check (FeatureRegistry has no XML parser to run
// one against -- see sc18.json's S44 rejected[]); this pins the eight FDLs
// this repo actually embeds against their hand-written FQI constants, so a
// typo in a constant is caught here instead of silently advertised over
// SiLAService.GetImplementedFeatures. FeatureCatalog::add() IS that
// comparison: a matching pair passes silently, a drifted one throws.
struct ServerFeature {
    std::string_view name;
    std::string_view fqi;
    const std::string& fdlXml;
};

TEST(FeatureCatalogIdentity, ServerEmbeddedFdlIdentitiesMatchTheirFqiConstants) {
    // Function-local so the fdlXml references bind at test run time: at
    // namespace scope they would bind during this TU's dynamic init to
    // kFdlXml strings whose cross-TU construction order is unspecified.
    const ServerFeature kServerFeatures[] = {
        {"SiLAService", sila2::kSiLAServiceFqi, sila2::silaServiceFdlXml()},
        {"AuthenticationService", sila2::kAuthenticationServiceFqi, sila2::authenticationServiceFdlXml()},
        {"AuthorizationService", sila2::kAuthorizationServiceFqi, sila2::authorizationServiceFdlXml()},
        {"AuthorizationConfigurationService", sila2::kAuthorizationConfigurationServiceFqi,
         sila2::authorizationConfigurationServiceFdlXml()},
        {"ErrorRecoveryService", sila2::kErrorRecoveryServiceFqi, sila2::errorRecoveryServiceFdlXml()},
        {"LockController", sila2::kLockControllerFqi, sila2::lockControllerFdlXml()},
        {"SimulationController", sila2::kSimulationControllerFqi, sila2::simulationControllerFdlXml()},
        {"ConnectionConfigurationService", sila2::kConnectionConfigurationServiceFqi,
         sila2::connectionConfigurationServiceFdlXml()},
    };
    for (const auto& feature : kServerFeatures) {
        SCOPED_TRACE(feature.name);
        FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
        EXPECT_NO_THROW(catalog.add(std::string{feature.fqi}, feature.fdlXml));
    }
}

}  // namespace
