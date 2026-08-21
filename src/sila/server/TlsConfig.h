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
/// \file   SelfSignedCertificateHelper.h
/// \author Florian Meinicke (florian.meinicke@cetoni.de)
/// \date   23.09.2020
/// \brief  Declaration of helper functions for creation of a self-signed
/// certificate and key
//============================================================================
#ifndef SELFSIGNEDCERTIFICATEHELPER_H
#define SELFSIGNEDCERTIFICATEHELPER_H

//============================================================================
//                                  INCLUDES
//============================================================================
#include <QUuid>

#include <memory>
#include <stdexcept>
#include <string>

//============================================================================
//                            FORWARD DECLARATIONS
//============================================================================
struct evp_pkey_st;
using EVP_PKEY = evp_pkey_st;
struct x509_st;
using X509 = x509_st;

namespace SiLA2
{
/**
 * @brief The COpenSSLError class is used for all OpenSSL related errors that can
 * happen during creation of a self-signed certificate.
 */
class COpenSSLError : public std::runtime_error
{
public:
    /**
     * @brief Constructs a new @b COpenSSLError from  @a Description and the
     * OpenSSL internal error
     *
     * @param Description A description of what exactly went wrong
     */
    explicit COpenSSLError(const std::string& Description);
};

using evp_pkey_unique_ptr = std::unique_ptr<EVP_PKEY, void (*)(EVP_PKEY*)>;
using x509_unique_ptr = std::unique_ptr<X509, void (*)(X509*)>;

/**
 * @brief Helper to generate an RSA key using OpenSSL
 *
 * @return The private key as a @c unique_ptr
 */
evp_pkey_unique_ptr generateKey();

/**
 * @brief Helper to generate an X509 self-signed certificate using OpenSSL
 *
 * @param Key The private key to use for creation of the certificate
 * @param Hostname The name of the host for which the certificate is created. This
 * is set as the value of the 'CN' field in the certificate subject.
 * @param IP The IP address to use for generating the certificate's subject
 * alternative names (SANs)
 * @param ServerUUID The ServerUUID that should be put into the certificate if the
 * @a Hostname is "SiLA2"
 *
 * @return The certificate as a @c unique_ptr
 */
x509_unique_ptr generateCertificate(const evp_pkey_unique_ptr& Key,
                                    const std::string& Hostname,
                                    const QString& IP,
                                    const QUuid& ServerUUID = {});

/**
 * @brief Helper to convert the given @a Key to a @c std::string
 *
 * @param Key The key to convert
 *
 * @return The key as a @c std::string
 */
std::string keyToString(const evp_pkey_unique_ptr& Key);

/**
 * @brief Helper to convert the given @a Certificate to a @c std::string
 *
 * @param Certificate The certificate to convert
 *
 * @return The certificate as a @c std::string
 */
std::string certificateToString(const x509_unique_ptr& Certificate);

}  // namespace SiLA2

#endif  // SELFSIGNEDCERTIFICATEHELPER_H
