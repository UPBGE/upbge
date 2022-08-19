/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2013 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup intern_mem
 */

#ifndef __MALLOCN_INTERN_H__
#define __MALLOCN_INTERN_H__

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_##x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_##x
#endif

#undef HAVE_MALLOC_STATS
#define USE_MALLOC_USABLE_SIZE /* internal, when we have malloc_usable_size() */

#if defined(HAVE_MALLOC_STATS_H)
#  include <malloc.h>
#  define HAVE_MALLOC_STATS
#elif defined(__FreeBSD__)
#  include <malloc_np.h>
#elif defined(__APPLE__)
#  include <malloc/malloc.h>
#  define malloc_usable_size malloc_size
#elif defined(WIN32)
#  include <malloc.h>
#  define malloc_usable_size _msize
#elif defined(__HAIKU__)
#  include <malloc.h>
size_t malloc_usable_size(void *ptr);
#else
#  pragma message "We don't know how to use malloc_usable_size on your platform"
#  undef USE_MALLOC_USABLE_SIZE
#endif

#define SIZET_FORMAT "%zu"
#define SIZET_ARG(a) ((size_t)(a))

#define SIZET_ALIGN_4(len) ((len + 3) & ~(size_t)3)

#ifdef __GNUC__
#  define LIKELY(x) __builtin_expect(!!(x), 1)
#  define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define LIKELY(x) (x)
#  define UNLIKELY(x) (x)
#endif

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__NetBSD__)
// Needed for memalign on Linux and _aligned_alloc on Windows.

#  include <malloc.h>
#else
// Apple's malloc is 16-byte aligned, and does not have malloc.h, so include
// stdilb instead.
#  include <stdlib.h>
#endif

/* visual studio 2012 does not define inline for C */
#ifdef _MSC_VER
#  define MEM_INLINE static __inline
#else
#  define MEM_INLINE static inline
#endif

#define IS_POW2(a) (((a) & ((a)-1)) == 0)

/* Extra padding which needs to be applied on MemHead to make it aligned. */
#define MEMHEAD_ALIGN_PADDING(alignment) \
  ((size_t)alignment - (sizeof(MemHeadAligned) % (size_t)alignment))

/* Real pointer returned by the malloc or aligned_alloc. */
#define MEMHEAD_REAL_PTR(memh) ((char *)memh - MEMHEAD_ALIGN_PADDING(memh->alignment))

#include "mallocn_inline.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALIGNED_MALLOC_MINIMUM_ALIGNMENT sizeof(void *)

void *aligned_malloc(size_t size, size_t alignment);
void aligned_free(void *ptr);

extern bool leak_detector_has_run;
extern char free_after_leak_detection_message[];

/* Prototypes for counted allocator functions */
size_t MEM_lockfree_allocN_len(const void *vmemh) ATTR_WARN_UNUSED_RESULT;
void MEM_lockfree_freeN(void *vmemh);
void *MEM_lockfree_dupallocN(const void *vmemh) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT;
void *MEM_lockfree_reallocN_id(void *vmemh,
                               size_t len,
                               const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(2);
void *MEM_lockfree_recallocN_id(void *vmemh,
                                size_t len,
                                const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(2);
void *MEM_lockfree_callocN(size_t len, const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(1) ATTR_NONNULL(2);
void *MEM_lockfree_calloc_arrayN(size_t len,
                                 size_t size,
                                 const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(1, 2) ATTR_NONNULL(3);
void *MEM_lockfree_mallocN(size_t len, const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(1) ATTR_NONNULL(2);
void *MEM_lockfree_malloc_arrayN(size_t len,
                                 size_t size,
                                 const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(1, 2) ATTR_NONNULL(3);
void *MEM_lockfree_mallocN_aligned(size_t len,
                                   size_t alignment,
                                   const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(1) ATTR_NONNULL(3);
void MEM_lockfree_printmemlist_pydict(void);
void MEM_lockfree_printmemlist(void);
void MEM_lockfree_callbackmemlist(void (*func)(void *));
void MEM_lockfree_printmemlist_stats(void);
void MEM_lockfree_set_error_callback(void (*func)(const char *));
bool MEM_lockfree_consistency_check(void);
void MEM_lockfree_set_memory_debug(void);
size_t MEM_lockfree_get_memory_in_use(void);
unsigned int MEM_lockfree_get_memory_blocks_in_use(void);
void MEM_lockfree_reset_peak_memory(void);
size_t MEM_lockfree_get_peak_memory(void) ATTR_WARN_UNUSED_RESULT;
#ifndef NDEBUG
const char *MEM_lockfree_name_ptr(void *vmemh);
void MEM_lockfree_name_ptr_set(void *vmemh, const char *str);
#endif

/* Prototypes for fully guarded allocator functions */
size_t MEM_guarded_allocN_len(const void *vmemh) ATTR_WARN_UNUSED_RESULT;
void MEM_guarded_freeN(void *vmemh);
void *MEM_guarded_dupallocN(const void *vmemh) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT;
void *MEM_guarded_reallocN_id(void *vmemh,
                              size_t len,
                              const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(2);
void *MEM_guarded_recallocN_id(void *vmemh,
                               size_t len,
                               const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(2);
void *MEM_guarded_callocN(size_t len, const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(1) ATTR_NONNULL(2);
void *MEM_guarded_calloc_arrayN(size_t len,
                                size_t size,
                                const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(1, 2) ATTR_NONNULL(3);
void *MEM_guarded_mallocN(size_t len, const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(1) ATTR_NONNULL(2);
void *MEM_guarded_malloc_arrayN(size_t len,
                                size_t size,
                                const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(1, 2) ATTR_NONNULL(3);
void *MEM_guarded_mallocN_aligned(size_t len,
                                  size_t alignment,
                                  const char *str) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT
    ATTR_ALLOC_SIZE(1) ATTR_NONNULL(3);
void MEM_guarded_printmemlist_pydict(void);
void MEM_guarded_printmemlist(void);
void MEM_guarded_callbackmemlist(void (*func)(void *));
void MEM_guarded_printmemlist_stats(void);
void MEM_guarded_set_error_callback(void (*func)(const char *));
bool MEM_guarded_consistency_check(void);
void MEM_guarded_set_memory_debug(void);
size_t MEM_guarded_get_memory_in_use(void);
unsigned int MEM_guarded_get_memory_blocks_in_use(void);
void MEM_guarded_reset_peak_memory(void);
size_t MEM_guarded_get_peak_memory(void) ATTR_WARN_UNUSED_RESULT;
#ifndef NDEBUG
const char *MEM_guarded_name_ptr(void *vmemh);
void MEM_guarded_name_ptr_set(void *vmemh, const char *str);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __MALLOCN_INTERN_H__ */
