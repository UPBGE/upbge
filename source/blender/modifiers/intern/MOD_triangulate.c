/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
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

static Mesh *triangulate_mesh(Mesh *mesh,
                              const int quad_method,
                              const int ngon_method,
                              const int min_vertices,
                              const int flag)
{
  Mesh *result;
  BMesh *bm;
  int edges_num, i;
  MEdge *me;
  CustomData_MeshMasks cd_mask_extra = {
      .vmask = CD_MASK_ORIGINDEX, .emask = CD_MASK_ORIGINDEX, .pmask = CD_MASK_ORIGINDEX};

  bool keep_clnors = (flag & MOD_TRIANGULATE_KEEP_CUSTOMLOOP_NORMALS) != 0;

  if (keep_clnors) {
    BKE_mesh_calc_normals_split(mesh);
    /* We need that one to 'survive' to/from BMesh conversions. */
    CustomData_clear_layer_flag(&mesh->ldata, CD_NORMAL, CD_FLAG_TEMPORARY);
    cd_mask_extra.lmask |= CD_MASK_NORMAL;
  }

  bm = BKE_mesh_to_bmesh_ex(mesh,
                            &((struct BMeshCreateParams){0}),
                            &((struct BMeshFromMeshParams){
                                .calc_face_normal = true,
                                .calc_vert_normal = false,
                                .cd_mask_extra = cd_mask_extra,
                            }));

  BM_mesh_triangulate(bm, quad_method, ngon_method, min_vertices, false, NULL, NULL, NULL);

  result = BKE_mesh_from_bmesh_for_eval_nomain(bm, &cd_mask_extra, mesh);
  BM_mesh_free(bm);

  if (keep_clnors) {
    float(*lnors)[3] = CustomData_get_layer(&result->ldata, CD_NORMAL);
    BLI_assert(lnors != NULL);

    BKE_mesh_set_custom_normals(result, lnors);

    /* Do some cleanup, we do not want those temp data to stay around. */
    CustomData_set_layer_flag(&mesh->ldata, CD_NORMAL, CD_FLAG_TEMPORARY);
    CustomData_set_layer_flag(&result->ldata, CD_NORMAL, CD_FLAG_TEMPORARY);
  }

  edges_num = result->totedge;
  me = result->medge;

  /* force drawing of all edges (seems to be omitted in CDDM_from_bmesh) */
  for (i = 0; i < edges_num; i++, me++) {
    me->flag |= ME_EDGEDRAW | ME_EDGERENDER;
  }

  return result;
}

static void initData(ModifierData *md)
{
  TriangulateModifierData *tmd = (TriangulateModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(tmd, modifier));

  MEMCPY_STRUCT_AFTER(tmd, DNA_struct_default_get(TriangulateModifierData), modifier);

  /* Enable in editmode by default */
  md->mode |= eModifierMode_Editmode;
}

static Mesh *modifyMesh(ModifierData *md, const ModifierEvalContext *UNUSED(ctx), Mesh *mesh)
{
  TriangulateModifierData *tmd = (TriangulateModifierData *)md;
  Mesh *result;
  if (!(result = triangulate_mesh(
            mesh, tmd->quad_method, tmd->ngon_method, tmd->min_vertices, tmd->flag))) {
    return mesh;
  }

  return result;
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "quad_method", 0, NULL, ICON_NONE);
  uiItemR(layout, ptr, "ngon_method", 0, NULL, ICON_NONE);
  uiItemR(layout, ptr, "min_vertices", 0, NULL, ICON_NONE);
  uiItemR(layout, ptr, "keep_custom_normals", 0, NULL, ICON_NONE);

  modifier_panel_end(layout, ptr);
}

static void panelRegister(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Triangulate, panel_draw);
}

ModifierTypeInfo modifierType_Triangulate = {
    /* name */ N_("Triangulate"),
    /* structName */ "TriangulateModifierData",
    /* structSize */ sizeof(TriangulateModifierData),
    /* srna */ &RNA_TriangulateModifier,
    /* type */ eModifierTypeType_Constructive,
    /* flags */ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_SupportsMapping | eModifierTypeFlag_EnableInEditmode |
        eModifierTypeFlag_AcceptsCVs,
    /* icon */ ICON_MOD_TRIANGULATE,

    /* copyData */ BKE_modifier_copydata_generic,

    /* deformVerts */ NULL,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ NULL,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ modifyMesh,
    /* modifyGeometrySet */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ NULL,  // requiredDataMask,
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
