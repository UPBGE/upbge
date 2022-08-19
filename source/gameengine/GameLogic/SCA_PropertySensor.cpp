/*
 * Property sensor
 *
 *
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/SCA_PropertySensor.cpp
 *  \ingroup gamelogic
 */

#include "SCA_PropertySensor.h"

#include <boost/algorithm/string.hpp>

#include "CM_Format.h"
#include "EXP_FloatValue.h"

#include "BLI_compiler_attrs.h"

SCA_PropertySensor::SCA_PropertySensor(SCA_EventManager *eventmgr,
                                       SCA_IObject *gameobj,
                                       const std::string &propname,
                                       const std::string &propval,
                                       const std::string &propmaxval,
                                       KX_PROPSENSOR_TYPE checktype)
    : SCA_ISensor(gameobj, eventmgr),
      m_checktype(checktype),
      m_checkpropval(propval),
      m_checkpropmaxval(propmaxval),
      m_checkpropname(propname)
{
  // EXP_Parser pars;
  // pars.SetContext(this->AddRef());
  // EXP_Value* resultval = m_rightexpr->Calculate();

  EXP_Value *orgprop = GetParent()->FindIdentifier(m_checkpropname);
  if (!orgprop->IsError()) {
    m_previoustext = orgprop->GetText();
  }
  orgprop->Release();

  Init();
}

void SCA_PropertySensor::Init()
{
  m_recentresult = false;
  m_lastresult = m_invert ? true : false;
  m_reset = true;
}

EXP_Value *SCA_PropertySensor::GetReplica()
{
  SCA_PropertySensor *replica = new SCA_PropertySensor(*this);
  // m_range_expr must be recalculated on replica!
  replica->ProcessReplica();
  replica->Init();

  return replica;
}

bool SCA_PropertySensor::IsPositiveTrigger()
{
  bool result = m_recentresult;  // CheckPropertyCondition();
  if (m_invert)
    result = !result;

  return result;
}

SCA_PropertySensor::~SCA_PropertySensor()
{
}

bool SCA_PropertySensor::Evaluate()
{
  bool result = CheckPropertyCondition();
  bool reset = m_reset && m_level;

  m_reset = false;
  if (m_lastresult != result) {
    m_lastresult = result;
    return true;
  }
  return (reset) ? true : false;
}

bool SCA_PropertySensor::CheckPropertyCondition()
{
  m_recentresult = false;
  bool result = false;
  bool reverse = false;
  switch (m_checktype) {
    case KX_PROPSENSOR_NOTEQUAL:
      reverse = true;
      ATTR_FALLTHROUGH;
    case KX_PROPSENSOR_EQUAL: {
      EXP_Value *orgprop = GetParent()->FindIdentifier(m_checkpropname);
      if (!orgprop->IsError()) {
        const std::string &testprop = orgprop->GetText();
        // Force strings to upper case, to avoid confusion in
        // bool tests. It's stupid the prop's identity is lost
        // on the way here...
        if ((testprop == EXP_BoolValue::sTrueString) ||
            (testprop == EXP_BoolValue::sFalseString)) {
          boost::to_upper(m_checkpropval);
        }
        result = (testprop == m_checkpropval);

        /* Patch: floating point values cant use strings usefully since you can have "0.0" ==
         * "0.0000" this could be made into a generic Value class function for comparing values
         * with a string.
         */
        if (result == false && (orgprop->GetValueType() == VALUE_FLOAT_TYPE)) {
          float f;
          if (CM_StringTo(m_checkpropval, f)) {
            result = (f == ((EXP_FloatValue *)orgprop)->GetFloat());
          }
        }
        /* end patch */
      }
      orgprop->Release();

      if (reverse)
        result = !result;
      break;
    }

    case KX_PROPSENSOR_EXPRESSION: {
      break;
    }
    case KX_PROPSENSOR_INTERVAL: {
      EXP_Value *orgprop = GetParent()->FindIdentifier(m_checkpropname);
      if (!orgprop->IsError()) {
        float min;
        float max;
        float val;
        CM_StringTo(m_checkpropval, min);
        CM_StringTo(m_checkpropmaxval, max);

        if (orgprop->GetValueType() == VALUE_STRING_TYPE) {
          CM_StringTo(orgprop->GetText(), val);
        }
        else {
          val = orgprop->GetNumber();
        }

        result = (min <= val) && (val <= max);
      }
      orgprop->Release();

      break;
    }
    case KX_PROPSENSOR_CHANGED: {
      EXP_Value *orgprop = GetParent()->FindIdentifier(m_checkpropname);

      if (!orgprop->IsError()) {
        if (m_previoustext != orgprop->GetText()) {
          m_previoustext = orgprop->GetText();
          result = true;
        }
      }
      orgprop->Release();

      break;
    }
    case KX_PROPSENSOR_LESSTHAN:
      reverse = true;
      ATTR_FALLTHROUGH;
    case KX_PROPSENSOR_GREATERTHAN: {
      EXP_Value *orgprop = GetParent()->FindIdentifier(m_checkpropname);
      if (!orgprop->IsError()) {
        float ref;
        CM_StringTo(m_checkpropval, ref);
        float val;

        if (orgprop->GetValueType() == VALUE_STRING_TYPE) {
          CM_StringTo(orgprop->GetText(), val);
        }
        else {
          val = orgprop->GetNumber();
        }

        if (reverse) {
          result = val < ref;
        }
        else {
          result = val > ref;
        }
      }
      orgprop->Release();

      break;
    }
    default:; /* error */
  }

  // the concept of Edge and Level triggering has unwanted effect for KX_PROPSENSOR_CHANGED
  // see Game Engine bugtracker [ #3809 ]
  m_recentresult = result;

  return result;
}

EXP_Value *SCA_PropertySensor::FindIdentifier(const std::string &identifiername)
{
  return GetParent()->FindIdentifier(identifiername);
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

int SCA_PropertySensor::validValueForProperty(EXP_PyObjectPlus *self, const PyAttributeDef *)
{
  /* If someone actually do type checking please make sure the 'max' and 'min'
   * are checked as well (currently they are calling the PrecalculateRangeExpression
   * function directly */

  /*  There is no type checking at this moment, unfortunately...           */
  return 0;
}

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_PropertySensor::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_PropertySensor",
                                         sizeof(EXP_PyObjectPlus_Proxy),
                                         0,
                                         py_base_dealloc,
                                         0,
                                         0,
                                         0,
                                         0,
                                         py_base_repr,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         Methods,
                                         0,
                                         0,
                                         &SCA_ISensor::Type,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         py_base_new};

PyMethodDef SCA_PropertySensor::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_PropertySensor::Attributes[] = {
    EXP_PYATTRIBUTE_INT_RW("mode",
                           KX_PROPSENSOR_NODEF,
                           KX_PROPSENSOR_MAX - 1,
                           false,
                           SCA_PropertySensor,
                           m_checktype),
    EXP_PYATTRIBUTE_STRING_RW_CHECK(
        "propName", 0, MAX_PROP_NAME, false, SCA_PropertySensor, m_checkpropname, CheckProperty),
    EXP_PYATTRIBUTE_STRING_RW_CHECK(
        "value", 0, 100, false, SCA_PropertySensor, m_checkpropval, validValueForProperty),
    EXP_PYATTRIBUTE_STRING_RW_CHECK(
        "min", 0, 100, false, SCA_PropertySensor, m_checkpropval, validValueForProperty),
    EXP_PYATTRIBUTE_STRING_RW_CHECK(
        "max", 0, 100, false, SCA_PropertySensor, m_checkpropmaxval, validValueForProperty),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON

/* eof */
