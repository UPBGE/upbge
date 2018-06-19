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
from bpy.types import Panel, Menu, UIList


class PhysicsButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "physics"

class PHYSICS_PT_game_physics(PhysicsButtonsPanel, Panel):
    bl_label = "Physics"
    COMPAT_ENGINES = {'BLENDER_GAME'}

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
            col.prop(game, "form_factor")
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
            col.prop(game, "use_ghost")
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
    COMPAT_ENGINES = {'BLENDER_GAME'}

    @classmethod
    def poll(cls, context):
        game = context.object.game
        rd = context.scene.render
        return (rd.engine in cls.COMPAT_ENGINES) \
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
    COMPAT_ENGINES = {'BLENDER_GAME'}

    @classmethod
    def poll(cls, context):
        game = context.object.game
        rd = context.scene.render
        return (rd.engine in cls.COMPAT_ENGINES) \
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
        rd = context.scene.render
        return (rd.engine in cls.COMPAT_ENGINES)


class RENDER_PT_embedded(RenderButtonsPanel, Panel):
    bl_label = "Embedded Player"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    def draw(self, context):
        layout = self.layout

        rd = context.scene.render

        row = layout.row()
        row.operator("view3d.game_start", text="Start")
        row = layout.row()
        row.label(text="Resolution:")
        row = layout.row(align=True)
        row.prop(rd, "resolution_x", slider=False, text="X")
        row.prop(rd, "resolution_y", slider=False, text="Y")


class RENDER_PT_game_player(RenderButtonsPanel, Panel):
    bl_label = "Standalone Player"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    def draw(self, context):
        import sys
        layout = self.layout
        not_osx = sys.platform != "darwin"

        gs = context.scene.game_settings

        row = layout.row()
        row.operator("wm.blenderplayer_start", text="Start")
        row = layout.row()
        row.label(text="Resolution:")
        row = layout.row(align=True)
        row.active = not_osx or not gs.show_fullscreen
        row.prop(gs, "resolution_x", slider=False, text="X")
        row.prop(gs, "resolution_y", slider=False, text="Y")
        row = layout.row()
        col = row.column()
        col.prop(gs, "show_fullscreen")

        if not_osx:
            col = row.column()
            col.prop(gs, "use_desktop")
            col.active = gs.show_fullscreen

        col = layout.column()
        col.label(text="Quality:")
        col = layout.column(align=True)
        col.prop(gs, "depth", text="Bit Depth", slider=False)
        col.prop(gs, "frequency", text="Refresh Rate", slider=False)


class RENDER_PT_game_stereo(RenderButtonsPanel, Panel):
    bl_label = "Stereo"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings
        stereo_mode = gs.stereo

        # stereo options:
        layout.row().prop(gs, "stereo", expand=True)

        # stereo:
        if stereo_mode == 'STEREO':
            layout.prop(gs, "stereo_mode")
            layout.prop(gs, "stereo_eye_separation")


class RENDER_PT_game_shading(RenderButtonsPanel, Panel):
    bl_label = "Shading"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings
        rd = context.scene.render

        split = layout.split()

        col = split.column()
        col.prop(gs, "use_glsl_lights", text="Lights")
        col.prop(gs, "use_glsl_shaders", text="Shaders")
        col.prop(gs, "use_glsl_shadows", text="Shadows")
        col.prop(gs, "use_glsl_environment_lighting", text="Environment Lighting")
        col = split.column()
        col.prop(gs, "use_glsl_ramps", text="Ramps")
        col.prop(gs, "use_glsl_nodes", text="Nodes")
        col.prop(gs, "use_glsl_extra_textures", text="Extra Textures")
        col.prop(rd, "use_world_space_shading", text="World Space Shading")


class RENDER_PT_game_system(RenderButtonsPanel, Panel):
    bl_label = "System"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings
        split = layout.split(percentage=0.4)

        col = split.column()
        col.prop(gs, "use_frame_rate")
        col.prop(gs, "use_deprecation_warnings")

        col = split.column()
        col.prop(gs, "vsync")
        col.prop(gs, "samples")
        col.prop(gs, "hdr")

        row = layout.row()
        col = row.column()
        col.label("Exit Key:")
        col.prop(gs, "exit_key", text="", event=True)

class RENDER_UL_attachments(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        if item is not None:
            layout.prop(item, "name", text="", emboss=False, icon="TEXTURE")
            layout.label(text=str(index))
        else:
            layout.label(text="", icon="TEXTURE")

class RENDER_PT_game_attachments(RenderButtonsPanel, Panel):
    bl_label = "Attachments"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings

        row = layout.row()

        row.template_list("RENDER_UL_attachments", "", gs, "attachment_slots", gs, "active_attachment_index", rows=2)

        col = row.column(align=True)
        col.operator("scene.render_attachment_new", icon='ZOOMIN', text="")
        col.operator("scene.render_attachment_remove", icon='ZOOMOUT', text="")

        attachment = gs.active_attachment

        if attachment is not None:
            row = layout.row()
            row.prop(attachment, "type")
            row.prop(attachment, "hdr")

            if attachment.type == "CUSTOM":
                row = layout.row()
                row.prop(attachment, "size")


class RENDER_PT_game_animations(RenderButtonsPanel, Panel):
    bl_label = "Animations"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings

        layout.prop(context.scene.render, "fps", text="Animation Frame Rate", slider=False)
        layout.prop(gs, "use_restrict_animation_updates")


class RENDER_PT_game_display(RenderButtonsPanel, Panel):
    bl_label = "Display"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings

        col = layout.column()
        col.prop(gs, "show_mouse", text="Mouse Cursor")

        col = layout.column()
        col.label(text="Framing:")
        col.row().prop(gs, "frame_type", expand=True)
        col.prop(gs, "frame_color", text="")

class RENDER_PT_game_color_management(RenderButtonsPanel, Panel):
    bl_label = "Color Management"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    def draw(self, context):
        layout = self.layout
        gs = context.scene.game_settings

        layout.prop(gs, "color_management")

class RENDER_PT_game_debug(RenderButtonsPanel, Panel):
    bl_label = "Debug"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings

        split = layout.split(percentage=0.4)

        col = split.column()
        col.prop(gs, "show_framerate_profile", text="Framerate and Profile")
        col.prop(gs, "show_render_queries", text="Render Queries")
        col.prop(gs, "show_debug_properties", text="Properties")
        col.prop(gs, "show_physics_visualization", text="Physics Visualization")

        col = split.column()
        col.prop(gs, "show_bounding_box", text="Bounding Box")
        col.prop(gs, "show_armatures", text="Armatures")
        col.prop(gs, "show_camera_frustum", text="Camera Frustum")
        col.prop(gs, "show_shadow_frustum", text="Shadow Frustum")


class SceneButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "scene"


class SCENE_PT_game_physics(SceneButtonsPanel, Panel):
    bl_label = "Physics"
    COMPAT_ENGINES = {'BLENDER_GAME'}

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

            split = layout.split()

            col = split.column()
            col.label(text="Culling:")
            col.prop(gs, "use_occlusion_culling", text="Occlusion Culling")
            sub = col.column()
            sub.active = gs.use_occlusion_culling
            sub.prop(gs, "occlusion_culling_resolution", text="Resolution")

            col = split.column()
            col.label(text="Object Activity:")
            col.prop(gs, "use_activity_culling")

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
    COMPAT_ENGINES = {'BLENDER_GAME'}

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
    COMPAT_ENGINES = {'BLENDER_GAME'}

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
    COMPAT_ENGINES = {'BLENDER_GAME'}

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
    bl_label = "Python Console"
    COMPAT_ENGINES = {'BLENDER_GAME'}

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
        row.label("Keys:")
        row.prop(gs, "python_console_key1", text="", event=True)
        row.prop(gs, "python_console_key2", text="", event=True)
        row.prop(gs, "python_console_key3", text="", event=True)
        row.prop(gs, "python_console_key4", text="", event=True)


class SCENE_PT_game_audio(SceneButtonsPanel, Panel):
    bl_label = "Audio"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_GAME'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene and scene.render.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout

        scene = context.scene

        split = layout.split()

        col = layout.column()
        col.prop(scene, "audio_distance_model", text="Distance Model")
        col = layout.column(align=True)
        col.prop(scene, "audio_doppler_speed", text="Speed")
        col.prop(scene, "audio_doppler_factor", text="Doppler")


class WorldButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "world"


class WORLD_PT_game_context_world(WorldButtonsPanel, Panel):
    bl_label = ""
    bl_options = {'HIDE_HEADER'}
    COMPAT_ENGINES = {'BLENDER_GAME'}

    @classmethod
    def poll(cls, context):
        rd = context.scene.render
        return (context.scene) and (rd.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout

        scene = context.scene
        world = context.world
        space = context.space_data

        split = layout.split(percentage=0.65)
        if scene:
            split.template_ID(scene, "world", new="world.new")
        elif world:
            split.template_ID(space, "pin_id")


class WORLD_PT_game_world(WorldButtonsPanel, Panel):
    bl_label = "World"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene.world and scene.render.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout

        self.layout.template_preview(context.world)

        world = context.world

        row = layout.row()
        row.prop(world, "use_sky_paper")
        row.prop(world, "use_sky_blend")
        row.prop(world, "use_sky_real")

        row = layout.row()
        row.column().prop(world, "horizon_color")
        col = row.column()
        col.prop(world, "zenith_color")
        col.active = world.use_sky_blend
        row.column().prop(world, "ambient_color")

        row = layout.row()
        row.prop(world, "exposure")
        row.prop(world, "color_range")


class WORLD_PT_game_environment_lighting(WorldButtonsPanel, Panel):
    bl_label = "Environment Lighting"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene.world and scene.render.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        light = context.world.light_settings
        self.layout.prop(light, "use_environment_light", text="")

    def draw(self, context):
        layout = self.layout

        light = context.world.light_settings

        layout.active = light.use_environment_light

        split = layout.split()
        split.prop(light, "environment_energy", text="Energy")
        split.prop(light, "environment_color", text="")


class WORLD_PT_game_mist(WorldButtonsPanel, Panel):
    bl_label = "Mist"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene.world and scene.render.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        world = context.world

        self.layout.prop(world.mist_settings, "use_mist", text="")

    def draw(self, context):
        layout = self.layout

        world = context.world

        layout.active = world.mist_settings.use_mist

        layout.prop(world.mist_settings, "falloff")

        row = layout.row(align=True)
        row.prop(world.mist_settings, "start")
        row.prop(world.mist_settings, "depth")

        layout.prop(world.mist_settings, "intensity", text="Minimum Intensity")


class DataButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"


class DATA_PT_shadow_game(DataButtonsPanel, Panel):
    bl_label = "Shadow"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    @classmethod
    def poll(cls, context):
        COMPAT_LIGHTS = {'SPOT', 'SUN'}
        lamp = context.lamp
        engine = context.scene.render.engine
        return (lamp and lamp.type in COMPAT_LIGHTS) and (engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        lamp = context.lamp

        self.layout.prop(lamp, "use_shadow", text="")

    def draw(self, context):
        layout = self.layout

        lamp = context.lamp

        layout.active = lamp.use_shadow

        split = layout.split()

        col = split.column()
        col.prop(lamp, "shadow_color", text="")
        if lamp.type in ('SUN', 'SPOT'):
            col.prop(lamp, "show_shadow_box")
        col.prop(lamp, "static_shadow")

        col = split.column()
        col.prop(lamp, "use_shadow_layer", text="This Layer Only")
        col.prop(lamp, "use_only_shadow")

        col = layout.column()
        col.label("Buffer Type:")
        col.prop(lamp, "ge_shadow_buffer_type", text="", toggle=True)
        if lamp.ge_shadow_buffer_type == "SIMPLE":
            col.label("Filter Type:")
            col.prop(lamp, "shadow_filter", text="", toggle=True)

        col.label("Quality:")
        col = layout.column(align=True)
        col.prop(lamp, "shadow_buffer_size", text="Size")
        if lamp.ge_shadow_buffer_type == "VARIANCE":
            col.prop(lamp, "shadow_buffer_sharp", text="Sharpness")
        elif lamp.shadow_filter in ("PCF", "PCF_BAIL", "PCF_JITTER"):
            col.prop(lamp, "shadow_buffer_samples", text="Samples")
            col.prop(lamp, "shadow_buffer_soft", text="Soft")

        row = layout.row()
        row.label("Bias:")
        row = layout.row(align=True)
        row.prop(lamp, "shadow_buffer_bias", text="Bias")
        if lamp.ge_shadow_buffer_type == "VARIANCE":
            row.prop(lamp, "shadow_buffer_bleed_bias", text="Bleed Bias")
        else:
            row.prop(lamp, "shadow_buffer_slope_bias", text="Slope Bias")

        row = layout.row()
        row.label("Clipping:")
        row = layout.row(align=True)
        row.prop(lamp, "shadow_buffer_clip_start", text="Clip Start")
        row.prop(lamp, "shadow_buffer_clip_end", text="Clip End")

        if lamp.type == 'SUN':
            row = layout.row()
            row.prop(lamp, "shadow_frustum_size", text="Frustum Size")


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

class OBJECT_MT_culling(ObjectButtonsPanel, Panel):
    bl_label = "Culling Bounding Volume"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return context.scene.render.engine in cls.COMPAT_ENGINES and ob.type not in {'CAMERA', 'EMPTY', 'LAMP'}

    def draw(self, context):
        layout = self.layout
        game = context.active_object.game

        layout.label(text="Predefined Bound:")
        layout.prop(game, "predefined_bound", "")

class OBJECT_PT_activity_culling(ObjectButtonsPanel, Panel):
    bl_label = "Activity Culling"
    COMPAT_ENGINES = {'BLENDER_GAME'}

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

class OBJECT_PT_levels_of_detail(ObjectButtonsPanel, Panel):
    bl_label = "Levels of Detail"
    COMPAT_ENGINES = {'BLENDER_GAME'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return context.scene.render.engine in cls.COMPAT_ENGINES and ob.type not in {'CAMERA', 'EMPTY', 'LAMP'}

    def draw(self, context):
        layout = self.layout
        ob = context.object
        gs = context.scene.game_settings

        col = layout.column()
        col.prop(ob, "lod_factor", text="Distance Factor")

        for i, level in enumerate(ob.lod_levels):
            if i == 0:
                continue
            box = col.box()
            row = box.row()
            row.prop(level, "object", text="")
            row.operator("object.lod_remove", text="", icon='PANEL_CLOSE').index = i

            row = box.row()
            row.prop(level, "distance")
            row = row.row(align=True)
            row.prop(level, "use_mesh", text="")
            row.prop(level, "use_material", text="")

            row = box.row()
            row.active = gs.use_scene_hysteresis
            row.prop(level, "use_object_hysteresis", text="Hysteresis Override")
            row = box.row()
            row.active = gs.use_scene_hysteresis and level.use_object_hysteresis
            row.prop(level, "object_hysteresis_percentage", text="")

        row = col.row(align=True)
        row.operator("object.lod_add", text="Add", icon='ZOOMIN')
        row.menu("OBJECT_MT_lod_tools", text="", icon='TRIA_DOWN')


classes = (
    PHYSICS_PT_game_physics,
    PHYSICS_PT_game_collision_bounds,
    PHYSICS_PT_game_obstacles,
    RENDER_PT_embedded,
    RENDER_PT_game_player,
    RENDER_PT_game_stereo,
    RENDER_PT_game_shading,
    RENDER_PT_game_system,
    RENDER_PT_game_attachments,
    RENDER_PT_game_animations,
    RENDER_PT_game_display,
    RENDER_PT_game_color_management,
    RENDER_PT_game_debug,
	RENDER_UL_attachments,
    SCENE_PT_game_physics,
    SCENE_PT_game_physics_obstacles,
    SCENE_PT_game_navmesh,
    SCENE_PT_game_hysteresis,
    SCENE_PT_game_console,
    SCENE_PT_game_audio,
    WORLD_PT_game_context_world,
    WORLD_PT_game_world,
    WORLD_PT_game_environment_lighting,
    WORLD_PT_game_mist,
    DATA_PT_shadow_game,
    OBJECT_MT_lod_tools,
    OBJECT_MT_culling,
    OBJECT_PT_activity_culling,
    OBJECT_PT_levels_of_detail,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
