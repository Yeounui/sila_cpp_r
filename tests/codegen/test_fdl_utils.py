from __future__ import annotations

import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "src" / "codegen"))

from fdl_parser import fqi, package_name, property_rpc_name
from meta_emitter import emit_meta_header, emit_meta_source
from models.data_types import BasicType, DataTypeType, SiLaelement
from models.feature_definition import CommandObservable, Feature, PropertyObservable
from proto_emitter import emit_proto


def _make_feature(
    *,
    identifier: str = "TestFeature",
    originator: str = "com.test",
    category: str = "examples",
    feature_version: str = "1.0",
    si_la2_version: str = "1.0",
    display_name: str = "Test",
    description: str = "A test",
    command: list[Feature.Command] | None = None,
    property: list[Feature.Property] | None = None,
    metadata: list[Feature.Metadata] | None = None,
) -> Feature:
    return Feature(
        identifier=identifier,
        display_name=display_name,
        description=description,
        si_la2_version=si_la2_version,
        feature_version=feature_version,
        originator=originator,
        category=category,
        command=command or [],
        property=property or [],
        metadata=metadata or [],
    )


def test_package_name() -> None:
    feature = _make_feature(originator="com.example", category="pumps", identifier="MyPump")
    assert package_name(feature) == "sila2.com.example.pumps.mypump.v1"


def test_package_name_no_category() -> None:
    feature = _make_feature(originator="com.example", category="", identifier="MyPump")
    assert package_name(feature) == "sila2.com.example.none.mypump.v1"


def test_fqi() -> None:
    feature = _make_feature(originator="com.example", category="pumps", identifier="MyPump")
    assert fqi(feature) == "com.example/pumps/MyPump/v1"


def test_property_rpc_name_observable() -> None:
    prop = Feature.Property(
        identifier="Temperature",
        display_name="Temperature",
        description="Current temperature",
        observable=PropertyObservable.YES,
        data_type=DataTypeType(basic=BasicType.REAL),
    )
    assert property_rpc_name(prop) == "Subscribe_Temperature"


def test_property_rpc_name_unobservable() -> None:
    prop = Feature.Property(
        identifier="Temperature",
        display_name="Temperature",
        description="Current temperature",
        observable=PropertyObservable.NO,
        data_type=DataTypeType(basic=BasicType.REAL),
    )
    assert property_rpc_name(prop) == "Get_Temperature"


def _minimal_feature() -> Feature:
    command = Feature.Command(
        identifier="DoSomething",
        display_name="Do Something",
        description="Does something",
        observable=CommandObservable.NO,
        parameter=[
            SiLaelement(
                identifier="Input",
                display_name="Input",
                description="An input",
                data_type=DataTypeType(basic=BasicType.STRING),
            )
        ],
        response=[
            SiLaelement(
                identifier="Output",
                display_name="Output",
                description="An output",
                data_type=DataTypeType(basic=BasicType.INTEGER),
            )
        ],
    )
    prop = Feature.Property(
        identifier="Status",
        display_name="Status",
        description="Current status",
        observable=PropertyObservable.YES,
        data_type=DataTypeType(basic=BasicType.STRING),
    )
    metadata = Feature.Metadata(
        identifier="RequestId",
        display_name="Request Id",
        description="A request identifier",
        data_type=DataTypeType(basic=BasicType.STRING),
    )
    return _make_feature(command=[command], property=[prop], metadata=[metadata])


def test_emit_proto_minimal() -> None:
    proto_text = emit_proto(_minimal_feature())
    assert "service TestFeature" in proto_text
    assert "rpc DoSomething" in proto_text
    assert "rpc Subscribe_Status" in proto_text
    assert "message DoSomething_Parameters" in proto_text
    assert "message DoSomething_Responses" in proto_text
    assert "message Subscribe_Status_Responses" in proto_text


def test_emit_meta_header_minimal() -> None:
    header = emit_meta_header(_minimal_feature(), "<fake/>")
    assert "#pragma once" in header
    assert "namespace sila2::generated::testfeature" in header
    assert "kFdlXml" in header
    assert "com.test/examples/TestFeature/v1" in header
    assert '"1.0"' in header


def test_emit_meta_source_minimal() -> None:
    source = emit_meta_source(_minimal_feature(), "<fake/>")
    assert "TestFeature" in source


def _observable_command_feature(*, with_intermediate: bool) -> Feature:
    command = Feature.Command(
        identifier="LongRun",
        display_name="Long Run",
        description="Runs for a while",
        observable=CommandObservable.YES,
        parameter=[
            SiLaelement(
                identifier="Duration",
                display_name="Duration",
                description="How long to run",
                data_type=DataTypeType(basic=BasicType.INTEGER),
            )
        ],
        response=[
            SiLaelement(
                identifier="Result",
                display_name="Result",
                description="The result",
                data_type=DataTypeType(basic=BasicType.STRING),
            )
        ],
        intermediate_response=[
            SiLaelement(
                identifier="Progress",
                display_name="Progress",
                description="Current progress",
                data_type=DataTypeType(basic=BasicType.REAL),
            )
        ]
        if with_intermediate
        else [],
    )
    return _make_feature(command=[command])


def test_emit_proto_observable_command() -> None:
    proto_text = emit_proto(_observable_command_feature(with_intermediate=True))
    assert "rpc LongRun(" in proto_text
    assert "rpc LongRun_Info(" in proto_text
    assert "rpc LongRun_Intermediate(" in proto_text
    assert "rpc LongRun_Result(" in proto_text
    assert "message LongRun_Parameters" in proto_text
    assert "message LongRun_Responses" in proto_text
    assert "message LongRun_IntermediateResponses" in proto_text


def test_emit_proto_observable_command_no_intermediate() -> None:
    proto_text = emit_proto(_observable_command_feature(with_intermediate=False))
    assert "rpc LongRun(" in proto_text
    assert "rpc LongRun_Info(" in proto_text
    assert "rpc LongRun_Result(" in proto_text
    assert "rpc LongRun_Intermediate(" not in proto_text
    assert "message LongRun_IntermediateResponses" not in proto_text


def test_emit_meta_raw_delimiter_default() -> None:
    source = emit_meta_source(_minimal_feature(), "<normal xml/>")
    assert 'R"xml(' in source


def test_emit_meta_raw_delimiter_escape() -> None:
    source = emit_meta_source(_minimal_feature(), '<xml with )xml" inside/>')
    assert 'R"sila(' in source
    assert 'R"xml(' not in source


def test_emit_meta_raw_delimiter_skips_every_taken_terminator() -> None:
    # Codex review of 299bccc: text holding both `)xml"` and `)sila"` must
    # not be emitted with an unterminated raw string.
    source = emit_meta_source(_minimal_feature(), '<x>)xml" and )sila" here</x>')
    assert 'R"sila1(' in source
    assert ')sila1"' in source
