/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup freestyle
 * \brief Base class for all shapes.
 * Inherits from BasicObjects for references counter management (addRef, release).
 */

#include <string>

#include "FrsMaterial.h"
#include "SceneVisitor.h"

#include "../geometry/BBox.h"
#include "../geometry/Geom.h"

#include "../system/BaseObject.h"
#include "../system/Id.h"
#include "../system/Precision.h"

using namespace std;

namespace Freestyle {

using namespace Geometry;

class Rep : public BaseObject {
 public:
  inline Rep() : BaseObject()
  {
    _Id = 0;
    _FrsMaterial = 0;
  }

  inline Rep(const Rep &iBrother) : BaseObject()
  {
    _Id = iBrother._Id;
    _Name = iBrother._Name;
    _LibraryPath = iBrother._LibraryPath;
    if (0 == iBrother._FrsMaterial) {
      _FrsMaterial = 0;
    }
    else {
      _FrsMaterial = new FrsMaterial(*(iBrother._FrsMaterial));
    }

    _BBox = iBrother.bbox();
  }

  inline void swap(Rep &ioOther)
  {
    std::swap(_BBox, ioOther._BBox);
    std::swap(_Id, ioOther._Id);
    std::swap(_Name, ioOther._Name);
    std::swap(_LibraryPath, ioOther._LibraryPath);
    std::swap(_FrsMaterial, ioOther._FrsMaterial);
  }

  Rep &operator=(const Rep &iBrother)
  {
    if (&iBrother != this) {
      _Id = iBrother._Id;
      _Name = iBrother._Name;
      _LibraryPath = iBrother._LibraryPath;
      if (0 == iBrother._FrsMaterial) {
        _FrsMaterial = 0;
      }
      else {
        if (_FrsMaterial == 0) {
          _FrsMaterial = new FrsMaterial(*iBrother._FrsMaterial);
        }
        else {
          (*_FrsMaterial) = (*(iBrother._FrsMaterial));
        }
        _BBox = iBrother.bbox();
      }
    }
    return *this;
  }

  virtual ~Rep()
  {
    if (0 != _FrsMaterial) {
      delete _FrsMaterial;
      _FrsMaterial = 0;
    }
  }

  /** Accept the corresponding visitor
   *  Must be overload by inherited classes
   */
  virtual void accept(SceneVisitor &v)
  {
    if (_FrsMaterial) {
      v.visitFrsMaterial(*_FrsMaterial);
    }
    v.visitRep(*this);
  }

  /** Computes the rep bounding box.
   *  Each Inherited rep must compute its bbox depending on the way the data are stored. So, each
   * inherited class must overload this method
   */
  virtual void ComputeBBox() = 0;

  /** Returns the rep bounding box */
  virtual const BBox<Vec3f> &bbox() const
  {
    return _BBox;
  }

  inline Id getId() const
  {
    return _Id;
  }

  inline const string &getName() const
  {
    return _Name;
  }

  inline const string &getLibraryPath() const
  {
    return _LibraryPath;
  }

  inline const FrsMaterial *frs_material() const
  {
    return _FrsMaterial;
  }

  /** Sets the Rep bounding box */
  virtual void setBBox(const BBox<Vec3f> &iBox)
  {
    _BBox = iBox;
  }

  inline void setId(const Id &id)
  {
    _Id = id;
  }

  inline void setName(const string &name)
  {
    _Name = name;
  }

  inline void setLibraryPath(const string &path)
  {
    _LibraryPath = path;
  }

  inline void setFrsMaterial(const FrsMaterial &iMaterial)
  {
    _FrsMaterial = new FrsMaterial(iMaterial);
  }

 private:
  BBox<Vec3f> _BBox;
  Id _Id;
  string _Name;
  string _LibraryPath;
  FrsMaterial *_FrsMaterial;
};

} /* namespace Freestyle */
