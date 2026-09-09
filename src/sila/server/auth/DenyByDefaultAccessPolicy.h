// DenyByDefaultAccessPolicy.h — secure-by-default access policy (§3.11)
//
// Protects all listed FQIs and everything under them — anonymous callers are
// denied for any FQI a protected entry covers (see FqiMatch.h: the entry
// itself, or a "/"-delimited child of it), unless the FQI is omitted from the
// protected list entirely (e.g. Login, Get_FCPAffectedByMetadata).
// Authenticated callers are allowed for all protected FQIs; per-user
// restriction requires a site-specific subclass.
#pragma once

#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/FqiMatch.h>

#include <string>
#include <vector>

namespace sila2::auth {

/// Default AccessPolicy: any authenticated caller may invoke any FQI on the
/// protected list; an anonymous caller may invoke only what the list does not cover.
///
/// The policy to pass to SiLAServerBase::Builder::WithAuthentication() when no
/// site-specific rule is needed. Has no notion of per-user restriction -- a site that needs
/// one implements AccessPolicy directly instead.
/// @see AccessPolicy
class DenyByDefaultAccessPolicy final : public AccessPolicy {
public:
    /// @param protectedFqis FQIs that require an access token. A feature FQI entry
    ///        also covers every command/property/parameter FQI under it (FqiMatch.h)
    ///        — the same coverage rule the gate in SiLAServerBase::Build() uses, so
    ///        both transports agree on what is protected regardless of which FQI
    ///        granularity they hand in.
    ///        Any FQI this list does not cover is pre-auth (accessible without a
    ///        token). Typical pre-auth: AuthenticationService (Login),
    ///        AuthorizationService (Get_FCPAffectedByMetadata_AccessToken).
    explicit DenyByDefaultAccessPolicy(std::vector<std::string> protectedFqis)
        : protectedFqis_{std::move(protectedFqis)} {}

    /// Anonymous callers pass only for FQIs no protected entry covers; authenticated
    /// callers pass for every FQI.
    bool isAllowed(const std::string& userIdentifier,
                   const std::string& fqi) const override {
        // Anonymous: only pre-auth FQIs — those no protected entry covers.
        if (userIdentifier.empty()) {
            return !anyFqiCovers(protectedFqis_, fqi);
        }
        // Authenticated: all FQIs allowed. Site subclasses for per-user ACLs.
        return true;
    }

    /// Returns the whole protected list for any authenticated user, empty for an
    /// anonymous one -- Login scopes the issued token to exactly this list.
    std::vector<std::string> allowedFqis(
        const std::string& userIdentifier) const override {
        if (userIdentifier.empty()) return {};
        // Login grants tokens for all protected FQIs.
        return protectedFqis_;
    }

private:
    std::vector<std::string> protectedFqis_;
};

}  // namespace sila2::auth
