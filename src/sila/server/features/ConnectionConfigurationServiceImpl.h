// ConnectionConfigurationServiceImpl.h — SiLA2 core feature (architecture.md §3.9)
//
// New component, not a port. ConnectionConfigurationService lets a SiLA Client
// configure server-initiated connections. This implementation manages a map of
// client name → CloudTransport, with optional persistence of connection details.
#pragma once

// Generated proto header provides the service base class and all message types.
// protoc output goes to CMAKE_CURRENT_BINARY_DIR which is on the include path.
#include "ConnectionConfigurationService.grpc.pb.h"

// CloudEnvelopeRouter& (reference member) still requires the full definition
// for callers that construct this class.
#include <sila/common/tls/UntrustedTlsCredentials.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>
#include <sila/server/transport/SilaHandler.h>

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace sila2 {

// CloudTransport is only held via unique_ptr in ClientEntry, so a forward
// declaration is enough here; the full definition is needed only where the
// destructor is generated, i.e. in the .cc file.
class CloudTransport;
struct InterceptorChain;

// FQI constant for ConnectionConfigurationService — used by Builder::Build() to auto-register.
// Major version only — the FDL is FeatureVersion="1.1" but the FQI (and the
// generated package, ConnectionConfigurationService.proto:5) carry v1, per
// sila_base xslt/fdl2proto.xsl:25 substring-before(FeatureVersion, '.').
inline constexpr std::string_view kConnectionConfigurationServiceFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1";

// Per-RPC granular authorization FQIs. The direct-gRPC auth gate matches a
// call's FQI against protectedFqis; passing these Command/Property-level FQIs
// (not the feature-level one above) lets a command-level protectedFqis entry
// actually gate the matching RPC, keeping direct-gRPC coverage aligned with
// the cloud path. Full-literal string_view, not runtime concatenation, to stay
// constexpr and match kAuthorizationProviderParamFqi's existing style. Major
// version v1 only, matching kConnectionConfigurationServiceFqi above.
inline constexpr std::string_view kEnableServerInitiatedConnectionModeFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/EnableServerInitiatedConnectionMode";
inline constexpr std::string_view kDisableServerInitiatedConnectionModeFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/DisableServerInitiatedConnectionMode";
inline constexpr std::string_view kConnectSiLAClientFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/ConnectSiLAClient";
inline constexpr std::string_view kDisconnectSiLAClientFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/DisconnectSiLAClient";
inline constexpr std::string_view kGet_ServerInitiatedConnectionModeStatusFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Property/ServerInitiatedConnectionModeStatus";
inline constexpr std::string_view kGet_ConfiguredSiLAClientsFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Property/ConfiguredSiLAClients";

// Returns the FDL XML for ConnectionConfigurationService, embedded as a string constant.
// ponytail: codegen will generate ConnectionConfigurationServiceMeta.cc with this constant
// (§2); until then, a raw string literal in the .cc file serves the same purpose.
const std::string& connectionConfigurationServiceFdlXml();

// Namespace alias shortens the generated proto namespace for readability.
namespace connconfig_proto = sila2::org::silastandard::core::connectionconfigurationservice::v1;

class ConnectionConfigurationServiceImpl final
    : public connconfig_proto::ConnectionConfigurationService::Service {
public:
    /// @param defaultCreds Channel credentials for every outbound
    ///        CloudTransport this service creates. Must not be null.
    /// @throws std::invalid_argument if defaultCreds is null — see the
    ///         fail-closed note in the .cc constructor.
    /// @param storePath File to persist connection mode and Persist=true
    ///        clients to. Required and must not be empty — the FDL requires the
    ///        connection mode to survive a restart, so a server offering this
    ///        Feature must have somewhere to persist it.
    /// @throws std::invalid_argument if storePath is empty — see the .cc
    ///         constructor.
    explicit ConnectionConfigurationServiceImpl(
        CloudEnvelopeRouter& router,
        std::shared_ptr<grpc::ChannelCredentials> defaultCreds,
        const InterceptorChain* chain,
        std::filesystem::path storePath);

    /// Same as above, but the credentials are chosen per target host: the
    /// provider returns null for a host the current trust configuration may
    /// not connect to (Part B p74/p75: an untrusted peer certificate is
    /// accepted only inside a private-range network), and ConnectSiLAClient
    /// then reports InvalidSiLAClient instead of opening the stream.
    /// @throws std::invalid_argument if credentialsFor is empty.
    ConnectionConfigurationServiceImpl(
        CloudEnvelopeRouter& router,
        tls::OutboundCredentialsProvider credentialsFor,
        const InterceptorChain* chain,
        std::filesystem::path storePath);

    // User-declared destructor, defined in the .cc file: ClientEntry holds a
    // unique_ptr<CloudTransport>, and CloudTransport is only forward-declared
    // here — the compiler needs CloudTransport's full definition to generate
    // the destructor, which is only available where CloudTransport.h is included.
    ~ConnectionConfigurationServiceImpl();

    // ---- Commands ----

    grpc::Status EnableServerInitiatedConnectionMode(
        grpc::ServerContext* context,
        const connconfig_proto::EnableServerInitiatedConnectionMode_Parameters* request,
        connconfig_proto::EnableServerInitiatedConnectionMode_Responses* response) override;

    grpc::Status DisableServerInitiatedConnectionMode(
        grpc::ServerContext* context,
        const connconfig_proto::DisableServerInitiatedConnectionMode_Parameters* request,
        connconfig_proto::DisableServerInitiatedConnectionMode_Responses* response) override;

    grpc::Status ConnectSiLAClient(
        grpc::ServerContext* context,
        const connconfig_proto::ConnectSiLAClient_Parameters* request,
        connconfig_proto::ConnectSiLAClient_Responses* response) override;

    grpc::Status DisconnectSiLAClient(
        grpc::ServerContext* context,
        const connconfig_proto::DisconnectSiLAClient_Parameters* request,
        connconfig_proto::DisconnectSiLAClient_Responses* response) override;

    // ---- Properties ----

    grpc::Status Get_ServerInitiatedConnectionModeStatus(
        grpc::ServerContext* context,
        const connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Parameters* request,
        connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Responses* response) override;

    grpc::Status Get_ConfiguredSiLAClients(
        grpc::ServerContext* context,
        const connconfig_proto::Get_ConfiguredSiLAClients_Parameters* request,
        connconfig_proto::Get_ConfiguredSiLAClients_Responses* response) override;

    void enableServerInitiatedConnectionMode(
        const connconfig_proto::EnableServerInitiatedConnectionMode_Parameters& request,
        CallContext& ctx,
        ResponseSink<connconfig_proto::EnableServerInitiatedConnectionMode_Responses>& sink);
    void disableServerInitiatedConnectionMode(
        const connconfig_proto::DisableServerInitiatedConnectionMode_Parameters& request,
        CallContext& ctx,
        ResponseSink<connconfig_proto::DisableServerInitiatedConnectionMode_Responses>& sink);
    void connectSiLAClient(const connconfig_proto::ConnectSiLAClient_Parameters& request,
                           CallContext& ctx,
                           ResponseSink<connconfig_proto::ConnectSiLAClient_Responses>& sink);
    void disconnectSiLAClient(const connconfig_proto::DisconnectSiLAClient_Parameters& request,
                              CallContext& ctx,
                              ResponseSink<connconfig_proto::DisconnectSiLAClient_Responses>& sink);
    void getServerInitiatedConnectionModeStatus(
        const connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Parameters& request,
        CallContext& ctx,
        ResponseSink<connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Responses>& sink);
    void getConfiguredSiLAClients(
        const connconfig_proto::Get_ConfiguredSiLAClients_Parameters& request,
        CallContext& ctx,
        ResponseSink<connconfig_proto::Get_ConfiguredSiLAClients_Responses>& sink);

    // ---- Lifecycle ----

    // Connects every persisted client entry (persist == true) on server startup.
    void connectPersistentClients();

    // Disconnects all managed CloudTransports on server shutdown.
    void shutdown();

private:
    struct ClientEntry {
        std::string host;
        uint16_t port;
        bool persist;
        // Forward-declared CloudTransport is sufficient inside unique_ptr as
        // long as the enclosing class's destructor is defined out-of-line.
        std::unique_ptr<CloudTransport> transport;
    };

    // Persists modeEnabled_ and every persist==true client to storePath_.
    // No-op when storePath_ is empty. Caller must already hold mu_.
    void saveState();
    // Reconstructs modeEnabled_ and the persist==true clients from storePath_
    // as UNCONNECTED transports. No-op when storePath_ is empty or the file is
    // absent. Caller must already hold mu_. Throws std::runtime_error on corrupt
    // persistent state, naming the path.
    void loadState();

    std::mutex mu_;
    bool modeEnabled_{false};
    std::map<std::string, ClientEntry> clients_;
    tls::OutboundCredentialsProvider credentialsFor_;
    CloudEnvelopeRouter& router_;
    const InterceptorChain* chain_;
    std::filesystem::path storePath_;
};

}  // namespace sila2
