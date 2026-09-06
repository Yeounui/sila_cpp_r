from __future__ import annotations

import difflib
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "src" / "codegen"))

from fdl_parser import FdlError, parse_fdl  # noqa: E402
XSD_PATH = REPO_ROOT / "third_party/sila_base/schema/FeatureDefinition.xsd"
FDL_ROOT = REPO_ROOT / "tests/examples/fdl/sila_base"
VALID_DIR = FDL_ROOT / "valid-fdl"
INVALID_DIR = FDL_ROOT / "invalid-fdl"

# Part B p84-85 (R2-9): Conversion Factor/Offset are IEEE 754 doubles. Part B
# p85 (R2-7): ElementCount/MinimalElementCount/MaximalElementCount MUST accept
# a 0 bound. Part A p67 / Part B p83 (R9-6): MinimalLength MUST accept a 0
# bound too. The overlay below retypes all of these; this drift guard makes
# sure a future submodule bump does not silently change anything else in the
# copy.
SILA_BASE_CONSTRAINTS = REPO_ROOT / "third_party/sila_base/schema/Constraints.xsd"
OVERLAY_CONSTRAINTS = REPO_ROOT / "src/schema/Constraints.xsd"

# Part A p66 / Part B p83 (R9-47): a SiLA Constrained Type MAY be based on
# another SiLA Constrained Type (the two constraint layers act as a logical
# AND). The pinned sila_base fdl-validation.xsl rejects that nesting outright,
# so this overlay removes only that one xsl:when.
SILA_BASE_XSLT = REPO_ROOT / "third_party/sila_base/xslt/fdl-validation.xsl"
OVERLAY_XSLT = REPO_ROOT / "src/schema/fdl-validation.xsl"

# Part A p66 / Part B p83 (R9-47): the corpus fixture stays filed under
# invalid-fdl/ (the corpus mirrors sila_base's own fixtures verbatim and is
# never moved), but the overlay XSLT accepts it, so it is reclassified here
# instead.



@pytest.fixture
def xsd_path() -> Path:
    return XSD_PATH


def _sila_files(directory: Path) -> list[Path]:
    return sorted(directory.glob("*.sila.xml"))


def _invalid_fdl_files_excluding_reclassified() -> list[Path]:
    # ConstrainedConstrained.sila.xml is spec-valid (R9-47) but still lives
    # under invalid-fdl/ because the corpus mirrors sila_base verbatim; drop
    # it here and cover it separately in test_reclassified_constrained_on_constrained_accepted.
    return _sila_files(INVALID_DIR)


@pytest.mark.parametrize("fdl_path", _sila_files(VALID_DIR), ids=lambda p: p.stem)
def test_valid_fdl_accepted(fdl_path: Path, xsd_path: Path) -> None:
    parse_fdl(fdl_path, xsd_path)


@pytest.mark.parametrize("fdl_path", _invalid_fdl_files_excluding_reclassified(), ids=lambda p: p.stem)
def test_invalid_fdl_rejected(fdl_path: Path, xsd_path: Path) -> None:
    # Every fixture under invalid-fdl/ must be rejected: the seven hand-rolled
    # _check_* functions catch the proto-affecting errors, and sila_base's own
    # fdl-validation.xsl (run inside parse_fdl) covers the value-range and
    # XSD-only rest. EmptyDataType is caught ONLY by the hand-rolled checks --
    # the stylesheet accepts it -- so this test also guards their survival.
    with pytest.raises(FdlError):
        parse_fdl(fdl_path, xsd_path)


def _nested_constrained_fdl(inner_base: str, inner_constraints: str, outer_constraints: str) -> str:
    # A property whose type is Constrained(Constrained(<inner_base>)), the
    # nesting Part A p66 / Part B p83 allow (R9-47).
    return f"""<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>NestedConstrained</Identifier>
    <DisplayName>Nested Constrained</DisplayName>
    <Description>Feature with a Constrained type based on a Constrained type</Description>
    <Property>
        <Identifier>Value</Identifier>
        <DisplayName>Value</DisplayName>
        <Description>A doubly constrained value</Description>
        <Observable>No</Observable>
        <DataType>
            <Constrained>
                <DataType>
                    <Constrained>
                        <DataType>
                            <Basic>{inner_base}</Basic>
                        </DataType>
                        <Constraints>{inner_constraints}</Constraints>
                    </Constrained>
                </DataType>
                <Constraints>{outer_constraints}</Constraints>
            </Constrained>
        </DataType>
    </Property>
</Feature>
"""


def test_nested_constrained_accepted(xsd_path: Path, tmp_path: Path) -> None:
    # Part A p66 / Part B p83 (R9-47, owner ruling 2026-09-04): the overlay
    # XSLT accepts Constrained-on-Constrained when both layers fit the
    # ultimate base type.
    fdl_path = tmp_path / "NestedConstrained.sila.xml"
    fdl_path.write_text(
        _nested_constrained_fdl("String", "<MaximalLength>10</MaximalLength>", "<Pattern>[A-Z]+</Pattern>"),
        encoding="utf-8",
    )

    parse_fdl(fdl_path, xsd_path)


def test_nested_constrained_outer_constraint_checked_against_ultimate_base(xsd_path: Path, tmp_path: Path) -> None:
    # The outer layer is still checked against the type it finally applies
    # to: Pattern is String-only, so Pattern over Constrained(Integer) is
    # rejected (this is also why the corpus fixture ConstrainedConstrained
    # stays under invalid-fdl/ -- it is invalid for this reason, not for the
    # nesting).
    fdl_path = tmp_path / "NestedConstrained.sila.xml"
    fdl_path.write_text(
        _nested_constrained_fdl("Integer", "<MaximalExclusive>100</MaximalExclusive>", "<Pattern>ABC</Pattern>"),
        encoding="utf-8",
    )

    with pytest.raises(FdlError, match="Invalid constraint on type Integer"):
        parse_fdl(fdl_path, xsd_path)


CROSS_KIND_DUPLICATE_FDL = """<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>CrossKindDuplicate</Identifier>
    <DisplayName>Cross Kind Duplicate</DisplayName>
    <Description>Feature with a shared name across different identifier kinds</Description>
    <DataTypeDefinition>
        <Identifier>Shared</Identifier>
        <DisplayName>Shared</DisplayName>
        <Description>A data type definition named Shared</Description>
        <DataType>
            <Basic>String</Basic>
        </DataType>
    </DataTypeDefinition>
    <Property>
        <Identifier>Shared</Identifier>
        <DisplayName>Shared</DisplayName>
        <Description>A property also named Shared</Description>
        <Observable>No</Observable>
        <DataType>
            <Basic>String</Basic>
        </DataType>
    </Property>
</Feature>
"""


def test_duplicate_identifier_cross_kind_allowed(xsd_path: Path, tmp_path: Path) -> None:
    fdl_path = tmp_path / "CrossKindDuplicate.sila.xml"
    fdl_path.write_text(CROSS_KIND_DUPLICATE_FDL, encoding="utf-8")

    feature = parse_fdl(fdl_path, xsd_path)

    assert feature.data_type_definition[0].identifier == "Shared"
    assert feature.property[0].identifier == "Shared"


SELF_REFERENCING_DATA_TYPE_FDL = """<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>SelfReferencingDataType</Identifier>
    <DisplayName>Self Referencing Data Type</DisplayName>
    <Description>Feature with a recursive data type definition</Description>
    <DataTypeDefinition>
        <Identifier>TreeNode</Identifier>
        <DisplayName>Tree Node</DisplayName>
        <Description>A recursive tree node</Description>
        <DataType>
            <Structure>
                <Element>
                    <Identifier>Value</Identifier>
                    <DisplayName>Value</DisplayName>
                    <Description>Node value</Description>
                    <DataType><Basic>String</Basic></DataType>
                </Element>
                <Element>
                    <Identifier>Children</Identifier>
                    <DisplayName>Children</DisplayName>
                    <Description>Child nodes</Description>
                    <DataType>
                        <List>
                            <DataType>
                                <DataTypeIdentifier>TreeNode</DataTypeIdentifier>
                            </DataType>
                        </List>
                    </DataType>
                </Element>
            </Structure>
        </DataType>
    </DataTypeDefinition>
</Feature>
"""


def test_self_referencing_data_type_allowed(xsd_path: Path, tmp_path: Path) -> None:
    fdl_path = tmp_path / "SelfReferencingDataType.sila.xml"
    fdl_path.write_text(SELF_REFERENCING_DATA_TYPE_FDL, encoding="utf-8")

    feature = parse_fdl(fdl_path, xsd_path)

    assert feature.data_type_definition[0].identifier == "TreeNode"


MULTI_NODE_CYCLE_FDL = """<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>MultiNodeCycle</Identifier>
    <DisplayName>Multi Node Cycle</DisplayName>
    <Description>Feature with a two-node cyclic data type reference</Description>
    <DataTypeDefinition>
        <Identifier>TypeA</Identifier>
        <DisplayName>Type A</DisplayName>
        <Description>References TypeB</Description>
        <DataType><DataTypeIdentifier>TypeB</DataTypeIdentifier></DataType>
    </DataTypeDefinition>
    <DataTypeDefinition>
        <Identifier>TypeB</Identifier>
        <DisplayName>Type B</DisplayName>
        <Description>References TypeA</Description>
        <DataType><DataTypeIdentifier>TypeA</DataTypeIdentifier></DataType>
    </DataTypeDefinition>
</Feature>
"""


def test_multi_node_cycle_rejected(xsd_path: Path, tmp_path: Path) -> None:
    fdl_path = tmp_path / "MultiNodeCycle.sila.xml"
    fdl_path.write_text(MULTI_NODE_CYCLE_FDL, encoding="utf-8")

    with pytest.raises(FdlError):
        parse_fdl(fdl_path, xsd_path)


CASE_INSENSITIVE_DUPLICATE_COMMAND_FDL = """<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>CaseInsensitiveDuplicateCommand</Identifier>
    <DisplayName>Case Insensitive Duplicate Command</DisplayName>
    <Description>Feature with two commands whose identifiers differ only by case</Description>
    <Command>
        <Identifier>TestCommand</Identifier>
        <DisplayName>Test Command</DisplayName>
        <Description>First command</Description>
        <Observable>No</Observable>
    </Command>
    <Command>
        <Identifier>testcommand</Identifier>
        <DisplayName>test command</DisplayName>
        <Description>Second command, same identifier ignoring case</Description>
        <Observable>No</Observable>
    </Command>
</Feature>
"""


def test_case_insensitive_duplicate_command_rejected(xsd_path: Path, tmp_path: Path) -> None:
    # SiLA 2 Part B p80: Command Response Identifier uniqueness MUST be
    # checked without regard to case; 'TestCommand' and 'testcommand' collide.
    fdl_path = tmp_path / "CaseInsensitiveDuplicateCommand.sila.xml"
    fdl_path.write_text(CASE_INSENSITIVE_DUPLICATE_COMMAND_FDL, encoding="utf-8")

    with pytest.raises(FdlError):
        parse_fdl(fdl_path, xsd_path)


DUPLICATE_STRUCTURE_ELEMENT_FDL = """<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>DuplicateStructureElement</Identifier>
    <DisplayName>Duplicate Structure Element</DisplayName>
    <Description>Feature with a Structure whose element identifiers collide by case</Description>
    <DataTypeDefinition>
        <Identifier>Person</Identifier>
        <DisplayName>Person</DisplayName>
        <Description>A structure with duplicate element identifiers</Description>
        <DataType>
            <Structure>
                <Element>
                    <Identifier>Name</Identifier>
                    <DisplayName>Name</DisplayName>
                    <Description>Given name</Description>
                    <DataType><Basic>String</Basic></DataType>
                </Element>
                <Element>
                    <Identifier>name</Identifier>
                    <DisplayName>name</DisplayName>
                    <Description>Same identifier ignoring case</Description>
                    <DataType><Basic>String</Basic></DataType>
                </Element>
            </Structure>
        </DataType>
    </DataTypeDefinition>
</Feature>
"""


def test_duplicate_structure_element_rejected(xsd_path: Path, tmp_path: Path) -> None:
    # SiLA 2 Part A p66: Structure Element Identifier uniqueness MUST be
    # checked without regard to case; 'Name' and 'name' collide.
    fdl_path = tmp_path / "DuplicateStructureElement.sila.xml"
    fdl_path.write_text(DUPLICATE_STRUCTURE_ELEMENT_FDL, encoding="utf-8")

    with pytest.raises(FdlError):
        parse_fdl(fdl_path, xsd_path)


def _unit_factor_offset_fdl(factor: str, offset: str) -> str:
    # Mirrors the Unit constraint shape in
    # tests/examples/fdl/sila_base/AbsorbanceReaderService-v1_0.sila.xml.
    return f"""<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>UnitFactor</Identifier>
    <DisplayName>Unit Factor</DisplayName>
    <Description>Feature with a Unit constraint exercising Factor/Offset</Description>
    <Property>
        <Identifier>Measurement</Identifier>
        <DisplayName>Measurement</DisplayName>
        <Description>A constrained real with a Unit</Description>
        <Observable>No</Observable>
        <DataType>
            <Constrained>
                <DataType>
                    <Basic>Real</Basic>
                </DataType>
                <Constraints>
                    <Unit>
                        <Label>testunit</Label>
                        <Factor>{factor}</Factor>
                        <Offset>{offset}</Offset>
                        <UnitComponent>
                            <SIUnit>Meter</SIUnit>
                            <Exponent>1</Exponent>
                        </UnitComponent>
                    </Unit>
                </Constraints>
            </Constrained>
        </DataType>
    </Property>
</Feature>
"""


def _element_count_list_fdl(constraints_xml: str) -> str:
    # A List<Boolean> property carrying an element-count constraint; mirrors the
    # Constrained-List shape in tests/examples/fdl/panda/RobotController.sila.xml.
    return f"""<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>ElementCountList</Identifier>
    <DisplayName>Element Count List</DisplayName>
    <Description>Feature with a List constrained by element count</Description>
    <Property>
        <Identifier>Items</Identifier>
        <DisplayName>Items</DisplayName>
        <Description>A list constrained by element count</Description>
        <Observable>No</Observable>
        <DataType>
            <Constrained>
                <DataType>
                    <List>
                        <DataType>
                            <Basic>Boolean</Basic>
                        </DataType>
                    </List>
                </DataType>
                <Constraints>{constraints_xml}</Constraints>
            </Constrained>
        </DataType>
    </Property>
</Feature>
"""


def test_element_count_zero_accepted(xsd_path: Path, tmp_path: Path) -> None:
    # Part B p85: ElementCount MUST accept 0 (e.g. a list forced to stay empty).
    fdl_path = tmp_path / "ElementCountList.sila.xml"
    fdl_path.write_text(_element_count_list_fdl("<ElementCount>0</ElementCount>"), encoding="utf-8")

    parse_fdl(fdl_path, xsd_path)


def test_maximal_element_count_zero_accepted(xsd_path: Path, tmp_path: Path) -> None:
    # Part B p85: MaximalElementCount MUST accept 0 the same way as ElementCount.
    fdl_path = tmp_path / "ElementCountList.sila.xml"
    fdl_path.write_text(
        _element_count_list_fdl("<MaximalElementCount>0</MaximalElementCount>"), encoding="utf-8"
    )

    parse_fdl(fdl_path, xsd_path)


def test_element_count_negative_rejected(xsd_path: Path, tmp_path: Path) -> None:
    # xs:nonNegativeInteger still rejects a negative bound.
    fdl_path = tmp_path / "ElementCountListInvalid.sila.xml"
    fdl_path.write_text(_element_count_list_fdl("<ElementCount>-1</ElementCount>"), encoding="utf-8")

    with pytest.raises(FdlError):
        parse_fdl(fdl_path, xsd_path)


def _minimal_length_string_fdl(constraints_xml: str) -> str:
    # A String property carrying a MinimalLength constraint; mirrors the
    # _unit_factor_offset_fdl skeleton, swapping Real->String and the Unit
    # block for the constraints under test.
    return f"""<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>MinimalLengthString</Identifier>
    <DisplayName>Minimal Length String</DisplayName>
    <Description>Feature with a String constrained by MinimalLength</Description>
    <Property>
        <Identifier>Text</Identifier>
        <DisplayName>Text</DisplayName>
        <Description>A string constrained by minimal length</Description>
        <Observable>No</Observable>
        <DataType>
            <Constrained>
                <DataType>
                    <Basic>String</Basic>
                </DataType>
                <Constraints>{constraints_xml}</Constraints>
            </Constrained>
        </DataType>
    </Property>
</Feature>
"""


def test_minimal_length_zero_accepted(xsd_path: Path, tmp_path: Path) -> None:
    # Part A p67 / Part B p83 (R9-6): MinimalLength MUST accept 0.
    fdl_path = tmp_path / "MinimalLengthString.sila.xml"
    fdl_path.write_text(_minimal_length_string_fdl("<MinimalLength>0</MinimalLength>"), encoding="utf-8")

    parse_fdl(fdl_path, xsd_path)


def test_minimal_length_negative_rejected(xsd_path: Path, tmp_path: Path) -> None:
    # xs:nonNegativeInteger still rejects a negative bound.
    fdl_path = tmp_path / "MinimalLengthStringInvalid.sila.xml"
    fdl_path.write_text(_minimal_length_string_fdl("<MinimalLength>-1</MinimalLength>"), encoding="utf-8")

    with pytest.raises(FdlError):
        parse_fdl(fdl_path, xsd_path)


def test_unit_factor_offset_accept_ieee754_double_literals(xsd_path: Path, tmp_path: Path) -> None:
    # Part B p84-85: Factor/Offset are IEEE 754 doubles. 6.022e23 and -INF are
    # valid xs:double literals but not valid xs:decimal ones -- this is what
    # the src/schema/Constraints.xsd overlay (R2-9) exists to accept.
    # libxml2 is XSD 1.0: only INF/-INF/NaN are recognized special values, so
    # this uses -INF, never +INF (which XSD 1.0 rejects).
    fdl_path = tmp_path / "UnitFactor.sila.xml"
    fdl_path.write_text(_unit_factor_offset_fdl("6.022e23", "-INF"), encoding="utf-8")

    parse_fdl(fdl_path, xsd_path)


def test_unit_factor_non_numeric_rejected(xsd_path: Path, tmp_path: Path) -> None:
    # xs:double still rejects non-numeric lexical values.
    fdl_path = tmp_path / "UnitFactorInvalid.sila.xml"
    fdl_path.write_text(_unit_factor_offset_fdl("abc", "0"), encoding="utf-8")

    with pytest.raises(FdlError):
        parse_fdl(fdl_path, xsd_path)


def test_constraints_overlay_retypes_only_documented_lines() -> None:
    # Guards the vendored overlay against silent drift on a submodule bump:
    # the overlay must differ from the submodule copy only by the inserted
    # header comment plus six 'replace' opcodes -- two decimal->double
    # (Factor/Offset, R2-9) and four positiveInteger->nonNegativeInteger
    # (the ElementCount trio, R2-7, plus MinimalLength, R9-6).
    base_lines = SILA_BASE_CONSTRAINTS.read_text().splitlines()
    overlay_lines = OVERLAY_CONSTRAINTS.read_text().splitlines()

    matcher = difflib.SequenceMatcher(a=base_lines, b=overlay_lines)
    replace_pairs: list[tuple[str, str]] = []
    for tag, a_lo, a_hi, b_lo, b_hi in matcher.get_opcodes():
        if tag == "equal":
            continue
        assert tag != "delete", f"overlay dropped submodule line(s): {base_lines[a_lo:a_hi]}"
        if tag == "insert":
            for line in overlay_lines[b_lo:b_hi]:
                assert "<xs:" not in line, f"overlay inserted an XSD element line: {line!r}"
        elif tag == "replace":
            for before, after in zip(base_lines[a_lo:a_hi], overlay_lines[b_lo:b_hi]):
                replace_pairs.append((before, after))

    assert len(replace_pairs) == 6
    # Classify by content, not opcode order, so this is robust to how
    # difflib.SequenceMatcher groups the six retyped lines.
    retyped_names: set[str] = set()
    for before, after in replace_pairs:
        if "xs:decimal" in before:
            # Part B p84-85 (R2-9): Conversion Factor/Offset are IEEE 754 doubles.
            assert "xs:double" in after, (before, after)
            for name in ("Factor", "Offset"):
                if f'name="{name}"' in before:
                    retyped_names.add(name)
        elif "xs:positiveInteger" in before:
            # Part B p85 (R2-7): the ElementCount trio MUST allow a 0 bound.
            # Part A p67 / Part B p83 (R9-6): MinimalLength MUST allow a 0 bound.
            assert "xs:nonNegativeInteger" in after, (before, after)
            for name in ("ElementCount", "MinimalElementCount", "MaximalElementCount", "MinimalLength"):
                if f'name="{name}"' in before:
                    retyped_names.add(name)
        else:
            raise AssertionError(f"unexpected overlay retype: {before!r} -> {after!r}")
    assert retyped_names == {
        "Factor", "Offset",
        "ElementCount", "MinimalElementCount", "MaximalElementCount",
        "MinimalLength",
    }


def test_validation_xslt_overlay_differs_only_on_documented_lines() -> None:
    # Part A p66 / Part B p83 (R9-47) and owner ruling 2026-09-04: the overlay
    # is a verbatim copy of the submodule stylesheet except for (1) the header
    # comment, (2) the one 3-line xsl:when block (detect-invalid-constraint-
    # base-type) that wrongly rejected a Constrained type based on another
    # Constrained type, (3) the $base variable that resolves the ultimate base
    # type through nested Constrained layers and (4) the constraint-
    # applicability arms of detect-invalid-constraint testing $base instead of
    # the immediate sila:DataType child.
    base_lines = SILA_BASE_XSLT.read_text().splitlines()
    overlay_lines = OVERLAY_XSLT.read_text().splitlines()

    matcher = difflib.SequenceMatcher(a=base_lines, b=overlay_lines)
    deleted_runs: list[list[str]] = []
    replaced_arms = 0
    for tag, a_lo, a_hi, b_lo, b_hi in matcher.get_opcodes():
        if tag == "equal":
            continue
        if tag == "insert":
            for line in overlay_lines[b_lo:b_hi]:
                assert "<xsl:" not in line or '<xsl:variable name="base"' in line, (
                    f"overlay inserted an XSLT element line: {line!r}"
                )
        elif tag == "delete":
            deleted_runs.append(base_lines[a_lo:a_hi])
        else:
            assert a_hi - a_lo == b_hi - b_lo, (base_lines[a_lo:a_hi], overlay_lines[b_lo:b_hi])
            for before, after in zip(base_lines[a_lo:a_hi], overlay_lines[b_lo:b_hi]):
                assert "sila:DataType/sila:Basic/text()" in before or "sila:DataType/sila:List" in before, before
                assert after == before.replace("sila:DataType/sila:Basic/text()", "$base/self::sila:Basic/text()").replace(
                    'test="sila:DataType/sila:List"', 'test="$base/self::sila:List"'
                ), (before, after)
                replaced_arms += 1

    assert replaced_arms == 9, f"expected the eight Basic arms plus the List arm, got {replaced_arms}"
    assert len(deleted_runs) == 1, f"expected exactly one deleted run, got: {deleted_runs}"
    deleted_run = deleted_runs[0]
    assert len(deleted_run) == 3, f"expected a 3-line xsl:when delete, got {len(deleted_run)} lines"
    assert "Constrained constrained types are not allowed" in "\n".join(deleted_run)
