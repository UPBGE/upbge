# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import (
    Panel,
)
from bl_ui.utils import PresetPanel

from bl_ui.properties_physics_common import (
    point_cache_ui,
    effector_weights_ui,
)


def cloth_panel_enabled(md):
    return md.point_cache.is_baked is False


class CLOTH_PT_presets(PresetPanel, Panel):
    bl_label = "Cloth Presets"
    preset_subdir = "cloth"
    preset_operator = "script.execute_preset"
    preset_add_operator = "cloth.preset_add"


class PhysicButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "physics"

    @classmethod
    def poll(cls, context):
        ob = context.object
        return (ob and ob.type == 'MESH') and (context.engine in cls.COMPAT_ENGINES) and (context.cloth)


class PHYSICS_PT_cloth(PhysicButtonsPanel, Panel):
    bl_label = "Cloth"
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw_header_preset(self, _context):
        CLOTH_PT_presets.draw_panel_header(self.layout)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.cloth
        cloth = md.settings

        layout.active = cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column()
        col.prop(cloth, "quality", text="Quality Steps")
        col = flow.column()
        col.prop(cloth, "time_scale", text="Speed Multiplier")


class PHYSICS_PT_cloth_physical_properties(PhysicButtonsPanel, Panel):
    bl_label = "Physical Properties"
    bl_parent_id = 'PHYSICS_PT_cloth'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.cloth
        cloth = md.settings

        layout.active = cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column()
        col.prop(cloth, "mass", text="Vertex Mass")
        col = flow.column()
        col.prop(cloth, "air_damping", text="Air Viscosity")
        col = flow.column()
        col.prop(cloth, "bending_model")


class PHYSICS_PT_cloth_stiffness(PhysicButtonsPanel, Panel):
    bl_label = "Stiffness"
    bl_parent_id = 'PHYSICS_PT_cloth_physical_properties'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.cloth
        cloth = md.settings

        layout.active = cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column()

        if cloth.bending_model == 'ANGULAR':
            col.prop(cloth, "tension_stiffness", text="Tension")
            col = flow.column()
            col.prop(cloth, "compression_stiffness", text="Compression")
        else:
            col.prop(cloth, "tension_stiffness", text="Structural")

        col = flow.column()
        col.prop(cloth, "shear_stiffness", text="Shear")
        col = flow.column()
        col.prop(cloth, "bending_stiffness", text="Bending")


class PHYSICS_PT_cloth_damping(PhysicButtonsPanel, Panel):
    bl_label = "Damping"
    bl_parent_id = 'PHYSICS_PT_cloth_physical_properties'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.cloth
        cloth = md.settings

        layout.active = cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column()

        if cloth.bending_model == 'ANGULAR':
            col.prop(cloth, "tension_damping", text="Tension")
            col = flow.column()
            col.prop(cloth, "compression_damping", text="Compression")
        else:
            col.prop(cloth, "tension_damping", text="Structural")

        col = flow.column()
        col.prop(cloth, "shear_damping", text="Shear")
        col = flow.column()
        col.prop(cloth, "bending_damping", text="Bending")


class PHYSICS_PT_cloth_internal_springs(PhysicButtonsPanel, Panel):
    bl_label = "Internal Springs"
    bl_parent_id = 'PHYSICS_PT_cloth_physical_properties'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw_header(self, context):
        cloth = context.cloth.settings

        self.layout.active = cloth_panel_enabled(context.cloth)
        self.layout.prop(cloth, "use_internal_springs", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        cloth = context.cloth.settings
        md = context.cloth
        ob = context.object

        layout.active = cloth.use_internal_springs and cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column()
        col.prop(cloth, "internal_spring_max_length", text="Max Spring Creation Length")
        col = flow.column()
        col.prop(cloth, "internal_spring_max_diversion", text="Max Creation Diversion")
        col = flow.column()
        col.prop(cloth, "internal_spring_normal_check", text="Check Surface Normals")
        col = flow.column()
        col.prop(cloth, "internal_tension_stiffness", text="Tension")
        col = flow.column()
        col.prop(cloth, "internal_compression_stiffness", text="Compression")

        col = flow.column()
        col.prop_search(cloth, "vertex_group_intern", ob, "vertex_groups", text="Vertex Group")
        col = flow.column()
        col.prop(cloth, "internal_tension_stiffness_max", text="Max Tension")
        col = flow.column()
        col.prop(cloth, "internal_compression_stiffness_max", text="Max Compression")


class PHYSICS_PT_cloth_pressure(PhysicButtonsPanel, Panel):
    bl_label = "Pressure"
    bl_parent_id = 'PHYSICS_PT_cloth_physical_properties'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw_header(self, context):
        cloth = context.cloth.settings

        self.layout.active = cloth_panel_enabled(context.cloth)
        self.layout.prop(cloth, "use_pressure", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        cloth = context.cloth.settings
        md = context.cloth
        ob = context.object

        layout.active = cloth.use_pressure and cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column()
        col.prop(cloth, "uniform_pressure_force")

        col = flow.column()
        col.prop(cloth, "use_pressure_volume", text="Custom Volume")

        col = flow.column()
        col.active = cloth.use_pressure_volume
        col.prop(cloth, "target_volume")

        col = flow.column()
        col.prop(cloth, "pressure_factor")

        col = flow.column()
        col.prop(cloth, "fluid_density")

        col = flow.column()
        col.prop_search(cloth, "vertex_group_pressure", ob, "vertex_groups", text="Vertex Group")


class PHYSICS_PT_cloth_cache(PhysicButtonsPanel, Panel):
    bl_label = "Cache"
    bl_parent_id = 'PHYSICS_PT_cloth'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        md = context.cloth
        point_cache_ui(self, md.point_cache, cloth_panel_enabled(md), 'CLOTH')


class PHYSICS_PT_cloth_shape(PhysicButtonsPanel, Panel):
    bl_label = "Shape"
    bl_parent_id = 'PHYSICS_PT_cloth'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.cloth
        ob = context.object
        cloth = md.settings

        layout.active = cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column(align=True)
        col.prop_search(cloth, "vertex_group_mass", ob, "vertex_groups", text="Pin Group")

        sub = col.column(align=True)
        sub.active = cloth.vertex_group_mass != ""
        sub.prop(cloth, "pin_stiffness", text="Stiffness")

        col.separator()

        col = flow.column(align=True)
        col.prop(cloth, "use_sewing_springs", text="Sewing")

        sub = col.column(align=True)
        sub.active = cloth.use_sewing_springs
        sub.prop(cloth, "sewing_force_max", text="Max Sewing Force")

        col.separator()

        col = flow.column()
        col.prop(cloth, "shrink_min", text="Shrinking Factor")

        col = flow.column()
        col.prop(cloth, "use_dynamic_mesh", text="Dynamic Mesh")

        key = ob.data.shape_keys

        if key:
            col = flow.column()
            col.active = not cloth.use_dynamic_mesh
            col.prop_search(cloth, "rest_shape_key", key, "key_blocks", text="Rest Shape Key")


class PHYSICS_PT_cloth_collision(PhysicButtonsPanel, Panel):
    bl_label = "Collisions"
    bl_parent_id = 'PHYSICS_PT_cloth'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        cloth = context.cloth.collision_settings
        md = context.cloth

        layout.active = (cloth.use_collision or cloth.use_self_collision) and cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column()
        col.prop(cloth, "collision_quality", text="Quality")


class PHYSICS_PT_cloth_object_collision(PhysicButtonsPanel, Panel):
    bl_label = "Object Collisions"
    bl_parent_id = 'PHYSICS_PT_cloth_collision'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw_header(self, context):
        cloth = context.cloth.collision_settings

        self.layout.active = cloth_panel_enabled(context.cloth)
        self.layout.prop(cloth, "use_collision", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        cloth = context.cloth.collision_settings
        md = context.cloth
        ob = context.object

        layout.active = cloth.use_collision and cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column()
        col.prop(cloth, "distance_min", slider=True, text="Distance")

        col = flow.column()
        col.prop(cloth, "impulse_clamp")

        col = flow.column()
        col.prop_search(cloth, "vertex_group_object_collisions", ob, "vertex_groups", text="Vertex Group")

        col = flow.column()
        col.prop(cloth, "collection")


class PHYSICS_PT_cloth_self_collision(PhysicButtonsPanel, Panel):
    bl_label = "Self Collisions"
    bl_parent_id = 'PHYSICS_PT_cloth_collision'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw_header(self, context):
        cloth = context.cloth.collision_settings

        self.layout.active = cloth_panel_enabled(context.cloth)
        self.layout.prop(cloth, "use_self_collision", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        cloth = context.cloth.collision_settings
        md = context.cloth
        ob = context.object

        layout.active = cloth.use_self_collision and cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column()
        col.prop(cloth, "self_friction", text="Friction")

        col = flow.column()
        col.prop(cloth, "self_distance_min", slider=True, text="Distance")

        col = flow.column()
        col.prop(cloth, "self_impulse_clamp")

        col = flow.column()
        col.prop_search(cloth, "vertex_group_self_collisions", ob, "vertex_groups", text="Vertex Group")


class PHYSICS_PT_cloth_property_weights(PhysicButtonsPanel, Panel):
    bl_label = "Property Weights"
    bl_parent_id = 'PHYSICS_PT_cloth'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.cloth
        ob = context.object
        cloth = context.cloth.settings

        layout.active = cloth_panel_enabled(md)

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=True)

        col = flow.column()
        col.prop_search(
            cloth, "vertex_group_structural_stiffness", ob, "vertex_groups",
            text="Structural Group",
        )
        col.prop(cloth, "tension_stiffness_max", text="Max Tension")
        col.prop(cloth, "compression_stiffness_max", text="Max Compression")

        col.separator()

        col = flow.column()
        col.prop_search(
            cloth, "vertex_group_shear_stiffness", ob, "vertex_groups",
            text="Shear Group",
        )
        col.prop(cloth, "shear_stiffness_max", text="Max Shearing")

        col.separator()

        col = flow.column()
        col.prop_search(
            cloth, "vertex_group_bending", ob, "vertex_groups",
            text="Bending Group"
        )
        col.prop(cloth, "bending_stiffness_max", text="Max Bending")

        col.separator()

        col = flow.column()
        col.prop_search(
            cloth, "vertex_group_shrink", ob, "vertex_groups",
            text="Shrinking Group"
        )
        col.prop(cloth, "shrink_max", text="Max Shrinking")


class PHYSICS_PT_cloth_field_weights(PhysicButtonsPanel, Panel):
    bl_label = "Field Weights"
    bl_parent_id = 'PHYSICS_PT_cloth'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        cloth = context.cloth.settings
        effector_weights_ui(self, cloth.effector_weights, 'CLOTH')


classes = (
    CLOTH_PT_presets,
    PHYSICS_PT_cloth,
    PHYSICS_PT_cloth_physical_properties,
    PHYSICS_PT_cloth_stiffness,
    PHYSICS_PT_cloth_damping,
    PHYSICS_PT_cloth_internal_springs,
    PHYSICS_PT_cloth_pressure,
    PHYSICS_PT_cloth_cache,
    PHYSICS_PT_cloth_shape,
    PHYSICS_PT_cloth_collision,
    PHYSICS_PT_cloth_object_collision,
    PHYSICS_PT_cloth_self_collision,
    PHYSICS_PT_cloth_property_weights,
    PHYSICS_PT_cloth_field_weights,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
