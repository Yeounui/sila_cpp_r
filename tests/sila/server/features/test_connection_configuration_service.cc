// Tests for ConnectionConfigurationServiceImpl: server-initiated connection
// mode toggling, Get_ConfiguredSiLAClients on an empty registry, and
// ConnectSiLAClient/DisconnectSiLAClient input validation that short-circuits
// before a CloudTransport is created (a real CloudTransport requires a live
// network connection and is out of scope for this suite).
#include <sila/server/features/ConnectionConfigurationServiceImpl.h>

#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/common/types/Constraints.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>

#include "ConnectionConfigurationService.grpc.pb.h"
#include "SiLAFramework.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <filesystem>
#include <string>
#include <unistd.h>

namespace
{
using sila2::CloudEnvelopeRouter;
using sila2::ConnectionConfigurationServiceImpl;
using sila2::FeatureRegistry;
using sila2::error::DefinedExecutionError;
using sila2::error::fromGrpcStatus;
using sila2::error::SilaError;
using sila2::error::ValidationError;

namespace connconfig_proto = sila2::org::silastandard::core::connectionconfigurationservice::v1;

// storePath is mandatory now, so every svc needs a real file. These tests don't
// assert on persistence, but the service still writes on each mutation, so give
// each test its own path (unique by test name + pid) to keep runs independent.
std::filesystem::path serviceStorePath() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return std::filesystem::temp_directory_path() /
        ("sila_connconfig_service_" + std::string(info->name()) + "_" +
         std::to_string(::getpid()) + ".tsv");
}

const std::string kInvalidSiLAClientErrorId =
    "org.silastandard/core/ConnectionConfigurationService/v1/DefinedExecutionError/InvalidSiLAClient";
const std::string kClientPortParamFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/ConnectSiLAClient/Parameter/SiLAClientPort";
const std::string kClientNameParamFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/ConnectSiLAClient/Parameter/ClientName";
const std::string kClientHostParamFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/ConnectSiLAClient/Parameter/SiLAClientHost";
const std::string kDisconnectClientNameParamFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/DisconnectSiLAClient/Parameter/ClientName";

grpc::Status enableMode(ConnectionConfigurationServiceImpl& svc) {
    grpc::ServerContext ctx;
    connconfig_proto::EnableServerInitiatedConnectionMode_Parameters request;
    connconfig_proto::EnableServerInitiatedConnectionMode_Responses response;
    return svc.EnableServerInitiatedConnectionMode(&ctx, &request, &response);
}

grpc::Status disableMode(ConnectionConfigurationServiceImpl& svc) {
    grpc::ServerContext ctx;
    connconfig_proto::DisableServerInitiatedConnectionMode_Parameters request;
    connconfig_proto::DisableServerInitiatedConnectionMode_Responses response;
    return svc.DisableServerInitiatedConnectionMode(&ctx, &request, &response);
}

bool getModeStatus(ConnectionConfigurationServiceImpl& svc) {
    grpc::ServerContext ctx;
    connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Parameters request;
    connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Responses response;
    svc.Get_ServerInitiatedConnectionModeStatus(&ctx, &request, &response);
    return response.serverinitiatedconnectionmodestatus().value();
}

connconfig_proto::Get_ConfiguredSiLAClients_Responses getConfiguredClients(
    ConnectionConfigurationServiceImpl& svc) {
    grpc::ServerContext ctx;
    connconfig_proto::Get_ConfiguredSiLAClients_Parameters request;
    connconfig_proto::Get_ConfiguredSiLAClients_Responses response;
    svc.Get_ConfiguredSiLAClients(&ctx, &request, &response);
    return response;
}

grpc::Status connectSiLAClient(ConnectionConfigurationServiceImpl& svc,
                                const std::string& clientName,
                                const std::string& host,
                                int64_t port,
                                bool persist) {
    grpc::ServerContext ctx;
    connconfig_proto::ConnectSiLAClient_Parameters request;
    request.mutable_clientname()->set_value(clientName);
    request.mutable_silaclienthost()->set_value(host);
    request.mutable_silaclientport()->set_value(port);
    request.mutable_persist()->set_value(persist);
    connconfig_proto::ConnectSiLAClient_Responses response;
    return svc.ConnectSiLAClient(&ctx, &request, &response);
}

grpc::Status disconnectSiLAClient(ConnectionConfigurationServiceImpl& svc,
                                   const std::string& clientName,
                                   bool remove) {
    grpc::ServerContext ctx;
    connconfig_proto::DisconnectSiLAClient_Parameters request;
    request.mutable_clientname()->set_value(clientName);
    request.mutable_remove()->set_value(remove);
    connconfig_proto::DisconnectSiLAClient_Responses response;
    return svc.DisconnectSiLAClient(&ctx, &request, &response);
}

// ---------------------------------------------------------------------------
// True paths
// ---------------------------------------------------------------------------

TEST(ConnectionConfigurationService, EnableModeThenStatusIsTrue) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    ASSERT_TRUE(enableMode(svc).ok());
    EXPECT_TRUE(getModeStatus(svc));
}

TEST(ConnectionConfigurationService, DisableModeThenStatusIsFalse) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    ASSERT_TRUE(disableMode(svc).ok());
    EXPECT_FALSE(getModeStatus(svc));
}

TEST(ConnectionConfigurationService, EnableThenDisableTogglesStatus) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    ASSERT_TRUE(enableMode(svc).ok());
    ASSERT_TRUE(getModeStatus(svc));

    ASSERT_TRUE(disableMode(svc).ok());
    EXPECT_FALSE(getModeStatus(svc));
}

TEST(ConnectionConfigurationService, GetConfiguredClientsWhenEmptyReturnsNoEntries) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const auto response = getConfiguredClients(svc);
    EXPECT_EQ(response.configuredsilaclients_size(), 0);
}

// FDL ConnectSiLAClient/ClientName and /SiLAClientHost both carry
// MaximalLength 255 (S22). connectSiLAClient checks both lengths before the
// port gate and creates the CloudTransport only after it, so pairing
// 255-char values with port 0 pins the length checks deterministically with
// no channel ever opened — the earlier port-8080 form was this suite's only
// real gRPC connection attempt (DNS/timing dependent).
TEST(ConnectionConfigurationService, ConnectWithClientNameAtMaxLengthPassesValidation) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const std::string maxName(255, 'a');
    const std::string maxHost(255, 'b');
    const grpc::Status status = connectSiLAClient(svc, maxName, maxHost, 0, false);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    // The port FQI (not Name/Host) means both length constraints cleared.
    EXPECT_EQ(err->parameter(), kClientPortParamFqi);
}

// ---------------------------------------------------------------------------
// FQI shape — catches any future minor-version leak without pinning a literal
// ---------------------------------------------------------------------------

TEST(ConnectionConfigurationService, FeatureFqiIsAWellFormedSiLAFqi) {
    // sila_base xslt/fdl2proto.xsl:25 truncates FeatureVersion at the first
    // '.', so the FQI must end in "v<major>" — never "v<major>_<minor>".
    EXPECT_EQ(sila2::types::checkFullyQualifiedIdentifier(
                  std::string{sila2::kConnectionConfigurationServiceFqi}),
              std::nullopt);
}

TEST(ConnectionConfigurationService, MinorVersionFqiFormIsRejectedByConstraintChecker) {
    // Guards against reintroducing the "v1_1" spelling: the repo's own FQI
    // grammar (Constraints.cc) requires v[0-9]+$ and rejects underscores.
    EXPECT_NE(sila2::types::checkFullyQualifiedIdentifier(
                  "org.silastandard/core/ConnectionConfigurationService/v1_1"),
              std::nullopt);
}

// ---------------------------------------------------------------------------
// False paths — CAUGHT: rejected by validation before CloudTransport creation
// ---------------------------------------------------------------------------

TEST(ConnectionConfigurationService, ConnectWithEmptyHostReturnsInvalidSiLAClient) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const grpc::Status status = connectSiLAClient(svc, "client-1", "", 8080, false);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kInvalidSiLAClientErrorId);
    EXPECT_NE(std::string{err->what()}.find("SiLAClientHost must not be empty"), std::string::npos);
}

TEST(ConnectionConfigurationService, ConnectWithPortZeroReturnsValidationError) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const grpc::Status status = connectSiLAClient(svc, "client-1", "127.0.0.1", 0, false);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    // Message wording is not pinned here (S21): the parameter FQI is the
    // stronger, wire-visible check, and the message is free to reword.
    EXPECT_EQ(err->parameter(), kClientPortParamFqi);
}

// 65536 satisfies the FDL's MaximalInclusive but overflows the uint16_t that
// ClientEntry stores the port in — accepting it would truncate to port 0 and
// register a client whose transport silently points nowhere (S21). It must
// therefore be refused as InvalidSiLAClient, not accepted and not reported as
// a ValidationError against a parameter that in fact satisfies its constraint.
TEST(ConnectionConfigurationService, ConnectWithPort65536IsNotAValidationError) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const grpc::Status status = connectSiLAClient(svc, "client-1", "127.0.0.1", 65536, false);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kInvalidSiLAClientErrorId);
    // Nothing was stored with a truncated port — the rejection happens
    // before ClientEntry construction, not after a bad registration.
    EXPECT_EQ(getConfiguredClients(svc).configuredsilaclients_size(), 0);
}

TEST(ConnectionConfigurationService, ConnectWithPort65537ReturnsValidationError) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const grpc::Status status = connectSiLAClient(svc, "client-1", "127.0.0.1", 65537, false);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->parameter(), kClientPortParamFqi);
}

TEST(ConnectionConfigurationService, ConnectWithPortAbove65536ReturnsValidationError) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const grpc::Status status = connectSiLAClient(svc, "client-1", "127.0.0.1", 70000, false);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->parameter(), kClientPortParamFqi);
}

TEST(ConnectionConfigurationService, ConnectWithClientNameOver255CharsReturnsValidationError) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const std::string tooLongName(256, 'a');
    const grpc::Status status = connectSiLAClient(svc, tooLongName, "127.0.0.1", 8080, false);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->parameter(), kClientNameParamFqi);
}

TEST(ConnectionConfigurationService, ConnectWithSiLAClientHostOver255CharsReturnsValidationError) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const std::string tooLongHost(256, 'b');
    const grpc::Status status = connectSiLAClient(svc, "client-1", tooLongHost, 8080, false);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    // Must be the length ValidationError, not the empty-host InvalidSiLAClient
    // DefinedExecutionError — the constraint check runs on the actual value.
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->parameter(), kClientHostParamFqi);
}

TEST(ConnectionConfigurationService, DisconnectWithClientNameOver255CharsReturnsValidationError) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const std::string tooLongName(256, 'a');
    const grpc::Status status = disconnectSiLAClient(svc, tooLongName, false);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    // Must be the length ValidationError, not the unknown-client
    // InvalidSiLAClient DEE — the constraint check runs before the lookup.
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->parameter(), kDisconnectClientNameParamFqi);
}

TEST(ConnectionConfigurationService, DisconnectWithUnknownClientNameReturnsInvalidSiLAClient) {
    FeatureRegistry registry;
    CloudEnvelopeRouter router{registry};
    ConnectionConfigurationServiceImpl svc{
        router, grpc::InsecureChannelCredentials(), nullptr, serviceStorePath()};

    const grpc::Status status = disconnectSiLAClient(svc, "no-such-client", false);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kInvalidSiLAClientErrorId);
    EXPECT_NE(std::string{err->what()}.find("Unknown client name"), std::string::npos);
}

// The 2-arg (unconfigured) constructor is gone: Part A p32 (SHALL support)
// means every server now gets working server-initiated defaults from
// SilaServerBase::Builder::build(), so a service instance always carries real
// credentials and a real store. Their old obligations moved to the Builder
// level: positive (Enable works without withConnectionConfiguration) is
// test_sila_server_base.cc's DefaultBuildEnablesServerInitiatedModeOverCloud;
// rejection (withConnectionConfiguration itself still requires both
// arguments) is WithConnectionConfigurationRejectsEmptyPathAndNullCredentials
// in the same file.

}  // namespace
