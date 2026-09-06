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

struct JsonCodec {
    [[nodiscard("caller expects the JSON string")]]
    static std::string toJson(const google::protobuf::Message& msg);

    [[nodiscard("caller expects the deserialized message")]]
    static std::unique_ptr<google::protobuf::Message> fromJson(
        std::string_view json,
        const google::protobuf::Descriptor* desc,
        google::protobuf::MessageFactory* factory);
};

}  // namespace dynamic
}  // namespace sila2
