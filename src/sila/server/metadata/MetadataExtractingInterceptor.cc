// MetadataExtractingInterceptor.cc
#include <sila/server/metadata/MetadataExtractingInterceptor.h>

#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/transport/CallContext.h>

// SiLA 2 wire format for the standard AccessToken header is the serialized
// Metadata_AccessToken message (see SilaClientBase.cc), not the bare token.
#include "AuthorizationService.pb.h"

namespace sila2 {

namespace {
// Derived, not spelled out: one copy of the rule for both sides of the wire.
// The second, dot-folded constant this file used to carry existed only
// because this fork's own client emitted a dialect key (MetadataInjector.cc);
// with the fold removed the two collapse into one.
const std::string kAccessTokenStandardKey = metadataHeaderKey(kAccessTokenMetadataFqi);
}  // namespace

void MetadataExtractingInterceptor::extract(
    const std::multimap<std::string, std::string>& headers,
    CallContext& ctx) {
    for (const auto& [key, value] : headers) {
        if (key.starts_with("sila-") || key == "access-token") {
            ctx.setMetadata(key, value);
            if (key == kAccessTokenStandardKey) {
                // The header carries a serialized Metadata_AccessToken, not the
                // raw token, so pull the inner string out before normalizing.
                // A value that fails to parse is dropped rather than passed
                // through as-is: falling back to "treat it as a raw token"
                // would let arbitrary bytes reach AuthTokenStore::validate()
                // under the "access-token" key, i.e. accept whatever a
                // malformed/adversarial client sent as if it were a token
                // string. That's a security regression, not a compatibility
                // convenience, so an unparseable header is treated as "no
                // standard access token" instead.
                sila2::org::silastandard::core::authorizationservice::v1::Metadata_AccessToken
                    metadata;
                if (metadata.ParseFromString(value)) {
                    ctx.setMetadata("access-token", metadata.accesstoken().value());
                }
            }
        }
    }
}

}  // namespace sila2
