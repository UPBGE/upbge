/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_mem
 *
 * Guarded memory allocation, and boundary-write detection.
 */

#include <stdarg.h>
#include <stddef.h> /* offsetof */
#include <stdio.h>  /* printf */
#include <stdlib.h>
#include <string.h> /* memcpy */
#include <sys/types.h>

#include <pthread.h>

#include "MEM_guardedalloc.h"

/* Quiet warnings when dealing with allocated data written into the blend file.
 * This also rounds up and causes warnings which we don't consider bugs in practice. */
#ifdef WITH_MEM_VALGRIND
#  include "valgrind/memcheck.h"
#endif

/* to ensure strict conversions */
#include "../../source/blender/blenlib/BLI_strict_flags.h"

#include "atomic_ops.h"
#include "mallocn_intern.hh"
#include "mallocn_intern_function_pointers.hh"

using namespace mem_guarded::internal;

/* Only for debugging:
 * store original buffer's name when doing MEM_dupallocN
 * helpful to profile issues with non-freed "dup_alloc" buffers,
 * but this introduces some overhead to memory header and makes
 * things slower a bit, so better to keep disabled by default
 */
// #define DEBUG_MEMDUPLINAME

/* Only for debugging:
 * lets you count the allocations so as to find the allocator of unfreed memory
 * in situations where the leak is predictable */

// #define DEBUG_MEMCOUNTER

/* Only for debugging:
 * Defining DEBUG_BACKTRACE will display a back-trace from where memory block was allocated and
 * print this trace for all unfreed blocks. This will only work for ASAN enabled builds. This
 * option will be on by default for MSVC as it currently does not have LSAN which would normally
 * report these leaks, off by default on all other platforms because it would report the leaks
 * twice, once here, and once by LSAN.
 */
#if defined(_MSC_VER)
#  ifdef WITH_ASAN
#    define DEBUG_BACKTRACE
#  endif
#else
/* Un-comment to report back-traces with leaks, uses ASAN when enabled.
 * NOTE: The default linking options cause the stack traces only to include addresses.
 * Use `addr2line` to expand into file, line & function identifiers,
 * see: `tools/utils/addr2line_backtrace.py` convenience utility. */
// #  define DEBUG_BACKTRACE
#endif

#ifdef DEBUG_BACKTRACE
#  ifdef WITH_ASAN
/* Rely on address sanitizer. */
#  else
#    if defined(__linux__) || defined(__APPLE__)
#      define DEBUG_BACKTRACE_EXECINFO
#    else
#      error "DEBUG_BACKTRACE: not supported for this platform!"
#    endif
#  endif
#endif

#ifdef DEBUG_BACKTRACE_EXECINFO
#  define BACKTRACE_SIZE 100
#endif

#ifdef DEBUG_MEMCOUNTER
/* set this to the value that isn't being freed */
#  define DEBUG_MEMCOUNTER_ERROR_VAL 0
static int _mallocn_count = 0;

/* Break-point here. */
static void memcount_raise(const char *name)
{
  fprintf(stderr, "%s: memcount-leak, %d\n", name, _mallocn_count);
}
#endif

/* --------------------------------------------------------------------- */
/* Data definition                                                       */
/* --------------------------------------------------------------------- */
/* all memory chunks are put in linked lists */
typedef struct localLink {
  localLink *next, *prev;
} localLink;

typedef struct localListBase {
  void *first, *last;
} localListBase;

namespace {

/* NOTE(@hos): keep this struct aligned (e.g., IRIX/GCC). */
typedef struct MemHead {
  int tag1;
  size_t len;
  MemHead *next, *prev;
  const char *name;
  const char *nextname;
  int tag2;
  uint16_t flag;
  /* if non-zero aligned allocation was used and alignment is stored here. */
  short alignment;
#ifdef DEBUG_MEMCOUNTER
  int _count;
#endif

#ifdef DEBUG_MEMDUPLINAME
  int need_free_name, pad;
#endif

#ifdef DEBUG_BACKTRACE_EXECINFO
  void *backtrace[BACKTRACE_SIZE];
  int backtrace_size;
#endif

} MemHead;
static_assert(MEM_MIN_CPP_ALIGNMENT <= alignof(MemHead), "Bad alignment of MemHead");
static_assert(MEM_MIN_CPP_ALIGNMENT <= sizeof(MemHead), "Bad size of MemHead");

typedef MemHead MemHeadAligned;

}  // namespace

/* #MemHead::flag. */
enum MemHeadFlag {
  /**
   * This block of memory has been allocated from CPP `new` (e.g. #MEM_new, or some
   * guardedalloc-overloaded `new` operator). It mainly checks that #MEM_freeN is not directly
   * called on it (#MEM_delete or some guardedalloc-overloaded `delete` operator should always be
   * used instead).
   */
  MEMHEAD_FLAG_FROM_CPP_NEW = 1 << 1,
};

typedef struct MemTail {
  int tag3, pad;
} MemTail;

#ifdef DEBUG_BACKTRACE_EXECINFO
#  include <execinfo.h>
#endif

/* --------------------------------------------------------------------- */
/* local functions                                                       */
/* --------------------------------------------------------------------- */

static void addtail(volatile localListBase *listbase, void *vlink);
static void remlink(volatile localListBase *listbase, void *vlink);
static void rem_memblock(MemHead *memh);
static void MemorY_ErroR(const char *block, const char *error);
static const char *check_memlist(const MemHead *memh);

/* --------------------------------------------------------------------- */
/* locally used defines                                                  */
/* --------------------------------------------------------------------- */

/* NOTE: this is endianness-sensitive. */
#define MAKE_ID(a, b, c, d) (int(d) << 24 | int(c) << 16 | (b) << 8 | (a))

#define MEMTAG1 MAKE_ID('M', 'E', 'M', 'O')
#define MEMTAG2 MAKE_ID('R', 'Y', 'B', 'L')
#define MEMTAG3 MAKE_ID('O', 'C', 'K', '!')
#define MEMFREE MAKE_ID('F', 'R', 'E', 'E')

#define MEMNEXT(x) ((MemHead *)(((char *)x) - offsetof(MemHead, next)))

/* --------------------------------------------------------------------- */
/* vars                                                                  */
/* --------------------------------------------------------------------- */

static uint totblock = 0;
static size_t mem_in_use = 0, peak_mem = 0;

static volatile localListBase _membase;
static volatile localListBase *membase = &_membase;
static void (*error_callback)(const char *) = nullptr;

static bool malloc_debug_memset = false;

#ifdef malloc
#  undef malloc
#endif

#ifdef calloc
#  undef calloc
#endif

#ifdef free
#  undef free
#endif

/* --------------------------------------------------------------------- */
/* implementation                                                        */
/* --------------------------------------------------------------------- */

#ifdef __GNUC__
__attribute__((format(printf, 1, 0)))
#endif
static void
print_error(const char *message, va_list str_format_args)
{
  char buf[512];
  vsnprintf(buf, sizeof(buf), message, str_format_args);
  buf[sizeof(buf) - 1] = '\0';

  if (error_callback) {
    error_callback(buf);
  }
  else {
    fputs(buf, stderr);
  }
}

#ifdef __GNUC__
__attribute__((format(printf, 1, 2)))
#endif
static void
print_error(const char *message, ...)
{
  va_list str_format_args;
  va_start(str_format_args, message);
  print_error(message, str_format_args);
  va_end(str_format_args);
}

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static void
report_error_on_address(const void *vmemh, const char *message, ...)
{
  va_list str_format_args;

  va_start(str_format_args, message);
  print_error(message, str_format_args);
  va_end(str_format_args);

  if (vmemh == nullptr) {
    MEM_trigger_error_on_memory_block(nullptr, 0);
    return;
  }

  const MemHead *memh = static_cast<const MemHead *>(vmemh);
  memh--;
  size_t len = memh->len;

  const void *address = memh;
  size_t size = len + sizeof(*memh) + sizeof(MemTail);
  if (UNLIKELY(memh->alignment > 0)) {
    const MemHeadAligned *memh_aligned = memh;
    address = MEMHEAD_REAL_PTR(memh_aligned);
    size = len + sizeof(*memh_aligned) + MEMHEAD_ALIGN_PADDING(memh_aligned->alignment) +
           sizeof(MemTail);
  }
  MEM_trigger_error_on_memory_block(address, size);
}

static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;

static void mem_lock_thread()
{
  pthread_mutex_lock(&thread_lock);
}

static void mem_unlock_thread()
{
  pthread_mutex_unlock(&thread_lock);
}

bool MEM_guarded_consistency_check()
{
  const char *err_val = nullptr;
  const MemHead *listend;
  /* check_memlist starts from the front, and runs until it finds
   * the requested chunk. For this test, that's the last one. */
  listend = static_cast<MemHead *>(membase->last);

  err_val = check_memlist(listend);

  return (err_val == nullptr);
}

void MEM_guarded_set_error_callback(void (*func)(const char *))
{
  error_callback = func;
}

void MEM_guarded_set_memory_debug()
{
  malloc_debug_memset = true;
}

size_t MEM_guarded_allocN_len(const void *vmemh)
{
  if (vmemh) {
    const MemHead *memh = static_cast<const MemHead *>(vmemh);

    memh--;
    return memh->len;
  }

  return 0;
}

void *MEM_guarded_dupallocN(const void *vmemh)
{
  void *newp = nullptr;

  if (vmemh) {
    const MemHead *memh = static_cast<const MemHead *>(vmemh);
    memh--;

    if ((memh->flag & MEMHEAD_FLAG_FROM_CPP_NEW) != 0) {
      report_error_on_address(vmemh,
                              "Attempt to use C-style MEM_dupallocN on a pointer created with "
                              "CPP-style MEM_new or new\n");
    }

#ifndef DEBUG_MEMDUPLINAME
    if (LIKELY(memh->alignment == 0)) {
      newp = MEM_guarded_mallocN(memh->len, "dupli_alloc");
    }
    else {
      newp = MEM_guarded_mallocN_aligned(
          memh->len, size_t(memh->alignment), "dupli_alloc", AllocationType::ALLOC_FREE);
    }

    if (newp == nullptr) {
      return nullptr;
    }
#else
    {
      MemHead *nmemh;
      const char name_prefix[] = "dupli_alloc ";
      const size_t name_prefix_len = sizeof(name_prefix) - 1;
      const size_t name_size = strlen(memh->name) + 1;
      char *name = malloc(name_prefix_len + name_size);
      memcpy(name, name_prefix, sizeof(name_prefix));
      memcpy(name + name_prefix_len, memh->name, name_size);

      if (LIKELY(memh->alignment == 0)) {
        newp = MEM_guarded_mallocN(memh->len, name);
      }
      else {
        newp = MEM_guarded_mallocN_aligned(
            memh->len, size_t(memh->alignment), name, AllocationType::ALLOC_FREE);
      }

      if (newp == nullptr)
        return nullptr;

      nmemh = newp;
      nmemh--;

      nmemh->need_free_name = 1;
    }
#endif

    memcpy(newp, vmemh, memh->len);
  }

  return newp;
}

void *MEM_guarded_reallocN_id(void *vmemh, size_t len, const char *str)
{
  void *newp = nullptr;

  if (vmemh) {
    MemHead *memh = static_cast<MemHead *>(vmemh);
    memh--;

    if ((memh->flag & MEMHEAD_FLAG_FROM_CPP_NEW) != 0) {
      report_error_on_address(vmemh,
                              "Attempt to use C-style MEM_reallocN on a pointer created with "
                              "CPP-style MEM_new or new\n");
    }

    if (LIKELY(memh->alignment == 0)) {
      newp = MEM_guarded_mallocN(len, memh->name);
    }
    else {
      newp = MEM_guarded_mallocN_aligned(
          len, size_t(memh->alignment), memh->name, AllocationType::ALLOC_FREE);
    }

    if (newp) {
      if (len < memh->len) {
        /* shrink */
        memcpy(newp, vmemh, len);
      }
      else {
        /* grow (or remain same size) */
        memcpy(newp, vmemh, memh->len);
      }
    }

    MEM_guarded_freeN(vmemh, AllocationType::ALLOC_FREE);
  }
  else {
    newp = MEM_guarded_mallocN(len, str);
  }

  return newp;
}

void *MEM_guarded_recallocN_id(void *vmemh, size_t len, const char *str)
{
  void *newp = nullptr;

  if (vmemh) {
    MemHead *memh = static_cast<MemHead *>(vmemh);
    memh--;

    if ((memh->flag & MEMHEAD_FLAG_FROM_CPP_NEW) != 0) {
      report_error_on_address(vmemh,
                              "Attempt to use C-style MEM_recallocN on a pointer created with "
                              "CPP-style MEM_new or new\n");
    }

    if (LIKELY(memh->alignment == 0)) {
      newp = MEM_guarded_mallocN(len, memh->name);
    }
    else {
      newp = MEM_guarded_mallocN_aligned(
          len, size_t(memh->alignment), memh->name, AllocationType::ALLOC_FREE);
    }

    if (newp) {
      if (len < memh->len) {
        /* shrink */
        memcpy(newp, vmemh, len);
      }
      else {
        memcpy(newp, vmemh, memh->len);

        if (len > memh->len) {
          /* grow */
          /* zero new bytes */
          memset(((char *)newp) + memh->len, 0, len - memh->len);
        }
      }
    }

    MEM_guarded_freeN(vmemh, AllocationType::ALLOC_FREE);
  }
  else {
    newp = MEM_guarded_callocN(len, str);
  }

  return newp;
}

#ifdef DEBUG_BACKTRACE_EXECINFO
static void make_memhead_backtrace(MemHead *memh)
{
  memh->backtrace_size = backtrace(memh->backtrace, BACKTRACE_SIZE);
}

static void print_memhead_backtrace(MemHead *memh)
{
  char **strings;
  int i;

  strings = backtrace_symbols(memh->backtrace, memh->backtrace_size);
  for (i = 0; i < memh->backtrace_size; i++) {
    print_error("  %s\n", strings[i]);
  }

  free(strings);
}
#endif /* DEBUG_BACKTRACE_EXECINFO */

static void make_memhead_header(MemHead *memh,
                                size_t len,
                                const char *str,
                                const AllocationType allocation_type)
{
  MemTail *memt;

  memh->tag1 = MEMTAG1;
  memh->name = str;
  memh->nextname = nullptr;
  memh->len = len;
  memh->flag = (allocation_type == AllocationType::NEW_DELETE ? MEMHEAD_FLAG_FROM_CPP_NEW : 0);
  memh->alignment = 0;
  memh->tag2 = MEMTAG2;

#ifdef DEBUG_MEMDUPLINAME
  memh->need_free_name = 0;
#endif

#ifdef DEBUG_BACKTRACE_EXECINFO
  make_memhead_backtrace(memh);
#endif

  memt = (MemTail *)(((char *)memh) + sizeof(MemHead) + len);
  memt->tag3 = MEMTAG3;

  atomic_add_and_fetch_u(&totblock, 1);
  atomic_add_and_fetch_z(&mem_in_use, len);

  mem_lock_thread();
  addtail(membase, &memh->next);
  if (memh->next) {
    memh->nextname = MEMNEXT(memh->next)->name;
  }
  peak_mem = mem_in_use > peak_mem ? mem_in_use : peak_mem;
  mem_unlock_thread();
}

void *MEM_guarded_mallocN(size_t len, const char *str)
{
  MemHead *memh;

#ifdef WITH_MEM_VALGRIND
  const size_t len_unaligned = len;
#endif
  len = SIZET_ALIGN_4(len);

  memh = (MemHead *)malloc(len + sizeof(MemHead) + sizeof(MemTail));

  if (LIKELY(memh)) {
    make_memhead_header(memh, len, str, AllocationType::ALLOC_FREE);

    if (LIKELY(len)) {
      if (UNLIKELY(malloc_debug_memset)) {
        memset(memh + 1, 255, len);
      }
#ifdef WITH_MEM_VALGRIND
      if (malloc_debug_memset) {
        VALGRIND_MAKE_MEM_UNDEFINED(memh + 1, len_unaligned);
      }
      else {
        VALGRIND_MAKE_MEM_DEFINED((const char *)(memh + 1) + len_unaligned, len - len_unaligned);
      }
#endif /* WITH_MEM_VALGRIND */
    }

#ifdef DEBUG_MEMCOUNTER
    if (_mallocn_count == DEBUG_MEMCOUNTER_ERROR_VAL) {
      memcount_raise(__func__);
    }
    memh->_count = _mallocn_count++;
#endif
    return (++memh);
  }
  print_error("Malloc returns null: len=" SIZET_FORMAT " in %s, total " SIZET_FORMAT "\n",
              SIZET_ARG(len),
              str,
              mem_in_use);
  return nullptr;
}

void *MEM_guarded_malloc_arrayN(size_t len, size_t size, const char *str)
{
  size_t total_size;
  if (UNLIKELY(!MEM_size_safe_multiply(len, size, &total_size))) {
    print_error(
        "Malloc array aborted due to integer overflow: "
        "len=" SIZET_FORMAT "x" SIZET_FORMAT " in %s, total " SIZET_FORMAT "\n",
        SIZET_ARG(len),
        SIZET_ARG(size),
        str,
        mem_in_use);
    abort();
    return nullptr;
  }

  return MEM_guarded_mallocN(total_size, str);
}

void *MEM_guarded_mallocN_aligned(size_t len,
                                  size_t alignment,
                                  const char *str,
                                  const AllocationType allocation_type)
{
  /* Huge alignment values doesn't make sense and they wouldn't fit into 'short' used in the
   * MemHead. */
  assert(alignment < 1024);

  /* We only support alignments that are a power of two. */
  assert(IS_POW2(alignment));

  /* Some OS specific aligned allocators require a certain minimal alignment. */
  /* And #MEM_guarded_freeN also checks that it is freeing a pointer aligned with `sizeof(void *)`.
   */
  if (alignment < ALIGNED_MALLOC_MINIMUM_ALIGNMENT) {
    alignment = ALIGNED_MALLOC_MINIMUM_ALIGNMENT;
  }

  /* It's possible that MemHead's size is not properly aligned,
   * do extra padding to deal with this.
   *
   * We only support small alignments which fits into short in
   * order to save some bits in MemHead structure.
   */
  size_t extra_padding = MEMHEAD_ALIGN_PADDING(alignment);

#ifdef WITH_MEM_VALGRIND
  const size_t len_unaligned = len;
#endif
  len = SIZET_ALIGN_4(len);

  MemHead *memh = (MemHead *)aligned_malloc(
      len + extra_padding + sizeof(MemHead) + sizeof(MemTail), alignment);

  if (LIKELY(memh)) {
    /* We keep padding in the beginning of MemHead,
     * this way it's always possible to get MemHead
     * from the data pointer.
     */
    memh = (MemHead *)((char *)memh + extra_padding);

    make_memhead_header(memh, len, str, allocation_type);
    memh->alignment = short(alignment);
    if (LIKELY(len)) {
      if (UNLIKELY(malloc_debug_memset)) {
        memset(memh + 1, 255, len);
      }
#ifdef WITH_MEM_VALGRIND
      if (malloc_debug_memset) {
        VALGRIND_MAKE_MEM_UNDEFINED(memh + 1, len_unaligned);
      }
      else {
        VALGRIND_MAKE_MEM_DEFINED((const char *)(memh + 1) + len_unaligned, len - len_unaligned);
      }
#endif /* WITH_MEM_VALGRIND */
    }

#ifdef DEBUG_MEMCOUNTER
    if (_mallocn_count == DEBUG_MEMCOUNTER_ERROR_VAL) {
      memcount_raise(__func__);
    }
    memh->_count = _mallocn_count++;
#endif
    return (++memh);
  }
  print_error("aligned_malloc returns null: len=" SIZET_FORMAT " in %s, total " SIZET_FORMAT "\n",
              SIZET_ARG(len),
              str,
              mem_in_use);
  return nullptr;
}

void *MEM_guarded_callocN(size_t len, const char *str)
{
  MemHead *memh;

  len = SIZET_ALIGN_4(len);

  memh = (MemHead *)calloc(len + sizeof(MemHead) + sizeof(MemTail), 1);

  if (memh) {
    make_memhead_header(memh, len, str, AllocationType::ALLOC_FREE);
#ifdef DEBUG_MEMCOUNTER
    if (_mallocn_count == DEBUG_MEMCOUNTER_ERROR_VAL) {
      memcount_raise(__func__);
    }
    memh->_count = _mallocn_count++;
#endif
    return (++memh);
  }
  print_error("Calloc returns null: len=" SIZET_FORMAT " in %s, total " SIZET_FORMAT "\n",
              SIZET_ARG(len),
              str,
              mem_in_use);
  return nullptr;
}

void *MEM_guarded_calloc_arrayN(size_t len, size_t size, const char *str)
{
  size_t total_size;
  if (UNLIKELY(!MEM_size_safe_multiply(len, size, &total_size))) {
    print_error(
        "Calloc array aborted due to integer overflow: "
        "len=" SIZET_FORMAT "x" SIZET_FORMAT " in %s, total " SIZET_FORMAT "\n",
        SIZET_ARG(len),
        SIZET_ARG(size),
        str,
        mem_in_use);
    abort();
    return nullptr;
  }

  return MEM_guarded_callocN(total_size, str);
}

static void *mem_guarded_malloc_arrayN_aligned(const size_t len,
                                               const size_t size,
                                               const size_t alignment,
                                               const char *str,
                                               size_t &r_bytes_num)
{
  if (UNLIKELY(!MEM_size_safe_multiply(len, size, &r_bytes_num))) {
    print_error(
        "Calloc array aborted due to integer overflow: "
        "len=" SIZET_FORMAT "x" SIZET_FORMAT " in %s, total " SIZET_FORMAT "\n",
        SIZET_ARG(len),
        SIZET_ARG(size),
        str,
        mem_in_use);
    abort();
    return nullptr;
  }
  if (alignment <= MEM_MIN_CPP_ALIGNMENT) {
    return mem_mallocN(r_bytes_num, str);
  }
  return MEM_mallocN_aligned(r_bytes_num, alignment, str);
}

void *MEM_guarded_malloc_arrayN_aligned(const size_t len,
                                        const size_t size,
                                        const size_t alignment,
                                        const char *str)
{
  size_t bytes_num;
  return mem_guarded_malloc_arrayN_aligned(len, size, alignment, str, bytes_num);
}

void *MEM_guarded_calloc_arrayN_aligned(const size_t len,
                                        const size_t size,
                                        const size_t alignment,
                                        const char *str)
{
  size_t bytes_num;
  /* There is no lower level #calloc with an alignment parameter, so we have to fall back to using
   * #memset unfortunately. */
  void *ptr = mem_guarded_malloc_arrayN_aligned(len, size, alignment, str, bytes_num);
  if (!ptr) {
    return nullptr;
  }
  memset(ptr, 0, bytes_num);
  return ptr;
}

/* Memory statistics print */
typedef struct MemPrintBlock {
  const char *name;
  uintptr_t len;
  int items;
} MemPrintBlock;

static int compare_name(const void *p1, const void *p2)
{
  const MemPrintBlock *pb1 = (const MemPrintBlock *)p1;
  const MemPrintBlock *pb2 = (const MemPrintBlock *)p2;

  return strcmp(pb1->name, pb2->name);
}

static int compare_len(const void *p1, const void *p2)
{
  const MemPrintBlock *pb1 = (const MemPrintBlock *)p1;
  const MemPrintBlock *pb2 = (const MemPrintBlock *)p2;

  if (pb1->len < pb2->len) {
    return 1;
  }
  if (pb1->len == pb2->len) {
    return 0;
  }

  return -1;
}

void MEM_guarded_printmemlist_stats()
{
  MemHead *membl;
  MemPrintBlock *pb, *printblock;
  uint totpb, a, b;
  size_t mem_in_use_slop = 0;

  mem_lock_thread();

  if (totblock != 0) {
    /* put memory blocks into array */
    printblock = static_cast<MemPrintBlock *>(malloc(sizeof(MemPrintBlock) * totblock));

    if (UNLIKELY(!printblock)) {
      mem_unlock_thread();
      print_error("malloc returned null while generating stats");
      return;
    }
  }
  else {
    printblock = nullptr;
  }

  pb = printblock;
  totpb = 0;

  membl = static_cast<MemHead *>(membase->first);
  if (membl) {
    membl = MEMNEXT(membl);
  }

  while (membl && pb) {
    pb->name = membl->name;
    pb->len = membl->len;
    pb->items = 1;

    totpb++;
    pb++;

#ifdef USE_MALLOC_USABLE_SIZE
    if (membl->alignment == 0) {
      mem_in_use_slop += (sizeof(MemHead) + sizeof(MemTail) + malloc_usable_size((void *)membl)) -
                         membl->len;
    }
#endif

    if (membl->next) {
      membl = MEMNEXT(membl->next);
    }
    else {
      break;
    }
  }

  /* sort by name and add together blocks with the same name */
  if (totpb > 1) {
    qsort(printblock, totpb, sizeof(MemPrintBlock), compare_name);
  }

  for (a = 0, b = 0; a < totpb; a++) {
    if (a == b) {
      continue;
    }
    if (strcmp(printblock[a].name, printblock[b].name) == 0) {
      printblock[b].len += printblock[a].len;
      printblock[b].items++;
    }
    else {
      b++;
      memcpy(&printblock[b], &printblock[a], sizeof(MemPrintBlock));
    }
  }
  totpb = b + 1;

  /* sort by length and print */
  if (totpb > 1) {
    qsort(printblock, totpb, sizeof(MemPrintBlock), compare_len);
  }

  printf("\ntotal memory len: %.3f MB\n", double(mem_in_use) / double(1024 * 1024));
  printf("peak memory len: %.3f MB\n", double(peak_mem) / double(1024 * 1024));
  printf("slop memory len: %.3f MB\n", double(mem_in_use_slop) / double(1024 * 1024));
  printf(" ITEMS TOTAL-MiB AVERAGE-KiB TYPE\n");
  for (a = 0, pb = printblock; a < totpb; a++, pb++) {
    printf("%6d (%8.3f  %8.3f) %s\n",
           pb->items,
           double(pb->len) / double(1024 * 1024),
           double(pb->len) / 1024.0 / double(pb->items),
           pb->name);
  }

  if (printblock != nullptr) {
    free(printblock);
  }

  mem_unlock_thread();

#ifdef HAVE_MALLOC_STATS
  printf("System Statistics:\n");
  malloc_stats();
#endif
}

static const char mem_printmemlist_pydict_script[] =
    "mb_userinfo = {}\n"
    "totmem = 0\n"
    "for mb_item in membase:\n"
    "    mb_item_user_size = mb_userinfo.setdefault(mb_item['name'], [0,0])\n"
    "    mb_item_user_size[0] += 1 # Add a user\n"
    "    mb_item_user_size[1] += mb_item['len'] # Increment the size\n"
    "    totmem += mb_item['len']\n"
    "print('(membase) items:', len(membase), '| unique-names:',\n"
    "      len(mb_userinfo), '| total-mem:', totmem)\n"
    "mb_userinfo_sort = list(mb_userinfo.items())\n"
    "for sort_name, sort_func in (('size', lambda a: -a[1][1]),\n"
    "                             ('users', lambda a: -a[1][0]),\n"
    "                             ('name', lambda a: a[0])):\n"
    "    print('\\nSorting by:', sort_name)\n"
    "    mb_userinfo_sort.sort(key = sort_func)\n"
    "    for item in mb_userinfo_sort:\n"
    "        print('name:%%s, users:%%i, len:%%i' %%\n"
    "              (item[0], item[1][0], item[1][1]))\n";

/* Prints in python syntax for easy */
static void MEM_guarded_printmemlist_internal(int pydict)
{
  MemHead *membl;

  mem_lock_thread();

  membl = static_cast<MemHead *>(membase->first);
  if (membl) {
    membl = MEMNEXT(membl);
  }

  if (pydict) {
    print_error("# membase_debug.py\n");
    print_error("membase = [\n");
  }
  while (membl) {
    if (pydict) {
      print_error("    {'len':" SIZET_FORMAT
                  ", "
                  "'name':'''%s''', "
                  "'pointer':'%p'},\n",
                  SIZET_ARG(membl->len),
                  membl->name,
                  (void *)(membl + 1));
    }
    else {
#ifdef DEBUG_MEMCOUNTER
      print_error("%s len: " SIZET_FORMAT " %p, count: %d\n",
                  membl->name,
                  SIZET_ARG(membl->len),
                  membl + 1,
                  membl->_count);
#else
      print_error("%s len: " SIZET_FORMAT " %p\n",
                  membl->name,
                  SIZET_ARG(membl->len),
                  (void *)(membl + 1));
#endif

#ifdef DEBUG_BACKTRACE_EXECINFO
      print_memhead_backtrace(membl);
#elif defined(DEBUG_BACKTRACE) && defined(WITH_ASAN)
      __asan_describe_address(membl);
#endif
    }
    if (membl->next) {
      membl = MEMNEXT(membl->next);
    }
    else {
      break;
    }
  }
  if (pydict) {
    print_error("]\n\n");
    print_error(mem_printmemlist_pydict_script);
  }

  mem_unlock_thread();
}

void MEM_guarded_callbackmemlist(void (*func)(void *))
{
  MemHead *membl;

  mem_lock_thread();

  membl = static_cast<MemHead *>(membase->first);
  if (membl) {
    membl = MEMNEXT(membl);
  }

  while (membl) {
    func(membl + 1);
    if (membl->next) {
      membl = MEMNEXT(membl->next);
    }
    else {
      break;
    }
  }

  mem_unlock_thread();
}

#if 0
short MEM_guarded_testN(void *vmemh)
{
  MemHead *membl;

  mem_lock_thread();

  membl = membase->first;
  if (membl) {
    membl = MEMNEXT(membl);
  }

  while (membl) {
    if (vmemh == membl + 1) {
      mem_unlock_thread();
      return 1;
    }

    if (membl->next)
      membl = MEMNEXT(membl->next);
    else
      break;
  }

  mem_unlock_thread();

  print_error("Memoryblock %p: pointer not in memlist\n", vmemh);
  return 0;
}
#endif

void MEM_guarded_printmemlist()
{
  MEM_guarded_printmemlist_internal(0);
}
void MEM_guarded_printmemlist_pydict()
{
  MEM_guarded_printmemlist_internal(1);
}
void mem_guarded_clearmemlist()
{
  membase->first = membase->last = nullptr;
}

void MEM_guarded_freeN(void *vmemh, const AllocationType allocation_type)
{
  MemTail *memt;
  MemHead *memh = static_cast<MemHead *>(vmemh);
  const char *name;

  if (memh == nullptr) {
    MemorY_ErroR("free", "attempt to free nullptr pointer");
    // print_error(err_stream, "%d\n", (memh+4000)->tag1);
    return;
  }

  if (sizeof(intptr_t) == 8) {
    if (intptr_t(memh) & 0x7) {
      MemorY_ErroR("free", "attempt to free illegal pointer");
      return;
    }
  }
  else {
    if (intptr_t(memh) & 0x3) {
      MemorY_ErroR("free", "attempt to free illegal pointer");
      return;
    }
  }

  memh--;

  if (allocation_type != AllocationType::NEW_DELETE &&
      (memh->flag & MEMHEAD_FLAG_FROM_CPP_NEW) != 0)
  {
    report_error_on_address(
        vmemh,
        "Attempt to use C-style MEM_freeN on a pointer created with CPP-style MEM_new or new\n");
  }

  if (memh->tag1 == MEMFREE && memh->tag2 == MEMFREE) {
    MemorY_ErroR(memh->name, "double free");
    return;
  }

  if ((memh->tag1 == MEMTAG1) && (memh->tag2 == MEMTAG2) && ((memh->len & 0x3) == 0)) {
    memt = (MemTail *)(((char *)memh) + sizeof(MemHead) + memh->len);
    if (memt->tag3 == MEMTAG3) {

      if (leak_detector_has_run) {
        MemorY_ErroR(memh->name, free_after_leak_detection_message);
      }

      memh->tag1 = MEMFREE;
      memh->tag2 = MEMFREE;
      memt->tag3 = MEMFREE;
      /* after tags !!! */
      rem_memblock(memh);

      return;
    }
    MemorY_ErroR(memh->name, "end corrupt");
    name = check_memlist(memh);
    if (name != nullptr) {
      if (name != memh->name) {
        MemorY_ErroR(name, "is also corrupt");
      }
    }
  }
  else {
    mem_lock_thread();
    name = check_memlist(memh);
    mem_unlock_thread();
    if (name == nullptr) {
      MemorY_ErroR("free", "pointer not in memlist");
    }
    else {
      MemorY_ErroR(name, "error in header");
    }
  }

  totblock--;
  /* here a DUMP should happen */
}

/* --------------------------------------------------------------------- */
/* local functions                                                       */
/* --------------------------------------------------------------------- */

static void addtail(volatile localListBase *listbase, void *vlink)
{
  localLink *link = static_cast<localLink *>(vlink);

  /* for a generic API error checks here is fine but
   * the limited use here they will never be nullptr */
#if 0
  if (link == nullptr) {
    return;
  }
  if (listbase == nullptr) {
    return;
  }
#endif

  link->next = nullptr;
  link->prev = static_cast<localLink *>(listbase->last);

  if (listbase->last) {
    ((localLink *)listbase->last)->next = link;
  }
  if (listbase->first == nullptr) {
    listbase->first = link;
  }
  listbase->last = link;
}

static void remlink(volatile localListBase *listbase, void *vlink)
{
  localLink *link = static_cast<localLink *>(vlink);

  /* for a generic API error checks here is fine but
   * the limited use here they will never be nullptr */
#if 0
  if (link == nullptr) {
    return;
  }
  if (listbase == nullptr) {
    return;
  }
#endif

  if (link->next) {
    link->next->prev = link->prev;
  }
  if (link->prev) {
    link->prev->next = link->next;
  }

  if (listbase->last == link) {
    listbase->last = link->prev;
  }
  if (listbase->first == link) {
    listbase->first = link->next;
  }
}

static void rem_memblock(MemHead *memh)
{
  mem_lock_thread();
  remlink(membase, &memh->next);
  if (memh->prev) {
    if (memh->next) {
      MEMNEXT(memh->prev)->nextname = MEMNEXT(memh->next)->name;
    }
    else {
      MEMNEXT(memh->prev)->nextname = nullptr;
    }
  }
  mem_unlock_thread();

  atomic_sub_and_fetch_u(&totblock, 1);
  atomic_sub_and_fetch_z(&mem_in_use, memh->len);

#ifdef DEBUG_MEMDUPLINAME
  if (memh->need_free_name) {
    free((char *)memh->name);
  }
#endif

  if (UNLIKELY(malloc_debug_memset && memh->len)) {
    memset(memh + 1, 255, memh->len);
  }
  if (LIKELY(memh->alignment == 0)) {
    free(memh);
  }
  else {
    aligned_free(MEMHEAD_REAL_PTR(memh));
  }
}

static void MemorY_ErroR(const char *block, const char *error)
{
  print_error("Memoryblock %s: %s\n", block, error);

#ifdef WITH_ASSERT_ABORT
  abort();
#endif
}

static const char *check_memlist(const MemHead *memh)
{
  MemHead *forw, *back, *forwok, *backok;
  const char *name;

  forw = static_cast<MemHead *>(membase->first);
  if (forw) {
    forw = MEMNEXT(forw);
  }
  forwok = nullptr;
  while (forw) {
    if (forw->tag1 != MEMTAG1 || forw->tag2 != MEMTAG2) {
      break;
    }
    forwok = forw;
    if (forw->next) {
      forw = MEMNEXT(forw->next);
    }
    else {
      forw = nullptr;
    }
  }

  back = (MemHead *)membase->last;
  if (back) {
    back = MEMNEXT(back);
  }
  backok = nullptr;
  while (back) {
    if (back->tag1 != MEMTAG1 || back->tag2 != MEMTAG2) {
      break;
    }
    backok = back;
    if (back->prev) {
      back = MEMNEXT(back->prev);
    }
    else {
      back = nullptr;
    }
  }

  if (forw != back) {
    return ("MORE THAN 1 MEMORYBLOCK CORRUPT");
  }

  if (forw == nullptr && back == nullptr) {
    /* no wrong headers found then but in search of memblock */

    forw = static_cast<MemHead *>(membase->first);
    if (forw) {
      forw = MEMNEXT(forw);
    }
    forwok = nullptr;
    while (forw) {
      if (forw == memh) {
        break;
      }
      if (forw->tag1 != MEMTAG1 || forw->tag2 != MEMTAG2) {
        break;
      }
      forwok = forw;
      if (forw->next) {
        forw = MEMNEXT(forw->next);
      }
      else {
        forw = nullptr;
      }
    }
    if (forw == nullptr) {
      return nullptr;
    }

    back = (MemHead *)membase->last;
    if (back) {
      back = MEMNEXT(back);
    }
    backok = nullptr;
    while (back) {
      if (back == memh) {
        break;
      }
      if (back->tag1 != MEMTAG1 || back->tag2 != MEMTAG2) {
        break;
      }
      backok = back;
      if (back->prev) {
        back = MEMNEXT(back->prev);
      }
      else {
        back = nullptr;
      }
    }
  }

  if (forwok) {
    name = forwok->nextname;
  }
  else {
    name = "No name found";
  }

  if (forw == memh) {
    /* to be sure but this block is removed from the list */
    if (forwok) {
      if (backok) {
        forwok->next = (MemHead *)&backok->next;
        backok->prev = (MemHead *)&forwok->next;
        forwok->nextname = backok->name;
      }
      else {
        forwok->next = nullptr;
        membase->last = (localLink *)&forwok->next;
      }
    }
    else {
      if (backok) {
        backok->prev = nullptr;
        membase->first = &backok->next;
      }
      else {
        membase->first = membase->last = nullptr;
      }
    }
  }
  else {
    MemorY_ErroR(name, "Additional error in header");
    return ("Additional error in header");
  }

  return name;
}

size_t MEM_guarded_get_peak_memory()
{
  size_t _peak_mem;

  mem_lock_thread();
  _peak_mem = peak_mem;
  mem_unlock_thread();

  return _peak_mem;
}

void MEM_guarded_reset_peak_memory()
{
  mem_lock_thread();
  peak_mem = mem_in_use;
  mem_unlock_thread();
}

size_t MEM_guarded_get_memory_in_use()
{
  size_t _mem_in_use;

  mem_lock_thread();
  _mem_in_use = mem_in_use;
  mem_unlock_thread();

  return _mem_in_use;
}

uint MEM_guarded_get_memory_blocks_in_use()
{
  uint _totblock;

  mem_lock_thread();
  _totblock = totblock;
  mem_unlock_thread();

  return _totblock;
}

#ifndef NDEBUG
const char *MEM_guarded_name_ptr(void *vmemh)
{
  if (vmemh) {
    MemHead *memh = static_cast<MemHead *>(vmemh);
    memh--;
    return memh->name;
  }

  return "MEM_guarded_name_ptr(nullptr)";
}

void MEM_guarded_name_ptr_set(void *vmemh, const char *str)
{
  if (!vmemh) {
    return;
  }

  MemHead *memh = static_cast<MemHead *>(vmemh);
  memh--;
  memh->name = str;
  if (memh->prev) {
    MEMNEXT(memh->prev)->nextname = str;
  }
}
#endif /* !NDEBUG */
