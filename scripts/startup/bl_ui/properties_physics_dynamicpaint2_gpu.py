# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Physics tab UI panels for the DynamicPaint2Gpu modifier.

Mirrors the architecture of properties_physics_dynamicpaint.py:
  - Panels live in the Physics properties tab (bl_context = "physics").
  - The modifier's C++ panel_draw() says "Settings are inside the Physics tab".
  - context.dynamic_paint2gpu is provided by buttons_context.cc.
"""

from bpy.types import (
    Panel,
    UIList,
)


# ---------------------------------------------------------------------------
# UIList for brushes

class PHYSICS_UL_dp2gpu_brushes(UIList):
    def draw_item(self, _context, layout, _data, item, icon, _active_data, _active_propname, _index):
        brush = item
        if self.layout_type in {'DEFAULT', 'COMPACT'}:
            row = layout.row(align=True)
            row.prop(brush, "name", text="", emboss=False, icon='BRUSH_DATA')
        elif self.layout_type == 'GRID':
            layout.alignment = 'CENTER'
            layout.label(text="", icon='BRUSH_DATA')


# ---------------------------------------------------------------------------
# Base class

class PhysicButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "physics"

    @staticmethod
    def poll_dp2gpu(context):
        ob = context.object
        return (ob and ob.type == 'MESH') and context.dynamic_paint2gpu

    @staticmethod
    def poll_dp2gpu_brush(context):
        if not PhysicButtonsPanel.poll_dp2gpu(context):
            return False
        md = context.dynamic_paint2gpu
        return md is not None and md.ui_type == 'BRUSH'

    @staticmethod
    def poll_dp2gpu_canvas(context):
        if not PhysicButtonsPanel.poll_dp2gpu(context):
            return False
        md = context.dynamic_paint2gpu
        return md is not None and md.ui_type == 'CANVAS'

    @staticmethod
    def poll_dp2gpu_has_brushes(context):
        if not PhysicButtonsPanel.poll_dp2gpu_brush(context):
            return False
        md = context.dynamic_paint2gpu
        return md is not None and len(md.brushes) > 0


# ---------------------------------------------------------------------------
# Main panel -- Canvas / Brush toggle  (like Dynamic Paint)

class PHYSICS_PT_dynamic_paint2gpu(PhysicButtonsPanel, Panel):
    bl_label = "Dynamic Paint GPU"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH',
    }

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_dp2gpu(context):
            return False
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.dynamic_paint2gpu
        if md is None:
            return

        layout.prop(md, "ui_type")


# ---------------------------------------------------------------------------
# Canvas panel  (placeholder -- will be expanded later)

class PHYSICS_PT_dp2gpu_canvas(PhysicButtonsPanel, Panel):
    bl_label = "Canvas"
    bl_parent_id = "PHYSICS_PT_dynamic_paint2gpu"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH',
    }

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_dp2gpu_canvas(context):
            return False
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.dynamic_paint2gpu
        if md is None:
            return

        layout.prop(md, "brush_collection", text="Brush Collection")


# ---------------------------------------------------------------------------
# Brush settings panel -- all brush settings in one panel, one box per brush
# containing direction, falloff, and texture settings together.

class PHYSICS_PT_dp2gpu_brushes(PhysicButtonsPanel, Panel):
    bl_label = "Brushes"
    bl_parent_id = "PHYSICS_PT_dynamic_paint2gpu"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH',
    }

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_dp2gpu_brush(context):
            return False
        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.dynamic_paint2gpu
        if md is None:
            return

        row = layout.row()
        row.operator("dpaint2gpu.brush_add", text="Add Brush", icon='ADD')

        brushes = md.brushes
        if len(brushes) == 0:
            return

        for i, brush in enumerate(brushes):
            box = layout.box()

            # -- Header with name + remove button --
            header = box.row(align=True)
            header.prop(brush, "name", text="", icon='BRUSH_DATA')
            remove_op = header.operator("dpaint2gpu.brush_remove", text="", icon='X')
            remove_op.index = i

            col = box.column()

            # -- Direction settings --
            col.prop(brush, "direction_type", text="Direction Type")

            if brush.direction_type == 'OBJECT':
                col.prop(brush, "direction_object", text="Object Mode")
                col.prop(brush, "origin", text="Ray Origin")
                if brush.direction_object == 'ORIGIN_TO_TARGET':
                    col.prop(brush, "target", text="Ray Target")
            else:
                col.prop(brush, "direction_axis", text="Ray Axis")

            col.prop(brush, "use_vertex_normals")

            col.separator()

            col.prop(brush, "ray_length")
            col.prop(brush, "radius")
            col.prop(brush, "intensity", slider=True)

            # -- Falloff settings --
            col.separator()
            col.label(text="Falloff", icon='SMOOTHCURVE')
            col.prop(brush, "falloff_type")

            sub = col.row()
            sub.active = (brush.falloff_type != 'NONE')
            sub.prop(brush, "falloff")

            if brush.falloff_type == 'CURVE':
                col.template_curve_mapping(brush, "falloff_curve")

            # -- Texture settings --
            col.separator()
            col.label(text="Texture", icon='TEXTURE')
            col.template_ID(brush, "mask_texture", new="texture.new")

            if brush.mask_texture:
                col.prop(brush, "texture_coords", text="Coordinates")

                if brush.texture_coords == 'OBJECT':
                    col.prop(brush, "texture_coords_object", text="Object")
                elif brush.texture_coords == 'UV':
                    col.prop(brush, "uv_layer", text="UV Map")


# ---------------------------------------------------------------------------
# Registration

classes = (
    PHYSICS_UL_dp2gpu_brushes,
    PHYSICS_PT_dynamic_paint2gpu,
    PHYSICS_PT_dp2gpu_canvas,
    PHYSICS_PT_dp2gpu_brushes,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
