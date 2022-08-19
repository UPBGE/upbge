/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 by Bastien Montagne. All rights reserved. */

/** \file
 * \ingroup modifiers
 */

#include "BLI_utildefines.h"

#include "BLI_listbase.h"
#include "BLI_math.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_deform.h"
#include "BKE_lib_query.h"
#include "BKE_modifier.h"
#include "BKE_screen.h"
#include "BKE_texture.h" /* Texture masking. */

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "MEM_guardedalloc.h"

#include "MOD_modifiertypes.h"
#include "MOD_ui_common.h"
#include "MOD_util.h"
#include "MOD_weightvg_util.h"

/**
 * This mixes the old weight with the new weight factor.
 */
static float mix_weight(float weight, float weight2, char mix_mode)
{
#if 0
  /*
   * XXX Don't know why, but the switch version takes many CPU time,
   *     and produces lag in realtime playback...
   */
  switch (mix_mode) {
    case MOD_WVG_MIX_ADD:
      return (weight + weight2);
    case MOD_WVG_MIX_SUB:
      return (weight - weight2);
    case MOD_WVG_MIX_MUL:
      return (weight * weight2);
    case MOD_WVG_MIX_DIV:
      /* Avoid dividing by zero (or really small values). */
      if (0.0 <= weight2 < MOD_WVG_ZEROFLOOR) {
        weight2 = MOD_WVG_ZEROFLOOR;
      }
      else if (-MOD_WVG_ZEROFLOOR < weight2) {
        weight2 = -MOD_WVG_ZEROFLOOR;
      }
      return (weight / weight2);
    case MOD_WVG_MIX_DIF:
      return (weight < weight2 ? weight2 - weight : weight - weight2);
    case MOD_WVG_MIX_AVG:
      return (weight + weight2) / 2.0;
    case MOD_WVG_MIX_SET:
    default:
      return weight2;
  }
#endif
  if (mix_mode == MOD_WVG_MIX_SET) {
    return weight2;
  }
  if (mix_mode == MOD_WVG_MIX_ADD) {
    return (weight + weight2);
  }
  if (mix_mode == MOD_WVG_MIX_SUB) {
    return (weight - weight2);
  }
  if (mix_mode == MOD_WVG_MIX_MUL) {
    return (weight * weight2);
  }
  if (mix_mode == MOD_WVG_MIX_DIV) {
    /* Avoid dividing by zero (or really small values). */
    if (weight2 < 0.0f && weight2 > -MOD_WVG_ZEROFLOOR) {
      weight2 = -MOD_WVG_ZEROFLOOR;
    }
    else if (weight2 >= 0.0f && weight2 < MOD_WVG_ZEROFLOOR) {
      weight2 = MOD_WVG_ZEROFLOOR;
    }
    return (weight / weight2);
  }
  if (mix_mode == MOD_WVG_MIX_DIF) {
    return (weight < weight2 ? weight2 - weight : weight - weight2);
  }
  if (mix_mode == MOD_WVG_MIX_AVG) {
    return (weight + weight2) * 0.5f;
  }
  if (mix_mode == MOD_WVG_MIX_MIN) {
    return (weight < weight2 ? weight : weight2);
  }
  if (mix_mode == MOD_WVG_MIX_MAX) {
    return (weight > weight2 ? weight : weight2);
  }

  return weight2;
}

/**************************************
 * Modifiers functions.               *
 **************************************/
static void initData(ModifierData *md)
{
  WeightVGMixModifierData *wmd = (WeightVGMixModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(wmd, modifier));

  MEMCPY_STRUCT_AFTER(wmd, DNA_struct_default_get(WeightVGMixModifierData), modifier);
}

static void requiredDataMask(Object *UNUSED(ob),
                             ModifierData *md,
                             CustomData_MeshMasks *r_cddata_masks)
{
  WeightVGMixModifierData *wmd = (WeightVGMixModifierData *)md;

  /* We need vertex groups! */
  r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;

  /* Ask for UV coordinates if we need them. */
  if (wmd->mask_tex_mapping == MOD_DISP_MAP_UV) {
    r_cddata_masks->fmask |= CD_MASK_MTFACE;
  }

  /* No need to ask for CD_PREVIEW_MLOOPCOL... */
}

static bool dependsOnTime(struct Scene *UNUSED(scene), ModifierData *md)
{
  WeightVGMixModifierData *wmd = (WeightVGMixModifierData *)md;

  if (wmd->mask_texture) {
    return BKE_texture_dependsOnTime(wmd->mask_texture);
  }
  return false;
}

static void foreachIDLink(ModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  WeightVGMixModifierData *wmd = (WeightVGMixModifierData *)md;

  walk(userData, ob, (ID **)&wmd->mask_texture, IDWALK_CB_USER);
  walk(userData, ob, (ID **)&wmd->mask_tex_map_obj, IDWALK_CB_NOP);
}

static void foreachTexLink(ModifierData *md, Object *ob, TexWalkFunc walk, void *userData)
{
  walk(userData, ob, md, "mask_texture");
}

static void updateDepsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  WeightVGMixModifierData *wmd = (WeightVGMixModifierData *)md;
  bool need_transform_relation = false;

  if (wmd->mask_texture != NULL) {
    DEG_add_generic_id_relation(ctx->node, &wmd->mask_texture->id, "WeightVGMix Modifier");

    if (wmd->mask_tex_map_obj != NULL && wmd->mask_tex_mapping == MOD_DISP_MAP_OBJECT) {
      MOD_depsgraph_update_object_bone_relation(
          ctx->node, wmd->mask_tex_map_obj, wmd->mask_tex_map_bone, "WeightVGMix Modifier");
      need_transform_relation = true;
    }
    else if (wmd->mask_tex_mapping == MOD_DISP_MAP_GLOBAL) {
      need_transform_relation = true;
    }
  }

  if (need_transform_relation) {
    DEG_add_depends_on_transform_relation(ctx->node, "WeightVGMix Modifier");
  }
}

static bool isDisabled(const struct Scene *UNUSED(scene),
                       ModifierData *md,
                       bool UNUSED(useRenderParams))
{
  WeightVGMixModifierData *wmd = (WeightVGMixModifierData *)md;
  /* If no vertex group, bypass. */
  return (wmd->defgrp_name_a[0] == '\0');
}

static Mesh *modifyMesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  BLI_assert(mesh != NULL);

  WeightVGMixModifierData *wmd = (WeightVGMixModifierData *)md;

  MDeformVert *dvert = NULL;
  MDeformWeight **dw1, **tdw1, **dw2, **tdw2;
  float *org_w;
  float *new_w;
  int *tidx, *indices = NULL;
  int index_num = 0;
  int i;
  const bool invert_vgroup_mask = (wmd->flag & MOD_WVG_MIX_INVERT_VGROUP_MASK) != 0;
  const bool do_normalize = (wmd->flag & MOD_WVG_MIX_WEIGHTS_NORMALIZE) != 0;

  /*
   * Note that we only invert the weight values within provided vgroups, the selection based on
   * which vertex is affected because it belongs or not to a group remains unchanged.
   * In other words, vertices not belonging to a group won't be affected, even though their
   * inverted 'virtual' weight would be 1.0f.
   */
  const bool invert_vgroup_a = (wmd->flag & MOD_WVG_MIX_INVERT_VGROUP_A) != 0;
  const bool invert_vgroup_b = (wmd->flag & MOD_WVG_MIX_INVERT_VGROUP_B) != 0;

  /* Flags. */
#if 0
  const bool do_prev = (wmd->modifier.mode & eModifierMode_DoWeightPreview) != 0;
#endif

  /* Get number of verts. */
  const int verts_num = mesh->totvert;

  /* Check if we can just return the original mesh.
   * Must have verts and therefore verts assigned to vgroups to do anything useful!
   */
  if ((verts_num == 0) || BLI_listbase_is_empty(&mesh->vertex_group_names)) {
    return mesh;
  }

  /* Get vgroup idx from its name. */
  const int defgrp_index = BKE_id_defgroup_name_index(&mesh->id, wmd->defgrp_name_a);
  if (defgrp_index == -1) {
    return mesh;
  }
  /* Get second vgroup idx from its name, if given. */
  int defgrp_index_other = -1;
  if (wmd->defgrp_name_b[0] != '\0') {
    defgrp_index_other = BKE_id_defgroup_name_index(&mesh->id, wmd->defgrp_name_b);
    if (defgrp_index_other == -1) {
      return mesh;
    }
  }

  const bool has_mdef = CustomData_has_layer(&mesh->vdata, CD_MDEFORMVERT);
  /* If no vertices were ever added to an object's vgroup, dvert might be NULL. */
  if (!has_mdef) {
    /* If not affecting all vertices, just return. */
    if (wmd->mix_set != MOD_WVG_SET_ALL) {
      return mesh;
    }
  }

  if (has_mdef) {
    dvert = CustomData_duplicate_referenced_layer(&mesh->vdata, CD_MDEFORMVERT, verts_num);
  }
  else {
    /* Add a valid data layer! */
    dvert = CustomData_add_layer(&mesh->vdata, CD_MDEFORMVERT, CD_CALLOC, NULL, verts_num);
  }
  /* Ultimate security check. */
  if (!dvert) {
    return mesh;
  }
  mesh->dvert = dvert;

  /* Find out which vertices to work on. */
  tidx = MEM_malloc_arrayN(verts_num, sizeof(int), "WeightVGMix Modifier, tidx");
  tdw1 = MEM_malloc_arrayN(verts_num, sizeof(MDeformWeight *), "WeightVGMix Modifier, tdw1");
  tdw2 = MEM_malloc_arrayN(verts_num, sizeof(MDeformWeight *), "WeightVGMix Modifier, tdw2");
  switch (wmd->mix_set) {
    case MOD_WVG_SET_A:
      /* All vertices in first vgroup. */
      for (i = 0; i < verts_num; i++) {
        MDeformWeight *dw = BKE_defvert_find_index(&dvert[i], defgrp_index);
        if (dw) {
          tdw1[index_num] = dw;
          tdw2[index_num] = (defgrp_index_other >= 0) ?
                                BKE_defvert_find_index(&dvert[i], defgrp_index_other) :
                                NULL;
          tidx[index_num++] = i;
        }
      }
      break;
    case MOD_WVG_SET_B:
      /* All vertices in second vgroup. */
      for (i = 0; i < verts_num; i++) {
        MDeformWeight *dw = (defgrp_index_other >= 0) ?
                                BKE_defvert_find_index(&dvert[i], defgrp_index_other) :
                                NULL;
        if (dw) {
          tdw1[index_num] = BKE_defvert_find_index(&dvert[i], defgrp_index);
          tdw2[index_num] = dw;
          tidx[index_num++] = i;
        }
      }
      break;
    case MOD_WVG_SET_OR:
      /* All vertices in one vgroup or the other. */
      for (i = 0; i < verts_num; i++) {
        MDeformWeight *adw = BKE_defvert_find_index(&dvert[i], defgrp_index);
        MDeformWeight *bdw = (defgrp_index_other >= 0) ?
                                 BKE_defvert_find_index(&dvert[i], defgrp_index_other) :
                                 NULL;
        if (adw || bdw) {
          tdw1[index_num] = adw;
          tdw2[index_num] = bdw;
          tidx[index_num++] = i;
        }
      }
      break;
    case MOD_WVG_SET_AND:
      /* All vertices in both vgroups. */
      for (i = 0; i < verts_num; i++) {
        MDeformWeight *adw = BKE_defvert_find_index(&dvert[i], defgrp_index);
        MDeformWeight *bdw = (defgrp_index_other >= 0) ?
                                 BKE_defvert_find_index(&dvert[i], defgrp_index_other) :
                                 NULL;
        if (adw && bdw) {
          tdw1[index_num] = adw;
          tdw2[index_num] = bdw;
          tidx[index_num++] = i;
        }
      }
      break;
    case MOD_WVG_SET_ALL:
    default:
      /* Use all vertices. */
      for (i = 0; i < verts_num; i++) {
        tdw1[i] = BKE_defvert_find_index(&dvert[i], defgrp_index);
        tdw2[i] = (defgrp_index_other >= 0) ?
                      BKE_defvert_find_index(&dvert[i], defgrp_index_other) :
                      NULL;
      }
      index_num = -1;
      break;
  }
  if (index_num == 0) {
    /* Use no vertices! Hence, return org data. */
    MEM_freeN(tdw1);
    MEM_freeN(tdw2);
    MEM_freeN(tidx);
    return mesh;
  }
  if (index_num != -1) {
    indices = MEM_malloc_arrayN(index_num, sizeof(int), "WeightVGMix Modifier, indices");
    memcpy(indices, tidx, sizeof(int) * index_num);
    dw1 = MEM_malloc_arrayN(index_num, sizeof(MDeformWeight *), "WeightVGMix Modifier, dw1");
    memcpy(dw1, tdw1, sizeof(MDeformWeight *) * index_num);
    MEM_freeN(tdw1);
    dw2 = MEM_malloc_arrayN(index_num, sizeof(MDeformWeight *), "WeightVGMix Modifier, dw2");
    memcpy(dw2, tdw2, sizeof(MDeformWeight *) * index_num);
    MEM_freeN(tdw2);
  }
  else {
    /* Use all vertices. */
    index_num = verts_num;
    /* Just copy MDeformWeight pointers arrays, they will be freed at the end. */
    dw1 = tdw1;
    dw2 = tdw2;
  }
  MEM_freeN(tidx);

  org_w = MEM_malloc_arrayN(index_num, sizeof(float), "WeightVGMix Modifier, org_w");
  new_w = MEM_malloc_arrayN(index_num, sizeof(float), "WeightVGMix Modifier, new_w");

  /* Mix weights. */
  for (i = 0; i < index_num; i++) {
    float weight2;
    if (invert_vgroup_a) {
      org_w[i] = 1.0f - (dw1[i] ? dw1[i]->weight : wmd->default_weight_a);
    }
    else {
      org_w[i] = dw1[i] ? dw1[i]->weight : wmd->default_weight_a;
    }
    if (invert_vgroup_b) {
      weight2 = 1.0f - (dw2[i] ? dw2[i]->weight : wmd->default_weight_b);
    }
    else {
      weight2 = dw2[i] ? dw2[i]->weight : wmd->default_weight_b;
    }

    new_w[i] = mix_weight(org_w[i], weight2, wmd->mix_mode);
  }

  /* Do masking. */
  struct Scene *scene = DEG_get_evaluated_scene(ctx->depsgraph);
  weightvg_do_mask(ctx,
                   index_num,
                   indices,
                   org_w,
                   new_w,
                   ctx->object,
                   mesh,
                   wmd->mask_constant,
                   wmd->mask_defgrp_name,
                   scene,
                   wmd->mask_texture,
                   wmd->mask_tex_use_channel,
                   wmd->mask_tex_mapping,
                   wmd->mask_tex_map_obj,
                   wmd->mask_tex_map_bone,
                   wmd->mask_tex_uvlayer_name,
                   invert_vgroup_mask);

  /* Update (add to) vgroup.
   * XXX Depending on the MOD_WVG_SET_xxx option chosen, we might have to add vertices to vgroup.
   */
  weightvg_update_vg(dvert,
                     defgrp_index,
                     dw1,
                     index_num,
                     indices,
                     org_w,
                     true,
                     -FLT_MAX,
                     false,
                     0.0f,
                     do_normalize);

  /* If weight preview enabled... */
#if 0 /* XXX Currently done in mod stack :/ */
  if (do_prev) {
    DM_update_weight_mcol(ob, dm, 0, org_w, index_num, indices);
  }
#endif

  /* Freeing stuff. */
  MEM_freeN(org_w);
  MEM_freeN(new_w);
  MEM_freeN(dw1);
  MEM_freeN(dw2);
  MEM_SAFE_FREE(indices);

  mesh->runtime.is_original_bmesh = false;

  /* Return the vgroup-modified mesh. */
  return mesh;
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayoutSetPropSep(layout, true);

  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group_a", "invert_vertex_group_a", NULL);
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group_b", "invert_vertex_group_b", IFACE_("B"));

  uiItemS(layout);

  uiItemR(layout, ptr, "default_weight_a", 0, NULL, ICON_NONE);
  uiItemR(layout, ptr, "default_weight_b", 0, IFACE_("B"), ICON_NONE);

  uiItemS(layout);

  uiItemR(layout, ptr, "mix_set", 0, NULL, ICON_NONE);
  uiItemR(layout, ptr, "mix_mode", 0, NULL, ICON_NONE);

  uiItemR(layout, ptr, "normalize", 0, NULL, ICON_NONE);

  modifier_panel_end(layout, ptr);
}

static void influence_panel_draw(const bContext *C, Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  weightvg_ui_common(C, &ob_ptr, ptr, layout);
}

static void panelRegister(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(
      region_type, eModifierType_WeightVGMix, panel_draw);
  modifier_subpanel_register(
      region_type, "influence", "Influence", NULL, influence_panel_draw, panel_type);
}

ModifierTypeInfo modifierType_WeightVGMix = {
    /* name */ N_("VertexWeightMix"),
    /* structName */ "WeightVGMixModifierData",
    /* structSize */ sizeof(WeightVGMixModifierData),
    /* srna */ &RNA_VertexWeightMixModifier,
    /* type */ eModifierTypeType_NonGeometrical,
    /* flags */ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
        eModifierTypeFlag_SupportsEditmode | eModifierTypeFlag_UsesPreview,
    /* icon */ ICON_MOD_VERTEX_WEIGHT,

    /* copyData */ BKE_modifier_copydata_generic,

    /* deformVerts */ NULL,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ NULL,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ modifyMesh,
    /* modifyGeometrySet */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ requiredDataMask,
    /* freeData */ NULL,
    /* isDisabled */ isDisabled,
    /* updateDepsgraph */ updateDepsgraph,
    /* dependsOnTime */ dependsOnTime,
    /* dependsOnNormals */ NULL,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ foreachTexLink,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ NULL,
    /* blendRead */ NULL,
};
