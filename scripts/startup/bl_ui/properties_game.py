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
from bpy.types import Panel, Menu

import re


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


class GAME_MT_component_context_menu(Menu):
    bl_label = "Game Component"


    @classmethod
    def poll(cls, context):
        ob = context.active_object

        return ob and ob.game and ob.game.components

    def draw(self, context):
        layout = self.layout

        components = context.active_object.game.components
        index = components.find(context.component.name)  # FIXME: Should not use component.name as a key.

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
        'BLENDER_EEVEE_NEXT',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        rd = context.scene.render
        return ob and ob.game and (rd.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout

        ob = context.active_object
        game = ob.game
        soft = ob.game.soft_body

        layout.prop(game, "physics_type")
        layout.separator()

        physics_type = game.physics_type

        if physics_type == 'CHARACTER':
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
            col.prop(game, "use_actor")
            col.prop(game, "use_ghost")
            col.prop(ob, "hide_render", text="Invisible")  # out of place but useful

            col = split.column()
            col.prop(game, "use_physics_fh")
            col.prop(game, "use_rotate_from_normal")
            col.prop(game, "use_sleep")

            layout.separator()

            split = layout.split()

            col = split.column()
            col.label(text="Attributes:")
            col.prop(game, "mass")
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
            col.prop(game, "rolling_friction")
            col.separator()

            sub = col.column()
            sub.prop(game, "use_anisotropic_friction")
            subsub = sub.column()
            subsub.active = game.use_anisotropic_friction
            subsub.prop(game, "friction_coefficients", text="", slider=True)

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
            sub = col.column()
            sub.active = game.use_ccd_rigid_body
            sub.prop(game, "ccd_motion_threshold")
            sub.prop(game, "ccd_swept_sphere_radius")

            layout.separator()
            col = layout.column()

            col.label(text="Lock Translation:")
            row = col.row()
            row.prop(game, "lock_location_x", text="X")
            row.prop(game, "lock_location_y", text="Y")
            row.prop(game, "lock_location_z", text="Z")

        if physics_type == 'RIGID_BODY':
            col = layout.column()

            col.label(text="Lock Rotation:")
            row = col.row()
            row.prop(game, "lock_rotation_x", text="X")
            row.prop(game, "lock_rotation_y", text="Y")
            row.prop(game, "lock_rotation_z", text="Z")

        elif physics_type == 'SOFT_BODY':
            col = layout.column()
            col.prop(game, "use_actor")
            #col.prop(game, "use_ghost") Seems not supported in bullet for SoftBodies
            col.prop(ob, "hide_render", text="Invisible")

            layout.separator()

            split = layout.split()

            col = split.column()
            col.label(text="General Attributes:")
            col.prop(game, "mass")
            # disabled in the code
            # col.prop(soft, "weld_threshold")
            col.prop(soft, "linear_stiffness", slider=True)
            col.prop(soft, "dynamic_friction", slider=True)
            col.prop(soft, "kdp", text="Damping", slider=True)
            col.prop(soft, "collision_margin", slider=True)
            col.prop(soft, "kvcf", text="Velocity Correction", slider=True)
            col.prop(soft, "use_bending_constraints", text="Bending Constraints")

            sub = col.column()
            sub.active = soft.use_bending_constraints
            sub.prop(soft, "bending_distance")

            col.prop(soft, "use_shape_match")

            sub = col.column()
            sub.active = soft.use_shape_match
            sub.prop(soft, "shape_threshold", slider=True)

            col.label(text="Solver Iterations:")
            col.prop(soft, "position_solver_iterations", text="Position Solver")
            col.prop(soft, "velocity_solver_iterations", text="Velocity Solver")
            col.prop(soft, "cluster_solver_iterations", text="Cluster Solver")
            col.prop(soft, "drift_solver_iterations", text="Drift Solver")

            col = split.column()
            col.label(text="Hardness:")
            col.prop(soft, "kchr", text="Rigid Contacts", slider=True)
            col.prop(soft, "kkhr", text="Kinetic Contacts", slider=True)
            col.prop(soft, "kshr", text="Soft Contacts", slider=True)
            col.prop(soft, "kahr", text="Anchors", slider=True)

            col.label(text="Cluster Collision:")
            col.prop(soft, "use_cluster_rigid_to_softbody")
            col.prop(soft, "use_cluster_soft_to_softbody")
            sub = col.column()
            sub.active = (soft.use_cluster_rigid_to_softbody or soft.use_cluster_soft_to_softbody)
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
            col.prop(soft, "kvc", text="Volume Conservation")

            col = split.column()
            col.label(text="Aerodynamics:")
            col.prop(soft, "kdg", text="Drag Coefficient")
            col.prop(soft, "klf", text="Lift Coefficient")

        elif physics_type == 'STATIC':
            col = layout.column()
            col.prop(game, "use_actor")
            col.prop(game, "use_ghost")
            col.prop(ob, "hide_render", text="Invisible")

            layout.separator()

            split = layout.split()

            col = split.column()
            col.label(text="Attributes:")
            col.prop(game, "radius")
            col.prop(game, "elasticity", slider=True)
            col.label(text="Friction:")
            col.prop(game, "friction")
            col.prop(game, "rolling_friction")

            col = split.column()
            sub = col.column()
            sub.prop(game, "use_anisotropic_friction")
            subsub = sub.column()
            subsub.active = game.use_anisotropic_friction
            subsub.prop(game, "friction_coefficients", text="", slider=True)

        elif physics_type == 'SENSOR':
            col = layout.column()
            col.prop(game, "use_actor", text="Detect Actors")
            col.prop(ob, "hide_render", text="Invisible")

        elif physics_type in {'INVISIBLE', 'NO_COLLISION', 'OCCLUDER'}:
            layout.prop(ob, "hide_render", text="Invisible")

        elif physics_type == 'NAVMESH':
            layout.operator("mesh.navmesh_face_copy")
            layout.operator("mesh.navmesh_face_add")

            layout.separator()

            layout.operator("mesh.navmesh_reset")
            layout.operator("mesh.navmesh_clear")

        if physics_type in {"STATIC", "DYNAMIC", "RIGID_BODY"}:
            row = layout.row()
            row.label(text="Force Field:")

            row = layout.row()
            row.prop(game, "fh_force")
            row.prop(game, "fh_damping", slider=True)

            row = layout.row()
            row.prop(game, "fh_distance")
            row.prop(game, "use_fh_normal")


class PHYSICS_PT_game_collision_bounds(PhysicsButtonsPanel, Panel):
    bl_label = "Collision Bounds"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE_NEXT',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        game = context.object.game
        return (context.scene.render.engine in cls.COMPAT_ENGINES) \
                and (game.physics_type in {'SENSOR', 'STATIC', 'DYNAMIC', 'RIGID_BODY', 'CHARACTER', 'SOFT_BODY'})

    def draw_header(self, context):
        game = context.active_object.game

        self.layout.prop(game, "use_collision_bounds", text="")

    def draw(self, context):
        layout = self.layout

        game = context.active_object.game
        split = layout.split()
        split.active = game.use_collision_bounds

        col = split.column()
        col.prop(game, "collision_bounds_type", text="Bounds")

        row = col.row()
        row.prop(game, "collision_margin", text="Margin", slider=True)

        sub = row.row()
        sub.active = game.physics_type not in {'SOFT_BODY', 'CHARACTER'}
        sub.prop(game, "use_collision_compound", text="Compound")

        layout.separator()
        split = layout.split()
        col = split.column()
        col.prop(game, "collision_group")
        col = split.column()
        col.prop(game, "collision_mask")


class PHYSICS_PT_game_obstacles(PhysicsButtonsPanel, Panel):
    bl_label = "Create Obstacle"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE_NEXT',
        'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        game = context.object.game
        return (context.scene.render.engine in cls.COMPAT_ENGINES) \
                and (game.physics_type in {'SENSOR', 'STATIC', 'DYNAMIC', 'RIGID_BODY', 'SOFT_BODY', 'CHARACTER', 'NO_COLLISION'})

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
        'BLENDER_EEVEE_NEXT',
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
            layout.prop(gs, "physics_solver")
            layout.prop(gs, "physics_gravity", text="Gravity")

            split = layout.split()

            col = split.column()
            col.label(text="Physics Steps:")
            sub = col.column(align=True)
            sub.prop(gs, "physics_step_max", text="Max")
            sub.prop(gs, "physics_step_sub", text="Substeps")

            col = split.column()
            col.label(text="Logic Steps:")
            col.prop(gs, "logic_step_max", text="Max")

            row = layout.row()
            row.prop(gs, "fps", text="FPS")
            row.prop(gs, "time_scale")

            col = layout.column()
            col.label(text="Physics Deactivation:")
            sub = col.row(align=True)
            sub.prop(gs, "deactivation_linear_threshold", text="Linear Threshold")
            sub.prop(gs, "deactivation_angular_threshold", text="Angular Threshold")
            sub = col.row()
            sub.prop(gs, "deactivation_time", text="Time")

            col = layout.column()
            col.label(text="Physics Joint Error Reduction:")
            sub = col.column(align=True)
            sub.prop(gs, "erp_parameter", text="ERP for Non Contact Constraints")
            sub.prop(gs, "erp2_parameter", text="ERP for Contact Constraints")
            sub.prop(gs, "cfm_parameter", text="CFM for Soft Constraints")

            row = layout.row()
            row.label(text="Object Activity:")
            row.prop(gs, "use_activity_culling")

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
        'BLENDER_EEVEE_NEXT',
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
        'BLENDER_EEVEE_NEXT',
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
        'BLENDER_EEVEE_NEXT',
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
        'BLENDER_EEVEE_NEXT',
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
        'BLENDER_EEVEE_NEXT',
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
        'BLENDER_EEVEE_NEXT',
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
        'BLENDER_EEVEE_NEXT',
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
    GAME_PT_game_properties,
    GAME_MT_component_context_menu,
    PHYSICS_PT_game_physics,
    PHYSICS_PT_game_collision_bounds,
    PHYSICS_PT_game_obstacles,
    SCENE_PT_game_physics,
    SCENE_PT_game_blender_physics,
    SCENE_PT_game_physics_obstacles,
    SCENE_PT_game_navmesh,
    SCENE_PT_game_hysteresis,
    SCENE_PT_game_console,
    OBJECT_MT_lod_tools,
    OBJECT_PT_activity_culling,
    OBJECT_PT_levels_of_detail,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
