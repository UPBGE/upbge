/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup modifiers
 */

#include "BLI_utildefines.h"

#include "BLI_math.h"
#include "BLI_task.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_deform.h"
#include "BKE_editmesh.h"
#include "BKE_image.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_mesh.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_screen.h"
#include "BKE_texture.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "MEM_guardedalloc.h"

#include "MOD_ui_common.h"
#include "MOD_util.h"

#include "RE_texture.h"

/* Displace */

static void initData(ModifierData *md)
{
  DisplaceModifierData *dmd = (DisplaceModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(dmd, modifier));

  MEMCPY_STRUCT_AFTER(dmd, DNA_struct_default_get(DisplaceModifierData), modifier);
}

static void requiredDataMask(Object *UNUSED(ob),
                             ModifierData *md,
                             CustomData_MeshMasks *r_cddata_masks)
{
  DisplaceModifierData *dmd = (DisplaceModifierData *)md;

  /* ask for vertexgroups if we need them */
  if (dmd->defgrp_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }

  /* ask for UV coordinates if we need them */
  if (dmd->texmapping == MOD_DISP_MAP_UV) {
    r_cddata_masks->fmask |= CD_MASK_MTFACE;
  }

  if (dmd->direction == MOD_DISP_DIR_CLNOR) {
    r_cddata_masks->lmask |= CD_MASK_CUSTOMLOOPNORMAL;
  }
}

static bool dependsOnTime(struct Scene *UNUSED(scene), ModifierData *md)
{
  DisplaceModifierData *dmd = (DisplaceModifierData *)md;

  if (dmd->texture) {
    return BKE_texture_dependsOnTime(dmd->texture);
  }

  return false;
}

static bool dependsOnNormals(ModifierData *md)
{
  DisplaceModifierData *dmd = (DisplaceModifierData *)md;
  return ELEM(dmd->direction, MOD_DISP_DIR_NOR, MOD_DISP_DIR_CLNOR);
}

static void foreachIDLink(ModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  DisplaceModifierData *dmd = (DisplaceModifierData *)md;

  walk(userData, ob, (ID **)&dmd->texture, IDWALK_CB_USER);
  walk(userData, ob, (ID **)&dmd->map_object, IDWALK_CB_NOP);
}

static void foreachTexLink(ModifierData *md, Object *ob, TexWalkFunc walk, void *userData)
{
  walk(userData, ob, md, "texture");
}

static bool isDisabled(const struct Scene *UNUSED(scene),
                       ModifierData *md,
                       bool UNUSED(useRenderParams))
{
  DisplaceModifierData *dmd = (DisplaceModifierData *)md;
  return ((!dmd->texture && dmd->direction == MOD_DISP_DIR_RGB_XYZ) || dmd->strength == 0.0f);
}

static void updateDepsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  DisplaceModifierData *dmd = (DisplaceModifierData *)md;
  bool need_transform_relation = false;

  if (dmd->space == MOD_DISP_SPACE_GLOBAL &&
      ELEM(dmd->direction, MOD_DISP_DIR_X, MOD_DISP_DIR_Y, MOD_DISP_DIR_Z, MOD_DISP_DIR_RGB_XYZ)) {
    need_transform_relation = true;
  }

  if (dmd->texture != NULL) {
    DEG_add_generic_id_relation(ctx->node, &dmd->texture->id, "Displace Modifier");

    if (dmd->map_object != NULL && dmd->texmapping == MOD_DISP_MAP_OBJECT) {
      MOD_depsgraph_update_object_bone_relation(
          ctx->node, dmd->map_object, dmd->map_bone, "Displace Modifier");
      need_transform_relation = true;
    }
    if (dmd->texmapping == MOD_DISP_MAP_GLOBAL) {
      need_transform_relation = true;
    }
  }

  if (need_transform_relation) {
    DEG_add_depends_on_transform_relation(ctx->node, "Displace Modifier");
  }
}

typedef struct DisplaceUserdata {
  /*const*/ DisplaceModifierData *dmd;
  struct Scene *scene;
  struct ImagePool *pool;
  MDeformVert *dvert;
  float weight;
  int defgrp_index;
  int direction;
  bool use_global_direction;
  Tex *tex_target;
  float (*tex_co)[3];
  float (*vertexCos)[3];
  float local_mat[4][4];
  MVert *mvert;
  const float (*vert_normals)[3];
  float (*vert_clnors)[3];
} DisplaceUserdata;

static void displaceModifier_do_task(void *__restrict userdata,
                                     const int iter,
                                     const TaskParallelTLS *__restrict UNUSED(tls))
{
  DisplaceUserdata *data = (DisplaceUserdata *)userdata;
  DisplaceModifierData *dmd = data->dmd;
  MDeformVert *dvert = data->dvert;
  const bool invert_vgroup = (dmd->flag & MOD_DISP_INVERT_VGROUP) != 0;
  float weight = data->weight;
  int defgrp_index = data->defgrp_index;
  int direction = data->direction;
  bool use_global_direction = data->use_global_direction;
  float(*tex_co)[3] = data->tex_co;
  float(*vertexCos)[3] = data->vertexCos;
  float(*vert_clnors)[3] = data->vert_clnors;

  const float delta_fixed = 1.0f -
                            dmd->midlevel; /* when no texture is used, we fallback to white */

  TexResult texres;
  float strength = dmd->strength;
  float delta;
  float local_vec[3];

  if (dvert) {
    weight = invert_vgroup ? 1.0f - BKE_defvert_find_weight(dvert + iter, defgrp_index) :
                             BKE_defvert_find_weight(dvert + iter, defgrp_index);
    if (weight == 0.0f) {
      return;
    }
  }

  if (data->tex_target) {
    BKE_texture_get_value_ex(
        data->scene, data->tex_target, tex_co[iter], &texres, data->pool, false);
    delta = texres.tin - dmd->midlevel;
  }
  else {
    delta = delta_fixed; /* (1.0f - dmd->midlevel) */ /* never changes */
  }

  if (dvert) {
    strength *= weight;
  }

  delta *= strength;
  CLAMP(delta, -10000, 10000);

  switch (direction) {
    case MOD_DISP_DIR_X:
      if (use_global_direction) {
        vertexCos[iter][0] += delta * data->local_mat[0][0];
        vertexCos[iter][1] += delta * data->local_mat[1][0];
        vertexCos[iter][2] += delta * data->local_mat[2][0];
      }
      else {
        vertexCos[iter][0] += delta;
      }
      break;
    case MOD_DISP_DIR_Y:
      if (use_global_direction) {
        vertexCos[iter][0] += delta * data->local_mat[0][1];
        vertexCos[iter][1] += delta * data->local_mat[1][1];
        vertexCos[iter][2] += delta * data->local_mat[2][1];
      }
      else {
        vertexCos[iter][1] += delta;
      }
      break;
    case MOD_DISP_DIR_Z:
      if (use_global_direction) {
        vertexCos[iter][0] += delta * data->local_mat[0][2];
        vertexCos[iter][1] += delta * data->local_mat[1][2];
        vertexCos[iter][2] += delta * data->local_mat[2][2];
      }
      else {
        vertexCos[iter][2] += delta;
      }
      break;
    case MOD_DISP_DIR_RGB_XYZ:
      local_vec[0] = texres.trgba[0] - dmd->midlevel;
      local_vec[1] = texres.trgba[1] - dmd->midlevel;
      local_vec[2] = texres.trgba[2] - dmd->midlevel;
      if (use_global_direction) {
        mul_transposed_mat3_m4_v3(data->local_mat, local_vec);
      }
      mul_v3_fl(local_vec, strength);
      add_v3_v3(vertexCos[iter], local_vec);
      break;
    case MOD_DISP_DIR_NOR:
      madd_v3_v3fl(vertexCos[iter], data->vert_normals[iter], delta);
      break;
    case MOD_DISP_DIR_CLNOR:
      madd_v3_v3fl(vertexCos[iter], vert_clnors[iter], delta);
      break;
  }
}

static void displaceModifier_do(DisplaceModifierData *dmd,
                                const ModifierEvalContext *ctx,
                                Mesh *mesh,
                                float (*vertexCos)[3],
                                const int verts_num)
{
  Object *ob = ctx->object;
  MVert *mvert;
  MDeformVert *dvert;
  int direction = dmd->direction;
  int defgrp_index;
  float(*tex_co)[3];
  float weight = 1.0f; /* init value unused but some compilers may complain */
  float(*vert_clnors)[3] = NULL;
  float local_mat[4][4] = {{0}};
  const bool use_global_direction = dmd->space == MOD_DISP_SPACE_GLOBAL;

  if (dmd->texture == NULL && dmd->direction == MOD_DISP_DIR_RGB_XYZ) {
    return;
  }
  if (dmd->strength == 0.0f) {
    return;
  }

  mvert = mesh->mvert;
  MOD_get_vgroup(ob, mesh, dmd->defgrp_name, &dvert, &defgrp_index);

  if (defgrp_index >= 0 && dvert == NULL) {
    /* There is a vertex group, but it has no vertices. */
    return;
  }

  Tex *tex_target = dmd->texture;
  if (tex_target != NULL) {
    tex_co = MEM_calloc_arrayN((size_t)verts_num, sizeof(*tex_co), "displaceModifier_do tex_co");
    MOD_get_texture_coords((MappingInfoModifierData *)dmd, ctx, ob, mesh, vertexCos, tex_co);

    MOD_init_texture((MappingInfoModifierData *)dmd, ctx);
  }
  else {
    tex_co = NULL;
  }

  if (direction == MOD_DISP_DIR_CLNOR) {
    CustomData *ldata = &mesh->ldata;

    if (CustomData_has_layer(ldata, CD_CUSTOMLOOPNORMAL)) {
      if (!CustomData_has_layer(ldata, CD_NORMAL)) {
        BKE_mesh_calc_normals_split(mesh);
      }

      float(*clnors)[3] = CustomData_get_layer(ldata, CD_NORMAL);
      vert_clnors = MEM_malloc_arrayN(verts_num, sizeof(*vert_clnors), __func__);
      BKE_mesh_normals_loop_to_vertex(
          verts_num, mesh->mloop, mesh->totloop, (const float(*)[3])clnors, vert_clnors);
    }
    else {
      direction = MOD_DISP_DIR_NOR;
    }
  }
  else if (ELEM(direction, MOD_DISP_DIR_X, MOD_DISP_DIR_Y, MOD_DISP_DIR_Z, MOD_DISP_DIR_RGB_XYZ) &&
           use_global_direction) {
    copy_m4_m4(local_mat, ob->obmat);
  }

  DisplaceUserdata data = {NULL};
  data.scene = DEG_get_evaluated_scene(ctx->depsgraph);
  data.dmd = dmd;
  data.dvert = dvert;
  data.weight = weight;
  data.defgrp_index = defgrp_index;
  data.direction = direction;
  data.use_global_direction = use_global_direction;
  data.tex_target = tex_target;
  data.tex_co = tex_co;
  data.vertexCos = vertexCos;
  copy_m4_m4(data.local_mat, local_mat);
  data.mvert = mvert;
  if (direction == MOD_DISP_DIR_NOR) {
    data.vert_normals = BKE_mesh_vertex_normals_ensure(mesh);
  }
  data.vert_clnors = vert_clnors;
  if (tex_target != NULL) {
    data.pool = BKE_image_pool_new();
    BKE_texture_fetch_images_for_pool(tex_target, data.pool);
  }
  TaskParallelSettings settings;
  BLI_parallel_range_settings_defaults(&settings);
  settings.use_threading = (verts_num > 512);
  BLI_task_parallel_range(0, verts_num, &data, displaceModifier_do_task, &settings);

  if (data.pool != NULL) {
    BKE_image_pool_free(data.pool);
  }

  if (tex_co) {
    MEM_freeN(tex_co);
  }

  if (vert_clnors) {
    MEM_freeN(vert_clnors);
  }
}

static void deformVerts(ModifierData *md,
                        const ModifierEvalContext *ctx,
                        Mesh *mesh,
                        float (*vertexCos)[3],
                        int verts_num)
{
  Mesh *mesh_src = MOD_deform_mesh_eval_get(ctx->object, NULL, mesh, NULL, verts_num, false);

  displaceModifier_do((DisplaceModifierData *)md, ctx, mesh_src, vertexCos, verts_num);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static void deformVertsEM(ModifierData *md,
                          const ModifierEvalContext *ctx,
                          struct BMEditMesh *editData,
                          Mesh *mesh,
                          float (*vertexCos)[3],
                          int verts_num)
{
  Mesh *mesh_src = MOD_deform_mesh_eval_get(ctx->object, editData, mesh, NULL, verts_num, false);

  /* TODO(@campbellbarton): use edit-mode data only (remove this line). */
  if (mesh_src != NULL) {
    BKE_mesh_wrapper_ensure_mdata(mesh_src);
  }

  displaceModifier_do((DisplaceModifierData *)md, ctx, mesh_src, vertexCos, verts_num);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static void panel_draw(const bContext *C, Panel *panel)
{
  uiLayout *col;
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  PointerRNA obj_data_ptr = RNA_pointer_get(&ob_ptr, "data");

  PointerRNA texture_ptr = RNA_pointer_get(ptr, "texture");
  bool has_texture = !RNA_pointer_is_null(&texture_ptr);
  int texture_coords = RNA_enum_get(ptr, "texture_coords");

  uiLayoutSetPropSep(layout, true);

  uiTemplateID(layout, C, ptr, "texture", "texture.new", NULL, NULL, 0, ICON_NONE, NULL);

  col = uiLayoutColumn(layout, false);
  uiLayoutSetActive(col, has_texture);
  uiItemR(col, ptr, "texture_coords", 0, IFACE_("Coordinates"), ICON_NONE);
  if (texture_coords == MOD_DISP_MAP_OBJECT) {
    uiItemR(col, ptr, "texture_coords_object", 0, IFACE_("Object"), ICON_NONE);
    PointerRNA texture_coords_obj_ptr = RNA_pointer_get(ptr, "texture_coords_object");
    if (!RNA_pointer_is_null(&texture_coords_obj_ptr) &&
        (RNA_enum_get(&texture_coords_obj_ptr, "type") == OB_ARMATURE)) {
      PointerRNA texture_coords_obj_data_ptr = RNA_pointer_get(&texture_coords_obj_ptr, "data");
      uiItemPointerR(col,
                     ptr,
                     "texture_coords_bone",
                     &texture_coords_obj_data_ptr,
                     "bones",
                     IFACE_("Bone"),
                     ICON_NONE);
    }
  }
  else if (texture_coords == MOD_DISP_MAP_UV && RNA_enum_get(&ob_ptr, "type") == OB_MESH) {
    uiItemPointerR(col, ptr, "uv_layer", &obj_data_ptr, "uv_layers", NULL, ICON_NONE);
  }

  uiItemS(layout);

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "direction", 0, 0, ICON_NONE);
  if (ELEM(RNA_enum_get(ptr, "direction"),
           MOD_DISP_DIR_X,
           MOD_DISP_DIR_Y,
           MOD_DISP_DIR_Z,
           MOD_DISP_DIR_RGB_XYZ)) {
    uiItemR(col, ptr, "space", 0, NULL, ICON_NONE);
  }

  uiItemS(layout);

  col = uiLayoutColumn(layout, false);
  uiItemR(col, ptr, "strength", 0, NULL, ICON_NONE);
  uiItemR(col, ptr, "mid_level", 0, NULL, ICON_NONE);

  modifier_vgroup_ui(col, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", NULL);

  modifier_panel_end(layout, ptr);
}

static void panelRegister(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Displace, panel_draw);
}

ModifierTypeInfo modifierType_Displace = {
    /* name */ N_("Displace"),
    /* structName */ "DisplaceModifierData",
    /* structSize */ sizeof(DisplaceModifierData),
    /* srna */ &RNA_DisplaceModifier,
    /* type */ eModifierTypeType_OnlyDeform,
    /* flags */ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /* icon */ ICON_MOD_DISPLACE,

    /* copyData */ BKE_modifier_copydata_generic,

    /* deformVerts */ deformVerts,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ deformVertsEM,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ NULL,
    /* modifyGeometrySet */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ requiredDataMask,
    /* freeData */ NULL,
    /* isDisabled */ isDisabled,
    /* updateDepsgraph */ updateDepsgraph,
    /* dependsOnTime */ dependsOnTime,
    /* dependsOnNormals */ dependsOnNormals,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ foreachTexLink,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ NULL,
    /* blendRead */ NULL,
};
