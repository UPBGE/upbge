/* SPDX-License-Identifier: GPL-2.0-or-later
 * The Original Code is:
 *   OGF/Graphite: Geometry and Graphics Programming Library + Utilities
 *   Copyright 2000 Bruno Levy <levy@loria.fr> */

#pragma once

/** \file
 * \ingroup freestyle
 */

#include "Geom.h"

#include "../system/FreestyleConfig.h"

#ifdef WITH_CXX_GUARDEDALLOC
#  include "MEM_guardedalloc.h"
#endif

namespace Freestyle {

using namespace Geometry;

namespace OGF {

template<class T> inline void ogf_swap(T &x, T &y)
{
  T z = x;
  x = y;
  y = z;
}

//_________________________________________________________

/**
 * NormalCycle evaluates the curvature tensor in function
 * of a set of dihedral angles and associated vectors.
 * Reference:
 *    Restricted Delaunay Triangulation and Normal Cycle,
 *    D. Cohen-Steiner and J.M. Morvan,
 *    SOCG 2003
 */
class NormalCycle {
 public:
  void begin();
  void end();
  /**
   * NOTE: the specified edge vector needs to be pre-clipped by the neighborhood.
   */
  void accumulate_dihedral_angle(const Vec3r &edge, real angle, real neigh_area = 1.0);

  const Vec3r &eigen_vector(int i) const
  {
    return axis_[i_[i]];
  }

  real eigen_value(int i) const
  {
    return eigen_value_[i_[i]];
  }

  const Vec3r &N() const
  {
    return eigen_vector(2);
  }

  const Vec3r &Kmax() const
  {
    return eigen_vector(1);
  }

  const Vec3r &Kmin() const
  {
    return eigen_vector(0);
  }

  real n() const
  {
    return eigen_value(2);
  }

  real kmax() const
  {
    return eigen_value(1);
  }

  real kmin() const
  {
    return eigen_value(0);
  }

 private:
  /* UNUSED */
  // real center_[3];
  Vec3r axis_[3];
  real eigen_value_[3];
  real M_[6];
  int i_[3];

#ifdef WITH_CXX_GUARDEDALLOC
  MEM_CXX_CLASS_ALLOC_FUNCS("Freestyle:OGF:NormalCycle")
#endif
};

inline void NormalCycle::accumulate_dihedral_angle(const Vec3r &edge,
                                                   const double beta,
                                                   double neigh_area)
{
  double s = beta * neigh_area / edge.norm();

  M_[0] += s * edge.x() * edge.x();
  M_[1] += s * edge.x() * edge.y();
  M_[2] += s * edge.y() * edge.y();
  M_[3] += s * edge.x() * edge.z();
  M_[4] += s * edge.y() * edge.z();
  M_[5] += s * edge.z() * edge.z();
}

//_________________________________________________________

}  // namespace OGF

} /* namespace Freestyle */
