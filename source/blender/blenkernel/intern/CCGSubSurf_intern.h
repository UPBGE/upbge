/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

/**
 * Definitions which defines internal behavior of CCGSubSurf.
 */

/* Define this to see dump of the grids after the subsurf applied. */
#undef DUMP_RESULT_GRIDS

/* used for normalize_v3 in BLI_math_vector
 * float.h's FLT_EPSILON causes trouble with subsurf normals - campbell */
#define EPSILON (1.0e-35f)

/* With this limit a single triangle becomes over 3 million faces */
#define CCGSUBSURF_LEVEL_MAX 11

/**
 * Common type definitions.
 */

typedef unsigned char byte;

/**
 * Hash implementation.
 */

typedef struct _EHEntry {
  struct _EHEntry *next;
  void *key;
} EHEntry;

typedef struct _EHash {
  EHEntry **buckets;
  int numEntries, curSize, curSizeIdx;

  CCGAllocatorIFC allocatorIFC;
  CCGAllocatorHDL allocator;
} EHash;

typedef void (*EHEntryFreeFP)(EHEntry *, void *);

#define EHASH_alloc(eh, nb) ((eh)->allocatorIFC.alloc((eh)->allocator, nb))
#define EHASH_free(eh, ptr) ((eh)->allocatorIFC.free((eh)->allocator, ptr))
#define EHASH_hash(eh, item) (((uintptr_t)(item)) % ((unsigned int)(eh)->curSize))

/* Generic hash functions. */

EHash *ccg_ehash_new(int estimatedNumEntries,
                     CCGAllocatorIFC *allocatorIFC,
                     CCGAllocatorHDL allocator);
void ccg_ehash_free(EHash *eh, EHEntryFreeFP freeEntry, void *userData);
void ccg_ehash_insert(EHash *eh, EHEntry *entry);
void *ccg_ehash_lookupWithPrev(EHash *eh, void *key, void ***prevp_r);
void *ccg_ehash_lookup(EHash *eh, void *key);

/* Hash elements iteration. */

void ccg_ehashIterator_init(EHash *eh, EHashIterator *ehi);
void *ccg_ehashIterator_getCurrent(EHashIterator *ehi);
void ccg_ehashIterator_next(EHashIterator *ehi);
int ccg_ehashIterator_isStopped(EHashIterator *ehi);

/**
 * Standard allocator implementation.
 */

CCGAllocatorIFC *ccg_getStandardAllocatorIFC(void);

/**
 * Catmull-Clark Gridding Subdivision Surface.
 */

/* TODO(sergey): Get rid of this, it's more or less a bad level call. */
struct DerivedMesh;

/* ** Data structures, constants. enums ** */

enum {
  Vert_eEffected = (1 << 0),
  Vert_eChanged = (1 << 1),
  Vert_eSeam = (1 << 2),
} /*VertFlags*/;

enum {
  Edge_eEffected = (1 << 0),
} /*CCGEdgeFlags*/;

enum {
  Face_eEffected = (1 << 0),
} /*FaceFlags*/;

struct CCGVert {
  CCGVert *next;   /* EHData.next */
  CCGVertHDL vHDL; /* EHData.key */

  short numEdges, numFaces, flags;
  int osd_index; /* Index of the vertex in the map, used by OSD. */

  CCGEdge **edges;
  CCGFace **faces;
  /* byte *levelData; */
  /* byte *userData; */
};

struct CCGEdge {
  CCGEdge *next;   /* EHData.next */
  CCGEdgeHDL eHDL; /* EHData.key */

  short numFaces, flags;
  float crease;

  CCGVert *v0, *v1;
  CCGFace **faces;

  /* byte *levelData; */
  /* byte *userData; */
};

struct CCGFace {
  CCGFace *next;   /* EHData.next */
  CCGFaceHDL fHDL; /* EHData.key */

  short numVerts, flags;
  int osd_index;

  /* CCGVert **verts; */
  /* CCGEdge **edges; */
  /* byte *centerData; */
  /* byte **gridData; */
  /* byte *userData; */
};

typedef enum {
  eSyncState_None = 0,
  eSyncState_Vert,
  eSyncState_Edge,
  eSyncState_Face,
  eSyncState_Partial,
} SyncState;

struct CCGSubSurf {
  EHash *vMap; /* map of CCGVertHDL -> Vert */
  EHash *eMap; /* map of CCGEdgeHDL -> Edge */
  EHash *fMap; /* map of CCGFaceHDL -> Face */

  CCGMeshIFC meshIFC;

  CCGAllocatorIFC allocatorIFC;
  CCGAllocatorHDL allocator;

  int subdivLevels;
  int numGrids;
  int allowEdgeCreation;
  float defaultCreaseValue;
  void *defaultEdgeUserData;

  void *q, *r;

  /* Data for calc vert normals. */
  int calcVertNormals;
  int normalDataOffset;

  /* Data for paint masks. */
  int allocMask;
  int maskDataOffset;

  /* Data for age'ing (to debug sync). */
  int currentAge;
  int useAgeCounts;
  int vertUserAgeOffset;
  int edgeUserAgeOffset;
  int faceUserAgeOffset;

  /* Data used during syncing. */
  SyncState syncState;

  EHash *oldVMap, *oldEMap, *oldFMap;
  int lenTempArrays;
  CCGVert **tempVerts;
  CCGEdge **tempEdges;
};

/* ** Utility macros ** */

#define CCGSUBSURF_alloc(ss, nb) ((ss)->allocatorIFC.alloc((ss)->allocator, nb))
#define CCGSUBSURF_realloc(ss, ptr, nb, ob) \
  ((ss)->allocatorIFC.realloc((ss)->allocator, ptr, nb, ob))
#define CCGSUBSURF_free(ss, ptr) ((ss)->allocatorIFC.free((ss)->allocator, ptr))

#define VERT_getCo(v, lvl) ccg_vert_getCo(v, lvl, vertDataSize)
#define VERT_getNo(v, lvl) ccg_vert_getNo(v, lvl, vertDataSize, normalDataOffset)
#define EDGE_getCo(e, lvl, x) ccg_edge_getCo(e, lvl, x, vertDataSize)
#define EDGE_getNo(e, lvl, x) ccg_edge_getNo(e, lvl, x, vertDataSize, normalDataOffset)
#define FACE_getIFNo(f, lvl, S, x, y) \
  ccg_face_getIFNo(f, lvl, S, x, y, subdivLevels, vertDataSize, normalDataOffset)
#if 0
#  define FACE_calcIFNo(f, lvl, S, x, y, no) \
    _face_calcIFNo(f, lvl, S, x, y, no, subdivLevels, vertDataSize)
#endif
#define FACE_getIENo(f, lvl, S, x) \
  ccg_face_getIENo(f, lvl, S, x, subdivLevels, vertDataSize, normalDataOffset)
#define FACE_getIECo(f, lvl, S, x) ccg_face_getIECo(f, lvl, S, x, subdivLevels, vertDataSize)
#define FACE_getIFCo(f, lvl, S, x, y) ccg_face_getIFCo(f, lvl, S, x, y, subdivLevels, vertDataSize)

#define NormZero(av) \
  { \
    float *_a = (float *)av; \
    _a[0] = _a[1] = _a[2] = 0.0f; \
  } \
  (void)0
#define NormCopy(av, bv) \
  { \
    float *_a = (float *)av, *_b = (float *)bv; \
    _a[0] = _b[0]; \
    _a[1] = _b[1]; \
    _a[2] = _b[2]; \
  } \
  (void)0
#define NormAdd(av, bv) \
  { \
    float *_a = (float *)av, *_b = (float *)bv; \
    _a[0] += _b[0]; \
    _a[1] += _b[1]; \
    _a[2] += _b[2]; \
  } \
  (void)0

/* ** General purpose functions  ** */

/* * CCGSubSurf.c * */

void ccgSubSurf__allFaces(CCGSubSurf *ss, CCGFace ***faces, int *numFaces, int *freeFaces);
void ccgSubSurf__effectedFaceNeighbors(CCGSubSurf *ss,
                                       CCGFace **faces,
                                       int numFaces,
                                       CCGVert ***verts,
                                       int *numVerts,
                                       CCGEdge ***edges,
                                       int *numEdges);

/* * CCGSubSurf_legacy.c * */

void ccgSubSurf__sync_legacy(CCGSubSurf *ss);

/* * CCGSubSurf_opensubdiv.c * */

void ccgSubSurf__sync_opensubdiv(CCGSubSurf *ss);

/* * CCGSubSurf_opensubdiv_converter.c * */

struct OpenSubdiv_Converter;

void ccgSubSurf_converter_setup_from_derivedmesh(CCGSubSurf *ss,
                                                 struct DerivedMesh *dm,
                                                 struct OpenSubdiv_Converter *converter);

void ccgSubSurf_converter_setup_from_ccg(CCGSubSurf *ss, struct OpenSubdiv_Converter *converter);

void ccgSubSurf_converter_free(struct OpenSubdiv_Converter *converter);

/* * CCGSubSurf_util.c * */

#ifdef DUMP_RESULT_GRIDS
void ccgSubSurf__dumpCoords(CCGSubSurf *ss);
#endif

#include "CCGSubSurf_inline.h"
