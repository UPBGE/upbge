/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

#pragma once

/** \file
 * \ingroup bli
 */

struct ScanFillVert;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScanFillContext {
  ListBase fillvertbase;
  ListBase filledgebase;
  ListBase fillfacebase;

  /* increment this value before adding each curve to skip having to calculate
   * 'poly_nr' for edges and verts (which can take approx half scan-fill time) */
  unsigned short poly_nr;

  /* private */
  struct MemArena *arena;
} ScanFillContext;

#define BLI_SCANFILL_ARENA_SIZE MEM_SIZE_OPTIMAL(1 << 14)

/**
 * \note this is USHRT_MAX so incrementing will set to zero
 * which happens if callers choose to increment #ScanFillContext.poly_nr before adding each curve.
 * Nowhere else in scan-fill do we make use of intentional overflow like this.
 */
#define SF_POLY_UNSET ((unsigned short)-1)

typedef struct ScanFillVert {
  struct ScanFillVert *next, *prev;
  union {
    struct ScanFillVert *v;
    void *p;
    int i;
    unsigned int u;
  } tmp;
  /** vertex location */
  float co[3];
  /** 2D projection of vertex location */
  float xy[2];
  /** index, caller can use how it likes to match the scan-fill result with own data */
  unsigned int keyindex;
  unsigned short poly_nr;
  /** number of edges using this vertex */
  unsigned char edge_count;
  /** vert status */
  unsigned int f : 4;
  /** flag callers can use as they like */
  unsigned int user_flag : 4;
} ScanFillVert;

typedef struct ScanFillEdge {
  struct ScanFillEdge *next, *prev;
  struct ScanFillVert *v1, *v2;
  unsigned short poly_nr;
  unsigned int f : 4;         /* edge status */
  unsigned int user_flag : 4; /* flag callers can use as they like */
  union {
    unsigned char c;
  } tmp;
} ScanFillEdge;

typedef struct ScanFillFace {
  struct ScanFillFace *next, *prev;
  struct ScanFillVert *v1, *v2, *v3;
} ScanFillFace;

/* scanfill.c */

struct ScanFillVert *BLI_scanfill_vert_add(ScanFillContext *sf_ctx, const float vec[3]);
struct ScanFillEdge *BLI_scanfill_edge_add(ScanFillContext *sf_ctx,
                                           struct ScanFillVert *v1,
                                           struct ScanFillVert *v2);

enum {
  /* NOTE(@campbellbarton): using #BLI_SCANFILL_CALC_REMOVE_DOUBLES
   * Assumes ordered edges, otherwise we risk an eternal loop
   * removing double verts. */
  BLI_SCANFILL_CALC_REMOVE_DOUBLES = (1 << 1),

  /* calculate isolated polygons */
  BLI_SCANFILL_CALC_POLYS = (1 << 2),

  /* NOTE: This flag removes checks for overlapping polygons.
   * when this flag is set, we'll never get back more faces than (totvert - 2) */
  BLI_SCANFILL_CALC_HOLES = (1 << 3),

  /* checks valid edge users - can skip for simple loops */
  BLI_SCANFILL_CALC_LOOSE = (1 << 4),
};
void BLI_scanfill_begin(ScanFillContext *sf_ctx);
unsigned int BLI_scanfill_calc(ScanFillContext *sf_ctx, int flag);
unsigned int BLI_scanfill_calc_ex(ScanFillContext *sf_ctx, int flag, const float nor_proj[3]);
void BLI_scanfill_end(ScanFillContext *sf_ctx);

void BLI_scanfill_begin_arena(ScanFillContext *sf_ctx, struct MemArena *arena);
void BLI_scanfill_end_arena(ScanFillContext *sf_ctx, struct MemArena *arena);

/* scanfill_utils.c */

/**
 * Call before scan-fill to remove self intersections.
 *
 * \return false if no changes were made.
 */
bool BLI_scanfill_calc_self_isect(ScanFillContext *sf_ctx,
                                  ListBase *fillvertbase,
                                  ListBase *filledgebase);

#ifdef __cplusplus
}
#endif
