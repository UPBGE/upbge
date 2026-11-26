# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Render settings of the EEVEE-Game renderer, which draws the game engine. The scene's render
# engine stays EEVEE, so these panels show next to the EEVEE ones.

from bpy.types import Panel


class EEVEEGameButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "render"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        return (context.engine in cls.COMPAT_ENGINES)


class RENDER_PT_eevee_game(EEVEEGameButtonsPanel, Panel):
    bl_label = "EEVEE-Game"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        pass


class RENDER_PT_eevee_game_antialiasing(EEVEEGameButtonsPanel, Panel):
    bl_label = "Anti-Aliasing"
    bl_parent_id = "RENDER_PT_eevee_game"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        props = context.scene.eevee

        col = layout.column()
        col.prop(props, "game_aa_method", text="Method")
        temporal = props.game_aa_method not in {'NONE', 'FXAA', 'SMAA'}
        sub = col.column()
        sub.active = temporal
        sub.prop(props, "game_upscale_quality", text="Quality")
        sub.prop(props, "game_upscale_sharpness")


class RENDER_PT_eevee_game_bloom(EEVEEGameButtonsPanel, Panel):
    bl_label = "Bloom"
    bl_parent_id = "RENDER_PT_eevee_game"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, context):
        self.layout.prop(context.scene.eevee, "use_bloom", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        props = context.scene.eevee

        layout.active = props.use_bloom
        col = layout.column()
        col.prop(props, "bloom_threshold")
        col.prop(props, "bloom_knee")
        col.prop(props, "bloom_radius")
        col.prop(props, "bloom_color")
        col.prop(props, "bloom_intensity")
        col.prop(props, "bloom_clamp")


class DATA_PT_eevee_game_light_shadow(Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"
    bl_label = "EEVEE-Game Shadow"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        light = context.light
        return light and light.type != 'SUN' and (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        light = context.light
        layout.active = light.use_shadow
        layout.prop(light, "shadow_buffer_bias", text="Bias")


classes = (
    RENDER_PT_eevee_game,
    RENDER_PT_eevee_game_antialiasing,
    RENDER_PT_eevee_game_bloom,
    DATA_PT_eevee_game_light_shadow,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
