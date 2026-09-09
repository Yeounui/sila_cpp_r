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

/// Mirrors a Feature Definition's `<Basic>` element: the fixed SiLA scalar
/// kinds (String, Integer, Real, Boolean, Binary, Date, Time, Timestamp, Any) without further
/// structure.
enum class BasicType { String, Integer, Real, Boolean, Binary, Date, Time, Timestamp, Any };

struct DataType;

/// Mirrors one `<Element>` of a Feature Definition's `<Structure>` data type.
struct StructureElement {
    std::string identifier;
    std::unique_ptr<DataType> dataType;
};

// One parsed <Constraints> child element (SiLA2 FDL schema).
/// One parsed `<Constraints>` child element -- mirrors a Feature Definition's
/// @ref gl_constraint "Constraint" on a `Constrained` data type.
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

    /// Which @ref gl_fully_qualified_identifier "Fully Qualified Identifier"
    /// kind a `FullyQualifiedIdentifier` Constraint restricts values to.
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

    /// Mirrors one `<UnitComponent>` of a Unit Constraint: one SI unit raised
    /// to an exponent.
    struct UnitComponent {
        std::string siUnit;
        std::string exponent;  // XSD lexical value (xs:integer)
    };

    /// Mirrors a `Unit` Constraint's `<Unit>` element: a labeled unit derived
    /// from SI components via a linear factor/offset.
    struct UnitValue {
        std::string label;
        std::string factor;  // XSD lexical value (xs:double)
        std::string offset;  // XSD lexical value (xs:double)
        std::vector<UnitComponent> components;
    };

    /// Mirrors one `<Parameter>` of a ContentType Constraint's MIME type.
    struct ContentTypeParameter {
        std::string attribute;
        std::string value;
    };

    /// Mirrors a `ContentType` Constraint: a MIME type/subtype plus
    /// parameters that a Binary value
    /// must match.
    struct ContentTypeValue {
        std::string type;
        std::string subtype;
        std::vector<ContentTypeParameter> parameters;
    };

    /// Mirrors a `Schema` Constraint: an XML or JSON schema, given either by
    /// URL or inline, that a String value must validate against.
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

/// Mirrors a Feature Definition's `<DataType>` element: one of the five
/// SiLA Data Type shapes (Basic, List, Structure,
/// Constrained, or a `<DataTypeIdentifier>` reference).
struct DataType {
    /// Mirrors `<DataType><Basic>`.
    struct Basic { BasicType type; };
    /// Mirrors `<DataType><List>`: a repeated value of elementType.
    struct List { std::unique_ptr<DataType> elementType; };
    /// Mirrors `<DataType><Structure>`.
    struct Structure { std::vector<StructureElement> elements; };
    /// Mirrors `<DataType><Constrained>`: inner restricted by constraints.
    struct Constrained {
        std::unique_ptr<DataType> inner;
        std::vector<ConstraintValue> constraints;  // parsed from <Constraints> XML
    };
    /// Mirrors `<DataType><DataTypeIdentifier>`: a reference to a
    /// DataTypeDefinition's identifier, resolved by the caller.
    struct Identifier { std::string typeId; };
    std::variant<Basic, List, Structure, Constrained, Identifier> value;
};

/// Mirrors one `<Parameter>` or `<Response>` element of a Command, or a
/// Property's response element.
struct Parameter {
    std::string identifier;
    DataType dataType;
};

/// Mirrors a Feature Definition's `<Command>` element: one @ref gl_command "Command" ,
/// @ref gl_observable_command "Observable" or @ref gl_unobservable_command "Unobservable" per the
/// observable flag.
struct Command {
    std::string identifier;
    bool observable;
    std::vector<Parameter> parameters;
    std::vector<Parameter> responses;
    std::vector<Parameter> intermediateResponses;
    std::vector<std::string> definedExecutionErrors;
};

/// Mirrors a Feature Definition's `<Property>` element: one @ref gl_property "Property" ,
/// @ref gl_observable_property "Observable" or unobservable per
/// the observable flag.
struct Property {
    std::string identifier;
    bool observable;
    DataType dataType;
};

// FDL <Metadata>: a client-supplied value attached to calls, carried on the
// wire as a Metadata_<Identifier> message (fdl2proto-messages.xsl:136-166).
// Metadata is never observable, so there is no `observable` flag here.
/// Mirrors a Feature Definition's `<Metadata>` element: a
/// @ref gl_sila_client_metadata "SiLA Client Metadata" item a client can attach to
/// its calls.
struct Metadata {
    std::string identifier;
    DataType dataType;
};

/// Mirrors a Feature Definition's `<DataTypeDefinition>` element: a named
/// data type other Commands, Properties, or types in the same Feature can
/// reference by identifier via DataType::Identifier.
struct DataTypeDefinition {
    std::string identifier;
    DataType dataType;
};

/// The parsed form of one @ref gl_feature_definition "Feature Definition"
/// XML document, produced by parseFdl() and consumed by FeatureCatalog::add()
/// and DescriptorBuilder.
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
