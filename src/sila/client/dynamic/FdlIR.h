// FdlIR.h — Runtime FDL intermediate representation (architecture.md §4.2)
#pragma once

#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <variant>
#include <vector>

namespace sila2 {
namespace dynamic {

enum class BasicType { String, Integer, Real, Boolean, Binary, Date, Time, Timestamp, Any };

struct DataType;

struct StructureElement {
    std::string identifier;
    std::unique_ptr<DataType> dataType;
};

// One parsed <Constraints> child element (SiLA2 FDL schema).
struct ConstraintValue {
    enum Kind {
        Length, MinLength, MaxLength,
        Pattern,
        Set,
        MinInclusive, MaxInclusive, MinExclusive, MaxExclusive,
        ElementCount, MinElementCount, MaxElementCount,
        Unit, ContentType, Schema, AllowedTypes,
        FullyQualifiedIdentifier
    };

    enum class FqiKind {
        Unknown,
        FeatureIdentifier,
        CommandIdentifier,
        CommandParameterIdentifier,
        CommandResponseIdentifier,
        IntermediateCommandResponseIdentifier,
        DefinedExecutionErrorIdentifier,
        PropertyIdentifier,
        TypeIdentifier,
        MetadataIdentifier
    };

    struct UnitComponent {
        std::string siUnit;
        std::string exponent;  // XSD lexical value (xs:integer)
    };

    struct UnitValue {
        std::string label;
        std::string factor;  // XSD lexical value (xs:double)
        std::string offset;  // XSD lexical value (xs:double)
        std::vector<UnitComponent> components;
    };

    struct ContentTypeParameter {
        std::string attribute;
        std::string value;
    };

    struct ContentTypeValue {
        std::string type;
        std::string subtype;
        std::vector<ContentTypeParameter> parameters;
    };

    struct SchemaValue {
        enum class Type { Xml, Json };
        enum class Source { Url, Inline };

        Type type = Type::Xml;
        Source source = Source::Url;
        std::string value;
    };

    Kind kind;
    std::string stringValue;                   // Pattern regex, FQI format string
    std::vector<std::string> stringValues;      // Set entries
    std::string lexicalValue;                  // Original scalar XML text
    double numericValue = 0;                   // Legacy numeric view for existing callers
    FqiKind fqiKind = FqiKind::Unknown;
    std::optional<UnitValue> unit;
    std::optional<ContentTypeValue> contentType;
    std::optional<SchemaValue> schema;
    std::vector<std::shared_ptr<DataType>> allowedTypes;

    // Pattern only: the compiled form of stringValue, prepared once by the FDL
    // runtime parser (sila2::types::compilePattern) so repeated-field validation
    // shares one compiled std::regex across every element instead of recompiling
    // it per element. shared_ptr keeps ConstraintValue copies cheap (refcount
    // bump, not a regex recompile). Null when unprepared (hand-built
    // ConstraintValue, e.g. in tests) or when the pattern could not be
    // compiled -- callers fall back to the uncompiled types::checkPattern then.
    std::shared_ptr<const std::regex> preparedPattern;
};

struct DataType {
    struct Basic { BasicType type; };
    struct List { std::unique_ptr<DataType> elementType; };
    struct Structure { std::vector<StructureElement> elements; };
    struct Constrained {
        std::unique_ptr<DataType> inner;
        std::vector<ConstraintValue> constraints;  // parsed from <Constraints> XML
    };
    struct Identifier { std::string typeId; };
    std::variant<Basic, List, Structure, Constrained, Identifier> value;
};

struct Parameter {
    std::string identifier;
    DataType dataType;
};

struct Command {
    std::string identifier;
    bool observable;
    std::vector<Parameter> parameters;
    std::vector<Parameter> responses;
    std::vector<Parameter> intermediateResponses;
    std::vector<std::string> definedExecutionErrors;
};

struct Property {
    std::string identifier;
    bool observable;
    DataType dataType;
};

// FDL <Metadata>: a client-supplied value attached to calls, carried on the
// wire as a Metadata_<Identifier> message (fdl2proto-messages.xsl:136-166).
// Metadata is never observable, so there is no `observable` flag here.
struct Metadata {
    std::string identifier;
    DataType dataType;
};

struct DataTypeDefinition {
    std::string identifier;
    DataType dataType;
};

struct Feature {
    std::string identifier;
    std::string featureVersion;
    std::string originator;
    std::string category;
    std::vector<Command> commands;
    std::vector<Property> properties;
    std::vector<Metadata> metadata;
    std::vector<DataTypeDefinition> dataTypeDefinitions;
};

}  // namespace dynamic
}  // namespace sila2
