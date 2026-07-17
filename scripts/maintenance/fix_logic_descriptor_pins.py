#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One-shot fix for NOD_logic_descriptors.hh pin identifiers vs node_logic_*.cc."""

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
METADATA_NODE_RE = re.compile(
    r'\{"(LogicNative[^"]+)",.*?\n\s*(\{\}|pin_span\((\w+)\)),\s*\n\s*(\{\}|pin_span\((\w+)\)),',
    re.DOTALL,
)


def parse_cc() -> dict[str, tuple[list[str], list[str]]]:
    result: dict[str, tuple[list[str], list[str]]] = {}
    for path in sorted(NODES_DIR.glob("node_logic_*.cc")):
        text = path.read_text(encoding="utf-8")
        m = IDNAME_RE.search(text)
        if not m:
            continue
        inputs: list[str] = []
        outputs: list[str] = []
        for line in text.splitlines():
            for match in SOCKET_LINE_RE.finditer(line):
                pin_id = match.group(3) or match.group(2)
                if match.group(1) == "input":
                    inputs.append(pin_id)
                else:
                    outputs.append(pin_id)
        result[m.group(1)] = (inputs, outputs)
    return result


def main() -> None:
    cc = parse_cc()
    text = DESCRIPTORS_HH.read_text(encoding="utf-8")

    # --- Pin array definitions ---
    if "inline constexpr PinMetadata done_output[]" not in text:
        text = text.replace(
            "inline constexpr PinMetadata condition_output[] = {\n"
            '  {"Out", "Out", PinType::Condition, "false", false},\n'
            "};",
            "inline constexpr PinMetadata condition_output[] = {\n"
            '  {"Out", "Out", PinType::Condition, "false", false},\n'
            "};\n"
            "inline constexpr PinMetadata done_output[] = {\n"
            '  {"Done", "Done", PinType::Condition, "false", false},\n'
            "};\n"
            "inline constexpr PinMetadata out_and_done_outputs[] = {\n"
            '  {"Out", "Out", PinType::Condition, "false", false},\n'
            '  {"Done", "Done", PinType::Condition, "false", false},\n'
            "};\n"
            "inline constexpr PinMetadata timer_inputs[] = {\n"
            '  {"Set Timer", "Set Timer", PinType::Condition, "false", false},\n'
            '  {"Seconds", "Seconds", PinType::Float, "0.0", false},\n'
            "};\n"
            "inline constexpr PinMetadata timer_outputs[] = {\n"
            '  {"Out", "When Elapsed", PinType::Condition, "false", false},\n'
            "};\n"
            "inline constexpr PinMetadata float_value_output[] = {\n"
            '  {"Value", "Value", PinType::Float, "0.0", false},\n'
            "};",
        )

    text = text.replace(
        '{"Interval", "Interval", PinType::Float, "0.5", false}',
        '{"Gap", "Gap", PinType::Float, "0.5", false}',
    )

    text = text.replace(
        '  {"If True", "If True", PinType::Condition, "false", false},\n'
        "};\n"
        "inline constexpr PinMetadata math_inputs[]",
        '  {"Result", "Result", PinType::Bool, "false", false},\n'
        "};\n"
        "inline constexpr PinMetadata math_inputs[]",
    )

    # float nodes that share value_float_outputs but use "Value" in .cc
    for old in (
        "pin_span(value_float_outputs),\n   false,\n   RequiredPhase::None,\n   false,\n   true,\n   ExecutionClass::SnapshotReadOnly},\n  {\"LogicNativeRandomValue\"",
        "pin_span(value_float_outputs),\n   false,\n   RequiredPhase::FixedUpdate",
        "pin_span(value_float_outputs),\n   false,\n   RequiredPhase::None,\n   false,\n   true,\n   ExecutionClass::SnapshotReadOnly},\n  {\"LogicNativeStoreValue\"",
    ):
        text = text.replace(
            old,
            old.replace("value_float_outputs", "float_value_output", 1),
        )

    # Timer metadata block
    text = text.replace(
        '  {"LogicNativeTimer",\n'
        '   "Time",\n'
        '   "Timer",\n'
        '   "Pulses after a triggered duration elapses",\n'
        "   pin_span(time_condition_inputs),\n"
        "   pin_span(condition_output),",
        '  {"LogicNativeTimer",\n'
        '   "Time",\n'
        '   "Timer",\n'
        '   "Pulses after a triggered duration elapses",\n'
        "   pin_span(timer_inputs),\n"
        "   pin_span(timer_outputs),",
    )

    OUT_AND_DONE = {
        "LogicNativeSetCamera",
        "LogicNativeSetCameraFov",
        "LogicNativeSetCameraOrthoScale",
        "LogicNativeSetCharacterGravity",
        "LogicNativeSetCharacterJumpSpeed",
        "LogicNativeSetCharacterMaxJumps",
        "LogicNativeSetCharacterVelocity",
        "LogicNativeSetCharacterWalkDirection",
        "LogicNativeSetFullscreen",
        "LogicNativeSetResolution",
        "LogicNativeSetVSync",
        "LogicNativeSetParent",
        "LogicNativeSetLightColor",
        "LogicNativeSetLightPower",
        "LogicNativeSetLightShadow",
    }
    DONE_ONLY_REPLACE = {
        "LogicNativeModifyProperty",
        "LogicNativeModifyPropertyClamped",
    }

    for idname in OUT_AND_DONE:
        text = re.sub(
            rf'(\{{"{idname}",.*?\n\s*pin_span\([^)]+\),\s*\n\s*)pin_span\(condition_output\)',
            r"\1pin_span(out_and_done_outputs)",
            text,
            count=1,
            flags=re.DOTALL,
        )
        text = re.sub(
            rf'(\{{"{idname}",.*?\n\s*pin_span\([^)]+\),\s*\n\s*)\{{\}}',
            r"\1pin_span(out_and_done_outputs)",
            text,
            count=1,
            flags=re.DOTALL,
        )

    for idname in DONE_ONLY_REPLACE:
        text = re.sub(
            rf'(\{{"{idname}",.*?\n\s*pin_span\([^)]+\),\s*\n\s*)pin_span\(condition_output\)',
            r"\1pin_span(done_output)",
            text,
            count=1,
            flags=re.DOTALL,
        )

    # Nodes whose .cc has only Done output but metadata uses empty outputs.
    done_only_ids = {
        idname
        for idname, (_, outs) in cc.items()
        if outs == ["Done"]
    }
    for idname in sorted(done_only_ids):
        text = re.sub(
            rf'(\{{"{re.escape(idname)}",.*?\n\s*pin_span\([^)]+\),\s*\n\s*)\{{\}}',
            r"\1pin_span(done_output)",
            text,
            count=1,
            flags=re.DOTALL,
        )

    DESCRIPTORS_HH.write_text(text, encoding="utf-8")
    print(f"Updated {DESCRIPTORS_HH}")


if __name__ == "__main__":
    main()
