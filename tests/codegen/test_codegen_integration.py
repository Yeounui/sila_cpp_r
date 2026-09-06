from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "src" / "codegen"))

from fdl_parser import parse_fdl
from proto_emitter import emit_proto
from meta_emitter import emit_meta_header, emit_meta_source
_EXAMPLES_DIR = _REPO_ROOT / "tests" / "examples" / "fdl"
_XSD_PATH = _REPO_ROOT / "third_party" / "sila_base" / "schema" / "FeatureDefinition.xsd"
_PROTO_IMPORT_DIR = _REPO_ROOT / "third_party" / "sila_base" / "protobuf"


def _all_example_fdl_files() -> list[Path]:
    excluded = {"invalid-fdl", "valid-fdl"}
    return sorted(
        p
        for p in _EXAMPLES_DIR.rglob("*.sila.xml")
        if excluded.isdisjoint(p.relative_to(_EXAMPLES_DIR).parts)
    )


_FDL_FILES = _all_example_fdl_files()
_FDL_PARAMS = [pytest.param(p, id=p.stem) for p in _FDL_FILES]


@pytest.fixture
def xsd_path() -> Path:
    return _XSD_PATH


@pytest.mark.parametrize("fdl_path", _FDL_PARAMS)
def test_parse_all_example_fdl(fdl_path: Path, xsd_path: Path) -> None:
    parse_fdl(fdl_path, xsd_path)


@pytest.mark.parametrize("fdl_path", _FDL_PARAMS)
def test_proto_generation_all(fdl_path: Path, xsd_path: Path) -> None:
    feature = parse_fdl(fdl_path, xsd_path)
    proto_text = emit_proto(feature)
    assert proto_text
    assert proto_text.startswith('syntax = "proto3";')
    assert f"service {feature.identifier}" in proto_text
    assert "package sila2." in proto_text


@pytest.mark.skipif(shutil.which("protoc") is None, reason="protoc not available")
@pytest.mark.parametrize("fdl_path", _FDL_PARAMS)
def test_proto_compiles_with_protoc(fdl_path: Path, xsd_path: Path, tmp_path: Path) -> None:
    feature = parse_fdl(fdl_path, xsd_path)
    proto_text = emit_proto(feature)
    proto_file = tmp_path / f"{fdl_path.stem}.proto"
    proto_file.write_text(proto_text, encoding="utf-8")

    result = subprocess.run(
        [
            "protoc",
            f"--proto_path={tmp_path}",
            f"--proto_path={_PROTO_IMPORT_DIR}",
            "--descriptor_set_out=/dev/null",
            str(proto_file),
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("fdl_path", _FDL_PARAMS)
def test_meta_generation_all(fdl_path: Path, xsd_path: Path) -> None:
    feature = parse_fdl(fdl_path, xsd_path)
    xml_text = fdl_path.read_text(encoding="utf-8")

    header = emit_meta_header(feature, xml_text)
    assert "#pragma once" in header
    assert "kFdlXml" in header

    source = emit_meta_source(feature, xml_text)
    assert f'#include "{feature.identifier}Meta.h"' in source
    assert "kFdlXml" in source
