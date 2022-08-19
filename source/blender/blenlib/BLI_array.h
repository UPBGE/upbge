/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. All rights reserved. */

#pragma once

/** \file
 * \ingroup bli
 * \brief A (mainly) macro array library.
 */

/* -------------------------------------------------------------------- */
/** \name Internal defines
 * \{ */

/** This returns the entire size of the array, including any buffering. */
#define _bli_array_totalsize_dynamic(arr) \
  (((arr) == NULL) ? 0 : MEM_allocN_len(arr) / sizeof(*(arr)))

#define _bli_array_totalsize_static(arr) (sizeof(_##arr##_static) / sizeof(*(arr)))

#define _bli_array_totalsize(arr) \
  ((size_t)(((void *)(arr) == (void *)_##arr##_static && (void *)(arr) != NULL) ? \
                _bli_array_totalsize_static(arr) : \
                _bli_array_totalsize_dynamic(arr)))

/** \} */

/**
 * BLI_array.c
 *
 * Doing the reallocation in a macro isn't so simple,
 * so use a function the macros can use.
 *
 * This function is only to be called via macros.
 *
 * \note The caller must adjust \a arr_len
 */
void _bli_array_grow_func(void **arr_p,
                          const void *arr_static,
                          int sizeof_arr_p,
                          int arr_len,
                          int num,
                          const char *alloc_str);

/* -------------------------------------------------------------------- */
/** \name Public defines
 * \{ */

/** use `sizeof(*(arr))` to ensure the array exists and is an array */
#define BLI_array_declare(arr) \
  int _##arr##_len = ((void)(sizeof(*(arr))), 0); \
  void *_##arr##_static = NULL

/**
 * This will use stack space, up to `maxstatic` array elements,
 * before switching to dynamic heap allocation.
 */
#define BLI_array_staticdeclare(arr, maxstatic) \
  int _##arr##_len = 0; \
  char _##arr##_static[maxstatic * sizeof(*(arr))]

/** returns the logical size of the array, not including buffering. */
#define BLI_array_len(arr) ((void)0, _##arr##_len)

/**
 * Grow the array by a fixed number of items.
 *
 * Allow for a large 'num' value when the new size is more than double
 * to allocate the exact sized array.
 */
#define BLI_array_reserve(arr, num) \
  (void)((((void *)(arr) == NULL) && \
          ((void *)(_##arr##_static) != \
           NULL) && /* don't add _##arr##_len below because it must be zero */ \
          (_bli_array_totalsize_static(arr) >= \
           (size_t)(_##arr##_len + \
                    (num)))) ? /* we have an empty array and a static var big enough */ \
             (void)(arr = (void *)_##arr##_static) : /* use existing static array or allocate */ \
             (LIKELY(_bli_array_totalsize(arr) >= (size_t)(_##arr##_len + (num))) ? \
                  (void)0 /* do nothing */ : \
                  _bli_array_grow_func((void **)&(arr), \
                                       _##arr##_static, \
                                       sizeof(*(arr)), \
                                       _##arr##_len, \
                                       num, \
                                       "BLI_array." #arr)))

/**
 * Returns length of array.
 */
#define BLI_array_grow_items(arr, num) (BLI_array_reserve(arr, num), (_##arr##_len += num))

#define BLI_array_grow_one(arr) BLI_array_grow_items(arr, 1)

/**
 * Appends an item to the array.
 */
#define BLI_array_append(arr, item) \
  ((void)BLI_array_grow_one(arr), (void)(arr[_##arr##_len - 1] = item))

/**
 * Appends an item to the array and returns a pointer to the item in the array.
 * item is not a pointer, but actual data value.
 */
#define BLI_array_append_r(arr, item) \
  ((void)BLI_array_grow_one(arr), (void)(arr[_##arr##_len - 1] = item), (&arr[_##arr##_len - 1]))

/**
 * Appends (grows) & returns a pointer to the uninitialized memory.
 */
#define BLI_array_append_ret(arr) (BLI_array_reserve(arr, 1), &arr[(_##arr##_len++)])

#define BLI_array_free(arr) \
  { \
    if (arr && (char *)arr != _##arr##_static) { \
      BLI_array_fake_user(arr); \
      MEM_freeN((void *)arr); \
    } \
  } \
  ((void)0)

#define BLI_array_pop(arr) ((arr && _##arr##_len) ? arr[--_##arr##_len] : NULL)

/**
 * Resets the logical size of an array to zero, but doesn't
 * free the memory.
 */
#define BLI_array_clear(arr) \
  { \
    _##arr##_len = 0; \
  } \
  ((void)0)

/**
 * Set the length of the array, doesn't actually increase the allocated array size.
 * Don't use this unless you know what you're doing.
 */
#define BLI_array_len_set(arr, len) \
  { \
    _##arr##_len = (len); \
  } \
  ((void)0)

/**
 * Only to prevent unused warnings.
 */
#define BLI_array_fake_user(arr) ((void)_##arr##_len, (void)_##arr##_static)

/**
 * Trim excess items from the array (when they exist).
 */
#define BLI_array_trim(arr) \
  { \
    if (_bli_array_totalsize_dynamic(arr) != _##arr##_len) { \
      arr = MEM_reallocN(arr, sizeof(*arr) * _##arr##_len); \
    } \
  } \
  ((void)0)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic Array Utils
 *
 * Other useful defines (unrelated to the main array macros).
 * \{ */

/**
 * Not part of the 'API' but handy functions, same purpose as #BLI_array_staticdeclare()
 * but use when the max size is known ahead of time.
 */
#define BLI_array_fixedstack_declare(arr, maxstatic, realsize, allocstr) \
  char _##arr##_static[maxstatic * sizeof(*(arr))]; \
  const bool _##arr##_is_static = ((void *)_##arr##_static) != \
                                  (arr = ((realsize) <= maxstatic) ? \
                                             (void *)_##arr##_static : \
                                             MEM_mallocN(sizeof(*(arr)) * (realsize), allocstr))

#define BLI_array_fixedstack_free(arr) \
  if (_##arr##_is_static) { \
    MEM_freeN(arr); \
  } \
  ((void)0)

/** \} */
