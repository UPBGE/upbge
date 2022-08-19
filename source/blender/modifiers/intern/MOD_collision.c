/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup modifiers
 */

#include "BLI_utildefines.h"

#include "BLI_kdopbvh.h"
#include "BLI_math.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_force_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "MEM_guardedalloc.h"

#include "BKE_collision.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_modifier.h"
#include "BKE_pointcache.h"
#include "BKE_scene.h"
#include "BKE_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "MOD_modifiertypes.h"
#include "MOD_ui_common.h"
#include "MOD_util.h"

#include "BLO_read_write.h"

#include "DEG_depsgraph_query.h"

static void initData(ModifierData *md)
{
  CollisionModifierData *collmd = (CollisionModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(collmd, modifier));

  MEMCPY_STRUCT_AFTER(collmd, DNA_struct_default_get(CollisionModifierData), modifier);
}

static void freeData(ModifierData *md)
{
  CollisionModifierData *collmd = (CollisionModifierData *)md;

  if (collmd) { /* Seriously? */
    if (collmd->bvhtree) {
      BLI_bvhtree_free(collmd->bvhtree);
      collmd->bvhtree = NULL;
    }

    MEM_SAFE_FREE(collmd->x);
    MEM_SAFE_FREE(collmd->xnew);
    MEM_SAFE_FREE(collmd->current_x);
    MEM_SAFE_FREE(collmd->current_xnew);
    MEM_SAFE_FREE(collmd->current_v);

    MEM_SAFE_FREE(collmd->tri);

    collmd->time_x = collmd->time_xnew = -1000;
    collmd->mvert_num = 0;
    collmd->tri_num = 0;
    collmd->is_static = false;
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
  CollisionModifierData *collmd = (CollisionModifierData *)md;
  Mesh *mesh_src;
  MVert *tempVert = NULL;
  Object *ob = ctx->object;

  /* If collision is disabled, free the stale data and exit. */
  if (!ob->pd || !ob->pd->deflect) {
    if (!ob->pd) {
      printf("CollisionModifier: collision settings are missing!\n");
    }

    freeData(md);
    return;
  }

  if (mesh == NULL) {
    mesh_src = MOD_deform_mesh_eval_get(ob, NULL, NULL, NULL, verts_num, false);
  }
  else {
    /* Not possible to use get_mesh() in this case as we'll modify its vertices
     * and get_mesh() would return 'mesh' directly. */
    mesh_src = (Mesh *)BKE_id_copy_ex(NULL, (ID *)mesh, NULL, LIB_ID_COPY_LOCALIZE);
  }

  if (mesh_src) {
    float current_time = 0;
    uint mvert_num = 0;

    BKE_mesh_vert_coords_apply(mesh_src, vertexCos);

    current_time = DEG_get_ctime(ctx->depsgraph);

    if (G.debug & G_DEBUG_SIMDATA) {
      printf("current_time %f, collmd->time_xnew %f\n", current_time, collmd->time_xnew);
    }

    mvert_num = mesh_src->totvert;

    if (current_time < collmd->time_xnew) {
      freeData((ModifierData *)collmd);
    }
    else if (current_time == collmd->time_xnew) {
      if (mvert_num != collmd->mvert_num) {
        freeData((ModifierData *)collmd);
      }
    }

    /* check if mesh has changed */
    if (collmd->x && (mvert_num != collmd->mvert_num)) {
      freeData((ModifierData *)collmd);
    }

    if (collmd->time_xnew == -1000) { /* first time */

      collmd->x = MEM_dupallocN(mesh_src->mvert); /* frame start position */

      for (uint i = 0; i < mvert_num; i++) {
        /* we save global positions */
        mul_m4_v3(ob->obmat, collmd->x[i].co);
      }

      collmd->xnew = MEM_dupallocN(collmd->x);         /* Frame end position. */
      collmd->current_x = MEM_dupallocN(collmd->x);    /* Inter-frame. */
      collmd->current_xnew = MEM_dupallocN(collmd->x); /* Inter-frame. */
      collmd->current_v = MEM_dupallocN(collmd->x);    /* Inter-frame. */

      collmd->mvert_num = mvert_num;

      {
        const MLoop *mloop = mesh_src->mloop;
        const MLoopTri *looptri = BKE_mesh_runtime_looptri_ensure(mesh_src);
        collmd->tri_num = BKE_mesh_runtime_looptri_len(mesh_src);
        MVertTri *tri = MEM_mallocN(sizeof(*tri) * collmd->tri_num, __func__);
        BKE_mesh_runtime_verttri_from_looptri(tri, mloop, looptri, collmd->tri_num);
        collmd->tri = tri;
      }

      /* create bounding box hierarchy */
      collmd->bvhtree = bvhtree_build_from_mvert(
          collmd->x, collmd->tri, collmd->tri_num, ob->pd->pdef_sboft);

      collmd->time_x = collmd->time_xnew = current_time;
      collmd->is_static = true;
    }
    else if (mvert_num == collmd->mvert_num) {
      /* put positions to old positions */
      tempVert = collmd->x;
      collmd->x = collmd->xnew;
      collmd->xnew = tempVert;
      collmd->time_x = collmd->time_xnew;

      memcpy(collmd->xnew, mesh_src->mvert, mvert_num * sizeof(MVert));

      bool is_static = true;

      for (uint i = 0; i < mvert_num; i++) {
        /* we save global positions */
        mul_m4_v3(ob->obmat, collmd->xnew[i].co);

        /* detect motion */
        is_static = is_static && equals_v3v3(collmd->x[i].co, collmd->xnew[i].co);
      }

      memcpy(collmd->current_xnew, collmd->x, mvert_num * sizeof(MVert));
      memcpy(collmd->current_x, collmd->x, mvert_num * sizeof(MVert));

      /* check if GUI setting has changed for bvh */
      if (collmd->bvhtree) {
        if (ob->pd->pdef_sboft != BLI_bvhtree_get_epsilon(collmd->bvhtree)) {
          BLI_bvhtree_free(collmd->bvhtree);
          collmd->bvhtree = bvhtree_build_from_mvert(
              collmd->current_x, collmd->tri, collmd->tri_num, ob->pd->pdef_sboft);
        }
      }

      /* Happens on file load (ONLY when I un-comment changes in readfile.c) */
      if (!collmd->bvhtree) {
        collmd->bvhtree = bvhtree_build_from_mvert(
            collmd->current_x, collmd->tri, collmd->tri_num, ob->pd->pdef_sboft);
      }
      else if (!collmd->is_static || !is_static) {
        /* recalc static bounding boxes */
        bvhtree_update_from_mvert(collmd->bvhtree,
                                  collmd->current_x,
                                  collmd->current_xnew,
                                  collmd->tri,
                                  collmd->tri_num,
                                  true);
      }

      collmd->is_static = is_static;
      collmd->time_xnew = current_time;
    }
    else if (mvert_num != collmd->mvert_num) {
      freeData((ModifierData *)collmd);
    }
  }

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static void updateDepsgraph(ModifierData *UNUSED(md), const ModifierUpdateDepsgraphContext *ctx)
{
  DEG_add_depends_on_transform_relation(ctx->node, "Collision Modifier");
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
  modifier_panel_register(region_type, eModifierType_Collision, panel_draw);
}

static void blendRead(BlendDataReader *UNUSED(reader), ModifierData *md)
{
  CollisionModifierData *collmd = (CollisionModifierData *)md;
#if 0
      /* TODO: #CollisionModifier should use point-cache
       * + have proper reset events before enabling this. */
      collmd->x = newdataadr(fd, collmd->x);
      collmd->xnew = newdataadr(fd, collmd->xnew);
      collmd->mfaces = newdataadr(fd, collmd->mfaces);

      collmd->current_x = MEM_calloc_arrayN(collmd->mvert_num, sizeof(MVert), "current_x");
      collmd->current_xnew = MEM_calloc_arrayN(collmd->mvert_num, sizeof(MVert), "current_xnew");
      collmd->current_v = MEM_calloc_arrayN(collmd->mvert_num, sizeof(MVert), "current_v");
#endif

  collmd->x = NULL;
  collmd->xnew = NULL;
  collmd->current_x = NULL;
  collmd->current_xnew = NULL;
  collmd->current_v = NULL;
  collmd->time_x = collmd->time_xnew = -1000;
  collmd->mvert_num = 0;
  collmd->tri_num = 0;
  collmd->is_static = false;
  collmd->bvhtree = NULL;
  collmd->tri = NULL;
}

ModifierTypeInfo modifierType_Collision = {
    /* name */ N_("Collision"),
    /* structName */ "CollisionModifierData",
    /* structSize */ sizeof(CollisionModifierData),
    /* srna */ &RNA_CollisionModifier,
    /* type */ eModifierTypeType_OnlyDeform,
    /* flags */ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_Single,
    /* icon */ ICON_MOD_PHYSICS,

    /* copyData */ NULL,

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
    /* updateDepsgraph */ updateDepsgraph,
    /* dependsOnTime */ dependsOnTime,
    /* dependsOnNormals */ NULL,
    /* foreachIDLink */ NULL,
    /* foreachTexLink */ NULL,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ NULL,
    /* blendRead */ blendRead,
};
