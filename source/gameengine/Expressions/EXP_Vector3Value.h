/** \file EXP_Vector3Value.h
 *  \ingroup expressions
 */

/*
 * UPBGE: Vector3 value type for the Expressions system.
 *
 * Storage: MT_Vector3.
 * MT_Vector3.h is already pulled in by EXP_PyObjectPlus.h, so no
 * additional include cost at the translation unit level.
 *
 * Python: returns mathutils.Vector(3) when USE_MATHUTILS is defined
 * (standard for all MT_Vector3-backed attributes in the engine).
 * MT_Vector3::getValue() returns const MT_Scalar* accepted directly
 * by Vector_CreatePyObject — zero intermediate copies.
 */

#pragma once

#include "EXP_Value.h"
#include "MT_Vector3.h"

class EXP_Vector3Value : public EXP_PropValue {
 public:
  EXP_Vector3Value();
  explicit EXP_Vector3Value(const MT_Vector3 &v);
  EXP_Vector3Value(MT_Scalar x, MT_Scalar y, MT_Scalar z);

  // EXP_Value interface.
  virtual std::string  GetText() override;
  virtual double       GetNumber() override;
  virtual int          GetValueType() override;
  virtual void         SetValue(EXP_Value *newval) override;
  virtual EXP_Value   *GetReplica() override;
  virtual void         ProcessReplica() override;

  virtual EXP_Value   *Calc(VALUE_OPERATOR op, EXP_Value *val) override;
  virtual EXP_Value   *CalcFinal(VALUE_DATA_TYPE dtype,
                                  VALUE_OPERATOR op,
                                  EXP_Value *val) override;

  // Direct accessors — inline, zero overhead.
  const MT_Vector3 &GetVector3() const { return m_vec; }
  void              SetVector3(const MT_Vector3 &v) { m_vec = v; }

  // Delegates to MT_Vector3::length2() — avoids sqrt in sensor hot-paths.
  MT_Scalar GetLengthSq() const { return m_vec.length2(); }

#ifdef WITH_PYTHON
  virtual PyObject  *ConvertValueToPython() override;
  // Accepts mathutils.Vector(3) (fast path) or any sequence of 3 floats.
  virtual EXP_Value *ConvertPythonToValue(PyObject *pyobj,
                                          const bool do_type_exception,
                                          const char *error_prefix) override;
#endif

 private:
  MT_Vector3 m_vec;
};
