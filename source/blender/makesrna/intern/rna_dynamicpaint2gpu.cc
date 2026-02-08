/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 *
 * RNA bindings for Dynamic Paint 2 GPU modifier.
 * Follows the pattern of rna_dynamicpaint.cc:
 *   - Runtime callbacks inside #ifdef RNA_RUNTIME
 *   - Definition functions (rna_def_*) outside #ifdef RNA_RUNTIME
 */

#include "RNA_define.hh"
#include "BLT_translation.hh"
#include "rna_internal.hh"
#include "DNA_modifier_types.h"
#include "DNA_dynamicpaint2gpu_types.h"

#ifdef RNA_RUNTIME

#  include <fmt/format.h>

#  include "BLI_listbase.h"
#  include "BLI_string.h"

namespace blender {

static std::optional<std::string> rna_DynamicPaint2GpuCanvasSettings_path(const PointerRNA *ptr)
{
  const DynamicPaint2GpuCanvasSettings *settings =
      static_cast<DynamicPaint2GpuCanvasSettings *>(ptr->data);
  const ModifierData *md = reinterpret_cast<ModifierData *>(settings->pmd);
  char name_esc[sizeof(md->name) * 2];

  BLI_str_escape(name_esc, md->name, sizeof(name_esc));
  return fmt::format("modifiers[\"{}\"].canvas", name_esc);
}

static std::optional<std::string> rna_DynamicPaint2GpuSurface_path(const PointerRNA *ptr)
{
  const DynamicPaint2GpuSurface *surface =
      static_cast<DynamicPaint2GpuSurface *>(ptr->data);
  if (surface->canvas && surface->canvas->pmd) {
    const ModifierData *md = reinterpret_cast<ModifierData *>(surface->canvas->pmd);
    char name_esc[sizeof(md->name) * 2];
    char name_esc_surface[sizeof(surface->name) * 2];

    BLI_str_escape(name_esc, md->name, sizeof(name_esc));
    BLI_str_escape(name_esc_surface, surface->name, sizeof(name_esc_surface));
    return fmt::format(
        "modifiers[\"{}\"].canvas.canvas_surfaces[\"{}\"]", name_esc, name_esc_surface);
  }
  return std::optional<std::string>();
}

static PointerRNA rna_GPU_PaintSurface_active_get(PointerRNA *ptr)
{
  DynamicPaint2GpuCanvasSettings *canvas =
      static_cast<DynamicPaint2GpuCanvasSettings *>(ptr->data);
  DynamicPaint2GpuSurface *surface =
      static_cast<DynamicPaint2GpuSurface *>(canvas->surfaces.first);
  int id = 0;

  for (; surface; surface = surface->next) {
    if (id == canvas->active_sur) {
      return RNA_pointer_create_with_parent(*ptr, RNA_DynamicPaint2GpuSurface, surface);
    }
    id++;
  }
  return PointerRNA_NULL;
}

static void rna_DynamicPaint2Gpu_surfaces_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  DynamicPaint2GpuCanvasSettings *canvas =
      static_cast<DynamicPaint2GpuCanvasSettings *>(ptr->data);
  rna_iterator_listbase_begin(iter, ptr, &canvas->surfaces, nullptr);
}

static int rna_GPU_Surface_active_index_get(PointerRNA *ptr)
{
  DynamicPaint2GpuCanvasSettings *canvas =
      static_cast<DynamicPaint2GpuCanvasSettings *>(ptr->data);
  return canvas->active_sur;
}

static void rna_GPU_Surface_active_index_set(PointerRNA *ptr, int value)
{
  DynamicPaint2GpuCanvasSettings *canvas =
      static_cast<DynamicPaint2GpuCanvasSettings *>(ptr->data);
  canvas->active_sur = short(value);
}

static void rna_GPU_Surface_active_index_range(
    PointerRNA *ptr, int *min, int *max, int * /*softmin*/, int * /*softmax*/)
{
  DynamicPaint2GpuCanvasSettings *canvas =
      static_cast<DynamicPaint2GpuCanvasSettings *>(ptr->data);
  *min = 0;
  *max = BLI_listbase_count(&canvas->surfaces) - 1;
}

}  // namespace blender

#endif

namespace blender {

static const EnumPropertyItem rna_enum_dp2gpu_direction_type_items[] = {
    {DP2GPU_DIRTYPE_AXIS, "AXIS", 0, "Axis", "Cast ray along a world axis"},
    {DP2GPU_DIRTYPE_OBJECT, "OBJECT", 0, "Object", "Cast ray using origin/target objects"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_dp2gpu_direction_axis_items[] = {
    {DP2GPU_DIR_X, "X", 0, "+X", "Cast ray along the +X axis"},
    {DP2GPU_DIR_NEG_X, "NEG_X", 0, "-X", "Cast ray along the -X axis"},
    {DP2GPU_DIR_Y, "Y", 0, "+Y", "Cast ray along the +Y axis"},
    {DP2GPU_DIR_NEG_Y, "NEG_Y", 0, "-Y", "Cast ray along the -Y axis"},
    {DP2GPU_DIR_Z, "Z", 0, "+Z", "Cast ray along the +Z axis"},
    {DP2GPU_DIR_NEG_Z, "NEG_Z", 0, "-Z", "Cast ray along the -Z axis"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_dp2gpu_direction_object_items[] = {
    {DP2GPU_DIR_ORIGIN_TO_TARGET,
     "ORIGIN_TO_TARGET",
     0,
     "Origin \u2192 Target",
     "Ray from origin object toward target object (uses brush position as fallback)"},
    {DP2GPU_DIR_ORIGIN_FORWARD,
     "ORIGIN_FORWARD",
     0,
     "Origin Forward",
     "Use origin object's local forward (-Y) axis"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_dp2gpu_falloff_items[] = {
    {DP2GPU_FALLOFF_NONE, "NONE", 0, "No Falloff", "Constant intensity within radius"},
    {DP2GPU_FALLOFF_CURVE, "CURVE", 0, "Curve", "Custom curve falloff"},
    {DP2GPU_FALLOFF_SHARP, "SHARP", 0, "Sharp", "Sharp falloff"},
    {DP2GPU_FALLOFF_SMOOTH, "SMOOTH", 0, "Smooth", "Smooth falloff"},
    {DP2GPU_FALLOFF_ROOT, "ROOT", 0, "Root", "Square root falloff"},
    {DP2GPU_FALLOFF_LINEAR, "LINEAR", 0, "Linear", "Linear falloff"},
    {DP2GPU_FALLOFF_CONST, "CONST", 0, "Constant", "Constant (no falloff)"},
    {DP2GPU_FALLOFF_SPHERE, "SPHERE", 0, "Sphere", "Spherical falloff"},
    {DP2GPU_FALLOFF_INVSQUARE, "INVSQUARE", 0, "Inverse Square", "Inverse square falloff"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_dp2gpu_texmapping_items[] = {
    {MOD_DISP_MAP_LOCAL, "LOCAL", 0, "Local", "Use local coordinates for texture mapping"},
    {MOD_DISP_MAP_GLOBAL, "GLOBAL", 0, "Global", "Use global coordinates for texture mapping"},
    {MOD_DISP_MAP_OBJECT, "OBJECT", 0, "Object", "Use another object's coordinates for texture mapping"},
    {MOD_DISP_MAP_UV, "UV", 0, "UV", "Use UV coordinates for texture mapping"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void rna_def_dynamic_paint2gpu_brush(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "DynamicPaint2GpuBrush", nullptr);
  RNA_def_struct_sdna(srna, "DynamicPaint2GpuBrushSettings");
  RNA_def_struct_ui_text(srna, "GPU Brush", "GPU dynamic paint brush settings");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "name");
  RNA_def_property_ui_text(prop, "Name", "Brush name");
  RNA_def_struct_name_property(srna, prop);

  /* Ray settings */
  prop = RNA_def_property(srna, "origin", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "origin");
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Ray Origin", "Object used as ray origin");

  prop = RNA_def_property(srna, "target", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "target");
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Ray Target",
                           "Target object (ray direction / length computed from origin to target)");

  prop = RNA_def_property(srna, "direction_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "direction_type");
  RNA_def_property_enum_items(prop, rna_enum_dp2gpu_direction_type_items);
  RNA_def_property_ui_text(prop, "Direction Type", "How the ray direction is determined");

  prop = RNA_def_property(srna, "direction_axis", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "direction_mode");
  RNA_def_property_enum_items(prop, rna_enum_dp2gpu_direction_axis_items);
  RNA_def_property_ui_text(prop, "Ray Axis", "World axis along which to cast the ray");

  prop = RNA_def_property(srna, "direction_object", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "direction_mode");
  RNA_def_property_enum_items(prop, rna_enum_dp2gpu_direction_object_items);
  RNA_def_property_ui_text(prop, "Object Mode", "Object-based ray direction mode");

  prop = RNA_def_property(srna, "use_vertex_normals", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_vertex_normals", 1);
  RNA_def_property_ui_text(prop, "Use Vertex Normals",
                           "Use vertex normals for displacement direction instead of axis");

  prop = RNA_def_property(srna, "ray_length", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "ray_length");
  RNA_def_property_range(prop, 0.0, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0, 100.0, 1, 3);
  RNA_def_property_ui_text(prop, "Ray Length",
                           "Ray cast length (0 = automatic, use distance to target or global)");

  /* Per-brush parameters */
  prop = RNA_def_property(srna, "radius", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "radius");
  RNA_def_property_range(prop, 0.0001, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.001, 10.0, 0.1, 3);
  RNA_def_property_ui_text(prop, "Radius", "Brush influence radius");

  prop = RNA_def_property(srna, "intensity", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "intensity");
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_ui_text(prop, "Intensity", "Brush intensity");

  /* Falloff */
  prop = RNA_def_property(srna, "falloff_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "falloff_type");
  RNA_def_property_enum_items(prop, rna_enum_dp2gpu_falloff_items);
  RNA_def_property_ui_text(prop, "Falloff Type", "Falloff curve shape");

  prop = RNA_def_property(srna, "falloff", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "falloff");
  RNA_def_property_range(prop, 0.0, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0, 100.0, 1, 3);
  RNA_def_property_ui_text(prop, "Falloff Radius",
                           "Distance over which intensity decays (0 = use brush radius)");

  prop = RNA_def_property(srna, "falloff_curve", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "curfalloff");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(prop, "Falloff Curve",
                           "Custom curve controlling falloff (used when Falloff Type = Curve)");

  prop = RNA_def_property(srna, "mask_texture", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "mask_texture");
  RNA_def_property_struct_type(prop, "Texture");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Mask Texture",
                           "Procedural texture to modulate brush intensity");

  /* Texture coordinate mapping */
  prop = RNA_def_property(srna, "texture_coords", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "texmapping");
  RNA_def_property_enum_items(prop, rna_enum_dp2gpu_texmapping_items);
  RNA_def_property_ui_text(prop, "Texture Coordinates",
                           "Coordinate system for texture mapping");

  prop = RNA_def_property(srna, "texture_coords_object", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "map_object");
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Texture Coordinate Object",
                           "Object to use for texture coordinate mapping");

  prop = RNA_def_property(srna, "uv_layer", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "uvlayer_name");
  RNA_def_property_ui_text(prop, "UV Map", "UV map name for UV texture mapping");
}

static void rna_def_dynamic_paint2gpu_surface(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "DynamicPaint2GpuSurface", nullptr);
  RNA_def_struct_sdna(srna, "DynamicPaint2GpuSurface");
  RNA_def_struct_path_func(srna, "rna_DynamicPaint2GpuSurface_path");
  RNA_def_struct_ui_text(srna, "GPU Surface", "GPU dynamic paint surface settings");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "name");
  RNA_def_property_ui_text(prop, "Name", "Surface name");
  RNA_def_struct_name_property(srna, prop);

  prop = RNA_def_property(srna, "brush_collection", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Collection");
  RNA_def_property_pointer_sdna(prop, nullptr, "brush_group");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Brush Collection",
                           "Only use brush objects from this collection");

}

static void rna_def_dynamic_paint2gpu_canvas_surfaces(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  PropertyRNA *prop;

  RNA_def_property_srna(cprop, "DynamicPaint2GpuSurfaces");
  srna = RNA_def_struct(brna, "DynamicPaint2GpuSurfaces", nullptr);
  RNA_def_struct_sdna(srna, "DynamicPaint2GpuCanvasSettings");
  RNA_def_struct_ui_text(
      srna, "GPU Canvas Surfaces", "Collection of GPU dynamic paint canvas surfaces");

  prop = RNA_def_property(srna, "active_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_int_funcs(prop,
                             "rna_GPU_Surface_active_index_get",
                             "rna_GPU_Surface_active_index_set",
                             "rna_GPU_Surface_active_index_range");
  RNA_def_property_ui_text(prop, "Active Surface Index", "Index of the active surface");

  prop = RNA_def_property(srna, "active", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "DynamicPaint2GpuSurface");
  RNA_def_property_pointer_funcs(
      prop, "rna_GPU_PaintSurface_active_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(
      prop, "Active Surface", "Active GPU dynamic paint surface being displayed");
}

void RNA_def_dynamic_paint2gpu(BlenderRNA *brna)
{
  /* NOTE: The modifier struct "DynamicPaint2GpuModifier" is defined in rna_modifier.cc
   * (same pattern as DynamicPaintModifier). Here we only define the sub-structs. */

  /* Sub-struct definitions */
  rna_def_dynamic_paint2gpu_surface(brna);
  rna_def_dynamic_paint2gpu_brush(brna);

  /* Canvas settings */
  {
    StructRNA *canvas_srna;
    PropertyRNA *cprop;

    canvas_srna = RNA_def_struct(brna, "DynamicPaint2GpuCanvasSettings", nullptr);
    RNA_def_struct_sdna(canvas_srna, "DynamicPaint2GpuCanvasSettings");
    RNA_def_struct_path_func(canvas_srna, "rna_DynamicPaint2GpuCanvasSettings_path");
    RNA_def_struct_ui_text(
        canvas_srna, "Canvas Settings", "GPU Dynamic Paint canvas settings");

    cprop = RNA_def_property(canvas_srna, "canvas_surfaces", PROP_COLLECTION, PROP_NONE);
    RNA_def_property_collection_funcs(cprop,
                                      "rna_DynamicPaint2Gpu_surfaces_begin",
                                      "rna_iterator_listbase_next",
                                      "rna_iterator_listbase_end",
                                      "rna_iterator_listbase_get",
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr);
    RNA_def_property_struct_type(cprop, "DynamicPaint2GpuSurface");
    RNA_def_property_ui_text(cprop, "GPU Surface List", "GPU dynamic paint surface list");
    rna_def_dynamic_paint2gpu_canvas_surfaces(brna, cprop);
  }
}

}  // namespace blender

