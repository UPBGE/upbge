# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

# <pep8 compliant>
import bpy
from bpy.types import Panel, Menu

import re

from bl_operators.logic_nodes_bindings import (
    logic_nodes_available,
    logic_nodes_binding_status,
    logic_nodes_clear_bindings,
    logic_nodes_get_context_tree,
    logic_nodes_has_bindings,
)


PascalCasePattern = r"((?<=[a-z])[A-Z]|(?<!\A)[A-Z](?=[a-z]))"
ReplacementPattern = r" \1"


def split_pascal_case(text):
    return re.sub(PascalCasePattern, ReplacementPattern, text)


class GameButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "game"
    bl_order = 1000


class GAME_PT_game_object(GameButtonsPanel, Panel):
    bl_label = "Game Object"

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return ob and ob.game

    def draw(self, context):
        layout = self.layout

        ob = context.active_object
        game = ob.game

        row = layout.row()

        if game.custom_object and game.custom_object.name:
            obj = game.custom_object

            box = layout.box()
            row = box.row()

            row.prop(obj, "show_expanded", text="", emboss=False)
            row.label(text=split_pascal_case(obj.name))

            row.operator("logic.custom_object_reload", text="", icon="RECOVER_LAST")
            row.operator("logic.custom_object_remove", text="", icon="X")

            if obj.show_expanded and len(obj.properties) > 0:
                box = box.box()
                for prop in obj.properties:
                    row = box.row()
                    row.label(text=split_pascal_case(prop.name))
                    col = row.column()
                    col.prop(prop, "value", text="")
        else:
            row.operator("logic.custom_object_register", icon="PLUS", text="Select")
            row.operator("logic.custom_object_create", icon="PLUS", text="Create")


class GAME_PT_game_components(GameButtonsPanel, Panel):
    bl_label = "Game Components"

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return ob and ob.game

    def draw(self, context):
        layout = self.layout

        ob = context.active_object
        game = ob.game

        row = layout.row()

        row.operator("logic.python_component_register", icon="PLUS", text="Add")
        row.operator("logic.python_component_create", icon="PLUS", text="Create")

        for i, c in enumerate(game.components):
            box = layout.box()
            row = box.row()

            row.prop(c, "show_expanded", text="", emboss=False)
            row.label(text=split_pascal_case(c.name))
            row.context_pointer_set("component", c)
            row.menu("GAME_MT_component_context_menu", icon="DOWNARROW_HLT", text="")

            row.operator("logic.python_component_remove", text="", icon="X").index = i

            if c.show_expanded and len(c.properties) > 0:
                box = box.box()
                for prop in c.properties:
                    row = box.row()
                    row.label(text=split_pascal_case(prop.name))
                    col = row.column()
                    col.prop(prop, "value", text="")


class GAME_PT_logic_nodes(GameButtonsPanel, Panel):
    bl_label = "Logic Nodes"

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return ob and ob.game and (logic_nodes_available() or logic_nodes_has_bindings(ob))

    def draw(self, context):
        layout = self.layout
        ob = context.active_object

        if not logic_nodes_available():
            layout.label(text="Native Logic Nodes unavailable", icon='ERROR')
            return

        apply_box = layout.box()
        apply_col = apply_box.column()
        apply_col.scale_y = 1.2
        context_tree = logic_nodes_get_context_tree(context)
        if context_tree is not None:
            apply_col.label(text=f"Active tree: {context_tree.name}", icon='NODETREE')
        else:
            apply_col.label(text="Open a Logic Node Tree in the editor", icon='INFO')
        apply_col.operator("object.logic_nodes_apply_tree", icon="PREFERENCES", text="Apply To Selected")

        bindings = ob.game.logic_node_bindings
        if len(bindings) == 0:
            layout.label(text="No logic trees applied", icon='INFO')
            return

        layout.label(text="Applied Logic Trees:")
        for index, binding in enumerate(bindings):
            tree_name = (binding.tree_name or "").strip()
            if not tree_name:
                continue

            box = layout.box()
            header = box.row(align=True)
            header.prop(binding, "enabled", text="")
            header.label(text=tree_name, icon='NODETREE')
            op = header.operator("object.logic_nodes_binding_remove", text="", icon='X')
            op.index = index

            body = box.row(align=True)
            body.prop(binding, "tree", text="")
            body.operator(
                "object.logic_nodes_find_tree",
                text="Edit",
                icon='NODETREE',
            ).tree_name = tree_name

            status_text, status_icon = logic_nodes_binding_status(context, binding)
            if status_text:
                box.label(text=status_text, icon=status_icon)

        layout.operator("object.logic_nodes_binding_clear", icon="TRASH", text="Clear All Trees")


class GAME_MT_component_context_menu(Menu):
    bl_label = "Game Component"


    @classmethod
    def poll(cls, context):
        ob = context.active_object

        return ob and ob.game and ob.game.components

    def draw(self, context):
        layout = self.layout

        components = context.active_object.game.components
        # Use RNA identity instead of the (non-unique) name. This ensures we act on the exact instance.
        comp_ptr = context.component.as_pointer()
        index = next((i for i, comp in enumerate(components)
                    if comp.as_pointer() == comp_ptr), -1)

        layout.operator("logic.python_component_reload", icon="RECOVER_LAST").index = index
        layout.separator()
        layout.operator("logic.python_component_move_up", icon="TRIA_UP").index = index
        layout.operator("logic.python_component_move_down", icon="TRIA_DOWN").index = index


class GAME_PT_game_properties(GameButtonsPanel, Panel):
    bl_label = "Game Properties"

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return ob and ob.game

    def draw(self, context):
        layout = self.layout

        ob = context.active_object
        game = ob.game
        is_font = (ob.type == 'FONT')

        if is_font:
            prop_index = game.properties.find("Text")
            if prop_index != -1:
                layout.operator("object.game_property_remove", text="Remove Text Game Property",
                                icon='X').index = prop_index
                row = layout.row()
                sub = row.row()
                sub.enabled = 0
                prop = game.properties[prop_index]
                sub.prop(prop, "name", text="")
                row.prop(prop, "type", text="")
                # get the property from the body, not the game property
                # note, don't do this - it's too slow and body can potentially be a really long string.
                # ~ row.prop(ob.data, "body", text="")
                row.label(text="See Text Object")
            else:
                props = layout.operator("object.game_property_new", text="Add Text Game Property", icon='PLUS')
                props.name = "Text"
                props.type = 'STRING'

        props = layout.operator("object.game_property_new", text="Add Game Property", icon='PLUS')
        props.name = ""

        for i, prop in enumerate(game.properties):

            if is_font and i == prop_index:
                continue

            box = layout.box()
            row = box.row()
            row.prop(prop, "name", text="")
            row.prop(prop, "type", text="")
            row.prop(prop, "value", text="")
            row.prop(prop, "show_debug", text="", toggle=True, icon='INFO')
            sub = row.row(align=True)
            props = sub.operator("object.game_property_move", text="", icon='TRIA_UP')
            props.index = i
            props.direction = 'UP'
            props = sub.operator("object.game_property_move", text="", icon='TRIA_DOWN')
            props.index = i
            props.direction = 'DOWN'
            row.operator("object.game_property_remove", text="", icon='X', emboss=False).index = i


class PhysicsButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "physics"
    bl_order = 1000


class PHYSICS_PT_game_physics(PhysicsButtonsPanel, Panel):
    bl_label = "Game Physics"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        rd = context.scene.render
        return ob and ob.game and (rd.engine in cls.COMPAT_ENGINES)

    def draw_vehicle_chassis(self, layout, game, vehicle, is_jolt):
        """Draw chassis-specific settings."""
        traction_managed_by_preset = vehicle.lsd_preset in {'FWD_ROAD', 'RWD_DRIFT', 'AWD_RALLY', 'OFFROAD'}

        split = layout.split()
        col = split.column()
        col.label(text="Chassis:")
        if is_jolt:
            col.prop(vehicle, "solver_iterations")
        col.prop(game, "mass")
        if is_jolt:
            col.prop(game, "elasticity", slider=True)
            col.prop(game, "velocity_max", text="Linear Velocity Max")
            col.prop(game, "angular_velocity_max", text="Angular Velocity Max")
        if not is_jolt:
            col.prop(vehicle, "engine_force")
            col.prop(vehicle, "brake")

        col = split.column()
        col.label(text="Object:")
        if not is_jolt:
            col.prop(game, "use_actor")
        col.prop(game, "use_ghost")
        col.prop(game, "use_sleep")
        col.label(text="Damping:")
        sub = col.column(align=True)
        sub.prop(game, "damping", text="Translation", slider=True)
        sub.prop(game, "rotation_damping", text="Rotation", slider=True)
        col.label(text="Body Friction:")
        col.prop(game, "friction")
        if not is_jolt:
            col.prop(game, "rolling_friction")
            sub = col.column()
            sub.prop(game, "use_anisotropic_friction")
            subsub = sub.column()
            subsub.active = game.use_anisotropic_friction
            subsub.prop(game, "friction_coefficients", text="", slider=True)

        layout.separator()

        split = layout.split()
        col = split.column()
        col.prop(game, "gravity_factor")
        col.prop(vehicle, "max_pitch_roll_angle")
        col.prop(vehicle, "anti_roll_front")
        col.prop(vehicle, "anti_roll_rear")
        if is_jolt:
            col.prop(vehicle, "chassis_roll_influence")
            col.prop(vehicle, "center_of_mass_offset", slider=True)
        if not is_jolt:
            col.prop(vehicle, "handbrake_torque")

        col = split.column()
        col.prop(vehicle, "forward_axis")
        col.prop(vehicle, "up_axis")
        if is_jolt:
            col.prop(vehicle, "engine_torque")
            col.prop(vehicle, "lsd_preset")
            if traction_managed_by_preset:
                col.label(text="Preset controls linked wheel traction flags.", icon='INFO')
            sub = col.column()
            sub.prop(vehicle, "limited_slip_ratio")
            sub.enabled = (vehicle.lsd_preset == 'CUSTOM')
            if vehicle.lsd_preset != 'CUSTOM':
                col.label(text="Layout: " + vehicle.detected_layout)
            simple_drive_col = col.column(align=True)
            simple_drive_col.prop(vehicle, "steering_speed")
            simple_drive_col.prop(vehicle, "high_speed_steering_reduction")
        if not is_jolt:
            col.prop(vehicle, "wheel_inertia")
            col.prop(vehicle, "wheel_angular_damping")

        layout.separator()
        col = layout.column()
        col.label(text="Lock Translation:")
        row = col.row()
        row.prop(game, "lock_location_x", text="X")
        row.prop(game, "lock_location_y", text="Y")
        row.prop(game, "lock_location_z", text="Z")

        col.label(text="Lock Rotation:")
        row = col.row()
        row.prop(game, "lock_rotation_x", text="X")
        row.prop(game, "lock_rotation_y", text="Y")
        row.prop(game, "lock_rotation_z", text="Z")

    def draw_motorcycle_chassis(self, layout, game, vehicle, is_jolt):
        """Draw motorcycle chassis settings (Jolt MotorcycleController)."""
        if not is_jolt:
            layout.label(text="Motorcycle requires the Jolt physics backend.",
                         icon='ERROR')
            return

        split = layout.split()
        col = split.column()
        col.label(text="Chassis:")
        col.prop(vehicle, "solver_iterations")
        col.prop(game, "mass")
        col.prop(game, "elasticity", slider=True)
        col.prop(game, "velocity_max", text="Linear Velocity Max")
        col.prop(game, "angular_velocity_max", text="Angular Velocity Max")

        col = split.column()
        col.label(text="Object:")
        if not is_jolt:
            col.prop(game, "use_actor")
        col.prop(game, "use_ghost")
        col.prop(game, "use_sleep")
        col.label(text="Damping:")
        sub = col.column(align=True)
        sub.prop(game, "damping", text="Translation", slider=True)
        sub.prop(game, "rotation_damping", text="Rotation", slider=True)
        col.label(text="Body Friction:")
        col.prop(game, "friction")

        layout.separator()

        split = layout.split()
        col = split.column()
        col.prop(game, "gravity_factor")
        col.prop(vehicle, "max_pitch_roll_angle")

        col = split.column()
        col.prop(vehicle, "forward_axis")
        col.prop(vehicle, "up_axis")
        col.prop(vehicle, "engine_torque")
        # High-speed steering reduction is redundant on motorcycles: Jolt's
        # MotorcycleController applies a physics-correct steering clamp
        # (asin(wheelbase * tan(maxLean) * g / v^2)) whenever Lean Steering
        # Limit is enabled, which always wins above low speeds. Only expose
        # the input smoothing time.
        col.prop(vehicle, "steering_speed")

        layout.separator()
        box = layout.box()
        box.label(text="Lean Controller (auto-balance):")
        row = box.row()
        row.prop(vehicle, "mc_enable_lean_controller")
        row.prop(vehicle, "mc_enable_lean_steering_limit")

        split2 = box.split()
        lcol = split2.column(align=True)
        max_lean_col = lcol.column(align=True)
        max_lean_col.active = (vehicle.mc_enable_lean_controller or
                       vehicle.mc_enable_lean_steering_limit)
        max_lean_col.prop(vehicle, "mc_max_lean_angle")
        smoothing_col = lcol.column(align=True)
        smoothing_col.active = vehicle.mc_enable_lean_controller
        smoothing_col.prop(vehicle, "mc_lean_smoothing_factor", slider=True)

        rcol = split2.column(align=True)
        rcol.active = vehicle.mc_enable_lean_controller
        rcol.prop(vehicle, "mc_lean_spring_constant")
        rcol.prop(vehicle, "mc_lean_spring_damping")
        rcol.prop(vehicle, "mc_lean_spring_integration_coefficient")
        rcol.prop(vehicle, "mc_lean_spring_integration_decay")

        force_box = layout.box()
        force_box.label(text="Suspension Force Points:")
        force_box.prop(vehicle, "mc_override_suspension_force_points")
        force_box.label(text="Disable to use Jolt's contact-point behavior.")

        layout.separator()
        col = layout.column()
        col.label(text="Lock Translation:")
        row = col.row()
        row.prop(game, "lock_location_x", text="X")
        row.prop(game, "lock_location_y", text="Y")
        row.prop(game, "lock_location_z", text="Z")

        col.label(text="Lock Rotation:")
        row = col.row()
        row.prop(game, "lock_rotation_x", text="X")
        row.prop(game, "lock_rotation_y", text="Y")
        row.prop(game, "lock_rotation_z", text="Z")

    def draw_vehicle_wheel(self, layout, vehicle, is_jolt):
        """Draw wheel-specific settings."""
        layout.prop(vehicle, "chassis_object")

        chassis_vehicle = None
        traction_managed_by_preset = False
        if vehicle.chassis_object and vehicle.chassis_object.game and vehicle.chassis_object.game.vehicle:
            chassis_vehicle = vehicle.chassis_object.game.vehicle
            traction_managed_by_preset = chassis_vehicle.lsd_preset in {'FWD_ROAD', 'RWD_DRIFT', 'AWD_RALLY', 'OFFROAD'}

        col = layout.column()
        traction_col = col.column()
        traction_col.enabled = not traction_managed_by_preset
        traction_col.prop(vehicle, "use_as_traction")
        if traction_managed_by_preset:
            col.label(text="Traction is controlled by the chassis LSD preset.", icon='INFO')
            col.label(text="Set chassis LSD Preset to Custom or Open to edit it.")
        col.prop(vehicle, "use_as_steering")

        if is_jolt:
            steer_col = col.column()
            steer_col.active = vehicle.use_as_steering
            steer_col.prop(vehicle, "wheel_steering", text="Max Steer Angle")

        col.prop(vehicle, "use_as_brake")

        if is_jolt:
            brake_col = col.column()
            brake_col.active = vehicle.use_as_brake
            brake_col.prop(vehicle, "wheel_brake")

        # Motorcycles have no parking/handbrake lever in Jolt's MotorcycleController;
        # only the normal brake channel is physically meaningful on a two-wheeler.
        if vehicle.vehicle_type != 'MOTORCYCLE_WHEEL':
            col.prop(vehicle, "use_handbrake")

        if is_jolt:
            col.prop(vehicle, "use_auto_inertia")
            sub = col.column()
            sub.active = not vehicle.use_auto_inertia
            sub.prop(vehicle, "wheel_inertia")
            col.prop(vehicle, "wheel_angular_damping")
        else:
            row = layout.row(align=True)
            row.prop(vehicle, "use_steering_override", text="Steering Override")
            sub = row.row(align=True)
            sub.active = vehicle.use_steering_override
            sub.prop(vehicle, "wheel_steering", text="")

        split = layout.split()
        col = split.column()
        col.label(text="Wheel Setup:")
        col.prop(vehicle, "collision_mode")
        col.prop(vehicle, "wheel_ray_mask")
        col.prop(vehicle, "use_auto_radius")
        radius_col = col.column()
        radius_col.active = not vehicle.use_auto_radius
        radius_col.prop(vehicle, "wheel_radius")
        col.prop(vehicle, "use_auto_width")
        width_col = col.column()
        width_col.active = not vehicle.use_auto_width
        width_col.prop(vehicle, "wheel_width")
        if is_jolt:
            col.prop(vehicle, "use_combined_friction_axes")
            if vehicle.use_combined_friction_axes:
                col.prop(vehicle, "wheel_friction_slip")
            else:
                col.prop(vehicle, "wheel_longitudinal_friction")
                col.prop(vehicle, "wheel_lateral_friction")
        else:
            col.prop(vehicle, "wheel_friction_slip")
            col.prop(vehicle, "wheel_roll_influence")

        col = split.column()
        col.label(text="Suspension:")
        col.prop(vehicle, "suspension_stiffness")
        col.prop(vehicle, "suspension_travel")
        if is_jolt:
            col.prop(vehicle, "damping_compression", text="Damping")
        else:
            col.prop(vehicle, "damping_compression")
            col.prop(vehicle, "damping_relaxation")

        transform = layout.column()
        transform.label(text="Transform:")
        transform.prop(vehicle, "use_derive_from_transform")
        manual = transform.column()
        manual.active = not vehicle.use_derive_from_transform
        manual.prop(vehicle, "connection_point")

        row = manual.row()
        row.label(text="Down Direction:")
        row.prop(vehicle, "down_direction_axis", text="")

        row = manual.row()
        row.label(text="Axle Direction:")
        row.prop(vehicle, "axle_direction_axis", text="")

    def draw(self, context):
        layout = self.layout

        ob = context.active_object
        game = ob.game
        soft = ob.game.soft_body
        gs = context.scene.game_settings
        is_jolt = (gs.physics_engine == 'JOLT')

        layout.prop(game, "physics_type")
        layout.separator()

        physics_type = game.physics_type

        if physics_type == 'CHARACTER':
            if not is_jolt:
                layout.prop(game, "use_actor")
                layout.prop(ob, "hide_render", text="Invisible")  # out of place but useful

            layout.separator()

            split = layout.split()

            col = split.column()
            col.prop(game, "step_height", slider=True)
            col.prop(game, "fall_speed")
            col.prop(game, "max_slope")
            col = split.column()
            col.prop(game, "jump_speed")
            col.prop(game, "jump_max")

        elif physics_type in {'DYNAMIC', 'RIGID_BODY'}:
            split = layout.split()

            col = split.column()
            if not is_jolt:
                col.prop(game, "use_actor")
            col.prop(game, "use_ghost")
            if not is_jolt:
                col.prop(ob, "hide_render", text="Invisible")  # out of place but useful

            col = split.column()
            if not is_jolt:
                col.prop(game, "use_physics_fh")
                col.prop(game, "use_rotate_from_normal")
            col.prop(game, "use_sleep")

            layout.separator()

            split = layout.split()

            col = split.column()
            col.label(text="Attributes:")
            col.prop(game, "mass")
            if not is_jolt:
                col.prop(game, "radius")
            col.prop(game, "form_factor", slider=True)
            col.prop(game, "elasticity", slider=True)

            col.label(text="Linear Velocity:")
            sub = col.column(align=True)
            sub.prop(game, "velocity_min", text="Minimum")
            sub.prop(game, "velocity_max", text="Maximum")

            col = split.column()
            col.label(text="Friction:")
            col.prop(game, "friction")
            if not is_jolt:
                col.prop(game, "rolling_friction")
                col.separator()

                sub = col.column()
                sub.prop(game, "use_anisotropic_friction")
                subsub = sub.column()
                subsub.active = game.use_anisotropic_friction
                subsub.prop(game, "friction_coefficients", text="", slider=True)
            else:
                col.separator()

            split = layout.split()
            col = split.column()
            col.label(text="Angular velocity:")
            sub = col.column(align=True)
            sub.prop(game, "angular_velocity_min", text="Minimum")
            sub.prop(game, "angular_velocity_max", text="Maximum")

            col = split.column()
            col.label(text="Damping:")
            sub = col.column(align=True)
            sub.prop(game, "damping", text="Translation", slider=True)
            sub.prop(game, "rotation_damping", text="Rotation", slider=True)

            layout.separator()
            split = layout.split()

            col = split.column()
            col.prop(game, "use_ccd_rigid_body")
            if not is_jolt:
                sub = col.column()
                sub.active = game.use_ccd_rigid_body
                sub.prop(game, "ccd_motion_threshold")
                sub.prop(game, "ccd_swept_sphere_radius")

            if is_jolt:
                col = split.column()
                col.label(text="Jolt Physics:")
                col.prop(game, "gravity_factor")
                col.prop(game, "use_jolt_solver_iterations_override", text="Override Solver Iterations")
                sub = col.column(align=True)
                sub.active = game.use_jolt_solver_iterations_override
                sub.prop(game, "jolt_velocity_solver_iterations", text="Velocity Iterations")
                sub.prop(game, "jolt_position_solver_iterations", text="Position Iterations")

            layout.separator()
            col = layout.column()

            col.label(text="Lock Translation:")
            row = col.row()
            row.prop(game, "lock_location_x", text="X")
            row.prop(game, "lock_location_y", text="Y")
            row.prop(game, "lock_location_z", text="Z")

        elif physics_type == 'VEHICLE':
            vehicle = game.vehicle

            layout.prop(vehicle, "vehicle_type")

            if vehicle.vehicle_type == 'CHASSIS':
                self.draw_vehicle_chassis(layout, game, vehicle, is_jolt)
            elif vehicle.vehicle_type == 'WHEEL':
                self.draw_vehicle_wheel(layout, vehicle, is_jolt)
            elif vehicle.vehicle_type == 'MOTORCYCLE_CHASSIS':
                self.draw_motorcycle_chassis(layout, game, vehicle, is_jolt)
            elif vehicle.vehicle_type == 'MOTORCYCLE_WHEEL':
                self.draw_vehicle_wheel(layout, vehicle, is_jolt)

        if physics_type == 'RIGID_BODY':
            col = layout.column()

            col.label(text="Lock Rotation:")
            row = col.row()
            row.prop(game, "lock_rotation_x", text="X")
            row.prop(game, "lock_rotation_y", text="Y")
            row.prop(game, "lock_rotation_z", text="Z")

        elif physics_type == 'SOFT_BODY':
            col = layout.column()
            if not is_jolt:
                col.prop(game, "use_actor")
                #col.prop(game, "use_ghost") Seems not supported in bullet for SoftBodies
                col.prop(ob, "hide_render", text="Invisible")

            layout.separator()

            split = layout.split()

            col = split.column()
            col.label(text="General Attributes:")
            col.prop(game, "mass")
            col.prop(soft, "linear_stiffness", slider=True)
            col.prop(soft, "shear_stiffness", slider=True)
            col.prop(soft, "angular_stiffness", slider=True)
            col.prop(game, "elasticity", slider=True)
            col.prop(game, "gravity_factor", slider=True)
            col.prop(soft, "dynamic_friction", slider=True)
            col.prop(soft, "kdp", text="Damping", slider=True)
            col.prop(soft, "collision_margin", slider=True)

            if not is_jolt:
                col.prop(soft, "kvcf", text="Velocity Correction")

            col.prop(soft, "use_bending_constraints", text="Bending Constraints")

            if not is_jolt:
                sub = col.column()
                sub.active = soft.use_bending_constraints
                sub.prop(soft, "bending_distance")

            if is_jolt:
                col.prop(soft, "use_lra_constraints", text="Long Range Attachment")
                sub = col.column()
                sub.active = soft.use_lra_constraints
                sub.prop(soft, "lra_type", text="LRA Type")
                col.prop(soft, "use_faces_double_sided", text="Double-Sided Faces")

                col.separator()
                col.label(text="Plasticity (Jolt):")
                col.prop(soft, "use_plasticity", text="Plasticity")
                sub = col.column()
                sub.active = soft.use_plasticity
                sub.prop(soft, "plastic_threshold", text="Plastic Threshold")
                sub.prop(soft, "plasticity_strength", text="Plasticity Strength")
                sub.prop(soft, "plastic_max_deform", text="Max Permanent Deform")
                sub.prop(soft, "plastic_repair_rate", text="Repair Rate")

                col.separator()
                col.label(text="Vertex Pinning (Jolt):")
                col.prop_search(soft, "pin_vgroup", ob, "vertex_groups", text="Pin Group")
                has_pin_group = bool(soft.pin_vgroup)
                pin_col = col.column()
                pin_col.active = has_pin_group
                pin_col.prop(soft, "pin_weight_threshold", text="Vertex Weight Pin Threshold", slider=True)
                pin_col.prop(soft, "pin_object", text="", icon='OBJECT_DATA')
                has_pin_obj = bool(soft.pin_object)
                no_pin_col = col.column()
                no_pin_col.active = has_pin_obj
                no_pin_col.prop(soft, "use_no_pin_collision", text="No Force on Pin Object")
                no_pin_col.prop(soft, "use_pin_transform_follow", text="Follow Pin Transform")

            if not is_jolt:
                col.prop(soft, "use_shape_match")
                sub = col.column()
                sub.active = soft.use_shape_match
                sub.prop(soft, "shape_threshold", slider=True)

            col.label(text="Solver Iterations:")
            col.prop(soft, "position_solver_iterations", text="Position Solver")

            if not is_jolt:
                col.prop(soft, "velocity_solver_iterations", text="Velocity Solver")
                col.prop(soft, "cluster_solver_iterations", text="Cluster Solver")
                col.prop(soft, "drift_solver_iterations", text="Drift Solver")

            col = split.column()

            if not is_jolt:
                col.label(text="Hardness:")
                col.prop(soft, "kchr", text="Rigid Contacts", slider=True)
                col.prop(soft, "kkhr", text="Kinetic Contacts", slider=True)
                col.prop(soft, "kshr", text="Soft Contacts", slider=True)
                col.prop(soft, "kahr", text="Anchors", slider=True)

                col.label(text="Cluster Collision:")
                col.prop(soft, "use_cluster_rigid_to_softbody")
                col.prop(soft, "use_cluster_soft_to_softbody")
                sub = col.column()
                sub.active = soft.use_cluster_rigid_to_softbody or soft.use_cluster_soft_to_softbody
                sub.prop(soft, "cluster_iterations", text="Iterations")
                sub.prop(soft, "ksrhr_cl", text="Rigid Hardness", slider=True)
                sub.prop(soft, "kskhr_cl", text="Kinetic Hardness", slider=True)
                sub.prop(soft, "ksshr_cl", text="Soft Hardness", slider=True)
                sub.prop(soft, "ksr_split_cl", text="Rigid Impulse Split", slider=True)
                sub.prop(soft, "ksk_split_cl", text="Kinetic Impulse Split", slider=True)
                sub.prop(soft, "kss_split_cl", text="Soft Impulse Split", slider=True)

            split = layout.split()

            col = split.column()
            col.label(text="Volume:")
            col.prop(soft, "kpr", text="Pressure Coefficient")
            if not is_jolt:
                col.prop(soft, "kvc", text="Volume Conservation")

            if not is_jolt:
                col = split.column()
                col.label(text="Aerodynamics:")
                col.prop(soft, "kdg", text="Drag Coefficient")
                col.prop(soft, "klf", text="Lift Coefficient")

        elif physics_type == 'STATIC':
            col = layout.column()
            if not is_jolt:
                col.prop(game, "use_actor")
            col.prop(game, "use_ghost")
            if not is_jolt:
                col.prop(ob, "hide_render", text="Invisible")

            layout.separator()

            split = layout.split()

            col = split.column()
            col.label(text="Attributes:")
            if not is_jolt:
                col.prop(game, "radius")
            col.prop(game, "elasticity", slider=True)
            col.label(text="Friction:")
            col.prop(game, "friction")
            if not is_jolt:
                col.prop(game, "rolling_friction")

                col = split.column()
                sub = col.column()
                sub.prop(game, "use_anisotropic_friction")
                subsub = sub.column()
                subsub.active = game.use_anisotropic_friction
                subsub.prop(game, "friction_coefficients", text="", slider=True)

        elif physics_type == 'SENSOR':
            col = layout.column()
            if is_jolt:
                col.prop(game, "use_jolt_sensor_static_detection", text="Include Static Objects")
            else:
                col.prop(game, "use_actor", text="Detect Actors")
                col.prop(ob, "hide_render", text="Invisible")

        elif physics_type in {'INVISIBLE', 'NO_COLLISION', 'OCCLUDER'}:
            if not is_jolt:
                layout.prop(ob, "hide_render", text="Invisible")

        elif physics_type == 'NAVMESH':
            layout.operator("mesh.navmesh_face_copy")
            layout.operator("mesh.navmesh_face_add")

            layout.separator()

            layout.operator("mesh.navmesh_reset")
            layout.operator("mesh.navmesh_clear")

        if not is_jolt and physics_type in {"STATIC", "DYNAMIC", "RIGID_BODY"}:
            row = layout.row()
            row.label(text="Force Field:")

            row = layout.row()
            row.prop(game, "fh_force")
            row.prop(game, "fh_damping", slider=True)

            row = layout.row()
            row.prop(game, "fh_distance")
            row.prop(game, "use_fh_normal")


class PHYSICS_PT_game_force_field(PhysicsButtonsPanel, Panel):
    bl_label = "Hover Spring"
    bl_parent_id = "PHYSICS_PT_game_physics"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        if not (ob and ob.game and context.scene.render.engine in cls.COMPAT_ENGINES):
            return False
        if context.scene.game_settings.physics_engine != 'JOLT':
            return False
        return ob.game.physics_type in {'DYNAMIC', 'RIGID_BODY'}

    def draw(self, context):
        layout = self.layout
        game = context.active_object.game

        layout.prop(game, "use_physics_fh", text="Use Hover Spring")

        settings = layout.column()
        settings.active = game.use_physics_fh

        split = settings.split()
        col = split.column()
        col.prop(game, "use_rotate_from_normal")
        col.prop(game, "fh_force", text="Spring Strength")
        col.prop(game, "fh_distance", text="Hover Distance")

        col = split.column()
        col.prop(game, "fh_damping", slider=True)
        col.prop(game, "use_fh_normal", text="Align to Normal")


class PHYSICS_PT_game_buoyancy(PhysicsButtonsPanel, Panel):
    bl_label = "Fluid Volume"
    bl_parent_id = "PHYSICS_PT_game_physics"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        if not (ob and ob.game and context.scene.render.engine in cls.COMPAT_ENGINES):
            return False
        if context.scene.game_settings.physics_engine != 'JOLT':
            return False
        return ob.game.physics_type == 'SENSOR'

    def draw(self, context):
        layout = self.layout
        game = context.active_object.game

        layout.prop(game, "use_jolt_buoyancy", text="Use as Fluid Volume")

        settings = layout.column()
        settings.active = game.use_jolt_buoyancy

        split = settings.split()
        col = split.column()
        col.prop(game, "jolt_buoyancy", text="Float Strength")
        col.prop(game, "jolt_buoyancy_linear_drag", text="Linear Drag")

        col = split.column()
        col.prop(game, "jolt_buoyancy_angular_drag", text="Angular Drag")
        col.prop(game, "jolt_buoyancy_velocity", text="Fluid Velocity")


class PHYSICS_PT_game_collision_bounds(PhysicsButtonsPanel, Panel):
    bl_label = "Collision Bounds"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        game = context.object.game
        gs = context.scene.game_settings
        if not (context.scene.render.engine in cls.COMPAT_ENGINES):
            return False
        if game.physics_type not in {'SENSOR', 'STATIC', 'DYNAMIC', 'RIGID_BODY', 'CHARACTER', 'SOFT_BODY', 'VEHICLE'}:
            return False
        if gs.physics_engine == 'JOLT' and game.physics_type == 'SOFT_BODY':
            return False
        if (game.physics_type == 'VEHICLE' and game.vehicle and
                game.vehicle.vehicle_type in {'WHEEL', 'MOTORCYCLE_WHEEL'}):
            return False
        return True

    def draw_header(self, context):
        gs = context.scene.game_settings
        is_jolt = (gs.physics_engine == 'JOLT')
        if not is_jolt:
            game = context.active_object.game
            self.layout.prop(game, "use_collision_bounds", text="")

    def draw(self, context):
        layout = self.layout

        game = context.active_object.game
        gs = context.scene.game_settings
        is_jolt = (gs.physics_engine == 'JOLT')

        split = layout.split()
        if not is_jolt:
            split.active = game.use_collision_bounds

        col = split.column()
        col.prop(game, "collision_bounds_type", text="Bounds")

        row = col.row()

        margin_row = row.row()
        if is_jolt:
            margin_row.active = game.collision_bounds_type in {
                'BOX', 'CYLINDER', 'CONE', 'CONVEX_HULL'}
        margin_text = "Rounded Corners" if is_jolt else "Margin"
        margin_row.prop(game, "collision_margin", text=margin_text, slider=True)

        sub = row.row()
        sub.active = game.physics_type not in {'SOFT_BODY', 'CHARACTER'}
        sub.prop(game, "use_collision_compound", text="Compound")

        layout.separator()
        if is_jolt:
            layout.prop(game, "collision_layers")
        else:
            split = layout.split()
            col = split.column()
            col.prop(game, "collision_group")
            col = split.column()
            col.prop(game, "collision_mask")


class PHYSICS_PT_game_collision_filtering(PhysicsButtonsPanel, Panel):
    bl_label = "Collision Filtering"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        game = context.object.game
        gs = context.scene.game_settings
        if not (context.scene.render.engine in cls.COMPAT_ENGINES):
            return False
        return gs.physics_engine == 'JOLT' and game.physics_type == 'SOFT_BODY'

    def draw(self, context):
        layout = self.layout
        game = context.active_object.game

        layout.prop(game, "collision_layers")


class PHYSICS_PT_game_obstacles(PhysicsButtonsPanel, Panel):
    bl_label = "Create Obstacle"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        game = context.object.game
        if not (context.scene.render.engine in cls.COMPAT_ENGINES):
            return False
        if game.physics_type == 'VEHICLE':
            return False
        if game.physics_type not in {'SENSOR', 'STATIC', 'DYNAMIC', 'RIGID_BODY', 'SOFT_BODY', 'CHARACTER', 'NO_COLLISION'}:
            return False
        return True

    def draw_header(self, context):
        game = context.active_object.game

        self.layout.prop(game, "use_obstacle_create", text="")

    def draw(self, context):
        layout = self.layout

        game = context.active_object.game

        layout.active = game.use_obstacle_create

        row = layout.row()
        row.prop(game, "obstacle_radius", text="Radius")
        row.label()


class RenderButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "render"

    @classmethod
    def poll(cls, context):
        return (context.scene.render.engine in cls.COMPAT_ENGINES)


class SceneButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "scene"


class SCENE_PT_game_physics(SceneButtonsPanel, Panel):
    bl_label = "Game Physics"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene.render.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings

        layout.prop(gs, "physics_engine", text="Engine")
        if gs.physics_engine != 'NONE':
            if gs.physics_engine != 'JOLT':
                layout.prop(gs, "physics_solver")
            layout.prop(gs, "physics_gravity", text="Gravity")

            # Fixed Physics Timestep controls
            layout.separator()
            row = layout.row()
            row.label(text="Physics Timestep Method:")
            row.prop(gs, "physics_timestep_method", text="")
            
            if gs.physics_timestep_method == 'FIXED':
                # For Fixed mode: put Logic+Physics Steps Per Second and Max Physics Steps side by side
                split_rate = layout.split()
                col_left = split_rate.column()
                col_left.prop(gs, "physics_tick_rate", text="Logic+Physics Steps Per Second")
                col_right = split_rate.column()
                col_right.prop(gs, "physics_step_max", text="Max Logic+Physics Steps")
                
                # Continue with FPS Limit and Render Cap Rate in a new split
                split = layout.split()
                col = split.column()
                col.prop(gs, "use_fixed_fps_cap", text="FPS Limit ( Fixed )")
                # Grey out Render Frames Per Second if FPS Limit is disabled
                row_render = col.row()
                row_render.enabled = gs.use_fixed_fps_cap
                row_render.prop(gs, "fixed_render_cap_rate", text="Render Frames Per Second")
                col.prop(gs, "use_fixed_physics_interpolation", text="Physics Interpolation")

                if hasattr(bpy.types, "LogicNodeTree") and hasattr(gs, "use_logic_nodes_parallel"):
                    layout.separator()
                    box = layout.box()
                    box.label(text="Logic Nodes", icon='NODETREE')
                    box.prop(
                        gs,
                        "use_logic_nodes_parallel",
                        text="Parallel Tree Execution",
                    )
                    if hasattr(gs, "show_logic_nodes_profile"):
                        box.prop(
                            gs,
                            "show_logic_nodes_profile",
                            text="Console Tick Profiling",
                        )
            else:
                # For Variable mode: keep original layout
                split = layout.split()
                col = split.column()
                col.label(text="Physics Steps:")
                sub = col.column(align=True)
                sub.prop(gs, "physics_step_max", text="Max")
                sub.prop(gs, "physics_step_sub", text="Substeps")
                row_ufr = col.row()
                row_ufr.prop(gs, "use_frame_rate", text="FPS Limit ( Variable )")

            # Show Logic Steps section only for variable physics mode
            if gs.physics_timestep_method != 'FIXED':
                col = split.column()
                col.label(text="Logic Steps:")
                col.prop(gs, "logic_step_max", text="Max")
            else:
                # Empty column for fixed mode to maintain layout
                col = split.column()

            row = layout.row()
            # Show different FPS control based on mode
            if gs.physics_timestep_method == 'FIXED':
                # No FPS label for fixed mode, just show Time Scale
                row.prop(gs, "time_scale")
            else:
                row.prop(gs, "fps", text="FPS")
                row.prop(gs, "time_scale")

            col = layout.column()
            col.label(text="Physics Deactivation:")
            if gs.physics_engine == 'JOLT':
                sub = col.row()
                sub.prop(gs, "deactivation_linear_threshold", text="Sleep Velocity Threshold")
                sub = col.row()
                sub.prop(gs, "deactivation_time", text="Time")
                col.label(text="Jolt Position Correction:")
                sub = col.column(align=True)
                sub.prop(gs, "jolt_correction_strength", text="Correction Strength")
            else:
                sub = col.row(align=True)
                sub.prop(gs, "deactivation_linear_threshold", text="Linear Threshold")
                sub.prop(gs, "deactivation_angular_threshold", text="Angular Threshold")
                sub = col.row()
                sub.prop(gs, "deactivation_time", text="Time")

            if gs.physics_engine != 'JOLT':
                col = layout.column()
                col.label(text="Physics Joint Error Reduction:")
                sub = col.column(align=True)
                sub.prop(gs, "erp_parameter", text="ERP for Non Contact Constraints")
                sub.prop(gs, "erp2_parameter", text="ERP for Contact Constraints")
                sub.prop(gs, "cfm_parameter", text="CFM for Soft Constraints")

            row = layout.row()
            row.label(text="Object Activity:")
            row.prop(gs, "use_activity_culling")

            # Jolt Physics engine settings
            if gs.physics_engine == 'JOLT':
                layout.separator()
                col = layout.column()
                col.label(text="Jolt Physics Settings:")
                col.prop(gs, "jolt_velocity_solver_iterations", text="Velocity Solver Iterations")
                col.prop(gs, "jolt_position_solver_iterations", text="Position Solver Iterations")
                col.prop(gs, "jolt_physics_threads", text="Physics Threads (-1 = Auto)")
                col.prop(gs, "jolt_max_bodies", text="Max Bodies")
                col.prop(gs, "jolt_debug_errors", text="Debug Errors")
                # Max Body Pairs, Contact Constraints, Temp Allocator are auto-calculated from Max Bodies

        else:
            split = layout.split()

            col = split.column()
            col.label(text="Physics Steps:")
            col.prop(gs, "fps", text="FPS")

            col = split.column()
            col.label(text="Logic Steps:")
            col.prop(gs, "logic_step_max", text="Max")

class SCENE_PT_game_blender_physics(SceneButtonsPanel, Panel):
    bl_label = "Game Blender Physics"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene.render.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings

        layout.prop(gs, "use_interactive_dynapaint")
        row = layout.row()
        row.prop(gs, "use_interactive_rigidbody")

class SCENE_PT_game_physics_obstacles(SceneButtonsPanel, Panel):
    bl_label = "Obstacle Simulation"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene.render.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings

        layout.prop(gs, "obstacle_simulation", text="Type")
        if gs.obstacle_simulation != 'NONE':
            layout.prop(gs, "level_height")
            layout.prop(gs, "show_obstacle_simulation")


class SCENE_PT_game_navmesh(SceneButtonsPanel, Panel):
    bl_label = "Navigation Mesh"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene and scene.render.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout

        rd = context.scene.game_settings.recast_data

        layout.operator("mesh.navmesh_make", text="Build Navigation Mesh")

        col = layout.column()
        col.label(text="Rasterization:")
        row = col.row()
        row.prop(rd, "cell_size")
        row.prop(rd, "cell_height")

        col = layout.column()
        col.label(text="Agent:")
        split = col.split()

        col = split.column()
        col.prop(rd, "agent_height", text="Height")
        col.prop(rd, "agent_radius", text="Radius")

        col = split.column()
        col.prop(rd, "slope_max")
        col.prop(rd, "climb_max")

        col = layout.column()
        col.label(text="Region:")
        row = col.row()
        row.prop(rd, "region_min_size")
        if rd.partitioning != 'LAYERS':
            row.prop(rd, "region_merge_size")

        col = layout.column()
        col.prop(rd, "partitioning")

        col = layout.column()
        col.label(text="Polygonization:")
        split = col.split()

        col = split.column()
        col.prop(rd, "edge_max_len")
        col.prop(rd, "edge_max_error")

        split.prop(rd, "verts_per_poly")

        col = layout.column()
        col.label(text="Detail Mesh:")
        row = col.row()
        row.prop(rd, "sample_dist")
        row.prop(rd, "sample_max_error")


class SCENE_PT_game_hysteresis(SceneButtonsPanel, Panel):
    bl_label = "Level of Detail"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene and scene.render.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        gs = context.scene.game_settings

        row = layout.row()
        row.prop(gs, "use_scene_hysteresis", text="Hysteresis")
        row = layout.row()
        row.active = gs.use_scene_hysteresis
        row.prop(gs, "scene_hysteresis_percentage", text="")

class SCENE_PT_game_console(SceneButtonsPanel, Panel):
    bl_label = "Game Python Console"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene and scene.render.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        gs = context.scene.game_settings

        self.layout.prop(gs, "use_python_console", text="")

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings
        row = layout.row(align=True)
        row.active = gs.use_python_console
        row.label(text="Keys:")
        row.prop(gs, "python_console_key1", text="", event=True)
        row.prop(gs, "python_console_key2", text="", event=True)
        row.prop(gs, "python_console_key3", text="", event=True)
        row.prop(gs, "python_console_key4", text="", event=True)

class DataButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"


class ObjectButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "object"

class OBJECT_PT_activity_culling(ObjectButtonsPanel, Panel):
    bl_label = "Activity Culling"
    COMPAT_ENGINES = {
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return context.scene.render.engine in cls.COMPAT_ENGINES and ob.type not in {'CAMERA'}

    def draw(self, context):
        layout = self.layout
        activity = context.object.game.activity_culling

        split = layout.split()

        col = split.column()
        col.prop(activity, "use_physics", text="Physics")
        sub = col.column()
        sub.active = activity.use_physics
        sub.prop(activity, "physics_radius")

        col = split.column()
        col.prop(activity, "use_logic", text="Logic")
        sub = col.column()
        sub.active = activity.use_logic
        sub.prop(activity, "logic_radius")

class OBJECT_PT_upbge_dupli_base(ObjectButtonsPanel, Panel):
    bl_label = "UPBGE Dupli Base"
    COMPAT_ENGINES = {
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return ob and ob.game and context.scene.render.engine in cls.COMPAT_ENGINES

    def draw(self, context):
        layout = self.layout
        ob = context.object
        layout.prop(ob.game, "use_upbge_dupli_base")

class OBJECT_MT_lod_tools(Menu):
    bl_label = "Level Of Detail Tools"

    def draw(self, context):
        layout = self.layout

        layout.operator("object.lod_by_name", text="Set By Name")
        layout.operator("object.lod_generate", text="Generate")
        layout.operator("object.lod_clear_all", text="Clear All", icon='PANEL_CLOSE')


class OBJECT_PT_levels_of_detail(ObjectButtonsPanel, Panel):
    bl_label = "Levels of Detail"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return context.engine in cls.COMPAT_ENGINES and ob.type not in {'CAMERA', 'EMPTY', 'LIGHT'}

    def draw(self, context):
        layout = self.layout
        ob = context.object
        gs = context.scene.game_settings

        col = layout.column()
        col.prop(ob, "lod_factor", text="Distance Factor")

        col = layout.column()
        col.prop(ob, "use_lod_physics", text="Physics Update")

        for i, level in enumerate(ob.lod_levels):
            if i == 0:
                continue
            box = col.box()
            row = box.row()
            row.prop(level, "object", text="")
            row.operator("object.lod_remove", text="", icon='PANEL_CLOSE').index = i

            row = box.row()
            row.prop(level, "distance")
            #Disable distinction material/mesh for now
            #row = row.row(align=True)
            #row.prop(level, "use_mesh", text="")
            #row.prop(level, "use_material", text="")

            row = box.row()
            row.active = gs.use_scene_hysteresis
            row.prop(level, "use_object_hysteresis", text="Hysteresis Override")
            row = box.row()
            row.active = gs.use_scene_hysteresis and level.use_object_hysteresis
            row.prop(level, "object_hysteresis_percentage", text="")

        row = col.row(align=True)
        row.operator("object.lod_add", text="Add", icon='PLUS')
        row.menu("OBJECT_MT_lod_tools", text="", icon='TRIA_DOWN')


classes = (
    GAME_PT_game_object,
    GAME_PT_game_components,
    GAME_PT_logic_nodes,
    GAME_PT_game_properties,
    GAME_MT_component_context_menu,
    PHYSICS_PT_game_physics,
    PHYSICS_PT_game_force_field,
    PHYSICS_PT_game_buoyancy,
    PHYSICS_PT_game_collision_bounds,
    PHYSICS_PT_game_collision_filtering,
    PHYSICS_PT_game_obstacles,
    SCENE_PT_game_physics,
    SCENE_PT_game_blender_physics,
    SCENE_PT_game_physics_obstacles,
    SCENE_PT_game_navmesh,
    SCENE_PT_game_hysteresis,
    SCENE_PT_game_console,
    OBJECT_MT_lod_tools,
    OBJECT_PT_upbge_dupli_base,
    OBJECT_PT_activity_culling,
    OBJECT_PT_levels_of_detail,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
