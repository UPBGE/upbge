
/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/space_text/text_format_glsl.c
 *  \ingroup sptext
 */

#include <cstring>

#include "BLI_blenlib.h"

#include "DNA_space_types.h"
#include "DNA_text_types.h"

#include "BKE_text.h"

#include "text_format.hh"

/* *** glsl Keywords (for format_line) *** */

static const char *text_format_glsl_literals_keyword_data[]{
    /* Force single column, sorted list. */
    /* clang-format off */
    "attribute",
    "break",
    "bvec2",
    "bvec3",
    "bvec4",
    "case",
    "centroid",
    "const",
    "continue",
    "default",
    "discard",
    "dmat2",
    "dmat3",
    "dmat4",
    "do",
    "double",
    "dvec2",
    "dvec3",
    "dvec4",
    "else",
    "flat",
    "float",
    "for",
    "goto",
    "highp",
    "if",
    "in",
    "inout",
    "int",
    "invariant",
    "ivec2",
    "ivec3",
    "ivec4",
    "layout",
    "location",
    "lowp",
    "mat2",
    "mat3",
    "mat4",
    "mediump",
    "out",
    "patch",
    "precision",
    "return",
    "sample",
    "sampler1D",
    "sampler2D",
    "sampler3D",
    "samplerCube",
    "sizeof",
    "smooth",
    "struct",
    "subroutine",
    "switch",
    "uint",
    "uniform",
    "uvec2",
    "uvec3",
    "uvec4",
    "varying",
    "vec2",
    "vec3",
    "vec4",
    "void",
    "while",
    /* clang-format on */
};

static const Span<const char *> text_format_glsl_literals_keyword(
    text_format_glsl_literals_keyword_data,
    ARRAY_SIZE(text_format_glsl_literals_keyword_data));

static const char *text_format_glsl_literals_reserved_data[]{
"abs",
"acos",
"all",
"any",
"asin",
"atan",
"atan2",
"ceil",
"clamp",
"cos",
"cross",
"degrees",
"distance",
"dot",
"exp",
"exp2",
"faceforward",
"floor",
"fract",
"inversesqrt",
"length",
"log",
"log2",
"max",
"min",
"mix",
"mod",
"normalize",
"not",
"pow",
"radians",
"reflect",
"refract",
"round",
"sign",
"sin",
"smoothstep",
"sqrt",
"step",
"tan",
"texture",
"texture1D",
"texture1DLod",
"texture2D",
"texture2DLod",
"texture3D",
"texture3DLod",
"textureCube",
"textureCubeLod",
"trunc",
};

static const Span<const char *> text_format_glsl_literals_reserved(
    text_format_glsl_literals_reserved_data, ARRAY_SIZE(text_format_glsl_literals_reserved_data));


static const char *text_format_glsl_literals_specialvar_data[]{
    /* Force single column, sorted list. */
    /* clang-format off */
    "ftransform",
    "getmetatable",
    "gl_BackColor",
    "gl_BackMaterial",
    "gl_ClipDistance",
    "gl_ClipPlane",
    "gl_ClipVertex",
    "gl_Color",
    "gl_Fog",
    "gl_FogCoord",
    "gl_FogFragCoord",
    "gl_FragColor",
    "gl_FragCoord",
    "gl_FragData",
    "gl_FragDepth",
    "gl_FrontColor",
    "gl_FrontFacing",
    "gl_FrontMaterial",
    "gl_InstanceID;",
    "gl_Layer",
    "gl_LightModel",
    "gl_LightSource",
    "gl_MaxDrawBuffers",
    "gl_MaxLights",
    "gl_MaxTextureCoords",
    "gl_ModelViewMatrix",
    "gl_ModelViewMatrixInverse",
    "gl_ModelViewMatrixTranspose",
    "gl_ModelViewProjectionMatrix",
    "gl_ModelViewProjectionMatrixInverse",
    "gl_ModelViewProjectionMatrixTranspose",
    "gl_MultiTexCoord0",
    "gl_MultiTexCoord1",
    "gl_MultiTexCoord2",
    "gl_MultiTexCoord3",
    "gl_MultiTexCoord4",
    "gl_MultiTexCoord5",
    "gl_MultiTexCoord6",
    "gl_MultiTexCoord7",
    "gl_Normal",
    "gl_NormalMatrix",
    "gl_NormalScale",
    "gl_PerVertex",
    "gl_Point",
    "gl_PointCoord",
    "gl_PointSize;",
    "gl_Position;",
    "gl_PrimitiveID",
    "gl_ProjectionMatrix",
    "gl_ProjectionMatrixInverse",
    "gl_ProjectionMatrixTranspose",
    "gl_SampleID",
    "gl_SampleMask",
    "gl_SamplePosition",
    "gl_TessLevelInner",
    "gl_TessLevelOuter",
    "gl_TexCoord",
    "gl_TextureMatrix",
    "gl_VertexID;",
    "gl_ViewportIndex",
    /* clang-format on */
};

static const Span<const char *> text_format_glsl_literals_specialvar(
    text_format_glsl_literals_specialvar_data,
    ARRAY_SIZE(text_format_glsl_literals_specialvar_data));

static const char *text_format_glsl_literals_bool_data[]{
    /* Force single column, sorted list. */
    /* clang-format off */
    "false",
    "null",
    "true",
    /* clang-format on */
};

static const Span<const char *> text_format_glsl_literals_bool(
    text_format_glsl_literals_bool_data,
    ARRAY_SIZE(text_format_glsl_literals_bool_data));

static int txtfmt_glsl_find_keyword(const char *string)
{
  const int i = text_format_string_literal_find(text_format_glsl_literals_keyword, string);

  /* If next source char is an identifier (eg. 'i' in "definite") no match */
  if (i == 0 || text_check_identifier(string[i])) {
    return -1;
  }
  return i;
}

static int txtfmt_glsl_find_reserved(const char *string)
{
  const int i = text_format_string_literal_find(text_format_glsl_literals_reserved, string);

  /* If next source char is an identifier (eg. 'i' in "definite") no match */
  if (i == 0 || text_check_identifier(string[i])) {
    return -1;
  }
  return i;
}

static int txtfmt_glsl_find_specialvar(const char *string)
{
  const int i = text_format_string_literal_find(text_format_glsl_literals_specialvar, string);

  /* If next source char is an identifier (eg. 'i' in "definite") no match */
  if (i == 0 || text_check_identifier(string[i])) {
    return -1;
  }
  return i;
}

static int txtfmt_glsl_find_bool(const char *string)
{
  const int i = text_format_string_literal_find(text_format_glsl_literals_bool, string);

  /* If next source char is an identifier (eg. 'i' in "definite") no match */
  if (i == 0 || text_check_identifier(string[i])) {
    return -1;
  }
  return i;
}

static char txtfmt_glsl_format_identifier(const char *str)
{
  char fmt;
  if ((txtfmt_glsl_find_specialvar(str)) != -1)
    fmt = FMT_TYPE_SPECIAL;
  else if ((txtfmt_glsl_find_keyword(str)) != -1)
    fmt = FMT_TYPE_KEYWORD;
  else if ((txtfmt_glsl_find_reserved(str)) != -1)
    fmt = FMT_TYPE_RESERVED;
  else
    fmt = FMT_TYPE_DEFAULT;
  return fmt;
}

static void txtfmt_glsl_format_line(SpaceText *st, TextLine *line, const bool do_next)
{
  FlattenString fs;
  const char *str;
  char *fmt;
  char cont_orig, cont, find, prev = ' ';
  int len, i;

  /* Get continuation from previous line */
  if (line->prev && line->prev->format != NULL) {
    fmt = line->prev->format;
    cont = fmt[strlen(fmt) + 1]; /* Just after the null-terminator */
    BLI_assert((FMT_CONT_ALL & cont) == cont);
  }
  else {
    cont = FMT_CONT_NOP;
  }

  /* Get original continuation from this line */
  if (line->format != NULL) {
    fmt = line->format;
    cont_orig = fmt[strlen(fmt) + 1]; /* Just after the null-terminator */
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
    /* Handle escape sequences by skipping both \ and next char */
    if (*str == '\\') {
      *fmt = prev;
      fmt++;
      str++;
      if (*str == '\0')
        break;
      *fmt = prev;
      fmt++;
      str += BLI_str_utf8_size_safe(str);
      continue;
    }
    /* Handle continuations */
    if (cont) {
      /* Multi-line comments */
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
        /* Handle other comments */
      }
      else {
        find = (cont & FMT_CONT_QUOTEDOUBLE) ? '"' : '\'';
        if (*str == find)
          cont = 0;
        *fmt = FMT_TYPE_STRING;
      }

      str += BLI_str_utf8_size_safe(str) - 1;
    }
    /* Not in a string... */
    else {
      /* Multi-line comments */
      if (*str == '/' && *(str + 1) == '*') {
        cont = FMT_CONT_COMMENT_C;
        *fmt = FMT_TYPE_COMMENT;
        fmt++;
        str++;
        *fmt = FMT_TYPE_COMMENT;
        fmt++;
        str++;
        *fmt = FMT_TYPE_COMMENT;
        fmt++;
        str++;
        *fmt = FMT_TYPE_COMMENT;
      }
      /* Single line comment */
      else if (*str == '/' && *(str + 1) == '/') {
        text_format_fill(&str, &fmt, FMT_TYPE_COMMENT, len - (int)(fmt - line->format));
      }
      else if (*str == '"' || *str == '\'') {
        /* Strings */
        find = *str;
        cont = (*str == '"') ? FMT_CONT_QUOTEDOUBLE : FMT_CONT_QUOTESINGLE;
        *fmt = FMT_TYPE_STRING;
      }
      /* Whitespace (all ws. has been converted to spaces) */
      else if (*str == ' ') {
        *fmt = FMT_TYPE_WHITESPACE;
      }
      /* Numbers (digits not part of an identifier and periods followed by digits) */
      else if ((prev != FMT_TYPE_DEFAULT && text_check_digit(*str)) ||
               (*str == '.' && text_check_digit(*(str + 1)))) {
        *fmt = FMT_TYPE_NUMERAL;
      }
      /* Booleans */
      else if (prev != FMT_TYPE_DEFAULT && (i = txtfmt_glsl_find_bool(str)) != -1) {
        if (i > 0) {
          text_format_fill_ascii(&str, &fmt, FMT_TYPE_NUMERAL, i);
        }
        else {
          str += BLI_str_utf8_size_safe(str) - 1;
          *fmt = FMT_TYPE_DEFAULT;
        }
      }
      /* Punctuation */
      else if ((*str != '#') && text_check_delim(*str)) {
        *fmt = FMT_TYPE_SYMBOL;
      }

      /* Preprocessor */
      else if ((*str == '#')) {
        text_format_fill(&str, &fmt, FMT_TYPE_DIRECTIVE, len - (int)(fmt - line->format));
      }

      /* Identifiers and other text (no previous ws. or delims. so text continues) */
      else if (prev == FMT_TYPE_DEFAULT) {
        str += BLI_str_utf8_size_safe(str) - 1;
        *fmt = FMT_TYPE_DEFAULT;
      }
      /* Not ws, a digit, punct, or continuing text. Must be new, check for special words */
      else {
        /* Special vars(v) or built-in keywords(b) */
        /* keep in sync with 'txtfmt_osl_format_identifier()' */
        if ((i = txtfmt_glsl_find_specialvar(str)) != -1)
          prev = FMT_TYPE_SPECIAL;
        else if ((i = txtfmt_glsl_find_keyword(str)) != -1)
          prev = FMT_TYPE_KEYWORD;
        else if ((i = txtfmt_glsl_find_reserved(str)) != -1)
          prev = FMT_TYPE_RESERVED;

        if (i > 0) {
          text_format_fill_ascii(&str, &fmt, prev, i);
        }
        else {
          str += BLI_str_utf8_size_safe(str) - 1;
          *fmt = FMT_TYPE_DEFAULT;
        }
      }
    }
    prev = *fmt;
    fmt++;
    str++;
  }

  /* Terminate and add continuation char */
  *fmt = '\0';
  fmt++;
  *fmt = cont;

  /* If continuation has changed and we're allowed, process the next line */
  if (cont != cont_orig && do_next && line->next) {
    txtfmt_glsl_format_line(st, line->next, do_next);
  }

  flatten_string_free(&fs);
}

void ED_text_format_register_glsl(void)
{
  static TextFormatType tft = {NULL};
  static const char *ext[] = {"glsl", "frag", "vert", "fx", "fs", "vs", NULL};

  tft.format_identifier = txtfmt_glsl_format_identifier;
  tft.format_line = txtfmt_glsl_format_line;
  tft.ext = ext;

  ED_text_format_register(&tft);

  BLI_assert(text_format_string_literals_check_sorted_array(text_format_glsl_literals_keyword));
  BLI_assert(text_format_string_literals_check_sorted_array(text_format_glsl_literals_reserved));
  BLI_assert(text_format_string_literals_check_sorted_array(text_format_glsl_literals_specialvar));
  BLI_assert(text_format_string_literals_check_sorted_array(text_format_glsl_literals_bool));
}
