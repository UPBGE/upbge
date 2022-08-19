/*
 * global game stuff
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

/** \file gameengine/Ketsji/SCA_GameActuator.cpp
 *  \ingroup ketsji
 */

#include "SCA_GameActuator.h"

#include "CM_Message.h"
#include "KX_KetsjiEngine.h"
#include "KX_PythonInit.h" /* for config load/saving */
#include "RAS_ICanvas.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_GameActuator::SCA_GameActuator(SCA_IObject *gameobj,
                                   int mode,
                                   const std::string &filename,
                                   const std::string &loadinganimationname,
                                   SCA_IScene *scene,
                                   KX_KetsjiEngine *ketsjiengine)
    : SCA_IActuator(gameobj, KX_ACT_GAME)
{
  m_mode = mode;
  m_filename = filename;
  m_loadinganimationname = loadinganimationname;
  m_scene = scene;
  m_ketsjiengine = ketsjiengine;
} /* End of constructor */

SCA_GameActuator::~SCA_GameActuator()
{
  // there's nothing to be done here, really....
} /* end of destructor */

EXP_Value *SCA_GameActuator::GetReplica()
{
  SCA_GameActuator *replica = new SCA_GameActuator(*this);
  replica->ProcessReplica();

  return replica;
}

bool SCA_GameActuator::Update()
{
  // bool result = false;	 /*unused*/
  bool bNegativeEvent = IsNegativeEvent();
  RemoveAllEvents();

  if (bNegativeEvent)
    return false;  // do nothing on negative events

  switch (m_mode) {
    case KX_GAME_LOAD:
    case KX_GAME_START: {
      if (m_ketsjiengine) {
        std::string exitstring = "start other game";
        m_ketsjiengine->RequestExit(KX_ExitRequest::START_OTHER_GAME);
        m_ketsjiengine->SetNameNextGame(m_filename);
        m_scene->AddDebugProperty((this)->GetParent(), exitstring);
      }

      break;
    }
    case KX_GAME_RESTART: {
      if (m_ketsjiengine) {
        std::string exitstring = "restarting game";
        m_ketsjiengine->RequestExit(KX_ExitRequest::RESTART_GAME);
        m_ketsjiengine->SetNameNextGame(m_filename);
        m_scene->AddDebugProperty((this)->GetParent(), exitstring);
      }
      break;
    }
    case KX_GAME_QUIT: {
      if (m_ketsjiengine) {
        std::string exitstring = "quitting game";
        m_ketsjiengine->RequestExit(KX_ExitRequest::QUIT_GAME);
        m_scene->AddDebugProperty((this)->GetParent(), exitstring);
      }
      break;
    }
    case KX_GAME_SAVECFG: {
#ifdef WITH_PYTHON
      if (m_ketsjiengine) {
        saveGamePythonConfig();
      }
      break;
#endif  // WITH_PYTHON
    }
    case KX_GAME_LOADCFG: {
#ifdef WITH_PYTHON
      if (m_ketsjiengine) {
        loadGamePythonConfig();
      }
      break;
#endif  // WITH_PYTHON
    }
    case KX_GAME_SCREENSHOT: {
      RAS_ICanvas *canvas = m_ketsjiengine->GetCanvas();
      if (canvas) {
        canvas->MakeScreenShot(m_filename);
      }
      else {
        CM_LogicBrickError(this, "KX_GAME_SCREENSHOT Rasterizer not available");
      }
      break;
    }
    default:; /* do nothing? this is an internal error !!! */
  }

  return false;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_GameActuator::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_GameActuator",
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

PyMethodDef SCA_GameActuator::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_GameActuator::Attributes[] = {
    EXP_PYATTRIBUTE_STRING_RW("fileName", 0, 100, false, SCA_GameActuator, m_filename),
    EXP_PYATTRIBUTE_INT_RW(
        "mode", KX_GAME_NODEF + 1, KX_GAME_MAX - 1, true, SCA_GameActuator, m_mode),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON
