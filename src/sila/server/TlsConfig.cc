/**
 ** This file is part of the sila_cpp project.
 ** Copyright 2020 SiLA2
 **
 ** Permission is hereby granted, free of charge, to any person obtaining a copy
 ** of this software and associated documentation files (the "Software"), to deal
 ** in the Software without restriction, including without limitation the rights
 ** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 ** copies of the Software, and to permit persons to whom the Software is
 ** furnished to do so, subject to the following conditions:
 **
 ** The above copyright notice and this permission notice shall be included in all
 ** copies or substantial portions of the Software.
 **
 ** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 ** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 ** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 ** OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 ** SOFTWARE.
 **/

//============================================================================
/// \file   SelfSignedCertificateHelper.cpp
/// \author Florian Meinicke (florian.meinicke@cetoni.de)
/// \date   23.09.2020
/// \brief  Implementation of helper functions for creation of a self-signed
/// certificate and key
/// Most of this was adapted from https://gist.github.com/nathan-osman/5041136 and
/// https://stackoverflow.com/a/57478849/12780516
//============================================================================

//============================================================================
//                                  INCLUDES
//============================================================================
#include "SelfSignedCertificateHelper.h"

#include <sila_cpp/common/constants.h>
#include <sila_cpp/common/logging.h>

#include <QHostInfo>
#include <QNetworkInterface>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <iostream>
#include <iterator>

using namespace std;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
BN_GENCB* BN_GENCB_new()
{
    return new BN_GENCB;
}

void BN_GENCB_free(BN_GENCB* cb)
{
    delete cb;
}
#endif

namespace SiLA2
{
using asn1_octet_string_unique_ptr =
    unique_ptr<ASN1_OCTET_STRING, void (*)(ASN1_OCTET_STRING*)>;
using bignum_unique_ptr = unique_ptr<BIGNUM, void (*)(BIGNUM*)>;
using bn_gencb_unique_ptr = unique_ptr<BN_GENCB, void (*)(BN_GENCB*)>;
using bio_unique_ptr = unique_ptr<BIO, int (*)(BIO*)>;
using x509_extension_unique_ptr =
    unique_ptr<X509_EXTENSION, void (*)(X509_EXTENSION*)>;

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

    transform(cbegin(Addresses), cend(Addresses), back_inserter(SubjectAltNames),
              [i = 0](const auto& Addr) mutable {
                  return QString{"IP.%1:%2"}.arg(i++).arg(
                      Addr.toString().remove("%" + Addr.scopeId()));
              });

    return SubjectAltNames;
}

/**
 * @brief Callback function for the RSA key generation that displays the
 * progress of the calculation
 */
[[maybe_unused]] static int callback(int p, int /*n*/, BN_GENCB* /*cb*/)
{
    if (sila_cpp_server().isInfoEnabled())
    {
        if (p == 0)
        {
            cout << '.';
        }
        else if (p == 1)
        {
            cout << '+';
        }
        else if (p == 2)
        {
            cout << '*';
        }
        else if (p == 3)
        {
            cout << '\n';
        }
        else
        {
            cout << 'B';
        }
    }
    return 1;
}

//============================================================================
COpenSSLError::COpenSSLError(const std::string& Description)
    : runtime_error{Description
                    + "\nOpenSSL: " + ERR_error_string(ERR_get_error(), nullptr)}
{}

//============================================================================
evp_pkey_unique_ptr generateKey()
{
    qCDebug(sila_cpp_common) << "Generating RSA private key";
    // 1. Allocate the key structure
    evp_pkey_unique_ptr Key{EVP_PKEY_new(), EVP_PKEY_free};
    if (!Key)
    {
        throw COpenSSLError{"Could not allocate EVP_PKEY structure"};
    }

    // 2. Create the RSA key
    const auto BigNum = bignum_unique_ptr{BN_new(), BN_free};
    BN_set_word(BigNum.get(), RSA_F4);
    auto* RSA = RSA_new();

    auto cb = bn_gencb_unique_ptr{BN_GENCB_new(), BN_GENCB_free};
    BN_GENCB_set(cb.get(), callback, nullptr);
    RSA_generate_key_ex(RSA, 4096, BigNum.get(), cb.get());
    cout << endl;

    // 3. Assign the RSA key to the Key
    if (!EVP_PKEY_assign_RSA(Key.get(), RSA))
    {
        throw COpenSSLError{"Could not generate RSA private key"};
    }

    return Key;
}

//============================================================================
x509_unique_ptr generateCertificate(const evp_pkey_unique_ptr& Key,
                                    const string& Hostname, const QString& IP,
                                    const QUuid& ServerUUID)
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

//============================================================================
string keyToString(const evp_pkey_unique_ptr& Key)
{
    const auto BIOBuffer = bio_unique_ptr{BIO_new(BIO_s_mem()), BIO_free};
    if (PEM_write_bio_PrivateKey(BIOBuffer.get(), Key.get(), nullptr, nullptr, 0,
                                 nullptr, nullptr)
        == 0)
    {
        throw COpenSSLError{"Could not convert private key"};
    }

    const char* Buffer;
    SILA_CPP_DISABLE_WARNING_PUSH
    SILA_CPP_DISABLE_WARNING_OLD_STYLE_CAST
    BIO_get_mem_data(BIOBuffer.get(), &Buffer);
    SILA_CPP_DISABLE_WARNING_POP

    return string{Buffer};
}

//============================================================================
string certificateToString(const x509_unique_ptr& Certificate)
{
    const auto BIOBuffer = bio_unique_ptr{BIO_new(BIO_s_mem()), BIO_free};
    if (PEM_write_bio_X509(BIOBuffer.get(), Certificate.get()) == 0)
    {
        throw COpenSSLError{"Could not convert certificate"};
    }

    const char* Buffer;
    SILA_CPP_DISABLE_WARNING_PUSH
    SILA_CPP_DISABLE_WARNING_OLD_STYLE_CAST
    BIO_get_mem_data(BIOBuffer.get(), &Buffer);
    SILA_CPP_DISABLE_WARNING_POP

    return string{Buffer};
}
}  // namespace SiLA2
