# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Add > Game Navigation Examples (2026-11-20, the navigation ease-of-use plan's item 14; named
so 2026-11-21): the 3D view's Add menu gets a menu of ready NPC behaviours and level pieces
(_bpy_internal.navigation.templates builds them). A startup module: Blender loads it and calls
register(); no Blender file lists it, and the menu is appended to the Add menu.
"""

import os

import bpy
from bpy.props import EnumProperty, FloatProperty, StringProperty
from bpy.types import Menu, Operator


def _templates():
    from _bpy_internal.navigation import templates
    return templates


# (A fixed list: Blender keeps no reference to the strings of a list an items function returns.)
_TEMPLATE_ITEMS = [(t[0], t[1], t[3], t[2], i) for i, t in enumerate(_templates().TEMPLATES)]


class OBJECT_OT_navigation_template_add(Operator):
    """Add a ready navigation behaviour or level piece"""
    bl_idname = "object.navigation_template_add"
    bl_label = "Add Navigation Template"
    bl_options = {'REGISTER', 'UNDO'}

    template: EnumProperty(name="Template", items=_TEMPLATE_ITEMS)
    target: StringProperty(
        name="Target",
        description="The object it goes after (the player): chasers, a horde, a companion and "
        "a cover soldier need one",
    )
    height: FloatProperty(
        name="Height",
        description="A ladder's or a lift's height (to the floor above)",
        default=4.0,
        min=1.0,
        soft_max=30.0,
        subtype='DISTANCE',
    )

    @classmethod
    def description(cls, _context, properties):
        for t in _templates().TEMPLATES:
            if t[0] == properties.template:
                return t[3]
        return cls.__doc__

    def _default_target(self, context):
        """A Character not selected (the player, most likely), else any object called Player."""
        chosen = set(context.selected_objects)
        for ob in context.scene.objects:
            if ob not in chosen and ob.game.physics_type == 'CHARACTER':
                return ob.name
        player = context.scene.objects.get("Player")
        return player.name if player is not None else ""

    def invoke(self, context, event):
        templates = _templates()
        if self.template in templates.NEEDS_TARGET:
            if not self.target:
                self.target = self._default_target(context)
            return context.window_manager.invoke_props_dialog(self)
        if self.template in {'LADDER', 'LIFT'}:
            return context.window_manager.invoke_props_dialog(self)
        return self.execute(context)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        templates = _templates()
        if self.template in templates.NEEDS_TARGET:
            layout.prop_search(self, "target", context.scene, "objects")
        if self.template in {'LADDER', 'LIFT'}:
            layout.prop(self, "height")

    def execute(self, context):
        templates = _templates()
        target = None
        if self.template in templates.NEEDS_TARGET:
            target = context.scene.objects.get(self.target)
            if target is None:
                self.report({'ERROR'}, "Choose a target object (the player it goes after)")
                return {'CANCELLED'}
        try:
            tree, made = templates.add(context, self.template, target=target, height=self.height)
        except templates.TemplateError as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        if made:
            context.view_layer.objects.active = made[0]
        for note in templates.scene_notes(context.scene):
            self.report({'WARNING'}, note)
        if tree is not None:
            self.report({'INFO'}, "Logic tree \"%s\" made and bound" % tree.name)
        return {'FINISHED'}


class VIEW3D_MT_navigation_add(Menu):
    # (Named for what it holds: ready examples to build on, 2026-11-21, the user's call.)
    bl_label = "Game Navigation Examples"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, _context):
        layout = self.layout
        layout.operator_context = 'INVOKE_REGION_WIN'
        templates = _templates()
        for i, (identifier, text, icon, _description, target, objects) in \
                enumerate(templates.TEMPLATES):
            if objects and not templates.TEMPLATES[i - 1][5]:
                layout.separator()
            label = text + ("..." if target or identifier in {'LADDER', 'LIFT'} else "")
            layout.operator(OBJECT_OT_navigation_template_add.bl_idname,
                            text=label, icon=icon).template = identifier


def _draw_add_menu(self, _context):
    self.layout.menu(VIEW3D_MT_navigation_add.__name__, icon='CON_FOLLOWPATH')


classes = (
    OBJECT_OT_navigation_template_add,
    VIEW3D_MT_navigation_add,
)


def _is_player():
    return os.path.basename(bpy.app.binary_path or "").lower().startswith("blenderplayer") or \
        not hasattr(bpy.types, "VIEW3D_MT_add")


def register():
    if _is_player() or not hasattr(bpy.types, "LogicNodeTree"):
        return
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.VIEW3D_MT_add.append(_draw_add_menu)


def unregister():
    if not hasattr(bpy.types, VIEW3D_MT_navigation_add.__name__):
        return
    bpy.types.VIEW3D_MT_add.remove(_draw_add_menu)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
