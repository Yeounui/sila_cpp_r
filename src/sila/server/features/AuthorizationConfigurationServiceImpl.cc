// AuthorizationConfigurationServiceImpl.cc — authorization provider configuration (architecture.md §3.11)
#include "AuthorizationConfigurationServiceImpl.h"

#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/common/types/Constraints.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/config/ServerConfig.h>
#include <sila/server/features/AuthorizationConfigurationServiceFdl.h>
#include <sila/server/transport/GrpcTransport.h>

#include "SiLAFramework.pb.h"

#include <string>

namespace sila2 {
namespace {
const std::string kFdlXml = generated::kAuthorizationConfigurationServiceFdlXml;

const std::string kAuthorizationProviderParamFqi =
    "org.silastandard/core/AuthorizationConfigurationService/v1/Command/"
    "SetAuthorizationProvider/Parameter/AuthorizationProvider";

// FDL AuthorizationConfigurationService-v1_0.sila.xml:41-42 constrains this
// parameter to Length 36 plus a lowercase-hex UUID Pattern. The hyphens are
// written unescaped here: the FDL spells them \- for XSD, which std::regex's
// ECMAScript grammar does not require outside a character class.
const std::string kAuthorizationProviderPattern =
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";

}  // namespace

const std::string& authorizationConfigurationServiceFdlXml() { return kFdlXml; }

AuthorizationConfigurationServiceImpl::AuthorizationConfigurationServiceImpl(
    auth::AuthTokenStore& store, ServerConfig& config, const InterceptorChain* chain)
    : store_{store}, config_{config}, chain_{chain} {}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

grpc::Status AuthorizationConfigurationServiceImpl::SetAuthorizationProvider(
    grpc::ServerContext* context,
    const authzconfig_proto::SetAuthorizationProvider_Parameters* request,
    authzconfig_proto::SetAuthorizationProvider_Responses* response) {
    GrpcUnaryResponseSink<authzconfig_proto::SetAuthorizationProvider_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { setAuthorizationProvider(req, ctx, out); },
        chain_, kSetAuthorizationProviderFqi, response);
    return sink.status();
}

void AuthorizationConfigurationServiceImpl::setAuthorizationProvider(
    const authzconfig_proto::SetAuthorizationProvider_Parameters& request, CallContext&,
    ResponseSink<authzconfig_proto::SetAuthorizationProvider_Responses>& sink) {
    const std::string& provider = request.authorizationprovider().value();
    // Validate before any mutation: a malformed provider must not still
    // invalidate every issued token via store_.clear() below.
    if (auto lengthError = types::checkLength(provider, 36)) {
        throw error::ValidationError{kAuthorizationProviderParamFqi, *lengthError};
    }
    if (auto patternError = types::checkPattern(provider, kAuthorizationProviderPattern)) {
        throw error::ValidationError{kAuthorizationProviderParamFqi, *patternError};
    }
    config_.setAuthorizationProviderUuid(provider);
    // §3.11: provider change invalidates all cached tokens.
    store_.clear();
    sink.send(authzconfig_proto::SetAuthorizationProvider_Responses{});
    sink.finish();
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

grpc::Status AuthorizationConfigurationServiceImpl::Get_AuthorizationProvider(
    grpc::ServerContext* context,
    const authzconfig_proto::Get_AuthorizationProvider_Parameters* request,
    authzconfig_proto::Get_AuthorizationProvider_Responses* response) {
    GrpcUnaryResponseSink<authzconfig_proto::Get_AuthorizationProvider_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getAuthorizationProvider(req, ctx, out); },
        chain_, kGet_AuthorizationProviderFqi, response);
    return sink.status();
}

void AuthorizationConfigurationServiceImpl::getAuthorizationProvider(
    const authzconfig_proto::Get_AuthorizationProvider_Parameters&, CallContext&,
    ResponseSink<authzconfig_proto::Get_AuthorizationProvider_Responses>& sink) {
    authzconfig_proto::Get_AuthorizationProvider_Responses response;
    response.mutable_authorizationprovider()->set_value(config_.authorizationProviderUuid());
    sink.send(response);
    sink.finish();
}

}  // namespace sila2
