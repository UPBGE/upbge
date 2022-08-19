/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup freestyle
 * \brief Class gathering stroke creation algorithms
 */

#include <string>

#include "AdvancedFunctions1D.h"
#include "Predicates1D.h"

#include "../view_map/Interface1D.h"

//
// Predicates definitions
//
///////////////////////////////////////////////////////////

namespace Freestyle {

namespace Predicates1D {

// DensityLowerThanUP1D
/** Returns true if the density evaluated for the
 *  Interface1D is less than a user-defined density value.
 */
class DensityLowerThanUP1D : public UnaryPredicate1D {
 public:
  /** Builds the functor.
   *  \param threshold:
   *    The value of the threshold density.
   *    Any Interface1D having a density lower than this threshold will match.
   *  \param sigma:
   *    The sigma value defining the density evaluation window size used in the DensityF0D functor.
   */
  DensityLowerThanUP1D(double threshold, double sigma = 2)
  {
    _threshold = threshold;
    _sigma = sigma;
  }

  /** Returns the string "DensityLowerThanUP1D" */
  string getName() const
  {
    return "DensityLowerThanUP1D";
  }

  /** The () operator. */
  int operator()(Interface1D &inter)
  {
    Functions1D::DensityF1D fun(_sigma);
    if (fun(inter) < 0) {
      return -1;
    }
    result = (fun.result < _threshold);
    return 0;
  }

 private:
  double _sigma;
  double _threshold;
};

}  // end of namespace Predicates1D

} /* namespace Freestyle */
