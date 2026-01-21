/* SPDX-FileCopyrightText: 2024 UPBGE Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sptext
 *
 * Syntax highlighting for JavaScript and TypeScript.
 * Supports: // line comments, C-style block comments, " and ' strings, keywords and literals.
 * Template literals (backticks) are not highlighted as strings.
 */

#include <cstring>

#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "DNA_space_types.h"
#include "DNA_text_types.h"

#include "BKE_text.h"

#include "text_format.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Local Literal Definitions
 * \{ */

/**
 * JavaScript/TypeScript keywords and reserved words.
 * Sorted for binary search in text_format_string_literal_find.
 */
static const char *text_format_js_literals_keyword_data[] = {
    /* clang-format off */
    "as",
    "async",
    "await",
    "break",
    "case",
    "catch",
    "class",
    "const",
    "continue",
    "debugger",
    "default",
    "delete",
    "do",
    "else",
    "export",
    "extends",
    "finally",
    "for",
    "from",
    "function",
    "get",
    "if",
    "implements",
    "import",
    "in",
    "instanceof",
    "interface",
    "let",
    "new",
    "of",
    "package",
    "private",
    "protected",
    "public",
    "return",
    "set",
    "static",
    "super",
    "switch",
    "this",
    "throw",
    "try",
    "type",
    "typeof",
    "var",
    "void",
    "while",
    "with",
    "yield",
    /* clang-format on */
};
static const Span<const char *> text_format_js_literals_keyword(
    text_format_js_literals_keyword_data, ARRAY_SIZE(text_format_js_literals_keyword_data));

/** JS/TS literals: true, false, null, undefined. */
static const char *text_format_js_literals_value_data[] = {
    /* clang-format off */
    "false",
    "null",
    "true",
    "undefined",
    /* clang-format on */
};
static const Span<const char *> text_format_js_literals_value(
    text_format_js_literals_value_data, ARRAY_SIZE(text_format_js_literals_value_data));

/** JS/TS primitive types: string, number, boolean, void, etc. */
static const char *text_format_js_literals_type_data[] = {
    /* clang-format off */
    "any",
    "bigint",
    "boolean",
    "never",
    "number",
    "object",
    "string",
    "symbol",
    "undefined",
    "unknown",
    "void",
    /* clang-format on */
};
static const Span<const char *> text_format_js_literals_type(
    text_format_js_literals_type_data, ARRAY_SIZE(text_format_js_literals_type_data));

/** \} */

/* -------------------------------------------------------------------- */
/** \name Local Functions
 * \{ */

static int txtfmt_js_find_keyword(const char *string)
{
  const int i = text_format_string_literal_find(text_format_js_literals_keyword, string);
  if (i == 0 || text_check_identifier(string[i])) {
    return -1;
  }
  return i;
}

static int txtfmt_js_find_value(const char *string)
{
  const int i = text_format_string_literal_find(text_format_js_literals_value, string);
  if (i == 0 || text_check_identifier(string[i])) {
    return -1;
  }
  return i;
}

static int txtfmt_js_find_type(const char *string)
{
  const int i = text_format_string_literal_find(text_format_js_literals_type, string);
  if (i == 0 || text_check_identifier(string[i])) {
    return -1;
  }
  return i;
}

static char txtfmt_js_format_identifier(const char *str)
{
  if (txtfmt_js_find_keyword(str) != -1) {
    return FMT_TYPE_KEYWORD;
  }
  if (txtfmt_js_find_value(str) != -1) {
    return FMT_TYPE_NUMERAL; /* use numeral color for true/false/null/undefined */
  }
  if (txtfmt_js_find_type(str) != -1) {
    return FMT_TYPE_RESERVED; /* use reserved color (purple) for primitive types */
  }
  return FMT_TYPE_DEFAULT;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Format Line Implementation (#TextFormatType::format_line)
 * \{ */

static void txtfmt_js_format_line(SpaceText *st, TextLine *line, const bool do_next)
{
  FlattenString fs;
  const char *str;
  char *fmt;
  char cont_orig, cont, find, prev = ' ';
  int len, i;
  bool after_dot = false; /* Track if we're after a dot (property access) */
  bool expect_type_name = false; /* Track if we expect a type name (after interface/class/type) */

  /* Get continuation from previous line. */
  if (line->prev && line->prev->format != nullptr) {
    fmt = line->prev->format;
    cont = fmt[strlen(fmt) + 1];
    BLI_assert((FMT_CONT_ALL & cont) == cont);
  }
  else {
    cont = FMT_CONT_NOP;
  }

  /* Get original continuation from this line. */
  if (line->format != nullptr) {
    fmt = line->format;
    cont_orig = fmt[strlen(fmt) + 1];
    BLI_assert((FMT_CONT_ALL & cont_orig) == cont_orig);
  }
  else {
    cont_orig = 0xFF;
  }

  len = flatten_string(st, &fs, line->line);
  str = fs.buf;
  if (!text_check_format_len(line, len)) {
    flatten_string_free(&fs);
    return;
  }
  fmt = line->format;

  while (*str) {
    /* Escape sequences: skip \ and next char. */
    if (*str == '\\') {
      *fmt = prev;
      fmt++;
      str++;
      if (*str == '\0') {
        break;
      }
      *fmt = prev;
      fmt++;
      str += BLI_str_utf8_size_safe(str);
      continue;
    }
    /* Handle continuations (multi-line strings, block comments). */
    if (cont) {
      if (cont & FMT_CONT_COMMENT_C) {
        if (*str == '*' && *(str + 1) == '/') {
          *fmt = FMT_TYPE_COMMENT;
          fmt++;
          str++;
          *fmt = FMT_TYPE_COMMENT;
          cont = FMT_CONT_NOP;
        }
        else {
          *fmt = FMT_TYPE_COMMENT;
        }
      }
      else {
        find = (cont & FMT_CONT_QUOTEDOUBLE) ? '"' : '\'';
        if (*str == find) {
          cont = FMT_CONT_NOP;
        }
        *fmt = FMT_TYPE_STRING;
      }
      str += BLI_str_utf8_size_safe(str) - 1;
    }
    else {
      /* Line comment // */
      if (*str == '/' && *(str + 1) == '/') {
        text_format_fill(&str, &fmt, FMT_TYPE_COMMENT, len - int(fmt - line->format));
      }
      /* Block comment /* */
      else if (*str == '/' && *(str + 1) == '*') {
        cont = FMT_CONT_COMMENT_C;
        *fmt = FMT_TYPE_COMMENT;
        fmt++;
        str++;
        *fmt = FMT_TYPE_COMMENT;
      }
      else if (ELEM(*str, '"', '\'')) {
        find = *str;
        cont = (*str == '"') ? FMT_CONT_QUOTEDOUBLE : FMT_CONT_QUOTESINGLE;
        *fmt = FMT_TYPE_STRING;
      }
      else if (*str == ' ') {
        *fmt = FMT_TYPE_WHITESPACE;
      }
      else if ((prev != FMT_TYPE_DEFAULT && text_check_digit(*str)) ||
               (*str == '.' && text_check_digit(*(str + 1))))
      {
        *fmt = FMT_TYPE_NUMERAL;
      }
      else if (text_check_delim(*str)) {
        *fmt = FMT_TYPE_SYMBOL;
        /* Track if we're after a dot for property access */
        after_dot = (*str == '.');
      }
      else if (prev == FMT_TYPE_DEFAULT) {
        str += BLI_str_utf8_size_safe(str) - 1;
        *fmt = FMT_TYPE_DEFAULT;
        after_dot = false;
      }
      else {
        char ident_type = FMT_TYPE_DEFAULT;
        if ((i = txtfmt_js_find_value(str)) != -1) {
          ident_type = FMT_TYPE_NUMERAL;
        }
        else if ((i = txtfmt_js_find_keyword(str)) != -1) {
          ident_type = FMT_TYPE_KEYWORD;
          /* Check if this is 'interface', 'class', or 'type' - next identifier is a type name */
          if (i == 9 && strncmp(str, "interface", 9) == 0) {
            expect_type_name = true;
          }
          else if (i == 5 && strncmp(str, "class", 5) == 0) {
            expect_type_name = true;
          }
          else if (i == 4 && strncmp(str, "type", 4) == 0) {
            expect_type_name = true;
          }
          else {
            expect_type_name = false;
          }
        }
        else if ((i = txtfmt_js_find_type(str)) != -1) {
          ident_type = FMT_TYPE_RESERVED; /* Primitive types: purple */
        }
        else {
          i = -1;
          /* Check if this is a property access (after dot) */
          if (after_dot) {
            /* Property access: check if followed by '(' for function call */
            const char *next = str;
            int ident_len = 0;
            while (*next && text_check_identifier(*next)) {
              int char_size = BLI_str_utf8_size_safe(next);
              next += char_size;
              ident_len += char_size;
            }
            /* Skip whitespace */
            const char *after_ident = next;
            while (*after_ident == ' ' || *after_ident == '\t') {
              after_ident++;
            }
            if (*after_ident == '(') {
              ident_type = FMT_TYPE_SPECIAL; /* Function call: blue */
            }
            else {
              /* Property: use DEFAULT but will need special handling for red color */
              /* For now, use DEFAULT - we'll improve this later */
              ident_type = FMT_TYPE_DEFAULT;
            }
            i = ident_len;
          }
          else if (expect_type_name) {
            /* This is a type name after interface/class/type */
            ident_type = FMT_TYPE_DIRECTIVE; /* Type/interface name: yellow */
            i = 0;
            const char *ident_start = str;
            while (*ident_start && text_check_identifier(*ident_start)) {
              i += BLI_str_utf8_size_safe(ident_start);
              ident_start += BLI_str_utf8_size_safe(ident_start);
            }
            expect_type_name = false;
          }
        }
        if (i > 0) {
          prev = ident_type;
          text_format_fill_ascii(&str, &fmt, prev, i);
        }
        else {
          str += BLI_str_utf8_size_safe(str) - 1;
          *fmt = FMT_TYPE_DEFAULT;
        }
        after_dot = false;
      }
    }
    prev = *fmt;
    fmt++;
    str++;
  }

  *fmt = '\0';
  fmt++;
  *fmt = cont;

  if (cont != cont_orig && do_next && line->next) {
    txtfmt_js_format_line(st, line->next, do_next);
  }

  flatten_string_free(&fs);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void ED_text_format_register_js()
{
  static TextFormatType tft = {nullptr};
  static const char *ext[] = {"js", "mjs", "cjs", "ts", "mts", "cts", nullptr};

  tft.format_identifier = txtfmt_js_format_identifier;
  tft.format_line = txtfmt_js_format_line;
  tft.ext = ext;
  tft.comment_line = "//";

  ED_text_format_register(&tft);

#ifndef NDEBUG
  BLI_assert(text_format_string_literals_check_sorted_array(text_format_js_literals_keyword));
  BLI_assert(text_format_string_literals_check_sorted_array(text_format_js_literals_value));
  BLI_assert(text_format_string_literals_check_sorted_array(text_format_js_literals_type));
#endif
}

/** \} */

}  // namespace blender
