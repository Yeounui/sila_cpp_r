// JsonSchemaSupport.cc — the ONLY TU that includes the JSON schema library.
#include <sila/client/dynamic/JsonSchemaSupport.h>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <exception>
#include <stdexcept>
#include <string>

namespace sila2 {
namespace dynamic {

std::optional<std::string> validateJsonAgainstInlineSchema(std::string_view schemaJson,
                                                            std::string_view documentJson) {
    using nlohmann::json;
    using nlohmann::json_schema::json_validator;

    // Parse the value as JSON. A malformed value is a rejected value (Part A
    // p70), never a throw that escapes the resolver.
    json instance;
    try {
        instance = json::parse(documentJson.begin(), documentJson.end());
    } catch (const std::exception& error) {
        return std::string{"value is not well-formed JSON: "} + error.what();
    }

    // Parse the inline schema text as JSON before compiling it -- an
    // FDL-author error surfaces only now (FeatureDefinition.xsd validates the
    // FDL structure, not the inline schema's own content).
    json schemaDoc;
    try {
        schemaDoc = json::parse(schemaJson.begin(), schemaJson.end());
    } catch (const std::exception& error) {
        return std::string{"inline JSON Schema is not well-formed JSON: "} + error.what();
    }

    // Refuse every remote/external $ref: the loader throws before any network
    // or filesystem access, so set_root_schema fails closed on a schema that
    // needs one (Part A p70: the inline schema must be self-contained -- the
    // JSON twin of validateXmlAgainstInlineSchema's refuse-all resource
    // loader). A local '#/...' $ref resolves inside the root schema without
    // the loader. Leaving the loader unset also throws on an unresolved remote
    // $ref, but an explicit refusing loader documents the guarantee and is
    // immune to any future default-loader change.
    const nlohmann::json_schema::schema_loader refuseExternalRef =
        [](const nlohmann::json_uri& uri, json&) {
            throw std::runtime_error{
                "external $ref refused (no network or filesystem resolution): " + uri.url()};
        };

    // Implementation policy, not a proven ceiling: pboettch json-schema-
    // validator 2.4.0 compiles JSON Schema draft-7 (partial 2019-09); the SiLA
    // spec names no draft (Part A p70), so draft-7 is our stated policy.
    json_validator validator{refuseExternalRef};
    try {
        validator.set_root_schema(schemaDoc);
    } catch (const std::exception& error) {
        // Fail closed: a schema that cannot compile proves no value compliant
        // (mirrors validateXmlAgainstInlineSchema's xmlSchemaParse==nullptr arm).
        return std::string{"inline JSON Schema is not valid: "} + error.what();
    }

    try {
        validator.validate(instance);
    } catch (const std::exception& error) {
        return std::string{"value does not satisfy the inline JSON Schema: "} + error.what();
    }

    return std::nullopt;
}

}  // namespace dynamic
}  // namespace sila2
