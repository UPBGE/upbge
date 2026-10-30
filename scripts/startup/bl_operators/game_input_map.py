# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import Operator
from bpy.props import EnumProperty


def _editable_scene(context):
    scene = context.scene
    return scene is not None and scene.library is None


def _active_action(context):
    gm = context.scene.game_settings
    index = gm.active_input_action_index
    return gm.input_actions[index] if 0 <= index < len(gm.input_actions) else None


class SCENE_OT_game_input_action_add(Operator):
    """Add an action of the game to the Input Map"""
    bl_idname = "scene.game_input_action_add"
    bl_label = "Add Input Action"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return _editable_scene(context)

    def execute(self, context):
        context.scene.game_settings.input_actions.new(name="Action")
        return {'FINISHED'}


class SCENE_OT_game_input_action_remove(Operator):
    """Remove the active action of the game from the Input Map"""
    bl_idname = "scene.game_input_action_remove"
    bl_label = "Remove Input Action"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        if not _editable_scene(context):
            return False
        action = _active_action(context)
        if action is None:
            return False
        if action.is_ui_control:
            cls.poll_message_set("UI controls cannot be removed")
            return False
        return True

    def execute(self, context):
        context.scene.game_settings.input_actions.remove(_active_action(context))
        return {'FINISHED'}


class SCENE_OT_game_input_binding_add(Operator):
    """Add a key, mouse button or gamepad control to the active input action"""
    bl_idname = "scene.game_input_binding_add"
    bl_label = "Add Binding"
    bl_options = {'REGISTER', 'UNDO'}

    device: EnumProperty(
        name="Device",
        items=(
            ('KEYBOARD', "Keyboard", "Add a key"),
            ('MOUSE', "Mouse", "Add a mouse button"),
            ('GAMEPAD', "Gamepad", "Add a gamepad control"),
        ),
    )

    @classmethod
    def poll(cls, context):
        return _editable_scene(context) and _active_action(context) is not None

    def execute(self, context):
        _active_action(context).bindings.new(device=self.device)
        return {'FINISHED'}


class SCENE_OT_game_input_binding_remove(Operator):
    """Remove the active binding of the active input action"""
    bl_idname = "scene.game_input_binding_remove"
    bl_label = "Remove Binding"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        if not _editable_scene(context):
            return False
        action = _active_action(context)
        return action is not None and 0 <= action.active_binding_index < len(action.bindings)

    def execute(self, context):
        action = _active_action(context)
        action.bindings.remove(action.bindings[action.active_binding_index])
        return {'FINISHED'}


class SCENE_OT_game_input_map_reset(Operator):
    """Give the UI controls and the input settings their defaults again. """ \
        """The game's own actions stay"""
    bl_idname = "scene.game_input_map_reset"
    bl_label = "Reset UI Controls"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return _editable_scene(context)

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, context):
        context.scene.game_settings.input_actions.reset_ui_controls()
        return {'FINISHED'}


classes = (
    SCENE_OT_game_input_action_add,
    SCENE_OT_game_input_action_remove,
    SCENE_OT_game_input_binding_add,
    SCENE_OT_game_input_binding_remove,
    SCENE_OT_game_input_map_reset,
)
