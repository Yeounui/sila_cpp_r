// SiLAServerBase.cc
#include "SiLAServerBase.h"

#include <sila/common/tls/UntrustedTlsCredentials.h>
#include <sila/common/types/Constraints.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/common/util/uuid.h>
#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <sila/server/auth/FqiMatch.h>
#include <sila/server/binary/BinaryDownloadService.h>
#include <sila/server/binary/HybridBinaryStore.h>
#include <sila/server/binary/BinaryUploadService.h>
#include <sila/server/config/ServerConfig.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>
#include <sila/server/transport/cloud/CloudHandlerRegistration.h>
#include <sila/server/config/TlsConfig.h>
#include <sila/server/discovery/MdnsPublisher.h>
#include <sila/server/features/AuthenticationServiceImpl.h>
#include <sila/server/features/AuthorizationConfigurationServiceImpl.h>
#include <sila/server/features/AuthorizationServiceImpl.h>
#include <sila/server/features/ConnectionConfigurationServiceImpl.h>
#include <sila/server/features/LockControllerImpl.h>
#include <sila/server/recovery/ErrorRecoveryServiceImpl.h>
#include <sila/server/recovery/RecoverableErrorGate.h>
#include <sila/server/metadata/MetadataPolicy.h>
#include <sila/server/SiLAServiceImpl.h>
#include <sila/server/property/ObservablePropertyManager.h>

#include "ErrorRecoveryService.pb.h"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <any>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

// The gate's local spelling (MetadataPolicy.h, spelled there to keep the
// generated-adapter include graph free of SiLAService.grpc.pb.h) must never
// drift from the canonical one: a drift would silently disable rule (a) for
// the real SiLAService while WithMetadata kept rejecting the other spelling.
static_assert(sila2::kSiLAServiceFqi == sila2::kSiLAServiceFeatureFqi);

namespace sila2 {

namespace {

// The three FDL Patterns Build() validates a default-or-caller-supplied
// Identity against. Duplicated from the FDL into C++ string literals rather
// than parsed from it at runtime -- codegen-emitted constraint constants
// would remove the duplication but that is out of this item's scope.

// SiLAService-v1_0.sila.xml:126 -- ServerType Pattern.
const std::string kServerTypePattern = "[A-Z][a-zA-Z0-9]*";

// SiLAService-v1_0.sila.xml:144-147 -- ServerUUID Length and Pattern.
const std::string kServerUuidPattern =
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";

// SiLAService-v1_0.sila.xml:177 -- ServerVersion Pattern.
const std::string kServerVersionPattern =
    R"((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*))?(_[_a-zA-Z0-9]+)?)";

// SiLAService-v1_0.sila.xml:197 -- ServerVendorURL Pattern.
const std::string kServerVendorUrlPattern = "https?://.+";

// One throw site for all three identity checks in Build(), so the
// std::logic_error construction is written once instead of three times.
void requireIdentityPattern(const char* property, const std::string& value,
                             const std::string& pattern) {
    if (auto patternError = types::checkPattern(value, pattern)) {
        throw std::logic_error{
            "SiLAServerBase::Builder::Build: " + std::string{property} +
            " does not match its FDL Pattern: " + *patternError};
    }
}

// Reads the UUID persisted at `path`, or generates and writes a fresh one if
// the file does not exist yet (audit S30b). Conformance is delegated to
// InMemoryServerConfig's explicit-uuid constructor (S30a's checkLength +
// checkPattern pair) rather than duplicated here -- the constructed object
// itself is discarded, only its constructor's throw-or-not matters. A
// malformed or non-conformant file is converted into std::runtime_error
// naming the path, and the file is left untouched: a silent overwrite would
// replace the identity a client re-binds on, which is exactly what
// SiLAService-v1_0.sila.xml:134-137's stability obligation forbids.
std::string loadOrCreatePersistentUuid(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) {
        std::ifstream in{path};
        std::string storedUuid;
        std::getline(in, storedUuid);  // getline strips the tolerated trailing newline
        try {
            // "SiLAServer", not "SiLA Server": this object is discarded (only its
            // constructor's throw-or-not matters), but S66 keeps every default-name
            // literal in this file matching ServerConfig.h's default ServerType.
            InMemoryServerConfig{storedUuid, "SiLAServer"};
        } catch (const std::invalid_argument& badUuid) {
            throw std::runtime_error{
                "SiLAServerBase::Builder::WithPersistentUuid: " + path.string() +
                " does not hold a conformant ServerUUID: " + badUuid.what()};
        }
        return storedUuid;
    }

    std::string freshUuid = util::generateUuid();
    std::ofstream out{path};
    if (!out) {
        throw std::runtime_error{
            "SiLAServerBase::Builder::WithPersistentUuid: could not create " + path.string()};
    }
    out << freshUuid;
    out.flush();
    if (!out) {
        throw std::runtime_error{
            "SiLAServerBase::Builder::WithPersistentUuid: could not write " + path.string()};
    }
    return freshUuid;
}

}  // namespace

SiLAServerBase::OwnedComponents::OwnedComponents() = default;
SiLAServerBase::OwnedComponents::~OwnedComponents() = default;
SiLAServerBase::OwnedComponents::OwnedComponents(OwnedComponents&&) noexcept = default;
SiLAServerBase::OwnedComponents& SiLAServerBase::OwnedComponents::operator=(OwnedComponents&&) noexcept = default;

SiLAServerBase::SiLAServerBase(FeatureRegistry featureRegistry,
                                std::string certificatePem, std::string privateKeyPem,
                                std::string caCertPem,
                                std::unique_ptr<ServerConfig> config,
                                OwnedComponents components,
                                std::filesystem::path connectionConfigurationStorePath,
                                tls::OutboundCredentialsProvider connectionConfigurationCredentials,
                                uint16_t port)
    /* std::move: 힙 메모리를 가진 객체에서, 이동은 내부 포인터만 넘기고 원본을 빈 상태로 만듦. 깊은 복사(할당+memcpy) 생략.
       std::move로 멤버에 이동하면 추가 복사 없이 포인터만 옮김.
        멤버.ptr = 매개변수.ptr;
        멤버.len = 매개변수.len;
        매개변수.ptr = nullptr;
        매개변수.len = 0;
       const&로 받으면 멤버 초기화 시 반드시 복사가 발생한다.

       Uniform Initialization(중괄호): {}은 ()와 같은 뜻이나, 축소 변환(double→int 등)을 컴파일 타임에 차단.
        int x(3.14);   // OK — 3으로 잘림 (경고만)
        int x{3.14};   // 컴파일 에러 — double→int 축소 불허
       주의: {}가 initializer_list 생성자를 우선 매칭하기에 initializer_list 생성자를 가진 타입은 차이 있음.
        std::vector<int> v(3, 0);   // 원소 3개: {0, 0, 0}
        std::vector<int> v{3, 0};   // 원소 2개: {3, 0}
    */
    : featureRegistry_{std::move(featureRegistry)},
      certificatePem_{std::move(certificatePem)},
      privateKeyPem_{std::move(privateKeyPem)},
      caCertPem_{std::move(caCertPem)},
      config_{std::move(config)},
      // featureRegistry_ and config_ are initialized above, safe to reference.
      // components (parameter) has not been moved yet — silaService_ is declared
      // before components_ in the class, so its initializer runs first.
      silaService_{std::make_shared<SiLAServiceImpl>(featureRegistry_, *config_,
                                                      components.mdnsPublisher.get(),
                                                      components.chain.get())},
      components_{std::move(components)},
      port_{port} {
    featureRegistry_.registerService(std::string{kSiLAServiceFqi}, silaService_);

    // Built here, not in Build(): the router built in Build() would reference
    // the Builder's featureRegistry_, which dangles once moved into the
    // member above. Recreating with featureRegistry_ (already moved-into)
    // keeps the reference valid for the server's lifetime.
    components_.cloudRouter = std::make_unique<CloudEnvelopeRouter>(
        featureRegistry_, components_.chain.get(),
        components_.binaryStore.get(), components_.commandManagers,
        config_->cloudWriteTimeout(), config_->maxConcurrentCloudSubscriptions());
    auto& r = *components_.cloudRouter;

    // Part A p32 (SHALL): every SiLA Server conforming to SiLA 2 Version >=
    // "1.1" SHALL support the Server-Initiated Connection Method, indicated
    // by offering this Feature (Part A p80 SHALL: implement it). Build()
    // guarantees connectionConfigurationCredentials is never null here --
    // either the caller's WithConnectionConfiguration, or Build()'s own
    // default derivation -- so the 4-arg constructor is the only one needed.
    components_.connectionConfigurationService =
        std::make_shared<ConnectionConfigurationServiceImpl>(
            r, std::move(connectionConfigurationCredentials), components_.chain.get(),
            std::move(connectionConfigurationStorePath));
    featureRegistry_.registerService(
        std::string{kConnectionConfigurationServiceFqi},
        components_.connectionConfigurationService);

    std::string silaFqi{kSiLAServiceFqi};
    regCmd(r, silaFqi, "GetFeatureDefinition", silaService_, &SiLAServiceImpl::getFeatureDefinition);
    regCmd(r, silaFqi, "SetServerName",        silaService_, &SiLAServiceImpl::setServerName);
    regProp(r, silaFqi, "ServerName",          silaService_, &SiLAServiceImpl::getServerName);
    regProp(r, silaFqi, "ServerType",          silaService_, &SiLAServiceImpl::getServerType);
    regProp(r, silaFqi, "ServerUUID",          silaService_, &SiLAServiceImpl::getServerUuid);
    regProp(r, silaFqi, "ServerDescription",   silaService_, &SiLAServiceImpl::getServerDescription);
    regProp(r, silaFqi, "ServerVersion",       silaService_, &SiLAServiceImpl::getServerVersion);
    regProp(r, silaFqi, "ServerVendorURL",     silaService_, &SiLAServiceImpl::getServerVendorUrl);
    regProp(r, silaFqi, "ImplementedFeatures", silaService_, &SiLAServiceImpl::getImplementedFeatures);

    // Unconditional now (Part A p80): the service always exists, so both the
    // direct-gRPC service base (registerService above) and these cloud handlers
    // are wired on every server.
    std::string connectionConfigurationFqi{kConnectionConfigurationServiceFqi};
    regCmd(r, connectionConfigurationFqi, "EnableServerInitiatedConnectionMode",
           components_.connectionConfigurationService,
           &ConnectionConfigurationServiceImpl::enableServerInitiatedConnectionMode);
    regCmd(r, connectionConfigurationFqi, "DisableServerInitiatedConnectionMode",
           components_.connectionConfigurationService,
           &ConnectionConfigurationServiceImpl::disableServerInitiatedConnectionMode);
    regCmd(r, connectionConfigurationFqi, "ConnectSiLAClient",
           components_.connectionConfigurationService,
           &ConnectionConfigurationServiceImpl::connectSiLAClient);
    regCmd(r, connectionConfigurationFqi, "DisconnectSiLAClient",
           components_.connectionConfigurationService,
           &ConnectionConfigurationServiceImpl::disconnectSiLAClient);
    regProp(r, connectionConfigurationFqi, "ServerInitiatedConnectionModeStatus",
            components_.connectionConfigurationService,
            &ConnectionConfigurationServiceImpl::getServerInitiatedConnectionModeStatus);
    regProp(r, connectionConfigurationFqi, "ConfiguredSiLAClients",
            components_.connectionConfigurationService,
            &ConnectionConfigurationServiceImpl::getConfiguredSiLAClients);

    if (components_.authService) {
        std::string authFqi{kAuthenticationServiceFqi};
        regCmd(r, authFqi, "Login",  components_.authService, &AuthenticationServiceImpl::login);
        regCmd(r, authFqi, "Logout", components_.authService, &AuthenticationServiceImpl::logout);
    }

    if (components_.authzConfigService) {
        std::string authzFqi{kAuthorizationConfigurationServiceFqi};
        regCmd(r, authzFqi, "SetAuthorizationProvider",
               components_.authzConfigService, &AuthorizationConfigurationServiceImpl::setAuthorizationProvider);
        regProp(r, authzFqi, "AuthorizationProvider",
                components_.authzConfigService, &AuthorizationConfigurationServiceImpl::getAuthorizationProvider);
    }

    if (components_.authzService) {
        std::string authzSvcFqi{kAuthorizationServiceFqi};
        regProp(r, authzSvcFqi, "FCPAffectedByMetadata_AccessToken",
                components_.authzService, &AuthorizationServiceImpl::getFcpAffectedByMetadataAccessToken);
    }

    if (components_.errorRecoveryService) {
        std::string errFqi{kErrorRecoveryServiceFqi};
        regCmd(r, errFqi, "ExecuteContinuationOption",
               components_.errorRecoveryService, &ErrorRecoveryServiceImpl::executeContinuationOption);
        regCmd(r, errFqi, "AbortErrorHandling",
               components_.errorRecoveryService, &ErrorRecoveryServiceImpl::abortErrorHandling);
        regCmd(r, errFqi, "SetErrorHandlingTimeout",
               components_.errorRecoveryService, &ErrorRecoveryServiceImpl::setErrorHandlingTimeout);

        if (components_.errorRecoveryPropMgr) {
            namespace erp = sila2::org::silastandard::core::errorrecoveryservice::v2;
            r.registerObservableProperty(
                errFqi + "/Property/RecoverableErrors",
                recovery::kRecoverableErrorsPropertyId,
                components_.errorRecoveryPropMgr.get(),
                [](const std::any& val) -> std::string {
                    // Gate now publishes full RecoverableError structs, not bare
                    // message strings; share the serializer with the direct-gRPC
                    // path (ErrorRecoveryServiceImpl.cc) so both transports emit
                    // the same wire shape.
                    const auto& errors =
                        std::any_cast<const std::vector<recovery::RecoverableError>&>(val);
                    erp::Subscribe_RecoverableErrors_Responses response;
                    fillRecoverableErrorsResponse(errors, response);
                    return response.SerializeAsString();
                });
        }
    }

    if (components_.lockController) {
        // Without these four the lock is enforceable over cloud but not
        // ACQUIRABLE over cloud: a cloud-only client would be refused by a gate
        // it has no way to satisfy, since LockServer would have no cloud handler.
        std::string lockFqi{kLockControllerFqi};
        regCmd(r, lockFqi, "LockServer", components_.lockController,
               &LockControllerImpl::lockServer);
        regCmd(r, lockFqi, "UnlockServer", components_.lockController,
               &LockControllerImpl::unlockServer);
        regProp(r, lockFqi, "IsLocked", components_.lockController,
                &LockControllerImpl::getIsLocked);
        regProp(r, lockFqi, "FCPAffectedByMetadata_LockIdentifier", components_.lockController,
                &LockControllerImpl::getFcpAffectedByMetadataLockIdentifier);
    }

    auto* router = components_.cloudRouter.get();
    auto* chain = components_.chain.get();
    for (auto* mgr : components_.commandManagers) {
        // Cloud: drop the execution's FQI+token snapshot (executionFqis_) when
        // the manager GCs the execution.
        mgr->addRemovalObserver(
            [router](const std::string& uuid) { router->removeExecutionFQI(uuid); });
        // gRPC (Batch C, High-2): drop the transport owner-registry entry on the
        // same GC signal. The InterceptorChain is per-server and long-lived, so
        // without this the uuid->owner-FQI map would grow unbounded. Same
        // lifecycle as the cloud observer above, a different map.
        mgr->addRemovalObserver(
            [chain](const std::string& uuid) { chain->eraseObservableOwner(uuid); });
    }
}

void SiLAServerBase::stopCommandManagerGC() {
    for (auto* mgr : components_.commandManagers) {
        mgr->stopAutoGC();
        // Also drop the observers themselves: after this point cloudRouter and
        // the InterceptorChain may be destroyed while mgr (application-owned) is
        // still alive, so observers capturing raw pointers to them must not
        // remain reachable even if something were to call startAutoGC() again on
        // mgr directly.
        mgr->clearRemovalObservers();
    }
}

// Defined here (not = default in header) so that the implicit
// unique_ptr<grpc::Server> destructor sees the complete type.
SiLAServerBase::~SiLAServerBase() {
    // Shutdown(), not just stopCommandManagerGC(): members die in reverse
    // declaration order, components_ before server_, so errorRecoveryPropMgr
    // would be destroyed while a Subscribe_RecoverableErrors handler still
    // holds it by reference, and only then would ~unique_ptr<grpc::Server>
    // run its own no-deadline shutdown and wait for that same handler --
    // a use-after-free followed by a hang. Shutdown() cancels and joins
    // first. Idempotent, so this is a no-op after an explicit Shutdown().
    Shutdown();
}
// Explicit: chain_ must be heap-allocated upfront so chain() returns a
// stable pointer that adapters can capture before Build() populates it.
SiLAServerBase::Builder::Builder()
    : logCallback_{defaultLogCallback()},
      chain_{std::make_unique<InterceptorChain>()} {}
SiLAServerBase::Builder::~Builder() = default;

void SiLAServerBase::Run(bool block) {
    grpc::ServerBuilder builder;
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    grpc::SslServerCredentialsOptions ssl_opts;
    ssl_opts.pem_key_cert_pairs.push_back(
        {privateKeyPem_, certificatePem_});
    if (!caCertPem_.empty()) {
        ssl_opts.pem_root_certs = caCertPem_;
        ssl_opts.client_certificate_request =
            GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
    }
    auto credentials = grpc::SslServerCredentials(ssl_opts);

    // selected_port out-param: with port_ == 0 the OS picks a free port and
    // BuildAndStart writes it back here. Without it a caller has no way to
    // learn the bound port, which forced every harness onto hard-coded ports.
    int selectedPort = 0;
    builder.AddListeningPort("0.0.0.0:" + std::to_string(port_), credentials,
                             &selectedPort);
    builder.SetMaxReceiveMessageSize(kMaxReceiveMessageSizeBytes);
    // Keepalive for *inbound* client connections — observable-property and
    // command-info subscriptions idle for hours. It does not cover the
    // server-initiated cloud stream, which rides the outbound channel in
    // CloudTransport::openStream; that file carries the matching values and
    // the two must change together.
    // MIN_RECV_PING_INTERVAL_WITHOUT_DATA is the server half of the contract:
    // it must stay <= a peer's KEEPALIVE_TIME, or gRPC answers a conforming
    // client's pings with GOAWAY/ENHANCE_YOUR_CALM after 2 strikes.
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 60000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    builder.AddChannelArgument(
        GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 30000);

    for (auto* service : featureRegistry_.registeredServices()) {
        builder.RegisterService(service);
    }

    // Part A p32/p80: the service is always present and always configured
    // (Build() supplies default credentials/store when the caller did not),
    // so this reconnects every persisted client on every server.
    components_.connectionConfigurationService->connectPersistentClients();

    server_ = builder.BuildAndStart();
    if (!server_) {
        throw std::runtime_error{
            "SiLAServerBase::Run: gRPC server failed to start — "
            "check port availability and TLS credentials"};
    }

    // Written by BuildAndStart, not by AddListeningPort, so it is only
    // meaningful once the server actually started. Unchanged from port_ when
    // a non-zero port was configured.
    port_ = static_cast<uint16_t>(selectedPort);

    // Advertise only now: port_ above is the port the listener actually holds,
    // and BuildAndStart has already succeeded, so every SRV record this sends
    // names a socket that accepts connections. A Run() that threw above leaves
    // the publisher silent, which is the correct outcome for a server that
    // never bound.
    if (components_.mdnsPublisher) {
        try {
            components_.mdnsPublisher->setPort(port_);
            components_.mdnsPublisher->publish();
        } catch (const std::exception& e) {
            // publish() throwing (no mDNS socket could open) must not tear down
            // a gRPC server that already started: callers would see Run() fail
            // half-done and might retry over a live server_. The server stays
            // reachable by direct dial, only undiscoverable.
            logEvent(components_.chain->logCallback, LogLevel::kWarning, "discovery",
                     std::string{"mDNS publish failed, serving without discovery: "} + e.what());
        }
    }

    if (block) {
        server_->Wait();
    }
}

void SiLAServerBase::Shutdown() {
    for (auto* mgr : components_.commandManagers) {
        mgr->interruptAll();
    }
    // Command managers are application-owned and can outlive this server, so
    // Shutdown() must drop the callback into cloudRouter (which does not
    // outlive this server) before returning. binaryStore's and
    // authTokenStore's GC are server-owned components with no such dangling
    // risk, so they are left running until ~SiLAServerBase.
    stopCommandManagerGC();
    if (components_.mdnsPublisher) {
        components_.mdnsPublisher->shutdown();
    }
    // Must run before server_->Shutdown(): a Subscribe_RecoverableErrors
    // handler parks in Subscription::waitForNext(), on a condition variable
    // gRPC knows nothing about, so grpc::Server::Shutdown cannot wake it --
    // with or without a deadline -- and its closing wait for handler threads
    // would never return (audit 3.2m). Cancelling the subscriptions is what
    // unparks it: waitForNext()'s predicate checks cancelled_ first and
    // returns nullopt, which breaks the handler's loop. Same cancel-then-join
    // shape as ~CloudEnvelopeRouter. ObservablePropertyManager::shutdown() is
    // a different function from this one despite the name.
    if (components_.errorRecoveryPropMgr) {
        components_.errorRecoveryPropMgr->shutdown();
    }
    // Part A p32/p80: the service is always present and always configured;
    // shutdown() disconnects whatever clients it holds (empty on a server
    // that never enabled server-initiated mode).
    components_.connectionConfigurationService->shutdown();
    if (server_) {
        server_->Shutdown();
    }
}
/*  const FeatureRegistry: 반환값 수정 못하게.
    const {...}: 메서드 내 멤버 수정 못하게.
*/
const FeatureRegistry& SiLAServerBase::featureRegistry() const { return featureRegistry_; }
const std::string& SiLAServerBase::certificatePem() const { return certificatePem_; }
const std::string& SiLAServerBase::privateKeyPem() const { return privateKeyPem_; }
ServerConfig& SiLAServerBase::serverConfig() { return *config_; }
const ServerConfig& SiLAServerBase::serverConfig() const { return *config_; }
CloudEnvelopeRouter* SiLAServerBase::cloudRouter() { return components_.cloudRouter.get(); }
ObservablePropertyManager* SiLAServerBase::errorRecoveryPropertyManager() { return components_.errorRecoveryPropMgr.get(); }
const discovery::MdnsPublisher* SiLAServerBase::mdnsPublisher() const { return components_.mdnsPublisher.get(); }
uint16_t SiLAServerBase::port() const { return port_; }

SiLAServerBase::Builder& SiLAServerBase::Builder::AddFeature(
    std::string fqi, std::string fdlXml, std::shared_ptr<grpc::Service> service) {
    featureRegistry_.registerFeature(fqi, std::move(fdlXml));
    if (service) {
        featureRegistry_.registerService(fqi, std::move(service));
    }
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithConfig(
    std::unique_ptr<ServerConfig> config) {
    config_ = std::move(config);
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithPersistentUuid(std::filesystem::path path) {
    persistentUuidPath_ = std::move(path);
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithSelfSignedCertificate(
    std::string hostname, std::string ip) {
    // Defer key+cert generation to Build(), where the server UUID is known
    // and can be embedded as the OID extension (Part B p75 RECOMMENDED).
    selfSignedParams_ = SelfSignedParams{std::move(hostname), std::move(ip)};
    // Clear any WithCertificate material so last-write-wins and Build()
    // retry after a late failure does not see stale cert data.
    certificatePem_.clear();
    privateKeyPem_.clear();
    discoveryCaCertPem_.clear();
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithCertificate(
    std::string certificatePem, std::string privateKeyPem,
    std::string caCertPemForDiscovery) {
    certificatePem_ = std::move(certificatePem);
    privateKeyPem_ = std::move(privateKeyPem);
    discoveryCaCertPem_ = std::move(caCertPemForDiscovery);
    selfSignedParams_.reset();
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithMutualTls(std::string caCertPem) {
    caCertPem_ = std::move(caCertPem);
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithBinaryTransfer() {
    enableBinaryTransfer_ = true;
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithAuthentication(
    std::unique_ptr<auth::CredentialVerifier> verifier,
    std::unique_ptr<auth::AccessPolicy> policy,
    std::vector<std::string> protectedFqis) {
    if (!verifier) {
        throw std::invalid_argument{
            "SiLAServerBase::Builder::WithAuthentication: verifier must not be null"};
    }
    if (!policy) {
        throw std::invalid_argument{
            "SiLAServerBase::Builder::WithAuthentication: policy must not be null"};
    }
    // protectedFqis accepts Feature, Command, and Property FQIs: the codegen
    // upgrade makes the gRPC path gate at Command/Property granularity (symmetric
    // with cloud), so a sub-feature entry now enforces correctly rather than
    // under-enforcing. Metadata is refused: a Binary-typed Metadata FQI gates only
    // CreateBinary (isKnownParameterFqi accepts it) while UploadChunk/DeleteBinary
    // fall back to the coarser BinaryUpload FQI, so a Metadata entry would enforce
    // partially and inconsistently -- fail loud until that lifecycle is defined.
    for (const auto& entry : protectedFqis) {
        if (auth::isMetadataFqi(entry)) {
            throw std::invalid_argument{
                "SiLAServerBase::Builder::WithAuthentication: protectedFqis entry '"
                + entry + "' names a Metadata item; Metadata-granular authorization "
                "is not supported -- use its Feature, Command, or Property FQI."};
        }
    }
    authConfig_ = AuthConfig{std::move(verifier), std::move(policy),
                             std::move(protectedFqis)};
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithMetadata(
    std::string metadataFqi, std::vector<std::string> affectedCalls) {
    for (const auto& entry : affectedCalls) {
        // Part A: the affected list "MUST NOT contain any Commands or
        // Properties of the SiLA Service Feature or the SiLA Service Feature
        // Identifier itself". Refused rather than silently dropped: a list
        // naming SiLAService tells the client to make a call whose only
        // possible outcome is NO_METADATA_ALLOWED, and that is a Feature bug
        // worth failing startup over -- the gate itself is already immune
        // (MetadataPolicy.h returns before the loop). fqiCovers, not ==, so a
        // Command-granular entry under SiLAService is caught too.
        if (auth::fqiCovers(kSiLAServiceFqi, entry)) {
            throw std::invalid_argument{
                "SiLAServerBase::Builder::WithMetadata: affectedCalls entry '"
                + entry + "' is part of the SiLAService Feature, which must never"
                          " be affected by SiLA Client Metadata."};
        }
    }
    declaredMetadata_[std::move(metadataFqi)] = std::move(affectedCalls);
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithLock() {
    enableLock_ = true;
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithErrorRecovery() {
    enableErrorRecovery_ = true;
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithConnectionConfiguration(
    std::filesystem::path storePath,
    std::shared_ptr<grpc::ChannelCredentials> outboundCredentials) {
    if (storePath.empty()) {
        throw std::invalid_argument{
            "SiLAServerBase::Builder::WithConnectionConfiguration: storePath must not be empty"};
    }
    if (!outboundCredentials) {
        throw std::invalid_argument{
            "SiLAServerBase::Builder::WithConnectionConfiguration: outboundCredentials must not be null"};
    }
    connectionConfigurationConfig_ = {
        std::move(storePath), std::move(outboundCredentials)};
    return *this;
}

tls::OutboundCredentialsProvider SiLAServerBase::Builder::defaultOutboundCredentials(
    std::string certificatePem, std::string privateKeyPem, std::string caCertPem) {
    // Part A p32 default. This server's own certificate is always presented
    // as identity (CloudClientListener on the client side binds the connection
    // to the server's UUID). Peer trust follows Part B p74/p75 exactly as the
    // client leg does in ClientConfig::channelCredentials: a configured CA is
    // the trust anchor for any host; without one, an untrusted certificate is
    // accepted only inside a private-range network, and every other target is
    // refused (null) so this Command cannot be used to make the server dial an
    // arbitrary endpoint and trust whatever answers (Codex review, 2026-09-04).
    return [certificatePem = std::move(certificatePem), privateKeyPem = std::move(privateKeyPem),
            caCertPem = std::move(caCertPem)](std::string_view host)
               -> std::shared_ptr<grpc::ChannelCredentials> {
        if (!caCertPem.empty()) {
            grpc::SslCredentialsOptions ssl_opts;
            ssl_opts.pem_root_certs = caCertPem;
            ssl_opts.pem_private_key = privateKeyPem;
            ssl_opts.pem_cert_chain = certificatePem;
            return grpc::SslCredentials(ssl_opts);
        }
        if (tls::isPrivateAddress(host)) {
            return tls::untrustedTlsChannelCredentials(certificatePem, privateKeyPem);
        }
        return nullptr;
    };
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithDiscovery(uint16_t port) {
    discoveryPort_ = port;
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::RegisterCommandManager(
    ObservableCommandManager* mgr) {
    // GC is started in Build(), not here — see this method's doc comment.
    commandManagers_.push_back(mgr);
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::setLogCallback(LogCallback cb) {
    logCallback_ = std::move(cb);
    return *this;
}

const InterceptorChain* SiLAServerBase::Builder::chain() const {
    return chain_.get();
}

SiLAServerBase SiLAServerBase::Builder::Build() {
    if (!selfSignedParams_ && (certificatePem_.empty() || privateKeyPem_.empty())) {
        throw std::logic_error{
            "SiLAServerBase::Builder::Build: TLS material required — call "
            "WithSelfSignedCertificate or WithCertificate first"};
    }

    // WithConfig and WithPersistentUuid each supply their own uuid source, so
    // calling both is ambiguous about which one should win. config_ is still
    // nullptr here unless WithConfig set it, which is the only signal
    // available to detect the conflict.
    if (persistentUuidPath_ && config_) {
        throw std::logic_error{
            "SiLAServerBase::Builder::Build: WithConfig and WithPersistentUuid "
            "are mutually exclusive"};
    }

    if (!config_) {
        if (persistentUuidPath_) {
            const std::string persistedUuid = loadOrCreatePersistentUuid(*persistentUuidPath_);
            // Part B p77 SHOULD: "By default this name SHOULD be equal to the
            // SiLA Server Type" -- ServerConfig.h's default serverType is
            // "SiLAServer", so the default ServerName mirrors it.
            config_ = std::make_unique<InMemoryServerConfig>(persistedUuid, "SiLAServer");
        } else {
            // No identity source. A freshly generated UUID here would differ on
            // every boot, violating SiLAService-v1_0.sila.xml:134-137's "generated
            // once and remain the same for all times" obligation -- and doing so
            // silently, which is the worst failure mode. Refuse instead: the caller
            // owns where the UUID is persisted (WithConfig injects a caller-stored
            // one, WithPersistentUuid delegates the file to this library).
            throw std::logic_error{
                "SiLAServerBase::Builder::Build: no server identity -- call WithConfig "
                "with a caller-persisted ServerUUID, or WithPersistentUuid(path). A UUID "
                "generated fresh each boot violates SiLAService-v1_0.sila.xml:134-137."};
        }
    }

    // Deferred self-signed certificate generation: now that config_ is
    // settled, the server UUID can be embedded in the certificate's OID
    // extension (Part B p75 RECOMMENDED: OID 1.3.6.1.4.1.58583).
    if (selfSignedParams_) {
        const auto key = generateKey();
        const auto certificate = generateCertificate(
            key, selfSignedParams_->hostname, selfSignedParams_->ip, config_->uuid());
        privateKeyPem_ = keyToPem(key);
        certificatePem_ = certificateToPem(certificate);
        discoveryCaCertPem_ = certificatePem_;
    }

    // Checked here, not in ServerConfig's constructors: Build() is the one
    // seam every ServerConfig implementation -- present and future -- passes
    // through, and it is the last point at which these values are still
    // ours to reject.
    const std::string serverUuid = config_->uuid();
    if (auto lengthError = types::checkLength(serverUuid, 36)) {
        throw std::logic_error{
            "SiLAServerBase::Builder::Build: ServerUUID " + *lengthError};
    }
    requireIdentityPattern("ServerUUID", serverUuid, kServerUuidPattern);

    if (auto lengthError = types::checkMaximalLength(config_->serverType(), 255)) {
        throw std::logic_error{
            "SiLAServerBase::Builder::Build: ServerType " + *lengthError};
    }
    requireIdentityPattern("ServerType", config_->serverType(), kServerTypePattern);
    requireIdentityPattern("ServerVersion", config_->version(), kServerVersionPattern);
    requireIdentityPattern("ServerVendorURL", config_->vendorUrl(), kServerVendorUrlPattern);

    // ServerName's MaximalLength 255 (SiLAService-v1_0.sila.xml:107) is enforced
    // on the SetServerName path, but the *initial* name injected through WithConfig
    // never passes through that command -- Build() is its only conformance seam
    // (audit #6). checkMaximalLength counts Unicode code points, matching
    // SetServerName's own check so both paths reject the same values.
    if (auto lengthError = types::checkMaximalLength(config_->name(), 255)) {
        throw std::logic_error{
            "SiLAServerBase::Builder::Build: ServerName " + *lengthError};
    }

    // ponytail: codegen will emit this in SiLAServiceMeta (§2); explicit call removed then
    featureRegistry_.registerFeature(std::string{kSiLAServiceFqi}, silaServiceFdlXml());
    // Part A p32 SHALL support + p80 SHALL implement: unconditional, like
    // SiLAService above. The FDL is advertised whether or not
    // WithConnectionConfiguration was called, so the Feature appears in
    // ListImplementedFeatures / GetFeatureDefinition on every 1.1 server.
    featureRegistry_.registerFeature(
        std::string{kConnectionConfigurationServiceFqi},
        connectionConfigurationServiceFdlXml());

    OwnedComponents components;

    if (enableBinaryTransfer_) {
        auto slotLifetime = config_->binarySlotLifetime();
        components.binaryStore = std::make_unique<HybridBinaryStore>(
            config_->binarySpoolThreshold(),
            std::filesystem::temp_directory_path() / ("sila2-spool-" + config_->uuid()));
        // Without this sweep, slots the client never deletes live until process
        // exit. Same interval as the token store; ~BinaryStore stops the thread.
        components.binaryStore->startAutoGC(std::chrono::seconds{60});
        components.uploadService = std::make_shared<BinaryUploadService>(
            *components.binaryStore, slotLifetime, chain_.get());
        components.downloadService = std::make_shared<BinaryDownloadService>(
            *components.binaryStore, slotLifetime, chain_.get());
        featureRegistry_.registerService(
            "org.silastandard/core/BinaryUpload/v1", components.uploadService);
        featureRegistry_.registerService(
            "org.silastandard/core/BinaryDownload/v1", components.downloadService);
    }

    if (authConfig_) {
        components.credentialVerifier = std::move(authConfig_->verifier);
        components.accessPolicy = std::move(authConfig_->policy);
        components.authTokenStore = std::make_unique<auth::AuthTokenStore>();
        components.authTokenStore->startAutoGC(std::chrono::seconds{60});

        components.authService = std::make_shared<AuthenticationServiceImpl>(
            *components.authTokenStore, *components.credentialVerifier,
            *components.accessPolicy, *config_, chain_.get());
        // Copies rather than moves: the gate below still reads
        // authConfig_->protectedFqis, and the list is a handful of short
        // strings built once at Build().
        components.authzService = std::make_shared<AuthorizationServiceImpl>(
            authConfig_->protectedFqis, chain_.get());
        components.authzConfigService =
            std::make_shared<AuthorizationConfigurationServiceImpl>(
                *components.authTokenStore, *config_, chain_.get());

        // isProtected is derived from the explicit FQI list, not the access
        // policy — a permissive policy (AllowAll) must not disable the auth gate.
        // Coverage, not exact membership: both transports now hand intercept()
        // a "<feature>/Command/<Name>" / "<feature>/Property/<Name>" FQI (gRPC
        // from the generated adapter, service_adapter.h.j2:26-28; cloud from
        // CloudHandlerRegistration.h:102) or a parameter identifier
        // (CloudEnvelopeRouter.cc:618), so a feature-level entry must still
        // cover the call it names rather than match it exactly (§3.1s).
        components.authzInterceptor = std::make_unique<auth::AuthorizationInterceptor>(
            *components.authTokenStore,
            [protectedFqis = authConfig_->protectedFqis](const std::string& fqi) {
                return auth::anyFqiCovers(protectedFqis, fqi);
            });

        featureRegistry_.registerFeature(
            std::string{kAuthenticationServiceFqi}, authenticationServiceFdlXml());
        featureRegistry_.registerService(
            std::string{kAuthenticationServiceFqi}, components.authService);
        featureRegistry_.registerFeature(
            std::string{kAuthorizationServiceFqi}, authorizationServiceFdlXml());
        featureRegistry_.registerService(
            std::string{kAuthorizationServiceFqi}, components.authzService);
        featureRegistry_.registerFeature(
            std::string{kAuthorizationConfigurationServiceFqi},
            authorizationConfigurationServiceFdlXml());
        featureRegistry_.registerService(
            std::string{kAuthorizationConfigurationServiceFqi},
            components.authzConfigService);
    }

    if (enableErrorRecovery_) {
        components.errorRecoveryPropMgr = std::make_unique<ObservablePropertyManager>(
            config_->subscriptionQueueDepth());
        components.errorGate = std::make_unique<recovery::RecoverableErrorGate>(
            *components.errorRecoveryPropMgr, config_->errorHandlingTimeout());
        components.errorRecoveryService = std::make_shared<ErrorRecoveryServiceImpl>(
            *components.errorGate, *components.errorRecoveryPropMgr, chain_.get());

        featureRegistry_.registerFeature(
            std::string{kErrorRecoveryServiceFqi}, errorRecoveryServiceFdlXml());
        featureRegistry_.registerService(
            std::string{kErrorRecoveryServiceFqi}, components.errorRecoveryService);
        // v1 FDL is not advertised: no v1 gRPC service exists behind it, and
        // its RecoverableError shape differs from v2's (S8b, architecture-v2.md §3.12).
    }

    if (enableLock_) {
        components.lockController = std::make_shared<LockControllerImpl>(chain_.get());
        featureRegistry_.registerFeature(std::string{kLockControllerFqi}, lockControllerFdlXml());
        featureRegistry_.registerService(std::string{kLockControllerFqi}, components.lockController);
    }

    uint16_t port = discoveryPort_.value_or(50051);

    // Part B p75 MUST: "the SiLA Server MUST have SiLA Server Discovery
    // enabled by default. It MUST NOT be possible to disable any part of
    // SiLA Server Discovery." -- constructed unconditionally; WithDiscovery
    // only overrides `port` above, it cannot suppress this. The instance
    // name is the ServerUUID, not ServerName (Part B p76 MUST), so uuid is
    // passed first; serverName/description feed the server_name/description
    // TXT records (Part B p77 SHOULD) and discoveryCaCertPem_ feeds the
    // ca<l>= TXT records for an untrusted certificate (Part B p75-76 MUST).
    components.mdnsPublisher = std::make_unique<discovery::MdnsPublisher>(
        config_->uuid(), config_->name(), config_->description(), discoveryCaCertPem_,
        port, config_->mdnsReadvertiseInterval(), config_->mdnsRecordTtl(),
        config_->mdnsProbeWait());
    // Constructed here (SiLAServiceImpl below captures this pointer for
    // SetServerName's mDNS rename) but deliberately NOT published here:
    // the SRV port is only final after Run()'s BuildAndStart, so
    // publishing now would advertise the requested port before anything
    // is listening on it, and the literal 0 for a WithDiscovery(0)
    // server (audit S31).

    // Populate interceptor chain — adapters already hold chain() pointer;
    // the heap address is stable across the move into OwnedComponents.
    chain_->auth = components.authzInterceptor.get();
    chain_->binaryStore = components.binaryStore.get();
    chain_->binarySlotLifetime = config_->binarySlotLifetime();
    chain_->logCallback = std::move(logCallback_);
    // Snapshot before featureRegistry_ is moved into SiLAServerBase below —
    // every built-in Feature (SiLAService above, auth/error-recovery Features
    // above) is already registered by this point, so CreateBinary's parameter-
    // FQI gate (BinaryUploadService.cc) sees the complete list.
    chain_->registeredFeatureFqis = featureRegistry_.registeredFeatureIdentifiers();
    // Same snapshot discipline as registeredFeatureFqis above, for the same
    // reason: Part A makes the affected list immutable for the server's
    // lifetime, so it is fixed here and read lock-free by both the admission
    // gate and cloud FCP discovery.
    chain_->metadataAffectedCalls = std::move(declaredMetadata_);
    if (authConfig_) {
        // The access token is SiLA Client Metadata like any other and discovery
        // has to name the calls it applies to. Its presence is enforced by
        // AuthorizationInterceptor, not by the gate, which skips this row --
        // see MetadataPolicy.h.
        chain_->metadataAffectedCalls[kAccessTokenMetadataFqi] = authConfig_->protectedFqis;
    }
    if (components.lockController) {
        // Feature granularity, matching what registeredFeatureFqis holds and what
        // the reference implementation advertises
        // (sila_python lockcontroller_impl.py:66-71, which excludes the same two).
        // LockController itself: IsLocked "MUST NOT be lock protected"
        // (LockController-v1_0.sila.xml:97-98, v2_0:97) and LockServer/UnlockServer
        // take the identifier as a Parameter (:29-39, :78-85), so no FCP of this
        // Feature is affected by the metadata. SiLAService: Part A's affected list
        // "MUST NOT contain ... the SiLA Service Feature Identifier itself", which
        // is also why WithMetadata refuses it (above).
        // Snapshotted from the same frozen registry as registeredFeatureFqis above,
        // so the list cannot change during the server's lifetime, as Part A requires.
        std::vector<std::string> affected;
        for (const auto& fqi : chain_->registeredFeatureFqis) {
            if (fqi == kLockControllerFqi || fqi == kSiLAServiceFqi) {
                continue;
            }
            affected.push_back(fqi);
        }
        chain_->metadataAffectedCalls[kLockIdentifierMetadataFqi] = std::move(affected);
        // A std::function rather than a LockControllerImpl* member on the chain:
        // GrpcTransport.h is included by every generated adapter and must not pull
        // in LockController.grpc.pb.h -- the same constraint MetadataPolicy.h:24-27
        // names for SiLAService. Capturing the raw pointer is safe: components is
        // moved into the returned server below and the chain dies with it, same
        // lifetime argument this file already makes for the other chain members
        // just above.
        chain_->lockGate = [lock = components.lockController.get()](
            std::string_view fqi, const std::optional<std::string>& serialized) {
            lock->checkLockMetadata(fqi, serialized);
        };
    }
    components.chain = std::move(chain_);
    // Started here rather than in RegisterCommandManager: a Builder that is
    // registered but never successfully built (the TLS/config checks above
    // throw) leaves no sweep thread running on the caller-owned manager. The
    // UUID->execution map only shrinks via removeExpired(); nothing else
    // starts the sweep in production. The SiLAServerBase{...} constructor
    // below can still throw during feature/property registration, but its
    // last action is addRemovalObserver() (this file, above), so nothing
    // dangles on that path — and ~ObservableCommandManager stops the sweep
    // regardless, since mgr is application-owned and can outlive the server.
    // Stopped in the non-throwing case by SiLAServerBase::Shutdown()/
    // ~SiLAServerBase (stopCommandManagerGC).
    for (auto* mgr : commandManagers_) {
        mgr->startAutoGC(std::chrono::seconds{60});
    }
    components.commandManagers = std::move(commandManagers_);

    std::filesystem::path connectionConfigurationStorePath;
    tls::OutboundCredentialsProvider connectionConfigurationCredentials;
    if (connectionConfigurationConfig_) {
        connectionConfigurationStorePath =
            std::move(connectionConfigurationConfig_->storePath);
        // An explicit credential applies to every host: the operator chose it.
        connectionConfigurationCredentials =
            [creds = std::move(connectionConfigurationConfig_->outboundCredentials)](
                std::string_view) { return creds; };
    } else {
        // Part A p32: every SiLA 2 Version >= "1.1" server SHALL support
        // server-initiated connections, so a server built without
        // WithConnectionConfiguration still gets a working store and
        // outbound credentials rather than being left "unconfigured".
        connectionConfigurationStorePath = persistentUuidPath_
            ? std::filesystem::path{persistentUuidPath_->string() + ".connections"}
            // Same temp-directory-keyed-by-uuid precedent as the binary spool
            // store above (HybridBinaryStore's "sila2-spool-" + uuid path).
            : std::filesystem::temp_directory_path() / ("sila2-connections-" + config_->uuid());
        connectionConfigurationCredentials =
            defaultOutboundCredentials(certificatePem_, privateKeyPem_, caCertPem_);
    }

    return SiLAServerBase{std::move(featureRegistry_), std::move(certificatePem_),
                          std::move(privateKeyPem_), std::move(caCertPem_),
                          std::move(config_), std::move(components),
                          std::move(connectionConfigurationStorePath),
                          std::move(connectionConfigurationCredentials),
                          port};
}
}  // namespace sila2
