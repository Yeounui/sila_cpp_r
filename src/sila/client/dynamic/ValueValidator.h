// ValueValidator.h — validate the supported value constraints in a protobuf message.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <sila/client/dynamic/FdlIR.h>

namespace sila2 {
namespace dynamic {

// The resolver is deliberately small: a caller that has parsed a Feature can
// look up one DataTypeDefinition and return its address.  A null result means
// that the identifier cannot be resolved and validation fails closed.
using DataTypeResolver = std::function<const DataType*(std::string_view)>;

// Unit, ContentType, Schema, and AllowedTypes need caller-owned runtime
// semantics. A null result accepts the constraint; a diagnostic rejects it.
// The final argument is -1 for a scalar field and the repeated element index
// otherwise.
using ConstraintResolver = std::function<std::optional<std::string>(
    const ConstraintValue&, const google::protobuf::Message&,
    const google::protobuf::FieldDescriptor&, int)>;

class ValueValidator {
public:
    // Returns std::nullopt when the selected field is valid, or a diagnostic
    // for the first unsupported/mismatching value or constraint.
    [[nodiscard("caller must inspect the validation result")]]
    static std::optional<std::string> validate(
        const DataType& dataType,
        const google::protobuf::Message& message,
        const google::protobuf::FieldDescriptor& field,
        DataTypeResolver resolver = {}, ConstraintResolver constraintResolver = {});

    // Part A p67 A224 / p69: server-side AllowedTypes decision. The wire Any
    // located by (owner, field, index) must carry a type that structurally
    // matches one entry of constraint.allowedTypes AND whose decoded value
    // satisfies that entry's Constraints. Returns std::nullopt on accept, a
    // Validation Error diagnostic on reject; never throws (malformed type XML,
    // a Custom/Structure-based Any type, or an undecodable payload are all
    // rejections). CommandParameterValidator's resolver forwards the
    // AllowedTypes kind here so both transports decide it identically; the
    // client dynamic path keeps delegating to its caller's ConstraintResolver.
    [[nodiscard("caller must inspect the validation result")]]
    static std::optional<std::string> validateAllowedTypes(
        const ConstraintValue& constraint,
        const google::protobuf::Message& owner,
        const google::protobuf::FieldDescriptor& field, int index);

    // Part A p70/p63: a textual Content Type (text/*, */xml, */*+xml,
    // application/json, */*+json -- matched case-insensitively) on a SiLA
    // Binary requires its bytes to be valid UTF-8. Returns std::nullopt on
    // accept (non-textual media, a String field, or valid UTF-8), a Validation
    // Error diagnostic on reject; never throws. CommandParameterValidator's
    // resolver forwards the ContentType kind here so both transports decide it
    // identically; the client dynamic path keeps delegating ContentType to its
    // caller's ConstraintResolver.
    [[nodiscard("caller must inspect the validation result")]]
    static std::optional<std::string> validateContentType(
        const ConstraintValue& constraint,
        const google::protobuf::Message& owner,
        const google::protobuf::FieldDescriptor& field, int index);

    // Part A p67 A224 / p70: server-side Schema decision for an inline schema
    // (Source Inline, Type Xml OR Json). The String/Binary value located by
    // (owner, field, index) must comply with the inline schema -- XML compliance
    // via libxml2 (FdlRuntimeParser) or JSON Schema compliance via the vcpkg
    // json-schema-validator (JsonSchemaSupport) -- and a Binary's bytes must be
    // UTF-8 first (Part A p70). Returns std::nullopt on accept, a Validation
    // Error diagnostic on reject; never throws (R10-9f/g2). On the generated
    // server path, CommandParameterValidator resolves an Xml OR Json Url Schema
    // to Inline against its codegen-provisioned table BEFORE this is reached,
    // so it is validated like any other inline schema (R10-9g1/g2); only the
    // runtime client path (which has no such table) still reaches this
    // function with an unresolved Url, and accepts it (nullopt).
    // CommandParameterValidator's resolver and the nested resolver in
    // validateAllowedTypesImpl forward Schema here so both transports decide it
    // identically; the client dynamic path keeps delegating Schema to its
    // caller's ConstraintResolver.
    [[nodiscard("caller must inspect the validation result")]]
    static std::optional<std::string> validateSchema(
        const ConstraintValue& constraint, const google::protobuf::Message& owner,
        const google::protobuf::FieldDescriptor& field, int index);
};

}  // namespace dynamic
}  // namespace sila2
