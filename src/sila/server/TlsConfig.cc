// TlsConfig.cc
//
// Ported from sila_cpp v0.3.11
// src/lib/common/SelfSignedCertificateHelper.cpp (MIT License, Copyright
// 2020 SiLA2). Most of this was adapted from
// https://gist.github.com/nathan-osman/5041136 and
// https://stackoverflow.com/a/57478849/12780516.
#include "TlsConfig.h"

#include <QHostInfo>
#include <QNetworkInterface>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <iterator>

namespace sila2
{
using asn1_octet_string_unique_ptr =
    std::unique_ptr<ASN1_OCTET_STRING, void (*)(ASN1_OCTET_STRING*)>;
using bio_unique_ptr = std::unique_ptr<BIO, int (*)(BIO*)>;
using x509_extension_unique_ptr =
    std::unique_ptr<X509_EXTENSION, void (*)(X509_EXTENSION*)>;

/**
 * @brief Helper function to add a subject entry to an X509 certificate
 *
 * @param Name The subject name of the certificate for which the entry should
 * be added
 * @param FieldID The name of the object that should be added to the subject
 * @param Value The value to set for the object identified by @a Field
 */
void addX509SubjectEntry(X509_NAME* Name, const char* FieldID, const char* Value)
{
    X509_NAME_add_entry_by_txt(Name, FieldID, MBSTRING_ASC,
                               reinterpret_cast<const uchar*>(Value), -1, -1, 0);
}

/**
 * @brief Helper function to add an X509v3 extension to a certificate
 *
 * @param Cert The certificate to which the extension should be added
 * @param NID The extension NID
 * @param Value The extension content
 */
void addX509v3Extension(X509& Cert, int NID, const char* Value)
{
    X509V3_CTX Ctx;
    X509V3_set_ctx_nodb(&Ctx);
    X509V3_set_ctx(&Ctx, &Cert, &Cert, nullptr, nullptr, 0);

    auto* Ext = X509V3_EXT_conf_nid(nullptr, &Ctx, NID, Value);
    if (!Ext)
    {
        throw COpenSSLError{"Could not add X509v3 extension to the certificate"};
    }

    X509_add_ext(&Cert, Ext, -1);
    X509_EXTENSION_free(Ext);
}

/**
 * @brief Helper function to generate a list of subject alternative names for a
 * given address.
 *
 * @param Address Either a host name or an IP.
 *
 * @return A list of SANs with all names and addresses associated with the given
 * address.
 */
QStringList generateSubjectAlternativeNames(const QString& Address)
{
    QStringList SubjectAltNames{"DNS:localhost"};

    if (!QHostInfo::localHostName().isEmpty())
    {
        SubjectAltNames.append(QString{"DNS:"} + QHostInfo::localHostName());
    }

    auto Addresses = QHostInfo::fromName(Address).addresses();
    if (!Addresses.empty()
        && (Addresses.constFirst() == QHostAddress::Any
            || Addresses.constFirst() == QHostAddress::AnyIPv4
            || Addresses.constFirst() == QHostAddress::AnyIPv6))
    {
        Addresses = QNetworkInterface::allAddresses();
    }

    std::transform(
        std::cbegin(Addresses), std::cend(Addresses),
        std::back_inserter(SubjectAltNames), [i = 0](const auto& Addr) mutable {
            return QString{"IP.%1:%2"}.arg(i++).arg(
                Addr.toString().remove("%" + Addr.scopeId()));
        });

    return SubjectAltNames;
}

OpenSslError::OpenSslError(const std::string& description)
    : std::runtime_error{
          description + "\nOpenSSL: " + ERR_error_string(ERR_get_error(), nullptr)}
{}

// The original built the key via RSA_new() + BN_new()/BN_set_word(RSA_F4) +
// a BN_GENCB progress callback + RSA_generate_key_ex() +
// EVP_PKEY_assign_RSA() — four steps, all deprecated since OpenSSL 3.0.
// EVP_RSA_gen(bits) generates an RSA key with exponent 65537 (same as
// RSA_F4) in a single call, so it replaces the whole chain. The original
// hard-coded the key length at 4096; here it is a bits argument (default
// 2048) — 2048 matches the reference implementation sila_java's default
// (SelfSignedCertificate.KeySize.SIZE_2048). Generating a 4096-bit key adds
// a few seconds to the very first boot before a certificate exists — a
// one-time cost, not one paid on every boot — but nothing about this
// self-signed device certificate's threat model justifies paying it.
EvpPkeyPtr generateKey(int bits)
{
    auto* rawKey = EVP_RSA_gen(static_cast<unsigned int>(bits));
    if (rawKey == nullptr)
    {
        throw OpenSslError{"Could not generate RSA private key"};
    }

    return EvpPkeyPtr{rawKey, EVP_PKEY_free};
}

// Only the signature was matched to TlsConfig.h (Qt types -> std::string).
// The body still uses Qt (QString/QUuid) code as-is, so this will not
// compile yet — porting the body, including SAN collection and the subject
// name, is deferred to the next batch.
X509Ptr generateCertificate(const EvpPkeyPtr& key, const std::string& hostname,
                            const std::string& ip, const std::string& serverUuid)
{
    qCDebug(sila_cpp_common)
        << "Generating X509 certificate for host" << Hostname << "with IP" << IP;

    // 1. Allocate the x509 structure
    x509_unique_ptr Cert{X509_new(), X509_free};
    if (!Cert)
    {
        throw COpenSSLError{"Could not allocate X509 structure"};
    }

    // 2. Set necessary attributes
    // 2.1 Serial number
    ASN1_INTEGER_set(X509_get_serialNumber(Cert.get()), 1);
    // 2.2 Valid days
    // beginning now
    X509_time_adj_ex(X509_get_notBefore(Cert.get()), 0, 0, nullptr);
    // ending a year from now
    X509_time_adj_ex(X509_get_notAfter(Cert.get()), 365, 0, nullptr);
    // 2.3 the public key
    X509_set_pubkey(Cert.get(), Key.get());
    // 2.4 the certificate version
    X509_set_version(Cert.get(), 2);
    // 2.5 construct the issuer name from the subject name adding country
    // code, location, common name, and organization
    auto* IssuerName = X509_get_subject_name(Cert.get());
    addX509SubjectEntry(IssuerName, "C", "CH");   ///< Switzerland
    addX509SubjectEntry(IssuerName, "ST", "SG");  ///< Canton of St. Gallen
    addX509SubjectEntry(IssuerName, "L", "Rapperswil-Jona");
    addX509SubjectEntry(
        IssuerName, "O",
        "Association Consortium Standardization in Lab Automation (SiLA)");
    addX509SubjectEntry(IssuerName, "CN", Hostname.c_str());
    X509_set_issuer_name(Cert.get(), IssuerName);

    if (Hostname == "SiLA2")
    {
        // 2.5 OID 1.3.536 with the UUID
        const auto UUIDString =
            ServerUUID.toString(QUuid::WithoutBraces).toStdString();
        asn1_octet_string_unique_ptr Value{ASN1_OCTET_STRING_new(),
                                           ASN1_OCTET_STRING_free};
        ASN1_OCTET_STRING_set(
            Value.get(),
            reinterpret_cast<const unsigned char*>(UUIDString.c_str()),
            static_cast<int>(UUIDString.length()));

        const auto NID = OBJ_create(constants::SILA2_IANA_PEN, "sila2ServerUUID",
                                    "ASN.1 - Server UUID of the SiLA 2 Server");
        const auto Extension = x509_extension_unique_ptr{
            X509_EXTENSION_create_by_NID(nullptr, NID, 0, Value.get()),
            X509_EXTENSION_free};
        X509_add_ext(Cert.get(), Extension.get(), -1);
    }

    // 2.6 subject alternative names
    addX509v3Extension(*Cert, NID_subject_alt_name,
                       generateSubjectAlternativeNames(IP).join(',').toLatin1());

    // 2.7 subject key identifier
    addX509v3Extension(*Cert, NID_subject_key_identifier, "hash");

    // 2.8 key usage flags
    addX509v3Extension(*Cert, NID_key_usage,
                       "critical,digitalSignature,keyEncipherment,keyCertSign");
    addX509v3Extension(*Cert, NID_ext_key_usage, "serverAuth,clientAuth");

    // 3. sign the certificate with our key
    if (X509_sign(Cert.get(), Key.get(), EVP_sha256()) == 0)
    {
        throw COpenSSLError{"Could not sign the certificate"};
    }

    return Cert;
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
std::string keyToPem(const EvpPkeyPtr& key)
{
    const auto pemBio = bio_unique_ptr{BIO_new(BIO_s_mem()), BIO_free};
    const auto wroteOk = PEM_write_bio_PrivateKey(pemBio.get(), key.get(), nullptr,
                                                   nullptr, 0, nullptr, nullptr);
    if (wroteOk == 0)
    {
        throw OpenSslError{"Could not convert private key"};
    }

    char* pemBuffer = nullptr;
    const auto pemLength = BIO_get_mem_data(pemBio.get(), &pemBuffer);
    if (pemLength < 0)
    {
        throw OpenSslError{"Could not read PEM buffer length"};
    }
    return std::string{pemBuffer, static_cast<std::size_t>(pemLength)};
}

std::string certificateToPem(const X509Ptr& certificate)
{
    const auto pemBio = bio_unique_ptr{BIO_new(BIO_s_mem()), BIO_free};
    const auto wroteOk = PEM_write_bio_X509(pemBio.get(), certificate.get());
    if (wroteOk == 0)
    {
        throw OpenSslError{"Could not convert certificate"};
    }

    char* pemBuffer = nullptr;
    const auto pemLength = BIO_get_mem_data(pemBio.get(), &pemBuffer);
    if (pemLength < 0)
    {
        throw OpenSslError{"Could not read PEM buffer length"};
    }
    return std::string{pemBuffer, static_cast<std::size_t>(pemLength)};
}
}  // namespace sila2
