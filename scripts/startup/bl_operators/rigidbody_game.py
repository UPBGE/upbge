# SPDX-FileCopyrightText: 2013-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Operator
from bpy.props import EnumProperty


class ConnectRigidBodiesGame(Operator):
    """Create rigid body constraints between selected objects (Game Engine)"""
    bl_idname = "rigidbody.connect_game"
    bl_label = "Connect Rigid Bodies (Game)"
    bl_options = {'REGISTER', 'UNDO'}

    con_type: EnumProperty(
        name="Type",
        description="Type of generated constraint",
        # XXX Would be nice to get icons too, but currently not possible ;)
        items=tuple(
            (e.identifier, e.name, e.description, e.value)
            for e in bpy.types.RigidBodyConstraint.bl_rna.properties["type"].enum_items
        ),
        default='FIXED',
    )
    pivot_type: EnumProperty(
        name="Location",
        description="Constraint pivot location",
        items=(
            ('CENTER', "Center", "Pivot location is between the constrained rigid bodies"),
            ('ACTIVE', "Active", "Pivot location is at the active object position"),
            ('SELECTED', "Selected", "Pivot location is at the selected object position"),
        ),
        default='CENTER',
    )
    connection_pattern: EnumProperty(
        name="Connection Pattern",
        description="Pattern used to connect objects",
        items=(
            ('SELECTED_TO_ACTIVE', "Selected to Active", "Connect selected objects to the active object"),
            ('CHAIN_DISTANCE', "Chain by Distance", "Connect objects as a chain based on distance, "
             "starting at the active object"),
        ),
        default='SELECTED_TO_ACTIVE',
    )

    @classmethod
    def poll(cls, context):
        obj = context.object
        # Game engine allows connecting objects that are not yet rigid bodies
        return (obj is not None)

    def _add_constraint(self, context, object1, object2):
        if object1 == object2:
            return

        if self.pivot_type == 'ACTIVE':
            loc = object1.location
        elif self.pivot_type == 'SELECTED':
            loc = object2.location
        else:
            loc = (object1.location + object2.location) / 2.0

        ob = bpy.data.objects.new("Constraint", object_data=None)
        ob.location = loc
        context.scene.collection.objects.link(ob)
        context.view_layer.objects.active = ob
        ob.select_set(True)

        bpy.ops.rigidbody.constraint_add()
        con_obj = context.active_object
        con_obj.empty_display_type = 'ARROWS'
        con = con_obj.rigid_body_constraint
        con.type = self.con_type

        con.object1 = object1
        con.object2 = object2

    def execute(self, context):
        view_layer = context.view_layer
        objects = context.selected_objects
        obj_act = context.active_object
        change = False

        if self.connection_pattern == 'CHAIN_DISTANCE':
            objs_sorted = [obj_act]
            objects_tmp = context.selected_objects
            try:
                objects_tmp.remove(obj_act)
            except ValueError:
                pass

            last_obj = obj_act

            while objects_tmp:
                objects_tmp.sort(key=lambda o: (last_obj.location - o.location).length)
                last_obj = objects_tmp.pop(0)
                objs_sorted.append(last_obj)

            for i in range(1, len(objs_sorted)):
                self._add_constraint(context, objs_sorted[i - 1], objs_sorted[i])
                change = True

        else:  # SELECTED_TO_ACTIVE
            for obj in objects:
                self._add_constraint(context, obj_act, obj)
                change = True

        if change:
            # restore selection
            bpy.ops.object.select_all(action='DESELECT')
            for obj in objects:
                obj.select_set(True)
            view_layer.objects.active = obj_act
            return {'FINISHED'}
        else:
            self.report({'WARNING'}, "No other objects selected")
            return {'CANCELLED'}


classes = (
    ConnectRigidBodiesGame,
)
