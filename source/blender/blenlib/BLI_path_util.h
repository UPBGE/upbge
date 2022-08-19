/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */
#pragma once

/** \file
 * \ingroup bli
 */

#include "BLI_compiler_attrs.h"
#include "BLI_utildefines.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sets the specified environment variable to the specified value,
 * and clears it if `val == NULL`.
 */
void BLI_setenv(const char *env, const char *val) ATTR_NONNULL(1);
/**
 * Only set an environment variable if already not there.
 * Like Unix `setenv(env, val, 0);`
 *
 * (not used anywhere).
 */
void BLI_setenv_if_new(const char *env, const char *val) ATTR_NONNULL(1);
/**
 * Get an environment variable, result has to be used immediately.
 *
 * On windows #getenv gets its variables from a static copy of the environment variables taken at
 * process start-up, causing it to not pick up on environment variables created during runtime.
 * This function uses an alternative method to get environment variables that does pick up on
 * runtime environment variables. The result will be UTF-8 encoded.
 */
const char *BLI_getenv(const char *env) ATTR_NONNULL(1) ATTR_WARN_UNUSED_RESULT;

/**
 * Returns in `string` the concatenation of `dir` and `file` (also with `relabase` on the
 * front if specified and `dir` begins with "//"). Normalizes all occurrences of path
 * separators, including ensuring there is exactly one between the copies of `dir` and `file`,
 * and between the copies of `relabase` and `dir`.
 *
 * \param relabase: Optional prefix to substitute for "//" on front of `dir`.
 * \param string: Area to return result.
 */
void BLI_make_file_string(const char *relabase, char *string, const char *dir, const char *file);
/**
 * Ensures that the parent directory of `name` exists.
 *
 * \return true on success (i.e. given path now exists on file-system), false otherwise.
 */
bool BLI_make_existing_file(const char *name);
/**
 * Converts `/foo/bar.txt` to `/foo/` and `bar.txt`
 *
 * - Won't change \a string.
 * - Won't create any directories.
 * - Doesn't use CWD, or deal with relative paths.
 * - Only fill's in \a dir and \a file when they are non NULL.
 */
void BLI_split_dirfile(const char *string, char *dir, char *file, size_t dirlen, size_t filelen);
/**
 * Copies the parent directory part of string into `dir`, max length `dirlen`.
 */
void BLI_split_dir_part(const char *string, char *dir, size_t dirlen);
/**
 * Copies the leaf filename part of string into `file`, max length `filelen`.
 */
void BLI_split_file_part(const char *string, char *file, size_t filelen);
/**
 * Returns a pointer to the last extension (e.g. the position of the last period).
 * Returns NULL if there is no extension.
 */
const char *BLI_path_extension(const char *filepath) ATTR_NONNULL();

/**
 * Append a filename to a dir, ensuring slash separates.
 */
void BLI_path_append(char *__restrict dst, size_t maxlen, const char *__restrict file)
    ATTR_NONNULL();
/**
 * Simple appending of filename to dir, does not check for valid path!
 * Puts result into `dst`, which may be same area as `dir`.
 *
 * \note Consider using #BLI_path_join for more general path joining
 * that de-duplicates separators and can handle an arbitrary number of paths.
 */
void BLI_join_dirfile(char *__restrict dst,
                      size_t maxlen,
                      const char *__restrict dir,
                      const char *__restrict file) ATTR_NONNULL();
/**
 * Join multiple strings into a path, ensuring only a single path separator between each,
 * and trailing slash is kept.
 *
 * \note If you want a trailing slash, add `SEP_STR` as the last path argument,
 * duplicate slashes will be cleaned up.
 */
size_t BLI_path_join(char *__restrict dst, size_t dst_len, const char *path_first, ...)
    ATTR_NONNULL(1, 3) ATTR_SENTINEL(0);
/**
 * Like Python's `os.path.basename()`
 *
 * \return The pointer into \a path string immediately after last slash,
 * or start of \a path if none found.
 */
const char *BLI_path_basename(const char *path) ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;
/**
 * Get an element of the path at an index, eg:
 * "/some/path/file.txt" where an index of:
 * - 0 or -3: "some"
 * - 1 or -2: "path"
 * - 2 or -1: "file.txt"
 *
 * Ignores multiple slashes at any point in the path (including start/end).
 */
bool BLI_path_name_at_index(const char *__restrict path,
                            int index,
                            int *__restrict r_offset,
                            int *__restrict r_len) ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;

/** Return true only if #containee_path is contained in #container_path. */
bool BLI_path_contains(const char *container_path,
                       const char *containee_path) ATTR_WARN_UNUSED_RESULT;

/**
 * \return pointer to the leftmost path separator in string (or NULL when not found).
 */
const char *BLI_path_slash_find(const char *string) ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;
/**
 * \return pointer to the rightmost path separator in string (or NULL when not found).
 */
const char *BLI_path_slash_rfind(const char *string) ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;
/**
 * Appends a slash to string if there isn't one there already.
 * Returns the new length of the string.
 */
int BLI_path_slash_ensure(char *string) ATTR_NONNULL();
/**
 * Removes the last slash and everything after it to the end of string, if there is one.
 */
void BLI_path_slash_rstrip(char *string) ATTR_NONNULL();
/**
 * Changes to the path separators to the native ones for this OS.
 */
void BLI_path_slash_native(char *path) ATTR_NONNULL();

#ifdef _WIN32
bool BLI_path_program_extensions_add_win32(char *name, size_t maxlen);
#endif
/**
 * Search for a binary (executable)
 */
bool BLI_path_program_search(char *fullname, size_t maxlen, const char *name);

/**
 * \return true when `str` end with `ext` (case insensitive).
 */
bool BLI_path_extension_check(const char *str, const char *ext)
    ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;
bool BLI_path_extension_check_n(const char *str, ...) ATTR_NONNULL(1) ATTR_SENTINEL(0);
/**
 * \return true when `str` ends with any of the suffixes in `ext_array`.
 */
bool BLI_path_extension_check_array(const char *str, const char **ext_array)
    ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;
/**
 * Semicolon separated wildcards, eg: `*.zip;*.py;*.exe`
 * does `str` match any of the semicolon-separated glob patterns in #fnmatch.
 */
bool BLI_path_extension_check_glob(const char *str, const char *ext_fnmatch)
    ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;
/**
 * Does basic validation of the given glob string, to prevent common issues from string
 * truncation.
 *
 * For now, only forbids last group to be a wildcard-only one, if there are more than one group
 * (i.e. things like `*.txt;*.cpp;*` are changed to `*.txt;*.cpp;`)
 *
 * \returns true if it had to modify given \a ext_fnmatch pattern.
 */
bool BLI_path_extension_glob_validate(char *ext_fnmatch) ATTR_NONNULL();
/**
 * Removes any existing extension on the end of \a path and appends \a ext.
 * \return false if there was no room.
 */
bool BLI_path_extension_replace(char *path, size_t maxlen, const char *ext) ATTR_NONNULL();
/**
 * Strip's trailing '.'s and adds the extension only when needed
 */
bool BLI_path_extension_ensure(char *path, size_t maxlen, const char *ext) ATTR_NONNULL();
/**
 * Ensure `filepath` has a file component, adding `filename` when it's empty or ends with a slash.
 * \return true if the `filename` was appended to `filepath`.
 */
bool BLI_path_filename_ensure(char *filepath, size_t maxlen, const char *filename) ATTR_NONNULL();
/**
 * Looks for a sequence of decimal digits in string, preceding any filename extension,
 * returning the integer value if found, or 0 if not.
 *
 * \param string: String to scan.
 * \param head: Optional area to return copy of part of string prior to digits,
 * or before dot if no digits.
 * \param tail: Optional area to return copy of part of string following digits,
 * or from dot if no digits.
 * \param r_digits_len: Optional to return number of digits found.
 */
int BLI_path_sequence_decode(const char *string,
                             char *head,
                             char *tail,
                             unsigned short *r_digits_len);
/**
 * Returns in area pointed to by string a string of the form `<head><pic><tail>`,
 * where pic is formatted as `numlen` digits with leading zeroes.
 */
void BLI_path_sequence_encode(
    char *string, const char *head, const char *tail, unsigned short numlen, int pic);

/**
 * Remove redundant characters from \a path and optionally make absolute.
 *
 * \param relabase: The path this is relative to, or ignored when NULL.
 * \param path: Can be any input, and this function converts it to a regular full path.
 * Also removes garbage from directory paths, like `/../` or double slashes etc.
 *
 * \note \a path isn't protected for max string names.
 */
void BLI_path_normalize(const char *relabase, char *path) ATTR_NONNULL(2);
/**
 * Cleanup file-path ensuring a trailing slash.
 *
 * \note Same as #BLI_path_normalize but adds a trailing slash.
 */
void BLI_path_normalize_dir(const char *relabase, char *dir) ATTR_NONNULL(2);

/**
 * Make given name safe to be used in paths.
 *
 * \param allow_tokens: Permit the usage of '<' and '>' characters. This can be
 * leveraged by higher layers to support "virtual filenames" which contain
 * substitution markers delineated between the two characters.
 *
 * \return true if \a fname was changed, false otherwise.
 *
 * For now, simply replaces reserved chars (as listed in
 * https://en.wikipedia.org/wiki/Filename#Reserved_characters_and_words )
 * by underscores ('_').
 *
 * \note Space case ' ' is a bit of an edge case here - in theory it is allowed,
 * but again can be an issue in some cases, so we simply replace it by an underscore too
 * (good practice anyway).
 * REMOVED based on popular demand (see T45900).
 * Percent '%' char is a bit same case - not recommended to use it,
 * but supported by all decent file-systems/operating-systems around.
 *
 * \note On Windows, it also ensures there is no '.' (dot char) at the end of the file,
 * this can lead to issues.
 *
 * \note On Windows, it also checks for forbidden names
 * (see https://msdn.microsoft.com/en-us/library/windows/desktop/aa365247%28v=vs.85%29.aspx ).
 */
bool BLI_filename_make_safe_ex(char *fname, bool allow_tokens) ATTR_NONNULL(1);
bool BLI_filename_make_safe(char *fname) ATTR_NONNULL(1);

/**
 * Make given path OS-safe.
 *
 * \return true if \a path was changed, false otherwise.
 */
bool BLI_path_make_safe(char *path) ATTR_NONNULL(1);

/**
 * Go back one directory.
 *
 * Replaces path with the path of its parent directory, returning true if
 * it was able to find a parent directory within the path.
 */
bool BLI_path_parent_dir(char *path) ATTR_NONNULL();
/**
 * Go back until the directory is found.
 *
 * Strips off nonexistent (or non-accessible) sub-directories from the end of `dir`,
 * leaving the path of the lowest-level directory that does exist and we can read.
 */
bool BLI_path_parent_dir_until_exists(char *path) ATTR_NONNULL();

/**
 * If path begins with "//", strips that and replaces it with `basepath` directory.
 *
 * \note Also converts drive-letter prefix to something more sensible
 * if this is a non-drive-letter-based system.
 *
 * \param path: The path to convert.
 * \param basepath: The directory to base relative paths with.
 * \return true if the path was relative (started with "//").
 */
bool BLI_path_abs(char *path, const char *basepath) ATTR_NONNULL();
/**
 * Replaces "#" character sequence in last slash-separated component of `path`
 * with frame as decimal integer, with leading zeroes as necessary, to make digits.
 */
bool BLI_path_frame(char *path, int frame, int digits) ATTR_NONNULL();
/**
 * Replaces "#" character sequence in last slash-separated component of `path`
 * with sta and end as decimal integers, with leading zeroes as necessary, to make digits
 * digits each, with a hyphen in-between.
 */
bool BLI_path_frame_range(char *path, int sta, int end, int digits) ATTR_NONNULL();
/**
 * Get the frame from a filename formatted by blender's frame scheme
 */
bool BLI_path_frame_get(char *path, int *r_frame, int *r_digits_len) ATTR_NONNULL();
/**
 * Given a `path` with digits representing frame numbers, replace the digits with the '#'
 * character and extract the extension.
 * So:      `/some/path_123.jpeg`
 * Becomes: `/some/path_###` with `r_ext` set to `.jpeg`.
 */
void BLI_path_frame_strip(char *path, char *r_ext) ATTR_NONNULL();
/**
 * Check if we have '#' chars, usable for #BLI_path_frame, #BLI_path_frame_range
 */
bool BLI_path_frame_check_chars(const char *path) ATTR_NONNULL(1) ATTR_WARN_UNUSED_RESULT;
/**
 * Checks for a relative path (ignoring Blender's "//") prefix
 * (unlike `!BLI_path_is_rel(path)`).
 * When false, #BLI_path_abs_from_cwd would expand the absolute path.
 */
bool BLI_path_is_abs_from_cwd(const char *path) ATTR_NONNULL(1) ATTR_WARN_UNUSED_RESULT;
/**
 * Checks for relative path, expanding them relative to the current working directory.
 * \returns true if the expansion was performed.
 *
 * \note Should only be called with command line paths.
 * This is _not_ something Blender's internal paths support, instead they use the "//" prefix.
 * In most cases #BLI_path_abs should be used instead.
 */
bool BLI_path_abs_from_cwd(char *path, size_t maxlen) ATTR_NONNULL();
/**
 * Replaces `file` with a relative version (prefixed by "//") such that #BLI_path_abs, given
 * the same `relfile`, will convert it back to its original value.
 */
void BLI_path_rel(char *file, const char *relfile) ATTR_NONNULL();

/**
 * Does path begin with the special "//" prefix that Blender uses to indicate
 * a path relative to the .blend file.
 */
bool BLI_path_is_rel(const char *path) ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;
/**
 * Return true if the path is a UNC share.
 */
bool BLI_path_is_unc(const char *path) ATTR_NONNULL(1) ATTR_WARN_UNUSED_RESULT;

/**
 * Creates a display string from path to be used menus and the user interface.
 * Like `bpy.path.display_name()`.
 */
void BLI_path_to_display_name(char *display_name, int maxlen, const char *name) ATTR_NONNULL();

#if defined(WIN32)
void BLI_path_normalize_unc_16(wchar_t *path_16);
void BLI_path_normalize_unc(char *path_16, int maxlen);
#endif

/**
 * Appends a suffix to the string, fitting it before the extension
 *
 * string = Foo.png, suffix = 123, separator = _
 * Foo.png -> Foo_123.png
 *
 * \param string: original (and final) string
 * \param maxlen: Maximum length of string
 * \param suffix: String to append to the original string
 * \param sep: Optional separator character
 * \return true if succeeded
 */
bool BLI_path_suffix(char *string, size_t maxlen, const char *suffix, const char *sep)
    ATTR_NONNULL();

/* Path string comparisons: case-insensitive for Windows, case-sensitive otherwise. */
#if defined(WIN32)
#  define BLI_path_cmp BLI_strcasecmp
#  define BLI_path_ncmp BLI_strncasecmp
#else
#  define BLI_path_cmp strcmp
#  define BLI_path_ncmp strncmp
#endif

/**
 * Returns the result of #BLI_path_cmp with both paths normalized and slashes made native.
 *
 * \note #BLI_path_cmp is used for Blender's internal logic to consider paths to be the same
 * #BLI_path_cmp_normalized may be used in when handling other kinds of paths
 * (e.g. importers/exporters) but should be used consistently.
 *
 * Checking the normalized paths is not a guarantee the paths reference different files.
 * An equivalent to Python's `os.path.samefile` could be supported for checking if paths
 * point to the same location on the file-system (following symbolic-links).
 */
int BLI_path_cmp_normalized(const char *p1, const char *p2)
    ATTR_NONNULL(1, 2) ATTR_WARN_UNUSED_RESULT;

/* These values need to be hard-coded in structs, dna does not recognize defines */
/* also defined in `DNA_space_types.h`. */
#ifndef FILE_MAXDIR
#  define FILE_MAXDIR 768
#  define FILE_MAXFILE 256
#  define FILE_MAX 1024
#endif

#ifdef WIN32
#  define SEP '\\'
#  define ALTSEP '/'
#  define SEP_STR "\\"
#  define ALTSEP_STR "/"
#else
#  define SEP '/'
#  define ALTSEP '\\'
#  define SEP_STR "/"
#  define ALTSEP_STR "\\"
#endif

/* Parent and current dir helpers. */
#define FILENAME_PARENT ".."
#define FILENAME_CURRENT "."

/* Avoid calling `strcmp` on one or two chars! */
#define FILENAME_IS_PARENT(_n) (((_n)[0] == '.') && ((_n)[1] == '.') && ((_n)[2] == '\0'))
#define FILENAME_IS_CURRENT(_n) (((_n)[0] == '.') && ((_n)[1] == '\0'))
#define FILENAME_IS_CURRPAR(_n) \
  (((_n)[0] == '.') && (((_n)[1] == '\0') || (((_n)[1] == '.') && ((_n)[2] == '\0'))))

#ifdef __cplusplus
}
#endif
