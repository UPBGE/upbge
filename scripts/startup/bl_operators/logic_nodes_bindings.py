# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Shared helpers for native C++ Logic Nodes object bindings."""

import bpy


def logic_nodes_available():
    if not hasattr(bpy.types, "LogicNodeTree"):
        return False
    game_settings = getattr(bpy.types, "GameObjectSettings", None)
    if game_settings is None:
        return False
    return "logic_node_bindings" in game_settings.bl_rna.properties


def logic_nodes_tree_from_space(space):
    """Return the LogicNodeTree in a node editor space, or None.

    Does not read ``space.tree_type`` so invalid ``tree_idname`` values do not
    spam RNA warnings when the enum maps to -1.
    """
    if space is None or space.type != 'NODE_EDITOR':
        return None
    tree = getattr(space, "edit_tree", None) or getattr(space, "node_tree", None)
    if tree is not None and getattr(tree, "bl_idname", None) == "LogicNodeTree":
        return tree
    return None


def logic_nodes_is_logic_editor_space(space):
    """True when the space is editing a native LogicNodeTree."""
    if not logic_nodes_available():
        return False
    return logic_nodes_tree_from_space(space) is not None


def node_editor_active_tree_bl_idname(space):
    """Return bl_idname of the open node tree without reading ``space.tree_type``."""
    if space is None or space.type != 'NODE_EDITOR':
        return None
    tree = getattr(space, "edit_tree", None) or getattr(space, "node_tree", None)
    if tree is None:
        return None
    return getattr(tree, "bl_idname", None)


def logic_nodes_has_bindings(ob):
    if not ob or not ob.game:
        return False
    return len(ob.game.logic_node_bindings) > 0


def logic_nodes_filter_tree(_self, item):
    return item.bl_idname == "LogicNodeTree"


def logic_nodes_refresh_objects(objects):
    """Ensure depsgraph picks up binding changes."""
    updated_ids = set()
    for ob in objects:
        if ob is None:
            continue
        ob.update_tag()
        updated_ids.add(ob)
    if updated_ids:
        view_layer = bpy.context.view_layer
        if view_layer is not None:
            view_layer.update()


def logic_nodes_purge_invalid_bindings(ob):
    """Remove stale bindings left by corrupted copies (empty tree name)."""
    if not ob or not ob.game:
        return
    for index in range(len(ob.game.logic_node_bindings) - 1, -1, -1):
        binding = ob.game.logic_node_bindings[index]
        if not (binding.tree_name or "").strip():
            ob.game.logic_node_binding_remove(index)


def logic_nodes_binding_enabled_icon(bindings):
    """Return addon-style checkbox icon for a set of bindings on selected objects."""
    if not bindings:
        return 'CHECKBOX_DEHLT'
    enabled = None
    for binding in bindings:
        state = bool(binding.enabled)
        if enabled is None:
            enabled = state
        elif enabled != state:
            return 'QUESTION'
    return 'CHECKBOX_HLT' if enabled else 'CHECKBOX_DEHLT'


def logic_nodes_binding_status(context, binding):
    if binding is None:
        return "Invalid binding", 'ERROR'

    tree_name = (binding.tree_name or "").strip()
    if not tree_name:
        return "No tree selected", 'INFO'

    tree = bpy.data.node_groups.get(tree_name)
    if tree is None:
        return "Missing tree", 'ERROR'
    if tree.bl_idname != "LogicNodeTree":
        return "Selected tree is not a LogicNodeTree", 'ERROR'
    if not binding.enabled:
        return "", 'NONE'

    game_settings = context.scene.game_settings
    if game_settings.physics_engine != 'JOLT':
        return "Skipped: Jolt physics required", 'ERROR'
    if not game_settings.use_fixed_physics_timestep:
        return "Skipped: fixed timestep required", 'ERROR'

    return "", 'NONE'


def logic_nodes_object_has_tree(ob, tree_name):
    if not ob or not ob.game or not tree_name:
        return False
    for binding in ob.game.logic_node_bindings:
        if binding.tree_name == tree_name:
            return True
    return False


def logic_nodes_apply_tree_to_objects(tree, objects, initial_enabled=True):
    if tree is None or tree.bl_idname != "LogicNodeTree":
        return

    tree.use_fake_user = True
    touched = []
    for ob in objects:
        if ob is None or not ob.game:
            continue

        logic_nodes_purge_invalid_bindings(ob)
        if logic_nodes_object_has_tree(ob, tree.name):
            for binding in ob.game.logic_node_bindings:
                if binding.tree_name == tree.name:
                    binding.enabled = initial_enabled
                    break
            touched.append(ob)
            continue

        binding = ob.game.logic_node_binding_new()
        binding.tree = tree
        binding.tree_name = tree.name
        binding.enabled = initial_enabled
        touched.append(ob)

    logic_nodes_refresh_objects(touched)


def logic_nodes_remove_tree_from_object(ob, tree_name):
    if not ob or not ob.game or not tree_name:
        return False

    logic_nodes_purge_invalid_bindings(ob)
    removed = False
    for index, binding in enumerate(list(ob.game.logic_node_bindings)):
        if binding.tree_name == tree_name:
            ob.game.logic_node_binding_remove(index)
            removed = True
    if removed:
        logic_nodes_refresh_objects([ob])
    return removed


def logic_nodes_clear_bindings(ob):
    if not ob or not ob.game:
        return

    ob.game.logic_node_binding_clear()

    logic_nodes_refresh_objects([ob])


def logic_nodes_selected_objects(context):
    return [ob for ob in context.scene.objects if ob.select_get()]


def logic_nodes_get_context_tree(context):
    """Return the LogicNodeTree being edited, even when invoked outside the node editor."""
    if context is None:
        return None

    tree = logic_nodes_tree_from_space(getattr(context, "space_data", None))
    if tree is not None:
        return tree

    screen = getattr(context, "screen", None)
    if screen is None:
        return None

    current_area = getattr(context, "area", None)
    fallback = None
    for area in screen.areas:
        if area.type != 'NODE_EDITOR':
            continue
        for space in area.spaces:
            candidate = logic_nodes_tree_from_space(space)
            if candidate is None:
                continue
            if area == current_area:
                return candidate
            if fallback is None:
                fallback = candidate
    return fallback


def logic_nodes_set_node_editor_tree(context, tree_name):
    tree = bpy.data.node_groups.get(tree_name)
    if tree is None or tree.bl_idname != "LogicNodeTree":
        return False

    screen = getattr(context, "screen", None)
    if screen is None:
        return False

    for area in screen.areas:
        if area.type != 'NODE_EDITOR':
            continue
        space = area.spaces.active
        if space is None:
            continue
        space.tree_type = 'LogicNodeTree'
        space.node_tree = tree
        return True
    return False
