"""Entry point: python -m codegen path/to/feature.sila.xml -o generated/"""
import argparse
import sys
from pathlib import Path

from fdl_parser import parse_fdl
from meta_emitter import emit_meta_header, emit_meta_source, emit_service_adapter
from proto_emitter import emit_proto
from schema_provisioning import provision_schemas

_XSD_RELATIVE_PATH = Path("third_party/sila_base/schema/FeatureDefinition.xsd")


def _find_xsd() -> Path:
    """Walk upward from this file looking for the sila_base XSD."""
    for directory in Path(__file__).resolve().parents:
        candidate = directory / _XSD_RELATIVE_PATH
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(
        f"Could not locate {_XSD_RELATIVE_PATH} by walking up from {__file__}. "
        "Pass --xsd explicitly."
    )


def _parse_schema_sources(raw: list[str]) -> dict[str, Path]:
    """Parse repeated --schema-source URL=PATH into {url: path}."""
    overrides: dict[str, Path] = {}
    for entry in raw:
        url, sep, path = entry.partition("=")
        if not sep:
            print(f"error: --schema-source must be URL=PATH, got: {entry}", file=sys.stderr)
            raise SystemExit(1)
        overrides[url] = Path(path)
    return overrides


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate .proto files from SiLA2 Feature Definition (FDL) files."
    )
    parser.add_argument(
        "fdl_files", nargs="+", type=Path, help="Path(s) to .sila.xml feature definition files"
    )
    parser.add_argument(
        "-o", "--output-dir", type=Path, default=Path("generated"), help="Output directory"
    )
    parser.add_argument(
        "--xsd", type=Path, default=None, help="Path to FeatureDefinition.xsd (auto-detected if omitted)"
    )
    parser.add_argument(
        "--schema-source",
        action="append",
        default=[],
        metavar="URL=PATH",
        help=(
            "Provide a Schema Url content from a local file (repeatable); "
            "hermetic override so builds/tests need no network."
        ),
    )
    args = parser.parse_args()
    schema_overrides = _parse_schema_sources(args.schema_source)

    try:
        xsd_path = args.xsd if args.xsd is not None else _find_xsd()
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc

    proto_dir = args.output_dir / "proto"
    proto_dir.mkdir(parents=True, exist_ok=True)
    meta_dir = args.output_dir / "meta"
    meta_dir.mkdir(parents=True, exist_ok=True)

    for fdl_path in args.fdl_files:
        feature = parse_fdl(fdl_path, xsd_path)
        xml_text = fdl_path.read_text()

        try:
            provisioned = provision_schemas(feature, schema_overrides)
        except RuntimeError as exc:
            print(f"error: {exc}", file=sys.stderr)
            raise SystemExit(1) from exc

        proto_text = emit_proto(feature)
        out_path = proto_dir / f"{feature.identifier}.proto"
        out_path.write_text(proto_text)
        print(out_path)

        adapter_text = emit_service_adapter(feature)
        adapter_path = proto_dir / f"{feature.identifier}ServiceAdapter.h"
        adapter_path.write_text(adapter_text)
        print(adapter_path)

        meta_h_text = emit_meta_header(feature, xml_text, provisioned)
        meta_h_path = meta_dir / f"{feature.identifier}Meta.h"
        meta_h_path.write_text(meta_h_text)
        print(meta_h_path)

        meta_cc_text = emit_meta_source(feature, xml_text, provisioned)
        meta_cc_path = meta_dir / f"{feature.identifier}Meta.cc"
        meta_cc_path.write_text(meta_cc_text)
        print(meta_cc_path)


if __name__ == "__main__":
    main()
