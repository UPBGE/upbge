/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "BLT_translation.hh"

#include "BLO_read_write.hh"

#include "DNA_collection_types.h"
#include "DNA_defaults.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_scene_types.h"

#include "BKE_collection.hh"
#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_lib_query.hh"
#include "BKE_material.hh"
#include "BKE_modifier.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "MOD_grease_pencil_util.hh"
#include "MOD_lineart.hh"
#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_grease_pencil.hh"

namespace blender {

static bool is_first_lineart(const GreasePencilLineartModifierData &md)
{
  if (md.modifier.type != eModifierType_GreasePencilLineart) {
    return false;
  }
  ModifierData *imd = md.modifier.prev;
  while (imd != nullptr) {
    if (imd->type == eModifierType_GreasePencilLineart) {
      return false;
    }
    imd = imd->prev;
  }
  return true;
}

static bool is_last_line_art(const GreasePencilLineartModifierData &md, const bool use_render)
{
  if (md.modifier.type != eModifierType_GreasePencilLineart) {
    return false;
  }
  ModifierData *imd = md.modifier.next;
  while (imd != nullptr) {
    if (imd->type == eModifierType_GreasePencilLineart) {
      if (use_render && (imd->mode & eModifierMode_Render)) {
        return false;
      }
      if ((!use_render) && (imd->mode & eModifierMode_Realtime)) {
        return false;
      }
    }
    imd = imd->next;
  }
  return true;
}

static void init_data(ModifierData *md)
{
  GreasePencilLineartModifierData *gpmd = (GreasePencilLineartModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(gpmd, modifier));

  MEMCPY_STRUCT_AFTER(gpmd, DNA_struct_default_get(GreasePencilLineartModifierData), modifier);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  BKE_modifier_copydata_generic(md, target, flag);

  const GreasePencilLineartModifierData *source_lmd =
      reinterpret_cast<const GreasePencilLineartModifierData *>(md);
  const LineartModifierRuntime *source_runtime = source_lmd->runtime;

  GreasePencilLineartModifierData *target_lmd =
      reinterpret_cast<GreasePencilLineartModifierData *>(target);

  target_lmd->runtime = MEM_new<LineartModifierRuntime>(__func__, *source_runtime);
}

static void free_data(ModifierData *md)
{
  GreasePencilLineartModifierData *lmd = reinterpret_cast<GreasePencilLineartModifierData *>(md);
  if (LineartModifierRuntime *runtime = lmd->runtime) {
    MEM_delete(runtime);
    lmd->runtime = nullptr;
  }
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  GreasePencilLineartModifierData *lmd = (GreasePencilLineartModifierData *)md;

  if (lmd->target_layer[0] == '\0' || !lmd->target_material) {
    return true;
  }
  if (lmd->source_type == LINEART_SOURCE_OBJECT && !lmd->source_object) {
    return true;
  }
  if (lmd->source_type == LINEART_SOURCE_COLLECTION && !lmd->source_collection) {
    return true;
  }
  /* Preventing calculation in depsgraph when baking frames. */
  if (lmd->flags & MOD_LINEART_IS_BAKED) {
    return true;
  }

  return false;
}

static void add_this_collection(Collection &collection,
                                const ModifierUpdateDepsgraphContext *ctx,
                                const int mode,
                                Set<const Object *> &object_dependencies)
{
  bool default_add = true;
  /* Do not do nested collection usage check, this is consistent with lineart calculation, because
   * collection usage doesn't have a INHERIT mode. This might initially be derived from the fact
   * that an object can be inside multiple collections, but might be irrelevant now with the way
   * objects are iterated. Keep this logic for now. */
  if (collection.lineart_usage & COLLECTION_LRT_EXCLUDE) {
    default_add = false;
  }
  FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN (&collection, ob, mode) {
    if (ELEM(ob->type, OB_MESH, OB_MBALL, OB_CURVES_LEGACY, OB_SURF, OB_FONT)) {
      if ((ob->lineart.usage == OBJECT_LRT_INHERIT && default_add) ||
          ob->lineart.usage != OBJECT_LRT_EXCLUDE)
      {
        DEG_add_object_relation(ctx->node, ob, DEG_OB_COMP_GEOMETRY, "Line Art Modifier");
        DEG_add_object_relation(ctx->node, ob, DEG_OB_COMP_TRANSFORM, "Line Art Modifier");
        object_dependencies.add(ob);
      }
    }
    if (ob->type == OB_EMPTY && (ob->transflag & OB_DUPLICOLLECTION)) {
      if (!ob->instance_collection) {
        continue;
      }
      add_this_collection(*ob->instance_collection, ctx, mode, object_dependencies);
      object_dependencies.add(ob);
    }
  }
  FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END;
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  DEG_add_object_relation(ctx->node, ctx->object, DEG_OB_COMP_TRANSFORM, "Line Art Modifier");

  GreasePencilLineartModifierData *lmd = (GreasePencilLineartModifierData *)md;

  /* Always add whole master collection because line art will need the whole scene for
   * visibility computation. Line art exclusion is handled inside #add_this_collection. */

  /* Do we need to distinguish DAG_EVAL_VIEWPORT or DAG_EVAL_RENDER here? */

  LineartModifierRuntime *runtime = reinterpret_cast<LineartModifierRuntime *>(lmd->runtime);
  if (!runtime) {
    runtime = MEM_new<LineartModifierRuntime>(__func__);
    lmd->runtime = runtime;
  }
  Set<const Object *> &object_dependencies = runtime->object_dependencies;
  object_dependencies.clear();

  add_this_collection(*ctx->scene->master_collection, ctx, DAG_EVAL_VIEWPORT, object_dependencies);

  /* No need to add any non-geometry objects into `lmd->object_dependencies` because we won't be
   * loading... */
  if (lmd->calculation_flags & MOD_LINEART_USE_CUSTOM_CAMERA && lmd->source_camera) {
    DEG_add_object_relation(
        ctx->node, lmd->source_camera, DEG_OB_COMP_TRANSFORM, "Line Art Modifier");
    DEG_add_object_relation(
        ctx->node, lmd->source_camera, DEG_OB_COMP_PARAMETERS, "Line Art Modifier");
  }
  else if (ctx->scene->camera) {
    DEG_add_object_relation(
        ctx->node, ctx->scene->camera, DEG_OB_COMP_TRANSFORM, "Line Art Modifier");
    DEG_add_object_relation(
        ctx->node, ctx->scene->camera, DEG_OB_COMP_PARAMETERS, "Line Art Modifier");
    DEG_add_scene_relation(ctx->node, ctx->scene, DEG_SCENE_COMP_PARAMETERS, "Line Art Modifier");
  }
  if (lmd->light_contour_object) {
    DEG_add_object_relation(
        ctx->node, lmd->light_contour_object, DEG_OB_COMP_TRANSFORM, "Line Art Modifier");
  }
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  GreasePencilLineartModifierData *lmd = (GreasePencilLineartModifierData *)md;

  walk(user_data, ob, (ID **)&lmd->target_material, IDWALK_CB_USER);
  walk(user_data, ob, (ID **)&lmd->source_collection, IDWALK_CB_NOP);

  walk(user_data, ob, (ID **)&lmd->source_object, IDWALK_CB_NOP);
  walk(user_data, ob, (ID **)&lmd->source_camera, IDWALK_CB_NOP);
  walk(user_data, ob, (ID **)&lmd->light_contour_object, IDWALK_CB_NOP);
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  PointerRNA obj_data_ptr = RNA_pointer_get(&ob_ptr, "data");

  const int source_type = RNA_enum_get(ptr, "source_type");
  const bool is_baked = RNA_boolean_get(ptr, "is_baked");

  layout->use_property_split_set(true);
  layout->enabled_set(!is_baked);

  if (!is_first_lineart(*static_cast<const GreasePencilLineartModifierData *>(ptr->data))) {
    layout->prop(ptr, "use_cache", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  layout->prop(ptr, "source_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (source_type == LINEART_SOURCE_OBJECT) {
    layout->prop(ptr, "source_object", UI_ITEM_NONE, std::nullopt, ICON_OBJECT_DATA);
  }
  else if (source_type == LINEART_SOURCE_COLLECTION) {
    uiLayout *sub = &layout->row(true);
    sub->prop(ptr, "source_collection", UI_ITEM_NONE, std::nullopt, ICON_OUTLINER_COLLECTION);
    sub->prop(ptr, "use_invert_collection", UI_ITEM_NONE, "", ICON_ARROW_LEFTRIGHT);
  }
  else {
    /* Source is Scene. */
  }

  uiLayout *col = &layout->column(false);
  col->prop_search(
      ptr, "target_layer", &obj_data_ptr, "layers", std::nullopt, ICON_OUTLINER_DATA_GP_LAYER);
  col->prop_search(
      ptr, "target_material", &obj_data_ptr, "materials", std::nullopt, ICON_MATERIAL);

  col = &layout->column(false);
  col->prop(ptr, "radius", UI_ITEM_R_SLIDER, IFACE_("Line Radius"), ICON_NONE);
  col->prop(ptr, "opacity", UI_ITEM_R_SLIDER, std::nullopt, ICON_NONE);

  modifier_error_message_draw(layout, ptr);
}

static void edge_types_panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");
  const bool use_cache = RNA_boolean_get(ptr, "use_cache");
  const bool is_first = is_first_lineart(
      *static_cast<const GreasePencilLineartModifierData *>(ptr->data));
  const bool has_light = RNA_pointer_get(ptr, "light_contour_object").data != nullptr;

  layout->enabled_set(!is_baked);

  layout->use_property_split_set(true);

  uiLayout *sub = &layout->row(false);
  sub->active_set(has_light);
  sub->prop(
      ptr, "shadow_region_filtering", UI_ITEM_NONE, IFACE_("Illumination Filtering"), ICON_NONE);

  uiLayout *col = &layout->column(true);

  sub = &col->row(false, IFACE_("Create"));
  sub->prop(ptr, "use_contour", UI_ITEM_NONE, "", ICON_NONE);

  uiLayout *entry = &sub->row(true);
  entry->active_set(RNA_boolean_get(ptr, "use_contour"));
  entry->prop(ptr, "silhouette_filtering", UI_ITEM_NONE, "", ICON_NONE);

  const int silhouette_filtering = RNA_enum_get(ptr, "silhouette_filtering");
  if (silhouette_filtering != LINEART_SILHOUETTE_FILTER_NONE) {
    entry->prop(ptr, "use_invert_silhouette", UI_ITEM_NONE, "", ICON_ARROW_LEFTRIGHT);
  }

  sub = &col->row(false);
  if (use_cache && !is_first) {
    sub->prop(ptr, "use_crease", UI_ITEM_NONE, IFACE_("Crease (Angle Cached)"), ICON_NONE);
  }
  else {
    sub->prop(ptr, "use_crease", UI_ITEM_NONE, "", ICON_NONE);
    sub->prop(ptr,
              "crease_threshold",
              UI_ITEM_R_SLIDER | UI_ITEM_R_FORCE_BLANK_DECORATE,
              std::nullopt,
              ICON_NONE);
  }

  col->prop(ptr, "use_intersection", UI_ITEM_NONE, IFACE_("Intersections"), ICON_NONE);
  col->prop(ptr, "use_material", UI_ITEM_NONE, IFACE_("Material Borders"), ICON_NONE);
  col->prop(ptr, "use_edge_mark", UI_ITEM_NONE, IFACE_("Edge Marks"), ICON_NONE);
  col->prop(ptr, "use_loose", UI_ITEM_NONE, IFACE_("Loose"), ICON_NONE);

  entry = &col->column(false);
  entry->active_set(has_light);

  sub = &entry->row(false);
  sub->prop(ptr, "use_light_contour", UI_ITEM_NONE, IFACE_("Light Contour"), ICON_NONE);

  entry->prop(ptr,
              "use_shadow",
              UI_ITEM_NONE,
              CTX_IFACE_(BLT_I18NCONTEXT_ID_GPENCIL, "Cast Shadow"),
              ICON_NONE);

  layout->label(IFACE_("Options"), ICON_NONE);

  sub = &layout->column(false);
  if (use_cache && !is_first) {
    sub->label(IFACE_("Type overlapping cached"), ICON_INFO);
  }
  else {
    sub->prop(ptr,
              "use_overlap_edge_type_support",
              UI_ITEM_NONE,
              IFACE_("Allow Overlapping Types"),
              ICON_NONE);
  }
}

static void options_light_reference_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");
  const bool use_cache = RNA_boolean_get(ptr, "use_cache");
  const bool has_light = RNA_pointer_get(ptr, "light_contour_object").data != nullptr;
  const bool is_first = is_first_lineart(
      *static_cast<const GreasePencilLineartModifierData *>(ptr->data));

  layout->use_property_split_set(true);
  layout->enabled_set(!is_baked);

  if (use_cache && !is_first) {
    layout->label(RPT_("Cached from the first Line Art modifier."), ICON_INFO);
    return;
  }

  layout->prop(ptr, "light_contour_object", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  uiLayout *remaining = &layout->column(false);
  remaining->active_set(has_light);

  remaining->prop(ptr, "shadow_camera_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  uiLayout *col = &remaining->column(true);
  col->prop(ptr, "shadow_camera_near", UI_ITEM_NONE, IFACE_("Near"), ICON_NONE);
  col->prop(ptr, "shadow_camera_far", UI_ITEM_NONE, IFACE_("Far"), ICON_NONE);
}

static void options_panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");
  const bool use_cache = RNA_boolean_get(ptr, "use_cache");
  const bool is_first = is_first_lineart(
      *static_cast<const GreasePencilLineartModifierData *>(ptr->data));

  layout->use_property_split_set(true);
  layout->enabled_set(!is_baked);

  if (use_cache && !is_first) {
    layout->label(TIP_("Cached from the first Line Art modifier"), ICON_INFO);
    return;
  }

  uiLayout *row = &layout->row(false, IFACE_("Custom Camera"));
  row->prop(ptr, "use_custom_camera", UI_ITEM_NONE, "", ICON_NONE);
  uiLayout *subrow = &row->row(true);
  subrow->active_set(RNA_boolean_get(ptr, "use_custom_camera"));
  subrow->use_property_split_set(true);
  subrow->prop(ptr, "source_camera", UI_ITEM_NONE, "", ICON_OBJECT_DATA);

  uiLayout *col = &layout->column(true);

  col->prop(
      ptr, "use_edge_overlap", UI_ITEM_NONE, IFACE_("Overlapping Edges As Contour"), ICON_NONE);
  col->prop(ptr, "use_object_instances", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "use_clip_plane_boundaries", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "use_crease_on_smooth", UI_ITEM_NONE, IFACE_("Crease On Smooth"), ICON_NONE);
  col->prop(ptr, "use_crease_on_sharp", UI_ITEM_NONE, IFACE_("Crease On Sharp"), ICON_NONE);
  col->prop(
      ptr, "use_back_face_culling", UI_ITEM_NONE, IFACE_("Force Backface Culling"), ICON_NONE);
}

static void occlusion_panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");

  const bool use_multiple_levels = RNA_boolean_get(ptr, "use_multiple_levels");
  const bool show_in_front = RNA_boolean_get(&ob_ptr, "show_in_front");

  layout->use_property_split_set(true);
  layout->enabled_set(!is_baked);

  if (!show_in_front) {
    layout->label(TIP_("Object is not in front"), ICON_INFO);
  }

  layout = &layout->column(false);
  layout->active_set(show_in_front);

  layout->prop(ptr, "use_multiple_levels", UI_ITEM_NONE, IFACE_("Range"), ICON_NONE);

  if (use_multiple_levels) {
    uiLayout *col = &layout->column(true);
    col->prop(ptr, "level_start", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "level_end", UI_ITEM_NONE, IFACE_("End"), ICON_NONE);
  }
  else {
    layout->prop(ptr, "level_start", UI_ITEM_NONE, IFACE_("Level"), ICON_NONE);
  }
}

static bool anything_showing_through(PointerRNA *ptr)
{
  const bool use_multiple_levels = RNA_boolean_get(ptr, "use_multiple_levels");
  const int level_start = RNA_int_get(ptr, "level_start");
  const int level_end = RNA_int_get(ptr, "level_end");
  if (use_multiple_levels) {
    return std::max(level_start, level_end) > 0;
  }
  return level_start > 0;
}

static void material_mask_panel_draw_header(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");
  const bool show_in_front = RNA_boolean_get(&ob_ptr, "show_in_front");

  layout->enabled_set(!is_baked);
  layout->active_set(show_in_front && anything_showing_through(ptr));

  layout->prop(ptr, "use_material_mask", UI_ITEM_NONE, IFACE_("Material Mask"), ICON_NONE);
}

static void material_mask_panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");
  layout->enabled_set(!is_baked);
  layout->active_set(anything_showing_through(ptr));

  layout->use_property_split_set(true);

  layout->enabled_set(RNA_boolean_get(ptr, "use_material_mask"));

  uiLayout *col = &layout->column(true);
  uiLayout *sub = &col->row(true, IFACE_("Masks"));

  PropertyRNA *prop = RNA_struct_find_property(ptr, "use_material_mask_bits");
  for (int i = 0; i < 8; i++) {
    sub->prop(ptr, prop, i, 0, UI_ITEM_R_TOGGLE, " ", ICON_NONE);
    if (i == 3) {
      sub = &col->row(true);
    }
  }

  layout->prop(ptr, "use_material_mask_match", UI_ITEM_NONE, IFACE_("Exact Match"), ICON_NONE);
}

static void intersection_panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");
  layout->enabled_set(!is_baked);

  layout->use_property_split_set(true);

  layout->active_set(RNA_boolean_get(ptr, "use_intersection"));

  uiLayout *col = &layout->column(true);
  uiLayout *sub = &col->row(true, IFACE_("Collection Masks"));

  PropertyRNA *prop = RNA_struct_find_property(ptr, "use_intersection_mask");
  for (int i = 0; i < 8; i++) {
    sub->prop(ptr, prop, i, 0, UI_ITEM_R_TOGGLE, " ", ICON_NONE);
    if (i == 3) {
      sub = &col->row(true);
    }
  }

  layout->prop(ptr, "use_intersection_match", UI_ITEM_NONE, IFACE_("Exact Match"), ICON_NONE);
}

static void face_mark_panel_draw_header(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");
  const bool use_cache = RNA_boolean_get(ptr, "use_cache");
  const bool is_first = is_first_lineart(
      *static_cast<const GreasePencilLineartModifierData *>(ptr->data));

  if (!use_cache || is_first) {
    layout->enabled_set(!is_baked);
    layout->prop(ptr, "use_face_mark", UI_ITEM_NONE, IFACE_("Face Mark Filtering"), ICON_NONE);
  }
  else {
    layout->label(IFACE_("Face Mark Filtering"), ICON_NONE);
  }
}

static void face_mark_panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");
  const bool use_mark = RNA_boolean_get(ptr, "use_face_mark");
  const bool use_cache = RNA_boolean_get(ptr, "use_cache");
  const bool is_first = is_first_lineart(
      *static_cast<const GreasePencilLineartModifierData *>(ptr->data));

  layout->enabled_set(!is_baked);

  if (use_cache && !is_first) {
    layout->label(TIP_("Cached from the first Line Art modifier"), ICON_INFO);
    return;
  }

  layout->use_property_split_set(true);

  layout->active_set(use_mark);

  layout->prop(ptr, "use_face_mark_invert", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout->prop(ptr, "use_face_mark_boundaries", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout->prop(ptr, "use_face_mark_keep_contour", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void chaining_panel_draw(const bContext * /*C*/, Panel *panel)
{
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayout *layout = panel->layout;

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");
  const bool use_cache = RNA_boolean_get(ptr, "use_cache");
  const bool is_first = is_first_lineart(
      *static_cast<const GreasePencilLineartModifierData *>(ptr->data));
  const bool is_geom = RNA_boolean_get(ptr, "use_geometry_space_chain");

  layout->use_property_split_set(true);
  layout->enabled_set(!is_baked);

  if (use_cache && !is_first) {
    layout->label(TIP_("Cached from the first Line Art modifier"), ICON_INFO);
    return;
  }

  uiLayout *col = &layout->column(true, IFACE_("Chain"));
  col->prop(ptr, "use_fuzzy_intersections", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "use_fuzzy_all", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "use_loose_edge_chain", UI_ITEM_NONE, IFACE_("Loose Edges"), ICON_NONE);
  col->prop(
      ptr, "use_loose_as_contour", UI_ITEM_NONE, IFACE_("Loose Edges As Contour"), ICON_NONE);
  col->prop(ptr, "use_detail_preserve", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "use_geometry_space_chain", UI_ITEM_NONE, IFACE_("Geometry Space"), ICON_NONE);

  layout->prop(ptr,
               "chaining_image_threshold",
               UI_ITEM_NONE,
               is_geom ? std::make_optional<StringRefNull>(IFACE_("Geometry Threshold")) :
                         std::nullopt,
               ICON_NONE);

  layout->prop(ptr, "smooth_tolerance", UI_ITEM_R_SLIDER, std::nullopt, ICON_NONE);
  layout->prop(ptr, "split_angle", UI_ITEM_R_SLIDER, std::nullopt, ICON_NONE);
}

static void vgroup_panel_draw(const bContext * /*C*/, Panel *panel)
{
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayout *layout = panel->layout;

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");
  const bool use_cache = RNA_boolean_get(ptr, "use_cache");
  const bool is_first = is_first_lineart(
      *static_cast<const GreasePencilLineartModifierData *>(ptr->data));

  layout->use_property_split_set(true);
  layout->enabled_set(!is_baked);

  if (use_cache && !is_first) {
    layout->label(TIP_("Cached from the first Line Art modifier"), ICON_INFO);
    return;
  }

  uiLayout *col = &layout->column(true);

  uiLayout *row = &col->row(true);

  row->prop(ptr, "source_vertex_group", UI_ITEM_NONE, IFACE_("Filter Source"), ICON_GROUP_VERTEX);
  row->prop(ptr, "invert_source_vertex_group", UI_ITEM_R_TOGGLE, "", ICON_ARROW_LEFTRIGHT);

  col->prop(ptr, "use_output_vertex_group_match_by_name", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  col->prop_search(ptr, "vertex_group", &ob_ptr, "vertex_groups", IFACE_("Target"), ICON_NONE);
}

static void bake_panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  const bool is_baked = RNA_boolean_get(ptr, "is_baked");

  layout->use_property_split_set(true);

  if (is_baked) {
    uiLayout *col = &layout->column(false);
    col->use_property_split_set(false);
    col->label(TIP_("Modifier has baked data"), ICON_NONE);
    col->prop(ptr, "is_baked", UI_ITEM_R_TOGGLE, IFACE_("Continue Without Clearing"), ICON_NONE);
  }

  uiLayout *col = &layout->column(false);
  col->enabled_set(!is_baked);
  col->op("OBJECT_OT_lineart_bake_strokes", std::nullopt, ICON_NONE);
  PointerRNA op_ptr = col->op("OBJECT_OT_lineart_bake_strokes", IFACE_("Bake All"), ICON_NONE);
  RNA_boolean_set(&op_ptr, "bake_all", true);

  col = &layout->column(false);
  col->op("OBJECT_OT_lineart_clear", std::nullopt, ICON_NONE);
  op_ptr = col->op("OBJECT_OT_lineart_clear", IFACE_("Clear All"), ICON_NONE);
  RNA_boolean_set(&op_ptr, "clear_all", true);
}

static void composition_panel_draw(const bContext * /*C*/, Panel *panel)
{
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayout *layout = panel->layout;

  const bool show_in_front = RNA_boolean_get(&ob_ptr, "show_in_front");

  layout->use_property_split_set(true);

  layout->prop(ptr, "overscan", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout->prop(ptr, "use_image_boundary_trimming", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (show_in_front) {
    layout->label(TIP_("Object is shown in front"), ICON_ERROR);
  }

  uiLayout *col = &layout->column(false);
  col->active_set(!show_in_front);

  col->prop(ptr, "stroke_depth_offset", UI_ITEM_R_SLIDER, IFACE_("Depth Offset"), ICON_NONE);
  col->prop(ptr,
            "use_offset_towards_custom_camera",
            UI_ITEM_NONE,
            IFACE_("Towards Custom Camera"),
            ICON_NONE);
}

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(
      region_type, eModifierType_GreasePencilLineart, panel_draw);

  modifier_subpanel_register(
      region_type, "edge_types", "Edge Types", nullptr, edge_types_panel_draw, panel_type);
  modifier_subpanel_register(region_type,
                             "light_reference",
                             "Light Reference",
                             nullptr,
                             options_light_reference_draw,
                             panel_type);
  modifier_subpanel_register(
      region_type, "geometry", "Geometry Processing", nullptr, options_panel_draw, panel_type);
  PanelType *occlusion_panel = modifier_subpanel_register(
      region_type, "occlusion", "Occlusion", nullptr, occlusion_panel_draw, panel_type);
  modifier_subpanel_register(region_type,
                             "material_mask",
                             "",
                             material_mask_panel_draw_header,
                             material_mask_panel_draw,
                             occlusion_panel);
  modifier_subpanel_register(
      region_type, "intersection", "Intersection", nullptr, intersection_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "face_mark", "", face_mark_panel_draw_header, face_mark_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "chaining", "Chaining", nullptr, chaining_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "vgroup", "Vertex Weight Transfer", nullptr, vgroup_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "composition", "Composition", nullptr, composition_panel_draw, panel_type);
  modifier_subpanel_register(region_type, "bake", "Bake", nullptr, bake_panel_draw, panel_type);
}

static void generate_strokes(ModifierData &md,
                             const ModifierEvalContext &ctx,
                             GreasePencil &grease_pencil,
                             GreasePencilLineartModifierData &first_lineart,
                             const bool force_compute)
{
  using namespace bke::greasepencil;
  auto &lmd = reinterpret_cast<GreasePencilLineartModifierData &>(md);

  TreeNode *node = grease_pencil.find_node_by_name(lmd.target_layer);
  if (!node || !node->is_layer()) {
    return;
  }

  const bool is_first_lineart = (&first_lineart == &lmd);
  const bool use_cache = (lmd.flags & MOD_LINEART_USE_CACHE);
  LineartCache *local_lc = (is_first_lineart || use_cache) ? first_lineart.shared_cache : nullptr;

  /* Only calculate strokes in these three conditions:
   * 1. It's the very first line art modifier in the stack.
   * 2. This line art modifier doesn't want to use globally cached data.
   * 3. This modifier is not the first line art in stack, but it's the first that's visible (so we
   *    need to do a `force_compute`). */
  if (is_first_lineart || (!use_cache) || force_compute) {
    MOD_lineart_compute_feature_lines_v3(
        ctx.depsgraph, lmd, &local_lc, !(ctx.object->dtx & OB_DRAW_IN_FRONT));
    MOD_lineart_destroy_render_data_v3(&lmd);
  }
  MOD_lineart_chain_clear_picked_flag(local_lc);
  lmd.cache = local_lc;

  const int current_frame = grease_pencil.runtime->eval_frame;

  /* Ensure we have a frame in the selected layer to put line art result in. */
  Layer &layer = node->as_layer();

  const float4x4 &mat = ctx.object->world_to_object();

  /* `drawing` can be nullptr if current frame is before any of the key frames, in which case no
   * strokes are generated. We still allow cache operations to run at the end of this function
   * because there might be other line art modifiers in the same stack. */
  Drawing *drawing = [&]() -> Drawing * {
    if (Drawing *drawing = grease_pencil.get_drawing_at(layer, current_frame)) {
      return drawing;
    }
    return grease_pencil.insert_frame(layer, current_frame);
  }();

  if (drawing) {
    MOD_lineart_gpencil_generate_v3(
        lmd.cache,
        mat,
        ctx.depsgraph,
        *drawing,
        lmd.source_type,
        lmd.source_object,
        lmd.source_collection,
        lmd.level_start,
        lmd.use_multiple_levels ? lmd.level_end : lmd.level_start,
        lmd.target_material ? BKE_object_material_index_get(ctx.object, lmd.target_material) : 0,
        lmd.edge_types,
        lmd.mask_switches,
        lmd.material_mask_bits,
        lmd.intersection_mask,
        lmd.radius,
        lmd.opacity,
        lmd.shadow_selection,
        lmd.silhouette_selection,
        lmd.source_vertex_group,
        lmd.vgname,
        lmd.flags,
        lmd.calculation_flags);
  }

  if ((!is_first_lineart) && (!use_cache)) {
    /* We only clear local cache, not global cache from the first line art modifier. */
    BLI_assert(local_lc != first_lineart.shared_cache);
    MOD_lineart_clear_cache(&local_lc);
    /* Restore the original cache pointer so the modifiers below still have access to the "global"
     * cache. */
    lmd.cache = first_lineart.shared_cache;
  }
}

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  if (!geometry_set->has_grease_pencil()) {
    return;
  }
  GreasePencil &grease_pencil = *geometry_set->get_grease_pencil_for_write();
  auto *mmd = reinterpret_cast<GreasePencilLineartModifierData *>(md);

  GreasePencilLineartModifierData *first_lineart =
      blender::ed::greasepencil::get_first_lineart_modifier(*ctx->object);
  BLI_assert(first_lineart);

  /* Since settings for line art cached data are always in the first line art modifier, we need to
   * get and set overall calculation limits on the first modifier regardless of its visibility
   * state. If line art cache doesn't exist, it means line art hasn't done any calculation. */
  const bool cache_ready = (first_lineart->shared_cache != nullptr);
  if (!cache_ready) {
    first_lineart->shared_cache = MOD_lineart_init_cache();
    ed::greasepencil::get_lineart_modifier_limits(*ctx->object,
                                                  first_lineart->shared_cache->LimitInfo);
  }
  ed::greasepencil::set_lineart_modifier_limits(
      *mmd, first_lineart->shared_cache->LimitInfo, cache_ready);

  generate_strokes(*md, *ctx, grease_pencil, *first_lineart, (!cache_ready));

  const bool use_render_params = (ctx->flag & MOD_APPLY_RENDER);
  if (is_last_line_art(*mmd, use_render_params)) {
    MOD_lineart_clear_cache(&first_lineart->shared_cache);
  }

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const auto *lmd = reinterpret_cast<const GreasePencilLineartModifierData *>(md);

  BLO_write_struct(writer, GreasePencilLineartModifierData, lmd);
}

static void blend_read(BlendDataReader * /*reader*/, ModifierData *md)
{
  GreasePencilLineartModifierData *lmd = reinterpret_cast<GreasePencilLineartModifierData *>(md);
  lmd->runtime = MEM_new<LineartModifierRuntime>(__func__);
}

}  // namespace blender

ModifierTypeInfo modifierType_GreasePencilLineart = {
    /*idname*/ "Lineart Modifier",
    /*name*/ N_("Lineart"),
    /*struct_name*/ "GreasePencilLineartModifierData",
    /*struct_size*/ sizeof(GreasePencilLineartModifierData),
    /*srna*/ &RNA_GreasePencilLineartModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsGreasePencil,
    /*icon*/ ICON_MOD_LINEART,

    /*copy_data*/ blender::copy_data,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ blender::modify_geometry_set,

    /*init_data*/ blender::init_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ blender::free_data,
    /*is_disabled*/ blender::is_disabled,
    /*update_depsgraph*/ blender::update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ blender::foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ blender::panel_register,
    /*blend_write*/ blender::blend_write,
    /*blend_read*/ blender::blend_read,
};
