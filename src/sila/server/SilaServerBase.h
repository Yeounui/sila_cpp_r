// SilaServerBase.h
//
// New component, not a port. sila_cpp's CSiLAServer
// (reference/sila_cpp/src/include/sila_cpp/server/SiLAServer.h) is a QObject
// with a Qt-PIMPL'd (isocpp_p0201::polymorphic_value) private implementation
// — the same Qt entanglement that already ruled sila_cpp out for
// FeatureRegistry.h, so it isn't a usable reference here either. sila_java's
// SiLAServer.Builder
// (reference/sila_java/library/server_base/src/main/java/sila_java/library/server_base/SiLAServer.java)
// keeps a plain fluent chain instead — withX()...addFeature()...build() —
// which matches architecture.md §3.1's own example line
// (SilaServerBase::Builder().withConfig(...).addFeature(...).withBinaryTransfer().build()),
// so that shape is adopted here.
//
// Builder methods wire binary transfer (§3.5), authentication (§3.11),
// connection configuration (§3.9), error recovery (§3.12), and mDNS
// discovery (§6).
// The interceptor chain is hardcoded in dispatchToHandler (§3.8).
#pragma once

#include <sila/common/tls/UntrustedTlsCredentials.h>
#include <sila/server/config/ServerConfig.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/LogCallback.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace grpc {
class ChannelCredentials;
class Server;
}  // namespace grpc

namespace sila2 {

class SilaServiceImpl;
class BinaryStore;
class BinaryUploadService;
class BinaryDownloadService;
class ObservablePropertyManager;
class AuthenticationServiceImpl;
class AuthorizationServiceImpl;
class AuthorizationConfigurationServiceImpl;
class ErrorRecoveryServiceImpl;
class ObservableCommandManager;
class LockControllerImpl;
class ConnectionConfigurationServiceImpl;
struct InterceptorChain;
class CloudEnvelopeRouter;

namespace auth {
class CredentialVerifier;
class AccessPolicy;
class AuthTokenStore;
class AuthorizationInterceptor;
}  // namespace auth

namespace recovery {
class RecoverableErrorGate;
}  // namespace recovery

namespace discovery {
class MdnsPublisher;
}  // namespace discovery

// gRPC's implicit receive cap is 4 MB and nothing set it explicitly, while the
// interop client already dialed with 16 MB — two numbers that disagreed with
// each other and with SiLA's 2 MiB inline-binary limit
// (binary::kBinaryInlineThreshold). One named number so server and client are
// wrong or right together. Send size is left at gRPC's default (unlimited),
// which is why the 8.1 MB BinaryValueDownload response already works unset.
/// The largest gRPC message this server will accept on an inbound RPC.
inline constexpr int kMaxReceiveMessageSizeBytes = 16 * 1024 * 1024;

/// A SiLA 2 server assembled by Builder: after run() it serves its registered
/// @ref gl_feature "Features" over gRPC and is discoverable by @ref gl_sila_client "SiLA Clients".
///
/// Assembles a SiLA2 server from registered Features and TLS material via its
/// nested Builder, then holds the assembled, read-only result.
class SilaServerBase {
public:
    /// Internal components assembled by build(); not for direct use by callers.
    struct OwnedComponents {
        std::unique_ptr<BinaryStore> binaryStore;  ///< Binary Transfer chunk store, when withBinaryTransfer() was called.
        std::shared_ptr<BinaryUploadService> uploadService;  ///< gRPC service for binary chunk uploads.
        std::shared_ptr<BinaryDownloadService> downloadService;  ///< gRPC service for binary chunk downloads.
        std::unique_ptr<auth::AuthTokenStore> authTokenStore;  ///< Issued access tokens, when withAuthentication() was called.
        std::unique_ptr<auth::CredentialVerifier> credentialVerifier;  ///< Caller-supplied verifier from withAuthentication().
        std::unique_ptr<auth::AccessPolicy> accessPolicy;  ///< Caller-supplied access policy from withAuthentication().
        std::shared_ptr<AuthenticationServiceImpl> authService;  ///< AuthenticationService Feature implementation (Login/Logout).
        std::shared_ptr<AuthorizationServiceImpl> authzService;  ///< AuthorizationService Feature implementation.
        std::shared_ptr<AuthorizationConfigurationServiceImpl> authzConfigService;  ///< AuthorizationConfigurationService Feature implementation.
        std::unique_ptr<auth::AuthorizationInterceptor> authzInterceptor;  ///< Runs the auth gate on every protected call.
        std::unique_ptr<recovery::RecoverableErrorGate> errorGate;  ///< Tracks recoverable errors, when withErrorRecovery() was called.
        std::shared_ptr<ErrorRecoveryServiceImpl> errorRecoveryService;  ///< ErrorRecoveryService Feature implementation.
        std::unique_ptr<ObservablePropertyManager> errorRecoveryPropMgr;  ///< Backs ErrorRecoveryService's RecoverableErrors subscription.
        // shared_ptr, not unique_ptr: registerService and regCmd/regProp both
        // take shared ownership, the same shape authService above uses.
        std::shared_ptr<LockControllerImpl> lockController;  ///< LockController Feature implementation, when withLock() was called.
        std::unique_ptr<discovery::MdnsPublisher> mdnsPublisher;  ///< SiLA Server Discovery mDNS advertiser.
        std::unique_ptr<InterceptorChain> chain;  ///< Interceptor bundle applied to every dispatched call.
        std::vector<ObservableCommandManager*> commandManagers;  ///< Every registered manager, stopped on shutdown.
        std::unique_ptr<CloudEnvelopeRouter> cloudRouter;  ///< Router for Server-Initiated Connection envelopes.
        // Must be destroyed before cloudRouter: its managed CloudTransports
        // dispatch through that router while they are shutting down.
        std::shared_ptr<ConnectionConfigurationServiceImpl> connectionConfigurationService;  ///< ConnectionConfigurationService Feature implementation.
        ~OwnedComponents();
        OwnedComponents();
        /// Moves every owned component from `other`, leaving it empty.
        OwnedComponents(OwnedComponents&&) noexcept;
        /// Moves every owned component from `other`, leaving it empty.
        OwnedComponents& operator=(OwnedComponents&&) noexcept;
    };

    /// Fluent chain that assembles a SilaServerBase: call the With...() methods to
    /// configure TLS, identity, and optional capabilities, addFeature() for each
    /// @ref gl_feature "Feature" to expose, then build().
    ///
    /// @code{.cpp}
    /// namespace gen = sila2::generated::temperaturecontroller;  // emitted by codegen
    ///
    /// sila2::SilaServerBase::Builder builder;
    /// builder.withSelfSignedCertificate("localhost", "127.0.0.1")
    ///        .withConfig(std::make_unique<sila2::InMemoryServerConfig>("BioShakeQX"));
    ///
    /// // Your Feature implementation, wrapping the codegen ServiceAdapter:
    /// TemperatureControllerImpl impl(builder.chain());
    ///
    /// auto server = builder
    ///     .addFeature(std::string{gen::kFqi}, std::string{gen::kFdlXml}, impl.service())
    ///     .registerCommandManager(&impl.commandManager())
    ///     .withDiscovery(50052)
    ///     .build();
    ///
    /// server.run(true);
    /// @endcode
    class Builder {
    public:
        /// Adds a @ref gl_feature "Feature" to the server so clients can discover it
        /// and call its commands and properties. Call once per Feature before build().
        ///
        /// Registers fqi/fdlXml with the FeatureRegistry this Builder is assembling.
        /// @param fqi Fully Qualified Feature Identifier, e.g.
        /// "org.silastandard/core/SiLAService/v1".
        /// @param fdlXml The Feature's @ref gl_feature_definition "Feature Definition" as XML text.
        /// @param service The gRPC service that serves the Feature's RPCs; must outlive the server.
        /// @throws std::invalid_argument, propagated from FeatureRegistry::registerFeature,
        /// if fqi is already registered or if fdlXml's own Feature identity does not spell fqi.
        /*  Return type of the member function is a reference (&) to the class type itself;
            returning a reference to the object enables method chaining.
        */
        Builder& addFeature(std::string fqi, std::string fdlXml,
                           std::shared_ptr<grpc::Service> service = {});

        /// Generates a self-signed TLS certificate for the server, so no separate
        /// certificate authority is needed to get a server running.
        ///
        /// Stores hostname/ip for self-signed certificate generation, which is
        /// deferred to build() so the server UUID (from withConfig/withPersistentUuid)
        /// can be embedded as the OID extension (Part B p75 RECOMMENDED).
        /// @throws sila2::CryptoError, propagated from TlsConfig in build(), on generation failure.
        Builder& withSelfSignedCertificate(std::string hostname, std::string ip);

        /// Uses a TLS certificate and key the caller already obtained, instead of
        /// generating a self-signed one.
        ///
        /// Uses caller-supplied certificate/key PEM material instead of generating one.
        /// Neither string is parsed here — TlsConfig only offers construction,
        /// not loading, so a caller providing its own PEM is trusted to have obtained it validly.
        /// @param certificatePem The server certificate, PEM-encoded.
        /// @param privateKeyPem Its private key, PEM-encoded.
        /// @param caCertPemForDiscovery Non-empty when certificatePem is untrusted (e.g.
        /// self-signed or signed by a private CA): its PEM is published as ca<l>= mDNS TXT
        /// records so clients can pre-validate it (Part B p75-76 MUST). Empty (default) means
        /// the certificate is trusted (e.g. by a public CA) and no ca<l>= lines are published.
        /// Distinct from withMutualTls's caCertPem_, which verifies INBOUND client certs.
        Builder& withCertificate(std::string certificatePem, std::string privateKeyPem,
                                 std::string caCertPemForDiscovery = "");

        /// Requires every connecting client to present a TLS certificate signed
        /// by this CA, rejecting any client that does not.
        ///
        /// Enables mutual TLS: the server requires and verifies client
        /// certificates signed by this CA.
        /// @see withCertificate
        Builder& withMutualTls(std::string caCertPem);

        /// Sets the server's @ref gl_sila_server_uuid "Server UUID" and name from
        /// caller-managed storage, so identity survives a restart the way the caller
        /// chooses to persist it.
        ///
        /// Provides a ServerConfig for server identity (UUID, name). This is
        /// the caller-injection path for a UUID the caller persists in its own
        /// storage. If neither this nor withPersistentUuid is called, build()
        /// throws -- it will not fabricate a volatile UUID (SiLAService-v1_0.sila.xml:134-137).
        /// @see withPersistentUuid
        Builder& withConfig(std::unique_ptr<ServerConfig> config);

        /// Sets the server's @ref gl_sila_server_uuid "Server UUID" and makes it survive
        /// a restart, without requiring the caller to manage its own storage: the UUID
        /// is read from or written to a file at `path`.
        ///
        /// Makes the server's UUID survive a restart without a caller-supplied
        /// ServerConfig (SiLAService-v1_0.sila.xml:134-137's stability
        /// obligation, audit S30b). An existing file at `path` holding an
        /// S30a-conformant UUID (36-char lowercase-hex, canonically
        /// hyphenated; a single trailing newline tolerated) is reused; a
        /// missing file gets a freshly generated UUID written to it; a
        /// present-but-malformed or non-conformant file is left UNCHANGED --
        /// silently overwriting the identity a client re-binds on is exactly
        /// what the stability obligation forbids. The server NAME does not
        /// persist (architecture-v2.md:372: the mDNS instance name need not
        /// survive a restart; the rebinding basis is the TXT record's UUID).
        /// @throws std::runtime_error, from build(), naming `path`, if the
        ///         file is present but malformed or non-conformant.
        /// @throws std::logic_error, from build(), if withConfig was also
        ///         called -- withConfig already supplies its own uuid source.
        /// @see withConfig
        Builder& withPersistentUuid(std::filesystem::path path);

        /// Enables @ref gl_binary_transfer "Binary Transfer", so clients can send and
        /// receive binary parameter/response values larger than 2 MiB.
        Builder& withBinaryTransfer();

        /// Restricts the listed Features (by Feature FQI or FQI prefix) to authenticated
        /// clients: an unauthenticated call into any of them is rejected.
        ///
        /// Rejects Command, Property, and Metadata FQIs; Feature FQIs,
        /// coarse prefixes, and binary parameter FQIs are valid.
        /// Do not list the SiLAService FQI: rule (a) (MetadataPolicy.h)
        /// rejects the very header that would carry the required token, so
        /// that configuration is satisfiable by no client.
        Builder& withAuthentication(std::unique_ptr<auth::CredentialVerifier> verifier,
                                    std::unique_ptr<auth::AccessPolicy> policy,
                                    std::vector<std::string> protectedFqis);
        /// Declares one @ref gl_sila_client_metadata "SiLA Client Metadata" this
        /// server expects, and which calls require it.
        ///
        /// Declares one SiLA Client Metadata this server expects, and the
        /// Features / Commands / Properties it affects (Part A's affected
        /// list). One declaration serves both readers: calls covered by
        /// affectedCalls are rejected with INVALID_METADATA when the metadata
        /// is absent, and Get_FCPAffectedByMetadata / the cloud kMetadataRequest
        /// answer with this same list. Do NOT declare the standard AccessToken
        /// metadata: build() registers it from protectedFqis itself, and the
        /// gate defers its enforcement to AuthorizationInterceptor. The same is
        /// now true of LockIdentifier -- withLock() below derives its own
        /// affected list at build() and overwrites any row this method was
        /// given for it. Without withLock(), a LockIdentifier row declared
        /// here is advertised for FCP discovery but never enforced: the
        /// presence loop skips it and no lock gate is installed.
        /// @throws std::invalid_argument if affectedCalls names the SiLAService
        ///         Feature or anything under it.
        Builder& withMetadata(std::string metadataFqi,
                              std::vector<std::string> affectedCalls);

        /// Enables the @ref gl_lock "Lock" Feature: a client can call LockServer to reserve
        /// exclusive use of this server, and every affected call then requires the
        /// matching lock identifier as @ref gl_sila_client_metadata "SiLA Client Metadata".
        ///
        /// Registers the LockController Feature (§3.10) and its LockIdentifier
        /// metadata, and installs the gate that checks it on every affected
        /// call. Opt-in like every other assembly axis: a locked server refuses
        /// every Feature call that does not carry the matching identifier, and
        /// LockServer with Timeout=0 holds that lock forever
        /// (LockController-v1_0.sila.xml:45), so exposing it is the operator's
        /// decision. The affected list is derived at build() from the Features
        /// registered by then -- everything except LockController itself
        /// (IsLocked MUST NOT be lock protected, :97-98) and SiLAService (Part A
        /// forbids listing it), so call addFeature before build(), as usual.
        /// Note: once called, AuthenticationService/Login also lands in the
        /// affected list, so a second client cannot log in while another holds
        /// the lock -- that is what "exclusive use" means (:9-10).
        Builder& withLock();

        /// Enables the ErrorRecoveryService Feature, letting clients subscribe to
        /// errors the server considers recoverable.
        Builder& withErrorRecovery();

        /// Configures the @ref gl_connection_method "Server-Initiated Connection"
        /// (cloud connectivity) capability, overriding the defaults build() would
        /// otherwise derive.
        ///
        /// Overrides the defaults build() derives for server-initiated
        /// connections (Part A p32 SHALL support): store file next to the
        /// withPersistentUuid file or under the temp directory, and TLS
        /// outbound credentials presenting this server's certificate (peer
        /// verified against the withMutualTls CA when one is set). Both
        /// arguments are required when this is called.
        Builder& withConnectionConfiguration(
            std::filesystem::path storePath,
            std::shared_ptr<grpc::ChannelCredentials> outboundCredentials);
        /// Overrides the TCP port the server listens on and advertises via
        /// @ref gl_sila_server_discovery "SiLA Server Discovery".
        ///
        /// SiLA Server Discovery is always enabled (Part B p75 MUST: "the SiLA
        /// Server MUST have SiLA Server Discovery enabled by default. It MUST
        /// NOT be possible to disable any part of SiLA Server Discovery.") —
        /// build() constructs and run() publishes the mDNS advertisement
        /// regardless of whether this is called. Calling it only overrides
        /// the TCP port the gRPC server listens on and advertises; the
        /// default is 50051. There is no way to opt out.
        /// @see SilaServerBase::port
        Builder& withDiscovery(uint16_t port);

        /// Registers an @ref gl_observable_command "Observable Command" manager so
        /// its background garbage-collection thread is stopped when the server shuts
        /// down. Call once per manager, typically one per Feature that has
        /// Observable Commands.
        ///
        /// Registers an ObservableCommandManager for shutdown notification.
        /// The manager must outlive the server (typically owned by the Feature
        /// implementation registered via addFeature). Its periodic GC thread
        /// is not started here — that happens in build(), so a Builder that
        /// is never built (or that throws before build() finishes) leaves no
        /// background thread running on a caller-owned object.
        Builder& registerCommandManager(ObservableCommandManager* mgr);

        /// Receives a log line for every auth rejection and dispatch event, so a
        /// caller can wire its own logging framework in.
        ///
        /// Installs a structured-logging callback invoked on auth rejections
        /// and dispatch events.
        Builder& setLogCallback(LogCallback cb);

        /// Exposes the interceptor chain this Builder is assembling, for a Feature
        /// implementation constructed before build() (e.g. to wire per-command
        /// interceptors) to hold onto.
        ///
        /// @return Pointer to the InterceptorChain this Builder will populate
        /// in build(). The returned pointer is stable — ownership transfers to
        /// OwnedComponents but the heap address does not change.
        const InterceptorChain* chain() const;

        /// Finishes configuration and returns the assembled server, not yet listening;
        /// call run() on it to start serving. Call once, after every addFeature() and With...()
        /// call.
        ///
        /// @throws std::logic_error if neither withSelfSignedCertificate nor
        /// withCertificate was called — SiLA2 requires TLS (architecture.md §3.7).
        /// @see SilaServerBase::run
        SilaServerBase build();

        Builder();
        ~Builder();

    private:
    public:
        /// Picks the TLS credentials this server presents when it connects out to a
        /// client under the @ref gl_connection_method "Server-Initiated Connection"
        /// method, for the defaults build() uses when withConnectionConfiguration
        /// is not called.
        ///
        /// Outbound credential policy for a server built without
        /// withConnectionConfiguration (Part A p32 SHALL support). Per
        /// target host, mirroring ClientConfig::channelCredentials: with a
        /// withMutualTls CA, TLS verifying the peer against it for any host;
        /// without one, TLS accepting the untrusted peer certificate ONLY for
        /// a private-range IP literal (Part B p75 "untrusted certificates SHALL
        /// be accepted for setups using private IP addresses"), and null --
        /// ConnectSiLAClient reports InvalidSiLAClient -- for every other host
        /// (Part B p74: untrusted certificates are never accepted implicitly).
        /// Either way this server's own certificate is presented as identity
        /// (the client's CloudClientListener binds it to the UUID). Public and
        /// static so the policy is testable without building a server.
        static tls::OutboundCredentialsProvider defaultOutboundCredentials(
            std::string certificatePem, std::string privateKeyPem, std::string caCertPem);

    private:

        FeatureRegistry featureRegistry_;
        std::string certificatePem_;
        std::string privateKeyPem_;
        std::string caCertPem_;
        // The untrusted certificate's CA PEM to publish as ca<l>= mDNS TXT
        // records (Part B p75-76), or empty when the certificate is trusted.
        // Set by withSelfSignedCertificate (CA = the cert itself) or by
        // withCertificate's caCertPemForDiscovery param -- distinct from
        // caCertPem_ above, which is the INBOUND client-cert trust anchor
        // for withMutualTls and must never be reused for this purpose.
        std::string discoveryCaCertPem_;
        std::unique_ptr<ServerConfig> config_;
        std::optional<std::filesystem::path> persistentUuidPath_;

        bool enableBinaryTransfer_ = false;

        struct AuthConfig {
            std::unique_ptr<auth::CredentialVerifier> verifier;
            std::unique_ptr<auth::AccessPolicy> policy;
            std::vector<std::string> protectedFqis;
        };
        std::optional<AuthConfig> authConfig_;
        std::map<std::string, std::vector<std::string>> declaredMetadata_;
        bool enableLock_ = false;

        bool enableErrorRecovery_ = false;
        struct ConnectionConfigurationConfig {
            std::filesystem::path storePath;
            std::shared_ptr<grpc::ChannelCredentials> outboundCredentials;
        };
        std::optional<ConnectionConfigurationConfig> connectionConfigurationConfig_;
        std::optional<uint16_t> discoveryPort_;

        // Deferred self-signed certificate generation: hostname/ip are stored
        // here and the key+cert are generated in build() once the server UUID
        // is known, so the OID extension (Part B p75 RECOMMENDED) can embed it.
        struct SelfSignedParams {
            std::string hostname;
            std::string ip;
        };
        std::optional<SelfSignedParams> selfSignedParams_;

        LogCallback logCallback_;
        std::unique_ptr<InterceptorChain> chain_;
        std::vector<ObservableCommandManager*> commandManagers_;
    };

    /// The registry of every @ref gl_feature "Feature" this server offers,
    /// as added via Builder::addFeature.
    const FeatureRegistry& featureRegistry() const;
    /// This server's TLS certificate, in PEM form.
    const std::string& certificatePem() const;
    /// This server's TLS private key, in PEM form.
    const std::string& privateKeyPem() const;
    /// This server's identity (@ref gl_sila_server_uuid "Server UUID" and name),
    /// mutable so a caller can update the name after build().
    ServerConfig& serverConfig();
    /// @overload
    const ServerConfig& serverConfig() const;
    /// The router for @ref gl_connection_method "Server-Initiated Connection"
    /// envelopes, or nullptr when the server was built without
    /// withConnectionConfiguration().
    CloudEnvelopeRouter* cloudRouter();

    /// Lets a caller observe or drive the RecoverableErrors
    /// @ref gl_observable_property "Observable Property" subscription directly.
    ///
    /// The manager behind ErrorRecoveryService's RecoverableErrors
    /// subscription, or nullptr when the server was built without
    /// withErrorRecovery(). Exposed for the same reason as cloudRouter():
    /// a subscription registers on a gRPC handler thread, so a caller has no
    /// other way to observe that one is live.
    ObservablePropertyManager* errorRecoveryPropertyManager();

    /// The @ref gl_sila_server_discovery "SiLA Server Discovery" publisher for
    /// this server, so a caller can inspect what is being advertised.
    ///
    /// The mDNS publisher — never nullptr after build(): discovery is always
    /// enabled (Part B p75 MUST) and build() constructs it unconditionally.
    /// Exposed for the same reason as cloudRouter(): the advertisement is
    /// only otherwise observable through a multicast round trip, which does
    /// not run on every host.
    [[nodiscard]]
    const discovery::MdnsPublisher* mdnsPublisher() const;

    /// The TCP port this server listens on and advertises.
    ///
    /// @return The TCP port the gRPC server listens on. Before run() this is
    /// the port passed to Builder::withDiscovery, or 50051 if it was not
    /// called; after a successful run() it is the port the OS actually
    /// selected, so a caller that built with port 0 can learn where to dial
    /// instead of racing on a fixed number.
    /// @see Builder::withDiscovery
    uint16_t port() const;

    /// Starts the server: from this point clients can connect, call Features,
    /// and discover it. Call once, after build().
    ///
    /// Builds the gRPC server with TLS credentials, registers all services
    /// from FeatureRegistry, and starts listening. A failed mDNS publish after
    /// the successful bind is logged (kWarning, "discovery") and does not
    /// throw: the server keeps serving, just undiscoverable.
    /// @param block If true, blocks the calling thread until shutdown() is called.
    /// After a successful return, port() reports the bound port.
    /// @see shutdown
    void run(bool block = true);

    /// Stops the server, so a caller controlling its own lifecycle (rather than
    /// waiting on a blocking run()) can end it cleanly.
    ///
    /// Initiates graceful shutdown of the gRPC server.
    /// @see run
    void shutdown();

    // Destructor defined in .cc where grpc::Server and SilaServiceImpl are complete.
    ~SilaServerBase();

    // SilaServiceImpl holds references to featureRegistry_ and config_,
    // which would dangle after a move. C++17 mandatory copy elision
    // makes build()'s return-by-value safe without a move constructor.
    SilaServerBase(SilaServerBase&&) = delete;
    SilaServerBase& operator=(SilaServerBase&&) = delete;

private:
    /* No friend declaration needed: since C++11, a nested class (Builder) is
       treated as a member of its enclosing class (SilaServerBase), so it has
       the same access to that class's private members as any other member
       function does — Builder::build() can already reach this constructor. */
    SilaServerBase(FeatureRegistry featureRegistry, std::string certificatePem,
                   std::string privateKeyPem, std::string caCertPem,
                   std::unique_ptr<ServerConfig> config,
                   OwnedComponents components,
                   std::filesystem::path connectionConfigurationStorePath,
                   tls::OutboundCredentialsProvider connectionConfigurationCredentials,
                   uint16_t port);

    /// Stops the GC thread on every registered ObservableCommandManager and
    /// clears its removal callback. Must run before components_.cloudRouter
    /// is destroyed: the callback set in the constructor captures cloudRouter
    /// by raw pointer, and commandManagers are application-owned, so they can
    /// outlive this server — their GC thread cannot be left running past the
    /// router's lifetime. Called from shutdown(), which the destructor also
    /// runs, so an application that never calls shutdown() explicitly still
    /// gets the callback cleared.
    void stopCommandManagerGC();

    // Declaration order matters: featureRegistry_ and config_ must be
    // initialized before silaService_, which references both.
    FeatureRegistry featureRegistry_;
    std::string certificatePem_;
    std::string privateKeyPem_;
    std::string caCertPem_;
    std::unique_ptr<ServerConfig> config_;
    std::shared_ptr<SilaServiceImpl> silaService_;
    std::unique_ptr<grpc::Server> server_;
    OwnedComponents components_;
    uint16_t port_;
};
}  // namespace sila2
