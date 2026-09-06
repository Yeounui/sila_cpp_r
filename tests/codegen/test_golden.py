from __future__ import annotations

import difflib
import os
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "src" / "codegen"))

from fdl_parser import parse_fdl
from proto_emitter import emit_proto
from meta_emitter import emit_meta_header, emit_meta_source, emit_service_adapter
_EXAMPLES_DIR = _REPO_ROOT / "tests" / "examples" / "fdl"
_XSD_PATH = _REPO_ROOT / "third_party" / "sila_base" / "schema" / "FeatureDefinition.xsd"
_GOLDEN_DIR = Path(__file__).resolve().parent / "golden"
_REFERENCE_PROTO_DIR = _REPO_ROOT / "third_party" / "sila_base" / "xslt" / "test" / "reference-proto"

_UPDATE_GOLDEN = os.environ.get("UPDATE_GOLDEN") == "1"

# (fdl path, golden filename prefix, what it exercises)
_CASES = [
    pytest.param(
        _EXAMPLES_DIR / "teleshake" / "Settings.sila.xml",
        "SettingsService",
        id="unobservable-only",
    ),
    pytest.param(
        _EXAMPLES_DIR / "sila_base" / "valid-fdl" / "ObservableBasic.sila.xml",
        "ObservableBasic",
        id="observable-with-intermediate-response",
    ),
    pytest.param(
        _EXAMPLES_DIR / "sila_base" / "valid-fdl" / "DataTypeDefinition.sila.xml",
        "DataTypeDefinition",
        id="data-type-definition",
    ),
    pytest.param(
        _EXAMPLES_DIR / "sila_base" / "valid-fdl" / "Constrained.sila.xml",
        "Constrained",
        id="constraints",
    ),
    pytest.param(
        _EXAMPLES_DIR / "sila_base" / "valid-fdl" / "Metadata.sila.xml",
        "Metadata",
        id="metadata",
    ),
    pytest.param(
        _EXAMPLES_DIR / "sila_base" / "valid-fdl" / "NestedStructure.sila.xml",
        "NestedStructure",
        id="nested-structure",
    ),
    pytest.param(
        _EXAMPLES_DIR / "sila_base" / "valid-fdl" / "List.sila.xml",
        "List",
        id="list-type",
    ),
    pytest.param(
        _EXAMPLES_DIR / "sila_base" / "valid-fdl" / "NestedDataTypeDefinition.sila.xml",
        "NestedDataTypeDefinition",
        id="nested-data-type-definition",
    ),
    pytest.param(
        _EXAMPLES_DIR / "sila_base" / "valid-fdl" / "ObservableCommandNoIntermediateResponse.sila.xml",
        "ObservableCommandNoIntermediateResponse",
        id="observable-no-intermediate",
    ),
    pytest.param(
        _EXAMPLES_DIR / "sila_base" / "valid-fdl" / "SimpleStructure.sila.xml",
        "SimpleStructure",
        id="simple-structure",
    ),
]


def _assert_matches_golden(golden_path: Path, actual: str) -> None:
    if _UPDATE_GOLDEN:
        golden_path.write_text(actual, encoding="utf-8")
        return

    expected = golden_path.read_text(encoding="utf-8")
    if actual != expected:
        diff = "".join(
            difflib.unified_diff(
                expected.splitlines(keepends=True),
                actual.splitlines(keepends=True),
                fromfile=str(golden_path),
                tofile="actual",
            )
        )
        pytest.fail(
            f"Output for {golden_path.name} does not match golden file.\n"
            f"Run with UPDATE_GOLDEN=1 to update if this change is intentional.\n{diff}"
        )


@pytest.mark.parametrize("fdl_path, golden_prefix", _CASES)
def test_golden(fdl_path: Path, golden_prefix: str) -> None:
    feature = parse_fdl(fdl_path, _XSD_PATH)
    xml_text = fdl_path.read_text(encoding="utf-8")

    _assert_matches_golden(_GOLDEN_DIR / f"{golden_prefix}.proto", emit_proto(feature))
    _assert_matches_golden(
        _GOLDEN_DIR / f"{golden_prefix}Meta.h", emit_meta_header(feature, xml_text)
    )
    _assert_matches_golden(
        _GOLDEN_DIR / f"{golden_prefix}Meta.cc", emit_meta_source(feature, xml_text)
    )
    _assert_matches_golden(
        _GOLDEN_DIR / f"{golden_prefix}ServiceAdapter.h", emit_service_adapter(feature)
    )


@pytest.mark.parametrize("fdl_path, reference_name", _CASES[1:])
def test_proto_matches_sila_base_reference(fdl_path: Path, reference_name: str) -> None:
    feature = parse_fdl(fdl_path, _XSD_PATH)
    expected = (_REFERENCE_PROTO_DIR / f"{reference_name}.proto").read_text(encoding="utf-8")
    normalize = lambda text: "\n".join(line.rstrip() for line in text.rstrip().splitlines())
    assert normalize(emit_proto(feature)) == normalize(expected)
