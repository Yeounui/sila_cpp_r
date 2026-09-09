// JsonCodec.h — JSON to protobuf message conversion (architecture.md §4.2)
#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace google { namespace protobuf {
class Descriptor;
class Message;
class MessageFactory;
}}

namespace sila2 {
namespace dynamic {

/// Converts a dynamically-built protobuf message to and from JSON text.
///
/// The dynamic client (@ref gl_sila_client "SiLA Client") uses this to hand a
/// request/response built by DynamicCall to a caller as readable JSON, and to
/// turn caller-supplied JSON back into the message DynamicCall expects.
struct JsonCodec {
    /// Serializes msg to its JSON representation.
    /// @throws std::invalid_argument if msg cannot be serialized to JSON.
    [[nodiscard("caller expects the JSON string")]]
    static std::string toJson(const google::protobuf::Message& msg);

    /// Parses json into a new message of the type desc describes.
    /// @param json The JSON text to parse.
    /// @param factory The factory that instantiates messages of desc's type.
    /// @param desc The message type to instantiate, e.g. a request descriptor
    /// obtained from FeatureCatalog.
    /// @throws std::invalid_argument if json does not parse as desc's type.
    [[nodiscard("caller expects the deserialized message")]]
    static std::unique_ptr<google::protobuf::Message> fromJson(
        std::string_view json,
        const google::protobuf::Descriptor* desc,
        google::protobuf::MessageFactory* factory);
};

}  // namespace dynamic
}  // namespace sila2
