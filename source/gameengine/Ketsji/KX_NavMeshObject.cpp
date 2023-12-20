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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "KX_NavMeshObject.h"

#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BLI_sort.h"
#include "DEG_depsgraph_query.hh"
#include "DNA_meshdata_types.h"
#include "MEM_guardedalloc.h"

#include "BL_Converter.h"
#include "CM_Message.h"
#include "DetourStatNavMeshBuilder.h"
#include "KX_Globals.h"
#include "KX_ObstacleSimulation.h"
#include "KX_PyMath.h"
#include "RAS_IVertex.h"
#include "RAS_Polygon.h"
#include "Recast.h"

#define MAX_PATH_LEN 256
static const float polyPickExt[3] = {2, 4, 2};

static void calcMeshBounds(const float *vert, int nverts, float *bmin, float *bmax)
{
  bmin[0] = bmax[0] = vert[0];
  bmin[1] = bmax[1] = vert[1];
  bmin[2] = bmax[2] = vert[2];
  for (int i = 1; i < nverts; i++) {
    if (bmin[0] > vert[3 * i + 0])
      bmin[0] = vert[3 * i + 0];
    if (bmin[1] > vert[3 * i + 1])
      bmin[1] = vert[3 * i + 1];
    if (bmin[2] > vert[3 * i + 2])
      bmin[2] = vert[3 * i + 2];

    if (bmax[0] < vert[3 * i + 0])
      bmax[0] = vert[3 * i + 0];
    if (bmax[1] < vert[3 * i + 1])
      bmax[1] = vert[3 * i + 1];
    if (bmax[2] < vert[3 * i + 2])
      bmax[2] = vert[3 * i + 2];
  }
}

inline void flipAxes(float *vec)
{
  std::swap(vec[1], vec[2]);
}

BLI_INLINE float area2(const float *a, const float *b, const float *c)
{
  return (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
}

BLI_INLINE int left(const float *a, const float *b, const float *c)
{
  return area2(a, b, c) < 0;
}

static int polyNumVerts(const unsigned short *p, const int vertsPerPoly)
{
  int i, nv = 0;
  for (i = 0; i < vertsPerPoly; i++) {
    if (p[i] == 0xffff)
      break;
    nv++;
  }
  return nv;
}

// static int polyIsConvex(const unsigned short *p, const int vertsPerPoly, const float *verts)
//{
//  int j, nv = polyNumVerts(p, vertsPerPoly);
//  if (nv < 3)
//    return 0;
//  for (j = 0; j < nv; j++) {
//    const float *v = &verts[3 * p[j]];
//    const float *v_next = &verts[3 * p[(j + 1) % nv]];
//    const float *v_prev = &verts[3 * p[(nv + j - 1) % nv]];
//    if (!left(v_prev, v, v_next))
//      return 0;
//  }
//  return 1;
//}

/* XXX, could replace with #dist_to_line_segment_v3(), or add a squared version */
static float distPointToSegmentSq(const float point[3], const float a[3], const float b[3])
{
  float abx[3], dx[3];
  float d, t;

  sub_v3_v3v3(abx, b, a);
  sub_v3_v3v3(dx, point, a);

  d = abx[0] * abx[0] + abx[2] * abx[2];
  t = abx[0] * dx[0] + abx[2] * dx[2];

  if (d > 0.0f)
    t /= d;
  if (t < 0.0f)
    t = 0.0f;
  else if (t > 1.0f)
    t = 1.0f;
  dx[0] = a[0] + t * abx[0] - point[0];
  dx[2] = a[2] + t * abx[2] - point[2];

  return dx[0] * dx[0] + dx[2] * dx[2];
}

static int buildRawVertIndicesData(Mesh *me,
                                   int *nverts_r,
                                   float **verts_r,
                                   int *ntris_r,
                                   unsigned short **tris_r,
                                   int **trisToFacesMap_r,
                                   int **recastData)
{
  int vi, fi, triIdx;
  int nverts, ntris;
  int *trisToFacesMap;
  float *verts;
  unsigned short *tris, *tri;
  int nfaces;
  MFace *faces;

  nverts = me->verts_num;
  if (nverts >= 0xffff) {
    printf("Converting navmesh: Error! Too many vertices. Max number of vertices %d\n", 0xffff);
    return 0;
  }
  if (nverts == 0) {
    printf("Converting navmesh: Error! There are no vertices!\n");
    return 0;
  }

  float(*v)[3] = (float(*)[3])MEM_mallocN(sizeof(float[3]) * me->verts_num, __func__);
  blender::MutableSpan(reinterpret_cast<blender::float3 *>(v), me->verts_num).copy_from(me->vert_positions());
  verts = (float *)v;

  /* flip coordinates */
  for (vi = 0; vi < nverts; vi++) {
    SWAP(float, verts[3 * vi + 1], verts[3 * vi + 2]);
  }

  /* calculate number of tris */
  BKE_mesh_tessface_ensure(me);
  nfaces = me->totface_legacy;
  if (nfaces == 0) {
    printf("Converting navmesh: Error! There are %i vertices, but no faces!\n", nverts);
    return 0;
  }

  faces = (MFace *)CustomData_get_layer(&me->fdata_legacy, CD_MFACE);
  ntris = nfaces;
  for (fi = 0; fi < nfaces; fi++) {
    MFace *face = &faces[fi];
    if (face->v4)
      ntris++;
  }

  /* copy and transform to triangles (reorder on the run) */
  trisToFacesMap = (int *)MEM_callocN(sizeof(int) * ntris,
                                      "buildRawVertIndicesData trisToFacesMap");
  tris = (unsigned short *)MEM_callocN(sizeof(unsigned short) * 3 * ntris,
                                       "buildRawVertIndicesData tris");
  tri = tris;
  triIdx = 0;
  for (fi = 0; fi < nfaces; fi++) {
    MFace *face = &faces[fi];
    tri[3 * triIdx + 0] = (unsigned short)face->v1;
    tri[3 * triIdx + 1] = (unsigned short)face->v3;
    tri[3 * triIdx + 2] = (unsigned short)face->v2;
    trisToFacesMap[triIdx++] = fi;
    if (face->v4) {
      tri[3 * triIdx + 0] = (unsigned short)face->v1;
      tri[3 * triIdx + 1] = (unsigned short)face->v4;
      tri[3 * triIdx + 2] = (unsigned short)face->v3;
      trisToFacesMap[triIdx++] = fi;
    }
  }

  /* carefully, recast data is just reference to data in mesh */
  *recastData = (int *)CustomData_get_layer(&me->face_data, CD_RECAST);

  *nverts_r = nverts;
  *verts_r = verts;
  *ntris_r = ntris;
  *tris_r = tris;
  *trisToFacesMap_r = trisToFacesMap;

  return 1;
}

static int buildPolygonsByDetailedMeshes(const int vertsPerPoly,
                                         const int npolys,
                                         unsigned short *polys,
                                         const unsigned short *dmeshes,
                                         const float *verts,
                                         const unsigned short *dtris,
                                         const int *dtrisToPolysMap)
{
  int polyidx;
  int capacity = vertsPerPoly;
  unsigned short *newPoly = (unsigned short *)MEM_callocN(sizeof(unsigned short) * capacity,
                                                          "buildPolygonsByDetailedMeshes newPoly");
  memset(newPoly, 0xff, sizeof(unsigned short) * capacity);

  for (polyidx = 0; polyidx < npolys; polyidx++) {
    size_t i;
    int j, k;
    int nv = 0;
    /* search border */
    int tri, btri = -1;
    int edge, bedge = -1;
    int dtrisNum = dmeshes[polyidx * 4 + 3];
    int dtrisBase = dmeshes[polyidx * 4 + 2];
    unsigned char *traversedTris = (unsigned char *)MEM_callocN(
        sizeof(unsigned char) * dtrisNum, "buildPolygonsByDetailedMeshes traversedTris");
    unsigned short *adjustedPoly;
    int adjustedNv;
    int allBorderTraversed;

    for (j = 0; j < dtrisNum && btri == -1; j++) {
      int curpolytri = dtrisBase + j;
      for (k = 0; k < 3; k++) {
        unsigned short neighbortri = dtris[curpolytri * 3 * 2 + 3 + k];
        if (neighbortri == 0xffff || dtrisToPolysMap[neighbortri] != polyidx + 1) {
          btri = curpolytri;
          bedge = k;
          break;
        }
      }
    }
    if (btri == -1 || bedge == -1) {
      /* can't find triangle with border edge */
      MEM_freeN(traversedTris);
      MEM_freeN(newPoly);

      return 0;
    }

    newPoly[nv++] = dtris[btri * 3 * 2 + bedge];
    tri = btri;
    edge = (bedge + 1) % 3;
    traversedTris[tri - dtrisBase] = 1;
    while (tri != btri || edge != bedge) {
      int neighbortri = dtris[tri * 3 * 2 + 3 + edge];
      if (neighbortri == 0xffff || dtrisToPolysMap[neighbortri] != polyidx + 1) {
        if (nv == capacity) {
          unsigned short *newPolyBig;
          capacity += vertsPerPoly;
          newPolyBig = (unsigned short *)MEM_callocN(sizeof(unsigned short) * capacity,
                                                     "buildPolygonsByDetailedMeshes newPolyBig");
          memset(newPolyBig, 0xff, sizeof(unsigned short) * capacity);
          memcpy(newPolyBig, newPoly, sizeof(unsigned short) * nv);
          MEM_freeN(newPoly);
          newPoly = newPolyBig;
        }
        newPoly[nv++] = dtris[tri * 3 * 2 + edge];
        /* move to next edge */
        edge = (edge + 1) % 3;
      }
      else {
        /* move to next tri */
        int twinedge = -1;
        for (k = 0; k < 3; k++) {
          if (dtris[neighbortri * 3 * 2 + 3 + k] == tri) {
            twinedge = k;
            break;
          }
        }
        if (twinedge == -1) {
          printf("Converting navmesh: Error! Can't find neighbor edge - invalid adjacency info\n");
          MEM_freeN(traversedTris);
          goto returnLabel;
        }
        tri = neighbortri;
        edge = (twinedge + 1) % 3;
        traversedTris[tri - dtrisBase] = 1;
      }
    }

    adjustedPoly = (unsigned short *)MEM_callocN(sizeof(unsigned short) * nv,
                                                 "buildPolygonsByDetailedMeshes adjustedPoly");
    adjustedNv = 0;
    for (i = 0; i < nv; i++) {
      unsigned short prev = newPoly[(nv + i - 1) % nv];
      unsigned short cur = newPoly[i];
      unsigned short next = newPoly[(i + 1) % nv];
      float distSq = distPointToSegmentSq(&verts[3 * cur], &verts[3 * prev], &verts[3 * next]);
      static const float tolerance = 0.001f;
      if (distSq > tolerance)
        adjustedPoly[adjustedNv++] = cur;
    }
    memcpy(newPoly, adjustedPoly, adjustedNv * sizeof(unsigned short));
    MEM_freeN(adjustedPoly);
    nv = adjustedNv;

    allBorderTraversed = 1;
    for (i = 0; i < dtrisNum; i++) {
      if (traversedTris[i] == 0) {
        /* check whether it has border edges */
        int curpolytri = dtrisBase + i;
        for (k = 0; k < 3; k++) {
          unsigned short neighbortri = dtris[curpolytri * 3 * 2 + 3 + k];
          if (neighbortri == 0xffff || dtrisToPolysMap[neighbortri] != polyidx + 1) {
            allBorderTraversed = 0;
            break;
          }
        }
      }
    }

    if (nv <= vertsPerPoly && allBorderTraversed) {
      for (i = 0; i < nv; i++) {
        polys[polyidx * vertsPerPoly * 2 + i] = newPoly[i];
      }
    }

    MEM_freeN(traversedTris);
  }

returnLabel:
  MEM_freeN(newPoly);

  return 1;
}

struct SortContext {
  const int *recastData;
  const int *trisToFacesMap;
};

static int compareByData(const void *a, const void *b, void *ctx)
{
  return (((struct SortContext *)ctx)
              ->recastData[((struct SortContext *)ctx)->trisToFacesMap[*(int *)a]] -
          ((struct SortContext *)ctx)
              ->recastData[((struct SortContext *)ctx)->trisToFacesMap[*(int *)b]]);
}

static int buildNavMeshData(const int nverts,
                            const float *verts,
                            const int ntris,
                            const unsigned short *tris,
                            const int *recastData,
                            const int *trisToFacesMap,
                            int *ndtris_r,
                            unsigned short **dtris_r,
                            int *npolys_r,
                            unsigned short **dmeshes_r,
                            unsigned short **polys_r,
                            int *vertsPerPoly_r,
                            int **dtrisToPolysMap_r,
                            int **dtrisToTrisMap_r)

{
  int *trisMapping;
  int i;
  struct SortContext context;
  int validTriStart, prevPolyIdx, curPolyIdx, newPolyIdx, prevpolyidx;
  unsigned short *dmesh;

  int ndtris, npolys, vertsPerPoly;
  unsigned short *dtris, *dmeshes, *polys;
  int *dtrisToPolysMap, *dtrisToTrisMap;

  if (!recastData) {
    printf("Converting navmesh: Error! Can't find recast custom data\n");
    return 0;
  }

  trisMapping = (int *)MEM_callocN(sizeof(int) * ntris, "buildNavMeshData trisMapping");

  /* sort the triangles by polygon idx */
  for (i = 0; i < ntris; i++)
    trisMapping[i] = i;
  context.recastData = recastData;
  context.trisToFacesMap = trisToFacesMap;
  BLI_qsort_r(trisMapping, ntris, sizeof(int), compareByData, &context);

  /* search first valid triangle - triangle of convex polygon */
  validTriStart = -1;
  for (i = 0; i < ntris; i++) {
    if (recastData[trisToFacesMap[trisMapping[i]]] > 0) {
      validTriStart = i;
      break;
    }
  }

  if (validTriStart < 0) {
    printf("Converting navmesh: Error! No valid polygons in mesh\n");
    MEM_freeN(trisMapping);
    return 0;
  }

  ndtris = ntris - validTriStart;
  /* fill dtris to faces mapping */
  dtrisToTrisMap = (int *)MEM_callocN(sizeof(int) * ndtris, "buildNavMeshData dtrisToTrisMap");
  memcpy(dtrisToTrisMap, &trisMapping[validTriStart], ndtris * sizeof(int));
  MEM_freeN(trisMapping);

  /* create detailed mesh triangles  - copy only valid triangles
   * and reserve memory for adjacency info */
  dtris = (unsigned short *)MEM_callocN(sizeof(unsigned short) * 3 * 2 * ndtris,
                                        "buildNavMeshData dtris");
  memset(dtris, 0xff, sizeof(unsigned short) * 3 * 2 * ndtris);
  for (i = 0; i < ndtris; i++) {
    memcpy(dtris + 3 * 2 * i, tris + 3 * dtrisToTrisMap[i], sizeof(unsigned short) * 3);
  }

  /* create new recast data corresponded to dtris and renumber for continuous indices */
  prevPolyIdx = -1;
  newPolyIdx = 0;
  dtrisToPolysMap = (int *)MEM_callocN(sizeof(int) * ndtris, "buildNavMeshData dtrisToPolysMap");
  for (i = 0; i < ndtris; i++) {
    curPolyIdx = recastData[trisToFacesMap[dtrisToTrisMap[i]]];
    if (curPolyIdx != prevPolyIdx) {
      newPolyIdx++;
      prevPolyIdx = curPolyIdx;
    }
    dtrisToPolysMap[i] = newPolyIdx;
  }

  /* build adjacency info for detailed mesh triangles */
  if (!buildMeshAdjacency(dtris, ndtris, nverts, 3)) {
    printf("Converting navmesh: Error! Unable to build mesh adjacency information\n");
    MEM_freeN(trisMapping);
    MEM_freeN(dtrisToPolysMap);
    return 0;
  }

  /* create detailed mesh description for each navigation polygon */
  npolys = dtrisToPolysMap[ndtris - 1];
  dmeshes = (unsigned short *)MEM_callocN(sizeof(unsigned short) * npolys * 4,
                                          "buildNavMeshData dmeshes");
  memset(dmeshes, 0, npolys * 4 * sizeof(unsigned short));
  dmesh = nullptr;
  prevpolyidx = 0;
  for (i = 0; i < ndtris; i++) {
    int curpolyidx = dtrisToPolysMap[i];
    if (curpolyidx != prevpolyidx) {
      if (curpolyidx != prevpolyidx + 1) {
        printf("Converting navmesh: Error! Wrong order of detailed mesh faces\n");
        goto fail;
      }
      dmesh = dmesh == nullptr ? dmeshes : dmesh + 4;
      dmesh[2] = (unsigned short)i; /* tbase */
      dmesh[3] = 0;                 /* tnum */
      prevpolyidx = curpolyidx;
    }
    dmesh[3]++;
  }

  /* create navigation polygons */
  vertsPerPoly = 6;
  polys = (unsigned short *)MEM_callocN(sizeof(unsigned short) * npolys * vertsPerPoly * 2,
                                        "buildNavMeshData polys");
  memset(polys, 0xff, sizeof(unsigned short) * vertsPerPoly * 2 * npolys);

  if (!buildPolygonsByDetailedMeshes(
          vertsPerPoly, npolys, polys, dmeshes, verts, dtris, dtrisToPolysMap)) {
    printf("Converting navmesh: Error! Unable to build polygons from detailed mesh\n");
    goto fail;
  }

  *ndtris_r = ndtris;
  *npolys_r = npolys;
  *vertsPerPoly_r = vertsPerPoly;
  *dtris_r = dtris;
  *dmeshes_r = dmeshes;
  *polys_r = polys;
  *dtrisToPolysMap_r = dtrisToPolysMap;
  *dtrisToTrisMap_r = dtrisToTrisMap;

  return 1;

fail:
  MEM_freeN(dmeshes);
  MEM_freeN(dtrisToPolysMap);
  MEM_freeN(dtrisToTrisMap);
  return 0;
}

static int buildNavMeshDataByMesh(Mesh *me,
                                  int *vertsPerPoly,
                                  int *nverts,
                                  float **verts,
                                  int *ndtris,
                                  unsigned short **dtris,
                                  int *npolys,
                                  unsigned short **dmeshes,
                                  unsigned short **polys,
                                  int **dtrisToPolysMap,
                                  int **dtrisToTrisMap,
                                  int **trisToFacesMap)
{
  int res;
  int ntris = 0, *recastData = nullptr;
  unsigned short *tris = nullptr;

  res = buildRawVertIndicesData(me, nverts, verts, &ntris, &tris, trisToFacesMap, &recastData);
  if (!res) {
    printf("Converting navmesh: Error! Can't get raw vertices and indices from mesh\n");
    goto exit;
  }

  res = buildNavMeshData(*nverts,
                         *verts,
                         ntris,
                         tris,
                         recastData,
                         *trisToFacesMap,
                         ndtris,
                         dtris,
                         npolys,
                         dmeshes,
                         polys,
                         vertsPerPoly,
                         dtrisToPolysMap,
                         dtrisToTrisMap);
  if (!res) {
    printf("Converting navmesh: Error! Can't build navmesh data from mesh\n");
    goto exit;
  }

exit:
  if (tris)
    MEM_freeN(tris);

  return res;
}

static int polyFindVertex(const unsigned short *p,
                          const int vertsPerPoly,
                          unsigned short vertexIdx)
{
  int i, res = -1;
  for (i = 0; i < vertsPerPoly; i++) {
    if (p[i] == 0xffff)
      break;
    if (p[i] == vertexIdx) {
      res = i;
      break;
    }
  }
  return res;
}

KX_NavMeshObject::KX_NavMeshObject() : KX_GameObject(), m_navMesh(nullptr)
{
}

KX_NavMeshObject::~KX_NavMeshObject()
{
  if (m_navMesh)
    delete m_navMesh;
}

KX_PythonProxy *KX_NavMeshObject::NewInstance()
{
  return new KX_NavMeshObject(*this);
}

void KX_NavMeshObject::ProcessReplica()
{
  KX_GameObject::ProcessReplica();
  m_navMesh = nullptr; /* without this, building frees the navmesh we copied from */
  if (!BuildNavMesh()) {
    CM_FunctionError("unable to build navigation mesh");
    return;
  }
  KX_ObstacleSimulation *obssimulation = GetScene()->GetObstacleSimulation();
  if (obssimulation)
    obssimulation->AddObstaclesForNavMesh(this);
}

bool KX_NavMeshObject::BuildVertIndArrays(float *&vertices,
                                          int &nverts,
                                          unsigned short *&polys,
                                          int &npolys,
                                          unsigned short *&dmeshes,
                                          float *&dvertices,
                                          int &ndvertsuniq,
                                          unsigned short *&dtris,
                                          int &ndtris,
                                          int &vertsPerPoly)
{
  /* TODO: This doesn't work currently because of eval_ctx. */
  bContext *C = KX_GetActiveEngine()->GetContext();
  Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
  Object *ob_eval = DEG_get_evaluated_object(depsgraph, GetBlenderObject());
  Mesh *final_me = (Mesh *)ob_eval->data;
  BKE_mesh_tessface_ensure(final_me);
  CustomData *pdata = &final_me->face_data;
  int *recastData = (int *)CustomData_get_layer(pdata, CD_RECAST);
  if (recastData) {
    int *dtrisToPolysMap = nullptr, *dtrisToTrisMap = nullptr, *trisToFacesMap = nullptr;
    int nAllVerts = 0;
    float *allVerts = nullptr;
    buildNavMeshDataByMesh(final_me,
                           &vertsPerPoly,
                           &nAllVerts,
                           &allVerts,
                           &ndtris,
                           &dtris,
                           &npolys,
                           &dmeshes,
                           &polys,
                           &dtrisToPolysMap,
                           &dtrisToTrisMap,
                           &trisToFacesMap);

    MEM_SAFE_FREE(dtrisToPolysMap);
    MEM_SAFE_FREE(dtrisToTrisMap);
    MEM_SAFE_FREE(trisToFacesMap);

    unsigned short *verticesMap = (unsigned short *)MEM_mallocN(sizeof(*verticesMap) * nAllVerts,
                                                                __func__);
    memset(verticesMap, 0xff, sizeof(*verticesMap) * nAllVerts);
    int curIdx = 0;
    // vertices - mesh verts
    // iterate over all polys and create map for their vertices first...
    for (int polyidx = 0; polyidx < npolys; polyidx++) {
      unsigned short *poly = &polys[polyidx * vertsPerPoly * 2];
      for (int i = 0; i < vertsPerPoly; i++) {
        unsigned short idx = poly[i];
        if (idx == 0xffff)
          break;
        if (verticesMap[idx] == 0xffff) {
          verticesMap[idx] = curIdx++;
        }
        poly[i] = verticesMap[idx];
      }
    }
    nverts = curIdx;
    //...then iterate over detailed meshes
    // transform indices to local ones (for each navigation polygon)
    for (int polyidx = 0; polyidx < npolys; polyidx++) {
      unsigned short *poly = &polys[polyidx * vertsPerPoly * 2];
      int nv = polyNumVerts(poly, vertsPerPoly);
      unsigned short *dmesh = &dmeshes[4 * polyidx];
      unsigned short tribase = dmesh[2];
      unsigned short trinum = dmesh[3];
      unsigned short vbase = curIdx;
      for (int j = 0; j < trinum; j++) {
        unsigned short *dtri = &dtris[(tribase + j) * 3 * 2];
        for (int k = 0; k < 3; k++) {
          int newVertexIdx = verticesMap[dtri[k]];
          if (newVertexIdx == 0xffff) {
            newVertexIdx = curIdx++;
            verticesMap[dtri[k]] = newVertexIdx;
          }

          if (newVertexIdx < nverts) {
            // it's polygon vertex ("shared")
            int idxInPoly = polyFindVertex(poly, vertsPerPoly, newVertexIdx);
            if (idxInPoly == -1) {
              CM_Error("building NavMeshObject, can't find vertex in polygon\n");
              MEM_SAFE_FREE(allVerts);
              MEM_freeN(verticesMap);
              return false;
            }
            dtri[k] = idxInPoly;
          }
          else {
            dtri[k] = newVertexIdx - vbase + nv;
          }
        }
      }
      dmesh[0] = vbase - nverts;  // verts base
      dmesh[1] = curIdx - vbase;  // verts num
    }

    vertices = new float[nverts * 3];
    ndvertsuniq = curIdx - nverts;
    if (ndvertsuniq > 0) {
      dvertices = new float[ndvertsuniq * 3];
    }
    for (int vi = 0; vi < nAllVerts; vi++) {
      int newIdx = verticesMap[vi];
      if (newIdx != 0xffff) {
        if (newIdx < nverts) {
          // navigation mesh vertex
          memcpy(vertices + 3 * newIdx, allVerts + 3 * vi, 3 * sizeof(float));
        }
        else {
          // detailed mesh vertex
          memcpy(dvertices + 3 * (newIdx - nverts), allVerts + 3 * vi, 3 * sizeof(float));
        }
      }
    }

    MEM_SAFE_FREE(allVerts);

    MEM_freeN(verticesMap);
  }
  else {
    // create from RAS_MeshObject (detailed mesh is fake)
    RAS_MeshObject *meshobj = GetMesh(0);
    vertsPerPoly = 3;
    nverts = meshobj->m_sharedvertex_map.size();
    if (nverts >= 0xffff)
      return false;
    // calculate count of tris
    int nmeshpolys = meshobj->NumPolygons();
    npolys = nmeshpolys;
    for (int p = 0; p < nmeshpolys; p++) {
      int vertcount = meshobj->GetPolygon(p)->VertexCount();
      npolys += vertcount - 3;
    }

    // create verts
    vertices = new float[nverts * 3];
    float *vert = vertices;
    for (int vi = 0; vi < nverts; vi++) {
      const float *pos = !meshobj->m_sharedvertex_map[vi].empty() ?
                             meshobj->GetVertexLocation(vi) :
                             nullptr;
      if (pos)
        copy_v3_v3(vert, pos);
      else {
        memset(vert, 0, 3 * sizeof(float));  // vertex isn't in any poly, set dummy zero
                                             // coordinates
      }
      vert += 3;
    }

    // create tris
    polys = (unsigned short *)MEM_callocN(sizeof(unsigned short) * 3 * 2 * npolys,
                                          "BuildVertIndArrays polys");
    memset(polys, 0xff, sizeof(unsigned short) * 3 * 2 * npolys);
    unsigned short *poly = polys;
    RAS_Polygon *raspoly;
    for (int p = 0; p < nmeshpolys; p++) {
      raspoly = meshobj->GetPolygon(p);
      for (int v = 0; v < raspoly->VertexCount() - 2; v++) {
        poly[0] = raspoly->GetVertexInfo(0).getOrigIndex();
        for (size_t i = 1; i < 3; i++) {
          poly[i] = raspoly->GetVertexInfo(v + i).getOrigIndex();
        }
        poly += 6;
      }
    }
    dmeshes = nullptr;
    dvertices = nullptr;
    ndvertsuniq = 0;
    dtris = nullptr;
    ndtris = npolys;
  }

  return true;
}

bool KX_NavMeshObject::BuildNavMesh()
{
  if (m_navMesh) {
    delete m_navMesh;
    m_navMesh = nullptr;
  }

  if (GetMeshCount() == 0) {
    CM_Error("can't find mesh for navmesh object: " << m_name);
    return false;
  }

  float *vertices = nullptr, *dvertices = nullptr;
  unsigned short *polys = nullptr, *dtris = nullptr, *dmeshes = nullptr;
  int nverts = 0, npolys = 0, ndvertsuniq = 0, ndtris = 0;
  int vertsPerPoly = 0;
  if (!BuildVertIndArrays(vertices,
                          nverts,
                          polys,
                          npolys,
                          dmeshes,
                          dvertices,
                          ndvertsuniq,
                          dtris,
                          ndtris,
                          vertsPerPoly) ||
      vertsPerPoly < 3) {
    CM_Error("can't build navigation mesh data for object: " << m_name);
    if (vertices) {
      delete[] vertices;
    }
    if (dvertices) {
      delete[] dvertices;
    }
    if (polys) {
      MEM_freeN(polys);
    }
    if (dmeshes) {
      MEM_freeN(dmeshes);
    }
    if (dtris) {
      MEM_freeN(dtris);
    }
    return false;
  }

  MT_Vector3 pos;
  if (dmeshes == nullptr) {
    for (int i = 0; i < nverts; i++) {
      flipAxes(&vertices[i * 3]);
    }
    for (int i = 0; i < ndvertsuniq; i++) {
      flipAxes(&dvertices[i * 3]);
    }
  }

  if (!buildMeshAdjacency(polys, npolys, nverts, vertsPerPoly)) {
    CM_FunctionError("unable to build mesh adjacency information.");
    if (vertices) {
      delete[] vertices;
    }
    if (dvertices) {
      delete[] dvertices;
    }
    if (polys) {
      MEM_freeN(polys);
    }
    if (dmeshes) {
      MEM_freeN(dmeshes);
    }
    if (dtris) {
      MEM_freeN(dtris);
    }
    return false;
  }

  float cs = 0.2f;

  if (!nverts || !npolys) {
    if (vertices) {
      delete[] vertices;
    }
    if (dvertices) {
      delete[] dvertices;
    }
    if (polys) {
      MEM_freeN(polys);
    }
    if (dmeshes) {
      MEM_freeN(dmeshes);
    }
    if (dtris) {
      MEM_freeN(dtris);
    }
    return false;
  }

  float bmin[3], bmax[3];
  calcMeshBounds(vertices, nverts, bmin, bmax);
  // quantize vertex pos
  unsigned short *vertsi = new unsigned short[3 * nverts];
  float ics = 1.f / cs;
  for (int i = 0; i < nverts; i++) {
    vertsi[3 * i + 0] = static_cast<unsigned short>((vertices[3 * i + 0] - bmin[0]) * ics);
    vertsi[3 * i + 1] = static_cast<unsigned short>((vertices[3 * i + 1] - bmin[1]) * ics);
    vertsi[3 * i + 2] = static_cast<unsigned short>((vertices[3 * i + 2] - bmin[2]) * ics);
  }

  // Calculate data size
  const int headerSize = sizeof(dtStatNavMeshHeader);
  const int vertsSize = sizeof(float) * 3 * nverts;
  const int polysSize = sizeof(dtStatPoly) * npolys;
  const int nodesSize = sizeof(dtStatBVNode) * npolys * 2;
  const int detailMeshesSize = sizeof(dtStatPolyDetail) * npolys;
  const int detailVertsSize = sizeof(float) * 3 * ndvertsuniq;
  const int detailTrisSize = sizeof(unsigned char) * 4 * ndtris;

  const int dataSize = headerSize + vertsSize + polysSize + nodesSize + detailMeshesSize +
                       detailVertsSize + detailTrisSize;
  unsigned char *data = new unsigned char[dataSize];

  memset(data, 0, dataSize);

  unsigned char *d = data;
  dtStatNavMeshHeader *header = (dtStatNavMeshHeader *)d;
  d += headerSize;
  float *navVerts = (float *)d;
  d += vertsSize;
  dtStatPoly *navPolys = (dtStatPoly *)d;
  d += polysSize;
  dtStatBVNode *navNodes = (dtStatBVNode *)d;
  d += nodesSize;
  dtStatPolyDetail *navDMeshes = (dtStatPolyDetail *)d;
  d += detailMeshesSize;
  float *navDVerts = (float *)d;
  d += detailVertsSize;
  unsigned char *navDTris = (unsigned char *)d;
  d += detailTrisSize;

  // Store header
  header->magic = DT_STAT_NAVMESH_MAGIC;
  header->version = DT_STAT_NAVMESH_VERSION;
  header->npolys = npolys;
  header->nverts = nverts;
  header->cs = cs;
  header->bmin[0] = bmin[0];
  header->bmin[1] = bmin[1];
  header->bmin[2] = bmin[2];
  header->bmax[0] = bmax[0];
  header->bmax[1] = bmax[1];
  header->bmax[2] = bmax[2];
  header->ndmeshes = npolys;
  header->ndverts = ndvertsuniq;
  header->ndtris = ndtris;

  // Store vertices
  for (int i = 0; i < nverts; ++i) {
    const unsigned short *iv = &vertsi[i * 3];
    float *v = &navVerts[i * 3];
    v[0] = bmin[0] + iv[0] * cs;
    v[1] = bmin[1] + iv[1] * cs;
    v[2] = bmin[2] + iv[2] * cs;
  }
  // memcpy(navVerts, vertices, nverts*3*sizeof(float));

  // Store polygons
  const unsigned short *src = polys;
  for (int i = 0; i < npolys; ++i) {
    dtStatPoly *p = &navPolys[i];
    p->nv = 0;
    for (int j = 0; j < vertsPerPoly; ++j) {
      if (src[j] == 0xffff)
        break;
      p->v[j] = src[j];
      p->n[j] = src[vertsPerPoly + j] + 1;
      p->nv++;
    }
    src += vertsPerPoly * 2;
  }

  header->nnodes = createBVTree(
      vertsi, nverts, polys, npolys, vertsPerPoly, cs, cs, npolys * 2, navNodes);

  if (dmeshes == nullptr) {
    // create fake detail meshes
    for (int i = 0; i < npolys; ++i) {
      dtStatPolyDetail &dtl = navDMeshes[i];
      dtl.vbase = 0;
      dtl.nverts = 0;
      dtl.tbase = i;
      dtl.ntris = 1;
    }
    // setup triangles.
    unsigned char *tri = navDTris;
    for (size_t i = 0; i < ndtris; i++) {
      for (size_t j = 0; j < 3; j++)
        tri[4 * i + j] = j;
    }
  }
  else {
    // verts
    memcpy(navDVerts, dvertices, ndvertsuniq * 3 * sizeof(float));
    // tris
    unsigned char *tri = navDTris;
    for (size_t i = 0; i < ndtris; i++) {
      for (size_t j = 0; j < 3; j++)
        tri[4 * i + j] = dtris[6 * i + j];
    }
    // detailed meshes
    for (int i = 0; i < npolys; ++i) {
      dtStatPolyDetail &dtl = navDMeshes[i];
      dtl.vbase = dmeshes[i * 4 + 0];
      dtl.nverts = dmeshes[i * 4 + 1];
      dtl.tbase = dmeshes[i * 4 + 2];
      dtl.ntris = dmeshes[i * 4 + 3];
    }
  }

  m_navMesh = new dtStatNavMesh;
  m_navMesh->init(data, dataSize, true);

  delete[] vertices;

  /* navmesh conversion is using C guarded alloc for memory allocaitons */
  MEM_freeN(polys);
  if (dmeshes)
    MEM_freeN(dmeshes);
  if (dtris)
    MEM_freeN(dtris);

  if (dvertices)
    delete[] dvertices;

  if (vertsi)
    delete[] vertsi;

  return true;
}

dtStatNavMesh *KX_NavMeshObject::GetNavMesh()
{
  return m_navMesh;
}

void KX_NavMeshObject::DrawNavMesh(NavMeshRenderMode renderMode)
{
  if (!m_navMesh)
    return;
  MT_Vector4 color(0.0f, 0.0f, 0.0f, 1.0f);

  switch (renderMode) {
    case RM_POLYS:
    case RM_WALLS:
      for (int pi = 0; pi < m_navMesh->getPolyCount(); pi++) {
        const dtStatPoly *poly = m_navMesh->getPoly(pi);

        for (int i = 0, j = (int)poly->nv - 1; i < (int)poly->nv; j = i++) {
          if (poly->n[j] && renderMode == RM_WALLS)
            continue;
          const float *vif = m_navMesh->getVertex(poly->v[i]);
          const float *vjf = m_navMesh->getVertex(poly->v[j]);
          MT_Vector3 vi(vif[0], vif[2], vif[1]);
          MT_Vector3 vj(vjf[0], vjf[2], vjf[1]);
          vi = TransformToWorldCoords(vi);
          vj = TransformToWorldCoords(vj);
          KX_RasterizerDrawDebugLine(vi, vj, color);
        }
      }
      break;
    case RM_TRIS:
      for (int i = 0; i < m_navMesh->getPolyDetailCount(); ++i) {
        const dtStatPoly *p = m_navMesh->getPoly(i);
        const dtStatPolyDetail *pd = m_navMesh->getPolyDetail(i);

        for (int j = 0; j < pd->ntris; ++j) {
          const unsigned char *t = m_navMesh->getDetailTri(pd->tbase + j);
          MT_Vector3 tri[3];
          for (int k = 0; k < 3; ++k) {
            const float *v;
            if (t[k] < p->nv)
              v = m_navMesh->getVertex(p->v[t[k]]);
            else
              v = m_navMesh->getDetailVertex(pd->vbase + (t[k] - p->nv));
            float pos[3];
            rcVcopy(pos, v);
            flipAxes(pos);
            tri[k].setValue(pos);
          }

          for (int k = 0; k < 3; k++)
            tri[k] = TransformToWorldCoords(tri[k]);

          for (int k = 0; k < 3; k++)
            KX_RasterizerDrawDebugLine(tri[k], tri[(k + 1) % 3], color);
        }
      }
      break;
    default:
      /* pass */
      break;
  }
}

MT_Vector3 KX_NavMeshObject::TransformToLocalCoords(const MT_Vector3 &wpos)
{
  MT_Matrix3x3 orientation = NodeGetWorldOrientation();
  const MT_Vector3 &scaling = NodeGetWorldScaling();
  orientation.scale(scaling[0], scaling[1], scaling[2]);
  MT_Transform worldtr(NodeGetWorldPosition(), orientation);
  MT_Transform invworldtr;
  invworldtr.invert(worldtr);
  MT_Vector3 lpos = invworldtr(wpos);
  return lpos;
}

MT_Vector3 KX_NavMeshObject::TransformToWorldCoords(const MT_Vector3 &lpos)
{
  MT_Matrix3x3 orientation = NodeGetWorldOrientation();
  const MT_Vector3 &scaling = NodeGetWorldScaling();
  orientation.scale(scaling[0], scaling[1], scaling[2]);
  MT_Transform worldtr(NodeGetWorldPosition(), orientation);
  MT_Vector3 wpos = worldtr(lpos);
  return wpos;
}

int KX_NavMeshObject::FindPath(const MT_Vector3 &from,
                               const MT_Vector3 &to,
                               float *path,
                               int maxPathLen)
{
  if (!m_navMesh)
    return 0;
  MT_Vector3 localfrom = TransformToLocalCoords(from);
  MT_Vector3 localto = TransformToLocalCoords(to);
  float spos[3], epos[3];
  localfrom.getValue(spos);
  flipAxes(spos);
  localto.getValue(epos);
  flipAxes(epos);
  dtStatPolyRef sPolyRef = m_navMesh->findNearestPoly(spos, polyPickExt);
  dtStatPolyRef ePolyRef = m_navMesh->findNearestPoly(epos, polyPickExt);

  int pathLen = 0;
  if (sPolyRef && ePolyRef) {
    dtStatPolyRef *polys = new dtStatPolyRef[maxPathLen];
    int npolys;
    npolys = m_navMesh->findPath(sPolyRef, ePolyRef, spos, epos, polys, maxPathLen);
    if (npolys) {
      pathLen = m_navMesh->findStraightPath(spos, epos, polys, npolys, path, maxPathLen);
      for (int i = 0; i < pathLen; i++) {
        flipAxes(&path[i * 3]);
        MT_Vector3 waypoint(&path[i * 3]);
        waypoint = TransformToWorldCoords(waypoint);
        waypoint.getValue(&path[i * 3]);
      }
    }

    delete[] polys;
  }

  return pathLen;
}

float KX_NavMeshObject::Raycast(const MT_Vector3 &from, const MT_Vector3 &to)
{
  if (!m_navMesh)
    return 0.f;
  MT_Vector3 localfrom = TransformToLocalCoords(from);
  MT_Vector3 localto = TransformToLocalCoords(to);
  float spos[3], epos[3];
  localfrom.getValue(spos);
  flipAxes(spos);
  localto.getValue(epos);
  flipAxes(epos);
  dtStatPolyRef sPolyRef = m_navMesh->findNearestPoly(spos, polyPickExt);
  float t = 0;
  static dtStatPolyRef polys[MAX_PATH_LEN];
  m_navMesh->raycast(sPolyRef, spos, epos, t, polys, MAX_PATH_LEN);
  return t;
}

void KX_NavMeshObject::DrawPath(const float *path, int pathLen, const MT_Vector4 &color)
{
  MT_Vector3 a, b;
  for (int i = 0; i < pathLen - 1; i++) {
    a.setValue(&path[3 * i]);
    b.setValue(&path[3 * (i + 1)]);
    KX_RasterizerDrawDebugLine(a, b, color);
  }
}

#ifdef WITH_PYTHON
//----------------------------------------------------------------------------
// Python
PyObject *KX_NavMeshObject::game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  KX_NavMeshObject *obj = new KX_NavMeshObject();

  PyObject *proxy = py_base_new(type, PyTuple_Pack(1, obj->GetProxy()), kwds);
  if (!proxy) {
    delete obj;
    return nullptr;
  }

  return proxy;
}

PyTypeObject KX_NavMeshObject::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_NavMeshObject",
                                       sizeof(EXP_PyObjectPlus_Proxy),
                                       0,
                                       py_base_dealloc,
                                       0,
                                       0,
                                       0,
                                       0,
                                       py_base_repr,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       Methods,
                                       0,
                                       0,
                                       &KX_GameObject::Type,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       game_object_new};

PyAttributeDef KX_NavMeshObject::Attributes[] = {
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

// EXP_PYMETHODTABLE_NOARGS(KX_GameObject, getD),
PyMethodDef KX_NavMeshObject::Methods[] = {
    EXP_PYMETHODTABLE(KX_NavMeshObject, findPath),
    EXP_PYMETHODTABLE(KX_NavMeshObject, raycast),
    EXP_PYMETHODTABLE(KX_NavMeshObject, draw),
    EXP_PYMETHODTABLE(KX_NavMeshObject, rebuild),
    {nullptr, nullptr}  // Sentinel
};

EXP_PYMETHODDEF_DOC(KX_NavMeshObject,
                    findPath,
                    "findPath(start, goal): find path from start to goal points\n"
                    "Returns a path as list of points)\n")
{
  PyObject *ob_from, *ob_to;
  if (!PyArg_ParseTuple(args, "OO:getPath", &ob_from, &ob_to))
    return nullptr;
  MT_Vector3 from, to;
  if (!PyVecTo(ob_from, from) || !PyVecTo(ob_to, to))
    return nullptr;

  float path[MAX_PATH_LEN * 3];
  int pathLen = FindPath(from, to, path, MAX_PATH_LEN);
  PyObject *pathList = PyList_New(pathLen);
  for (int i = 0; i < pathLen; i++) {
    MT_Vector3 point(&path[3 * i]);
    PyList_SET_ITEM(pathList, i, PyObjectFrom(point));
  }

  return pathList;
}

EXP_PYMETHODDEF_DOC(KX_NavMeshObject,
                    raycast,
                    "raycast(start, goal): raycast from start to goal points\n"
                    "Returns hit factor)\n")
{
  PyObject *ob_from, *ob_to;
  if (!PyArg_ParseTuple(args, "OO:getPath", &ob_from, &ob_to))
    return nullptr;
  MT_Vector3 from, to;
  if (!PyVecTo(ob_from, from) || !PyVecTo(ob_to, to))
    return nullptr;
  float hit = Raycast(from, to);
  return PyFloat_FromDouble(hit);
}

EXP_PYMETHODDEF_DOC(KX_NavMeshObject,
                    draw,
                    "draw(mode): navigation mesh debug drawing\n"
                    "mode: WALLS, POLYS, TRIS\n")
{
  int arg;
  NavMeshRenderMode renderMode = RM_TRIS;
  if (PyArg_ParseTuple(args, "i:rebuild", &arg) && arg >= 0 && arg < RM_MAX)
    renderMode = (NavMeshRenderMode)arg;
  DrawNavMesh(renderMode);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_NavMeshObject, rebuild, "rebuild(): rebuild navigation mesh\n")
{
  BuildNavMesh();
  Py_RETURN_NONE;
}

#endif  // WITH_PYTHON
