from __future__ import annotations

from pathlib import Path
from typing import Any

import jinja2

from fdl_parser import fqi, package_name, property_rpc_name
from models.feature_definition import CommandObservable, Feature
from schema_provisioning import collect_schema_urls

_TEMPLATE_DIR = Path(__file__).parent / "templates"
_ENV = jinja2.Environment(
    loader=jinja2.FileSystemLoader(_TEMPLATE_DIR),
    undefined=jinja2.StrictUndefined,
    trim_blocks=True,
    lstrip_blocks=True,
    keep_trailing_newline=True,
)


def _raw_delimiter(text: str) -> str:
    """Pick a raw-string delimiter that `text` cannot prematurely close.

    Fetched schema text (R10-9g1) is arbitrary, so one alternate is not
    enough: walk `xml`, `sila`, `sila1`, ... until the closing sequence
    `)<delim>"` is absent. A raw-string delimiter may be at most 16 chars.
    """
    candidates = ["xml", "sila"] + [f"sila{i}" for i in range(1, 100)]
    for delimiter in candidates:
        if f"){delimiter}\"" not in text:
            return delimiter
    raise RuntimeError("no raw-string delimiter free in embedded text")


def _build_context(
    feature: Feature, xml_text: str, provisioned_schemas: list[dict[str, Any]] | None
) -> dict[str, Any]:
    fqi_str = fqi(feature)
    return {
        "feature_identifier": feature.identifier,
        "namespace_lower": feature.identifier.lower(),
        "fqi": fqi_str,
        "feature_version": feature.feature_version,
        "errors": feature.defined_execution_error,
        "xml_text": xml_text,
        "raw_delimiter": _raw_delimiter(xml_text),
        # R10-9g1: emitted only for url-bearing features (provisioned_schemas
        # is None/empty for every current golden/example), so the kFdlXml
        # block above stays byte-for-byte and existing goldens are unchanged.
        "provisioned_schemas": [
            {
                "url": schema["url"],
                "schema_xml": schema["schema_xml"],
                "raw_delimiter": _raw_delimiter(schema["schema_xml"]),
            }
            for schema in (provisioned_schemas or [])
        ],
    }


def emit_meta_header(
    feature: Feature, xml_text: str, provisioned_schemas: list[dict[str, Any]] | None = None
) -> str:
    """Generate <Feature>Meta.h content using Jinja2 template templates/meta.h.j2."""
    template = _ENV.get_template("meta.h.j2")
    return template.render(_build_context(feature, xml_text, provisioned_schemas))


def emit_meta_source(
    feature: Feature, xml_text: str, provisioned_schemas: list[dict[str, Any]] | None = None
) -> str:
    """Generate <Feature>Meta.cc content using Jinja2 template templates/meta.cc.j2."""
    template = _ENV.get_template("meta.cc.j2")
    return template.render(_build_context(feature, xml_text, provisioned_schemas))


def _build_adapter_rpcs(feature: Feature) -> list[dict[str, Any]]:
    rpcs: list[dict[str, Any]] = []

    for cmd in feature.command:
        if cmd.observable == CommandObservable.NO:
            rpcs.append({
                "name": cmd.identifier,
                "handler_name": "on" + cmd.identifier.replace("_", ""),
                "req_type": f"proto::{cmd.identifier}_Parameters",
                "resp_type": f"proto::{cmd.identifier}_Responses",
                "is_streaming": False,
                "cloud_type": "command",
                "cloud_name": cmd.identifier,
                "command_name": cmd.identifier,
                "auth_kind": "Command",
                "auth_name": cmd.identifier,
            })
        else:
            # 1.2i: both observable shapes register through regObsCmd; the
            # no-intermediate one simply has no _Intermediate handler to pass.
            # int_handler_name is set on BOTH shapes -- the template is rendered
            # under StrictUndefined, so a key present on only one shape would
            # raise at render time for the other.
            intermediate_handler = None
            if cmd.intermediate_response:
                intermediate_handler = "on" + f"{cmd.identifier}Intermediate".replace("_", "")
            rpcs.append({
                "name": cmd.identifier,
                "handler_name": "on" + cmd.identifier.replace("_", ""),
                "req_type": f"proto::{cmd.identifier}_Parameters",
                "resp_type": "sila2::org::silastandard::CommandConfirmation",
                "is_streaming": False,
                "cloud_type": "observable_command",
                "cloud_name": cmd.identifier,
                "command_name": cmd.identifier,
                "int_handler_name": intermediate_handler,
                "res_handler_name": "on" + f"{cmd.identifier}Result".replace("_", ""),
                "auth_kind": "Command",
                "auth_name": cmd.identifier,
                # Character-identical to the handler_name computed for the
                # separate "_Info" rpc entry below (the "_Info" form, not
                # f"{cmd.identifier}Info") -- regObsCmd's infoMethod slot and
                # the generated onXInfo SilaHandler member must name the same
                # method.
                "info_handler_name": "on" + f"{cmd.identifier}_Info".replace("_", ""),
            })
            info_name = f"{cmd.identifier}_Info"
            rpcs.append({
                "name": info_name,
                "handler_name": "on" + info_name.replace("_", ""),
                "req_type": "sila2::org::silastandard::CommandExecutionUUID",
                "resp_type": "sila2::org::silastandard::ExecutionInfo",
                "is_streaming": True,
                "cloud_type": None,
                "cloud_name": None,
                "command_name": None,
                # The _Info follow-up carries no cloud registration of its own,
                # but on the gRPC path it must gate on the OWNING command's FQI
                # (not the feature FQI) so it matches the same authorization
                # granularity the cloud follow-ups already use.
                "auth_kind": "Command",
                "auth_name": cmd.identifier,
            })
            if cmd.intermediate_response:
                intermediate_name = f"{cmd.identifier}_Intermediate"
                rpcs.append({
                    "name": intermediate_name,
                    "handler_name": "on" + intermediate_name.replace("_", ""),
                    "req_type": "sila2::org::silastandard::CommandExecutionUUID",
                    "resp_type": f"proto::{cmd.identifier}_IntermediateResponses",
                    "is_streaming": True,
                    "cloud_type": None,
                    "cloud_name": None,
                    "command_name": None,
                    "auth_kind": "Command",
                    "auth_name": cmd.identifier,
                })
            result_name = f"{cmd.identifier}_Result"
            rpcs.append({
                "name": result_name,
                "handler_name": "on" + result_name.replace("_", ""),
                "req_type": "sila2::org::silastandard::CommandExecutionUUID",
                "resp_type": f"proto::{cmd.identifier}_Responses",
                "is_streaming": False,
                "cloud_type": None,
                "cloud_name": None,
                "command_name": None,
                "auth_kind": "Command",
                "auth_name": cmd.identifier,
            })

    for prop in feature.property:
        rpc_name = property_rpc_name(prop)
        is_streaming = rpc_name.startswith("Subscribe_")
        rpcs.append({
            "name": rpc_name,
            "handler_name": "on" + rpc_name.replace("_", ""),
            "req_type": f"proto::{rpc_name}_Parameters",
            "resp_type": f"proto::{rpc_name}_Responses",
            "is_streaming": is_streaming,
            "cloud_type": "observable_property" if is_streaming else "property",
            "cloud_name": prop.identifier,
            "command_name": None,
            "auth_kind": "Property",
            "auth_name": prop.identifier,
        })

    for meta in feature.metadata:
        meta_name = f"Get_FCPAffectedByMetadata_{meta.identifier}"
        rpcs.append({
            "name": meta_name,
            "handler_name": "on" + meta_name.replace("_", ""),
            "req_type": f"proto::{meta_name}_Parameters",
            "resp_type": f"proto::{meta_name}_Responses",
            "is_streaming": False,
            "cloud_type": "property",
            "cloud_name": f"FCPAffectedByMetadata_{meta.identifier}",
            "command_name": None,
            # Stays feature-level (no owning Command/Property): this is the
            # pre-auth discovery query a client issues to learn which calls a
            # metadata item affects, so it must remain reachable without a
            # sub-feature grant.
            "auth_kind": None,
            "auth_name": None,
        })

    return rpcs


def emit_service_adapter(feature: Feature) -> str:
    """Generate <Feature>ServiceAdapter.h content using Jinja2 template templates/service_adapter.h.j2."""
    rpcs = _build_adapter_rpcs(feature)
    needs_framework = any(cmd.observable == CommandObservable.YES for cmd in feature.command)
    pkg = package_name(feature).replace(".", "::")
    context = {
        "feature_identifier": feature.identifier,
        "namespace_lower": feature.identifier.lower(),
        "proto_package": pkg,
        "needs_framework_include": needs_framework,
        "rpcs": rpcs,
        "fqi": fqi(feature),
        # R10-9g1: pass kProvisionedSchemas as the ctor's 3rd arg only when the
        # feature declares a Schema Url; every current golden/example feature
        # has none, so this stays False and the ctor call is unchanged.
        "has_provisioned_schemas": bool(collect_schema_urls(feature)),
    }
    template = _ENV.get_template("service_adapter.h.j2")
    return template.render(context)
