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
 * Contributor(s): Geoffrey Gollmer, Jorge Bernal
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#pragma once

#include "SCA_IActuator.h"

class KX_KetsjiEngine;
class SCA_MouseManager;
class SCA_IInputDevice;
class RAS_ICanvas;

class SCA_MouseActuator : public SCA_IActuator {
  Py_Header

      private :

      KX_KetsjiEngine *m_ketsji;
  SCA_MouseManager *m_eventmgr;
  SCA_IInputDevice *m_mouse;
  RAS_ICanvas *m_canvas;
  int m_type;
  bool m_initialSkipping;

  bool m_visible;

  bool m_use_axis_x; /* 0 for calculate axis, 1 for ignore axis */
  bool m_use_axis_y;
  float m_threshold[2];
  bool m_reset_x; /* 0=reset, 1=free */
  bool m_reset_y;
  int m_object_axis[2]; /* 0=x, 1=y, 2=z */
  bool m_local_x;       /* 0=local, 1=global*/
  bool m_local_y;
  float m_sensitivity[2];
  float m_limit_x[2];
  float m_limit_y[2];

  float m_oldposition[2];
  float m_angle[2];

 public:
  enum KX_ACT_MOUSE_OBJECT_AXIS {
    KX_ACT_MOUSE_OBJECT_AXIS_X = 0,
    KX_ACT_MOUSE_OBJECT_AXIS_Y,
    KX_ACT_MOUSE_OBJECT_AXIS_Z
  };

  enum KX_ACT_MOUSE_MODE {
    KX_ACT_MOUSE_NODEF = 0,
    KX_ACT_MOUSE_VISIBILITY,
    KX_ACT_MOUSE_LOOK,
    KX_ACT_MOUSE_MAX
  };

  SCA_MouseActuator(SCA_IObject *gameobj,
                    KX_KetsjiEngine *ketsjiEngine,
                    SCA_MouseManager *eventmgr,
                    int acttype,
                    bool visible,
                    bool *use_axis,
                    float *threshold,
                    bool *reset,
                    int *object_axis,
                    bool *local,
                    float *sensitivity,
                    float *limit_x,
                    float *limit_y);

  ~SCA_MouseActuator();

  EXP_Value *GetReplica();
  virtual void ProcessReplica();

  virtual void Replace_IScene(SCA_IScene *scene);

  virtual bool Update();

  virtual void getMousePosition(float *);
  virtual void setMousePosition(float, float);

#ifdef WITH_PYTHON

  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  /* Methods */

  EXP_PYMETHOD_DOC_NOARGS(SCA_MouseActuator, Reset);

  /* Attributes */

  static PyObject *pyattr_get_limit_x(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_limit_x(EXP_PyObjectPlus *self_v,
                                const EXP_PYATTRIBUTE_DEF *attrdef,
                                PyObject *value);

  static PyObject *pyattr_get_limit_y(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_limit_y(EXP_PyObjectPlus *self_v,
                                const EXP_PYATTRIBUTE_DEF *attrdef,
                                PyObject *value);

  static PyObject *pyattr_get_angle(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_angle(EXP_PyObjectPlus *self_v,
                              const EXP_PYATTRIBUTE_DEF *attrdef,
                              PyObject *value);
#endif /* WITH_PYTHON */
};
