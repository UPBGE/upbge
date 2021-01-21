/*
 * Assign, change, copy properties
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

/** \file gameengine/GameLogic/SCA_PropertyActuator.cpp
 *  \ingroup gamelogic
 */

#include "SCA_PropertyActuator.h"

#include "EXP_ConstExpr.h"
#include "EXP_InputParser.h"
#include "EXP_Operator2Expr.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_PropertyActuator::SCA_PropertyActuator(SCA_IObject *gameobj,
                                           SCA_IObject *sourceObj,
                                           const std::string &propname,
                                           const std::string &expr,
                                           int acttype)
    : SCA_IActuator(gameobj, KX_ACT_PROPERTY),
      m_type(acttype),
      m_propname(propname),
      m_exprtxt(expr),
      m_sourceObj(sourceObj)
{
  // protect ourselves against someone else deleting the source object
  // don't protect against ourselves: it would create a dead lock
  if (m_sourceObj)
    m_sourceObj->RegisterActuator(this);
}

SCA_PropertyActuator::~SCA_PropertyActuator()
{
  if (m_sourceObj)
    m_sourceObj->UnregisterActuator(this);
}

bool SCA_PropertyActuator::Update()
{
  bool result = false;

  bool bNegativeEvent = IsNegativeEvent();
  RemoveAllEvents();
  EXP_Value *propowner = GetParent();

  if (bNegativeEvent) {
    if (m_type == KX_ACT_PROP_LEVEL) {
      EXP_Value *newval = new EXP_BoolValue(false);
      EXP_Value *oldprop = propowner->GetProperty(m_propname);
      if (oldprop) {
        oldprop->SetValue(newval);
      }
      newval->Release();
    }
    return false;
  }

  EXP_Parser parser;
  parser.SetContext(propowner->AddRef());

  EXP_Expression *userexpr = nullptr;

  if (m_type == KX_ACT_PROP_TOGGLE) {
    /* don't use */
    EXP_Value *newval;
    EXP_Value *oldprop = propowner->GetProperty(m_propname);
    if (oldprop) {
      newval = new EXP_BoolValue((oldprop->GetNumber() == 0.0) ? true : false);
      oldprop->SetValue(newval);
    }
    else { /* as not been assigned, evaluate as false, so assign true */
      newval = new EXP_BoolValue(true);
      propowner->SetProperty(m_propname, newval);
    }
    newval->Release();
  }
  else if (m_type == KX_ACT_PROP_LEVEL) {
    EXP_Value *newval = new EXP_BoolValue(true);
    EXP_Value *oldprop = propowner->GetProperty(m_propname);
    if (oldprop) {
      oldprop->SetValue(newval);
    }
    else {
      propowner->SetProperty(m_propname, newval);
    }
    newval->Release();
  }
  else if ((userexpr = parser.ProcessText(m_exprtxt))) {
    switch (m_type) {

      case KX_ACT_PROP_ASSIGN: {

        EXP_Value *newval = userexpr->Calculate();
        EXP_Value *oldprop = propowner->GetProperty(m_propname);
        if (oldprop) {
          oldprop->SetValue(newval);
        }
        else {
          propowner->SetProperty(m_propname, newval);
        }
        newval->Release();
        break;
      }
      case KX_ACT_PROP_ADD: {
        EXP_Value *oldprop = propowner->GetProperty(m_propname);
        if (oldprop) {
          // int waarde = (int)oldprop->GetNumber();  /*unused*/
          EXP_Expression *expr = new EXP_Operator2Expr(
              VALUE_ADD_OPERATOR, new EXP_ConstExpr(oldprop->AddRef()), userexpr->AddRef());

          EXP_Value *newprop = expr->Calculate();
          oldprop->SetValue(newprop);
          newprop->Release();
          expr->Release();
        }

        break;
      }
      case KX_ACT_PROP_COPY: {
        if (m_sourceObj) {
          EXP_Value *copyprop = m_sourceObj->GetProperty(m_exprtxt);
          if (copyprop) {
            EXP_Value *val = copyprop->GetReplica();
            GetParent()->SetProperty(m_propname, val);
            val->Release();
          }
        }
        break;
      }
      /* case KX_ACT_PROP_TOGGLE: */ /* accounted for above, no need for userexpr */
      default: {
      }
    }

    userexpr->Release();
  }

  return result;
}

EXP_Value *

SCA_PropertyActuator::

    GetReplica()
{

  SCA_PropertyActuator *replica = new SCA_PropertyActuator(*this);

  replica->ProcessReplica();
  return replica;
};

void SCA_PropertyActuator::ProcessReplica()
{
  // no need to check for self reference like in the constructor:
  // the replica will always have a different parent
  if (m_sourceObj)
    m_sourceObj->RegisterActuator(this);
  SCA_IActuator::ProcessReplica();
}

bool SCA_PropertyActuator::UnlinkObject(SCA_IObject *clientobj)
{
  if (clientobj == m_sourceObj) {
    // this object is being deleted, we cannot continue to track it.
    m_sourceObj = nullptr;
    return true;
  }
  return false;
}

void SCA_PropertyActuator::Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map)
{
  SCA_IObject *obj = obj_map[m_sourceObj];
  if (obj) {
    if (m_sourceObj)
      m_sourceObj->UnregisterActuator(this);
    m_sourceObj = obj;
    m_sourceObj->RegisterActuator(this);
  }
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_PropertyActuator::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "SCA_PropertyActuator",
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
    &SCA_IActuator::Type,
    0,
    0,
    0,
    0,
    0,
    0,
    py_base_new};

PyMethodDef SCA_PropertyActuator::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_PropertyActuator::Attributes[] = {
    EXP_PYATTRIBUTE_STRING_RW_CHECK(
        "propName", 0, MAX_PROP_NAME, false, SCA_PropertyActuator, m_propname, CheckProperty),
    EXP_PYATTRIBUTE_STRING_RW("value", 0, 100, false, SCA_PropertyActuator, m_exprtxt),
    EXP_PYATTRIBUTE_INT_RW("mode",
                           KX_ACT_PROP_NODEF + 1,
                           KX_ACT_PROP_MAX - 1,
                           false,
                           SCA_PropertyActuator,
                           m_type), /* ATTR_TODO add constents to game logic dict */
    EXP_PYATTRIBUTE_NULL            // Sentinel
};

#endif

/* eof */
