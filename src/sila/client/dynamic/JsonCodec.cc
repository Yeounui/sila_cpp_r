// JsonCodec.cc — JSON to protobuf message conversion (architecture.md §4.2)
#include <sila/client/dynamic/JsonCodec.h>

#include <stdexcept>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

namespace sila2 {
namespace dynamic {

std::string JsonCodec::toJson(const google::protobuf::Message& msg) {
    std::string output;
    auto status = google::protobuf::util::MessageToJsonString(msg, &output);
    if (!status.ok()) {
        throw std::invalid_argument{
            std::string{"Failed to serialize message to JSON: "} +
            std::string{status.message()}};
    }
    return output;
}

std::unique_ptr<google::protobuf::Message> JsonCodec::fromJson(
    std::string_view json,
    const google::protobuf::Descriptor* desc,
    google::protobuf::MessageFactory* factory) {
    const auto* prototype = factory->GetPrototype(desc);
    auto msg = std::unique_ptr<google::protobuf::Message>{prototype->New()};
    auto status = google::protobuf::util::JsonStringToMessage(
        std::string{json}, msg.get());
    if (!status.ok()) {
        throw std::invalid_argument{
            "Failed to parse JSON into " + std::string{desc->full_name()} +
            ": " + std::string{status.message()}};
    }
    return msg;
}

}  // namespace dynamic
}  // namespace sila2
