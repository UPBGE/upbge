/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 * \brief Functions taking 1D input
 */

#include "Functions1D.h"

using namespace std;

namespace Freestyle::Functions1D {

int GetXF1D::operator()(Interface1D &inter)
{
  result = integrate(_func, inter.verticesBegin(), inter.verticesEnd(), _integration);
  return 0;
}

int GetYF1D::operator()(Interface1D &inter)
{
  result = integrate(_func, inter.verticesBegin(), inter.verticesEnd(), _integration);
  return 0;
}

int GetZF1D::operator()(Interface1D &inter)
{
  result = integrate(_func, inter.verticesBegin(), inter.verticesEnd(), _integration);
  return 0;
}

int GetProjectedXF1D::operator()(Interface1D &inter)
{
  result = integrate(_func, inter.verticesBegin(), inter.verticesEnd(), _integration);
  return 0;
}

int GetProjectedYF1D::operator()(Interface1D &inter)
{
  result = integrate(_func, inter.verticesBegin(), inter.verticesEnd(), _integration);
  return 0;
}

int GetProjectedZF1D::operator()(Interface1D &inter)
{
  result = integrate(_func, inter.verticesBegin(), inter.verticesEnd(), _integration);
  return 0;
}

int Orientation2DF1D::operator()(Interface1D &inter)
{
  FEdge *fe = dynamic_cast<FEdge *>(&inter);
  if (fe) {
    Vec3r res = fe->orientation2d();
    result = Vec2f(res[0], res[1]);
  }
  else {
    result = integrate(_func, inter.verticesBegin(), inter.verticesEnd(), _integration);
  }
  return 0;
}

int Orientation3DF1D::operator()(Interface1D &inter)
{
  result = integrate(_func, inter.verticesBegin(), inter.verticesEnd(), _integration);
  return 0;
}

int ZDiscontinuityF1D::operator()(Interface1D &inter)
{
  result = integrate(_func, inter.verticesBegin(), inter.verticesEnd(), _integration);
  return 0;
}

int QuantitativeInvisibilityF1D::operator()(Interface1D &inter)
{
  ViewEdge *ve = dynamic_cast<ViewEdge *>(&inter);
  if (ve) {
    result = ve->qi();
    return 0;
  }
  FEdge *fe = dynamic_cast<FEdge *>(&inter);
  if (fe) {
    result = fe->qi();
    return 0;
  }
  result = integrate(_func, inter.verticesBegin(), inter.verticesEnd(), _integration);
  return 0;
}

int CurveNatureF1D::operator()(Interface1D &inter)
{
  ViewEdge *ve = dynamic_cast<ViewEdge *>(&inter);
  if (ve) {
    result = ve->getNature();
  }
  else {
    // we return a nature that contains every natures of the viewedges spanned by the chain.
    Nature::EdgeNature nat = Nature::NO_FEATURE;
    Interface0DIterator it = inter.verticesBegin();
    while (!it.isEnd()) {
      nat |= _func(it);
      ++it;
    }
    result = nat;
  }
  return 0;
}

int TimeStampF1D::operator()(Interface1D &inter)
{
  TimeStamp *timestamp = TimeStamp::instance();
  inter.setTimeStamp(timestamp->getTimeStamp());
  return 0;
}

int ChainingTimeStampF1D::operator()(Interface1D &inter)
{
  TimeStamp *timestamp = TimeStamp::instance();
  ViewEdge *ve = dynamic_cast<ViewEdge *>(&inter);
  if (ve) {
    ve->setChainingTimeStamp(timestamp->getTimeStamp());
  }
  return 0;
}

int IncrementChainingTimeStampF1D::operator()(Interface1D &inter)
{
  ViewEdge *ve = dynamic_cast<ViewEdge *>(&inter);
  if (ve) {
    ve->setChainingTimeStamp(ve->getChainingTimeStamp() + 1);
  }
  return 0;
}

int GetShapeF1D::operator()(Interface1D &inter)
{
  vector<ViewShape *> shapesVector;
  set<ViewShape *> shapesSet;
  ViewEdge *ve = dynamic_cast<ViewEdge *>(&inter);
  if (ve) {
    shapesVector.push_back(ve->viewShape());
  }
  else {
    Interface0DIterator it = inter.verticesBegin(), itend = inter.verticesEnd();
    for (; it != itend; ++it) {
      shapesSet.insert(Functions0D::getShapeF0D(it));
    }
    shapesVector.insert<set<ViewShape *>::iterator>(
        shapesVector.begin(), shapesSet.begin(), shapesSet.end());
  }
  result = shapesVector;
  return 0;
}

int GetOccludersF1D::operator()(Interface1D &inter)
{
  vector<ViewShape *> shapesVector;
  set<ViewShape *> shapesSet;
  ViewEdge *ve = dynamic_cast<ViewEdge *>(&inter);
  if (ve) {
    result = ve->occluders();
  }
  else {
    Interface0DIterator it = inter.verticesBegin(), itend = inter.verticesEnd();
    for (; it != itend; ++it) {
      Functions0D::getOccludersF0D(it, shapesSet);
    }
    shapesVector.insert(shapesVector.begin(), shapesSet.begin(), shapesSet.end());
    result = shapesVector;
  }
  return 0;
}

int GetOccludeeF1D::operator()(Interface1D &inter)
{
  vector<ViewShape *> shapesVector;
  set<ViewShape *> shapesSet;
  ViewEdge *ve = dynamic_cast<ViewEdge *>(&inter);
  if (ve) {
    ViewShape *aShape = ve->aShape();
    if (aShape) {
      shapesVector.push_back(aShape);
    }
  }
  else {
    Interface0DIterator it = inter.verticesBegin(), itend = inter.verticesEnd();
    for (; it != itend; ++it) {
      shapesSet.insert(Functions0D::getOccludeeF0D(it));
    }
    shapesVector.insert<set<ViewShape *>::iterator>(
        shapesVector.begin(), shapesSet.begin(), shapesSet.end());
  }
  result = shapesVector;
  return 0;
}

// Internal
////////////

void getOccludeeF1D(Interface1D &inter, set<ViewShape *> &oShapes)
{
  ViewEdge *ve = dynamic_cast<ViewEdge *>(&inter);
  if (ve) {
    ViewShape *aShape = ve->aShape();
    if (aShape == nullptr) {
      oShapes.insert((ViewShape *)nullptr);
      return;
    }
    oShapes.insert(aShape);
  }
  else {
    Interface0DIterator it = inter.verticesBegin(), itend = inter.verticesEnd();
    for (; it != itend; ++it) {
      oShapes.insert(Functions0D::getOccludeeF0D(it));
    }
  }
}

void getOccludersF1D(Interface1D &inter, set<ViewShape *> &oShapes)
{
  ViewEdge *ve = dynamic_cast<ViewEdge *>(&inter);
  if (ve) {
    vector<ViewShape *> &occluders = ve->occluders();
    oShapes.insert<vector<ViewShape *>::iterator>(occluders.begin(), occluders.end());
  }
  else {
    Interface0DIterator it = inter.verticesBegin(), itend = inter.verticesEnd();
    for (; it != itend; ++it) {
      set<ViewShape *> shapes;
      Functions0D::getOccludersF0D(it, shapes);
      for (set<ViewShape *>::iterator s = shapes.begin(), send = shapes.end(); s != send; ++s) {
        oShapes.insert(*s);
      }
    }
  }
}

void getShapeF1D(Interface1D &inter, set<ViewShape *> &oShapes)
{
  ViewEdge *ve = dynamic_cast<ViewEdge *>(&inter);
  if (ve) {
    oShapes.insert(ve->viewShape());
  }
  else {
    Interface0DIterator it = inter.verticesBegin(), itend = inter.verticesEnd();
    for (; it != itend; ++it) {
      oShapes.insert(Functions0D::getShapeF0D(it));
    }
  }
}

}  // namespace Freestyle::Functions1D
