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

/// Converts between a @ref gl_sila_any_type "SiLA Any Type" value (a type XML
/// string plus a serialized payload, sila2::types::AnyValue) and the
/// dynamically-built protobuf message DescriptorBuilder::buildFromTypeXml
/// describes for that type. Used wherever an Any crosses the dynamic
/// client/server boundary: ValueValidator::validateAllowedTypes and the
/// binary interceptors read the wire fields via readAnyFields().
struct AnyCodec {
    /// Decodes any's payload into a message of the type any.typeXml
    /// describes, registering that type in pool if not already present.
    /// @throws std::invalid_argument if factory is null, typeXml fails to
    /// parse, or payload does not match the decoded type.
    [[nodiscard("caller expects the decoded message")]]
    static std::unique_ptr<google::protobuf::Message> decode(
        const sila2::types::AnyValue& any,
        google::protobuf::DescriptorPool& pool,
        google::protobuf::DynamicMessageFactory* factory);

    /// Serializes msg into an AnyValue carrying the given type XML.
    [[nodiscard("caller expects the encoded AnyValue")]]
    static sila2::types::AnyValue encode(
        const google::protobuf::Message& msg,
        std::string typeXml);

    /// Reads a @ref gl_sila_any_type "SiLA Any Type" message's wire fields
    /// (its type XML and serialized payload) into typeXml and payloadBytes
    /// via protobuf reflection.
    /// @return false if anyMessage is not shaped like a SiLA Any (a field is
    /// missing or the wrong type), letting the caller leave it untouched.
    ///
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
