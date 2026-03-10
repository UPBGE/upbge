/** \file gameengine/Expressions/EXP_Vector2Value.cpp
 *  \ingroup expressions
 */

#include "EXP_Vector2Value.h"

#include <cstdio>

#include "EXP_BoolValue.h"
#include "EXP_ErrorValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"

using namespace blender;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

EXP_Vector2Value::EXP_Vector2Value() : m_vec(MT_Scalar(0.0f), MT_Scalar(0.0f))
{
}

EXP_Vector2Value::EXP_Vector2Value(const MT_Vector2 &v) : m_vec(v)
{
}

EXP_Vector2Value::EXP_Vector2Value(MT_Scalar x, MT_Scalar y) : m_vec(x, y)
{
}

// ---------------------------------------------------------------------------
// EXP_Value interface
// ---------------------------------------------------------------------------

std::string EXP_Vector2Value::GetText()
{
  char buf[64];
  snprintf(buf, sizeof(buf), "(%.6g, %.6g)", (double)m_vec[0], (double)m_vec[1]);
  return buf;
}

double EXP_Vector2Value::GetNumber()
{
  // Returns vector length. Internal code that only needs magnitude comparison
  // should call GetLengthSq() to avoid this sqrt.
  return (double)m_vec.length();
}

int EXP_Vector2Value::GetValueType()
{
  return VALUE_VECTOR2_TYPE;
}

void EXP_Vector2Value::SetValue(EXP_Value *newval)
{
  const int dtype = newval->GetValueType();
  if (dtype == VALUE_VECTOR2_TYPE) {
    // Same type: direct MT assignment, no allocation.
    m_vec = static_cast<EXP_Vector2Value *>(newval)->GetVector2();
  }
  else {
    // Scalar broadcast: fill both components with the numeric value.
    const MT_Scalar s = (MT_Scalar)newval->GetNumber();
    m_vec.setValue(s, s);
  }
}

EXP_Value *EXP_Vector2Value::GetReplica()
{
  EXP_Vector2Value *replica = new EXP_Vector2Value(*this);
  replica->ProcessReplica();
  return replica;
}

void EXP_Vector2Value::ProcessReplica()
{
  // MT_Vector2 owns no heap pointers — call parent only.
  EXP_PropValue::ProcessReplica();
}

// ---------------------------------------------------------------------------
// Unary operators — Calc()
// ---------------------------------------------------------------------------

EXP_Value *EXP_Vector2Value::Calc(VALUE_OPERATOR op, EXP_Value *val)
{
  switch (op) {
    case VALUE_POS_OPERATOR: {
      return new EXP_Vector2Value(m_vec);
    }
    case VALUE_NEG_OPERATOR: {
      // MT operator-() defined in MT_Vector2_inl.
      return new EXP_Vector2Value(-m_vec);
    }
    case VALUE_NOT_OPERATOR: {
      // True if length is effectively zero — delegates to MT fuzzy test.
      return new EXP_BoolValue(m_vec.fuzzyZero());
    }
    case VALUE_AND_OPERATOR:
    case VALUE_OR_OPERATOR: {
      return new EXP_ErrorValue(op2str(op) + " not defined for Vector2");
    }
    default: {
      // Binary: delegate to right-hand operand's CalcFinal.
      return val->CalcFinal(VALUE_VECTOR2_TYPE, op, this);
    }
  }
}

// ---------------------------------------------------------------------------
// Binary operators — CalcFinal()
//
// Convention (identical to EXP_FloatValue):
//   dtype = type of the LEFT operand (val)
//   this  = RIGHT operand
//   val   = LEFT operand pointer
// ---------------------------------------------------------------------------

EXP_Value *EXP_Vector2Value::CalcFinal(VALUE_DATA_TYPE dtype,
                                        VALUE_OPERATOR op,
                                        EXP_Value *val)
{
  switch (dtype) {

    // ------------------------------------------------------------------
    // Vector2 OP Vector2
    // ------------------------------------------------------------------
    case VALUE_VECTOR2_TYPE: {
      const MT_Vector2 &lhs = static_cast<EXP_Vector2Value *>(val)->GetVector2();

      switch (op) {
        case VALUE_ADD_OPERATOR: {
          // MT operator+ defined in MT_Vector2_inl.
          return new EXP_Vector2Value(lhs + m_vec);
        }
        case VALUE_SUB_OPERATOR: {
          return new EXP_Vector2Value(lhs - m_vec);
        }
        case VALUE_EQL_OPERATOR: {
          // MT_fuzzyEqual: MT_fuzzyZero(lhs - rhs) — uses length2, no sqrt.
          return new EXP_BoolValue(MT_fuzzyEqual(lhs, m_vec));
        }
        case VALUE_NEQ_OPERATOR: {
          return new EXP_BoolValue(!MT_fuzzyEqual(lhs, m_vec));
        }
        case VALUE_GRE_OPERATOR: {
          // Length comparison on squared values — no sqrt needed.
          return new EXP_BoolValue(
              static_cast<EXP_Vector2Value *>(val)->GetLengthSq() > GetLengthSq());
        }
        case VALUE_LES_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector2Value *>(val)->GetLengthSq() < GetLengthSq());
        }
        case VALUE_GEQ_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector2Value *>(val)->GetLengthSq() >= GetLengthSq());
        }
        case VALUE_LEQ_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector2Value *>(val)->GetLengthSq() <= GetLengthSq());
        }
        case VALUE_MUL_OPERATOR:
        case VALUE_DIV_OPERATOR: {
          return new EXP_ErrorValue(
              "Vector2 * Vector2 and Vector2 / Vector2 are not defined. "
              "Use scalar multiplication instead.");
        }
        default: {
          return new EXP_ErrorValue(val->GetText() + op2str(op) + GetText() +
                                    ": operator not valid for Vector2");
        }
      }
    }

    // ------------------------------------------------------------------
    // Scalar OP Vector2 — broadcast scalar component-wise.
    // ------------------------------------------------------------------
    case VALUE_FLOAT_TYPE:
    case VALUE_INT_TYPE: {
      const MT_Scalar s = (dtype == VALUE_FLOAT_TYPE) ?
                              (MT_Scalar)static_cast<EXP_FloatValue *>(val)->GetFloat() :
                              (MT_Scalar)static_cast<EXP_IntValue *>(val)->GetInt();

      switch (op) {
        case VALUE_ADD_OPERATOR: {
          return new EXP_Vector2Value(s + m_vec[0], s + m_vec[1]);
        }
        case VALUE_SUB_OPERATOR: {
          return new EXP_Vector2Value(s - m_vec[0], s - m_vec[1]);
        }
        case VALUE_MUL_OPERATOR: {
          // MT operator*(scalar) defined in MT_Vector2_inl.
          return new EXP_Vector2Value(m_vec * s);
        }
        case VALUE_DIV_OPERATOR: {
          if (m_vec.fuzzyZero()) {
            return new EXP_ErrorValue("Vector2 division: divisor vector is zero");
          }
          return new EXP_Vector2Value(s / m_vec[0], s / m_vec[1]);
        }
        case VALUE_EQL_OPERATOR: {
          // Compare scalar with vector length; both squared to avoid sqrt.
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
                                    ": operator not valid for scalar OP Vector2");
        }
      }
    }

    // ------------------------------------------------------------------
    // Cross-dimension operations: reject with a clear message.
    // ------------------------------------------------------------------
    case VALUE_VECTOR3_TYPE:
    case VALUE_VECTOR4_TYPE: {
      return new EXP_ErrorValue("Cannot mix Vector2 with Vector3/Vector4");
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

PyObject *EXP_Vector2Value::ConvertValueToPython()
{
  // No mathutils.Vector2 — return a plain tuple(2).
  // getValue() returns const MT_Scalar* pointing to internal storage.
  const MT_Scalar *v = m_vec.getValue();
  PyObject *t = PyTuple_New(2);
  PyTuple_SET_ITEM(t, 0, PyFloat_FromDouble((double)v[0]));
  PyTuple_SET_ITEM(t, 1, PyFloat_FromDouble((double)v[1]));
  return t;
}

EXP_Value *EXP_Vector2Value::ConvertPythonToValue(PyObject *pyobj,
                                                   const bool do_type_exception,
                                                   const char *error_prefix)
{
  if (!PySequence_Check(pyobj) || PySequence_Size(pyobj) != 2) {
    if (do_type_exception) {
      PyErr_Format(PyExc_TypeError,
                   "%sexpected a sequence of 2 floats for Vector2",
                   error_prefix);
    }
    return nullptr;
  }

  MT_Scalar v[2];
  for (int i = 0; i < 2; ++i) {
    PyObject *item = PySequence_GetItem(pyobj, i);
    if (!item) {
      return nullptr;
    }
    v[i] = (MT_Scalar)PyFloat_AsDouble(item);
    Py_DECREF(item);
    if (v[i] == MT_Scalar(-1.0f) && PyErr_Occurred()) {
      if (do_type_exception) {
        PyErr_Format(PyExc_TypeError,
                     "%sVector2 component %d is not a number",
                     error_prefix, i);
      }
      return nullptr;
    }
  }
  return new EXP_Vector2Value(v[0], v[1]);
}

#endif  // WITH_PYTHON
