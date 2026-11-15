# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Backspace over one logic node setting (UI_OT_reset_default_button) gives the setting's RNA
default; Reset Nodes (Node Wrangler's Backspace over a selected node) makes a new node, which runs
the node's init. Both must give the same values (2026-11-12: Explode or Implode Impulse's Center
and Falloff, Map Range's Clamp, the list nodes' Change and 26 more reset to 0, and the Collision
Layers buttons to off, while new nodes start elsewhere).

Checked here for every logic node of the Add menu and every option of the merged nodes: each own
setting's RNA default is the value a new node (of that option) starts with, and each socket
property whose default is not read from the socket's declaration starts at its default. Defaults
given by a function (Drive Vehicle's Axis per option, the Collision Layers buttons per declared
mask) are invisible to Python: ~/Desktop/CPP_LN_TESTS/reset_defaults/backspace_check.py presses
Backspace over every setting in a window.

    blender --background --factory-startup --python tests/python/logic_nodes_reset_defaults.py
"""

import os
import sys
from pathlib import Path

import bpy

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts/startup/bl_ui"))
from node_add_menu_logic_catalog import LOGIC_NODE_PARITY_CATALOG  # noqa: E402

PROBLEMS = []
CHECKS = [0]

NODE_BASE = set(bpy.types.Node.bl_rna.properties.keys())
SOCKET_BASE = (set(bpy.types.NodeSocket.bl_rna.properties.keys()) |
               set(bpy.types.NodeSocketStandard.bl_rna.properties.keys()))

# Material pickers list the chosen Material's nodes and sockets: a new node shows None (-1) until a
# Material is chosen, then the first node / socket, which is what the default 0 resets to.
DYNAMIC_PICKERS = {"material_node", "material_socket"}
# Settings that choose a merged node's option reset to the first option, as Reset Nodes does.
OPTION_CHOOSERS = {"variant", "operation", "pass_when"}
# Defaults given by an RNA function (rna_LogicVehicle_axis_default): (node, option) -> setting.
FUNCTION_DEFAULTS = {("LogicNativeDriveVehicle", "STEER"): {"vehicle_axis"}}
# Socket types whose default_value resets to the socket's declared default
# (rna_NodeSocketStandard_*_default), which is what a new node starts with.
DECLARED_DEFAULT_SOCKETS = {
    "NodeSocketLogicBool", "NodeSocketLogicCondition", "NodeSocketLogicInt",
    "NodeSocketLogicFloat", "NodeSocketLogicString", "NodeSocketLogicVector",
    "NodeSocketLogicVectorXYAngle", "NodeSocketLogicColor", "NodeSocketLogicCollisionLayers",
    "NodeSocketFloat", "NodeSocketFloatAngle", "NodeSocketFloatFactor", "NodeSocketFloatDistance",
    "NodeSocketInt", "NodeSocketBool", "NodeSocketVector", "NodeSocketColor", "NodeSocketString",
}


def check(condition, text):
    CHECKS[0] += 1
    if not condition:
        PROBLEMS.append(text)


def is_array(prop):
    return getattr(prop, "is_array", False) or getattr(prop, "array_length", 0) > 0


def rna_default(prop):
    if prop.type == 'ENUM':
        return set(prop.default_flag) if prop.is_enum_flag else prop.default
    if is_array(prop):
        return tuple(prop.default_array)
    return prop.default


def value_of(owner, prop):
    value = getattr(owner, prop.identifier)
    if prop.type == 'ENUM' and prop.is_enum_flag:
        return set(value)
    if is_array(prop):
        return tuple(value)
    return value


def same(a, b):
    if isinstance(a, tuple) and isinstance(b, tuple):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float) or isinstance(b, float):
        return abs(float(a) - float(b)) <= 1.0e-6
    return a == b


def check_settings(node, label, skip):
    for prop in node.bl_rna.properties:
        name = prop.identifier
        if (name in NODE_BASE or name in skip or prop.is_readonly or
                prop.type in {'POINTER', 'COLLECTION'}):
            continue
        default = rna_default(prop)
        start = value_of(node, prop)
        if prop.type == 'ENUM' and not prop.is_enum_flag:
            check(default in {item.identifier for item in prop.enum_items},
                  f"{label}.{name}: its RNA default {default!r} is none of its items")
        check(same(default, start),
              f"{label}.{name}: Backspace gives {default!r}, a new node starts {start!r} "
              f"(add RNA_def_property_*_default in rna_nodetree.cc)")


def check_sockets(node, label):
    for socket in node.inputs:
        for prop in socket.bl_rna.properties:
            name = prop.identifier
            if name in SOCKET_BASE or prop.is_readonly or prop.type == 'COLLECTION':
                continue
            where = f"{label} input {socket.identifier!r}.{name}"
            if prop.type == 'POINTER':
                check(getattr(socket, name) is None, f"{where}: a new node starts with a data-block")
                continue
            if name == "default_value" and socket.bl_idname in DECLARED_DEFAULT_SOCKETS:
                continue
            if name.startswith("collision_layer_"):
                # rna_NodeSocketLogicCollisionLayers_layer_default: the declared mask's bit.
                check(prop.default is True, f"{where}: the static default is not on (1023)")
                continue
            check(same(rna_default(prop), value_of(socket, prop)),
                  f"{where}: Backspace gives {rna_default(prop)!r}, a new node starts "
                  f"{value_of(socket, prop)!r}")


tree = bpy.data.node_groups.new("_ResetDefaults", "LogicNodeTree")
nodes_checked = 0
for entry in LOGIC_NODE_PARITY_CATALOG.values():
    idname = entry["idname"]
    try:
        node = tree.nodes.new(idname)
    except RuntimeError as exc:
        check(False, f"{idname}: cannot be added: {exc}")
        continue
    nodes_checked += 1
    check_settings(node, idname, DYNAMIC_PICKERS)
    check_sockets(node, idname)
    if "variant" in node.bl_rna.properties:
        for item in list(node.bl_rna.properties["variant"].enum_items)[1:]:
            option = tree.nodes.new(idname)
            option.variant = item.identifier
            label = f"{idname}[{item.identifier}]"
            skip = DYNAMIC_PICKERS | OPTION_CHOOSERS | FUNCTION_DEFAULTS.get(
                (idname, item.identifier), set())
            check_settings(option, label, skip)
            check_sockets(option, label)
            tree.nodes.remove(option)
    tree.nodes.remove(node)

check(nodes_checked > 250, f"only {nodes_checked} logic nodes were added")
# The node the user found it with (2026-11-12).
blast = tree.nodes.new("LogicNativeExplosionImpulse")
check((blast.center, blast.falloff, blast.direction) == ('OBJECT', 'LINEAR', 'EXPLODE') and
      blast.inputs["Mask"].default_value == 1023,
      "Explode or Implode Impulse no longer starts Object, Linear, Explode, every layer")

print("NODES", nodes_checked)
print("CHECKS", CHECKS[0])
print("PROBLEMS (%d):" % len(PROBLEMS))
for problem in PROBLEMS:
    print("  " + problem)
sys.stdout.flush()
os._exit(1 if PROBLEMS else 0)
