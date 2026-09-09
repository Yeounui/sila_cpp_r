// CredentialVerifier.h — pure interface for user credential verification (§3.11)
#pragma once

#include <optional>
#include <string>

namespace sila2::auth {

/// Checks a caller's credentials at @ref gl_sila_client "SiLA Client" Login and reports
/// who they are.
///
/// A server author implements this against their own credential backend (e.g. an
/// htpasswd file, LDAP); pass an instance to
/// SilaServerBase::Builder::withAuthentication(). The library ships no default
/// implementation -- verification always requires site-specific credentials.
///
// Pure interface: verification backend (e.g. htpasswd file, LDAP) is an implementation detail
// left to concrete subclasses, not this contract.
class CredentialVerifier {
public:
    virtual ~CredentialVerifier() = default;

    /// Checks `userIdentification`/`password` against the backend this verifier wraps.
    /// Called once per Login call, before any access token is issued.
    /// @return the user identifier to record on the issued token, or `nullopt` if the
    ///         credentials are rejected -- the client then sees a Login failure rather
    ///         than a token.
    // Returns user identifier on success, nullopt on failure.
    virtual std::optional<std::string> verify(
        const std::string& userIdentification,
        const std::string& password) = 0;
};

}  // namespace sila2::auth
