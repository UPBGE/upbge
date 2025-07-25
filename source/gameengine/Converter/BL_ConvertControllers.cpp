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

/** \file gameengine/Converter/BL_ConvertControllers.cpp
 *  \ingroup bgeconv
 */

#include "BL_ConvertControllers.h"

/* This little block needed for linking to Blender... */
#ifdef WIN32
#  include "BLI_winstuff.h"
#endif

#include "BKE_text.h"
#include "DNA_controller_types.h"
#include "DNA_text_types.h"

#include "BL_SceneConverter.h"
#include "EXP_IntValue.h"
#include "KX_GameObject.h"
#include "SCA_ANDController.h"
#include "SCA_ExpressionController.h"
#include "SCA_NANDController.h"
#include "SCA_NORController.h"
#include "SCA_ORController.h"
#include "SCA_PythonController.h"
#include "SCA_XNORController.h"
#include "SCA_XORController.h"

static void LinkControllerToActuators(SCA_IController *game_controller,
                                      bController *bcontr,
                                      SCA_LogicManager *logicmgr,
                                      BL_SceneConverter *converter)
{
  // Iterate through the actuators of the game blender
  // controller and find the corresponding ketsji actuator.

  for (int i = 0; i < bcontr->totlinks; i++) {
    bActuator *bact = (bActuator *)bcontr->links[i];
    SCA_IActuator *game_actuator = converter->FindGameActuator(bact);
    if (game_actuator) {
      logicmgr->RegisterToActuator(game_controller, game_actuator);
    }
  }
}

void BL_ConvertControllers(struct Object *blenderobject,
                           class KX_GameObject *gameobj,
                           SCA_LogicManager *logicmgr,
                           int activeLayerBitInfo,
                           bool isInActiveLayer,
                           BL_SceneConverter *converter,
                           bool libloading)
{
  int uniqueint = 0;
  int count = 0;
  int executePriority = 0;
  bController *bcontr = (bController *)blenderobject->controllers.first;
  while (bcontr) {
    bcontr = bcontr->next;
    count++;
  }
  bcontr = (bController *)blenderobject->controllers.first;
  while (bcontr) {
    SCA_IController *gamecontroller = nullptr;
    switch (bcontr->type) {
      case CONT_LOGIC_AND: {
        gamecontroller = new SCA_ANDController(gameobj);
        break;
      }
      case CONT_LOGIC_OR: {
        gamecontroller = new SCA_ORController(gameobj);
        break;
      }
      case CONT_LOGIC_NAND: {
        gamecontroller = new SCA_NANDController(gameobj);
        break;
      }
      case CONT_LOGIC_NOR: {
        gamecontroller = new SCA_NORController(gameobj);
        break;
      }
      case CONT_LOGIC_XOR: {
        gamecontroller = new SCA_XORController(gameobj);
        break;
      }
      case CONT_LOGIC_XNOR: {
        gamecontroller = new SCA_XNORController(gameobj);
        break;
      }
      case CONT_EXPRESSION: {
        bExpressionCont *bexpcont = (bExpressionCont *)bcontr->data;
        std::string expressiontext = std::string(bexpcont->str);
        if (!expressiontext.empty()) {
          gamecontroller = new SCA_ExpressionController(gameobj, expressiontext);
        }
        break;
      }
      case CONT_PYTHON: {
        bPythonCont *pycont = (bPythonCont *)bcontr->data;
        SCA_PythonController *pyctrl = new SCA_PythonController(gameobj, pycont->mode);
        gamecontroller = pyctrl;
#ifdef WITH_PYTHON
        if (pycont->mode == SCA_PythonController::SCA_PYEXEC_SCRIPT) {
          if (pycont->text) {
            char *buf;
            // this is some blender specific code
            size_t buf_len_dummy;
            buf = txt_to_buf(pycont->text, &buf_len_dummy);
            if (buf) {
              pyctrl->SetScriptText(std::string(buf));
              pyctrl->SetScriptName(pycont->text->id.name + 2);
              MEM_freeN(buf);
            }
          }
        }
        else {
          /* let the controller print any warnings here when importing */
          pyctrl->SetScriptText(std::string(pycont->module));
          pyctrl->SetScriptName(
              pycont
                  ->module); /* will be something like module.func so using it as the name is OK */

          if (pycont->flag & CONT_PY_DEBUG) {
            CM_Warning("debuging \"" << pycont->module << "\", module for object "
                                     << blenderobject->id.name + 2
                                     << " expect worse performance.");
            pyctrl->SetDebug(true);
          }
        }
#endif  // WITH_PYTHON

        break;
      }
      default: {
      }
    }

    if (gamecontroller && !(bcontr->flag & CONT_DEACTIVATE)) {
      LinkControllerToActuators(gamecontroller, bcontr, logicmgr, converter);
      gamecontroller->SetExecutePriority(executePriority++);
      gamecontroller->SetBookmark((bcontr->flag & CONT_PRIO) != 0);
      gamecontroller->SetState(bcontr->state_mask);
      std::string uniquename = bcontr->name;
      uniquename += "#CONTR#";
      uniqueint++;
      EXP_IntValue *uniqueval = new EXP_IntValue(uniqueint);
      uniquename += uniqueval->GetText();
      uniqueval->Release();
      // unique name was never implemented for sensors and actuators, only for controllers
      // and it's producing difference in the keys for the lists: obj.controllers/sensors/actuators
      // at some point it should either be implemented globally (and saved as a separate var) or
      // removed. gamecontroller->SetName(uniquename);
      gamecontroller->SetName(bcontr->name);
      gamecontroller->SetLogicManager(logicmgr);
      gameobj->AddController(gamecontroller);

      converter->RegisterGameController(gamecontroller, bcontr);

#ifdef WITH_PYTHON
      // When libloading, this is delayed to KX_Scene::MergeScene_LogicBrick to avoid GIL issues
      if (!libloading && bcontr->type == CONT_PYTHON) {
        SCA_PythonController *pyctrl = static_cast<SCA_PythonController *>(gamecontroller);
        /* not strictly needed but gives syntax errors early on and
         * gives more predictable performance for larger scripts */
        if (pyctrl->m_mode == SCA_PythonController::SCA_PYEXEC_SCRIPT)
          pyctrl->Compile();
        else {
          /* We cant do this because importing runs the script which could end up accessing
           * internal BGE functions, this is unstable while we're converting the scene.
           * This is a pity because its useful to see errors at startup but cant help it */

          // pyctrl->Import();
        }
      }

#endif  // WITH_PYTHON

      // done with gamecontroller
      gamecontroller->Release();
    }
    else if (gamecontroller)
      gamecontroller->Release();

    bcontr = bcontr->next;
  }
}
