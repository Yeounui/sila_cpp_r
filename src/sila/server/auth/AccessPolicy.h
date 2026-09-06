// AccessPolicy.h — pure interface for FQI-level access control (§3.11)
#pragma once

#include <string>
#include <vector>

namespace sila2::auth {

// Pure interface so callers (e.g. an interceptor) can swap decision
// strategies (allow-list, role-based, ...) without depending on a concrete
// implementation.
class AccessPolicy {
public:
    virtual ~AccessPolicy() = default;

    // fqi = fully qualified identifier of the SiLA feature/command/property
    // being invoked; userIdentifier identifies the caller (e.g. from a
    // client certificate or token).
    virtual bool isAllowed(const std::string& userIdentifier,
                          const std::string& fqi) const = 0;

    virtual std::vector<std::string> allowedFqis(
        const std::string& userIdentifier) const = 0;
};

}  // namespace sila2::auth
