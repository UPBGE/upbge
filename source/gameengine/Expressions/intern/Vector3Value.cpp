/** \file gameengine/Expressions/EXP_Vector3Value.cpp
 *  \ingroup expressions
 */

#include "EXP_Vector3Value.h"

#include <cstdio>

#include "EXP_BoolValue.h"
#include "EXP_ErrorValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"

#ifdef WITH_PYTHON
#  ifdef USE_MATHUTILS
#    include "../../blender/python/mathutils/mathutils.hh"
#  endif
#endif

using namespace blender;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

EXP_Vector3Value::EXP_Vector3Value()
    : m_vec(MT_Scalar(0.0f), MT_Scalar(0.0f), MT_Scalar(0.0f))
{
}

EXP_Vector3Value::EXP_Vector3Value(const MT_Vector3 &v) : m_vec(v)
{
}

EXP_Vector3Value::EXP_Vector3Value(MT_Scalar x, MT_Scalar y, MT_Scalar z) : m_vec(x, y, z)
{
}

// ---------------------------------------------------------------------------
// EXP_Value interface
// ---------------------------------------------------------------------------

std::string EXP_Vector3Value::GetText()
{
  char buf[96];
  snprintf(buf, sizeof(buf), "(%.6g, %.6g, %.6g)",
           (double)m_vec[0], (double)m_vec[1], (double)m_vec[2]);
  return buf;
}

double EXP_Vector3Value::GetNumber()
{
  return (double)m_vec.length();
}

int EXP_Vector3Value::GetValueType()
{
  return VALUE_VECTOR3_TYPE;
}

void EXP_Vector3Value::SetValue(EXP_Value *newval)
{
  const int dtype = newval->GetValueType();
  if (dtype == VALUE_VECTOR3_TYPE) {
    m_vec = static_cast<EXP_Vector3Value *>(newval)->GetVector3();
  }
  else {
    const MT_Scalar s = (MT_Scalar)newval->GetNumber();
    m_vec.setValue(s, s, s);
  }
}

EXP_Value *EXP_Vector3Value::GetReplica()
{
  EXP_Vector3Value *replica = new EXP_Vector3Value(*this);
  replica->ProcessReplica();
  return replica;
}

void EXP_Vector3Value::ProcessReplica()
{
  EXP_PropValue::ProcessReplica();
}

// ---------------------------------------------------------------------------
// Unary operators — Calc()
// ---------------------------------------------------------------------------

EXP_Value *EXP_Vector3Value::Calc(VALUE_OPERATOR op, EXP_Value *val)
{
  switch (op) {
    case VALUE_POS_OPERATOR: {
      return new EXP_Vector3Value(m_vec);
    }
    case VALUE_NEG_OPERATOR: {
      return new EXP_Vector3Value(-m_vec);
    }
    case VALUE_NOT_OPERATOR: {
      return new EXP_BoolValue(m_vec.fuzzyZero());
    }
    case VALUE_AND_OPERATOR:
    case VALUE_OR_OPERATOR: {
      return new EXP_ErrorValue(op2str(op) + " not defined for Vector3");
    }
    default: {
      return val->CalcFinal(VALUE_VECTOR3_TYPE, op, this);
    }
  }
}

// ---------------------------------------------------------------------------
// Binary operators — CalcFinal()
// ---------------------------------------------------------------------------

EXP_Value *EXP_Vector3Value::CalcFinal(VALUE_DATA_TYPE dtype,
                                        VALUE_OPERATOR op,
                                        EXP_Value *val)
{
  switch (dtype) {

    // ------------------------------------------------------------------
    // Vector3 OP Vector3
    // ------------------------------------------------------------------
    case VALUE_VECTOR3_TYPE: {
      const MT_Vector3 &lhs = static_cast<EXP_Vector3Value *>(val)->GetVector3();

      switch (op) {
        case VALUE_ADD_OPERATOR: {
          return new EXP_Vector3Value(lhs + m_vec);
        }
        case VALUE_SUB_OPERATOR: {
          return new EXP_Vector3Value(lhs - m_vec);
        }
        case VALUE_EQL_OPERATOR: {
          return new EXP_BoolValue(MT_fuzzyEqual(lhs, m_vec));
        }
        case VALUE_NEQ_OPERATOR: {
          return new EXP_BoolValue(!MT_fuzzyEqual(lhs, m_vec));
        }
        case VALUE_GRE_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector3Value *>(val)->GetLengthSq() > GetLengthSq());
        }
        case VALUE_LES_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector3Value *>(val)->GetLengthSq() < GetLengthSq());
        }
        case VALUE_GEQ_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector3Value *>(val)->GetLengthSq() >= GetLengthSq());
        }
        case VALUE_LEQ_OPERATOR: {
          return new EXP_BoolValue(
              static_cast<EXP_Vector3Value *>(val)->GetLengthSq() <= GetLengthSq());
        }
        case VALUE_MUL_OPERATOR:
        case VALUE_DIV_OPERATOR: {
          return new EXP_ErrorValue(
              "Vector3 * Vector3 and Vector3 / Vector3 are not defined. "
              "Dot/cross product is not available at this abstraction level.");
        }
        default: {
          return new EXP_ErrorValue(val->GetText() + op2str(op) + GetText() +
                                    ": operator not valid for Vector3");
        }
      }
    }

    // ------------------------------------------------------------------
    // Scalar OP Vector3
    // ------------------------------------------------------------------
    case VALUE_FLOAT_TYPE:
    case VALUE_INT_TYPE: {
      const MT_Scalar s = (dtype == VALUE_FLOAT_TYPE) ?
                              (MT_Scalar)static_cast<EXP_FloatValue *>(val)->GetFloat() :
                              (MT_Scalar)static_cast<EXP_IntValue *>(val)->GetInt();

      switch (op) {
        case VALUE_ADD_OPERATOR: {
          return new EXP_Vector3Value(s + m_vec[0], s + m_vec[1], s + m_vec[2]);
        }
        case VALUE_SUB_OPERATOR: {
          return new EXP_Vector3Value(s - m_vec[0], s - m_vec[1], s - m_vec[2]);
        }
        case VALUE_MUL_OPERATOR: {
          return new EXP_Vector3Value(m_vec * s);
        }
        case VALUE_DIV_OPERATOR: {
          if (m_vec.fuzzyZero()) {
            return new EXP_ErrorValue("Vector3 division: divisor vector is zero");
          }
          return new EXP_Vector3Value(s / m_vec[0], s / m_vec[1], s / m_vec[2]);
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
                                    ": operator not valid for scalar OP Vector3");
        }
      }
    }

    // ------------------------------------------------------------------
    // Cross-dimension operations.
    // ------------------------------------------------------------------
    case VALUE_VECTOR2_TYPE:
    case VALUE_VECTOR4_TYPE: {
      return new EXP_ErrorValue("Cannot mix Vector3 with Vector2/Vector4");
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

PyObject *EXP_Vector3Value::ConvertValueToPython()
{
#  ifdef USE_MATHUTILS
  // getValue() returns const MT_Scalar* — accepted directly by
  // Vector_CreatePyObject (const float* prototype), zero intermediate copy.
  return Vector_CreatePyObject(m_vec.getValue(), 3, nullptr);
#  else
  const MT_Scalar *v = m_vec.getValue();
  PyObject *t = PyTuple_New(3);
  PyTuple_SET_ITEM(t, 0, PyFloat_FromDouble((double)v[0]));
  PyTuple_SET_ITEM(t, 1, PyFloat_FromDouble((double)v[1]));
  PyTuple_SET_ITEM(t, 2, PyFloat_FromDouble((double)v[2]));
  return t;
#  endif
}

EXP_Value *EXP_Vector3Value::ConvertPythonToValue(PyObject *pyobj,
                                                   const bool do_type_exception,
                                                   const char *error_prefix)
{
#  ifdef USE_MATHUTILS
  // Fast path: accept mathutils.Vector of size 3.
  if (VectorObject_Check(pyobj)) {
    VectorObject *vec = (VectorObject *)pyobj;
    if (vec->vec_num == 3) {
      if (BaseMath_ReadCallback(vec) == -1) {
        return nullptr;
      }
      return new EXP_Vector3Value(
          (MT_Scalar)vec->vec[0], (MT_Scalar)vec->vec[1], (MT_Scalar)vec->vec[2]);
    }
    if (do_type_exception) {
      PyErr_Format(PyExc_TypeError,
                   "%sexpected a mathutils.Vector of size 3, got size %d",
                   error_prefix, vec->vec_num);
    }
    return nullptr;
  }
#  endif

  // Fallback: accept any sequence of 3 floats.
  if (!PySequence_Check(pyobj) || PySequence_Size(pyobj) != 3) {
    if (do_type_exception) {
      PyErr_Format(PyExc_TypeError,
                   "%sexpected a sequence of 3 floats for Vector3",
                   error_prefix);
    }
    return nullptr;
  }

  MT_Scalar v[3];
  for (int i = 0; i < 3; ++i) {
    PyObject *item = PySequence_GetItem(pyobj, i);
    if (!item) {
      return nullptr;
    }
    v[i] = (MT_Scalar)PyFloat_AsDouble(item);
    Py_DECREF(item);
    if (v[i] == MT_Scalar(-1.0f) && PyErr_Occurred()) {
      if (do_type_exception) {
        PyErr_Format(PyExc_TypeError,
                     "%sVector3 component %d is not a number",
                     error_prefix, i);
      }
      return nullptr;
    }
  }
  return new EXP_Vector3Value(v[0], v[1], v[2]);
}

#endif  // WITH_PYTHON
