// CredentialVerifier.h — pure interface for user credential verification (§3.11)
#pragma once

#include <optional>
#include <string>

namespace sila2::auth {

// Pure interface: verification backend (e.g. htpasswd file, LDAP) is an implementation detail
// left to concrete subclasses, not this contract.
class CredentialVerifier {
public:
    virtual ~CredentialVerifier() = default;

    // Returns user identifier on success, nullopt on failure.
    virtual std::optional<std::string> verify(
        const std::string& userIdentification,
        const std::string& password) = 0;
};

}  // namespace sila2::auth
