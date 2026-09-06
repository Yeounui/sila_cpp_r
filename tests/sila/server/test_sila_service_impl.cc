// Tests for SiLAServiceImpl: GetFeatureDefinition lookup through
// FeatureRegistry, SetServerName validation and config update, and property
// getters returning ServerConfig values.
#include <sila/server/SiLAServiceImpl.h>

#include <sila/server/config/ServerConfig.h>
#include <sila/common/error/SiLAErrorException.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/FeatureRegistry.h>

#include "SiLAService.grpc.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <string>

namespace
{
using sila2::FeatureRegistry;
using sila2::InMemoryServerConfig;
using sila2::SiLAServiceImpl;
using sila2::error::DefinedExecutionError;
using sila2::error::fromGrpcStatus;
using sila2::error::SiLAError;
using sila2::error::ValidationError;

namespace silaservice_proto = sila2::org::silastandard::core::silaservice::v1;

// Four segments (org(.sub)*/Category/Name/vN): GetFeatureDefinition's
// FullyQualifiedIdentifier constraint (SiLAService-v1_0.sila.xml:35-37)
// requires a Category segment that plain "org.example/TestFeature/v1" lacks.
const std::string kTestFqi = "org.example/test/TestFeature/v1";
// FeatureRegistry (S46) derives the registered FQI from these three root
// attributes plus <Identifier>, so they must spell out kTestFqi above.
const std::string kTestFdl =
    R"(<Feature Originator="org.example" Category="test" FeatureVersion="1.0">)"
    R"(<Identifier>TestFeature</Identifier></Feature>)";
const std::string kServerUuid = "00000000-1111-2222-3333-444444444444";

// ---------------------------------------------------------------------------
// GetFeatureDefinition — True paths
// ---------------------------------------------------------------------------

TEST(SiLAServiceImpl, GetFeatureDefinitionReturnsRegisteredFdl) {
    FeatureRegistry registry;
    registry.registerFeature(kTestFqi, kTestFdl);
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    SiLAServiceImpl service{registry, config};

    grpc::ServerContext ctx;
    silaservice_proto::GetFeatureDefinition_Parameters request;
    request.mutable_featureidentifier()->set_value(kTestFqi);
    silaservice_proto::GetFeatureDefinition_Responses response;

    const grpc::Status status = service.GetFeatureDefinition(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(response.featuredefinition().value(), kTestFdl);
}

// ---------------------------------------------------------------------------
// GetFeatureDefinition — False paths
// ---------------------------------------------------------------------------

TEST(SiLAServiceImpl, GetFeatureDefinitionUnknownFqiReturnsUnimplementedFeature) {
    FeatureRegistry registry;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    SiLAServiceImpl service{registry, config};

    grpc::ServerContext ctx;
    silaservice_proto::GetFeatureDefinition_Parameters request;
    request.mutable_featureidentifier()->set_value("org.example/test/NoSuchFeature/v1");
    silaservice_proto::GetFeatureDefinition_Responses response;

    const grpc::Status status = service.GetFeatureDefinition(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* definedError = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(definedError, nullptr);
    EXPECT_EQ(definedError->errorIdentifier(),
              "org.silastandard/core/SiLAService/v1/DefinedExecutionError/UnimplementedFeature");
}

// A malformed FQI (missing the Category segment) must be rejected as
// ValidationError before the registry lookup ever runs — not fall through to
// UnimplementedFeature, which is for well-formed-but-unregistered FQIs only.
TEST(SiLAServiceImpl, GetFeatureDefinitionMalformedFqiReturnsValidationError) {
    FeatureRegistry registry;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    SiLAServiceImpl service{registry, config};

    grpc::ServerContext ctx;
    silaservice_proto::GetFeatureDefinition_Parameters request;
    request.mutable_featureidentifier()->set_value("org.example/TestFeature/v1");
    silaservice_proto::GetFeatureDefinition_Responses response;

    const grpc::Status status = service.GetFeatureDefinition(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::ValidationError);
    const auto* validationError = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(validationError, nullptr);
    EXPECT_EQ(validationError->parameter(),
              "org.silastandard/core/SiLAService/v1/Command/GetFeatureDefinition/Parameter/FeatureIdentifier");
}

// ---------------------------------------------------------------------------
// SetServerName — True paths
// ---------------------------------------------------------------------------

TEST(SiLAServiceImpl, SetServerNameUpdatesConfigName) {
    FeatureRegistry registry;
    InMemoryServerConfig config{kServerUuid, "OriginalName"};
    SiLAServiceImpl service{registry, config};

    grpc::ServerContext setCtx;
    silaservice_proto::SetServerName_Parameters setReq;
    setReq.mutable_servername()->set_value("NewName");
    silaservice_proto::SetServerName_Responses setResp;

    const grpc::Status setStatus = service.SetServerName(&setCtx, &setReq, &setResp);
    ASSERT_TRUE(setStatus.ok());

    grpc::ServerContext getCtx;
    silaservice_proto::Get_ServerName_Parameters getReq;
    silaservice_proto::Get_ServerName_Responses getResp;
    service.Get_ServerName(&getCtx, &getReq, &getResp);

    EXPECT_EQ(getResp.servername().value(), "NewName");
}

TEST(SiLAServiceImpl, SetServerNameAcceptsMaxLength255) {
    FeatureRegistry registry;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    SiLAServiceImpl service{registry, config};

    const std::string maxName(255, 'A');
    grpc::ServerContext ctx;
    silaservice_proto::SetServerName_Parameters request;
    request.mutable_servername()->set_value(maxName);
    silaservice_proto::SetServerName_Responses response;

    const grpc::Status status = service.SetServerName(&ctx, &request, &response);
    EXPECT_TRUE(status.ok());
}

TEST(SiLAServiceImpl, SetServerNameAccepts255MultiByteCharacters) {
    FeatureRegistry registry;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    SiLAServiceImpl service{registry, config};

    // "한" is one 3-byte UTF-8 character; 255 of them is 765 bytes but only
    // 255 characters, which is what the FDL's MaximalLength=255 counts.
    std::string maxName;
    for (int i = 0; i < 255; ++i) {
        maxName += "한";
    }
    grpc::ServerContext ctx;
    silaservice_proto::SetServerName_Parameters request;
    request.mutable_servername()->set_value(maxName);
    silaservice_proto::SetServerName_Responses response;

    const grpc::Status status = service.SetServerName(&ctx, &request, &response);
    ASSERT_TRUE(status.ok());

    grpc::ServerContext getCtx;
    silaservice_proto::Get_ServerName_Parameters getReq;
    silaservice_proto::Get_ServerName_Responses getResp;
    service.Get_ServerName(&getCtx, &getReq, &getResp);
    EXPECT_EQ(getResp.servername().value(), maxName);
}

// The FDL declares only MaximalLength=255 for ServerName (no
// MinimalLength), and Part A p29 sets no lower bound on a Display Name — so
// an empty ServerName must be accepted, matching the Build() initial-name
// path (S73).
TEST(SiLAServiceImpl, SetServerNameAcceptsEmptyString) {
    FeatureRegistry registry;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    SiLAServiceImpl service{registry, config};

    grpc::ServerContext ctx;
    silaservice_proto::SetServerName_Parameters request;
    request.mutable_servername()->set_value("");
    silaservice_proto::SetServerName_Responses response;

    const grpc::Status status = service.SetServerName(&ctx, &request, &response);
    ASSERT_TRUE(status.ok());

    grpc::ServerContext getCtx;
    silaservice_proto::Get_ServerName_Parameters getReq;
    silaservice_proto::Get_ServerName_Responses getResp;
    service.Get_ServerName(&getCtx, &getReq, &getResp);
    EXPECT_EQ(getResp.servername().value(), "");
}

// ---------------------------------------------------------------------------
// SetServerName — False paths
// ---------------------------------------------------------------------------

TEST(SiLAServiceImpl, SetServerNameExceedsMaxLengthReturnsValidationError) {
    FeatureRegistry registry;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    SiLAServiceImpl service{registry, config};

    const std::string tooLong(256, 'X');
    grpc::ServerContext ctx;
    silaservice_proto::SetServerName_Parameters request;
    request.mutable_servername()->set_value(tooLong);
    silaservice_proto::SetServerName_Responses response;

    const grpc::Status status = service.SetServerName(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::ValidationError);
}

TEST(SiLAServiceImpl, SetServerNameRejects256MultiByteCharacters) {
    FeatureRegistry registry;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    SiLAServiceImpl service{registry, config};

    // 256 multi-byte characters must still be rejected — the fix must count
    // characters correctly, not simply stop rejecting anything.
    std::string tooLong;
    for (int i = 0; i < 256; ++i) {
        tooLong += "한";
    }
    grpc::ServerContext ctx;
    silaservice_proto::SetServerName_Parameters request;
    request.mutable_servername()->set_value(tooLong);
    silaservice_proto::SetServerName_Responses response;

    const grpc::Status status = service.SetServerName(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::ValidationError);
}

// ---------------------------------------------------------------------------
// Property getters — True paths
// ---------------------------------------------------------------------------

TEST(SiLAServiceImpl, PropertyGettersReturnConfigValues) {
    FeatureRegistry registry;
    InMemoryServerConfig config{kServerUuid, "TestServer",
        sila2::ServerConfig::Identity{
            "TestType", "Test description", "2.0", "https://example.com"}};
    SiLAServiceImpl service{registry, config};

    {
        grpc::ServerContext ctx;
        silaservice_proto::Get_ServerType_Parameters req;
        silaservice_proto::Get_ServerType_Responses resp;
        ASSERT_TRUE(service.Get_ServerType(&ctx, &req, &resp).ok());
        EXPECT_EQ(resp.servertype().value(), "TestType");
    }
    {
        grpc::ServerContext ctx;
        silaservice_proto::Get_ServerUUID_Parameters req;
        silaservice_proto::Get_ServerUUID_Responses resp;
        ASSERT_TRUE(service.Get_ServerUUID(&ctx, &req, &resp).ok());
        EXPECT_EQ(resp.serveruuid().value(), kServerUuid);
    }
    {
        grpc::ServerContext ctx;
        silaservice_proto::Get_ServerDescription_Parameters req;
        silaservice_proto::Get_ServerDescription_Responses resp;
        ASSERT_TRUE(service.Get_ServerDescription(&ctx, &req, &resp).ok());
        EXPECT_EQ(resp.serverdescription().value(), "Test description");
    }
    {
        grpc::ServerContext ctx;
        silaservice_proto::Get_ServerVersion_Parameters req;
        silaservice_proto::Get_ServerVersion_Responses resp;
        ASSERT_TRUE(service.Get_ServerVersion(&ctx, &req, &resp).ok());
        EXPECT_EQ(resp.serverversion().value(), "2.0");
    }
    {
        grpc::ServerContext ctx;
        silaservice_proto::Get_ServerVendorURL_Parameters req;
        silaservice_proto::Get_ServerVendorURL_Responses resp;
        ASSERT_TRUE(service.Get_ServerVendorURL(&ctx, &req, &resp).ok());
        EXPECT_EQ(resp.servervendorurl().value(), "https://example.com");
    }
}

// ---------------------------------------------------------------------------
// Get_ImplementedFeatures — True path
// ---------------------------------------------------------------------------

TEST(SiLAServiceImpl, GetImplementedFeaturesListsAllRegisteredFqis) {
    FeatureRegistry registry;
    // Four-segment FQIs (S46 requires an FDL-derivable identity); the
    // Category segment stays "test" for all three so alphabetical order over
    // Alpha/Beta/Gamma is preserved, matching the assertions below.
    registry.registerFeature("org.example/test/Alpha/v1",
                             R"(<Feature Originator="org.example" Category="test" FeatureVersion="1.0">)"
                             R"(<Identifier>Alpha</Identifier></Feature>)");
    registry.registerFeature("org.example/test/Beta/v1",
                             R"(<Feature Originator="org.example" Category="test" FeatureVersion="1.0">)"
                             R"(<Identifier>Beta</Identifier></Feature>)");
    registry.registerFeature("org.example/test/Gamma/v1",
                             R"(<Feature Originator="org.example" Category="test" FeatureVersion="1.0">)"
                             R"(<Identifier>Gamma</Identifier></Feature>)");
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    SiLAServiceImpl service{registry, config};

    grpc::ServerContext ctx;
    silaservice_proto::Get_ImplementedFeatures_Parameters request;
    silaservice_proto::Get_ImplementedFeatures_Responses response;

    const grpc::Status status = service.Get_ImplementedFeatures(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    ASSERT_EQ(response.implementedfeatures_size(), 3);
    EXPECT_EQ(response.implementedfeatures(0).value(), "org.example/test/Alpha/v1");
    EXPECT_EQ(response.implementedfeatures(1).value(), "org.example/test/Beta/v1");
    EXPECT_EQ(response.implementedfeatures(2).value(), "org.example/test/Gamma/v1");
}

}  // namespace
