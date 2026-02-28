/** \file EXP_Vector2Value.h
 *  \ingroup expressions
 */

/*
 * UPBGE: Vector2 value type for the Expressions system.
 *
 * Storage: MT_Vector2 — consistent with EXP_Vector3Value (MT_Vector3)
 * and EXP_Vector4Value (MT_Vector4). All three use the Moto math library.
 *
 * Python: returns a plain tuple(2) — no mathutils.Vector2 exists in
 * Blender's Python layer.
 */

#pragma once

#include "EXP_Value.h"
#include "MT_Vector2.h"

class EXP_Vector2Value : public EXP_PropValue {
 public:
  EXP_Vector2Value();
  explicit EXP_Vector2Value(const MT_Vector2 &v);
  EXP_Vector2Value(MT_Scalar x, MT_Scalar y);

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
  const MT_Vector2 &GetVector2() const { return m_vec; }
  void              SetVector2(const MT_Vector2 &v) { m_vec = v; }

  // Delegates to MT_Vector2::length2() — avoids sqrt in sensor hot-paths.
  MT_Scalar GetLengthSq() const { return m_vec.length2(); }

#ifdef WITH_PYTHON
  virtual PyObject  *ConvertValueToPython() override;
  // Accepts any sequence of 2 numeric items.
  virtual EXP_Value *ConvertPythonToValue(PyObject *pyobj,
                                          const bool do_type_exception,
                                          const char *error_prefix) override;
#endif

 private:
  MT_Vector2 m_vec;
};
