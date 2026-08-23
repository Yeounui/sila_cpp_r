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
// (SilaServerBase::Builder().WithConfig(...).AddFeature(...).WithBinaryTransfer().Build()),
// so that shape is adopted here.
//
// Scope: only Feature registration (§3.2, via FeatureRegistry) and TLS
// provisioning (§3.7, via TlsConfig) have a backing implementation today, so
// only those Builder methods do anything. The rest of §3.1's assembly —
// ServerConfig (§3.7), binary transfer (§3.5), authentication (§3.11), error
// recovery (§3.12), mDNS discovery (§6), and the interceptor chain (§3.1) —
// has no component to call yet. Those Builder methods, and the Run/Shutdown
// lifecycle that needs the still-nonexistent GrpcTransport (§3.8), are
// declared with a TODO(owner) and left undefined, so this header states the
// intended surface without inventing behavior ahead of its dependencies.
#pragma once

#include <sila/server/FeatureRegistry.h>

#include <string>

namespace sila2
{
/// Assembles a SiLA2 server from registered Features and TLS material via its
/// nested Builder, then holds the assembled, read-only result.
class SilaServerBase
{
public:
    class Builder
    {
    public:
        /// Registers fqi/fdlXml with the FeatureRegistry this Builder is
        /// assembling.
        /// @throws std::invalid_argument, propagated from
        /// FeatureRegistry::registerFeature, if fqi is already registered.
        Builder& AddFeature(std::string fqi, std::string fdlXml);

        /// Generates a self-signed certificate for hostname/ip
        /// (TlsConfig::generateKey/generateCertificate) and stores its PEM
        /// form for Build().
        /// @throws sila2::OpenSslError, propagated from TlsConfig, on
        /// generation failure.
        Builder& WithSelfSignedCertificate(std::string hostname, std::string ip);

        /// Uses caller-supplied certificate/key PEM material instead of
        /// generating one. Neither string is parsed here — TlsConfig only
        /// offers construction, not loading, so a caller providing its own
        /// PEM is trusted to have obtained it validly.
        Builder& WithCertificate(std::string certificatePem, std::string privateKeyPem);

        /* The six methods below are declared, not implemented: no backing
           component exists yet, so there is no .cc definition for them. A
           declaration alone is enough for any caller to compile — the linker
           only looks for a matching definition once that call site is
           actually built, so these stay silent until a caller reaches them,
           and any accidental call then fails loudly at link time (undefined
           reference) instead of being missed silently. */
        /// TODO(owner): needs ServerConfig (§3.7) for UUID/Name persistence.
        Builder& WithConfig();
        /// TODO(owner): needs BinaryStore/BinaryUploadService/BinaryDownloadService (§3.5).
        Builder& WithBinaryTransfer();
        /// TODO(owner): needs AuthTokenStore/AuthorizationInterceptor plus an
        /// injected CredentialVerifier/AccessPolicy pair (§3.11).
        Builder& WithAuthentication();
        /// TODO(owner): needs ErrorRecoveryServiceImpl/RecoverableErrorGate (§3.12).
        Builder& WithErrorRecovery();
        /// TODO(owner): needs MdnsPublisher (§6).
        Builder& WithDiscovery(bool enabled);
        /// TODO(owner): needs the interceptor chain (§3.1 Chain); none of its
        /// members (MetadataExtractingInterceptor etc.) exist yet either.
        Builder& AddInterceptor();

        /// @throws std::logic_error if neither WithSelfSignedCertificate nor
        /// WithCertificate was called — SiLA2 requires TLS (architecture.md §3.7).
        SilaServerBase Build();

    private:
        FeatureRegistry featureRegistry_;
        std::string certificatePem_;
        std::string privateKeyPem_;
    };

    const FeatureRegistry& featureRegistry() const;
    const std::string& certificatePem() const;
    const std::string& privateKeyPem() const;

    /// TODO(owner): needs GrpcTransport (§3.8) to actually bind and serve gRPC.
    void Run(bool block = true);
    /// TODO(owner): needs GrpcTransport (§3.8).
    void Shutdown();

private:
    /* No friend declaration needed: since C++11, a nested class (Builder) is
       treated as a member of its enclosing class (SilaServerBase), so it has
       the same access to that class's private members as any other member
       function does — Builder::Build() can already reach this constructor. */
    SilaServerBase(FeatureRegistry featureRegistry, std::string certificatePem, std::string privateKeyPem);

    FeatureRegistry featureRegistry_;
    std::string certificatePem_;
    std::string privateKeyPem_;
};
}  // namespace sila2
