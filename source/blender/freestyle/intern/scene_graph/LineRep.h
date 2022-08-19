/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup freestyle
 * \brief Class to define the representation of 3D Line.
 */

#include <list>
#include <vector>

#include "Rep.h"

#include "../system/FreestyleConfig.h"

using namespace std;

namespace Freestyle {

/** Base class for all lines objects */
class LineRep : public Rep {
 public:
  /** Line description style */
  enum LINES_STYLE {
    LINES,
    LINE_STRIP,
    LINE_LOOP,
  };

  inline LineRep() : Rep()
  {
    _width = 0.0f;
  }

  /** Builds a single line from 2 vertices
   *  v1
   *    first vertex
   *  v2
   *    second vertex
   */
  inline LineRep(const Vec3r &v1, const Vec3r &v2) : Rep()
  {
    setStyle(LINES);
    AddVertex(v1);
    AddVertex(v2);
    _width = 0.0f;
  }

  /** Builds a line rep from a vertex chain */
  inline LineRep(const vector<Vec3r> &vertices) : Rep()
  {
    _vertices = vertices;
    setStyle(LINE_STRIP);
    _width = 0.0f;
  }

  /** Builds a line rep from a vertex chain */
  inline LineRep(const list<Vec3r> &vertices) : Rep()
  {
    for (list<Vec3r>::const_iterator v = vertices.begin(), end = vertices.end(); v != end; ++v) {
      _vertices.push_back(*v);
    }
    setStyle(LINE_STRIP);
    _width = 0.0f;
  }

  virtual ~LineRep()
  {
    _vertices.clear();
  }

  /** accessors */
  inline const LINES_STYLE style() const
  {
    return _Style;
  }

  inline const vector<Vec3r> &vertices() const
  {
    return _vertices;
  }

  inline float width() const
  {
    return _width;
  }

  /** modifiers */
  inline void setStyle(const LINES_STYLE iStyle)
  {
    _Style = iStyle;
  }

  inline void AddVertex(const Vec3r &iVertex)
  {
    _vertices.push_back(iVertex);
  }

  inline void setVertices(const vector<Vec3r> &iVertices)
  {
    if (0 != _vertices.size()) {
      _vertices.clear();
    }
    for (vector<Vec3r>::const_iterator v = iVertices.begin(), end = iVertices.end(); v != end;
         ++v) {
      _vertices.push_back(*v);
    }
  }

  inline void setWidth(float iWidth)
  {
    _width = iWidth;
  }

  /** Accept the corresponding visitor */
  virtual void accept(SceneVisitor &v)
  {
    Rep::accept(v);
    v.visitLineRep(*this);
  }

  /** Computes the line bounding box. */
  virtual void ComputeBBox();

 private:
  LINES_STYLE _Style;
  vector<Vec3r> _vertices;
  float _width;

#ifdef WITH_CXX_GUARDEDALLOC
  MEM_CXX_CLASS_ALLOC_FUNCS("Freestyle:LineRep")
#endif
};

} /* namespace Freestyle */
