# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTIBILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

bl_info = {
    "name" : "UPBGE Speedy Pivots",
    "author" : "vuaieo", 
    "description" : "faster setup of rigid body joints constraints",
    "blender" : (3, 6, 1),
    "version" : (3, 6, 1),
    "location" : "press Shift + V in 3D viewport. additionally in Properties / object constraints > rigid body joint constraints",
    "warning" : "if you copy the objects and constrains and run Converter then the old constraints will not be overwritten. so delete them before converting...",
    "doc_url": "", 
    "tracker_url": "", 
    "category" : "Physics" 
}


import bpy
import bpy.utils.previews
import mathutils


addon_keymaps = {}
_icons = None
rbc_to_rbjc_converter = {'sna_active_object_name': '', 'sna_selected_objects_before_loop_start': [], 'sna_was_obj_active': False, 'sna_was_objs_selected': False, 'sna_rbc_objects_names': [], 'sna_rbjc_objs_namesfirst_objects_of_rbc_obj': [], 'sna_all_rbs_of_all_rbc_objswithout_duplicates': [], 'sna_ge_physics_deactivation_linear': 0.0, 'sna_ge_physics_deactivation_angular': 0.0, 'sna_selected_mode': '', 'sna_all_rigid_bodys_first_and_second_obj_of_rbcduplicates': [], 'sna_added_obj_for_use_to_get_rot': '', 'sna_rotation_for_pivot_from_rbc_objs': [], }
rigid_body_joint_constraints_buttons = {'sna_original_3d_cursor_location': [], 'sna_number_of_all_selected_vertecies__for_finding_out_the_index_of_last_added_vertex_': 0, 'sna_selected_mode': '', 'sna_soft_body_selected_vertex_locations': [], 'sna_active_obj_name': '', 'sna_toggle_enable_or_disable_obj_constraint': True, 'sna_selected_vertices_locations_local': [], 'sna_active_object_name': '', 'sna_selected_objects_names_4_selection': [], 'sna_sequence_of_selected_objs': [], 'sna_objs_with_rbj_constraint': [], }


def property_exists(prop_path, glob, loc):
    try:
        eval(prop_path, glob, loc)
        return True
    except:
        return False


def sna_function_execute_91C9E(active_object_matrix_world_inverted):
    active_object_matrix_world_inverted = active_object_matrix_world_inverted
    active_object_matrix_world_inverted = None
    active_object_matrix_world_inverted = bpy.context.object.matrix_world.inverted()
    return active_object_matrix_world_inverted


class SNA_OT_Blender_Rb_Settings_To_Bge_Rb_Settings_C26Ad(bpy.types.Operator):
    bl_idname = "sna.blender_rb_settings_to_bge_rb_settings_c26ad"
    bl_label = "Blender RB settings to BGE RB settings"
    bl_description = "it copys selected objects blender rigid body settings to blender game engine rigid body settings like: mass, friction damping, collision bounds,etc. all settings that are available in BGE object physics for passive and active ( static and rigid body) for the same object. so you still have to Hold ALT button and change value to update all selected RB settings and then run this operator to pass it on game RB... If the RB set to sphere and objecys acting crazy then change the radius for BGE RBs..."
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and True:
            cls.poll_message_set('')
        return not False

    def execute(self, context):
        for i_FCFD6 in range(len(bpy.context.view_layer.objects.selected)):
            if (bpy.context.view_layer.objects.selected[i_FCFD6].type == 'MESH'):
                if property_exists("bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.enabled", globals(), locals()):
                    if (bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.type == 'ACTIVE'):
                        bpy.context.view_layer.objects.selected[i_FCFD6].game.physics_type = 'RIGID_BODY'
                    else:
                        bpy.context.view_layer.objects.selected[i_FCFD6].game.physics_type = 'STATIC'
                    bpy.context.view_layer.objects.selected[i_FCFD6].game.mass = bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.mass
                    bpy.context.view_layer.objects.selected[i_FCFD6].game.use_collision_bounds = True
                    if False:
                        bpy.context.view_layer.objects.selected[i_FCFD6].game.use_collision_compound = True
                    else:
                        bpy.context.view_layer.objects.selected[i_FCFD6].game.use_collision_compound = False
                    bpy.context.view_layer.objects.selected[i_FCFD6].game.collision_bounds_type = {'BOX': 'BOX', 'SPHERE': 'SPHERE', 'CAPSULE': 'CAPSULE', 'MESH': 'TRIANGLE_MESH', 'COMPOUND': 'Empty', 'CONVEX_HULL': 'CONVEX_HULL', 'CONE': 'CONE', 'CYLINDER': 'CYLINDER', }[bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.collision_shape]
                    bpy.context.view_layer.objects.selected[i_FCFD6].game.friction = bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.friction
                    bpy.context.view_layer.objects.selected[i_FCFD6].game.elasticity = bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.restitution
                    bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.use_margin = True
                    bpy.context.view_layer.objects.selected[i_FCFD6].game.collision_margin = bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.collision_margin
                    bpy.context.view_layer.objects.selected[i_FCFD6].game.damping = bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.linear_damping
                    bpy.context.view_layer.objects.selected[i_FCFD6].game.rotation_damping = bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.angular_damping
                    bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.use_deactivation = True
                    bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.deactivate_linear_velocity = bpy.context.scene.game_settings.deactivation_linear_threshold
                    bpy.context.view_layer.objects.selected[i_FCFD6].rigid_body.deactivate_angular_velocity = bpy.context.scene.game_settings.deactivation_angular_threshold
        return {"FINISHED"}

    def invoke(self, context, event):
        return self.execute(context)


class SNA_OT_Rbc_To_Rbjc_Converting_C5172(bpy.types.Operator):
    bl_idname = "sna.rbc_to_rbjc_converting_c5172"
    bl_label = "RBC to RBJC Converting"
    bl_description = "Converts Rigid Body Constraints to the game engines Rigid Body Joint Constraints, so it will be 'same' settings that are available except collection... if the Constraints are not in same View, scene or are hidden then they will not be converted... and if its slow then go to local view with selected objects. Supported Constraints Types are: POINT,FIXED,GENERIC"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and True:
            cls.poll_message_set('')
        return not False

    def execute(self, context):
        if 'OBJECT'==bpy.context.mode:
            if bpy.context.view_layer.objects.active:
                rbc_to_rbjc_converter['sna_active_object_name'] = bpy.context.view_layer.objects.active.name
                rbc_to_rbjc_converter['sna_was_obj_active'] = True
                if property_exists("bpy.context.view_layer.objects.selected[0]", globals(), locals()):
                    for i_AAAA4 in range(len(bpy.context.view_layer.objects.selected)):
                        rbc_to_rbjc_converter['sna_selected_objects_before_loop_start'].append(bpy.context.view_layer.objects.selected[i_AAAA4].name)
                    rbc_to_rbjc_converter['sna_was_objs_selected'] = True
            else:
                rbc_to_rbjc_converter['sna_was_obj_active'] = False
                if property_exists("bpy.context.view_layer.objects.selected[0]", globals(), locals()):
                    for i_D25F0 in range(len(bpy.context.view_layer.objects.selected)):
                        rbc_to_rbjc_converter['sna_selected_objects_before_loop_start'].append(bpy.context.view_layer.objects.selected[i_D25F0].name)
                    rbc_to_rbjc_converter['sna_was_objs_selected'] = True
                else:
                    rbc_to_rbjc_converter['sna_was_objs_selected'] = False
            bpy.ops.object.select_all('INVOKE_DEFAULT', action='SELECT')
            for i_20834 in range(len(bpy.context.view_layer.objects.selected)):
                if bpy.context.view_layer.objects.selected[i_20834].rigid_body_constraint:
                    if property_exists("bpy.context.view_layer.objects.selected[i_20834].rigid_body_constraint.object1.name", globals(), locals()):
                        if property_exists("bpy.context.view_layer.objects.selected[i_20834].rigid_body_constraint.object2.name", globals(), locals()):
                            if (property_exists("bpy.context.view_layer.objects.selected[i_20834].rigid_body_constraint.object1.name", globals(), locals()) == property_exists("bpy.context.view_layer.objects.selected[i_20834].rigid_body_constraint.object2.name", globals(), locals())):
                                if (bpy.context.view_layer.objects.selected[i_20834].rigid_body_constraint.type == 'POINT'):
                                    rbc_to_rbjc_converter['sna_rbc_objects_names'].append(bpy.context.view_layer.objects.selected[i_20834].name)
                                else:
                                    if (bpy.context.view_layer.objects.selected[i_20834].rigid_body_constraint.type == 'FIXED'):
                                        rbc_to_rbjc_converter['sna_rbc_objects_names'].append(bpy.context.view_layer.objects.selected[i_20834].name)
                                    else:
                                        if (bpy.context.view_layer.objects.selected[i_20834].rigid_body_constraint.type == 'GENERIC'):
                                            rbc_to_rbjc_converter['sna_rbc_objects_names'].append(bpy.context.view_layer.objects.selected[i_20834].name)
            for i_56A8E in range(len(rbc_to_rbjc_converter['sna_rbc_objects_names'])):
                rbc_to_rbjc_converter['sna_rbjc_objs_namesfirst_objects_of_rbc_obj'].append(bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_56A8E]].rigid_body_constraint.object1.name)
                rbc_to_rbjc_converter['sna_all_rigid_bodys_first_and_second_obj_of_rbcduplicates'].append(bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_56A8E]].rigid_body_constraint.object1.name)
                rbc_to_rbjc_converter['sna_all_rigid_bodys_first_and_second_obj_of_rbcduplicates'].append(bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_56A8E]].rigid_body_constraint.object2.name)
            for i_15855 in range(len(rbc_to_rbjc_converter['sna_all_rigid_bodys_first_and_second_obj_of_rbcduplicates'])):
                if rbc_to_rbjc_converter['sna_all_rigid_bodys_first_and_second_obj_of_rbcduplicates'][i_15855] in rbc_to_rbjc_converter['sna_all_rbs_of_all_rbc_objswithout_duplicates']:
                    pass
                else:
                    rbc_to_rbjc_converter['sna_all_rbs_of_all_rbc_objswithout_duplicates'].append(rbc_to_rbjc_converter['sna_all_rigid_bodys_first_and_second_obj_of_rbcduplicates'][i_15855])
            for i_BD792 in range(len(rbc_to_rbjc_converter['sna_rbjc_objs_namesfirst_objects_of_rbc_obj'])):
                for i_76EAB in range(len(bpy.data.objects[rbc_to_rbjc_converter['sna_rbjc_objs_namesfirst_objects_of_rbc_obj'][i_BD792]].constraints)-1,-1,-1):
                    if (bpy.data.objects[rbc_to_rbjc_converter['sna_rbjc_objs_namesfirst_objects_of_rbc_obj'][i_BD792]].constraints[i_76EAB].type == 'RIGID_BODY_JOINT'):
                        if bpy.data.objects[rbc_to_rbjc_converter['sna_rbjc_objs_namesfirst_objects_of_rbc_obj'][i_BD792]].constraints[i_76EAB].name in rbc_to_rbjc_converter['sna_rbc_objects_names']:
                            bpy.data.objects[rbc_to_rbjc_converter['sna_rbjc_objs_namesfirst_objects_of_rbc_obj'][i_BD792]].constraints[0].id_data.constraints.remove(constraint=bpy.data.objects[rbc_to_rbjc_converter['sna_rbjc_objs_namesfirst_objects_of_rbc_obj'][i_BD792]].constraints[i_76EAB], )
            for i_FC8EF in range(len(rbc_to_rbjc_converter['sna_rbc_objects_names'])):
                constraint_852B9 = bpy.context.view_layer.objects[bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.object1.name].constraints.new(type='RIGID_BODY_JOINT', )
                constraint_852B9.target = bpy.data.objects[bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.object2.name]
                constraint_852B9.name = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].name
                bpy.context.view_layer.objects.active = bpy.data.objects[bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.object1.name]
                constraint_852B9.pivot_x = (float(tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[3]))[0]
                constraint_852B9.pivot_y = (float(tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[3]))[1]
                constraint_852B9.pivot_z = (float(tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[0]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[1]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_91C9E([]))[2]) * mathutils.Vector((bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[0], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[1], bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].location[2], 1.0)))[3]))[2]
                constraint_852B9.show_pivot = True
                constraint_852B9.enabled = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.enabled
                constraint_852B9.use_linked_collision = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.disable_collisions
                constraint_852B9.use_breaking = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.use_breaking
                constraint_852B9.breaking_threshold = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.breaking_threshold
                if (bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.type == 'POINT'):
                    pass
                else:
                    if (bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.type == 'FIXED'):
                        constraint_852B9.pivot_type = 'GENERIC_6_DOF'
                        constraint_852B9.use_limit_x = True
                        constraint_852B9.use_limit_y = True
                        constraint_852B9.use_limit_z = True
                        constraint_852B9.use_angular_limit_x = True
                        constraint_852B9.use_angular_limit_y = True
                        constraint_852B9.use_angular_limit_z = True
                    else:
                        constraint_852B9.pivot_type = 'GENERIC_6_DOF'
                        constraint_852B9.use_limit_x = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.use_limit_lin_x
                        constraint_852B9.limit_min_x = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_lin_x_lower
                        constraint_852B9.limit_max_x = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_lin_x_upper
                        constraint_852B9.use_limit_y = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.use_limit_lin_y
                        constraint_852B9.limit_min_y = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_lin_y_lower
                        constraint_852B9.limit_max_y = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_lin_y_upper
                        constraint_852B9.use_limit_z = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.use_limit_lin_z
                        constraint_852B9.limit_min_z = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_lin_z_lower
                        constraint_852B9.limit_max_z = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_lin_z_upper
                        constraint_852B9.use_angular_limit_x = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.use_limit_ang_x
                        constraint_852B9.limit_angle_min_x = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_ang_x_lower
                        constraint_852B9.limit_angle_max_x = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_ang_x_upper
                        constraint_852B9.use_angular_limit_y = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.use_limit_ang_y
                        constraint_852B9.limit_angle_min_y = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_ang_y_lower
                        constraint_852B9.limit_angle_max_y = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_ang_y_upper
                        constraint_852B9.use_angular_limit_z = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.use_limit_ang_z
                        constraint_852B9.limit_angle_min_z = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_ang_z_lower
                        constraint_852B9.limit_angle_max_z = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_FC8EF]].rigid_body_constraint.limit_ang_z_upper
            bpy.ops.object.empty_add('INVOKE_DEFAULT', type='ARROWS', radius=1.0, align='WORLD')
            rbc_to_rbjc_converter['sna_added_obj_for_use_to_get_rot'] = bpy.context.view_layer.objects.active.name
            bpy.data.objects[bpy.context.view_layer.objects.active.name].rotation_mode = 'AXIS_ANGLE'
            bpy.ops.object.parent_no_inverse_set('INVOKE_DEFAULT', confirm=False, keep_transform=False)
            for i_15A28 in range(len(rbc_to_rbjc_converter['sna_rbc_objects_names'])):
                bpy.data.objects[rbc_to_rbjc_converter['sna_added_obj_for_use_to_get_rot']].parent = bpy.data.objects[rbc_to_rbjc_converter['sna_rbjc_objs_namesfirst_objects_of_rbc_obj'][i_15A28]]
                constraint_9C387 = bpy.context.view_layer.objects[rbc_to_rbjc_converter['sna_added_obj_for_use_to_get_rot']].constraints.new(type='COPY_ROTATION', )
                constraint_9C387.target = bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_15A28]]
                bpy.ops.constraint.apply(constraint=constraint_9C387.name, owner='OBJECT')
                rbc_to_rbjc_converter['sna_rotation_for_pivot_from_rbc_objs'].append((bpy.data.objects[rbc_to_rbjc_converter['sna_added_obj_for_use_to_get_rot']].rotation_axis_angle[0], bpy.data.objects[rbc_to_rbjc_converter['sna_added_obj_for_use_to_get_rot']].rotation_axis_angle[1], bpy.data.objects[rbc_to_rbjc_converter['sna_added_obj_for_use_to_get_rot']].rotation_axis_angle[2], bpy.data.objects[rbc_to_rbjc_converter['sna_added_obj_for_use_to_get_rot']].rotation_axis_angle[3]))
            bpy.ops.object.delete('INVOKE_DEFAULT', use_global=False, confirm=False)
            for i_2F08C in range(len(rbc_to_rbjc_converter['sna_rbc_objects_names'])):
                bpy.data.objects[bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_2F08C]].rigid_body_constraint.object1.name].constraints[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_2F08C]].axis_x = float(rbc_to_rbjc_converter['sna_rotation_for_pivot_from_rbc_objs'][i_2F08C][1] * rbc_to_rbjc_converter['sna_rotation_for_pivot_from_rbc_objs'][i_2F08C][0])
                bpy.data.objects[bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_2F08C]].rigid_body_constraint.object1.name].constraints[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_2F08C]].axis_y = float(rbc_to_rbjc_converter['sna_rotation_for_pivot_from_rbc_objs'][i_2F08C][2] * rbc_to_rbjc_converter['sna_rotation_for_pivot_from_rbc_objs'][i_2F08C][0])
                bpy.data.objects[bpy.data.objects[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_2F08C]].rigid_body_constraint.object1.name].constraints[rbc_to_rbjc_converter['sna_rbc_objects_names'][i_2F08C]].axis_z = float(rbc_to_rbjc_converter['sna_rotation_for_pivot_from_rbc_objs'][i_2F08C][3] * rbc_to_rbjc_converter['sna_rotation_for_pivot_from_rbc_objs'][i_2F08C][0])
            bpy.ops.object.select_all('INVOKE_DEFAULT', action='DESELECT')
            if rbc_to_rbjc_converter['sna_was_objs_selected']:
                for i_904C8 in range(len(rbc_to_rbjc_converter['sna_selected_objects_before_loop_start'])):
                    bpy.context.view_layer.objects[rbc_to_rbjc_converter['sna_selected_objects_before_loop_start'][i_904C8]].select_set(state=True, view_layer=bpy.context.view_layer, )
                    if rbc_to_rbjc_converter['sna_was_obj_active']:
                        bpy.context.view_layer.objects.active = bpy.data.objects[rbc_to_rbjc_converter['sna_active_object_name']]
            if rbc_to_rbjc_converter['sna_was_obj_active']:
                bpy.context.view_layer.objects.active = bpy.data.objects[rbc_to_rbjc_converter['sna_active_object_name']]
            else:
                bpy.context.view_layer.objects.active = None
            rbc_to_rbjc_converter['sna_active_object_name'] = ''
            rbc_to_rbjc_converter['sna_selected_objects_before_loop_start'] = []
            rbc_to_rbjc_converter['sna_was_obj_active'] = False
            rbc_to_rbjc_converter['sna_was_objs_selected'] = False
            rbc_to_rbjc_converter['sna_rbc_objects_names'] = []
            rbc_to_rbjc_converter['sna_rbjc_objs_namesfirst_objects_of_rbc_obj'] = []
            rbc_to_rbjc_converter['sna_added_obj_for_use_to_get_rot'] = ''
            rbc_to_rbjc_converter['sna_rotation_for_pivot_from_rbc_objs'] = []
            rbc_to_rbjc_converter['sna_all_rigid_bodys_first_and_second_obj_of_rbcduplicates'] = []
            rbc_to_rbjc_converter['sna_all_rbs_of_all_rbc_objswithout_duplicates'] = []
            self.report({'INFO'}, message='Converting Completed')
        else:
            self.report({'ERROR'}, message='Need be in OBJECT Mode')
        return {"FINISHED"}

    def invoke(self, context, event):
        return self.execute(context)


class SNA_OT_Pivot_To_3D_Cursor_Fe228(bpy.types.Operator):
    bl_idname = "sna.pivot_to_3d_cursor_fe228"
    bl_label = "pivot to 3D cursor"
    bl_description = "moves the pivot to selection ( based on Transform Pivot Point )"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and False:
            cls.poll_message_set()
        return not False

    def execute(self, context):
        rigid_body_joint_constraints_buttons['sna_selected_mode'] = str(bpy.context.mode)
        if 'EDIT_MESH'==bpy.context.mode:
            pass
        else:
            bpy.ops.object.mode_set('INVOKE_DEFAULT', mode='EDIT')
        bpy.ops.mesh.select_mode('INVOKE_DEFAULT', type='VERT')
        bpy.ops.mesh.primitive_vert_add('INVOKE_DEFAULT', )
        bpy.ops.object.vertex_group_add('INVOKE_DEFAULT', )
        bpy.ops.object.vertex_group_assign('INVOKE_DEFAULT', )
        bpy.ops.mesh.select_all('INVOKE_DEFAULT', action='SELECT')
        rigid_body_joint_constraints_buttons['sna_number_of_all_selected_vertecies__for_finding_out_the_index_of_last_added_vertex_'] = bpy.context.active_object.data.total_vert_sel
        bpy.context.active_object.constraints.active.pivot_x = bpy.context.active_object.data.vertices[int(rigid_body_joint_constraints_buttons['sna_number_of_all_selected_vertecies__for_finding_out_the_index_of_last_added_vertex_'] - 1.0)].co[0]
        bpy.context.active_object.constraints.active.pivot_y = bpy.context.active_object.data.vertices[int(rigid_body_joint_constraints_buttons['sna_number_of_all_selected_vertecies__for_finding_out_the_index_of_last_added_vertex_'] - 1.0)].co[1]
        bpy.context.active_object.constraints.active.pivot_z = bpy.context.active_object.data.vertices[int(rigid_body_joint_constraints_buttons['sna_number_of_all_selected_vertecies__for_finding_out_the_index_of_last_added_vertex_'] - 1.0)].co[2]
        bpy.ops.mesh.select_all('INVOKE_DEFAULT', )
        bpy.ops.object.vertex_group_select('INVOKE_DEFAULT', )
        bpy.ops.mesh.delete('INVOKE_DEFAULT', type='VERT')
        bpy.ops.object.vertex_group_remove('INVOKE_DEFAULT', )
        bpy.ops.object.mode_set('INVOKE_DEFAULT', mode={'EDIT_MESH': 'EDIT', 'OBJECT': 'OBJECT', 'PAINT_VERTEX': 'VERTEX_PAINT', 'PAINT_WEIGHT': 'WEIGHT_PAINT', }[rigid_body_joint_constraints_buttons['sna_selected_mode']])
        bpy.context.active_object.constraints.active.show_pivot = True
        return {"FINISHED"}

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)


def sna_add_to_object_pt_brigidbodyjointconstraint_487D0(self, context):
    if not (False):
        layout = self.layout
        op = layout.operator('sna.pivot_to_3d_cursor_fe228', text='Pivot to 3D Cursor', icon_value=550, emboss=True, depress=False)


def sna_add_to_object_pt_brigidbodyjointconstraint_1E5A8(self, context):
    if not (False):
        layout = self.layout
        op = layout.operator('sna.pivot_to_selction_d59e1', text='Pivot to Selection', icon_value=260, emboss=True, depress=False)


def sna_function_execute_760FC(active_object_matrix_world_inverted):
    active_object_matrix_world_inverted = active_object_matrix_world_inverted
    active_object_matrix_world_inverted = None
    active_object_matrix_world_inverted = bpy.context.object.matrix_world.inverted()
    return active_object_matrix_world_inverted


class SNA_OT_Disables_All_Selected_Objs_Constraints_63397(bpy.types.Operator):
    bl_idname = "sna.disables_all_selected_objs_constraints_63397"
    bl_label = "Disables all Selected OBJs Constraints"
    bl_description = "DISABLES all Object Constraints for Active and Selected Objects"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and True:
            cls.poll_message_set('')
        return not False

    def execute(self, context):
        for i_96438 in range(len(bpy.context.view_layer.objects.selected)):
            for i_B4852 in range(len(bpy.context.view_layer.objects.selected[i_96438].constraints)):
                bpy.context.view_layer.objects.selected[i_96438].constraints[i_B4852].enabled = False
        return {"FINISHED"}

    def invoke(self, context, event):
        return self.execute(context)


class SNA_OT_Show_Pivots_On_3281B(bpy.types.Operator):
    bl_idname = "sna.show_pivots_on_3281b"
    bl_label = "Show Pivot/s ON"
    bl_description = "Turns ON show Pivot Point for active and or all selected objs"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and True:
            cls.poll_message_set('')
        return not False

    def execute(self, context):
        for i_1B0FC in range(len(bpy.context.view_layer.objects.selected)):
            for i_E9A8E in range(len(bpy.context.view_layer.objects.selected[i_1B0FC].constraints)):
                if (bpy.context.view_layer.objects.selected[i_1B0FC].constraints[i_E9A8E].type == 'RIGID_BODY_JOINT'):
                    bpy.context.view_layer.objects.selected[i_1B0FC].constraints[i_E9A8E].show_pivot = True
        return {"FINISHED"}

    def invoke(self, context, event):
        return self.execute(context)


class SNA_OT_Delete_Constraints_A5C4F(bpy.types.Operator):
    bl_idname = "sna.delete_constraints_a5c4f"
    bl_label = "Delete constraints"
    bl_description = "Removes all Constraints from active and or selected objects"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and False:
            cls.poll_message_set()
        return not False

    def execute(self, context):
        bpy.context.view_layer.objects.active.constraints.clear()
        for i_F4A6F in range(len(bpy.context.view_layer.objects.selected)):
            bpy.context.view_layer.objects.selected[i_F4A6F].constraints.clear()
        return {"FINISHED"}

    def invoke(self, context, event):
        return self.execute(context)


class SNA_OT_Enables_All_Selected_Objs_Constraints_C322A(bpy.types.Operator):
    bl_idname = "sna.enables_all_selected_objs_constraints_c322a"
    bl_label = "Enables all Selected OBJs Constraints"
    bl_description = "ENABLES all Object Constraints for Active and Selected Objects"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and True:
            cls.poll_message_set('')
        return not False

    def execute(self, context):
        for i_34C6B in range(len(bpy.context.view_layer.objects.selected)):
            for i_92624 in range(len(bpy.context.view_layer.objects.selected[i_34C6B].constraints)):
                bpy.context.view_layer.objects.selected[i_34C6B].constraints[i_92624].enabled = True
        return {"FINISHED"}

    def invoke(self, context, event):
        return self.execute(context)


class SNA_OT_Show_Pivots_Off_3D242(bpy.types.Operator):
    bl_idname = "sna.show_pivots_off_3d242"
    bl_label = "Show Pivot/s OFF"
    bl_description = "Turns OFF show Pivot Point for active and or all selected objs"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and True:
            cls.poll_message_set('')
        return not False

    def execute(self, context):
        for i_8EDDE in range(len(bpy.context.view_layer.objects.selected)):
            for i_14263 in range(len(bpy.context.view_layer.objects.selected[i_8EDDE].constraints)):
                if (bpy.context.view_layer.objects.selected[i_8EDDE].constraints[i_14263].type == 'RIGID_BODY_JOINT'):
                    bpy.context.view_layer.objects.selected[i_8EDDE].constraints[i_14263].show_pivot = False
        return {"FINISHED"}

    def invoke(self, context, event):
        return self.execute(context)


class SNA_OT_Each_Vertex_To_Rbj_Pivot_802E0(bpy.types.Operator):
    bl_idname = "sna.each_vertex_to_rbj_pivot_802e0"
    bl_label = "each Vertex To RBJ Pivot"
    bl_description = "for Each Vertex that is selected a Rigid Body Joint Pivot bill be made for all and each Selected Object, and placed where the vertex are , the Target will be the Active Object"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and False:
            cls.poll_message_set()
        return not False

    def execute(self, context):
        rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'] = []
        rigid_body_joint_constraints_buttons['sna_active_object_name'] = bpy.context.active_object.name
        bpy.data.objects[bpy.context.active_object.name].select_set(state=False, )
        for i_815FD in range(len(bpy.context.view_layer.objects.selected)):
            rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'].append(bpy.context.view_layer.objects.selected[i_815FD].name)
        prev_context = bpy.context.area.type
        bpy.context.area.type = 'VIEW_3D'
        bpy.ops.object.mode_set('INVOKE_DEFAULT', )
        bpy.context.area.type = prev_context
        bpy.ops.object.select_all('INVOKE_DEFAULT', action='DESELECT')
        bpy.context.view_layer.objects.active = None
        for i_836D5 in range(len(rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'])):
            bpy.data.objects[rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'][i_836D5]].select_set(state=True, )
            bpy.context.view_layer.objects.active = None
            for i_34AC0 in range(len(bpy.context.view_layer.objects.selected)):
                pass
            bpy.context.view_layer.objects.active = bpy.context.view_layer.objects.selected[i_34AC0]
            bpy.context.view_layer.objects.selected[i_34AC0].select_set(state=False, )
            bpy.ops.object.mode_set('INVOKE_DEFAULT', mode='EDIT')
            for i_AC21F in range(len(bpy.context.view_layer.objects.active.data.vertices)):
                rigid_body_joint_constraints_buttons['sna_selected_vertices_locations_local'] = []
                if bpy.context.view_layer.objects.active.data.vertices[i_AC21F].select:
                    rigid_body_joint_constraints_buttons['sna_selected_vertices_locations_local'].append(bpy.context.view_layer.objects.active.data.vertices[i_AC21F].co)
                    for i_A5342 in range(len(rigid_body_joint_constraints_buttons['sna_selected_vertices_locations_local'])):
                        constraint_D25D0 = bpy.context.view_layer.objects.active.constraints.new(type='RIGID_BODY_JOINT', )
                        constraint_D25D0.pivot_x = rigid_body_joint_constraints_buttons['sna_selected_vertices_locations_local'][i_A5342][0]
                        constraint_D25D0.pivot_y = rigid_body_joint_constraints_buttons['sna_selected_vertices_locations_local'][i_A5342][1]
                        constraint_D25D0.pivot_z = rigid_body_joint_constraints_buttons['sna_selected_vertices_locations_local'][i_A5342][2]
                        constraint_D25D0.target = bpy.data.objects[rigid_body_joint_constraints_buttons['sna_active_object_name']]
                        constraint_D25D0.show_pivot = True
        return {"FINISHED"}

    def invoke(self, context, event):
        return self.execute(context)


_0508E_running = False
class SNA_OT_Rbj_Chain_Maker_0508E(bpy.types.Operator):
    bl_idname = "sna.rbj_chain_maker_0508e"
    bl_label = "RBJ Chain Maker"
    bl_description = "Select sequence of objects to make chain of ball type RBJ Constraint. if the object have already RBJ constraints then it will change the target object for all the pivot constraints. the each object musst be active object..."
    bl_options = {"REGISTER", "UNDO"}
    cursor = "EYEDROPPER"
    _handle = None
    _event = {}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and True:
            cls.poll_message_set('')
        if not False or context.area.spaces[0].bl_rna.identifier == 'SpaceView3D':
            return not False
        return False

    def save_event(self, event):
        event_options = ["type", "value", "alt", "shift", "ctrl", "oskey", "mouse_region_x", "mouse_region_y", "mouse_x", "mouse_y", "pressure", "tilt"]
        if bpy.app.version >= (3, 2, 1):
            event_options += ["type_prev", "value_prev"]
        for option in event_options: self._event[option] = getattr(event, option)

    def draw_callback_px(self, context):
        event = self._event
        if event.keys():
            event = dotdict(event)
            try:
                pass
            except Exception as error:
                print(error)

    def execute(self, context):
        global _0508E_running
        _0508E_running = False
        context.window.cursor_set("DEFAULT")
        for i_CA92F in range(len(rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'])):
            for i_973D4 in range(len(bpy.data.objects[rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'][i_CA92F]].constraints)):
                if (bpy.data.objects[rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'][i_CA92F]].constraints[i_973D4].type == 'RIGID_BODY_JOINT'):
                    if rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'][i_CA92F] in rigid_body_joint_constraints_buttons['sna_objs_with_rbj_constraint']:
                        pass
                    else:
                        rigid_body_joint_constraints_buttons['sna_objs_with_rbj_constraint'].append(rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'][i_CA92F])
        for i_EA57C in range(len(rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'])-1,-1,-1):
            if rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'][i_EA57C] in rigid_body_joint_constraints_buttons['sna_objs_with_rbj_constraint']:
                rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'].remove(rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'][i_EA57C])
        for i_4E4C5 in range(len(rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'])):
            constraint_E9FC8 = bpy.context.view_layer.objects[rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'][i_4E4C5]].constraints.new(type='RIGID_BODY_JOINT', )
        for i_7F9C6 in range(int(len(rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs']) - 1.0)):
            for i_522A0 in range(len(bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][i_7F9C6]].constraints)):
                if (bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][i_7F9C6]].constraints[i_522A0].type == 'RIGID_BODY_JOINT'):
                    bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][i_7F9C6]].constraints[i_522A0].target = bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][int(i_7F9C6 + 1.0)]]
        constraint_B84FE = bpy.context.view_layer.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][-1]].constraints.new(type='RIGID_BODY_JOINT', )
        for i_6EB7E in range(len(bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][-1]].constraints)):
            pass
        bpy.context.view_layer.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][-1]].constraints.remove(constraint=bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][-1]].constraints[i_6EB7E], )
        for i_9C6E8 in range(len(bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][-1]].constraints)):
            pass
        if bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][-1]].constraints[i_9C6E8].target:
            pass
        else:
            bpy.context.view_layer.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][-1]].constraints.remove(constraint=bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][-1]].constraints[i_9C6E8], )
        for i_4F2D9 in range(len(rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'])):
            bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][i_4F2D9]].select_set(state=True, view_layer=bpy.context.view_layer, )
        bpy.ops.object.mode_set('INVOKE_DEFAULT', mode={'EDIT_MESH': 'EDIT', 'PAINT_VERTEX': 'VERTEX_PAINT', 'PAINT_WEIGHT': 'WEIGHT_PAINT', 'OBJECT': 'OBJECT', }[rigid_body_joint_constraints_buttons['sna_selected_mode']])
        rigid_body_joint_constraints_buttons['sna_selected_mode'] = ''
        for i_85CA1 in range(len(rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'])):
            for i_ABE92 in range(len(bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][i_85CA1]].constraints)):
                if (bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][i_85CA1]].constraints[i_ABE92].type == 'RIGID_BODY_JOINT'):
                    bpy.data.objects[rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'][i_85CA1]].constraints[i_ABE92].show_pivot = True
        for area in context.screen.areas:
            area.tag_redraw()
        return {"FINISHED"}

    def modal(self, context, event):
        global _0508E_running
        if not context.area or not _0508E_running:
            self.execute(context)
            return {'CANCELLED'}
        self.save_event(event)
        context.window.cursor_set('EYEDROPPER')
        try:
            if bpy.context.active_object.name in rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection']:
                pass
            else:
                if bpy.context.view_layer.objects.active.type == 'MESH':
                    rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'].append(bpy.context.active_object.name)
            if bpy.context.active_object.name in rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs']:
                pass
            else:
                if bpy.context.view_layer.objects.active.type == 'MESH':
                    rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'].append(bpy.context.active_object.name)
        except Exception as error:
            print(error)
        if event.type in ['RIGHTMOUSE', 'ESC']:
            self.execute(context)
            return {'CANCELLED'}
        return {'PASS_THROUGH'}

    def invoke(self, context, event):
        global _0508E_running
        if _0508E_running:
            _0508E_running = False
            return {'FINISHED'}
        else:
            self.save_event(event)
            self.start_pos = (event.mouse_x, event.mouse_y)
            rigid_body_joint_constraints_buttons['sna_selected_mode'] = bpy.context.mode
            if 'EDIT_MESH'==bpy.context.mode:
                bpy.ops.object.mode_set('INVOKE_DEFAULT', mode='OBJECT')
            bpy.ops.object.select_all('INVOKE_DEFAULT', action='DESELECT')
            rigid_body_joint_constraints_buttons['sna_objs_with_rbj_constraint'] = []
            rigid_body_joint_constraints_buttons['sna_selected_objects_names_4_selection'] = []
            rigid_body_joint_constraints_buttons['sna_sequence_of_selected_objs'] = []
            bpy.context.view_layer.objects.active = None
            for i_2B8BC in range(len(bpy.context.view_layer.objects.selected)):
                bpy.context.view_layer.objects.selected[i_2B8BC].select_set(state=False, )
            context.window_manager.modal_handler_add(self)
            _0508E_running = True
            return {'RUNNING_MODAL'}


class SNA_OT_Pivot_To_Selction_D59E1(bpy.types.Operator):
    bl_idname = "sna.pivot_to_selction_d59e1"
    bl_label = "pivot to selction"
    bl_description = "moves the pivot to selection ( based on Transform Pivot Point )"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and False:
            cls.poll_message_set()
        return not False

    def execute(self, context):
        if 'EDIT_MESH'==bpy.context.mode:
            if (bpy.context.active_object.data.count_selected_items() > (0.0, 0.0, 0.0)):
                bpy.ops.object.vertex_group_add('INVOKE_DEFAULT', )
                bpy.ops.object.vertex_group_assign('INVOKE_DEFAULT', )
                rigid_body_joint_constraints_buttons['sna_original_3d_cursor_location'] = list(bpy.context.scene.cursor.location)
                prev_context = bpy.context.area.type
                bpy.context.area.type = 'VIEW_3D'
                bpy.ops.view3d.snap_cursor_to_selected('INVOKE_DEFAULT', )
                bpy.context.area.type = prev_context
                bpy.ops.mesh.primitive_vert_add('INVOKE_DEFAULT', )
                bpy.ops.object.vertex_group_add('INVOKE_DEFAULT', )
                bpy.ops.object.vertex_group_assign('INVOKE_DEFAULT', )
                bpy.ops.mesh.select_all('INVOKE_DEFAULT', action='SELECT')
                rigid_body_joint_constraints_buttons['sna_number_of_all_selected_vertecies__for_finding_out_the_index_of_last_added_vertex_'] = bpy.context.active_object.data.total_vert_sel
                bpy.context.active_object.constraints.active.pivot_x = bpy.context.active_object.data.vertices[int(rigid_body_joint_constraints_buttons['sna_number_of_all_selected_vertecies__for_finding_out_the_index_of_last_added_vertex_'] - 1.0)].co[0]
                bpy.context.active_object.constraints.active.pivot_y = bpy.context.active_object.data.vertices[int(rigid_body_joint_constraints_buttons['sna_number_of_all_selected_vertecies__for_finding_out_the_index_of_last_added_vertex_'] - 1.0)].co[1]
                bpy.context.active_object.constraints.active.pivot_z = bpy.context.active_object.data.vertices[int(rigid_body_joint_constraints_buttons['sna_number_of_all_selected_vertecies__for_finding_out_the_index_of_last_added_vertex_'] - 1.0)].co[2]
                bpy.ops.mesh.select_all('INVOKE_DEFAULT', )
                bpy.ops.object.vertex_group_select('INVOKE_DEFAULT', )
                bpy.ops.mesh.delete('INVOKE_DEFAULT', type='VERT')
                bpy.ops.object.vertex_group_remove('INVOKE_DEFAULT', )
                bpy.context.scene.cursor.location = tuple(rigid_body_joint_constraints_buttons['sna_original_3d_cursor_location'])
                bpy.ops.object.vertex_group_select('INVOKE_DEFAULT', )
                bpy.ops.object.vertex_group_remove('INVOKE_DEFAULT', )
                bpy.context.active_object.constraints.active.show_pivot = True
            else:
                self.report({'ERROR'}, message='need have something selected')
        else:
            self.report({'ERROR'}, message='works only if you in Edit Mode, so please switch to Edit Mode and press again')
        return {"FINISHED"}

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)


class SNA_OT_Pivots_For_Soft_Bodys_Dd35F(bpy.types.Operator):
    bl_idname = "sna.pivots_for_soft_bodys_dd35f"
    bl_label = "Pivots for Soft Bodys"
    bl_description = "Makes 1 Rigid Body Joint Constraint on Selected Objects for each selected vertex on the active object ( active object musst be soft body, its BGE limitation.. ) HOW TO USE?: 1. select some object/s and then select the last object and go in edit mode, select some vertecies. the selected vertecies locations of active object will be used for the RBJ constraint location, and then press the button. TIPP: the soft body scale musst be applyed...."
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        if bpy.app.version >= (3, 0, 0) and True:
            cls.poll_message_set('')
        return not False

    def execute(self, context):
        bpy.ops.object.mode_set('INVOKE_DEFAULT', mode='OBJECT')
        bpy.ops.object.mode_set('INVOKE_DEFAULT', mode='EDIT')
        rigid_body_joint_constraints_buttons['sna_active_obj_name'] = str(bpy.context.active_object.name)
        if 'EDIT_MESH'==bpy.context.mode:
            pass
        else:
            bpy.ops.object.mode_set('INVOKE_DEFAULT', mode='EDIT')
        for i_A138C in range(len(bpy.context.view_layer.objects.active.data.vertices)):
            if bpy.context.view_layer.objects.active.data.vertices[i_A138C].select:
                rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'].append((float(tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[0]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[0]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[0]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[0]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[1]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[1]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[1]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[1]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[2]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[0] + tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[2]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[1] + tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[2]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[2] + tuple(mathutils.Vector(tuple(bpy.context.view_layer.objects.active.matrix_world)[2]) * mathutils.Vector((bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[0], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[1], bpy.context.view_layer.objects.active.data.vertices[i_A138C].co[2], 1.0)))[3])))
        bpy.context.active_object.select_set(state=False, view_layer=bpy.context.view_layer, )
        bpy.context.view_layer.objects.active = None
        for i_220A4 in range(len(bpy.context.view_layer.objects.selected)):
            bpy.context.view_layer.objects.active = bpy.context.view_layer.objects.selected[i_220A4]
            for i_5D5BB in range(len(rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'])):
                constraint_6841E = bpy.context.view_layer.objects.active.constraints.new(type='RIGID_BODY_JOINT', )
                constraint_6841E.pivot_x = (float(tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[3]))[0]
                constraint_6841E.pivot_y = (float(tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[3]))[1]
                constraint_6841E.pivot_z = (float(tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[0]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[1]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[3]), float(tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[0] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[1] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[2] + tuple(mathutils.Vector(tuple(sna_function_execute_760FC([]))[2]) * mathutils.Vector((rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][0], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][1], rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'][i_5D5BB][2], 1.0)))[3]))[2]
                constraint_6841E.show_pivot = True
                constraint_6841E.target = bpy.data.objects[rigid_body_joint_constraints_buttons['sna_active_obj_name']]
        bpy.ops.object.mode_set('INVOKE_DEFAULT', mode='OBJECT')
        bpy.data.objects[rigid_body_joint_constraints_buttons['sna_active_obj_name']].select_set(state=True, view_layer=bpy.context.view_layer, )
        bpy.context.view_layer.objects.active = bpy.data.objects[rigid_body_joint_constraints_buttons['sna_active_obj_name']]
        rigid_body_joint_constraints_buttons['sna_soft_body_selected_vertex_locations'] = []
        rigid_body_joint_constraints_buttons['sna_active_obj_name'] = ''
        return {"FINISHED"}

    def invoke(self, context, event):
        return self.execute(context)


class SNA_MT_10727(bpy.types.Menu):
    bl_idname = "SNA_MT_10727"
    bl_label = ""

    @classmethod
    def poll(cls, context):
        return not (False)

    def draw(self, context):
        layout = self.layout.menu_pie()
        op = layout.operator('sna.delete_constraints_a5c4f', text='Delete All Constraints', icon_value=19, emboss=True, depress=False)
        box_817F1 = layout.box()
        box_817F1.alert = False
        box_817F1.enabled = True
        box_817F1.active = True
        box_817F1.use_property_split = False
        box_817F1.use_property_decorate = True
        box_817F1.alignment = 'Expand'.upper()
        box_817F1.scale_x = 1.0
        box_817F1.scale_y = 1.0
        if not True: box_817F1.operator_context = "EXEC_DEFAULT"
        box_817F1.label(text='Enable / Disable Object Constraints', icon_value=0)
        op = box_817F1.operator('sna.enables_all_selected_objs_constraints_c322a', text='ON', icon_value=254, emboss=True, depress=False)
        op = box_817F1.operator('sna.disables_all_selected_objs_constraints_63397', text='OFF', icon_value=253, emboss=True, depress=False)
        box_E055E = layout.box()
        box_E055E.alert = False
        box_E055E.enabled = True
        box_E055E.active = True
        box_E055E.use_property_split = False
        box_E055E.use_property_decorate = True
        box_E055E.alignment = 'Expand'.upper()
        box_E055E.scale_x = 1.0
        box_E055E.scale_y = 1.0
        if not True: box_E055E.operator_context = "EXEC_DEFAULT"
        box_E055E.label(text='Show Pivot Points for Rigid Body Joint Constraints', icon_value=0)
        op = box_E055E.operator('sna.show_pivots_on_3281b', text='ON', icon_value=39, emboss=True, depress=False)
        op = box_E055E.operator('sna.show_pivots_off_3d242', text='OFF', icon_value=38, emboss=True, depress=False)
        op = layout.operator('sna.each_vertex_to_rbj_pivot_802e0', text='Each Selected Vertex to RBJ pivot Constraint', icon_value=88, emboss=True, depress=False)
        op = layout.operator('sna.rbj_chain_maker_0508e', text='RBJ Chain Maker', icon_value=259, emboss=True, depress=False)
        op = layout.operator('sna.pivots_for_soft_bodys_dd35f', text='Pivots for Soft Bodys', icon_value=569, emboss=True, depress=False)
        op = layout.operator('sna.rbc_to_rbjc_converting_c5172', text='Convert RBCs to RBJs', icon_value=542, emboss=True, depress=False)
        op = layout.operator('sna.blender_rb_settings_to_bge_rb_settings_c26ad', text='Copy Blender RB settings to BGE RB settings', icon_value=20, emboss=True, depress=False)


def register():
    global _icons
    _icons = bpy.utils.previews.new()
    bpy.utils.register_class(SNA_OT_Blender_Rb_Settings_To_Bge_Rb_Settings_C26Ad)
    bpy.utils.register_class(SNA_OT_Rbc_To_Rbjc_Converting_C5172)
    bpy.utils.register_class(SNA_OT_Pivot_To_3D_Cursor_Fe228)
    bpy.types.OBJECT_PT_bRigidBodyJointConstraint.append(sna_add_to_object_pt_brigidbodyjointconstraint_487D0)
    bpy.types.OBJECT_PT_bRigidBodyJointConstraint.append(sna_add_to_object_pt_brigidbodyjointconstraint_1E5A8)
    bpy.utils.register_class(SNA_OT_Disables_All_Selected_Objs_Constraints_63397)
    bpy.utils.register_class(SNA_OT_Show_Pivots_On_3281B)
    bpy.utils.register_class(SNA_OT_Delete_Constraints_A5C4F)
    bpy.utils.register_class(SNA_OT_Enables_All_Selected_Objs_Constraints_C322A)
    bpy.utils.register_class(SNA_OT_Show_Pivots_Off_3D242)
    bpy.utils.register_class(SNA_OT_Each_Vertex_To_Rbj_Pivot_802E0)
    bpy.utils.register_class(SNA_OT_Rbj_Chain_Maker_0508E)
    bpy.utils.register_class(SNA_OT_Pivot_To_Selction_D59E1)
    bpy.utils.register_class(SNA_OT_Pivots_For_Soft_Bodys_Dd35F)
    bpy.utils.register_class(SNA_MT_10727)
    kc = bpy.context.window_manager.keyconfigs.addon
    km = kc.keymaps.new(name='3D View', space_type='VIEW_3D')
    kmi = km.keymap_items.new('wm.call_menu_pie', 'V', 'PRESS',
        ctrl=False, alt=False, shift=True, repeat=False)
    kmi.properties.name = 'SNA_MT_10727'
    addon_keymaps['1DF52'] = (km, kmi)


def unregister():
    global _icons
    bpy.utils.previews.remove(_icons)
    wm = bpy.context.window_manager
    kc = wm.keyconfigs.addon
    for km, kmi in addon_keymaps.values():
        km.keymap_items.remove(kmi)
    addon_keymaps.clear()
    bpy.utils.unregister_class(SNA_OT_Blender_Rb_Settings_To_Bge_Rb_Settings_C26Ad)
    bpy.utils.unregister_class(SNA_OT_Rbc_To_Rbjc_Converting_C5172)
    bpy.utils.unregister_class(SNA_OT_Pivot_To_3D_Cursor_Fe228)
    bpy.types.OBJECT_PT_bRigidBodyJointConstraint.remove(sna_add_to_object_pt_brigidbodyjointconstraint_487D0)
    bpy.types.OBJECT_PT_bRigidBodyJointConstraint.remove(sna_add_to_object_pt_brigidbodyjointconstraint_1E5A8)
    bpy.utils.unregister_class(SNA_OT_Disables_All_Selected_Objs_Constraints_63397)
    bpy.utils.unregister_class(SNA_OT_Show_Pivots_On_3281B)
    bpy.utils.unregister_class(SNA_OT_Delete_Constraints_A5C4F)
    bpy.utils.unregister_class(SNA_OT_Enables_All_Selected_Objs_Constraints_C322A)
    bpy.utils.unregister_class(SNA_OT_Show_Pivots_Off_3D242)
    bpy.utils.unregister_class(SNA_OT_Each_Vertex_To_Rbj_Pivot_802E0)
    bpy.utils.unregister_class(SNA_OT_Rbj_Chain_Maker_0508E)
    bpy.utils.unregister_class(SNA_OT_Pivot_To_Selction_D59E1)
    bpy.utils.unregister_class(SNA_OT_Pivots_For_Soft_Bodys_Dd35F)
    bpy.utils.unregister_class(SNA_MT_10727)
