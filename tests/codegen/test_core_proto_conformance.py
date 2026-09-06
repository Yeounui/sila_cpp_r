from __future__ import annotations

import re
from pathlib import Path

import pytest
from lxml import etree

REPO_ROOT = Path(__file__).resolve().parents[2]
FDL_ROOT = REPO_ROOT / "third_party/sila_base/feature_definitions/org/silastandard/core"
XSLT_PATH = REPO_ROOT / "third_party/sila_base/xslt/fdl2proto.xsl"
PROTO_ROOT = REPO_ROOT / "src/sila/common/proto"

# proto basename -> the FDL revision that hand-written proto tracks. Note
# LockController.proto tracks v1_0, not the newer v2_0 also present in the tree.
CORE_PROTOS = {
    "AuthenticationService": "AuthenticationService-v1_0",
    "AuthorizationService": "AuthorizationService-v1_0",
    "AuthorizationConfigurationService": "AuthorizationConfigurationService-v1_0",
    "AuthorizationProviderService": "AuthorizationProviderService-v1_0",
    "ConnectionConfigurationService": "ConnectionConfigurationService-v1_1",
    "ErrorRecoveryService": "ErrorRecoveryService-v2_0",
    "LockController": "LockController-v1_0",
    "SiLAService": "SiLAService-v1_0",
    "SimulationController": "SimulationController-v1_0",
}


def _tokens(proto_text: str) -> list[str]:
    # Comments and whitespace are free-form in the hand-written protos -- they
    # carry shortened descriptions on purpose. The normative content is the
    # token stream: identifiers, field numbers and structure.
    stripped = re.sub(r"/\*.*?\*/", " ", proto_text, flags=re.S)
    return re.findall(r"[A-Za-z0-9_.]+|[{}();=]", stripped)


@pytest.mark.parametrize("proto_name", sorted(CORE_PROTOS))
def test_core_proto_matches_normative_xslt(proto_name: str) -> None:
    transform = etree.XSLT(etree.parse(str(XSLT_PATH)))
    fdl = FDL_ROOT / f"{CORE_PROTOS[proto_name]}.sila.xml"
    expected = str(transform(etree.parse(str(fdl))))
    actual = (PROTO_ROOT / f"{proto_name}.proto").read_text(encoding="utf-8")
    assert _tokens(actual) == _tokens(expected)


def test_core_proto_pins_match_cmake_fdl_embeds() -> None:
    # src/sila/CMakeLists.txt's _fdl_embed_entries decides which FDL revision
    # the server embeds and advertises. Bumping an embed (say LockController
    # -> v2_0) while the checked-in proto tracks the old revision would let
    # advertised FDL and served proto diverge (the S8 defect class), so the
    # CORE_PROTOS pins are checked against the embed list, not restated.
    cmake_text = (REPO_ROOT / "src/sila/CMakeLists.txt").read_text(encoding="utf-8")
    embedded = dict(re.findall(r'"([A-Za-z]+)-(v\d+_\d+)\|', cmake_text))
    shared = sorted(CORE_PROTOS.keys() & embedded.keys())
    assert shared, "parsed no overlap with _fdl_embed_entries -- layout drift?"
    for name in shared:
        assert CORE_PROTOS[name] == f"{name}-{embedded[name]}"
