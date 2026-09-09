// AccessPolicy.h — pure interface for FQI-level access control (§3.11)
#pragma once

#include <string>
#include <vector>

namespace sila2::auth {

/// Decides which @ref gl_fully_qualified_identifier "Fully Qualified Identifiers" (FQIs) a
/// caller may invoke, once that caller has passed credential verification.
///
/// A server author implements this to plug in a site-specific authorization strategy
/// (allow-list, role-based, ...); pass an instance to
/// SilaServerBase::Builder::withAuthentication(). Ships with @ref DenyByDefaultAccessPolicy.
// Pure interface so callers (e.g. an interceptor) can swap decision
// strategies (allow-list, role-based, ...) without depending on a concrete
// implementation.
class AccessPolicy {
public:
    virtual ~AccessPolicy() = default;

    /// Decides whether `userIdentifier` may invoke `fqi` right now, at Login
    /// time (which FQIs a token may be scoped to) and per-call (whether an
    /// already-issued token's target FQI is still allowed).
    /// @param userIdentifier the caller's identity, as CredentialVerifier::verify
    ///        returned it; empty for an unauthenticated caller.
    /// @param fqi the Feature/Command/Property FQI the caller is invoking.
    /// @see DenyByDefaultAccessPolicy
    // fqi = fully qualified identifier of the SiLA feature/command/property
    // being invoked; userIdentifier identifies the caller (e.g. from a
    // client certificate or token).
    virtual bool isAllowed(const std::string& userIdentifier,
                          const std::string& fqi) const = 0;

    /// Lists every FQI `userIdentifier` may invoke, at the granularity this policy
    /// tracks. AuthTokenStore::issue() scopes the token it grants at Login to exactly
    /// this list, so an entry omitted here stays unreachable for that user even if
    /// isAllowed() would otherwise permit it.
    virtual std::vector<std::string> allowedFqis(
        const std::string& userIdentifier) const = 0;
};

}  // namespace sila2::auth
