/*
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

/** \file SCA_ILogicBrick.h
 *  \ingroup gamelogic
 */

#pragma once

#include "EXP_BoolValue.h"
#include "EXP_Value.h"
#include "SCA_IObject.h"

class KX_NetworkMessageScene;
class SCA_IScene;
class SCA_LogicManager;

class SCA_ILogicBrick : public EXP_Value, public SG_QList {
  Py_Header protected : SCA_IObject *m_gameobj;
  SCA_LogicManager *m_logicManager;
  int m_Execute_Priority;
  int m_Execute_Ueber_Priority;

  bool m_bActive;
  EXP_Value *m_eventval;
  std::string m_name;
  // unsigned long		m_drawcolor;
  void RemoveEvent();

 public:
  SCA_ILogicBrick(SCA_IObject *gameobj);
  virtual ~SCA_ILogicBrick();

  void SetExecutePriority(int execute_Priority);
  void SetUeberExecutePriority(int execute_Priority);

  SCA_IObject *GetParent()
  {
    return m_gameobj;
  }

  virtual void ReParent(SCA_IObject *parent);
  virtual void Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map);
  virtual void Delete()
  {
    Release();
  }

  virtual std::string GetName();
  virtual void SetName(const std::string &name);

  bool IsActive()
  {
    return m_bActive;
  }

  void SetActive(bool active)
  {
    m_bActive = active;
  }

  // insert in a QList at position corresponding to m_Execute_Priority
  void InsertActiveQList(SG_QList &head)
  {
    SG_QList::iterator<SCA_ILogicBrick> it(head);
    for (it.begin(); !it.end() && m_Execute_Priority > (*it)->m_Execute_Priority; ++it)
      ;
    it.add_back(this);
  }

  // insert in a QList at position corresponding to m_Execute_Priority
  // inside a longer list that contains elements of other objects.
  // Sorting is done only between the elements of the same object.
  // head is the head of the combined list
  // current points to the first element of the object in the list, nullptr if none yet
  void InsertSelfActiveQList(SG_QList &head, SG_QList **current)
  {
    if (!*current) {
      // first element can be put anywhere
      head.QAddBack(this);
      *current = this;
      return;
    }
    // note: we assume current points actually to one o our element, skip the tests
    SG_QList::iterator<SCA_ILogicBrick> it(head, *current);
    if (m_Execute_Priority <= (*it)->m_Execute_Priority) {
      // this element comes before the first
      *current = this;
    }
    else {
      for (++it; !it.end() && (*it)->m_gameobj == m_gameobj &&
                 m_Execute_Priority > (*it)->m_Execute_Priority;
           ++it)
        ;
    }
    it.add_back(this);
  }

  virtual void SetLogicManager(SCA_LogicManager *logicmgr);
  SCA_LogicManager *GetLogicManager();

  /* for moving logic bricks between scenes */
  virtual void Replace_IScene(SCA_IScene *val)
  {
  }
  virtual void Replace_NetworkScene(KX_NetworkMessageScene *val)
  {
  }

#ifdef WITH_PYTHON
  // python methods

  static PyObject *pyattr_get_owner(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

  // check that attribute is a property
  static int CheckProperty(EXP_PyObjectPlus *self, const PyAttributeDef *attrdef);

  enum KX_BOOL_TYPE { KX_BOOL_NODEF = 0, KX_TRUE, KX_FALSE, KX_BOOL_MAX };
#endif /* WITH_PYTHON */
};
