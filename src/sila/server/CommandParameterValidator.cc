// CommandParameterValidator.cc
#include <sila/server/CommandParameterValidator.h>

#include <sila/client/dynamic/FdlRuntimeParser.h>
#include <sila/client/dynamic/ValueValidator.h>
#include <sila/common/error/SiLAErrorSubtypes.h>

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace sila2 {

namespace {
// Walk one parsed DataType and, for every Schema constraint whose source is a
// Url (Xml OR Json), replace it in place with the provisioned inline text so
// the existing inline-Schema validation path checks it (Part A p70: a Url is
// an alternative source of the schema, not an exemption from A224). Also
// descends into every AllowedTypes constraint's candidates (R10-9g2): an
// Any's candidate DataType carries its own Constrained subtree, and a Schema
// Url nested there needs the same provisioning. A Url absent from a supplied
// (non-empty) table is a codegen defect -> fail closed, mirroring the
// 'generated Command is absent from its FDL' logic_error.
void rewriteSchemaUrls(dynamic::DataType& dataType, std::span<const ProvisionedSchema> table) {
    using Schema = dynamic::ConstraintValue::SchemaValue;
    std::visit([&](auto& node) {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, dynamic::DataType::List>) {
            rewriteSchemaUrls(*node.elementType, table);
        } else if constexpr (std::is_same_v<Node, dynamic::DataType::Structure>) {
            for (auto& element : node.elements) rewriteSchemaUrls(*element.dataType, table);
        } else if constexpr (std::is_same_v<Node, dynamic::DataType::Constrained>) {
            for (auto& constraint : node.constraints) {
                // R10-9g2: an Any's AllowedTypes candidates carry their own
                // DataType subtrees; a Schema Url nested in one must be
                // provisioned like any other (Part A p70). rewriteSchemaUrls
                // skipped them before g2, so a Url-sourced Schema inside
                // AllowedTypes was never resolved and the nested resolver in
                // validateAllowedTypesImpl accepted it.
                if (constraint.kind == dynamic::ConstraintValue::AllowedTypes) {
                    for (auto& candidate : constraint.allowedTypes) {
                        if (candidate != nullptr) rewriteSchemaUrls(*candidate, table);
                    }
                    continue;  // an AllowedTypes constraint carries no Schema of its own
                }
                if (constraint.kind != dynamic::ConstraintValue::Schema) continue;
                if (!constraint.schema.has_value()) continue;
                auto& schema = *constraint.schema;
                if (schema.source != Schema::Source::Url) continue;
                // R10-9g2: both Xml AND Json Url schemas are provisioned and
                // validated now (Part A p70: Type is Xml or Json); the Xml-only
                // guard is gone.
                const auto entry = std::find_if(table.begin(), table.end(),
                    [&](const ProvisionedSchema& e) { return e.url == schema.value; });
                if (entry == table.end()) {
                    throw std::logic_error{
                        "generated Schema Url is absent from the provisioned schema table: " + schema.value};
                }
                schema.source = Schema::Source::Inline;
                schema.value = std::string(entry->schemaXml);  // string_view -> string (not braces: template ctor deduction)
            }
            rewriteSchemaUrls(*node.inner, table);
        }
        // Basic and Identifier hold no constraints; Identifier resolves through
        // dataTypeDefinitions, which the constructor walks separately.
    }, dataType.value);
}
}  // namespace

CommandParameterValidator::CommandParameterValidator(
    std::string_view fdlXml, std::string_view featureFqi,
    std::span<const ProvisionedSchema> provisionedSchemas)
    : featureFqi_{featureFqi}, feature_{dynamic::parseFdl(fdlXml)} {
    // R10-9g1: resolve every Xml/Url Schema against the codegen-provisioned
    // table now, at construction, so the validation path never touches the
    // network (Part A p70). Only the generated server path supplies a table;
    // the runtime client path and direct-from-text tests pass none, leaving a
    // Url unresolved -> ValueValidator::validateSchema keeps accepting it (the
    // documented asymmetry, since a real server always constructs from
    // generated code).
    if (provisionedSchemas.empty()) return;
    for (auto& command : feature_.commands) {
        for (auto& parameter : command.parameters) rewriteSchemaUrls(parameter.dataType, provisionedSchemas);
    }
    for (auto& definition : feature_.dataTypeDefinitions) rewriteSchemaUrls(definition.dataType, provisionedSchemas);
}

void CommandParameterValidator::validate(std::string_view command,
                                         const google::protobuf::Message& request) const {
    const auto commandIt = std::find_if(
        feature_.commands.begin(), feature_.commands.end(), [command](const auto& candidate) {
            return candidate.identifier == command;
        });
    if (commandIt == feature_.commands.end()) {
        throw std::logic_error{"generated Command is absent from its FDL"};
    }

    const auto resolveType = [this](std::string_view identifier) -> const dynamic::DataType* {
        const auto typeIt = std::find_if(
            feature_.dataTypeDefinitions.begin(), feature_.dataTypeDefinitions.end(),
            [identifier](const auto& candidate) { return candidate.identifier == identifier; });
        return typeIt == feature_.dataTypeDefinitions.end() ? nullptr : &typeIt->dataType;
    };
    // Part A p67 A224: the server MUST check every Constraint. Three value-
    // dependent kinds are decided here because this server can prove a
    // violation from the wire alone: AllowedTypes (the wire Any carries its
    // own type -- ValueValidator::validateAllowedTypes), ContentType (a
    // textual Content Type on a Binary requires UTF-8 bytes, Part A p70/p63 --
    // ValueValidator::validateContentType), and Schema (a String/Binary value
    // must comply with an inline W3C XML Schema, Part A p70 --
    // ValueValidator::validateSchema). Unit stays a documented no-op (Part A
    // p69: nothing is transmitted for it). An Xml OR Json Url Schema was
    // rewritten to Inline in the constructor above (R10-9g1/g2) when a
    // provisioned table was supplied, so it reaches validateSchema's Inline
    // arm like any other inline schema. ValueValidator
    // itself still enforces the value-independent kinds (Pattern, Length, Set,
    // Min/Max) and validates each Any against its own embedded type (Part B
    // p66).
    const dynamic::ConstraintResolver resolveConstraint =
        [](const dynamic::ConstraintValue& constraint, const google::protobuf::Message& owner,
           const google::protobuf::FieldDescriptor& field, int index) -> std::optional<std::string> {
        if (constraint.kind == dynamic::ConstraintValue::AllowedTypes) {
            return dynamic::ValueValidator::validateAllowedTypes(constraint, owner, field, index);
        }
        if (constraint.kind == dynamic::ConstraintValue::ContentType) {
            return dynamic::ValueValidator::validateContentType(constraint, owner, field, index);
        }
        if (constraint.kind == dynamic::ConstraintValue::Schema) {
            return dynamic::ValueValidator::validateSchema(constraint, owner, field, index);
        }
        return std::nullopt;
    };

    for (const auto& parameter : commandIt->parameters) {
        const auto* field = request.GetDescriptor()->FindFieldByName(parameter.identifier);
        if (field == nullptr) {
            throw std::logic_error{"generated Command parameter is absent from its protobuf request"};
        }
        if (const auto error = dynamic::ValueValidator::validate(
                parameter.dataType, request, *field, resolveType, resolveConstraint)) {
            throw error::ValidationError{
                featureFqi_ + "/Command/" + std::string{command} + "/Parameter/" +
                    parameter.identifier,
                *error};
        }
    }
}

}  // namespace sila2
