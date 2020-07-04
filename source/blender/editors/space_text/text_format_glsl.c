
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

#include <string.h>

#include "BLI_blenlib.h"

#include "DNA_space_types.h"
#include "DNA_text_types.h"

#include "BKE_text.h"

#include "text_format.h"

/* *** glsl Keywords (for format_line) *** */

/* Checks the specified source string for a glsl keyword (minus boolean & 'nil').
 * This name must start at the beginning of the source string and must be
 * followed by a non-identifier (see text_check_identifier(char)) or null char.
 *
 * If a keyword is found, the length of the matching word is returned.
 * Otherwise, -1 is returned.
 *
 */

static int txtfmt_glsl_find_keyword(const char *string)
{
  int i, len;

  if (STR_LITERAL_STARTSWITH(string, "if", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "else", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "void", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "float", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "int", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "uniform", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "varying", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "location", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "in", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "out", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "discard", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "return", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "vec2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "vec3", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "vec4", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "mat3", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "mat4", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "sampler2D", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "sampler1D", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "samplerCube", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "const", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "uint", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "uvec2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "uvec3", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "uvec4", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "for", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "while", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "continue", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "break", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "inout", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "attribute", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "layout", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "centroid", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "flat", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "smooth", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "patch", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "sample", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "do", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "switch", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "case", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "default", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "subroutine", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "double", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "invariant", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "dmat2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "dmat3", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "dmat4", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "mat2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "ivec2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "ivec3", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "ivec4", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "bvec2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "bvec3", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "bvec4", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "dvec2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "dvec3", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "dvec4", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "precision", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "lowp", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "highp", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "mediump", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "sampler3D", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "struct", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "goto", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "sizeof", len))
    i = len;
  else
    i = 0;

  /* If next source char is an identifier (eg. 'i' in "definate") no match */
  if (i == 0 || text_check_identifier(string[i]))
    return -1;
  return i;
}

/* Checks the specified source string for a glsl special name/function. This
 * name must start at the beginning of the source string and must be followed
 * by a non-identifier (see text_check_identifier(char)) or null character.
 *
 * If a special name is found, the length of the matching name is returned.
 * Otherwise, -1 is returned.
 *
 */

static int txtfmt_glsl_find_specialvar(const char *string)
{
  int i, len;

  if (STR_LITERAL_STARTSWITH(string, "gl_Position;", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_VertexID;", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_InstanceID;", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_PointSize;", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ClipDistance", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_PerVertex", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "getmetatable", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ViewportIndex", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_Layer", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ViewportIndex", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_TessLevelOuter", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_TessLevelInner", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_FragCoord", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_FrontFacing", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_PointCoord", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_PrimitiveID", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_SampleID", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_SamplePosition", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_FragColor", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_FragData", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MaxDrawBuffers", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_FragDepth", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_SampleMask", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ClipVertex", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_FrontColor", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_BackColor", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_TexCoord", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_FogFragCoord", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_Color", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_Normal", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MultiTexCoord0", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MultiTexCoord1", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MultiTexCoord2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MultiTexCoord3", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MultiTexCoord4", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MultiTexCoord5", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MultiTexCoord6", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MultiTexCoord7", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_FogCoord", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_NormalMatrix", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ModelViewMatrix", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ProjectionMatrix", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ModelViewProjectionMatrix", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "ftransform", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_TextureMatrix", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MaxTextureCoords", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ModelViewMatrixInverse", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ProjectionMatrixInverse", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ModelViewProjectionMatrixInverse", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ModelViewMatrixTranspose", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ProjectionMatrixTranspose", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ModelViewProjectionMatrixTranspose", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_ClipPlane", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_LightSource", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_MaxLights", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_LightModel", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_Fog", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_FrontMaterial", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_BackMaterial", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_Point", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "gl_NormalScale", len))
    i = len;
  else
    i = 0;

  /* If next source char is an identifier (eg. 'i' in "definate") no match */
  if (i == 0 || text_check_identifier(string[i]))
    return -1;
  return i;
}

static int txtfmt_glsl_find_bool(const char *string)
{
  int i, len;

  if (STR_LITERAL_STARTSWITH(string, "null", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "true", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "false", len))
    i = len;
  else
    i = 0;

  /* If next source char is an identifier (eg. 'i' in "Nonetheless") no match */
  if (i == 0 || text_check_identifier(string[i]))
    return -1;
  return i;
}

static int txtfmt_glsl_find_reserved(const char *string)
{
  int i, len;

  if (STR_LITERAL_STARTSWITH(string, "min", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "max", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "abs", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "dot", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "cross", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "reflect", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "refract", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "pow", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "exp", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "dFdx", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "dFdy", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "log", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "cos", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "sin", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "tan", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "smoothstep", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "mix", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "radians", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "degrees", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "asin", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "acos", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "atan", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "atan2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "exp2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "log2", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "sqrt", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "inversesqrt", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "sign", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "floor", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "ceil", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "fract", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "mod", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "clamp", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "step", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "trunc", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "round", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "length", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "distance", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "normalize", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "faceforward", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "any", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "all", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "not", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "texture3DLod", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "texture2DLod", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "texture1DLod", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "textureCubeLod", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "texture3D", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "texture2D", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "texture1D", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "textureCube", len))
    i = len;
  else if (STR_LITERAL_STARTSWITH(string, "texture", len))
    i = len;
  else
    i = 0;
  /* If next source char is an identifier (eg. 'i' in "Nonetheless") no match */
  if (i == 0 || text_check_identifier(string[i]))
    return -1;
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
}
