// AnyCodec.cc — SiLA Any value encode/decode (architecture.md §4.2)
#include <sila/client/dynamic/AnyCodec.h>

#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/common/types/BasicTypes.h>

#include <limits>
#include <stdexcept>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

namespace sila2 {
namespace dynamic {

std::unique_ptr<google::protobuf::Message> AnyCodec::decode(
        const sila2::types::AnyValue& any,
        google::protobuf::DescriptorPool& pool,
        google::protobuf::DynamicMessageFactory* factory) {
    if (factory == nullptr) {
        throw std::invalid_argument{"AnyCodec::decode: message factory is null"};
    }
    DescriptorBuilder builder;
    google::protobuf::FileDescriptorProto fileProto = builder.buildFromTypeXml(any.typeXml);

    const google::protobuf::FileDescriptor* file = pool.BuildFile(fileProto);
    if (file == nullptr) {
        throw std::invalid_argument{"AnyCodec::decode: failed to build descriptor for type XML"};
    }

    const std::string messageName = fileProto.package() + ".DataType_Payload";
    const google::protobuf::Descriptor* desc = pool.FindMessageTypeByName(messageName);
    if (desc == nullptr) {
        throw std::invalid_argument{"AnyCodec::decode: DataType_Payload message not found"};
    }

    const google::protobuf::Message* prototype = factory->GetPrototype(desc);
    if (prototype == nullptr) {
        throw std::invalid_argument{"AnyCodec::decode: unable to create message prototype"};
    }
    auto msg = std::unique_ptr<google::protobuf::Message>{prototype->New()};
    if (msg == nullptr) {
        throw std::invalid_argument{"AnyCodec::decode: unable to allocate message"};
    }
    if (any.payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !msg->ParseFromArray(any.payload.data(), static_cast<int>(any.payload.size()))) {
        throw std::invalid_argument{"AnyCodec::decode: payload does not match type XML"};
    }
    return msg;
}

sila2::types::AnyValue AnyCodec::encode(
        const google::protobuf::Message& msg,
        std::string typeXml) {
    sila2::types::AnyValue result;
    result.typeXml = std::move(typeXml);
    std::string serialized = msg.SerializeAsString();
    result.payload.assign(serialized.begin(), serialized.end());
    return result;
}

bool AnyCodec::readAnyFields(const google::protobuf::Message& anyMessage,
                             std::string& typeXml, std::string& payloadBytes) {
    const google::protobuf::Descriptor* descriptor = anyMessage.GetDescriptor();
    const google::protobuf::FieldDescriptor* typeField = descriptor->FindFieldByName("type");
    const google::protobuf::FieldDescriptor* payloadField = descriptor->FindFieldByName("payload");
    // bytes and string both report CPPTYPE_STRING, so this admits the wire
    // Any's `bytes payload` alongside its `string type`.
    if (typeField == nullptr || payloadField == nullptr ||
        typeField->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING ||
        payloadField->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
        return false;
    }
    const google::protobuf::Reflection* reflection = anyMessage.GetReflection();
    typeXml = reflection->GetString(anyMessage, typeField);
    payloadBytes = reflection->GetString(anyMessage, payloadField);
    return true;
}

}  // namespace dynamic
}  // namespace sila2
