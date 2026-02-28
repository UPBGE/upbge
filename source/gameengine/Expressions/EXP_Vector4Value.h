/** \file EXP_Vector4Value.h
 *  \ingroup expressions
 */

/*
 * UPBGE: Vector4 value type for the Expressions system.
 *
 * Storage: MT_Vector4.
 * MT_Vector4.h includes MT_Vector3.h which includes MT_Vector2.h,
 * so including this header transitively provides all three MT types.
 *
 * Python: returns a plain tuple(4) — no mathutils.Vector4 exists in
 * Blender's Python layer.
 *
 * MT_Vector4 provides to2d() and to3d() conversion methods which are
 * available for future use by Logic Brick runtime binding.
 */

#pragma once

#include "EXP_Value.h"
#include "MT_Vector4.h"

class EXP_Vector4Value : public EXP_PropValue {
 public:
  EXP_Vector4Value();
  explicit EXP_Vector4Value(const MT_Vector4 &v);
  EXP_Vector4Value(MT_Scalar x, MT_Scalar y, MT_Scalar z, MT_Scalar w);

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
  const MT_Vector4 &GetVector4() const { return m_vec; }
  void              SetVector4(const MT_Vector4 &v) { m_vec = v; }

  // Delegates to MT_Vector4::length2() — avoids sqrt in sensor hot-paths.
  MT_Scalar GetLengthSq() const { return m_vec.length2(); }

#ifdef WITH_PYTHON
  virtual PyObject  *ConvertValueToPython() override;
  // Accepts any sequence of 4 numeric items.
  virtual EXP_Value *ConvertPythonToValue(PyObject *pyobj,
                                          const bool do_type_exception,
                                          const char *error_prefix) override;
#endif

 private:
  MT_Vector4 m_vec;
};
