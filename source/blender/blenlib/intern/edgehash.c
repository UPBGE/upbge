/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * An (edge -> pointer) hash table.
 * Using unordered int-pairs as keys.
 *
 * \note The API matches BLI_ghash.c, but the implementation is different.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_edgehash.h"
#include "BLI_strict_flags.h"
#include "BLI_utildefines.h"

typedef struct _EdgeHash_Edge Edge;
typedef struct _EdgeHash_Entry EdgeHashEntry;

typedef struct EdgeHash {
  EdgeHashEntry *entries;
  int32_t *map;
  uint32_t slot_mask;
  uint capacity_exp;
  uint length;
  uint dummy_count;
} EdgeHash;

typedef struct EdgeSet {
  Edge *entries;
  int32_t *map;
  uint32_t slot_mask;
  uint capacity_exp;
  uint length;
} EdgeSet;

/* -------------------------------------------------------------------- */
/** \name Internal Helper Macros & Defines
 * \{ */

#define ENTRIES_CAPACITY(container) (uint)(1 << (container)->capacity_exp)
#define MAP_CAPACITY(container) (uint)(1 << ((container)->capacity_exp + 1))
#define CLEAR_MAP(container) \
  memset((container)->map, 0xFF, sizeof(int32_t) * MAP_CAPACITY(container))
#define UPDATE_SLOT_MASK(container) \
  { \
    (container)->slot_mask = MAP_CAPACITY(container) - 1; \
  } \
  ((void)0)
#define PERTURB_SHIFT 5

#define ITER_SLOTS(CONTAINER, EDGE, SLOT, INDEX) \
  uint32_t hash = calc_edge_hash(EDGE); \
  uint32_t mask = (CONTAINER)->slot_mask; \
  uint32_t perturb = hash; \
  int32_t *map = (CONTAINER)->map; \
  uint32_t SLOT = mask & hash; \
  int INDEX = map[SLOT]; \
  for (;; SLOT = mask & ((5 * SLOT) + 1 + perturb), perturb >>= PERTURB_SHIFT, INDEX = map[SLOT])

#define SLOT_EMPTY -1
#define SLOT_DUMMY -2

#define CAPACITY_EXP_DEFAULT 3

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Edge API
 * \{ */

BLI_INLINE uint32_t calc_edge_hash(Edge edge)
{
  return (edge.v_low << 8) ^ edge.v_high;
}

BLI_INLINE Edge init_edge(uint v0, uint v1)
{
  /* If there are use cases where we need this it could be removed (or flag to allow),
   * for now this helps avoid incorrect usage (creating degenerate geometry). */
  BLI_assert(v0 != v1);
  Edge edge;
  if (v0 < v1) {
    edge.v_low = v0;
    edge.v_high = v1;
  }
  else {
    edge.v_low = v1;
    edge.v_high = v0;
  }
  return edge;
}

BLI_INLINE bool edges_equal(Edge e1, Edge e2)
{
  return memcmp(&e1, &e2, sizeof(Edge)) == 0;
}

static uint calc_capacity_exp_for_reserve(uint reserve)
{
  uint result = 1;
  while (reserve >>= 1) {
    result++;
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Utility API
 * \{ */

#define EH_INDEX_HAS_EDGE(eh, index, edge) \
  ((index) >= 0 && edges_equal((edge), (eh)->entries[index].edge))

static void edgehash_free_values(EdgeHash *eh, EdgeHashFreeFP free_value)
{
  if (free_value) {
    for (uint i = 0; i < eh->length; i++) {
      free_value(eh->entries[i].value);
    }
  }
}

BLI_INLINE void edgehash_insert_index(EdgeHash *eh, Edge edge, uint entry_index)
{
  ITER_SLOTS (eh, edge, slot, index) {
    if (index == SLOT_EMPTY) {
      eh->map[slot] = (int32_t)entry_index;
      break;
    }
  }
}

BLI_INLINE EdgeHashEntry *edgehash_insert_at_slot(EdgeHash *eh, uint slot, Edge edge, void *value)
{
  EdgeHashEntry *entry = &eh->entries[eh->length];
  entry->edge = edge;
  entry->value = value;
  eh->map[slot] = (int32_t)eh->length;
  eh->length++;
  return entry;
}

BLI_INLINE bool edgehash_ensure_can_insert(EdgeHash *eh)
{
  if (UNLIKELY(ENTRIES_CAPACITY(eh) <= eh->length + eh->dummy_count)) {
    eh->capacity_exp++;
    UPDATE_SLOT_MASK(eh);
    eh->dummy_count = 0;
    eh->entries = MEM_reallocN(eh->entries, sizeof(EdgeHashEntry) * ENTRIES_CAPACITY(eh));
    eh->map = MEM_reallocN(eh->map, sizeof(int32_t) * MAP_CAPACITY(eh));
    CLEAR_MAP(eh);
    for (uint i = 0; i < eh->length; i++) {
      edgehash_insert_index(eh, eh->entries[i].edge, i);
    }
    return true;
  }
  return false;
}

BLI_INLINE EdgeHashEntry *edgehash_insert(EdgeHash *eh, Edge edge, void *value)
{
  ITER_SLOTS (eh, edge, slot, index) {
    if (index == SLOT_EMPTY) {
      return edgehash_insert_at_slot(eh, slot, edge, value);
    }
    if (index == SLOT_DUMMY) {
      eh->dummy_count--;
      return edgehash_insert_at_slot(eh, slot, edge, value);
    }
  }
}

BLI_INLINE EdgeHashEntry *edgehash_lookup_entry(const EdgeHash *eh, uint v0, uint v1)
{
  Edge edge = init_edge(v0, v1);

  ITER_SLOTS (eh, edge, slot, index) {
    if (EH_INDEX_HAS_EDGE(eh, index, edge)) {
      return &eh->entries[index];
    }
    if (index == SLOT_EMPTY) {
      return NULL;
    }
  }
}

BLI_INLINE void edgehash_change_index(EdgeHash *eh, Edge edge, int new_index)
{
  ITER_SLOTS (eh, edge, slot, index) {
    if (EH_INDEX_HAS_EDGE(eh, index, edge)) {
      eh->map[slot] = new_index;
      break;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge Hash API
 * \{ */

EdgeHash *BLI_edgehash_new_ex(const char *info, const uint reserve)
{
  EdgeHash *eh = MEM_mallocN(sizeof(EdgeHash), info);
  eh->capacity_exp = calc_capacity_exp_for_reserve(reserve);
  UPDATE_SLOT_MASK(eh);
  eh->length = 0;
  eh->dummy_count = 0;
  eh->entries = MEM_calloc_arrayN(ENTRIES_CAPACITY(eh), sizeof(EdgeHashEntry), "eh entries");
  eh->map = MEM_malloc_arrayN(MAP_CAPACITY(eh), sizeof(int32_t), "eh map");
  CLEAR_MAP(eh);
  return eh;
}

EdgeHash *BLI_edgehash_new(const char *info)
{
  return BLI_edgehash_new_ex(info, 1 << CAPACITY_EXP_DEFAULT);
}

void BLI_edgehash_free(EdgeHash *eh, EdgeHashFreeFP free_value)
{
  edgehash_free_values(eh, free_value);
  MEM_freeN(eh->map);
  MEM_freeN(eh->entries);
  MEM_freeN(eh);
}

void BLI_edgehash_print(EdgeHash *eh)
{
  printf("Edgehash at %p:\n", eh);
  printf("  Map:\n");
  for (uint i = 0; i < MAP_CAPACITY(eh); i++) {
    int index = eh->map[i];
    printf("    %u: %d", i, index);
    if (index >= 0) {
      EdgeHashEntry entry = eh->entries[index];
      printf(" -> (%u, %u) -> %p", entry.edge.v_low, entry.edge.v_high, entry.value);
    }
    printf("\n");
  }
  printf("  Entries:\n");
  for (uint i = 0; i < ENTRIES_CAPACITY(eh); i++) {
    if (i == eh->length) {
      printf("    **** below is rest capacity ****\n");
    }
    EdgeHashEntry entry = eh->entries[i];
    printf("    %u: (%u, %u) -> %p\n", i, entry.edge.v_low, entry.edge.v_high, entry.value);
  }
}

void BLI_edgehash_insert(EdgeHash *eh, uint v0, uint v1, void *value)
{
  edgehash_ensure_can_insert(eh);
  Edge edge = init_edge(v0, v1);
  edgehash_insert(eh, edge, value);
}

bool BLI_edgehash_reinsert(EdgeHash *eh, uint v0, uint v1, void *value)
{
  Edge edge = init_edge(v0, v1);

  ITER_SLOTS (eh, edge, slot, index) {
    if (EH_INDEX_HAS_EDGE(eh, index, edge)) {
      eh->entries[index].value = value;
      return false;
    }
    if (index == SLOT_EMPTY) {
      if (edgehash_ensure_can_insert(eh)) {
        edgehash_insert(eh, edge, value);
      }
      else {
        edgehash_insert_at_slot(eh, slot, edge, value);
      }
      return true;
    }
  }
}

void *BLI_edgehash_lookup_default(const EdgeHash *eh, uint v0, uint v1, void *default_value)
{
  EdgeHashEntry *entry = edgehash_lookup_entry(eh, v0, v1);
  return entry ? entry->value : default_value;
}

void *BLI_edgehash_lookup(const EdgeHash *eh, uint v0, uint v1)
{
  EdgeHashEntry *entry = edgehash_lookup_entry(eh, v0, v1);
  return entry ? entry->value : NULL;
}

void **BLI_edgehash_lookup_p(EdgeHash *eh, uint v0, uint v1)
{
  EdgeHashEntry *entry = edgehash_lookup_entry(eh, v0, v1);
  return entry ? &entry->value : NULL;
}

bool BLI_edgehash_ensure_p(EdgeHash *eh, uint v0, uint v1, void ***r_value)
{
  Edge edge = init_edge(v0, v1);

  ITER_SLOTS (eh, edge, slot, index) {
    if (EH_INDEX_HAS_EDGE(eh, index, edge)) {
      *r_value = &eh->entries[index].value;
      return true;
    }
    if (index == SLOT_EMPTY) {
      if (edgehash_ensure_can_insert(eh)) {
        *r_value = &edgehash_insert(eh, edge, NULL)->value;
      }
      else {
        *r_value = &edgehash_insert_at_slot(eh, slot, edge, NULL)->value;
      }
      return false;
    }
  }
}

bool BLI_edgehash_remove(EdgeHash *eh, uint v0, uint v1, EdgeHashFreeFP free_value)
{
  uint old_length = eh->length;
  void *value = BLI_edgehash_popkey(eh, v0, v1);
  if (free_value && value) {
    free_value(value);
  }
  return old_length > eh->length;
}

void *BLI_edgehash_popkey(EdgeHash *eh, uint v0, uint v1)
{
  /* Same as #BLI_edgehash_remove but return the value,
   * no free value argument since it will be returned */

  Edge edge = init_edge(v0, v1);

  ITER_SLOTS (eh, edge, slot, index) {
    if (EH_INDEX_HAS_EDGE(eh, index, edge)) {
      void *value = eh->entries[index].value;
      eh->length--;
      eh->dummy_count++;
      eh->map[slot] = SLOT_DUMMY;
      eh->entries[index] = eh->entries[eh->length];
      if ((uint)index < eh->length) {
        edgehash_change_index(eh, eh->entries[index].edge, index);
      }
      return value;
    }
    if (index == SLOT_EMPTY) {
      return NULL;
    }
  }
}

bool BLI_edgehash_haskey(const EdgeHash *eh, uint v0, uint v1)
{
  return edgehash_lookup_entry(eh, v0, v1) != NULL;
}

int BLI_edgehash_len(const EdgeHash *eh)
{
  return (int)eh->length;
}

void BLI_edgehash_clear_ex(EdgeHash *eh, EdgeHashFreeFP free_value, const uint UNUSED(reserve))
{
  /* TODO: handle reserve */
  edgehash_free_values(eh, free_value);
  eh->length = 0;
  eh->dummy_count = 0;
  eh->capacity_exp = CAPACITY_EXP_DEFAULT;
  CLEAR_MAP(eh);
}

void BLI_edgehash_clear(EdgeHash *eh, EdgeHashFreeFP free_value)
{
  BLI_edgehash_clear_ex(eh, free_value, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge Hash Iterator API
 * \{ */

EdgeHashIterator *BLI_edgehashIterator_new(EdgeHash *eh)
{
  EdgeHashIterator *ehi = MEM_mallocN(sizeof(EdgeHashIterator), __func__);
  BLI_edgehashIterator_init(ehi, eh);
  return ehi;
}

void BLI_edgehashIterator_init(EdgeHashIterator *ehi, EdgeHash *eh)
{
  ehi->entries = eh->entries;
  ehi->length = eh->length;
  ehi->index = 0;
}

void BLI_edgehashIterator_free(EdgeHashIterator *ehi)
{
  MEM_freeN(ehi);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name EdgeSet API
 *
 * Use edgehash API to give 'set' functionality
 * \{ */

#define ES_INDEX_HAS_EDGE(es, index, edge) \
  (index) >= 0 && edges_equal((edge), (es)->entries[index])

EdgeSet *BLI_edgeset_new_ex(const char *info, const uint reserve)
{
  EdgeSet *es = MEM_mallocN(sizeof(EdgeSet), info);
  es->capacity_exp = calc_capacity_exp_for_reserve(reserve);
  UPDATE_SLOT_MASK(es);
  es->length = 0;
  es->entries = MEM_malloc_arrayN(ENTRIES_CAPACITY(es), sizeof(Edge), "es entries");
  es->map = MEM_malloc_arrayN(MAP_CAPACITY(es), sizeof(int32_t), "es map");
  CLEAR_MAP(es);
  return es;
}

EdgeSet *BLI_edgeset_new(const char *info)
{
  return BLI_edgeset_new_ex(info, 1 << CAPACITY_EXP_DEFAULT);
}

void BLI_edgeset_free(EdgeSet *es)
{
  MEM_freeN(es->entries);
  MEM_freeN(es->map);
  MEM_freeN(es);
}

int BLI_edgeset_len(const EdgeSet *es)
{
  return (int)es->length;
}

static void edgeset_insert_index(EdgeSet *es, Edge edge, uint entry_index)
{
  ITER_SLOTS (es, edge, slot, index) {
    if (index == SLOT_EMPTY) {
      es->map[slot] = (int)entry_index;
      break;
    }
  }
}

BLI_INLINE void edgeset_ensure_can_insert(EdgeSet *es)
{
  if (UNLIKELY(ENTRIES_CAPACITY(es) <= es->length)) {
    es->capacity_exp++;
    UPDATE_SLOT_MASK(es);
    es->entries = MEM_reallocN(es->entries, sizeof(Edge) * ENTRIES_CAPACITY(es));
    es->map = MEM_reallocN(es->map, sizeof(int32_t) * MAP_CAPACITY(es));
    CLEAR_MAP(es);
    for (uint i = 0; i < es->length; i++) {
      edgeset_insert_index(es, es->entries[i], i);
    }
  }
}

BLI_INLINE void edgeset_insert_at_slot(EdgeSet *es, uint slot, Edge edge)
{
  es->entries[es->length] = edge;
  es->map[slot] = (int)es->length;
  es->length++;
}

bool BLI_edgeset_add(EdgeSet *es, uint v0, uint v1)
{
  edgeset_ensure_can_insert(es);
  Edge edge = init_edge(v0, v1);

  ITER_SLOTS (es, edge, slot, index) {
    if (ES_INDEX_HAS_EDGE(es, index, edge)) {
      return false;
    }
    if (index == SLOT_EMPTY) {
      edgeset_insert_at_slot(es, slot, edge);
      return true;
    }
  }
}

void BLI_edgeset_insert(EdgeSet *es, uint v0, uint v1)
{
  edgeset_ensure_can_insert(es);
  Edge edge = init_edge(v0, v1);

  ITER_SLOTS (es, edge, slot, index) {
    if (index == SLOT_EMPTY) {
      edgeset_insert_at_slot(es, slot, edge);
      return;
    }
  }
}

bool BLI_edgeset_haskey(const EdgeSet *es, uint v0, uint v1)
{
  Edge edge = init_edge(v0, v1);

  ITER_SLOTS (es, edge, slot, index) {
    if (ES_INDEX_HAS_EDGE(es, index, edge)) {
      return true;
    }
    if (index == SLOT_EMPTY) {
      return false;
    }
  }
}

EdgeSetIterator *BLI_edgesetIterator_new(EdgeSet *es)
{
  EdgeSetIterator *esi = MEM_mallocN(sizeof(EdgeSetIterator), __func__);
  esi->edges = es->entries;
  esi->length = es->length;
  esi->index = 0;
  return esi;
}

void BLI_edgesetIterator_free(EdgeSetIterator *esi)
{
  MEM_freeN(esi);
}

/** \} */
