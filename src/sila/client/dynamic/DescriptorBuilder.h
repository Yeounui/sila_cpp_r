// DescriptorBuilder.h — IR to FileDescriptorProto (architecture.md §4.2)
#pragma once

#include <sila/client/dynamic/FdlIR.h>

#include <google/protobuf/descriptor.pb.h>

namespace sila2 {
namespace dynamic {

/// Turns a parsed @ref gl_feature_definition "Feature Definition" (FdlIR's
/// Feature) into the protobuf FileDescriptorProto that
/// FeatureCatalog::add() registers, so the dynamic client can build and read
/// request/response messages for that Feature's commands and properties
/// without codegen. Also used by AnyCodec to build a one-message file for a
/// @ref gl_sila_any_type "SiLA Any Type"'s inline type XML.
class DescriptorBuilder {
public:
    /// Builds the FileDescriptorProto for a whole Feature: one message per
    /// command parameter/response, property, and Metadata_<Identifier>.
    [[nodiscard("caller expects the built FileDescriptorProto")]]
    google::protobuf::FileDescriptorProto build(const Feature& ir);

    /// Builds a single-message FileDescriptorProto for one Basic/List/
    /// Structure/Constrained type XML (as carried by a
    /// @ref gl_sila_any_type "SiLA Any Type"), named DataType_Payload.
    [[nodiscard("caller expects the built FileDescriptorProto")]]
    google::protobuf::FileDescriptorProto buildFromTypeXml(std::string_view typeXml);

};

}  // namespace dynamic
}  // namespace sila2
