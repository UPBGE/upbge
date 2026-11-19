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

#include "BL_Converter.h"
#include "CM_Message.h"
#include "KX_Globals.h"
#include "KX_NavBuild.h"
#include "KX_NavMesh.h"
#include "KX_ObstacleSimulation.h"
#include "KX_PyMath.h"
#include "KX_Scene.h"
#include "RAS_IVertex.h"
#include "RAS_Polygon.h"
#include "recast-capi.h"

using namespace blender;

#define MAX_PATH_LEN 256

/* Recast's adjacency builder (the copy in extern's recast-capi: Recast keeps its own static). */
static bool buildMeshAdjacency(unsigned short *polys,
                               const int npolys,
                               const int nverts,
                               const int vertsPerPoly)
{
  return recast_buildMeshAdjacency(polys, npolys, nverts, vertsPerPoly) != 0;
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

static int buildRawVertIndicesData(blender::Mesh *me,
                                   int *nverts_r,
                                   float **verts_r,
                                   int *ntris_r,
                                   unsigned short **tris_r,
                                   int **trisToFacesMap_r,
                                   int **recastData)
{
  int nverts = me->verts_num;
  if (nverts >= 0xffff) {
    printf("Converting navmesh: Error! Too many vertices. Max number of vertices %d\n", 0xffff);
    return 0;
  }
  if (nverts == 0) {
    printf("Converting navmesh: Error! There are no vertices!\n");
    return 0;
  }

  // Copie des positions avec swap Y/Z
  float(*v)[3] = (float(*)[3])MEM_new_uninitialized(sizeof(float[3]) * nverts, __func__);
  blender::MutableSpan(reinterpret_cast<blender::float3 *>(v), nverts)
      .copy_from(me->vert_positions());
  float *verts = (float *)v;
  for (int vi = 0; vi < nverts; vi++) {
    float tmp = verts[3 * vi + 1];
    verts[3 * vi + 1] = verts[3 * vi + 2];
    verts[3 * vi + 2] = tmp;
  }

  // Triangulation moderne
  const blender::Span<blender::int3> tris = me->corner_tris();
  const blender::Span<int> tri_faces = me->corner_tri_faces();

  int ntris = tris.size();
  if (ntris == 0) {
    printf("Converting navmesh: Error! There are %i vertices, but no triangles!\n", nverts);
    MEM_delete(verts);
    return 0;
  }

  unsigned short *tris_out = (unsigned short *)MEM_new_zeroed(sizeof(unsigned short) * 3 * ntris,
                                                           "buildRawVertIndicesData tris");
  int *trisToFacesMap = (int *)MEM_new_zeroed(sizeof(int) * ntris,
                                           "buildRawVertIndicesData trisToFacesMap");

  const blender::Span<int> corner_verts = me->corner_verts();

  for (int triIdx = 0; triIdx < ntris; triIdx++) {
    const blender::int3 &tri = tris[triIdx];
    tris_out[3 * triIdx + 0] = (unsigned short)corner_verts[tri[0]];
    tris_out[3 * triIdx + 1] = (unsigned short)corner_verts[tri[2]];  // swap
    tris_out[3 * triIdx + 2] = (unsigned short)corner_verts[tri[1]];  // swap
    trisToFacesMap[triIdx] = tri_faces[triIdx];
  }

  using namespace blender;
  using namespace blender::bke;
  const AttributeAccessor attributes = me->attributes();
  GVArray recast_array = *attributes.lookup(".recast_data", AttrDomain::Face);
  *recastData = (int *)recast_array.get_internal_span().data();

  *nverts_r = nverts;
  *verts_r = verts;
  *ntris_r = ntris;
  *tris_r = tris_out;
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
  unsigned short *newPoly = (unsigned short *)MEM_new_zeroed(sizeof(unsigned short) * capacity,
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
    unsigned char *traversedTris = (unsigned char *)MEM_new_zeroed(
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
      MEM_delete(traversedTris);
      MEM_delete(newPoly);

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
          newPolyBig = (unsigned short *)MEM_new_zeroed(sizeof(unsigned short) * capacity,
                                                     "buildPolygonsByDetailedMeshes newPolyBig");
          memset(newPolyBig, 0xff, sizeof(unsigned short) * capacity);
          memcpy(newPolyBig, newPoly, sizeof(unsigned short) * nv);
          MEM_delete(newPoly);
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
          MEM_delete(traversedTris);
          goto returnLabel;
        }
        tri = neighbortri;
        edge = (twinedge + 1) % 3;
        traversedTris[tri - dtrisBase] = 1;
      }
    }

    adjustedPoly = (unsigned short *)MEM_new_zeroed(sizeof(unsigned short) * nv,
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
    MEM_delete(adjustedPoly);
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

    MEM_delete(traversedTris);
  }

returnLabel:
  MEM_delete(newPoly);

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

  trisMapping = (int *)MEM_new_zeroed(sizeof(int) * ntris, "buildNavMeshData trisMapping");

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
    MEM_delete(trisMapping);
    return 0;
  }

  ndtris = ntris - validTriStart;
  /* fill dtris to faces mapping */
  dtrisToTrisMap = (int *)MEM_new_zeroed(sizeof(int) * ndtris, "buildNavMeshData dtrisToTrisMap");
  memcpy(dtrisToTrisMap, &trisMapping[validTriStart], ndtris * sizeof(int));
  MEM_delete(trisMapping);

  /* create detailed mesh triangles  - copy only valid triangles
   * and reserve memory for adjacency info */
  dtris = (unsigned short *)MEM_new_zeroed(sizeof(unsigned short) * 3 * 2 * ndtris,
                                        "buildNavMeshData dtris");
  memset(dtris, 0xff, sizeof(unsigned short) * 3 * 2 * ndtris);
  for (i = 0; i < ndtris; i++) {
    memcpy(dtris + 3 * 2 * i, tris + 3 * dtrisToTrisMap[i], sizeof(unsigned short) * 3);
  }

  /* create new recast data corresponded to dtris and renumber for continuous indices */
  prevPolyIdx = -1;
  newPolyIdx = 0;
  dtrisToPolysMap = (int *)MEM_new_zeroed(sizeof(int) * ndtris, "buildNavMeshData dtrisToPolysMap");
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
    MEM_delete(trisMapping);
    MEM_delete(dtrisToPolysMap);
    return 0;
  }

  /* create detailed mesh description for each navigation polygon */
  npolys = dtrisToPolysMap[ndtris - 1];
  dmeshes = (unsigned short *)MEM_new_zeroed(sizeof(unsigned short) * npolys * 4,
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
  polys = (unsigned short *)MEM_new_zeroed(sizeof(unsigned short) * npolys * vertsPerPoly * 2,
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
  MEM_delete(dmeshes);
  MEM_delete(dtrisToPolysMap);
  MEM_delete(dtrisToTrisMap);
  return 0;
}

static int buildNavMeshDataByMesh(blender::Mesh *me,
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
    MEM_delete(tris);

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
  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  blender::Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
  blender::Object *ob_eval = DEG_get_evaluated(depsgraph, GetBlenderObject());
  blender::Mesh *final_me = (blender::Mesh *)ob_eval->data;
  using namespace blender;
  using namespace blender::bke;
  const AttributeAccessor attributes = final_me->attributes();
  int *recastData = nullptr;
  if (attributes.contains(".recast_data")) {
    GVArray recast_array = *attributes.lookup(".recast_data", AttrDomain::Face);
    recastData = (int *)recast_array.get_internal_span().data();
  }
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

    MEM_SAFE_DELETE(dtrisToPolysMap);
    MEM_SAFE_DELETE(dtrisToTrisMap);
    MEM_SAFE_DELETE(trisToFacesMap);

    unsigned short *verticesMap = (unsigned short *)MEM_new_uninitialized(sizeof(*verticesMap) * nAllVerts,
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
              MEM_SAFE_DELETE(allVerts);
              MEM_delete(verticesMap);
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

    MEM_SAFE_DELETE(allVerts);

    MEM_delete(verticesMap);
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
    polys = (unsigned short *)MEM_new_zeroed(sizeof(unsigned short) * 3 * 2 * npolys,
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
  delete m_navMesh;
  m_navMesh = nullptr;

  if (GetMeshCount() == 0) {
    CM_Error("can't find mesh for navmesh object: " << m_name);
    return false;
  }

  float *vertices = nullptr, *dvertices = nullptr;
  unsigned short *polys = nullptr, *dtris = nullptr, *dmeshes = nullptr;
  int nverts = 0, npolys = 0, ndvertsuniq = 0, ndtris = 0;
  int vertsPerPoly = 0;
  auto free_arrays = [&]() {
    delete[] vertices;
    delete[] dvertices;
    if (polys) {
      MEM_delete(polys);
    }
    if (dmeshes) {
      MEM_delete(dmeshes);
    }
    if (dtris) {
      MEM_delete(dtris);
    }
  };
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
      vertsPerPoly < 3)
  {
    CM_Error("can't build navigation mesh data for object: " << m_name);
    free_arrays();
    return false;
  }

  /* A mesh without the bake's data (any mesh set to Navigation Mesh): Recast's space here. */
  if (dmeshes == nullptr) {
    for (int i = 0; i < nverts; i++) {
      flipAxes(&vertices[i * 3]);
    }
    for (int i = 0; i < ndvertsuniq; i++) {
      flipAxes(&dvertices[i * 3]);
    }
  }

  if (!nverts || !npolys || !buildMeshAdjacency(polys, npolys, nverts, vertsPerPoly)) {
    CM_FunctionError("unable to build mesh adjacency information.");
    free_arrays();
    return false;
  }

  KX_NavMeshPolygons polygons;
  polygons.verts.assign(vertices, vertices + size_t(nverts) * 3);
  polygons.nvp = vertsPerPoly;
  polygons.polys.assign(polys, polys + size_t(npolys) * vertsPerPoly * 2);
  if (dmeshes != nullptr) {
    polygons.detail_meshes.resize(size_t(npolys) * 4);
    for (size_t i = 0; i < size_t(npolys) * 4; i++) {
      polygons.detail_meshes[i] = dmeshes[i];
    }
    if (ndvertsuniq > 0) {
      polygons.detail_verts.assign(dvertices, dvertices + size_t(ndvertsuniq) * 3);
    }
    polygons.detail_tris.resize(size_t(ndtris) * 3);
    for (int i = 0; i < ndtris; i++) {
      for (int j = 0; j < 3; j++) {
        polygons.detail_tris[size_t(i) * 3 + j] = uint8_t(dtris[6 * i + j]);
      }
    }
  }
  if (KX_Scene *scene = GetScene()) {
    const blender::RecastData &settings = scene->GetBlenderScene()->gm.recastData;
    polygons.walkable_height = settings.agentheight;
    polygons.walkable_radius = settings.agentradius;
    polygons.walkable_climb = settings.agentmaxclimb;
    /* The scene's navigation links (objects with a nav_link property), in this navmesh's space:
     * their numbers are the scene's (KX_Navigation reads the same list). */
    for (const KX_NavLink &link : KX_NavCollectLinks(scene)) {
      for (const MT_Vector3 *point : {&link.start, &link.end}) {
        const MT_Vector3 local = TransformToLocalCoords(*point);
        polygons.link_verts.push_back(float(local[0]));
        polygons.link_verts.push_back(float(local[2]));
        polygons.link_verts.push_back(float(local[1]));
      }
      polygons.link_radii.push_back(link.radius);
      polygons.link_both_ways.push_back(link.both_ways ? 1 : 0);
      polygons.link_ids.push_back(link.user_id);
    }
  }
  free_arrays();

  std::string error;
  std::unique_ptr<KX_NavMesh> navmesh = KX_NavMesh::FromPolygons(polygons, error);
  if (!navmesh) {
    CM_Error("can't build navigation mesh for object " << m_name << ": " << error);
    return false;
  }
  navmesh->SetOwner(this);
  m_navMesh = navmesh.release();
  return true;
}

KX_NavMesh *KX_NavMeshObject::GetNavMesh()
{
  return m_navMesh;
}

void KX_NavMeshObject::DrawNavMesh(NavMeshRenderMode renderMode)
{
  if (!m_navMesh) {
    return;
  }
  const MT_Vector4 color(0.0f, 1.0f, 1.0f, 1.0f);
  const KX_NavMesh::DrawMode mode = renderMode == RM_TRIS  ? KX_NavMesh::DrawMode::Triangles :
                                    renderMode == RM_POLYS ? KX_NavMesh::DrawMode::Polygons :
                                                             KX_NavMesh::DrawMode::Walls;
  m_navMesh->Draw(mode, [&](const MT_Vector3 &a, const MT_Vector3 &b) {
    KX_RasterizerDrawDebugLine(a, b, color);
  });
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
  if (!m_navMesh || maxPathLen <= 0) {
    return 0;
  }
  std::vector<MT_Vector3> corners;
  if (!m_navMesh->FindPath(from, to, corners)) {
    return 0;
  }
  const int count = std::min(int(corners.size()), maxPathLen);
  for (int i = 0; i < count; i++) {
    corners[size_t(i)].getValue(&path[i * 3]);
  }
  return count;
}

float KX_NavMeshObject::Raycast(const MT_Vector3 &from, const MT_Vector3 &to)
{
  if (!m_navMesh) {
    return 0.0f;
  }
  MT_Vector3 start;
  if (!m_navMesh->NearestPoint(from, start)) {
    return 0.0f;
  }
  float fraction = 0.0f;
  MT_Vector3 hit, normal;
  return m_navMesh->Raycast(from, to, fraction, hit, normal) ? fraction : FLT_MAX;
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
