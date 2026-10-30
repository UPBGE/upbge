# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit LogicNative node sockets vs NOD_logic_descriptors.hh (compile-time pin validation)."""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DESCRIPTORS_HH = REPO_ROOT / "source/blender/nodes/NOD_logic_descriptors.hh"
NODES_DIR = REPO_ROOT / "source/blender/nodes/logic/nodes"
NODE_LOGIC_UTIL_CC = REPO_ROOT / "source/blender/nodes/logic/node_logic_util.cc"
COMPILER_CC = REPO_ROOT / "source/gameengine/LogicNodes/intern/LN_TreeCompiler.cpp"
COMPILER_HELPERS_CC = (
    REPO_ROOT / "source/gameengine/LogicNodes/intern/compile/LN_TreeCompiler_helpers_inputs.cc"
)
COMPILER_EXPRESSION_OUTPUTS_BY_KIND_CC = (
    REPO_ROOT
    / "source/gameengine/LogicNodes/intern/compile/LN_TreeCompiler_expression_outputs_by_kind.cc"
)
SOCKET_DECLARATIONS_CC = REPO_ROOT / "source/blender/nodes/logic/node_logic_socket_declarations.cc"

PIN_ARRAY_RE = re.compile(
    r"inline constexpr PinMetadata (\w+)\[\] = \{(.*?)\};",
    re.DOTALL,
)
PIN_ENTRY_RE = re.compile(
    r'\{\s*"([^"]*)",\s*"([^"]*)",\s*PinType::(\w+),\s*"[^"]*",\s*'
    r'((?:true|false)(?:\s*,\s*(?:true|false)){0,2})\s*\}'
)
NODE_ENTRY_RE = re.compile(
    r'\{"(LogicNative[^"]+)",.*?\n\s*(\{\}|pin_span\((\w+)\)),\s*\n\s*(\{\}|pin_span\((\w+)\)),',
    re.DOTALL,
)
SOCKET_DECL_RE = re.compile(
    r'add_(input|output)<(?:logic_)?decl::(\w+)>\(\s*"([^"]*)"_ustr(?:\s*,\s*"([^"]*)"_ustr)?',
)
DYNAMIC_IDENTIFIER_SOCKET_DECL_RE = re.compile(
    r'add_(input|output)<(?:logic_)?decl::(\w+)>\([^,\n]+,\s*"([^"]*)"_ustr\)',
)
DYNAMIC_SOCKET_NAME_RE = re.compile(
    r'const UString (\w+)\(\s*"([^"]*)"\s*\+[^;]+\);'
)
DYNAMIC_SOCKET_DECL_RE = re.compile(
    r'add_(input|output)<(?:logic_)?decl::(\w+)>\(\s*(\w+)\s*\)'
)
SOCKET_HELPER_RE = re.compile(
    r"(?:static\s+)?void\s+(add_\w+)\(NodeDeclarationBuilder &b\)\s*\{(.*?)^\}",
    re.DOTALL | re.MULTILINE,
)
SOCKET_HELPER_CALL_RE = re.compile(r"(?:\w+::)*(add_\w+)\(b\);")
IDNAME_RE = re.compile(r'logic_node_type_base\([^,]+,\s*"(LogicNative[^"]+)"')
TYPE_BASE_RE = re.compile(
    r'logic_node_type_base\(\s*&?(\w+)\s*,\s*"([^"]+)"_ustr\)'
)
DECLARE_ASSIGN_RE = re.compile(r"(\w+)\.declare\s*=\s*(\w+);")
DECLARE_FN_RE = re.compile(
    r"static void (\w+)\(NodeDeclarationBuilder &b\)\s*\{(.*?)^\}",
    re.DOTALL | re.MULTILINE,
)
REGISTER_C_TIER_RE = re.compile(
    r'REGISTER_C_TIER_(?:WIDE_)?NODE\(\s*\w+\s*,\s*"(LogicNative[^"]+)"\s*,\s*[^,]+,'
    r'\s*(\w+)\s*,',
    re.DOTALL,
)
SETUP_NODE_TYPE_RE = re.compile(
    r'setup_node_type\(\s*\w+\s*,\s*"(LogicNative[^"]+)"\s*,\s*[^,]+,\s*(\w+)\s*(?:,|\))',
    re.DOTALL,
)
REVERSE_LEGACY_EXEC_ALIAS_RE = re.compile(
    r'SocketMatchesNameOrIdentifier\(\s*socket\s*,\s*"Flow"\s*\).*?'
    r'pin\.kind\s*==\s*LN_PinKind::Execution\s*&&\s*pin\.name\s*==\s*"Condition"',
    re.DOTALL,
)

PIN_TYPE_TO_DECL = {
    "Execution": "Execution",
    "Condition": "Condition",
    "Generic": "Generic",
    "Bool": "Bool",
    "Int": "Int",
    "CollisionLayers": "CollisionLayers",
    "Float": "Float",
    "Angle": "Float",
    "String": "String",
    "Vector": "Vector",
    "Rotation": "Rotation",
    "Color": "Color",
    "Object": "Object",
    "List": "List",
    "Dictionary": "Dictionary",
    "Material": "Material",
    "Image": "Image",
    "Sound": "Sound",
    "Font": "Font",
    "GeometryTree": "GeometryTree",
    "Text": "Text",
    "Mesh": "Mesh",
    "Collection": "Collection",
    "Scene": "Scene",
    "Datablock": "Datablock",
    "UI": "UI",
    "Python": "Python",
}


@dataclass(frozen=True)
class Pin:
    identifier: str
    ui_name: str
    pin_type: str
    allows_multiple_links: bool = False
    declares_multiple_links: bool = False
    is_variadic: bool = False


@dataclass(frozen=True)
class SocketDecl:
    identifier: str
    ui_name: str
    decl_type: str


def parse_pin_arrays(text: str) -> dict[str, list[Pin]]:
    arrays: dict[str, list[Pin]] = {}
    for match in PIN_ARRAY_RE.finditer(text):
        name = match.group(1)
        body = match.group(2)
        pins = []
        for entry in PIN_ENTRY_RE.finditer(body):
            flags = [flag.strip() == "true" for flag in entry.group(4).split(",")]
            pins.append(
                Pin(
                    entry.group(1),
                    entry.group(2),
                    entry.group(3),
                    allows_multiple_links=len(flags) >= 2 and flags[1],
                    declares_multiple_links=len(flags) >= 2,
                    is_variadic=len(flags) >= 3 and flags[2],
                )
            )
        arrays[name] = pins
    return arrays


def parse_node_metadata_pins(
    text: str, pin_arrays: dict[str, list[Pin]]
) -> dict[str, tuple[list[Pin], list[Pin]]]:
    result: dict[str, tuple[list[Pin], list[Pin]]] = {}
    for match in NODE_ENTRY_RE.finditer(text):
        idname = match.group(1)
        inputs_span = match.group(3) if match.group(2) != "{}" else None
        outputs_span = match.group(5) if match.group(4) != "{}" else None
        inputs = pin_arrays.get(inputs_span, []) if inputs_span else []
        outputs = pin_arrays.get(outputs_span, []) if outputs_span else []
        result[idname] = (inputs, outputs)
    return result


def append_sockets(
    target_inputs: list[SocketDecl],
    target_outputs: list[SocketDecl],
    sockets: tuple[list[SocketDecl], list[SocketDecl]],
) -> None:
    target_inputs.extend(sockets[0])
    target_outputs.extend(sockets[1])


def parse_sockets(
    text: str,
    *,
    prefer_first: bool = False,
    helpers: dict[str, tuple[list[SocketDecl], list[SocketDecl]]] | None = None,
) -> tuple[list[SocketDecl], list[SocketDecl]]:
    inputs: list[SocketDecl] = []
    outputs: list[SocketDecl] = []
    dynamic_names = {
        match.group(1): match.group(2) for match in DYNAMIC_SOCKET_NAME_RE.finditer(text)
    }
    for line in text.splitlines():
        parsed_literal = False
        for match in SOCKET_DECL_RE.finditer(line):
            parsed_literal = True
            direction = match.group(1)
            decl_type = match.group(2)
            first_name = match.group(3)
            second_name = match.group(4)
            identifier = first_name
            ui_name = second_name or first_name
            socket = SocketDecl(identifier, ui_name, decl_type)
            if direction == "output":
                outputs.append(socket)
            elif direction == "input":
                inputs.append(socket)
        if not parsed_literal:
            for match in DYNAMIC_IDENTIFIER_SOCKET_DECL_RE.finditer(line):
                direction = match.group(1)
                socket = SocketDecl(match.group(3), match.group(3), match.group(2))
                (outputs if direction == "output" else inputs).append(socket)
        for match in DYNAMIC_SOCKET_DECL_RE.finditer(line):
            prefix = dynamic_names.get(match.group(3))
            if prefix is None:
                continue
            direction = match.group(1)
            socket = SocketDecl(prefix, prefix, match.group(2))
            (outputs if direction == "output" else inputs).append(socket)
        if helpers is not None:
            for match in SOCKET_HELPER_CALL_RE.finditer(line):
                helper_sockets = helpers.get(match.group(1))
                if helper_sockets is not None:
                    append_sockets(inputs, outputs, helper_sockets)
    return inputs, outputs


def parse_socket_helpers() -> dict[str, tuple[list[SocketDecl], list[SocketDecl]]]:
    text = NODE_LOGIC_UTIL_CC.read_text(encoding="utf-8")
    return {
        match.group(1): parse_sockets(match.group(2))
        for match in SOCKET_HELPER_RE.finditer(text)
    }


def parse_node_cc_file(
    path: Path,
    helpers: dict[str, tuple[list[SocketDecl], list[SocketDecl]]],
) -> dict[str, tuple[list[SocketDecl], list[SocketDecl]]]:
    text = path.read_text(encoding="utf-8")
    entries: dict[str, tuple[list[SocketDecl], list[SocketDecl]]] = {}

    local_helpers = dict(helpers)
    local_helpers.update(
        {
            match.group(1): parse_sockets(match.group(2), helpers=local_helpers)
            for match in SOCKET_HELPER_RE.finditer(text)
        }
    )

    declare_sockets = {
        match.group(1): parse_sockets(match.group(2), helpers=local_helpers)
        for match in DECLARE_FN_RE.finditer(text)
    }
    for match in REGISTER_C_TIER_RE.finditer(text):
        idname = match.group(1)
        declare_fn = match.group(2)
        if declare_fn in declare_sockets:
            entries[idname] = declare_sockets[declare_fn]

    for match in SETUP_NODE_TYPE_RE.finditer(text):
        idname = match.group(1)
        declare_fn = match.group(2)
        if declare_fn in declare_sockets:
            entries[idname] = declare_sockets[declare_fn]

    type_vars: dict[str, str] = {}
    for match in TYPE_BASE_RE.finditer(text):
        type_vars[match.group(1)] = match.group(2)
    for match in DECLARE_ASSIGN_RE.finditer(text):
        idname = type_vars.get(match.group(1))
        declare_fn = match.group(2)
        if idname is not None and declare_fn in declare_sockets:
            entries[idname] = declare_sockets[declare_fn]

    if entries:
        return entries
    id_match = IDNAME_RE.search(text)
    if not id_match:
        return {}
    idname = id_match.group(1)
    entries[idname] = parse_sockets(text, helpers=local_helpers)
    return entries


def pin_ids(pins: list[Pin]) -> list[str]:
    return [pin.identifier for pin in pins]


def socket_ids(sockets: list[SocketDecl]) -> list[str]:
    return [
        socket.identifier
        if socket.identifier == socket.ui_name
        else f"{socket.identifier} / {socket.ui_name}"
        for socket in sockets
    ]


def descriptor_matches_socket(pin: Pin, socket: SocketDecl) -> bool:
    return pin.identifier == socket.identifier or pin.identifier == socket.ui_name


def descriptor_socket_lists_match(pins: list[Pin], sockets: list[SocketDecl]) -> bool:
    return len(pins) == len(sockets) and all(
        descriptor_matches_socket(pin, socket) for pin, socket in zip(pins, sockets)
    )


def runtime_identifier_matches_pin(pin: Pin, identifier: str) -> bool:
    """Blender may expose either declared socket string as its runtime identifier.

    The descriptor retains the compiler-facing identifier and the UI-facing name.  Nodes using
    the two-string declaration overload expose the latter through ``NodeSocket.identifier``.
    Both are therefore part of the declared socket contract.
    """
    return identifier == pin.identifier or identifier == pin.ui_name


def runtime_socket_lists_match(pins: list[Pin], identifiers: list[str]) -> bool:
    """Match concrete Blender sockets against fixed and prefix-variadic descriptor pins."""
    pin_index = 0
    socket_index = 0
    while pin_index < len(pins):
        pin = pins[pin_index]
        if not pin.is_variadic:
            if socket_index >= len(identifiers) or not runtime_identifier_matches_pin(
                pin, identifiers[socket_index]
            ):
                return False
            pin_index += 1
            socket_index += 1
            continue

        next_pin = None
        if pin_index + 1 < len(pins):
            next_pin = pins[pin_index + 1]
        match_count = 0
        while socket_index < len(identifiers):
            identifier = identifiers[socket_index]
            if next_pin is not None and runtime_identifier_matches_pin(next_pin, identifier):
                break
            if not (identifier.startswith(pin.identifier) or identifier.startswith(pin.ui_name)):
                return False
            match_count += 1
            socket_index += 1
        if match_count == 0:
            return False
        pin_index += 1
    return socket_index == len(identifiers)


def audit_execution_condition_schema(pin_arrays: dict[str, list[Pin]]) -> list[str]:
    errors: list[str] = []
    for array_name, pins in sorted(pin_arrays.items()):
        for pin in pins:
            if pin.pin_type == "Execution" and (
                pin.identifier == "Condition" or pin.ui_name == "Condition"
            ):
                errors.append(
                    f"{array_name}: execution pin must not be named Condition "
                    f"({pin.identifier!r}, {pin.ui_name!r})"
                )
    return errors


def audit_removed_migration_paths() -> list[str]:
    helpers_text = COMPILER_HELPERS_CC.read_text(encoding="utf-8")
    compiler_text = COMPILER_CC.read_text(encoding="utf-8")
    socket_decl_text = SOCKET_DECLARATIONS_CC.read_text(encoding="utf-8")
    combined = "\n".join((helpers_text, compiler_text, socket_decl_text))

    errors: list[str] = []
    if REVERSE_LEGACY_EXEC_ALIAS_RE.search(helpers_text):
        errors.append("compiler helper keeps reverse Flow -> legacy Condition execution alias")

    forbidden_fragments = [
        ("LogicPinsUseTemporaryBoolToExecutionCompatibility", "bool-to-exec compatibility helper"),
        ("temporary boolean-to-execution compatibility", "bool-to-exec migration diagnostic"),
        ("CanUseLegacyConditionExecutionInput", "legacy Condition execution input fallback"),
        ("LEGACY_CONDITION_EXECUTION_INPUT", "legacy Condition execution input constant"),
        ("logic_condition_socket_idname_for_pin", "Condition declaration execution remapping"),
        ("logic_condition_socket_is_execution_flow", "Condition declaration execution test"),
        (
            'name == "Condition" && std::strcmp(node.idname, "LogicNativeBranch") != 0',
            "Condition input lookup fallback",
        ),
        (
            'SocketMatchesNameOrIdentifier(socket, "Condition")',
            "Condition socket-to-Flow pin fallback",
        ),
    ]
    for fragment, description in forbidden_fragments:
        if fragment in combined:
            errors.append(f"migration path remains: {description}: {fragment!r}")
    return errors


def audit_pin_kind_diagnostics() -> list[str]:
    compiler_text = COMPILER_CC.read_text(encoding="utf-8")
    helpers_text = COMPILER_HELPERS_CC.read_text(encoding="utf-8")
    expression_outputs_text = COMPILER_EXPRESSION_OUTPUTS_BY_KIND_CC.read_text(encoding="utf-8")
    combined = "\n".join((compiler_text, helpers_text, expression_outputs_text))

    errors: list[str] = []
    forbidden_fragments = [
        ("Link type mismatch", "generic link mismatch diagnostic"),
        ("Required flow input is not linked", "legacy required flow diagnostic"),
        (
            'FindInputSocket(node, "Flow") != nullptr ? "Flow" : "Condition"',
            "name-probed primary execution input fallback",
        ),
        ('BuildExecutionWithFallback("Flow"', "per-call primary execution fallback"),
        ('ResolveRequiredFlowWithFallback("Flow"', "per-call primary flow fallback"),
        ("BuildPrimaryExecutionExpressionWithLegacyFallback", "primary execution fallback helper"),
        ("ResolveRequiredPrimaryFlowWithLegacyFallback", "primary flow fallback resolver"),
    ]
    for fragment, description in forbidden_fragments:
        if fragment in combined:
            errors.append(f"compiler keeps {description}: {fragment!r}")

    required_fragments = [
        "connect execution to 'Flow' and boolean data to 'Condition'",
        "Required execution input '",
        "Required input socket is missing:",
        "IsExecutionOutputSocket(definition, route_socket)",
        "IsExecutionOutputSocket(definition, *link.fromsock)",
        "BuildPrimaryExecutionExpression",
    ]
    for fragment in required_fragments:
        if fragment not in combined:
            errors.append(f"compiler is missing pin-kind diagnostic fragment: {fragment!r}")

    return errors


def audit_decl_types(
    idname: str,
    direction: str,
    descriptor_pins: list[Pin],
    sockets: list[SocketDecl],
    cc_file: str,
) -> list[str]:
    errors: list[str] = []
    for pin, socket in zip(descriptor_pins, sockets):
        expected_decl = PIN_TYPE_TO_DECL.get(pin.pin_type)
        if expected_decl is None or socket.decl_type == expected_decl:
            continue
        errors.append(
            f"{idname} ({cc_file}): {direction} {pin.identifier!r} has descriptor "
            f"PinType::{pin.pin_type} but node_declare uses decl::{socket.decl_type}"
        )
    return errors


def audit_descriptors_vs_cc() -> list[str]:
    hh_text = DESCRIPTORS_HH.read_text(encoding="utf-8")
    pin_arrays = parse_pin_arrays(hh_text)
    descriptor_pins = parse_node_metadata_pins(hh_text, pin_arrays)
    helpers = parse_socket_helpers()

    cc_by_idname: dict[str, tuple[list[SocketDecl], list[SocketDecl], str]] = {}
    for cc_path in sorted(NODES_DIR.glob("node_logic_*.cc")):
        for idname, (inputs, outputs) in parse_node_cc_file(cc_path, helpers).items():
            cc_by_idname[idname] = (inputs, outputs, cc_path.name)

    errors: list[str] = audit_execution_condition_schema(pin_arrays)
    errors.extend(audit_removed_migration_paths())
    errors.extend(audit_pin_kind_diagnostics())
    errors.extend(audit_socket_names(pin_arrays))

    for idname, (desc_in, desc_out) in sorted(descriptor_pins.items()):
        if idname not in cc_by_idname:
            errors.append(f"{idname}: in descriptors but no node_logic_*.cc")
            continue
        cc_in, cc_out, cc_file = cc_by_idname[idname]
        if not descriptor_socket_lists_match(desc_in, cc_in):
            errors.append(
                f"{idname} ({cc_file}): input mismatch\n"
                f"  descriptor: {pin_ids(desc_in)}\n"
                f"  node_declare: {socket_ids(cc_in)}"
            )
        if not descriptor_socket_lists_match(desc_out, cc_out):
            errors.append(
                f"{idname} ({cc_file}): output mismatch\n"
                f"  descriptor: {pin_ids(desc_out)}\n"
                f"  node_declare: {socket_ids(cc_out)}"
            )
        errors.extend(audit_decl_types(idname, "input", desc_in, cc_in, cc_file))
        errors.extend(audit_decl_types(idname, "output", desc_out, cc_out, cc_file))

    for idname, (cc_in, cc_out, cc_file) in sorted(cc_by_idname.items()):
        if idname not in descriptor_pins:
            errors.append(f"{idname} ({cc_file}): missing from logic_node_metadata()")

    return errors


EMPTY_SOCKET_NAME_RE = re.compile(r'add_(?:input|output)<[^>]+>\(\s*""_ustr')
LABELLESS_PROP_RE = re.compile(r'\.prop\(\s*[&\w.]*ptr\w*,\s*"(\w+)",[^;]*?,\s*""\s*,\s*ICON_NONE')
RNA_BOOLEAN_RE = re.compile(r'RNA_def_property\(srna,\s*"(\w+)",\s*PROP_BOOLEAN')
RNA_NODETREE_CC = REPO_ROOT / "source/blender/makesrna/intern/rna_nodetree.cc"


def audit_socket_names(pin_arrays: dict[str, list[Pin]]) -> list[str]:
    """Every socket and descriptor pin has a name: a nameless one shows a bare field (or only a
    dot for Generic sockets) and an empty row once linked. A checkbox drawn without text is just
    as unclear (Store Value's Initialize until 2026-10-15)."""
    errors: list[str] = []
    booleans = set(RNA_BOOLEAN_RE.findall(RNA_NODETREE_CC.read_text(encoding="utf-8")))
    for path in sorted([*NODES_DIR.glob("node_logic_*.cc"), NODE_LOGIC_UTIL_CC]):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if EMPTY_SOCKET_NAME_RE.search(line):
                errors.append(f"{path.name}:{number}: socket declared with an empty name")
            for prop in LABELLESS_PROP_RE.findall(line):
                if prop in booleans:
                    errors.append(f"{path.name}:{number}: checkbox {prop!r} drawn without a label")
    for array, pins in pin_arrays.items():
        for pin in pins:
            if not pin.ui_name:
                errors.append(f"{array}: pin {pin.identifier!r} has an empty name")
    return errors


def audit_blender_runtime() -> list[str]:
    import bpy

    sys.path.insert(0, str(REPO_ROOT / "scripts/startup/bl_ui"))
    from node_add_menu_logic_catalog import LOGIC_NODE_PARITY_CATALOG  # noqa: E402

    hh_text = DESCRIPTORS_HH.read_text(encoding="utf-8")
    pin_arrays = parse_pin_arrays(hh_text)
    descriptor_pins = parse_node_metadata_pins(hh_text, pin_arrays)

    tree = bpy.data.node_groups.new("_DescriptorAudit", "LogicNodeTree")
    errors: list[str] = []
    try:
        for entry in LOGIC_NODE_PARITY_CATALOG.values():
            idname = entry["idname"]
            if idname not in descriptor_pins:
                continue
            desc_in, desc_out = descriptor_pins[idname]
            try:
                node = tree.nodes.new(idname)
            except RuntimeError as exc:
                errors.append(f"{idname}: failed to spawn in Blender: {exc}")
                continue
            actual_in = [s.identifier for s in node.inputs]
            actual_out = [s.identifier for s in node.outputs]
            for socket in [*node.inputs, *node.outputs]:
                if not socket.name:
                    errors.append(f"{idname}: socket {socket.identifier!r} has no name")
            if not runtime_socket_lists_match(desc_in, actual_in):
                errors.append(
                    f"{idname}: runtime input != descriptor\n"
                    f"  descriptor: {pin_ids(desc_in)}\n"
                    f"  Blender:    {actual_in}"
                )
            if not runtime_socket_lists_match(desc_out, actual_out):
                errors.append(
                    f"{idname}: runtime output != descriptor\n"
                    f"  descriptor: {pin_ids(desc_out)}\n"
                    f"  Blender:    {actual_out}"
                )
            for pin, socket in zip(desc_in, node.inputs):
                if pin.is_variadic:
                    break
                if (
                    pin.declares_multiple_links
                    and pin.allows_multiple_links != socket.is_multi_input
                ):
                    errors.append(
                        f"{idname}: runtime multi-input flag differs for {pin.identifier!r}\n"
                        f"  descriptor: {pin.allows_multiple_links}\n"
                        f"  Blender:    {socket.is_multi_input}"
                    )
            tree.nodes.remove(node)
    finally:
        bpy.data.node_groups.remove(tree)
    return errors


def main() -> int:
    errors = audit_descriptors_vs_cc()
    print(f"Static descriptor vs .cc audit: {len(errors)} issue(s)")
    for err in errors:
        print(err)
        print()

    if "--blender" in sys.argv:
        import bpy

        bpy.ops.wm.read_homefile(app_template="")
        runtime_errors = audit_blender_runtime()
        print(f"Blender runtime vs descriptor: {len(runtime_errors)} issue(s)")
        for err in runtime_errors:
            print(err)
            print()
        errors.extend(runtime_errors)

    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
