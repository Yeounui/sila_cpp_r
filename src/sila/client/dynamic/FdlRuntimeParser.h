// FdlRuntimeParser.h — Runtime FDL XML to IR parsing (architecture.md §4.2)
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <sila/client/dynamic/FdlIR.h>

namespace sila2 {
namespace dynamic {

Feature parseFdl(std::string_view fdlXml);
DataType parseDataTypeXml(std::string_view typeXml);

// Part A p70 Schema (Type Xml, Source Inline): validate documentXml against the
// inline W3C XML Schema schemaXml. Both are parsed with no external resolution
// (self-contained): a refuse-all resource loader blocks xs:import/xs:include and
// the value document is parsed with XML_PARSE_NONET|XML_PARSE_NO_XXE. Returns
// std::nullopt when documentXml is schema-valid, or a Validation Error
// diagnostic (never throws) so the server resolver can wrap it. Exposed here so
// ValueValidator (libxml2-free) reuses this TU's parse/validate/diagnostics
// block instead of duplicating it.
std::optional<std::string> validateXmlAgainstInlineSchema(std::string_view schemaXml,
                                                          std::string_view documentXml);

}  // namespace dynamic
}  // namespace sila2
