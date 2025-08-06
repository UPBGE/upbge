/* SPDX-FileCopyrightText: 2017 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <cctype>
#include <cstdlib>
#include <cstring>

#include <array>

#include <stdexcept>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_utildefines.h"

#include "BLI_dynstr.h"

#include "DNA_listBase.h"

#include "BLI_strict_flags.h" /* IWYU pragma: keep. Keep last. */

/* -------------------------------------------------------------------- */
/** \name String Replace
 * \{ */

char *BLI_string_replaceN(const char *__restrict str,
                          const char *__restrict substr_old,
                          const char *__restrict substr_new)
{
  DynStr *ds = nullptr;
  size_t len_old = strlen(substr_old);
  const char *match;

  BLI_assert(substr_old[0] != '\0');

  /* While we can still find a match for the old sub-string that we're searching for,
   * keep dicing and replacing. */
  while ((match = strstr(str, substr_old))) {
    /* the assembly buffer only gets created when we actually need to rebuild the string */
    if (ds == nullptr) {
      ds = BLI_dynstr_new();
    }

    /* If the match position does not match the current position in the string,
     * copy the text up to this position and advance the current position in the string. */
    if (str != match) {
      /* Add the segment of the string from `str` to match to the buffer,
       * then restore the value at match. */
      BLI_dynstr_nappend(ds, str, int(match - str));

      /* now our current position should be set on the start of the match */
      str = match;
    }

    /* Add the replacement text to the accumulation buffer. */
    BLI_dynstr_append(ds, substr_new);

    /* Advance the current position of the string up to the end of the replaced segment. */
    str += len_old;
  }

  /* Finish off and return a new string that has had all occurrences of. */
  if (ds) {
    char *str_new;

    /* Add what's left of the string to the assembly buffer
     * - we've been adjusting `str` to point at the end of the replaced segments. */
    BLI_dynstr_append(ds, str);

    /* Convert to new c-string (MEM_malloc'd), and free the buffer. */
    str_new = BLI_dynstr_get_cstring(ds);
    BLI_dynstr_free(ds);

    return str_new;
  }
  /* Just create a new copy of the entire string - we avoid going through the assembly buffer
   * for what should be a bit more efficiency. */
  return BLI_strdup(str);
}

void BLI_string_replace(std::string &haystack,
                        const blender::StringRef needle,
                        const blender::StringRef other)
{
  size_t i = 0;
  size_t index;
  while ((index = haystack.find(needle, i)) != std::string::npos) {
    haystack.replace(index, size_t(needle.size()), other);
    i = index + size_t(other.size());
  }
}

void BLI_string_replace_char(char *str, char src, char dst)
{
  while (*str) {
    if (*str == src) {
      *str = dst;
    }
    str++;
  }
}

bool BLI_string_replace_table_exact(char *string,
                                    const size_t string_len,
                                    const char *replace_table[][2],
                                    int replace_table_len)
{
  BLI_string_debug_size_after_nil(string, string_len);

  for (int i = 0; i < replace_table_len; i++) {
    if (STREQ(string, replace_table[i][0])) {
      BLI_strncpy(string, replace_table[i][1], string_len);
      return true;
    }
  }
  return false;
}

size_t BLI_string_replace_range(
    char *string, size_t string_maxncpy, int src_beg, int src_end, const char *dst)
{
  int string_len = int(strlen(string));
  BLI_assert(src_beg <= src_end);
  BLI_assert(src_end <= string_len);
  const int src_len = src_end - src_beg;
  int dst_len = int(strlen(dst));

  if (src_len < dst_len) {
    /* Grow, first handle special cases. */

    /* Special case, the src_end is entirely clipped. */
    if (UNLIKELY(int(string_maxncpy) <= src_beg + dst_len)) {
      /* There is only room for the destination. */
      dst_len = (int(string_maxncpy) - src_beg) - 1;
      string_len = src_end;
      string[string_len] = '\0';
    }

    const int ofs = dst_len - src_len;
    /* Clip the string when inserting the destination string exceeds `string_maxncpy`. */
    if (string_len + ofs >= int(string_maxncpy)) {
      string_len = (int(string_maxncpy) - ofs) - 1;
      string[string_len] = '\0';
      BLI_assert(src_end <= string_len);
    }

    /* Grow. */
    memmove(string + (src_end + ofs), string + src_end, size_t(string_len - src_end) + 1);
    string_len += ofs;
  }
  else if (src_len > dst_len) {
    /* Shrink. */
    const int ofs = src_len - dst_len;
    memmove(string + (src_end - ofs), string + src_end, size_t(string_len - src_end) + 1);
    string_len -= ofs;
  }
  else { /* Simple case, no resizing. */
    BLI_assert(src_len == dst_len);
  }

  if (dst_len > 0) {
    memcpy(string + src_beg, dst, size_t(dst_len));
  }
  BLI_assert(string[string_len] == '\0');
  return size_t(string_len);
}

/** \} */

blender::StringRef BLI_string_split_name_number(const blender::StringRef name_full,
                                                const char delim,
                                                int &r_number)
{
  const int64_t delim_index = name_full.rfind(delim);
  r_number = 0;
  if (delim_index == blender::StringRef::not_found) {
    return name_full;
  }

  blender::StringRef name_base = name_full.substr(0, delim_index);

  if (delim_index < name_full.size() - 1) {
    const blender::StringRef num_str = name_full.substr(delim_index + 1);
    if (!std::all_of(num_str.begin(), num_str.end(), ::isdigit)) {
      return name_full;
    }
    /* Converting numerical suffix to an int, can overflow for large numbers. */
    try {
      r_number = std::stoi(num_str);
      return name_base;
    }
    catch (std::out_of_range const & /*ex*/) {
      r_number = 0;
    }
  }

  return name_full;
}

size_t BLI_string_split_name_number(const char *name,
                                    const char delim,
                                    char *r_name_left,
                                    int *r_number)
{
  const std::string name_base = BLI_string_split_name_number(name, delim, *r_number);
  BLI_strncpy(r_name_left, name_base.c_str(), name_base.size() + 1);
  return name_base.size();
}

bool BLI_string_is_decimal(const char *string)
{
  if (*string == '\0') {
    return false;
  }

  /* Keep iterating over the string until a non-digit is found. */
  while (isdigit(*string)) {
    string++;
  }

  /* If the non-digit we found is the terminating \0, everything was digits. */
  return *string == '\0';
}

static bool is_char_sep(const char c)
{
  return ELEM(c, '.', ' ', '-', '_');
}

void BLI_string_split_suffix(const char *string,
                             const size_t string_maxlen,
                             char *r_body,
                             char *r_suf)
{
  BLI_string_debug_size(r_body, string_maxlen);
  BLI_string_debug_size(r_suf, string_maxlen);

  size_t len = BLI_strnlen(string, string_maxlen);
  size_t i;

  r_body[0] = r_suf[0] = '\0';

  for (i = len; i > 0; i--) {
    if (is_char_sep(string[i])) {
      BLI_strncpy(r_body, string, i + 1);
      BLI_strncpy(r_suf, string + i, (len + 1) - i);
      return;
    }
  }

  memcpy(r_body, string, len + 1);
}

void BLI_string_split_prefix(const char *string,
                             const size_t string_maxlen,
                             char *r_pre,
                             char *r_body)
{
  BLI_string_debug_size(r_pre, string_maxlen);
  BLI_string_debug_size(r_body, string_maxlen);

  size_t len = BLI_strnlen(string, string_maxlen);
  size_t i;

  r_body[0] = r_pre[0] = '\0';

  for (i = 1; i < len; i++) {
    if (is_char_sep(string[i])) {
      i++;
      BLI_strncpy(r_pre, string, i + 1);
      BLI_strncpy(r_body, string + i, (len + 1) - i);
      return;
    }
  }

  BLI_strncpy(r_body, string, len);
}

size_t BLI_string_flip_side_name(char *name_dst,
                                 const char *name_src,
                                 const bool strip_number,
                                 const size_t name_dst_maxncpy)
{
  BLI_string_debug_size(name_dst, name_dst_maxncpy);

  size_t len;
  char *prefix = static_cast<char *>(alloca(name_dst_maxncpy)); /* The part before the facing */
  char *suffix = static_cast<char *>(alloca(name_dst_maxncpy)); /* The part after the facing */
  char *number = static_cast<char *>(alloca(name_dst_maxncpy)); /* The number extension string */
  const char *replace = nullptr;
  char *index = nullptr;
  bool is_set = false;

  *prefix = *suffix = *number = '\0';

  /* always copy the name, since this can be called with an uninitialized string */
  len = BLI_strncpy_utf8_rlen(name_dst, name_src, name_dst_maxncpy);
  if (len < 3) {
    /* We don't support names such as `.R` or `.L`. */
    return len;
  }

  /* We first check the case with a .### extension, let's find the last period */
  if (isdigit(name_dst[len - 1])) {
    index = strrchr(name_dst, '.');   /* Last occurrence. */
    if (index && isdigit(index[1])) { /* Doesn't handle case `bone.1abc2` correct..., whatever! */
      if (strip_number == false) {
        BLI_strncpy_utf8(number, index, name_dst_maxncpy);
      }
      *index = '\0';
      len = size_t(index - name_dst); /* Same as `strlen(name_dst)`. */
    }
  }

  BLI_strncpy_utf8(prefix, name_dst, name_dst_maxncpy);

  /* First case; separator (`.` or `_`) with extensions in `r R l L`. */
  if ((len > 1) && is_char_sep(name_dst[len - 2])) {
    is_set = true;
    switch (name_dst[len - 1]) {
      case 'l':
        prefix[len - 1] = 0;
        replace = "r";
        break;
      case 'r':
        prefix[len - 1] = 0;
        replace = "l";
        break;
      case 'L':
        prefix[len - 1] = 0;
        replace = "R";
        break;
      case 'R':
        prefix[len - 1] = 0;
        replace = "L";
        break;
      default:
        is_set = false;
    }
  }

  /* case; beginning with r R l L, with separator after it */
  if (!is_set && is_char_sep(name_dst[1])) {
    is_set = true;
    switch (name_dst[0]) {
      case 'l':
        replace = "r";
        BLI_strncpy_utf8(suffix, name_dst + 1, name_dst_maxncpy);
        prefix[0] = 0;
        break;
      case 'r':
        replace = "l";
        BLI_strncpy_utf8(suffix, name_dst + 1, name_dst_maxncpy);
        prefix[0] = 0;
        break;
      case 'L':
        replace = "R";
        BLI_strncpy_utf8(suffix, name_dst + 1, name_dst_maxncpy);
        prefix[0] = 0;
        break;
      case 'R':
        replace = "L";
        BLI_strncpy_utf8(suffix, name_dst + 1, name_dst_maxncpy);
        prefix[0] = 0;
        break;
      default:
        is_set = false;
    }
  }

  if (!is_set && len > 5) {
    /* Test for a separator to apply the rule: ultimate left or right. */
    if (((index = BLI_strcasestr(prefix, "right")) == prefix) || (index == prefix + len - 5)) {
      is_set = true;
      if (index[0] == 'r') {
        replace = "left";
      }
      else {
        replace = (index[1] == 'I' ? "LEFT" : "Left");
      }
      *index = 0;
      BLI_strncpy_utf8(suffix, index + 5, name_dst_maxncpy);
    }
    else if (((index = BLI_strcasestr(prefix, "left")) == prefix) || (index == prefix + len - 4)) {
      is_set = true;
      if (index[0] == 'l') {
        replace = "right";
      }
      else {
        replace = (index[1] == 'E' ? "RIGHT" : "Right");
      }
      *index = 0;
      BLI_strncpy_utf8(suffix, index + 4, name_dst_maxncpy);
    }
  }

  return BLI_snprintf_utf8_rlen(
      name_dst, name_dst_maxncpy, "%s%s%s%s", prefix, replace ? replace : "", suffix, number);
}

/* Unique name utils. */

void BLI_uniquename_cb(blender::FunctionRef<bool(blender::StringRefNull)> unique_check,
                       const char *defname,
                       char delim,
                       char *name,
                       size_t name_maxncpy)
{
  BLI_string_debug_size_after_nil(name, name_maxncpy);

  if (name[0] == '\0') {
    BLI_strncpy_utf8(name, defname, name_maxncpy);
  }

  if (unique_check(name)) {
    char numstr[16];
    char *tempname = static_cast<char *>(alloca(name_maxncpy));
    char *left = static_cast<char *>(alloca(name_maxncpy));
    int number;
    size_t len = BLI_string_split_name_number(name, delim, left, &number);
    do {
      const size_t numlen = SNPRINTF_UTF8(numstr, "%c%03d", delim, ++number);

      /* highly unlikely the string only has enough room for the number
       * but support anyway */
      if (UNLIKELY((len == 0) || (numlen + 1 >= name_maxncpy))) {
        /* Number is known not to be UTF8. */
        BLI_strncpy(tempname, numstr, name_maxncpy);
      }
      else {
        char *tempname_buf;
        tempname_buf = tempname + BLI_strncpy_utf8_rlen(tempname, left, name_maxncpy - numlen);
        memcpy(tempname_buf, numstr, numlen + 1);
      }
    } while (unique_check(tempname));
    /* There will always be enough room for this string. */
    BLI_strncpy_utf8(name, tempname, name_maxncpy);
  }
}

std::string BLI_uniquename_cb(blender::FunctionRef<bool(blender::StringRef)> unique_check,
                              const char delim,
                              const blender::StringRef name)
{
  std::string new_name = name;

  if (!unique_check(new_name)) {
    return new_name;
  }

  int number;
  blender::Array<char> left_buffer(int64_t(new_name.size()) + 1);
  const size_t len = BLI_string_split_name_number(
      new_name.c_str(), delim, left_buffer.data(), &number);

  const std::string left = left_buffer.data();

  do {
    std::array<char, 16> num_str;
    BLI_snprintf(num_str.data(), num_str.size(), "%c%03d", delim, ++number);

    if (len == 0) {
      new_name = num_str.data();
    }
    else {
      new_name = left + num_str.data();
    }
  } while (unique_check(new_name));

  return new_name;
}

void BLI_uniquename(const ListBase *list,
                    void *vlink,
                    const char *defname,
                    char delim,
                    int name_offset,
                    size_t name_maxncpy)
{
  BLI_assert(name_maxncpy > 1);

  /* See if we are given an empty string */
  if (ELEM(nullptr, vlink)) {
    return;
  }

  BLI_uniquename_cb(
      [&](const blender::StringRefNull name) {
        LISTBASE_FOREACH (Link *, link, list) {
          if (link != vlink) {
            const char *link_name = POINTER_OFFSET((const char *)link, name_offset);
            if (name == link_name) {
              return true;
            }
          }
        }
        return false;
      },
      defname,
      delim,
      static_cast<char *>(POINTER_OFFSET(vlink, name_offset)),
      name_maxncpy);
}

size_t BLI_string_len_array(const char *strings[], uint strings_num)
{
  size_t total_len = 0;
  for (uint i = 0; i < strings_num; i++) {
    total_len += strlen(strings[i]);
  }
  return total_len;
}

/* ------------------------------------------------------------------------- */
/** \name Join Strings
 *
 * For non array versions of these functions, use the macros:
 * - #BLI_string_join
 * - #BLI_string_joinN
 * - #BLI_string_join_by_sep_charN
 * - #BLI_string_join_by_sep_char_with_tableN
 *
 * \{ */

size_t BLI_string_join_array(char *result,
                             size_t result_maxncpy,
                             const char *strings[],
                             uint strings_num)
{
  BLI_string_debug_size(result, result_maxncpy);

  char *c = result;
  char *c_end = &result[result_maxncpy - 1];
  for (uint i = 0; i < strings_num; i++) {
    const char *p = strings[i];
    while (*p) {
      if (UNLIKELY(!(c < c_end))) {
        i = strings_num; /* Break outer loop. */
        break;
      }
      *c++ = *p++;
    }
  }
  *c = '\0';
  return size_t(c - result);
}

size_t BLI_string_join_array_by_sep_char(
    char *result, size_t result_maxncpy, char sep, const char *strings[], uint strings_num)
{
  BLI_string_debug_size(result, result_maxncpy);

  char *c = result;
  char *c_end = &result[result_maxncpy - 1];
  for (uint i = 0; i < strings_num; i++) {
    if (i != 0) {
      if (UNLIKELY(!(c < c_end))) {
        break;
      }
      *c++ = sep;
    }
    const char *p = strings[i];
    while (*p) {
      if (UNLIKELY(!(c < c_end))) {
        i = strings_num; /* Break outer loop. */
        break;
      }
      *c++ = *p++;
    }
  }
  *c = '\0';
  return size_t(c - result);
}

char *BLI_string_join_arrayN(const char *strings[], uint strings_num)
{
  const size_t result_size = BLI_string_len_array(strings, strings_num) + 1;
  char *result = MEM_calloc_arrayN<char>(result_size, __func__);
  char *c = result;
  for (uint i = 0; i < strings_num; i++) {
    const size_t string_len = strlen(strings[i]);
    memcpy(c, strings[i], string_len);
    c += string_len;
  }
  /* Only needed when `strings_num == 0`. */
  *c = '\0';
  BLI_assert(result + result_size == c + 1);
  return result;
}

char *BLI_string_join_array_by_sep_charN(char sep, const char *strings[], uint strings_num)
{
  const size_t result_size = BLI_string_len_array(strings, strings_num) +
                             (strings_num ? strings_num - 1 : 0) + 1;
  char *result = MEM_calloc_arrayN<char>(result_size, __func__);
  char *c = result;
  if (strings_num != 0) {
    for (uint i = 0; i < strings_num; i++) {
      const size_t string_len = strlen(strings[i]);
      memcpy(c, strings[i], string_len);
      c += string_len;
      *c = sep;
      c++;
    }
    c--;
  }
  *c = '\0';
  BLI_assert(result + result_size == c + 1);
  return result;
}

char *BLI_string_join_array_by_sep_char_with_tableN(char sep,
                                                    char *table[],
                                                    const char *strings[],
                                                    uint strings_num)
{
  size_t result_size = 0;
  for (uint i = 0; i < strings_num; i++) {
    result_size += strlen(strings[i]) + 1;
  }
  if (result_size == 0) {
    result_size = 1;
  }

  char *result = MEM_calloc_arrayN<char>(result_size, __func__);
  char *c = result;
  if (strings_num != 0) {
    for (uint i = 0; i < strings_num; i++) {
      const size_t string_len = strlen(strings[i]);
      memcpy(c, strings[i], string_len);
      table[i] = c; /* <-- only difference to BLI_string_join_array_by_sep_charN. */
      memcpy(c, strings[i], string_len);
      c += string_len;
      *c = sep;
      c++;
    }
    c--;
  }
  *c = '\0';
  BLI_assert(result + result_size == c + 1);
  return result;
}

/** \} */
