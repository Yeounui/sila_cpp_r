// DescriptorBuilder.cc — IR to FileDescriptorProto (architecture.md §4.2)
#include <sila/client/dynamic/DescriptorBuilder.h>

#include <sila/client/dynamic/FdlRuntimeParser.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sila2 {
namespace dynamic {

namespace gpb = google::protobuf;

namespace {

// Mirrors fdl_parser.py::package_name() exactly, including the "none"
// fallback for an empty Category and the major-version truncation.
std::string packageName(const Feature& ir) {
    std::string category = ir.category.empty() ? "none" : ir.category;
    std::string major = ir.featureVersion.substr(0, ir.featureVersion.find('.'));
    std::string identLower = ir.identifier;
    std::transform(identLower.begin(), identLower.end(), identLower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return "sila2." + ir.originator + "." + category + "." + identLower + ".v" + major;
}

// Mirrors fdl_parser.py::property_rpc_name().
std::string propertyRpcName(const Property& prop) {
    return (prop.observable ? "Subscribe_" : "Get_") + prop.identifier;
}

const char* basicTypeName(BasicType type) {
    switch (type) {
    case BasicType::String:    return "String";
    case BasicType::Integer:   return "Integer";
    case BasicType::Real:      return "Real";
    case BasicType::Boolean:   return "Boolean";
    case BasicType::Binary:    return "Binary";
    case BasicType::Date:      return "Date";
    case BasicType::Time:      return "Time";
    case BasicType::Timestamp: return "Timestamp";
    case BasicType::Any:       return "Any";
    }
    return "String"; // unreachable: BasicType is exhaustively enumerated above
}

const DataType& dataTypeOf(const Parameter& parameter) {
    return parameter.dataType;
}

const DataType& dataTypeOf(const StructureElement& element) {
    return *element.dataType;
}

void addField(gpb::DescriptorProto* msg, const std::string& fieldId, const DataType& dt,
              const std::string& pkg, const std::string& msgPath, int number);

// Mutually recursive with addField(): a Structure field attaches a nested
// message to `parentMsg` and recurses into its own elements.
template <typename Element>
void fillFields(gpb::DescriptorProto* msg, const std::vector<Element>& elements,
                 const std::string& pkg, const std::string& msgPath) {
    int number = 1;
    for (const auto& element : elements) {
        addField(msg, element.identifier, dataTypeOf(element), pkg, msgPath, number++);
    }
}

// Mirrors proto_emitter.py::_resolve_type() exactly. All SiLA field types
// resolve to TYPE_MESSAGE; the variant only changes label and type_name.
void resolveType(const std::string& fieldId, const DataType& dt, const std::string& pkg,
                  const std::string& msgPath, gpb::DescriptorProto* parentMsg,
                  gpb::FieldDescriptorProto* field) {
    field->set_type(gpb::FieldDescriptorProto::TYPE_MESSAGE);
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, DataType::Basic>) {
                field->set_type_name(std::string{".sila2.org.silastandard."} +
                                      basicTypeName(value.type));
            } else if constexpr (std::is_same_v<T, DataType::List>) {
                field->set_label(gpb::FieldDescriptorProto::LABEL_REPEATED);
                resolveType(fieldId, *value.elementType, pkg, msgPath, parentMsg, field);
            } else if constexpr (std::is_same_v<T, DataType::Structure>) {
                std::string structName = fieldId + "_Struct";
                auto* nested = parentMsg->add_nested_type();
                nested->set_name(structName);
                fillFields(nested, value.elements, pkg, msgPath + "." + structName);
                field->set_type_name("." + pkg + "." + msgPath + "." + structName);
            } else if constexpr (std::is_same_v<T, DataType::Constrained>) {
                // Constraints are not representable in proto; unwrap.
                resolveType(fieldId, *value.inner, pkg, msgPath, parentMsg, field);
            } else if constexpr (std::is_same_v<T, DataType::Identifier>) {
                field->set_type_name("." + pkg + ".DataType_" + value.typeId);
            }
        },
        dt.value);
}

void addField(gpb::DescriptorProto* msg, const std::string& fieldId, const DataType& dt,
              const std::string& pkg, const std::string& msgPath, int number) {
    auto* field = msg->add_field();
    field->set_name(fieldId);
    field->set_number(number);
    field->set_label(gpb::FieldDescriptorProto::LABEL_OPTIONAL);
    resolveType(fieldId, dt, pkg, msgPath, msg, field);
}

// Builds a top-level message from a list of Parameter/StructureElement.
template <typename Element>
gpb::DescriptorProto* addMessage(gpb::FileDescriptorProto* file, const std::string& name,
                                  const std::vector<Element>& elements, const std::string& pkg) {
    auto* msg = file->add_message_type();
    msg->set_name(name);
    fillFields(msg, elements, pkg, name);
    return msg;
}

// Builds a top-level message with a single field, used for DataTypeDefinition
// messages and for a Property's Responses message (the property itself).
gpb::DescriptorProto* addMessageFromDataType(gpb::FileDescriptorProto* file, const std::string& name,
                                              const std::string& fieldId, const DataType& dt,
                                              const std::string& pkg) {
    auto* msg = file->add_message_type();
    msg->set_name(name);
    addField(msg, fieldId, dt, pkg, name, 1);
    return msg;
}

void addMethod(gpb::ServiceDescriptorProto* service, const std::string& name,
               const std::string& inputType, const std::string& outputType, bool streaming) {
    auto* method = service->add_method();
    method->set_name(name);
    method->set_input_type("." + inputType);
    method->set_output_type("." + outputType);
    method->set_server_streaming(streaming);
}

void addCommandRpcs(gpb::ServiceDescriptorProto* service, const Command& cmd, const std::string& pkg) {
    const std::string& name = cmd.identifier;
    if (!cmd.observable) {
        addMethod(service, name, pkg + "." + name + "_Parameters", pkg + "." + name + "_Responses",
                  false);
        return;
    }
    addMethod(service, name, pkg + "." + name + "_Parameters",
              "sila2.org.silastandard.CommandConfirmation", false);
    addMethod(service, name + "_Info", "sila2.org.silastandard.CommandExecutionUUID",
              "sila2.org.silastandard.ExecutionInfo", true);
    if (!cmd.intermediateResponses.empty()) {
        addMethod(service, name + "_Intermediate", "sila2.org.silastandard.CommandExecutionUUID",
                  pkg + "." + name + "_IntermediateResponses", true);
    }
    addMethod(service, name + "_Result", "sila2.org.silastandard.CommandExecutionUUID",
              pkg + "." + name + "_Responses", false);
}

void addPropertyRpc(gpb::ServiceDescriptorProto* service, const Property& prop, const std::string& pkg) {
    std::string rpcName = propertyRpcName(prop);
    addMethod(service, rpcName, pkg + "." + rpcName + "_Parameters",
              pkg + "." + rpcName + "_Responses", prop.observable);
}

// Per fdl2proto-messages.xsl:136-166 each <Metadata> contributes three
// messages. Only _Responses is hand-built: its single field is a repeated
// framework String fixed by the stylesheet, with no counterpart in the FDL IR.
void addMetadataMessages(gpb::FileDescriptorProto* file, const Metadata& meta,
                         const std::string& pkg) {
    const std::string rpcName = "Get_FCPAffectedByMetadata_" + meta.identifier;
    addMessage(file, rpcName + "_Parameters", std::vector<Parameter>{}, pkg);

    auto* responses = file->add_message_type();
    responses->set_name(rpcName + "_Responses");
    auto* affected = responses->add_field();
    affected->set_name("AffectedCalls");
    affected->set_number(1);
    affected->set_label(gpb::FieldDescriptorProto::LABEL_REPEATED);
    affected->set_type(gpb::FieldDescriptorProto::TYPE_MESSAGE);
    affected->set_type_name(".sila2.org.silastandard.String");

    addMessageFromDataType(file, "Metadata_" + meta.identifier, meta.identifier,
                           meta.dataType, pkg);
}

}  // namespace

gpb::FileDescriptorProto DescriptorBuilder::build(const Feature& ir) {
    std::string pkg = packageName(ir);

    gpb::FileDescriptorProto file;
    file.set_name(pkg + ".proto"); // synthetic file name, not backed by a real file
    file.set_package(pkg);
    file.set_syntax("proto3");
    file.add_dependency("SiLAFramework.proto");

    for (const auto& dtd : ir.dataTypeDefinitions) {
        addMessageFromDataType(&file, "DataType_" + dtd.identifier, dtd.identifier, dtd.dataType, pkg);
    }

    for (const auto& cmd : ir.commands) {
        addMessage(&file, cmd.identifier + "_Parameters", cmd.parameters, pkg);
        addMessage(&file, cmd.identifier + "_Responses", cmd.responses, pkg);
        if (cmd.observable && !cmd.intermediateResponses.empty()) {
            addMessage(&file, cmd.identifier + "_IntermediateResponses", cmd.intermediateResponses,
                       pkg);
        }
    }

    for (const auto& prop : ir.properties) {
        std::string rpcName = propertyRpcName(prop);
        addMessage(&file, rpcName + "_Parameters", std::vector<Parameter>{}, pkg);
        addMessageFromDataType(&file, rpcName + "_Responses", prop.identifier, prop.dataType, pkg);
    }

    for (const auto& meta : ir.metadata) {
        addMetadataMessages(&file, meta, pkg);
    }

    auto* service = file.add_service();
    service->set_name(ir.identifier);
    for (const auto& cmd : ir.commands) {
        addCommandRpcs(service, cmd, pkg);
    }
    for (const auto& prop : ir.properties) {
        addPropertyRpc(service, prop, pkg);
    }
    for (const auto& meta : ir.metadata) {
        const std::string rpcName = "Get_FCPAffectedByMetadata_" + meta.identifier;
        // Unary: metadata FCP discovery is never streamed
        // (fdl2proto-service.xsl:74-82).
        addMethod(service, rpcName, pkg + "." + rpcName + "_Parameters",
                  pkg + "." + rpcName + "_Responses", false);
    }

    return file;
}

gpb::FileDescriptorProto DescriptorBuilder::buildFromTypeXml(std::string_view typeXml) {
    // Hash the typeXml to produce a unique Identifier per distinct type,
    // preventing DescriptorPool collisions on repeated decode() calls.
    auto xmlHash = std::hash<std::string_view>{}(typeXml);
    Feature ir;
    ir.identifier = "AnyPayload" + std::to_string(xmlHash);
    ir.featureVersion = "1.0";
    ir.originator = "anon";
    ir.category = "anon";
    ir.dataTypeDefinitions.push_back(
        DataTypeDefinition{"Payload", parseDataTypeXml(typeXml)});
    return build(ir);
}

}  // namespace dynamic
}  // namespace sila2
