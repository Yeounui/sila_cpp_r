// TlsConfig.h
//
// 출처: sila_cpp v0.3.11 src/lib/common/SelfSignedCertificateHelper.h를
// 이식했다 (MIT License, Copyright 2020 SiLA2). 원본은 Qt(QUuid)에 의존하지만
// 이 프로젝트는 Qt를 쓰지 않으므로 std::string 기반으로 옮긴다.
#pragma once

#include <memory>
#include <stdexcept>
#include <string>

struct evp_pkey_st;
using EVP_PKEY = evp_pkey_st;
struct x509_st;
using X509 = x509_st;

namespace sila2
{
/// 자체서명 인증서 생성 중 발생하는 OpenSSL 오류.
/// 전달한 설명 뒤에 OpenSSL의 마지막 오류 문자열(ERR_get_error())을
/// 덧붙여 예외 메시지를 만든다.
class OpenSslError : public std::runtime_error
{
public:
    /// @param description 무엇을 하려다 실패했는지에 대한 설명
    explicit OpenSslError(const std::string& description);
};

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, void (*)(EVP_PKEY*)>;
using X509Ptr = std::unique_ptr<X509, void (*)(X509*)>;

/// RSA 개인키를 생성한다.
/// @param bits 키 길이(비트). 기본값 2048 — 4096은 신규 발급 기준으로도
/// 과도하고 핸드셰이크 비용만 늘어난다. 2048이 현재 RSA 권장 최소치다.
/// @return 생성된 개인키
EvpPkeyPtr generateKey(int bits = 2048);

/// 자체서명 X.509 인증서를 생성한다.
/// @param key 인증서 서명에 사용할 개인키
/// @param hostname 인증서 주체(subject)의 CN 필드에 들어갈 호스트 이름
/// @param ip SAN(subject alternative name) 생성에 사용할 IP 주소
/// @param serverUuid hostname이 "SiLA2"일 때 인증서에 함께 넣을 서버 UUID
/// @return 생성된 인증서
X509Ptr generateCertificate(const EvpPkeyPtr& key, const std::string& hostname,
                             const std::string& ip,
                             const std::string& serverUuid = {});

/// 개인키를 PEM 형식 문자열로 변환한다.
std::string keyToPem(const EvpPkeyPtr& key);

/// 인증서를 PEM 형식 문자열로 변환한다.
std::string certificateToPem(const X509Ptr& certificate);

}  // namespace sila2
