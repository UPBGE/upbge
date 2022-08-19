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

/** \file gameengine/Converter/BL_ConvertSensors.cpp
 *  \ingroup bgeconv
 *
 * Conversion of Blender data blocks to KX sensor system
 */

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#endif

#include "BL_ConvertSensors.h"

#include "DNA_actuator_types.h" /* for SENS_ALL_KEYS ? this define is */
#include "DNA_controller_types.h"
/* #include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_sensor_types.h"
#include "wm_event_types.h"
                                 * probably misplaced */
#ifdef _MSC_VER
//#  include "BLI_winstuff.h"
#endif

#include "BL_DataConversion.h"
#include "BL_SceneConverter.h"
#include "CM_Utils.h"
#include "EXP_IntValue.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "PHY_IPhysicsEnvironment.h"
#include "RAS_ICanvas.h"
#include "SCA_ActuatorSensor.h"
#include "SCA_AlwaysSensor.h"
#include "SCA_ArmatureSensor.h"
#include "SCA_DelaySensor.h"
#include "SCA_JoystickSensor.h"
#include "SCA_KeyboardSensor.h"
#include "SCA_MouseFocusSensor.h"
#include "SCA_MovementSensor.h"
#include "SCA_NetworkMessageSensor.h"
#include "SCA_PropertySensor.h"
#include "SCA_RadarSensor.h"
#include "SCA_RandomSensor.h"
#include "SCA_RaySensor.h"

void BL_ConvertSensors(struct Object *blenderobject,
                       class KX_GameObject *gameobj,
                       SCA_LogicManager *logicmgr,
                       KX_Scene *kxscene,
                       KX_KetsjiEngine *kxengine,
                       int activeLayerBitInfo,
                       bool isInActiveLayer,
                       RAS_ICanvas *canvas,
                       BL_SceneConverter *converter)
{

  int executePriority = 0;
  int uniqueint = 0;
  int count = 0;
  bSensor *sens = (bSensor *)blenderobject->sensors.first;
  bool pos_pulsemode = false;
  bool neg_pulsemode = false;
  int skipped_ticks = 0;
  bool invert = false;
  bool level = false;
  bool tap = false;

  while (sens) {
    sens = sens->next;
    count++;
  }
  sens = (bSensor *)blenderobject->sensors.first;

  while (sens) {
    if (!(sens->flag & SENS_DEACTIVATE)) {
      SCA_ISensor *gamesensor = nullptr;
      /* All sensors have a pulse toggle, skipped ticks parameter, and invert field.     */
      /* These are extracted here, and set when the sensor is added to the */
      /* list.                                                             */
      pos_pulsemode = (sens->pulse & SENS_PULSE_REPEAT) != 0;
      neg_pulsemode = (sens->pulse & SENS_NEG_PULSE_MODE) != 0;

      skipped_ticks = sens->freq;
      invert = !(sens->invert == 0);
      level = !(sens->level == 0);
      tap = !(sens->tap == 0);

      switch (sens->type) {
        case SENS_ALWAYS: {

          SCA_EventManager *eventmgr = logicmgr->FindEventManager(
              SCA_EventManager::BASIC_EVENTMGR);
          if (eventmgr) {
            gamesensor = new SCA_AlwaysSensor(eventmgr, gameobj);
          }

          break;
        }

        case SENS_DELAY: {
          // we can reuse the Always event manager for the delay sensor
          SCA_EventManager *eventmgr = logicmgr->FindEventManager(
              SCA_EventManager::BASIC_EVENTMGR);
          if (eventmgr) {
            bDelaySensor *delaysensor = (bDelaySensor *)sens->data;
            gamesensor = new SCA_DelaySensor(eventmgr,
                                             gameobj,
                                             delaysensor->delay,
                                             delaysensor->duration,
                                             (delaysensor->flag & SENS_DELAY_REPEAT) != 0);
          }
          break;
        }

        case SENS_COLLISION: {
          SCA_EventManager *eventmgr = logicmgr->FindEventManager(
              SCA_EventManager::TOUCH_EVENTMGR);
          if (eventmgr) {
            // collision sensor can sense both materials and properties.

            bool bFindMaterial = false, bCollisionPulse = false;

            bCollisionSensor *blendercollisionsensor = (bCollisionSensor *)sens->data;

            bFindMaterial = (blendercollisionsensor->mode & SENS_COLLISION_MATERIAL);
            bCollisionPulse = (blendercollisionsensor->mode & SENS_COLLISION_PULSE);

            const std::string touchPropOrMatName = bFindMaterial ?
                                                       CM_RemovePrefix(
                                                           blendercollisionsensor->materialName) :
                                                       blendercollisionsensor->name;

            if (gameobj->GetPhysicsController()) {
              gamesensor = new SCA_CollisionSensor(
                  eventmgr, gameobj, bFindMaterial, bCollisionPulse, touchPropOrMatName);
            }
          }

          break;
        }
        case SENS_MESSAGE: {
          SCA_EventManager *eventmgr = logicmgr->FindEventManager(
              SCA_EventManager::BASIC_EVENTMGR);
          bMessageSensor *msgSens = (bMessageSensor *)sens->data;

          /* Get our NetworkScene */
          KX_NetworkMessageScene *NetworkScene = kxscene->GetNetworkMessageScene();
          /* filter on the incoming subjects, might be empty */
          const std::string subject = msgSens->subject;

          gamesensor = new SCA_NetworkMessageSensor(eventmgr,      // our eventmanager
                                                    NetworkScene,  // our NetworkScene
                                                    gameobj,       // the sensor controlling object
                                                    subject);      // subject to filter on
          break;
        }
        case SENS_NEAR: {

          SCA_EventManager *eventmgr = logicmgr->FindEventManager(
              SCA_EventManager::TOUCH_EVENTMGR);
          if (eventmgr) {
            bNearSensor *blendernearsensor = (bNearSensor *)sens->data;
            const std::string nearpropertyname = (char *)blendernearsensor->name;

            // DT_ShapeHandle shape	=	DT_Sphere(0.0);

            // this sumoObject is not deleted by a gameobj, so delete it ourself
            // later (memleaks)!
            float radius = blendernearsensor->dist;
            const MT_Vector3 &wpos = gameobj->NodeGetWorldPosition();
            bool bFindMaterial = false;
            PHY_IPhysicsController *physCtrl =
                kxscene->GetPhysicsEnvironment()->CreateSphereController(radius, wpos);

            // will be done in KX_CollisionEventManager::RegisterSensor()
            // if (isInActiveLayer)
            //	kxscene->GetPhysicsEnvironment()->addSensor(physCtrl);

            gamesensor = new SCA_NearSensor(eventmgr,
                                            gameobj,
                                            blendernearsensor->dist,
                                            blendernearsensor->resetdist,
                                            bFindMaterial,
                                            nearpropertyname,
                                            physCtrl);
          }
          break;
        }

        case SENS_KEYBOARD: {
          /* temporary input device, for converting the code for the keyboard sensor */

          bKeyboardSensor *blenderkeybdsensor = (bKeyboardSensor *)sens->data;
          SCA_KeyboardManager *eventmgr = (SCA_KeyboardManager *)logicmgr->FindEventManager(
              SCA_EventManager::KEYBOARD_EVENTMGR);
          if (eventmgr) {
            gamesensor = new SCA_KeyboardSensor(
                eventmgr,
                BL_ConvertKeyCode(blenderkeybdsensor->key),
                BL_ConvertKeyCode(blenderkeybdsensor->qual),
                BL_ConvertKeyCode(blenderkeybdsensor->qual2),
                (blenderkeybdsensor->type == SENS_ALL_KEYS),
                blenderkeybdsensor->targetName,
                blenderkeybdsensor->toggleName,
                gameobj,
                KX_GetActiveEngine()->GetExitKey());  //			blenderkeybdsensor->pad);
          }

          break;
        }
        case SENS_MOUSE: {
          int keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_NODEF;
          int trackfocus = 0;
          bMouseSensor *bmouse = (bMouseSensor *)sens->data;

          /* There are two main types of mouse sensors. If there is
           * no focus-related behavior requested, we can make do
           * with a basic sensor. This cuts down memory usage and
           * gives a slight performance gain. */

          SCA_MouseManager *eventmgr = (SCA_MouseManager *)logicmgr->FindEventManager(
              SCA_EventManager::MOUSE_EVENTMGR);
          if (eventmgr) {

            /* Determine key mode. There is at most one active mode. */
            switch (bmouse->type) {
              case BL_SENS_MOUSE_LEFT_BUTTON:
                keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_LEFTBUTTON;
                break;
              case BL_SENS_MOUSE_MIDDLE_BUTTON:
                keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_MIDDLEBUTTON;
                break;
              case BL_SENS_MOUSE_RIGHT_BUTTON:
                keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_RIGHTBUTTON;
                break;
              case BL_SENS_MOUSE_BUTTON4:
                keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_BUTTON4;
                break;
              case BL_SENS_MOUSE_BUTTON5:
                keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_BUTTON5;
                break;
              case BL_SENS_MOUSE_BUTTON6:
                keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_BUTTON6;
                break;
              case BL_SENS_MOUSE_BUTTON7:
                keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_BUTTON7;
                break;
              case BL_SENS_MOUSE_WHEEL_UP:
                keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_WHEELUP;
                break;
              case BL_SENS_MOUSE_WHEEL_DOWN:
                keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_WHEELDOWN;
                break;
              case BL_SENS_MOUSE_MOVEMENT:
                keytype = SCA_MouseSensor::KX_MOUSESENSORMODE_MOVEMENT;
                break;
              case BL_SENS_MOUSE_MOUSEOVER:
                trackfocus = 1;
                break;
              case BL_SENS_MOUSE_MOUSEOVER_ANY:
                trackfocus = 2;
                break;

              default:; /* error */
            }

            /* initial mouse position */
            int startx = canvas->GetWidth() / 2;
            int starty = canvas->GetHeight() / 2;

            if (!trackfocus) {
              /* plain, simple mouse sensor */
              gamesensor = new SCA_MouseSensor(eventmgr, startx, starty, keytype, gameobj);
            }
            else {
              /* give us a focus-aware sensor */
              bool bFindMaterial = (bmouse->mode & SENS_COLLISION_MATERIAL);
              bool bXRay = (bmouse->flag & SENS_RAY_XRAY);
              int mask = bmouse->mask;
              std::string checkname = (bFindMaterial ? CM_RemovePrefix(bmouse->matname) :
                                                       bmouse->propname);

              gamesensor = new SCA_MouseFocusSensor(
                  eventmgr,
                  startx,
                  starty,
                  keytype,
                  trackfocus,
                  (bmouse->flag & SENS_MOUSE_FOCUS_PULSE) ? true : false,
                  checkname,
                  bFindMaterial,
                  bXRay,
                  mask,
                  kxscene,
                  kxengine,
                  gameobj);
            }
          }
          break;
        }
        case SENS_PROPERTY: {
          bPropertySensor *blenderpropsensor = (bPropertySensor *)sens->data;
          SCA_EventManager *eventmgr = logicmgr->FindEventManager(
              SCA_EventManager::BASIC_EVENTMGR);
          if (eventmgr) {
            std::string propname = blenderpropsensor->name;
            std::string propval = blenderpropsensor->value;
            std::string propmaxval = blenderpropsensor->maxvalue;

            SCA_PropertySensor::KX_PROPSENSOR_TYPE propchecktype =
                SCA_PropertySensor::KX_PROPSENSOR_NODEF;

            /* Better do an explicit conversion here! (was implicit      */
            /* before...)                                                */
            switch (blenderpropsensor->type) {
              case SENS_PROP_EQUAL:
                propchecktype = SCA_PropertySensor::KX_PROPSENSOR_EQUAL;
                break;
              case SENS_PROP_NEQUAL:
                propchecktype = SCA_PropertySensor::KX_PROPSENSOR_NOTEQUAL;
                break;
              case SENS_PROP_INTERVAL:
                propchecktype = SCA_PropertySensor::KX_PROPSENSOR_INTERVAL;
                break;
              case SENS_PROP_CHANGED:
                propchecktype = SCA_PropertySensor::KX_PROPSENSOR_CHANGED;
                break;
              case SENS_PROP_EXPRESSION:
                propchecktype = SCA_PropertySensor::KX_PROPSENSOR_EXPRESSION;
                /* error */
                break;
              case SENS_PROP_LESSTHAN:
                propchecktype = SCA_PropertySensor::KX_PROPSENSOR_LESSTHAN;
                break;
              case SENS_PROP_GREATERTHAN:
                propchecktype = SCA_PropertySensor::KX_PROPSENSOR_GREATERTHAN;
                break;
              default:; /* error */
            }
            gamesensor = new SCA_PropertySensor(
                eventmgr, gameobj, propname, propval, propmaxval, propchecktype);
          }

          break;
        }
        case SENS_ACTUATOR: {
          bActuatorSensor *blenderactsensor = (bActuatorSensor *)sens->data;
          // we will reuse the property event manager, there is nothing special with this sensor
          SCA_EventManager *eventmgr = logicmgr->FindEventManager(
              SCA_EventManager::ACTUATOR_EVENTMGR);
          if (eventmgr) {
            std::string propname = blenderactsensor->name;
            gamesensor = new SCA_ActuatorSensor(eventmgr, gameobj, propname);
          }
          break;
        }

        case SENS_ARMATURE: {
          bArmatureSensor *blenderarmsensor = (bArmatureSensor *)sens->data;
          // we will reuse the property event manager, there is nothing special with this sensor
          SCA_EventManager *eventmgr = logicmgr->FindEventManager(
              SCA_EventManager::BASIC_EVENTMGR);
          if (eventmgr) {
            std::string bonename = blenderarmsensor->posechannel;
            std::string constraintname = blenderarmsensor->constraint;
            gamesensor = new SCA_ArmatureSensor(eventmgr,
                                                gameobj,
                                                bonename,
                                                constraintname,
                                                blenderarmsensor->type,
                                                blenderarmsensor->value);
          }
          break;
        }

        case SENS_RADAR: {

          SCA_EventManager *eventmgr = logicmgr->FindEventManager(
              SCA_EventManager::TOUCH_EVENTMGR);
          if (eventmgr) {
            bRadarSensor *blenderradarsensor = (bRadarSensor *)sens->data;
            const std::string radarpropertyname = blenderradarsensor->name;

            int radaraxis = blenderradarsensor->axis;

            MT_Scalar coneheight = blenderradarsensor->range;

            // janco: the angle was doubled, so should I divide the factor in 2
            // or the blenderradarsensor->angle?
            // nzc: the angle is the opening angle. We need to init with
            // the axis-hull angle,so /2.0.
            MT_Scalar factor = tan(blenderradarsensor->angle * 0.5f);
            // MT_Scalar coneradius = coneheight * (factor / 2);
            MT_Scalar coneradius = coneheight * factor;

            // this sumoObject is not deleted by a gameobj, so delete it ourself
            // later (memleaks)!
            MT_Scalar smallmargin = 0.0;
            MT_Scalar largemargin = 0.0;

            bool bFindMaterial = false;
            PHY_IPhysicsController *ctrl = kxscene->GetPhysicsEnvironment()->CreateConeController(
                (float)coneradius, (float)coneheight);

            gamesensor = new SCA_RadarSensor(eventmgr,
                                             gameobj,
                                             ctrl,
                                             coneradius,
                                             coneheight,
                                             radaraxis,
                                             smallmargin,
                                             largemargin,
                                             bFindMaterial,
                                             radarpropertyname);
          }

          break;
        }
        case SENS_RAY: {
          bRaySensor *blenderraysensor = (bRaySensor *)sens->data;

          // blenderradarsensor->angle;
          SCA_EventManager *eventmgr = logicmgr->FindEventManager(
              SCA_EventManager::BASIC_EVENTMGR);
          if (eventmgr) {
            bool bFindMaterial = (blenderraysensor->mode & SENS_COLLISION_MATERIAL);
            bool bXRay = (blenderraysensor->mode & SENS_RAY_XRAY);

            std::string checkname = (bFindMaterial ? CM_RemovePrefix(blenderraysensor->matname) :
                                                     blenderraysensor->propname);

            // don't want to get rays of length 0.0 or so
            double distance = (blenderraysensor->range < 0.01f ? 0.01f : blenderraysensor->range);
            int axis = blenderraysensor->axisflag;
            int mask = blenderraysensor->mask;

            gamesensor = new SCA_RaySensor(
                eventmgr, gameobj, checkname, bFindMaterial, bXRay, distance, axis, mask, kxscene);
          }
          break;
        }

        case SENS_RANDOM: {
          bRandomSensor *blenderrndsensor = (bRandomSensor *)sens->data;
          // some files didn't write randomsensor, avoid crash now for nullptr ptr's
          if (blenderrndsensor) {
            SCA_EventManager *eventmgr = logicmgr->FindEventManager(
                SCA_EventManager::BASIC_EVENTMGR);
            if (eventmgr) {
              int randomSeed = blenderrndsensor->seed;
              if (randomSeed == 0) {
                randomSeed = (int)(kxengine->GetRealTime() * 100000.0);
                randomSeed ^= (intptr_t)blenderrndsensor;
              }
              gamesensor = new SCA_RandomSensor(eventmgr, gameobj, randomSeed);
            }
          }
          break;
        }
        case SENS_MOVEMENT: {
          bMovementSensor *blendermovsensor = (bMovementSensor *)sens->data;
          // some files didn't write movementsensor, avoid crash now for NULL ptr's
          if (blendermovsensor) {
            SCA_EventManager *eventmgr = logicmgr->FindEventManager(
                SCA_EventManager::BASIC_EVENTMGR);
            if (eventmgr) {
              bool localflag = (blendermovsensor->localflag & SENS_MOVEMENT_LOCAL);
              int axis = blendermovsensor->axisflag;
              float threshold = blendermovsensor->threshold;
              gamesensor = new SCA_MovementSensor(eventmgr, gameobj, axis, localflag, threshold);
            }
          }
          break;
        }
        case SENS_JOYSTICK: {
          int joysticktype = SCA_JoystickSensor::KX_JOYSENSORMODE_NODEF;

          bJoystickSensor *bjoy = (bJoystickSensor *)sens->data;

          SCA_JoystickManager *eventmgr = (SCA_JoystickManager *)logicmgr->FindEventManager(
              SCA_EventManager::JOY_EVENTMGR);
          if (eventmgr) {
            int axis = 0;
            int axisf = 0;
            int button = 0;
            int prec = 0;

            switch (bjoy->type) {
              case SENS_JOY_AXIS: {
                axis = bjoy->axis;
                axisf = bjoy->axisf;
                prec = bjoy->precision;
                joysticktype = SCA_JoystickSensor::KX_JOYSENSORMODE_AXIS;
                break;
              }
              case SENS_JOY_BUTTON: {
                button = bjoy->button;
                joysticktype = SCA_JoystickSensor::KX_JOYSENSORMODE_BUTTON;
                break;
              }
              case SENS_JOY_AXIS_SINGLE: {
                axis = bjoy->axis_single;
                prec = bjoy->precision;
                joysticktype = SCA_JoystickSensor::KX_JOYSENSORMODE_AXIS_SINGLE;
                break;
              }
              case SENS_JOY_SHOULDER_TRIGGER: {
                axis = bjoy->axis_single;
                prec = bjoy->precision;
                joysticktype = SCA_JoystickSensor::KX_JOYSENSORMODE_SHOULDER_TRIGGER;
                break;
              }
              default: {
                CM_Error("bad case statement");
                break;
              }
            }
            gamesensor = new SCA_JoystickSensor(eventmgr,
                                                gameobj,
                                                bjoy->joyindex,
                                                joysticktype,
                                                axis,
                                                axisf,
                                                prec,
                                                button,
                                                (bjoy->flag & SENS_JOY_ANY_EVENT));
          }
          else {
            CM_Error("problem finding the event manager");
          }

          break;
        }
        default: {
        }
      }

      if (gamesensor) {
        gamesensor->SetExecutePriority(executePriority++);
        std::string uniquename = sens->name;
        uniquename += "#SENS#";
        uniqueint++;
        EXP_IntValue *uniqueval = new EXP_IntValue(uniqueint);
        uniquename += uniqueval->GetText();
        uniqueval->Release();

        /* Conversion succeeded, so we can set the generic props here.   */
        gamesensor->SetPulseMode(pos_pulsemode, neg_pulsemode, skipped_ticks);
        gamesensor->SetInvert(invert);
        gamesensor->SetLevel(level);
        gamesensor->SetTap(tap);
        gamesensor->SetName(sens->name);
        gamesensor->SetLogicManager(logicmgr);

        gameobj->AddSensor(gamesensor);

        for (int i = 0; i < sens->totlinks; i++) {
          bController *linkedcont = (bController *)sens->links[i];
          if (linkedcont) {
            // If the controller is deactived doesn't register it
            if (!(linkedcont->flag & CONT_DEACTIVATE)) {
              SCA_IController *gamecont = converter->FindGameController(linkedcont);

              if (gamecont) {
                logicmgr->RegisterToSensor(gamecont, gamesensor);
              }
              else {
                CM_Warning("sensor \"" << sens->name << "\" could not find its controller (link "
                                       << (i + 1) << " of " << sens->totlinks << ") from object \""
                                       << blenderobject->id.name + 2
                                       << "\". There has been an error converting the blender "
                                          "controller for the game engine, "
                                       << "logic may be incorrect");
              }
            }
          }
          else {
            CM_Warning("sensor \"" << sens->name << "\" has lost a link to a controller (link "
                                   << (i + 1) << " of " << sens->totlinks << ") from object \""
                                   << blenderobject->id.name + 2
                                   << "\". Possible causes are partially appended objects or an "
                                      "error reading the file, "
                                   << "logic may be incorrect");
          }
        }
        // special case: Keyboard sensor with no link
        // this combination is usually used for key logging.
        if (sens->type == SENS_KEYBOARD && sens->totlinks == 0) {
          // Force the registration so that the sensor runs
          gamesensor->IncLink();
        }

        // done with gamesensor
        gamesensor->Release();
      }
    }

    sens = sens->next;
  }
}
