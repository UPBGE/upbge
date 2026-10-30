# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import Operator
from bpy.props import IntProperty


def _editable_scene(context):
    scene = context.scene
    return scene is not None and scene.library is None


def _active_rules(context):
    gm = context.scene.game_settings
    index = gm.active_inventory_rules_index
    return gm.inventory_rules[index] if 0 <= index < len(gm.inventory_rules) else None


class SCENE_OT_game_inventory_rules_add(Operator):
    """Add a set of Inventory Rules for the game's lists (List or Table game properties)"""
    bl_idname = "scene.game_inventory_rules_add"
    bl_label = "Add Inventory Rules"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return _editable_scene(context)

    def execute(self, context):
        context.scene.game_settings.inventory_rules.new(name="Rules")
        return {'FINISHED'}


class SCENE_OT_game_inventory_rules_remove(Operator):
    """Remove the active set of Inventory Rules; the lists that picked it follow the defaults"""
    bl_idname = "scene.game_inventory_rules_remove"
    bl_label = "Remove Inventory Rules"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return _editable_scene(context) and _active_rules(context) is not None

    def execute(self, context):
        context.scene.game_settings.inventory_rules.remove(_active_rules(context))
        return {'FINISHED'}


class OBJECT_OT_game_property_inventory_rules_new(Operator):
    """Make a set of Inventory Rules named after this list and let the list follow it"""
    bl_idname = "object.game_property_inventory_rules_new"
    bl_label = "New Inventory Rules"
    bl_options = {'REGISTER', 'UNDO'}

    index: IntProperty(name="Index", description="The list's place in the object's game properties",
                       min=0)

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return _editable_scene(context) and ob is not None and ob.library is None

    def execute(self, context):
        properties = context.active_object.game.properties
        if not 0 <= self.index < len(properties):
            return {'CANCELLED'}
        prop = properties[self.index]
        if prop.type not in {'LIST', 'TABLE'}:
            self.report({'ERROR'}, "Only a List or Table game property follows Inventory Rules")
            return {'CANCELLED'}
        rules = context.scene.game_settings.inventory_rules.new(name=prop.name)
        prop.inventory_rules = rules.name
        return {'FINISHED'}


classes = (
    SCENE_OT_game_inventory_rules_add,
    SCENE_OT_game_inventory_rules_remove,
    OBJECT_OT_game_property_inventory_rules_new,
)
