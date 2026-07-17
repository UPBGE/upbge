#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Align metadata output pin_span with node_logic_*.cc socket identifiers."""

from __future__ import annotations

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DESCRIPTORS_HH = REPO_ROOT / "source/blender/nodes/NOD_logic_descriptors.hh"
NODES_DIR = REPO_ROOT / "source/blender/nodes/logic/nodes"

SOCKET_LINE_RE = re.compile(
    r'add_(input|output)<[^>]+>\(\s*"([^"]+)"_ustr(?:\s*,\s*"([^"]+)"_ustr)?'
)
IDNAME_RE = re.compile(r'logic_node_type_base\([^,]+,\s*"(LogicNative[^"]+)"')
NODE_BLOCK_RE = re.compile(
    r'(\{"(LogicNative[^"]+)",.*?\n\s*(\{\}|pin_span\(\w+\)),\s*\n\s*)(\{\}|pin_span\(\w+\)),',
    re.DOTALL,
)


def parse_cc_outputs() -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for path in sorted(NODES_DIR.glob("node_logic_*.cc")):
        text = path.read_text(encoding="utf-8")
        m = IDNAME_RE.search(text)
        if not m:
            continue
        outputs: list[str] = []
        for line in text.splitlines():
            if "add_output<" not in line:
                continue
            for match in SOCKET_LINE_RE.finditer(line):
                outputs.append(match.group(3) or match.group(2))
        result[m.group(1)] = outputs
    return result


def output_span_for_pins(outputs: list[str]) -> str:
    if not outputs:
        return "{}"
    if outputs == ["Done"]:
        return "pin_span(done_output)"
    if outputs == ["Out"]:
        return "pin_span(condition_output)"
    if outputs == ["Out", "Done"]:
        return "pin_span(out_and_done_outputs)"
    return ""  # keep custom spans


def main() -> None:
    cc_out = parse_cc_outputs()
    text = DESCRIPTORS_HH.read_text(encoding="utf-8")
    changes = 0

    def replacer(match: re.Match[str]) -> str:
        nonlocal changes
        idname = match.group(2)
        prefix = match.group(1)
        current = match.group(3)
        if idname not in cc_out:
            return match.group(0)
        desired = output_span_for_pins(cc_out[idname])
        if not desired or current == desired:
            return match.group(0)
        changes += 1
        return prefix + desired + ","

    text = NODE_BLOCK_RE.sub(replacer, text)

    # Has property output identifier is Out (display If True).
    text = text.replace(
        '  {"If True", "Out", PinType::Condition, "false", false},',
        '  {"Out", "If True", PinType::Condition, "false", false},',
    )

    DESCRIPTORS_HH.write_text(text, encoding="utf-8")
    print(f"Updated {changes} metadata output span(s)")


if __name__ == "__main__":
    main()
