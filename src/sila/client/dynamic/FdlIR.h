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
    std::string identifier;             ///< The `<Element>`'s `<Identifier>`: this member's name within the Structure.
    std::unique_ptr<DataType> dataType; ///< The `<Element>`'s `<DataType>`: this member's type.
};

// One parsed <Constraints> child element (SiLA2 FDL schema).
/// One parsed `<Constraints>` child element -- mirrors a Feature Definition's
/// @ref gl_constraint "Constraint" on a `Constrained` data type.
struct ConstraintValue {
    /// Which @ref gl_constraint "Constraint" this value is; selects which of
    /// the fields below is populated.
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
        std::string siUnit;    ///< The `<SIUnit>`: the SI base unit this component raises to a power (e.g. "Meter").
        std::string exponent;  ///< The `<Exponent>` the SI unit is raised to; XSD lexical value (xs:integer).
    };

    /// Mirrors a `Unit` Constraint's `<Unit>` element: a labeled unit derived
    /// from SI components via a linear factor/offset.
    struct UnitValue {
        std::string label;   ///< The `<Label>`: the derived unit's display symbol (e.g. "rpm").
        std::string factor;  ///< The `<Factor>` a raw SI value is multiplied by to reach this unit; XSD lexical value (xs:double).
        std::string offset;  ///< The `<Offset>` added after the factor; XSD lexical value (xs:double).
        std::vector<UnitComponent> components;  ///< The `<UnitComponent>`s the derived unit is composed from.
    };

    /// Mirrors one `<Parameter>` of a ContentType Constraint's MIME type.
    struct ContentTypeParameter {
        std::string attribute;  ///< The `<Attribute>`: the MIME parameter's name (e.g. "charset").
        std::string value;      ///< The `<Value>` of the MIME parameter.
    };

    /// Mirrors a `ContentType` Constraint: a MIME type/subtype plus
    /// parameters that a Binary value
    /// must match.
    struct ContentTypeValue {
        std::string type;                             ///< The MIME `<Type>` (e.g. "text").
        std::string subtype;                          ///< The MIME `<Subtype>` (e.g. "csv").
        std::vector<ContentTypeParameter> parameters;  ///< The MIME type's `<Parameter>`s.
    };

    /// Mirrors a `Schema` Constraint: an XML or JSON schema, given either by
    /// URL or inline, that a String value must validate against.
    struct SchemaValue {
        /// The `<Type>`: whether the referenced schema validates XML or JSON.
        enum class Type { Xml, Json };
        /// Whether value came from the Schema Constraint's `<Url>` or
        /// `<Inline>` child element.
        enum class Source { Url, Inline };

        Type type = Type::Xml;        ///< Which schema language value is written in.
        Source source = Source::Url;  ///< Whether value is a URL to fetch or inline schema text.
        std::string value;            ///< The `<Url>` or `<Inline>` element's content, per source.
    };

    Kind kind;                                  ///< Which Constraint this value is.
    std::string stringValue;                    ///< The Pattern Constraint's regex, or the FullyQualifiedIdentifier Constraint's format string.
    std::vector<std::string> stringValues;      ///< The Set Constraint's allowed values.
    std::string lexicalValue;                   ///< The original XML text of a scalar bound (MinInclusive, MaxInclusive, Length, …).
    double numericValue = 0;                    ///< Legacy double view of the scalar bound, for callers predating lexicalValue.
    FqiKind fqiKind = FqiKind::Unknown;         ///< For a FullyQualifiedIdentifier Constraint, which FQI kind values must match.
    std::optional<UnitValue> unit;               ///< Set when kind is Unit.
    std::optional<ContentTypeValue> contentType;  ///< Set when kind is ContentType.
    std::optional<SchemaValue> schema;            ///< Set when kind is Schema.
    std::vector<std::shared_ptr<DataType>> allowedTypes;  ///< For an AllowedTypes Constraint, the Data Types a value may take.

    // Pattern only: the compiled form of stringValue, prepared once by the FDL
    // runtime parser (sila2::types::compilePattern) so repeated-field validation
    // shares one compiled std::regex across every element instead of recompiling
    // it per element. shared_ptr keeps ConstraintValue copies cheap (refcount
    // bump, not a regex recompile). Null when unprepared (hand-built
    // ConstraintValue, e.g. in tests) or when the pattern could not be
    // compiled -- callers fall back to the uncompiled types::checkPattern then.
    std::shared_ptr<const std::regex> preparedPattern;  ///< The compiled form of a Pattern Constraint's stringValue, or null (see above).
};

/// Mirrors a Feature Definition's `<DataType>` element: one of the five
/// SiLA Data Type shapes (Basic, List, Structure,
/// Constrained, or a `<DataTypeIdentifier>` reference).
struct DataType {
    /// Mirrors `<DataType><Basic>`.
    struct Basic { BasicType type; /**< Which SiLA scalar kind this is. */ };
    /// Mirrors `<DataType><List>`: a repeated value of elementType.
    struct List { std::unique_ptr<DataType> elementType; /**< The type of each repeated value. */ };
    /// Mirrors `<DataType><Structure>`.
    struct Structure { std::vector<StructureElement> elements; /**< The Structure's members, in declaration order. */ };
    /// Mirrors `<DataType><Constrained>`: inner restricted by constraints.
    struct Constrained {
        std::unique_ptr<DataType> inner;           ///< The type being constrained.
        std::vector<ConstraintValue> constraints;  ///< The `<Constraints>` a value of inner must satisfy.
    };
    /// Mirrors `<DataType><DataTypeIdentifier>`: a reference to a
    /// DataTypeDefinition's identifier, resolved by the caller.
    struct Identifier { std::string typeId; /**< The referenced DataTypeDefinition's identifier. */ };
    std::variant<Basic, List, Structure, Constrained, Identifier> value;  ///< Which of the five Data Type shapes this is, and its content.
};

/// Mirrors one `<Parameter>` or `<Response>` element of a Command, or a
/// Property's response element.
struct Parameter {
    std::string identifier;  ///< The `<Identifier>`: this Parameter or Response's name within its Command.
    DataType dataType;       ///< The `<DataType>` a value for this Parameter or Response must have.
};

/// Mirrors a Feature Definition's `<Command>` element: one @ref gl_command "Command" ,
/// @ref gl_observable_command "Observable" or @ref gl_unobservable_command "Unobservable" per the
/// observable flag.
struct Command {
    std::string identifier;                              ///< The `<Identifier>`: this Command's name within its Feature.
    bool observable;                                      ///< True for an Observable Command, false for Unobservable.
    std::vector<Parameter> parameters;                    ///< The Command's `<Parameter>`s, in call order.
    std::vector<Parameter> responses;                     ///< The Command's `<Response>`s: the fields of its result.
    std::vector<Parameter> intermediateResponses;         ///< The @ref gl_intermediate_command_response "Intermediate Command Response" fields; empty if the Command reports none.
    std::vector<std::string> definedExecutionErrors;      ///< Identifiers of the @ref gl_defined_execution_error "Defined Execution Errors" this Command may raise.
};

/// Mirrors a Feature Definition's `<Property>` element: one @ref gl_property "Property" ,
/// @ref gl_observable_property "Observable" or unobservable per
/// the observable flag.
struct Property {
    std::string identifier;  ///< The `<Identifier>`: this Property's name within its Feature.
    bool observable;         ///< True for an Observable Property, false for unobservable.
    DataType dataType;       ///< The `<DataType>` this Property's value has.
};

// FDL <Metadata>: a client-supplied value attached to calls, carried on the
// wire as a Metadata_<Identifier> message (fdl2proto-messages.xsl:136-166).
// Metadata is never observable, so there is no `observable` flag here.
/// Mirrors a Feature Definition's `<Metadata>` element: a
/// @ref gl_sila_client_metadata "SiLA Client Metadata" item a client can attach to
/// its calls.
struct Metadata {
    std::string identifier;  ///< The `<Identifier>`: this Metadata's name within its Feature.
    DataType dataType;       ///< The `<DataType>` a value for this Metadata must have.
};

/// Mirrors a Feature Definition's `<DataTypeDefinition>` element: a named
/// data type other Commands, Properties, or types in the same Feature can
/// reference by identifier via DataType::Identifier.
struct DataTypeDefinition {
    std::string identifier;  ///< The `<Identifier>`: the name other types reference via DataType::Identifier.
    DataType dataType;       ///< The `<DataType>` this identifier stands for.
};

/// The parsed form of one @ref gl_feature_definition "Feature Definition"
/// XML document, produced by parseFdl() and consumed by FeatureCatalog::add()
/// and DescriptorBuilder.
struct Feature {
    std::string identifier;        ///< The `<Identifier>`: the Feature's name (the last segment of its FQI).
    std::string featureVersion;    ///< The `<Feature>` element's `FeatureVersion` attribute (e.g. "1.0").
    std::string originator;        ///< The `<Feature>` element's `Originator` attribute: who defines it (e.g. "org.silastandard").
    std::string category;          ///< The `<Feature>` element's `Category` attribute, or empty if the Feature declares none.
    std::vector<Command> commands;              ///< The Feature's `<Command>`s.
    std::vector<Property> properties;           ///< The Feature's `<Property>`s.
    std::vector<Metadata> metadata;             ///< The Feature's `<Metadata>` items.
    std::vector<DataTypeDefinition> dataTypeDefinitions;  ///< The Feature's `<DataTypeDefinition>`s, referenceable by DataType::Identifier.
};

}  // namespace dynamic
}  // namespace sila2
