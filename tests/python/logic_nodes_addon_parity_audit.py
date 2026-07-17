# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Parity audit: native LogicNative* vs bge_netlogic addon reference."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import bpy

REPO_ROOT = Path(__file__).resolve().parents[2]
ADDON_NODES = REPO_ROOT / "scripts/addons_core/bge_netlogic/editor/nodes"

sys.path.insert(0, str(REPO_ROOT / "scripts/startup/bl_ui"))
from logic_node_addon_reference import (  # noqa: E402
    EXPECTED_ADDON_SOCKETS,
    NATIVE_ONLY,
    SNAPSHOT_ATTRIBUTE_NATIVE,
    SNAPSHOT_OUTPUT_BY_NATIVE,
    addon_idname_for_native,
    addon_inputs_satisfied,
    addon_outputs_satisfied,
)
from logic_node_parity_batches import PARITY_BATCHES, entry_in_batch  # noqa: E402
from node_add_menu_logic_catalog import LOGIC_NODE_PARITY_CATALOG  # noqa: E402


def parse_addon_sockets(bl_idname: str) -> tuple[dict[str, str], dict[str, str]] | None:
    for path in ADDON_NODES.rglob("*.py"):
        text = path.read_text(encoding="utf-8", errors="ignore")
        if f'bl_idname = "{bl_idname}"' not in text and f"bl_idname = '{bl_idname}'" not in text:
            continue
        inputs: dict[str, str] = {}
        outputs: dict[str, str] = {}
        for match in re.finditer(
            r"self\.add_(input|output)\([^,]+,\s*([\"'])([^\"']*)\2",
            text,
        ):
            bucket = inputs if match.group(1) == "input" else outputs
            name = match.group(3).strip()
            if name:
                bucket[name] = "socket"
        if inputs or outputs:
            return inputs, outputs
    return None


def expected_addon_sockets(native_idname: str, addon_in: dict, addon_out: dict):
    if native_idname in EXPECTED_ADDON_SOCKETS:
        return EXPECTED_ADDON_SOCKETS[native_idname]
    if native_idname in SNAPSHOT_ATTRIBUTE_NATIVE:
        outs = SNAPSHOT_OUTPUT_BY_NATIVE.get(native_idname, set())
        return {}, {name: "socket" for name in outs}
    return addon_in, addon_out


def native_socket_map(idname: str) -> tuple[dict[str, str], dict[str, str]]:
    inputs: dict[str, str] = {}
    outputs: dict[str, str] = {}
    tree = bpy.data.node_groups.new("_LN_Audit", "LogicNodeTree")
    try:
        node = tree.nodes.new(idname)
        for sock in node.inputs:
            # Blank display names are common in the addon for value widgets; use the identifier so
            # parity overrides can still distinguish those sockets without forcing visible labels.
            inputs[sock.name or sock.identifier] = sock.bl_idname
        for sock in node.outputs:
            outputs[sock.name or sock.identifier] = sock.bl_idname
    finally:
        bpy.data.node_groups.remove(tree)
    return inputs, outputs


def _argv_for_parse(argv: list[str] | None) -> list[str]:
    if argv is not None:
        return argv
    if "--" in sys.argv:
        return sys.argv[sys.argv.index("--") + 1 :]
    return []


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Audit native logic node socket parity vs addon")
    parser.add_argument(
        "--family",
        action="append",
        dest="families",
        help="Catalog family prefix filter (repeatable)",
    )
    parser.add_argument(
        "--batch",
        type=int,
        choices=sorted(PARITY_BATCHES),
        help="Predefined batch number from logic_node_parity_batches.py",
    )
    parser.add_argument(
        "--json-out",
        type=Path,
        help="Write gap report JSON to this path",
    )
    parser.add_argument(
        "--gap-limit",
        type=int,
        default=60,
        help="Max gap lines printed to stdout",
    )
    return parser.parse_args(_argv_for_parse(argv))


def entry_matches_filters(entry: dict, args: argparse.Namespace) -> bool:
    if args.batch is not None and not entry_in_batch(entry, args.batch):
        return False
    if args.families:
        family = entry.get("family", "")
        if not any(
            family == f or family.startswith(f + "/") or f.startswith(family) for f in args.families
        ):
            return False
    return True


def run_audit(args: argparse.Namespace) -> tuple[dict, int]:
    ok = 0
    native_only = 0
    gaps: list[str] = []
    unmapped: list[str] = []
    gap_details: list[dict] = []
    ok_nodes: list[str] = []

    for entry in LOGIC_NODE_PARITY_CATALOG.values():
        if entry.get("status") != "implemented":
            continue
        if not entry_matches_filters(entry, args):
            continue

        native_id = entry["idname"]
        label = entry["label"]
        family = entry.get("family", "")

        if native_id in NATIVE_ONLY:
            native_only += 1
            continue

        addon_id = addon_idname_for_native(native_id, label)
        if addon_id is None:
            unmapped.append(f"{native_id} ({label})")
            gap_details.append(
                {"idname": native_id, "label": label, "family": family, "kind": "unmapped"}
            )
            continue

        if native_id in EXPECTED_ADDON_SOCKETS:
            addon_in, addon_out = EXPECTED_ADDON_SOCKETS[native_id]
        else:
            addon_sockets = parse_addon_sockets(addon_id)
            if addon_sockets is None:
                gaps.append(f"{native_id}: parse failed for {addon_id}")
                gap_details.append(
                    {
                        "idname": native_id,
                        "label": label,
                        "family": family,
                        "kind": "parse_failed",
                        "addon_id": addon_id,
                    }
                )
                continue
            addon_in, addon_out = expected_addon_sockets(native_id, *addon_sockets)

        native_in, native_out = native_socket_map(native_id)

        ni, no = set(native_in), set(native_out)
        ai, ao = set(addon_in), set(addon_out)

        in_ok = addon_inputs_satisfied(native_id, ni, ai)
        out_ok = addon_outputs_satisfied(ni, no, ao)

        if in_ok and out_ok:
            ok += 1
            ok_nodes.append(native_id)
        else:
            detail = {
                "idname": native_id,
                "label": label,
                "family": family,
                "addon_id": addon_id,
                "native_inputs": sorted(ni),
                "native_outputs": sorted(no),
                "addon_inputs": sorted(ai),
                "addon_outputs": sorted(ao),
            }
            if not in_ok:
                missing = sorted(name for name in ai if not addon_inputs_satisfied(native_id, ni, {name}))
                gaps.append(f"{native_id}: missing inputs {missing} (addon {addon_id})")
                detail["missing_inputs"] = missing
            if not out_ok:
                missing = sorted(
                    name
                    for name in ao
                    if not addon_outputs_satisfied(ni, no, {name})
                )
                gaps.append(f"{native_id}: missing outputs {missing} (addon {addon_id})")
                detail["missing_outputs"] = missing
            gap_details.append(detail)

    total_filtered = ok + native_only + len(unmapped) + len(
        {d["idname"] for d in gap_details if d.get("kind") != "unmapped"}
    ) + len([d for d in gap_details if "missing_inputs" in d or "missing_outputs" in d])
    # Re-count total in filter
    total_filtered = sum(
        1
        for e in LOGIC_NODE_PARITY_CATALOG.values()
        if e.get("status") == "implemented" and entry_matches_filters(e, args)
    )

    report = {
        "total": total_filtered,
        "parity_ok": ok,
        "native_only": native_only,
        "unmapped": unmapped,
        "gaps": gaps,
        "ok_nodes": ok_nodes,
        "gap_details": gap_details,
        "batch": args.batch,
        "families": args.families,
    }

    print(f"Catalog nodes: {total_filtered}")
    print(f"Parity OK: {ok}")
    print(f"Native-only: {native_only}")
    print(f"Unmapped: {len(unmapped)}")
    print(f"Gaps: {len(gaps)}")

    if unmapped:
        print("\n-- Unmapped --")
        for line in unmapped:
            print(line)
    if gaps:
        print("\n-- Gaps --")
        for line in gaps[: args.gap_limit]:
            print(line)
        if len(gaps) > args.gap_limit:
            print(f"... and {len(gaps) - args.gap_limit} more")

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"\nWrote {args.json_out}")

    exit_code = 0
    if gaps or unmapped:
        exit_code = 1
    elif ok + native_only == total_filtered:
        print("All catalog nodes accounted for.")
    else:
        exit_code = 1
    report["exit_code"] = exit_code
    return report, exit_code


def main(argv: list[str] | None = None) -> int:
    if not hasattr(bpy.types, "LogicNodeTree"):
        print("LogicNodeTree is not registered")
        return 1
    args = parse_args(argv)
    _, code = run_audit(args)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
