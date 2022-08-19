/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

#pragma once

/** \file
 * \ingroup bli
 */

#include <inttypes.h>
#include <stdarg.h>

#include "BLI_compiler_attrs.h"
#include "BLI_utildefines.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Duplicates the first \a len bytes of cstring \a str
 * into a newly mallocN'd string and returns it. \a str
 * is assumed to be at least len bytes long.
 *
 * \param str: The string to be duplicated
 * \param len: The number of bytes to duplicate
 * \retval Returns the duplicated string
 */
char *BLI_strdupn(const char *str, size_t len) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT ATTR_NONNULL();

/**
 * Duplicates the cstring \a str into a newly mallocN'd
 * string and returns it.
 *
 * \param str: The string to be duplicated
 * \retval Returns the duplicated string
 */
char *BLI_strdup(const char *str) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL() ATTR_MALLOC;

/**
 * Appends the two strings, and returns new mallocN'ed string
 * \param str1: first string for copy
 * \param str2: second string for append
 * \retval Returns dst
 */
char *BLI_strdupcat(const char *__restrict str1,
                    const char *__restrict str2) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL() ATTR_MALLOC;

/**
 * Like strncpy but ensures dst is always
 * '\0' terminated.
 *
 * \param dst: Destination for copy
 * \param src: Source string to copy
 * \param maxncpy: Maximum number of characters to copy (generally
 * the size of dst)
 * \retval Returns dst
 */
char *BLI_strncpy(char *__restrict dst, const char *__restrict src, size_t maxncpy) ATTR_NONNULL();

/**
 * Like BLI_strncpy but ensures dst is always padded by given char,
 * on both sides (unless src is empty).
 *
 * \param dst: Destination for copy
 * \param src: Source string to copy
 * \param pad: the char to use for padding
 * \param maxncpy: Maximum number of characters to copy (generally the size of dst)
 * \retval Returns dst
 */
char *BLI_strncpy_ensure_pad(char *__restrict dst,
                             const char *__restrict src,
                             char pad,
                             size_t maxncpy) ATTR_NONNULL();

/**
 * Like strncpy but ensures dst is always
 * '\0' terminated.
 *
 * \note This is a duplicate of #BLI_strncpy that returns bytes copied.
 * And is a drop in replacement for 'snprintf(str, sizeof(str), "%s", arg);'
 *
 * \param dst: Destination for copy
 * \param src: Source string to copy
 * \param maxncpy: Maximum number of characters to copy (generally
 * the size of dst)
 * \retval The number of bytes copied (The only difference from BLI_strncpy).
 */
size_t BLI_strncpy_rlen(char *__restrict dst,
                        const char *__restrict src,
                        size_t maxncpy) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL();

size_t BLI_strcpy_rlen(char *__restrict dst, const char *__restrict src) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL();

/**
 * Return the range of the quoted string (excluding quotes) `str` after `prefix`.
 *
 * A version of #BLI_str_quoted_substrN that calculates the range
 * instead of un-escaping and allocating the result.
 *
 * \param str: String potentially including `prefix`.
 * \param prefix: Quoted string prefix.
 * \param r_start: The start of the quoted string (after the first quote).
 * \param r_end: The end of the quoted string (before the last quote).
 * \return True when a quoted string range could be found after `prefix`.
 */
bool BLI_str_quoted_substr_range(const char *__restrict str,
                                 const char *__restrict prefix,
                                 int *__restrict r_start,
                                 int *__restrict r_end) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL(1, 2, 3, 4);
#if 0 /* UNUSED */
char *BLI_str_quoted_substrN(const char *__restrict str,
                             const char *__restrict prefix) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL() ATTR_MALLOC;
#endif
/**
 * Fills \a result with text within "" that appear after some the contents of \a prefix.
 * i.e. for string `pose["apples"]` with prefix `pose[`, it will return `apples`.
 *
 * \param str: is the entire string to chop.
 * \param prefix: is the part of the string to step over.
 * \param result: The buffer to fill.
 * \param result_maxlen: The maximum size of the buffer (including nil terminator).
 * \return True if the prefix was found and the entire quoted string was copied into result.
 *
 * Assume that the strings returned must be freed afterwards,
 * and that the inputs will contain data we want.
 */
bool BLI_str_quoted_substr(const char *__restrict str,
                           const char *__restrict prefix,
                           char *result,
                           size_t result_maxlen);
/**
 * string with all instances of substr_old replaced with substr_new,
 * Returns a copy of the c-string \a str into a newly #MEM_mallocN'd
 * and returns it.
 *
 * \note A rather wasteful string-replacement utility, though this shall do for now.
 * Feel free to replace this with an even safe + nicer alternative
 *
 * \param str: The string to replace occurrences of substr_old in
 * \param substr_old: The text in the string to find and replace
 * \param substr_new: The text in the string to find and replace
 * \retval Returns the duplicated string
 */
char *BLI_str_replaceN(const char *__restrict str,
                       const char *__restrict substr_old,
                       const char *__restrict substr_new) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL() ATTR_MALLOC;

/**
 * In-place replace every \a src to \a dst in \a str.
 *
 * \param str: The string to operate on.
 * \param src: The character to replace.
 * \param dst: The character to replace with.
 */
void BLI_str_replace_char(char *str, char src, char dst) ATTR_NONNULL();

/**
 * Simple exact-match string replacement.
 *
 * \param replace_table: Array of source, destination pairs.
 *
 * \note Larger tables should use a hash table.
 */
bool BLI_str_replace_table_exact(char *string,
                                 size_t string_len,
                                 const char *replace_table[][2],
                                 int replace_table_len);

/**
 * Portable replacement for #snprintf
 */
size_t BLI_snprintf(char *__restrict dst, size_t maxncpy, const char *__restrict format, ...)
    ATTR_NONNULL(1, 3) ATTR_PRINTF_FORMAT(3, 4);
/**
 * A version of #BLI_snprintf that returns `strlen(dst)`
 */
size_t BLI_snprintf_rlen(char *__restrict dst, size_t maxncpy, const char *__restrict format, ...)
    ATTR_NONNULL(1, 3) ATTR_PRINTF_FORMAT(3, 4);

/**
 * Portable replacement for `vsnprintf`.
 */
size_t BLI_vsnprintf(char *__restrict buffer,
                     size_t maxncpy,
                     const char *__restrict format,
                     va_list arg) ATTR_PRINTF_FORMAT(3, 0);
/**
 * A version of #BLI_vsnprintf that returns `strlen(buffer)`
 */
size_t BLI_vsnprintf_rlen(char *__restrict buffer,
                          size_t maxncpy,
                          const char *__restrict format,
                          va_list arg) ATTR_PRINTF_FORMAT(3, 0);

/**
 * Print formatted string into a newly #MEM_mallocN'd string
 * and return it.
 */
char *BLI_sprintfN(const char *__restrict format, ...) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL(1) ATTR_MALLOC ATTR_PRINTF_FORMAT(1, 2);

/**
 * This roughly matches C and Python's string escaping with double quotes - `"`.
 *
 * Since every character may need escaping,
 * it's common to create a buffer twice as large as the input.
 *
 * \param dst: The destination string, at least \a dst_maxncpy, typically `(strlen(src) * 2) + 1`.
 * \param src: The un-escaped source string.
 * \param dst_maxncpy: The maximum number of bytes allowable to copy.
 *
 * \note This is used for creating animation paths in blend files.
 */
size_t BLI_str_escape(char *__restrict dst, const char *__restrict src, size_t dst_maxncpy)
    ATTR_NONNULL();
/**
 * This roughly matches C and Python's string escaping with double quotes - `"`.
 *
 * The destination will never be larger than the source, it will either be the same
 * or up to half when all characters are escaped.
 *
 * \param dst: The destination string, at least the size of `strlen(src) + 1`.
 * \param src: The escaped source string.
 * \param src_maxncpy: The maximum number of bytes allowable to copy from `src`.
 * \param dst_maxncpy: The maximum number of bytes allowable to copy into `dst`.
 * \param r_is_complete: Set to true when
 */
size_t BLI_str_unescape_ex(char *__restrict dst,
                           const char *__restrict src,
                           size_t src_maxncpy,
                           /* Additional arguments. */
                           size_t dst_maxncpy,
                           bool *r_is_complete) ATTR_NONNULL();
/**
 * See #BLI_str_unescape_ex doc-string.
 *
 * This function makes the assumption that `dst` always has
 * at least `src_maxncpy` bytes available.
 *
 * Use #BLI_str_unescape_ex if `dst` has a smaller fixed size.
 *
 * \note This is used for parsing animation paths in blend files (runs often).
 */
size_t BLI_str_unescape(char *__restrict dst, const char *__restrict src, size_t src_maxncpy)
    ATTR_NONNULL();

/**
 * Find the first un-escaped quote in the string (to find the end of the string).
 *
 * \param str: Typically this is the first character in a quoted string.
 * Where the character before `*str` would be `"`.
 *
 * \return The pointer to the first un-escaped quote.
 */
const char *BLI_str_escape_find_quote(const char *str) ATTR_NONNULL();

/**
 * Format ints with decimal grouping.
 * 1000 -> 1,000
 *
 * \param dst: The resulting string
 * \param num: Number to format
 * \return The length of \a dst
 */
size_t BLI_str_format_int_grouped(char dst[16], int num) ATTR_NONNULL();
/**
 * Format uint64_t with decimal grouping.
 * 1000 -> 1,000
 *
 * \param dst: The resulting string
 * \param num: Number to format
 * \return The length of \a dst
 */
size_t BLI_str_format_uint64_grouped(char dst[16], uint64_t num) ATTR_NONNULL();
/**
 * Format a size in bytes using binary units.
 * 1000 -> 1 KB
 * Number of decimal places grows with the used unit (e.g. 1.5 MB, 1.55 GB, 1.545 TB).
 *
 * \param dst: The resulting string.
 * Dimension of 14 to support largest possible value for \a bytes (#LLONG_MAX).
 * \param bytes: Number to format.
 * \param base_10: Calculate using base 10 (GB, MB, ...) or 2 (GiB, MiB, ...).
 */
void BLI_str_format_byte_unit(char dst[15], long long int bytes, bool base_10) ATTR_NONNULL();
/**
 * Format a count to up to 6 places (plus '\0' terminator) string using long number
 * names abbreviations. Used to produce a compact representation of large numbers.
 *
 * 1 -> 1
 * 15 -> 15
 * 155 -> 155
 * 1555 -> 1.6K
 * 15555 -> 15.6K
 * 155555 -> 156K
 * 1555555 -> 1.6M
 * 15555555 -> 15.6M
 * 155555555 -> 156M
 * 1000000000 -> 1B
 * ...
 *
 * Length of 7 is the maximum of the resulting string, for example, `-15.5K\0`.
 */
void BLI_str_format_decimal_unit(char dst[7], int number_to_format) ATTR_NONNULL();
/**
 * Compare two strings without regard to case.
 *
 * \retval True if the strings are equal, false otherwise.
 */
int BLI_strcaseeq(const char *a, const char *b) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL();
/**
 * Portable replacement for `strcasestr` (not available in MSVC)
 */
char *BLI_strcasestr(const char *s, const char *find) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL();
/**
 * Variation of #BLI_strcasestr with string length limited to \a len
 */
char *BLI_strncasestr(const char *s, const char *find, size_t len) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL();
int BLI_strcasecmp(const char *s1, const char *s2) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL();
int BLI_strncasecmp(const char *s1, const char *s2, size_t len) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL();
/**
 * Case insensitive, *natural* string comparison,
 * keeping numbers in order.
 */
int BLI_strcasecmp_natural(const char *s1, const char *s2) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL();
/**
 * Like strcmp, but will ignore any heading/trailing pad char for comparison.
 * So e.g. if pad is '*', '*world' and 'world*' will compare equal.
 */
int BLI_strcmp_ignore_pad(const char *str1, const char *str2, char pad) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL();

/**
 * Determine the length of a fixed-size string.
 */
size_t BLI_strnlen(const char *str, size_t maxlen) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL();

/**
 * String case conversion, not affected by locale.
 */

void BLI_str_tolower_ascii(char *str, size_t len) ATTR_NONNULL();
void BLI_str_toupper_ascii(char *str, size_t len) ATTR_NONNULL();

char BLI_tolower_ascii(const char c);
char BLI_toupper_ascii(const char c);

/**
 * Strip white-space from end of the string.
 */
void BLI_str_rstrip(char *str) ATTR_NONNULL();
/**
 * Strip trailing zeros from a float, eg:
 *   0.0000 -> 0.0
 *   2.0010 -> 2.001
 *
 * \param str:
 * \param pad:
 * \return The number of zeros stripped.
 */
int BLI_str_rstrip_float_zero(char *str, char pad) ATTR_NONNULL();

/**
 * Return index of a string in a string array.
 *
 * \param str: The string to find.
 * \param str_array: Array of strings.
 * \param str_array_len: The length of the array, or -1 for a NULL-terminated array.
 * \return The index of str in str_array or -1.
 */
int BLI_str_index_in_array_n(const char *__restrict str,
                             const char **__restrict str_array,
                             int str_array_len) ATTR_NONNULL();
/**
 * Return index of a string in a string array.
 *
 * \param str: The string to find.
 * \param str_array: Array of strings, (must be NULL-terminated).
 * \return The index of str in str_array or -1.
 */
int BLI_str_index_in_array(const char *__restrict str, const char **__restrict str_array)
    ATTR_NONNULL();

/**
 * Find if a string starts with another string.
 *
 * \param str: The string to search within.
 * \param start: The string we look for at the start.
 * \return If str starts with start.
 */
bool BLI_str_startswith(const char *__restrict str, const char *__restrict start) ATTR_NONNULL();
/**
 * Find if a string ends with another string.
 *
 * \param str: The string to search within.
 * \param end: The string we look for at the end.
 * \return If str ends with end.
 */
bool BLI_str_endswith(const char *__restrict str, const char *__restrict end) ATTR_NONNULL();
bool BLI_strn_endswith(const char *__restrict str, const char *__restrict end, size_t length)
    ATTR_NONNULL();

/**
 * Find the first char matching one of the chars in \a delim, from left.
 *
 * \param str: The string to search within.
 * \param delim: The set of delimiters to search for, as unicode values.
 * \param sep: Return value, set to the first delimiter found (or NULL if none found).
 * \param suf: Return value, set to next char after the first delimiter found
 * (or NULL if none found).
 * \return The length of the prefix (i.e. *sep - str).
 */
size_t BLI_str_partition(const char *str, const char delim[], const char **sep, const char **suf)
    ATTR_NONNULL();
/**
 * Find the first char matching one of the chars in \a delim, from right.
 *
 * \param str: The string to search within.
 * \param delim: The set of delimiters to search for, as unicode values.
 * \param sep: Return value, set to the first delimiter found (or NULL if none found).
 * \param suf: Return value, set to next char after the first delimiter found
 * (or NULL if none found).
 * \return The length of the prefix (i.e. *sep - str).
 */
size_t BLI_str_rpartition(const char *str, const char delim[], const char **sep, const char **suf)
    ATTR_NONNULL();
/**
 * Find the first char matching one of the chars in \a delim, either from left or right.
 *
 * \param str: The string to search within.
 * \param end: If non-NULL, the right delimiter of the string.
 * \param delim: The set of delimiters to search for, as unicode values.
 * \param sep: Return value, set to the first delimiter found (or NULL if none found).
 * \param suf: Return value, set to next char after the first delimiter found
 * (or NULL if none found).
 * \param from_right: If %true, search from the right of \a str, else, search from its left.
 * \return The length of the prefix (i.e. *sep - str).
 */
size_t BLI_str_partition_ex(const char *str,
                            const char *end,
                            const char delim[],
                            const char **sep,
                            const char **suf,
                            bool from_right) ATTR_NONNULL(1, 3, 4, 5);

int BLI_string_max_possible_word_count(int str_len);
bool BLI_string_has_word_prefix(const char *haystack, const char *needle, size_t needle_len);
bool BLI_string_all_words_matched(const char *name,
                                  const char *str,
                                  int (*words)[2],
                                  int words_len);

/**
 * Find the ranges needed to split \a str into its individual words.
 *
 * \param str: The string to search for words.
 * \param len: Size of the string to search.
 * \param delim: Character to use as a delimiter.
 * \param r_words: Info about the words found. Set to [index, len] pairs.
 * \param words_max: Max number of words to find
 * \return The number of words found in \a str
 */
int BLI_string_find_split_words(const char *str,
                                size_t len,
                                char delim,
                                int r_words[][2],
                                int words_max) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL();

/* -------------------------------------------------------------------- */
/** \name String Copy/Format Macros
 * Avoid repeating destination with `sizeof(..)`.
 * \note `ARRAY_SIZE` allows pointers on some platforms.
 * \{ */

#define STRNCPY(dst, src) BLI_strncpy(dst, src, ARRAY_SIZE(dst))
#define STRNCPY_RLEN(dst, src) BLI_strncpy_rlen(dst, src, ARRAY_SIZE(dst))
#define SNPRINTF(dst, format, ...) BLI_snprintf(dst, ARRAY_SIZE(dst), format, __VA_ARGS__)
#define SNPRINTF_RLEN(dst, format, ...) \
  BLI_snprintf_rlen(dst, ARRAY_SIZE(dst), format, __VA_ARGS__)
#define STR_CONCAT(dst, len, suffix) \
  len += BLI_strncpy_rlen(dst + len, suffix, ARRAY_SIZE(dst) - len)
#define STR_CONCATF(dst, len, format, ...) \
  len += BLI_snprintf_rlen(dst + len, ARRAY_SIZE(dst) - len, format, __VA_ARGS__)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Equal to Any Element (STR_ELEM) Macro
 *
 * Follows #ELEM macro convention.
 * \{ */

/* Manual line breaks for readability. */
/* clang-format off */
/* STR_ELEM#(v, ...): is the first arg equal any others? */
/* Internal helpers. */
#define _VA_STR_ELEM2(v, a) (strcmp(v, a) == 0)
#define _VA_STR_ELEM3(v, a, b) \
  (_VA_STR_ELEM2(v, a) || (_VA_STR_ELEM2(v, b)))
#define _VA_STR_ELEM4(v, a, b, c) \
  (_VA_STR_ELEM3(v, a, b) || (_VA_STR_ELEM2(v, c)))
#define _VA_STR_ELEM5(v, a, b, c, d) \
  (_VA_STR_ELEM4(v, a, b, c) || (_VA_STR_ELEM2(v, d)))
#define _VA_STR_ELEM6(v, a, b, c, d, e) \
  (_VA_STR_ELEM5(v, a, b, c, d) || (_VA_STR_ELEM2(v, e)))
#define _VA_STR_ELEM7(v, a, b, c, d, e, f) \
  (_VA_STR_ELEM6(v, a, b, c, d, e) || (_VA_STR_ELEM2(v, f)))
#define _VA_STR_ELEM8(v, a, b, c, d, e, f, g) \
  (_VA_STR_ELEM7(v, a, b, c, d, e, f) || (_VA_STR_ELEM2(v, g)))
#define _VA_STR_ELEM9(v, a, b, c, d, e, f, g, h) \
  (_VA_STR_ELEM8(v, a, b, c, d, e, f, g) || (_VA_STR_ELEM2(v, h)))
#define _VA_STR_ELEM10(v, a, b, c, d, e, f, g, h, i) \
  (_VA_STR_ELEM9(v, a, b, c, d, e, f, g, h) || (_VA_STR_ELEM2(v, i)))
#define _VA_STR_ELEM11(v, a, b, c, d, e, f, g, h, i, j) \
  (_VA_STR_ELEM10(v, a, b, c, d, e, f, g, h, i) || (_VA_STR_ELEM2(v, j)))
#define _VA_STR_ELEM12(v, a, b, c, d, e, f, g, h, i, j, k) \
  (_VA_STR_ELEM11(v, a, b, c, d, e, f, g, h, i, j) || (_VA_STR_ELEM2(v, k)))
#define _VA_STR_ELEM13(v, a, b, c, d, e, f, g, h, i, j, k, l) \
  (_VA_STR_ELEM12(v, a, b, c, d, e, f, g, h, i, j, k) || (_VA_STR_ELEM2(v, l)))
#define _VA_STR_ELEM14(v, a, b, c, d, e, f, g, h, i, j, k, l, m) \
  (_VA_STR_ELEM13(v, a, b, c, d, e, f, g, h, i, j, k, l) || (_VA_STR_ELEM2(v, m)))
#define _VA_STR_ELEM15(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n) \
  (_VA_STR_ELEM14(v, a, b, c, d, e, f, g, h, i, j, k, l, m) || (_VA_STR_ELEM2(v, n)))
#define _VA_STR_ELEM16(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o) \
  (_VA_STR_ELEM15(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n) || (_VA_STR_ELEM2(v, o)))
#define _VA_STR_ELEM17(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
  (_VA_STR_ELEM16(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o) || (_VA_STR_ELEM2(v, p)))
/* clang-format on */

/* reusable STR_ELEM macro */
#define STR_ELEM(...) VA_NARGS_CALL_OVERLOAD(_VA_STR_ELEM, __VA_ARGS__)

/** \} */

#ifdef __cplusplus
}
#endif
