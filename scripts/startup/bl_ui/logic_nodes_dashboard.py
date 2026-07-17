# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Panel

from bl_operators.logic_nodes_bindings import (
    logic_nodes_binding_enabled_icon,
    logic_nodes_binding_status,
    logic_nodes_is_logic_editor_space,
    logic_nodes_object_has_tree,
    logic_nodes_selected_objects,
)


class NODE_PT_logic_nodes_administration(Panel):
    bl_label = "Administration"
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Dashboard"

    @classmethod
    def poll(cls, context):
        return logic_nodes_is_logic_editor_space(context.space_data)

    def draw(self, context):
        layout = self.layout
        box = layout.box()
        col = box.column()
        col.scale_y = 1.4
        col.operator("object.logic_nodes_apply_tree", text="Apply To Selected", icon='PREFERENCES')


class NODE_PT_logic_nodes_object_trees(Panel):
    bl_label = "Object Trees"
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Dashboard"

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        if ob is None or not ob.select_get():
            return False
        return logic_nodes_is_logic_editor_space(context.space_data)

    def draw(self, context):
        layout = self.layout
        selected_objects = logic_nodes_selected_objects(context)
        active_tree_items = {}

        box_over = layout.box()
        title = box_over.row()
        for ob in selected_objects:
            for binding in ob.game.logic_node_bindings:
                tree_name = (binding.tree_name or "").strip()
                if not tree_name:
                    continue
                active_tree_items.setdefault(tree_name, []).append((ob, binding))

        tree_count = len(active_tree_items)
        if context.object:
            title.label(text=f"Trees applied to {context.object.name}: {tree_count}")

        if tree_count == 0:
            box_over.label(text="No logic trees applied", icon='INFO')
            return

        for tree_name in sorted(active_tree_items):
            entries = active_tree_items[tree_name]
            box = box_over.box()
            bindings = [binding for _ob, binding in entries]

            row = box.row(align=False)
            row.label(text="", icon=logic_nodes_binding_enabled_icon(bindings))
            row.label(text=tree_name, icon='NODETREE')
            row.operator("object.logic_nodes_unapply_tree", text="", icon="X").tree_name = tree_name

            row = box.row(align=False)
            row.operator("object.logic_nodes_find_tree", text="Edit", icon="NODETREE").tree_name = tree_name

            status_text, status_icon = logic_nodes_binding_status(context, bindings[0])
            if status_text:
                box.label(text=status_text, icon=status_icon)


class NODE_PT_logic_nodes_tree_applied_to(Panel):
    bl_label = "Tree applied to:"
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Dashboard"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return (
            logic_nodes_is_logic_editor_space(space)
            and getattr(space, "edit_tree", None) is not None
        )

    def draw_owner(self, layout, obj, binding, tree_name):
        box = layout.box()
        row = box.split(factor=0.65)
        name = row.row()
        name.alignment = 'LEFT'
        name.prop(binding, "enabled", text="")
        name.label(text=obj.name)

        buttons = row.row(align=True)
        buttons.alignment = 'RIGHT'
        buttons.operator(
            "object.logic_nodes_select_owner",
            text="",
            icon="RESTRICT_SELECT_OFF",
        ).applied_object = obj.name
        op = buttons.operator("object.logic_nodes_unapply_tree", text="", icon="X")
        op.tree_name = tree_name
        op.from_object = obj.name

        status_text, status_icon = logic_nodes_binding_status(bpy.context, binding)
        if status_text:
            box.label(text=status_text, icon=status_icon)

    def draw(self, context):
        layout = self.layout
        tree = context.space_data.edit_tree
        container = layout.column(align=True)
        found = False

        for obj in bpy.data.objects:
            if obj.name not in context.view_layer.objects:
                continue
            if not obj.game:
                continue
            if not logic_nodes_object_has_tree(obj, tree.name):
                continue
            for binding in obj.game.logic_node_bindings:
                if binding.tree_name == tree.name:
                    self.draw_owner(container, obj, binding, tree.name)
                    found = True
                    break

        if not found:
            container.label(text="Not applied to any object", icon='INFO')


classes = (
    NODE_PT_logic_nodes_administration,
    NODE_PT_logic_nodes_object_trees,
    NODE_PT_logic_nodes_tree_applied_to,
)

if __name__ == "__main__":
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
