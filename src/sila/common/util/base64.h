// base64.h — Base64 for the SiLA gRPC status-message error encoders.
#pragma once
#include <string>
#include <openssl/evp.h>
namespace sila2 {
/// Base64-encodes bytes (RFC 4648). An internal helper for the gRPC
/// status-message error encoders; a library user does not need this
/// directly.
// SiLA 2 §3.4 / Part B p65: the serialized SiLA error proto is Base64-encoded
// into the gRPC status *message* field (raw bytes stay in error_details).
// One definition shared by SilaError::toStatus() and makeBinaryTransferStatus()
// so the two encoders cannot drift.
inline std::string base64Encode(const std::string& bytes) {
    std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
    encoded.resize(EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
    return encoded;
}
}  // namespace sila2
