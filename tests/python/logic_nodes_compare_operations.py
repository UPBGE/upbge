# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare's Operation follows its Type (2026-11-17): an Object lists only Equal and Not Equal
(objects are only the same or not: Greater and Less were always false), Number, Vector and Text
list all six, each with what it means for that type (Vector: lengths, Text: order). A Type
changed to Object turns Greater and Less into Equal. Evaluate Object Property keeps its list. The
engine's side (a node saved with Object and Greater warns) is the gtest
LN_RemovedObjects.CompareObjectsByOrderWarnsAndIsFalse.

  blender --background --factory-startup --python logic_nodes_compare_operations.py
"""

import sys

import bpy

ALL = ["EQUAL", "NOT_EQUAL", "GREATER_THAN", "LESS_THAN", "GREATER_EQUAL", "LESS_EQUAL"]
failures = []


def check(ok, what):
    print("ok  " if ok else "BAD ", what)
    if not ok:
        failures.append(what)


def accepted(node, identifier):
    """Whether the Operation takes `identifier` for this node (its own list)."""
    before = node.operation
    try:
        node.operation = identifier
    except TypeError:
        return False
    node.operation = before
    return True


def description(node, identifier):
    return bpy.types.UILayout.enum_item_description(node, "operation", identifier)


def main():
    tree = bpy.data.node_groups.new("CompareOperations", "LogicNodeTree")
    node = tree.nodes.new("LogicNativeCompare")
    check(node.data_type == 'NUMBER' and node.operation == 'EQUAL', "a new Compare: Number, Equal")

    for data_type, allowed in (('NUMBER', ALL), ('VECTOR', ALL), ('TEXT', ALL),
                               ('OBJECT', ["EQUAL", "NOT_EQUAL"])):
        node.data_type = data_type
        node.operation = 'EQUAL'
        took = [identifier for identifier in ALL if accepted(node, identifier)]
        check(took == allowed, f"{data_type} lists {took}")

    node.data_type = 'VECTOR'
    check("longer" in description(node, "GREATER_THAN"), "Vector: Greater Than compares lengths")
    check("Tolerance" in description(node, "EQUAL"), "Vector: Equal is within Tolerance")
    node.data_type = 'TEXT'
    check("after" in description(node, "GREATER_THAN"), "Text: Greater Than is the order")
    check("capitals" in description(node, "EQUAL"), "Text: Equal counts capitals")
    node.data_type = 'OBJECT'
    check("same object" in description(node, "EQUAL"), "Object: Equal is the same object")
    node.data_type = 'NUMBER'
    check("tolerance" in description(node, "EQUAL"), "Number: Equal keeps its words")

    # A Type changed to Object turns Greater and Less into Equal, keeps Equal and Not Equal.
    for operation, expected in (('GREATER_THAN', 'EQUAL'), ('LESS_EQUAL', 'EQUAL'),
                                ('NOT_EQUAL', 'NOT_EQUAL'), ('EQUAL', 'EQUAL')):
        node.data_type = 'NUMBER'
        node.operation = operation
        node.data_type = 'OBJECT'
        check(node.operation == expected, f"{operation} becomes {node.operation} for Object")
    # Back to Number: the Operation stays what it is.
    node.data_type = 'TEXT'
    node.operation = 'LESS_THAN'
    node.data_type = 'NUMBER'
    check(node.operation == 'LESS_THAN', "Text to Number keeps Less Than")

    # Evaluate Object Property shares the Operation's list, unchanged.
    evaluate = None
    for idname in ("LogicNativeEvaluateObjectProperty", "LogicNativeEvaluateGameProperty"):
        if hasattr(bpy.types, idname):
            evaluate = tree.nodes.new(idname)
            break
    if evaluate is None:
        for name in dir(bpy.types):
            rna = getattr(getattr(bpy.types, name, None), "bl_rna", None)
            if name.startswith("LogicNative") and rna is not None and \
                    "operation" in rna.properties and "data_type" not in rna.properties and \
                    "GREATER_THAN" in [i.identifier for i in rna.properties["operation"].enum_items]:
                evaluate = tree.nodes.new(name)
                break
    check(evaluate is not None, "a node sharing the Operation list exists")
    if evaluate is not None:
        took = [identifier for identifier in ALL if accepted(evaluate, identifier)]
        check(took == ALL, f"{evaluate.bl_idname} keeps all six ({took})")

    bpy.data.node_groups.remove(tree)
    print("COMPARE OPERATIONS", "OK" if not failures else f"BAD {len(failures)}")
    sys.exit(1 if failures else 0)


main()
