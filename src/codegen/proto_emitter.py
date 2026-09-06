from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import jinja2

from fdl_parser import package_name, property_rpc_name
from models.data_types import DataTypeType, SiLaelement
from models.feature_definition import CommandObservable, Feature

_TEMPLATE_DIR = Path(__file__).parent / "templates"
_ENV = jinja2.Environment(
    loader=jinja2.FileSystemLoader(_TEMPLATE_DIR),
    undefined=jinja2.StrictUndefined,
    trim_blocks=True,
    lstrip_blocks=True,
    # Preserve the template's own trailing newline so the rendered .proto
    # file ends with one, as required by the spec.
    keep_trailing_newline=True,
)


@dataclass
class ProtoField:
    repeated: bool
    type: str
    name: str
    number: int
    description: str


@dataclass
class ProtoMessage:
    name: str
    # None for nested "_Struct" messages, which have no leading comment.
    comment: str | None
    nested: list[ProtoMessage] = field(default_factory=list)
    fields: list[ProtoField] = field(default_factory=list)


@dataclass
class RpcLine:
    comment: str
    # Fully formatted "Name(...) returns (...)" text, without the leading
    # "rpc " keyword or trailing " {}" -- those are added by the template.
    signature: str


def emit_proto(feature: Feature) -> str:
    """Generate a complete .proto file from a parsed Feature model."""
    pkg = package_name(feature)
    context = {
        "import_framework": _needs_framework_import(feature),
        "package": pkg,
        "feature_identifier": feature.identifier,
        "feature_description": feature.description,
        "service_rpcs": _build_service_rpcs(feature, pkg),
        "messages": _build_messages(feature, pkg),
    }
    template = _ENV.get_template("feature.proto.j2")
    return template.render(context)


def _needs_framework_import(feature: Feature) -> bool:
    if feature.metadata:
        return True
    if any(cmd.observable == CommandObservable.YES for cmd in feature.command):
        return True

    roots: list[DataTypeType] = []
    for cmd in feature.command:
        roots += [e.data_type for e in cmd.parameter]
        roots += [e.data_type for e in cmd.response]
        roots += [e.data_type for e in cmd.intermediate_response]
    roots += [p.data_type for p in feature.property]
    roots += [d.data_type for d in feature.data_type_definition]
    return any(_uses_basic_type(dt) for dt in roots)


def _uses_basic_type(dt: DataTypeType) -> bool:
    if dt.basic is not None:
        return True
    if dt.list_value is not None:
        return _uses_basic_type(dt.list_value.data_type)
    if dt.structure is not None:
        return any(_uses_basic_type(e.data_type) for e in dt.structure.element)
    if dt.constrained is not None:
        return _uses_basic_type(dt.constrained.data_type)
    # A bare DataTypeIdentifier is not itself Basic; the DataTypeDefinition
    # it references is scanned separately as one of the feature's own roots.
    return False


def _build_service_rpcs(feature: Feature, pkg: str) -> list[RpcLine]:
    rpcs: list[RpcLine] = []
    for cmd in feature.command:
        rpcs += _command_rpcs(cmd, pkg)
    for prop in feature.property:
        rpcs.append(_property_rpc(prop, pkg))
    for meta in feature.metadata:
        rpcs.append(_metadata_rpc(meta, pkg))
    return rpcs


def _command_rpcs(cmd: Feature.Command, pkg: str) -> list[RpcLine]:
    name = cmd.identifier
    if cmd.observable == CommandObservable.NO:
        return [
            RpcLine(
                cmd.description,
                f"{name}({pkg}.{name}_Parameters) returns ({pkg}.{name}_Responses)",
            )
        ]

    rpcs = [
        RpcLine(
            cmd.description,
            f"{name}({pkg}.{name}_Parameters) returns (sila2.org.silastandard.CommandConfirmation)",
        ),
        RpcLine(
            f"Monitor the state of {name}",
            f"{name}_Info(sila2.org.silastandard.CommandExecutionUUID) returns "
            f"(stream sila2.org.silastandard.ExecutionInfo)",
        ),
    ]
    if cmd.intermediate_response:
        rpcs.append(
            RpcLine(
                f"Retrieve intermediate responses of {name}",
                f"{name}_Intermediate(sila2.org.silastandard.CommandExecutionUUID) returns "
                f"(stream {pkg}.{name}_IntermediateResponses)",
            )
        )
    rpcs.append(
        RpcLine(
            f"Retrieve result of {name}",
            f"{name}_Result(sila2.org.silastandard.CommandExecutionUUID) returns "
            f"({pkg}.{name}_Responses)",
        )
    )
    return rpcs


def _property_rpc(prop: Feature.Property, pkg: str) -> RpcLine:
    rpc_name = property_rpc_name(prop)
    stream = "stream " if rpc_name.startswith("Subscribe_") else ""
    return RpcLine(
        prop.description,
        f"{rpc_name}({pkg}.{rpc_name}_Parameters) returns "
        f"({stream}{pkg}.{rpc_name}_Responses)",
    )


def _metadata_rpc(meta: Feature.Metadata, pkg: str) -> RpcLine:
    name = f"Get_FCPAffectedByMetadata_{meta.identifier}"
    return RpcLine(
        f"Get fully qualified identifiers of all features, commands and properties "
        f"affected by {meta.identifier}",
        f"{name}({pkg}.{name}_Parameters) returns ({pkg}.{name}_Responses)",
    )


def _build_messages(feature: Feature, pkg: str) -> list[ProtoMessage]:
    messages: list[ProtoMessage] = []

    for dtd in feature.data_type_definition:
        messages.append(_build_message(f"DataType_{dtd.identifier}", dtd.description, [dtd], pkg))

    for cmd in feature.command:
        messages += _command_messages(cmd, pkg)

    for prop in feature.property:
        messages += _property_messages(prop, pkg)

    for meta in feature.metadata:
        messages += _metadata_messages(meta, pkg)

    return messages


def _command_messages(cmd: Feature.Command, pkg: str) -> list[ProtoMessage]:
    name = cmd.identifier
    messages = [
        _build_message(f"{name}_Parameters", f"Parameters for {name}", cmd.parameter, pkg),
        _build_message(f"{name}_Responses", f"Responses of {name}", cmd.response, pkg),
    ]
    if cmd.observable == CommandObservable.YES and cmd.intermediate_response:
        messages.append(
            _build_message(
                f"{name}_IntermediateResponses",
                f"Intermediate responses of {name}",
                cmd.intermediate_response,
                pkg,
            )
        )
    return messages


def _property_messages(prop: Feature.Property, pkg: str) -> list[ProtoMessage]:
    rpc_name = property_rpc_name(prop)
    # The property's single response field is the property itself; model it
    # as a SiLAElement so it can go through the same field-resolution path
    # (including struct/list nesting) as any other element.
    response_element = SiLaelement(
        identifier=prop.identifier,
        display_name=prop.display_name,
        description=prop.description,
        data_type=prop.data_type,
    )
    return [
        _build_message(f"{rpc_name}_Parameters", f"Parameters for {prop.identifier}", [], pkg),
        _build_message(
            f"{rpc_name}_Responses",
            f"Responses of {prop.identifier}",
            [response_element],
            pkg,
        ),
    ]


def _metadata_messages(meta: Feature.Metadata, pkg: str) -> list[ProtoMessage]:
    base = f"Get_FCPAffectedByMetadata_{meta.identifier}"
    affected_calls = ProtoMessage(
        name=f"{base}_Responses",
        comment=f"Responses of {base}",
        fields=[
            ProtoField(
                repeated=True,
                type="sila2.org.silastandard.String",
                name="AffectedCalls",
                number=1,
                description=(
                    "Fully qualified identifiers of all features, commands and "
                    f"properties affected by {meta.identifier}"
                ),
            )
        ],
    )
    metadata_element = SiLaelement(
        identifier=meta.identifier,
        display_name=meta.display_name,
        description=meta.description,
        data_type=meta.data_type,
    )
    return [
        ProtoMessage(name=f"{base}_Parameters", comment=f"Parameters for {base}"),
        affected_calls,
        _build_message(f"Metadata_{meta.identifier}", meta.description, [metadata_element], pkg),
    ]


def _build_message(
    name: str,
    comment: str | None,
    elements: list[SiLaelement],
    pkg: str,
    msg_path: str | None = None,
) -> ProtoMessage:
    msg = ProtoMessage(name=name, comment=comment)
    if msg_path is None:
        msg_path = name
    for number, element in enumerate(elements, start=1):
        repeated, type_str = _resolve_type(element.identifier, element.data_type, pkg, msg_path, msg)
        msg.fields.append(
            ProtoField(
                repeated=repeated,
                type=type_str,
                name=element.identifier,
                number=number,
                description=element.description,
            )
        )
    return msg


def _resolve_type(
    field_id: str, dt: DataTypeType, pkg: str, msg_path: str, parent: ProtoMessage
) -> tuple[bool, str]:
    """Resolve a DataTypeType to a (repeated, proto_type) pair.

    `msg_path` is the dotted path of the enclosing message relative to the
    package (e.g. "TestCommand_Responses.Response1_Struct"), used both to
    qualify nested struct types and as the base for further nesting.
    Structure fields attach a "_Struct" message directly onto `parent`.
    """
    if dt.basic is not None:
        return False, f"sila2.org.silastandard.{dt.basic.value}"
    if dt.list_value is not None:
        _, inner_type = _resolve_type(field_id, dt.list_value.data_type, pkg, msg_path, parent)
        return True, inner_type
    if dt.structure is not None:
        struct_name = f"{field_id}_Struct"
        nested = _build_message(struct_name, None, dt.structure.element, pkg, f"{msg_path}.{struct_name}")
        parent.nested.append(nested)
        return False, f"{pkg}.{msg_path}.{struct_name}"
    if dt.constrained is not None:
        return _resolve_type(field_id, dt.constrained.data_type, pkg, msg_path, parent)
    if dt.data_type_identifier is not None:
        return False, f"{pkg}.DataType_{dt.data_type_identifier}"
    raise ValueError(f"DataType for '{field_id}' has no variant set")
