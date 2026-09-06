// FdlRuntimeParser.cc — Runtime FDL XML to IR parsing (architecture.md §4.2)
#include <sila/client/dynamic/FdlRuntimeParser.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlschemas.h>
#include <libxml/xmlversion.h>
#include <libxslt/security.h>
#include <libxslt/transform.h>
#include <libxslt/xslt.h>
#include <libxslt/xsltutils.h>

#include <sila/client/dynamic/AnyTypeDataTypeXsd.h>
#include <sila/client/dynamic/ConstraintsXsd.h>
#include <sila/client/dynamic/DataTypesXsd.h>
#include <sila/client/dynamic/FeatureDefinitionXsd.h>
#include <sila/client/dynamic/FdlValidationXslt.h>
#include <sila/common/types/Constraints.h>

namespace sila2 {
namespace dynamic {

namespace {

#include "FdlXmlSupport.inc"

void initializeXmlRuntime() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        xmlInitParser();
        xsltInit();
    });
}

struct Diagnostics {
    std::string text;

    void append(std::string_view message) {
        if (message.empty()) return;
        if (!text.empty()) text += "; ";
        text.append(message);
    }
};

void XMLCALL collectStructuredError(void* context, const xmlError* error) noexcept {
    try {
        if (context == nullptr || error == nullptr || error->message == nullptr) {
            return;
        }

        std::string message{error->message};
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.pop_back();
        }
        static_cast<Diagnostics*>(context)->append(message);
    } catch (...) {
        // libxml2 invokes this callback from C; never let a C++ exception cross
        // that boundary and obscure the parser's own failure.
    }
}

void XMLCDECL collectGenericError(void* context, const char* format, ...) noexcept {
    try {
        if (context == nullptr || format == nullptr) {
            return;
        }

        va_list arguments;
        va_start(arguments, format);
        char buffer[1024];
        const int length = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
        va_end(arguments);
        if (length <= 0) return;

        const auto count =
            std::min<std::size_t>(static_cast<std::size_t>(length), sizeof(buffer) - 1);
        auto message = std::string_view{buffer, count};
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.remove_suffix(1);
        }
        static_cast<Diagnostics*>(context)->append(message);
    } catch (...) {
        // libxslt invokes this callback from C; never let a C++ exception cross
        // that boundary and obscure the transformation's own failure.
    }
}

std::string phaseMessage(std::string_view phase, const Diagnostics& diagnostics) {
    std::string message{"FDL "};
    message += phase;
    message += " validation failed";
    if (!diagnostics.text.empty()) {
        message += ": ";
        message += diagnostics.text;
    }
    return message;
}

using XmlDocument = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
using XmlParserContext = std::unique_ptr<xmlParserCtxt, decltype(&xmlFreeParserCtxt)>;
using XsltSecurityPrefs = std::unique_ptr<xsltSecurityPrefs, decltype(&xsltFreeSecurityPrefs)>;
using XsltStylesheet = std::unique_ptr<xsltStylesheet, decltype(&xsltFreeStylesheet)>;
using XsltTransformContext =
    std::unique_ptr<xsltTransformContext, decltype(&xsltFreeTransformContext)>;

XmlDocument readFdlDocument(std::string_view fdlXml) {
    if (fdlXml.empty()) {
        throw std::invalid_argument{"FDL XML parse failed: input is empty"};
    }
    if (fdlXml.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument{"FDL XML parse failed: input is too large"};
    }
    if (fdlXml.find('\0') != std::string_view::npos) {
        throw std::invalid_argument{"FDL XML parse failed: embedded NUL byte"};
    }

    auto* parserContext = xmlNewParserCtxt();
    if (parserContext == nullptr) {
        throw std::invalid_argument{"FDL XML parse failed: unable to create parser context"};
    }
    Diagnostics diagnostics;
    xmlCtxtSetErrorHandler(parserContext, collectStructuredError, &diagnostics);
    auto* document = xmlCtxtReadMemory(parserContext, fdlXml.data(), static_cast<int>(fdlXml.size()),
                                       "fdl.xml", nullptr, secureXmlParseOptions());
    xmlFreeParserCtxt(parserContext);
    if (document == nullptr) {
        throw std::invalid_argument{phaseMessage("XML", diagnostics)};
    }
    return XmlDocument{document, &xmlFreeDoc};
}

// Refuse every resource an inline user schema tries to pull in (xs:import,
// xs:include, schemaLocation). Part A p70 inline schemas must be self-contained;
// this closes the filesystem/network path libxml2's default schema resource
// loader would otherwise take. Distinct from loadEmbeddedSchemaResource (which
// serves the four pinned SiLA XSDs for FDL validation): a user schema is
// arbitrary and gets no resolution at all (design doc, R10-9 section 'Schema').
xmlParserErrors refuseExternalSchemaResource(void*, const char*, const char*, xmlResourceType,
                                             xmlParserInputFlags, xmlParserInput** output) {
    if (output != nullptr) *output = nullptr;
    return XML_IO_ENOENT;
}

void validateWithEmbeddedSchema(xmlDocPtr document, const char* schemaName) {
    const auto* schemaResource = embeddedSchemaForUri(schemaName);
    if (schemaResource == nullptr) {
        throw std::invalid_argument{std::string{"FDL XSD validation failed: embedded schema '"} +
                                    schemaName + "' is unavailable"};
    }
    const auto schemaSize = schemaResource->size;
    if (schemaSize > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument{"FDL XSD validation failed: embedded schema is too large"};
    }

    auto* parserContext =
        xmlSchemaNewMemParserCtxt(schemaResource->content, static_cast<int>(schemaSize));
    if (parserContext == nullptr) {
        throw std::invalid_argument{"FDL XSD validation failed: unable to create schema context"};
    }

    Diagnostics parserDiagnostics;
    xmlSchemaSetParserStructuredErrors(parserContext, collectStructuredError, &parserDiagnostics);
    auto* schema = xmlSchemaParse(parserContext);
    xmlSchemaFreeParserCtxt(parserContext);
    if (schema == nullptr) {
        throw std::invalid_argument{phaseMessage("XSD", parserDiagnostics)};
    }

    auto* validationContext = xmlSchemaNewValidCtxt(schema);
    if (validationContext == nullptr) {
        xmlSchemaFree(schema);
        throw std::invalid_argument{"FDL XSD validation failed: unable to create validator"};
    }

    Diagnostics validationDiagnostics;
    xmlSchemaSetValidStructuredErrors(validationContext, collectStructuredError,
                                      &validationDiagnostics);
    const int result = xmlSchemaValidateDoc(validationContext, document);
    xmlSchemaFreeValidCtxt(validationContext);
    xmlSchemaFree(schema);
    if (result != 0) {
        throw std::invalid_argument{phaseMessage("XSD", validationDiagnostics)};
    }
}

void validateWithOfficialSchema(xmlDocPtr document) {
    validateWithEmbeddedSchema(document, "FeatureDefinition.xsd");
}

void validateWithAnyTypeSchema(xmlDocPtr document) {
    validateWithEmbeddedSchema(document, "AnyTypeDataType.xsd");
}

void validateWithOfficialXslt(xmlDocPtr document) {
    const auto stylesheetSize = sizeof(sila2::generated::kFdlValidationXslt) - 1;
    if (stylesheetSize > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument{"FDL XSLT validation failed: embedded stylesheet is too large"};
    }

    Diagnostics diagnostics;

    XmlParserContext parserContext{xmlNewParserCtxt(), &xmlFreeParserCtxt};
    if (!parserContext) {
        throw std::invalid_argument{
            "FDL XSLT validation failed: unable to create stylesheet parser context"};
    }
    xmlCtxtSetErrorHandler(parserContext.get(), collectStructuredError, &diagnostics);
    auto stylesheetDocument = XmlDocument{
        xmlCtxtReadMemory(parserContext.get(), sila2::generated::kFdlValidationXslt,
                          static_cast<int>(stylesheetSize), "fdl-validation.xsl", nullptr,
                          secureXmlParseOptions()),
        &xmlFreeDoc};
    if (!stylesheetDocument) {
        throw std::invalid_argument{phaseMessage("XSLT", diagnostics)};
    }

    // xsltParseStylesheetDoc owns the document after a successful parse. On
    // failure the document remains ours, so the XmlDocument guard handles it.
    auto* parsedStylesheet = xsltParseStylesheetDoc(stylesheetDocument.get());
    if (parsedStylesheet == nullptr) {
        throw std::invalid_argument{phaseMessage("XSLT", diagnostics)};
    }
    stylesheetDocument.release();
    XsltStylesheet stylesheet{parsedStylesheet, &xsltFreeStylesheet};

    XsltSecurityPrefs securityPrefs{xsltNewSecurityPrefs(), &xsltFreeSecurityPrefs};
    if (!securityPrefs) {
        throw std::invalid_argument{
            "FDL XSLT validation failed: unable to create security preferences"};
    }
    for (const auto option : {XSLT_SECPREF_READ_FILE, XSLT_SECPREF_WRITE_FILE,
                              XSLT_SECPREF_CREATE_DIRECTORY, XSLT_SECPREF_READ_NETWORK,
                              XSLT_SECPREF_WRITE_NETWORK}) {
        if (xsltSetSecurityPrefs(securityPrefs.get(), option, xsltSecurityForbid) != 0) {
            throw std::invalid_argument{
                "FDL XSLT validation failed: unable to configure security preferences"};
        }
    }

    XsltTransformContext transformContext{
        xsltNewTransformContext(stylesheet.get(), document), &xsltFreeTransformContext};
    if (!transformContext) {
        throw std::invalid_argument{
            "FDL XSLT validation failed: unable to create transform context"};
    }
    if (xsltSetCtxtSecurityPrefs(securityPrefs.get(), transformContext.get()) != 0) {
        throw std::invalid_argument{
            "FDL XSLT validation failed: unable to attach security preferences"};
    }
    xsltSetTransformErrorFunc(transformContext.get(), &diagnostics, collectGenericError);

    XmlDocument result{xsltApplyStylesheetUser(stylesheet.get(), document, nullptr, nullptr,
                                                nullptr, transformContext.get()),
                       &xmlFreeDoc};
    if (!result) {
        throw std::invalid_argument{phaseMessage("XSLT", diagnostics)};
    }
}

bool isSilaElement(xmlNodePtr node, const char* localName) {
    return node != nullptr && node->type == XML_ELEMENT_NODE && node->ns != nullptr &&
           node->ns->href != nullptr && xmlStrEqual(node->ns->href, BAD_CAST kSilaNamespace) &&
           xmlStrEqual(node->name, BAD_CAST localName);
}

void qualifyStandaloneDataType(xmlNodePtr node, const xmlNsPtr silaNamespace) {
    if (node == nullptr || node->type != XML_ELEMENT_NODE) return;
    if (node->ns == nullptr) {
        xmlSetNs(node, silaNamespace);
    } else if (node->ns->href == nullptr ||
               !xmlStrEqual(node->ns->href, BAD_CAST kSilaNamespace)) {
        throw std::invalid_argument{
            "DataType XML parse failed: element is outside the SiLA namespace"};
    }
    for (auto* child = node->children; child != nullptr; child = child->next) {
        qualifyStandaloneDataType(child, silaNamespace);
    }
}

const char* localName(xmlNodePtr node) {
    return node != nullptr && node->name != nullptr ? reinterpret_cast<const char*>(node->name)
                                                    : "?";
}

xmlNodePtr requiredChild(xmlNodePtr parent, const char* name) {
    for (auto* child = parent == nullptr ? nullptr : parent->children; child != nullptr;
         child = child->next) {
        if (isSilaElement(child, name)) return child;
    }
    throw std::invalid_argument{std::string{"Missing required element <"} + name + "> in <" +
                                localName(parent) + ">"};
}

std::vector<xmlNodePtr> children(xmlNodePtr parent, const char* name) {
    std::vector<xmlNodePtr> result;
    for (auto* child = parent == nullptr ? nullptr : parent->children; child != nullptr;
         child = child->next) {
        if (isSilaElement(child, name)) result.push_back(child);
    }
    return result;
}

xmlNodePtr optionalChild(xmlNodePtr parent, const char* name) {
    for (auto* child = parent == nullptr ? nullptr : parent->children; child != nullptr;
         child = child->next) {
        if (isSilaElement(child, name)) return child;
    }
    return nullptr;
}

std::string contentOf(xmlNodePtr node) {
    auto* content = xmlNodeGetContent(node);
    if (content == nullptr) return {};
    std::string result{reinterpret_cast<const char*>(content)};
    xmlFree(content);
    return result;
}

std::string requiredAttribute(xmlNodePtr node, const char* name) {
    auto* value = xmlGetNoNsProp(node, BAD_CAST name);
    if (value == nullptr) {
        throw std::invalid_argument{std::string{"Missing required attribute "} + name + " on <" +
                                    localName(node) + ">"};
    }
    std::string result{reinterpret_cast<const char*>(value)};
    xmlFree(value);
    return result;
}

BasicType parseBasicType(std::string_view text) {
    if (text == "String") return BasicType::String;
    if (text == "Integer") return BasicType::Integer;
    if (text == "Real") return BasicType::Real;
    if (text == "Boolean") return BasicType::Boolean;
    if (text == "Binary") return BasicType::Binary;
    if (text == "Date") return BasicType::Date;
    if (text == "Time") return BasicType::Time;
    if (text == "Timestamp") return BasicType::Timestamp;
    if (text == "Any") return BasicType::Any;
    throw std::invalid_argument{std::string{"Unknown Basic data type: "} + std::string{text}};
}

DataType parseDataType(xmlNodePtr dataTypeNode, int depth = 0);

double numericValueOrZero(std::string_view text) {
    try {
        std::size_t consumed = 0;
        const auto result = std::stod(std::string{text}, &consumed);
        if (consumed == text.size()) return result;
    } catch (const std::exception&) {
    }
    // Bounds are deliberately stored lexically. Date/time values and values
    // outside double's range are valid FDL constraint text but have no legacy
    // double representation for the old checker API.
    return 0;
}

ConstraintValue::FqiKind parseFqiKind(std::string_view value) {
    using FqiKind = ConstraintValue::FqiKind;
    if (value == "FeatureIdentifier") return FqiKind::FeatureIdentifier;
    if (value == "CommandIdentifier") return FqiKind::CommandIdentifier;
    if (value == "CommandParameterIdentifier") return FqiKind::CommandParameterIdentifier;
    if (value == "CommandResponseIdentifier") return FqiKind::CommandResponseIdentifier;
    if (value == "IntermediateCommandResponseIdentifier") {
        return FqiKind::IntermediateCommandResponseIdentifier;
    }
    if (value == "DefinedExecutionErrorIdentifier") {
        return FqiKind::DefinedExecutionErrorIdentifier;
    }
    if (value == "PropertyIdentifier") return FqiKind::PropertyIdentifier;
    if (value == "TypeIdentifier") return FqiKind::TypeIdentifier;
    if (value == "MetadataIdentifier") return FqiKind::MetadataIdentifier;
    throw std::invalid_argument{"Unknown FullyQualifiedIdentifier kind: " + std::string{value}};
}

void setScalarConstraintValue(ConstraintValue& constraint, xmlNodePtr node) {
    constraint.lexicalValue = contentOf(node);
    constraint.stringValue = constraint.lexicalValue;
    constraint.numericValue = numericValueOrZero(constraint.lexicalValue);
}

ConstraintValue::SchemaValue::Type parseSchemaType(std::string_view value) {
    using Type = ConstraintValue::SchemaValue::Type;
    if (value == "Xml") return Type::Xml;
    if (value == "Json") return Type::Json;
    throw std::invalid_argument{"Unknown Schema type: " + std::string{value}};
}

std::vector<ConstraintValue> parseConstraints(xmlNodePtr constrainedNode, int depth) {
    std::vector<ConstraintValue> result;
    auto* constraintsNode = optionalChild(constrainedNode, "Constraints");
    if (constraintsNode == nullptr) return result;

    for (auto* child = constraintsNode->children; child != nullptr; child = child->next) {
        if (child->type != XML_ELEMENT_NODE || child->ns == nullptr ||
            !xmlStrEqual(child->ns->href, BAD_CAST kSilaNamespace)) {
            continue;
        }

        const std::string_view name{localName(child)};
        ConstraintValue constraint;
        if (name == "Length") {
            constraint.kind = ConstraintValue::Length;
            setScalarConstraintValue(constraint, child);
        } else if (name == "MinimalLength") {
            constraint.kind = ConstraintValue::MinLength;
            setScalarConstraintValue(constraint, child);
        } else if (name == "MaximalLength") {
            constraint.kind = ConstraintValue::MaxLength;
            setScalarConstraintValue(constraint, child);
        } else if (name == "Pattern") {
            constraint.kind = ConstraintValue::Pattern;
            setScalarConstraintValue(constraint, child);
            // Compile once here so every element of a repeated field shares
            // the same std::regex instead of recompiling it per element (S45).
            // Null on failure; ValueValidator falls back to the uncompiled
            // checkPattern(), which reports the same diagnostic.
            constraint.preparedPattern = types::compilePattern(constraint.stringValue);
        } else if (name == "Set") {
            constraint.kind = ConstraintValue::Set;
            for (auto* valueNode : children(child, "Value")) {
                constraint.stringValues.push_back(contentOf(valueNode));
            }
        } else if (name == "MinimalInclusive") {
            constraint.kind = ConstraintValue::MinInclusive;
            setScalarConstraintValue(constraint, child);
        } else if (name == "MaximalInclusive") {
            constraint.kind = ConstraintValue::MaxInclusive;
            setScalarConstraintValue(constraint, child);
        } else if (name == "MinimalExclusive") {
            constraint.kind = ConstraintValue::MinExclusive;
            setScalarConstraintValue(constraint, child);
        } else if (name == "MaximalExclusive") {
            constraint.kind = ConstraintValue::MaxExclusive;
            setScalarConstraintValue(constraint, child);
        } else if (name == "ElementCount") {
            constraint.kind = ConstraintValue::ElementCount;
            setScalarConstraintValue(constraint, child);
        } else if (name == "MinimalElementCount") {
            constraint.kind = ConstraintValue::MinElementCount;
            setScalarConstraintValue(constraint, child);
        } else if (name == "MaximalElementCount") {
            constraint.kind = ConstraintValue::MaxElementCount;
            setScalarConstraintValue(constraint, child);
        } else if (name == "Unit") {
            constraint.kind = ConstraintValue::Unit;
            ConstraintValue::UnitValue unit;
            unit.label = contentOf(requiredChild(child, "Label"));
            unit.factor = contentOf(requiredChild(child, "Factor"));
            unit.offset = contentOf(requiredChild(child, "Offset"));
            for (auto* componentNode : children(child, "UnitComponent")) {
                ConstraintValue::UnitComponent component;
                component.siUnit = contentOf(requiredChild(componentNode, "SIUnit"));
                component.exponent = contentOf(requiredChild(componentNode, "Exponent"));
                unit.components.push_back(std::move(component));
            }
            constraint.unit = std::move(unit);
        } else if (name == "ContentType") {
            constraint.kind = ConstraintValue::ContentType;
            ConstraintValue::ContentTypeValue contentType;
            contentType.type = contentOf(requiredChild(child, "Type"));
            contentType.subtype = contentOf(requiredChild(child, "Subtype"));
            if (auto* parameters = optionalChild(child, "Parameters")) {
                for (auto* parameter : children(parameters, "Parameter")) {
                    ConstraintValue::ContentTypeParameter value;
                    value.attribute = contentOf(requiredChild(parameter, "Attribute"));
                    value.value = contentOf(requiredChild(parameter, "Value"));
                    contentType.parameters.push_back(std::move(value));
                }
            }
            constraint.contentType = std::move(contentType);
        } else if (name == "Schema") {
            constraint.kind = ConstraintValue::Schema;
            ConstraintValue::SchemaValue schema;
            schema.type = parseSchemaType(contentOf(requiredChild(child, "Type")));
            if (auto* url = optionalChild(child, "Url")) {
                schema.source = ConstraintValue::SchemaValue::Source::Url;
                schema.value = contentOf(url);
            } else if (auto* inlineSchema = optionalChild(child, "Inline")) {
                schema.source = ConstraintValue::SchemaValue::Source::Inline;
                schema.value = contentOf(inlineSchema);
            } else {
                throw std::invalid_argument{"Schema constraint has no Url or Inline value"};
            }
            constraint.schema = std::move(schema);
        } else if (name == "AllowedTypes") {
            constraint.kind = ConstraintValue::AllowedTypes;
            for (auto* allowedTypeNode : children(child, "DataType")) {
                constraint.allowedTypes.push_back(
                    std::make_shared<DataType>(parseDataType(allowedTypeNode, depth + 1)));
            }
        } else if (name == "FullyQualifiedIdentifier") {
            constraint.kind = ConstraintValue::FullyQualifiedIdentifier;
            setScalarConstraintValue(constraint, child);
            constraint.fqiKind = parseFqiKind(constraint.lexicalValue);
        } else {
            // The official XSD rejects unknown children. Keep this guard so a
            // future schema update cannot silently discard a standard field.
            throw std::invalid_argument{"Unsupported SiLA constraint: " + std::string{name}};
        }
        result.push_back(std::move(constraint));
    }
    return result;
}

StructureElement parseStructureElement(xmlNodePtr elementNode, int depth) {
    StructureElement element;
    element.identifier = contentOf(requiredChild(elementNode, "Identifier"));
    element.dataType = std::make_unique<DataType>(
        parseDataType(requiredChild(elementNode, "DataType"), depth));
    return element;
}

DataType parseDataType(xmlNodePtr dataTypeNode, int depth) {
    if (depth > 64) {
        throw std::invalid_argument{"DataType nesting exceeds maximum depth (64)"};
    }

    DataType dataType;
    if (auto* basic = optionalChild(dataTypeNode, "Basic")) {
        dataType.value = DataType::Basic{parseBasicType(contentOf(basic))};
        return dataType;
    }
    if (auto* list = optionalChild(dataTypeNode, "List")) {
        auto inner = std::make_unique<DataType>(
            parseDataType(requiredChild(list, "DataType"), depth + 1));
        dataType.value = DataType::List{std::move(inner)};
        return dataType;
    }
    if (auto* structure = optionalChild(dataTypeNode, "Structure")) {
        DataType::Structure structureValue;
        for (auto* element : children(structure, "Element")) {
            structureValue.elements.push_back(parseStructureElement(element, depth + 1));
        }
        dataType.value = std::move(structureValue);
        return dataType;
    }
    if (auto* constrained = optionalChild(dataTypeNode, "Constrained")) {
        auto inner = std::make_unique<DataType>(
            parseDataType(requiredChild(constrained, "DataType"), depth + 1));
        auto constraints = parseConstraints(constrained, depth);
        dataType.value = DataType::Constrained{std::move(inner), std::move(constraints)};
        return dataType;
    }
    if (auto* typeIdentifier = optionalChild(dataTypeNode, "DataTypeIdentifier")) {
        dataType.value = DataType::Identifier{contentOf(typeIdentifier)};
        return dataType;
    }
    throw std::invalid_argument{"<DataType> has no recognized child element"};
}

Parameter parseParameter(xmlNodePtr parameterNode) {
    Parameter parameter;
    parameter.identifier = contentOf(requiredChild(parameterNode, "Identifier"));
    parameter.dataType = parseDataType(requiredChild(parameterNode, "DataType"));
    return parameter;
}

Command parseCommand(xmlNodePtr commandNode) {
    Command command;
    command.identifier = contentOf(requiredChild(commandNode, "Identifier"));
    command.observable = contentOf(requiredChild(commandNode, "Observable")) == "Yes";
    for (auto* parameter : children(commandNode, "Parameter")) {
        command.parameters.push_back(parseParameter(parameter));
    }
    for (auto* response : children(commandNode, "Response")) {
        command.responses.push_back(parseParameter(response));
    }
    for (auto* intermediateResponse : children(commandNode, "IntermediateResponse")) {
        command.intermediateResponses.push_back(parseParameter(intermediateResponse));
    }
    if (auto* definedExecutionErrors = optionalChild(commandNode, "DefinedExecutionErrors")) {
        for (auto* identifier : children(definedExecutionErrors, "Identifier")) {
            command.definedExecutionErrors.push_back(contentOf(identifier));
        }
    }
    return command;
}

Property parseProperty(xmlNodePtr propertyNode) {
    Property property;
    property.identifier = contentOf(requiredChild(propertyNode, "Identifier"));
    property.observable = contentOf(requiredChild(propertyNode, "Observable")) == "Yes";
    property.dataType = parseDataType(requiredChild(propertyNode, "DataType"));
    return property;
}

Metadata parseMetadata(xmlNodePtr metadataNode) {
    Metadata metadata;
    metadata.identifier = contentOf(requiredChild(metadataNode, "Identifier"));
    metadata.dataType = parseDataType(requiredChild(metadataNode, "DataType"));
    return metadata;
}

DataTypeDefinition parseDataTypeDefinition(xmlNodePtr dataTypeDefinitionNode) {
    DataTypeDefinition dataTypeDefinition;
    dataTypeDefinition.identifier = contentOf(requiredChild(dataTypeDefinitionNode, "Identifier"));
    dataTypeDefinition.dataType = parseDataType(requiredChild(dataTypeDefinitionNode, "DataType"));
    return dataTypeDefinition;
}

void collectDataTypeReferences(const DataType& dataType, std::vector<std::string>& references) {
    std::visit(
        [&references](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, DataType::Identifier>) {
                references.push_back(value.typeId);
            } else if constexpr (std::is_same_v<Value, DataType::List>) {
                if (value.elementType != nullptr) {
                    collectDataTypeReferences(*value.elementType, references);
                }
            } else if constexpr (std::is_same_v<Value, DataType::Structure>) {
                for (const auto& element : value.elements) {
                    if (element.dataType != nullptr) {
                        collectDataTypeReferences(*element.dataType, references);
                    }
                }
            } else if constexpr (std::is_same_v<Value, DataType::Constrained>) {
                if (value.inner != nullptr) {
                    collectDataTypeReferences(*value.inner, references);
                }
            }
        },
        dataType.value);
}

void validateDataTypeDefinitionReferences(const Feature& feature) {
    std::unordered_map<std::string, std::vector<std::string>> references;
    for (const auto& definition : feature.dataTypeDefinitions) {
        auto& definitionReferences = references[definition.identifier];
        collectDataTypeReferences(definition.dataType, definitionReferences);

        // Recursive self-references are valid FDL (and are handled by the
        // generated type machinery); only references between definitions can
        // form an unbounded expansion cycle here.
        definitionReferences.erase(
            std::remove(definitionReferences.begin(), definitionReferences.end(),
                        definition.identifier),
            definitionReferences.end());
    }

    enum class VisitState { Visiting, Visited };
    std::unordered_map<std::string, VisitState> visitState;
    const auto visit = [&references, &visitState](auto&& self,
                                                   const std::string& identifier) -> void {
        const auto existing = visitState.find(identifier);
        if (existing != visitState.end()) {
            if (existing->second == VisitState::Visiting) {
                throw std::invalid_argument{
                    "FDL semantic validation failed: cyclic DataTypeDefinition reference "
                    "involving '" + identifier + "'"};
            }
            return;
        }
        visitState.emplace(identifier, VisitState::Visiting);

        const auto definitionReferences = references.find(identifier);
        if (definitionReferences != references.end()) {
            for (const auto& reference : definitionReferences->second) {
                if (references.find(reference) != references.end()) {
                    self(self, reference);
                }
            }
        }
        visitState[identifier] = VisitState::Visited;
    };

    for (const auto& definition : feature.dataTypeDefinitions) {
        visit(visit, definition.identifier);
    }
}

}  // namespace

Feature parseFdl(std::string_view fdlXml) {
    initializeXmlRuntime();
    auto document = readFdlDocument(fdlXml);
    validateWithOfficialSchema(document.get());
    validateWithOfficialXslt(document.get());

    auto* root = xmlDocGetRootElement(document.get());
    if (!isSilaElement(root, "Feature")) {
        throw std::invalid_argument{"FDL DOM parse failed: root is not <Feature> in the SiLA namespace"};
    }

    Feature feature;
    feature.identifier = contentOf(requiredChild(root, "Identifier"));
    feature.featureVersion = requiredAttribute(root, "FeatureVersion");
    feature.originator = requiredAttribute(root, "Originator");
    if (auto* category = xmlGetNoNsProp(root, BAD_CAST "Category")) {
        feature.category = reinterpret_cast<const char*>(category);
        xmlFree(category);
    }

    for (auto* command : children(root, "Command")) {
        feature.commands.push_back(parseCommand(command));
    }
    for (auto* property : children(root, "Property")) {
        feature.properties.push_back(parseProperty(property));
    }
    for (auto* metadata : children(root, "Metadata")) {
        feature.metadata.push_back(parseMetadata(metadata));
    }
    for (auto* dataTypeDefinition : children(root, "DataTypeDefinition")) {
        feature.dataTypeDefinitions.push_back(parseDataTypeDefinition(dataTypeDefinition));
    }
    validateDataTypeDefinitionReferences(feature);

    return feature;
}

std::optional<std::string> validateXmlAgainstInlineSchema(std::string_view schemaXml,
                                                          std::string_view documentXml) {
    initializeXmlRuntime();  // idempotent (std::call_once); the value path may be the first XML use
    const auto tooLarge = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (schemaXml.size() > tooLarge || documentXml.size() > tooLarge) {
        return std::string{"Schema validation failed: input is too large"};
    }
    // Parse the value as XML with the same hardened options the FDL reader uses
    // (XML_PARSE_NONET|XML_PARSE_NO_XXE, secureXmlParseOptions): no network, no
    // external-entity expansion. An external entity (XXE) is therefore never
    // loaded -- its reference fails the parse, so the value is rejected, never
    // fetched. A malformed value is a rejected value, not an FDL-author error.
    XmlParserContext documentParserContext{xmlNewParserCtxt(), &xmlFreeParserCtxt};
    if (documentParserContext == nullptr) {
        return std::string{"Schema validation failed: unable to create parser context"};
    }
    Diagnostics documentDiagnostics;
    xmlCtxtSetErrorHandler(documentParserContext.get(), collectStructuredError, &documentDiagnostics);
    XmlDocument document{xmlCtxtReadMemory(documentParserContext.get(), documentXml.data(),
                                           static_cast<int>(documentXml.size()), "value.xml", nullptr,
                                           secureXmlParseOptions()),
                         &xmlFreeDoc};
    if (document == nullptr) {
        std::string message{"value is not well-formed XML"};
        if (!documentDiagnostics.text.empty()) message += ": " + documentDiagnostics.text;
        return message;
    }
    // Compile the inline schema. A refuse-everything resource loader blocks any
    // xs:import/xs:include/schemaLocation from reaching disk or the network --
    // the inline schema must be self-contained (Part A p70).
    auto* schemaParserContext =
        xmlSchemaNewMemParserCtxt(schemaXml.data(), static_cast<int>(schemaXml.size()));
    if (schemaParserContext == nullptr) {
        return std::string{"Schema validation failed: unable to create schema context"};
    }
    Diagnostics schemaDiagnostics;
    xmlSchemaSetParserStructuredErrors(schemaParserContext, collectStructuredError, &schemaDiagnostics);
    xmlSchemaSetResourceLoader(schemaParserContext, refuseExternalSchemaResource, nullptr);
    // ponytail: compile the inline XSD per call. A cache keyed by schema text
    // would save the recompile on a hot Schema-constrained command; add it only
    // if such a command is measurably hot (follow-up, R10-9 section 'Schema').
    auto* schema = xmlSchemaParse(schemaParserContext);
    xmlSchemaFreeParserCtxt(schemaParserContext);
    if (schema == nullptr) {
        // The inline schema is not valid XSD -- the FDL author's error, but it
        // surfaces only now (FeatureDefinition.xsd + fdl-validation.xsl validate
        // the FDL structure, not an inline schema's own content). Fail closed:
        // if the declared schema cannot compile, no value can be proven
        // compliant, so reject as a Validation Error rather than throwing.
        std::string message{"inline XML Schema is not valid XSD"};
        if (!schemaDiagnostics.text.empty()) message += ": " + schemaDiagnostics.text;
        return message;
    }
    auto* validationContext = xmlSchemaNewValidCtxt(schema);
    if (validationContext == nullptr) {
        xmlSchemaFree(schema);
        return std::string{"Schema validation failed: unable to create validator"};
    }
    Diagnostics validationDiagnostics;
    xmlSchemaSetValidStructuredErrors(validationContext, collectStructuredError, &validationDiagnostics);
    const int result = xmlSchemaValidateDoc(validationContext, document.get());
    xmlSchemaFreeValidCtxt(validationContext);
    xmlSchemaFree(schema);
    if (result != 0) {
        std::string message{"value does not satisfy the inline XML Schema"};
        if (!validationDiagnostics.text.empty()) message += ": " + validationDiagnostics.text;
        return message;
    }
    return std::nullopt;
}

DataType parseDataTypeXml(std::string_view typeXml) {
    initializeXmlRuntime();
    auto document = readFdlDocument(typeXml);
    auto* root = xmlDocGetRootElement(document.get());
    if (root == nullptr || root->type != XML_ELEMENT_NODE ||
        !xmlStrEqual(root->name, BAD_CAST "DataType") ||
        (root->ns != nullptr &&
         (root->ns->href == nullptr ||
          !xmlStrEqual(root->ns->href, BAD_CAST kSilaNamespace)))) {
        throw std::invalid_argument{
            "DataType XML parse failed: root is not <DataType> in the SiLA namespace"};
    }
    if (root->ns == nullptr) {
        auto* silaNamespace = xmlNewNs(root, BAD_CAST kSilaNamespace, nullptr);
        if (silaNamespace == nullptr) {
            throw std::invalid_argument{
                "DataType XML parse failed: unable to assign the SiLA namespace"};
        }
        qualifyStandaloneDataType(root, silaNamespace);
    }
    for (auto* node = root->next; node != nullptr; node = node->next) {
        if (node->type == XML_ELEMENT_NODE) {
            throw std::invalid_argument{"DataType XML parse failed: multiple root elements"};
        }
    }

    validateWithAnyTypeSchema(document.get());
    return parseDataType(root);
}

}  // namespace dynamic
}  // namespace sila2
