// AnyCodec.h — SiLA Any value encode/decode (architecture.md §4.2)
#pragma once

#include <memory>
#include <string>

namespace google { namespace protobuf {
class DescriptorPool;
class DynamicMessageFactory;
class Message;
}}

namespace sila2 {
namespace types { struct AnyValue; }
namespace dynamic {

struct AnyCodec {
    [[nodiscard("caller expects the decoded message")]]
    static std::unique_ptr<google::protobuf::Message> decode(
        const sila2::types::AnyValue& any,
        google::protobuf::DescriptorPool& pool,
        google::protobuf::DynamicMessageFactory* factory);

    [[nodiscard("caller expects the encoded AnyValue")]]
    static sila2::types::AnyValue encode(
        const google::protobuf::Message& msg,
        std::string typeXml);

    // Reads the SiLAFramework.Any wire fields (Part B p66: string type = 1;
    // bytes payload = 2) off a proto message by reflection into typeXml/
    // payloadBytes. Returns false when anyMessage is not shaped like a
    // SiLAFramework.Any (a field missing, or not a string/bytes field), so a
    // caller can leave a non-Any message untouched. Both the client validator
    // (ValueValidator::readWireAnyType) and the server binary interceptor read
    // an Any this one way -- neither re-implements the reflection.
    [[nodiscard("caller must handle a non-Any message")]]
    static bool readAnyFields(const google::protobuf::Message& anyMessage,
                              std::string& typeXml, std::string& payloadBytes);
};

}  // namespace dynamic
}  // namespace sila2
