// CommandParameterValidator.h — FDL-backed generated Command input validation.
#pragma once

#include <span>
#include <string>
#include <string_view>

#include <google/protobuf/message.h>

#include <sila/client/dynamic/FdlIR.h>

namespace sila2 {

/// One Schema whose FDL source was a Url, resolved to its text at codegen time.
// One Schema whose FDL source was a Url, resolved to its text at codegen time
// (R10-9g1/g2). The generated <Feature>Meta.cc holds one entry per distinct Url;
// the constructor rewrites each matching Xml or Json Url Schema to use this text
// so the inline-Schema path (ValueValidator::validateSchema) checks it (Part A p70).
struct ProvisionedSchema {
    std::string_view url;        ///< The Schema's Url as it appears in the FDL.
    std::string_view schemaXml;  ///< The resolved schema text (Xml or Json) for that Url.
};

/// Checks a @ref gl_command "Command"'s parameters against its Feature's FDL constraints
/// before the generated service adapter enters the application handler.
///
/// Owns the parsed FDL for one generated Feature and validates each declared
/// Command parameter before its application handler is entered. One instance is held
/// per generated service adapter (`service_adapter.h.j2`), constructed once at server
/// startup from that Feature's FDL XML.
class CommandParameterValidator {
public:
    /// Parses fdlXml once at construction and prepares to validate that Feature's
    /// @ref gl_command "Command" parameters against it.
    /// @param fdlXml The Feature's @ref gl_feature_definition "Feature Definition" as XML text.
    /// @param featureFqi The Feature's Fully Qualified Identifier, used to build each
    ///        offending parameter's own FQI when validate() throws.
    /// @param provisionedSchemas Url -> resolved-text pairs for every Schema Constraint whose
    ///        source is a Url rather than inline text; empty when the Feature declares none.
    CommandParameterValidator(std::string_view fdlXml, std::string_view featureFqi,
                               std::span<const ProvisionedSchema> provisionedSchemas = {});

    /// Validates the request's fields against the named command's declared parameter constraints.
    ///
    /// @throws sila2::error::ValidationError naming the offending parameter's FQI if any
    ///         constraint is violated -- the SiLA client receives this as a
    ///         @ref gl_validation_error "Validation Error", not as the command's own response.
    void validate(std::string_view command, const google::protobuf::Message& request) const;

private:
    std::string featureFqi_;
    dynamic::Feature feature_;
};

}  // namespace sila2
