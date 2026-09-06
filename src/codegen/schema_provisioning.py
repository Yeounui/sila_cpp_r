"""R10-9g1: fetch and embed Schema constraint content referenced by <Url>.

Part A p70 lists URL as an alternative SOURCE of a Schema constraint, not an
exemption from the A224 MUST-validate rule. A Schema whose source is a Url
must therefore be resolved BEFORE serving requests (owner D1=a), never on the
validation path -- so codegen walks a parsed Feature for every such Url,
fetches each one once, and hands the embedded text to the meta emitter.
"""
from __future__ import annotations

import json
import urllib.request
from pathlib import Path
from typing import Any
from urllib.request import Request

from lxml import etree

from models.data_types import DataTypeType
from models.feature_definition import Feature

# ponytail: 4MiB is a tunable ceiling, not a proven limit on legitimate schemas.
_MAX_SCHEMA_BYTES = 4 * 1024 * 1024
_FETCH_TIMEOUT_SECONDS = 30


def collect_schema_urls(feature: Feature) -> dict[str, str]:
    """Map each distinct Schema Url in `feature` to its Schema Type ("Xml"/"Json").

    Walks the same roots as proto_emitter._needs_framework_import (command
    parameter/response/intermediate_response, property, metadata,
    data_type_definition) and recurses each DataTypeType the same shape as
    proto_emitter._uses_basic_type. First occurrence of a Url wins (dict
    dedups); a DataTypeIdentifier is not walked here -- it is one of the
    feature's own data_type_definition roots already in this list.
    """
    roots: list[DataTypeType] = []
    for cmd in feature.command:
        roots += [e.data_type for e in cmd.parameter]
        roots += [e.data_type for e in cmd.response]
        roots += [e.data_type for e in cmd.intermediate_response]
    roots += [p.data_type for p in feature.property]
    roots += [m.data_type for m in feature.metadata]
    roots += [d.data_type for d in feature.data_type_definition]

    urls: dict[str, str] = {}
    for root in roots:
        _collect_from_data_type(root, urls)
    return urls


def _collect_from_data_type(data_type: DataTypeType, urls: dict[str, str]) -> None:
    if data_type.list_value is not None:
        _collect_from_data_type(data_type.list_value.data_type, urls)
    elif data_type.structure is not None:
        for element in data_type.structure.element:
            _collect_from_data_type(element.data_type, urls)
    elif data_type.constrained is not None:
        constraints = data_type.constrained.constraints
        schema = constraints.schema
        if schema is not None and schema.url is not None and schema.url not in urls:
            urls[schema.url] = schema.type_value.value
        # R10-9g2: an Any's AllowedTypes candidates carry their own DataType
        # subtrees; a Schema Url nested in one must be provisioned too (Part A
        # p70). This walker previously stopped at data_type.constrained.data_type,
        # so a Url inside an AllowedTypes candidate was never collected.
        if constraints.allowed_types is not None:
            for candidate in constraints.allowed_types.data_type:
                _collect_from_data_type(candidate, urls)
        _collect_from_data_type(data_type.constrained.data_type, urls)
    # Basic and a bare DataTypeIdentifier carry no Schema constraint of their own.


def _read_schema(url: str, overrides: dict[str, Path]) -> bytes:
    """Return the raw bytes for `url`, from `overrides` if given, else fetched."""
    if url in overrides:
        path = overrides[url]
        try:
            return path.read_bytes()
        except FileNotFoundError as exc:
            raise FileNotFoundError(f"--schema-source override file not found for {url}: {path}") from exc

    # urlopen follows redirects and raises urllib.error.HTTPError on a non-2xx
    # status; both propagate up to provision_schemas' catch-all fail-closed wrap.
    request = Request(url, headers={"User-Agent": "sila2-codegen"})
    with urllib.request.urlopen(request, timeout=_FETCH_TIMEOUT_SECONDS) as response:
        body = response.read(_MAX_SCHEMA_BYTES + 1)
    if len(body) > _MAX_SCHEMA_BYTES:
        raise ValueError(f"Schema at {url} exceeds the {_MAX_SCHEMA_BYTES}-byte cap")
    return body


def provision_schemas(feature: Feature, overrides: dict[str, Path]) -> list[dict[str, Any]]:
    """Resolve every Schema Url in `feature` to its content, failing closed.

    Returns a list of {"url": str, "schema_xml": str} in collect_schema_urls
    iteration order. Any read/decode/well-formedness failure raises
    RuntimeError naming the offending url (D1=a: fail closed at codegen time
    so the validation path never touches the network). Xml is checked for
    XML well-formedness and Json for JSON well-formedness (fail closed at
    codegen time -- the validation path never touches the network).
    """
    provisioned: list[dict[str, Any]] = []
    for url, schema_type in collect_schema_urls(feature).items():
        try:
            text = _read_schema(url, overrides).decode("utf-8")
            if schema_type == "Xml":
                etree.fromstring(text.encode("utf-8"))
            else:  # "Json" -- fail closed on a malformed embedded schema (R10-9g2)
                json.loads(text)
        except Exception as exc:
            raise RuntimeError(f"could not provision Schema Url {url}: {exc}") from exc
        provisioned.append({"url": url, "schema_xml": text})
    return provisioned
