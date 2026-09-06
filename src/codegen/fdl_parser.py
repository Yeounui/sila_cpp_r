from __future__ import annotations

from functools import lru_cache
from pathlib import Path
from typing import Callable

from lxml import etree
from xsdata.formats.dataclass.parsers import XmlParser

from models.data_types import DataTypeType
from models.feature_definition import CommandObservable, Feature, PropertyObservable


class FdlError(Exception):
    """Raised for FDL validation failures."""


# Part B p84-85: Conversion Factor/Offset are IEEE 754 doubles, but the pinned
# sila_base Constraints.xsd:81-82 types them xs:decimal. sila_base is a git
# submodule we must not edit in place, so redirect the schema's Constraints.xsd
# include to an in-repo overlay that types them xs:double.
# Located relative to this module, not to --xsd: the copy lives in this repo at src/schema/.
_CONSTRAINTS_OVERLAY = Path(__file__).resolve().parent.parent / "schema" / "Constraints.xsd"

# Part A p66 / Part B p83: a SiLA Constrained Type MAY be based on another SiLA
# Constrained Type (the two constraint layers act as a logical AND), but the
# pinned sila_base fdl-validation.xsl rejects that nesting outright. sila_base
# is a submodule we must not edit in place, so validate against an in-repo
# overlay with only that one rejection removed instead.
_VALIDATION_XSLT_OVERLAY = Path(__file__).resolve().parent.parent / "schema" / "fdl-validation.xsl"


class _ConstraintsOverlayResolver(etree.Resolver):
    """Serves the xs:double Constraints.xsd overlay for any include of it.

    resolve_string (not resolve_filename) with base_url pinned to the ORIGINAL
    submodule Constraints.xsd keeps the overlay's own
    <xs:include schemaLocation="DataTypes.xsd"/> resolving against the submodule
    schema directory, where DataTypes.xsd actually lives.
    """

    def __init__(self, overlay_path: Path, base_url: str) -> None:
        self._overlay_bytes = overlay_path.read_bytes()
        self._base_url = base_url

    def resolve(self, url, pubid, context):
        if url and url.endswith("Constraints.xsd"):
            return self.resolve_string(self._overlay_bytes, context, base_url=self._base_url)
        return None


def _load_overlaid_schema(xsd_path: Path) -> etree.XMLSchema:
    overlay_path = _CONSTRAINTS_OVERLAY
    if not overlay_path.is_file():
        raise FdlError(
            f"Constraints overlay not found at {overlay_path}; the in-repo "
            f"src/schema/Constraints.xsd must ship with the codegen package "
            f"for the Part B p84-85 xs:double fix"
        )
    original_constraints = xsd_path.parent / "Constraints.xsd"
    parser = etree.XMLParser()
    parser.resolvers.add(_ConstraintsOverlayResolver(overlay_path, str(original_constraints)))
    return etree.XMLSchema(etree.parse(str(xsd_path), parser=parser))


@lru_cache(maxsize=None)
def _compiled_xslt(xslt_path: str) -> etree.XSLT:
    # Compiled once per stylesheet path: parse_fdl runs once per FDL file and
    # the stylesheet never changes within a codegen run.
    return etree.XSLT(etree.parse(xslt_path))


def _validate_against_normative_xslt(doc: etree._ElementTree, fdl_path: Path) -> None:
    """Run the overlay fdl-validation.xsl over the feature definition.

    The stylesheet produces no output for a valid feature and terminates with
    an xsl:message for an invalid one (third_party/sila_base/xslt/README.md:5-7),
    which lxml surfaces as XSLTApplyError. Running it is what keeps this
    parser's accept set identical to the normative one instead of tracking it
    check by check. ``doc`` is the tree parse_fdl already parsed from
    fdl_path; the path is only used in error messages. The overlay (not
    sila_base's own copy) is run because it accepts a Constrained Type based
    on another Constrained Type (Part A p66 / Part B p83) that the pinned
    stylesheet wrongly rejects; it has no xsl:include, so no resolver is needed.
    """
    xslt_path = _VALIDATION_XSLT_OVERLAY
    if not xslt_path.is_file():
        raise FdlError(
            f"Validation stylesheet overlay not found at {xslt_path}; the "
            f"in-repo src/schema/fdl-validation.xsl must ship with the "
            f"codegen package for the Part A p66 / Part B p83 nesting fix"
        )
    transform = _compiled_xslt(str(xslt_path))
    try:
        transform(doc)
    except etree.XSLTApplyError as exc:
        raise FdlError(f"{fdl_path.name}: {exc}") from exc


def parse_fdl(fdl_path: Path, xsd_path: Path) -> Feature:
    schema = _load_overlaid_schema(xsd_path)
    doc = etree.parse(str(fdl_path))
    if not schema.validate(doc):
        raise FdlError(str(schema.error_log))

    _validate_against_normative_xslt(doc, fdl_path)

    parser = XmlParser()
    feature = parser.from_path(fdl_path, Feature)

    _check_no_duplicate_identifiers(feature)
    _check_command_sub_element_duplicates(feature)
    _check_structure_element_duplicates(feature)
    _check_execution_error_references(feature)
    _check_data_type_references(feature)
    _check_no_nested_list(feature)
    _check_no_cyclic_data_type_refs(feature)
    _check_no_intermediate_response_on_unobservable(feature)

    return feature


def _check_unique(items: list, label: str, get_id: Callable[[object], str]) -> None:
    # SiLA 2 Part B p80 / Part A p66: identifier uniqueness MUST be checked
    # without regard to case, so compare on the case-folded key.
    seen: set[str] = set()
    for item in items:
        identifier = get_id(item)
        folded = identifier.casefold()
        if folded in seen:
            raise FdlError(f"Duplicate {label} '{identifier}'")
        seen.add(folded)


def _check_no_duplicate_identifiers(feature: Feature) -> None:
    # SiLA2 spec: identifiers must be unique within their own kind,
    # not across kinds (a Property and a DataTypeDefinition may share a name).
    _check_unique(feature.command, "Command", lambda c: c.identifier)
    _check_unique(feature.property, "Property", lambda p: p.identifier)
    _check_unique(feature.metadata, "Metadata", lambda m: m.identifier)
    _check_unique(feature.defined_execution_error, "DefinedExecutionError", lambda e: e.identifier)
    _check_unique(feature.data_type_definition, "DataTypeDefinition", lambda d: d.identifier)


def _check_command_sub_element_duplicates(feature: Feature) -> None:
    for cmd in feature.command:
        for label, elements in [
            ("Parameter", cmd.parameter),
            ("Response", cmd.response),
            ("IntermediateResponse", cmd.intermediate_response),
        ]:
            _check_unique(
                elements, f"{label} in command '{cmd.identifier}'", lambda el: el.identifier
            )
        if cmd.defined_execution_errors is not None:
            _check_unique(
                cmd.defined_execution_errors.identifier,
                f"DefinedExecutionError reference in command '{cmd.identifier}'",
                lambda eid: eid,
            )


def _check_structure_element_duplicates(feature: Feature) -> None:
    # SiLA 2 Part A p66 (SiLA Structure Type): Element Identifiers MUST be
    # unique within each Structure, checked case-insensitively. Walk every
    # DataType (mirrors _check_no_nested_list's List/Structure/Constrained
    # traversal) and check each Structure's element list via the shared,
    # now case-folding _check_unique.
    def _walk(dt: DataTypeType, ctx: str) -> None:
        if dt.structure is not None:
            _check_unique(
                dt.structure.element,
                f"Structure Element in '{ctx}'",
                lambda el: el.identifier,
            )
            for el in dt.structure.element:
                _walk(el.data_type, f"{ctx}/{el.identifier}")
        elif dt.list_value is not None:
            _walk(dt.list_value.data_type, ctx)
        elif dt.constrained is not None:
            _walk(dt.constrained.data_type, ctx)

    for cmd in feature.command:
        for el in list(cmd.parameter) + list(cmd.response) + list(cmd.intermediate_response):
            _walk(el.data_type, f"{cmd.identifier}/{el.identifier}")
    for prop in feature.property:
        _walk(prop.data_type, prop.identifier)
    for meta in feature.metadata:
        _walk(meta.data_type, meta.identifier)
    for dtd in feature.data_type_definition:
        _walk(dtd.data_type, f"DataType/{dtd.identifier}")


def _check_execution_error_references(feature: Feature) -> None:
    defined = {e.identifier for e in feature.defined_execution_error}
    elements = list(feature.command) + list(feature.property) + list(feature.metadata)
    for element in elements:
        errors = element.defined_execution_errors
        if errors is None:
            continue
        for error_id in errors.identifier:
            if error_id not in defined:
                raise FdlError(
                    f"'{element.identifier}' references undefined execution "
                    f"error '{error_id}'"
                )


def _check_data_type_references(feature: Feature) -> None:
    defined = {d.identifier for d in feature.data_type_definition}
    refs: set[str] = set()

    for command in feature.command:
        for element in (
            list(command.parameter)
            + list(command.response)
            + list(command.intermediate_response)
        ):
            _collect_type_refs(element.data_type, refs)
    for prop in feature.property:
        _collect_type_refs(prop.data_type, refs)
    for metadata in feature.metadata:
        _collect_type_refs(metadata.data_type, refs)
    for data_type_definition in feature.data_type_definition:
        _collect_type_refs(data_type_definition.data_type, refs)

    undefined = refs - defined
    if undefined:
        raise FdlError(
            f"Undefined DataTypeIdentifier reference(s) in feature "
            f"'{feature.identifier}': {sorted(undefined)}"
        )


def _check_no_nested_list(feature: Feature) -> None:
    def _walk(dt: DataTypeType, in_list: bool, ctx: str) -> None:
        if dt.list_value is not None:
            if in_list:
                raise FdlError(f"Nested List in '{ctx}': List of List is forbidden")
            _walk(dt.list_value.data_type, True, ctx)
        elif dt.structure is not None:
            for el in dt.structure.element:
                _walk(el.data_type, False, ctx)
        elif dt.constrained is not None:
            _walk(dt.constrained.data_type, in_list, ctx)

    for cmd in feature.command:
        for el in list(cmd.parameter) + list(cmd.response) + list(cmd.intermediate_response):
            _walk(el.data_type, False, f"{cmd.identifier}/{el.identifier}")
    for prop in feature.property:
        _walk(prop.data_type, False, prop.identifier)
    for dtd in feature.data_type_definition:
        _walk(dtd.data_type, False, f"DataType/{dtd.identifier}")


def _check_no_cyclic_data_type_refs(feature: Feature) -> None:
    # Self-references (A → A) are valid in proto (recursive message).
    # Only multi-node cycles (A → B → A) are rejected.
    adjacency: dict[str, set[str]] = {}
    for dtd in feature.data_type_definition:
        refs: set[str] = set()
        _collect_type_refs(dtd.data_type, refs)
        refs.discard(dtd.identifier)
        adjacency[dtd.identifier] = refs

    visited: set[str] = set()
    path: set[str] = set()

    def _dfs(name: str) -> None:
        if name in path:
            raise FdlError(f"Cyclic DataTypeDefinition reference: '{name}'")
        if name in visited:
            return
        path.add(name)
        for dep in adjacency.get(name, set()):
            _dfs(dep)
        path.remove(name)
        visited.add(name)

    for name in adjacency:
        _dfs(name)


def _check_no_intermediate_response_on_unobservable(feature: Feature) -> None:
    for cmd in feature.command:
        if cmd.observable == CommandObservable.NO and cmd.intermediate_response:
            raise FdlError(
                f"Unobservable command '{cmd.identifier}' must not have "
                f"IntermediateResponse"
            )


def _collect_type_refs(dt: DataTypeType, refs: set[str]) -> None:
    if dt.data_type_identifier is not None:
        refs.add(dt.data_type_identifier)
    elif dt.list_value is not None:
        _collect_type_refs(dt.list_value.data_type, refs)
    elif dt.structure is not None:
        for element in dt.structure.element:
            _collect_type_refs(element.data_type, refs)
    elif dt.constrained is not None:
        _collect_type_refs(dt.constrained.data_type, refs)
    # dt.basic is a leaf: nothing to check.


def package_name(feature: Feature) -> str:
    category = feature.category if feature.category else "none"
    major_version = feature.feature_version.split(".")[0]
    return (
        f"sila2.{feature.originator}.{category}."
        f"{feature.identifier.lower()}.v{major_version}"
    )


def fqi(feature: Feature) -> str:
    category = feature.category if feature.category else "none"
    major_version = feature.feature_version.split(".")[0]
    return f"{feature.originator}/{category}/{feature.identifier}/v{major_version}"


def property_rpc_name(prop: Feature.Property) -> str:
    prefix = "Subscribe_" if prop.observable == PropertyObservable.YES else "Get_"
    return prefix + prop.identifier
