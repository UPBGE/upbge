/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup modifiers
 *
 * Edge Split modifier
 *
 * Splits edges in the mesh according to sharpness flag
 * or edge angle (can be used to achieve auto-smoothing)
 */

#include "BLI_utildefines.h"

#include "BLI_math.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.h"
#include "BKE_mesh.h"
#include "BKE_modifier.h"
#include "BKE_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "bmesh.h"
#include "bmesh_tools.h"

#include "MOD_modifiertypes.h"
#include "MOD_ui_common.h"

/* For edge split modifier node. */
Mesh *doEdgeSplit(const Mesh *mesh, EdgeSplitModifierData *emd);

Mesh *doEdgeSplit(const Mesh *mesh, EdgeSplitModifierData *emd)
{
  Mesh *result;
  BMesh *bm;
  BMIter iter;
  BMEdge *e;
  const float threshold = cosf(emd->split_angle + 0.000000175f);
  const bool do_split_angle = (emd->flags & MOD_EDGESPLIT_FROMANGLE) != 0 &&
                              emd->split_angle < (float)M_PI;
  const bool do_split_all = do_split_angle && emd->split_angle < FLT_EPSILON;
  const bool calc_face_normals = do_split_angle && !do_split_all;

  bm = BKE_mesh_to_bmesh_ex(mesh,
                            &(struct BMeshCreateParams){0},
                            &(struct BMeshFromMeshParams){
                                .calc_face_normal = calc_face_normals,
                                .calc_vert_normal = false,
                                .add_key_index = false,
                                .use_shapekey = false,
                                .active_shapekey = 0,
                                .cd_mask_extra = {.vmask = CD_MASK_ORIGINDEX,
                                                  .emask = CD_MASK_ORIGINDEX,
                                                  .pmask = CD_MASK_ORIGINDEX},
                            });

  if (do_split_angle) {
    BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
      /* check for 1 edge having 2 face users */
      BMLoop *l1, *l2;
      if ((l1 = e->l) && (l2 = e->l->radial_next) != l1) {
        if (/* 3+ faces on this edge, always split */
            UNLIKELY(l1 != l2->radial_next) ||
            /* O° angle setting, we want to split on all edges. */
            do_split_all ||
            /* 2 face edge - check angle. */
            (dot_v3v3(l1->f->no, l2->f->no) < threshold)) {
          BM_elem_flag_enable(e, BM_ELEM_TAG);
        }
      }
    }
  }

  if (emd->flags & MOD_EDGESPLIT_FROMFLAG) {
    BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
      /* check for 2 or more edge users */
      if ((e->l) && (e->l->next != e->l)) {
        if (!BM_elem_flag_test(e, BM_ELEM_SMOOTH)) {
          BM_elem_flag_enable(e, BM_ELEM_TAG);
        }
      }
    }
  }

  BM_mesh_edgesplit(bm, false, true, false);

  /* Uncomment for troubleshooting. */
  // BM_mesh_validate(bm);

  result = BKE_mesh_from_bmesh_for_eval_nomain(bm, NULL, mesh);
  BM_mesh_free(bm);

  return result;
}

static void initData(ModifierData *md)
{
  EdgeSplitModifierData *emd = (EdgeSplitModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(emd, modifier));

  MEMCPY_STRUCT_AFTER(emd, DNA_struct_default_get(EdgeSplitModifierData), modifier);
}

static Mesh *modifyMesh(ModifierData *md, const ModifierEvalContext *UNUSED(ctx), Mesh *mesh)
{
  Mesh *result;
  EdgeSplitModifierData *emd = (EdgeSplitModifierData *)md;

  if (!(emd->flags & (MOD_EDGESPLIT_FROMANGLE | MOD_EDGESPLIT_FROMFLAG))) {
    return mesh;
  }

  result = doEdgeSplit(mesh, emd);

  return result;
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *row, *sub;
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, NULL);

  uiLayoutSetPropSep(layout, true);

  row = uiLayoutRowWithHeading(layout, true, IFACE_("Edge Angle"));
  uiItemR(row, ptr, "use_edge_angle", 0, "", ICON_NONE);
  sub = uiLayoutRow(row, true);
  uiLayoutSetActive(sub, RNA_boolean_get(ptr, "use_edge_angle"));
  uiItemR(sub, ptr, "split_angle", 0, "", ICON_NONE);

  uiItemR(layout, ptr, "use_edge_sharp", 0, IFACE_("Sharp Edges"), ICON_NONE);

  modifier_panel_end(layout, ptr);
}

static void panelRegister(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_EdgeSplit, panel_draw);
}

ModifierTypeInfo modifierType_EdgeSplit = {
    /* name */ N_("EdgeSplit"),
    /* structName */ "EdgeSplitModifierData",
    /* structSize */ sizeof(EdgeSplitModifierData),
    /* srna */ &RNA_EdgeSplitModifier,
    /* type */ eModifierTypeType_Constructive,
    /* flags */ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_AcceptsCVs |
        eModifierTypeFlag_SupportsMapping | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /* icon */ ICON_MOD_EDGESPLIT,

    /* copyData */ BKE_modifier_copydata_generic,

    /* deformVerts */ NULL,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ NULL,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ modifyMesh,
    /* modifyGeometrySet */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ NULL,
    /* freeData */ NULL,
    /* isDisabled */ NULL,
    /* updateDepsgraph */ NULL,
    /* dependsOnTime */ NULL,
    /* dependsOnNormals */ NULL,
    /* foreachIDLink */ NULL,
    /* foreachTexLink */ NULL,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ NULL,
    /* blendRead */ NULL,
};
