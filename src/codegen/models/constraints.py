from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

__NAMESPACE__ = "http://www.sila-standard.org"


class ConstraintsFullyQualifiedIdentifier(Enum):
    """
    Applicable to SiLA basic type: String.
    """

    FEATURE_IDENTIFIER = "FeatureIdentifier"
    COMMAND_IDENTIFIER = "CommandIdentifier"
    COMMAND_PARAMETER_IDENTIFIER = "CommandParameterIdentifier"
    COMMAND_RESPONSE_IDENTIFIER = "CommandResponseIdentifier"
    INTERMEDIATE_COMMAND_RESPONSE_IDENTIFIER = (
        "IntermediateCommandResponseIdentifier"
    )
    DEFINED_EXECUTION_ERROR_IDENTIFIER = "DefinedExecutionErrorIdentifier"
    PROPERTY_IDENTIFIER = "PropertyIdentifier"
    TYPE_IDENTIFIER = "TypeIdentifier"
    METADATA_IDENTIFIER = "MetadataIdentifier"


class SchemaType(Enum):
    XML = "Xml"
    JSON = "Json"


class UnitComponentSiunit(Enum):
    DIMENSIONLESS = "Dimensionless"
    METER = "Meter"
    KILOGRAM = "Kilogram"
    SECOND = "Second"
    AMPERE = "Ampere"
    KELVIN = "Kelvin"
    MOLE = "Mole"
    CANDELA = "Candela"


@dataclass(kw_only=True)
class Constraints:
    """
    :ivar length: Applicable to SiLA basic type: String, Binary
    :ivar minimal_length: Applicable to SiLA basic type: String, Binary
    :ivar maximal_length: Applicable to SiLA basic type: String, Binary
    :ivar set:
    :ivar pattern: Applicable to SiLA basic type: String. The pattern
        needs to be an XML schema regular expression
    :ivar maximal_exclusive: Applicable to SiLA basic type:
        Integer[xs:double], Real[xs:double], Date[xs:date],
        Time[xs:time] and Timestamp[xs:dateTime]
    :ivar maximal_inclusive: Applicable to SiLA basic type:
        Integer[xs:double], Real[xs:double], Date[xs:date],
        Time[xs:time] and Timestamp[xs:dateTime]
    :ivar minimal_exclusive: Applicable to SiLA basic type:
        Integer[xs:double], Real[xs:double], Date[xs:date],
        Time[xs:time] and Timestamp[xs:dateTime]
    :ivar minimal_inclusive: Applicable to SiLA basic type:
        Integer[xs:double], Real[xs:double], Date[xs:date],
        Time[xs:time] and Timestamp[xs:dateTime]
    :ivar unit: Applicable to SiLA basic type: Integer, Real
    :ivar content_type: Applicable to SiLA basic type: String, Binary
    :ivar element_count: Applicable to SiLA derived type: List
    :ivar minimal_element_count: Applicable to SiLA derived type: List
    :ivar maximal_element_count: Applicable to SiLA derived type: List
    :ivar fully_qualified_identifier:
    :ivar schema: Applicable to SiLA basic type: String, Binary
    :ivar allowed_types: Applicable to SiLA basic type Any
    """

    length: None | int = field(
        default=None,
        metadata={
            "name": "Length",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    minimal_length: None | int = field(
        default=None,
        metadata={
            "name": "MinimalLength",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    maximal_length: None | int = field(
        default=None,
        metadata={
            "name": "MaximalLength",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    set: None | Constraints.Set = field(
        default=None,
        metadata={
            "name": "Set",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    pattern: None | str = field(
        default=None,
        metadata={
            "name": "Pattern",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    maximal_exclusive: None | str = field(
        default=None,
        metadata={
            "name": "MaximalExclusive",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    maximal_inclusive: None | str = field(
        default=None,
        metadata={
            "name": "MaximalInclusive",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    minimal_exclusive: None | str = field(
        default=None,
        metadata={
            "name": "MinimalExclusive",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    minimal_inclusive: None | str = field(
        default=None,
        metadata={
            "name": "MinimalInclusive",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    unit: None | Constraints.Unit = field(
        default=None,
        metadata={
            "name": "Unit",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    content_type: None | Constraints.ContentType = field(
        default=None,
        metadata={
            "name": "ContentType",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    element_count: None | int = field(
        default=None,
        metadata={
            "name": "ElementCount",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    minimal_element_count: None | int = field(
        default=None,
        metadata={
            "name": "MinimalElementCount",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    maximal_element_count: None | int = field(
        default=None,
        metadata={
            "name": "MaximalElementCount",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    fully_qualified_identifier: None | ConstraintsFullyQualifiedIdentifier = (
        field(
            default=None,
            metadata={
                "name": "FullyQualifiedIdentifier",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
            },
        )
    )
    schema: None | Constraints.Schema = field(
        default=None,
        metadata={
            "name": "Schema",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    allowed_types: None | Constraints.AllowedTypes = field(
        default=None,
        metadata={
            "name": "AllowedTypes",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )

    @dataclass(kw_only=True)
    class Set:
        """
        :ivar value: Applicable to SiLA basic type: String,
            Integer[xs:integer], Real[xs:double], Date[xs:date],
            Time[xs:time] and Timestamp[xs:dateTime]
        """

        value: list[str] = field(
            default_factory=list,
            metadata={
                "name": "Value",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
                "min_occurs": 1,
            },
        )

    @dataclass(kw_only=True)
    class Unit:
        label: str = field(
            metadata={
                "name": "Label",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
            }
        )
        # Part B p84-85: IEEE 754 double; follows the xs:double overlay Constraints.xsd.
        factor: float = field(
            metadata={
                "name": "Factor",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
            }
        )
        offset: float = field(
            metadata={
                "name": "Offset",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
            }
        )
        unit_component: list[Constraints.Unit.UnitComponent] = field(
            default_factory=list,
            metadata={
                "name": "UnitComponent",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
                "min_occurs": 1,
            },
        )

        @dataclass(kw_only=True)
        class UnitComponent:
            siunit: UnitComponentSiunit = field(
                metadata={
                    "name": "SIUnit",
                    "type": "Element",
                    "namespace": "http://www.sila-standard.org",
                }
            )
            exponent: int = field(
                metadata={
                    "name": "Exponent",
                    "type": "Element",
                    "namespace": "http://www.sila-standard.org",
                }
            )

    @dataclass(kw_only=True)
    class ContentType:
        type_value: str = field(
            metadata={
                "name": "Type",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
            }
        )
        subtype: str = field(
            metadata={
                "name": "Subtype",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
            }
        )
        parameters: None | Constraints.ContentType.Parameters = field(
            default=None,
            metadata={
                "name": "Parameters",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
            },
        )

        @dataclass(kw_only=True)
        class Parameters:
            parameter: list[Constraints.ContentType.Parameters.Parameter] = (
                field(
                    default_factory=list,
                    metadata={
                        "name": "Parameter",
                        "type": "Element",
                        "namespace": "http://www.sila-standard.org",
                        "min_occurs": 1,
                    },
                )
            )

            @dataclass(kw_only=True)
            class Parameter:
                attribute: str = field(
                    metadata={
                        "name": "Attribute",
                        "type": "Element",
                        "namespace": "http://www.sila-standard.org",
                    }
                )
                value: str = field(
                    metadata={
                        "name": "Value",
                        "type": "Element",
                        "namespace": "http://www.sila-standard.org",
                    }
                )

    @dataclass(kw_only=True)
    class Schema:
        type_value: SchemaType = field(
            metadata={
                "name": "Type",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
            }
        )
        url: None | str = field(
            default=None,
            metadata={
                "name": "Url",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
            },
        )
        inline: None | str = field(
            default=None,
            metadata={
                "name": "Inline",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
            },
        )

    @dataclass(kw_only=True)
    class AllowedTypes:
        data_type: list[DataTypeType] = field(
            default_factory=list,
            metadata={
                "name": "DataType",
                "type": "Element",
                "namespace": "http://www.sila-standard.org",
                "min_occurs": 1,
            },
        )


# Deferred to break constraints <-> data_types circular import.
# xsdata resolves this annotation at runtime via get_type_hints(),
# so the name must exist in module globals, not just under TYPE_CHECKING.
from models.data_types import DataTypeType  # noqa: E402
