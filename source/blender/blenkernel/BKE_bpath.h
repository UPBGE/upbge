/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * \warning All paths manipulated by this API are assumed to be either constant char buffers of
 * `FILE_MAX` size, or allocated char buffers not bigger than `FILE_MAX`.
 */

/* TODO: Make this module handle a bit more safely string length, instead of assuming buffers are
 * FILE_MAX length etc. */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct ID;
struct ListBase;
struct Main;
struct ReportList;

/** \name Core `foreach_path` API.
 * \{ */

typedef enum eBPathForeachFlag {
  /** Flags controlling the behavior of the generic BPath API. */

  /** Ensures the `absolute_base_path` member of #BPathForeachPathData is initialized properly with
   * the path of the current .blend file. This can be used by the callbacks to convert relative
   * paths to absolute ones. */
  BKE_BPATH_FOREACH_PATH_ABSOLUTE = (1 << 0),
  /** Skip paths of linked IDs. */
  BKE_BPATH_FOREACH_PATH_SKIP_LINKED = (1 << 1),
  /** Skip paths when their matching data is packed. */
  BKE_BPATH_FOREACH_PATH_SKIP_PACKED = (1 << 2),
  /** Resolve tokens within a virtual filepath to a single, concrete, filepath. */
  BKE_BPATH_FOREACH_PATH_RESOLVE_TOKEN = (1 << 3),
  /* Skip weak reference paths. Those paths are typically 'nice to have' extra information, but are
   * not used as actual source of data by the current .blend file.
   *
   * NOTE: Currently this only concerns the weak reference to a library file stored in
   * `ID::library_weak_reference`. */
  BKE_BPATH_TRAVERSE_SKIP_WEAK_REFERENCES = (1 << 5),

  /** Flags not affecting the generic BPath API. Those may be used by specific IDTypeInfo
   * `foreach_path` implementations and/or callbacks to implement specific behaviors. */

  /** Skip paths where a single dir is used with an array of files, eg. sequence strip images or
   * point-caches. In this case only use the first file path is processed.
   *
   * This is needed for directory manipulation callbacks which might otherwise modify the same
   * directory multiple times. */
  BKE_BPATH_FOREACH_PATH_SKIP_MULTIFILE = (1 << 8),
  /** Reload data (when the path is edited).
   *  \note Only used by Image IDType currently. */
  BKE_BPATH_FOREACH_PATH_RELOAD_EDITED = (1 << 9),
} eBPathForeachFlag;

struct BPathForeachPathData;

/** Callback used to iterate over an ID's file paths.
 *
 * \note `path`s parameters should be considered as having a maximal `FILE_MAX` string length.
 *
 * \return `true` if the path has been changed, and in that case, result should be written into
 * `r_path_dst`. */
typedef bool (*BPathForeachPathFunctionCallback)(struct BPathForeachPathData *bpath_data,
                                                 char *r_path_dst,
                                                 const char *path_src);

/** Storage for common data needed across the BPath 'foreach_path' code. */
typedef struct BPathForeachPathData {
  struct Main *bmain;

  BPathForeachPathFunctionCallback callback_function;
  eBPathForeachFlag flag;

  void *user_data;

  /* 'Private' data, caller don't need to set those. */

  /** The root to use as base for relative paths. Only set if `BKE_BPATH_FOREACH_PATH_ABSOLUTE`
   * flag is set, NULL otherwise. */
  const char *absolute_base_path;
} BPathForeachPathData;

/** Run `bpath_data.callback_function` on all paths contained in `id`. */
void BKE_bpath_foreach_path_id(BPathForeachPathData *bpath_data, struct ID *id);

/** Run `bpath_data.callback_function` on all paths of all IDs in `bmain`. */
void BKE_bpath_foreach_path_main(BPathForeachPathData *bpath_data);

/** \} */

/** \name Helpers to handle common cases from `IDTypeInfo`'s `foreach_path` functions.
 * \{ */

/* TODO: Investigate using macros around those calls to check a bit better about actual
 * strings/buffers length (e,g, with static asserts). */

/**
 * Run the callback on a path, replacing the content of the string as needed.
 *
 * \param path: A fixed, FILE_MAX-sized char buffer.
 *
 * \return true is \a path was modified, false otherwise.
 */
bool BKE_bpath_foreach_path_fixed_process(struct BPathForeachPathData *bpath_data, char *path);

/**
 * Run the callback on a (directory + file) path, replacing the content of the two strings as
 * needed.
 *
 * \param path_dir: A fixed, FILE_MAXDIR-sized char buffer.
 * \param path_file: A fixed, FILE_MAXFILE-sized char buffer.
 *
 * \return true is \a path_dir and/or \a path_file were modified, false otherwise.
 */
bool BKE_bpath_foreach_path_dirfile_fixed_process(struct BPathForeachPathData *bpath_data,
                                                  char *path_dir,
                                                  char *path_file);

/**
 * Run the callback on a path, replacing the content of the string as needed.
 *
 * \param path: A pointer to a MEM-allocated string. If modified, it will be freed and replaced by
 * a new allocated string.
 * \note path is expected to be FILE_MAX size or smaller.
 *
 * \return true is \a path was modified and re-allocated, false otherwise.
 */
bool BKE_bpath_foreach_path_allocated_process(struct BPathForeachPathData *bpath_data,
                                              char **path);

/** \} */

/** \name High level features.
 * \{ */

/** Check for missing files. */
void BKE_bpath_missing_files_check(struct Main *bmain, struct ReportList *reports);

/** Recursively search into given search directory, for all file paths of all IDs in given \a
 * bmain, and replace existing paths as needed.
 *
 * \note The search will happen into the whole search directory tree recursively (with a limit of
 * MAX_DIR_RECURSE), if several files are found matching a searched filename, the biggest one will
 * be used. This is so that things like thumbnails don't get selected instead of the actual image
 * e.g.
 *
 * \param searchpath: The root directory in which the new filepaths should be searched for.
 * \param find_all: If `true`, also search for files which current path is still valid, if `false`
 *                  skip those still valid paths.
 * */
void BKE_bpath_missing_files_find(struct Main *bmain,
                                  const char *searchpath,
                                  struct ReportList *reports,
                                  bool find_all);

/** Rebase all relative file paths in given \a bmain from \a basedir_src to \a basedir_dst. */
void BKE_bpath_relative_rebase(struct Main *bmain,
                               const char *basedir_src,
                               const char *basedir_dst,
                               struct ReportList *reports);

/** Make all absolute file paths in given \a bmain relative to given \a basedir. */
void BKE_bpath_relative_convert(struct Main *bmain,
                                const char *basedir,
                                struct ReportList *reports);

/** Make all relative file paths in given \a bmain absolute, using given \a basedir as root. */
void BKE_bpath_absolute_convert(struct Main *bmain,
                                const char *basedir,
                                struct ReportList *reports);

/** Temp backup of paths from all IDs in given \a bmain.
 *
 *  \return An opaque handle to pass to #BKE_bpath_list_restore and #BKE_bpath_list_free.
 */
void *BKE_bpath_list_backup(struct Main *bmain, eBPathForeachFlag flag);

/** Restore the temp backup of paths from \a path_list_handle into all IDs in given \a bmain.
 *
 * \note This function assumes that the data in given Main did not change (no
 * addition/deletion/re-ordering of IDs, or their file paths) since the call to
 * #BKE_bpath_list_backup that generated the given \a path_list_handle. */
void BKE_bpath_list_restore(struct Main *bmain, eBPathForeachFlag flag, void *path_list_handle);

/** Free the temp backup of paths in \a path_list_handle.
 *
 * \note This function assumes that the path list has already been restored with a call to
 * #BKE_bpath_list_restore, and is therefore empty. */
void BKE_bpath_list_free(void *path_list_handle);

/** \} */

#ifdef __cplusplus
}
#endif
