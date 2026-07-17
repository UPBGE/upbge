# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify Apply To Selected works when invoked from Properties context."""

import bpy
from bl_operators.logic_nodes_bindings import (
    logic_nodes_get_context_tree,
    logic_nodes_object_has_tree,
)


def main():
    bpy.ops.wm.read_homefile(use_empty=True)

    tree = bpy.data.node_groups.new("ApplyTestTree", "LogicNodeTree")
    cube = bpy.data.objects.new("ApplyTestCube", None)
    bpy.context.collection.objects.link(cube)
    cube.select_set(True)
    bpy.context.view_layer.objects.active = cube

    # Use any area as a temporary Logic Node Editor.
    screen = bpy.context.window.screen
    area = screen.areas[0]
    area.type = 'NODE_EDITOR'
    space = area.spaces.active
    space.tree_type = 'LogicNodeTree'
    space.node_tree = tree

    # Properties-style context: active area is not the node editor.
    props_area = None
    for candidate in screen.areas:
        if candidate.type == 'PROPERTIES':
            props_area = candidate
            break
    if props_area is None:
        props_area = screen.areas[-1]
        props_area.type = 'PROPERTIES'

    props_context = bpy.context.temp_override(
        window=bpy.context.window,
        screen=screen,
        area=props_area,
        region=props_area.regions[-1],
    )
    with props_context:
        found = logic_nodes_get_context_tree(bpy.context)
        assert found is not None, "Expected to find LogicNodeTree from screen areas"
        assert found.name == "ApplyTestTree", found.name

        result = bpy.ops.object.logic_nodes_apply_tree()
        assert result == {'FINISHED'}, result

    assert logic_nodes_object_has_tree(cube, "ApplyTestTree"), "Tree was not applied to cube"
    print("PASS: apply from properties context")


if __name__ == "__main__":
    main()
