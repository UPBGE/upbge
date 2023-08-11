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

#include "SCA_MouseActuator.h"

#include "BLI_math_rotation.h"

#include "KX_GameObject.h"
#include "KX_PyMath.h"
#include "RAS_ICanvas.h"
#include "SCA_MouseManager.h"

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

/* Global variables to assure that the events are analyzed by all mouse actuators */
unsigned int mouact_total = 0;
unsigned int mouact_count = 0;

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_MouseActuator::SCA_MouseActuator(SCA_IObject *gameobj,

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
                                     float *limit_y)
    : SCA_IActuator(gameobj, KX_ACT_MOUSE),
      m_ketsji(ketsjiEngine),
      m_eventmgr(eventmgr),
      m_type(acttype),
      m_visible(visible),
      m_use_axis_x(use_axis[0]),
      m_use_axis_y(use_axis[1]),
      m_reset_x(reset[0]),
      m_reset_y(reset[1]),
      m_local_x(local[0]),
      m_local_y(local[1])
{
  m_canvas = m_ketsji->GetCanvas();
  m_oldposition[0] = m_oldposition[1] = 0.0f;
  m_initialSkipping = true;
  m_limit_x[0] = limit_x[0];
  m_limit_x[1] = limit_x[1];
  m_limit_y[0] = limit_y[0];
  m_limit_y[1] = limit_y[1];
  m_threshold[0] = threshold[0];
  m_threshold[1] = threshold[1];
  m_object_axis[0] = object_axis[0];
  m_object_axis[1] = object_axis[1];
  m_sensitivity[0] = sensitivity[0];
  m_sensitivity[1] = sensitivity[1];
  m_angle[0] = 0.f;
  m_angle[1] = 0.f;

  mouact_total++;
}

SCA_MouseActuator::~SCA_MouseActuator()
{
  mouact_total--;
}

bool SCA_MouseActuator::Update()
{
  bool bNegativeEvent = IsNegativeEvent();
  mouact_count++;
  if (mouact_count == mouact_total) {
    RemoveAllEvents();
    mouact_count = 0;
  }

  if (bNegativeEvent) {
    // Reset initial skipping check on negative events.
    m_initialSkipping = true;
    return false;  // do nothing on negative events
  }

  KX_GameObject *parent = static_cast<KX_GameObject *>(GetParent());

  m_mouse = ((SCA_MouseManager *)m_eventmgr)->GetInputDevice();

  switch (m_type) {
    case KX_ACT_MOUSE_VISIBILITY: {
      if (m_visible) {
        if (m_canvas) {
          m_canvas->SetMouseState(RAS_ICanvas::MOUSE_NORMAL);
        }
      }
      else {
        if (m_canvas) {
          m_canvas->SetMouseState(RAS_ICanvas::MOUSE_INVISIBLE);
        }
      }
      break;
    }
    case KX_ACT_MOUSE_LOOK: {
      if (m_mouse) {

        float position[2];
        float movement[2];
        MT_Vector3 rotation;
        float setposition[2] = {0.0f};
        float center_x = 0.5f, center_y = 0.5f;

        getMousePosition(position);

        movement[0] = position[0];
        movement[1] = position[1];

        // preventing undesired drifting when resolution is odd
        if ((m_canvas->GetWidth() % 2) != 0) {
          center_x = ((m_canvas->GetWidth() - 1.0f) / 2.0f) / (m_canvas->GetWidth());
        }
        if ((m_canvas->GetHeight() % 2) != 0) {
          center_y = ((m_canvas->GetHeight() - 1.0f) / 2.0f) / (m_canvas->GetHeight());
        }

        // preventing initial skipping.
        if (m_initialSkipping) {

          if (m_reset_x) {
            m_oldposition[0] = center_x;
          }
          else {
            m_oldposition[0] = position[0];
          }

          if (m_reset_y) {
            m_oldposition[1] = center_y;
          }
          else {
            m_oldposition[1] = position[1];
          }
          setMousePosition(m_oldposition[0], m_oldposition[1]);
          m_initialSkipping = false;
          break;
        }

        // Calculating X axis.
        if (m_use_axis_x) {

          if (m_reset_x) {
            setposition[0] = center_x;
            movement[0] -= center_x;
          }
          else {
            setposition[0] = position[0];
            movement[0] -= m_oldposition[0];
          }

          movement[0] *= -1.0f;

          /* Don't apply the rotation when we are under a certain threshold for mouse
            movement */

          if (((movement[0] > (m_threshold[0] / 10.0f)) ||
               ((movement[0] * (-1.0f)) > (m_threshold[0] / 10.0f)))) {

            movement[0] *= m_sensitivity[0];

            if ((m_limit_x[0] != 0.0f) && ((m_angle[0] + movement[0]) <= m_limit_x[0])) {
              movement[0] = m_limit_x[0] - m_angle[0];
            }

            if ((m_limit_x[1] != 0.0f) && ((m_angle[0] + movement[0]) >= m_limit_x[1])) {
              movement[0] = m_limit_x[1] - m_angle[0];
            }

            m_angle[0] += movement[0];

            switch (m_object_axis[0]) {
              case KX_ACT_MOUSE_OBJECT_AXIS_X: {
                rotation = MT_Vector3(movement[0], 0.0f, 0.0f);
                break;
              }
              case KX_ACT_MOUSE_OBJECT_AXIS_Y: {
                rotation = MT_Vector3(0.0f, movement[0], 0.0f);
                break;
              }
              case KX_ACT_MOUSE_OBJECT_AXIS_Z: {
                rotation = MT_Vector3(0.0f, 0.0f, movement[0]);
                break;
              }
              default:
                break;
            }
            parent->ApplyRotation(rotation, m_local_x);
          }
        }
        else {
          setposition[0] = center_x;
        }

        // Calculating Y axis.
        if (m_use_axis_y) {

          if (m_reset_y) {
            setposition[1] = center_y;
            movement[1] -= center_y;
          }
          else {
            setposition[1] = position[1];
            movement[1] -= m_oldposition[1];
          }

          movement[1] *= -1.0f;

          /* Don't apply the rotation when we are under a certain threshold for mouse
            movement */

          if (((movement[1] > (m_threshold[1] / 10.0f)) ||
               ((movement[1] * (-1.0f)) > (m_threshold[1] / 10.0f)))) {

            movement[1] *= m_sensitivity[1];

            if ((m_limit_y[0] != 0.0f) && ((m_angle[1] + movement[1]) <= m_limit_y[0])) {
              movement[1] = m_limit_y[0] - m_angle[1];
            }

            if ((m_limit_y[1] != 0.0f) && ((m_angle[1] + movement[1]) >= m_limit_y[1])) {
              movement[1] = m_limit_y[1] - m_angle[1];
            }

            m_angle[1] += movement[1];

            switch (m_object_axis[1]) {
              case KX_ACT_MOUSE_OBJECT_AXIS_X: {
                rotation = MT_Vector3(movement[1], 0.0f, 0.0f);
                break;
              }
              case KX_ACT_MOUSE_OBJECT_AXIS_Y: {
                rotation = MT_Vector3(0.0f, movement[1], 0.0f);
                break;
              }
              case KX_ACT_MOUSE_OBJECT_AXIS_Z: {
                rotation = MT_Vector3(0.0f, 0.0f, movement[1]);
                break;
              }
              default:
                break;
            }
            parent->ApplyRotation(rotation, m_local_y);
          }
        }
        else {
          setposition[1] = center_y;
        }

        // only trigger mouse event when it is necessary
        if (m_oldposition[0] != position[0] || m_oldposition[1] != position[1]) {
          setMousePosition(setposition[0], setposition[1]);
        }

        m_oldposition[0] = position[0];
        m_oldposition[1] = position[1];
      }
      break;
    }
    default:
      break;
  }
  return false;
}

EXP_Value *SCA_MouseActuator::GetReplica()
{
  SCA_MouseActuator *replica = new SCA_MouseActuator(*this);

  replica->ProcessReplica();
  return replica;
}

void SCA_MouseActuator::ProcessReplica()
{
  SCA_IActuator::ProcessReplica();
}

void SCA_MouseActuator::Replace_IScene(SCA_IScene *scene)
{
  /* Changes the event manager when the scene changes in case of lib loading.
   * Using an event manager in an actuator is not a regular behaviour which is
   * to avoid if it is possible.
   */
  SCA_LogicManager *logicmgr = ((KX_Scene *)scene)->GetLogicManager();
  m_eventmgr = (SCA_MouseManager *)logicmgr->FindEventManager(m_eventmgr->GetType());
}

void SCA_MouseActuator::getMousePosition(float *pos)
{
  BLI_assert(m_mouse);
  const SCA_InputEvent &xevent = m_mouse->GetInput(SCA_IInputDevice::MOUSEX);
  const SCA_InputEvent &yevent = m_mouse->GetInput(SCA_IInputDevice::MOUSEY);

  pos[0] = m_canvas->GetMouseNormalizedX(xevent.m_values[xevent.m_values.size() - 1]);
  pos[1] = m_canvas->GetMouseNormalizedY(yevent.m_values[yevent.m_values.size() - 1]);
}

void SCA_MouseActuator::setMousePosition(float fx, float fy)
{
  int x, y;

  x = (int)(fx * m_canvas->GetWidth());
  y = (int)(fy * m_canvas->GetHeight());

  m_canvas->SetMousePosition(x, y);
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_MouseActuator::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_MouseActuator",
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

PyMethodDef SCA_MouseActuator::Methods[] = {
    {"reset",
     (PyCFunction)SCA_MouseActuator::sPyReset,
     METH_NOARGS,
     "reset() : undo rotation caused by actuator\n"},
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_MouseActuator::Attributes[] = {
    EXP_PYATTRIBUTE_BOOL_RW("visible", SCA_MouseActuator, m_visible),
    EXP_PYATTRIBUTE_BOOL_RW("use_axis_x", SCA_MouseActuator, m_use_axis_x),
    EXP_PYATTRIBUTE_BOOL_RW("use_axis_y", SCA_MouseActuator, m_use_axis_y),
    EXP_PYATTRIBUTE_FLOAT_ARRAY_RW("threshold", 0.0f, 0.5f, SCA_MouseActuator, m_threshold, 2),
    EXP_PYATTRIBUTE_BOOL_RW("reset_x", SCA_MouseActuator, m_reset_x),
    EXP_PYATTRIBUTE_BOOL_RW("reset_y", SCA_MouseActuator, m_reset_y),
    EXP_PYATTRIBUTE_INT_ARRAY_RW("object_axis", 0, 2, 1, SCA_MouseActuator, m_object_axis, 2),
    EXP_PYATTRIBUTE_BOOL_RW("local_x", SCA_MouseActuator, m_local_x),
    EXP_PYATTRIBUTE_BOOL_RW("local_y", SCA_MouseActuator, m_local_y),
    EXP_PYATTRIBUTE_FLOAT_ARRAY_RW(
        "sensitivity", -FLT_MAX, FLT_MAX, SCA_MouseActuator, m_sensitivity, 2),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "limit_x", SCA_MouseActuator, pyattr_get_limit_x, pyattr_set_limit_x),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "limit_y", SCA_MouseActuator, pyattr_get_limit_y, pyattr_set_limit_y),
    EXP_PYATTRIBUTE_RW_FUNCTION("angle", SCA_MouseActuator, pyattr_get_angle, pyattr_set_angle),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *SCA_MouseActuator::pyattr_get_limit_x(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_MouseActuator *self = static_cast<SCA_MouseActuator *>(self_v);
  return PyObjectFrom(MT_Vector2(self->m_limit_x[0] / (float)M_PI * 180.0f,
                                 self->m_limit_x[1] / (float)M_PI * 180.0f));
}

int SCA_MouseActuator::pyattr_set_limit_x(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef,
                                          PyObject *value)
{
  PyObject *item1, *item2;
  SCA_MouseActuator *self = static_cast<SCA_MouseActuator *>(self_v);

  if (!PyList_Check(value))
    return PY_SET_ATTR_FAIL;

  if (PyList_Size(value) != 2)
    return PY_SET_ATTR_FAIL;

  item1 = PyList_GET_ITEM(value, 0);
  item2 = PyList_GET_ITEM(value, 1);

  if (!(PyFloat_Check(item1)) || !(PyFloat_Check(item2))) {
    return PY_SET_ATTR_FAIL;
  }
  else {
    self->m_limit_x[0] = (float)((PyFloat_AsDouble(item1) * M_PI) / 180.0f);
    self->m_limit_x[1] = (float)((PyFloat_AsDouble(item2) * M_PI) / 180.0f);
  }

  return PY_SET_ATTR_SUCCESS;
}

PyObject *SCA_MouseActuator::pyattr_get_limit_y(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_MouseActuator *self = static_cast<SCA_MouseActuator *>(self_v);
  return PyObjectFrom(MT_Vector2(self->m_limit_y[0] / (float)M_PI * 180.0f,
                                 self->m_limit_y[1] / (float)M_PI * 180.0f));
}

int SCA_MouseActuator::pyattr_set_limit_y(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef,
                                          PyObject *value)
{
  PyObject *item1, *item2;
  SCA_MouseActuator *self = static_cast<SCA_MouseActuator *>(self_v);

  if (!PyList_Check(value))
    return PY_SET_ATTR_FAIL;

  if (PyList_Size(value) != 2)
    return PY_SET_ATTR_FAIL;

  item1 = PyList_GET_ITEM(value, 0);
  item2 = PyList_GET_ITEM(value, 1);

  if (!(PyFloat_Check(item1)) || !(PyFloat_Check(item2))) {
    return PY_SET_ATTR_FAIL;
  }
  else {
    self->m_limit_y[0] = (float)((PyFloat_AsDouble(item1) * M_PI) / 180.0f);
    self->m_limit_y[1] = (float)((PyFloat_AsDouble(item2) * M_PI) / 180.0f);
  }

  return PY_SET_ATTR_SUCCESS;
}

PyObject *SCA_MouseActuator::pyattr_get_angle(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_MouseActuator *self = static_cast<SCA_MouseActuator *>(self_v);
  return PyObjectFrom(MT_Vector2(self->m_angle[0] / (float)M_PI * 180.0f,
                                 self->m_angle[1] / (float)M_PI * 180.0f));
}

int SCA_MouseActuator::pyattr_set_angle(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef,
                                        PyObject *value)
{
  PyObject *item1, *item2;
  SCA_MouseActuator *self = static_cast<SCA_MouseActuator *>(self_v);

  if (!PyList_Check(value))
    return PY_SET_ATTR_FAIL;

  if (PyList_Size(value) != 2)
    return PY_SET_ATTR_FAIL;

  item1 = PyList_GET_ITEM(value, 0);
  item2 = PyList_GET_ITEM(value, 1);

  if (!(PyFloat_Check(item1)) || !(PyFloat_Check(item2))) {
    return PY_SET_ATTR_FAIL;
  }
  else {
    self->m_angle[0] = ((float)(PyFloat_AsDouble(item1) * M_PI) / 180.0f);
    self->m_angle[1] = ((float)(PyFloat_AsDouble(item2) * M_PI) / 180.0f);
  }

  return PY_SET_ATTR_SUCCESS;
}

PyObject *SCA_MouseActuator::PyReset()
{
  MT_Vector3 rotation;
  KX_GameObject *parent = static_cast<KX_GameObject *>(GetParent());

  switch (m_object_axis[0]) {
    case KX_ACT_MOUSE_OBJECT_AXIS_X: {
      rotation = MT_Vector3(-1.0f * m_angle[0], 0.0f, 0.0f);
      break;
    }
    case KX_ACT_MOUSE_OBJECT_AXIS_Y: {
      rotation = MT_Vector3(0.0f, -1.0f * m_angle[0], 0.0f);
      break;
    }
    case KX_ACT_MOUSE_OBJECT_AXIS_Z: {
      rotation = MT_Vector3(0.0f, 0.0f, -1.0f * m_angle[0]);
      break;
    }
    default:
      break;
  }
  parent->ApplyRotation(rotation, m_local_x);

  switch (m_object_axis[1]) {
    case KX_ACT_MOUSE_OBJECT_AXIS_X: {
      rotation = MT_Vector3(-1.0f * m_angle[1], 0.0f, 0.0f);
      break;
    }
    case KX_ACT_MOUSE_OBJECT_AXIS_Y: {
      rotation = MT_Vector3(0.0f, -1.0f * m_angle[1], 0.0f);
      break;
    }
    case KX_ACT_MOUSE_OBJECT_AXIS_Z: {
      rotation = MT_Vector3(0.0f, 0.0f, -1.0f * m_angle[1]);
      break;
    }
    default:
      break;
  }
  parent->ApplyRotation(rotation, m_local_y);

  m_angle[0] = 0.0f;
  m_angle[1] = 0.0f;

  Py_RETURN_NONE;
}

#endif /* WITH_PYTHON */
