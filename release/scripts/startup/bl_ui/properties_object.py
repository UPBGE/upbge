# SPDX-License-Identifier: GPL-2.0-or-later
from bl_ui.properties_animviz import (
    MotionPathButtonsPanel,
    MotionPathButtonsPanel_display,
)
import bpy
from bpy.types import Panel, Menu
from rna_prop_ui import PropertyPanel


class ObjectButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "object"


class OBJECT_PT_context_object(ObjectButtonsPanel, Panel):
    bl_label = ""
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        layout = self.layout
        space = context.space_data

        if space.use_pin_id:
            layout.template_ID(space, "pin_id")
        else:
            row = layout.row()
            row.template_ID(context.view_layer.objects, "active", filter='AVAILABLE')


class OBJECT_PT_transform(ObjectButtonsPanel, Panel):
    bl_label = "Transform"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        ob = context.object

        col = layout.column()
        row = col.row(align=True)
        row.prop(ob, "location")
        row.use_property_decorate = False
        row.prop(ob, "lock_location", text="", emboss=False, icon='DECORATE_UNLOCKED')

        rotation_mode = ob.rotation_mode
        if rotation_mode == 'QUATERNION':
            col = layout.column()
            row = col.row(align=True)
            row.prop(ob, "rotation_quaternion", text="Rotation")
            sub = row.column(align=True)
            sub.use_property_decorate = False
            sub.prop(ob, "lock_rotation_w", text="", emboss=False, icon='DECORATE_UNLOCKED')
            sub.prop(ob, "lock_rotation", text="", emboss=False, icon='DECORATE_UNLOCKED')
        elif rotation_mode == 'AXIS_ANGLE':
            col = layout.column()
            row = col.row(align=True)
            row.prop(ob, "rotation_axis_angle", text="Rotation")

            sub = row.column(align=True)
            sub.use_property_decorate = False
            sub.prop(ob, "lock_rotation_w", text="", emboss=False, icon='DECORATE_UNLOCKED')
            sub.prop(ob, "lock_rotation", text="", emboss=False, icon='DECORATE_UNLOCKED')
        else:
            col = layout.column()
            row = col.row(align=True)
            row.prop(ob, "rotation_euler", text="Rotation")
            row.use_property_decorate = False
            row.prop(ob, "lock_rotation", text="", emboss=False, icon='DECORATE_UNLOCKED')
        row = layout.row(align=True)
        row.prop(ob, "rotation_mode", text="Mode")
        row.label(text="", icon='BLANK1')

        col = layout.column()
        row = col.row(align=True)
        row.prop(ob, "scale")
        row.use_property_decorate = False
        row.prop(ob, "lock_scale", text="", emboss=False, icon='DECORATE_UNLOCKED')


# Game engine transition
class OBJECT_PT_transform_game(ObjectButtonsPanel, Panel):
    bl_label = "Transform Game"

    def draw(self, context):
        layout = self.layout
        ob = context.object

        col = layout.column()
        col.prop(ob, "override_game_transform_priority", text="Override logic transform priority")


class OBJECT_PT_delta_transform(ObjectButtonsPanel, Panel):
    bl_label = "Delta Transform"
    bl_parent_id = "OBJECT_PT_transform"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        ob = context.object

        col = layout.column()
        col.prop(ob, "delta_location", text="Location")

        rotation_mode = ob.rotation_mode
        if rotation_mode == 'QUATERNION':
            col.prop(ob, "delta_rotation_quaternion", text="Rotation")
        elif rotation_mode == 'AXIS_ANGLE':
            pass
        else:
            col.prop(ob, "delta_rotation_euler", text="Rotation")

        col.prop(ob, "delta_scale", text="Scale")


class OBJECT_PT_relations(ObjectButtonsPanel, Panel):
    bl_label = "Relations"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)

        ob = context.object

        col = flow.column()
        col.prop(ob, "parent")
        sub = col.column()
        sub.prop(ob, "parent_type")
        parent = ob.parent
        if parent and ob.parent_type == 'BONE' and parent.type == 'ARMATURE':
            sub.prop_search(ob, "parent_bone", parent.data, "bones")
        sub.active = (parent is not None)
        sub.prop(ob, "use_camera_lock_parent")

        col.separator()

        col = flow.column()

        col.prop(ob, "track_axis", text="Tracking Axis")
        col.prop(ob, "up_axis", text="Up Axis")

        col.separator()

        col = flow.column()

        col.label(text="Tracking Axes:")
        col.prop(ob, "track_axis", text="Axis")
        col.prop(ob, "up_axis", text="Up Axis")

        col = flow.column()
        col.prop(ob, "use_slow_parent")
        row = col.row()
        row.active = ((ob.parent is not None) and (ob.use_slow_parent))
        row.prop(ob, "slow_parent_offset", text="Offset")

        col.separator()

        col = flow.column()

        col.prop(ob, "pass_index")


class COLLECTION_MT_context_menu(Menu):
    bl_label = "Collection Specials"

    def draw(self, _context):
        layout = self.layout

        layout.operator("object.collection_unlink", icon='X')
        layout.operator("object.collection_objects_select")
        layout.operator("object.instance_offset_from_cursor")

class OBJECT_PT_collections(ObjectButtonsPanel, Panel):
    bl_label = "Collections"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout

        obj = context.object

        row = layout.row(align=True)
        if bpy.data.collections:
            row.operator("object.collection_link", text="Add to Collection")
        else:
            row.operator("object.collection_add", text="Add to Collection")
        row.operator("object.collection_add", text="", icon='ADD')

        for collection in obj.users_collection:
            col = layout.column(align=True)

            col.context_pointer_set("collection", collection)

            row = col.box().row()
            row.prop(collection, "name", text="")
            row.operator("object.collection_remove", text="", icon='X', emboss=False)
            row.menu("COLLECTION_MT_context_menu", icon='DOWNARROW_HLT', text="")

            row = col.box().row()
            row.prop(collection, "instance_offset", text="")

            row = col.box().row()
            row.prop(collection, "use_collection_spawn", text="Instance Spawn")

class OBJECT_PT_display(ObjectButtonsPanel, Panel):
    bl_label = "Viewport Display"
    bl_options = {'DEFAULT_CLOSED'}
    bl_order = 10

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        obj = context.object
        obj_type = obj.type
        is_geometry = (obj_type in {'MESH', 'CURVE', 'SURFACE', 'META', 'FONT', 'VOLUME', 'CURVES', 'POINTCLOUD'})
        has_bounds = (is_geometry or obj_type in {'LATTICE', 'ARMATURE'})
        is_wire = (obj_type in {'CAMERA', 'EMPTY'})
        is_empty_image = (obj_type == 'EMPTY' and obj.empty_display_type == 'IMAGE')
        is_dupli = (obj.instance_type != 'NONE')
        is_gpencil = (obj_type == 'GPENCIL')

        col = layout.column(heading="Show")
        col.prop(obj, "show_name", text="Name")
        col.prop(obj, "show_axis", text="Axis")

        # Makes no sense for cameras, armatures, etc.!
        # but these settings do apply to dupli instances
        if is_geometry or is_dupli:
            col.prop(obj, "show_wire", text="Wireframe")
        if obj_type == 'MESH' or is_dupli:
            col.prop(obj, "show_all_edges", text="All Edges")
        if is_geometry:
            col.prop(obj, "show_texture_space", text="Texture Space")
            col.prop(obj.display, "show_shadows", text="Shadow")
        col.prop(obj, "show_in_front", text="In Front")
        # if obj_type == 'MESH' or is_empty_image:
        #    col.prop(obj, "show_transparent", text="Transparency")
        sub = layout.column()
        if is_wire:
            # wire objects only use the max. display type for duplis
            sub.active = is_dupli
        sub.prop(obj, "display_type", text="Display As")

        if is_geometry or is_dupli or is_empty_image or is_gpencil:
            # Only useful with object having faces/materials...
            col.prop(obj, "color")

        if has_bounds:
            col = layout.column(align=False, heading="Bounds")
            col.use_property_decorate = False
            row = col.row(align=True)
            sub = row.row(align=True)
            sub.prop(obj, "show_bounds", text="")
            sub = sub.row(align=True)
            sub.active = obj.show_bounds or (obj.display_type == 'BOUNDS')
            sub.prop(obj, "display_bounds_type", text="")
            row.prop_decorator(obj, "display_bounds_type")


class OBJECT_PT_instancing(ObjectButtonsPanel, Panel):
    bl_label = "Instancing"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        # FONT objects need (vertex) instancing for the 'Object Font' feature
        return (ob.type in {'MESH', 'EMPTY', 'FONT'})

    def draw(self, context):
        layout = self.layout

        ob = context.object

        row = layout.row()
        row.prop(ob, "instance_type", expand=True)

        layout.use_property_split = True

        if ob.instance_type == 'VERTS':
            layout.prop(ob, "use_instance_vertices_rotation", text="Align to Vertex Normal")

        elif ob.instance_type == 'COLLECTION':
            col = layout.column()
            col.prop(ob, "instance_collection", text="Collection")

        if ob.instance_type != 'NONE' or ob.particle_systems:
            col = layout.column(heading="Show Instancer", align=True)
            col.prop(ob, "show_instancer_for_viewport", text="Viewport")
            col.prop(ob, "show_instancer_for_render", text="Render")


class OBJECT_PT_instancing_size(ObjectButtonsPanel, Panel):
    bl_label = "Scale by Face Size"
    bl_parent_id = "OBJECT_PT_instancing"

    @classmethod
    def poll(cls, context):
        ob = context.object
        return ob.instance_type == 'FACES'

    def draw_header(self, context):

        ob = context.object
        self.layout.prop(ob, "use_instance_faces_scale", text="")

    def draw(self, context):
        layout = self.layout
        ob = context.object
        layout.use_property_split = True

        layout.active = ob.use_instance_faces_scale
        layout.prop(ob, "instance_faces_scale", text="Factor")


class OBJECT_PT_lineart(ObjectButtonsPanel, Panel):
    bl_label = "Line Art"
    bl_options = {'DEFAULT_CLOSED'}
    bl_order = 10

    @classmethod
    def poll(cls, context):
        ob = context.object
        return (ob.type in {'MESH', 'FONT', 'CURVE', 'SURFACE'})

    def draw(self, context):
        layout = self.layout
        lineart = context.object.lineart

        layout.use_property_split = True

        layout.prop(lineart, "usage")
        layout.use_property_split = True

        row = layout.row(heading="Override Crease")
        row.prop(lineart, "use_crease_override", text="")
        subrow = row.row()
        subrow.active = lineart.use_crease_override
        subrow.prop(lineart, "crease_threshold", slider=True, text="")

        row = layout.row(heading="Intersection Priority")
        row.prop(lineart, "use_intersection_priority_override", text="")
        subrow = row.row()
        subrow.active = lineart.use_intersection_priority_override
        subrow.prop(lineart, "intersection_priority", text="")


class OBJECT_PT_motion_paths(MotionPathButtonsPanel, Panel):
    #bl_label = "Object Motion Paths"
    bl_context = "object"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return (context.object)

    def draw(self, context):
        # layout = self.layout

        ob = context.object
        avs = ob.animation_visualization
        mpath = ob.motion_path

        self.draw_settings(context, avs, mpath)


class OBJECT_PT_motion_paths_display(MotionPathButtonsPanel_display, Panel):
    #bl_label = "Object Motion Paths"
    bl_context = "object"
    bl_parent_id = "OBJECT_PT_motion_paths"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return (context.object)

    def draw(self, context):
        # layout = self.layout

        ob = context.object
        avs = ob.animation_visualization
        mpath = ob.motion_path

        self.draw_settings(context, avs, mpath)


class OBJECT_PT_visibility(ObjectButtonsPanel, Panel):
    bl_label = "Visibility"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        return (context.object) and (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        layout = self.layout
        ob = context.object

        layout.prop(ob, "hide_select", text="Selectable", toggle=False, invert_checkbox=True)

        col = layout.column(heading="Show In")
        col.prop(ob, "hide_viewport", text="Viewports", toggle=False, invert_checkbox=True)
        col.prop(ob, "hide_render", text="Renders", toggle=False, invert_checkbox=True)

        if context.object.type == 'GPENCIL':
            col = layout.column(heading="Grease Pencil")
            col.prop(ob, "use_grease_pencil_lights", toggle=False)

        layout.separator()
        col = layout.column(heading="Mask")
        col.prop(ob, "is_holdout")


class OBJECT_PT_custom_props(ObjectButtonsPanel, PropertyPanel, Panel):
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}
    _context_path = "object"
    _property_type = bpy.types.Object


classes = (
    OBJECT_PT_context_object,
    OBJECT_PT_transform,
    OBJECT_PT_transform_game, # Game engine transition
    OBJECT_PT_delta_transform,
    OBJECT_PT_relations,
    COLLECTION_MT_context_menu,
    OBJECT_PT_collections,
    OBJECT_PT_instancing,
    OBJECT_PT_instancing_size,
    OBJECT_PT_motion_paths,
    OBJECT_PT_motion_paths_display,
    OBJECT_PT_display,
    OBJECT_PT_visibility,
    OBJECT_PT_lineart,
    OBJECT_PT_custom_props,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
