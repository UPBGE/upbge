/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Blender Foundation. */

/** \file
 * \ingroup modifiers
 */

#include <stdio.h>

#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "MEM_guardedalloc.h"

#include "DNA_defaults.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_geom.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_lib_query.h"
#include "BKE_modifier.h"
#include "BKE_screen.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"

#include "MOD_gpencil_modifiertypes.h"
#include "MOD_gpencil_ui_common.h"
#include "MOD_gpencil_util.h"

static void initData(GpencilModifierData *md)
{
  NoiseGpencilModifierData *gpmd = (NoiseGpencilModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(gpmd, modifier));

  MEMCPY_STRUCT_AFTER(gpmd, DNA_struct_default_get(NoiseGpencilModifierData), modifier);

  gpmd->curve_intensity = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  CurveMapping *curve = gpmd->curve_intensity;
  BKE_curvemap_reset(curve->cm, &curve->clipr, CURVE_PRESET_BELL, CURVEMAP_SLOPE_POSITIVE);
  BKE_curvemapping_init(curve);
}

static void freeData(GpencilModifierData *md)
{
  NoiseGpencilModifierData *gpmd = (NoiseGpencilModifierData *)md;

  if (gpmd->curve_intensity) {
    BKE_curvemapping_free(gpmd->curve_intensity);
  }
}

static void copyData(const GpencilModifierData *md, GpencilModifierData *target)
{
  NoiseGpencilModifierData *gmd = (NoiseGpencilModifierData *)md;
  NoiseGpencilModifierData *tgmd = (NoiseGpencilModifierData *)target;

  if (tgmd->curve_intensity != NULL) {
    BKE_curvemapping_free(tgmd->curve_intensity);
    tgmd->curve_intensity = NULL;
  }

  BKE_gpencil_modifier_copydata_generic(md, target);

  tgmd->curve_intensity = BKE_curvemapping_copy(gmd->curve_intensity);
}

static bool dependsOnTime(GpencilModifierData *md)
{
  NoiseGpencilModifierData *mmd = (NoiseGpencilModifierData *)md;
  return (mmd->flag & GP_NOISE_USE_RANDOM) != 0;
}

static float *noise_table(int len, int offset, int seed)
{
  float *table = MEM_callocN(sizeof(float) * len, __func__);
  for (int i = 0; i < len; i++) {
    table[i] = BLI_hash_int_01(BLI_hash_int_2d(seed, i + offset + 1));
  }
  return table;
}

BLI_INLINE float table_sample(float *table, float x)
{
  return interpf(table[(int)ceilf(x)], table[(int)floor(x)], fractf(x));
}

/**
 * Apply noise effect based on stroke direction.
 */
static void deformStroke(GpencilModifierData *md,
                         Depsgraph *depsgraph,
                         Object *ob,
                         bGPDlayer *gpl,
                         bGPDframe *gpf,
                         bGPDstroke *gps)
{
  NoiseGpencilModifierData *mmd = (NoiseGpencilModifierData *)md;
  MDeformVert *dvert = NULL;
  /* Noise value in range [-1..1] */
  float normal[3];
  float vec1[3], vec2[3];
  const int def_nr = BKE_object_defgroup_name_index(ob, mmd->vgname);
  const bool invert_group = (mmd->flag & GP_NOISE_INVERT_VGROUP) != 0;
  const bool use_curve = (mmd->flag & GP_NOISE_CUSTOM_CURVE) != 0 && mmd->curve_intensity;
  const int cfra = (int)DEG_get_ctime(depsgraph);
  const bool is_keyframe = (mmd->noise_mode == GP_NOISE_RANDOM_KEYFRAME);

  if (!is_stroke_affected_by_modifier(ob,
                                      mmd->layername,
                                      mmd->material,
                                      mmd->pass_index,
                                      mmd->layer_pass,
                                      1,
                                      gpl,
                                      gps,
                                      mmd->flag & GP_NOISE_INVERT_LAYER,
                                      mmd->flag & GP_NOISE_INVERT_PASS,
                                      mmd->flag & GP_NOISE_INVERT_LAYERPASS,
                                      mmd->flag & GP_NOISE_INVERT_MATERIAL)) {
    return;
  }

  int seed = mmd->seed;
  /* FIXME(fclem): This is really slow. We should get the stroke index in another way. */
  int stroke_seed = BLI_findindex(&gpf->strokes, gps);
  seed += stroke_seed;

  /* Make sure different modifiers get different seeds. */
  seed += BLI_hash_string(ob->id.name + 2);
  seed += BLI_hash_string(md->name);

  if (mmd->flag & GP_NOISE_USE_RANDOM) {
    if (!is_keyframe) {
      seed += cfra / mmd->step;
    }
    else {
      /* If change every keyframe, use the last keyframe. */
      seed += gpf->framenum;
    }
  }

  /* Sanitize as it can create out of bound reads. */
  float noise_scale = clamp_f(mmd->noise_scale, 0.0f, 1.0f);

  int len = ceilf(gps->totpoints * noise_scale) + 2;
  float *noise_table_position = (mmd->factor > 0.0f) ?
                                    noise_table(len, (int)floor(mmd->noise_offset), seed + 2) :
                                    NULL;
  float *noise_table_strength = (mmd->factor_strength > 0.0f) ?
                                    noise_table(len, (int)floor(mmd->noise_offset), seed + 3) :
                                    NULL;
  float *noise_table_thickness = (mmd->factor_thickness > 0.0f) ?
                                     noise_table(len, (int)floor(mmd->noise_offset), seed) :
                                     NULL;
  float *noise_table_uvs = (mmd->factor_uvs > 0.0f) ?
                               noise_table(len, (int)floor(mmd->noise_offset), seed + 4) :
                               NULL;

  /* Calculate stroke normal. */
  if (gps->totpoints > 2) {
    BKE_gpencil_stroke_normal(gps, normal);
    if (is_zero_v3(normal)) {
      copy_v3_fl(normal, 1.0f);
    }
  }
  else {
    copy_v3_fl(normal, 1.0f);
  }

  /* move points */
  for (int i = 0; i < gps->totpoints; i++) {
    bGPDspoint *pt = &gps->points[i];
    /* verify vertex group */
    dvert = gps->dvert != NULL ? &gps->dvert[i] : NULL;
    float weight = get_modifier_point_weight(dvert, invert_group, def_nr);
    if (weight < 0.0f) {
      continue;
    }

    if (use_curve) {
      float value = (float)i / (gps->totpoints - 1);
      weight *= BKE_curvemapping_evaluateF(mmd->curve_intensity, 0, value);
    }

    if (mmd->factor > 0.0f) {
      /* Offset point randomly around the bi-normal vector. */
      if (gps->totpoints == 1) {
        copy_v3_fl3(vec1, 1.0f, 0.0f, 0.0f);
      }
      else if (i != gps->totpoints - 1) {
        /* Initial vector (p1 -> p0). */
        sub_v3_v3v3(vec1, &gps->points[i].x, &gps->points[i + 1].x);
        /* if vec2 is zero, set to something */
        if (len_squared_v3(vec1) < 1e-8f) {
          copy_v3_fl3(vec1, 1.0f, 0.0f, 0.0f);
        }
      }
      else {
        /* Last point reuse the penultimate normal (still stored in vec1)
         * because the previous point is already modified. */
      }
      /* Vector orthogonal to normal. */
      cross_v3_v3v3(vec2, vec1, normal);
      normalize_v3(vec2);

      float noise = table_sample(noise_table_position,
                                 i * noise_scale + fractf(mmd->noise_offset));
      madd_v3_v3fl(&pt->x, vec2, (noise * 2.0f - 1.0f) * weight * mmd->factor * 0.1f);
    }

    if (mmd->factor_thickness > 0.0f) {
      float noise = table_sample(noise_table_thickness,
                                 i * noise_scale + fractf(mmd->noise_offset));
      pt->pressure *= max_ff(1.0f + (noise * 2.0f - 1.0f) * weight * mmd->factor_thickness, 0.0f);
      CLAMP_MIN(pt->pressure, GPENCIL_STRENGTH_MIN);
    }

    if (mmd->factor_strength > 0.0f) {
      float noise = table_sample(noise_table_strength,
                                 i * noise_scale + fractf(mmd->noise_offset));
      pt->strength *= max_ff(1.0f - noise * weight * mmd->factor_strength, 0.0f);
      CLAMP(pt->strength, GPENCIL_STRENGTH_MIN, 1.0f);
    }

    if (mmd->factor_uvs > 0.0f) {
      float noise = table_sample(noise_table_uvs, i * noise_scale + fractf(mmd->noise_offset));
      pt->uv_rot += (noise * 2.0f - 1.0f) * weight * mmd->factor_uvs * M_PI_2;
      CLAMP(pt->uv_rot, -M_PI_2, M_PI_2);
    }
  }

  MEM_SAFE_FREE(noise_table_position);
  MEM_SAFE_FREE(noise_table_strength);
  MEM_SAFE_FREE(noise_table_thickness);
  MEM_SAFE_FREE(noise_table_uvs);
}

static void bakeModifier(struct Main *UNUSED(bmain),
                         Depsgraph *depsgraph,
                         GpencilModifierData *md,
                         Object *ob)
{
  generic_bake_deform_stroke(depsgraph, md, ob, false, deformStroke);
}

static void foreachIDLink(GpencilModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  NoiseGpencilModifierData *mmd = (NoiseGpencilModifierData *)md;

  walk(userData, ob, (ID **)&mmd->material, IDWALK_CB_USER);
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *col;
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  uiLayoutSetPropSep(layout, true);

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "factor", 0, IFACE_("Position"), ICON_NONE);
  uiItemR(col, ptr, "factor_strength", 0, IFACE_("Strength"), ICON_NONE);
  uiItemR(col, ptr, "factor_thickness", 0, IFACE_("Thickness"), ICON_NONE);
  uiItemR(col, ptr, "factor_uvs", 0, IFACE_("UV"), ICON_NONE);
  uiItemR(col, ptr, "noise_scale", 0, NULL, ICON_NONE);
  uiItemR(col, ptr, "noise_offset", 0, NULL, ICON_NONE);
  uiItemR(col, ptr, "seed", 0, NULL, ICON_NONE);

  gpencil_modifier_panel_end(layout, ptr);
}

static void random_header_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  uiItemR(layout, ptr, "use_random", 0, IFACE_("Randomize"), ICON_NONE);
}

static void random_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  uiLayoutSetPropSep(layout, true);

  uiLayoutSetActive(layout, RNA_boolean_get(ptr, "use_random"));

  uiItemR(layout, ptr, "random_mode", 0, NULL, ICON_NONE);

  const int mode = RNA_enum_get(ptr, "random_mode");
  if (mode != GP_NOISE_RANDOM_KEYFRAME) {
    uiItemR(layout, ptr, "step", 0, NULL, ICON_NONE);
  }
}

static void mask_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  gpencil_modifier_masking_panel_draw(panel, true, true);
}

static void panelRegister(ARegionType *region_type)
{
  PanelType *panel_type = gpencil_modifier_panel_register(
      region_type, eGpencilModifierType_Noise, panel_draw);
  gpencil_modifier_subpanel_register(
      region_type, "randomize", "", random_header_draw, random_panel_draw, panel_type);
  PanelType *mask_panel_type = gpencil_modifier_subpanel_register(
      region_type, "mask", "Influence", NULL, mask_panel_draw, panel_type);
  gpencil_modifier_subpanel_register(region_type,
                                     "curve",
                                     "",
                                     gpencil_modifier_curve_header_draw,
                                     gpencil_modifier_curve_panel_draw,
                                     mask_panel_type);
}

GpencilModifierTypeInfo modifierType_Gpencil_Noise = {
    /* name */ N_("Noise"),
    /* structName */ "NoiseGpencilModifierData",
    /* structSize */ sizeof(NoiseGpencilModifierData),
    /* type */ eGpencilModifierTypeType_Gpencil,
    /* flags */ eGpencilModifierTypeFlag_SupportsEditmode,

    /* copyData */ copyData,

    /* deformStroke */ deformStroke,
    /* generateStrokes */ NULL,
    /* bakeModifier */ bakeModifier,
    /* remapTime */ NULL,

    /* initData */ initData,
    /* freeData */ freeData,
    /* isDisabled */ NULL,
    /* updateDepsgraph */ NULL,
    /* dependsOnTime */ dependsOnTime,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ NULL,
    /* panelRegister */ panelRegister,
};
