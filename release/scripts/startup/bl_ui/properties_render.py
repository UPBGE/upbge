# SPDX-License-Identifier: GPL-2.0-or-later
from bpy.types import Panel
from bl_ui.space_view3d import (
    VIEW3D_PT_shading_lighting,
    VIEW3D_PT_shading_color,
    VIEW3D_PT_shading_options,
)

from bl_ui.properties_grease_pencil_common import GreasePencilSimplifyPanel


class RenderButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "render"
    # COMPAT_ENGINES must be defined in each subclass, external engines can add themselves here

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)


class RENDER_PT_context(Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "render"
    bl_options = {'HIDE_HEADER'}
    bl_label = ""

    @classmethod
    def poll(cls, context):
        return context.scene

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        scene = context.scene
        rd = scene.render

        if rd.has_multiple_engines:
            layout.prop(rd, "engine", text="Render Engine")


class RENDER_PT_game_resolution(RenderButtonsPanel, Panel):
    bl_label = "Game Resolution"
    COMPAT_ENGINES = {'BLENDER_EEVEE', 'BLENDER_WORKBENCH', 'BLENDER_EEVEE_NEXT'}

    def draw(self, context):
        import sys
        layout = self.layout
        not_osx = sys.platform != "darwin"

        view_render = context.scene.render

        row = layout.row()
        row.operator("view3d.game_start", text="Embedded Start")
        row = layout.row(align=True)
        row.prop(view_render, "resolution_x", slider=False, text="X:")
        row.prop(view_render, "resolution_y", slider=False, text="Y:")

        gs = context.scene.game_settings

        row = layout.row()
        row.operator("wm.blenderplayer_start", text="Standalone Start")
        row = layout.row(align=True)
        row.prop(gs, "resolution_x", slider=False, text="X:")
        row.prop(gs, "resolution_y", slider=False, text="Y:")
        row = layout.row()
        col = row.column()
        col.prop(gs, "show_fullscreen")

        if not_osx:
            col = row.column()
            col.prop(gs, "use_desktop")
            col.active = gs.show_fullscreen

        row = layout.row()
        col = row.column()
        col.prop(gs, "use_viewport_render")

        row = layout.row()
        row.active = not gs.use_viewport_render
        row.prop(gs, "samp_per_frame", text="Samples Per Frame")

class RENDER_PT_game_debug(RenderButtonsPanel, Panel):
    bl_label = "Game Debug"
    COMPAT_ENGINES = {'BLENDER_EEVEE', 'BLENDER_WORKBENCH', 'BLENDER_EEVEE_NEXT'}

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings

        col = layout.column()
        row = col.row()
        col = row.column()
        col.prop(gs, "use_frame_rate")

        row = layout.row()
        row.prop(gs, "vsync")

        row = layout.row()
        row.label(text="Exit Key")
        row.prop(gs, "exit_key", text="", event=True)

        flow = layout.column_flow()
        flow.prop(gs, "show_debug_properties", text="Debug Properties")
        flow.prop(gs, "show_framerate_profile", text="Framerate and Profile")
        flow.prop(gs, "show_physics_visualization", text="Physics Visualization")
        flow.prop(gs, "use_deprecation_warnings")
        flow.prop(gs, "show_mouse", text="Mouse Cursor")
        flow.prop(gs, "use_undo", text="Undo at exit")

        row = layout.row()
        row.prop(gs, "log_level", text="Log Level")

class RENDER_PT_color_management(RenderButtonsPanel, Panel):
    bl_label = "Color Management"
    bl_options = {'DEFAULT_CLOSED'}
    bl_order = 100
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        scene = context.scene
        view = scene.view_settings

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=False, even_rows=False, align=True)

        col = flow.column()
        col.prop(scene.display_settings, "display_device")

        col.separator()

        col.prop(view, "view_transform")
        col.prop(view, "look")

        col = flow.column()
        col.prop(view, "exposure")
        col.prop(view, "gamma")

        col.separator()

        col.prop(scene.sequencer_colorspace_settings, "name", text="Sequencer")


class RENDER_PT_color_management_curves(RenderButtonsPanel, Panel):
    bl_label = "Use Curves"
    bl_parent_id = "RENDER_PT_color_management"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw_header(self, context):

        scene = context.scene
        view = scene.view_settings

        self.layout.prop(view, "use_curve_mapping", text="")

    def draw(self, context):
        layout = self.layout

        scene = context.scene
        view = scene.view_settings

        layout.use_property_split = False
        layout.use_property_decorate = False  # No animation.

        layout.enabled = view.use_curve_mapping

        layout.template_curve_mapping(view, "curve_mapping", type='COLOR', levels=True)


class RENDER_PT_eevee_ambient_occlusion(RenderButtonsPanel, Panel):
    bl_label = "Ambient Occlusion"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        scene = context.scene
        props = scene.eevee
        self.layout.prop(props, "use_gtao", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        scene = context.scene
        props = scene.eevee

        layout.active = props.use_gtao
        col = layout.column()
        col.prop(props, "gtao_distance")
        col.prop(props, "gtao_factor")
        col.prop(props, "gtao_quality")
        col.prop(props, "use_gtao_bent_normals")
        col.prop(props, "use_gtao_bounce")


class RENDER_PT_eevee_motion_blur(RenderButtonsPanel, Panel):
    bl_label = "Motion Blur"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        scene = context.scene
        props = scene.eevee
        self.layout.prop(props, "use_motion_blur", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        scene = context.scene
        props = scene.eevee

        layout.active = props.use_motion_blur
        col = layout.column()
        col.prop(props, "motion_blur_position", text="Position")
        col.prop(props, "motion_blur_shutter")
        col.separator()
        col.prop(props, "motion_blur_depth_scale")
        col.prop(props, "motion_blur_max")
        col.prop(props, "motion_blur_steps", text="Steps")


class RENDER_PT_eevee_next_motion_blur(RenderButtonsPanel, Panel):
    bl_label = "Motion Blur"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE_NEXT'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        scene = context.scene
        props = scene.eevee
        self.layout.prop(props, "use_motion_blur", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        scene = context.scene
        props = scene.eevee

        layout.active = props.use_motion_blur
        col = layout.column()
        col.prop(props, "motion_blur_position", text="Position")
        col.prop(props, "motion_blur_shutter")
        col.separator()
        col.prop(props, "motion_blur_depth_scale")
        col.prop(props, "motion_blur_steps", text="Steps")


class RENDER_PT_motion_blur_curve(RenderButtonsPanel, Panel):
    bl_label = "Shutter Curve"
    bl_parent_id = "RENDER_PT_eevee_next_motion_blur"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE_NEXT'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        scene = context.scene
        rd = scene.render
        layout.active = rd.use_motion_blur

        col = layout.column()

        col.template_curve_mapping(rd, "motion_blur_shutter_curve")

        col = layout.column(align=True)
        row = col.row(align=True)
        row.operator("render.shutter_curve_preset", icon='SMOOTHCURVE', text="").shape = 'SMOOTH'
        row.operator("render.shutter_curve_preset", icon='SPHERECURVE', text="").shape = 'ROUND'
        row.operator("render.shutter_curve_preset", icon='ROOTCURVE', text="").shape = 'ROOT'
        row.operator("render.shutter_curve_preset", icon='SHARPCURVE', text="").shape = 'SHARP'
        row.operator("render.shutter_curve_preset", icon='LINCURVE', text="").shape = 'LINE'
        row.operator("render.shutter_curve_preset", icon='NOCURVE', text="").shape = 'MAX'


class RENDER_PT_eevee_depth_of_field(RenderButtonsPanel, Panel):
    bl_label = "Depth of Field"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        scene = context.scene
        props = scene.eevee

        col = layout.column()
        col.prop(props, "bokeh_max_size")
        col.prop(props, "bokeh_threshold")
        col.prop(props, "bokeh_neighbor_max")
        col.prop(props, "bokeh_denoise_fac")
        col.prop(props, "use_bokeh_high_quality_slight_defocus")
        col.prop(props, "use_bokeh_jittered")

        col = layout.column()
        col.active = props.use_bokeh_jittered
        col.prop(props, "bokeh_overblur")


class RENDER_PT_eevee_next_depth_of_field(RenderButtonsPanel, Panel):
    bl_label = "Depth of Field"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE_NEXT'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        scene = context.scene
        props = scene.eevee

        col = layout.column()
        col.prop(props, "bokeh_max_size")
        col.prop(props, "bokeh_threshold")
        col.prop(props, "bokeh_neighbor_max")
        col.prop(props, "use_bokeh_jittered")

        col = layout.column()
        col.active = props.use_bokeh_jittered
        col.prop(props, "bokeh_overblur")


class RENDER_PT_eevee_bloom(RenderButtonsPanel, Panel):
    bl_label = "Bloom"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        scene = context.scene
        props = scene.eevee
        self.layout.prop(props, "use_bloom", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        scene = context.scene
        props = scene.eevee

        layout.active = props.use_bloom
        col = layout.column()
        col.prop(props, "bloom_threshold")
        col.prop(props, "bloom_knee")
        col.prop(props, "bloom_radius")
        col.prop(props, "bloom_color")
        col.prop(props, "bloom_intensity")
        col.prop(props, "bloom_clamp")


class RENDER_PT_eevee_volumetric(RenderButtonsPanel, Panel):
    bl_label = "Volumetrics"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        scene = context.scene
        props = scene.eevee

        col = layout.column(align=True)
        col.prop(props, "volumetric_start")
        col.prop(props, "volumetric_end")

        col = layout.column()
        col.prop(props, "volumetric_tile_size")
        col.prop(props, "volumetric_samples")
        col.prop(props, "volumetric_sample_distribution", text="Distribution")


class RENDER_PT_eevee_volumetric_lighting(RenderButtonsPanel, Panel):
    bl_label = "Volumetric Lighting"
    bl_parent_id = "RENDER_PT_eevee_volumetric"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw_header(self, context):
        scene = context.scene
        props = scene.eevee
        self.layout.prop(props, "use_volumetric_lights", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        scene = context.scene
        props = scene.eevee

        layout.active = props.use_volumetric_lights
        layout.prop(props, "volumetric_light_clamp", text="Light Clamping")


class RENDER_PT_eevee_volumetric_shadows(RenderButtonsPanel, Panel):
    bl_label = "Volumetric Shadows"
    bl_parent_id = "RENDER_PT_eevee_volumetric"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw_header(self, context):
        scene = context.scene
        props = scene.eevee
        self.layout.prop(props, "use_volumetric_shadows", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        scene = context.scene
        props = scene.eevee

        layout.active = props.use_volumetric_shadows
        layout.prop(props, "volumetric_shadow_samples", text="Samples")

# UPBGE
class RENDER_PT_eevee_volumetric_blending(RenderButtonsPanel, Panel):
    bl_label = "Volumetric Blending"
    bl_parent_id = "RENDER_PT_eevee_volumetric"
    COMPAT_ENGINES = {'BLENDER_EEVEE', 'BLENDER_WORKBENCH', 'BLENDER_EEVEE_NEXT'}

    def draw_header(self, context):
        scene = context.scene
        props = scene.eevee
        self.layout.prop(props, "use_volumetric_blending", text="")
    def draw(self, context):
        pass
######


class RENDER_PT_eevee_subsurface_scattering(RenderButtonsPanel, Panel):
    bl_label = "Subsurface Scattering"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        scene = context.scene
        props = scene.eevee

        col = layout.column()
        col.prop(props, "sss_samples")
        col.prop(props, "sss_jitter_threshold")


class RENDER_PT_eevee_screen_space_reflections(RenderButtonsPanel, Panel):
    bl_label = "Screen Space Reflections"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        scene = context.scene
        props = scene.eevee
        self.layout.prop(props, "use_ssr", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        scene = context.scene
        props = scene.eevee

        col = layout.column()
        col.active = props.use_ssr
        col.prop(props, "use_ssr_refraction", text="Refraction")
        col.prop(props, "use_ssr_halfres")
        col.prop(props, "ssr_quality")
        col.prop(props, "ssr_max_roughness")
        col.prop(props, "ssr_thickness")
        col.prop(props, "ssr_border_fade")
        col.prop(props, "ssr_firefly_fac")


class RENDER_PT_eevee_shadows(RenderButtonsPanel, Panel):
    bl_label = "Shadows"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        scene = context.scene
        props = scene.eevee

        col = layout.column()
        col.prop(props, "shadow_cube_size", text="Cube Size")
        col.prop(props, "shadow_cascade_size", text="Cascade Size")
        col.prop(props, "use_shadow_high_bitdepth")
        col.prop(props, "use_soft_shadows")
        col.prop(props, "light_threshold")


class RENDER_PT_eevee_sampling(RenderButtonsPanel, Panel):
    bl_label = "Sampling"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        scene = context.scene
        props = scene.eevee

        col = layout.column(align=True)
        col.prop(props, "taa_render_samples", text="Render")
        col.prop(props, "taa_samples", text="Viewport")

        col = layout.column()
        col.prop(props, "use_taa_reprojection")

class RENDER_PT_eevee_sampling_smaa(RenderButtonsPanel, Panel):
    bl_label = "SMAA"
    bl_parent_id = "RENDER_PT_eevee_sampling"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw_header(self, context):
        scene = context.scene
        props = scene.eevee
        self.layout.prop(props, "use_eevee_smaa", text="")

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        props = scene.eevee

        layout.active = props.use_eevee_smaa
        col = layout.column(align=True)
        col.prop(props, "smaa_quality", text="Quality")
        row = layout.row()
        row.prop(props, "smaa_predication_scale", text="SMAA Scale")


class RENDER_PT_eevee_next_sampling(RenderButtonsPanel, Panel):
    bl_label = "Sampling"
    COMPAT_ENGINES = {'BLENDER_EEVEE_NEXT'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        scene = context.scene
        props = scene.eevee

        col = layout.column(align=True)
        col.prop(props, "taa_render_samples", text="Render")
        col.prop(props, "taa_samples", text="Viewport")

        col = layout.column()
        col.prop(props, "use_taa_reprojection")


class RENDER_PT_eevee_indirect_lighting(RenderButtonsPanel, Panel):
    bl_label = "Indirect Lighting"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        scene = context.scene
        props = scene.eevee

        col = layout.column()
        col.operator("scene.light_cache_bake", text="Bake Indirect Lighting", icon='RENDER_STILL')
        col.operator("scene.light_cache_bake", text="Bake Cubemap Only", icon='LIGHTPROBE_CUBEMAP').subset = 'CUBEMAPS'
        col.operator("scene.light_cache_free", text="Delete Lighting Cache")

        cache_info = scene.eevee.gi_cache_info
        if cache_info:
            col.label(text=cache_info)

        col.prop(props, "gi_auto_bake")

        col.prop(props, "gi_diffuse_bounces")
        col.prop(props, "gi_cubemap_resolution")
        col.prop(props, "gi_visibility_resolution", text="Diffuse Occlusion")
        col.prop(props, "gi_irradiance_smoothing")
        col.prop(props, "gi_glossy_clamp")
        col.prop(props, "gi_filter_quality")


class RENDER_PT_eevee_indirect_lighting_display(RenderButtonsPanel, Panel):
    bl_label = "Display"
    bl_parent_id = "RENDER_PT_eevee_indirect_lighting"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        scene = context.scene
        props = scene.eevee

        row = layout.row(align=True)
        row.prop(props, "gi_cubemap_display_size", text="Cubemap Size")
        row.prop(props, "gi_show_cubemaps", text="", toggle=True)

        row = layout.row(align=True)
        row.prop(props, "gi_irradiance_display_size", text="Irradiance Size")
        row.prop(props, "gi_show_irradiance", text="", toggle=True)


class RENDER_PT_eevee_film(RenderButtonsPanel, Panel):
    bl_label = "Film"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        scene = context.scene
        rd = scene.render
        props = scene.eevee

        col = layout.column()
        col.prop(rd, "filter_size")
        col.prop(rd, "film_transparent", text="Transparent")

        col = layout.column(align=False, heading="Overscan")
        row = col.row(align=True)
        sub = row.row(align=True)
        sub.prop(props, "use_overscan", text="")
        sub = sub.row(align=True)
        sub.active = props.use_overscan
        sub.prop(props, "overscan_size", text="")


class RENDER_PT_eevee_next_film(RenderButtonsPanel, Panel):
    bl_label = "Film"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE_NEXT'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        scene = context.scene
        rd = scene.render
        props = scene.eevee

        col = layout.column()
        col.prop(rd, "filter_size")
        col.prop(rd, "film_transparent", text="Transparent")


def draw_curves_settings(self, context):
    layout = self.layout
    scene = context.scene
    rd = scene.render

    layout.use_property_split = True
    layout.use_property_decorate = False  # No animation.

    layout.prop(rd, "hair_type", text="Shape", expand=True)
    layout.prop(rd, "hair_subdiv")


class RENDER_PT_eevee_hair(RenderButtonsPanel, Panel):
    bl_label = "Curves"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        draw_curves_settings(self, context)


class RENDER_PT_eevee_performance(RenderButtonsPanel, Panel):
    bl_label = "Performance"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        rd = scene.render

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        layout.prop(rd, "use_high_quality_normals")


class RENDER_PT_gpencil(RenderButtonsPanel, Panel):
    bl_label = "Grease Pencil"
    bl_options = {'DEFAULT_CLOSED'}
    bl_order = 10
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        scene = context.scene
        props = scene.grease_pencil_settings

        col = layout.column()
        col.prop(props, "antialias_threshold")


class RENDER_PT_opengl_sampling(RenderButtonsPanel, Panel):
    bl_label = "Sampling"
    COMPAT_ENGINES = {'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        scene = context.scene
        props = scene.display

        col = layout.column()
        col.prop(props, "render_aa", text="Render")
        col.prop(props, "viewport_aa", text="Viewport")


class RENDER_PT_opengl_film(RenderButtonsPanel, Panel):
    bl_label = "Film"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        rd = context.scene.render
        layout.prop(rd, "film_transparent", text="Transparent")


class RENDER_PT_opengl_lighting(RenderButtonsPanel, Panel):
    bl_label = "Lighting"
    COMPAT_ENGINES = {'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        VIEW3D_PT_shading_lighting.draw(self, context)


class RENDER_PT_opengl_color(RenderButtonsPanel, Panel):
    bl_label = "Color"
    COMPAT_ENGINES = {'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        VIEW3D_PT_shading_color._draw_color_type(self, context)


class RENDER_PT_opengl_options(RenderButtonsPanel, Panel):
    bl_label = "Options"
    COMPAT_ENGINES = {'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        VIEW3D_PT_shading_options.draw(self, context)


class RENDER_PT_simplify(RenderButtonsPanel, Panel):
    bl_label = "Simplify"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw_header(self, context):
        rd = context.scene.render
        self.layout.prop(rd, "use_simplify", text="")

    def draw(self, context):
        pass


class RENDER_PT_simplify_viewport(RenderButtonsPanel, Panel):
    bl_label = "Viewport"
    bl_parent_id = "RENDER_PT_simplify"
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        rd = context.scene.render

        layout.active = rd.use_simplify

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=False, even_rows=False, align=True)

        col = flow.column()
        col.prop(rd, "simplify_subdivision", text="Max Subdivision")

        col = flow.column()
        col.prop(rd, "simplify_child_particles", text="Max Child Particles")

        col = flow.column()
        col.prop(rd, "simplify_volumes", text="Volume Resolution")


class RENDER_PT_simplify_render(RenderButtonsPanel, Panel):
    bl_label = "Render"
    bl_parent_id = "RENDER_PT_simplify"
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        rd = context.scene.render

        layout.active = rd.use_simplify

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=False, even_rows=False, align=True)

        col = flow.column()
        col.prop(rd, "simplify_subdivision_render", text="Max Subdivision")

        col = flow.column()
        col.prop(rd, "simplify_child_particles_render", text="Max Child Particles")


class RENDER_PT_simplify_greasepencil(RenderButtonsPanel, Panel, GreasePencilSimplifyPanel):
    bl_label = "Grease Pencil"
    bl_parent_id = "RENDER_PT_simplify"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_GAME',
        'BLENDER_CLAY',
        'BLENDER_EEVEE',
        'BLENDER_EEVEE_NEXT',
        'BLENDER_WORKBENCH',
    }
    bl_options = {'DEFAULT_CLOSED'}


classes = (
    RENDER_PT_context,
    RENDER_PT_game_resolution, # UPBGE
    RENDER_PT_game_debug, # UPBGE
    RENDER_PT_eevee_sampling,
    RENDER_PT_eevee_sampling_smaa, # UPBGE
    RENDER_PT_eevee_next_sampling,
    RENDER_PT_eevee_ambient_occlusion,
    RENDER_PT_eevee_bloom,
    RENDER_PT_eevee_depth_of_field,
    RENDER_PT_eevee_next_depth_of_field,
    RENDER_PT_eevee_subsurface_scattering,
    RENDER_PT_eevee_screen_space_reflections,
    RENDER_PT_eevee_motion_blur,
    RENDER_PT_eevee_next_motion_blur,
    RENDER_PT_motion_blur_curve,
    RENDER_PT_eevee_volumetric,
    RENDER_PT_eevee_volumetric_lighting,
    RENDER_PT_eevee_volumetric_shadows,
    RENDER_PT_eevee_volumetric_blending, # UPBGE
    RENDER_PT_eevee_performance,
    RENDER_PT_eevee_hair,
    RENDER_PT_eevee_shadows,
    RENDER_PT_eevee_indirect_lighting,
    RENDER_PT_eevee_indirect_lighting_display,
    RENDER_PT_eevee_film,
    RENDER_PT_eevee_next_film,


    RENDER_PT_gpencil,
    RENDER_PT_opengl_sampling,
    RENDER_PT_opengl_lighting,
    RENDER_PT_opengl_color,
    RENDER_PT_opengl_options,
    RENDER_PT_opengl_film,
    RENDER_PT_color_management,
    RENDER_PT_color_management_curves,
    RENDER_PT_simplify,
    RENDER_PT_simplify_viewport,
    RENDER_PT_simplify_render,
    RENDER_PT_simplify_greasepencil,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
