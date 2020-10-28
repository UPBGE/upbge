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


class PhysicsButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "physics"


class PHYSICS_PT_game_physics(PhysicsButtonsPanel, Panel):
    bl_label = "Physics"
    COMPAT_ENGINES = {'BLENDER_GAME', 'BLENDER_EEVEE'}

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
            col.active = game.use_ccd_rigid_body
            col.prop(game, "ccd_motion_threshold")
            col.prop(game, "ccd_swept_sphere_radius")

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
    COMPAT_ENGINES = {'BLENDER_GAME', 'BLENDER_EEVEE'}

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
    COMPAT_ENGINES = {'BLENDER_GAME', 'BLENDER_EEVEE'}

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
    bl_label = "Physics"
    COMPAT_ENGINES = {'BLENDER_GAME', 'BLENDER_EEVEE'}

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

        else:
            split = layout.split()

            col = split.column()
            col.label(text="Physics Steps:")
            col.prop(gs, "fps", text="FPS")

            col = split.column()
            col.label(text="Logic Steps:")
            col.prop(gs, "logic_step_max", text="Max")


class SCENE_PT_game_physics_obstacles(SceneButtonsPanel, Panel):
    bl_label = "Obstacle Simulation"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_GAME', 'BLENDER_EEVEE'}

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
    COMPAT_ENGINES = {'BLENDER_GAME', 'BLENDER_EEVEE'}

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
    COMPAT_ENGINES = {'BLENDER_GAME', 'BLENDER_EEVEE'}

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
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

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


class OBJECT_MT_lod_tools(Menu):
    bl_label = "Level Of Detail Tools"

    def draw(self, context):
        layout = self.layout

        layout.operator("object.lod_by_name", text="Set By Name")
        layout.operator("object.lod_generate", text="Generate")
        layout.operator("object.lod_clear_all", text="Clear All", icon='PANEL_CLOSE')


class OBJECT_PT_levels_of_detail(ObjectButtonsPanel, Panel):
    bl_label = "Levels of Detail"
    COMPAT_ENGINES = {'BLENDER_GAME', 'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return context.engine in cls.COMPAT_ENGINES

    def draw(self, context):
        layout = self.layout
        ob = context.object
        gs = context.scene.game_settings

        col = layout.column()

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
    PHYSICS_PT_game_physics,
    PHYSICS_PT_game_collision_bounds,
    PHYSICS_PT_game_obstacles,
    SCENE_PT_game_physics,
    SCENE_PT_game_physics_obstacles,
    SCENE_PT_game_navmesh,
    SCENE_PT_game_hysteresis,
    SCENE_PT_game_console,
    OBJECT_MT_lod_tools,
    OBJECT_PT_levels_of_detail,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
