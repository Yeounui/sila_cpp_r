// DescriptorBuilder.h — IR to FileDescriptorProto (architecture.md §4.2)
#pragma once

#include <sila/client/dynamic/FdlIR.h>

#include <google/protobuf/descriptor.pb.h>

namespace sila2 {
namespace dynamic {

class DescriptorBuilder {
public:
    [[nodiscard("caller expects the built FileDescriptorProto")]]
    google::protobuf::FileDescriptorProto build(const Feature& ir);

    [[nodiscard("caller expects the built FileDescriptorProto")]]
    google::protobuf::FileDescriptorProto buildFromTypeXml(std::string_view typeXml);

};

}  // namespace dynamic
}  // namespace sila2
