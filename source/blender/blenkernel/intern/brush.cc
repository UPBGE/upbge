/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_defaults.h"
#include "DNA_gpencil_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rand.h"

#include "BLT_translation.h"

#include "BKE_bpath.h"
#include "BKE_brush.h"
#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_icons.h"
#include "BKE_idtype.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_lib_remap.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_paint.h"
#include "BKE_texture.h"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "RE_texture.h" /* RE_texture_evaluate */

#include "BLO_read_write.h"

static void brush_init_data(ID *id)
{
  Brush *brush = (Brush *)id;
  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(brush, id));

  MEMCPY_STRUCT_AFTER(brush, DNA_struct_default_get(Brush), id);

  /* enable fake user by default */
  id_fake_user_set(&brush->id);

  /* the default alpha falloff curve */
  BKE_brush_curve_preset(brush, CURVE_PRESET_SMOOTH);
}

static void brush_copy_data(Main *UNUSED(bmain), ID *id_dst, const ID *id_src, const int flag)
{
  Brush *brush_dst = (Brush *)id_dst;
  const Brush *brush_src = (const Brush *)id_src;
  if (brush_src->icon_imbuf) {
    brush_dst->icon_imbuf = IMB_dupImBuf(brush_src->icon_imbuf);
  }

  if ((flag & LIB_ID_COPY_NO_PREVIEW) == 0) {
    BKE_previewimg_id_copy(&brush_dst->id, &brush_src->id);
  }
  else {
    brush_dst->preview = nullptr;
  }

  brush_dst->curve = BKE_curvemapping_copy(brush_src->curve);
  if (brush_src->gpencil_settings != nullptr) {
    brush_dst->gpencil_settings = MEM_cnew(__func__, *(brush_src->gpencil_settings));
    brush_dst->gpencil_settings->curve_sensitivity = BKE_curvemapping_copy(
        brush_src->gpencil_settings->curve_sensitivity);
    brush_dst->gpencil_settings->curve_strength = BKE_curvemapping_copy(
        brush_src->gpencil_settings->curve_strength);
    brush_dst->gpencil_settings->curve_jitter = BKE_curvemapping_copy(
        brush_src->gpencil_settings->curve_jitter);

    brush_dst->gpencil_settings->curve_rand_pressure = BKE_curvemapping_copy(
        brush_src->gpencil_settings->curve_rand_pressure);
    brush_dst->gpencil_settings->curve_rand_strength = BKE_curvemapping_copy(
        brush_src->gpencil_settings->curve_rand_strength);
    brush_dst->gpencil_settings->curve_rand_uv = BKE_curvemapping_copy(
        brush_src->gpencil_settings->curve_rand_uv);
    brush_dst->gpencil_settings->curve_rand_hue = BKE_curvemapping_copy(
        brush_src->gpencil_settings->curve_rand_hue);
    brush_dst->gpencil_settings->curve_rand_saturation = BKE_curvemapping_copy(
        brush_src->gpencil_settings->curve_rand_saturation);
    brush_dst->gpencil_settings->curve_rand_value = BKE_curvemapping_copy(
        brush_src->gpencil_settings->curve_rand_value);
  }
  if (brush_src->curves_sculpt_settings != nullptr) {
    brush_dst->curves_sculpt_settings = MEM_cnew(__func__, *(brush_src->curves_sculpt_settings));
  }

  /* enable fake user by default */
  id_fake_user_set(&brush_dst->id);
}

static void brush_free_data(ID *id)
{
  Brush *brush = (Brush *)id;
  if (brush->icon_imbuf) {
    IMB_freeImBuf(brush->icon_imbuf);
  }
  BKE_curvemapping_free(brush->curve);

  if (brush->gpencil_settings != nullptr) {
    BKE_curvemapping_free(brush->gpencil_settings->curve_sensitivity);
    BKE_curvemapping_free(brush->gpencil_settings->curve_strength);
    BKE_curvemapping_free(brush->gpencil_settings->curve_jitter);

    BKE_curvemapping_free(brush->gpencil_settings->curve_rand_pressure);
    BKE_curvemapping_free(brush->gpencil_settings->curve_rand_strength);
    BKE_curvemapping_free(brush->gpencil_settings->curve_rand_uv);
    BKE_curvemapping_free(brush->gpencil_settings->curve_rand_hue);
    BKE_curvemapping_free(brush->gpencil_settings->curve_rand_saturation);
    BKE_curvemapping_free(brush->gpencil_settings->curve_rand_value);

    MEM_SAFE_FREE(brush->gpencil_settings);
  }
  if (brush->curves_sculpt_settings != nullptr) {
    MEM_freeN(brush->curves_sculpt_settings);
  }

  MEM_SAFE_FREE(brush->gradient);

  BKE_previewimg_free(&(brush->preview));
}

static void brush_make_local(Main *bmain, ID *id, const int flags)
{
  if (!ID_IS_LINKED(id)) {
    return;
  }

  Brush *brush = (Brush *)id;
  const bool lib_local = (flags & LIB_ID_MAKELOCAL_FULL_LIBRARY) != 0;

  bool force_local, force_copy;
  BKE_lib_id_make_local_generic_action_define(bmain, id, flags, &force_local, &force_copy);

  if (brush->clone.image) {
    /* Special case: ima always local immediately. Clone image should only have one user anyway. */
    /* FIXME: Recursive calls affecting other non-embedded IDs are really bad and should be avoided
     * in IDType callbacks. Higher-level ID management code usually does not expect such things and
     * does not deal properly with it. */
    /* NOTE: assert below ensures that the comment above is valid, and that exception is
     * acceptable for the time being. */
    BKE_lib_id_make_local(bmain, &brush->clone.image->id, 0);
    BLI_assert(!ID_IS_LINKED(brush->clone.image) && brush->clone.image->id.newid == nullptr);
  }

  if (force_local) {
    BKE_lib_id_clear_library_data(bmain, &brush->id, flags);
    BKE_lib_id_expand_local(bmain, &brush->id, flags);

    /* enable fake user by default */
    id_fake_user_set(&brush->id);
  }
  else if (force_copy) {
    Brush *brush_new = (Brush *)BKE_id_copy(bmain, &brush->id); /* Ensures FAKE_USER is set */

    brush_new->id.us = 0;

    /* setting newid is mandatory for complex make_lib_local logic... */
    ID_NEW_SET(brush, brush_new);

    if (!lib_local) {
      BKE_libblock_remap(bmain, brush, brush_new, ID_REMAP_SKIP_INDIRECT_USAGE);
    }
  }
}

static void brush_foreach_id(ID *id, LibraryForeachIDData *data)
{
  Brush *brush = (Brush *)id;

  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, brush->toggle_brush, IDWALK_CB_NOP);
  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, brush->clone.image, IDWALK_CB_NOP);
  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, brush->paint_curve, IDWALK_CB_USER);
  if (brush->gpencil_settings) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, brush->gpencil_settings->material, IDWALK_CB_USER);
  }
  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(data, BKE_texture_mtex_foreach_id(data, &brush->mtex));
  BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(data,
                                          BKE_texture_mtex_foreach_id(data, &brush->mask_mtex));
}

static void brush_foreach_path(ID *id, BPathForeachPathData *bpath_data)
{
  Brush *brush = (Brush *)id;
  if (brush->icon_filepath[0] != '\0') {
    BKE_bpath_foreach_path_fixed_process(bpath_data, brush->icon_filepath);
  }
}

static void brush_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  Brush *brush = (Brush *)id;

  BLO_write_id_struct(writer, Brush, id_address, &brush->id);
  BKE_id_blend_write(writer, &brush->id);

  if (brush->curve) {
    BKE_curvemapping_blend_write(writer, brush->curve);
  }

  if (brush->gpencil_settings) {
    BLO_write_struct(writer, BrushGpencilSettings, brush->gpencil_settings);

    if (brush->gpencil_settings->curve_sensitivity) {
      BKE_curvemapping_blend_write(writer, brush->gpencil_settings->curve_sensitivity);
    }
    if (brush->gpencil_settings->curve_strength) {
      BKE_curvemapping_blend_write(writer, brush->gpencil_settings->curve_strength);
    }
    if (brush->gpencil_settings->curve_jitter) {
      BKE_curvemapping_blend_write(writer, brush->gpencil_settings->curve_jitter);
    }
    if (brush->gpencil_settings->curve_rand_pressure) {
      BKE_curvemapping_blend_write(writer, brush->gpencil_settings->curve_rand_pressure);
    }
    if (brush->gpencil_settings->curve_rand_strength) {
      BKE_curvemapping_blend_write(writer, brush->gpencil_settings->curve_rand_strength);
    }
    if (brush->gpencil_settings->curve_rand_uv) {
      BKE_curvemapping_blend_write(writer, brush->gpencil_settings->curve_rand_uv);
    }
    if (brush->gpencil_settings->curve_rand_hue) {
      BKE_curvemapping_blend_write(writer, brush->gpencil_settings->curve_rand_hue);
    }
    if (brush->gpencil_settings->curve_rand_saturation) {
      BKE_curvemapping_blend_write(writer, brush->gpencil_settings->curve_rand_saturation);
    }
    if (brush->gpencil_settings->curve_rand_value) {
      BKE_curvemapping_blend_write(writer, brush->gpencil_settings->curve_rand_value);
    }
  }
  if (brush->curves_sculpt_settings) {
    BLO_write_struct(writer, BrushCurvesSculptSettings, brush->curves_sculpt_settings);
  }
  if (brush->gradient) {
    BLO_write_struct(writer, ColorBand, brush->gradient);
  }
}

static void brush_blend_read_data(BlendDataReader *reader, ID *id)
{
  Brush *brush = (Brush *)id;

  /* Falloff curve. */
  BLO_read_data_address(reader, &brush->curve);

  BLO_read_data_address(reader, &brush->gradient);

  if (brush->curve) {
    BKE_curvemapping_blend_read(reader, brush->curve);
  }
  else {
    BKE_brush_curve_preset(brush, CURVE_PRESET_SHARP);
  }

  /* grease pencil */
  BLO_read_data_address(reader, &brush->gpencil_settings);
  if (brush->gpencil_settings != nullptr) {
    BLO_read_data_address(reader, &brush->gpencil_settings->curve_sensitivity);
    BLO_read_data_address(reader, &brush->gpencil_settings->curve_strength);
    BLO_read_data_address(reader, &brush->gpencil_settings->curve_jitter);

    BLO_read_data_address(reader, &brush->gpencil_settings->curve_rand_pressure);
    BLO_read_data_address(reader, &brush->gpencil_settings->curve_rand_strength);
    BLO_read_data_address(reader, &brush->gpencil_settings->curve_rand_uv);
    BLO_read_data_address(reader, &brush->gpencil_settings->curve_rand_hue);
    BLO_read_data_address(reader, &brush->gpencil_settings->curve_rand_saturation);
    BLO_read_data_address(reader, &brush->gpencil_settings->curve_rand_value);

    if (brush->gpencil_settings->curve_sensitivity) {
      BKE_curvemapping_blend_read(reader, brush->gpencil_settings->curve_sensitivity);
    }

    if (brush->gpencil_settings->curve_strength) {
      BKE_curvemapping_blend_read(reader, brush->gpencil_settings->curve_strength);
    }

    if (brush->gpencil_settings->curve_jitter) {
      BKE_curvemapping_blend_read(reader, brush->gpencil_settings->curve_jitter);
    }

    if (brush->gpencil_settings->curve_rand_pressure) {
      BKE_curvemapping_blend_read(reader, brush->gpencil_settings->curve_rand_pressure);
    }

    if (brush->gpencil_settings->curve_rand_strength) {
      BKE_curvemapping_blend_read(reader, brush->gpencil_settings->curve_rand_strength);
    }

    if (brush->gpencil_settings->curve_rand_uv) {
      BKE_curvemapping_blend_read(reader, brush->gpencil_settings->curve_rand_uv);
    }

    if (brush->gpencil_settings->curve_rand_hue) {
      BKE_curvemapping_blend_read(reader, brush->gpencil_settings->curve_rand_hue);
    }

    if (brush->gpencil_settings->curve_rand_saturation) {
      BKE_curvemapping_blend_read(reader, brush->gpencil_settings->curve_rand_saturation);
    }

    if (brush->gpencil_settings->curve_rand_value) {
      BKE_curvemapping_blend_read(reader, brush->gpencil_settings->curve_rand_value);
    }
  }

  BLO_read_data_address(reader, &brush->curves_sculpt_settings);

  brush->preview = nullptr;
  brush->icon_imbuf = nullptr;
}

static void brush_blend_read_lib(BlendLibReader *reader, ID *id)
{
  Brush *brush = (Brush *)id;

  /* brush->(mask_)mtex.obj is ignored on purpose? */
  BLO_read_id_address(reader, brush->id.lib, &brush->mtex.tex);
  BLO_read_id_address(reader, brush->id.lib, &brush->mask_mtex.tex);
  BLO_read_id_address(reader, brush->id.lib, &brush->clone.image);
  BLO_read_id_address(reader, brush->id.lib, &brush->toggle_brush);
  BLO_read_id_address(reader, brush->id.lib, &brush->paint_curve);

  /* link default grease pencil palette */
  if (brush->gpencil_settings != nullptr) {
    if (brush->gpencil_settings->flag & GP_BRUSH_MATERIAL_PINNED) {
      BLO_read_id_address(reader, brush->id.lib, &brush->gpencil_settings->material);

      if (!brush->gpencil_settings->material) {
        brush->gpencil_settings->flag &= ~GP_BRUSH_MATERIAL_PINNED;
      }
    }
    else {
      brush->gpencil_settings->material = nullptr;
    }
  }
}

static void brush_blend_read_expand(BlendExpander *expander, ID *id)
{
  Brush *brush = (Brush *)id;
  BLO_expand(expander, brush->mtex.tex);
  BLO_expand(expander, brush->mask_mtex.tex);
  BLO_expand(expander, brush->clone.image);
  BLO_expand(expander, brush->paint_curve);
  if (brush->gpencil_settings != nullptr) {
    BLO_expand(expander, brush->gpencil_settings->material);
  }
}

static int brush_undo_preserve_cb(LibraryIDLinkCallbackData *cb_data)
{
  BlendLibReader *reader = (BlendLibReader *)cb_data->user_data;
  ID *id_old = *cb_data->id_pointer;
  /* Old data has not been remapped to new values of the pointers, if we want to keep the old
   * pointer here we need its new address. */
  ID *id_old_new = id_old != nullptr ? BLO_read_get_new_id_address(reader, id_old->lib, id_old) :
                                       nullptr;
  BLI_assert(id_old_new == nullptr || ELEM(id_old, id_old_new, id_old_new->orig_id));
  if (cb_data->cb_flag & IDWALK_CB_USER) {
    id_us_plus_no_lib(id_old_new);
    id_us_min(id_old);
  }
  *cb_data->id_pointer = id_old_new;
  return IDWALK_RET_NOP;
}

static void brush_undo_preserve(BlendLibReader *reader, ID *id_new, ID *id_old)
{
  /* Whole Brush is preserved across undo-steps. */
  BKE_lib_id_swap(nullptr, id_new, id_old);

  /* `id_new` now has content from `id_old`, we need to ensure those old ID pointers are valid.
   * NOTE: Since we want to re-use all old pointers here, code is much simpler than for Scene. */
  BKE_library_foreach_ID_link(nullptr, id_new, brush_undo_preserve_cb, reader, IDWALK_NOP);

  /* NOTE: We do not swap IDProperties, as dealing with potential ID pointers in those would be
   *       fairly delicate. */
  SWAP(IDProperty *, id_new->properties, id_old->properties);
}

IDTypeInfo IDType_ID_BR = {
    /* id_code */ ID_BR,
    /* id_filter */ FILTER_ID_BR,
    /* main_listbase_index */ INDEX_ID_BR,
    /* struct_size */ sizeof(Brush),
    /* name */ "Brush",
    /* name_plural */ "brushes",
    /* translation_context */ BLT_I18NCONTEXT_ID_BRUSH,
    /* flags */ IDTYPE_FLAGS_NO_ANIMDATA,
    /* asset_type_info */ nullptr,

    /* init_data */ brush_init_data,
    /* copy_data */ brush_copy_data,
    /* free_data */ brush_free_data,
    /* make_local */ brush_make_local,
    /* foreach_id */ brush_foreach_id,
    /* foreach_cache */ nullptr,
    /* foreach_path */ brush_foreach_path,
    /* owner_get */ nullptr,

    /* blend_write */ brush_blend_write,
    /* blend_read_data */ brush_blend_read_data,
    /* blend_read_lib */ brush_blend_read_lib,
    /* blend_read_expand */ brush_blend_read_expand,

    /* blend_read_undo_preserve */ brush_undo_preserve,

    /* lib_override_apply_post */ nullptr,
};

static RNG *brush_rng;

void BKE_brush_system_init(void)
{
  brush_rng = BLI_rng_new(0);
  BLI_rng_srandom(brush_rng, 31415682);
}

void BKE_brush_system_exit(void)
{
  if (brush_rng == nullptr) {
    return;
  }
  BLI_rng_free(brush_rng);
  brush_rng = nullptr;
}

static void brush_defaults(Brush *brush)
{

  const Brush *brush_def = DNA_struct_default_get(Brush);

#define FROM_DEFAULT(member) \
  memcpy((void *)&brush->member, (void *)&brush_def->member, sizeof(brush->member))
#define FROM_DEFAULT_PTR(member) memcpy(brush->member, brush_def->member, sizeof(brush->member))

  FROM_DEFAULT(blend);
  FROM_DEFAULT(flag);
  FROM_DEFAULT(weight);
  FROM_DEFAULT(size);
  FROM_DEFAULT(alpha);
  FROM_DEFAULT(hardness);
  FROM_DEFAULT(autosmooth_factor);
  FROM_DEFAULT(topology_rake_factor);
  FROM_DEFAULT(crease_pinch_factor);
  FROM_DEFAULT(normal_radius_factor);
  FROM_DEFAULT(wet_paint_radius_factor);
  FROM_DEFAULT(area_radius_factor);
  FROM_DEFAULT(disconnected_distance_max);
  FROM_DEFAULT(sculpt_plane);
  FROM_DEFAULT(plane_offset);
  FROM_DEFAULT(clone.alpha);
  FROM_DEFAULT(normal_weight);
  FROM_DEFAULT(fill_threshold);
  FROM_DEFAULT(flag);
  FROM_DEFAULT(sampling_flag);
  FROM_DEFAULT_PTR(rgb);
  FROM_DEFAULT_PTR(secondary_rgb);
  FROM_DEFAULT(spacing);
  FROM_DEFAULT(smooth_stroke_radius);
  FROM_DEFAULT(smooth_stroke_factor);
  FROM_DEFAULT(rate);
  FROM_DEFAULT(jitter);
  FROM_DEFAULT(texture_sample_bias);
  FROM_DEFAULT(texture_overlay_alpha);
  FROM_DEFAULT(mask_overlay_alpha);
  FROM_DEFAULT(cursor_overlay_alpha);
  FROM_DEFAULT(overlay_flags);
  FROM_DEFAULT_PTR(add_col);
  FROM_DEFAULT_PTR(sub_col);
  FROM_DEFAULT(stencil_pos);
  FROM_DEFAULT(stencil_dimension);
  FROM_DEFAULT(mtex);
  FROM_DEFAULT(mask_mtex);

#undef FROM_DEFAULT
#undef FROM_DEFAULT_PTR
}

/* Datablock add/copy/free/make_local */

Brush *BKE_brush_add(Main *bmain, const char *name, const eObjectMode ob_mode)
{
  Brush *brush = (Brush *)BKE_id_new(bmain, ID_BR, name);

  brush->ob_mode = ob_mode;

  if (ob_mode == OB_MODE_SCULPT_CURVES) {
    BKE_brush_init_curves_sculpt_settings(brush);
  }

  return brush;
}

void BKE_brush_init_gpencil_settings(Brush *brush)
{
  if (brush->gpencil_settings == nullptr) {
    brush->gpencil_settings = MEM_cnew<BrushGpencilSettings>("BrushGpencilSettings");
  }

  brush->gpencil_settings->draw_smoothlvl = 1;
  brush->gpencil_settings->flag = 0;
  brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;
  brush->gpencil_settings->draw_strength = 1.0f;
  brush->gpencil_settings->draw_jitter = 0.0f;
  brush->gpencil_settings->flag |= GP_BRUSH_USE_JITTER_PRESSURE;
  brush->gpencil_settings->icon_id = GP_BRUSH_ICON_PEN;

  /* curves */
  brush->gpencil_settings->curve_sensitivity = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  brush->gpencil_settings->curve_strength = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  brush->gpencil_settings->curve_jitter = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);

  brush->gpencil_settings->curve_rand_pressure = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  brush->gpencil_settings->curve_rand_strength = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  brush->gpencil_settings->curve_rand_uv = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  brush->gpencil_settings->curve_rand_hue = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  brush->gpencil_settings->curve_rand_saturation = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  brush->gpencil_settings->curve_rand_value = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
}

Brush *BKE_brush_add_gpencil(Main *bmain, ToolSettings *ts, const char *name, eObjectMode mode)
{
  Paint *paint = nullptr;
  Brush *brush;
  switch (mode) {
    case OB_MODE_PAINT_GPENCIL: {
      paint = &ts->gp_paint->paint;
      break;
    }
    case OB_MODE_SCULPT_GPENCIL: {
      paint = &ts->gp_sculptpaint->paint;
      break;
    }
    case OB_MODE_WEIGHT_GPENCIL: {
      paint = &ts->gp_weightpaint->paint;
      break;
    }
    case OB_MODE_VERTEX_GPENCIL: {
      paint = &ts->gp_vertexpaint->paint;
      break;
    }
    default:
      paint = &ts->gp_paint->paint;
  }

  brush = BKE_brush_add(bmain, name, mode);

  BKE_paint_brush_set(paint, brush);
  id_us_min(&brush->id);

  brush->size = 3;

  /* grease pencil basic settings */
  BKE_brush_init_gpencil_settings(brush);

  /* return brush */
  return brush;
}

bool BKE_brush_delete(Main *bmain, Brush *brush)
{
  if (brush->id.tag & LIB_TAG_INDIRECT) {
    return false;
  }
  if (ID_REAL_USERS(brush) <= 1 && ID_EXTRA_USERS(brush) == 0 &&
      BKE_library_ID_is_indirectly_used(bmain, brush)) {
    return false;
  }

  BKE_id_delete(bmain, brush);

  return true;
}

/** Local grease pencil curve mapping preset. */
using eGPCurveMappingPreset = enum eGPCurveMappingPreset {
  GPCURVE_PRESET_PENCIL = 0,
  GPCURVE_PRESET_INK = 1,
  GPCURVE_PRESET_INKNOISE = 2,
  GPCURVE_PRESET_MARKER = 3,
  GPCURVE_PRESET_CHISEL_SENSIVITY = 4,
  GPCURVE_PRESET_CHISEL_STRENGTH = 5,
};

static void brush_gpencil_curvemap_reset(CurveMap *cuma, int tot, eGPCurveMappingPreset preset)
{
  if (cuma->curve) {
    MEM_freeN(cuma->curve);
  }

  cuma->totpoint = tot;
  cuma->curve = (CurveMapPoint *)MEM_callocN(cuma->totpoint * sizeof(CurveMapPoint), __func__);

  switch (preset) {
    case GPCURVE_PRESET_PENCIL:
      cuma->curve[0].x = 0.0f;
      cuma->curve[0].y = 0.0f;
      cuma->curve[1].x = 0.75115f;
      cuma->curve[1].y = 0.25f;
      cuma->curve[2].x = 1.0f;
      cuma->curve[2].y = 1.0f;
      break;
    case GPCURVE_PRESET_INK:
      cuma->curve[0].x = 0.0f;
      cuma->curve[0].y = 0.0f;
      cuma->curve[1].x = 0.63448f;
      cuma->curve[1].y = 0.375f;
      cuma->curve[2].x = 1.0f;
      cuma->curve[2].y = 1.0f;
      break;
    case GPCURVE_PRESET_INKNOISE:
      cuma->curve[0].x = 0.0f;
      cuma->curve[0].y = 0.0f;
      cuma->curve[1].x = 0.55f;
      cuma->curve[1].y = 0.45f;
      cuma->curve[2].x = 0.85f;
      cuma->curve[2].y = 1.0f;
      break;
    case GPCURVE_PRESET_MARKER:
      cuma->curve[0].x = 0.0f;
      cuma->curve[0].y = 0.0f;
      cuma->curve[1].x = 0.38f;
      cuma->curve[1].y = 0.22f;
      cuma->curve[2].x = 0.65f;
      cuma->curve[2].y = 0.68f;
      cuma->curve[3].x = 1.0f;
      cuma->curve[3].y = 1.0f;
      break;
    case GPCURVE_PRESET_CHISEL_SENSIVITY:
      cuma->curve[0].x = 0.0f;
      cuma->curve[0].y = 0.0f;
      cuma->curve[1].x = 0.25f;
      cuma->curve[1].y = 0.40f;
      cuma->curve[2].x = 1.0f;
      cuma->curve[2].y = 1.0f;
      break;
    case GPCURVE_PRESET_CHISEL_STRENGTH:
      cuma->curve[0].x = 0.0f;
      cuma->curve[0].y = 0.0f;
      cuma->curve[1].x = 0.31f;
      cuma->curve[1].y = 0.22f;
      cuma->curve[2].x = 0.61f;
      cuma->curve[2].y = 0.88f;
      cuma->curve[3].x = 1.0f;
      cuma->curve[3].y = 1.0f;
      break;
    default:
      break;
  }

  MEM_SAFE_FREE(cuma->table);
}

void BKE_gpencil_brush_preset_set(Main *bmain, Brush *brush, const short type)
{
#define SMOOTH_STROKE_RADIUS 40
#define SMOOTH_STROKE_FACTOR 0.9f
#define ACTIVE_SMOOTH 0.35f

  CurveMapping *custom_curve = nullptr;

  /* Optionally assign a material preset. */
  enum {
    PRESET_MATERIAL_NONE = 0,
    PRESET_MATERIAL_DOT_STROKE,
  } material_preset = PRESET_MATERIAL_NONE;

  /* Set general defaults at brush level. */
  brush->smooth_stroke_radius = SMOOTH_STROKE_RADIUS;
  brush->smooth_stroke_factor = SMOOTH_STROKE_FACTOR;

  brush->rgb[0] = 0.498f;
  brush->rgb[1] = 1.0f;
  brush->rgb[2] = 0.498f;

  brush->secondary_rgb[0] = 1.0f;
  brush->secondary_rgb[1] = 1.0f;
  brush->secondary_rgb[2] = 1.0f;

  brush->curve_preset = BRUSH_CURVE_SMOOTH;

  if (brush->gpencil_settings == nullptr) {
    return;
  }

  /* Set preset type. */
  brush->gpencil_settings->preset_type = type;

  /* Set vertex mix factor. */
  brush->gpencil_settings->vertex_mode = GPPAINT_MODE_BOTH;
  brush->gpencil_settings->vertex_factor = 1.0f;

  switch (type) {
    case GP_BRUSH_PRESET_AIRBRUSH: {
      brush->size = 300.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.4f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;

      brush->gpencil_settings->input_samples = 10;
      brush->gpencil_settings->active_smooth = ACTIVE_SMOOTH;
      brush->gpencil_settings->draw_angle = 0.0f;
      brush->gpencil_settings->draw_angle_factor = 0.0f;
      brush->gpencil_settings->hardeness = 0.9f;
      copy_v2_fl(brush->gpencil_settings->aspect_ratio, 1.0f);

      brush->gpencil_tool = GPAINT_TOOL_DRAW;
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_AIRBRUSH;

      zero_v3(brush->secondary_rgb);

      material_preset = PRESET_MATERIAL_DOT_STROKE;

      break;
    }
    case GP_BRUSH_PRESET_INK_PEN: {

      brush->size = 60.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 1.0f;

      brush->gpencil_settings->input_samples = 10;
      brush->gpencil_settings->active_smooth = ACTIVE_SMOOTH;
      brush->gpencil_settings->draw_angle = 0.0f;
      brush->gpencil_settings->draw_angle_factor = 0.0f;
      brush->gpencil_settings->hardeness = 1.0f;
      copy_v2_fl(brush->gpencil_settings->aspect_ratio, 1.0f);

      brush->gpencil_settings->flag |= GP_BRUSH_GROUP_SETTINGS;
      brush->gpencil_settings->draw_smoothfac = 0.1f;
      brush->gpencil_settings->draw_smoothlvl = 1;
      brush->gpencil_settings->draw_subdivide = 0;
      brush->gpencil_settings->simplify_f = 0.002f;

      brush->gpencil_settings->draw_random_press = 0.0f;
      brush->gpencil_settings->draw_jitter = 0.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

      /* Curve. */
      custom_curve = brush->gpencil_settings->curve_sensitivity;
      BKE_curvemapping_set_defaults(custom_curve, 0, 0.0f, 0.0f, 1.0f, 1.0f);
      BKE_curvemapping_init(custom_curve);
      brush_gpencil_curvemap_reset(custom_curve->cm, 3, GPCURVE_PRESET_INK);

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_INK;
      brush->gpencil_tool = GPAINT_TOOL_DRAW;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_INK_PEN_ROUGH: {
      brush->size = 60.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 1.0f;

      brush->gpencil_settings->input_samples = 10;
      brush->gpencil_settings->active_smooth = ACTIVE_SMOOTH;
      brush->gpencil_settings->draw_angle = 0.0f;
      brush->gpencil_settings->draw_angle_factor = 0.0f;
      brush->gpencil_settings->hardeness = 1.0f;
      copy_v2_fl(brush->gpencil_settings->aspect_ratio, 1.0f);

      brush->gpencil_settings->flag &= ~GP_BRUSH_GROUP_SETTINGS;
      brush->gpencil_settings->draw_smoothfac = 0.0f;
      brush->gpencil_settings->draw_smoothlvl = 2;
      brush->gpencil_settings->draw_subdivide = 0;
      brush->gpencil_settings->simplify_f = 0.000f;

      brush->gpencil_settings->flag |= GP_BRUSH_GROUP_RANDOM;
      brush->gpencil_settings->draw_random_press = 0.6f;
      brush->gpencil_settings->draw_random_strength = 0.0f;
      brush->gpencil_settings->draw_jitter = 0.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

      /* Curve. */
      custom_curve = brush->gpencil_settings->curve_sensitivity;
      BKE_curvemapping_set_defaults(custom_curve, 0, 0.0f, 0.0f, 1.0f, 1.0f);
      BKE_curvemapping_init(custom_curve);
      brush_gpencil_curvemap_reset(custom_curve->cm, 3, GPCURVE_PRESET_INKNOISE);

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_INKNOISE;
      brush->gpencil_tool = GPAINT_TOOL_DRAW;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_MARKER_BOLD: {
      brush->size = 150.0f;
      brush->gpencil_settings->flag &= ~GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.3f;

      brush->gpencil_settings->input_samples = 10;
      brush->gpencil_settings->active_smooth = ACTIVE_SMOOTH;
      brush->gpencil_settings->draw_angle = 0.0f;
      brush->gpencil_settings->draw_angle_factor = 0.0f;
      brush->gpencil_settings->hardeness = 1.0f;
      copy_v2_fl(brush->gpencil_settings->aspect_ratio, 1.0f);

      brush->gpencil_settings->flag |= GP_BRUSH_GROUP_SETTINGS;
      brush->gpencil_settings->draw_smoothfac = 0.1f;
      brush->gpencil_settings->draw_smoothlvl = 1;
      brush->gpencil_settings->draw_subdivide = 0;
      brush->gpencil_settings->simplify_f = 0.002f;

      brush->gpencil_settings->flag &= ~GP_BRUSH_GROUP_RANDOM;
      brush->gpencil_settings->draw_random_press = 0.0f;
      brush->gpencil_settings->draw_random_strength = 0.0f;
      brush->gpencil_settings->draw_jitter = 0.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

      /* Curve. */
      custom_curve = brush->gpencil_settings->curve_sensitivity;
      BKE_curvemapping_set_defaults(custom_curve, 0, 0.0f, 0.0f, 1.0f, 1.0f);
      BKE_curvemapping_init(custom_curve);
      brush_gpencil_curvemap_reset(custom_curve->cm, 4, GPCURVE_PRESET_MARKER);

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_MARKER;
      brush->gpencil_tool = GPAINT_TOOL_DRAW;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_MARKER_CHISEL: {
      brush->size = 150.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 1.0f;

      brush->gpencil_settings->input_samples = 10;
      brush->gpencil_settings->active_smooth = 0.3f;
      brush->gpencil_settings->draw_angle = DEG2RAD(35.0f);
      brush->gpencil_settings->draw_angle_factor = 0.5f;
      brush->gpencil_settings->hardeness = 1.0f;
      copy_v2_fl(brush->gpencil_settings->aspect_ratio, 1.0f);

      brush->gpencil_settings->flag |= GP_BRUSH_GROUP_SETTINGS;
      brush->gpencil_settings->draw_smoothfac = 0.0f;
      brush->gpencil_settings->draw_smoothlvl = 1;
      brush->gpencil_settings->draw_subdivide = 0;
      brush->gpencil_settings->simplify_f = 0.002f;

      brush->gpencil_settings->flag &= ~GP_BRUSH_GROUP_RANDOM;
      brush->gpencil_settings->draw_random_press = 0.0f;
      brush->gpencil_settings->draw_jitter = 0.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

      /* Curve. */
      custom_curve = brush->gpencil_settings->curve_sensitivity;
      BKE_curvemapping_set_defaults(custom_curve, 0, 0.0f, 0.0f, 1.0f, 1.0f);
      BKE_curvemapping_init(custom_curve);
      brush_gpencil_curvemap_reset(custom_curve->cm, 3, GPCURVE_PRESET_CHISEL_SENSIVITY);

      custom_curve = brush->gpencil_settings->curve_strength;
      BKE_curvemapping_set_defaults(custom_curve, 0, 0.0f, 0.0f, 1.0f, 1.0f);
      BKE_curvemapping_init(custom_curve);
      brush_gpencil_curvemap_reset(custom_curve->cm, 4, GPCURVE_PRESET_CHISEL_STRENGTH);

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_CHISEL;
      brush->gpencil_tool = GPAINT_TOOL_DRAW;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_PEN: {
      brush->size = 25.0f;
      brush->gpencil_settings->flag &= ~GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 1.0f;
      brush->gpencil_settings->flag &= ~GP_BRUSH_USE_STRENGTH_PRESSURE;

      brush->gpencil_settings->input_samples = 10;
      brush->gpencil_settings->active_smooth = ACTIVE_SMOOTH;
      brush->gpencil_settings->draw_angle = 0.0f;
      brush->gpencil_settings->draw_angle_factor = 0.0f;
      brush->gpencil_settings->hardeness = 1.0f;
      copy_v2_fl(brush->gpencil_settings->aspect_ratio, 1.0f);

      brush->gpencil_settings->flag |= GP_BRUSH_GROUP_SETTINGS;
      brush->gpencil_settings->draw_smoothfac = 0.0f;
      brush->gpencil_settings->draw_smoothlvl = 1;
      brush->gpencil_settings->draw_subdivide = 1;
      brush->gpencil_settings->simplify_f = 0.002f;

      brush->gpencil_settings->draw_random_press = 0.0f;
      brush->gpencil_settings->draw_random_strength = 0.0f;
      brush->gpencil_settings->draw_jitter = 0.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_PEN;
      brush->gpencil_tool = GPAINT_TOOL_DRAW;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_PENCIL_SOFT: {
      brush->size = 80.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.4f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;

      brush->gpencil_settings->input_samples = 10;
      brush->gpencil_settings->active_smooth = ACTIVE_SMOOTH;
      brush->gpencil_settings->draw_angle = 0.0f;
      brush->gpencil_settings->draw_angle_factor = 0.0f;
      brush->gpencil_settings->hardeness = 0.8f;
      copy_v2_fl(brush->gpencil_settings->aspect_ratio, 1.0f);

      brush->gpencil_settings->flag |= GP_BRUSH_GROUP_SETTINGS;
      brush->gpencil_settings->draw_smoothfac = 0.0f;
      brush->gpencil_settings->draw_smoothlvl = 1;
      brush->gpencil_settings->draw_subdivide = 0;
      brush->gpencil_settings->simplify_f = 0.000f;

      brush->gpencil_settings->draw_random_press = 0.0f;
      brush->gpencil_settings->draw_random_strength = 0.0f;
      brush->gpencil_settings->draw_jitter = 0.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_PENCIL;
      brush->gpencil_tool = GPAINT_TOOL_DRAW;

      zero_v3(brush->secondary_rgb);

      material_preset = PRESET_MATERIAL_DOT_STROKE;

      break;
    }
    case GP_BRUSH_PRESET_PENCIL: {
      brush->size = 20.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.6f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;

      brush->gpencil_settings->input_samples = 10;
      brush->gpencil_settings->active_smooth = ACTIVE_SMOOTH;
      brush->gpencil_settings->draw_angle = 0.0f;
      brush->gpencil_settings->draw_angle_factor = 0.0f;
      brush->gpencil_settings->hardeness = 1.0f;
      copy_v2_fl(brush->gpencil_settings->aspect_ratio, 1.0f);

      brush->gpencil_settings->flag |= GP_BRUSH_GROUP_SETTINGS;
      brush->gpencil_settings->draw_smoothfac = 0.0f;
      brush->gpencil_settings->draw_smoothlvl = 1;
      brush->gpencil_settings->draw_subdivide = 0;
      brush->gpencil_settings->simplify_f = 0.002f;

      brush->gpencil_settings->draw_random_press = 0.0f;
      brush->gpencil_settings->draw_jitter = 0.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_PENCIL;
      brush->gpencil_tool = GPAINT_TOOL_DRAW;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_FILL_AREA: {
      brush->size = 5.0f;

      brush->gpencil_settings->fill_leak = 3;
      brush->gpencil_settings->fill_threshold = 0.1f;
      brush->gpencil_settings->fill_simplylvl = 1;
      brush->gpencil_settings->fill_factor = 1.0f;

      brush->gpencil_settings->draw_strength = 1.0f;
      brush->gpencil_settings->hardeness = 1.0f;
      copy_v2_fl(brush->gpencil_settings->aspect_ratio, 1.0f);
      brush->gpencil_settings->draw_smoothfac = 0.1f;
      brush->gpencil_settings->draw_smoothlvl = 1;
      brush->gpencil_settings->draw_subdivide = 1;
      brush->gpencil_settings->dilate_pixels = 1;

      brush->gpencil_settings->flag |= GP_BRUSH_FILL_SHOW_EXTENDLINES;

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_FILL;
      brush->gpencil_tool = GPAINT_TOOL_FILL;
      brush->gpencil_settings->vertex_mode = GPPAINT_MODE_FILL;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_ERASER_SOFT: {
      brush->size = 30.0f;
      brush->gpencil_settings->draw_strength = 0.5f;
      brush->gpencil_settings->flag |= GP_BRUSH_DEFAULT_ERASER;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_ERASE_SOFT;
      brush->gpencil_tool = GPAINT_TOOL_ERASE;
      brush->gpencil_settings->eraser_mode = GP_BRUSH_ERASER_SOFT;
      brush->gpencil_settings->era_strength_f = 100.0f;
      brush->gpencil_settings->era_thickness_f = 10.0f;

      break;
    }
    case GP_BRUSH_PRESET_ERASER_HARD: {
      brush->size = 30.0f;
      brush->gpencil_settings->draw_strength = 1.0f;
      brush->gpencil_settings->eraser_mode = GP_BRUSH_ERASER_SOFT;
      brush->gpencil_settings->era_strength_f = 100.0f;
      brush->gpencil_settings->era_thickness_f = 50.0f;

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_ERASE_HARD;
      brush->gpencil_tool = GPAINT_TOOL_ERASE;

      break;
    }
    case GP_BRUSH_PRESET_ERASER_POINT: {
      brush->size = 30.0f;
      brush->gpencil_settings->eraser_mode = GP_BRUSH_ERASER_HARD;

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_ERASE_HARD;
      brush->gpencil_tool = GPAINT_TOOL_ERASE;

      break;
    }
    case GP_BRUSH_PRESET_ERASER_STROKE: {
      brush->size = 30.0f;
      brush->gpencil_settings->eraser_mode = GP_BRUSH_ERASER_STROKE;

      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_ERASE_STROKE;
      brush->gpencil_tool = GPAINT_TOOL_ERASE;

      break;
    }
    case GP_BRUSH_PRESET_TINT: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_TINT;
      brush->gpencil_tool = GPAINT_TOOL_TINT;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.8f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_VERTEX_DRAW: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_VERTEX_DRAW;
      brush->gpencil_vertex_tool = GPVERTEX_TOOL_DRAW;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.8f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_VERTEX_BLUR: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_VERTEX_BLUR;
      brush->gpencil_vertex_tool = GPVERTEX_TOOL_BLUR;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.8f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_VERTEX_AVERAGE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_VERTEX_AVERAGE;
      brush->gpencil_vertex_tool = GPVERTEX_TOOL_AVERAGE;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.8f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_VERTEX_SMEAR: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_VERTEX_SMEAR;
      brush->gpencil_vertex_tool = GPVERTEX_TOOL_SMEAR;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.8f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_VERTEX_REPLACE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_VERTEX_REPLACE;
      brush->gpencil_vertex_tool = GPVERTEX_TOOL_REPLACE;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.8f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;

      zero_v3(brush->secondary_rgb);
      break;
    }
    case GP_BRUSH_PRESET_SMOOTH_STROKE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_GPBRUSH_SMOOTH;
      brush->gpencil_sculpt_tool = GPSCULPT_TOOL_SMOOTH;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.3f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->sculpt_flag = GP_SCULPT_FLAGMODE_APPLY_THICKNESS;
      brush->gpencil_settings->sculpt_mode_flag |= GP_SCULPT_FLAGMODE_APPLY_POSITION;

      break;
    }
    case GP_BRUSH_PRESET_STRENGTH_STROKE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_GPBRUSH_STRENGTH;
      brush->gpencil_sculpt_tool = GPSCULPT_TOOL_STRENGTH;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.3f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->sculpt_mode_flag |= GP_SCULPT_FLAGMODE_APPLY_POSITION;

      break;
    }
    case GP_BRUSH_PRESET_THICKNESS_STROKE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_GPBRUSH_THICKNESS;
      brush->gpencil_sculpt_tool = GPSCULPT_TOOL_THICKNESS;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.5f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->sculpt_mode_flag |= GP_SCULPT_FLAGMODE_APPLY_POSITION;

      break;
    }
    case GP_BRUSH_PRESET_GRAB_STROKE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_GPBRUSH_GRAB;
      brush->gpencil_sculpt_tool = GPSCULPT_TOOL_GRAB;
      brush->gpencil_settings->flag &= ~GP_BRUSH_USE_PRESSURE;

      brush->size = 25.0f;

      brush->gpencil_settings->draw_strength = 0.3f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->sculpt_mode_flag |= GP_SCULPT_FLAGMODE_APPLY_POSITION;

      break;
    }
    case GP_BRUSH_PRESET_PUSH_STROKE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_GPBRUSH_PUSH;
      brush->gpencil_sculpt_tool = GPSCULPT_TOOL_PUSH;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.3f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->sculpt_mode_flag |= GP_SCULPT_FLAGMODE_APPLY_POSITION;

      break;
    }
    case GP_BRUSH_PRESET_TWIST_STROKE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_GPBRUSH_TWIST;
      brush->gpencil_sculpt_tool = GPSCULPT_TOOL_TWIST;

      brush->size = 50.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.3f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->sculpt_mode_flag |= GP_SCULPT_FLAGMODE_APPLY_POSITION;

      break;
    }
    case GP_BRUSH_PRESET_PINCH_STROKE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_GPBRUSH_PINCH;
      brush->gpencil_sculpt_tool = GPSCULPT_TOOL_PINCH;

      brush->size = 50.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.5f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->sculpt_mode_flag |= GP_SCULPT_FLAGMODE_APPLY_POSITION;

      break;
    }
    case GP_BRUSH_PRESET_RANDOMIZE_STROKE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_GPBRUSH_RANDOMIZE;
      brush->gpencil_sculpt_tool = GPSCULPT_TOOL_RANDOMIZE;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.5f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->sculpt_mode_flag |= GP_SCULPT_FLAGMODE_APPLY_POSITION;

      break;
    }
    case GP_BRUSH_PRESET_CLONE_STROKE: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_GPBRUSH_CLONE;
      brush->gpencil_sculpt_tool = GPSCULPT_TOOL_CLONE;
      brush->gpencil_settings->flag &= ~GP_BRUSH_USE_PRESSURE;

      brush->size = 25.0f;

      brush->gpencil_settings->draw_strength = 1.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->sculpt_mode_flag |= GP_SCULPT_FLAGMODE_APPLY_POSITION;

      break;
    }
    case GP_BRUSH_PRESET_DRAW_WEIGHT: {
      brush->gpencil_settings->icon_id = GP_BRUSH_ICON_GPBRUSH_WEIGHT;
      brush->gpencil_weight_tool = GPWEIGHT_TOOL_DRAW;

      brush->size = 25.0f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_PRESSURE;

      brush->gpencil_settings->draw_strength = 0.8f;
      brush->gpencil_settings->flag |= GP_BRUSH_USE_STRENGTH_PRESSURE;
      brush->gpencil_settings->sculpt_mode_flag |= GP_SCULPT_FLAGMODE_APPLY_POSITION;

      break;
    }
    default:
      break;
  }

  switch (material_preset) {
    case PRESET_MATERIAL_NONE:
      break;
    case PRESET_MATERIAL_DOT_STROKE: {
      /* Create and link Black Dots material to brush.
       * This material is required because the brush uses the material
       * to define how the stroke is drawn. */
      const char *ma_id = "Dots Stroke";
      Material *ma = (Material *)BLI_findstring(&bmain->materials, ma_id, offsetof(ID, name) + 2);
      if (ma == nullptr) {
        ma = BKE_gpencil_material_add(bmain, ma_id);
        ma->gp_style->mode = GP_MATERIAL_MODE_DOT;
        BLI_assert(ma->id.us == 1);
        id_us_min(&ma->id);
      }

      BKE_gpencil_brush_material_set(brush, ma);

      /* Pin the material to the brush. */
      brush->gpencil_settings->flag |= GP_BRUSH_MATERIAL_PINNED;
      break;
    }
  }
}

static Brush *gpencil_brush_ensure(
    Main *bmain, ToolSettings *ts, const char *brush_name, eObjectMode mode, bool *r_new)
{
  *r_new = false;
  Brush *brush = (Brush *)BLI_findstring(&bmain->brushes, brush_name, offsetof(ID, name) + 2);

  /* If the brush exist, but the type is not GPencil or the mode is wrong, create a new one. */
  if ((brush != nullptr) && ((brush->gpencil_settings == nullptr) || (brush->ob_mode != mode))) {
    brush = nullptr;
  }

  if (brush == nullptr) {
    brush = BKE_brush_add_gpencil(bmain, ts, brush_name, mode);
    *r_new = true;
  }

  if (brush->gpencil_settings == nullptr) {
    BKE_brush_init_gpencil_settings(brush);
  }

  return brush;
}

void BKE_brush_gpencil_paint_presets(Main *bmain, ToolSettings *ts, const bool reset)
{
  bool r_new = false;

  Paint *paint = &ts->gp_paint->paint;
  Brush *brush_prev = paint->brush;
  Brush *brush, *deft_draw;
  /* Airbrush brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Airbrush", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_AIRBRUSH);
  }

  /* Ink Pen brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Ink Pen", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_INK_PEN);
  }

  /* Ink Pen Rough brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Ink Pen Rough", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_INK_PEN_ROUGH);
  }

  /* Marker Bold brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Marker Bold", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_MARKER_BOLD);
  }

  /* Marker Chisel brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Marker Chisel", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_MARKER_CHISEL);
  }

  /* Pen brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Pen", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_PEN);
  }

  /* Pencil Soft brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Pencil Soft", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_PENCIL_SOFT);
  }

  /* Pencil brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Pencil", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_PENCIL);
  }
  deft_draw = brush; /* save default brush. */

  /* Fill brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Fill Area", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_FILL_AREA);
  }

  /* Soft Eraser brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Eraser Soft", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_ERASER_SOFT);
  }

  /* Hard Eraser brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Eraser Hard", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_ERASER_HARD);
  }

  /* Point Eraser brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Eraser Point", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_ERASER_POINT);
  }

  /* Stroke Eraser brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Eraser Stroke", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_ERASER_STROKE);
  }

  /* Tint brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Tint", OB_MODE_PAINT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_TINT);
  }

  /* Set default Draw brush. */
  if ((reset == false) && (brush_prev != nullptr)) {
    BKE_paint_brush_set(paint, brush_prev);
  }
  else {
    BKE_paint_brush_set(paint, deft_draw);
  }
}

void BKE_brush_gpencil_vertex_presets(Main *bmain, ToolSettings *ts, const bool reset)
{
  bool r_new = false;

  Paint *vertexpaint = &ts->gp_vertexpaint->paint;
  Brush *brush_prev = vertexpaint->brush;
  Brush *brush, *deft_vertex;
  /* Vertex Draw brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Vertex Draw", OB_MODE_VERTEX_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_VERTEX_DRAW);
  }
  deft_vertex = brush; /* save default brush. */

  /* Vertex Blur brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Vertex Blur", OB_MODE_VERTEX_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_VERTEX_BLUR);
  }
  /* Vertex Average brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Vertex Average", OB_MODE_VERTEX_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_VERTEX_AVERAGE);
  }
  /* Vertex Smear brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Vertex Smear", OB_MODE_VERTEX_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_VERTEX_SMEAR);
  }
  /* Vertex Replace brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Vertex Replace", OB_MODE_VERTEX_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_VERTEX_REPLACE);
  }

  /* Set default Vertex brush. */
  if (reset || brush_prev == nullptr) {
    BKE_paint_brush_set(vertexpaint, deft_vertex);
  }
  else {
    if (brush_prev != nullptr) {
      BKE_paint_brush_set(vertexpaint, brush_prev);
    }
  }
}

void BKE_brush_gpencil_sculpt_presets(Main *bmain, ToolSettings *ts, const bool reset)
{
  bool r_new = false;

  Paint *sculptpaint = &ts->gp_sculptpaint->paint;
  Brush *brush_prev = sculptpaint->brush;
  Brush *brush, *deft_sculpt;

  /* Smooth brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Smooth Stroke", OB_MODE_SCULPT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_SMOOTH_STROKE);
  }
  deft_sculpt = brush;

  /* Strength brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Strength Stroke", OB_MODE_SCULPT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_STRENGTH_STROKE);
  }

  /* Thickness brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Thickness Stroke", OB_MODE_SCULPT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_THICKNESS_STROKE);
  }

  /* Grab brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Grab Stroke", OB_MODE_SCULPT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_GRAB_STROKE);
  }

  /* Push brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Push Stroke", OB_MODE_SCULPT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_PUSH_STROKE);
  }

  /* Twist brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Twist Stroke", OB_MODE_SCULPT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_TWIST_STROKE);
  }

  /* Pinch brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Pinch Stroke", OB_MODE_SCULPT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_PINCH_STROKE);
  }

  /* Randomize brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Randomize Stroke", OB_MODE_SCULPT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_RANDOMIZE_STROKE);
  }

  /* Clone brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Clone Stroke", OB_MODE_SCULPT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_CLONE_STROKE);
  }

  /* Set default brush. */
  if (reset || brush_prev == nullptr) {
    BKE_paint_brush_set(sculptpaint, deft_sculpt);
  }
  else {
    if (brush_prev != nullptr) {
      BKE_paint_brush_set(sculptpaint, brush_prev);
    }
  }
}

void BKE_brush_gpencil_weight_presets(Main *bmain, ToolSettings *ts, const bool reset)
{
  bool r_new = false;

  Paint *weightpaint = &ts->gp_weightpaint->paint;
  Brush *brush_prev = weightpaint->brush;
  Brush *brush, *deft_weight;
  /* Vertex Draw brush. */
  brush = gpencil_brush_ensure(bmain, ts, "Draw Weight", OB_MODE_WEIGHT_GPENCIL, &r_new);
  if ((reset) || (r_new)) {
    BKE_gpencil_brush_preset_set(bmain, brush, GP_BRUSH_PRESET_DRAW_WEIGHT);
  }
  deft_weight = brush; /* save default brush. */

  /* Set default brush. */
  if (reset || brush_prev == nullptr) {
    BKE_paint_brush_set(weightpaint, deft_weight);
  }
  else {
    if (brush_prev != nullptr) {
      BKE_paint_brush_set(weightpaint, brush_prev);
    }
  }
}

void BKE_brush_init_curves_sculpt_settings(Brush *brush)
{
  if (brush->curves_sculpt_settings == nullptr) {
    brush->curves_sculpt_settings = MEM_cnew<BrushCurvesSculptSettings>(__func__);
  }
  BrushCurvesSculptSettings *settings = brush->curves_sculpt_settings;
  settings->add_amount = 1;
  settings->points_per_curve = 8;
  settings->minimum_length = 0.01f;
  settings->curve_length = 0.3f;
  settings->density_add_attempts = 100;
}

struct Brush *BKE_brush_first_search(struct Main *bmain, const eObjectMode ob_mode)
{
  LISTBASE_FOREACH (Brush *, brush, &bmain->brushes) {
    if (brush->ob_mode & ob_mode) {
      return brush;
    }
  }
  return nullptr;
}

void BKE_brush_debug_print_state(Brush *br)
{
  /* create a fake brush and set it to the defaults */
  Brush def = {{nullptr}};
  brush_defaults(&def);

#define BR_TEST(field, t) \
  if (br->field != def.field) { \
    printf("br->" #field " = %" #t ";\n", br->field); \
  } \
  ((void)0)

#define BR_TEST_FLAG(_f) \
  if ((br->flag & _f) && !(def.flag & _f)) { \
    printf("br->flag |= " #_f ";\n"); \
  } \
  else if (!(br->flag & _f) && (def.flag & _f)) { \
    printf("br->flag &= ~" #_f ";\n"); \
  } \
  ((void)0)

#define BR_TEST_FLAG_OVERLAY(_f) \
  if ((br->overlay_flags & _f) && !(def.overlay_flags & _f)) { \
    printf("br->overlay_flags |= " #_f ";\n"); \
  } \
  else if (!(br->overlay_flags & _f) && (def.overlay_flags & _f)) { \
    printf("br->overlay_flags &= ~" #_f ";\n"); \
  } \
  ((void)0)

  /* print out any non-default brush state */
  BR_TEST(normal_weight, f);

  BR_TEST(blend, d);
  BR_TEST(size, d);

  /* br->flag */
  BR_TEST_FLAG(BRUSH_AIRBRUSH);
  BR_TEST_FLAG(BRUSH_ALPHA_PRESSURE);
  BR_TEST_FLAG(BRUSH_SIZE_PRESSURE);
  BR_TEST_FLAG(BRUSH_JITTER_PRESSURE);
  BR_TEST_FLAG(BRUSH_SPACING_PRESSURE);
  BR_TEST_FLAG(BRUSH_ANCHORED);
  BR_TEST_FLAG(BRUSH_DIR_IN);
  BR_TEST_FLAG(BRUSH_SPACE);
  BR_TEST_FLAG(BRUSH_SMOOTH_STROKE);
  BR_TEST_FLAG(BRUSH_PERSISTENT);
  BR_TEST_FLAG(BRUSH_ACCUMULATE);
  BR_TEST_FLAG(BRUSH_LOCK_ALPHA);
  BR_TEST_FLAG(BRUSH_ORIGINAL_NORMAL);
  BR_TEST_FLAG(BRUSH_OFFSET_PRESSURE);
  BR_TEST_FLAG(BRUSH_SPACE_ATTEN);
  BR_TEST_FLAG(BRUSH_ADAPTIVE_SPACE);
  BR_TEST_FLAG(BRUSH_LOCK_SIZE);
  BR_TEST_FLAG(BRUSH_EDGE_TO_EDGE);
  BR_TEST_FLAG(BRUSH_DRAG_DOT);
  BR_TEST_FLAG(BRUSH_INVERSE_SMOOTH_PRESSURE);
  BR_TEST_FLAG(BRUSH_PLANE_TRIM);
  BR_TEST_FLAG(BRUSH_FRONTFACE);
  BR_TEST_FLAG(BRUSH_CUSTOM_ICON);

  BR_TEST_FLAG_OVERLAY(BRUSH_OVERLAY_CURSOR);
  BR_TEST_FLAG_OVERLAY(BRUSH_OVERLAY_PRIMARY);
  BR_TEST_FLAG_OVERLAY(BRUSH_OVERLAY_SECONDARY);
  BR_TEST_FLAG_OVERLAY(BRUSH_OVERLAY_CURSOR_OVERRIDE_ON_STROKE);
  BR_TEST_FLAG_OVERLAY(BRUSH_OVERLAY_PRIMARY_OVERRIDE_ON_STROKE);
  BR_TEST_FLAG_OVERLAY(BRUSH_OVERLAY_SECONDARY_OVERRIDE_ON_STROKE);

  BR_TEST(jitter, f);
  BR_TEST(spacing, d);
  BR_TEST(smooth_stroke_radius, d);
  BR_TEST(smooth_stroke_factor, f);
  BR_TEST(rate, f);

  BR_TEST(alpha, f);

  BR_TEST(sculpt_plane, d);

  BR_TEST(plane_offset, f);

  BR_TEST(autosmooth_factor, f);

  BR_TEST(topology_rake_factor, f);

  BR_TEST(crease_pinch_factor, f);

  BR_TEST(plane_trim, f);

  BR_TEST(texture_sample_bias, f);
  BR_TEST(texture_overlay_alpha, d);

  BR_TEST(add_col[0], f);
  BR_TEST(add_col[1], f);
  BR_TEST(add_col[2], f);
  BR_TEST(add_col[3], f);
  BR_TEST(sub_col[0], f);
  BR_TEST(sub_col[1], f);
  BR_TEST(sub_col[2], f);
  BR_TEST(sub_col[3], f);

  printf("\n");

#undef BR_TEST
#undef BR_TEST_FLAG
}

void BKE_brush_sculpt_reset(Brush *br)
{
  /* enable this to see any non-default
   * settings used by a brush: */
  // BKE_brush_debug_print_state(br);

  brush_defaults(br);
  BKE_brush_curve_preset(br, CURVE_PRESET_SMOOTH);

  /* Use the curve presets by default */
  br->curve_preset = BRUSH_CURVE_SMOOTH;

  /* Note that sculpt defaults where set when 0.5 was the default (now it's 1.0)
   * assign this so logic below can remain the same. */
  br->alpha = 0.5f;

  /* Brush settings */
  switch (br->sculpt_tool) {
    case SCULPT_TOOL_DRAW_SHARP:
      br->flag |= BRUSH_DIR_IN;
      br->curve_preset = BRUSH_CURVE_POW4;
      br->spacing = 5;
      break;
    case SCULPT_TOOL_DISPLACEMENT_ERASER:
      br->curve_preset = BRUSH_CURVE_SMOOTHER;
      br->spacing = 10;
      br->alpha = 1.0f;
      break;
    case SCULPT_TOOL_SLIDE_RELAX:
      br->spacing = 10;
      br->alpha = 1.0f;
      br->slide_deform_type = BRUSH_SLIDE_DEFORM_DRAG;
      break;
    case SCULPT_TOOL_CLAY:
      br->flag |= BRUSH_SIZE_PRESSURE;
      br->spacing = 3;
      br->autosmooth_factor = 0.25f;
      br->normal_radius_factor = 0.75f;
      br->hardness = 0.65f;
      break;
    case SCULPT_TOOL_CLAY_THUMB:
      br->alpha = 0.5f;
      br->normal_radius_factor = 1.0f;
      br->spacing = 6;
      br->hardness = 0.5f;
      br->flag |= BRUSH_SIZE_PRESSURE;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      break;
    case SCULPT_TOOL_CLAY_STRIPS:
      br->flag |= BRUSH_ACCUMULATE | BRUSH_SIZE_PRESSURE;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      br->alpha = 0.6f;
      br->spacing = 5;
      br->normal_radius_factor = 1.55f;
      br->tip_roundness = 0.18f;
      br->curve_preset = BRUSH_CURVE_SMOOTHER;
      break;
    case SCULPT_TOOL_MULTIPLANE_SCRAPE:
      br->flag2 |= BRUSH_MULTIPLANE_SCRAPE_DYNAMIC | BRUSH_MULTIPLANE_SCRAPE_PLANES_PREVIEW;
      br->alpha = 0.7f;
      br->normal_radius_factor = 0.70f;
      br->multiplane_scrape_angle = 60;
      br->curve_preset = BRUSH_CURVE_SMOOTH;
      br->spacing = 5;
      break;
    case SCULPT_TOOL_CREASE:
      br->flag |= BRUSH_DIR_IN;
      br->alpha = 0.25;
      break;
    case SCULPT_TOOL_SCRAPE:
    case SCULPT_TOOL_FILL:
      br->alpha = 0.7f;
      br->area_radius_factor = 0.5f;
      br->spacing = 7;
      br->flag |= BRUSH_ACCUMULATE;
      br->flag |= BRUSH_INVERT_TO_SCRAPE_FILL;
      break;
    case SCULPT_TOOL_ROTATE:
      br->alpha = 1.0;
      break;
    case SCULPT_TOOL_SMOOTH:
      br->flag &= ~BRUSH_SPACE_ATTEN;
      br->spacing = 5;
      br->alpha = 0.7f;
      br->surface_smooth_shape_preservation = 0.5f;
      br->surface_smooth_current_vertex = 0.5f;
      br->surface_smooth_iterations = 4;
      break;
    case SCULPT_TOOL_SNAKE_HOOK:
      br->alpha = 1.0f;
      br->rake_factor = 1.0f;
      break;
    case SCULPT_TOOL_THUMB:
      br->size = 75;
      br->flag &= ~BRUSH_ALPHA_PRESSURE;
      br->flag &= ~BRUSH_SPACE;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      break;
    case SCULPT_TOOL_ELASTIC_DEFORM:
      br->elastic_deform_volume_preservation = 0.4f;
      br->elastic_deform_type = BRUSH_ELASTIC_DEFORM_GRAB_TRISCALE;
      br->flag &= ~BRUSH_ALPHA_PRESSURE;
      br->flag &= ~BRUSH_SPACE;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      break;
    case SCULPT_TOOL_POSE:
      br->pose_smooth_iterations = 4;
      br->pose_ik_segments = 1;
      br->flag2 |= BRUSH_POSE_IK_ANCHORED | BRUSH_USE_CONNECTED_ONLY;
      br->flag &= ~BRUSH_ALPHA_PRESSURE;
      br->flag &= ~BRUSH_SPACE;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      break;
    case SCULPT_TOOL_BOUNDARY:
      br->flag &= ~BRUSH_ALPHA_PRESSURE;
      br->flag &= ~BRUSH_SPACE;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      br->curve_preset = BRUSH_CURVE_CONSTANT;
      break;
    case SCULPT_TOOL_DRAW_FACE_SETS:
      br->alpha = 0.5f;
      br->flag &= ~BRUSH_ALPHA_PRESSURE;
      br->flag &= ~BRUSH_SPACE;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      break;
    case SCULPT_TOOL_GRAB:
      br->alpha = 0.4f;
      br->size = 75;
      br->flag &= ~BRUSH_ALPHA_PRESSURE;
      br->flag &= ~BRUSH_SPACE;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      break;
    case SCULPT_TOOL_CLOTH:
      br->cloth_mass = 1.0f;
      br->cloth_damping = 0.01f;
      br->cloth_sim_limit = 2.5f;
      br->cloth_sim_falloff = 0.75f;
      br->cloth_deform_type = BRUSH_CLOTH_DEFORM_DRAG;
      br->flag &= ~(BRUSH_ALPHA_PRESSURE | BRUSH_SIZE_PRESSURE);
      break;
    case SCULPT_TOOL_LAYER:
      br->flag &= ~BRUSH_SPACE_ATTEN;
      br->hardness = 0.35f;
      br->alpha = 1.0f;
      br->height = 0.05f;
      break;
    case SCULPT_TOOL_PAINT:
      br->hardness = 0.4f;
      br->spacing = 10;
      br->alpha = 0.6f;
      br->flow = 1.0f;
      br->tip_scale_x = 1.0f;
      br->tip_roundness = 1.0f;
      br->density = 1.0f;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      copy_v3_fl(br->rgb, 1.0f);
      zero_v3(br->secondary_rgb);
      break;
    case SCULPT_TOOL_SMEAR:
      br->alpha = 1.0f;
      br->spacing = 5;
      br->flag &= ~BRUSH_ALPHA_PRESSURE;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      br->curve_preset = BRUSH_CURVE_SPHERE;
      break;
    case SCULPT_TOOL_DISPLACEMENT_SMEAR:
      br->alpha = 1.0f;
      br->spacing = 5;
      br->hardness = 0.7f;
      br->flag &= ~BRUSH_ALPHA_PRESSURE;
      br->flag &= ~BRUSH_SPACE_ATTEN;
      br->curve_preset = BRUSH_CURVE_SMOOTHER;
      break;
    default:
      break;
  }

  /* Cursor colors */

  /* Default Alpha */
  br->add_col[3] = 0.90f;
  br->sub_col[3] = 0.90f;

  switch (br->sculpt_tool) {
    case SCULPT_TOOL_DRAW:
    case SCULPT_TOOL_DRAW_SHARP:
    case SCULPT_TOOL_CLAY:
    case SCULPT_TOOL_CLAY_STRIPS:
    case SCULPT_TOOL_CLAY_THUMB:
    case SCULPT_TOOL_LAYER:
    case SCULPT_TOOL_INFLATE:
    case SCULPT_TOOL_BLOB:
    case SCULPT_TOOL_CREASE:
      br->add_col[0] = 0.0f;
      br->add_col[1] = 0.5f;
      br->add_col[2] = 1.0f;
      br->sub_col[0] = 0.0f;
      br->sub_col[1] = 0.5f;
      br->sub_col[2] = 1.0f;
      break;

    case SCULPT_TOOL_SMOOTH:
    case SCULPT_TOOL_FLATTEN:
    case SCULPT_TOOL_FILL:
    case SCULPT_TOOL_SCRAPE:
    case SCULPT_TOOL_MULTIPLANE_SCRAPE:
      br->add_col[0] = 0.877f;
      br->add_col[1] = 0.142f;
      br->add_col[2] = 0.117f;
      br->sub_col[0] = 0.877f;
      br->sub_col[1] = 0.142f;
      br->sub_col[2] = 0.117f;
      break;

    case SCULPT_TOOL_PINCH:
    case SCULPT_TOOL_GRAB:
    case SCULPT_TOOL_SNAKE_HOOK:
    case SCULPT_TOOL_THUMB:
    case SCULPT_TOOL_NUDGE:
    case SCULPT_TOOL_ROTATE:
    case SCULPT_TOOL_ELASTIC_DEFORM:
    case SCULPT_TOOL_POSE:
    case SCULPT_TOOL_BOUNDARY:
    case SCULPT_TOOL_SLIDE_RELAX:
      br->add_col[0] = 1.0f;
      br->add_col[1] = 0.95f;
      br->add_col[2] = 0.005f;
      br->sub_col[0] = 1.0f;
      br->sub_col[1] = 0.95f;
      br->sub_col[2] = 0.005f;
      break;

    case SCULPT_TOOL_SIMPLIFY:
    case SCULPT_TOOL_PAINT:
    case SCULPT_TOOL_MASK:
    case SCULPT_TOOL_DRAW_FACE_SETS:
    case SCULPT_TOOL_DISPLACEMENT_ERASER:
    case SCULPT_TOOL_DISPLACEMENT_SMEAR:
      br->add_col[0] = 0.75f;
      br->add_col[1] = 0.75f;
      br->add_col[2] = 0.75f;
      br->sub_col[0] = 0.75f;
      br->sub_col[1] = 0.75f;
      br->sub_col[2] = 0.75f;
      break;

    case SCULPT_TOOL_CLOTH:
      br->add_col[0] = 1.0f;
      br->add_col[1] = 0.5f;
      br->add_col[2] = 0.1f;
      br->sub_col[0] = 1.0f;
      br->sub_col[1] = 0.5f;
      br->sub_col[2] = 0.1f;
      break;
    default:
      break;
  }
}

void BKE_brush_curve_preset(Brush *b, eCurveMappingPreset preset)
{
  CurveMapping *cumap = nullptr;
  CurveMap *cuma = nullptr;

  if (!b->curve) {
    b->curve = BKE_curvemapping_add(1, 0, 0, 1, 1);
  }
  cumap = b->curve;
  cumap->flag &= ~CUMA_EXTEND_EXTRAPOLATE;
  cumap->preset = preset;

  cuma = b->curve->cm;
  BKE_curvemap_reset(cuma, &cumap->clipr, cumap->preset, CURVEMAP_SLOPE_NEGATIVE);
  BKE_curvemapping_changed(cumap, false);
}

float BKE_brush_sample_tex_3d(const Scene *scene,
                              const Brush *br,
                              const float point[3],
                              float rgba[4],
                              const int thread,
                              struct ImagePool *pool)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;
  const MTex *mtex = &br->mtex;
  float intensity = 1.0;
  bool hasrgb = false;

  if (!mtex->tex) {
    intensity = 1;
  }
  else if (mtex->brush_map_mode == MTEX_MAP_MODE_3D) {
    /* Get strength by feeding the vertex
     * location directly into a texture */
    hasrgb = RE_texture_evaluate(mtex, point, thread, pool, false, false, &intensity, rgba);
  }
  else if (mtex->brush_map_mode == MTEX_MAP_MODE_STENCIL) {
    float rotation = -mtex->rot;
    const float point_2d[2] = {point[0], point[1]};
    float x, y;
    float co[3];

    x = point_2d[0] - br->stencil_pos[0];
    y = point_2d[1] - br->stencil_pos[1];

    if (rotation > 0.001f || rotation < -0.001f) {
      const float angle = atan2f(y, x) + rotation;
      const float flen = sqrtf(x * x + y * y);

      x = flen * cosf(angle);
      y = flen * sinf(angle);
    }

    if (fabsf(x) > br->stencil_dimension[0] || fabsf(y) > br->stencil_dimension[1]) {
      zero_v4(rgba);
      return 0.0f;
    }
    x /= (br->stencil_dimension[0]);
    y /= (br->stencil_dimension[1]);

    co[0] = x;
    co[1] = y;
    co[2] = 0.0f;

    hasrgb = RE_texture_evaluate(mtex, co, thread, pool, false, false, &intensity, rgba);
  }
  else {
    float rotation = -mtex->rot;
    const float point_2d[2] = {point[0], point[1]};
    float x = 0.0f, y = 0.0f; /* Quite warnings */
    float invradius = 1.0f;   /* Quite warnings */
    float co[3];

    if (mtex->brush_map_mode == MTEX_MAP_MODE_VIEW) {
      /* keep coordinates relative to mouse */

      rotation += ups->brush_rotation;

      x = point_2d[0] - ups->tex_mouse[0];
      y = point_2d[1] - ups->tex_mouse[1];

      /* use pressure adjusted size for fixed mode */
      invradius = 1.0f / ups->pixel_radius;
    }
    else if (mtex->brush_map_mode == MTEX_MAP_MODE_TILED) {
      /* leave the coordinates relative to the screen */

      /* use unadjusted size for tiled mode */
      invradius = 1.0f / ups->start_pixel_radius;

      x = point_2d[0];
      y = point_2d[1];
    }
    else if (mtex->brush_map_mode == MTEX_MAP_MODE_RANDOM) {
      rotation += ups->brush_rotation;
      /* these contain a random coordinate */
      x = point_2d[0] - ups->tex_mouse[0];
      y = point_2d[1] - ups->tex_mouse[1];

      invradius = 1.0f / ups->pixel_radius;
    }

    x *= invradius;
    y *= invradius;

    /* it is probably worth optimizing for those cases where
     * the texture is not rotated by skipping the calls to
     * atan2, sqrtf, sin, and cos. */
    if (rotation > 0.001f || rotation < -0.001f) {
      const float angle = atan2f(y, x) + rotation;
      const float flen = sqrtf(x * x + y * y);

      x = flen * cosf(angle);
      y = flen * sinf(angle);
    }

    co[0] = x;
    co[1] = y;
    co[2] = 0.0f;

    hasrgb = RE_texture_evaluate(mtex, co, thread, pool, false, false, &intensity, rgba);
  }

  intensity += br->texture_sample_bias;

  if (!hasrgb) {
    rgba[0] = intensity;
    rgba[1] = intensity;
    rgba[2] = intensity;
    rgba[3] = 1.0f;
  }
  /* For consistency, sampling always returns color in linear space */
  else if (ups->do_linear_conversion) {
    IMB_colormanagement_colorspace_to_scene_linear_v3(rgba, ups->colorspace);
  }

  return intensity;
}

float BKE_brush_sample_masktex(
    const Scene *scene, Brush *br, const float point[2], const int thread, struct ImagePool *pool)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;
  MTex *mtex = &br->mask_mtex;
  float rgba[4], intensity;

  if (!mtex->tex) {
    return 1.0f;
  }
  if (mtex->brush_map_mode == MTEX_MAP_MODE_STENCIL) {
    float rotation = -mtex->rot;
    const float point_2d[2] = {point[0], point[1]};
    float x, y;
    float co[3];

    x = point_2d[0] - br->mask_stencil_pos[0];
    y = point_2d[1] - br->mask_stencil_pos[1];

    if (rotation > 0.001f || rotation < -0.001f) {
      const float angle = atan2f(y, x) + rotation;
      const float flen = sqrtf(x * x + y * y);

      x = flen * cosf(angle);
      y = flen * sinf(angle);
    }

    if (fabsf(x) > br->mask_stencil_dimension[0] || fabsf(y) > br->mask_stencil_dimension[1]) {
      zero_v4(rgba);
      return 0.0f;
    }
    x /= (br->mask_stencil_dimension[0]);
    y /= (br->mask_stencil_dimension[1]);

    co[0] = x;
    co[1] = y;
    co[2] = 0.0f;

    RE_texture_evaluate(mtex, co, thread, pool, false, false, &intensity, rgba);
  }
  else {
    float rotation = -mtex->rot;
    const float point_2d[2] = {point[0], point[1]};
    float x = 0.0f, y = 0.0f; /* Quite warnings */
    float invradius = 1.0f;   /* Quite warnings */
    float co[3];

    if (mtex->brush_map_mode == MTEX_MAP_MODE_VIEW) {
      /* keep coordinates relative to mouse */

      rotation += ups->brush_rotation_sec;

      x = point_2d[0] - ups->mask_tex_mouse[0];
      y = point_2d[1] - ups->mask_tex_mouse[1];

      /* use pressure adjusted size for fixed mode */
      invradius = 1.0f / ups->pixel_radius;
    }
    else if (mtex->brush_map_mode == MTEX_MAP_MODE_TILED) {
      /* leave the coordinates relative to the screen */

      /* use unadjusted size for tiled mode */
      invradius = 1.0f / ups->start_pixel_radius;

      x = point_2d[0];
      y = point_2d[1];
    }
    else if (mtex->brush_map_mode == MTEX_MAP_MODE_RANDOM) {
      rotation += ups->brush_rotation_sec;
      /* these contain a random coordinate */
      x = point_2d[0] - ups->mask_tex_mouse[0];
      y = point_2d[1] - ups->mask_tex_mouse[1];

      invradius = 1.0f / ups->pixel_radius;
    }

    x *= invradius;
    y *= invradius;

    /* it is probably worth optimizing for those cases where
     * the texture is not rotated by skipping the calls to
     * atan2, sqrtf, sin, and cos. */
    if (rotation > 0.001f || rotation < -0.001f) {
      const float angle = atan2f(y, x) + rotation;
      const float flen = sqrtf(x * x + y * y);

      x = flen * cosf(angle);
      y = flen * sinf(angle);
    }

    co[0] = x;
    co[1] = y;
    co[2] = 0.0f;

    RE_texture_evaluate(mtex, co, thread, pool, false, false, &intensity, rgba);
  }

  CLAMP(intensity, 0.0f, 1.0f);

  switch (br->mask_pressure) {
    case BRUSH_MASK_PRESSURE_CUTOFF:
      intensity = ((1.0f - intensity) < ups->size_pressure_value) ? 1.0f : 0.0f;
      break;
    case BRUSH_MASK_PRESSURE_RAMP:
      intensity = ups->size_pressure_value + intensity * (1.0f - ups->size_pressure_value);
      break;
    default:
      break;
  }

  return intensity;
}

/* Unified Size / Strength / Color */

/* XXX: be careful about setting size and unprojected radius
 * because they depend on one another
 * these functions do not set the other corresponding value
 * this can lead to odd behavior if size and unprojected
 * radius become inconsistent.
 * the biggest problem is that it isn't possible to change
 * unprojected radius because a view context is not
 * available.  my usual solution to this is to use the
 * ratio of change of the size to change the unprojected
 * radius.  Not completely convinced that is correct.
 * In any case, a better solution is needed to prevent
 * inconsistency. */

const float *BKE_brush_color_get(const struct Scene *scene, const struct Brush *brush)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;
  return (ups->flag & UNIFIED_PAINT_COLOR) ? ups->rgb : brush->rgb;
}

const float *BKE_brush_secondary_color_get(const struct Scene *scene, const struct Brush *brush)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;
  return (ups->flag & UNIFIED_PAINT_COLOR) ? ups->secondary_rgb : brush->secondary_rgb;
}

void BKE_brush_color_set(struct Scene *scene, struct Brush *brush, const float color[3])
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;

  if (ups->flag & UNIFIED_PAINT_COLOR) {
    copy_v3_v3(ups->rgb, color);
  }
  else {
    copy_v3_v3(brush->rgb, color);
  }
}

void BKE_brush_size_set(Scene *scene, Brush *brush, int size)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;

  /* make sure range is sane */
  CLAMP(size, 1, MAX_BRUSH_PIXEL_RADIUS);

  if (ups->flag & UNIFIED_PAINT_SIZE) {
    ups->size = size;
  }
  else {
    brush->size = size;
  }
}

int BKE_brush_size_get(const Scene *scene, const Brush *brush)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;
  int size = (ups->flag & UNIFIED_PAINT_SIZE) ? ups->size : brush->size;

  return size;
}

bool BKE_brush_use_locked_size(const Scene *scene, const Brush *brush)
{
  const short us_flag = scene->toolsettings->unified_paint_settings.flag;

  return (us_flag & UNIFIED_PAINT_SIZE) ? (us_flag & UNIFIED_PAINT_BRUSH_LOCK_SIZE) :
                                          (brush->flag & BRUSH_LOCK_SIZE);
}

bool BKE_brush_use_size_pressure(const Brush *brush)
{
  return brush->flag & BRUSH_SIZE_PRESSURE;
}

bool BKE_brush_use_alpha_pressure(const Brush *brush)
{
  return brush->flag & BRUSH_ALPHA_PRESSURE;
}

bool BKE_brush_sculpt_has_secondary_color(const Brush *brush)
{
  return ELEM(brush->sculpt_tool,
              SCULPT_TOOL_BLOB,
              SCULPT_TOOL_DRAW,
              SCULPT_TOOL_DRAW_SHARP,
              SCULPT_TOOL_INFLATE,
              SCULPT_TOOL_CLAY,
              SCULPT_TOOL_CLAY_STRIPS,
              SCULPT_TOOL_CLAY_THUMB,
              SCULPT_TOOL_PINCH,
              SCULPT_TOOL_CREASE,
              SCULPT_TOOL_LAYER,
              SCULPT_TOOL_FLATTEN,
              SCULPT_TOOL_FILL,
              SCULPT_TOOL_SCRAPE,
              SCULPT_TOOL_MASK);
}

void BKE_brush_unprojected_radius_set(Scene *scene, Brush *brush, float unprojected_radius)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;

  if (ups->flag & UNIFIED_PAINT_SIZE) {
    ups->unprojected_radius = unprojected_radius;
  }
  else {
    brush->unprojected_radius = unprojected_radius;
  }
}

float BKE_brush_unprojected_radius_get(const Scene *scene, const Brush *brush)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;

  return (ups->flag & UNIFIED_PAINT_SIZE) ? ups->unprojected_radius : brush->unprojected_radius;
}

void BKE_brush_alpha_set(Scene *scene, Brush *brush, float alpha)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;

  if (ups->flag & UNIFIED_PAINT_ALPHA) {
    ups->alpha = alpha;
  }
  else {
    brush->alpha = alpha;
  }
}

float BKE_brush_alpha_get(const Scene *scene, const Brush *brush)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;

  return (ups->flag & UNIFIED_PAINT_ALPHA) ? ups->alpha : brush->alpha;
}

float BKE_brush_weight_get(const Scene *scene, const Brush *brush)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;

  return (ups->flag & UNIFIED_PAINT_WEIGHT) ? ups->weight : brush->weight;
}

void BKE_brush_weight_set(const Scene *scene, Brush *brush, float value)
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;

  if (ups->flag & UNIFIED_PAINT_WEIGHT) {
    ups->weight = value;
  }
  else {
    brush->weight = value;
  }
}

void BKE_brush_scale_unprojected_radius(float *unprojected_radius,
                                        int new_brush_size,
                                        int old_brush_size)
{
  float scale = new_brush_size;
  /* avoid division by zero */
  if (old_brush_size != 0) {
    scale /= (float)old_brush_size;
  }
  (*unprojected_radius) *= scale;
}

void BKE_brush_scale_size(int *r_brush_size,
                          float new_unprojected_radius,
                          float old_unprojected_radius)
{
  float scale = new_unprojected_radius;
  /* avoid division by zero */
  if (old_unprojected_radius != 0) {
    scale /= new_unprojected_radius;
  }
  (*r_brush_size) = (int)((float)(*r_brush_size) * scale);
}

void BKE_brush_jitter_pos(const Scene *scene, Brush *brush, const float pos[2], float jitterpos[2])
{
  float rand_pos[2];
  float spread;
  int diameter;

  do {
    rand_pos[0] = BLI_rng_get_float(brush_rng) - 0.5f;
    rand_pos[1] = BLI_rng_get_float(brush_rng) - 0.5f;
  } while (len_squared_v2(rand_pos) > square_f(0.5f));

  if (brush->flag & BRUSH_ABSOLUTE_JITTER) {
    diameter = 2 * brush->jitter_absolute;
    spread = 1.0;
  }
  else {
    diameter = 2 * BKE_brush_size_get(scene, brush);
    spread = brush->jitter;
  }
  /* find random position within a circle of diameter 1 */
  jitterpos[0] = pos[0] + 2 * rand_pos[0] * diameter * spread;
  jitterpos[1] = pos[1] + 2 * rand_pos[1] * diameter * spread;
}

void BKE_brush_randomize_texture_coords(UnifiedPaintSettings *ups, bool mask)
{
  /* we multiply with brush radius as an optimization for the brush
   * texture sampling functions */
  if (mask) {
    ups->mask_tex_mouse[0] = BLI_rng_get_float(brush_rng) * ups->pixel_radius;
    ups->mask_tex_mouse[1] = BLI_rng_get_float(brush_rng) * ups->pixel_radius;
  }
  else {
    ups->tex_mouse[0] = BLI_rng_get_float(brush_rng) * ups->pixel_radius;
    ups->tex_mouse[1] = BLI_rng_get_float(brush_rng) * ups->pixel_radius;
  }
}

float BKE_brush_curve_strength(const Brush *br, float p, const float len)
{
  float strength = 1.0f;

  if (p >= len) {
    return 0;
  }

  p = p / len;
  p = 1.0f - p;

  switch (br->curve_preset) {
    case BRUSH_CURVE_CUSTOM:
      strength = BKE_curvemapping_evaluateF(br->curve, 0, 1.0f - p);
      break;
    case BRUSH_CURVE_SHARP:
      strength = p * p;
      break;
    case BRUSH_CURVE_SMOOTH:
      strength = 3.0f * p * p - 2.0f * p * p * p;
      break;
    case BRUSH_CURVE_SMOOTHER:
      strength = pow3f(p) * (p * (p * 6.0f - 15.0f) + 10.0f);
      break;
    case BRUSH_CURVE_ROOT:
      strength = sqrtf(p);
      break;
    case BRUSH_CURVE_LIN:
      strength = p;
      break;
    case BRUSH_CURVE_CONSTANT:
      strength = 1.0f;
      break;
    case BRUSH_CURVE_SPHERE:
      strength = sqrtf(2 * p - p * p);
      break;
    case BRUSH_CURVE_POW4:
      strength = p * p * p * p;
      break;
    case BRUSH_CURVE_INVSQUARE:
      strength = p * (2.0f - p);
      break;
  }

  return strength;
}

float BKE_brush_curve_strength_clamped(const Brush *br, float p, const float len)
{
  float strength = BKE_brush_curve_strength(br, p, len);

  CLAMP(strength, 0.0f, 1.0f);

  return strength;
}

/* TODO: should probably be unified with BrushPainter stuff? */
static bool brush_gen_texture(const Brush *br,
                              const int side,
                              const bool use_secondary,
                              float *rect)
{
  const MTex *mtex = (use_secondary) ? &br->mask_mtex : &br->mtex;
  if (mtex->tex == nullptr) {
    return false;
  }

  const float step = 2.0 / side;
  int ix, iy;
  float x, y;

  /* Do normalized canonical view coords for texture. */
  for (y = -1.0, iy = 0; iy < side; iy++, y += step) {
    for (x = -1.0, ix = 0; ix < side; ix++, x += step) {
      const float co[3] = {x, y, 0.0f};

      float intensity;
      float rgba_dummy[4];
      RE_texture_evaluate(mtex, co, 0, nullptr, false, false, &intensity, rgba_dummy);

      rect[iy * side + ix] = intensity;
    }
  }

  return true;
}

struct ImBuf *BKE_brush_gen_radial_control_imbuf(Brush *br, bool secondary, bool display_gradient)
{
  ImBuf *im = MEM_cnew<ImBuf>("radial control texture");
  int side = 512;
  int half = side / 2;

  BKE_curvemapping_init(br->curve);
  im->rect_float = (float *)MEM_callocN(sizeof(float) * side * side, "radial control rect");
  im->x = im->y = side;

  const bool have_texture = brush_gen_texture(br, side, secondary, im->rect_float);

  if (display_gradient || have_texture) {
    for (int i = 0; i < side; i++) {
      for (int j = 0; j < side; j++) {
        const float magn = sqrtf(pow2f(i - half) + pow2f(j - half));
        const float strength = BKE_brush_curve_strength_clamped(br, magn, half);
        im->rect_float[i * side + j] = (have_texture) ? im->rect_float[i * side + j] * strength :
                                                        strength;
      }
    }
  }

  return im;
}
