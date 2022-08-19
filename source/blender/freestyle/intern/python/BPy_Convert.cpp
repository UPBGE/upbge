/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_Convert.h"

#include "BPy_BBox.h"
#include "BPy_FrsMaterial.h"
#include "BPy_Id.h"
#include "BPy_IntegrationType.h"
#include "BPy_Interface0D.h"
#include "BPy_Interface1D.h"
#include "BPy_MediumType.h"
#include "BPy_Nature.h"
#include "BPy_SShape.h"
#include "BPy_StrokeAttribute.h"
#include "BPy_ViewShape.h"
#include "Interface0D/BPy_CurvePoint.h"
#include "Interface0D/BPy_SVertex.h"
#include "Interface0D/BPy_ViewVertex.h"
#include "Interface0D/CurvePoint/BPy_StrokeVertex.h"
#include "Interface0D/ViewVertex/BPy_NonTVertex.h"
#include "Interface0D/ViewVertex/BPy_TVertex.h"
#include "Interface1D/BPy_FEdge.h"
#include "Interface1D/BPy_Stroke.h"
#include "Interface1D/BPy_ViewEdge.h"
#include "Interface1D/Curve/BPy_Chain.h"
#include "Interface1D/FEdge/BPy_FEdgeSharp.h"
#include "Interface1D/FEdge/BPy_FEdgeSmooth.h"

#include "Iterator/BPy_AdjacencyIterator.h"
#include "Iterator/BPy_ChainPredicateIterator.h"
#include "Iterator/BPy_ChainSilhouetteIterator.h"
#include "Iterator/BPy_ChainingIterator.h"
#include "Iterator/BPy_CurvePointIterator.h"
#include "Iterator/BPy_Interface0DIterator.h"
#include "Iterator/BPy_SVertexIterator.h"
#include "Iterator/BPy_StrokeVertexIterator.h"
#include "Iterator/BPy_ViewEdgeIterator.h"
#include "Iterator/BPy_orientedViewEdgeIterator.h"

#include "../stroke/StrokeRep.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;
using namespace Freestyle::Geometry;

///////////////////////////////////////////////////////////////////////////////////////////

//==============================
// C++ => Python
//==============================

PyObject *PyBool_from_bool(bool b)
{
  return PyBool_FromLong(b ? 1 : 0);
}

PyObject *Vector_from_Vec2f(Vec2f &vec)
{
  float vec_data[2];  // because vec->_coord is protected
  vec_data[0] = vec.x();
  vec_data[1] = vec.y();
  return Vector_CreatePyObject(vec_data, 2, nullptr);
}

PyObject *Vector_from_Vec3f(Vec3f &vec)
{
  float vec_data[3];  // because vec->_coord is protected
  vec_data[0] = vec.x();
  vec_data[1] = vec.y();
  vec_data[2] = vec.z();
  return Vector_CreatePyObject(vec_data, 3, nullptr);
}

PyObject *Vector_from_Vec3r(Vec3r &vec)
{
  float vec_data[3];  // because vec->_coord is protected
  vec_data[0] = vec.x();
  vec_data[1] = vec.y();
  vec_data[2] = vec.z();
  return Vector_CreatePyObject(vec_data, 3, nullptr);
}

PyObject *BPy_Id_from_Id(Id &id)
{
  PyObject *py_id = Id_Type.tp_new(&Id_Type, nullptr, nullptr);
  ((BPy_Id *)py_id)->id = new Id(id.getFirst(), id.getSecond());
  return py_id;
}

PyObject *Any_BPy_Interface0D_from_Interface0D(Interface0D &if0D)
{
  if (typeid(if0D) == typeid(CurvePoint)) {
    return BPy_CurvePoint_from_CurvePoint(dynamic_cast<CurvePoint &>(if0D));
  }
  if (typeid(if0D) == typeid(StrokeVertex)) {
    return BPy_StrokeVertex_from_StrokeVertex(dynamic_cast<StrokeVertex &>(if0D));
  }
  if (typeid(if0D) == typeid(SVertex)) {
    return BPy_SVertex_from_SVertex(dynamic_cast<SVertex &>(if0D));
  }
  if (typeid(if0D) == typeid(ViewVertex)) {
    return BPy_ViewVertex_from_ViewVertex(dynamic_cast<ViewVertex &>(if0D));
  }
  if (typeid(if0D) == typeid(NonTVertex)) {
    return BPy_NonTVertex_from_NonTVertex(dynamic_cast<NonTVertex &>(if0D));
  }
  if (typeid(if0D) == typeid(TVertex)) {
    return BPy_TVertex_from_TVertex(dynamic_cast<TVertex &>(if0D));
  }
  if (typeid(if0D) == typeid(Interface0D)) {
    return BPy_Interface0D_from_Interface0D(if0D);
  }
  string msg("unexpected type: " + if0D.getExactTypeName());
  PyErr_SetString(PyExc_TypeError, msg.c_str());
  return nullptr;
}

PyObject *Any_BPy_Interface1D_from_Interface1D(Interface1D &if1D)
{
  if (typeid(if1D) == typeid(ViewEdge)) {
    return BPy_ViewEdge_from_ViewEdge(dynamic_cast<ViewEdge &>(if1D));
  }
  if (typeid(if1D) == typeid(Chain)) {
    return BPy_Chain_from_Chain(dynamic_cast<Chain &>(if1D));
  }
  if (typeid(if1D) == typeid(Stroke)) {
    return BPy_Stroke_from_Stroke(dynamic_cast<Stroke &>(if1D));
  }
  if (typeid(if1D) == typeid(FEdgeSharp)) {
    return BPy_FEdgeSharp_from_FEdgeSharp(dynamic_cast<FEdgeSharp &>(if1D));
  }
  if (typeid(if1D) == typeid(FEdgeSmooth)) {
    return BPy_FEdgeSmooth_from_FEdgeSmooth(dynamic_cast<FEdgeSmooth &>(if1D));
  }
  if (typeid(if1D) == typeid(FEdge)) {
    return BPy_FEdge_from_FEdge(dynamic_cast<FEdge &>(if1D));
  }
  if (typeid(if1D) == typeid(Interface1D)) {
    return BPy_Interface1D_from_Interface1D(if1D);
  }
  string msg("unexpected type: " + if1D.getExactTypeName());
  PyErr_SetString(PyExc_TypeError, msg.c_str());
  return nullptr;
}

PyObject *Any_BPy_FEdge_from_FEdge(FEdge &fe)
{
  if (typeid(fe) == typeid(FEdgeSharp)) {
    return BPy_FEdgeSharp_from_FEdgeSharp(dynamic_cast<FEdgeSharp &>(fe));
  }
  if (typeid(fe) == typeid(FEdgeSmooth)) {
    return BPy_FEdgeSmooth_from_FEdgeSmooth(dynamic_cast<FEdgeSmooth &>(fe));
  }
  if (typeid(fe) == typeid(FEdge)) {
    return BPy_FEdge_from_FEdge(fe);
  }
  string msg("unexpected type: " + fe.getExactTypeName());
  PyErr_SetString(PyExc_TypeError, msg.c_str());
  return nullptr;
}

PyObject *Any_BPy_ViewVertex_from_ViewVertex(ViewVertex &vv)
{
  if (typeid(vv) == typeid(NonTVertex)) {
    return BPy_NonTVertex_from_NonTVertex(dynamic_cast<NonTVertex &>(vv));
  }
  if (typeid(vv) == typeid(TVertex)) {
    return BPy_TVertex_from_TVertex(dynamic_cast<TVertex &>(vv));
  }
  if (typeid(vv) == typeid(ViewVertex)) {
    return BPy_ViewVertex_from_ViewVertex(vv);
  }
  string msg("unexpected type: " + vv.getExactTypeName());
  PyErr_SetString(PyExc_TypeError, msg.c_str());
  return nullptr;
}

PyObject *BPy_Interface0D_from_Interface0D(Interface0D &if0D)
{
  PyObject *py_if0D = Interface0D_Type.tp_new(&Interface0D_Type, nullptr, nullptr);
  ((BPy_Interface0D *)py_if0D)->if0D = &if0D;
  ((BPy_Interface0D *)py_if0D)->borrowed = true;
  return py_if0D;
}

PyObject *BPy_Interface1D_from_Interface1D(Interface1D &if1D)
{
  PyObject *py_if1D = Interface1D_Type.tp_new(&Interface1D_Type, nullptr, nullptr);
  ((BPy_Interface1D *)py_if1D)->if1D = &if1D;
  ((BPy_Interface1D *)py_if1D)->borrowed = true;
  return py_if1D;
}

PyObject *BPy_SVertex_from_SVertex(SVertex &sv)
{
  PyObject *py_sv = SVertex_Type.tp_new(&SVertex_Type, nullptr, nullptr);
  ((BPy_SVertex *)py_sv)->sv = &sv;
  ((BPy_SVertex *)py_sv)->py_if0D.if0D = ((BPy_SVertex *)py_sv)->sv;
  ((BPy_SVertex *)py_sv)->py_if0D.borrowed = true;
  return py_sv;
}

PyObject *BPy_FEdgeSharp_from_FEdgeSharp(FEdgeSharp &fes)
{
  PyObject *py_fe = FEdgeSharp_Type.tp_new(&FEdgeSharp_Type, nullptr, nullptr);
  ((BPy_FEdgeSharp *)py_fe)->fes = &fes;
  ((BPy_FEdgeSharp *)py_fe)->py_fe.fe = ((BPy_FEdgeSharp *)py_fe)->fes;
  ((BPy_FEdgeSharp *)py_fe)->py_fe.py_if1D.if1D = ((BPy_FEdgeSharp *)py_fe)->fes;
  ((BPy_FEdgeSharp *)py_fe)->py_fe.py_if1D.borrowed = true;
  return py_fe;
}

PyObject *BPy_FEdgeSmooth_from_FEdgeSmooth(FEdgeSmooth &fes)
{
  PyObject *py_fe = FEdgeSmooth_Type.tp_new(&FEdgeSmooth_Type, nullptr, nullptr);
  ((BPy_FEdgeSmooth *)py_fe)->fes = &fes;
  ((BPy_FEdgeSmooth *)py_fe)->py_fe.fe = ((BPy_FEdgeSmooth *)py_fe)->fes;
  ((BPy_FEdgeSmooth *)py_fe)->py_fe.py_if1D.if1D = ((BPy_FEdgeSmooth *)py_fe)->fes;
  ((BPy_FEdgeSmooth *)py_fe)->py_fe.py_if1D.borrowed = true;
  return py_fe;
}

PyObject *BPy_FEdge_from_FEdge(FEdge &fe)
{
  PyObject *py_fe = FEdge_Type.tp_new(&FEdge_Type, nullptr, nullptr);
  ((BPy_FEdge *)py_fe)->fe = &fe;
  ((BPy_FEdge *)py_fe)->py_if1D.if1D = ((BPy_FEdge *)py_fe)->fe;
  ((BPy_FEdge *)py_fe)->py_if1D.borrowed = true;
  return py_fe;
}

PyObject *BPy_Nature_from_Nature(unsigned short n)
{
  PyObject *args = PyTuple_New(1);
  PyTuple_SET_ITEM(args, 0, PyLong_FromLong(n));
  PyObject *py_n = Nature_Type.tp_new(&Nature_Type, args, nullptr);
  Py_DECREF(args);
  return py_n;
}

PyObject *BPy_Stroke_from_Stroke(Stroke &s)
{
  PyObject *py_s = Stroke_Type.tp_new(&Stroke_Type, nullptr, nullptr);
  ((BPy_Stroke *)py_s)->s = &s;
  ((BPy_Stroke *)py_s)->py_if1D.if1D = ((BPy_Stroke *)py_s)->s;
  ((BPy_Stroke *)py_s)->py_if1D.borrowed = true;
  return py_s;
}

PyObject *BPy_StrokeAttribute_from_StrokeAttribute(StrokeAttribute &sa)
{
  PyObject *py_sa = StrokeAttribute_Type.tp_new(&StrokeAttribute_Type, nullptr, nullptr);
  ((BPy_StrokeAttribute *)py_sa)->sa = &sa;
  ((BPy_StrokeAttribute *)py_sa)->borrowed = true;
  return py_sa;
}

PyObject *BPy_MediumType_from_MediumType(Stroke::MediumType n)
{
  PyObject *args = PyTuple_New(1);
  PyTuple_SET_ITEM(args, 0, PyLong_FromLong(n));
  PyObject *py_mt = MediumType_Type.tp_new(&MediumType_Type, args, nullptr);
  Py_DECREF(args);
  return py_mt;
}

PyObject *BPy_StrokeVertex_from_StrokeVertex(StrokeVertex &sv)
{
  PyObject *py_sv = StrokeVertex_Type.tp_new(&StrokeVertex_Type, nullptr, nullptr);
  ((BPy_StrokeVertex *)py_sv)->sv = &sv;
  ((BPy_StrokeVertex *)py_sv)->py_cp.cp = ((BPy_StrokeVertex *)py_sv)->sv;
  ((BPy_StrokeVertex *)py_sv)->py_cp.py_if0D.if0D = ((BPy_StrokeVertex *)py_sv)->sv;
  ((BPy_StrokeVertex *)py_sv)->py_cp.py_if0D.borrowed = true;
  return py_sv;
}

PyObject *BPy_ViewVertex_from_ViewVertex(ViewVertex &vv)
{
  PyObject *py_vv = ViewVertex_Type.tp_new(&ViewVertex_Type, nullptr, nullptr);
  ((BPy_ViewVertex *)py_vv)->vv = &vv;
  ((BPy_ViewVertex *)py_vv)->py_if0D.if0D = ((BPy_ViewVertex *)py_vv)->vv;
  ((BPy_ViewVertex *)py_vv)->py_if0D.borrowed = true;
  return py_vv;
}

PyObject *BPy_NonTVertex_from_NonTVertex(NonTVertex &ntv)
{
  PyObject *py_ntv = NonTVertex_Type.tp_new(&NonTVertex_Type, nullptr, nullptr);
  ((BPy_NonTVertex *)py_ntv)->ntv = &ntv;
  ((BPy_NonTVertex *)py_ntv)->py_vv.vv = ((BPy_NonTVertex *)py_ntv)->ntv;
  ((BPy_NonTVertex *)py_ntv)->py_vv.py_if0D.if0D = ((BPy_NonTVertex *)py_ntv)->ntv;
  ((BPy_NonTVertex *)py_ntv)->py_vv.py_if0D.borrowed = true;
  return py_ntv;
}

PyObject *BPy_TVertex_from_TVertex(TVertex &tv)
{
  PyObject *py_tv = TVertex_Type.tp_new(&TVertex_Type, nullptr, nullptr);
  ((BPy_TVertex *)py_tv)->tv = &tv;
  ((BPy_TVertex *)py_tv)->py_vv.vv = ((BPy_TVertex *)py_tv)->tv;
  ((BPy_TVertex *)py_tv)->py_vv.py_if0D.if0D = ((BPy_TVertex *)py_tv)->tv;
  ((BPy_TVertex *)py_tv)->py_vv.py_if0D.borrowed = true;
  return py_tv;
}

PyObject *BPy_BBox_from_BBox(const BBox<Vec3r> &bb)
{
  PyObject *py_bb = BBox_Type.tp_new(&BBox_Type, nullptr, nullptr);
  ((BPy_BBox *)py_bb)->bb = new BBox<Vec3r>(bb);
  return py_bb;
}

PyObject *BPy_ViewEdge_from_ViewEdge(ViewEdge &ve)
{
  PyObject *py_ve = ViewEdge_Type.tp_new(&ViewEdge_Type, nullptr, nullptr);
  ((BPy_ViewEdge *)py_ve)->ve = &ve;
  ((BPy_ViewEdge *)py_ve)->py_if1D.if1D = ((BPy_ViewEdge *)py_ve)->ve;
  ((BPy_ViewEdge *)py_ve)->py_if1D.borrowed = true;
  return py_ve;
}

PyObject *BPy_Chain_from_Chain(Chain &c)
{
  PyObject *py_c = Chain_Type.tp_new(&Chain_Type, nullptr, nullptr);
  ((BPy_Chain *)py_c)->c = &c;
  ((BPy_Chain *)py_c)->py_c.c = ((BPy_Chain *)py_c)->c;
  ((BPy_Chain *)py_c)->py_c.py_if1D.if1D = ((BPy_Chain *)py_c)->c;
  ((BPy_Chain *)py_c)->py_c.py_if1D.borrowed = true;
  return py_c;
}

PyObject *BPy_SShape_from_SShape(SShape &ss)
{
  PyObject *py_ss = SShape_Type.tp_new(&SShape_Type, nullptr, nullptr);
  ((BPy_SShape *)py_ss)->ss = &ss;
  ((BPy_SShape *)py_ss)->borrowed = true;
  return py_ss;
}

PyObject *BPy_ViewShape_from_ViewShape(ViewShape &vs)
{
  PyObject *py_vs = ViewShape_Type.tp_new(&ViewShape_Type, nullptr, nullptr);
  ((BPy_ViewShape *)py_vs)->vs = &vs;
  ((BPy_ViewShape *)py_vs)->borrowed = true;
  ((BPy_ViewShape *)py_vs)->py_ss = nullptr;
  return py_vs;
}

PyObject *BPy_FrsMaterial_from_FrsMaterial(const FrsMaterial &m)
{
  PyObject *py_m = FrsMaterial_Type.tp_new(&FrsMaterial_Type, nullptr, nullptr);
  ((BPy_FrsMaterial *)py_m)->m = new FrsMaterial(m);
  return py_m;
}

PyObject *BPy_IntegrationType_from_IntegrationType(IntegrationType i)
{
  PyObject *args = PyTuple_New(1);
  PyTuple_SET_ITEM(args, 0, PyLong_FromLong(i));
  PyObject *py_it = IntegrationType_Type.tp_new(&IntegrationType_Type, args, nullptr);
  Py_DECREF(args);
  return py_it;
}

PyObject *BPy_CurvePoint_from_CurvePoint(CurvePoint &cp)
{
  PyObject *py_cp = CurvePoint_Type.tp_new(&CurvePoint_Type, nullptr, nullptr);
  // CurvePointIterator::operator*() returns a reference of a class data
  // member whose value is mutable upon iteration over different CurvePoints.
  // It is likely that such a mutable reference is passed to this function,
  // so that a new allocated CurvePoint instance is created here to avoid
  // nasty bugs (cf. T41464).
  ((BPy_CurvePoint *)py_cp)->cp = new CurvePoint(cp);
  ((BPy_CurvePoint *)py_cp)->py_if0D.if0D = ((BPy_CurvePoint *)py_cp)->cp;
  ((BPy_CurvePoint *)py_cp)->py_if0D.borrowed = false;
  return py_cp;
}

PyObject *BPy_directedViewEdge_from_directedViewEdge(ViewVertex::directedViewEdge &dve)
{
  PyObject *py_dve = PyTuple_New(2);
  PyTuple_SET_ITEMS(
      py_dve, BPy_ViewEdge_from_ViewEdge(*(dve.first)), PyBool_from_bool(dve.second));
  return py_dve;
}

//==============================
// Iterators
//==============================

PyObject *BPy_AdjacencyIterator_from_AdjacencyIterator(AdjacencyIterator &a_it)
{
  PyObject *py_a_it = AdjacencyIterator_Type.tp_new(&AdjacencyIterator_Type, nullptr, nullptr);
  ((BPy_AdjacencyIterator *)py_a_it)->a_it = new AdjacencyIterator(a_it);
  ((BPy_AdjacencyIterator *)py_a_it)->py_it.it = ((BPy_AdjacencyIterator *)py_a_it)->a_it;
  ((BPy_AdjacencyIterator *)py_a_it)->at_start = true;
  return py_a_it;
}

PyObject *BPy_Interface0DIterator_from_Interface0DIterator(Interface0DIterator &if0D_it,
                                                           bool reversed)
{
  PyObject *py_if0D_it = Interface0DIterator_Type.tp_new(
      &Interface0DIterator_Type, nullptr, nullptr);
  ((BPy_Interface0DIterator *)py_if0D_it)->if0D_it = new Interface0DIterator(if0D_it);
  ((BPy_Interface0DIterator *)py_if0D_it)->py_it.it =
      ((BPy_Interface0DIterator *)py_if0D_it)->if0D_it;
  ((BPy_Interface0DIterator *)py_if0D_it)->at_start = true;
  ((BPy_Interface0DIterator *)py_if0D_it)->reversed = reversed;
  return py_if0D_it;
}

PyObject *BPy_CurvePointIterator_from_CurvePointIterator(CurveInternal::CurvePointIterator &cp_it)
{
  PyObject *py_cp_it = CurvePointIterator_Type.tp_new(&CurvePointIterator_Type, nullptr, nullptr);
  ((BPy_CurvePointIterator *)py_cp_it)->cp_it = new CurveInternal::CurvePointIterator(cp_it);
  ((BPy_CurvePointIterator *)py_cp_it)->py_it.it = ((BPy_CurvePointIterator *)py_cp_it)->cp_it;
  return py_cp_it;
}

PyObject *BPy_StrokeVertexIterator_from_StrokeVertexIterator(
    StrokeInternal::StrokeVertexIterator &sv_it, bool reversed)
{
  PyObject *py_sv_it = StrokeVertexIterator_Type.tp_new(
      &StrokeVertexIterator_Type, nullptr, nullptr);
  ((BPy_StrokeVertexIterator *)py_sv_it)->sv_it = new StrokeInternal::StrokeVertexIterator(sv_it);
  ((BPy_StrokeVertexIterator *)py_sv_it)->py_it.it = ((BPy_StrokeVertexIterator *)py_sv_it)->sv_it;
  ((BPy_StrokeVertexIterator *)py_sv_it)->at_start = true;
  ((BPy_StrokeVertexIterator *)py_sv_it)->reversed = reversed;
  return py_sv_it;
}

PyObject *BPy_SVertexIterator_from_SVertexIterator(ViewEdgeInternal::SVertexIterator &sv_it)
{
  PyObject *py_sv_it = SVertexIterator_Type.tp_new(&SVertexIterator_Type, nullptr, nullptr);
  ((BPy_SVertexIterator *)py_sv_it)->sv_it = new ViewEdgeInternal::SVertexIterator(sv_it);
  ((BPy_SVertexIterator *)py_sv_it)->py_it.it = ((BPy_SVertexIterator *)py_sv_it)->sv_it;
  return py_sv_it;
}

PyObject *BPy_orientedViewEdgeIterator_from_orientedViewEdgeIterator(
    ViewVertexInternal::orientedViewEdgeIterator &ove_it, bool reversed)
{
  PyObject *py_ove_it = orientedViewEdgeIterator_Type.tp_new(
      &orientedViewEdgeIterator_Type, nullptr, nullptr);
  ((BPy_orientedViewEdgeIterator *)py_ove_it)->ove_it =
      new ViewVertexInternal::orientedViewEdgeIterator(ove_it);
  ((BPy_orientedViewEdgeIterator *)py_ove_it)->py_it.it =
      ((BPy_orientedViewEdgeIterator *)py_ove_it)->ove_it;
  ((BPy_orientedViewEdgeIterator *)py_ove_it)->at_start = true;
  ((BPy_orientedViewEdgeIterator *)py_ove_it)->reversed = reversed;
  return py_ove_it;
}

PyObject *BPy_ViewEdgeIterator_from_ViewEdgeIterator(ViewEdgeInternal::ViewEdgeIterator &ve_it)
{
  PyObject *py_ve_it = ViewEdgeIterator_Type.tp_new(&ViewEdgeIterator_Type, nullptr, nullptr);
  ((BPy_ViewEdgeIterator *)py_ve_it)->ve_it = new ViewEdgeInternal::ViewEdgeIterator(ve_it);
  ((BPy_ViewEdgeIterator *)py_ve_it)->py_it.it = ((BPy_ViewEdgeIterator *)py_ve_it)->ve_it;
  return py_ve_it;
}

PyObject *BPy_ChainingIterator_from_ChainingIterator(ChainingIterator &c_it)
{
  PyObject *py_c_it = ChainingIterator_Type.tp_new(&ChainingIterator_Type, nullptr, nullptr);
  ((BPy_ChainingIterator *)py_c_it)->c_it = new ChainingIterator(c_it);
  ((BPy_ChainingIterator *)py_c_it)->py_ve_it.py_it.it = ((BPy_ChainingIterator *)py_c_it)->c_it;
  return py_c_it;
}

PyObject *BPy_ChainPredicateIterator_from_ChainPredicateIterator(ChainPredicateIterator &cp_it)
{
  PyObject *py_cp_it = ChainPredicateIterator_Type.tp_new(
      &ChainPredicateIterator_Type, nullptr, nullptr);
  ((BPy_ChainPredicateIterator *)py_cp_it)->cp_it = new ChainPredicateIterator(cp_it);
  ((BPy_ChainPredicateIterator *)py_cp_it)->py_c_it.py_ve_it.py_it.it =
      ((BPy_ChainPredicateIterator *)py_cp_it)->cp_it;
  return py_cp_it;
}

PyObject *BPy_ChainSilhouetteIterator_from_ChainSilhouetteIterator(ChainSilhouetteIterator &cs_it)
{
  PyObject *py_cs_it = ChainSilhouetteIterator_Type.tp_new(
      &ChainSilhouetteIterator_Type, nullptr, nullptr);
  ((BPy_ChainSilhouetteIterator *)py_cs_it)->cs_it = new ChainSilhouetteIterator(cs_it);
  ((BPy_ChainSilhouetteIterator *)py_cs_it)->py_c_it.py_ve_it.py_it.it =
      ((BPy_ChainSilhouetteIterator *)py_cs_it)->cs_it;
  return py_cs_it;
}

//==============================
// Python => C++
//==============================

bool bool_from_PyBool(PyObject *b)
{
  return PyObject_IsTrue(b) != 0;
}

IntegrationType IntegrationType_from_BPy_IntegrationType(PyObject *obj)
{
  return static_cast<IntegrationType>(PyLong_AsLong(obj));
}

Stroke::MediumType MediumType_from_BPy_MediumType(PyObject *obj)
{
  return static_cast<Stroke::MediumType>(PyLong_AsLong(obj));
}

Nature::EdgeNature EdgeNature_from_BPy_Nature(PyObject *obj)
{
  return static_cast<Nature::EdgeNature>(PyLong_AsLong(obj));
}

bool Vec2f_ptr_from_PyObject(PyObject *obj, Vec2f &vec)
{
  if (Vec2f_ptr_from_Vector(obj, vec)) {
    return true;
  }
  if (Vec2f_ptr_from_PyList(obj, vec)) {
    return true;
  }
  if (Vec2f_ptr_from_PyTuple(obj, vec)) {
    return true;
  }
  return false;
}

bool Vec3f_ptr_from_PyObject(PyObject *obj, Vec3f &vec)
{
  if (Vec3f_ptr_from_Vector(obj, vec)) {
    return true;
  }
  if (Vec3f_ptr_from_Color(obj, vec)) {
    return true;
  }
  if (Vec3f_ptr_from_PyList(obj, vec)) {
    return true;
  }
  if (Vec3f_ptr_from_PyTuple(obj, vec)) {
    return true;
  }
  return false;
}

bool Vec3r_ptr_from_PyObject(PyObject *obj, Vec3r &vec)
{
  if (Vec3r_ptr_from_Vector(obj, vec)) {
    return true;
  }
  if (Vec3r_ptr_from_Color(obj, vec)) {
    return true;
  }
  if (Vec3r_ptr_from_PyList(obj, vec)) {
    return true;
  }
  if (Vec3r_ptr_from_PyTuple(obj, vec)) {
    return true;
  }
  return false;
}

bool Vec2f_ptr_from_Vector(PyObject *obj, Vec2f &vec)
{
  if (!VectorObject_Check(obj) || ((VectorObject *)obj)->vec_num != 2) {
    return false;
  }
  if (BaseMath_ReadCallback((BaseMathObject *)obj) == -1) {
    return false;
  }
  vec[0] = ((VectorObject *)obj)->vec[0];
  vec[1] = ((VectorObject *)obj)->vec[1];
  return true;
}

bool Vec3f_ptr_from_Vector(PyObject *obj, Vec3f &vec)
{
  if (!VectorObject_Check(obj) || ((VectorObject *)obj)->vec_num != 3) {
    return false;
  }
  if (BaseMath_ReadCallback((BaseMathObject *)obj) == -1) {
    return false;
  }
  vec[0] = ((VectorObject *)obj)->vec[0];
  vec[1] = ((VectorObject *)obj)->vec[1];
  vec[2] = ((VectorObject *)obj)->vec[2];
  return true;
}

bool Vec3r_ptr_from_Vector(PyObject *obj, Vec3r &vec)
{
  if (!VectorObject_Check(obj) || ((VectorObject *)obj)->vec_num != 3) {
    return false;
  }
  if (BaseMath_ReadCallback((BaseMathObject *)obj) == -1) {
    return false;
  }
  vec[0] = ((VectorObject *)obj)->vec[0];
  vec[1] = ((VectorObject *)obj)->vec[1];
  vec[2] = ((VectorObject *)obj)->vec[2];
  return true;
}

bool Vec3f_ptr_from_Color(PyObject *obj, Vec3f &vec)
{
  if (!ColorObject_Check(obj)) {
    return false;
  }
  if (BaseMath_ReadCallback((BaseMathObject *)obj) == -1) {
    return false;
  }
  vec[0] = ((ColorObject *)obj)->col[0];
  vec[1] = ((ColorObject *)obj)->col[1];
  vec[2] = ((ColorObject *)obj)->col[2];
  return true;
}

bool Vec3r_ptr_from_Color(PyObject *obj, Vec3r &vec)
{
  if (!ColorObject_Check(obj)) {
    return false;
  }
  if (BaseMath_ReadCallback((BaseMathObject *)obj) == -1) {
    return false;
  }
  vec[0] = ((ColorObject *)obj)->col[0];
  vec[1] = ((ColorObject *)obj)->col[1];
  vec[2] = ((ColorObject *)obj)->col[2];
  return true;
}

static bool float_array_from_PyList(PyObject *obj, float *v, int n)
{
  for (int i = 0; i < n; i++) {
    v[i] = PyFloat_AsDouble(PyList_GET_ITEM(obj, i));
    if (v[i] == -1.0f && PyErr_Occurred()) {
      PyErr_SetString(PyExc_TypeError, "list elements must be a number");
      return false;
    }
  }
  return true;
}

bool Vec2f_ptr_from_PyList(PyObject *obj, Vec2f &vec)
{
  float v[2];

  if (!PyList_Check(obj) || PyList_GET_SIZE(obj) != 2) {
    return false;
  }
  if (!float_array_from_PyList(obj, v, 2)) {
    return false;
  }
  vec[0] = v[0];
  vec[1] = v[1];
  return true;
}

bool Vec3f_ptr_from_PyList(PyObject *obj, Vec3f &vec)
{
  float v[3];

  if (!PyList_Check(obj) || PyList_GET_SIZE(obj) != 3) {
    return false;
  }
  if (!float_array_from_PyList(obj, v, 3)) {
    return false;
  }
  vec[0] = v[0];
  vec[1] = v[1];
  vec[2] = v[2];
  return true;
}

bool Vec3r_ptr_from_PyList(PyObject *obj, Vec3r &vec)
{
  float v[3];

  if (!PyList_Check(obj) || PyList_GET_SIZE(obj) != 3) {
    return false;
  }
  if (!float_array_from_PyList(obj, v, 3)) {
    return false;
  }
  vec[0] = v[0];
  vec[1] = v[1];
  vec[2] = v[2];
  return true;
}

static bool float_array_from_PyTuple(PyObject *obj, float *v, int n)
{
  for (int i = 0; i < n; i++) {
    v[i] = PyFloat_AsDouble(PyTuple_GET_ITEM(obj, i));
    if (v[i] == -1.0f && PyErr_Occurred()) {
      PyErr_SetString(PyExc_TypeError, "tuple elements must be a number");
      return false;
    }
  }
  return true;
}

bool Vec2f_ptr_from_PyTuple(PyObject *obj, Vec2f &vec)
{
  float v[2];

  if (!PyTuple_Check(obj) || PyTuple_GET_SIZE(obj) != 2) {
    return false;
  }
  if (!float_array_from_PyTuple(obj, v, 2)) {
    return false;
  }
  vec[0] = v[0];
  vec[1] = v[1];
  return true;
}

bool Vec3f_ptr_from_PyTuple(PyObject *obj, Vec3f &vec)
{
  float v[3];

  if (!PyTuple_Check(obj) || PyTuple_GET_SIZE(obj) != 3) {
    return false;
  }
  if (!float_array_from_PyTuple(obj, v, 3)) {
    return false;
  }
  vec[0] = v[0];
  vec[1] = v[1];
  vec[2] = v[2];
  return true;
}

bool Vec3r_ptr_from_PyTuple(PyObject *obj, Vec3r &vec)
{
  float v[3];

  if (!PyTuple_Check(obj) || PyTuple_GET_SIZE(obj) != 3) {
    return false;
  }
  if (!float_array_from_PyTuple(obj, v, 3)) {
    return false;
  }
  vec[0] = v[0];
  vec[1] = v[1];
  vec[2] = v[2];
  return true;
}

// helpers for argument parsing

bool float_array_from_PyObject(PyObject *obj, float *v, int n)
{
  if (VectorObject_Check(obj) && ((VectorObject *)obj)->vec_num == n) {
    if (BaseMath_ReadCallback((BaseMathObject *)obj) == -1) {
      return false;
    }
    for (int i = 0; i < n; i++) {
      v[i] = ((VectorObject *)obj)->vec[i];
    }
    return true;
  }
  if (ColorObject_Check(obj) && n == 3) {
    if (BaseMath_ReadCallback((BaseMathObject *)obj) == -1) {
      return false;
    }
    for (int i = 0; i < n; i++) {
      v[i] = ((ColorObject *)obj)->col[i];
    }
    return true;
  }
  if (PyList_Check(obj) && PyList_GET_SIZE(obj) == n) {
    return float_array_from_PyList(obj, v, n);
  }
  if (PyTuple_Check(obj) && PyTuple_GET_SIZE(obj) == n) {
    return float_array_from_PyTuple(obj, v, n);
  }
  return false;
}

int convert_v4(PyObject *obj, void *v)
{
  return mathutils_array_parse((float *)v, 4, 4, obj, "Error parsing 4D vector");
}

int convert_v3(PyObject *obj, void *v)
{
  return mathutils_array_parse((float *)v, 3, 3, obj, "Error parsing 3D vector");
}

int convert_v2(PyObject *obj, void *v)
{
  return mathutils_array_parse((float *)v, 2, 2, obj, "Error parsing 2D vector");
}

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
