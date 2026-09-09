// SilaServiceImpl.cc — SiLA2 core feature (architecture.md §3.2, §3.10)
#include "SilaServiceImpl.h"

#include <sila/server/config/ServerConfig.h>
#include <sila/server/discovery/MdnsPublisher.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/common/types/Constraints.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/SiLAServiceFdl.h>
#include <sila/server/transport/GrpcTransport.h>

#include "SiLAFramework.pb.h"

#include <string>

namespace sila2 {
namespace {
const std::string kFdlXml = generated::kSiLAServiceFdlXml;

// The FDL's <DefinedExecutionErrors> section declares UnimplementedFeature
// as the error for GetFeatureDefinition when the FQI is not found.
// ponytail: codegen will emit this as a constant in SiLAServiceMeta (§2)
const std::string kUnimplementedFeatureErrorId =
    "org.silastandard/core/SiLAService/v1/DefinedExecutionError/UnimplementedFeature";

const std::string kGetFeatureDefinitionParamFqi =
    "org.silastandard/core/SiLAService/v1/Command/GetFeatureDefinition/Parameter/FeatureIdentifier";

}  // namespace

const std::string& silaServiceFdlXml() { return kFdlXml; }

SilaServiceImpl::SilaServiceImpl(const FeatureRegistry& registry, ServerConfig& config,
                                   discovery::MdnsPublisher* publisher,
                                   const InterceptorChain* chain)
    : registry_{registry}, config_{config}, publisher_{publisher}, chain_{chain} {}

grpc::Status SilaServiceImpl::GetFeatureDefinition(
    grpc::ServerContext* context,
    const silaservice_proto::GetFeatureDefinition_Parameters* request,
    silaservice_proto::GetFeatureDefinition_Responses* response) {
    GrpcUnaryResponseSink<silaservice_proto::GetFeatureDefinition_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getFeatureDefinition(req, ctx, out); },
        chain_, kGetFeatureDefinitionFqi, response);
    return sink.status();
}

void SilaServiceImpl::getFeatureDefinition(
    const silaservice_proto::GetFeatureDefinition_Parameters& request, CallContext&,
    ResponseSink<silaservice_proto::GetFeatureDefinition_Responses>& sink) {
    const auto& fqi = request.featureidentifier().value();
    // SiLAService-v1_0.sila.xml:35-37 constrains FeatureIdentifier to
    // FullyQualifiedIdentifier; a malformed FQI must return ValidationError,
    // not fall through to the registry lookup's UnimplementedFeature DEE.
    if (auto fqiError = types::checkFullyQualifiedIdentifier(fqi)) {
        throw error::ValidationError{kGetFeatureDefinitionParamFqi, *fqiError};
    }
    try {
        const auto& fdlXml = registry_.featureDefinition(fqi);
        silaservice_proto::GetFeatureDefinition_Responses response;
        response.mutable_featuredefinition()->set_value(fdlXml);
        sink.send(response);
        sink.finish();
    } catch (const std::out_of_range&) {
        throw error::DefinedExecutionError{
            kUnimplementedFeatureErrorId,
            "Feature not implemented: " + fqi};
    }
}

grpc::Status SilaServiceImpl::SetServerName(
    grpc::ServerContext* context,
    const silaservice_proto::SetServerName_Parameters* request,
    silaservice_proto::SetServerName_Responses* /*response*/) {
    silaservice_proto::SetServerName_Responses response;
    GrpcUnaryResponseSink<silaservice_proto::SetServerName_Responses> sink(&response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { setServerName(req, ctx, out); },
        chain_, kSetServerNameFqi, &response);
    return sink.status();
}

void SilaServiceImpl::setServerName(
    const silaservice_proto::SetServerName_Parameters& request, CallContext&,
    ResponseSink<silaservice_proto::SetServerName_Responses>& sink) {
    const auto& name = request.servername().value();
    // FDL constrains ServerName by MaximalLength=255 only (SiLAService-v1_0
    // .sila.xml:81), counted in Unicode code points; there is no lower bound
    // (Part A p29), so an empty name is accepted, matching build().
    static constexpr auto kParamFqi =
        "org.silastandard/core/SiLAService/v1/Command/SetServerName/Parameter/ServerName";
    if (auto lengthError = types::checkMaximalLength(name, 255)) {
        throw error::ValidationError{kParamFqi, *lengthError};
    }
    config_.setName(name);
    // Update the server_name TXT record so discovery clients see the new name;
    // the mDNS instance name stays the SiLA Server UUID (Part B p76 MUST).
    if (publisher_) {
        publisher_->setName(name);
    }
    sink.send(silaservice_proto::SetServerName_Responses{});
    sink.finish();
}

grpc::Status SilaServiceImpl::Get_ServerName(
    grpc::ServerContext* context,
    const silaservice_proto::Get_ServerName_Parameters* request,
    silaservice_proto::Get_ServerName_Responses* response) {
    GrpcUnaryResponseSink<silaservice_proto::Get_ServerName_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getServerName(req, ctx, out); },
        chain_, kGet_ServerNameFqi, response);
    return sink.status();
}

void SilaServiceImpl::getServerName(
    const silaservice_proto::Get_ServerName_Parameters&, CallContext&,
    ResponseSink<silaservice_proto::Get_ServerName_Responses>& sink) {
    silaservice_proto::Get_ServerName_Responses response;
    response.mutable_servername()->set_value(config_.name());
    sink.send(response);
    sink.finish();
}

grpc::Status SilaServiceImpl::Get_ServerType(
    grpc::ServerContext* context,
    const silaservice_proto::Get_ServerType_Parameters* request,
    silaservice_proto::Get_ServerType_Responses* response) {
    GrpcUnaryResponseSink<silaservice_proto::Get_ServerType_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getServerType(req, ctx, out); },
        chain_, kGet_ServerTypeFqi, response);
    return sink.status();
}

void SilaServiceImpl::getServerType(
    const silaservice_proto::Get_ServerType_Parameters&, CallContext&,
    ResponseSink<silaservice_proto::Get_ServerType_Responses>& sink) {
    silaservice_proto::Get_ServerType_Responses response;
    response.mutable_servertype()->set_value(config_.serverType());
    sink.send(response);
    sink.finish();
}

grpc::Status SilaServiceImpl::Get_ServerUUID(
    grpc::ServerContext* context,
    const silaservice_proto::Get_ServerUUID_Parameters* request,
    silaservice_proto::Get_ServerUUID_Responses* response) {
    GrpcUnaryResponseSink<silaservice_proto::Get_ServerUUID_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getServerUuid(req, ctx, out); },
        chain_, kGet_ServerUUIDFqi, response);
    return sink.status();
}

void SilaServiceImpl::getServerUuid(
    const silaservice_proto::Get_ServerUUID_Parameters&, CallContext&,
    ResponseSink<silaservice_proto::Get_ServerUUID_Responses>& sink) {
    silaservice_proto::Get_ServerUUID_Responses response;
    response.mutable_serveruuid()->set_value(config_.uuid());
    sink.send(response);
    sink.finish();
}

grpc::Status SilaServiceImpl::Get_ServerDescription(
    grpc::ServerContext* context,
    const silaservice_proto::Get_ServerDescription_Parameters* request,
    silaservice_proto::Get_ServerDescription_Responses* response) {
    GrpcUnaryResponseSink<silaservice_proto::Get_ServerDescription_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getServerDescription(req, ctx, out); },
        chain_, kGet_ServerDescriptionFqi, response);
    return sink.status();
}

void SilaServiceImpl::getServerDescription(
    const silaservice_proto::Get_ServerDescription_Parameters&, CallContext&,
    ResponseSink<silaservice_proto::Get_ServerDescription_Responses>& sink) {
    silaservice_proto::Get_ServerDescription_Responses response;
    response.mutable_serverdescription()->set_value(config_.description());
    sink.send(response);
    sink.finish();
}

grpc::Status SilaServiceImpl::Get_ServerVersion(
    grpc::ServerContext* context,
    const silaservice_proto::Get_ServerVersion_Parameters* request,
    silaservice_proto::Get_ServerVersion_Responses* response) {
    GrpcUnaryResponseSink<silaservice_proto::Get_ServerVersion_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getServerVersion(req, ctx, out); },
        chain_, kGet_ServerVersionFqi, response);
    return sink.status();
}

void SilaServiceImpl::getServerVersion(
    const silaservice_proto::Get_ServerVersion_Parameters&, CallContext&,
    ResponseSink<silaservice_proto::Get_ServerVersion_Responses>& sink) {
    silaservice_proto::Get_ServerVersion_Responses response;
    response.mutable_serverversion()->set_value(config_.version());
    sink.send(response);
    sink.finish();
}

grpc::Status SilaServiceImpl::Get_ServerVendorURL(
    grpc::ServerContext* context,
    const silaservice_proto::Get_ServerVendorURL_Parameters* request,
    silaservice_proto::Get_ServerVendorURL_Responses* response) {
    GrpcUnaryResponseSink<silaservice_proto::Get_ServerVendorURL_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getServerVendorUrl(req, ctx, out); },
        chain_, kGet_ServerVendorURLFqi, response);
    return sink.status();
}

void SilaServiceImpl::getServerVendorUrl(
    const silaservice_proto::Get_ServerVendorURL_Parameters&, CallContext&,
    ResponseSink<silaservice_proto::Get_ServerVendorURL_Responses>& sink) {
    silaservice_proto::Get_ServerVendorURL_Responses response;
    response.mutable_servervendorurl()->set_value(config_.vendorUrl());
    sink.send(response);
    sink.finish();
}

grpc::Status SilaServiceImpl::Get_ImplementedFeatures(
    grpc::ServerContext* context,
    const silaservice_proto::Get_ImplementedFeatures_Parameters* request,
    silaservice_proto::Get_ImplementedFeatures_Responses* response) {
    GrpcUnaryResponseSink<silaservice_proto::Get_ImplementedFeatures_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getImplementedFeatures(req, ctx, out); },
        chain_, kGet_ImplementedFeaturesFqi, response);
    return sink.status();
}

void SilaServiceImpl::getImplementedFeatures(
    const silaservice_proto::Get_ImplementedFeatures_Parameters&, CallContext&,
    ResponseSink<silaservice_proto::Get_ImplementedFeatures_Responses>& sink) {
    silaservice_proto::Get_ImplementedFeatures_Responses response;
    for (const auto& fqi : registry_.registeredFeatureIdentifiers()) {
        response.add_implementedfeatures()->set_value(fqi);
    }
    sink.send(response);
    sink.finish();
}

}  // namespace sila2
