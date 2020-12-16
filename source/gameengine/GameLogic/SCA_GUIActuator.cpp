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
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "SCA_GUIActuator.h"

#ifdef WITH_GAMEENGINE_CEGUI
#include <CEGUI/CEGUI.h>
#endif
#include <iostream>
#include <string>

#include "KX_Scene.h"
#include "KX_Camera.h"
#include "KX_KetsjiEngine.h"

SCA_GUIActuator::SCA_GUIActuator(SCA_IObject *gameobj,
                                 int mode,
                                 const std::string themename,
                                 const std::string cursorname,
                                 const std::string layoutname,
                                 const std::string prefix,
                                 bool cursordefault,
                                 KX_Scene *scene,
                                 KX_KetsjiEngine *ketsjiEngine)
    : SCA_IActuator(gameobj, KX_ACT_GUI)
{
  m_mode = mode;
  m_scene = scene;
  m_KetsjiEngine = ketsjiEngine;
  m_themeName = themename;
  m_cursorName = cursorname;
  m_layoutName = layoutname;
  m_prefix = prefix;
  m_cursorDefault = cursordefault;
}

SCA_GUIActuator::~SCA_GUIActuator()
{
}

CValue *SCA_GUIActuator::GetReplica()
{
  SCA_GUIActuator *replica = new SCA_GUIActuator(*this);
  replica->ProcessReplica();
  return replica;
}

bool SCA_GUIActuator::Update()
{
  bool bNegativeEvent = IsNegativeEvent();
  RemoveAllEvents();

  if (bNegativeEvent) {
    return false; // do nothing on negative events
  }

  try {
    //CEGUI::WindowManager& winMgr = CEGUI::WindowManager::getSingleton();
    /*CEGUI::Window *background = winMgr.getWindow("BGE Root Window");*/
    CEGUI::WindowManager *winMgr = CEGUI::WindowManager::getSingletonPtr();

    /*CEGUI::Window *background = CEGUI::WindowManager::getSingleton().createWindow("DefaultWindow", "root");*/
    CEGUI::Window *background = winMgr->createWindow("DefaultWindow", "root");
    CEGUI::System::getSingleton().getDefaultGUIContext().setRootWindow(background);

    switch (m_mode) {
      case KX_GUI_LAYOUT_ADD: {
        if (m_prefix.length()) {
          background->addChild(winMgr->loadLayoutFromFile(m_layoutName.c_str(), m_prefix.c_str()));
        }
        else {
          background->addChild(winMgr->loadLayoutFromFile(m_layoutName.c_str()));
        }
        break;
      }
      case KX_GUI_LAYOUT_REMOVE: {
        break;
      }
      case KX_GUI_SCHEME_LOAD: {
        try {
          // try to load with raw name
          CEGUI::SchemeManager::getSingleton().createFromFile(m_layoutName.c_str());
        }
        catch (CEGUI::Exception& gui_error) {
          // last chance... with .scheme suffix
          CEGUI::SchemeManager::getSingleton().createFromFile(
              (m_layoutName.append(".scheme")).c_str());
        }
        break;
      }
      case KX_GUI_MOUSE_CHANGE: {
        CEGUI::System::getSingleton().getDefaultGUIContext().getMouseCursor().setDefaultImage(
            /*m_themeName.c_str(),*/ m_cursorName.c_str());
        CEGUI::System::getSingleton().getDefaultGUIContext().getMouseCursor().show();
        break;
      }
      case KX_GUI_MOUSE_HIDE: {
        CEGUI::System::getSingleton().getDefaultGUIContext().getMouseCursor().hide();
        break;
      }
      case KX_GUI_MOUSE_SHOW: {
        CEGUI::System::getSingleton().getDefaultGUIContext().getMouseCursor().show();
        break;
      }
      default:; /* do nothing? this is an internal error !!! */
    }
  }
  catch (CEGUI::Exception& gui_error) {
    std::cout << "GUI Error: " << gui_error.getMessage() << std::endl;
  }
  catch (...) {
    std::cout << "GUI Error: look at CEGUI.log, something is wrong" << std::endl;
  };

  return false;
}

#ifdef WITH_PYTHON
/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_GUIActuator::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_GUIActuator",
                                       sizeof(PyObjectPlus_Proxy),
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

PyMethodDef SCA_GUIActuator::Methods[] =
{
  {nullptr,nullptr} //Sentinel
};

PyAttributeDef SCA_GUIActuator::Attributes[] = {
  KX_PYATTRIBUTE_STRING_RW("themeName", 0, 64, true, SCA_GUIActuator, m_themeName),
  KX_PYATTRIBUTE_STRING_RW("cursorName", 0, 64, true, SCA_GUIActuator, m_cursorName),
  KX_PYATTRIBUTE_STRING_RW("layoutName", 0, 64, true, SCA_GUIActuator, m_layoutName),
  KX_PYATTRIBUTE_STRING_RW("prefix", 0, 64, true, SCA_GUIActuator, m_prefix),
  KX_PYATTRIBUTE_BOOL_RW("changeDefault", SCA_GUIActuator, m_cursorDefault),
  KX_PYATTRIBUTE_INT_RW("mode", KX_GUI_NODEF + 1, KX_GUI_MAX - 1, true, SCA_GUIActuator, m_mode),
  KX_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON
