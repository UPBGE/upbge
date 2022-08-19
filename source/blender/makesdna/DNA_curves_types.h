/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_customdata_types.h"

#include "BLI_utildefines.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
namespace blender::bke {
class CurvesGeometryRuntime;
}  // namespace blender::bke
using CurvesGeometryRuntimeHandle = blender::bke::CurvesGeometryRuntime;
#else
typedef struct CurvesGeometryRuntimeHandle CurvesGeometryRuntimeHandle;
#endif

typedef enum CurveType {
  CURVE_TYPE_CATMULL_ROM = 0,
  CURVE_TYPE_POLY = 1,
  CURVE_TYPE_BEZIER = 2,
  CURVE_TYPE_NURBS = 3,
} CurveType;
#define CURVE_TYPES_NUM 4

typedef enum HandleType {
  /** The handle can be moved anywhere, and doesn't influence the point's other handle. */
  BEZIER_HANDLE_FREE = 0,
  /** The location is automatically calculated to be smooth. */
  BEZIER_HANDLE_AUTO = 1,
  /** The location is calculated to point to the next/previous control point. */
  BEZIER_HANDLE_VECTOR = 2,
  /** The location is constrained to point in the opposite direction as the other handle. */
  BEZIER_HANDLE_ALIGN = 3,
} HandleType;

/** Method used to calculate a NURBS curve's knot vector. */
typedef enum KnotsMode {
  NURBS_KNOT_MODE_NORMAL = 0,
  NURBS_KNOT_MODE_ENDPOINT = 1,
  NURBS_KNOT_MODE_BEZIER = 2,
  NURBS_KNOT_MODE_ENDPOINT_BEZIER = 3,
} KnotsMode;

/** Method used to calculate the normals of a curve's evaluated points. */
typedef enum NormalMode {
  NORMAL_MODE_MINIMUM_TWIST = 0,
  NORMAL_MODE_Z_UP = 1,
} NormalMode;

/**
 * A reusable data structure for geometry consisting of many curves. All control point data is
 * stored contiguously for better efficiency. Data for each curve is stored as a slice of the
 * main #point_data array.
 *
 * The data structure is meant to be embedded in other data-blocks to allow reusing
 * curve-processing algorithms for multiple Blender data-block types.
 */
typedef struct CurvesGeometry {
  /**
   * The start index of each curve in the point data. The size of each curve can be calculated by
   * subtracting the offset from the next offset. That is valid even for the last curve because
   * this array is allocated with a length one larger than the number of curves. This is allowed
   * to be null when there are no curves.
   *
   * Every curve offset must be at least one larger than the previous.
   * In other words, every curve must have at least one point.
   *
   * \note This is *not* stored in #CustomData because its size is one larger than #curve_data.
   */
  int *curve_offsets;

  /**
   * All attributes stored on control points (#ATTR_DOMAIN_POINT).
   * This might not contain a layer for positions if there are no points.
   */
  CustomData point_data;

  /**
   * All attributes stored on curves (#ATTR_DOMAIN_CURVE).
   */
  CustomData curve_data;

  /**
   * The total number of control points in all curves.
   */
  int point_num;
  /**
   * The number of curves in the data-block.
   */
  int curve_num;

  /**
   * Runtime data for curves, stored as a pointer to allow defining this as a C++ class.
   */
  CurvesGeometryRuntimeHandle *runtime;
} CurvesGeometry;

typedef struct Curves {
  ID id;
  /* Animation data (must be immediately after id). */
  struct AnimData *adt;

  CurvesGeometry geometry;

  int flag;
  int attributes_active_index;

  /* Materials. */
  struct Material **mat;
  short totcol;

  /**
   * User-defined symmetry flag (#eCurvesSymmetryType) that causes editing operations to maintain
   * symmetrical geometry.
   */
  char symmetry;
  /**
   * #eAttrDomain. The active selection mode domain. At most one selection mode can be active
   * at a time.
   */
  char selection_domain;
  char _pad[4];

  /**
   * Used as base mesh when curves represent e.g. hair or fur. This surface is used in edit modes.
   * When set, the curves will have attributes that indicate a position on this surface. This is
   * used for deforming the curves when the surface is deformed dynamically.
   *
   * This is expected to be a mesh object.
   */
  struct Object *surface;

  /**
   * The name of the attribute on the surface #Mesh used to give meaning to the UV attachment
   * coordinates stored on each curve. Expected to be a 2D vector attribute on the face corner
   * domain.
   */
  char *surface_uv_map;

  /* Draw Cache. */
  void *batch_cache;
} Curves;

/** #Curves.flag */
enum {
  HA_DS_EXPAND = (1 << 0),
  CV_SCULPT_SELECTION_ENABLED = (1 << 1),
};

/** #Curves.symmetry */
typedef enum eCurvesSymmetryType {
  CURVES_SYMMETRY_X = 1 << 0,
  CURVES_SYMMETRY_Y = 1 << 1,
  CURVES_SYMMETRY_Z = 1 << 2,
} eCurvesSymmetryType;
ENUM_OPERATORS(eCurvesSymmetryType, CURVES_SYMMETRY_Z)

/* Only one material supported currently. */
#define CURVES_MATERIAL_NR 1

#ifdef __cplusplus
}
#endif
