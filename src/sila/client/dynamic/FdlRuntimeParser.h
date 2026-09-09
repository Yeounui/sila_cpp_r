// FdlRuntimeParser.h — Runtime FDL XML to IR parsing (architecture.md §4.2)
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <sila/client/dynamic/FdlIR.h>

namespace sila2 {
namespace dynamic {

/// Parses one @ref gl_feature_definition "Feature Definition" XML document
/// into the Feature intermediate representation FeatureCatalog::add() and
/// DescriptorBuilder consume.
/// @throws std::invalid_argument if fdlXml is empty, not well-formed XML,
/// fails the official SiLA FDL XSD/XSLT validation, or names an unknown
/// @ref BasicType, unsupported @ref gl_constraint "Constraint", or a
/// FullyQualifiedIdentifier kind this parser does not recognize.
Feature parseFdl(std::string_view fdlXml);
/// Parses a standalone `<DataType>` XML document -- the type XML carried
/// inside a wire @ref gl_sila_any_type "SiLA Any Type" (`SiLAFramework.Any`)
/// message -- into a DataType, without the enclosing `<Feature>` element
/// parseFdl() expects.
/// @throws std::invalid_argument on the same conditions as parseFdl(), plus
/// if the root element is not `<DataType>` or the document has more than one
/// root element.
DataType parseDataTypeXml(std::string_view typeXml);

/// Checks documentXml against a `Schema` @ref gl_constraint "Constraint"'s
/// inline W3C XML Schema, schemaXml.
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
