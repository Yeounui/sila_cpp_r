#include "FeatureRegistry.h"

#include <limits>
#include <stdexcept>
#include <utility>

#include <libxml/parser.h>
#include <libxml/tree.h>

namespace sila2 {
namespace {

// Attribute value on the root <Feature> tag, or "" when absent.
// FeatureDefinition.xsd:2 sets attributeFormDefault="unqualified", so a
// Feature's attributes are never namespace-qualified -- xmlGetNoNsProp is the
// matching lookup, the same one FdlRuntimeParser.cc:321 uses.
std::string featureAttribute(xmlNodePtr node, const char* name) {
    xmlChar* raw = xmlGetNoNsProp(node, BAD_CAST name);
    if (raw == nullptr) {
        return {};
    }
    std::string value{reinterpret_cast<const char*>(raw)};
    xmlFree(raw);
    return value;
}

// Text of the Feature's own <Identifier>. Only DIRECT children are scanned:
// FeatureDefinition.xsd:7-11 makes Identifier the Feature's own first child,
// while the Identifiers of Command, Property, Metadata and
// DefinedExecutionError (:19,:40,:59,:70) sit one level deeper -- so no
// document-order assumption is needed to tell the Feature's from theirs.
std::string featureIdentifier(xmlNodePtr root) {
    for (xmlNodePtr child = root->children; child != nullptr; child = child->next) {
        if (child->type != XML_ELEMENT_NODE || !xmlStrEqual(child->name, BAD_CAST "Identifier")) {
            continue;
        }
        xmlChar* content = xmlNodeGetContent(child);
        if (content == nullptr) {
            return {};
        }
        std::string text{reinterpret_cast<const char*>(content)};
        xmlFree(content);
        return text;
    }
    return {};
}

// The FQI the FDL's own contents imply, or "" when the document carries no
// usable root <Feature> identity. Mirrors dynamic::FeatureCatalog's
// derivedFqi (FeatureCatalog.cc:29-33): an omitted Category becomes "none" --
// the XSD's own declared default (FeatureDefinition.xsd:113) -- and
// FeatureVersion is truncated to its major part.
// libxml2's node->name is the LOCAL name (the prefix lives in node->ns), so
// <sila:Feature> and <Feature> both match here without the namespace being
// spelled out, which is the shape S41 tripped on.
// ponytail: identity only, no schema validation -- the namespace href is not
// checked and nothing below the root is read, so this rejects a *wrong*
// identity, not a malformed Feature. Full FDL validation is available through
// CommandParameterValidator for generated Command handling; registration
// remains an identity-only operation.
std::string derivedFqi(const std::string& fdlXml) {
    if (fdlXml.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};  // xmlReadMemory takes an int length; refuse rather than truncate
    }
    xmlDocPtr document =
        xmlReadMemory(fdlXml.data(), static_cast<int>(fdlXml.size()), nullptr, nullptr,
                      XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (document == nullptr) {
        return {};
    }
    std::string fqi;
    xmlNodePtr root = xmlDocGetRootElement(document);
    if (root != nullptr && xmlStrEqual(root->name, BAD_CAST "Feature")) {
        const std::string originator = featureAttribute(root, "Originator");
        const std::string category = featureAttribute(root, "Category");
        const std::string featureVersion = featureAttribute(root, "FeatureVersion");
        const std::string identifier = featureIdentifier(root);
        if (!originator.empty() && !featureVersion.empty() && !identifier.empty()) {
            const std::string major = featureVersion.substr(0, featureVersion.find('.'));
            fqi = originator + "/" + (category.empty() ? "none" : category) + "/" + identifier +
                  "/v" + major;
        }
    }
    xmlFreeDoc(document);
    return fqi;
}

}  // namespace

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
    // The FQI is what ListImplementedFeatures advertises and the key
    // GetFeatureDefinition answers on; the FDL carries its own identity.
    // Checking here rather than in Builder::AddFeature covers every caller at
    // once: AddFeature (SiLAServerBase.cc:404-411) and the Builder's own
    // built-in registrations (:567, :620-654) all funnel through this
    // function. Mirrors dynamic::FeatureCatalog::add (FeatureCatalog.cc:53-58)
    // so a Feature is checked the same way whichever side registers it.
    const std::string derived = derivedFqi(fdlXml);
    if (derived.empty()) {
        throw std::invalid_argument{
            "Feature FDL carries no root <Feature> identity (Originator, FeatureVersion and "
            "Identifier are all required) for FQI: " +
            fqi};
    }
    if (derived != fqi) {
        throw std::invalid_argument{"FDL identity " + derived +
                                    " does not match the registered FQI " + fqi};
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

void FeatureRegistry::registerService(const std::string& fqi,
                                      std::shared_ptr<grpc::Service> service) {
    // Allow transport-level services (e.g. BinaryTransfer) without a Feature definition.
    services_[fqi] = std::move(service);
}

std::vector<grpc::Service*> FeatureRegistry::registeredServices() const {
    std::vector<grpc::Service*> result;
    result.reserve(services_.size());
    for (const auto& [fqi, service] : services_) {
        result.push_back(service.get());
    }
    return result;
}
}  // namespace sila2
