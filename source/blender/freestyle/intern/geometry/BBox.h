/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup freestyle
 * \brief A class to hold a bounding box
 */

#include <algorithm>
#include <stdlib.h>

#include "BLI_utildefines.h"

#ifdef WITH_CXX_GUARDEDALLOC
#  include "MEM_guardedalloc.h"
#endif

namespace Freestyle {

template<class Point> class BBox {
 public:
  inline BBox()
  {
    _empty = true;
  }

  template<class T> inline BBox(const T &min_in, const T &max_in) : _min(min_in), _max(max_in)
  {
    _empty = false;
  }

  template<class T> inline BBox(const BBox<T> &b) : _min(b.getMin()), _max(b.getMax())
  {
    _empty = false;
  }

  template<class T> inline void extendToContain(const T &p)
  {
    if (_empty) {
      _min = p;
      _max = p;
      _empty = false;
      return;
    }
    for (unsigned int i = 0; i < Point::dim(); i++) {
      if (p[i] < _min[i]) {
        _min[i] = p[i];
      }
      else if (p[i] > _max[i]) {
        _max[i] = p[i];
      }
    }
    _empty = false;
  }

  inline void clear()
  {
    _empty = true;
  }

  inline bool empty() const
  {
    return _empty;
  }

  inline const Point &getMin() const
  {
    return _min;
  }

  inline const Point &getMax() const
  {
    return _max;
  }

  inline BBox<Point> &operator=(const BBox<Point> &b)
  {
    BLI_assert(!b.empty());
    _min = b.getMin();
    _max = b.getMax();
    _empty = false;
    return *this;
  }

  inline BBox<Point> &operator+=(const BBox<Point> &b)
  {
    BLI_assert(!b.empty());
    if (_empty) {
      _min = b.getMin();
      _max = b.getMax();
      _empty = false;
    }
    else {
      for (unsigned int i = 0; i < Point::dim(); i++) {
        if (b.getMin()[i] < _min[i]) {
          _min[i] = b.getMin()[i];
        }
        if (b.getMax()[i] > _max[i]) {
          _max[i] = b.getMax()[i];
        }
      }
    }
    return *this;
  }

  inline bool inside(const Point &p)
  {
    if (empty()) {
      return false;
    }
    for (unsigned int i = 0; i < Point::dim(); i++) {
      if ((_min[i] > p[i]) || (_max[i] < p[i])) {
        return false;
      }
    }
    return true;
  }

 private:
  Point _min;
  Point _max;
  bool _empty;

#ifdef WITH_CXX_GUARDEDALLOC
  MEM_CXX_CLASS_ALLOC_FUNCS("Freestyle:BBox")
#endif
};

template<class Point> BBox<Point> &operator+(const BBox<Point> &b1, const BBox<Point> &b2)
{
  Point new_min;
  Point new_max;

  for (unsigned int i = 0; i < Point::dim(); i++) {
    new_min[i] = b1.getMin()[i] < b2.getMin()[i] ? b1.getMin()[i] : b2.getMin()[i];
    new_max[i] = b1.getMax()[i] > b2.getMax()[i] ? b1.getMax()[i] : b2.getMax()[i];
  }

  return BBox<Point>(new_min, new_max);
}

} /* namespace Freestyle */
