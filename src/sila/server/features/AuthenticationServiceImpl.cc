// AuthenticationServiceImpl.cc — Login/Logout token management (architecture.md §3.11)
#include "AuthenticationServiceImpl.h"

#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <sila/server/auth/FqiMatch.h>
#include <sila/server/config/ServerConfig.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/common/types/Constraints.h>
#include <sila/server/features/AuthenticationServiceFdl.h>
#include <sila/server/transport/GrpcTransport.h>
#include <sila/server/transport/InterceptorChain.h>

#include "SiLAFramework.pb.h"

#include <chrono>
#include <string>
#include <unordered_set>

namespace sila2 {
namespace {
const std::string kFdlXml = generated::kAuthenticationServiceFdlXml;

// FDL §DefinedExecutionError declares these two errors for AuthenticationService.
// ponytail: codegen will emit these as constants in AuthenticationServiceMeta (§2)
const std::string kAuthenticationFailedErrorId =
    "org.silastandard/core/AuthenticationService/v1/DefinedExecutionError/AuthenticationFailed";
const std::string kInvalidAccessTokenErrorId =
    "org.silastandard/core/AuthenticationService/v1/DefinedExecutionError/InvalidAccessToken";

// SiLAFramework.proto:96 requires a fully qualified parameter identifier here,
// not the bare <Parameter><Identifier> from the FDL.
const std::string kRequestedServerParamFqi =
    "org.silastandard/core/AuthenticationService/v1/Command/Login/Parameter/RequestedServer";

// SiLA has no per-element index syntax in a parameter identifier, so a
// malformed element of the RequestedFeatures list is reported against the
// list parameter itself, not against a numbered sub-position.
const std::string kRequestedFeaturesParamFqi =
    "org.silastandard/core/AuthenticationService/v1/Command/Login/Parameter/RequestedFeatures";

// AuthenticationService-v1_0.sila.xml:45-46 constrains RequestedServer to
// Length 36 plus a lowercase-hex UUID Pattern. Hyphens unescaped: the FDL
// spells them \- for XSD, which std::regex's ECMAScript grammar does not
// require outside a character class (mirrors AuthorizationConfiguration
// ServiceImpl.cc's kAuthorizationProviderPattern).
const std::string kServerUuidPattern =
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";

// Login grants tokens with a fixed lifetime; FDL exposes it back to the
// client via the TokenLifetime response but does not let the client request
// a different one.
const std::chrono::seconds kTokenLifetime{3600};

}  // namespace

const std::string& authenticationServiceFdlXml() { return kFdlXml; }

AuthenticationServiceImpl::AuthenticationServiceImpl(
    auth::AuthTokenStore& store,
    auth::CredentialVerifier& verifier,
    const auth::AccessPolicy& policy,
    const ServerConfig& config,
    const InterceptorChain* chain)
    : store_{store}, verifier_{verifier}, policy_{policy}, config_{config}, chain_{chain} {}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

grpc::Status AuthenticationServiceImpl::Login(
    grpc::ServerContext* context,
    const auth_proto::Login_Parameters* request,
    auth_proto::Login_Responses* response) {
    GrpcUnaryResponseSink<auth_proto::Login_Responses> sink(response);

    // Login is exempt from the access-token gate by construction: it is the
    // command that issues the token the gate checks for. Running it through
    // chain_->auth lets an operator who lists kAuthenticationServiceFqi in
    // protectedFqis lock every client out of the only path to a token, and
    // the rejection reads "No access token provided for protected feature",
    // which names the symptom and not the cause. DenyByDefaultAccessPolicy.h
    // already documents Login as pre-auth; clearing auth here makes that
    // structural instead of a naming convention nobody can enforce.
    // The rest of the chain is copied through, so Login still reaches the
    // dispatch log — the exemption drops the gate, not the audit trail.
    InterceptorChain loginChain = chain_ ? *chain_ : InterceptorChain{};
    loginChain.auth = nullptr;

    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { login(req, ctx, out); },
        &loginChain, kLoginFqi, response);
    return sink.status();
}

void AuthenticationServiceImpl::login(const auth_proto::Login_Parameters& request, CallContext&,
                                      ResponseSink<auth_proto::Login_Responses>& sink) {
    const std::string& userIdentification = request.useridentification().value();
    const std::string& password = request.password().value();
    const std::string& requestedServer = request.requestedserver().value();

    // Part B (Validation Error): a SiLA Server MUST apply the parameter
    // Constraints and throw a Validation Error before executing the Command,
    // so RequestedServer's Length/Pattern are checked ahead of verify() (the
    // execution) — mirrors AuthorizationConfigurationServiceImpl.cc:57-64.
    if (auto lengthError = types::checkLength(requestedServer, 36)) {
        throw error::ValidationError{kRequestedServerParamFqi, *lengthError};
    }
    if (auto patternError = types::checkPattern(requestedServer, kServerUuidPattern)) {
        throw error::ValidationError{kRequestedServerParamFqi, *patternError};
    }

    const std::optional<std::string> user = verifier_.verify(userIdentification, password);
    if (!user.has_value()) {
        // FDL §DefinedExecutionErrors/AuthenticationFailed
        throw error::DefinedExecutionError{
            kAuthenticationFailedErrorId,
            "The provided credentials are not valid"};
    }

    if (requestedServer != config_.uuid()) {
        throw error::ValidationError{
            kRequestedServerParamFqi,
            "Requested server UUID does not match this server"};
    }

    const std::vector<std::string> policyFqis = policy_.allowedFqis(*user);
    std::unordered_set<std::string> allowedFqis;
    if (request.requestedfeatures().empty()) {
        // No feature requested means "all features" (FDL §Parameter/RequestedFeatures).
        allowedFqis.insert(policyFqis.begin(), policyFqis.end());
    } else {
        for (const auto& requestedFqi : request.requestedfeatures()) {
            // AuthenticationService-v1_0.sila.xml:63 constrains each list
            // element to FullyQualifiedIdentifier/FeatureIdentifier. A
            // malformed element must surface as a ValidationError rather
            // than silently fail anyFqiCovers and degrade into a narrower
            // token scope than the client asked for (the audited failure).
            // Guard sits inside this loop rather than in a separate
            // pre-pass: allowedFqis is function-local and store_.issue()
            // has not run yet, so a mid-loop throw leaves no partial state
            // to unwind. (Contrast AuthorizationConfigurationServiceImpl.cc
            // :57-58, which validates before any mutation — there is a
            // mutation to protect against; here there is none yet.)
            if (auto fqiError = types::checkFullyQualifiedIdentifier(requestedFqi.value())) {
                throw error::ValidationError{kRequestedFeaturesParamFqi, *fqiError};
            }
            // Grant both coverage directions between the requested Feature and
            // each policy entry. A Command/Property policy entry does NOT cover
            // its parent Feature (fqiCovers is prefix-on-boundary, and the entry
            // is longer than the Feature), so matching only "policy covers
            // request" would drop every granular grant the moment a client
            // requests the Feature -- the token would then be refused by the
            // now-Command/Property-granular gate. So: a policy entry that covers
            // the requested Feature grants the whole Feature; a policy entry that
            // lies under it grants just that Command/Property.
            for (const auto& policyFqi : policyFqis) {
                if (auth::fqiCovers(policyFqi, requestedFqi.value())) {
                    allowedFqis.insert(requestedFqi.value());
                } else if (auth::fqiCovers(requestedFqi.value(), policyFqi)) {
                    allowedFqis.insert(policyFqi);
                }
            }
        }
    }

    const std::string token = store_.issue(*user, std::move(allowedFqis), kTokenLifetime);

    auth_proto::Login_Responses response;
    response.mutable_accesstoken()->set_value(token);
    response.mutable_tokenlifetime()->set_value(kTokenLifetime.count());
    sink.send(response);
    sink.finish();
}

grpc::Status AuthenticationServiceImpl::Logout(
    grpc::ServerContext* context,
    const auth_proto::Logout_Parameters* request,
    auth_proto::Logout_Responses* response) {
    GrpcUnaryResponseSink<auth_proto::Logout_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { logout(req, ctx, out); },
        chain_, kLogoutFqi, response);
    return sink.status();
}

void AuthenticationServiceImpl::logout(const auth_proto::Logout_Parameters& request, CallContext&,
                                       ResponseSink<auth_proto::Logout_Responses>& sink) {
    const std::string& accessToken = request.accesstoken().value();

    if (!store_.remove(accessToken)) {
        // FDL §DefinedExecutionErrors/InvalidAccessToken
        throw error::DefinedExecutionError{
            kInvalidAccessTokenErrorId,
            "The sent access token is not valid"};
    }

    sink.send(auth_proto::Logout_Responses{});
    sink.finish();
}

}  // namespace sila2
