/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_listBase.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Point cache file data types:
 * - Used as `(1 << flag)` so poke jahka if you reach the limit of 15.
 * - To add new data types update:
 *   - #BKE_ptcache_data_size()
 *   - #ptcache_file_pointers_init()
 */
#define BPHYS_DATA_INDEX 0
#define BPHYS_DATA_LOCATION 1
#define BPHYS_DATA_SMOKE_LOW 1
#define BPHYS_DATA_VELOCITY 2
#define BPHYS_DATA_SMOKE_HIGH 2
#define BPHYS_DATA_ROTATION 3
#define BPHYS_DATA_DYNAMICPAINT 3
#define BPHYS_DATA_AVELOCITY 4 /* used for particles */
#define BPHYS_DATA_XCONST 4    /* used for cloth */
#define BPHYS_DATA_SIZE 5
#define BPHYS_DATA_TIMES 6
#define BPHYS_DATA_BOIDS 7

#define BPHYS_TOT_DATA 8

#define BPHYS_EXTRA_FLUID_SPRINGS 1
#define BPHYS_EXTRA_CLOTH_ACCELERATION 2

typedef struct PTCacheExtra {
  struct PTCacheExtra *next, *prev;
  unsigned int type, totdata;
  void *data;
} PTCacheExtra;

typedef struct PTCacheMem {
  struct PTCacheMem *next, *prev;
  unsigned int frame, totpoint;
  unsigned int data_types, flag;

  /** BPHYS_TOT_DATA. */
  void *data[8];

  struct ListBase extradata;
} PTCacheMem;

typedef struct PointCache {
  struct PointCache *next, *prev;
  /** Generic flag. */
  int flag;

  /**
   * The number of frames between cached frames.
   * This should probably be an upper bound for a per point adaptive step in the future,
   * but for now it's the same for all points. Without adaptivity this can effect the perceived
   * simulation quite a bit though. If for example particles are colliding with a horizontal
   * plane (with high damping) they quickly come to a stop on the plane, however there are still
   * forces acting on the particle (gravity and collisions), so the particle velocity isn't
   * necessarily zero for the whole duration of the frame even if the particle seems stationary.
   * If all simulation frames aren't cached (step > 1) these velocities are interpolated into
   * movement for the non-cached frames.
   * The result will look like the point is oscillating around the collision location.
   * So for now cache step should be set to 1 for accurate reproduction of collisions.
   */
  int step;

  /** Current frame of simulation (only if SIMULATION_VALID). */
  int simframe;
  /** Simulation start frame. */
  int startframe;
  /** Simulation end frame. */
  int endframe;
  /** Frame being edited (runtime only). */
  int editframe;
  /** Last exact frame that's cached. */
  int last_exact;
  /** Used for editing cache - what is the last baked frame. */
  int last_valid;
  char _pad[4];

  /* for external cache files */
  /** Number of cached points. */
  int totpoint;
  /** Modifier stack index. */
  int index;
  short compression;
  char _pad0[2];

  char name[64];
  char prev_name[64];
  char info[128];
  /** File path, 1024 = FILE_MAX. */
  char path[1024];

  /**
   * Array of length `endframe - startframe + 1` with flags to indicate cached frames.
   * Can be later used for other per frame flags too if needed.
   */
  char *cached_frames;
  int cached_frames_len;
  char _pad1[4];

  struct ListBase mem_cache;

  struct PTCacheEdit *edit;
  /** Free callback. */
  void (*free_edit)(struct PTCacheEdit *edit);
} PointCache;

/** #PointCache.flag */
enum {
  PTCACHE_BAKED = 1 << 0,
  PTCACHE_OUTDATED = 1 << 1,
  PTCACHE_SIMULATION_VALID = 1 << 2,
  PTCACHE_BAKING = 1 << 3,
  //  PTCACHE_BAKE_EDIT = 1 << 4,
  //  PTCACHE_BAKE_EDIT_ACTIVE = 1 << 5,
  PTCACHE_DISK_CACHE = 1 << 6,
  /* removed since 2.64 - T30974, could be added back in a more useful way */
  //  PTCACHE_QUICK_CACHE = 1 << 7,
  PTCACHE_FRAMES_SKIPPED = 1 << 8,
  PTCACHE_EXTERNAL = 1 << 9,
  PTCACHE_READ_INFO = 1 << 10,
  /** Don't use the file-path of the blend-file the data is linked from (write a local cache). */
  PTCACHE_IGNORE_LIBPATH = 1 << 11,
  /**
   * High resolution cache is saved for smoke for backwards compatibility,
   * so set this flag to know it's a "fake" cache.
   */
  PTCACHE_FAKE_SMOKE = 1 << 12,
  PTCACHE_IGNORE_CLEAR = 1 << 13,

  PTCACHE_FLAG_INFO_DIRTY = 1 << 14,

  PTCACHE_REDO_NEEDED = PTCACHE_OUTDATED | PTCACHE_FRAMES_SKIPPED,
  PTCACHE_FLAGS_COPY = PTCACHE_DISK_CACHE | PTCACHE_EXTERNAL | PTCACHE_IGNORE_LIBPATH,
};

#define PTCACHE_COMPRESS_NO 0
#define PTCACHE_COMPRESS_LZO 1
#define PTCACHE_COMPRESS_LZMA 2

#ifdef __cplusplus
}
#endif
