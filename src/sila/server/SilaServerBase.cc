// SilaServerBase.cc
#include "SilaServerBase.h"

#include <sila/config/TlsConfig.h>

#include <stdexcept>
#include <utility>

namespace sila2
{
SilaServerBase::SilaServerBase(FeatureRegistry featureRegistry, std::string certificatePem,
                                std::string privateKeyPem)
    : featureRegistry_{std::move(featureRegistry)},
      certificatePem_{std::move(certificatePem)},
      privateKeyPem_{std::move(privateKeyPem)}
{}

const FeatureRegistry& SilaServerBase::featureRegistry() const { return featureRegistry_; }
const std::string& SilaServerBase::certificatePem() const { return certificatePem_; }
const std::string& SilaServerBase::privateKeyPem() const { return privateKeyPem_; }

SilaServerBase::Builder& SilaServerBase::Builder::AddFeature(std::string fqi, std::string fdlXml)
{
    featureRegistry_.registerFeature(std::move(fqi), std::move(fdlXml));
    return *this;
}

SilaServerBase::Builder& SilaServerBase::Builder::WithSelfSignedCertificate(
    std::string hostname, std::string ip)
{
    // serverUuid is left at its default (empty): ServerConfig (§3.7), the
    // only source for a persisted server UUID, does not exist yet.
    const auto key = generateKey();
    const auto certificate = generateCertificate(key, hostname, ip);
    privateKeyPem_ = keyToPem(key);
    certificatePem_ = certificateToPem(certificate);
    return *this;
}

SilaServerBase::Builder& SilaServerBase::Builder::WithCertificate(std::string certificatePem,
                                                                    std::string privateKeyPem)
{
    certificatePem_ = std::move(certificatePem);
    privateKeyPem_ = std::move(privateKeyPem);
    return *this;
}

SilaServerBase SilaServerBase::Builder::Build()
{
    if (certificatePem_.empty() || privateKeyPem_.empty())
    {
        throw std::logic_error{
            "SilaServerBase::Builder::Build: TLS material required — call "
            "WithSelfSignedCertificate or WithCertificate first"};
    }
    return SilaServerBase{std::move(featureRegistry_), std::move(certificatePem_),
                          std::move(privateKeyPem_)};
}
}  // namespace sila2
