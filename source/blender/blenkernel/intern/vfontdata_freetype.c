/* SPDX-License-Identifier: GPL-2.0-or-later
 * The Original Code is written by Rob Haarsma (phase). All rights reserved. */

/** \file
 * \ingroup bke
 *
 * This code parses the Freetype font outline data to chains of Blender's bezier-triples.
 * Additional information can be found at the bottom of this file.
 *
 * Code that uses exotic character maps is present but commented out.
 */

#include <ft2build.h>
#include FT_FREETYPE_H
/* not needed yet */
// #include FT_GLYPH_H
// #include FT_BBOX_H
// #include FT_SIZES_H
// #include <freetype/ttnameid.h>

#include "MEM_guardedalloc.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_curve.h"
#include "BKE_vfontdata.h"

#include "DNA_curve_types.h"
#include "DNA_packedFile_types.h"
#include "DNA_vfont_types.h"

/* local variables */
static FT_Library library;
static FT_Error err;

static VChar *freetypechar_to_vchar(FT_Face face, FT_ULong charcode, VFontData *vfd)
{
  const float scale = vfd->scale;
  const float eps = 0.0001f;
  const float eps_sq = eps * eps;
  /* Blender */
  struct Nurb *nu;
  struct VChar *che;
  struct BezTriple *bezt;

  /* Freetype2 */
  FT_GlyphSlot glyph;
  FT_UInt glyph_index;
  FT_Outline ftoutline;
  float dx, dy;
  int j, k, l, l_first = 0;

  /*
   * Generate the character 3D data
   *
   * Get the FT Glyph index and load the Glyph */
  glyph_index = FT_Get_Char_Index(face, charcode);
  err = FT_Load_Glyph(face, glyph_index, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP);

  /* If loading succeeded, convert the FT glyph to the internal format */
  if (!err) {
    /* initialize as -1 to add 1 on first loop each time */
    int contour_prev;
    int *onpoints;

    /* First we create entry for the new character to the character list */
    che = (VChar *)MEM_callocN(sizeof(struct VChar), "objfnt_char");

    /* Take some data for modifying purposes */
    glyph = face->glyph;
    ftoutline = glyph->outline;

    /* Set the width and character code */
    che->index = charcode;
    che->width = glyph->advance.x * scale;

    BLI_ghash_insert(vfd->characters, POINTER_FROM_UINT(che->index), che);

    /* Start converting the FT data */
    onpoints = (int *)MEM_callocN((ftoutline.n_contours) * sizeof(int), "onpoints");

    /* Get number of on-curve points for bezier-triples (including conic virtual on-points). */
    for (j = 0, contour_prev = -1; j < ftoutline.n_contours; j++) {
      const int n = ftoutline.contours[j] - contour_prev;
      contour_prev = ftoutline.contours[j];

      for (k = 0; k < n; k++) {
        l = (j > 0) ? (k + ftoutline.contours[j - 1] + 1) : k;
        if (k == 0) {
          l_first = l;
        }

        if (ftoutline.tags[l] == FT_Curve_Tag_On) {
          onpoints[j]++;
        }

        {
          const int l_next = (k < n - 1) ? (l + 1) : l_first;
          if (ftoutline.tags[l] == FT_Curve_Tag_Conic &&
              ftoutline.tags[l_next] == FT_Curve_Tag_Conic) {
            onpoints[j]++;
          }
        }
      }
    }

    /* contour loop, bezier & conic styles merged */
    for (j = 0, contour_prev = -1; j < ftoutline.n_contours; j++) {
      const int n = ftoutline.contours[j] - contour_prev;
      contour_prev = ftoutline.contours[j];

      /* add new curve */
      nu = (Nurb *)MEM_callocN(sizeof(struct Nurb), "objfnt_nurb");
      bezt = (BezTriple *)MEM_callocN((onpoints[j]) * sizeof(BezTriple), "objfnt_bezt");
      BLI_addtail(&che->nurbsbase, nu);

      nu->type = CU_BEZIER;
      nu->pntsu = onpoints[j];
      nu->resolu = 8;
      nu->flagu = CU_NURB_CYCLIC;
      nu->bezt = bezt;

      /* individual curve loop, start-end */
      for (k = 0; k < n; k++) {
        l = (j > 0) ? (k + ftoutline.contours[j - 1] + 1) : k;
        if (k == 0) {
          l_first = l;
        }

        /* virtual conic on-curve points */
        {
          const int l_next = (k < n - 1) ? (l + 1) : l_first;
          if (ftoutline.tags[l] == FT_Curve_Tag_Conic &&
              ftoutline.tags[l_next] == FT_Curve_Tag_Conic) {
            dx = (ftoutline.points[l].x + ftoutline.points[l_next].x) * scale / 2.0f;
            dy = (ftoutline.points[l].y + ftoutline.points[l_next].y) * scale / 2.0f;

            /* left handle */
            bezt->vec[0][0] = (dx + (2 * ftoutline.points[l].x) * scale) / 3.0f;
            bezt->vec[0][1] = (dy + (2 * ftoutline.points[l].y) * scale) / 3.0f;

            /* midpoint (virtual on-curve point) */
            bezt->vec[1][0] = dx;
            bezt->vec[1][1] = dy;

            /* right handle */
            bezt->vec[2][0] = (dx + (2 * ftoutline.points[l_next].x) * scale) / 3.0f;
            bezt->vec[2][1] = (dy + (2 * ftoutline.points[l_next].y) * scale) / 3.0f;

            bezt->h1 = bezt->h2 = HD_ALIGN;
            bezt->radius = 1.0f;
            bezt++;
          }
        }

        /* on-curve points */
        if (ftoutline.tags[l] == FT_Curve_Tag_On) {
          const int l_prev = (k > 0) ? (l - 1) : ftoutline.contours[j];
          const int l_next = (k < n - 1) ? (l + 1) : l_first;

          /* left handle */
          if (ftoutline.tags[l_prev] == FT_Curve_Tag_Cubic) {
            bezt->vec[0][0] = ftoutline.points[l_prev].x * scale;
            bezt->vec[0][1] = ftoutline.points[l_prev].y * scale;
            bezt->h1 = HD_FREE;
          }
          else if (ftoutline.tags[l_prev] == FT_Curve_Tag_Conic) {
            bezt->vec[0][0] = (ftoutline.points[l].x + (2 * ftoutline.points[l_prev].x)) * scale /
                              3.0f;
            bezt->vec[0][1] = (ftoutline.points[l].y + (2 * ftoutline.points[l_prev].y)) * scale /
                              3.0f;
            bezt->h1 = HD_FREE;
          }
          else {
            bezt->vec[0][0] = ftoutline.points[l].x * scale -
                              (ftoutline.points[l].x - ftoutline.points[l_prev].x) * scale / 3.0f;
            bezt->vec[0][1] = ftoutline.points[l].y * scale -
                              (ftoutline.points[l].y - ftoutline.points[l_prev].y) * scale / 3.0f;
            bezt->h1 = HD_VECT;
          }

          /* midpoint (on-curve point) */
          bezt->vec[1][0] = ftoutline.points[l].x * scale;
          bezt->vec[1][1] = ftoutline.points[l].y * scale;

          /* right handle */
          if (ftoutline.tags[l_next] == FT_Curve_Tag_Cubic) {
            bezt->vec[2][0] = ftoutline.points[l_next].x * scale;
            bezt->vec[2][1] = ftoutline.points[l_next].y * scale;
            bezt->h2 = HD_FREE;
          }
          else if (ftoutline.tags[l_next] == FT_Curve_Tag_Conic) {
            bezt->vec[2][0] = (ftoutline.points[l].x + (2 * ftoutline.points[l_next].x)) * scale /
                              3.0f;
            bezt->vec[2][1] = (ftoutline.points[l].y + (2 * ftoutline.points[l_next].y)) * scale /
                              3.0f;
            bezt->h2 = HD_FREE;
          }
          else {
            bezt->vec[2][0] = ftoutline.points[l].x * scale -
                              (ftoutline.points[l].x - ftoutline.points[l_next].x) * scale / 3.0f;
            bezt->vec[2][1] = ftoutline.points[l].y * scale -
                              (ftoutline.points[l].y - ftoutline.points[l_next].y) * scale / 3.0f;
            bezt->h2 = HD_VECT;
          }

          /* get the handles that are aligned, tricky...
           * - check if one of them is a vector handle.
           * - dist_squared_to_line_v2, check if the three beztriple points are on one line
           * - len_squared_v2v2, see if there's a distance between the three points
           * - len_squared_v2v2 again, to check the angle between the handles
           */
          if ((bezt->h1 != HD_VECT && bezt->h2 != HD_VECT) &&
              (dist_squared_to_line_v2(bezt->vec[0], bezt->vec[1], bezt->vec[2]) <
               (0.001f * 0.001f)) &&
              (len_squared_v2v2(bezt->vec[0], bezt->vec[1]) > eps_sq) &&
              (len_squared_v2v2(bezt->vec[1], bezt->vec[2]) > eps_sq) &&
              (len_squared_v2v2(bezt->vec[0], bezt->vec[2]) > eps_sq) &&
              (len_squared_v2v2(bezt->vec[0], bezt->vec[2]) >
               max_ff(len_squared_v2v2(bezt->vec[0], bezt->vec[1]),
                      len_squared_v2v2(bezt->vec[1], bezt->vec[2])))) {
            bezt->h1 = bezt->h2 = HD_ALIGN;
          }
          bezt->radius = 1.0f;
          bezt++;
        }
      }
    }

    MEM_freeN(onpoints);

    return che;
  }

  return NULL;
}

static VChar *objchr_to_ftvfontdata(VFont *vfont, FT_ULong charcode)
{
  VChar *che;

  /* Freetype2 */
  FT_Face face;

  /* Load the font to memory */
  if (vfont->temp_pf) {
    err = FT_New_Memory_Face(library, vfont->temp_pf->data, vfont->temp_pf->size, 0, &face);
    if (err) {
      return NULL;
    }
  }
  else {
    err = true;
    return NULL;
  }

  /* Read the char */
  che = freetypechar_to_vchar(face, charcode, vfont->data);

  /* And everything went ok */
  return che;
}

static VFontData *objfnt_to_ftvfontdata(PackedFile *pf)
{
  /* Variables */
  FT_Face face;
  const FT_ULong charcode_reserve = 256;
  FT_ULong charcode = 0, lcode;
  FT_UInt glyph_index;
  VFontData *vfd;

  /* load the freetype font */
  err = FT_New_Memory_Face(library, pf->data, pf->size, 0, &face);

  if (err) {
    return NULL;
  }

  /* allocate blender font */
  vfd = MEM_callocN(sizeof(*vfd), "FTVFontData");

  /* Get the name. */
  if (face->family_name) {
    BLI_snprintf(vfd->name, sizeof(vfd->name), "%s %s", face->family_name, face->style_name);
    BLI_str_utf8_invalid_strip(vfd->name, strlen(vfd->name));
  }

  /* Select a character map. */
  err = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
  if (err) {
    err = FT_Select_Charmap(face, FT_ENCODING_APPLE_ROMAN);
  }
  if (err && face->num_charmaps > 0) {
    err = FT_Select_Charmap(face, face->charmaps[0]->encoding);
  }
  if (err) {
    FT_Done_Face(face);
    MEM_freeN(vfd);
    return NULL;
  }

  /* Extract the first 256 character from TTF */
  lcode = charcode = FT_Get_First_Char(face, &glyph_index);

  /* Blender default BFont is not "complete". */
  const bool complete_font = (face->ascender != 0) && (face->descender != 0) &&
                             (face->ascender != face->descender);

  if (complete_font) {
    /* We can get descender as well, but we simple store descender in relation to the ascender.
     * Also note that descender is stored as a negative number. */
    vfd->ascender = (float)face->ascender / (face->ascender - face->descender);
  }
  else {
    vfd->ascender = 0.8f;
    vfd->em_height = 1.0f;
  }

  /* Adjust font size */
  if (face->bbox.yMax != face->bbox.yMin) {
    vfd->scale = (float)(1.0 / (double)(face->bbox.yMax - face->bbox.yMin));

    if (complete_font) {
      vfd->em_height = (float)(face->ascender - face->descender) /
                       (face->bbox.yMax - face->bbox.yMin);
    }
  }
  else {
    vfd->scale = 1.0f / 1000.0f;
  }

  /* Load characters */
  vfd->characters = BLI_ghash_int_new_ex(__func__, charcode_reserve);

  while (charcode < charcode_reserve) {
    /* Generate the font data */
    freetypechar_to_vchar(face, charcode, vfd);

    /* Next glyph */
    charcode = FT_Get_Next_Char(face, charcode, &glyph_index);

    /* Check that we won't start infinite loop */
    if (charcode <= lcode) {
      break;
    }
    lcode = charcode;
  }

  return vfd;
}

static bool check_freetypefont(PackedFile *pf)
{
  FT_Face face = NULL;
  FT_UInt glyph_index = 0;
  bool success = false;

  err = FT_New_Memory_Face(library, pf->data, pf->size, 0, &face);
  if (err) {
    return false;
    // XXX error("This is not a valid font");
  }

  FT_Get_First_Char(face, &glyph_index);
  if (glyph_index) {
    err = FT_Load_Glyph(face, glyph_index, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP);
    if (!err) {
      success = (face->glyph->format == ft_glyph_format_outline);
    }
  }

  FT_Done_Face(face);

  return success;
}

VFontData *BKE_vfontdata_from_freetypefont(PackedFile *pf)
{
  VFontData *vfd = NULL;

  /* init Freetype */
  err = FT_Init_FreeType(&library);
  if (err) {
    /* XXX error("Failed to load the Freetype font library"); */
    return NULL;
  }

  if (check_freetypefont(pf)) {
    vfd = objfnt_to_ftvfontdata(pf);
  }

  /* free Freetype */
  FT_Done_FreeType(library);

  return vfd;
}

static void *vfontdata_copy_characters_value_cb(const void *src)
{
  return BKE_vfontdata_char_copy(src);
}

VFontData *BKE_vfontdata_copy(const VFontData *vfont_src, const int UNUSED(flag))
{
  VFontData *vfont_dst = MEM_dupallocN(vfont_src);

  if (vfont_src->characters != NULL) {
    vfont_dst->characters = BLI_ghash_copy(
        vfont_src->characters, NULL, vfontdata_copy_characters_value_cb);
  }

  return vfont_dst;
}

VChar *BKE_vfontdata_char_from_freetypefont(VFont *vfont, unsigned long character)
{
  VChar *che = NULL;

  if (!vfont) {
    return NULL;
  }

  /* Init Freetype */
  err = FT_Init_FreeType(&library);
  if (err) {
    /* XXX error("Failed to load the Freetype font library"); */
    return NULL;
  }

  /* Load the character */
  che = objchr_to_ftvfontdata(vfont, character);

  /* Free Freetype */
  FT_Done_FreeType(library);

  return che;
}

VChar *BKE_vfontdata_char_copy(const VChar *vchar_src)
{
  VChar *vchar_dst = MEM_dupallocN(vchar_src);

  BLI_listbase_clear(&vchar_dst->nurbsbase);
  BKE_nurbList_duplicate(&vchar_dst->nurbsbase, &vchar_src->nurbsbase);

  return vchar_dst;
}

/**
 * from: http://www.freetype.org/freetype2/docs/glyphs/glyphs-6.html#section-1
 *
 * Vectorial representation of Freetype glyphs
 *
 * The source format of outlines is a collection of closed paths called "contours". Each contour is
 * made of a series of line segments and bezier arcs. Depending on the file format, these can be
 * second-order or third-order polynomials. The former are also called quadratic or conic arcs, and
 * they come from the TrueType format. The latter are called cubic arcs and mostly come from the
 * Type1 format.
 *
 * Each arc is described through a series of start, end and control points.
 * Each point of the outline has a specific tag which indicates whether it is
 * used to describe a line segment or an arc.
 * The following rules are applied to decompose the contour's points into segments and arcs :
 *
 * # two successive "on" points indicate a line segment joining them.
 *
 * # one conic "off" point amidst two "on" points indicates a conic bezier arc,
 *   the "off" point being the control point, and the "on" ones the start and end points.
 *
 * # Two successive cubic "off" points amidst two "on" points indicate a cubic bezier arc.
 *   There must be exactly two cubic control points and two on points for each cubic arc
 *   (using a single cubic "off" point between two "on" points is forbidden, for example).
 *
 * # finally, two successive conic "off" points forces the rasterizer to create
 *   (during the scan-line conversion process exclusively) a virtual "on" point amidst them,
 *   at their exact middle.
 *   This greatly facilitates the definition of successive conic bezier arcs.
 *   Moreover, it's the way outlines are described in the TrueType specification.
 *
 * Note that it is possible to mix conic and cubic arcs in a single contour, even though no current
 * font driver produces such outlines.
 *
 * <pre>
 *                                   *            # on
 *                                                * off
 *                                __---__
 *   #-__                      _--       -_
 *       --__                _-            -
 *           --__           #               \
 *               --__                        #
 *                   -#
 *                            Two "on" points
 *    Two "on" points       and one "conic" point
 *                             between them
 *                 *
 *   #            __      Two "on" points with two "conic"
 *    \          -  -     points between them. The point
 *     \        /    \    marked '0' is the middle of the
 *      -      0      \   "off" points, and is a 'virtual'
 *       -_  _-       #   "on" point where the curve passes.
 *         --             It does not appear in the point
 *                        list.
 *         *
 *         *                # on
 *                    *     * off
 *          __---__
 *       _--       -_
 *     _-            -
 *    #               \
 *                     #
 *
 *      Two "on" points
 *    and two "cubic" point
 *       between them
 * </pre>
 *
 * Each glyphs original outline points are located on a grid of indivisible units.
 * The points are stored in the font file as 16-bit integer grid coordinates,
 * with the grid origin's being at (0, 0); they thus range from -16384 to 16383.
 *
 * Convert conic to bezier arcs:
 * Conic P0 P1 P2
 * Bezier B0 B1 B2 B3
 * B0=P0
 * B1=(P0+2*P1)/3
 * B2=(P2+2*P1)/3
 * B3=P2
 */
