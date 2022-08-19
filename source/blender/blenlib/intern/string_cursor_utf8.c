/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bli
 */

#include <stdio.h>
#include <stdlib.h>

#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BLI_string_cursor_utf8.h" /* own include */

#ifdef __GNUC__
#  pragma GCC diagnostic error "-Wsign-conversion"
#endif

typedef enum eStrCursorDelimType {
  STRCUR_DELIM_NONE,
  STRCUR_DELIM_ALPHANUMERIC,
  STRCUR_DELIM_PUNCT,
  STRCUR_DELIM_BRACE,
  STRCUR_DELIM_OPERATOR,
  STRCUR_DELIM_QUOTE,
  STRCUR_DELIM_WHITESPACE,
  STRCUR_DELIM_OTHER,
} eStrCursorDelimType;

static eStrCursorDelimType cursor_delim_type_unicode(const uint uch)
{
  switch (uch) {
    case ',':
    case '.':
      return STRCUR_DELIM_PUNCT;

    case '{':
    case '}':
    case '[':
    case ']':
    case '(':
    case ')':
      return STRCUR_DELIM_BRACE;

    case '+':
    case '-':
    case '=':
    case '~':
    case '%':
    case '/':
    case '<':
    case '>':
    case '^':
    case '*':
    case '&':
    case '|':
      return STRCUR_DELIM_OPERATOR;

    case '\'':
    case '\"':
      return STRCUR_DELIM_QUOTE;

    case ' ':
    case '\t':
    case '\n':
      return STRCUR_DELIM_WHITESPACE;

    case '\\':
    case '@':
    case '#':
    case '$':
    case ':':
    case ';':
    case '?':
    case '!':
    case 0xA3:        /* pound */
    case 0x80:        /* euro */
      /* case '_': */ /* special case, for python */
      return STRCUR_DELIM_OTHER;

    default:
      break;
  }
  return STRCUR_DELIM_ALPHANUMERIC; /* Not quite true, but ok for now */
}

static eStrCursorDelimType cursor_delim_type_utf8(const char *ch_utf8,
                                                  const size_t ch_utf8_len,
                                                  const int pos)
{
  /* for full unicode support we really need to have large lookup tables to figure
   * out what's what in every possible char set - and python, glib both have these. */
  size_t index = (size_t)pos;
  uint uch = BLI_str_utf8_as_unicode_step_or_error(ch_utf8, ch_utf8_len, &index);
  return cursor_delim_type_unicode(uch);
}

bool BLI_str_cursor_step_next_utf8(const char *str, size_t maxlen, int *pos)
{
  const char *str_end = str + (maxlen + 1);
  const char *str_pos = str + (*pos);
  const char *str_next = BLI_str_find_next_char_utf8(str_pos, str_end);
  if (str_next != str_end) {
    (*pos) += (str_next - str_pos);
    if ((*pos) > (int)maxlen) {
      (*pos) = (int)maxlen;
    }
    return true;
  }

  return false;
}

bool BLI_str_cursor_step_prev_utf8(const char *str, size_t UNUSED(maxlen), int *pos)
{
  if ((*pos) > 0) {
    const char *str_pos = str + (*pos);
    const char *str_prev = BLI_str_find_prev_char_utf8(str_pos, str);
    (*pos) -= (str_pos - str_prev);
    return true;
  }

  return false;
}

void BLI_str_cursor_step_utf8(const char *str,
                              size_t maxlen,
                              int *pos,
                              eStrCursorJumpDirection direction,
                              eStrCursorJumpType jump,
                              bool use_init_step)
{
  const int pos_orig = *pos;

  if (direction == STRCUR_DIR_NEXT) {
    if (use_init_step) {
      BLI_str_cursor_step_next_utf8(str, maxlen, pos);
    }
    else {
      BLI_assert(jump == STRCUR_JUMP_DELIM);
    }

    if (jump != STRCUR_JUMP_NONE) {
      const eStrCursorDelimType delim_type = (*pos) < maxlen ?
                                                 cursor_delim_type_utf8(str, maxlen, *pos) :
                                                 STRCUR_DELIM_NONE;
      /* jump between special characters (/,\,_,-, etc.),
       * look at function cursor_delim_type() for complete
       * list of special character, ctr -> */
      while ((*pos) < maxlen) {
        if (BLI_str_cursor_step_next_utf8(str, maxlen, pos)) {
          if (*pos == maxlen) {
            break;
          }
          if ((jump != STRCUR_JUMP_ALL) &&
              (delim_type != cursor_delim_type_utf8(str, maxlen, *pos))) {
            break;
          }
        }
        else {
          break; /* unlikely but just in case */
        }
      }
    }
  }
  else if (direction == STRCUR_DIR_PREV) {
    if (use_init_step) {
      BLI_str_cursor_step_prev_utf8(str, maxlen, pos);
    }
    else {
      BLI_assert(jump == STRCUR_JUMP_DELIM);
    }

    if (jump != STRCUR_JUMP_NONE) {
      const eStrCursorDelimType delim_type = (*pos) > 0 ?
                                                 cursor_delim_type_utf8(str, maxlen, *pos - 1) :
                                                 STRCUR_DELIM_NONE;
      /* jump between special characters (/,\,_,-, etc.),
       * look at function cursor_delim_type() for complete
       * list of special character, ctr -> */
      while ((*pos) > 0) {
        const int pos_prev = *pos;
        if (BLI_str_cursor_step_prev_utf8(str, maxlen, pos)) {
          if ((jump != STRCUR_JUMP_ALL) &&
              (delim_type != cursor_delim_type_utf8(str, maxlen, *pos))) {
            /* left only: compensate for index/change in direction */
            if ((pos_orig - (*pos)) >= 1) {
              *pos = pos_prev;
            }
            break;
          }
        }
        else {
          break;
        }
      }
    }
  }
  else {
    BLI_assert_unreachable();
  }
}

/* UTF32 version of BLI_str_cursor_step_utf8 (keep in sync!)
 * less complex since it doesn't need to do multi-byte stepping.
 */

/* helper funcs so we can match BLI_str_cursor_step_utf8 */
static bool cursor_step_next_utf32(const char32_t *UNUSED(str), size_t maxlen, int *pos)
{
  if ((*pos) >= (int)maxlen) {
    return false;
  }
  (*pos)++;
  return true;
}

static bool cursor_step_prev_utf32(const char32_t *UNUSED(str), size_t UNUSED(maxlen), int *pos)
{
  if ((*pos) <= 0) {
    return false;
  }
  (*pos)--;
  return true;
}

void BLI_str_cursor_step_utf32(const char32_t *str,
                               size_t maxlen,
                               int *pos,
                               eStrCursorJumpDirection direction,
                               eStrCursorJumpType jump,
                               bool use_init_step)
{
  const int pos_orig = *pos;

  if (direction == STRCUR_DIR_NEXT) {
    if (use_init_step) {
      cursor_step_next_utf32(str, maxlen, pos);
    }
    else {
      BLI_assert(jump == STRCUR_JUMP_DELIM);
    }

    if (jump != STRCUR_JUMP_NONE) {
      const eStrCursorDelimType delim_type = (*pos) < maxlen ?
                                                 cursor_delim_type_unicode((uint)str[*pos]) :
                                                 STRCUR_DELIM_NONE;
      /* jump between special characters (/,\,_,-, etc.),
       * look at function cursor_delim_type_unicode() for complete
       * list of special character, ctr -> */
      while ((*pos) < maxlen) {
        if (cursor_step_next_utf32(str, maxlen, pos)) {
          if ((jump != STRCUR_JUMP_ALL) &&
              (delim_type != cursor_delim_type_unicode((uint)str[*pos]))) {
            break;
          }
        }
        else {
          break; /* unlikely but just in case */
        }
      }
    }
  }
  else if (direction == STRCUR_DIR_PREV) {
    if (use_init_step) {
      cursor_step_prev_utf32(str, maxlen, pos);
    }
    else {
      BLI_assert(jump == STRCUR_JUMP_DELIM);
    }

    if (jump != STRCUR_JUMP_NONE) {
      const eStrCursorDelimType delim_type = (*pos) > 0 ?
                                                 cursor_delim_type_unicode((uint)str[(*pos) - 1]) :
                                                 STRCUR_DELIM_NONE;
      /* jump between special characters (/,\,_,-, etc.),
       * look at function cursor_delim_type() for complete
       * list of special character, ctr -> */
      while ((*pos) > 0) {
        const int pos_prev = *pos;
        if (cursor_step_prev_utf32(str, maxlen, pos)) {
          if ((jump != STRCUR_JUMP_ALL) &&
              (delim_type != cursor_delim_type_unicode((uint)str[*pos]))) {
            /* left only: compensate for index/change in direction */
            if ((pos_orig - (*pos)) >= 1) {
              *pos = pos_prev;
            }
            break;
          }
        }
        else {
          break;
        }
      }
    }
  }
  else {
    BLI_assert_unreachable();
  }
}
