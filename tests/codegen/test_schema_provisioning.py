from __future__ import annotations

import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "src" / "codegen"))

from fdl_parser import parse_fdl
from meta_emitter import emit_meta_source
from schema_provisioning import collect_schema_urls, provision_schemas

_XSD_PATH = _REPO_ROOT / "third_party" / "sila_base" / "schema" / "FeatureDefinition.xsd"
_NO_URL_FDL_PATH = (
    _REPO_ROOT / "tests" / "examples" / "fdl" / "sila_base" / "valid-fdl" / "Constrained.sila.xml"
)

# The raw (unescaped) form of the schema embedded in test_command_parameter_validator.cc's
# kNoteSchemaXml, so a passing C++ test and this Python test agree on the same schema text.
NOTE_SCHEMA = (
    '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">'
    '<xs:element name="note"><xs:complexType><xs:sequence>'
    '<xs:element name="header"><xs:complexType><xs:sequence>'
    '<xs:element name="to" type="xs:string"/>'
    "</xs:sequence></xs:complexType></xs:element>"
    '<xs:element name="body" type="xs:string"/>'
    "</xs:sequence></xs:complexType></xs:element></xs:schema>"
)

# R10-9g2: an Any parameter whose AllowedTypes list has a single candidate
# that is itself Constrained{String, Schema{Xml,Url}} -- proves
# _collect_from_data_type now recurses into constraints.allowed_types (the
# sibling gap the g1 writer flagged; previously such a nested Url was never
# collected/provisioned).
_ALLOWED_TYPES_URL_FDL = """<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="test"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
  <Identifier>AllowedTypesSchemaUrlTest</Identifier>
  <DisplayName>Allowed Types Schema Url Test</DisplayName>
  <Description>R10-9g2 test-only Any parameter whose AllowedTypes candidate carries a Url Schema constraint.</Description>
  <Command>
    <Identifier>CheckAnySchemaCandidate</Identifier>
    <DisplayName>Check Any Schema Candidate</DisplayName>
    <Description>An Any parameter whose sole AllowedTypes entry is a Schema-constrained String.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>An Any type parameter with an AllowedTypes constraint allowing only a String matching a Url Schema.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Any</Basic></DataType>
          <Constraints>
            <AllowedTypes>
              <DataType>
                <Constrained>
                  <DataType><Basic>String</Basic></DataType>
                  <Constraints>
                    <Schema>
                      <Type>Xml</Type>
                      <Url>https://example.test/note.xsd</Url>
                    </Schema>
                  </Constraints>
                </Constrained>
              </DataType>
            </AllowedTypes>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
</Feature>
"""

_URL_FDL = """<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="test"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
  <Identifier>SchemaXmlUrlTest</Identifier>
  <DisplayName>Schema Xml Url Test</DisplayName>
  <Description>R10-9g1 test-only String parameter carrying a Url Schema constraint.</Description>
  <Command>
    <Identifier>CheckStringSchemaUrl</Identifier>
    <DisplayName>Check String Schema Url</DisplayName>
    <Description>A String parameter with a Url-sourced W3C XML Schema constraint.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>A String parameter with a Schema constraint of Xml/Url.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>String</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Xml</Type>
              <Url>https://example.test/note.xsd</Url>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
</Feature>
"""

# R10-9g2: a String parameter with a Url-sourced Schema whose Type is Json --
# used to prove provision_schemas' new json.loads well-formedness check
# (mirroring the existing etree.fromstring check for Xml) fails closed on a
# malformed schema fetched for a Json Url.
_JSON_URL_FDL = """<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="test"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
  <Identifier>SchemaJsonUrlTest</Identifier>
  <DisplayName>Schema Json Url Test</DisplayName>
  <Description>R10-9g2 test-only String parameter carrying a Url-sourced JSON Schema constraint.</Description>
  <Command>
    <Identifier>CheckStringJsonSchemaUrl</Identifier>
    <DisplayName>Check String Json Schema Url</DisplayName>
    <Description>A String parameter with a Url-sourced JSON Schema constraint.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>A String parameter with a Schema constraint of Json/Url.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>String</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Json</Type>
              <Url>https://example.test/note.json</Url>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
</Feature>
"""


def _parse(tmp_path: Path, fdl_text: str):
    fdl_path = tmp_path / "test.sila.xml"
    fdl_path.write_text(fdl_text, encoding="utf-8")
    return parse_fdl(fdl_path, _XSD_PATH)


def test_override_embeds_schema_and_keeps_fdl_unchanged(tmp_path: Path) -> None:
    schema_file = tmp_path / "note.xsd"
    schema_file.write_text(NOTE_SCHEMA, encoding="utf-8")

    feature = _parse(tmp_path, _URL_FDL)
    assert collect_schema_urls(feature) == {"https://example.test/note.xsd": "Xml"}

    provisioned = provision_schemas(feature, {"https://example.test/note.xsd": schema_file})
    meta_cc = emit_meta_source(feature, _URL_FDL, provisioned)

    assert "kProvisionedSchemas" in meta_cc
    assert NOTE_SCHEMA in meta_cc
    assert "https://example.test/note.xsd" in meta_cc
    # Hard constraint: kFdlXml keeps the original <Url>, never rewritten to <Inline>.
    assert "<Url>https://example.test/note.xsd</Url>" in meta_cc


def test_no_url_feature_emits_no_table() -> None:
    feature = parse_fdl(_NO_URL_FDL_PATH, _XSD_PATH)
    xml_text = _NO_URL_FDL_PATH.read_text(encoding="utf-8")

    assert provision_schemas(feature, {}) == []
    assert "kProvisionedSchemas" not in emit_meta_source(feature, xml_text, [])


def test_missing_schema_override_fails_generation(tmp_path: Path) -> None:
    feature = _parse(tmp_path, _URL_FDL)
    overrides = {"https://example.test/note.xsd": tmp_path / "absent.xsd"}

    with pytest.raises((RuntimeError, FileNotFoundError)) as exc:
        provision_schemas(feature, overrides)
    assert "https://example.test/note.xsd" in str(exc.value)


def test_malformed_xml_schema_fails_generation(tmp_path: Path) -> None:
    schema_file = tmp_path / "note.xsd"
    schema_file.write_text("<xs:schema>", encoding="utf-8")  # unclosed -> not well-formed

    feature = _parse(tmp_path, _URL_FDL)
    with pytest.raises(RuntimeError) as exc:
        provision_schemas(feature, {"https://example.test/note.xsd": schema_file})
    assert "https://example.test/note.xsd" in str(exc.value)


def test_allowed_types_candidate_schema_url_is_collected(tmp_path: Path) -> None:
    feature = _parse(tmp_path, _ALLOWED_TYPES_URL_FDL)
    # Proves _collect_from_data_type now walks constraints.allowed_types: the
    # Url lives two levels deep (Constrained.Constraints.AllowedTypes ->
    # candidate DataType -> Constrained.Constraints.Schema), not at a root.
    assert collect_schema_urls(feature) == {"https://example.test/note.xsd": "Xml"}


def test_malformed_json_schema_fails_generation(tmp_path: Path) -> None:
    schema_file = tmp_path / "note.json"
    schema_file.write_text("{not json", encoding="utf-8")  # not well-formed JSON

    feature = _parse(tmp_path, _JSON_URL_FDL)
    with pytest.raises(RuntimeError) as exc:
        provision_schemas(feature, {"https://example.test/note.json": schema_file})
    assert "https://example.test/note.json" in str(exc.value)
