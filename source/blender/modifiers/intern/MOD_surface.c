/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup modifiers
 */

#include "BLI_utildefines.h"

#include "BLI_math.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_bvhutils.h"
#include "BKE_context.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "MOD_modifiertypes.h"
#include "MOD_ui_common.h"
#include "MOD_util.h"

#include "BLO_read_write.h"

#include "MEM_guardedalloc.h"

static void initData(ModifierData *md)
{
  SurfaceModifierData *surmd = (SurfaceModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(surmd, modifier));

  MEMCPY_STRUCT_AFTER(surmd, DNA_struct_default_get(SurfaceModifierData), modifier);
}

static void copyData(const ModifierData *md_src, ModifierData *md_dst, const int flag)
{
  SurfaceModifierData *surmd_dst = (SurfaceModifierData *)md_dst;

  BKE_modifier_copydata_generic(md_src, md_dst, flag);

  surmd_dst->bvhtree = NULL;
  surmd_dst->mesh = NULL;
  surmd_dst->x = NULL;
  surmd_dst->v = NULL;
}

static void freeData(ModifierData *md)
{
  SurfaceModifierData *surmd = (SurfaceModifierData *)md;

  if (surmd) {
    if (surmd->bvhtree) {
      free_bvhtree_from_mesh(surmd->bvhtree);
      MEM_SAFE_FREE(surmd->bvhtree);
    }

    if (surmd->mesh) {
      BKE_id_free(NULL, surmd->mesh);
      surmd->mesh = NULL;
    }

    MEM_SAFE_FREE(surmd->x);

    MEM_SAFE_FREE(surmd->v);
  }
}

static bool dependsOnTime(struct Scene *UNUSED(scene), ModifierData *UNUSED(md))
{
  return true;
}

static void deformVerts(ModifierData *md,
                        const ModifierEvalContext *ctx,
                        Mesh *mesh,
                        float (*vertexCos)[3],
                        int verts_num)
{
  SurfaceModifierData *surmd = (SurfaceModifierData *)md;
  const int cfra = (int)DEG_get_ctime(ctx->depsgraph);

  /* Free mesh and BVH cache. */
  if (surmd->bvhtree) {
    free_bvhtree_from_mesh(surmd->bvhtree);
    MEM_SAFE_FREE(surmd->bvhtree);
  }

  if (surmd->mesh) {
    BKE_id_free(NULL, surmd->mesh);
    surmd->mesh = NULL;
  }

  if (mesh) {
    /* Not possible to use get_mesh() in this case as we'll modify its vertices
     * and get_mesh() would return 'mesh' directly. */
    surmd->mesh = (Mesh *)BKE_id_copy_ex(NULL, (ID *)mesh, NULL, LIB_ID_COPY_LOCALIZE);
  }
  else {
    surmd->mesh = MOD_deform_mesh_eval_get(ctx->object, NULL, NULL, NULL, verts_num, false);
  }

  if (!ctx->object->pd) {
    printf("SurfaceModifier deformVerts: Should not happen!\n");
    return;
  }

  if (surmd->mesh) {
    uint mesh_verts_num = 0, i = 0;
    int init = 0;
    float *vec;
    MVert *x, *v;

    BKE_mesh_vert_coords_apply(surmd->mesh, vertexCos);

    mesh_verts_num = surmd->mesh->totvert;

    if (mesh_verts_num != surmd->verts_num || surmd->x == NULL || surmd->v == NULL ||
        cfra != surmd->cfra + 1) {
      if (surmd->x) {
        MEM_freeN(surmd->x);
        surmd->x = NULL;
      }
      if (surmd->v) {
        MEM_freeN(surmd->v);
        surmd->v = NULL;
      }

      surmd->x = MEM_calloc_arrayN(mesh_verts_num, sizeof(MVert), "MVert");
      surmd->v = MEM_calloc_arrayN(mesh_verts_num, sizeof(MVert), "MVert");

      surmd->verts_num = mesh_verts_num;

      init = 1;
    }

    /* convert to global coordinates and calculate velocity */
    for (i = 0, x = surmd->x, v = surmd->v; i < mesh_verts_num; i++, x++, v++) {
      vec = surmd->mesh->mvert[i].co;
      mul_m4_v3(ctx->object->obmat, vec);

      if (init) {
        v->co[0] = v->co[1] = v->co[2] = 0.0f;
      }
      else {
        sub_v3_v3v3(v->co, vec, x->co);
      }

      copy_v3_v3(x->co, vec);
    }

    surmd->cfra = cfra;

    const bool has_poly = surmd->mesh->totpoly > 0;
    const bool has_edge = surmd->mesh->totedge > 0;
    if (has_poly || has_edge) {
      surmd->bvhtree = MEM_callocN(sizeof(BVHTreeFromMesh), "BVHTreeFromMesh");

      if (has_poly) {
        BKE_bvhtree_from_mesh_get(surmd->bvhtree, surmd->mesh, BVHTREE_FROM_LOOPTRI, 2);
      }
      else if (has_edge) {
        BKE_bvhtree_from_mesh_get(surmd->bvhtree, surmd->mesh, BVHTREE_FROM_EDGES, 2);
      }
    }
  }
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, NULL);

  uiItemL(layout, TIP_("Settings are inside the Physics tab"), ICON_NONE);

  modifier_panel_end(layout, ptr);
}

static void panelRegister(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Surface, panel_draw);
}

static void blendRead(BlendDataReader *UNUSED(reader), ModifierData *md)
{
  SurfaceModifierData *surmd = (SurfaceModifierData *)md;

  surmd->mesh = NULL;
  surmd->bvhtree = NULL;
  surmd->x = NULL;
  surmd->v = NULL;
  surmd->verts_num = 0;
}

ModifierTypeInfo modifierType_Surface = {
    /* name */ N_("Surface"),
    /* structName */ "SurfaceModifierData",
    /* structSize */ sizeof(SurfaceModifierData),
    /* srna */ &RNA_SurfaceModifier,
    /* type */ eModifierTypeType_OnlyDeform,
    /* flags */ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_AcceptsCVs |
        eModifierTypeFlag_NoUserAdd,
    /* icon */ ICON_MOD_PHYSICS,

    /* copyData */ copyData,

    /* deformVerts */ deformVerts,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ NULL,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ NULL,
    /* modifyGeometrySet */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ NULL,
    /* freeData */ freeData,
    /* isDisabled */ NULL,
    /* updateDepsgraph */ NULL,
    /* dependsOnTime */ dependsOnTime,
    /* dependsOnNormals */ NULL,
    /* foreachIDLink */ NULL,
    /* foreachTexLink */ NULL,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ NULL,
    /* blendRead */ blendRead,
};
