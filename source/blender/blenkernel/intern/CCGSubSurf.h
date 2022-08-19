/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

typedef void *CCGMeshHDL;
typedef void *CCGVertHDL;
typedef void *CCGEdgeHDL;
typedef void *CCGFaceHDL;

typedef struct CCGEdge CCGEdge;
typedef struct CCGFace CCGFace;
typedef struct CCGSubSurf CCGSubSurf;
typedef struct CCGVert CCGVert;

typedef struct CCGMeshIFC {
  int vertUserSize, edgeUserSize, faceUserSize;
  int numLayers;
  int vertDataSize;
  int simpleSubdiv;
} CCGMeshIFC;

/***/

typedef void *CCGAllocatorHDL;

typedef struct CCGAllocatorIFC {
  void *(*alloc)(CCGAllocatorHDL a, int numBytes);
  void *(*realloc)(CCGAllocatorHDL a, void *ptr, int newSize, int oldSize);
  void (*free)(CCGAllocatorHDL a, void *ptr);
  void (*release)(CCGAllocatorHDL a);
} CCGAllocatorIFC;

/* private, so we can allocate on the stack */
typedef struct _EHashIterator {
  struct _EHash *eh;
  int curBucket;
  struct _EHEntry *curEntry;
} EHashIterator;

/***/

typedef enum {
  eCCGError_None = 0,

  eCCGError_InvalidSyncState,
  eCCGError_InvalidValue,
} CCGError;

/***/

/* TODO(sergey): This actually depends on subsurf level as well. */
#define CCG_TASK_LIMIT 16

/***/

CCGSubSurf *ccgSubSurf_new(CCGMeshIFC *ifc,
                           int subdivLevels,
                           CCGAllocatorIFC *allocatorIFC,
                           CCGAllocatorHDL allocator);
void ccgSubSurf_free(CCGSubSurf *ss);

CCGError ccgSubSurf_initFullSync(CCGSubSurf *ss);
CCGError ccgSubSurf_initPartialSync(CCGSubSurf *ss);
#ifdef WITH_OPENSUBDIV
CCGError ccgSubSurf_initOpenSubdivSync(CCGSubSurf *ss);
#endif

CCGError ccgSubSurf_syncVert(
    CCGSubSurf *ss, CCGVertHDL vHDL, const void *vertData, int seam, CCGVert **v_r);
CCGError ccgSubSurf_syncEdge(CCGSubSurf *ss,
                             CCGEdgeHDL eHDL,
                             CCGVertHDL e_vHDL0,
                             CCGVertHDL e_vHDL1,
                             float crease,
                             CCGEdge **e_r);
CCGError ccgSubSurf_syncFace(
    CCGSubSurf *ss, CCGFaceHDL fHDL, int numVerts, CCGVertHDL *vHDLs, CCGFace **f_r);

CCGError ccgSubSurf_syncVertDel(CCGSubSurf *ss, CCGVertHDL vHDL);
CCGError ccgSubSurf_syncEdgeDel(CCGSubSurf *ss, CCGEdgeHDL eHDL);
CCGError ccgSubSurf_syncFaceDel(CCGSubSurf *ss, CCGFaceHDL fHDL);

CCGError ccgSubSurf_processSync(CCGSubSurf *ss);

/**
 * Copy face grid coordinates to other places.
 */
CCGError ccgSubSurf_updateFromFaces(CCGSubSurf *ss,
                                    int lvl,
                                    CCGFace **effectedF,
                                    int numEffectedF);
/**
 * Copy other places to face grid coordinates.
 */
CCGError ccgSubSurf_updateToFaces(CCGSubSurf *ss, int lvl, CCGFace **effectedF, int numEffectedF);
/**
 * Update normals for specified faces.
 */
CCGError ccgSubSurf_updateNormals(CCGSubSurf *ss, CCGFace **effectedF, int numEffectedF);
/**
 * Compute subdivision levels from a given starting point, used by multi-res subdivide/propagate,
 * by filling in coordinates at a certain level, and then subdividing that up to the highest level.
 */
CCGError ccgSubSurf_updateLevels(CCGSubSurf *ss, int lvl, CCGFace **effectedF, int numEffectedF);
/**
 * Stitch together face grids, averaging coordinates at edges and vertices, for multi-res
 * displacements.
 */
CCGError ccgSubSurf_stitchFaces(CCGSubSurf *ss, int lvl, CCGFace **effectedF, int numEffectedF);

CCGError ccgSubSurf_setSubdivisionLevels(CCGSubSurf *ss, int subdivisionLevels);

CCGError ccgSubSurf_setAllowEdgeCreation(CCGSubSurf *ss,
                                         int allowEdgeCreation,
                                         float defaultCreaseValue,
                                         void *defaultUserData);
void ccgSubSurf_getAllowEdgeCreation(CCGSubSurf *ss,
                                     int *allowEdgeCreation_r,
                                     float *defaultCreaseValue_r,
                                     void *defaultUserData_r);

void ccgSubSurf_getUseAgeCounts(CCGSubSurf *ss,
                                int *useAgeCounts_r,
                                int *vertUserOffset_r,
                                int *edgeUserOffset_r,
                                int *faceUserOffset_r);
CCGError ccgSubSurf_setUseAgeCounts(
    CCGSubSurf *ss, int useAgeCounts, int vertUserOffset, int edgeUserOffset, int faceUserOffset);

CCGError ccgSubSurf_setCalcVertexNormals(CCGSubSurf *ss, int useVertNormals, int normalDataOffset);
void ccgSubSurf_setAllocMask(CCGSubSurf *ss, int allocMask, int maskOffset);

void ccgSubSurf_setNumLayers(CCGSubSurf *ss, int numLayers);

/***/

int ccgSubSurf_getNumVerts(const CCGSubSurf *ss);
int ccgSubSurf_getNumEdges(const CCGSubSurf *ss);
int ccgSubSurf_getNumFaces(const CCGSubSurf *ss);

int ccgSubSurf_getSubdivisionLevels(const CCGSubSurf *ss);
int ccgSubSurf_getEdgeSize(const CCGSubSurf *ss);
int ccgSubSurf_getEdgeLevelSize(const CCGSubSurf *ss, int level);
int ccgSubSurf_getGridSize(const CCGSubSurf *ss);
int ccgSubSurf_getGridLevelSize(const CCGSubSurf *ss, int level);
int ccgSubSurf_getSimpleSubdiv(const CCGSubSurf *ss);

CCGVert *ccgSubSurf_getVert(CCGSubSurf *ss, CCGVertHDL v);
CCGVertHDL ccgSubSurf_getVertVertHandle(CCGVert *v);
int ccgSubSurf_getVertNumFaces(CCGVert *v);
CCGFace *ccgSubSurf_getVertFace(CCGVert *v, int index);
int ccgSubSurf_getVertNumEdges(CCGVert *v);
CCGEdge *ccgSubSurf_getVertEdge(CCGVert *v, int index);

int ccgSubSurf_getVertAge(CCGSubSurf *ss, CCGVert *v);
void *ccgSubSurf_getVertUserData(CCGSubSurf *ss, CCGVert *v);
void *ccgSubSurf_getVertData(CCGSubSurf *ss, CCGVert *v);
void *ccgSubSurf_getVertLevelData(CCGSubSurf *ss, CCGVert *v, int level);

CCGEdge *ccgSubSurf_getEdge(CCGSubSurf *ss, CCGEdgeHDL e);
CCGEdgeHDL ccgSubSurf_getEdgeEdgeHandle(CCGEdge *e);
int ccgSubSurf_getEdgeNumFaces(CCGEdge *e);
CCGFace *ccgSubSurf_getEdgeFace(CCGEdge *e, int index);
CCGVert *ccgSubSurf_getEdgeVert0(CCGEdge *e);
CCGVert *ccgSubSurf_getEdgeVert1(CCGEdge *e);
float ccgSubSurf_getEdgeCrease(CCGEdge *e);

int ccgSubSurf_getEdgeAge(CCGSubSurf *ss, CCGEdge *e);
void *ccgSubSurf_getEdgeUserData(CCGSubSurf *ss, CCGEdge *e);
void *ccgSubSurf_getEdgeDataArray(CCGSubSurf *ss, CCGEdge *e);
void *ccgSubSurf_getEdgeData(CCGSubSurf *ss, CCGEdge *e, int x);
void *ccgSubSurf_getEdgeLevelData(CCGSubSurf *ss, CCGEdge *e, int x, int level);

CCGFace *ccgSubSurf_getFace(CCGSubSurf *ss, CCGFaceHDL f);
CCGFaceHDL ccgSubSurf_getFaceFaceHandle(CCGFace *f);
int ccgSubSurf_getFaceNumVerts(CCGFace *f);
CCGVert *ccgSubSurf_getFaceVert(CCGFace *f, int index);
CCGEdge *ccgSubSurf_getFaceEdge(CCGFace *f, int index);
int ccgSubSurf_getFaceEdgeIndex(CCGFace *f, CCGEdge *e);

int ccgSubSurf_getFaceAge(CCGSubSurf *ss, CCGFace *f);
void *ccgSubSurf_getFaceUserData(CCGSubSurf *ss, CCGFace *f);
void *ccgSubSurf_getFaceCenterData(CCGFace *f);
void *ccgSubSurf_getFaceGridEdgeDataArray(CCGSubSurf *ss, CCGFace *f, int gridIndex);
void *ccgSubSurf_getFaceGridEdgeData(CCGSubSurf *ss, CCGFace *f, int gridIndex, int x);
void *ccgSubSurf_getFaceGridDataArray(CCGSubSurf *ss, CCGFace *f, int gridIndex);
void *ccgSubSurf_getFaceGridData(CCGSubSurf *ss, CCGFace *f, int gridIndex, int x, int y);

int ccgSubSurf_getNumFinalVerts(const CCGSubSurf *ss);
int ccgSubSurf_getNumFinalEdges(const CCGSubSurf *ss);
int ccgSubSurf_getNumFinalFaces(const CCGSubSurf *ss);

/***/

typedef struct _EHashIterator CCGEdgeIterator;
typedef struct _EHashIterator CCGFaceIterator;
typedef struct _EHashIterator CCGVertIterator;

void ccgSubSurf_initVertIterator(CCGSubSurf *ss, CCGVertIterator *viter);
void ccgSubSurf_initEdgeIterator(CCGSubSurf *ss, CCGEdgeIterator *eiter);
void ccgSubSurf_initFaceIterator(CCGSubSurf *ss, CCGFaceIterator *fiter);

CCGVert *ccgVertIterator_getCurrent(CCGVertIterator *vi);
int ccgVertIterator_isStopped(CCGVertIterator *vi);
void ccgVertIterator_next(CCGVertIterator *vi);

CCGEdge *ccgEdgeIterator_getCurrent(CCGEdgeIterator *ei);
int ccgEdgeIterator_isStopped(CCGEdgeIterator *ei);
void ccgEdgeIterator_next(CCGEdgeIterator *ei);

CCGFace *ccgFaceIterator_getCurrent(CCGFaceIterator *fi);
int ccgFaceIterator_isStopped(CCGFaceIterator *fi);
void ccgFaceIterator_next(CCGFaceIterator *fi);
