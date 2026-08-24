// SiLAServerBase.cc
#include "SiLAServerBase.h"

#include <sila/config/TlsConfig.h>

#include <stdexcept>
#include <utility>

namespace sila2 {
SiLAServerBase::SiLAServerBase(FeatureRegistry featureRegistry,
                                std::string certificatePem, std::string privateKeyPem)
    /* std::move: 힙 메모리를 가진 객체에서, 이동은 내부 포인터만 넘기고 원본을 빈 상태로 만듦. 깊은 복사(할당+memcpy) 생략.
       std::move로 멤버에 이동하면 추가 복사 없이 포인터만 옮김.
        멤버.ptr = 매개변수.ptr;
        멤버.len = 매개변수.len;
        매개변수.ptr = nullptr;
        매개변수.len = 0;
       const&로 받으면 멤버 초기화 시 반드시 복사가 발생한다.

       Uniform Initialization(중괄호): {}은 ()와 같은 뜻이나, 축소 변환(double→int 등)을 컴파일 타임에 차단.
        int x(3.14);   // OK — 3으로 잘림 (경고만)
        int x{3.14};   // 컴파일 에러 — double→int 축소 불허
       주의: {}가 initializer_list 생성자를 우선 매칭하기에 initializer_list 생성자를 가진 타입은 차이 있음.
        std::vector<int> v(3, 0);   // 원소 3개: {0, 0, 0}
        std::vector<int> v{3, 0};   // 원소 2개: {3, 0}
    */
    : featureRegistry_{std::move(featureRegistry)},
      certificatePem_{std::move(certificatePem)},
      privateKeyPem_{std::move(privateKeyPem)} {}
/*  const FeatureRegistry: 반환값 수정 못하게.
    const {...}: 메서드 내 멤버 수정 못하게.
*/
const FeatureRegistry& SiLAServerBase::featureRegistry() const { return featureRegistry_; }
const std::string& SiLAServerBase::certificatePem() const { return certificatePem_; }
const std::string& SiLAServerBase::privateKeyPem() const { return privateKeyPem_; }

SiLAServerBase::Builder& SiLAServerBase::Builder::AddFeature(std::string fqi, std::string fdlXml) {
    featureRegistry_.registerFeature(std::move(fqi), std::move(fdlXml));
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithSelfSignedCertificate(
    std::string hostname, std::string ip) {
    // serverUuid is left at its default (empty): ServerConfig (§3.7), the
    // only source for a persisted server UUID, does not exist yet.
    const auto key = generateKey();
    const auto certificate = generateCertificate(key, hostname, ip);
    privateKeyPem_ = keyToPem(key);
    certificatePem_ = certificateToPem(certificate);
    return *this;
}

SiLAServerBase::Builder& SiLAServerBase::Builder::WithCertificate(std::string certificatePem,
                                                                    std::string privateKeyPem) {
    certificatePem_ = std::move(certificatePem);
    privateKeyPem_ = std::move(privateKeyPem);
    return *this;
}

SiLAServerBase SiLAServerBase::Builder::Build() {
    if (certificatePem_.empty() || privateKeyPem_.empty()) {
        throw std::logic_error{
            "SiLAServerBase::Builder::Build: TLS material required — call "
            "WithSelfSignedCertificate or WithCertificate first"};
    }
    return SiLAServerBase{std::move(featureRegistry_), std::move(certificatePem_),
                          std::move(privateKeyPem_)};
}
}  // namespace sila2
