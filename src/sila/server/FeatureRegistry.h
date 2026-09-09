// FeatureRegistry.h
//
// New component, not a port. sila_cpp's CSiLAServer hides its registry
// behind a PIMPL and is entangled with Qt (QObject, signals/slots), so it
// isn't a usable reference here; sila_java's SiLAServer.Builder keeps a
// plain Map<String, String> of FQI to FDL XML instead, which is the shape
// adopted below.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <sila/common/util/AsciiCase.h>

namespace grpc { class Service; }

namespace sila2 {
/// Every @ref gl_feature "Feature" a @ref gl_sila_server "SiLA Server" offers, in one place.
///
/// Holds the FDL XML of every Feature registered on a server, keyed by its
/// fully qualified identifier (FQI). Populated only during boot through the
/// Builder chain, then read-only for the lifetime of the server, so no
/// locking is needed here.
/// @see SiLAServerBase::Builder::AddFeature, which populates it.
class FeatureRegistry {
public:
    /// Adds a Feature to the registry so a server built from it can serve the Feature's
    /// @ref gl_feature_definition "Feature Definition" and dispatch its RPCs.
    ///
    /// Registers fdlXml under fqi (e.g. "org.silastandard/core/LockController/v1").
    /// The FQI already encodes the Feature version,
    /// so registering multiple versions of the same Feature needs no separate version axis.
    /// @throws std::invalid_argument if fqi is already registered, or if the
    /// identity the FDL itself carries (root `<Feature>` Originator/Category/
    /// FeatureVersion plus its own `<Identifier>`) does not spell fqi. Only the
    /// root identity is read -- the FDL is otherwise still stored verbatim and
    /// never parsed.
    /// A standard exception is enough here since the message has no extra context to append
    /// (contrast CryptoError in TlsConfig.h, which appends BoringSSL's own error string).
    void registerFeature(std::string fqi, std::string fdlXml);

    /// @return The @ref gl_feature_definition "Feature Definition" (FDL XML) registered under fqi.
    /// @throws std::out_of_range if fqi is not registered, matching
    /// std::map::at()'s convention.
    const std::string& featureDefinition(const std::string& fqi) const;

    /// @return The @ref gl_fully_qualified_identifier "FQI" of every registered Feature.
    /// FQI-sorted, for SiLAServiceImpl::ListImplementedFeatures.
    std::vector<std::string> registeredFeatureIdentifiers() const;

    /// Associates a gRPC service implementation with an already-registered FQI, so the
    /// server can dispatch RPC calls for that Feature (gRPC transport only --
    /// CloudTransport dispatches through SilaHandler tables instead).
    ///
    /// Called during Builder assembly; ownership is shared with the registry
    /// so the service stays alive as long as the FeatureRegistry does.
    /// @throws std::out_of_range if fqi was not previously registerFeature'd.
    /// @see registeredServices
    void registerService(const std::string& fqi, std::shared_ptr<grpc::Service> service);

    /// @return All registered gRPC service pointers, for ServerBuilder registration.
    [[nodiscard("caller expects the service list")]] \
    std::vector<grpc::Service*> registeredServices() const;

private:
    // std::map over unordered_map: gives ListImplementedFeatures a reproducible FQI-sorted order for free.
    // A server registers at most a few dozen Features, so the lookup speed difference is irrelevant.
    // Case-insensitive comparator: Part A p87 requires FQI comparison (and thus
    // uniqueness at registerFeature) without regard to case, while the stored
    // key keeps canonical case for registeredFeatureIdentifiers()/ListImplementedFeatures.
    std::map<std::string, std::string, util::CaseInsensitiveLess> definitions_;

    // Separate from definitions_: a Feature is always registered with FDL,
    // but only has a grpc::Service* when using the gRPC transport (§3.8).
    // CloudTransport (§3.9) dispatches via SilaHandler tables instead.
    std::map<std::string, std::shared_ptr<grpc::Service>> services_;
};
}  // namespace sila2
