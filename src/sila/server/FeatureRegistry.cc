#include "FeatureRegistry.h"

#include <stdexcept>
#include <utility>

namespace sila2 {
// No mutex: registration only happens during the SiLAServerBase::Builder
// chain at boot time, never concurrently with request handling.
void FeatureRegistry::registerFeature(std::string fqi, std::string fdlXml) {
    /*  definitions_.find(fqi) != definitions_.end()
        map.find(key) != map.end(): 키 존재 여부 확인 관용구.
        find()가 키를 찾으면 해당 iterator, 찾지 못했을 시 end()(끝 너머)를 반환.
        C++20부터 map.contains(key)로 대체 가능.

    - FQI (Fully Qualified Identifier) — Feature 고유 식별 문자열. 예: org.silastandard/core/SiLAService/v1
    - FDL XML (Feature Definition Language XML) — Feature 구조(Command, Property, Parameter 등) 정의하는 XML.
    */
    if (definitions_.contains(fqi)) {
        /*  std::invalid_argument 예외 객체 생성
            throw가 현재 함수 실행을 즉시 중단, 호출 스택을 거슬러 올라가며 매칭되는 catch 블록을 찾아 제어를 넘김
        */
        throw std::invalid_argument{"Feature already registered: " + fqi};
    }
    definitions_.emplace(std::move(fqi), std::move(fdlXml));
    /*  std::map.emplace: 원소 in-place construct(제자리 생성) 메서드
        인자를 맵 내부로 직접 전달해서 pair를 그 자리에서 생성
        
        emplace vs insert — 이동 횟수 차이:
        insert: 임시 pair 생성(이동 2회) → 맵 노드에 pair 이동(이동 2회) = 총 4회
            1. make_pair                    m.insert(std::make_pair(key, value));
            2. 중괄호 초기화 — 가장 짧음       m.insert({key, value});
            3. pair 직접 생성                m.insert(std::pair<std::string, std::string>(key, value));
        emplace: 맵 노드 안에서 바로 pair 생성(이동 2회) = 총 2회
        emplace는 중간 임시 pair를 생략, 최종 위치에 바로 생성. */
}

const std::string& FeatureRegistry::featureDefinition(const std::string& fqi) const {
    // std::map::at() already throws std::out_of_range for an unknown key.
    /* value = std::map::at(key) */
    return definitions_.at(fqi);
}

std::vector<std::string> FeatureRegistry::registeredFeatureIdentifiers() const {
    // std::map keys already iterate in sorted order.
    std::vector<std::string> fqis;
    /*  std::vector.reserve(n): 내부 배열 메모리를 n개 분량 미리 확보. size 불변, capacity만 증가.
        std::vector.push_back(x): 메모리 사이즈 확장. size == capacity이면 재할당(새 배열 할당 + 기존 원소 이동).
        개수를 미리 알 때 reserve로 한 번에 확보하면 재할당 0회.
        
        auto 키워드
        함수의 반환 타입을 자동으로 추론. auto는 이 긴 타입 선언을 생략.
        컴파일러가 대입 값으로부터 타입을 확정하기에, 런타임 비용은 없고 직접 타입을 쓴 것과 동일한 코드가 생성.
    */
    fqis.reserve(definitions_.size());
    for (const auto& [fqi, fdlXml] : definitions_) { fqis.push_back(fqi); }
    return fqis;
}
}  // namespace sila2
