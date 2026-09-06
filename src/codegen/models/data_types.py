from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from models.constraints import Constraints

__NAMESPACE__ = "http://www.sila-standard.org"


class BasicType(Enum):
    STRING = "String"
    INTEGER = "Integer"
    REAL = "Real"
    BOOLEAN = "Boolean"
    BINARY = "Binary"
    DATE = "Date"
    TIME = "Time"
    TIMESTAMP = "Timestamp"
    ANY = "Any"


@dataclass(kw_only=True)
class DataTypeType:
    basic: None | BasicType = field(
        default=None,
        metadata={
            "name": "Basic",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    list_value: None | ListType = field(
        default=None,
        metadata={
            "name": "List",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    structure: None | StructureType = field(
        default=None,
        metadata={
            "name": "Structure",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    constrained: None | ConstrainedType = field(
        default=None,
        metadata={
            "name": "Constrained",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        },
    )
    data_type_identifier: None | str = field(
        default=None,
        metadata={
            "name": "DataTypeIdentifier",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
            "max_length": 255,
            "pattern": r"[A-Z][a-zA-Z0-9]*",
        },
    )


@dataclass(kw_only=True)
class ConstrainedType:
    data_type: DataTypeType = field(
        metadata={
            "name": "DataType",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        }
    )
    constraints: Constraints = field(
        metadata={
            "name": "Constraints",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        }
    )


@dataclass(kw_only=True)
class ListType:
    data_type: DataTypeType = field(
        metadata={
            "name": "DataType",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        }
    )


@dataclass(kw_only=True)
class SiLaelement:
    class Meta:
        name = "SiLAElement"

    identifier: str = field(
        metadata={
            "name": "Identifier",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
            "max_length": 255,
            "pattern": r"[A-Z][a-zA-Z0-9]*",
        }
    )
    display_name: str = field(
        metadata={
            "name": "DisplayName",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
            "max_length": 255,
        }
    )
    description: str = field(
        metadata={
            "name": "Description",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        }
    )
    data_type: DataTypeType = field(
        metadata={
            "name": "DataType",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
        }
    )


@dataclass(kw_only=True)
class StructureType:
    element: list[SiLaelement] = field(
        default_factory=list,
        metadata={
            "name": "Element",
            "type": "Element",
            "namespace": "http://www.sila-standard.org",
            "min_occurs": 1,
        },
    )
