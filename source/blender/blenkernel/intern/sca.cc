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
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 * these all are linked to objects (listbase)
 * all data is 'direct data', not Blender lib data.
 */

/** \file blender/blenkernel/intern/sca.c
 *  \ingroup bke
 */

#include <float.h>
#include <stdio.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_actuator_types.h"
#include "DNA_collection_types.h"
#include "DNA_controller_types.h"
#include "DNA_object_types.h"
#include "DNA_sensor_types.h"
#include "DNA_text_types.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math_rotation.h"

#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_sca.hh"

/* ******************* SENSORS ************************ */

void BKE_sca_free_sensor(bSensor *sens)
{
  if (sens->links)
    MEM_freeN(sens->links);
  if (sens->data)
    MEM_freeN(sens->data);
  MEM_freeN(sens);
}

void BKE_sca_free_sensors(ListBase *lb)
{
  bSensor *sens;

  while ((sens = (bSensor *)BLI_pophead(lb))) {
    BKE_sca_free_sensor(sens);
  }
}

bSensor *BKE_sca_copy_sensor(bSensor *sens, const int /*flag*/)
{
  bSensor *sensn;

  sensn = (bSensor *)MEM_dupallocN(sens);
  sensn->flag |= SENS_NEW;
  if (sens->data) {
    sensn->data = (void *)MEM_dupallocN(sens->data);
  }

  if (sens->links)
    sensn->links = (bController **)MEM_dupallocN(sens->links);

  return sensn;
}

void BKE_sca_copy_sensors(ListBase *lbn, const ListBase *lbo, const int flag)
{
  bSensor *sens, *sensn;

  lbn->first = lbn->last = nullptr;
  sens = (bSensor *)lbo->first;
  while (sens) {
    sensn = BKE_sca_copy_sensor(sens, flag);
    BLI_addtail(lbn, sensn);
    sens = sens->next;
  }
}

void BKE_sca_init_sensor(bSensor *sens)
{
  /* also use when sensor changes type */
  bNearSensor *ns;
  bMouseSensor *ms;
  bJoystickSensor *js;
  bRaySensor *rs;
  bMovementSensor *movs;

  if (sens->data)
    MEM_freeN(sens->data);
  sens->data = nullptr;
  sens->pulse = 0;

  switch (sens->type) {
    case SENS_ALWAYS:
      sens->pulse = 0;
      break;
    case SENS_NEAR:
      sens->data = MEM_callocN(sizeof(bNearSensor), "nearsens");
      ns = (bNearSensor *)sens->data;
      ns->dist = 1.0;
      ns->resetdist = 2.0;
      break;
    case SENS_KEYBOARD:
      sens->data = MEM_callocN(sizeof(bKeyboardSensor), "keysens");
      break;
    case SENS_PROPERTY:
      sens->data = MEM_callocN(sizeof(bPropertySensor), "propsens");
      break;
    case SENS_ARMATURE:
      sens->data = MEM_callocN(sizeof(bArmatureSensor), "armsens");
      break;
    case SENS_ACTUATOR:
      sens->data = MEM_callocN(sizeof(bActuatorSensor), "actsens");
      break;
    case SENS_DELAY:
      sens->data = MEM_callocN(sizeof(bDelaySensor), "delaysens");
      break;
    case SENS_MOUSE:
      sens->data = MEM_callocN(sizeof(bMouseSensor), "mousesens");
      ms = (bMouseSensor *)sens->data;
      ms->type = 1;  // LEFTMOUSE workaround because Mouse Sensor types enum starts in 1
      ms->mask = (1 << OB_MAX_COL_MASKS) - 1;
      break;
    case SENS_COLLISION:
      sens->data = MEM_callocN(sizeof(bCollisionSensor), "colsens");
      break;
    case SENS_RADAR:
      sens->data = MEM_callocN(sizeof(bRadarSensor), "radarsens");
      break;
    case SENS_RANDOM:
      sens->data = MEM_callocN(sizeof(bRandomSensor), "randomsens");
      break;
    case SENS_MOVEMENT:
      sens->data = MEM_callocN(sizeof(bMovementSensor), "movementsens");
      movs = (bMovementSensor *)sens->data;
      movs->threshold = 0.01f;
      break;
    case SENS_RAY:
      sens->data = MEM_callocN(sizeof(bRaySensor), "raysens");
      rs = (bRaySensor *)sens->data;
      rs->range = 0.01f;
      rs->mask = (1 << OB_MAX_COL_MASKS) - 1;
      break;
    case SENS_MESSAGE:
      sens->data = MEM_callocN(sizeof(bMessageSensor), "messagesens");
      break;
    case SENS_JOYSTICK:
      sens->data = MEM_callocN(sizeof(bJoystickSensor), "joysticksens");
      js = (bJoystickSensor *)sens->data;
      js->type = SENS_JOY_AXIS;
      js->axis = SENS_JOY_LEFT_STICK;
      js->axis_single = SENS_JOY_LEFT_STICK_HORIZONTAL;
      js->precision = 5000;
      break;
    default:; /* this is very severe... I cannot make any memory for this        */
              /* logic brick...                                                    */
  }
}

bSensor *BKE_sca_new_sensor(int type)
{
  bSensor *sens;

  sens = (bSensor *)MEM_callocN(sizeof(bSensor), "Sensor");
  sens->type = type;
  sens->flag = SENS_SHOW;

  BKE_sca_init_sensor(sens);

  strcpy(sens->name, "sensor");
  // XXX	make_unique_prop_names(sens->name);

  return sens;
}

/* ******************* CONTROLLERS ************************ */

void BKE_sca_unlink_controller(bController *cont)
{
  bSensor *sens;
  Object *ob;

  /* check for controller pointers in sensors */
  ob = (Object *)G.main->objects.first;
  while (ob) {
    sens = (bSensor *)ob->sensors.first;
    while (sens) {
      BKE_sca_unlink_logicbricks((void **)&cont, (void ***)&(sens->links), &sens->totlinks);
      sens = sens->next;
    }
    ob = (Object *)ob->id.next;
  }
}

void BKE_sca_unlink_controllers(ListBase *lb)
{
  bController *cont;

  for (cont = (bController *)lb->first; cont; cont = cont->next)
    BKE_sca_unlink_controller(cont);
}

void BKE_sca_free_controller(bController *cont)
{
  if (cont->links)
    MEM_freeN(cont->links);

  /* the controller itself */
  if (cont->data) {
    MEM_freeN(cont->data);
  }
  MEM_freeN(cont);
}

void BKE_sca_free_controllers(ListBase *lb)
{
  bController *cont;

  while ((cont = (bController *)BLI_pophead(lb))) {
    if (cont->slinks)
      MEM_freeN(cont->slinks);
    BKE_sca_free_controller(cont);
  }
}

bController *BKE_sca_copy_controller(bController *cont, const int /*flag*/)
{
  bController *contn;

  cont->mynew = contn = (bController *)MEM_dupallocN(cont);
  contn->flag |= CONT_NEW;
  if (cont->data) {
    contn->data = MEM_dupallocN(cont->data);
  }

  if (cont->links)
    contn->links = (bActuator **)MEM_dupallocN(cont->links);
  contn->slinks = nullptr;
  contn->totslinks = 0;

  return contn;
}

void BKE_sca_copy_controllers(ListBase *lbn, const ListBase *lbo, const int flag)
{
  bController *cont, *contn;

  lbn->first = lbn->last = nullptr;
  cont = (bController *)lbo->first;
  while (cont) {
    contn = BKE_sca_copy_controller(cont, flag);
    BLI_addtail(lbn, contn);
    cont = cont->next;
  }
}

void BKE_sca_init_controller(bController *cont)
{
  /* also use when controller changes type, leave actuators... */

  if (cont->data)
    MEM_freeN(cont->data);
  cont->data = nullptr;

  switch (cont->type) {
    case CONT_EXPRESSION:
      cont->data = MEM_callocN(sizeof(bExpressionCont), "expcont");
      break;
    case CONT_PYTHON:
      cont->data = MEM_callocN(sizeof(bPythonCont), "pycont");
      break;
  }
}

bController *BKE_sca_new_controller(int type)
{
  bController *cont;

  cont = (bController *)MEM_callocN(sizeof(bController), "Controller");
  cont->type = type;
  cont->flag = CONT_SHOW;

  BKE_sca_init_controller(cont);

  strcpy(cont->name, "cont");
  // XXX	make_unique_prop_names(cont->name);

  return cont;
}

/* ******************* ACTUATORS ************************ */

void BKE_sca_unlink_actuator(bActuator *act)
{
  bController *cont;
  Object *ob;

  /* check for actuator pointers in controllers */
  ob = (Object *)G.main->objects.first;
  while (ob) {
    cont = (bController *)ob->controllers.first;
    while (cont) {
      BKE_sca_unlink_logicbricks((void **)&act, (void ***)&(cont->links), &cont->totlinks);
      cont = cont->next;
    }
    ob = (Object *)ob->id.next;
  }
}

void BKE_sca_unlink_actuators(ListBase *lb)
{
  bActuator *act;

  for (act = (bActuator *)lb->first; act; act = act->next)
    BKE_sca_unlink_actuator(act);
}

void BKE_sca_free_actuator(bActuator *act)
{
  if (act->data) {
    switch (act->type) {
      case ACT_ACTION:
      /*{
        bActionActuator *aa = (bActionActuator *)act->data;
        if (aa->act)
          id_us_min((ID *)aa->act);
        break;
      }*/
      case ACT_SOUND: {
        /*bSoundActuator *sa = (bSoundActuator *) act->data;
        if (sa->sound)
          id_us_min((ID *)sa->sound);
        break;*/
      }
    }

    MEM_freeN(act->data);
  }
  MEM_freeN(act);
}

void BKE_sca_free_actuators(ListBase *lb)
{
  bActuator *act;

  while ((act = (bActuator *)BLI_pophead(lb))) {
    BKE_sca_free_actuator(act);
  }
}

bActuator *BKE_sca_copy_actuator(bActuator *act)
{
  bActuator *actn;

  act->mynew = actn = (bActuator *)MEM_dupallocN(act);
  actn->flag |= ACT_NEW;
  if (act->data) {
    actn->data = MEM_dupallocN(act->data);
  }

  switch (act->type) {
    case ACT_ACTION:
    /*{
      bActionActuator *aa = (bActionActuator *)act->data;
      if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
        id_us_plus((ID *)aa->act);
      }
      break;
    }*/
    case ACT_SOUND: {
      /*bSoundActuator *sa = (bSoundActuator *)act->data;
      if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
        id_us_plus((ID *)sa->sound);
      }
      break;*/
    }
  }
  return actn;
}

void BKE_sca_copy_actuators(ListBase *lbn, const ListBase *lbo)
{
  bActuator *act, *actn;

  lbn->first = lbn->last = nullptr;
  act = (bActuator *)lbo->first;
  while (act) {
    actn = BKE_sca_copy_actuator(act);
    BLI_addtail(lbn, actn);
    act = act->next;
  }
}

void BKE_sca_init_actuator(bActuator *act)
{
  /* also use when actuator changes type */
  bCameraActuator *ca;
  bObjectActuator *oa;
  bRandomActuator *ra;
  bSoundActuator *sa;
  bSteeringActuator *sta;
  bArmatureActuator *arma;
  bMouseActuator *ma;
  bEditObjectActuator *eoa;
  bVibrationActuator *via;

  if (act->data)
    MEM_freeN(act->data);
  act->data = nullptr;

  switch (act->type) {
    case ACT_ACTION:
      act->data = MEM_callocN(sizeof(bActionActuator), "actionact");
      break;
    case ACT_SOUND:
      act->data = MEM_callocN(sizeof(bSoundActuator), "soundact");
      sa = (bSoundActuator *)act->data;
      sa->volume = 1.0f;
      sa->sound3D.rolloff_factor = 1.0f;
      sa->sound3D.reference_distance = 1.0f;
      sa->sound3D.max_gain = 1.0f;
      sa->sound3D.cone_inner_angle = DEG2RADF(360.0f);
      sa->sound3D.cone_outer_angle = DEG2RADF(360.0f);
      sa->sound3D.max_distance = FLT_MAX;
      break;
    case ACT_OBJECT:
      act->data = MEM_callocN(sizeof(bObjectActuator), "objectact");
      oa = (bObjectActuator *)act->data;
      oa->flag = 15;
      break;
    case ACT_PROPERTY:
      act->data = MEM_callocN(sizeof(bPropertyActuator), "propact");
      break;
    case ACT_CAMERA:
      act->data = MEM_callocN(sizeof(bCameraActuator), "camact");
      ca = (bCameraActuator *)act->data;
      ca->axis = OB_POSX;
      ca->damping = 1.0 / 32.0;
      break;
    case ACT_EDIT_OBJECT:
      act->data = MEM_callocN(sizeof(bEditObjectActuator), "editobact");
      eoa = (bEditObjectActuator *)act->data;
      eoa->upflag = ACT_TRACK_UP_Z;
      eoa->trackflag = ACT_TRACK_TRAXIS_Y;
      break;
    case ACT_CONSTRAINT:
      act->data = MEM_callocN(sizeof(bConstraintActuator), "cons act");
      break;
    case ACT_SCENE:
      act->data = MEM_callocN(sizeof(bSceneActuator), "scene act");
      break;
    case ACT_COLLECTION:
      act->data = MEM_callocN(sizeof(bCollectionActuator), "collection act");
      break;
    case ACT_GROUP:
      act->data = MEM_callocN(sizeof(bGroupActuator), "group act");
      break;
    case ACT_RANDOM:
      act->data = MEM_callocN(sizeof(bRandomActuator), "random act");
      ra = (bRandomActuator *)act->data;
      ra->float_arg_1 = 0.1f;
      break;
    case ACT_MESSAGE:
      act->data = MEM_callocN(sizeof(bMessageActuator), "message act");
      break;
    case ACT_GAME:
      act->data = MEM_callocN(sizeof(bGameActuator), "game act");
      break;
    case ACT_VIBRATION:
      act->data = MEM_callocN(sizeof(bVibrationActuator), "vibration act");
      via = (bVibrationActuator *)act->data;
      via->duration = 500;  // milliseconds
      via->strength = 0.4;
      break;
    case ACT_VISIBILITY:
      act->data = MEM_callocN(sizeof(bVisibilityActuator), "visibility act");
      break;
    case ACT_2DFILTER:
      act->data = MEM_callocN(sizeof(bTwoDFilterActuator), "2d filter act");
      break;
    case ACT_PARENT:
      act->data = MEM_callocN(sizeof(bParentActuator), "parent act");
      break;
    case ACT_STATE:
      act->data = MEM_callocN(sizeof(bStateActuator), "state act");
      break;
    case ACT_ARMATURE:
      act->data = MEM_callocN(sizeof(bArmatureActuator), "armature act");
      arma = (bArmatureActuator *)act->data;
      arma->influence = 1.f;
      break;
    case ACT_STEERING:
      act->data = MEM_callocN(sizeof(bSteeringActuator), "steering act");
      sta = (bSteeringActuator *)act->data;
      sta->acceleration = 3.f;
      sta->turnspeed = 120.f;
      sta->dist = 1.f;
      sta->velocity = 3.f;
      sta->flag = ACT_STEERING_AUTOMATICFACING | ACT_STEERING_LOCKZVEL;
      sta->facingaxis = 1;
      break;
    case ACT_MOUSE:
      act->data = MEM_callocN(sizeof(bMouseActuator), "mouse act");
      ma = (bMouseActuator *)act->data;
      ma->flag = ACT_MOUSE_VISIBLE | ACT_MOUSE_USE_AXIS_X | ACT_MOUSE_USE_AXIS_Y |
                 ACT_MOUSE_RESET_X | ACT_MOUSE_RESET_Y | ACT_MOUSE_LOCAL_Y;
      ma->sensitivity[0] = ma->sensitivity[1] = 2.f;
      ma->object_axis[0] = ACT_MOUSE_OBJECT_AXIS_Z;
      ma->object_axis[1] = ACT_MOUSE_OBJECT_AXIS_X;
      ma->limit_y[0] = DEG2RADF(-90.0f);
      ma->limit_y[1] = DEG2RADF(90.0f);
      break;
    default:; /* this is very severe... I cannot make any memory for this        */
              /* logic brick...                                                    */
  }
}

bActuator *BKE_sca_new_actuator(int type)
{
  bActuator *act;

  act = (bActuator *)MEM_callocN(sizeof(bActuator), "Actuator");
  act->type = type;
  act->flag = ACT_SHOW;

  BKE_sca_init_actuator(act);

  strcpy(act->name, "act");
  // XXX	make_unique_prop_names(act->name);

  return act;
}

/* ******************** GENERAL ******************* */
void BKE_sca_clear_new_points_ob(Object *ob)
{
  bSensor *sens;
  bController *cont;
  bActuator *act;

  sens = (bSensor *)ob->sensors.first;
  while (sens) {
    sens->flag &= ~SENS_NEW;
    sens = sens->next;
  }
  cont = (bController *)ob->controllers.first;
  while (cont) {
    cont->mynew = nullptr;
    cont->flag &= ~CONT_NEW;
    cont = cont->next;
  }
  act = (bActuator *)ob->actuators.first;
  while (act) {
    act->mynew = nullptr;
    act->flag &= ~ACT_NEW;
    act = act->next;
  }
}

void BKE_sca_clear_new_points(void)
{
  Object *ob;

  ob = (Object *)G.main->objects.first;
  while (ob) {
    BKE_sca_clear_new_points_ob(ob);
    ob = (Object *)ob->id.next;
  }
}

void BKE_sca_set_new_points_ob(Object *ob)
{
  bSensor *sens;
  bController *cont;
  bActuator *act;

  int a;

  sens = (bSensor *)ob->sensors.first;
  while (sens) {
    if (sens->flag & SENS_NEW) {
      for (a = 0; a < sens->totlinks; a++) {
        if (sens->links[a] && sens->links[a]->mynew)
          sens->links[a] = sens->links[a]->mynew;
      }
    }
    sens = sens->next;
  }

  cont = (bController *)ob->controllers.first;
  while (cont) {
    if (cont->flag & CONT_NEW) {
      for (a = 0; a < cont->totlinks; a++) {
        if (cont->links[a] && cont->links[a]->mynew)
          cont->links[a] = cont->links[a]->mynew;
      }
    }
    cont = cont->next;
  }

  act = (bActuator *)ob->actuators.first;
  while (act) {
    if (act->flag & ACT_NEW) {
      if (act->type == ACT_EDIT_OBJECT) {
        bEditObjectActuator *eoa = (bEditObjectActuator *)act->data;
        ID_NEW_REMAP(eoa->ob);
      }
      else if (act->type == ACT_SCENE) {
        bSceneActuator *sca = (bSceneActuator *)act->data;
        ID_NEW_REMAP(sca->camera);
      }
      else if (act->type == ACT_COLLECTION) {
        bCollectionActuator *ca = (bCollectionActuator *)act->data;
        ID_NEW_REMAP(ca->collection);
        ID_NEW_REMAP(ca->camera);
      }
      else if (act->type == ACT_CAMERA) {
        bCameraActuator *ca = (bCameraActuator *)act->data;
        ID_NEW_REMAP(ca->ob);
      }
      else if (act->type == ACT_OBJECT) {
        bObjectActuator *oa = (bObjectActuator *)act->data;
        ID_NEW_REMAP(oa->reference);
      }
      else if (act->type == ACT_MESSAGE) {
        bMessageActuator *ma = (bMessageActuator *)act->data;
        ID_NEW_REMAP(ma->toObject);
      }
      else if (act->type == ACT_PARENT) {
        bParentActuator *para = (bParentActuator *)act->data;
        ID_NEW_REMAP(para->ob);
      }
      else if (act->type == ACT_ARMATURE) {
        bArmatureActuator *aa = (bArmatureActuator *)act->data;
        ID_NEW_REMAP(aa->target);
        ID_NEW_REMAP(aa->subtarget);
      }
      else if (act->type == ACT_PROPERTY) {
        bPropertyActuator *pa = (bPropertyActuator *)act->data;
        ID_NEW_REMAP(pa->ob);
      }
      else if (act->type == ACT_STEERING) {
        bSteeringActuator *sta = (bSteeringActuator *)act->data;
        ID_NEW_REMAP(sta->navmesh);
        ID_NEW_REMAP(sta->target);
      }
    }
    act = act->next;
  }
}

void BKE_sca_set_new_points(void)
{
  Object *ob;

  ob = (Object *)G.main->objects.first;
  while (ob) {
    BKE_sca_set_new_points_ob(ob);
    ob = (Object *)ob->id.next;
  }
}

/**
 * Try to remap logic links to new object... Very, *very* weak.
 */
/* XXX Logick bricks... I don't have words to say what I think about this behavior.
 *     They have silent hidden ugly inter-objects dependencies (a sensor can link into any other
 *     object's controllers, and same between controllers and actuators, without *any* explicit
 * reference to data-block involved). This is bad, bad, bad!!!
 *     ...and forces us to add yet another very ugly hack to get remapping with logic bricks
 * working. */
void BKE_sca_remap_data_postprocess_links_logicbricks_update(Main *bmain, Object *ob_old, Object *ob_new)
{
  if (ob_new == nullptr || (ob_old->controllers.first == nullptr && ob_old->actuators.first == nullptr)) {
    /* Nothing to do here... */
    return;
  }

  GHash *controllers_map = ob_old->controllers.first ?
                               BLI_ghash_ptr_new_ex(__func__,
                                                    BLI_listbase_count(&ob_old->controllers)) :
                               nullptr;
  GHash *actuators_map = ob_old->actuators.first ?
                             BLI_ghash_ptr_new_ex(__func__,
                                                  BLI_listbase_count(&ob_old->actuators)) :
                             nullptr;

  /* We try to remap old controllers/actuators to new ones - in a very basic way. */
  for (bController *cont_old = (bController *)ob_old->controllers.first,
                   *cont_new = (bController *)ob_new->controllers.first;
       cont_old;
       cont_old = cont_old->next) {
    bController *cont_new2 = cont_new;

    if (cont_old->mynew != nullptr) {
      cont_new2 = cont_old->mynew;
      if (!(cont_new2 == cont_new || BLI_findindex(&ob_new->controllers, cont_new2) >= 0)) {
        cont_new2 = nullptr;
      }
    }
    else if (cont_new && cont_old->type != cont_new->type) {
      cont_new2 = nullptr;
    }

    BLI_ghash_insert(controllers_map, cont_old, cont_new2);

    if (cont_new) {
      cont_new = cont_new->next;
    }
  }

  for (bActuator *act_old = (bActuator *)ob_old->actuators.first,
                 *act_new = (bActuator *)ob_new->actuators.first;
       act_old;
       act_old = act_old->next) {
    bActuator *act_new2 = act_new;

    if (act_old->mynew != nullptr) {
      act_new2 = act_old->mynew;
      if (!(act_new2 == act_new || BLI_findindex(&ob_new->actuators, act_new2) >= 0)) {
        act_new2 = nullptr;
      }
    }
    else if (act_new && act_old->type != act_new->type) {
      act_new2 = nullptr;
    }

    BLI_ghash_insert(actuators_map, act_old, act_new2);

    if (act_new) {
      act_new = act_new->next;
    }
  }

  for (Object *ob = (Object *)bmain->objects.first; ob; ob = (Object *)ob->id.next) {
    if (controllers_map != nullptr) {
      for (bSensor *sens = (bSensor *)ob->sensors.first; sens; sens = sens->next) {
        for (int a = 0; a < sens->totlinks; a++) {
          if (sens->links[a]) {
            bController *old_link = sens->links[a];
            bController **new_link_p = (bController **)BLI_ghash_lookup_p(controllers_map,
                                                                          old_link);

            if (new_link_p == nullptr) {
              /* old_link is *not* in map's keys (i.e. not to any ob_old->controllers),
               * which means we ignore it totally here. */
            }
            else if (*new_link_p == nullptr) {
              BKE_sca_unlink_logicbricks(
                  (void **)&old_link, (void ***)&(sens->links), &sens->totlinks);
              a--;
            }
            else {
              sens->links[a] = *new_link_p;
            }
          }
        }
      }
    }

    if (actuators_map != nullptr) {
      for (bController *cont = (bController *)ob->controllers.first; cont; cont = cont->next) {
        for (int a = 0; a < cont->totlinks; a++) {
          if (cont->links[a]) {
            bActuator *old_link = cont->links[a];
            bActuator **new_link_p = (bActuator **)BLI_ghash_lookup_p(actuators_map, old_link);

            if (new_link_p == nullptr) {
              /* old_link is *not* in map's keys (i.e. not to any ob_old->actuators),
               * which means we ignore it totally here. */
            }
            else if (*new_link_p == nullptr) {
              BKE_sca_unlink_logicbricks(
                  (void **)&old_link, (void ***)&(cont->links), &cont->totlinks);
              a--;
            }
            else {
              cont->links[a] = *new_link_p;
            }
          }
        }
      }
    }
  }

  if (controllers_map) {
    BLI_ghash_free(controllers_map, nullptr, nullptr);
  }
  if (actuators_map) {
    BLI_ghash_free(actuators_map, nullptr, nullptr);
  }
}

/**
 * Handle the copying of logic data into a new object, including internal logic links update.
 * External links (links between logic bricks of different objects) must be handled separately.
 */
void BKE_sca_copy_logicbricks(Object *ob_new, const Object *ob, const int flag)
{
  BKE_sca_copy_sensors(&ob_new->sensors, &ob->sensors, flag);
  BKE_sca_copy_controllers(&ob_new->controllers, &ob->controllers, flag);
  BKE_sca_copy_actuators(&ob_new->actuators, &ob->actuators);

  for (bSensor *sens = (bSensor *)ob_new->sensors.first; sens; sens = sens->next) {
    if (sens->flag & SENS_NEW) {
      for (int a = 0; a < sens->totlinks; a++) {
        if (sens->links[a] && sens->links[a]->mynew) {
          sens->links[a] = sens->links[a]->mynew;
        }
      }
    }
  }

  for (bController *cont = (bController *)ob_new->controllers.first; cont; cont = cont->next) {
    if (cont->flag & CONT_NEW) {
      for (int a = 0; a < cont->totlinks; a++) {
        if (cont->links[a] && cont->links[a]->mynew) {
          cont->links[a] = cont->links[a]->mynew;
        }
      }
    }
  }
}

/* ******************** INTERFACE ******************* */
void BKE_sca_move_sensor(bSensor *sens_to_move, Object *ob, int move_up)
{
  bSensor *sens, *tmp;

  int val;
  val = move_up ? 1 : 2;

  /* make sure this sensor belongs to this object */
  sens = (bSensor *)ob->sensors.first;
  while (sens) {
    if (sens == sens_to_move)
      break;
    sens = sens->next;
  }
  if (!sens)
    return;

  /* move up */
  if (val == 1 && sens->prev) {
    for (tmp = sens->prev; tmp; tmp = tmp->prev) {
      if (tmp->flag & SENS_VISIBLE)
        break;
    }
    if (tmp) {
      BLI_remlink(&ob->sensors, sens);
      BLI_insertlinkbefore(&ob->sensors, tmp, sens);
    }
  }
  /* move down */
  else if (val == 2 && sens->next) {
    for (tmp = sens->next; tmp; tmp = tmp->next) {
      if (tmp->flag & SENS_VISIBLE)
        break;
    }
    if (tmp) {
      BLI_remlink(&ob->sensors, sens);
      BLI_insertlinkafter(&ob->sensors, tmp, sens);
    }
  }
}

void BKE_sca_move_controller(bController *cont_to_move, Object *ob, int move_up)
{
  bController *cont, *tmp;

  int val;
  val = move_up ? 1 : 2;

  /* make sure this controller belongs to this object */
  cont = (bController *)ob->controllers.first;
  while (cont) {
    if (cont == cont_to_move)
      break;
    cont = cont->next;
  }
  if (!cont)
    return;

  /* move up */
  if (val == 1 && cont->prev) {
    /* locate the controller that has the same state mask but is earlier in the list */
    tmp = cont->prev;
    while (tmp) {
      if (tmp->state_mask & cont->state_mask)
        break;
      tmp = tmp->prev;
    }
    if (tmp) {
      BLI_remlink(&ob->controllers, cont);
      BLI_insertlinkbefore(&ob->controllers, tmp, cont);
    }
  }

  /* move down */
  else if (val == 2 && cont->next) {
    tmp = cont->next;
    while (tmp) {
      if (tmp->state_mask & cont->state_mask)
        break;
      tmp = tmp->next;
    }
    BLI_remlink(&ob->controllers, cont);
    BLI_insertlinkafter(&ob->controllers, tmp, cont);
  }
}

void BKE_sca_move_actuator(bActuator *act_to_move, Object *ob, int move_up)
{
  bActuator *act, *tmp;
  int val;

  val = move_up ? 1 : 2;

  /* make sure this actuator belongs to this object */
  act = (bActuator *)ob->actuators.first;
  while (act) {
    if (act == act_to_move)
      break;
    act = act->next;
  }
  if (!act)
    return;

  /* move up */
  if (val == 1 && act->prev) {
    /* locate the first visible actuators before this one */
    for (tmp = act->prev; tmp; tmp = tmp->prev) {
      if (tmp->flag & ACT_VISIBLE)
        break;
    }
    if (tmp) {
      BLI_remlink(&ob->actuators, act);
      BLI_insertlinkbefore(&ob->actuators, tmp, act);
    }
  }
  /* move down */
  else if (val == 2 && act->next) {
    /* locate the first visible actuators after this one */
    for (tmp = act->next; tmp; tmp = tmp->next) {
      if (tmp->flag & ACT_VISIBLE)
        break;
    }
    if (tmp) {
      BLI_remlink(&ob->actuators, act);
      BLI_insertlinkafter(&ob->actuators, tmp, act);
    }
  }
}

void BKE_sca_link_logicbricks(void **poin, void ***ppoin, short *tot, short size)
{
  void **old_links = nullptr;

  int ibrick;

  /* check if the bricks are already linked */
  for (ibrick = 0; ibrick < *tot; ibrick++) {
    if ((*ppoin)[ibrick] == *poin)
      return;
  }

  if (*ppoin) {
    old_links = *ppoin;

    (*tot)++;
    *ppoin = (void **)MEM_callocN((*tot) * size, "new link");

    for (ibrick = 0; ibrick < *(tot)-1; ibrick++) {
      (*ppoin)[ibrick] = old_links[ibrick];
    }
    (*ppoin)[ibrick] = *poin;

    if (old_links)
      MEM_freeN(old_links);
  }
  else {
    (*tot) = 1;
    *ppoin = (void **)MEM_callocN((*tot) * size, "new link");
    (*ppoin)[0] = *poin;
  }
}

void BKE_sca_unlink_logicbricks(void **poin, void ***ppoin, short *tot)
{
  int ibrick, removed;

  removed = 0;
  for (ibrick = 0; ibrick < *tot; ibrick++) {
    if (removed)
      (*ppoin)[ibrick - removed] = (*ppoin)[ibrick];
    else if ((*ppoin)[ibrick] == *poin)
      removed = 1;
  }

  if (removed) {
    (*tot)--;

    if (*tot == 0) {
      MEM_freeN(*ppoin);
      (*ppoin) = nullptr;
    }
    return;
  }
}

void BKE_sca_sensors_id_loop(ListBase *senslist, SCASensorIDFunc func, void *userdata)
{
  bSensor *sensor;

  for (sensor = (bSensor *)senslist->first; sensor; sensor = sensor->next) {
    func(sensor, (ID **)&sensor->ob, userdata, IDWALK_CB_NOP);

    switch (sensor->type) {
      case SENS_TOUCH: /* DEPRECATED */
      {
        bTouchSensor *ts = (bTouchSensor *)sensor->data;
        func(sensor, (ID **)&ts->ma, userdata, IDWALK_CB_NOP);
        break;
      }
      case SENS_MESSAGE: {
        bMessageSensor *ms = (bMessageSensor *)sensor->data;
        func(sensor, (ID **)&ms->fromObject, userdata, IDWALK_CB_NOP);
        break;
      }
      case SENS_ALWAYS:
      case SENS_NEAR:
      case SENS_KEYBOARD:
      case SENS_PROPERTY:
      case SENS_MOUSE:
      case SENS_COLLISION:
      case SENS_RADAR:
      case SENS_RANDOM:
      case SENS_RAY:
      case SENS_MOVEMENT:
      case SENS_JOYSTICK:
      case SENS_ACTUATOR:
      case SENS_DELAY:
      case SENS_ARMATURE:
      default:
        break;
    }
  }
}

void BKE_sca_controllers_id_loop(ListBase *contlist, SCAControllerIDFunc func, void *userdata)
{
  bController *controller;

  for (controller = (bController *)contlist->first; controller; controller = controller->next) {
    switch (controller->type) {
      case CONT_PYTHON: {
        bPythonCont *pc = (bPythonCont *)controller->data;
        // if (strlen(pc->module)) {
        //  if (!pc->module_script) {
        //    char modulename[FILE_MAX];
        //    BLI_strncpy(modulename, pc->module, sizeof(modulename));
        //    char ext[FILE_MAX];
        //    strcpy(ext, ".py");
        //    char dest[FILE_MAX];
        //    strcpy(dest, "");
        //    char *classname;
        //    char *pos = strrchr(modulename, '.');
        //    if (pos) {
        //      *pos = '\0';
        //      classname = pos + 1;
        //    }
        //    strcat(dest, modulename);
        //    strcat(dest, ext);
        //    if (G_MAIN /* && G.file_loaded */) { // FIXME: Need to wait file has been completely
        //    read
        //      LISTBASE_FOREACH (Text *, text, &G_MAIN->texts) {
        //        if (strcmp(text->id.name + 2, dest) == 0) {
        //          if (text->filepath == nullptr) {  // Means the script is embedded
        //            pc->module_script = text;
        //          }
        //          break;
        //        }
        //      }
        //    }
        //  }
        //}

        func(controller, (ID **)&pc->module_script, userdata, IDWALK_CB_USER);
        func(controller, (ID **)&pc->text, userdata, IDWALK_CB_USER);
        break;
      }
      case CONT_LOGIC_AND:
      case CONT_LOGIC_OR:
      case CONT_EXPRESSION:
      case CONT_LOGIC_NAND:
      case CONT_LOGIC_NOR:
      case CONT_LOGIC_XOR:
      case CONT_LOGIC_XNOR:
      default:
        break;
    }
  }
}

void BKE_sca_actuators_id_loop(ListBase *actlist, SCAActuatorIDFunc func, void *userdata)
{
  bActuator *actuator;

  for (actuator = (bActuator *)actlist->first; actuator; actuator = actuator->next) {
    func(actuator, (ID **)&actuator->ob, userdata, IDWALK_CB_NOP);

    /*
     * Using IDWALK_CB_USER for pointer references to prevent a problem
     * with recomputing refcount upon loading. Needs more testing to
     * see if it has potential side-effects.
     *
     * (See https://github.com/UPBGE/upbge/pull/1371 for details.)
     */
    switch (actuator->type) {
      case ACT_ADD_OBJECT: /* DEPRECATED */
      {
        bAddObjectActuator *aoa = (bAddObjectActuator *)actuator->data;
        func(actuator, (ID **)&aoa->ob, userdata, IDWALK_CB_NOP);
        break;
      }
      case ACT_ACTION: {
        bActionActuator *aa = (bActionActuator *)actuator->data;
        func(actuator, (ID **)&aa->act, userdata, IDWALK_CB_USER);
        break;
      }
      case ACT_SOUND: {
        bSoundActuator *sa = (bSoundActuator *)actuator->data;
        func(actuator, (ID **)&sa->sound, userdata, IDWALK_CB_USER);
        break;
      }
      case ACT_EDIT_OBJECT: {
        bEditObjectActuator *eoa = (bEditObjectActuator *)actuator->data;
        func(actuator, (ID **)&eoa->ob, userdata, IDWALK_CB_NOP);
        func(actuator, (ID **)&eoa->me, userdata, IDWALK_CB_USER);
        break;
      }
      case ACT_SCENE: {
        bSceneActuator *sa = (bSceneActuator *)actuator->data;
        func(actuator, (ID **)&sa->scene, userdata, IDWALK_CB_NOP);
        func(actuator, (ID **)&sa->camera, userdata, IDWALK_CB_NOP);
        break;
      }
      case ACT_COLLECTION: {
        bCollectionActuator *ca = (bCollectionActuator *)actuator->data;
        func(actuator, (ID **)&ca->collection, userdata, IDWALK_CB_USER);
        func(actuator, (ID **)&ca->camera, userdata, IDWALK_CB_NOP);
        break;
      }
      case ACT_PROPERTY: {
        bPropertyActuator *pa = (bPropertyActuator *)actuator->data;
        func(actuator, (ID **)&pa->ob, userdata, IDWALK_CB_NOP);
        break;
      }
      case ACT_OBJECT: {
        bObjectActuator *oa = (bObjectActuator *)actuator->data;
        func(actuator, (ID **)&oa->reference, userdata, IDWALK_CB_NOP);
        break;
      }
      case ACT_CAMERA: {
        bCameraActuator *ca = (bCameraActuator *)actuator->data;
        func(actuator, (ID **)&ca->ob, userdata, IDWALK_CB_NOP);
        break;
      }
      case ACT_MESSAGE: {
        bMessageActuator *ma = (bMessageActuator *)actuator->data;
        func(actuator, (ID **)&ma->toObject, userdata, IDWALK_CB_NOP);
        break;
      }
      case ACT_2DFILTER: {
        bTwoDFilterActuator *tdfa = (bTwoDFilterActuator *)actuator->data;
        func(actuator, (ID **)&tdfa->text, userdata, IDWALK_CB_USER);
        break;
      }
      case ACT_PARENT: {
        bParentActuator *pa = (bParentActuator *)actuator->data;
        func(actuator, (ID **)&pa->ob, userdata, IDWALK_CB_NOP);
        break;
      }
      case ACT_ARMATURE: {
        bArmatureActuator *aa = (bArmatureActuator *)actuator->data;
        func(actuator, (ID **)&aa->target, userdata, IDWALK_CB_NOP);
        func(actuator, (ID **)&aa->subtarget, userdata, IDWALK_CB_NOP);
        break;
      }
      case ACT_STEERING: {
        bSteeringActuator *sa = (bSteeringActuator *)actuator->data;
        func(actuator, (ID **)&sa->target, userdata, IDWALK_CB_NOP);
        func(actuator, (ID **)&sa->navmesh, userdata, IDWALK_CB_NOP);
        break;
      }
      /* Note: some types seems to be non-implemented? ACT_LAMP, ACT_MATERIAL... */
      case ACT_LAMP:
      case ACT_MATERIAL:
      case ACT_END_OBJECT: /* DEPRECATED */
      case ACT_CONSTRAINT:
      case ACT_GROUP:
      case ACT_RANDOM:
      case ACT_GAME:
      case ACT_VIBRATION:
      case ACT_VISIBILITY:
      case ACT_SHAPEACTION:
      case ACT_STATE:
      case ACT_MOUSE:
      default:
        break;
    }
  }
}

const char *BKE_sca_get_name_state(Object *ob, short bit)
{
  bController *cont;
  unsigned int mask;

  mask = (1 << bit);
  cont = (bController *)ob->controllers.first;
  while (cont) {
    if (cont->state_mask & mask) {
      return cont->name;
    }
    cont = cont->next;
  }
  return nullptr;
}
