/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation, Joshua Leung. All rights reserved. */

/** \file
 * \ingroup edanimation
 */

/* System includes ----------------------------------------------------- */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_dlrbTree.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_range.h"
#include "BLI_utildefines.h"

#include "DNA_anim_types.h"
#include "DNA_cachefile_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_mask_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_fcurve.h"

#include "ED_anim_api.h"
#include "ED_keyframes_keylist.h"

extern "C" {
/* *************************** Keyframe Processing *************************** */

/* ActKeyColumns (Keyframe Columns) ------------------------------------------ */

BLI_INLINE bool is_cfra_eq(const float a, const float b)
{
  return IS_EQT(a, b, BEZT_BINARYSEARCH_THRESH);
}

BLI_INLINE bool is_cfra_lt(const float a, const float b)
{
  return (b - a) > BEZT_BINARYSEARCH_THRESH;
}

/* --------------- */

struct AnimKeylist {
  /* Number of ActKeyColumn's in the keylist. */
  size_t column_len = 0;

  bool is_runtime_initialized = false;

  /* Before initializing the runtime, the key_columns list base is used to quickly add columns.
   * Contains `ActKeyColumn`. Should not be used after runtime is initialized. */
  ListBase /* ActKeyColumn */ key_columns;
  /* Last accessed column in the key_columns list base. Inserting columns are typically done in
   * order. The last accessed column is used as starting point to search for a location to add or
   * update the next column. */
  std::optional<ActKeyColumn *> last_accessed_column = std::nullopt;

  struct {
    /* When initializing the runtime the columns from the list base `AnimKeyList.key_columns` are
     * transferred to an array to support binary searching and index based access. */
    blender::Array<ActKeyColumn> key_columns;
    /* Wrapper around runtime.key_columns so it can still be accessed as a ListBase. Elements are
     * owned by runtime.key_columns. */
    ListBase /* ActKeyColumn */ list_wrapper;
  } runtime;

  AnimKeylist()
  {
    BLI_listbase_clear(&this->key_columns);
    BLI_listbase_clear(&this->runtime.list_wrapper);
  }

  ~AnimKeylist()
  {
    BLI_freelistN(&this->key_columns);
    BLI_listbase_clear(&this->runtime.list_wrapper);
  }

#ifdef WITH_CXX_GUARDEDALLOC
  MEM_CXX_CLASS_ALLOC_FUNCS("editors:AnimKeylist")
#endif
};

AnimKeylist *ED_keylist_create()
{
  AnimKeylist *keylist = new AnimKeylist();
  return keylist;
}

void ED_keylist_free(AnimKeylist *keylist)
{
  BLI_assert(keylist);
  delete keylist;
}

static void ED_keylist_convert_key_columns_to_array(AnimKeylist *keylist)
{
  size_t index;
  LISTBASE_FOREACH_INDEX (ActKeyColumn *, key, &keylist->key_columns, index) {
    keylist->runtime.key_columns[index] = *key;
  }
}

static void ED_keylist_runtime_update_key_column_next_prev(AnimKeylist *keylist)
{
  for (size_t index = 0; index < keylist->column_len; index++) {
    const bool is_first = (index == 0);
    keylist->runtime.key_columns[index].prev = is_first ? nullptr :
                                                          &keylist->runtime.key_columns[index - 1];
    const bool is_last = (index == keylist->column_len - 1);
    keylist->runtime.key_columns[index].next = is_last ? nullptr :
                                                         &keylist->runtime.key_columns[index + 1];
  }
}

static void ED_keylist_runtime_init_listbase(AnimKeylist *keylist)
{
  if (ED_keylist_is_empty(keylist)) {
    BLI_listbase_clear(&keylist->runtime.list_wrapper);
    return;
  }

  keylist->runtime.list_wrapper.first = keylist->runtime.key_columns.data();
  keylist->runtime.list_wrapper.last = &keylist->runtime.key_columns[keylist->column_len - 1];
}

static void ED_keylist_runtime_init(AnimKeylist *keylist)
{
  BLI_assert(!keylist->is_runtime_initialized);

  keylist->runtime.key_columns = blender::Array<ActKeyColumn>(keylist->column_len);

  /* Convert linked list to array to support fast searching. */
  ED_keylist_convert_key_columns_to_array(keylist);
  /* Ensure that the array can also be used as a listbase for external usages. */
  ED_keylist_runtime_update_key_column_next_prev(keylist);
  ED_keylist_runtime_init_listbase(keylist);

  keylist->is_runtime_initialized = true;
}

static void ED_keylist_reset_last_accessed(AnimKeylist *keylist)
{
  BLI_assert(!keylist->is_runtime_initialized);
  keylist->last_accessed_column.reset();
}

void ED_keylist_prepare_for_direct_access(AnimKeylist *keylist)
{
  if (keylist->is_runtime_initialized) {
    return;
  }
  ED_keylist_runtime_init(keylist);
}

static const ActKeyColumn *ED_keylist_find_lower_bound(const AnimKeylist *keylist,
                                                       const float cfra)
{
  BLI_assert(!ED_keylist_is_empty(keylist));
  const ActKeyColumn *begin = std::begin(keylist->runtime.key_columns);
  const ActKeyColumn *end = std::end(keylist->runtime.key_columns);
  ActKeyColumn value;
  value.cfra = cfra;

  const ActKeyColumn *found_column = std::lower_bound(
      begin, end, value, [](const ActKeyColumn &column, const ActKeyColumn &other) {
        return is_cfra_lt(column.cfra, other.cfra);
      });
  return found_column;
}

static const ActKeyColumn *ED_keylist_find_upper_bound(const AnimKeylist *keylist,
                                                       const float cfra)
{
  BLI_assert(!ED_keylist_is_empty(keylist));
  const ActKeyColumn *begin = std::begin(keylist->runtime.key_columns);
  const ActKeyColumn *end = std::end(keylist->runtime.key_columns);
  ActKeyColumn value;
  value.cfra = cfra;

  const ActKeyColumn *found_column = std::upper_bound(
      begin, end, value, [](const ActKeyColumn &column, const ActKeyColumn &other) {
        return is_cfra_lt(column.cfra, other.cfra);
      });
  return found_column;
}

const ActKeyColumn *ED_keylist_find_exact(const AnimKeylist *keylist, const float cfra)
{
  BLI_assert_msg(keylist->is_runtime_initialized,
                 "ED_keylist_prepare_for_direct_access needs to be called before searching.");

  if (ED_keylist_is_empty(keylist)) {
    return nullptr;
  }

  const ActKeyColumn *found_column = ED_keylist_find_lower_bound(keylist, cfra);

  const ActKeyColumn *end = std::end(keylist->runtime.key_columns);
  if (found_column == end) {
    return nullptr;
  }
  if (is_cfra_eq(found_column->cfra, cfra)) {
    return found_column;
  }
  return nullptr;
}

const ActKeyColumn *ED_keylist_find_next(const AnimKeylist *keylist, const float cfra)
{
  BLI_assert_msg(keylist->is_runtime_initialized,
                 "ED_keylist_prepare_for_direct_access needs to be called before searching.");

  if (ED_keylist_is_empty(keylist)) {
    return nullptr;
  }

  const ActKeyColumn *found_column = ED_keylist_find_upper_bound(keylist, cfra);

  const ActKeyColumn *end = std::end(keylist->runtime.key_columns);
  if (found_column == end) {
    return nullptr;
  }
  return found_column;
}

const ActKeyColumn *ED_keylist_find_prev(const AnimKeylist *keylist, const float cfra)
{
  BLI_assert_msg(keylist->is_runtime_initialized,
                 "ED_keylist_prepare_for_direct_access needs to be called before searching.");

  if (ED_keylist_is_empty(keylist)) {
    return nullptr;
  }

  const ActKeyColumn *end = std::end(keylist->runtime.key_columns);
  const ActKeyColumn *found_column = ED_keylist_find_lower_bound(keylist, cfra);

  if (found_column == end) {
    /* Nothing found, return the last item. */
    return end - 1;
  }

  const ActKeyColumn *prev_column = found_column->prev;
  return prev_column;
}

const ActKeyColumn *ED_keylist_find_any_between(const AnimKeylist *keylist,
                                                const Range2f frame_range)
{
  BLI_assert_msg(keylist->is_runtime_initialized,
                 "ED_keylist_prepare_for_direct_access needs to be called before searching.");

  if (ED_keylist_is_empty(keylist)) {
    return nullptr;
  }

  const ActKeyColumn *column = ED_keylist_find_lower_bound(keylist, frame_range.min);
  const ActKeyColumn *end = std::end(keylist->runtime.key_columns);
  if (column == end) {
    return nullptr;
  }
  if (column->cfra >= frame_range.max) {
    return nullptr;
  }
  return column;
}

const ActKeyColumn *ED_keylist_array(const struct AnimKeylist *keylist)
{
  BLI_assert_msg(
      keylist->is_runtime_initialized,
      "ED_keylist_prepare_for_direct_access needs to be called before accessing array.");
  return keylist->runtime.key_columns.data();
}

int64_t ED_keylist_array_len(const struct AnimKeylist *keylist)
{
  return keylist->column_len;
}

bool ED_keylist_is_empty(const struct AnimKeylist *keylist)
{
  return keylist->column_len == 0;
}

const struct ListBase *ED_keylist_listbase(const AnimKeylist *keylist)
{
  if (keylist->is_runtime_initialized) {
    return &keylist->runtime.list_wrapper;
  }
  return &keylist->key_columns;
}

static void keylist_first_last(const struct AnimKeylist *keylist,
                               const struct ActKeyColumn **first_column,
                               const struct ActKeyColumn **last_column)
{
  if (keylist->is_runtime_initialized) {
    *first_column = keylist->runtime.key_columns.data();
    *last_column = &keylist->runtime.key_columns[keylist->column_len - 1];
  }
  else {
    *first_column = static_cast<const ActKeyColumn *>(keylist->key_columns.first);
    *last_column = static_cast<const ActKeyColumn *>(keylist->key_columns.last);
  }
}

bool ED_keylist_all_keys_frame_range(const struct AnimKeylist *keylist, Range2f *r_frame_range)
{
  BLI_assert(r_frame_range);

  if (ED_keylist_is_empty(keylist)) {
    return false;
  }

  const ActKeyColumn *first_column;
  const ActKeyColumn *last_column;
  keylist_first_last(keylist, &first_column, &last_column);
  r_frame_range->min = first_column->cfra;
  r_frame_range->max = last_column->cfra;

  return true;
}

bool ED_keylist_selected_keys_frame_range(const struct AnimKeylist *keylist,
                                          Range2f *r_frame_range)
{
  BLI_assert(r_frame_range);

  if (ED_keylist_is_empty(keylist)) {
    return false;
  }

  const ActKeyColumn *first_column;
  const ActKeyColumn *last_column;
  keylist_first_last(keylist, &first_column, &last_column);
  while (first_column && !(first_column->sel & SELECT)) {
    first_column = first_column->next;
  }
  while (last_column && !(last_column->sel & SELECT)) {
    last_column = last_column->prev;
  }
  if (!first_column || !last_column || first_column == last_column) {
    return false;
  }
  r_frame_range->min = first_column->cfra;
  r_frame_range->max = last_column->cfra;

  return true;
}

/* Set of references to three logically adjacent keys. */
struct BezTripleChain {
  /* Current keyframe. */
  BezTriple *cur;

  /* Logical neighbors. May be nullptr. */
  BezTriple *prev, *next;
};

/* Categorize the interpolation & handle type of the keyframe. */
static eKeyframeHandleDrawOpts bezt_handle_type(const BezTriple *bezt)
{
  if (bezt->h1 == HD_AUTO_ANIM && bezt->h2 == HD_AUTO_ANIM) {
    return KEYFRAME_HANDLE_AUTO_CLAMP;
  }
  if (ELEM(bezt->h1, HD_AUTO_ANIM, HD_AUTO) && ELEM(bezt->h2, HD_AUTO_ANIM, HD_AUTO)) {
    return KEYFRAME_HANDLE_AUTO;
  }
  if (bezt->h1 == HD_VECT && bezt->h2 == HD_VECT) {
    return KEYFRAME_HANDLE_VECTOR;
  }
  if (ELEM(HD_FREE, bezt->h1, bezt->h2)) {
    return KEYFRAME_HANDLE_FREE;
  }
  return KEYFRAME_HANDLE_ALIGNED;
}

/* Determine if the keyframe is an extreme by comparing with neighbors.
 * Ends of fixed-value sections and of the whole curve are also marked.
 */
static eKeyframeExtremeDrawOpts bezt_extreme_type(const BezTripleChain *chain)
{
  if (chain->prev == nullptr && chain->next == nullptr) {
    return KEYFRAME_EXTREME_NONE;
  }

  /* Keyframe values for the current one and neighbors. */
  const float cur_y = chain->cur->vec[1][1];
  float prev_y = cur_y, next_y = cur_y;

  if (chain->prev && !IS_EQF(cur_y, chain->prev->vec[1][1])) {
    prev_y = chain->prev->vec[1][1];
  }
  if (chain->next && !IS_EQF(cur_y, chain->next->vec[1][1])) {
    next_y = chain->next->vec[1][1];
  }

  /* Static hold. */
  if (prev_y == cur_y && next_y == cur_y) {
    return KEYFRAME_EXTREME_FLAT;
  }

  /* Middle of an incline. */
  if ((prev_y < cur_y && next_y > cur_y) || (prev_y > cur_y && next_y < cur_y)) {
    return KEYFRAME_EXTREME_NONE;
  }

  /* Bezier handle values for the overshoot check. */
  const bool l_bezier = chain->prev && chain->prev->ipo == BEZT_IPO_BEZ;
  const bool r_bezier = chain->next && chain->cur->ipo == BEZT_IPO_BEZ;
  const float handle_l = l_bezier ? chain->cur->vec[0][1] : cur_y;
  const float handle_r = r_bezier ? chain->cur->vec[2][1] : cur_y;

  /* Detect extremes. One of the neighbors is allowed to be equal to current. */
  if (prev_y < cur_y || next_y < cur_y) {
    const bool is_overshoot = (handle_l > cur_y || handle_r > cur_y);

    return static_cast<eKeyframeExtremeDrawOpts>(KEYFRAME_EXTREME_MAX |
                                                 (is_overshoot ? KEYFRAME_EXTREME_MIXED : 0));
  }

  if (prev_y > cur_y || next_y > cur_y) {
    const bool is_overshoot = (handle_l < cur_y || handle_r < cur_y);

    return static_cast<eKeyframeExtremeDrawOpts>(KEYFRAME_EXTREME_MIN |
                                                 (is_overshoot ? KEYFRAME_EXTREME_MIXED : 0));
  }

  return KEYFRAME_EXTREME_NONE;
}

/* New node callback used for building ActKeyColumns from BezTripleChain */
static ActKeyColumn *nalloc_ak_bezt(void *data)
{
  ActKeyColumn *ak = static_cast<ActKeyColumn *>(
      MEM_callocN(sizeof(ActKeyColumn), "ActKeyColumn"));
  const BezTripleChain *chain = static_cast<const BezTripleChain *>(data);
  const BezTriple *bezt = chain->cur;

  /* store settings based on state of BezTriple */
  ak->cfra = bezt->vec[1][0];
  ak->sel = BEZT_ISSEL_ANY(bezt) ? SELECT : 0;
  ak->key_type = BEZKEYTYPE(bezt);
  ak->handle_type = bezt_handle_type(bezt);
  ak->extreme_type = bezt_extreme_type(chain);

  /* count keyframes in this column */
  ak->totkey = 1;

  return ak;
}

/* Node updater callback used for building ActKeyColumns from BezTripleChain */
static void nupdate_ak_bezt(ActKeyColumn *ak, void *data)
{
  const BezTripleChain *chain = static_cast<const BezTripleChain *>(data);
  const BezTriple *bezt = chain->cur;

  /* set selection status and 'touched' status */
  if (BEZT_ISSEL_ANY(bezt)) {
    ak->sel = SELECT;
  }

  /* count keyframes in this column */
  ak->totkey++;

  /* For keyframe type, 'proper' keyframes have priority over breakdowns
   * (and other types for now). */
  if (BEZKEYTYPE(bezt) == BEZT_KEYTYPE_KEYFRAME) {
    ak->key_type = BEZT_KEYTYPE_KEYFRAME;
  }

  /* For interpolation type, select the highest value (enum is sorted). */
  ak->handle_type = MAX2((eKeyframeHandleDrawOpts)ak->handle_type, bezt_handle_type(bezt));

  /* For extremes, detect when combining different states. */
  const char new_extreme = bezt_extreme_type(chain);

  if (new_extreme != ak->extreme_type) {
    /* Replace the flat status without adding mixed. */
    if (ak->extreme_type == KEYFRAME_EXTREME_FLAT) {
      ak->extreme_type = new_extreme;
    }
    else if (new_extreme != KEYFRAME_EXTREME_FLAT) {
      ak->extreme_type |= (new_extreme | KEYFRAME_EXTREME_MIXED);
    }
  }
}

/* ......... */

/* New node callback used for building ActKeyColumns from GPencil frames */
static ActKeyColumn *nalloc_ak_gpframe(void *data)
{
  ActKeyColumn *ak = static_cast<ActKeyColumn *>(
      MEM_callocN(sizeof(ActKeyColumn), "ActKeyColumnGPF"));
  const bGPDframe *gpf = (bGPDframe *)data;

  /* store settings based on state of BezTriple */
  ak->cfra = gpf->framenum;
  ak->sel = (gpf->flag & GP_FRAME_SELECT) ? SELECT : 0;
  ak->key_type = gpf->key_type;

  /* count keyframes in this column */
  ak->totkey = 1;
  /* Set as visible block. */
  ak->totblock = 1;
  ak->block.sel = ak->sel;
  ak->block.flag |= ACTKEYBLOCK_FLAG_GPENCIL;

  return ak;
}

/* Node updater callback used for building ActKeyColumns from GPencil frames */
static void nupdate_ak_gpframe(ActKeyColumn *ak, void *data)
{
  bGPDframe *gpf = (bGPDframe *)data;

  /* set selection status and 'touched' status */
  if (gpf->flag & GP_FRAME_SELECT) {
    ak->sel = SELECT;
  }

  /* count keyframes in this column */
  ak->totkey++;

  /* for keyframe type, 'proper' keyframes have priority over breakdowns
   * (and other types for now). */
  if (gpf->key_type == BEZT_KEYTYPE_KEYFRAME) {
    ak->key_type = BEZT_KEYTYPE_KEYFRAME;
  }
}

/* ......... */

/* New node callback used for building ActKeyColumns from GPencil frames */
static ActKeyColumn *nalloc_ak_masklayshape(void *data)
{
  ActKeyColumn *ak = static_cast<ActKeyColumn *>(
      MEM_callocN(sizeof(ActKeyColumn), "ActKeyColumnGPF"));
  const MaskLayerShape *masklay_shape = (const MaskLayerShape *)data;

  /* store settings based on state of BezTriple */
  ak->cfra = masklay_shape->frame;
  ak->sel = (masklay_shape->flag & MASK_SHAPE_SELECT) ? SELECT : 0;

  /* count keyframes in this column */
  ak->totkey = 1;

  return ak;
}

/* Node updater callback used for building ActKeyColumns from GPencil frames */
static void nupdate_ak_masklayshape(ActKeyColumn *ak, void *data)
{
  MaskLayerShape *masklay_shape = (MaskLayerShape *)data;

  /* set selection status and 'touched' status */
  if (masklay_shape->flag & MASK_SHAPE_SELECT) {
    ak->sel = SELECT;
  }

  /* count keyframes in this column */
  ak->totkey++;
}

/* --------------- */
using KeylistCreateColumnFunction = std::function<ActKeyColumn *(void *userdata)>;
using KeylistUpdateColumnFunction = std::function<void(ActKeyColumn *, void *)>;

/* `ED_keylist_find_neighbor_front_to_back` is called before the runtime can be initialized so we
 * cannot use bin searching. */
static ActKeyColumn *ED_keylist_find_neighbor_front_to_back(ActKeyColumn *cursor, float cfra)
{
  while (cursor->next && cursor->next->cfra <= cfra) {
    cursor = cursor->next;
  }
  return cursor;
}

/* `ED_keylist_find_neighbor_back_to_front` is called before the runtime can be initialized so we
 * cannot use bin searching. */
static ActKeyColumn *ED_keylist_find_neighbor_back_to_front(ActKeyColumn *cursor, float cfra)
{
  while (cursor->prev && cursor->prev->cfra >= cfra) {
    cursor = cursor->prev;
  }
  return cursor;
}

/*
 * `ED_keylist_find_exact_or_neighbor_column` is called before the runtime can be initialized so
 * we cannot use bin searching.
 *
 * This function is called to add or update columns in the keylist.
 * Typically columns are sorted by frame number so keeping track of the last_accessed_column
 * reduces searching.
 */
static ActKeyColumn *ED_keylist_find_exact_or_neighbor_column(AnimKeylist *keylist, float cfra)
{
  BLI_assert(!keylist->is_runtime_initialized);
  if (ED_keylist_is_empty(keylist)) {
    return nullptr;
  }

  ActKeyColumn *cursor = keylist->last_accessed_column.value_or(
      static_cast<ActKeyColumn *>(keylist->key_columns.first));
  if (!is_cfra_eq(cursor->cfra, cfra)) {
    const bool walking_direction_front_to_back = cursor->cfra <= cfra;
    if (walking_direction_front_to_back) {
      cursor = ED_keylist_find_neighbor_front_to_back(cursor, cfra);
    }
    else {
      cursor = ED_keylist_find_neighbor_back_to_front(cursor, cfra);
    }
  }

  keylist->last_accessed_column = cursor;
  return cursor;
}

static void ED_keylist_add_or_update_column(AnimKeylist *keylist,
                                            float cfra,
                                            KeylistCreateColumnFunction create_func,
                                            KeylistUpdateColumnFunction update_func,
                                            void *userdata)
{
  BLI_assert_msg(
      !keylist->is_runtime_initialized,
      "Modifying AnimKeylist isn't allowed after runtime is initialized "
      "keylist->key_columns/columns_len will get out of sync with runtime.key_columns.");
  if (ED_keylist_is_empty(keylist)) {
    ActKeyColumn *key_column = create_func(userdata);
    BLI_addhead(&keylist->key_columns, key_column);
    keylist->column_len += 1;
    keylist->last_accessed_column = key_column;
    return;
  }

  ActKeyColumn *nearest = ED_keylist_find_exact_or_neighbor_column(keylist, cfra);
  if (is_cfra_eq(nearest->cfra, cfra)) {
    update_func(nearest, userdata);
  }
  else if (is_cfra_lt(nearest->cfra, cfra)) {
    ActKeyColumn *key_column = create_func(userdata);
    BLI_insertlinkafter(&keylist->key_columns, nearest, key_column);
    keylist->column_len += 1;
    keylist->last_accessed_column = key_column;
  }
  else {
    ActKeyColumn *key_column = create_func(userdata);
    BLI_insertlinkbefore(&keylist->key_columns, nearest, key_column);
    keylist->column_len += 1;
    keylist->last_accessed_column = key_column;
  }
}

/* Add the given BezTriple to the given 'list' of Keyframes */
static void add_bezt_to_keycolumns_list(AnimKeylist *keylist, BezTripleChain *bezt)
{
  if (ELEM(nullptr, keylist, bezt)) {
    return;
  }

  float cfra = bezt->cur->vec[1][0];
  ED_keylist_add_or_update_column(keylist, cfra, nalloc_ak_bezt, nupdate_ak_bezt, bezt);
}

/* Add the given GPencil Frame to the given 'list' of Keyframes */
static void add_gpframe_to_keycolumns_list(AnimKeylist *keylist, bGPDframe *gpf)
{
  if (ELEM(nullptr, keylist, gpf)) {
    return;
  }

  float cfra = gpf->framenum;
  ED_keylist_add_or_update_column(keylist, cfra, nalloc_ak_gpframe, nupdate_ak_gpframe, gpf);
}

/* Add the given MaskLayerShape Frame to the given 'list' of Keyframes */
static void add_masklay_to_keycolumns_list(AnimKeylist *keylist, MaskLayerShape *masklay_shape)
{
  if (ELEM(nullptr, keylist, masklay_shape)) {
    return;
  }

  float cfra = masklay_shape->frame;
  ED_keylist_add_or_update_column(
      keylist, cfra, nalloc_ak_masklayshape, nupdate_ak_masklayshape, masklay_shape);
}

/* ActKeyBlocks (Long Keyframes) ------------------------------------------ */

static const ActKeyBlockInfo dummy_keyblock = {0};

static void compute_keyblock_data(ActKeyBlockInfo *info,
                                  const BezTriple *prev,
                                  const BezTriple *beztn)
{
  memset(info, 0, sizeof(ActKeyBlockInfo));

  if (BEZKEYTYPE(beztn) == BEZT_KEYTYPE_MOVEHOLD) {
    /* Animator tagged a "moving hold"
     *   - Previous key must also be tagged as a moving hold, otherwise
     *     we're just dealing with the first of a pair, and we don't
     *     want to be creating any phantom holds...
     */
    if (BEZKEYTYPE(prev) == BEZT_KEYTYPE_MOVEHOLD) {
      info->flag |= ACTKEYBLOCK_FLAG_MOVING_HOLD | ACTKEYBLOCK_FLAG_ANY_HOLD;
    }
  }

  /* Check for same values...
   *  - Handles must have same central value as each other
   *  - Handles which control that section of the curve must be constant
   */
  if (IS_EQF(beztn->vec[1][1], prev->vec[1][1])) {
    bool hold;

    /* Only check handles in case of actual bezier interpolation. */
    if (prev->ipo == BEZT_IPO_BEZ) {
      hold = IS_EQF(beztn->vec[1][1], beztn->vec[0][1]) &&
             IS_EQF(prev->vec[1][1], prev->vec[2][1]);
    }
    /* This interpolation type induces movement even between identical columns. */
    else {
      hold = !ELEM(prev->ipo, BEZT_IPO_ELASTIC);
    }

    if (hold) {
      info->flag |= ACTKEYBLOCK_FLAG_STATIC_HOLD | ACTKEYBLOCK_FLAG_ANY_HOLD;
    }
  }

  /* Remember non-bezier interpolation info. */
  if (prev->ipo != BEZT_IPO_BEZ) {
    info->flag |= ACTKEYBLOCK_FLAG_NON_BEZIER;
  }

  info->sel = BEZT_ISSEL_ANY(prev) || BEZT_ISSEL_ANY(beztn);
}

static void add_keyblock_info(ActKeyColumn *col, const ActKeyBlockInfo *block)
{
  /* New curve and block. */
  if (col->totcurve <= 1 && col->totblock == 0) {
    memcpy(&col->block, block, sizeof(ActKeyBlockInfo));
  }
  /* Existing curve. */
  else {
    col->block.conflict |= (col->block.flag ^ block->flag);
    col->block.flag |= block->flag;
    col->block.sel |= block->sel;
  }

  if (block->flag) {
    col->totblock++;
  }
}

static void add_bezt_to_keyblocks_list(AnimKeylist *keylist, BezTriple *bezt, const int bezt_len)
{
  ActKeyColumn *col = static_cast<ActKeyColumn *>(keylist->key_columns.first);

  if (bezt && bezt_len >= 2) {
    ActKeyBlockInfo block;

    /* Find the first key column while inserting dummy blocks. */
    for (; col != nullptr && is_cfra_lt(col->cfra, bezt[0].vec[1][0]); col = col->next) {
      add_keyblock_info(col, &dummy_keyblock);
    }

    BLI_assert(col != nullptr);

    /* Insert real blocks. */
    for (int v = 1; col != nullptr && v < bezt_len; v++, bezt++) {
      /* Wrong order of bezier keys: resync position. */
      if (is_cfra_lt(bezt[1].vec[1][0], bezt[0].vec[1][0])) {
        /* Backtrack to find the right location. */
        if (is_cfra_lt(bezt[1].vec[1][0], col->cfra)) {
          ActKeyColumn *newcol = ED_keylist_find_exact_or_neighbor_column(keylist, col->cfra);

          BLI_assert(newcol);
          BLI_assert(newcol->cfra == col->cfra);

          col = newcol;
          /* The previous keyblock is garbage too. */
          if (col->prev != nullptr) {
            add_keyblock_info(col->prev, &dummy_keyblock);
          }
        }

        continue;
      }

      /* In normal situations all keyframes are sorted. However, while keys are transformed, they
       * may change order and then this assertion no longer holds. The effect is that the drawing
       * isn't perfect during the transform; the "constant value" bars aren't updated until the
       * transformation is confirmed. */
      // BLI_assert(is_cfra_eq(col->cfra, bezt[0].vec[1][0]));

      compute_keyblock_data(&block, bezt, bezt + 1);

      for (; col != nullptr && is_cfra_lt(col->cfra, bezt[1].vec[1][0]); col = col->next) {
        add_keyblock_info(col, &block);
      }

      BLI_assert(col != nullptr);
    }
  }

  /* Insert dummy blocks at the end. */
  for (; col != nullptr; col = col->next) {
    add_keyblock_info(col, &dummy_keyblock);
  }
}

/* Walk through columns and propagate blocks and totcurve.
 *
 * This must be called even by animation sources that don't generate
 * keyblocks to keep the data structure consistent after adding columns.
 */
static void update_keyblocks(AnimKeylist *keylist, BezTriple *bezt, const int bezt_len)
{
  /* Find the curve count */
  int max_curve = 0;

  LISTBASE_FOREACH (ActKeyColumn *, col, &keylist->key_columns) {
    max_curve = MAX2(max_curve, col->totcurve);
  }

  /* Propagate blocks to inserted keys */
  ActKeyColumn *prev_ready = nullptr;

  LISTBASE_FOREACH (ActKeyColumn *, col, &keylist->key_columns) {
    /* Pre-existing column. */
    if (col->totcurve > 0) {
      prev_ready = col;
    }
    /* Newly inserted column, so copy block data from previous. */
    else if (prev_ready != nullptr) {
      col->totblock = prev_ready->totblock;
      memcpy(&col->block, &prev_ready->block, sizeof(ActKeyBlockInfo));
    }

    col->totcurve = max_curve + 1;
  }

  /* Add blocks on top */
  add_bezt_to_keyblocks_list(keylist, bezt, bezt_len);
}

/* --------- */

bool actkeyblock_is_valid(const ActKeyColumn *ac)
{
  return ac != nullptr && ac->next != nullptr && ac->totblock > 0;
}

int actkeyblock_get_valid_hold(const ActKeyColumn *ac)
{
  /* check that block is valid */
  if (!actkeyblock_is_valid(ac)) {
    return 0;
  }

  const int hold_mask = (ACTKEYBLOCK_FLAG_ANY_HOLD | ACTKEYBLOCK_FLAG_STATIC_HOLD);
  return (ac->block.flag & ~ac->block.conflict) & hold_mask;
}

/* *************************** Keyframe List Conversions *************************** */

void summary_to_keylist(bAnimContext *ac, AnimKeylist *keylist, const int saction_flag)
{
  if (ac) {
    ListBase anim_data = {nullptr, nullptr};

    /* get F-Curves to take keyframes from */
    const eAnimFilter_Flags filter = ANIMFILTER_DATA_VISIBLE;
    ANIM_animdata_filter(
        ac, &anim_data, filter, ac->data, static_cast<eAnimCont_Types>(ac->datatype));

    /* loop through each F-Curve, grabbing the keyframes */
    LISTBASE_FOREACH (const bAnimListElem *, ale, &anim_data) {
      /* Why not use all #eAnim_KeyType here?
       * All of the other key types are actually "summaries" themselves,
       * and will just end up duplicating stuff that comes up through
       * standard filtering of just F-Curves. Given the way that these work,
       * there isn't really any benefit at all from including them. - Aligorith */
      switch (ale->datatype) {
        case ALE_FCURVE:
          fcurve_to_keylist(ale->adt, static_cast<FCurve *>(ale->data), keylist, saction_flag);
          break;
        case ALE_MASKLAY:
          mask_to_keylist(ac->ads, static_cast<MaskLayer *>(ale->data), keylist);
          break;
        case ALE_GPFRAME:
          gpl_to_keylist(ac->ads, static_cast<bGPDlayer *>(ale->data), keylist);
          break;
        default:
          // printf("%s: datatype %d unhandled\n", __func__, ale->datatype);
          break;
      }
    }

    ANIM_animdata_freelist(&anim_data);
  }
}

void scene_to_keylist(bDopeSheet *ads, Scene *sce, AnimKeylist *keylist, const int saction_flag)
{
  bAnimContext ac = {nullptr};
  ListBase anim_data = {nullptr, nullptr};

  bAnimListElem dummychan = {nullptr};

  if (sce == nullptr) {
    return;
  }

  /* create a dummy wrapper data to work with */
  dummychan.type = ANIMTYPE_SCENE;
  dummychan.data = sce;
  dummychan.id = &sce->id;
  dummychan.adt = sce->adt;

  ac.ads = ads;
  ac.data = &dummychan;
  ac.datatype = ANIMCONT_CHANNEL;

  /* get F-Curves to take keyframes from */
  const eAnimFilter_Flags filter = ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FCURVESONLY;

  ANIM_animdata_filter(
      &ac, &anim_data, filter, ac.data, static_cast<eAnimCont_Types>(ac.datatype));

  /* loop through each F-Curve, grabbing the keyframes */
  LISTBASE_FOREACH (const bAnimListElem *, ale, &anim_data) {
    fcurve_to_keylist(ale->adt, static_cast<FCurve *>(ale->data), keylist, saction_flag);
  }

  ANIM_animdata_freelist(&anim_data);
}

void ob_to_keylist(bDopeSheet *ads, Object *ob, AnimKeylist *keylist, const int saction_flag)
{
  bAnimContext ac = {nullptr};
  ListBase anim_data = {nullptr, nullptr};

  bAnimListElem dummychan = {nullptr};
  Base dummybase = {nullptr};

  if (ob == nullptr) {
    return;
  }

  /* create a dummy wrapper data to work with */
  dummybase.object = ob;

  dummychan.type = ANIMTYPE_OBJECT;
  dummychan.data = &dummybase;
  dummychan.id = &ob->id;
  dummychan.adt = ob->adt;

  ac.ads = ads;
  ac.data = &dummychan;
  ac.datatype = ANIMCONT_CHANNEL;

  /* get F-Curves to take keyframes from */
  const eAnimFilter_Flags filter = ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FCURVESONLY;
  ANIM_animdata_filter(
      &ac, &anim_data, filter, ac.data, static_cast<eAnimCont_Types>(ac.datatype));

  /* loop through each F-Curve, grabbing the keyframes */
  LISTBASE_FOREACH (const bAnimListElem *, ale, &anim_data) {
    fcurve_to_keylist(ale->adt, static_cast<FCurve *>(ale->data), keylist, saction_flag);
  }

  ANIM_animdata_freelist(&anim_data);
}

void cachefile_to_keylist(bDopeSheet *ads,
                          CacheFile *cache_file,
                          AnimKeylist *keylist,
                          const int saction_flag)
{
  if (cache_file == nullptr) {
    return;
  }

  /* create a dummy wrapper data to work with */
  bAnimListElem dummychan = {nullptr};
  dummychan.type = ANIMTYPE_DSCACHEFILE;
  dummychan.data = cache_file;
  dummychan.id = &cache_file->id;
  dummychan.adt = cache_file->adt;

  bAnimContext ac = {nullptr};
  ac.ads = ads;
  ac.data = &dummychan;
  ac.datatype = ANIMCONT_CHANNEL;

  /* get F-Curves to take keyframes from */
  ListBase anim_data = {nullptr, nullptr};
  const eAnimFilter_Flags filter = ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FCURVESONLY;
  ANIM_animdata_filter(
      &ac, &anim_data, filter, ac.data, static_cast<eAnimCont_Types>(ac.datatype));

  /* loop through each F-Curve, grabbing the keyframes */
  LISTBASE_FOREACH (const bAnimListElem *, ale, &anim_data) {
    fcurve_to_keylist(ale->adt, static_cast<FCurve *>(ale->data), keylist, saction_flag);
  }

  ANIM_animdata_freelist(&anim_data);
}

void fcurve_to_keylist(AnimData *adt, FCurve *fcu, AnimKeylist *keylist, const int saction_flag)
{
  if (fcu && fcu->totvert && fcu->bezt) {
    ED_keylist_reset_last_accessed(keylist);
    /* apply NLA-mapping (if applicable) */
    if (adt) {
      ANIM_nla_mapping_apply_fcurve(adt, fcu, false, false);
    }

    /* Check if the curve is cyclic. */
    bool is_cyclic = BKE_fcurve_is_cyclic(fcu) && (fcu->totvert >= 2);
    bool do_extremes = (saction_flag & SACTION_SHOW_EXTREMES) != 0;

    /* loop through beztriples, making ActKeysColumns */
    BezTripleChain chain = {nullptr};

    for (int v = 0; v < fcu->totvert; v++) {
      chain.cur = &fcu->bezt[v];

      /* Neighbor columns, accounting for being cyclic. */
      if (do_extremes) {
        chain.prev = (v > 0)   ? &fcu->bezt[v - 1] :
                     is_cyclic ? &fcu->bezt[fcu->totvert - 2] :
                                 nullptr;
        chain.next = (v + 1 < fcu->totvert) ? &fcu->bezt[v + 1] :
                     is_cyclic              ? &fcu->bezt[1] :
                                              nullptr;
      }

      add_bezt_to_keycolumns_list(keylist, &chain);
    }

    /* Update keyblocks. */
    update_keyblocks(keylist, fcu->bezt, fcu->totvert);

    /* unapply NLA-mapping if applicable */
    if (adt) {
      ANIM_nla_mapping_apply_fcurve(adt, fcu, true, false);
    }
  }
}

void agroup_to_keylist(AnimData *adt,
                       bActionGroup *agrp,
                       AnimKeylist *keylist,
                       const int saction_flag)
{
  if (agrp) {
    /* loop through F-Curves */
    LISTBASE_FOREACH (FCurve *, fcu, &agrp->channels) {
      if (fcu->grp != agrp) {
        break;
      }
      fcurve_to_keylist(adt, fcu, keylist, saction_flag);
    }
  }
}

void action_to_keylist(AnimData *adt, bAction *act, AnimKeylist *keylist, const int saction_flag)
{
  if (act) {
    /* loop through F-Curves */
    LISTBASE_FOREACH (FCurve *, fcu, &act->curves) {
      fcurve_to_keylist(adt, fcu, keylist, saction_flag);
    }
  }
}

void gpencil_to_keylist(bDopeSheet *ads, bGPdata *gpd, AnimKeylist *keylist, const bool active)
{
  if (gpd && keylist) {
    /* for now, just aggregate out all the frames, but only for visible layers */
    LISTBASE_FOREACH_BACKWARD (bGPDlayer *, gpl, &gpd->layers) {
      if ((gpl->flag & GP_LAYER_HIDE) == 0) {
        if ((!active) || ((active) && (gpl->flag & GP_LAYER_SELECT))) {
          gpl_to_keylist(ads, gpl, keylist);
        }
      }
    }
  }
}

void gpl_to_keylist(bDopeSheet *UNUSED(ads), bGPDlayer *gpl, AnimKeylist *keylist)
{
  if (gpl && keylist) {
    ED_keylist_reset_last_accessed(keylist);
    /* Although the frames should already be in an ordered list,
     * they are not suitable for displaying yet. */
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      add_gpframe_to_keycolumns_list(keylist, gpf);
    }

    update_keyblocks(keylist, nullptr, 0);
  }
}

void mask_to_keylist(bDopeSheet *UNUSED(ads), MaskLayer *masklay, AnimKeylist *keylist)
{
  if (masklay && keylist) {
    ED_keylist_reset_last_accessed(keylist);
    LISTBASE_FOREACH (MaskLayerShape *, masklay_shape, &masklay->splines_shapes) {
      add_masklay_to_keycolumns_list(keylist, masklay_shape);
    }

    update_keyblocks(keylist, nullptr, 0);
  }
}
}
