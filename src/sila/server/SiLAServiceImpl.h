// SiLAServiceImpl.h — SiLA2 core feature (architecture.md §3.2, §3.10)
//
// New component, not a port. The SiLAService Feature is mandatory for every
// SiLA 2 server: it exposes server identity (name, UUID, type, version) and
// introspection (list features, get FDL XML). This class extends the gRPC
// service base generated from SiLAService.proto.
#pragma once

// Generated proto header provides the service base class and all message types.
// protoc output goes to CMAKE_CURRENT_BINARY_DIR which is on the include path.
#include "SiLAService.grpc.pb.h"

#include <sila/server/transport/SilaHandler.h>

#include <string>
#include <string_view>

namespace sila2 {

class FeatureRegistry;
class ServerConfig;
struct InterceptorChain;
namespace discovery { class MdnsPublisher; }

// FQI constant for SiLAService — used by Builder::Build() to auto-register.
inline constexpr std::string_view kSiLAServiceFqi =
    "org.silastandard/core/SiLAService/v1";

// Per-RPC granular authorization FQIs. The direct-gRPC auth gate matches a
// call's FQI against protectedFqis; passing these Command/Property-level FQIs
// (not the feature-level one above) lets a command-level protectedFqis entry
// actually gate the matching RPC, keeping direct-gRPC coverage aligned with
// the cloud path. Full-literal string_view, not runtime concatenation, to stay
// constexpr and match kAuthorizationProviderParamFqi's existing style.
inline constexpr std::string_view kGetFeatureDefinitionFqi =
    "org.silastandard/core/SiLAService/v1/Command/GetFeatureDefinition";
inline constexpr std::string_view kSetServerNameFqi =
    "org.silastandard/core/SiLAService/v1/Command/SetServerName";
inline constexpr std::string_view kGet_ServerNameFqi =
    "org.silastandard/core/SiLAService/v1/Property/ServerName";
inline constexpr std::string_view kGet_ServerTypeFqi =
    "org.silastandard/core/SiLAService/v1/Property/ServerType";
inline constexpr std::string_view kGet_ServerUUIDFqi =
    "org.silastandard/core/SiLAService/v1/Property/ServerUUID";
inline constexpr std::string_view kGet_ServerDescriptionFqi =
    "org.silastandard/core/SiLAService/v1/Property/ServerDescription";
inline constexpr std::string_view kGet_ServerVersionFqi =
    "org.silastandard/core/SiLAService/v1/Property/ServerVersion";
inline constexpr std::string_view kGet_ServerVendorURLFqi =
    "org.silastandard/core/SiLAService/v1/Property/ServerVendorURL";
inline constexpr std::string_view kGet_ImplementedFeaturesFqi =
    "org.silastandard/core/SiLAService/v1/Property/ImplementedFeatures";

// Returns the FDL XML for SiLAService, embedded as a string constant.
// ponytail: codegen will generate SiLAServiceMeta.cc with this constant (§2);
// until then, a raw string literal in the .cc file serves the same purpose.
const std::string& silaServiceFdlXml();

// Namespace alias shortens the generated proto namespace for readability.
namespace silaservice_proto = sila2::org::silastandard::core::silaservice::v1;

class SiLAServiceImpl final : public silaservice_proto::SiLAService::Service {
public:
    // registry, config, and publisher must outlive this object — SiLAServerBase owns all.
    // publisher is nullable at the type level (defaults to nullptr) but Build() always
    // passes a non-null one: discovery is always enabled (Part B p75 MUST).
    SiLAServiceImpl(const FeatureRegistry& registry, ServerConfig& config,
                    discovery::MdnsPublisher* publisher = nullptr,
                    const InterceptorChain* chain = nullptr);

    // ---- Commands ----

    grpc::Status GetFeatureDefinition(
        grpc::ServerContext* context,
        const silaservice_proto::GetFeatureDefinition_Parameters* request,
        silaservice_proto::GetFeatureDefinition_Responses* response) override;

    grpc::Status SetServerName(
        grpc::ServerContext* context,
        const silaservice_proto::SetServerName_Parameters* request,
        silaservice_proto::SetServerName_Responses* response) override;

    // ---- Properties ----

    grpc::Status Get_ServerName(
        grpc::ServerContext* context,
        const silaservice_proto::Get_ServerName_Parameters* request,
        silaservice_proto::Get_ServerName_Responses* response) override;

    grpc::Status Get_ServerType(
        grpc::ServerContext* context,
        const silaservice_proto::Get_ServerType_Parameters* request,
        silaservice_proto::Get_ServerType_Responses* response) override;

    grpc::Status Get_ServerUUID(
        grpc::ServerContext* context,
        const silaservice_proto::Get_ServerUUID_Parameters* request,
        silaservice_proto::Get_ServerUUID_Responses* response) override;

    grpc::Status Get_ServerDescription(
        grpc::ServerContext* context,
        const silaservice_proto::Get_ServerDescription_Parameters* request,
        silaservice_proto::Get_ServerDescription_Responses* response) override;

    grpc::Status Get_ServerVersion(
        grpc::ServerContext* context,
        const silaservice_proto::Get_ServerVersion_Parameters* request,
        silaservice_proto::Get_ServerVersion_Responses* response) override;

    grpc::Status Get_ServerVendorURL(
        grpc::ServerContext* context,
        const silaservice_proto::Get_ServerVendorURL_Parameters* request,
        silaservice_proto::Get_ServerVendorURL_Responses* response) override;

    grpc::Status Get_ImplementedFeatures(
        grpc::ServerContext* context,
        const silaservice_proto::Get_ImplementedFeatures_Parameters* request,
        silaservice_proto::Get_ImplementedFeatures_Responses* response) override;

    void getFeatureDefinition(const silaservice_proto::GetFeatureDefinition_Parameters& request,
                              CallContext& ctx,
                              ResponseSink<silaservice_proto::GetFeatureDefinition_Responses>& sink);
    void setServerName(const silaservice_proto::SetServerName_Parameters& request,
                       CallContext& ctx,
                       ResponseSink<silaservice_proto::SetServerName_Responses>& sink);
    void getServerName(const silaservice_proto::Get_ServerName_Parameters& request,
                       CallContext& ctx,
                       ResponseSink<silaservice_proto::Get_ServerName_Responses>& sink);
    void getServerType(const silaservice_proto::Get_ServerType_Parameters& request,
                       CallContext& ctx,
                       ResponseSink<silaservice_proto::Get_ServerType_Responses>& sink);
    void getServerUuid(const silaservice_proto::Get_ServerUUID_Parameters& request,
                       CallContext& ctx,
                       ResponseSink<silaservice_proto::Get_ServerUUID_Responses>& sink);
    void getServerDescription(const silaservice_proto::Get_ServerDescription_Parameters& request,
                              CallContext& ctx,
                              ResponseSink<silaservice_proto::Get_ServerDescription_Responses>& sink);
    void getServerVersion(const silaservice_proto::Get_ServerVersion_Parameters& request,
                          CallContext& ctx,
                          ResponseSink<silaservice_proto::Get_ServerVersion_Responses>& sink);
    void getServerVendorUrl(const silaservice_proto::Get_ServerVendorURL_Parameters& request,
                            CallContext& ctx,
                            ResponseSink<silaservice_proto::Get_ServerVendorURL_Responses>& sink);
    void getImplementedFeatures(
        const silaservice_proto::Get_ImplementedFeatures_Parameters& request,
        CallContext& ctx,
        ResponseSink<silaservice_proto::Get_ImplementedFeatures_Responses>& sink);

private:
    const FeatureRegistry& registry_;
    ServerConfig& config_;
    discovery::MdnsPublisher* publisher_;
    const InterceptorChain* chain_;
};

}  // namespace sila2
