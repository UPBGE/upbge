// Copyright 2013 Blender Foundation. All rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//
// Author: Sergey Sharybin

#ifndef OPENSUBDIV_CAPI_TYPES_H_
#define OPENSUBDIV_CAPI_TYPES_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum eOpenSubdivEvaluator {
  OPENSUBDIV_EVALUATOR_CPU = 0,
  OPENSUBDIV_EVALUATOR_GPU = 1,
} eOpenSubdivEvaluator;

typedef enum OpenSubdiv_SchemeType {
  OSD_SCHEME_BILINEAR,
  OSD_SCHEME_CATMARK,
  OSD_SCHEME_LOOP,
} OpenSubdiv_SchemeType;

typedef enum OpenSubdiv_VtxBoundaryInterpolation {
  // Do not interpolate boundaries
  OSD_VTX_BOUNDARY_NONE,
  // Sharpen edges.
  OSD_VTX_BOUNDARY_EDGE_ONLY,
  // sharpen edges and corners,
  OSD_VTX_BOUNDARY_EDGE_AND_CORNER,
} OpenSubdiv_VtxBoundaryInterpolation;

typedef enum OpenSubdiv_FVarLinearInterpolation {
  OSD_FVAR_LINEAR_INTERPOLATION_NONE,
  OSD_FVAR_LINEAR_INTERPOLATION_CORNERS_ONLY,
  OSD_FVAR_LINEAR_INTERPOLATION_CORNERS_PLUS1,
  OSD_FVAR_LINEAR_INTERPOLATION_CORNERS_PLUS2,
  OSD_FVAR_LINEAR_INTERPOLATION_BOUNDARIES,
  OSD_FVAR_LINEAR_INTERPOLATION_ALL,
} OpenSubdiv_FVarLinearInterpolation;

typedef struct OpenSubdiv_PatchCoord {
  int ptex_face;

  // Parametric location on patch.
  float u, v;
} OpenSubdiv_PatchCoord;

#ifdef __cplusplus
}
#endif

#endif  // OPENSUBDIV_CAPI_TYPES_H_
