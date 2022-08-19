/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup freestyle
 * \brief An Algorithm for Automatically Fitting Digitized Curves by Philip J. Schneider,
 * \brief from "Graphics Gems", Academic Press, 1990
 */

#include <vector>

#include "Geom.h"

#include "../system/FreestyleConfig.h"

namespace Freestyle {

using namespace Geometry;

/* 2d point */
typedef struct Point2Struct {
  double coordinates[2];

  Point2Struct()
  {
    coordinates[0] = 0;
    coordinates[1] = 0;
  }

  inline double operator[](const int i) const
  {
    return coordinates[i];
  }

  inline double &operator[](const int i)
  {
    return coordinates[i];
  }

  inline double x() const
  {
    return coordinates[0];
  }

  inline double y() const
  {
    return coordinates[1];
  }
} Point2;

typedef Point2 Vector2;

class FitCurveWrapper {
 private:
  std::vector<Vector2> _vertices;

 public:
  ~FitCurveWrapper();

  /** Fits a set of 2D data points to a set of Bezier Curve segments
   *    data
   *      Input data points
   *    oCurve
   *      Control points of the sets of bezier curve segments.
   *      Each segment is made of 4 points (polynomial degree of curve = 3)
   *    error
   *      max error tolerance between resulting curve and input data
   */
  void FitCurve(std::vector<Vec2d> &data, std::vector<Vec2d> &oCurve, double error);

 protected:
  /* Vec2d  *d;    Array of digitized points
   * int    nPts;  Number of digitized points
   * double error; User-defined error squared
   */
  void FitCurve(Vector2 *d, int nPts, double error);

  /** Draws a Bezier curve segment
   *  n
   *    degree of curve (=3)
   *  curve
   *    bezier segments control points
   */
  void DrawBezierCurve(int n, Vector2 *curve);

  /* Vec2d  *d;           Array of digitized points
   * int    first, last;  Indices of first and last pts in region
   * Vec2d  tHat1, tHat2; Unit tangent vectors at endpoints
   * double error;        User-defined error squared
   */
  void FitCubic(Vector2 *d, int first, int last, Vector2 tHat1, Vector2 tHat2, double error);
};

} /* namespace Freestyle */
