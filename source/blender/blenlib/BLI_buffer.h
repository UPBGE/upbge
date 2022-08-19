/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BLI_Buffer {
  void *data;
  const size_t elem_size;
  size_t count, alloc_count;
  int flag;
} BLI_Buffer;

enum {
  BLI_BUFFER_NOP = 0,
  BLI_BUFFER_USE_STATIC = (1 << 0),
};

#define BLI_buffer_declare_static(type_, name_, flag_, static_count_) \
  char name_##user; /* warn for free only */ \
  type_ name_##_static_[static_count_]; \
  BLI_Buffer name_ = { \
      (name_##_static_), sizeof(type_), 0, static_count_, BLI_BUFFER_USE_STATIC | (flag_)}

/* Never use static. */
#define BLI_buffer_declare(type_, name_, flag_) \
  bool name_##user; /* warn for free only */ \
  BLI_Buffer name_ = {NULL, sizeof(type_), 0, 0, (flag_)}

#define BLI_buffer_at(buffer_, type_, index_) \
  ((((type_ *)(buffer_) \
         ->data)[(BLI_assert(sizeof(type_) == (buffer_)->elem_size)), \
                 (BLI_assert((int)(index_) >= 0 && (size_t)(index_) < (buffer_)->count)), \
                 index_]))

#define BLI_buffer_array(buffer_, type_) (&(BLI_buffer_at(buffer_, type_, 0)))

#define BLI_buffer_resize_data(buffer_, type_, new_count_) \
  ((BLI_buffer_resize(buffer_, new_count_), new_count_ ? BLI_buffer_array(buffer_, type_) : NULL))

#define BLI_buffer_reinit_data(buffer_, type_, new_count_) \
  ((BLI_buffer_reinit(buffer_, new_count_), new_count_ ? BLI_buffer_array(buffer_, type_) : NULL))

#define BLI_buffer_append(buffer_, type_, val_) \
  (BLI_buffer_resize(buffer_, (buffer_)->count + 1), \
   (BLI_buffer_at(buffer_, type_, (buffer_)->count - 1) = val_))

#define BLI_buffer_clear(buffer_) \
  { \
    (buffer_)->count = 0; \
  } \
  (void)0

/**
 * \note Never decreases the amount of memory allocated.
 */
void BLI_buffer_resize(BLI_Buffer *buffer, size_t new_count);

/**
 * Ensure size, throwing away old data, respecting #BLI_BUFFER_USE_CALLOC.
 *
 * Similar to #BLI_buffer_resize, but use when the existing data can be:
 * - Ignored (malloc'd).
 * - Cleared (when #BLI_BUFFER_USE_CALLOC is set).
 */
void BLI_buffer_reinit(BLI_Buffer *buffer, size_t new_count);

/**
 * Append an array of elements.
 *
 * Callers use #BLI_buffer_append_array.
 */
void _bli_buffer_append_array(BLI_Buffer *buffer, void *data, size_t count);
#define BLI_buffer_append_array(buffer_, type_, data_, count_) \
  { \
    type_ *__tmp = (data_); \
    BLI_assert(sizeof(type_) == (buffer_)->elem_size); \
    _bli_buffer_append_array(buffer_, __tmp, count_); \
  } \
  (void)0

/**
 * Does not free the buffer structure itself.
 *
 * Callers use #BLI_buffer_free.
 */
void _bli_buffer_free(BLI_Buffer *buffer);
#define BLI_buffer_free(name_) \
  { \
    _bli_buffer_free(name_); \
    (void)name_##user; /* ensure we free */ \
  } \
  (void)0

/**
 * A buffer embedded in a struct. Using #memcpy is allowed until first resize.
 */
#define BLI_buffer_field_init(name_, type_) \
  { \
    memset(name_, 0, sizeof(*name_)); \
    *(size_t *)&((name_)->elem_size) = sizeof(type_); \
  } \
  (void)0

#define BLI_buffer_field_free(name_) _bli_buffer_free(name_)

#ifdef __cplusplus
}
#endif
