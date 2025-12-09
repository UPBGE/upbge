# SPDX-FileCopyrightText: 2013-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Operator
from bpy.props import BoolProperty, EnumProperty, FloatProperty


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
            ('CUSTOM_DISTANCE', "Custom Distance", "Connect objects that are within a custom distance"),
        ),
        default='SELECTED_TO_ACTIVE',
    )
    use_cursor: BoolProperty(
        name="Place on 3D Cursor",
        description="Place the generated constraint at the 3D cursor location",
        default=False,
    )
    connect_distance: FloatProperty(
        name="Connect Distance",
        description="Maximum distance within which rigid bodies are connected (Custom Distance)",
        default=2.0,
        min=0.0,
        soft_max=20.0,
    )

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        layout.prop(self, "con_type")

        row = layout.row()
        row.enabled = not self.use_cursor
        row.use_property_split = True
        row.prop(self, "pivot_type")

        row = layout.row()
        row.enabled = not self.use_cursor
        row.use_property_split = True
        row.prop(self, "connection_pattern")

        row = layout.row()
        row.enabled = (self.connection_pattern != 'CUSTOM_DISTANCE')
        row.use_property_split = True
        row.prop(self, "use_cursor")

        if self.connection_pattern == 'CUSTOM_DISTANCE':
            layout.prop(self, "connect_distance")

    @classmethod
    def poll(cls, context):
        # Game engine allows connecting objects that are not yet rigid bodies.
        # Allow running as long as there is at least one selected object.
        return bool(context.selected_objects)

    @staticmethod
    def _is_no_collision(obj):
        game = getattr(obj, "game", None)
        return (game is not None and game.physics_type == 'NO_COLLISION')

    def _add_constraint(self, context, object1, object2):
        if object1 == object2:
            return
        if self._is_no_collision(object1) or self._is_no_collision(object2):
            return

        use_cursor = self.use_cursor and (self.connection_pattern != 'CUSTOM_DISTANCE')

        if use_cursor:
            loc = context.scene.cursor.location.copy()
        elif self.pivot_type == 'ACTIVE':
            loc = object1.location
        elif self.pivot_type == 'SELECTED':
            loc = object2.location
        else:
            loc = (object1.location + object2.location) / 2.0

        ob = bpy.data.objects.new("Constraint", object_data=None)
        ob.location = loc
        context.collection.objects.link(ob)
        context.view_layer.objects.active = ob
        ob.select_set(True)

        bpy.ops.rigidbody.constraint_add()
        con_obj = context.active_object
        con_obj.empty_display_type = 'ARROWS'
        con_obj.game.physics_type = 'NO_COLLISION'
        con = con_obj.rigid_body_constraint
        con.type = self.con_type

        con.object1 = object1
        con.object2 = object2

    def execute(self, context):
        view_layer = context.view_layer
        selected_all = list(context.selected_objects)
        objects = [obj for obj in selected_all if not self._is_no_collision(obj)]
        obj_act = context.active_object
        change = False

        if not objects:
            self.report({'WARNING'}, "No eligible objects (No Collision objects are skipped)")
            return {'CANCELLED'}

        # If no active object, promote the first selected one to active.
        if obj_act is None or self._is_no_collision(obj_act):
            obj_act = objects[0]
            view_layer.objects.active = obj_act

        if self.connection_pattern == 'CHAIN_DISTANCE':
            objs_sorted = [obj_act]
            objects_tmp = list(objects)
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

        elif self.connection_pattern == 'CUSTOM_DISTANCE':
            if len(objects) < 2:
                self.report({'WARNING'}, "Need at least two selected objects for Custom Distance")
                return {'CANCELLED'}

            dist_max = self.connect_distance
            for i in range(len(objects)):
                obj_a = objects[i]
                for j in range(i + 1, len(objects)):
                    obj_b = objects[j]
                    if (obj_a.location - obj_b.location).length <= dist_max:
                        self._add_constraint(context, obj_a, obj_b)
                        change = True

        else:  # SELECTED_TO_ACTIVE
            for obj in objects:
                self._add_constraint(context, obj_act, obj)
                change = True

        if change:
            # restore selection
            bpy.ops.object.select_all(action='DESELECT')
            for obj in selected_all:
                obj.select_set(True)
            view_layer.objects.active = obj_act
            return {'FINISHED'}
        else:
            if self.connection_pattern == 'CUSTOM_DISTANCE':
                self.report({'WARNING'}, "No object pairs found within the connect distance")
            else:
                self.report({'WARNING'}, "No other objects selected")
            return {'CANCELLED'}


classes = (
    ConnectRigidBodiesGame,
)
