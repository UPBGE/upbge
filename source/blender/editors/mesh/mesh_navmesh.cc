/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2011 by Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Benoit Bolsee,
 *                 Nick Samarin
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/mesh/mesh_navmesh.c
 *  \ingroup edmesh
 */

#include "MEM_guardedalloc.h"

#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_layer.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_mesh.hh"
#include "ED_object.hh"
#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "recast-capi.h"

#include "mesh_intern.hh" /* own include */

using namespace blender::ed::object;

static void createVertsTrisData(
    bContext *C, LinkNode *obs, int *nverts_r, float **verts_r, int *ntris_r, int **tris_r)
{
  Object *ob;
  Mesh *me = nullptr;
  LinkNode *oblink;
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  std::vector<float> verts_vec;
  std::vector<int> tris_vec;
  int base_vert_offset = 0;

  for (oblink = obs; oblink; oblink = oblink->next) {
    ob = (Object *)oblink->link;
    Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
    me = (Mesh *)ob_eval->data;

    const blender::Span<blender::float3> positions = me->vert_positions();
    const blender::Span<blender::int3> tris_span = me->corner_tris();
    const blender::Span<int> corner_verts = me->corner_verts();

    std::vector<int> vert_map(me->verts_num, -1);

    for (int t = 0; t < tris_span.size(); t++) {
      int tri_indices[3];
      for (int j = 0; j < 3; ++j) {
        int corner = tris_span[t][j];
        int vert_i = corner_verts[corner];

        if (vert_map[vert_i] == -1) {
          float co[3], wco[3];
          const float *v = &positions[vert_i][0];
          copy_v3_v3(co, v);
          mul_v3_m4v3(wco, ob->object_to_world().ptr(), co);
          verts_vec.push_back(wco[0]);
          verts_vec.push_back(wco[2]);  // swap Y/Z
          verts_vec.push_back(wco[1]);
          vert_map[vert_i] = base_vert_offset + (int)(verts_vec.size() / 3) - 1;
        }
        tri_indices[j] = vert_map[vert_i];
      }
      tris_vec.push_back(tri_indices[0]);
      tris_vec.push_back(tri_indices[2]);
      tris_vec.push_back(tri_indices[1]);
    }
    base_vert_offset = (int)(verts_vec.size() / 3);
  }

  int nverts = (int)(verts_vec.size() / 3);
  int ntris = (int)(tris_vec.size() / 3);

  float *verts = (float *)MEM_mallocN(sizeof(float) * 3 * nverts, "createVertsTrisData verts");
  int *tris = (int *)MEM_mallocN(sizeof(int) * 3 * ntris, "createVertsTrisData faces");

  std::copy(verts_vec.begin(), verts_vec.end(), verts);
  std::copy(tris_vec.begin(), tris_vec.end(), tris);

  *nverts_r = nverts;
  *verts_r = verts;
  *ntris_r = ntris;
  *tris_r = tris;
}

static bool buildNavMesh(const RecastData *recastParams,
                         int nverts,
                         float *verts,
                         int ntris,
                         int *tris,
                         struct recast_polyMesh **pmesh,
                         struct recast_polyMeshDetail **dmesh,
                         ReportList *reports)
{
  float bmin[3], bmax[3];
  struct recast_heightfield *solid;
  unsigned char *triflags;
  struct recast_compactHeightfield *chf;
  struct recast_contourSet *cset;
  int width, height, walkableHeight, walkableClimb, walkableRadius;
  int minRegionArea, mergeRegionArea, maxEdgeLen;
  float detailSampleDist, detailSampleMaxError;

  recast_calcBounds(verts, nverts, bmin, bmax);

  /* ** Step 1. Initialize build config ** */
  walkableHeight = (int)ceilf(recastParams->agentheight / recastParams->cellheight);
  walkableClimb = (int)floorf(recastParams->agentmaxclimb / recastParams->cellheight);
  walkableRadius = (int)ceilf(recastParams->agentradius / recastParams->cellsize);
  minRegionArea = (int)(recastParams->regionminsize * recastParams->regionminsize);
  mergeRegionArea = (int)(recastParams->regionmergesize * recastParams->regionmergesize);
  maxEdgeLen = (int)(recastParams->edgemaxlen / recastParams->cellsize);
  detailSampleDist = recastParams->detailsampledist < 0.9f ?
                         0 :
                         recastParams->cellsize * recastParams->detailsampledist;
  detailSampleMaxError = recastParams->cellheight * recastParams->detailsamplemaxerror;

  /* Set the area where the navigation will be build. */
  recast_calcGridSize(bmin, bmax, recastParams->cellsize, &width, &height);

  /* zero dimensions cause zero alloc later on [#33758] */
  if (width <= 0 || height <= 0) {
    BKE_report(reports, RPT_ERROR, "Object has a width or height of zero");
    return false;
  }

  /* ** Step 2: Rasterize input polygon soup ** */
  /* Allocate voxel heightfield where we rasterize our input data to */
  solid = recast_newHeightfield();

  if (!recast_createHeightfield(
          solid, width, height, bmin, bmax, recastParams->cellsize, recastParams->cellheight)) {
    recast_destroyHeightfield(solid);
    BKE_report(reports, RPT_ERROR, "Failed to create height field");
    return false;
  }

  /* Allocate array that can hold triangle flags */
  triflags = (unsigned char *)MEM_callocN(sizeof(unsigned char) * ntris, "buildNavMesh triflags");

  /* Find triangles which are walkable based on their slope and rasterize them */
  recast_markWalkableTriangles(
      RAD2DEGF(recastParams->agentmaxslope), verts, nverts, tris, ntris, triflags);
  recast_rasterizeTriangles(verts, nverts, tris, triflags, ntris, solid, 1);
  MEM_freeN(triflags);

  /* ** Step 3: Filter walkables surfaces ** */
  recast_filterLowHangingWalkableObstacles(walkableClimb, solid);
  recast_filterLedgeSpans(walkableHeight, walkableClimb, solid);
  recast_filterWalkableLowHeightSpans(walkableHeight, solid);

  /* ** Step 4: Partition walkable surface to simple regions ** */

  chf = recast_newCompactHeightfield();
  if (!recast_buildCompactHeightfield(walkableHeight, walkableClimb, solid, chf)) {
    recast_destroyHeightfield(solid);
    recast_destroyCompactHeightfield(chf);

    BKE_report(reports, RPT_ERROR, "Failed to create compact height field");
    return false;
  }

  recast_destroyHeightfield(solid);
  solid = nullptr;

  if (!recast_erodeWalkableArea(walkableRadius, chf)) {
    recast_destroyCompactHeightfield(chf);

    BKE_report(reports, RPT_ERROR, "Failed to erode walkable area");
    return false;
  }

  if (recastParams->partitioning == RC_PARTITION_WATERSHED) {
    /* Prepare for region partitioning, by calculating distance field along the walkable surface */
    if (!recast_buildDistanceField(chf)) {
      recast_destroyCompactHeightfield(chf);

      BKE_report(reports, RPT_ERROR, "Failed to build distance field");
      return false;
    }

    /* Partition the walkable surface into simple regions without holes */
    if (!recast_buildRegions(chf, 0, minRegionArea, mergeRegionArea)) {
      recast_destroyCompactHeightfield(chf);

      BKE_report(reports, RPT_ERROR, "Failed to build watershed regions");
      return false;
    }
  }
  else if (recastParams->partitioning == RC_PARTITION_MONOTONE) {
    /* Partition the walkable surface into simple regions without holes */
    /* Monotone partitioning does not need distancefield. */
    if (!recast_buildRegionsMonotone(chf, 0, minRegionArea, mergeRegionArea)) {
      recast_destroyCompactHeightfield(chf);

      BKE_report(reports, RPT_ERROR, "Failed to build monotone regions");
      return false;
    }
  }
  else { /* RC_PARTITION_LAYERS */
    /* Partition the walkable surface into simple regions without holes */
    if (!recast_buildLayerRegions(chf, 0, minRegionArea)) {
      recast_destroyCompactHeightfield(chf);

      BKE_report(reports, RPT_ERROR, "Failed to build layer regions");
      return false;
    }
  }

  /* ** Step 5: Trace and simplify region contours ** */
  /* Create contours */
  cset = recast_newContourSet();

  if (!recast_buildContours(
          chf, recastParams->edgemaxerror, maxEdgeLen, cset, RECAST_CONTOUR_TESS_WALL_EDGES)) {
    recast_destroyCompactHeightfield(chf);
    recast_destroyContourSet(cset);

    BKE_report(reports, RPT_ERROR, "Failed to build contours");
    return false;
  }

  /* ** Step 6: Build polygons mesh from contours ** */
  *pmesh = recast_newPolyMesh();
  if (!recast_buildPolyMesh(cset, recastParams->vertsperpoly, *pmesh)) {
    recast_destroyCompactHeightfield(chf);
    recast_destroyContourSet(cset);
    recast_destroyPolyMesh(*pmesh);

    BKE_report(reports, RPT_ERROR, "Failed to build poly mesh");
    return false;
  }

  /* ** Step 7: Create detail mesh which allows to access approximate height on each polygon ** */

  *dmesh = recast_newPolyMeshDetail();
  if (!recast_buildPolyMeshDetail(*pmesh, chf, detailSampleDist, detailSampleMaxError, *dmesh)) {
    recast_destroyCompactHeightfield(chf);
    recast_destroyContourSet(cset);
    recast_destroyPolyMesh(*pmesh);
    recast_destroyPolyMeshDetail(*dmesh);

    BKE_report(reports, RPT_ERROR, "Failed to build poly mesh detail");
    return false;
  }

  recast_destroyCompactHeightfield(chf);
  recast_destroyContourSet(cset);

  return true;
}

static Object *createRepresentation(bContext *C,
                                    struct recast_polyMesh *pmesh,
                                    struct recast_polyMeshDetail *dmesh,
                                    Base *base)
{
  float co[3], rot[3];
  BMEditMesh *em;
  int i, j, k;
  unsigned short *v;
  int face[3];
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Object *obedit;
  int createob = base == nullptr;
  int nverts, nmeshes, nvp;
  unsigned short *verts, *polys;
  unsigned int *meshes;
  float bmin[3], cs, ch, *dverts;
  unsigned char *tris;

  zero_v3(co);
  zero_v3(rot);

  if (createob) {
    /* create new object */
    obedit = blender::ed::object::add_type(C, OB_MESH, "Navmesh", co, rot, false, 0);
  }
  else {
    obedit = base->object;
    BKE_view_layer_base_deselect_all(scene, view_layer);
    BKE_view_layer_base_select_and_set_active(view_layer, base);
    copy_v3_v3(obedit->loc, co);
    copy_v3_v3(obedit->rot, rot);
  }

  blender::ed::object::editmode_enter_ex(bmain, scene, obedit, 0);
  em = BKE_editmesh_from_object(obedit);

  if (!createob) {
    /* clear */
    BM_mesh_clear(em->bm);
  }

  /* create verts for polygon mesh */
  verts = recast_polyMeshGetVerts(pmesh, &nverts);
  recast_polyMeshGetBoundbox(pmesh, bmin, nullptr);
  recast_polyMeshGetCell(pmesh, &cs, &ch);

  for (i = 0; i < nverts; i++) {
    v = &verts[3 * i];
    co[0] = bmin[0] + v[0] * cs;
    co[1] = bmin[1] + v[1] * ch;
    co[2] = bmin[2] + v[2] * cs;
    SWAP(float, co[1], co[2]);
    BM_vert_create(em->bm, co, nullptr, BM_CREATE_NOP);
  }

  /* create custom data layer to save polygon idx */
  CustomData_add_layer_named(
      &em->bm->pdata, CD_RECAST, CD_SET_DEFAULT, 0, "createRepresentation recastData");
  CustomData_bmesh_init_pool(&em->bm->pdata, 0, BM_FACE);

  /* create verts and faces for detailed mesh */
  meshes = recast_polyMeshDetailGetMeshes(dmesh, &nmeshes);
  polys = recast_polyMeshGetPolys(pmesh, nullptr, &nvp);
  dverts = recast_polyMeshDetailGetVerts(dmesh, nullptr);
  tris = recast_polyMeshDetailGetTris(dmesh, nullptr);

  for (i = 0; i < nmeshes; i++) {
    int uniquevbase = em->bm->totvert;
    unsigned int vbase = meshes[4 * i + 0];
    unsigned short ndv = meshes[4 * i + 1];
    unsigned short tribase = meshes[4 * i + 2];
    unsigned short trinum = meshes[4 * i + 3];
    const unsigned short *p = &polys[i * nvp * 2];
    int nv = 0;

    for (j = 0; j < nvp; ++j) {
      if (p[j] == 0xffff)
        break;
      nv++;
    }

    /* create unique verts  */
    for (j = nv; j < ndv; j++) {
      copy_v3_v3(co, &dverts[3 * (vbase + j)]);
      SWAP(float, co[1], co[2]);
      BM_vert_create(em->bm, co, nullptr, BM_CREATE_NOP);
    }

    /* need to rebuild entirely because array size changes */
    BM_mesh_elem_table_init(em->bm, BM_VERT);

    /* create faces */
    for (j = 0; j < trinum; j++) {
      unsigned char *tri = &tris[4 * (tribase + j)];
      BMFace *newFace;
      int *polygonIdx;

      for (k = 0; k < 3; k++) {
        if (tri[k] < nv)
          face[k] = p[tri[k]]; /* shared vertex */
        else
          face[k] = uniquevbase + tri[k] - nv; /* unique vertex */
      }
      newFace = BM_face_create_quad_tri(em->bm,
                                        BM_vert_at_index(em->bm, face[0]),
                                        BM_vert_at_index(em->bm, face[2]),
                                        BM_vert_at_index(em->bm, face[1]),
                                        nullptr,
                                        nullptr,
                                        BM_CREATE_NOP);

      /* set navigation polygon idx to the custom layer */
      polygonIdx = (int *)CustomData_bmesh_get(&em->bm->pdata, newFace->head.data, CD_RECAST);
      *polygonIdx = i + 1; /* add 1 to avoid zero idx */
    }
  }

  recast_destroyPolyMesh(pmesh);
  recast_destroyPolyMeshDetail(dmesh);

  DEG_id_tag_update((ID *)obedit->data, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);

  blender::ed::object::editmode_exit(C, EM_FREEDATA);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, obedit);

  if (createob) {
    obedit->gameflag &= ~OB_COLLISION;
    obedit->gameflag |= OB_NAVMESH;
    obedit->body_type = OB_BODY_TYPE_NAVMESH;
  }

  BKE_mesh_ensure_navmesh((Mesh *)obedit->data);

  return obedit;
}

static wmOperatorStatus navmesh_create_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  LinkNode *obs = nullptr;
  Base *navmeshBase = nullptr;

  CTX_DATA_BEGIN (C, Base *, base, selected_editable_bases) {
    if (base->object->type == OB_MESH) {
      if (base->object->body_type == OB_BODY_TYPE_NAVMESH) {
        if (!navmeshBase || base == view_layer->basact) {
          navmeshBase = base;
        }
      }
      else {
        BLI_linklist_prepend(&obs, base->object);
      }
    }
  }
  CTX_DATA_END;

  if (obs) {
    struct recast_polyMesh *pmesh = nullptr;
    struct recast_polyMeshDetail *dmesh = nullptr;
    bool ok;

    int nverts = 0, ntris = 0;
    int *tris = nullptr;
    float *verts = nullptr;

    createVertsTrisData(C, obs, &nverts, &verts, &ntris, &tris);
    BLI_linklist_free(obs, nullptr);
    if ((ok = buildNavMesh(
             &scene->gm.recastData, nverts, verts, ntris, tris, &pmesh, &dmesh, op->reports))) {
      createRepresentation(C, pmesh, dmesh, navmeshBase);
    }

    MEM_freeN(verts);
    MEM_freeN(tris);

    return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
  }
  else {
    BKE_report(op->reports, RPT_ERROR, "No mesh objects found");

    return OPERATOR_CANCELLED;
  }
}

void MESH_OT_navmesh_make(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Create Navigation Mesh";
  ot->description = "Create navigation mesh for selected objects";
  ot->idname = "MESH_OT_navmesh_make";

  /* api callbacks */
  ot->exec = navmesh_create_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus navmesh_face_copy_exec(bContext *C, wmOperator *op)
{
  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);

  /* do work here */
  BMFace *efa_act = BM_mesh_active_face_get(em->bm, false, false);

  if (efa_act) {
    if (CustomData_has_layer(&em->bm->pdata, CD_RECAST)) {
      BMFace *efa;
      BMIter iter;
      int targetPolyIdx = *(int *)CustomData_bmesh_get(
          &em->bm->pdata, efa_act->head.data, CD_RECAST);
      targetPolyIdx = targetPolyIdx >= 0 ? targetPolyIdx : -targetPolyIdx;

      if (targetPolyIdx > 0) {
        /* set target poly idx to other selected faces */
        BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
          if (BM_elem_flag_test(efa, BM_ELEM_SELECT) && efa != efa_act) {
            int *recastDataBlock = (int *)CustomData_bmesh_get(
                &em->bm->pdata, efa->head.data, CD_RECAST);
            *recastDataBlock = targetPolyIdx;
          }
        }
      }
      else {
        BKE_report(op->reports, RPT_ERROR, "Active face has no index set");
      }
    }
  }

  DEG_id_tag_update((ID *)obedit->data, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);

  return OPERATOR_FINISHED;
}

void MESH_OT_navmesh_face_copy(struct wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "NavMesh Copy Face Index";
  ot->description = "Copy the index from the active face";
  ot->idname = "MESH_OT_navmesh_face_copy";

  /* api callbacks */
  ot->poll = ED_operator_editmesh;
  ot->exec = navmesh_face_copy_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int compare(const void *a, const void *b)
{
  return (*(int *)a - *(int *)b);
}

static int findFreeNavPolyIndex(BMEditMesh *em)
{
  /* construct vector of indices */
  int numfaces = em->bm->totface;
  int *indices = (int *)MEM_callocN(sizeof(int) * numfaces, "findFreeNavPolyIndex(indices)");
  BMFace *ef;
  BMIter iter;
  int i, idx = em->bm->totface - 1, freeIdx = 1;

  /*XXX this originally went last to first, but that isn't possible anymore*/
  BM_ITER_MESH (ef, &iter, em->bm, BM_FACES_OF_MESH) {
    int polyIdx = *(int *)CustomData_bmesh_get(&em->bm->pdata, ef->head.data, CD_RECAST);
    indices[idx] = polyIdx;
    idx--;
  }

  qsort(indices, numfaces, sizeof(int), compare);

  /* search first free index */
  freeIdx = 1;
  for (i = 0; i < numfaces; i++) {
    if (indices[i] == freeIdx)
      freeIdx++;
    else if (indices[i] > freeIdx)
      break;
  }

  MEM_freeN(indices);

  return freeIdx;
}

static wmOperatorStatus navmesh_face_add_exec(bContext *C, wmOperator */*op*/)
{
  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  BMFace *ef;
  BMIter iter;

  if (CustomData_has_layer(&em->bm->pdata, CD_RECAST)) {
    int targetPolyIdx = findFreeNavPolyIndex(em);

    if (targetPolyIdx > 0) {
      /* set target poly idx to selected faces */
      /*XXX this originally went last to first, but that isn't possible anymore*/

      BM_ITER_MESH (ef, &iter, em->bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(ef, BM_ELEM_SELECT)) {
          int *recastDataBlock = (int *)CustomData_bmesh_get(
              &em->bm->pdata, ef->head.data, CD_RECAST);
          *recastDataBlock = targetPolyIdx;
        }
      }
    }
  }

  DEG_id_tag_update((ID *)obedit->data, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);

  return OPERATOR_FINISHED;
}

void MESH_OT_navmesh_face_add(struct wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "NavMesh New Face Index";
  ot->description = "Add a new index and assign it to selected faces";
  ot->idname = "MESH_OT_navmesh_face_add";

  /* api callbacks */
  ot->poll = ED_operator_editmesh;
  ot->exec = navmesh_face_add_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static bool navmesh_obmode_data_poll(bContext *C)
{
  Object *ob = blender::ed::object::context_active_object(C);
  if (ob && (ob->mode == OB_MODE_OBJECT) && (ob->type == OB_MESH)) {
    Mesh *me = (Mesh *)ob->data;
    return CustomData_has_layer(&me->face_data, CD_RECAST);
  }
  return false;
}

static bool navmesh_obmode_poll(bContext *C)
{
  Object *ob = blender::ed::object::context_active_object(C);
  if (ob && (ob->mode == OB_MODE_OBJECT) && (ob->type == OB_MESH)) {
    return true;
  }
  return false;
}

static wmOperatorStatus navmesh_reset_exec(bContext *C, wmOperator */*op*/)
{
  Object *ob = blender::ed::object::context_active_object(C);
  Mesh *me = (Mesh *)ob->data;

  CustomData_free_layers(&me->face_data, CD_RECAST);

  BKE_mesh_ensure_navmesh(me);

  DEG_id_tag_update(&me->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &me->id);

  return OPERATOR_FINISHED;
}

void MESH_OT_navmesh_reset(struct wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "NavMesh Reset Index Values";
  ot->description = "Assign a new index to every face";
  ot->idname = "MESH_OT_navmesh_reset";

  /* api callbacks */
  ot->poll = navmesh_obmode_poll;
  ot->exec = navmesh_reset_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus navmesh_clear_exec(bContext *C, wmOperator */*op*/)
{
  Object *ob = blender::ed::object::context_active_object(C);
  Mesh *me = (Mesh *)ob->data;

  CustomData_free_layers(&me->face_data, CD_RECAST);
  ob->gameflag &= ~OB_NAVMESH;

  DEG_id_tag_update(&me->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &me->id);

  return OPERATOR_FINISHED;
}

void MESH_OT_navmesh_clear(struct wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove NavMesh";
  ot->description = "Remove navmesh data from this mesh";
  ot->idname = "MESH_OT_navmesh_clear";

  /* api callbacks */
  ot->poll = navmesh_obmode_data_poll;
  ot->exec = navmesh_clear_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
