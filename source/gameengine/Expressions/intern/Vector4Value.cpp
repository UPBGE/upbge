/** \file gameengine/Expressions/EXP_Vector4Value.cpp
 *  \ingroup expressions
 */

#include "EXP_Vector4Value.h"

#include <cstdio>

#include "EXP_BoolValue.h"
#include "EXP_ErrorValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"

using namespace blender;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

EXP_Vector4Value::EXP_Vector4Value()
    : m_vec(MT_Scalar(0.0f), MT_Scalar(0.0f), MT_Scalar(0.0f), MT_Scalar(0.0f))
{
}

EXP_Vector4Value::EXP_Vector4Value(const MT_Vector4 &v) : m_vec(v)
{
}

EXP_Vector4Value::EXP_Vector4Value(MT_Scalar x, MT_Scalar y, MT_Scalar z, MT_Scalar w)
    : m_vec(x, y, z, w)
{
}

// ---------------------------------------------------------------------------
// EXP_Value interface
// ---------------------------------------------------------------------------

std::string EXP_Vector4Value::GetText()
{
  char buf[128];
  snprintf(buf, sizeof(buf), "(%.6g, %.6g, %.6g, %.6g)",
           (double)m_vec[0], (double)m_vec[1], (double)m_vec[2], (double)m_vec[3]);
  return buf;
}

double EXP_Vector4Value::GetNumber()
{
  return (double)m_vec.length();
}

int EXP_Vector4Value::GetValueType()
{
  return VALUE_VECTOR4_TYPE;
}

void EXP_Vector4Value::SetValue(EXP_Value *newval)
{
  const int dtype = newval->GetValueType();
  if (dtype == VALUE_VECTOR4_TYPE) {
    m_vec = static_cast<EXP_Vector4Value *>(newval)->GetVector4();
  }
  else {
    const MT_Scalar s = (MT_Scalar)newval->GetNumber();
    m_vec.setValue(s, s, s, s);
  }
}

EXP_Value *EXP_Vector4Value::GetReplica()
{
  EXP_Vector4Value *replica = new EXP_Vector4Value(*this);
  replica->ProcessReplica();
  return replica;
}

void EXP_Vector4Value::ProcessReplica()
{
  EXP_PropValue::ProcessReplica();
}

// ---------------------------------------------------------------------------
// Unary operators — Calc()
// ---------------------------------------------------------------------------

EXP_Value *EXP_Vector4Value::Calc(VALUE_OPERATOR op, EXP_Value *val)
{
  switch (op) {
    case VALUE_POS_OPERATOR: {
      return new EXP_Vector4Value(m_vec);
    }
    case VALUE_NEG_OPERATOR: {
      return new EXP_Vector4Value(-m_vec);
    }
    case VALUE_NOT_OPERATOR: {
      return new EXP_BoolValue(m_vec.fuzzyZero());
    }
    case VALUE_AND_OPERATOR:
    case VALUE_OR_OPERATOR: {
      return new EXP_ErrorValue(op2str(op) + " not defined for Vector4");
    }
    default: {
      return val->CalcFinal(VALUE_VECTOR4_TYPE, op, this);
    }
  }
}

// ---------------------------------------------------------------------------
// Binary operators — CalcFinal()
// ---------------------------------------------------------------------------

EXP_Value *EXP_Vector4Value::CalcFinal(VALUE_DATA_TYPE dtype,
                                        VALUE_OPERATOR op,
                                        EXP_Value *val)
{
  switch (dtype) {

    // ------------------------------------------------------------------
    // Vector4 OP Vector4
    // ------------------------------------------------------------------
    case VALUE_VECTOR4_TYPE: {
      const MT_Vector4 &lhs = static_cast<EXP_Vector4Value *>(val)->GetVector4();

      switch (op) {
        case VALUE_ADD_OPERATOR: {
          return new EXP_Vector4Value(lhs + m_vec);
        }
        case VALUE_SUB_OPERATOR: {
          return new EXP_Vector4Value(lhs - m_vec);
        }
        case VALUE_EQL_OPERATOR: {
          return new EXP_BoolValue(MT_fuzzyEqual(lhs, m_vec));
        }
        case VALUE_NEQ_OPERATOR: {
          return new EXP_BoolValue(!MT_fuzzyEqual(lhs, m_vec));
        }
        case VALUE_GRE_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector4Value *>(val)->GetLengthSq() > GetLengthSq());
        }
        case VALUE_LES_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector4Value *>(val)->GetLengthSq() < GetLengthSq());
        }
        case VALUE_GEQ_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector4Value *>(val)->GetLengthSq() >= GetLengthSq());
        }
        case VALUE_LEQ_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector4Value *>(val)->GetLengthSq() <= GetLengthSq());
        }
        case VALUE_MUL_OPERATOR:
        case VALUE_DIV_OPERATOR: {
          return new EXP_ErrorValue(
              "Vector4 * Vector4 and Vector4 / Vector4 are not defined.");
        }
        default: {
          return new EXP_ErrorValue(val->GetText() + op2str(op) + GetText() +
                                    ": operator not valid for Vector4");
        }
      }
    }

    // ------------------------------------------------------------------
    // Scalar OP Vector4
    // ------------------------------------------------------------------
    case VALUE_FLOAT_TYPE:
    case VALUE_INT_TYPE: {
      const MT_Scalar s = (dtype == VALUE_FLOAT_TYPE) ?
                              (MT_Scalar)static_cast<EXP_FloatValue *>(val)->GetFloat() :
                              (MT_Scalar)static_cast<EXP_IntValue *>(val)->GetInt();

      switch (op) {
        case VALUE_ADD_OPERATOR: {
          return new EXP_Vector4Value(s + m_vec[0], s + m_vec[1],
                                      s + m_vec[2], s + m_vec[3]);
        }
        case VALUE_SUB_OPERATOR: {
          return new EXP_Vector4Value(s - m_vec[0], s - m_vec[1],
                                      s - m_vec[2], s - m_vec[3]);
        }
        case VALUE_MUL_OPERATOR: {
          return new EXP_Vector4Value(m_vec * s);
        }
        case VALUE_DIV_OPERATOR: {
          if (m_vec.fuzzyZero()) {
            return new EXP_ErrorValue("Vector4 division: divisor vector is zero");
          }
          return new EXP_Vector4Value(s / m_vec[0], s / m_vec[1],
                                      s / m_vec[2], s / m_vec[3]);
        }
        case VALUE_EQL_OPERATOR: {
          return new EXP_BoolValue(MT_fuzzyZero(GetLengthSq() - s * s));
        }
        case VALUE_NEQ_OPERATOR: {
          return new EXP_BoolValue(!MT_fuzzyZero(GetLengthSq() - s * s));
        }
        case VALUE_GRE_OPERATOR: {
          return new EXP_BoolValue((double)s > GetNumber());
        }
        case VALUE_LES_OPERATOR: {
          return new EXP_BoolValue((double)s < GetNumber());
        }
        case VALUE_GEQ_OPERATOR: {
          return new EXP_BoolValue((double)s >= GetNumber());
        }
        case VALUE_LEQ_OPERATOR: {
          return new EXP_BoolValue((double)s <= GetNumber());
        }
        default: {
          return new EXP_ErrorValue(val->GetText() + op2str(op) + GetText() +
                                    ": operator not valid for scalar OP Vector4");
        }
      }
    }

    // ------------------------------------------------------------------
    // Cross-dimension operations.
    // ------------------------------------------------------------------
    case VALUE_VECTOR2_TYPE:
    case VALUE_VECTOR3_TYPE: {
      return new EXP_ErrorValue("Cannot mix Vector4 with Vector2/Vector3");
    }

    case VALUE_ERROR_TYPE: {
      return new EXP_ErrorValue(val->GetText() + op2str(op) + GetText());
    }

    default: {
      return new EXP_ErrorValue("[type mismatch] " + op2str(op) + GetText());
    }
  }
}

// ---------------------------------------------------------------------------
// Python
// ---------------------------------------------------------------------------

#ifdef WITH_PYTHON

PyObject *EXP_Vector4Value::ConvertValueToPython()
{
  // No mathutils.Vector4 — return a plain tuple(4).
  const MT_Scalar *v = m_vec.getValue();
  PyObject *t = PyTuple_New(4);
  PyTuple_SET_ITEM(t, 0, PyFloat_FromDouble((double)v[0]));
  PyTuple_SET_ITEM(t, 1, PyFloat_FromDouble((double)v[1]));
  PyTuple_SET_ITEM(t, 2, PyFloat_FromDouble((double)v[2]));
  PyTuple_SET_ITEM(t, 3, PyFloat_FromDouble((double)v[3]));
  return t;
}

EXP_Value *EXP_Vector4Value::ConvertPythonToValue(PyObject *pyobj,
                                                   const bool do_type_exception,
                                                   const char *error_prefix)
{
  if (!PySequence_Check(pyobj) || PySequence_Size(pyobj) != 4) {
    if (do_type_exception) {
      PyErr_Format(PyExc_TypeError,
                   "%sexpected a sequence of 4 floats for Vector4",
                   error_prefix);
    }
    return nullptr;
  }

  MT_Scalar v[4];
  for (int i = 0; i < 4; ++i) {
    PyObject *item = PySequence_GetItem(pyobj, i);
    if (!item) {
      return nullptr;
    }
    v[i] = (MT_Scalar)PyFloat_AsDouble(item);
    Py_DECREF(item);
    if (v[i] == MT_Scalar(-1.0f) && PyErr_Occurred()) {
      if (do_type_exception) {
        PyErr_Format(PyExc_TypeError,
                     "%sVector4 component %d is not a number",
                     error_prefix, i);
      }
      return nullptr;
    }
  }
  return new EXP_Vector4Value(v[0], v[1], v[2], v[3]);
}

#endif  // WITH_PYTHON
