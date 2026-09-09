from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from models.data_types import (
    DataTypeType,
    SilaElement,
)

__NAMESPACE__ = "http://www.sila-standard.org"


class CommandObservable(Enum):
    YES = "Yes"
    NO = "No"


@dataclass(kw_only=True)
class DefinedExecutionErrorList:
    identifier: list[str] = field(
        default_factory=list,
        metadata={
            "name": "Identifier",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
            "min_occurs": 1,
            "max_length": 255,
            "pattern": r"[A-Z][a-zA-Z0-9]*",
        },
    )


class FeatureMaturityLevel(Enum):
    DRAFT = "Draft"
    VERIFIED = "Verified"
    NORMATIVE = "Normative"


class PropertyObservable(Enum):
    YES = "Yes"
    NO = "No"


@dataclass(kw_only=True)
class Feature:
    class Meta:
        namespace = "http://www.sila-standard.org"

    identifier: str = field(
        metadata={
            "name": "Identifier",
            "type": "Element",
            "max_length": 255,
            "pattern": r"[A-Z][a-zA-Z0-9]*",
        }
    )
    display_name: str = field(
        metadata={
            "name": "DisplayName",
            "type": "Element",
            "max_length": 255,
        }
    )
    description: str = field(
        metadata={
            "name": "Description",
            "type": "Element",
        }
    )
    command: list[Feature.Command] = field(
        default_factory=list,
        metadata={
            "name": "Command",
            "type": "Element",
        },
    )
    property: list[Feature.Property] = field(
        default_factory=list,
        metadata={
            "name": "Property",
            "type": "Element",
        },
    )
    metadata: list[Feature.Metadata] = field(
        default_factory=list,
        metadata={
            "name": "Metadata",
            "type": "Element",
        },
    )
    defined_execution_error: list[Feature.DefinedExecutionError] = field(
        default_factory=list,
        metadata={
            "name": "DefinedExecutionError",
            "type": "Element",
        },
    )
    data_type_definition: list[SilaElement] = field(
        default_factory=list,
        metadata={
            "name": "DataTypeDefinition",
            "type": "Element",
        },
    )
    locale: str = field(
        default="en-us",
        metadata={
            "name": "Locale",
            "type": "Attribute",
        },
    )
    si_la2_version: str = field(
        metadata={
            "name": "SiLA2Version",
            "type": "Attribute",
            "pattern": r"\d+\.\d+",
        }
    )
    feature_version: str = field(
        metadata={
            "name": "FeatureVersion",
            "type": "Attribute",
            "pattern": r"\d+\.\d+",
        }
    )
    maturity_level: FeatureMaturityLevel = field(
        default=FeatureMaturityLevel.DRAFT,
        metadata={
            "name": "MaturityLevel",
            "type": "Attribute",
        },
    )
    originator: str = field(
        metadata={
            "name": "Originator",
            "type": "Attribute",
            "max_length": 255,
            "pattern": r"[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)*",
        }
    )
    category: str = field(
        default="none",
        metadata={
            "name": "Category",
            "type": "Attribute",
            "max_length": 255,
            "pattern": r"[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)*",
        },
    )

    @dataclass(kw_only=True)
    class Command:
        identifier: str = field(
            metadata={
                "name": "Identifier",
                "type": "Element",
                "max_length": 255,
                "pattern": r"[A-Z][a-zA-Z0-9]*",
            }
        )
        display_name: str = field(
            metadata={
                "name": "DisplayName",
                "type": "Element",
                "max_length": 255,
            }
        )
        description: str = field(
            metadata={
                "name": "Description",
                "type": "Element",
            }
        )
        observable: CommandObservable = field(
            metadata={
                "name": "Observable",
                "type": "Element",
            }
        )
        parameter: list[SilaElement] = field(
            default_factory=list,
            metadata={
                "name": "Parameter",
                "type": "Element",
            },
        )
        response: list[SilaElement] = field(
            default_factory=list,
            metadata={
                "name": "Response",
                "type": "Element",
            },
        )
        intermediate_response: list[SilaElement] = field(
            default_factory=list,
            metadata={
                "name": "IntermediateResponse",
                "type": "Element",
            },
        )
        defined_execution_errors: None | DefinedExecutionErrorList = field(
            default=None,
            metadata={
                "name": "DefinedExecutionErrors",
                "type": "Element",
            },
        )

    @dataclass(kw_only=True)
    class Property:
        identifier: str = field(
            metadata={
                "name": "Identifier",
                "type": "Element",
                "max_length": 255,
                "pattern": r"[A-Z][a-zA-Z0-9]*",
            }
        )
        display_name: str = field(
            metadata={
                "name": "DisplayName",
                "type": "Element",
                "max_length": 255,
            }
        )
        description: str = field(
            metadata={
                "name": "Description",
                "type": "Element",
            }
        )
        observable: PropertyObservable = field(
            metadata={
                "name": "Observable",
                "type": "Element",
            }
        )
        data_type: DataTypeType = field(
            metadata={
                "name": "DataType",
                "type": "Element",
            }
        )
        defined_execution_errors: None | DefinedExecutionErrorList = field(
            default=None,
            metadata={
                "name": "DefinedExecutionErrors",
                "type": "Element",
            },
        )

    @dataclass(kw_only=True)
    class Metadata:
        identifier: str = field(
            metadata={
                "name": "Identifier",
                "type": "Element",
                "max_length": 255,
                "pattern": r"[A-Z][a-zA-Z0-9]*",
            }
        )
        display_name: str = field(
            metadata={
                "name": "DisplayName",
                "type": "Element",
                "max_length": 255,
            }
        )
        description: str = field(
            metadata={
                "name": "Description",
                "type": "Element",
            }
        )
        data_type: DataTypeType = field(
            metadata={
                "name": "DataType",
                "type": "Element",
            }
        )
        defined_execution_errors: None | DefinedExecutionErrorList = field(
            default=None,
            metadata={
                "name": "DefinedExecutionErrors",
                "type": "Element",
            },
        )

    @dataclass(kw_only=True)
    class DefinedExecutionError:
        identifier: str = field(
            metadata={
                "name": "Identifier",
                "type": "Element",
                "max_length": 255,
                "pattern": r"[A-Z][a-zA-Z0-9]*",
            }
        )
        display_name: str = field(
            metadata={
                "name": "DisplayName",
                "type": "Element",
                "max_length": 255,
            }
        )
        description: str = field(
            metadata={
                "name": "Description",
                "type": "Element",
            }
        )
