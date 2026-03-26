#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent.parent.parent.parent
EXAMPLE_ROOT = ROOT / "examples" / "stm32f446"
DEFAULT_MANIFEST = EXAMPLE_ROOT / "configs" / "bringup" / "manifest.yaml"
DEFAULT_HEADER = EXAMPLE_ROOT / "include" / "bringup_manifest_gen.h"
DEFAULT_MARKDOWN = ROOT / "docs" / "generated" / "bringup_manifest.md"

PHASE_ENUMS = [
    "BRINGUP_PHASE_ROM_EARLY_INIT",
    "BRINGUP_PHASE_MPU_SETUP",
    "BRINGUP_PHASE_KERNEL_START",
    "BRINGUP_PHASE_DRIVER_PROBE_DIAG",
    "BRINGUP_PHASE_DRIVER_PROBE_UART",
    "BRINGUP_PHASE_DRIVER_PROBE_SENSOR",
    "BRINGUP_PHASE_DRIVER_PROBE_VM",
    "BRINGUP_PHASE_SERVICE_REGISTRATION",
    "BRINGUP_PHASE_USER_WORKLOAD_ENABLE",
]

STAGE_ENUMS = [
    "BRINGUP_STAGE_INIT",
    "BRINGUP_STAGE_MPU",
    "BRINGUP_STAGE_KERNEL",
    "BRINGUP_STAGE_DRIVERS",
    "BRINGUP_STAGE_READY",
]

DRIVER_ENUMS = {
    "KDI_DRIVER_KERNEL",
    "KDI_DRIVER_UART",
    "KDI_DRIVER_SENSOR",
    "KDI_DRIVER_VM_RUNTIME",
    "KDI_DRIVER_DIAG",
}

RESOURCE_ENUMS = {
    "DEP_RESOURCE_IRQ",
    "DEP_RESOURCE_DMA",
    "DEP_RESOURCE_MEMORY",
}


def unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
        return value[1:-1]
    return value


def parse_inline_list(value: str) -> list[str]:
    value = value.strip()
    if value == "[]":
        return []
    if not (value.startswith("[") and value.endswith("]")):
        raise ValueError(f"expected inline list, got: {value}")
    inner = value[1:-1].strip()
    if inner == "":
        return []
    return [unquote(item.strip()) for item in inner.split(",")]


def parse_scalar(value: str) -> Any:
    value = value.strip()
    if value.startswith("["):
        return parse_inline_list(value)
    return unquote(value)


def parse_yaml_mapping(line: str) -> tuple[str, Any]:
    if ":" not in line:
        raise ValueError(f"expected key:value line, got: {line}")
    key, raw_value = line.split(":", 1)
    key = key.strip()
    if key == "":
        raise ValueError(f"expected non-empty key, got: {line}")
    return key, parse_scalar(raw_value)


def load_simple_yaml(text: str, path: Path) -> dict[str, Any]:
    out: dict[str, Any] = {}
    current_section: str | None = None
    current_item: dict[str, Any] | None = None

    def flush_item() -> None:
        nonlocal current_item
        if current_section is None or current_item is None:
            return
        out[current_section].append(current_item)
        current_item = None

    for lineno, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.rstrip()
        stripped = line.strip()
        if stripped == "" or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip(" "))
        if indent == 0:
            flush_item()
            if not stripped.endswith(":"):
                raise ValueError(f"{path}:{lineno}: expected top-level section ending with ':'")
            current_section = stripped[:-1]
            out[current_section] = []
            continue
        if indent == 2 and stripped.startswith("- "):
            if current_section is None:
                raise ValueError(f"{path}:{lineno}: list item outside section")
            flush_item()
            current_item = {}
            tail = stripped[2:].strip()
            if tail != "":
                key, value = parse_yaml_mapping(tail)
                current_item[key] = value
            continue
        if indent == 4:
            if current_section is None or current_item is None:
                raise ValueError(f"{path}:{lineno}: field outside list item")
            key, value = parse_yaml_mapping(stripped)
            current_item[key] = value
            continue
        raise ValueError(f"{path}:{lineno}: unsupported indentation layout")

    flush_item()
    return out


def load_manifest(path: Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    try:
        obj = json.loads(text)
    except json.JSONDecodeError:
        obj = load_simple_yaml(text, path)
    if not isinstance(obj, dict):
        raise ValueError(f"manifest root must be an object: {path}")
    return obj


def expect_list(obj: dict[str, Any], key: str) -> list[Any]:
    value = obj.get(key)
    if not isinstance(value, list):
        raise ValueError(f"manifest key {key!r} must be a list")
    return value


def expect_str(obj: dict[str, Any], key: str) -> str:
    value = obj.get(key)
    if not isinstance(value, str) or value == "":
        raise ValueError(f"manifest key {key!r} must be a non-empty string")
    return value


def expect_str_list(obj: dict[str, Any], key: str) -> list[str]:
    value = expect_list(obj, key)
    out: list[str] = []
    for item in value:
        if not isinstance(item, str) or item == "":
            raise ValueError(f"manifest key {key!r} must contain only non-empty strings")
        out.append(item)
    return out


def ensure_unique(items: list[str], label: str) -> None:
    seen: set[str] = set()
    for item in items:
        if item in seen:
            raise ValueError(f"duplicate {label}: {item}")
        seen.add(item)


def validate_manifest(raw: dict[str, Any]) -> dict[str, Any]:
    phase_rows = expect_list(raw, "phases")
    stage_rows = expect_list(raw, "stages")
    driver_edges = expect_list(raw, "driver_edges")
    resource_edges = expect_list(raw, "resource_edges")
    stage_driver_edges = expect_list(raw, "stage_driver_edges")

    if len(phase_rows) != len(PHASE_ENUMS):
        raise ValueError(f"expected {len(PHASE_ENUMS)} phases, found {len(phase_rows)}")
    if len(stage_rows) != len(STAGE_ENUMS):
        raise ValueError(f"expected {len(STAGE_ENUMS)} stages, found {len(stage_rows)}")

    phases: list[dict[str, Any]] = []
    phase_names: list[str] = []
    phase_aliases: list[str] = []
    for idx, row in enumerate(phase_rows):
        if not isinstance(row, dict):
            raise ValueError("each phase entry must be an object")
        enum_name = expect_str(row, "enum")
        if enum_name != PHASE_ENUMS[idx]:
            raise ValueError(f"phase index {idx} must be {PHASE_ENUMS[idx]}, got {enum_name}")
        phase = {
            "enum": enum_name,
            "name": expect_str(row, "name"),
            "stage": expect_str(row, "stage"),
            "aliases": expect_str_list(row, "aliases"),
        }
        if phase["stage"] not in STAGE_ENUMS:
            raise ValueError(f"phase {enum_name} references unknown stage {phase['stage']}")
        phases.append(phase)
        phase_names.append(phase["name"])
        phase_aliases.extend(phase["aliases"])

    ensure_unique(phase_names, "phase name")
    ensure_unique(phase_aliases, "phase alias")
    if set(phase_names) & set(phase_aliases):
        overlap = sorted(set(phase_names) & set(phase_aliases))
        raise ValueError(f"phase aliases overlap exact names: {', '.join(overlap)}")

    stages: list[dict[str, Any]] = []
    stage_names: list[str] = []
    stage_aliases: list[str] = []
    phase_index = {name: idx for idx, name in enumerate(PHASE_ENUMS)}
    stage_index = {name: idx for idx, name in enumerate(STAGE_ENUMS)}
    for idx, row in enumerate(stage_rows):
        if not isinstance(row, dict):
            raise ValueError("each stage entry must be an object")
        enum_name = expect_str(row, "enum")
        if enum_name != STAGE_ENUMS[idx]:
            raise ValueError(f"stage index {idx} must be {STAGE_ENUMS[idx]}, got {enum_name}")
        phase_begin = expect_str(row, "phase_begin")
        phase_end = expect_str(row, "phase_end")
        if phase_begin not in phase_index or phase_end not in phase_index:
            raise ValueError(f"stage {enum_name} references unknown phase range")
        if phase_index[phase_begin] > phase_index[phase_end]:
            raise ValueError(f"stage {enum_name} phase range is reversed")
        stage = {
            "enum": enum_name,
            "name": expect_str(row, "name"),
            "phase_begin": phase_begin,
            "phase_end": phase_end,
            "entry_event": expect_str(row, "entry_event"),
            "exit_event": expect_str(row, "exit_event"),
            "aliases": expect_str_list(row, "aliases"),
        }
        stages.append(stage)
        stage_names.append(stage["name"])
        stage_aliases.extend(stage["aliases"])

    ensure_unique(stage_names, "stage name")
    ensure_unique(stage_aliases, "stage alias")
    if set(stage_names) & set(stage_aliases):
        overlap = sorted(set(stage_names) & set(stage_aliases))
        raise ValueError(f"stage aliases overlap exact names: {', '.join(overlap)}")

    covered_phases: set[str] = set()
    for stage in stages:
        begin = phase_index[stage["phase_begin"]]
        end = phase_index[stage["phase_end"]]
        for idx in range(begin, end + 1):
            phase_enum = PHASE_ENUMS[idx]
            covered_phases.add(phase_enum)
            if phases[idx]["stage"] != stage["enum"]:
                raise ValueError(
                    f"phase {phase_enum} declares stage {phases[idx]['stage']} outside stage range {stage['enum']}"
                )
    if covered_phases != set(PHASE_ENUMS):
        raise ValueError("stage ranges must cover all phases exactly")

    out_driver_edges: list[dict[str, str]] = []
    for row in driver_edges:
        if not isinstance(row, dict):
            raise ValueError("driver_edges entries must be objects")
        src = expect_str(row, "from")
        dst = expect_str(row, "to")
        if src not in DRIVER_ENUMS or dst not in DRIVER_ENUMS:
            raise ValueError(f"driver edge references unknown driver: {src} -> {dst}")
        out_driver_edges.append(
            {
                "from": src,
                "to": dst,
                "reason": expect_str(row, "reason"),
            }
        )

    out_resource_edges: list[dict[str, str]] = []
    for row in resource_edges:
        if not isinstance(row, dict):
            raise ValueError("resource_edges entries must be objects")
        driver = expect_str(row, "driver")
        kind = expect_str(row, "kind")
        if driver not in DRIVER_ENUMS:
            raise ValueError(f"resource edge references unknown driver: {driver}")
        if kind not in RESOURCE_ENUMS:
            raise ValueError(f"resource edge references unknown resource kind: {kind}")
        out_resource_edges.append(
            {
                "driver": driver,
                "kind": kind,
                "resource_id": expect_str(row, "resource_id"),
                "reason": expect_str(row, "reason"),
            }
        )

    out_stage_driver_edges: list[dict[str, str]] = []
    for row in stage_driver_edges:
        if not isinstance(row, dict):
            raise ValueError("stage_driver_edges entries must be objects")
        phase = expect_str(row, "phase")
        driver = expect_str(row, "driver")
        if phase not in phase_index:
            raise ValueError(f"stage_driver_edges references unknown phase: {phase}")
        if driver not in DRIVER_ENUMS:
            raise ValueError(f"stage_driver_edges references unknown driver: {driver}")
        out_stage_driver_edges.append(
            {
                "phase": phase,
                "driver": driver,
                "reason": expect_str(row, "reason"),
            }
        )

    return {
        "phases": phases,
        "stages": stages,
        "driver_edges": out_driver_edges,
        "resource_edges": out_resource_edges,
        "stage_driver_edges": out_stage_driver_edges,
    }


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def render_header(manifest: dict[str, Any]) -> str:
    phase_defs = [
        f"  [{phase['enum']}] = {{ {c_string(phase['name'])}, {phase['stage']} }},"
        for phase in manifest["phases"]
    ]
    phase_alias_defs = [
        f"  {{ {c_string(alias)}, {phase['enum']} }},"
        for phase in manifest["phases"]
        for alias in phase["aliases"]
    ]
    stage_defs = [
        "  [{enum}] = {{ {name}, {entry}, {exit}, {begin}, {end} }},".format(
            enum=stage["enum"],
            name=c_string(stage["name"]),
            entry=c_string(stage["entry_event"]),
            exit=c_string(stage["exit_event"]),
            begin=stage["phase_begin"],
            end=stage["phase_end"],
        )
        for stage in manifest["stages"]
    ]
    stage_alias_defs = [
        f"  {{ {c_string(alias)}, {stage['enum']} }},"
        for stage in manifest["stages"]
        for alias in stage["aliases"]
    ]
    driver_edges = [
        "  {{ {from_driver}, {to_driver}, {reason} }},".format(
            from_driver=edge["from"],
            to_driver=edge["to"],
            reason=c_string(edge["reason"]),
        )
        for edge in manifest["driver_edges"]
    ]
    resource_edges = [
        "  {{ {driver}, {kind}, {resource_id}, {reason} }},".format(
            driver=edge["driver"],
            kind=edge["kind"],
            resource_id=c_string(edge["resource_id"]),
            reason=c_string(edge["reason"]),
        )
        for edge in manifest["resource_edges"]
    ]
    stage_driver_edges = [
        "  {{ {phase}, {driver}, {reason} }},".format(
            phase=edge["phase"],
            driver=edge["driver"],
            reason=c_string(edge["reason"]),
        )
        for edge in manifest["stage_driver_edges"]
    ]

    lines = [
        "#ifndef BRINGUP_MANIFEST_GEN_H",
        "#define BRINGUP_MANIFEST_GEN_H",
        "",
        "/* Generated by tools/bringup_compile.py. Do not edit manually. */",
        "",
        "#ifdef BRINGUP_MANIFEST_INCLUDE_PHASE_SECTION",
        "",
        "typedef struct {",
        "  const char *name;",
        "  BringupStageId stage;",
        "} BringupManifestPhaseDef;",
        "",
        "typedef struct {",
        "  const char *alias;",
        "  BringupPhaseId phase;",
        "} BringupManifestPhaseAliasDef;",
        "",
        "typedef struct {",
        "  const char *name;",
        "  const char *entry_event;",
        "  const char *exit_event;",
        "  BringupPhaseId phase_begin;",
        "  BringupPhaseId phase_end;",
        "} BringupManifestStageDef;",
        "",
        "typedef struct {",
        "  const char *alias;",
        "  BringupStageId stage;",
        "} BringupManifestStageAliasDef;",
        "",
        f"#define BRINGUP_MANIFEST_PHASE_ALIAS_COUNT {len(phase_alias_defs)}U",
        f"#define BRINGUP_MANIFEST_STAGE_ALIAS_COUNT {len(stage_alias_defs)}U",
        "",
        "static const BringupManifestPhaseDef bringup_manifest_phase_defs[BRINGUP_PHASE_COUNT] = {",
        *phase_defs,
        "};",
        "",
        "static const BringupManifestPhaseAliasDef bringup_manifest_phase_alias_defs[BRINGUP_MANIFEST_PHASE_ALIAS_COUNT] = {",
        *phase_alias_defs,
        "};",
        "",
        "static const BringupManifestStageDef bringup_manifest_stage_defs[BRINGUP_STAGE_COUNT] = {",
        *stage_defs,
        "};",
        "",
        "static const BringupManifestStageAliasDef bringup_manifest_stage_alias_defs[BRINGUP_MANIFEST_STAGE_ALIAS_COUNT] = {",
        *stage_alias_defs,
        "};",
        "",
        "#endif",
        "",
        "#ifdef BRINGUP_MANIFEST_INCLUDE_DEPENDENCY_SECTION",
        "",
        f"#define BRINGUP_MANIFEST_DEP_DRIVER_EDGE_COUNT {len(driver_edges)}U",
        f"#define BRINGUP_MANIFEST_DEP_RESOURCE_EDGE_COUNT {len(resource_edges)}U",
        f"#define BRINGUP_MANIFEST_DEP_STAGE_DRIVER_EDGE_COUNT {len(stage_driver_edges)}U",
        "",
        "static const DepDriverEdge bringup_manifest_dep_driver_edges[BRINGUP_MANIFEST_DEP_DRIVER_EDGE_COUNT] = {",
        *driver_edges,
        "};",
        "",
        "static const DepResourceEdge bringup_manifest_dep_resource_edges[BRINGUP_MANIFEST_DEP_RESOURCE_EDGE_COUNT] = {",
        *resource_edges,
        "};",
        "",
        "static const DepStageDriverEdge bringup_manifest_dep_stage_driver_edges[BRINGUP_MANIFEST_DEP_STAGE_DRIVER_EDGE_COUNT] = {",
        *stage_driver_edges,
        "};",
        "",
        "#endif",
        "",
        "#endif",
        "",
    ]
    return "\n".join(lines)


def markdown_table(headers: list[str], rows: list[list[str]]) -> list[str]:
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join(["---"] * len(headers)) + " |",
    ]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return lines


def render_markdown(manifest: dict[str, Any], manifest_path: Path) -> str:
    phase_rows = [
        [
            phase["enum"],
            phase["name"],
            phase["stage"],
            ", ".join(phase["aliases"]) or "-",
        ]
        for phase in manifest["phases"]
    ]
    stage_rows = [
        [
            stage["enum"],
            stage["name"],
            f"{stage['phase_begin']} -> {stage['phase_end']}",
            stage["entry_event"],
            stage["exit_event"],
            ", ".join(stage["aliases"]) or "-",
        ]
        for stage in manifest["stages"]
    ]
    driver_rows = [
        [edge["from"], edge["to"], edge["reason"]] for edge in manifest["driver_edges"]
    ]
    resource_rows = [
        [edge["driver"], edge["kind"], edge["resource_id"], edge["reason"]]
        for edge in manifest["resource_edges"]
    ]
    stage_driver_rows = [
        [edge["phase"], edge["driver"], edge["reason"]]
        for edge in manifest["stage_driver_edges"]
    ]

    lines = [
        "# Bringup Manifest",
        "",
        f"Generated from `{manifest_path.relative_to(ROOT)}` by `tools/bringup_compile.py`.",
        "",
        "This document is the exported view of the declarative bringup manifest that drives:",
        "",
        "- `bringup stage wait|wait-json` stage -> driver -> resource triage",
        "- `dep whatif|whatif-json` static impact projection",
        "- generated C tables committed in `include/bringup_manifest_gen.h`",
        "",
        "## Summary",
        "",
        f"- phases: {len(manifest['phases'])}",
        f"- stages: {len(manifest['stages'])}",
        f"- driver dependency edges: {len(manifest['driver_edges'])}",
        f"- resource dependency edges: {len(manifest['resource_edges'])}",
        f"- stage-driver edges: {len(manifest['stage_driver_edges'])}",
        "",
        "## Phases",
        "",
        *markdown_table(["Enum", "Name", "Stage", "Aliases"], phase_rows),
        "",
        "## Stages",
        "",
        *markdown_table(
            ["Enum", "Name", "Phase Range", "Entry Event", "Exit Event", "Aliases"],
            stage_rows,
        ),
        "",
        "## Driver Edges",
        "",
        *markdown_table(["From", "To", "Reason"], driver_rows),
        "",
        "## Resource Edges",
        "",
        *markdown_table(["Driver", "Kind", "Resource", "Reason"], resource_rows),
        "",
        "## Stage Driver Edges",
        "",
        *markdown_table(["Phase", "Driver", "Reason"], stage_driver_rows),
        "",
    ]
    return "\n".join(lines)


def build_export_json(manifest: dict[str, Any]) -> dict[str, Any]:
    return {
        "phase_count": len(manifest["phases"]),
        "stage_count": len(manifest["stages"]),
        "driver_edge_count": len(manifest["driver_edges"]),
        "resource_edge_count": len(manifest["resource_edges"]),
        "stage_driver_edge_count": len(manifest["stage_driver_edges"]),
        "phases": manifest["phases"],
        "stages": manifest["stages"],
        "driver_edges": manifest["driver_edges"],
        "resource_edges": manifest["resource_edges"],
        "stage_driver_edges": manifest["stage_driver_edges"],
    }


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compile the declarative bringup manifest into generated code and docs."
    )
    ap.add_argument("--manifest", default=str(DEFAULT_MANIFEST), help="source manifest YAML path")
    ap.add_argument(
        "--header-out",
        default=str(DEFAULT_HEADER),
        help="generated manifest header path",
    )
    ap.add_argument(
        "--markdown-out",
        default=str(DEFAULT_MARKDOWN),
        help="generated markdown export path",
    )
    ap.add_argument("--json-out", help="optional normalized manifest export path")
    ap.add_argument("--json", action="store_true", help="print compact JSON summary")
    ns = ap.parse_args()

    manifest_path = Path(ns.manifest)
    header_out = Path(ns.header_out)
    markdown_out = Path(ns.markdown_out)

    if not manifest_path.is_absolute():
        manifest_path = ROOT / manifest_path
    if not header_out.is_absolute():
        header_out = ROOT / header_out
    if not markdown_out.is_absolute():
        markdown_out = ROOT / markdown_out

    manifest = validate_manifest(load_manifest(manifest_path))
    header_text = render_header(manifest)
    markdown_text = render_markdown(manifest, manifest_path)

    header_out.parent.mkdir(parents=True, exist_ok=True)
    markdown_out.parent.mkdir(parents=True, exist_ok=True)
    header_out.write_text(header_text, encoding="utf-8")
    markdown_out.write_text(markdown_text, encoding="utf-8")

    json_out_path: Path | None = None
    if ns.json_out:
        json_out_path = Path(ns.json_out)
        if not json_out_path.is_absolute():
            json_out_path = ROOT / json_out_path
        json_out_path.parent.mkdir(parents=True, exist_ok=True)
        json_out_path.write_text(
            json.dumps(build_export_json(manifest), ensure_ascii=True, indent=2) + "\n",
            encoding="utf-8",
        )

    summary = {
        "ok": True,
        "manifest": str(manifest_path),
        "header": str(header_out),
        "markdown_out": str(markdown_out),
        "json_out": "" if json_out_path is None else str(json_out_path),
        "phase_count": len(manifest["phases"]),
        "stage_count": len(manifest["stages"]),
        "driver_edge_count": len(manifest["driver_edges"]),
        "resource_edge_count": len(manifest["resource_edges"]),
        "stage_driver_edge_count": len(manifest["stage_driver_edges"]),
    }
    if ns.json:
        print(json.dumps(summary, ensure_ascii=True))
    else:
        for key, value in summary.items():
            print(f"{key}: {value}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
