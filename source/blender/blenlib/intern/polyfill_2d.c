/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * An ear clipping algorithm to triangulate single boundary polygons.
 *
 * Details:
 *
 * - The algorithm guarantees all triangles are assigned (number of coords - 2)
 *   and that triangles will have non-overlapping indices (even for degenerate geometry).
 * - Self-intersections are considered degenerate (resulting triangles will overlap).
 * - While multiple polygons aren't supported, holes can still be defined using *key-holes*
 *   (where the polygon doubles back on itself with *exactly* matching coordinates).
 *
 * \note
 *
 * Changes made for Blender.
 *
 * - loop the array to clip last verts first (less array resizing)
 *
 * - advance the ear to clip each iteration
 *   to avoid fan-filling convex shapes (USE_CLIP_EVEN).
 *
 * - avoid intersection tests when there are no convex points (USE_CONVEX_SKIP).
 *
 * \note
 *
 * No globals - keep threadsafe.
 */

#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BLI_alloca.h"
#include "BLI_memarena.h"

#include "BLI_polyfill_2d.h" /* own include */

#include "BLI_strict_flags.h"

/* avoid fan-fill topology */
#define USE_CLIP_EVEN
#define USE_CONVEX_SKIP
/* sweep back-and-forth about convex ears (avoids lop-sided fans) */
#define USE_CLIP_SWEEP
// #define USE_CONVEX_SKIP_TEST

#ifdef USE_CONVEX_SKIP
#  define USE_KDTREE
#endif

/* disable in production, it can fail on near zero area ngons */
// #define USE_STRICT_ASSERT

// #define DEBUG_TIME
#ifdef DEBUG_TIME
#  include "PIL_time_utildefines.h"
#endif

typedef signed char eSign;

#ifdef USE_KDTREE
/**
 * Spatial optimization for point-in-triangle intersection checks.
 * The simple version of this algorithm is `O(n^2)` complexity
 * (every point needing to check the triangle defined by every other point),
 * Using a binary-tree reduces the complexity to `O(n log n)`
 * plus some overhead of creating the tree.
 *
 * This is a single purpose KDTree based on BLI_kdtree with some modifications
 * to better suit polyfill2d.
 * - #KDTreeNode2D is kept small (only 16 bytes),
 *   by not storing coords in the nodes and using index values rather than pointers
 *   to reference neg/pos values.
 *
 * - #kdtree2d_isect_tri is the only function currently used.
 *   This simply intersects a triangle with the kdtree points.
 *
 * - the KDTree is only built & used when the polygon is concave.
 */

typedef bool axis_t;

/* use for sorting */
typedef struct KDTreeNode2D_head {
  uint neg, pos;
  uint index;
} KDTreeNode2D_head;

typedef struct KDTreeNode2D {
  uint neg, pos;
  uint index;
  axis_t axis; /* range is only (0-1) */
  ushort flag;
  uint parent;
} KDTreeNode2D;

struct KDTree2D {
  KDTreeNode2D *nodes;
  const float (*coords)[2];
  uint root;
  uint node_num;
  uint *nodes_map; /* index -> node lookup */
};

struct KDRange2D {
  float min, max;
};
#endif /* USE_KDTREE */

enum {
  CONCAVE = -1,
  TANGENTIAL = 0,
  CONVEX = 1,
};

typedef struct PolyFill {
  struct PolyIndex *indices; /* vertex aligned */

  const float (*coords)[2];
  uint coords_num;
#ifdef USE_CONVEX_SKIP
  uint coords_num_concave;
#endif

  /* A polygon with n vertices has a triangulation of n-2 triangles. */
  uint (*tris)[3];
  uint tris_num;

#ifdef USE_KDTREE
  struct KDTree2D kdtree;
#endif
} PolyFill;

/** Circular double linked-list. */
typedef struct PolyIndex {
  struct PolyIndex *next, *prev;
  uint index;
  eSign sign;
} PolyIndex;

/* based on libgdx 2013-11-28, apache 2.0 licensed */

static void pf_coord_sign_calc(PolyFill *pf, PolyIndex *pi);

static PolyIndex *pf_ear_tip_find(PolyFill *pf
#ifdef USE_CLIP_EVEN
                                  ,
                                  PolyIndex *pi_ear_init
#endif
#ifdef USE_CLIP_SWEEP
                                  ,
                                  bool reverse
#endif
);

static bool pf_ear_tip_check(PolyFill *pf, PolyIndex *pi_ear_tip);
static void pf_ear_tip_cut(PolyFill *pf, PolyIndex *pi_ear_tip);

BLI_INLINE eSign signum_enum(float a)
{
  if (UNLIKELY(a == 0.0f)) {
    return 0;
  }
  if (a > 0.0f) {
    return 1;
  }

  return -1;
}

/**
 * alternative version of #area_tri_signed_v2
 * needed because of float precision issues
 *
 * \note removes / 2 since its not needed since we only need the sign.
 */
BLI_INLINE float area_tri_signed_v2_alt_2x(const float v1[2], const float v2[2], const float v3[2])
{
  float d2[2], d3[2];
  sub_v2_v2v2(d2, v2, v1);
  sub_v2_v2v2(d3, v3, v1);
  return (d2[0] * d3[1]) - (d3[0] * d2[1]);
}

static eSign span_tri_v2_sign(const float v1[2], const float v2[2], const float v3[2])
{
  return signum_enum(area_tri_signed_v2_alt_2x(v3, v2, v1));
}

#ifdef USE_KDTREE
#  define KDNODE_UNSET ((uint)-1)

enum {
  KDNODE_FLAG_REMOVED = (1 << 0),
};

static void kdtree2d_new(struct KDTree2D *tree, uint tot, const float (*coords)[2])
{
  /* set by caller */
  // tree->nodes = nodes;
  tree->coords = coords;
  tree->root = KDNODE_UNSET;
  tree->node_num = tot;
}

/**
 * no need for kdtree2d_insert, since we know the coords array.
 */
static void kdtree2d_init(struct KDTree2D *tree, const uint coords_num, const PolyIndex *indices)
{
  KDTreeNode2D *node;
  uint i;

  for (i = 0, node = tree->nodes; i < coords_num; i++) {
    if (indices[i].sign != CONVEX) {
      node->neg = node->pos = KDNODE_UNSET;
      node->index = indices[i].index;
      node->axis = 0;
      node->flag = 0;
      node++;
    }
  }

  BLI_assert(tree->node_num == (uint)(node - tree->nodes));
}

static uint kdtree2d_balance_recursive(
    KDTreeNode2D *nodes, uint node_num, axis_t axis, const float (*coords)[2], const uint ofs)
{
  KDTreeNode2D *node;
  uint neg, pos, median, i, j;

  if (node_num <= 0) {
    return KDNODE_UNSET;
  }
  if (node_num == 1) {
    return 0 + ofs;
  }

  /* Quick-sort style sorting around median. */
  neg = 0;
  pos = node_num - 1;
  median = node_num / 2;

  while (pos > neg) {
    const float co = coords[nodes[pos].index][axis];
    i = neg - 1;
    j = pos;

    while (1) {
      while (coords[nodes[++i].index][axis] < co) { /* pass */
      }
      while (coords[nodes[--j].index][axis] > co && j > neg) { /* pass */
      }

      if (i >= j) {
        break;
      }
      SWAP(KDTreeNode2D_head, *(KDTreeNode2D_head *)&nodes[i], *(KDTreeNode2D_head *)&nodes[j]);
    }

    SWAP(KDTreeNode2D_head, *(KDTreeNode2D_head *)&nodes[i], *(KDTreeNode2D_head *)&nodes[pos]);
    if (i >= median) {
      pos = i - 1;
    }
    if (i <= median) {
      neg = i + 1;
    }
  }

  /* Set node and sort sub-nodes. */
  node = &nodes[median];
  node->axis = axis;
  axis = !axis;
  node->neg = kdtree2d_balance_recursive(nodes, median, axis, coords, ofs);
  node->pos = kdtree2d_balance_recursive(
      &nodes[median + 1], (node_num - (median + 1)), axis, coords, (median + 1) + ofs);

  return median + ofs;
}

static void kdtree2d_balance(struct KDTree2D *tree)
{
  tree->root = kdtree2d_balance_recursive(tree->nodes, tree->node_num, 0, tree->coords, 0);
}

static void kdtree2d_init_mapping(struct KDTree2D *tree)
{
  uint i;
  KDTreeNode2D *node;

  for (i = 0, node = tree->nodes; i < tree->node_num; i++, node++) {
    if (node->neg != KDNODE_UNSET) {
      tree->nodes[node->neg].parent = i;
    }
    if (node->pos != KDNODE_UNSET) {
      tree->nodes[node->pos].parent = i;
    }

    /* build map */
    BLI_assert(tree->nodes_map[node->index] == KDNODE_UNSET);
    tree->nodes_map[node->index] = i;
  }

  tree->nodes[tree->root].parent = KDNODE_UNSET;
}

static void kdtree2d_node_remove(struct KDTree2D *tree, uint index)
{
  uint node_index = tree->nodes_map[index];
  KDTreeNode2D *node;

  if (node_index == KDNODE_UNSET) {
    return;
  }

  tree->nodes_map[index] = KDNODE_UNSET;

  node = &tree->nodes[node_index];
  tree->node_num -= 1;

  BLI_assert((node->flag & KDNODE_FLAG_REMOVED) == 0);
  node->flag |= KDNODE_FLAG_REMOVED;

  while ((node->neg == KDNODE_UNSET) && (node->pos == KDNODE_UNSET) &&
         (node->parent != KDNODE_UNSET)) {
    KDTreeNode2D *node_parent = &tree->nodes[node->parent];

    BLI_assert((uint)(node - tree->nodes) == node_index);
    if (node_parent->neg == node_index) {
      node_parent->neg = KDNODE_UNSET;
    }
    else {
      BLI_assert(node_parent->pos == node_index);
      node_parent->pos = KDNODE_UNSET;
    }

    if (node_parent->flag & KDNODE_FLAG_REMOVED) {
      node_index = node->parent;
      node = node_parent;
    }
    else {
      break;
    }
  }
}

static bool kdtree2d_isect_tri_recursive(const struct KDTree2D *tree,
                                         const uint tri_index[3],
                                         const float *tri_coords[3],
                                         const float tri_center[2],
                                         const struct KDRange2D bounds[2],
                                         const KDTreeNode2D *node)
{
  const float *co = tree->coords[node->index];

  /* bounds then triangle intersect */
  if ((node->flag & KDNODE_FLAG_REMOVED) == 0) {
    /* bounding box test first */
    if ((co[0] >= bounds[0].min) && (co[0] <= bounds[0].max) && (co[1] >= bounds[1].min) &&
        (co[1] <= bounds[1].max)) {
      if ((span_tri_v2_sign(tri_coords[0], tri_coords[1], co) != CONCAVE) &&
          (span_tri_v2_sign(tri_coords[1], tri_coords[2], co) != CONCAVE) &&
          (span_tri_v2_sign(tri_coords[2], tri_coords[0], co) != CONCAVE)) {
        if (!ELEM(node->index, tri_index[0], tri_index[1], tri_index[2])) {
          return true;
        }
      }
    }
  }

#  define KDTREE2D_ISECT_TRI_RECURSE_NEG \
    (((node->neg != KDNODE_UNSET) && (co[node->axis] >= bounds[node->axis].min)) && \
     (kdtree2d_isect_tri_recursive( \
         tree, tri_index, tri_coords, tri_center, bounds, &tree->nodes[node->neg])))
#  define KDTREE2D_ISECT_TRI_RECURSE_POS \
    (((node->pos != KDNODE_UNSET) && (co[node->axis] <= bounds[node->axis].max)) && \
     (kdtree2d_isect_tri_recursive( \
         tree, tri_index, tri_coords, tri_center, bounds, &tree->nodes[node->pos])))

  if (tri_center[node->axis] > co[node->axis]) {
    if (KDTREE2D_ISECT_TRI_RECURSE_POS) {
      return true;
    }
    if (KDTREE2D_ISECT_TRI_RECURSE_NEG) {
      return true;
    }
  }
  else {
    if (KDTREE2D_ISECT_TRI_RECURSE_NEG) {
      return true;
    }
    if (KDTREE2D_ISECT_TRI_RECURSE_POS) {
      return true;
    }
  }

#  undef KDTREE2D_ISECT_TRI_RECURSE_NEG
#  undef KDTREE2D_ISECT_TRI_RECURSE_POS

  BLI_assert(node->index != KDNODE_UNSET);

  return false;
}

static bool kdtree2d_isect_tri(struct KDTree2D *tree, const uint ind[3])
{
  const float *vs[3];
  uint i;
  struct KDRange2D bounds[2] = {
      {FLT_MAX, -FLT_MAX},
      {FLT_MAX, -FLT_MAX},
  };
  float tri_center[2] = {0.0f, 0.0f};

  for (i = 0; i < 3; i++) {
    vs[i] = tree->coords[ind[i]];

    add_v2_v2(tri_center, vs[i]);

    CLAMP_MAX(bounds[0].min, vs[i][0]);
    CLAMP_MIN(bounds[0].max, vs[i][0]);
    CLAMP_MAX(bounds[1].min, vs[i][1]);
    CLAMP_MIN(bounds[1].max, vs[i][1]);
  }

  mul_v2_fl(tri_center, 1.0f / 3.0f);

  return kdtree2d_isect_tri_recursive(tree, ind, vs, tri_center, bounds, &tree->nodes[tree->root]);
}

#endif /* USE_KDTREE */

static uint *pf_tri_add(PolyFill *pf)
{
  return pf->tris[pf->tris_num++];
}

static void pf_coord_remove(PolyFill *pf, PolyIndex *pi)
{
#ifdef USE_KDTREE
  /* avoid double lookups, since convex coords are ignored when testing intersections */
  if (pf->kdtree.node_num) {
    kdtree2d_node_remove(&pf->kdtree, pi->index);
  }
#endif

  pi->next->prev = pi->prev;
  pi->prev->next = pi->next;

  if (UNLIKELY(pf->indices == pi)) {
    pf->indices = pi->next;
  }
#ifdef DEBUG
  pi->index = (uint)-1;
  pi->next = pi->prev = NULL;
#endif

  pf->coords_num -= 1;
}

static void pf_triangulate(PolyFill *pf)
{
  /* localize */
  PolyIndex *pi_ear;

#ifdef USE_CLIP_EVEN
  PolyIndex *pi_ear_init = pf->indices;
#endif
#ifdef USE_CLIP_SWEEP
  bool reverse = false;
#endif

  while (pf->coords_num > 3) {
    PolyIndex *pi_prev, *pi_next;
    eSign sign_orig_prev, sign_orig_next;

    pi_ear = pf_ear_tip_find(pf
#ifdef USE_CLIP_EVEN
                             ,
                             pi_ear_init
#endif
#ifdef USE_CLIP_SWEEP
                             ,
                             reverse
#endif
    );

#ifdef USE_CONVEX_SKIP
    if (pi_ear->sign != CONVEX) {
      pf->coords_num_concave -= 1;
    }
#endif

    pi_prev = pi_ear->prev;
    pi_next = pi_ear->next;

    pf_ear_tip_cut(pf, pi_ear);

    /* The type of the two vertices adjacent to the clipped vertex may have changed. */
    sign_orig_prev = pi_prev->sign;
    sign_orig_next = pi_next->sign;

    /* check if any verts became convex the (else if)
     * case is highly unlikely but may happen with degenerate polygons */
    if (sign_orig_prev != CONVEX) {
      pf_coord_sign_calc(pf, pi_prev);
#ifdef USE_CONVEX_SKIP
      if (pi_prev->sign == CONVEX) {
        pf->coords_num_concave -= 1;
#  ifdef USE_KDTREE
        kdtree2d_node_remove(&pf->kdtree, pi_prev->index);
#  endif
      }
#endif
    }
    if (sign_orig_next != CONVEX) {
      pf_coord_sign_calc(pf, pi_next);
#ifdef USE_CONVEX_SKIP
      if (pi_next->sign == CONVEX) {
        pf->coords_num_concave -= 1;
#  ifdef USE_KDTREE
        kdtree2d_node_remove(&pf->kdtree, pi_next->index);
#  endif
      }
#endif
    }

#ifdef USE_CLIP_EVEN
#  ifdef USE_CLIP_SWEEP
    pi_ear_init = reverse ? pi_prev->prev : pi_next->next;
#  else
    pi_ear_init = pi_next->next;
#  endif
#endif

#ifdef USE_CLIP_EVEN
#  ifdef USE_CLIP_SWEEP
    if (pi_ear_init->sign != CONVEX) {
      /* take the extra step since this ear isn't a good candidate */
      pi_ear_init = reverse ? pi_ear_init->prev : pi_ear_init->next;
      reverse = !reverse;
    }
#  endif
#else
    if ((reverse ? pi_prev->prev : pi_next->next)->sign != CONVEX) {
      reverse = !reverse;
    }
#endif
  }

  if (pf->coords_num == 3) {
    uint *tri = pf_tri_add(pf);
    pi_ear = pf->indices;
    tri[0] = pi_ear->index;
    pi_ear = pi_ear->next;
    tri[1] = pi_ear->index;
    pi_ear = pi_ear->next;
    tri[2] = pi_ear->index;
  }
}

/**
 * \return CONCAVE, TANGENTIAL or CONVEX
 */
static void pf_coord_sign_calc(PolyFill *pf, PolyIndex *pi)
{
  /* localize */
  const float(*coords)[2] = pf->coords;

  pi->sign = span_tri_v2_sign(coords[pi->prev->index], coords[pi->index], coords[pi->next->index]);
}

static PolyIndex *pf_ear_tip_find(PolyFill *pf
#ifdef USE_CLIP_EVEN
                                  ,
                                  PolyIndex *pi_ear_init
#endif
#ifdef USE_CLIP_SWEEP
                                  ,
                                  bool reverse
#endif
)
{
  /* localize */
  const uint coords_num = pf->coords_num;
  PolyIndex *pi_ear;

  uint i;

#ifdef USE_CLIP_EVEN
  pi_ear = pi_ear_init;
#else
  pi_ear = pf->indices;
#endif

  i = coords_num;
  while (i--) {
    if (pf_ear_tip_check(pf, pi_ear)) {
      return pi_ear;
    }
#ifdef USE_CLIP_SWEEP
    pi_ear = reverse ? pi_ear->prev : pi_ear->next;
#else
    pi_ear = pi_ear->next;
#endif
  }

  /* Desperate mode: if no vertex is an ear tip,
   * we are dealing with a degenerate polygon (e.g. nearly collinear).
   * Note that the input was not necessarily degenerate,
   * but we could have made it so by clipping some valid ears.
   *
   * Idea taken from Martin Held, "FIST: Fast industrial-strength triangulation of polygons",
   * Algorithmica (1998),
   * http://citeseerx.ist.psu.edu/viewdoc/summary?doi=10.1.1.115.291
   *
   * Return a convex or tangential vertex if one exists.
   */

#ifdef USE_CLIP_EVEN
  pi_ear = pi_ear_init;
#else
  pi_ear = pf->indices;
#endif

  i = coords_num;
  while (i--) {
    if (pi_ear->sign != CONCAVE) {
      return pi_ear;
    }
    pi_ear = pi_ear->next;
  }

  /* If all vertices are concave, just return the last one. */
  return pi_ear;
}

static bool pf_ear_tip_check(PolyFill *pf, PolyIndex *pi_ear_tip)
{
#ifndef USE_KDTREE
  /* localize */
  const float(*coords)[2] = pf->coords;
  PolyIndex *pi_curr;

  const float *v1, *v2, *v3;
#endif

#if defined(USE_CONVEX_SKIP) && !defined(USE_KDTREE)
  uint coords_num_concave_checked = 0;
#endif

#ifdef USE_CONVEX_SKIP

#  ifdef USE_CONVEX_SKIP_TEST
  /* check if counting is wrong */
  {
    uint coords_num_concave_test = 0;
    PolyIndex *pi_iter = pi_ear_tip;
    do {
      if (pi_iter->sign != CONVEX) {
        coords_num_concave_test += 1;
      }
    } while ((pi_iter = pi_iter->next) != pi_ear_tip);
    BLI_assert(coords_num_concave_test == pf->coords_num_concave);
  }
#  endif

  /* fast-path for circles */
  if (pf->coords_num_concave == 0) {
    return true;
  }
#endif

  if (UNLIKELY(pi_ear_tip->sign == CONCAVE)) {
    return false;
  }

#ifdef USE_KDTREE
  {
    const uint ind[3] = {pi_ear_tip->index, pi_ear_tip->next->index, pi_ear_tip->prev->index};

    if (kdtree2d_isect_tri(&pf->kdtree, ind)) {
      return false;
    }
  }
#else

  v1 = coords[pi_ear_tip->prev->index];
  v2 = coords[pi_ear_tip->index];
  v3 = coords[pi_ear_tip->next->index];

  /* Check if any point is inside the triangle formed by previous, current and next vertices.
   * Only consider vertices that are not part of this triangle,
   * or else we'll always find one inside. */

  for (pi_curr = pi_ear_tip->next->next; pi_curr != pi_ear_tip->prev; pi_curr = pi_curr->next) {
    /* Concave vertices can obviously be inside the candidate ear,
     * but so can tangential vertices if they coincide with one of the triangle's vertices. */
    if (pi_curr->sign != CONVEX) {
      const float *v = coords[pi_curr->index];
      /* Because the polygon has clockwise winding order,
       * the area sign will be positive if the point is strictly inside.
       * It will be 0 on the edge, which we want to include as well. */

      /* NOTE: check (v3, v1) first since it fails _far_ more often than the other 2 checks
       * (those fail equally).
       * It's logical - the chance is low that points exist on the
       * same side as the ear we're clipping off. */
      if ((span_tri_v2_sign(v3, v1, v) != CONCAVE) && (span_tri_v2_sign(v1, v2, v) != CONCAVE) &&
          (span_tri_v2_sign(v2, v3, v) != CONCAVE)) {
        return false;
      }

#  ifdef USE_CONVEX_SKIP
      coords_num_concave_checked += 1;
      if (coords_num_concave_checked == pf->coords_num_concave) {
        break;
      }
#  endif
    }
  }
#endif /* USE_KDTREE */

  return true;
}

static void pf_ear_tip_cut(PolyFill *pf, PolyIndex *pi_ear_tip)
{
  uint *tri = pf_tri_add(pf);

  tri[0] = pi_ear_tip->prev->index;
  tri[1] = pi_ear_tip->index;
  tri[2] = pi_ear_tip->next->index;

  pf_coord_remove(pf, pi_ear_tip);
}

/**
 * Initializes the #PolyFill structure before tessellating with #polyfill_calc.
 */
static void polyfill_prepare(PolyFill *pf,
                             const float (*coords)[2],
                             const uint coords_num,
                             int coords_sign,
                             uint (*r_tris)[3],
                             PolyIndex *r_indices)
{
  /* localize */
  PolyIndex *indices = r_indices;

  uint i;

  /* assign all polyfill members here */
  pf->indices = r_indices;
  pf->coords = coords;
  pf->coords_num = coords_num;
#ifdef USE_CONVEX_SKIP
  pf->coords_num_concave = 0;
#endif
  pf->tris = r_tris;
  pf->tris_num = 0;

  if (coords_sign == 0) {
    coords_sign = (cross_poly_v2(coords, coords_num) >= 0.0f) ? 1 : -1;
  }
  else {
    /* check we're passing in correct args */
#ifdef USE_STRICT_ASSERT
#  ifndef NDEBUG
    if (coords_sign == 1) {
      BLI_assert(cross_poly_v2(coords, coords_num) >= 0.0f);
    }
    else {
      BLI_assert(cross_poly_v2(coords, coords_num) <= 0.0f);
    }
#  endif
#endif
  }

  if (coords_sign == 1) {
    for (i = 0; i < coords_num; i++) {
      indices[i].next = &indices[i + 1];
      indices[i].prev = &indices[i - 1];
      indices[i].index = i;
    }
  }
  else {
    /* reversed */
    uint n = coords_num - 1;
    for (i = 0; i < coords_num; i++) {
      indices[i].next = &indices[i + 1];
      indices[i].prev = &indices[i - 1];
      indices[i].index = (n - i);
    }
  }
  indices[0].prev = &indices[coords_num - 1];
  indices[coords_num - 1].next = &indices[0];

  for (i = 0; i < coords_num; i++) {
    PolyIndex *pi = &indices[i];
    pf_coord_sign_calc(pf, pi);
#ifdef USE_CONVEX_SKIP
    if (pi->sign != CONVEX) {
      pf->coords_num_concave += 1;
    }
#endif
  }
}

static void polyfill_calc(PolyFill *pf)
{
#ifdef USE_KDTREE
#  ifdef USE_CONVEX_SKIP
  if (pf->coords_num_concave)
#  endif
  {
    kdtree2d_new(&pf->kdtree, pf->coords_num_concave, pf->coords);
    kdtree2d_init(&pf->kdtree, pf->coords_num, pf->indices);
    kdtree2d_balance(&pf->kdtree);
    kdtree2d_init_mapping(&pf->kdtree);
  }
#endif

  pf_triangulate(pf);
}

void BLI_polyfill_calc_arena(const float (*coords)[2],
                             const uint coords_num,
                             const int coords_sign,
                             uint (*r_tris)[3],

                             struct MemArena *arena)
{
  PolyFill pf;
  PolyIndex *indices = BLI_memarena_alloc(arena, sizeof(*indices) * coords_num);

#ifdef DEBUG_TIME
  TIMEIT_START(polyfill2d);
#endif

  polyfill_prepare(&pf,
                   coords,
                   coords_num,
                   coords_sign,
                   r_tris,
                   /* cache */
                   indices);

#ifdef USE_KDTREE
  if (pf.coords_num_concave) {
    pf.kdtree.nodes = BLI_memarena_alloc(arena, sizeof(*pf.kdtree.nodes) * pf.coords_num_concave);
    pf.kdtree.nodes_map = memset(
        BLI_memarena_alloc(arena, sizeof(*pf.kdtree.nodes_map) * coords_num),
        0xff,
        sizeof(*pf.kdtree.nodes_map) * coords_num);
  }
  else {
    pf.kdtree.node_num = 0;
  }
#endif

  polyfill_calc(&pf);

  /* indices are no longer needed,
   * caller can clear arena */

#ifdef DEBUG_TIME
  TIMEIT_END(polyfill2d);
#endif
}

void BLI_polyfill_calc(const float (*coords)[2],
                       const uint coords_num,
                       const int coords_sign,
                       uint (*r_tris)[3])
{
  /* Fallback to heap memory for large allocations.
   * Avoid running out of stack memory on systems with 512kb stack (macOS).
   * This happens at around 13,000 points, use a much lower value to be safe. */
  if (UNLIKELY(coords_num > 8192)) {
    /* The buffer size only accounts for the index allocation,
     * worst case we do two allocations when concave, while we should try to be efficient,
     * any caller that relies on this frequently should use #BLI_polyfill_calc_arena directly. */
    MemArena *arena = BLI_memarena_new(sizeof(PolyIndex) * coords_num, __func__);
    BLI_polyfill_calc_arena(coords, coords_num, coords_sign, r_tris, arena);
    BLI_memarena_free(arena);
    return;
  }

  PolyFill pf;
  PolyIndex *indices = BLI_array_alloca(indices, coords_num);

#ifdef DEBUG_TIME
  TIMEIT_START(polyfill2d);
#endif

  polyfill_prepare(&pf,
                   coords,
                   coords_num,
                   coords_sign,
                   r_tris,
                   /* cache */
                   indices);

#ifdef USE_KDTREE
  if (pf.coords_num_concave) {
    pf.kdtree.nodes = BLI_array_alloca(pf.kdtree.nodes, pf.coords_num_concave);
    pf.kdtree.nodes_map = memset(BLI_array_alloca(pf.kdtree.nodes_map, coords_num),
                                 0xff,
                                 sizeof(*pf.kdtree.nodes_map) * coords_num);
  }
  else {
    pf.kdtree.node_num = 0;
  }
#endif

  polyfill_calc(&pf);

#ifdef DEBUG_TIME
  TIMEIT_END(polyfill2d);
#endif
}
