// AnyWireValue.h — shared test helper for building a wire-format SiLA Any
// value (a host message with a single field "Payload" of message type
// SiLAFramework.Any) through the same DescriptorBuilder::buildFromTypeXml()
// pipeline production code uses (mirrors payloadPrototype() in
// tests/sila/server/test_any_codec_core_link.cc and
// tests/sila/client/dynamic/test_any_codec.cc). Included by two TUs
// (test_value_validator.cc, test_value_validator_external_resolvers.cc), so
// every function here MUST stay `inline` to avoid an ODR violation.
//
// Self-consistency note: the "Payload" field this header builds is exactly
// the SiLAFramework.Any message (string type=1; bytes payload=2) that
// AnyCodec::decode(sila2::types::AnyValue{typeXml, payload}, ...) expects on
// the wire (ValueValidator.cc's validateAnyValue reads it via reflection
// fields "type"/"payload"), so a WireAny built here is exactly what
// ValueValidator::validate() receives from a real gRPC/cloud request.
#pragma once

#include <sila/client/dynamic/AnyCodec.h>
#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/common/types/BasicTypes.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace anytest {

// The type XML for a bare SiLA Any: used to build the "Payload" field that
// every WireAny host wraps its Any message in.
inline const std::string& anyTypeXml() {
    static const std::string kAnyTypeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Any</Basic></DataType>";
    return kAnyTypeXml;
}

// A message with a single SiLA Any field named "Payload", plus the pool and
// factory that own its descriptor. Heap-owned (via unique_ptr) so a WireAny
// is movable and outlives the ValueValidator::validate() call a test makes
// against it.
struct WireAny {
    std::unique_ptr<google::protobuf::DescriptorPool> pool;
    std::unique_ptr<google::protobuf::DynamicMessageFactory> factory;
    std::unique_ptr<google::protobuf::Message> host;
    const google::protobuf::FieldDescriptor* anyField = nullptr;
};

// Builds the "DataType_Payload" message prototype that buildFromTypeXml()
// derives from `typeXml`, registered into `pool` (which must outlive the
// returned prototype and `factory`). Mirrors payloadPrototype() in
// test_any_codec_core_link.cc / test_any_codec.cc exactly, so an encode-side
// fixture built here goes through the identical pipeline decode() expects.
inline const google::protobuf::Message* payloadPrototype(
    const std::string& typeXml, google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
    sila2::dynamic::DescriptorBuilder builder;
    auto fileProto = builder.buildFromTypeXml(typeXml);
    const auto* file = pool.BuildFile(fileProto);
    if (file == nullptr) throw std::runtime_error{"unable to build descriptor for type XML"};
    const auto* descriptor = pool.FindMessageTypeByName(fileProto.package() + ".DataType_Payload");
    if (descriptor == nullptr) throw std::runtime_error{"DataType_Payload message not found"};
    return factory.GetPrototype(descriptor);
}

// Builds a host message with an Any "Payload" field and sets its 'type'/
// 'payload' subfields directly via reflection, with no encoding pass. For
// fixtures that must stay malformed or unresolvable on purpose (a custom
// DataTypeIdentifier, unparsable type XML, or a payload that does not match
// its own type XML) -- anytest::wireAny() below would refuse to build those.
inline WireAny rawWireAny(const std::string& typeString, const std::string& payloadBytes) {
    WireAny result;
    result.pool = std::make_unique<google::protobuf::DescriptorPool>(
        google::protobuf::DescriptorPool::generated_pool());
    result.factory = std::make_unique<google::protobuf::DynamicMessageFactory>(result.pool.get());

    const auto* prototype = payloadPrototype(anyTypeXml(), *result.pool, *result.factory);
    result.host.reset(prototype->New());
    result.anyField = result.host->GetDescriptor()->FindFieldByName("Payload");
    if (result.anyField == nullptr) throw std::runtime_error{"Payload field not found on Any host"};

    auto* anyMessage = result.host->GetReflection()->MutableMessage(result.host.get(), result.anyField);
    const auto* typeField = anyMessage->GetDescriptor()->FindFieldByName("type");
    const auto* payloadField = anyMessage->GetDescriptor()->FindFieldByName("payload");
    if (typeField == nullptr || payloadField == nullptr) {
        throw std::runtime_error{"Any message is missing its type/payload fields"};
    }
    anyMessage->GetReflection()->SetString(anyMessage, typeField, typeString);
    anyMessage->GetReflection()->SetString(anyMessage, payloadField, payloadBytes);
    return result;
}

// Builds a well-formed wire Any: encodes the payload through the same
// DescriptorBuilder::buildFromTypeXml() pipeline AnyCodec::decode expects
// (a LOCAL pool for the encode side, since DescriptorBuilder names each
// synthetic file by a hash of the type XML and DescriptorPool::BuildFile
// refuses to register the same file name twice -- the encode pool must stay
// distinct from the Any-host pool rawWireAny() builds below), calls `fill`
// to set the encoded wrapper's "Payload" field, serializes it, then wraps
// those bytes as a wire Any via rawWireAny().
inline WireAny wireAny(
    const std::string& innerTypeXml,
    const std::function<void(google::protobuf::Message& payloadWrapper,
                              const google::protobuf::FieldDescriptor& payloadField)>& fill) {
    google::protobuf::DescriptorPool encodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory encodeFactory{&encodePool};
    const auto* prototype = payloadPrototype(innerTypeXml, encodePool, encodeFactory);
    std::unique_ptr<google::protobuf::Message> wrapper{prototype->New()};
    const auto* payloadField = wrapper->GetDescriptor()->FindFieldByName("Payload");
    if (payloadField == nullptr) throw std::runtime_error{"Payload field not found on payload wrapper"};
    fill(*wrapper, *payloadField);

    const std::string serialized = wrapper->SerializeAsString();
    return rawWireAny(innerTypeXml, serialized);
}

}  // namespace anytest
