// TlsConfig.h
//
// Ported from sila_cpp v0.3.11
// src/lib/common/SelfSignedCertificateHelper.h (MIT License, Copyright 2020
// SiLA2). The original depends on Qt (QUuid), which this project does not
// use, so this port works in terms of std::string instead.
#pragma once

#include <memory>
#include <stdexcept>
#include <string>

struct evp_pkey_st;
using EVP_PKEY = evp_pkey_st;
struct x509_st;
using X509 = x509_st;

namespace sila2 {
/// Crypto error raised while creating a self-signed certificate.
/// Appends BoringSSL's last error string (ERR_get_error()) after the
/// description passed in, to build the exception message.
class CryptoError : public std::runtime_error {
public:
    /// @param description What was being attempted when it failed
    explicit CryptoError(const std::string& description);
};

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, void (*)(EVP_PKEY*)>;
using X509Ptr = std::unique_ptr<X509, void (*)(X509*)>;

/// Generates an RSA private key.
/// @param bits Key length in bits. Default 2048, matching the reference
/// implementation sila_java's default
/// (SelfSignedCertificate.KeySize.SIZE_2048).
/// @return The generated key
EvpPkeyPtr generateKey(int bits = 2048);

/// Generates a self-signed X.509 certificate.
/// @param key The private key used to sign the certificate
/// @param hostname The host name placed in the certificate's
/// SubjectAlternativeName; the CN is fixed to "SiLA2" per SiLA 2 Part B
/// @param ip The IP address used to generate the certificate's subject
/// alternative names (SANs)
/// @param serverUuid The server UUID embedded as an X.509 extension when
/// non-empty
/// @return The generated certificate
X509Ptr generateCertificate(const EvpPkeyPtr& key, const std::string& hostname,
                            const std::string& ip,
                            const std::string& serverUuid = {});

/// Converts the private key to a PEM-formatted string.
std::string keyToPem(const EvpPkeyPtr& key);

/// Converts the certificate to a PEM-formatted string.
std::string certificateToPem(const X509Ptr& certificate);

}  // namespace sila2
