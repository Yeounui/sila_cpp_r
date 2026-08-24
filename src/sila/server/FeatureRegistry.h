// FeatureRegistry.h
//
// New component, not a port. sila_cpp's CSiLAServer hides its registry
// behind a PIMPL and is entangled with Qt (QObject, signals/slots), so it
// isn't a usable reference here; sila_java's SiLAServer.Builder keeps a
// plain Map<String, String> of FQI to FDL XML instead, which is the shape
// adopted below.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace sila2 {
/// Holds the FDL XML of every Feature registered on a server, keyed by its
/// fully qualified identifier (FQI). Populated only during boot through the
/// Builder chain, then read-only for the lifetime of the server, so no
/// locking is needed here.
class FeatureRegistry {
public:
    /// Registers fdlXml under fqi (e.g. "org.silastandard/core/LockController/v1").
    /// The FQI already encodes the Feature version,
    /// so registering multiple versions of the same Feature needs no separate version axis.
    /// @throws std::invalid_argument if fqi is already registered.
    /// A standard exception is enough here since the message has no extra context to append 
    /// (contrast OpenSslError in TlsConfig.h, which appends OpenSSL's own error string).
    void registerFeature(std::string fqi, std::string fdlXml);

    /// @throws std::out_of_range if fqi is not registered, matching
    /// std::map::at()'s convention.
    const std::string& featureDefinition(const std::string& fqi) const;

    /// FQI-sorted, for SiLAServiceImpl::ListImplementedFeatures.
    std::vector<std::string> registeredFeatureIdentifiers() const;

private:
    // std::map over unordered_map: gives ListImplementedFeatures a reproducible FQI-sorted order for free.
    // A server registers at most a few dozen Features, so the lookup speed difference is irrelevant.
    std::map<std::string, std::string> definitions_;
};
}  // namespace sila2
