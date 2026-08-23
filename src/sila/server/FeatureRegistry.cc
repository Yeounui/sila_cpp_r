#include "FeatureRegistry.h"

#include <stdexcept>
#include <utility>

namespace sila2
{
// No mutex: registration only happens during the SilaServerBase::Builder
// chain at boot time, never concurrently with request handling.
void FeatureRegistry::registerFeature(std::string fqi, std::string fdlXml)
{
    if (definitions_.find(fqi) != definitions_.end())
    {
        throw std::invalid_argument{"Feature already registered: " + fqi};
    }
    definitions_.emplace(std::move(fqi), std::move(fdlXml));
}

const std::string& FeatureRegistry::featureDefinition(const std::string& fqi) const
{
    // std::map::at() already throws std::out_of_range for an unknown key.
    return definitions_.at(fqi);
}

std::vector<std::string> FeatureRegistry::registeredFeatureIdentifiers() const
{
    // std::map keys already iterate in sorted order.
    std::vector<std::string> fqis;
    fqis.reserve(definitions_.size());
    for (const auto& [fqi, fdlXml] : definitions_)
    {
        fqis.push_back(fqi);
    }
    return fqis;
}
}  // namespace sila2
