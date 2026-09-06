// Checks for the sila_cpp SelfSignedCertificateHelper port: the four places
// where this port deliberately departs from the original (OpenSSL 3.0 keygen, now on BoringSSL,
// random serial, CA:FALSE, PEM length handling), plus the SAN list this port
// builds from getifaddrs instead of QHostInfo.
#include <sila/server/config/TlsConfig.h>

#include <gtest/gtest.h>

#include <openssl/asn1.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>
#include <string>

namespace
{
constexpr auto kSiLA2IanaPen = "1.3.6.1.4.1.58583";

using Asn1ObjectPtr = std::unique_ptr<ASN1_OBJECT, void (*)(ASN1_OBJECT*)>;

/// Parses a PEM certificate back into an X509. Reading the PEM back is itself
/// part of what is under test: PEM_write_bio_X509 does not NUL-terminate its
/// buffer, so a certificateToPem that dropped the length would hand us
/// trailing garbage and fail to parse here.
sila2::X509Ptr parsePem(const std::string& pem)
{
    // BIO_new_mem_buf takes the length explicitly, so the string's own size
    // is the only thing deciding where the certificate ends.
    auto* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    auto* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return sila2::X509Ptr{cert, X509_free};
}

/// Returns the extension identified by nid, or nullptr if the certificate
/// does not carry it.
X509_EXTENSION* findExtension(const sila2::X509Ptr& cert, int nid)
{
    const auto index = X509_get_ext_by_NID(cert.get(), nid, -1);
    return index < 0 ? nullptr : X509_get_ext(cert.get(), index);
}

/// Renders an extension the way `openssl x509 -text` prints it, so the test
/// can assert on the human-readable form ("DNS:localhost", "CA:FALSE")
/// instead of walking GENERAL_NAME / BASIC_CONSTRAINTS structures.
std::string extensionText(X509_EXTENSION* ext)
{
    auto* bio = BIO_new(BIO_s_mem());
    X509V3_EXT_print(bio, ext, 0, 0);
    char* buffer = nullptr;
    const auto length = BIO_get_mem_data(bio, &buffer);
    std::string text{buffer, length < 0 ? 0u : static_cast<std::size_t>(length)};
    BIO_free(bio);
    return text;
}
}  // namespace

TEST(TlsConfig, RejectsNonPositiveKeySize)
{
    // A non-positive key size is never a valid RSA key size; generateKey
    // rejects it before handing it to EVP_PKEY_CTX_set_rsa_keygen_bits.
    EXPECT_THROW(sila2::generateKey(0), sila2::CryptoError);
    EXPECT_THROW(sila2::generateKey(-1), sila2::CryptoError);
}

TEST(TlsConfig, WritesPrivateKeyAsPem)
{
    const auto key = sila2::generateKey();
    const auto pem = sila2::keyToPem(key);
    EXPECT_EQ(pem.rfind("-----BEGIN PRIVATE KEY-----", 0), 0u);
    EXPECT_NE(pem.find("-----END PRIVATE KEY-----"), std::string::npos);
}

TEST(TlsConfig, GivesEachCertificateItsOwnSerial)
{
    // The original hard-coded serial 1, so every SiLA server shared the same
    // (issuer, serial) pair. Two certificates from one key must still differ.
    const auto key = sila2::generateKey();
    const auto first = parsePem(sila2::certificateToPem(
        sila2::generateCertificate(key, "SiLA2", "127.0.0.1")));
    const auto second = parsePem(sila2::certificateToPem(
        sila2::generateCertificate(key, "SiLA2", "127.0.0.1")));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_NE(ASN1_INTEGER_cmp(X509_get_serialNumber(first.get()),
                               X509_get_serialNumber(second.get())),
              0);
}

TEST(TlsConfig, MarksTheCertificateAsNotACa)
{
    const auto key = sila2::generateKey();
    const auto cert = parsePem(sila2::certificateToPem(
        sila2::generateCertificate(key, "SiLA2", "127.0.0.1")));
    ASSERT_NE(cert, nullptr);

    // keyUsage turns on keyCertSign, so without a critical CA:FALSE nothing
    // stops this certificate from signing further certificates.
    auto* constraints = findExtension(cert, NID_basic_constraints);
    ASSERT_NE(constraints, nullptr);
    EXPECT_EQ(X509_EXTENSION_get_critical(constraints), 1);
    EXPECT_EQ(extensionText(constraints), "CA:FALSE");
}

TEST(TlsConfig, PutsLocalhostAndTheBindAddressInTheSans)
{
    const auto key = sila2::generateKey();
    const auto cert = parsePem(sila2::certificateToPem(
        sila2::generateCertificate(key, "SiLA2", "192.0.2.1")));
    ASSERT_NE(cert, nullptr);

    auto* sans = findExtension(cert, NID_subject_alt_name);
    ASSERT_NE(sans, nullptr);
    const auto text = extensionText(sans);
    EXPECT_NE(text.find("DNS:localhost"), std::string::npos);
    // A non-wildcard ip is used as-is, without the DNS lookup the original did.
    EXPECT_NE(text.find("IP Address:192.0.2.1"), std::string::npos);
}

TEST(TlsConfig, SelfSignedCertificateCommonNameIsAlwaysSiLA2)
{
    // SiLA 2 Part B p75 (Encryption): the CN MUST be the literal "SiLA2",
    // regardless of the caller's hostname argument; the hostname still
    // shows up, but as a SAN entry.
    const auto key = sila2::generateKey();
    const auto cert = parsePem(sila2::certificateToPem(
        sila2::generateCertificate(key, "example.org", "127.0.0.1", "some-uuid")));
    ASSERT_NE(cert, nullptr);

    char buf[256] = {};
    ASSERT_GT(X509_NAME_get_text_by_NID(X509_get_subject_name(cert.get()),
                                         NID_commonName, buf, sizeof(buf)),
              0);
    EXPECT_EQ(std::string(buf), "SiLA2");

    auto* sans = findExtension(cert, NID_subject_alt_name);
    ASSERT_NE(sans, nullptr);
    EXPECT_NE(extensionText(sans).find("DNS:example.org"), std::string::npos);
}

TEST(TlsConfig, EmbedsTheServerUuidWheneverAUuidIsProvided)
{
    const auto key = sila2::generateKey();
    // no_name=1: look the OID up as dotted digits, not as a registered short
    // name — this OID is only registered once generateCertificate runs.
    const auto uuidOid = Asn1ObjectPtr{OBJ_txt2obj(kSiLA2IanaPen, 1),
                                       ASN1_OBJECT_free};

    const auto sila2Cert = parsePem(sila2::certificateToPem(
        sila2::generateCertificate(key, "SiLA2", "127.0.0.1", "some-uuid")));
    // The OID is no longer gated on the hostname == "SiLA2" check, so a
    // real deployment hostname with a UUID must also carry it now.
    const auto otherCert = parsePem(sila2::certificateToPem(
        sila2::generateCertificate(key, "example.org", "127.0.0.1", "some-uuid")));
    // No UUID supplied (default empty serverUuid) must never embed the OID.
    const auto noUuidCert = parsePem(sila2::certificateToPem(
        sila2::generateCertificate(key, "SiLA2", "127.0.0.1")));
    ASSERT_NE(sila2Cert, nullptr);
    ASSERT_NE(otherCert, nullptr);
    ASSERT_NE(noUuidCert, nullptr);

    EXPECT_GE(X509_get_ext_by_OBJ(sila2Cert.get(), uuidOid.get(), -1), 0);
    EXPECT_GE(X509_get_ext_by_OBJ(otherCert.get(), uuidOid.get(), -1), 0);
    EXPECT_LT(X509_get_ext_by_OBJ(noUuidCert.get(), uuidOid.get(), -1), 0);
}
