/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_bitmap.h"
#include "BLI_linklist.h"
#include "BLI_utildefines.h"

#include "BKE_DerivedMesh.h"
#include "BKE_crazyspace.h"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"
#include "BKE_editmesh.h"
#include "BKE_geometry_set.hh"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_modifier.h"
#include "BKE_multires.h"
#include "BKE_report.h"

#include "DEG_depsgraph_query.h"

BLI_INLINE void tan_calc_quat_v3(float r_quat[4],
                                 const float co_1[3],
                                 const float co_2[3],
                                 const float co_3[3])
{
  float vec_u[3], vec_v[3];
  float nor[3];

  sub_v3_v3v3(vec_u, co_1, co_2);
  sub_v3_v3v3(vec_v, co_1, co_3);

  cross_v3_v3v3(nor, vec_u, vec_v);

  if (normalize_v3(nor) > FLT_EPSILON) {
    const float zero_vec[3] = {0.0f};
    tri_to_quat_ex(r_quat, zero_vec, vec_u, vec_v, nor);
  }
  else {
    unit_qt(r_quat);
  }
}

static void set_crazy_vertex_quat(float r_quat[4],
                                  const float co_1[3],
                                  const float co_2[3],
                                  const float co_3[3],
                                  const float vd_1[3],
                                  const float vd_2[3],
                                  const float vd_3[3])
{
  float q1[4], q2[4];

  tan_calc_quat_v3(q1, co_1, co_2, co_3);
  tan_calc_quat_v3(q2, vd_1, vd_2, vd_3);

  sub_qt_qtqt(r_quat, q2, q1);
}

static bool modifiers_disable_subsurf_temporary(struct Scene *scene, Object *ob)
{
  bool disabled = false;
  int cageIndex = BKE_modifiers_get_cage_index(scene, ob, nullptr, true);

  ModifierData *md = static_cast<ModifierData *>(ob->modifiers.first);
  for (int i = 0; md && i <= cageIndex; i++, md = md->next) {
    if (md->type == eModifierType_Subsurf) {
      md->mode ^= eModifierMode_DisableTemporary;
      disabled = true;
    }
  }

  return disabled;
}

float (*BKE_crazyspace_get_mapped_editverts(struct Depsgraph *depsgraph, Object *obedit))[3]
{
  Scene *scene = DEG_get_input_scene(depsgraph);
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Object *obedit_eval = DEG_get_evaluated_object(depsgraph, obedit);
  Mesh *mesh_eval = static_cast<Mesh *>(obedit_eval->data);
  BMEditMesh *editmesh_eval = mesh_eval->edit_mesh;

  /* disable subsurf temporal, get mapped cos, and enable it */
  if (modifiers_disable_subsurf_temporary(scene_eval, obedit_eval)) {
    /* Need to make new derived-mesh. */
    makeDerivedMesh(depsgraph, scene_eval, obedit_eval, &CD_MASK_BAREMESH);
  }

  /* now get the cage */
  Mesh *mesh_eval_cage = editbmesh_get_eval_cage_from_orig(
      depsgraph, scene, obedit, &CD_MASK_BAREMESH);

  const int nverts = editmesh_eval->bm->totvert;
  float(*vertexcos)[3] = static_cast<float(*)[3]>(
      MEM_mallocN(sizeof(*vertexcos) * nverts, "vertexcos map"));
  mesh_get_mapped_verts_coords(mesh_eval_cage, vertexcos, nverts);

  /* set back the flag, no new cage needs to be built, transform does it */
  modifiers_disable_subsurf_temporary(scene_eval, obedit_eval);

  return vertexcos;
}

void BKE_crazyspace_set_quats_editmesh(BMEditMesh *em,
                                       float (*origcos)[3],
                                       float (*mappedcos)[3],
                                       float (*quats)[4],
                                       const bool use_select)
{
  BMFace *f;
  BMIter iter;
  int index;

  {
    BMVert *v;
    BM_ITER_MESH_INDEX (v, &iter, em->bm, BM_VERTS_OF_MESH, index) {
      BM_elem_flag_disable(v, BM_ELEM_TAG);
      BM_elem_index_set(v, index); /* set_inline */
    }
    em->bm->elem_index_dirty &= ~BM_VERT;
  }

  BM_ITER_MESH (f, &iter, em->bm, BM_FACES_OF_MESH) {
    BMLoop *l_iter, *l_first;

    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      if (BM_elem_flag_test(l_iter->v, BM_ELEM_HIDDEN) ||
          BM_elem_flag_test(l_iter->v, BM_ELEM_TAG) ||
          (use_select && !BM_elem_flag_test(l_iter->v, BM_ELEM_SELECT))) {
        continue;
      }

      if (!BM_elem_flag_test(l_iter->v, BM_ELEM_TAG)) {
        const float *co_prev, *co_curr, *co_next; /* orig */
        const float *vd_prev, *vd_curr, *vd_next; /* deform */

        const int i_prev = BM_elem_index_get(l_iter->prev->v);
        const int i_curr = BM_elem_index_get(l_iter->v);
        const int i_next = BM_elem_index_get(l_iter->next->v);

        /* retrieve mapped coordinates */
        vd_prev = mappedcos[i_prev];
        vd_curr = mappedcos[i_curr];
        vd_next = mappedcos[i_next];

        if (origcos) {
          co_prev = origcos[i_prev];
          co_curr = origcos[i_curr];
          co_next = origcos[i_next];
        }
        else {
          co_prev = l_iter->prev->v->co;
          co_curr = l_iter->v->co;
          co_next = l_iter->next->v->co;
        }

        set_crazy_vertex_quat(quats[i_curr], co_curr, co_next, co_prev, vd_curr, vd_next, vd_prev);

        BM_elem_flag_enable(l_iter->v, BM_ELEM_TAG);
      }
    } while ((l_iter = l_iter->next) != l_first);
  }
}

void BKE_crazyspace_set_quats_mesh(Mesh *me,
                                   float (*origcos)[3],
                                   float (*mappedcos)[3],
                                   float (*quats)[4])
{
  BLI_bitmap *vert_tag = BLI_BITMAP_NEW(me->totvert, __func__);

  /* first store two sets of tangent vectors in vertices, we derive it just from the face-edges */
  MVert *mvert = me->mvert;
  MPoly *mp = me->mpoly;
  MLoop *mloop = me->mloop;

  for (int i = 0; i < me->totpoly; i++, mp++) {
    MLoop *ml_next = &mloop[mp->loopstart];
    MLoop *ml_curr = &ml_next[mp->totloop - 1];
    MLoop *ml_prev = &ml_next[mp->totloop - 2];

    for (int j = 0; j < mp->totloop; j++) {
      if (!BLI_BITMAP_TEST(vert_tag, ml_curr->v)) {
        const float *co_prev, *co_curr, *co_next; /* orig */
        const float *vd_prev, *vd_curr, *vd_next; /* deform */

        /* retrieve mapped coordinates */
        vd_prev = mappedcos[ml_prev->v];
        vd_curr = mappedcos[ml_curr->v];
        vd_next = mappedcos[ml_next->v];

        if (origcos) {
          co_prev = origcos[ml_prev->v];
          co_curr = origcos[ml_curr->v];
          co_next = origcos[ml_next->v];
        }
        else {
          co_prev = mvert[ml_prev->v].co;
          co_curr = mvert[ml_curr->v].co;
          co_next = mvert[ml_next->v].co;
        }

        set_crazy_vertex_quat(
            quats[ml_curr->v], co_curr, co_next, co_prev, vd_curr, vd_next, vd_prev);

        BLI_BITMAP_ENABLE(vert_tag, ml_curr->v);
      }

      ml_prev = ml_curr;
      ml_curr = ml_next;
      ml_next++;
    }
  }

  MEM_freeN(vert_tag);
}

int BKE_crazyspace_get_first_deform_matrices_editbmesh(struct Depsgraph *depsgraph,
                                                       Scene *scene,
                                                       Object *ob,
                                                       BMEditMesh *em,
                                                       float (**deformmats)[3][3],
                                                       float (**deformcos)[3])
{
  ModifierData *md;
  Mesh *me_input = static_cast<Mesh *>(ob->data);
  Mesh *me = nullptr;
  int i, a, modifiers_left_num = 0, verts_num = 0;
  int cageIndex = BKE_modifiers_get_cage_index(scene, ob, nullptr, true);
  float(*defmats)[3][3] = nullptr, (*deformedVerts)[3] = nullptr;
  VirtualModifierData virtualModifierData;
  ModifierEvalContext mectx = {depsgraph, ob, ModifierApplyFlag(0)};

  BKE_modifiers_clear_errors(ob);

  md = BKE_modifiers_get_virtual_modifierlist(ob, &virtualModifierData);

  /* compute the deformation matrices and coordinates for the first
   * modifiers with on cage editing that are enabled and support computing
   * deform matrices */
  for (i = 0; md && i <= cageIndex; i++, md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info(static_cast<ModifierType>(md->type));

    if (!editbmesh_modifier_is_enabled(scene, ob, md, me != nullptr)) {
      continue;
    }

    if (mti->type == eModifierTypeType_OnlyDeform && mti->deformMatricesEM) {
      if (!defmats) {
        const int required_mode = eModifierMode_Realtime | eModifierMode_Editmode;
        CustomData_MeshMasks cd_mask_extra = CD_MASK_BAREMESH;
        CDMaskLink *datamasks = BKE_modifier_calc_data_masks(
            scene, ob, md, &cd_mask_extra, required_mode, nullptr, nullptr);
        cd_mask_extra = datamasks->mask;
        BLI_linklist_free((LinkNode *)datamasks, nullptr);

        me = BKE_mesh_wrapper_from_editmesh_with_coords(em, &cd_mask_extra, nullptr, me_input);
        deformedVerts = editbmesh_vert_coords_alloc(em, &verts_num);
        defmats = static_cast<float(*)[3][3]>(
            MEM_mallocN(sizeof(*defmats) * verts_num, "defmats"));

        for (a = 0; a < verts_num; a++) {
          unit_m3(defmats[a]);
        }
      }
      mti->deformMatricesEM(md, &mectx, em, me, deformedVerts, defmats, verts_num);
    }
    else {
      break;
    }
  }

  for (; md && i <= cageIndex; md = md->next, i++) {
    if (editbmesh_modifier_is_enabled(scene, ob, md, me != nullptr) &&
        BKE_modifier_is_correctable_deformed(md)) {
      modifiers_left_num++;
    }
  }

  if (me) {
    BKE_id_free(nullptr, me);
  }

  *deformmats = defmats;
  *deformcos = deformedVerts;

  return modifiers_left_num;
}

/**
 * Crazy-space evaluation needs to have an object which has all the fields
 * evaluated, but the mesh data being at undeformed state. This way it can
 * re-apply modifiers and also have proper pointers to key data blocks.
 *
 * Similar to #BKE_object_eval_reset(), but does not modify the actual evaluated object.
 */
static void crazyspace_init_object_for_eval(struct Depsgraph *depsgraph,
                                            Object *object,
                                            Object *object_crazy)
{
  Object *object_eval = DEG_get_evaluated_object(depsgraph, object);
  *object_crazy = blender::dna::shallow_copy(*object_eval);
  if (object_crazy->runtime.data_orig != nullptr) {
    object_crazy->data = object_crazy->runtime.data_orig;
  }
}

static void crazyspace_init_verts_and_matrices(const Mesh *mesh,
                                               float (**deformmats)[3][3],
                                               float (**deformcos)[3])
{
  int verts_num;
  *deformcos = BKE_mesh_vert_coords_alloc(mesh, &verts_num);
  *deformmats = static_cast<float(*)[3][3]>(
      MEM_callocN(sizeof(**deformmats) * verts_num, "defmats"));
  for (int a = 0; a < verts_num; a++) {
    unit_m3((*deformmats)[a]);
  }
  BLI_assert(verts_num == mesh->totvert);
}

static bool crazyspace_modifier_supports_deform_matrices(ModifierData *md)
{
  if (ELEM(md->type, eModifierType_Subsurf, eModifierType_Multires)) {
    return true;
  }
  const ModifierTypeInfo *mti = BKE_modifier_get_info(static_cast<ModifierType>(md->type));
  return (mti->type == eModifierTypeType_OnlyDeform);
}

static bool crazyspace_modifier_supports_deform(ModifierData *md)
{
  const ModifierTypeInfo *mti = BKE_modifier_get_info(static_cast<ModifierType>(md->type));
  return (mti->type == eModifierTypeType_OnlyDeform);
}

int BKE_sculpt_get_first_deform_matrices(struct Depsgraph *depsgraph,
                                         Scene *scene,
                                         Object *object,
                                         float (**deformmats)[3][3],
                                         float (**deformcos)[3])
{
  ModifierData *md;
  Mesh *me_eval = nullptr;
  float(*defmats)[3][3] = nullptr, (*deformedVerts)[3] = nullptr;
  int modifiers_left_num = 0;
  VirtualModifierData virtualModifierData;
  Object object_eval;
  crazyspace_init_object_for_eval(depsgraph, object, &object_eval);
  MultiresModifierData *mmd = get_multires_modifier(scene, &object_eval, false);
  const bool is_sculpt_mode = (object->mode & OB_MODE_SCULPT) != 0;
  const bool has_multires = mmd != nullptr && mmd->sculptlvl > 0;
  const ModifierEvalContext mectx = {depsgraph, &object_eval, ModifierApplyFlag(0)};

  if (is_sculpt_mode && has_multires) {
    *deformmats = nullptr;
    *deformcos = nullptr;
    return modifiers_left_num;
  }

  md = BKE_modifiers_get_virtual_modifierlist(&object_eval, &virtualModifierData);

  for (; md; md = md->next) {
    if (!BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime)) {
      continue;
    }

    if (crazyspace_modifier_supports_deform_matrices(md)) {
      const ModifierTypeInfo *mti = BKE_modifier_get_info(static_cast<ModifierType>(md->type));
      if (defmats == nullptr) {
        /* NOTE: Evaluated object is re-set to its original un-deformed state. */
        Mesh *me = static_cast<Mesh *>(object_eval.data);
        me_eval = BKE_mesh_copy_for_eval(me, true);
        crazyspace_init_verts_and_matrices(me_eval, &defmats, &deformedVerts);
      }

      if (mti->deformMatrices) {
        mti->deformMatrices(md, &mectx, me_eval, deformedVerts, defmats, me_eval->totvert);
      }
      else {
        /* More complex handling will continue in BKE_crazyspace_build_sculpt.
         * Exiting the loop on a non-deform modifier causes issues - T71213. */
        BLI_assert(crazyspace_modifier_supports_deform(md));
        break;
      }
    }
  }

  for (; md; md = md->next) {
    if (!BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime)) {
      continue;
    }

    if (crazyspace_modifier_supports_deform(md)) {
      modifiers_left_num++;
    }
  }

  if (me_eval != nullptr) {
    BKE_id_free(nullptr, me_eval);
  }

  *deformmats = defmats;
  *deformcos = deformedVerts;

  return modifiers_left_num;
}

void BKE_crazyspace_build_sculpt(struct Depsgraph *depsgraph,
                                 Scene *scene,
                                 Object *object,
                                 float (**deformmats)[3][3],
                                 float (**deformcos)[3])
{
  int totleft = BKE_sculpt_get_first_deform_matrices(
      depsgraph, scene, object, deformmats, deformcos);

  if (totleft) {
    /* There are deformation modifier which doesn't support deformation matrices calculation.
     * Need additional crazy-space correction. */

    Mesh *mesh = (Mesh *)object->data;
    Mesh *mesh_eval = nullptr;

    if (*deformcos == nullptr) {
      crazyspace_init_verts_and_matrices(mesh, deformmats, deformcos);
    }

    float(*deformedVerts)[3] = *deformcos;
    float(*origVerts)[3] = static_cast<float(*)[3]>(MEM_dupallocN(deformedVerts));
    float(*quats)[4];
    int i, deformed = 0;
    VirtualModifierData virtualModifierData;
    Object object_eval;
    crazyspace_init_object_for_eval(depsgraph, object, &object_eval);
    ModifierData *md = BKE_modifiers_get_virtual_modifierlist(&object_eval, &virtualModifierData);
    const ModifierEvalContext mectx = {depsgraph, &object_eval, ModifierApplyFlag(0)};

    for (; md; md = md->next) {
      if (!BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime)) {
        continue;
      }

      if (crazyspace_modifier_supports_deform(md)) {
        const ModifierTypeInfo *mti = BKE_modifier_get_info(static_cast<ModifierType>(md->type));

        /* skip leading modifiers which have been already
         * handled in sculpt_get_first_deform_matrices */
        if (mti->deformMatrices && !deformed) {
          continue;
        }

        if (mesh_eval == nullptr) {
          mesh_eval = BKE_mesh_copy_for_eval(mesh, true);
        }

        mti->deformVerts(md, &mectx, mesh_eval, deformedVerts, mesh_eval->totvert);
        deformed = 1;
      }
    }

    quats = static_cast<float(*)[4]>(MEM_mallocN(mesh->totvert * sizeof(*quats), "crazy quats"));

    BKE_crazyspace_set_quats_mesh(mesh, origVerts, deformedVerts, quats);

    for (i = 0; i < mesh->totvert; i++) {
      float qmat[3][3], tmat[3][3];

      quat_to_mat3(qmat, quats[i]);
      mul_m3_m3m3(tmat, qmat, (*deformmats)[i]);
      copy_m3_m3((*deformmats)[i], tmat);
    }

    MEM_freeN(origVerts);
    MEM_freeN(quats);

    if (mesh_eval != nullptr) {
      BKE_id_free(nullptr, mesh_eval);
    }
  }

  if (*deformmats == nullptr) {
    int a, verts_num;
    Mesh *mesh = (Mesh *)object->data;

    *deformcos = BKE_mesh_vert_coords_alloc(mesh, &verts_num);
    *deformmats = static_cast<float(*)[3][3]>(
        MEM_callocN(sizeof(*(*deformmats)) * verts_num, "defmats"));

    for (a = 0; a < verts_num; a++) {
      unit_m3((*deformmats)[a]);
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Crazyspace API
 * \{ */

void BKE_crazyspace_api_eval(Depsgraph *depsgraph,
                             Scene *scene,
                             Object *object,
                             struct ReportList *reports)
{
  if (object->runtime.crazyspace_deform_imats != nullptr ||
      object->runtime.crazyspace_deform_cos != nullptr) {
    return;
  }

  if (object->type != OB_MESH) {
    BKE_report(reports,
               RPT_ERROR,
               "Crazyspace transformation is only available for Mesh type of objects");
    return;
  }

  const Mesh *mesh = (const Mesh *)object->data;
  object->runtime.crazyspace_verts_num = mesh->totvert;
  BKE_crazyspace_build_sculpt(depsgraph,
                              scene,
                              object,
                              &object->runtime.crazyspace_deform_imats,
                              &object->runtime.crazyspace_deform_cos);
}

void BKE_crazyspace_api_displacement_to_deformed(struct Object *object,
                                                 struct ReportList *reports,
                                                 int vertex_index,
                                                 float displacement[3],
                                                 float r_displacement_deformed[3])
{
  if (vertex_index < 0 || vertex_index >= object->runtime.crazyspace_verts_num) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Invalid vertex index %d (expected to be within 0 to %d range)",
                vertex_index,
                object->runtime.crazyspace_verts_num);
    return;
  }

  mul_v3_m3v3(r_displacement_deformed,
              object->runtime.crazyspace_deform_imats[vertex_index],
              displacement);
}

void BKE_crazyspace_api_displacement_to_original(struct Object *object,
                                                 struct ReportList *reports,
                                                 int vertex_index,
                                                 float displacement_deformed[3],
                                                 float r_displacement[3])
{
  if (vertex_index < 0 || vertex_index >= object->runtime.crazyspace_verts_num) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Invalid vertex index %d (expected to be within 0 to %d range))",
                vertex_index,
                object->runtime.crazyspace_verts_num);
    return;
  }

  float mat[3][3];
  if (!invert_m3_m3(mat, object->runtime.crazyspace_deform_imats[vertex_index])) {
    copy_v3_v3(r_displacement, displacement_deformed);
    return;
  }

  mul_v3_m3v3(r_displacement, mat, displacement_deformed);
}

void BKE_crazyspace_api_eval_clear(Object *object)
{
  MEM_SAFE_FREE(object->runtime.crazyspace_deform_imats);
  MEM_SAFE_FREE(object->runtime.crazyspace_deform_cos);
}

/** \} */

namespace blender::bke::crazyspace {

GeometryDeformation get_evaluated_curves_deformation(const Depsgraph &depsgraph,
                                                     const Object &ob_orig)
{
  BLI_assert(ob_orig.type == OB_CURVES);
  const Curves &curves_id_orig = *static_cast<const Curves *>(ob_orig.data);
  const CurvesGeometry &curves_orig = CurvesGeometry::wrap(curves_id_orig.geometry);
  const int points_num = curves_orig.points_num();

  GeometryDeformation deformation;
  /* Use the undeformed positions by default. */
  deformation.positions = curves_orig.positions();

  const Object *ob_eval = DEG_get_evaluated_object(&depsgraph, const_cast<Object *>(&ob_orig));
  if (ob_eval == nullptr) {
    return deformation;
  }
  const GeometrySet *geometry_eval = ob_eval->runtime.geometry_set_eval;
  if (geometry_eval == nullptr) {
    return deformation;
  }

  /* If available, use deformation information generated during evaluation. */
  const GeometryComponentEditData *edit_component_eval =
      geometry_eval->get_component_for_read<GeometryComponentEditData>();
  bool uses_extra_positions = false;
  if (edit_component_eval != nullptr) {
    const CurvesEditHints *edit_hints = edit_component_eval->curves_edit_hints_.get();
    if (edit_hints != nullptr && &edit_hints->curves_id_orig == &curves_id_orig) {
      if (edit_hints->positions.has_value()) {
        BLI_assert(edit_hints->positions->size() == points_num);
        deformation.positions = *edit_hints->positions;
        uses_extra_positions = true;
      }
      if (edit_hints->deform_mats.has_value()) {
        BLI_assert(edit_hints->deform_mats->size() == points_num);
        deformation.deform_mats = *edit_hints->deform_mats;
      }
    }
  }

  /* Use the positions of the evaluated curves directly, if the number of points matches. */
  if (!uses_extra_positions) {
    const CurveComponent *curves_component_eval =
        geometry_eval->get_component_for_read<CurveComponent>();
    if (curves_component_eval != nullptr) {
      const Curves *curves_id_eval = curves_component_eval->get_for_read();
      if (curves_id_eval != nullptr) {
        const CurvesGeometry &curves_eval = CurvesGeometry::wrap(curves_id_eval->geometry);
        if (curves_eval.points_num() == points_num) {
          deformation.positions = curves_eval.positions();
        }
      }
    }
  }
  return deformation;
}

}  // namespace blender::bke::crazyspace
