// TlsConfig.cc
//
// Ported from sila_cpp v0.3.11
// src/lib/common/SelfSignedCertificateHelper.cpp (MIT License, Copyright 2020 SiLA2).
// Most of this was adapted from
// https://gist.github.com/nathan-osman/5041136 and
// https://stackoverflow.com/a/57478849/12780516.
#include "TlsConfig.h"

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>

namespace sila2 {
// ** Use left variable_name as an alias of right side.
using asn1_octet_string_unique_ptr = std::unique_ptr<ASN1_OCTET_STRING, void (*)(ASN1_OCTET_STRING*)>;
using bignum_unique_ptr = std::unique_ptr<BIGNUM, void (*)(BIGNUM*)>;
using bio_unique_ptr = std::unique_ptr<BIO, int (*)(BIO*)>;
using x509_extension_unique_ptr = std::unique_ptr<X509_EXTENSION, void (*)(X509_EXTENSION*)>;

/// Adds a subject entry to an X509 certificate.
/// @param name The subject name of the certificate the entry is added to
/// @param fieldId The name of the object being added to the subject
/// @param value The value to set for the object identified by fieldId
void addX509SubjectEntry(X509_NAME* name, const char* fieldId, const char* value) {
    X509_NAME_add_entry_by_txt(name, fieldId, MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(value), -1, -1, 0);
}

/// Adds an X509v3 extension to a certificate.
/// @param cert The certificate the extension is added to
/// @param nid The extension NID
/// @param value The extension content
void addX509v3Extension(X509& cert, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, &cert, &cert, nullptr, nullptr, 0);

    auto* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!ext) {
        throw OpenSslError{"Could not add X509v3 extension to the certificate"};
    }

    X509_add_ext(&cert, ext, -1);
    X509_EXTENSION_free(ext);
}

/// Builds the comma-joined subject alternative name (SAN) list for a
/// certificate: "DNS:localhost", this host's own DNS name (if any), and one
/// "IP.<n>:<addr>" entry per address — the numbered format
/// X509V3_EXT_conf_nid expects for subjectAltName.
///
/// The original resolved the ip argument via DNS (QHostInfo::fromName) and
/// only enumerated every network interface when the first resolved address
/// was a wildcard (0.0.0.0 / ::). This port skips that DNS lookup entirely:
/// ip is the address the server binds to, and resolving a bind address
/// through DNS makes no sense. An empty ip or an explicit wildcard
/// ("0.0.0.0" / "::") still means "all interfaces"; any other value is used
/// as-is, unresolved.
/// @param ip The bind address, or empty/a wildcard for "all interfaces"
/// @return The SAN list, already comma-joined
std::string generateSubjectAlternativeNames(const std::string& ip) {
    std::string sans = "DNS:localhost";

    // POSIX caps a host name at 255 bytes (_POSIX_HOST_NAME_MAX); using a
    // fixed 256-byte buffer avoids depending on HOST_NAME_MAX, which needs
    // feature-test macros to be visible from <limits.h>.
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) == 0 && hostname[0] != '\0') {
        sans += ",DNS:";
        sans += hostname;
    }

    int sanIpIndex = 0;
    const auto addSanIp = [&sans, &sanIpIndex](const std::string& addr) {
        sans += ",IP." + std::to_string(sanIpIndex++) + ":" + addr;
    };

    if (ip.empty() || ip == "0.0.0.0" || ip == "::") {
        // getifaddrs hands back a linked list that must be released with
        // freeifaddrs on every path, including exceptions thrown further
        // down — wrap it immediately so the unique_ptr's destructor does
        // that instead of a manual free at each exit point.
        ifaddrs* rawInterfaces = nullptr;
        if (getifaddrs(&rawInterfaces) != 0) {
            throw OpenSslError{"Could not enumerate network interfaces"};
        }
        const auto interfaces = std::unique_ptr<ifaddrs, void (*)(ifaddrs*)>{
            rawInterfaces, freeifaddrs};

        for (auto* iface = interfaces.get(); iface != nullptr;
             iface = iface->ifa_next) {
            if (iface->ifa_addr == nullptr) {
                continue;
            }
            const auto family = iface->ifa_addr->sa_family;
            if (family != AF_INET && family != AF_INET6) {
                continue;
            }

            // The original did not filter out loopback interfaces, so
            // neither does this port.
            const void* addr = nullptr;
            if (family == AF_INET) {
                addr = &reinterpret_cast<sockaddr_in*>(iface->ifa_addr)->sin_addr;
            } else {
                addr = &reinterpret_cast<sockaddr_in6*>(iface->ifa_addr)->sin6_addr;
            }

            char addrBuffer[INET6_ADDRSTRLEN] = {};
            if (inet_ntop(family, addr, addrBuffer, sizeof(addrBuffer)) != nullptr) {
                addSanIp(addrBuffer);
            }
        }
    } else {
        addSanIp(ip);
    }

    return sans;
}

OpenSslError::OpenSslError(const std::string& description)
    : std::runtime_error{
          description + "\nOpenSSL: " + ERR_error_string(ERR_get_error(), nullptr)} {}

// The original built the key via RSA_new() + BN_new()/BN_set_word(RSA_F4) +
// a BN_GENCB progress callback + RSA_generate_key_ex() +
// EVP_PKEY_assign_RSA() — four steps, all deprecated since OpenSSL 3.0.
// EVP_RSA_gen(bits) generates an RSA key with exponent 65537 (same as
// RSA_F4) in a single call, so it replaces the whole chain. The original
// hard-coded the key length at 4096; here it is a bits argument (default
// 2048) — generating a 4096-bit key adds a few seconds to the very first
// boot before a certificate exists. That cost is paid once, not on every
// boot, but nothing about this self-signed device certificate's threat
// model justifies paying it.
EvpPkeyPtr generateKey(int bits) {
    // EVP_RSA_gen is the macro EVP_PKEY_Q_keygen(..., (size_t)(0 + (bits))),
    // which already casts bits for us. A negative bits value would wrap to
    // a huge size_t in that cast, so it is rejected here instead of being
    // handed to a macro that would silently wrap it twice.
    if (bits <= 0) {
        throw OpenSslError{"Key size must be positive"};
    }

    auto* rawKey = EVP_RSA_gen(bits);
    if (rawKey == nullptr) {
        throw OpenSslError{"Could not generate RSA private key"};
    }

    return EvpPkeyPtr{rawKey, EVP_PKEY_free};
}

X509Ptr generateCertificate(const EvpPkeyPtr& key, const std::string& hostname,
                            const std::string& ip, const std::string& serverUuid) {
    // 1. Allocate the x509 structure
    X509Ptr cert{X509_new(), X509_free};
    if (!cert) {
        throw OpenSslError{"Could not allocate X509 structure"};
    }

    // 2. Set necessary attributes
    // 2.1 Serial number
    //
    // The original hard-coded serial number 1. Every SiLA server on the
    // same network then gets the same serial *and* the same issuer DN (all
    // "CN=SiLA2", same organization), so the (issuer, serial) pair — the
    // thing X.509 uses to uniquely identify a certificate — collides
    // across every server. sila_java avoids this with `new BigInteger(64,
    // secureRandom)`; BN_rand + BN_to_ASN1_INTEGER is the OpenSSL
    // equivalent. BN_rand never sets the BIGNUM's sign bit, and
    // BN_to_ASN1_INTEGER DER-encodes a non-negative BIGNUM with the
    // required zero-padding, so the serial comes out positive with no
    // extra step.
    const auto serial = bignum_unique_ptr{BN_new(), BN_free};
    if (serial == nullptr) {
        throw OpenSslError{"Could not allocate certificate serial number"};
    }
    if (BN_rand(serial.get(), 64, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) == 0) {
        throw OpenSslError{"Could not generate a random certificate serial number"};
    }
    if (BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(cert.get()))
        == nullptr) {
        throw OpenSslError{"Could not set certificate serial number"};
    }

    // 2.2 Valid days
    // beginning now
    X509_time_adj_ex(X509_get_notBefore(cert.get()), 0, 0, nullptr);
    // ending a year from now
    X509_time_adj_ex(X509_get_notAfter(cert.get()), 365, 0, nullptr);
    // 2.3 the public key
    X509_set_pubkey(cert.get(), key.get());
    // 2.4 the certificate version
    X509_set_version(cert.get(), 2);
    // 2.5 fill in the subject name, then reuse it as the issuer name too
    // (this is a self-signed certificate, so subject and issuer are the
    // same). No normative SiLA2 spec text was found pinning these DN
    // fields (country/state/locality/org) to these exact values —
    // sila_cpp and sila_java both use the same ones, so they are kept
    // here for interoperability with other SiLA2 implementations.
    auto* subjectName = X509_get_subject_name(cert.get());
    addX509SubjectEntry(subjectName, "C", "CH");   ///< Switzerland
    addX509SubjectEntry(subjectName, "ST", "SG");  ///< Canton of St. Gallen
    addX509SubjectEntry(subjectName, "L", "Rapperswil-Jona");
    addX509SubjectEntry(
        subjectName, "O",
        "Association Consortium Standardization in Lab Automation (SiLA)");
    addX509SubjectEntry(subjectName, "CN", hostname.c_str());
    X509_set_issuer_name(cert.get(), subjectName);

    // The UUID OID below is only added when hostname is "SiLA2" — sila_java's
    // comment on this same check cites it as "as specified in SiLA2
    // Standard Part B".
    if (hostname == "SiLA2") {
        // 2.5 OID 1.3.536 with the UUID.
        //
        // sila_cpp defines this OID as SILA2_IANA_PEN in
        // sila_cpp/common/constants.h; sila_java's SelfSignedCertificate.java
        // uses the same value, so it is inlined here instead of pulling in
        // that header for one constant.
        constexpr auto kSila2IanaPen = "1.3.6.1.4.1.58583";

        asn1_octet_string_unique_ptr uuidValue{ASN1_OCTET_STRING_new(),
                                               ASN1_OCTET_STRING_free};
        ASN1_OCTET_STRING_set(
            uuidValue.get(),
            reinterpret_cast<const unsigned char*>(serverUuid.c_str()),
            static_cast<int>(serverUuid.length()));

        const auto serverUuidNid = OBJ_create(
            kSila2IanaPen, "sila2ServerUUID",
            "ASN.1 - Server UUID of the SiLA 2 Server");
        const auto uuidExtension = x509_extension_unique_ptr{
            X509_EXTENSION_create_by_NID(nullptr, serverUuidNid, 0,
                                         uuidValue.get()),
            X509_EXTENSION_free};
        X509_add_ext(cert.get(), uuidExtension.get(), -1);
    }

    // 2.6 subject alternative names
    addX509v3Extension(*cert, NID_subject_alt_name,
                       generateSubjectAlternativeNames(ip).c_str());

    // 2.7 subject key identifier
    addX509v3Extension(*cert, NID_subject_key_identifier, "hash");

    // 2.8 key usage flags
    addX509v3Extension(*cert, NID_key_usage,
                       "critical,digitalSignature,keyEncipherment,keyCertSign");
    addX509v3Extension(*cert, NID_ext_key_usage, "serverAuth,clientAuth");

    // 2.9 basic constraints
    //
    // sila_cpp does not set this extension; sila_java's
    // SelfSignedCertificate sets it as a critical BasicConstraints(false).
    // keyUsage above already turns on keyCertSign, so without
    // basicConstraints marking this certificate as "not a CA", nothing
    // stops it from being used to sign further certificates.
    addX509v3Extension(*cert, NID_basic_constraints, "critical,CA:FALSE");

    // 3. sign the certificate with our key
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0) {
        throw OpenSslError{"Could not sign the certificate"};
    }

    return cert;
}

// The original (keyToString/certificateToString) discarded the length that
// BIO_get_mem_data also returns and converted with string{Buffer}. Buffers
// filled by PEM_write_bio_* are not guaranteed to be NUL-terminated, so
// that approach can read past the end of the buffer. Here the length is
// captured as pemLength and passed straight to the std::string constructor.
// BIO_get_mem_data is the BIO_ctrl macro and returns a long, which comes
// back negative on failure — casting a negative value to std::size_t would
// produce a huge size and crash the std::string construction, so pemLength
// is checked before the cast.
std::string keyToPem(const EvpPkeyPtr& key) {
    const auto pemBio = bio_unique_ptr{BIO_new(BIO_s_mem()), BIO_free};
    const auto wroteOk = PEM_write_bio_PrivateKey(pemBio.get(), key.get(), nullptr,
                                                   nullptr, 0, nullptr, nullptr);
    if (wroteOk == 0) {
        throw OpenSslError{"Could not convert private key"};
    }

    char* pemBuffer = nullptr;
    const auto pemLength = BIO_get_mem_data(pemBio.get(), &pemBuffer);
    if (pemLength < 0) {
        throw OpenSslError{"Could not read PEM buffer length"};
    }
    return std::string{pemBuffer, static_cast<std::size_t>(pemLength)};
}

std::string certificateToPem(const X509Ptr& certificate) {
    const auto pemBio = bio_unique_ptr{BIO_new(BIO_s_mem()), BIO_free};
    const auto wroteOk = PEM_write_bio_X509(pemBio.get(), certificate.get());
    if (wroteOk == 0) {
        throw OpenSslError{"Could not convert certificate"};
    }

    char* pemBuffer = nullptr;
    const auto pemLength = BIO_get_mem_data(pemBio.get(), &pemBuffer);
    if (pemLength < 0) {
        throw OpenSslError{"Could not read PEM buffer length"};
    }
    return std::string{pemBuffer, static_cast<std::size_t>(pemLength)};
}
}  // namespace sila2
