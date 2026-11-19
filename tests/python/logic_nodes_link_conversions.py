# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Logic Nodes link conversions in the editor (2026-11-16): a link between real nodes is drawn
without red exactly for the pairs the engine converts by itself (as Geometry Nodes converts):
true/false and numbers into each other and into a vector; a vector into true/false, a rotation or
a color; a rotation into a vector. A vector into a number, a color into anything else and a
rotation into anything but a vector stay red. Flow links (white diamonds, boolean sockets) stay
red into data inputs and from data outputs. The compiler's side is the gtest
LN_ImplicitConversion.*.

  blender --background --factory-startup --python logic_nodes_link_conversions.py
"""

import sys

import bpy

LOGIC_OPTIONS = None


def logic_options():
    """Former idname -> (merged idname, option identifier)."""
    global LOGIC_OPTIONS
    if LOGIC_OPTIONS is None:
        LOGIC_OPTIONS = {}
        tree = bpy.data.node_groups.new("__logic_options", "LogicNodeTree")
        for name in dir(bpy.types):
            rna = getattr(getattr(bpy.types, name, None), "bl_rna", None)
            if (not name.startswith("LogicNative") or rna is None or
                    "variant_idname" not in rna.properties):
                continue
            node = tree.nodes.new(name)
            for item in rna.properties["variant"].enum_items:
                node.variant = item.identifier
                LOGIC_OPTIONS[node.variant_idname] = (name, item.identifier)
        bpy.data.node_groups.remove(tree)
    return LOGIC_OPTIONS


def new_logic_node(nodes, idname):
    option = logic_options().get(idname)
    if option is None:
        return nodes.new(idname)
    node = nodes.new(option[0])
    node.variant = option[1]
    return node


def by_identifier(sockets, identifier):
    return next(socket for socket in sockets if socket.identifier == identifier)


# Outputs: (kind, node idname, output identifier, settings).
SOURCES = (
    ("bool", "LogicNativeGetGamePropertyBool", "Value", {}),
    ("int", "LogicNativeGetGamePropertyInt", "Value", {}),
    ("float", "LogicNativeGetGamePropertyFloat", "Value", {}),
    ("vector", "LogicNativeCombineXYZ", "Vector", {}),
    ("rotation", "LogicNativeEuler", "Rotation", {}),
    ("color", "LogicNativeColorRGBA", "Color", {}),
)
# Inputs: (kind, node idname, input identifier, settings).
TARGETS = (
    ("bool", "LogicNativeSetGamePropertyBool", "Value", {}),
    ("int", "LogicNativeSetGamePropertyInt", "Value", {}),
    ("float", "LogicNativeSetGamePropertyFloat", "Value", {}),
    ("vector", "LogicNativeSeparateXYZ", "Vector", {}),
    ("rotation", "LogicNativeSeparateEuler", "Rotation", {}),
    ("color", "LogicNativeSetObjectAttribute", "Color", {"attribute_type": 'COLOR'}),
    ("condition", "LogicNativeBranch", "Condition", {}),
)
CONVERTS = {
    "bool": {"int", "float", "vector"},
    "int": {"bool", "float", "vector", "condition"},
    "float": {"bool", "int", "vector", "condition"},
    "vector": {"bool", "rotation", "color", "condition"},
    "rotation": {"vector"},
    "color": set(),
}


def make(tree, idname, settings):
    node = new_logic_node(tree.nodes, idname)
    for key, value in settings.items():
        setattr(node, key, value)
    return node


def check_flow_links(tree, errors):
    """Flow sockets (white diamonds) are boolean sockets, so only the socket rule
    (link_socket_error) keeps flow links out of data inputs and data out of flow inputs. Flow into
    flow stays valid, through a reroute too."""
    nodes, links = tree.nodes, tree.links
    on_init = new_logic_node(nodes, "LogicNativeOnInit")
    branch = new_logic_node(nodes, "LogicNativeBranch")
    printer = new_logic_node(nodes, "LogicNativePrint")
    combine = new_logic_node(nodes, "LogicNativeCombineXYZ")
    compare = new_logic_node(nodes, "LogicNativeCompare")
    get_float = new_logic_node(nodes, "LogicNativeGetGamePropertyFloat")
    get_bool = new_logic_node(nodes, "LogicNativeGetGamePropertyBool")
    flow_out = on_init.outputs["Out"]
    cases = (
        ("flow -> Branch Condition", flow_out, by_identifier(branch.inputs, "Condition"), False),
        ("flow -> Combine XYZ X", flow_out, combine.inputs["X"], False),
        ("flow -> Print Message", flow_out, by_identifier(printer.inputs, "Message"), False),
        ("decimal -> Branch Flow", by_identifier(get_float.outputs, "Value"),
         by_identifier(branch.inputs, "Flow"), False),
        ("true/false -> Print Flow", by_identifier(get_bool.outputs, "Value"),
         by_identifier(printer.inputs, "Flow"), False),
        ("flow -> Print Flow", flow_out, by_identifier(printer.inputs, "Flow"), True),
        ("Compare Result -> Branch Condition", by_identifier(compare.outputs, "Result"),
         by_identifier(branch.inputs, "Condition"), True),
    )
    checked = 0
    for name, output, inp, expected in cases:
        link = links.new(output, inp)
        tree.update_tag()
        bpy.context.view_layer.update()
        if link.is_valid != expected:
            errors.append(f"{name}: drawn {'valid' if link.is_valid else 'red'}, expected "
                          f"{'valid' if expected else 'red'}")
        links.remove(link)
        checked += 1
    # Through a reroute: the reroute takes the flow socket's type.
    for name, inp, expected in (("flow -> reroute -> Print Flow",
                                 by_identifier(printer.inputs, "Flow"), True),
                                ("flow -> reroute -> Combine XYZ X", combine.inputs["X"], False)):
        reroute = nodes.new("NodeReroute")
        links.new(flow_out, reroute.inputs[0])
        link = links.new(reroute.outputs[0], inp)
        tree.update_tag()
        bpy.context.view_layer.update()
        if link.is_valid != expected:
            errors.append(f"{name}: drawn {'valid' if link.is_valid else 'red'}, expected "
                          f"{'valid' if expected else 'red'}")
        nodes.remove(reroute)
        checked += 1
    for node in (on_init, branch, printer, combine, compare, get_float, get_bool):
        nodes.remove(node)
    return checked


def main():
    errors = []
    tree = bpy.data.node_groups.new("LinkConversions", "LogicNodeTree")
    checked = 0
    for source_kind, source_idname, output, source_settings in SOURCES:
        for target_kind, target_idname, inp, target_settings in TARGETS:
            same = source_kind == target_kind or (source_kind, target_kind) == ("bool",
                                                                                 "condition")
            if same:
                continue
            source = make(tree, source_idname, source_settings)
            target = make(tree, target_idname, target_settings)
            link = tree.links.new(by_identifier(source.outputs, output),
                                  by_identifier(target.inputs, inp))
            bpy.context.view_layer.update()
            tree.update_tag()
            bpy.context.view_layer.update()
            expected = target_kind in CONVERTS[source_kind]
            if link.is_valid != expected:
                errors.append(f"{source_kind} -> {target_kind}: drawn "
                              f"{'valid' if link.is_valid else 'red'}, expected "
                              f"{'valid' if expected else 'red'}")
            checked += 1
            tree.nodes.remove(source)
            tree.nodes.remove(target)
    # The case that asked for it: Random Value (Boolean) straight into Combine XYZ.
    random = tree.nodes.new("LogicNativeRandomValue")
    random.data_type = 'BOOLEAN'
    combine = tree.nodes.new("LogicNativeCombineXYZ")
    link = tree.links.new(by_identifier(random.outputs, "Value_Bool"), combine.inputs["X"])
    bpy.context.view_layer.update()
    if not link.is_valid:
        errors.append("Random Value (Boolean) -> Combine XYZ X is red")
    # Still red: a vector into a number (which axis?), a color into a vector.
    for source_kind, target_kind in (("vector", "float"), ("color", "vector")):
        if target_kind in CONVERTS[source_kind]:
            errors.append(f"{source_kind} -> {target_kind} must stay refused")
    checked += check_flow_links(tree, errors)
    print(f"LINKS checked {checked} pairs")
    if errors:
        print("LINK CONVERSIONS FAILED:\n  " + "\n  ".join(errors))
        sys.exit(1)
    print("LINK CONVERSIONS OK")


if __name__ == "__main__":
    main()
