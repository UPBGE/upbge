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
 * The Original Code is Copyright (C) 2009 Blender Foundation.
 * All rights reserved.
 *
 *
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/space_logic/logic_window.c
 *  \ingroup splogic
 */

#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "DNA_actuator_types.h"
#include "DNA_constraint_types.h"
#include "DNA_controller_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_property_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_sensor_types.h"
#include "DNA_sound_types.h"
#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utils.hh"

#include "BKE_action.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_sca.hh"

#include "ED_undo.hh"

#include "BLT_translation.hh"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "RNA_access.hh"
#include "rna_prototypes.hh"

/* XXX BAD BAD */
#include "../interface/interface_intern.hh"

#include "logic_intern.hh"

namespace blender {

using namespace blender::ui;

#define B_REDR 1

#define B_ADD_SENS 2703
#define B_CHANGE_SENS 2704
#define B_DEL_SENS 2705

#define B_ADD_CONT 2706
#define B_CHANGE_CONT 2707
#define B_DEL_CONT 2708

#define B_ADD_ACT 2709
#define B_CHANGE_ACT 2710
#define B_DEL_ACT 2711

#define B_SOUNDACT_BROWSE 2712

#define B_SETPROP 2714
#define B_SETACTOR 2715
#define B_SETMAINACTOR 2716
#define B_SETDYNA 2717
#define B_SET_STATE_BIT 2718
#define B_INIT_STATE_BIT 2719

/* proto */
static ID **get_selected_and_linked_obs(bContext *C, short *count, short scavisflag);

static void do_logic_buts(bContext *C, void */*arg*/, int event)
{
  Main *bmain = CTX_data_main(C);
  bSensor *sens;
  bController *cont;
  bActuator *act;
  Object *ob;
  int didit, bit;

  ob = CTX_data_active_object(C);
  if (ob == nullptr)
    return;

  switch (event) {

    case B_SETPROP:
      /* check for inconsistent types */
      ob->gameflag &= ~(OB_SECTOR | OB_MAINACTOR | OB_DYNAMIC | OB_ACTOR);
      break;

    case B_SETACTOR:
    case B_SETDYNA:
    case B_SETMAINACTOR:
      ob->gameflag &= ~(OB_SECTOR | OB_PROP);
      break;

    case B_ADD_SENS:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        if (ob->scaflag & OB_ADDSENS) {
          ob->scaflag &= ~OB_ADDSENS;
          sens = BKE_sca_new_sensor(SENS_ALWAYS);
          BLI_addtail(&(ob->sensors), sens);
          BLI_uniquename(&ob->sensors,
                         sens,
                         DATA_("Sensor"),
                         '.',
                         offsetof(bSensor, name),
                         sizeof(sens->name));
          ob->scaflag |= OB_SHOWSENS;
        }
      }
      ED_undo_push_old(C, "sensor add");
      break;

    case B_CHANGE_SENS:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        sens = static_cast<bSensor *>(ob->sensors.first);
        while (sens) {
          if (sens->type != sens->otype) {
            BKE_sca_init_sensor(sens);
            sens->otype = sens->type;
            break;
          }
          sens = sens->next;
        }
      }
      break;

    case B_DEL_SENS:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        sens = static_cast<bSensor *>(ob->sensors.first);
        while (sens) {
          if (sens->flag & SENS_DEL) {
            BLI_remlink(&(ob->sensors), sens);
            BKE_sca_free_sensor(sens);
            break;
          }
          sens = sens->next;
        }
      }
      ED_undo_push_old(C, "sensor delete");
      break;

    case B_ADD_CONT:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        if (ob->scaflag & OB_ADDCONT) {
          ob->scaflag &= ~OB_ADDCONT;
          cont = BKE_sca_new_controller(CONT_LOGIC_AND);
          BLI_uniquename(&ob->controllers,
                         cont,
                         DATA_("Controller"),
                         '.',
                         offsetof(bController, name),
                         sizeof(cont->name));
          ob->scaflag |= OB_SHOWCONT;
          BLI_addtail(&(ob->controllers), cont);
          /* set the controller state mask from the current object state.
           * A controller is always in a single state, so select the lowest bit set
           * from the object state */
          for (bit = 0; bit < 32; bit++) {
            if (ob->state & (1 << bit))
              break;
          }
          cont->state_mask = (1 << bit);
          if (cont->state_mask == 0) {
            /* shouldn't happen, object state is never 0 */
            cont->state_mask = 1;
          }
        }
      }
      ED_undo_push_old(C, "controller add");
      break;

    case B_SET_STATE_BIT:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        if (ob->scaflag & OB_ALLSTATE) {
          ob->scaflag &= ~OB_ALLSTATE;
          ob->state = 0x3FFFFFFF;
        }
      }
      break;

    case B_INIT_STATE_BIT:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        if (ob->scaflag & OB_INITSTBIT) {
          ob->scaflag &= ~OB_INITSTBIT;
          ob->state = ob->init_state;
          if (!ob->state)
            ob->state = 1;
        }
      }
      break;

    case B_CHANGE_CONT:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        cont = static_cast<bController *>(ob->controllers.first);
        while (cont) {
          if (cont->type != cont->otype) {
            BKE_sca_init_controller(cont);
            cont->otype = cont->type;
            break;
          }
          cont = cont->next;
        }
      }
      break;

    case B_DEL_CONT:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        cont = static_cast<bController *>(ob->controllers.first);
        while (cont) {
          if (cont->flag & CONT_DEL) {
            BLI_remlink(&(ob->controllers), cont);
            BKE_sca_unlink_controller(cont);
            BKE_sca_free_controller(cont);
            break;
          }
          cont = cont->next;
        }
      }
      ED_undo_push_old(C, "controller delete");
      break;

    case B_ADD_ACT:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        if (ob->scaflag & OB_ADDACT) {
          ob->scaflag &= ~OB_ADDACT;
          act = BKE_sca_new_actuator(ACT_OBJECT);
          BLI_uniquename(&ob->actuators,
                         act,
                         DATA_("Actuator"),
                         '.',
                         offsetof(bActuator, name),
                         sizeof(act->name));
          BLI_addtail(&(ob->actuators), act);
          ob->scaflag |= OB_SHOWACT;
        }
      }
      ED_undo_push_old(C, "actuator add");
      break;

    case B_CHANGE_ACT:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        act = static_cast<bActuator *>(ob->actuators.first);
        while (act) {
          if (act->type != act->otype) {
            BKE_sca_init_actuator(act);
            act->otype = act->type;
            break;
          }
          act = act->next;
        }
      }
      break;

    case B_DEL_ACT:
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        act = static_cast<bActuator *>(ob->actuators.first);
        while (act) {
          if (act->flag & ACT_DEL) {
            BLI_remlink(&(ob->actuators), act);
            BKE_sca_unlink_actuator(act);
            BKE_sca_free_actuator(act);
            break;
          }
          act = act->next;
        }
      }
      ED_undo_push_old(C, "actuator delete");
      break;

    case B_SOUNDACT_BROWSE:
      /* since we don't know which... */
      didit = 0;
      for (ob = static_cast<Object *>(bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next)) {
        act = static_cast<bActuator *>(ob->actuators.first);
        while (act) {
          if (act->type == ACT_SOUND) {
            bSoundActuator *sa = (bSoundActuator *)act->data;
            if (sa->sndnr) {
              ID *sound = (ID *)bmain->sounds.first;
              int nr = 1;

              if (sa->sndnr == -2) {
                // XXX							activate_databrowse((ID *)bmain->sound.first, ID_SO, 0,
                // B_SOUNDACT_BROWSE,
                //											&sa->sndnr, do_logic_buts);
                break;
              }

              while (sound) {
                if (nr == sa->sndnr)
                  break;
                nr++;
                sound = (ID *)sound->next;
              }

              if (sa->sound)
                id_us_min(((ID *)sa->sound));

              sa->sound = (bSound *)sound;

              if (sound) {
                id_us_plus(sound);
              }

              sa->sndnr = 0;
              didit = 1;
            }
          }
          act = act->next;
        }
        if (didit)
          break;
      }

      break;
  }
}

static const char *sensor_name(int type)
{
  switch (type) {
    case SENS_ALWAYS:
      return N_("Always");
    case SENS_NEAR:
      return N_("Near");
    case SENS_KEYBOARD:
      return N_("Keyboard");
    case SENS_PROPERTY:
      return N_("Property");
    case SENS_ARMATURE:
      return N_("Armature");
    case SENS_ACTUATOR:
      return N_("Actuator");
    case SENS_DELAY:
      return N_("Delay");
    case SENS_MOUSE:
      return N_("Mouse");
    case SENS_COLLISION:
      return N_("Collision");
    case SENS_RADAR:
      return N_("Radar");
    case SENS_RANDOM:
      return N_("Random");
    case SENS_RAY:
      return N_("Ray");
    case SENS_MOVEMENT:
      return N_("Movement");
    case SENS_MESSAGE:
      return N_("Message");
    case SENS_JOYSTICK:
      return N_("Joystick");
  }
  return N_("Unknown");
}

static const char *controller_name(int type)
{
  switch (type) {
    case CONT_LOGIC_AND:
      return N_("And");
    case CONT_LOGIC_OR:
      return N_("Or");
    case CONT_LOGIC_NAND:
      return N_("Nand");
    case CONT_LOGIC_NOR:
      return N_("Nor");
    case CONT_LOGIC_XOR:
      return N_("Xor");
    case CONT_LOGIC_XNOR:
      return N_("Xnor");
    case CONT_EXPRESSION:
      return N_("Expression");
    case CONT_PYTHON:
      return N_("Python");
  }
  return N_("Unknown");
}

static const char *actuator_name(int type)
{
  switch (type) {
    case ACT_ACTION:
      return N_("Action");
    case ACT_OBJECT:
      return N_("Motion");
    case ACT_LAMP:
      return N_("Lamp");
    case ACT_CAMERA:
      return N_("Camera");
    case ACT_MATERIAL:
      return N_("Material");
    case ACT_SOUND:
      return N_("Sound");
    case ACT_PROPERTY:
      return N_("Property");
    case ACT_EDIT_OBJECT:
      return N_("Edit Object");
    case ACT_CONSTRAINT:
      return N_("Constraint");
    case ACT_SCENE:
      return N_("Scene");
    case ACT_COLLECTION:
      return N_("Collection");
    case ACT_GROUP:
      return N_("Group");
    case ACT_RANDOM:
      return N_("Random");
    case ACT_MESSAGE:
      return N_("Message");
    case ACT_GAME:
      return N_("Game");
    case ACT_VISIBILITY:
      return N_("Visibility");
    case ACT_2DFILTER:
      return N_("Filter 2D");
    case ACT_PARENT:
      return N_("Parent");
    case ACT_STATE:
      return N_("State");
    case ACT_VIBRATION:
      return N_("Vibration");
    case ACT_ARMATURE:
      return N_("Armature");
    case ACT_STEERING:
      return N_("Steering");
    case ACT_MOUSE:
      return N_("Mouse");
  }
  return N_("Unknown");
}

static void set_sca_ob(Object *ob)
{
  bController *cont;
  bActuator *act;

  cont = static_cast<bController *>(ob->controllers.first);
  while (cont) {
    cont->mynew = (bController *)ob;
    cont = cont->next;
  }
  act = static_cast<bActuator *>(ob->actuators.first);
  while (act) {
    act->mynew = (bActuator *)ob;
    act = act->next;
  }
}

static ID **get_selected_and_linked_obs(bContext *C, short *count, short scavisflag)
{
  Base *base;
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  /* Add view_layer_synced_ensure here just in case,
   * before iteration on view_layer->object_bases */
  BKE_view_layer_synced_ensure(scene, view_layer);
  Object *ob, *obt, *obact = CTX_data_active_object(C);
  ID **idar;
  bSensor *sens;
  bController *cont;
  int a, nr, do_it;

  /* we need a sorted object list */
  /* set scavisflags flags in Objects to indicate these should be evaluated */
  /* also hide ob pointers in ->new entries of controllerss/actuators */

  *count = 0;

  if (scene == nullptr)
    return nullptr;

  ob = static_cast<Object *>(bmain->objects.first);
  while (ob) {
    ob->scavisflag = 0;
    set_sca_ob(ob);
    ob = static_cast<Object *>(ob->id.next);
  }

  for (base = static_cast<Base *>(view_layer->object_bases.first); base; base = base->next) {
    if ((base->flag & BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT) && (base->flag & SELECT)) {
      if (scavisflag & BUTS_SENS_SEL)
        base->object->scavisflag |= OB_VIS_SENS;
      if (scavisflag & BUTS_CONT_SEL)
        base->object->scavisflag |= OB_VIS_CONT;
      if (scavisflag & BUTS_ACT_SEL)
        base->object->scavisflag |= OB_VIS_ACT;
    }
  }

  if (obact) {
    if (scavisflag & BUTS_SENS_ACT)
      obact->scavisflag |= OB_VIS_SENS;
    if (scavisflag & BUTS_CONT_ACT)
      obact->scavisflag |= OB_VIS_CONT;
    if (scavisflag & BUTS_ACT_ACT)
      obact->scavisflag |= OB_VIS_ACT;
  }

  /* BUTS_XXX_STATE are similar to BUTS_XXX_LINK for selecting the object */
  if (scavisflag &
      (BUTS_SENS_LINK | BUTS_CONT_LINK | BUTS_ACT_LINK | BUTS_SENS_STATE | BUTS_ACT_STATE)) {
    do_it = true;
    while (do_it) {
      do_it = false;

      ob = static_cast<Object *>(bmain->objects.first);
      while (ob) {

        /* 1st case: select sensor when controller selected */
        if ((scavisflag & (BUTS_SENS_LINK | BUTS_SENS_STATE)) &&
            (ob->scavisflag & OB_VIS_SENS) == 0) {
          sens = static_cast<bSensor *>(ob->sensors.first);
          while (sens) {
            for (a = 0; a < sens->totlinks; a++) {
              if (sens->links[a]) {
                obt = (Object *)sens->links[a]->mynew;
                if (obt && (obt->scavisflag & OB_VIS_CONT)) {
                  do_it = true;
                  ob->scavisflag |= OB_VIS_SENS;
                  break;
                }
              }
            }
            if (do_it)
              break;
            sens = sens->next;
          }
        }

        /* 2nd case: select cont when act selected */
        if ((scavisflag & BUTS_CONT_LINK) && (ob->scavisflag & OB_VIS_CONT) == 0) {
          cont = static_cast<bController *>(ob->controllers.first);
          while (cont) {
            for (a = 0; a < cont->totlinks; a++) {
              if (cont->links[a]) {
                obt = (Object *)cont->links[a]->mynew;
                if (obt && (obt->scavisflag & OB_VIS_ACT)) {
                  do_it = true;
                  ob->scavisflag |= OB_VIS_CONT;
                  break;
                }
              }
            }
            if (do_it)
              break;
            cont = cont->next;
          }
        }

        /* 3rd case: select controller when sensor selected */
        if ((scavisflag & BUTS_CONT_LINK) && (ob->scavisflag & OB_VIS_SENS)) {
          sens = static_cast<bSensor *>(ob->sensors.first);
          while (sens) {
            for (a = 0; a < sens->totlinks; a++) {
              if (sens->links[a]) {
                obt = (Object *)sens->links[a]->mynew;
                if (obt && (obt->scavisflag & OB_VIS_CONT) == 0) {
                  do_it = true;
                  obt->scavisflag |= OB_VIS_CONT;
                }
              }
            }
            sens = sens->next;
          }
        }

        /* 4th case: select actuator when controller selected */
        if ((scavisflag & (BUTS_ACT_LINK | BUTS_ACT_STATE)) && (ob->scavisflag & OB_VIS_CONT)) {
          cont = static_cast<bController *>(ob->controllers.first);
          while (cont) {
            for (a = 0; a < cont->totlinks; a++) {
              if (cont->links[a]) {
                obt = (Object *)cont->links[a]->mynew;
                if (obt && (obt->scavisflag & OB_VIS_ACT) == 0) {
                  do_it = true;
                  obt->scavisflag |= OB_VIS_ACT;
                }
              }
            }
            cont = cont->next;
          }
        }
        ob = static_cast<Object *>(ob->id.next);
      }
    }
  }

  /* now we count */
  ob = static_cast<Object *>(bmain->objects.first);
  while (ob) {
    if (ob->scavisflag)
      (*count)++;
    ob = static_cast<Object *>(ob->id.next);
  }

  if (*count == 0)
    return nullptr;
  // if (*count > 24)
  //*count = 24; /* temporal */
  idar = (ID **)MEM_callocN((*count) * sizeof(void *), "idar");

  ob = static_cast<Object *>(bmain->objects.first);
  nr = 0;

  /* make the active object always the first one of the list */
  if (obact) {
    idar[0] = (ID *)obact;
    nr++;
  }

  while (ob) {
    if ((ob->scavisflag) && (ob != obact)) {
      idar[nr] = (ID *)ob;
      nr++;
    }
    // if (nr >= 24)
    // break;
    ob = static_cast<Object *>(ob->id.next);
  }

  /* just to be sure... these were set in set_sca_done_ob() */
  BKE_sca_clear_new_points();

  return idar;
}

static void get_armature_bone_constraint(Object *ob,
                                         const char *posechannel,
                                         const char *constraint_name,
                                         bConstraint **constraint)
{
  /* check that bone exist in the active object */
  if (ob->type == OB_ARMATURE && ob->pose) {
    bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose, posechannel);
    if (pchan) {
      bConstraint *con = (bConstraint *)BLI_findstring(
          &pchan->constraints, constraint_name, offsetof(bConstraint, name));
      if (con) {
        *constraint = con;
      }
    }
  }
  /* didn't find any */
}

static void do_sensor_menu(bContext *C, void */*arg*/, int event)
{
  SpaceLogic *slogic = CTX_wm_space_logic(C);
  ID **idar;
  Object *ob;
  bSensor *sens;
  short count, a;

  idar = get_selected_and_linked_obs(C, &count, slogic->scaflag);

  for (a = 0; a < count; a++) {
    ob = (Object *)idar[a];
    if (event == 0 || event == 2)
      ob->scaflag |= OB_SHOWSENS;
    else if (event == 1)
      ob->scaflag &= ~OB_SHOWSENS;
  }

  for (a = 0; a < count; a++) {
    ob = (Object *)idar[a];
    sens = (bSensor *)ob->sensors.first;
    while (sens) {
      if (event == 2)
        sens->flag |= SENS_SHOW;
      else if (event == 3)
        sens->flag &= ~SENS_SHOW;
      sens = sens->next;
    }
  }

  if (idar)
    MEM_freeN(idar);
}

static Block *sensor_menu(bContext *C, ARegion *region, void */*arg*/)
{
  Block *block;
  int yco = 0;
  Button *but;

  block = block_begin(C, region, __func__, blender::ui::EmbossType::Pulldown);
  /* See
   * https://projects.blender.org/blender/blender/commit/f4e670af2ccec348378356512980554aec39ee3b
   * if issue */
  //UI_block_func_butmenu_set(block, do_sensor_menu, nullptr);

  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Show Objects"),
           0,
           (short)(yco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_sensor_menu(&C, nullptr, 0); });
  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Hide Objects"),
           0,
           (short)(yco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_sensor_menu(&C, nullptr, 1); });
  uiDefBut(
      block, ButtonType::SeprLine, "", 0, (short)(yco -= 6), 160, 6, nullptr, 0.0, 0.0, "");
  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Show Sensors"),
           0,
           (short)(yco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_sensor_menu(&C, nullptr, 2); });
  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Hide Sensors"),
           0,
           (short)(yco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_sensor_menu(&C, nullptr, 3); });

  block_direction_set(block, UI_DIR_UP);
  //UI_block_end(C, block);

  return block;
}

static void do_controller_menu(bContext *C, void */*arg*/, int event)
{
  SpaceLogic *slogic = CTX_wm_space_logic(C);
  ID **idar;
  Object *ob;
  bController *cont;
  short count, a;

  idar = get_selected_and_linked_obs(C, &count, slogic->scaflag);

  for (a = 0; a < count; a++) {
    ob = (Object *)idar[a];
    if (event == 0 || event == 2)
      ob->scaflag |= OB_SHOWCONT;
    else if (event == 1)
      ob->scaflag &= ~OB_SHOWCONT;
  }

  for (a = 0; a < count; a++) {
    ob = (Object *)idar[a];
    cont = static_cast<bController *>(ob->controllers.first);
    while (cont) {
      if (event == 2)
        cont->flag |= CONT_SHOW;
      else if (event == 3)
        cont->flag &= ~CONT_SHOW;
      cont = cont->next;
    }
  }

  if (idar)
    MEM_freeN(idar);
}

static Block *controller_menu(bContext *C, ARegion *region, void */*arg*/)
{
  Block *block;
  Button *but;
  int yco = 0;

  block = block_begin(C, region, __func__, blender::ui::EmbossType::Pulldown);
  /* See
   * https://projects.blender.org/blender/blender/commit/f4e670af2ccec348378356512980554aec39ee3b
   * if issue */
  //UI_block_func_butmenu_set(block, do_controller_menu, nullptr);

  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Show Objects"),
           0,
           (short)(yco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_controller_menu(&C, nullptr, 0);
  });
  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Hide Objects"),
           0,
           (short)(yco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_controller_menu(&C, nullptr, 1); });
  uiDefBut(
      block, ButtonType::SeprLine, "", 0, (short)(yco -= 6), 160, 6, nullptr, 0.0, 0.0, "");
  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Show Controllers"),
           0,
           (short)(yco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_controller_menu(&C, nullptr, 2); });
  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Hide Controllers"),
           0,
           (short)(yco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_controller_menu(&C, nullptr, 3); });

  block_direction_set(block, UI_DIR_UP);
  //UI_block_end(C, block);

  return block;
}

static void do_actuator_menu(bContext *C, void */*arg*/, int event)
{
  SpaceLogic *slogic = CTX_wm_space_logic(C);
  ID **idar;
  Object *ob;
  bActuator *act;
  short count, a;

  idar = get_selected_and_linked_obs(C, &count, slogic->scaflag);

  for (a = 0; a < count; a++) {
    ob = (Object *)idar[a];
    if (event == 0 || event == 2)
      ob->scaflag |= OB_SHOWACT;
    else if (event == 1)
      ob->scaflag &= ~OB_SHOWACT;
  }

  for (a = 0; a < count; a++) {
    ob = (Object *)idar[a];
    act = static_cast<bActuator *>(ob->actuators.first);
    while (act) {
      if (event == 2)
        act->flag |= ACT_SHOW;
      else if (event == 3)
        act->flag &= ~ACT_SHOW;
      act = act->next;
    }
  }

  if (idar)
    MEM_freeN(idar);
}

static Block *actuator_menu(bContext *C, ARegion *region, void */*arg*/)
{
  Block *block;
  int xco = 0;
  Button *but;

  block = block_begin(C, region, __func__, blender::ui::EmbossType::Pulldown);
  /* See
   * https://projects.blender.org/blender/blender/commit/f4e670af2ccec348378356512980554aec39ee3b
   * if issue */
  //UI_block_func_butmenu_set(block, do_actuator_menu, nullptr);

  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Show Objects"),
           0,
           (short)(xco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_actuator_menu(&C, nullptr, 0); });
  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Hide Objects"),
           0,
           (short)(xco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_actuator_menu(&C, nullptr, 1); });
  uiDefBut(
      block, ButtonType::SeprLine, "", 0, (short)(xco -= 6), 160, 6, nullptr, 0.0, 0.0, "");
  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Show Actuators"),
           0,
           (short)(xco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_actuator_menu(&C, nullptr, 2); });
  but = uiDefBut(block,
           ButtonType::ButMenu,
           IFACE_("Hide Actuators"),
           0,
           (short)(xco -= 20),
           160,
           19,
           nullptr,
           0.0,
           0.0,
           "");
  button_retval_set(but, 1);
  button_func_set(but, [](bContext &C) { do_actuator_menu(&C, nullptr, 3); });

  block_direction_set(block, UI_DIR_UP);
  //UI_block_end(C, block);

  return block;
}

static void check_controller_state_mask(bContext */*C*/, void *arg1_but, void *arg2_mask)
{
  unsigned int *cont_mask = (unsigned int *)arg2_mask;
  Button *but = (Button *)arg1_but;

  /* a controller is always in a single state */
  *cont_mask = (1 << but->retval);
  but->retval = B_REDR;
}

static Block *controller_state_mask_menu(bContext *C, ARegion *region, void *arg_cont)
{
  Block *block;
  Button *but;
  bController *cont = (bController *)arg_cont;

  short yco = 12, xco = 0, stbit, offset;

  block = block_begin(C, region, __func__, blender::ui::EmbossType::Emboss);

  /* use this for a fake extra empy space around the buttons */
  uiDefBut(block, ButtonType::Label, "", -5, -5, 200, 34, nullptr, 0, 0, "");

  for (offset = 0; offset < 15; offset += 5) {
    block_align_begin(block);
    for (stbit = 0; stbit < 5; stbit++) {
      but = uiDefButBitI(block,
                         ButtonType::Toggle,
                         (1 << (stbit + offset)),
                         "",
                         (short)(xco + 12 * stbit + 13 * offset),
                         yco,
                         12,
                         12,
                         (int *)&(cont->state_mask),
                         0,
                         0,
                         "");
      button_retval_set(but, (stbit + offset));
      button_func_set(but, check_controller_state_mask, but, &(cont->state_mask));
    }
    for (stbit = 0; stbit < 5; stbit++) {
      but = uiDefButBitI(block,
                         ButtonType::Toggle,
                         (1 << (stbit + offset + 15)),
                         "",
                         (short)(xco + 12 * stbit + 13 * offset),
                         yco - 12,
                         12,
                         12,
                         (int *)&(cont->state_mask),
                         0,
                         0,
                         "");
      button_retval_set(but, (stbit + offset + 15));
      button_func_set(but, check_controller_state_mask, but, &(cont->state_mask));
    }
  }
  block_align_end(block);

  block_direction_set(block, UI_DIR_UP);
  //UI_block_end(C, block);

  return block;
}

static bool is_sensor_linked(Block *block, bSensor *sens)
{
  bController *cont;
  int i;

  for (i = 0; i < sens->totlinks; i++) {
    cont = sens->links[i];
    if (UI_block_links_find_inlink(block, cont) != nullptr)
      return 1;
  }
  return 0;
}

/* Sensors code */

static void draw_sensor_header(blender::ui::Layout *layout, PointerRNA *ptr, PointerRNA *logic_ptr)
{
  blender::ui::Layout *box, *row, *sub;
  bSensor *sens = (bSensor *)ptr->data;

  box = &layout->box();
  row = &box->row(false);

  sub = &row->row(false);
  sub->active_set(RNA_boolean_get(ptr, "active"));
  sub->prop(ptr, "show_expanded", ITEM_R_NO_BG, "", ICON_NONE);
  if (RNA_boolean_get(ptr, "show_expanded")) {
    sub->prop(ptr, "type", UI_ITEM_NONE, "", ICON_NONE);
    sub->prop(ptr, "name", UI_ITEM_NONE, "", ICON_NONE);
  }
  else {
    sub->label(IFACE_(sensor_name(sens->type)), ICON_NONE);
    sub->label(sens->name, ICON_NONE);
  }

  sub = &row->row(false);
  sub->active_set(
                    (((RNA_boolean_get(logic_ptr, "show_sensors_active_states") &&
                       RNA_boolean_get(ptr, "show_expanded")) ||
                      RNA_boolean_get(ptr, "pin")) &&
                     RNA_boolean_get(ptr, "active")));
  sub->prop(ptr, "pin", ITEM_R_NO_BG, "", ICON_NONE);

  sub = &row->row(true);
  sub->active_set(RNA_boolean_get(ptr, "active"));
  PointerRNA op_ptr = sub->op("LOGIC_OT_sensor_move", "", ICON_TRIA_UP);    // up
  RNA_enum_set(&op_ptr, "direction", 1);
  op_ptr = sub->op("LOGIC_OT_sensor_move", "", ICON_TRIA_DOWN);  // down
  RNA_enum_set(&op_ptr, "direction", 2);

  sub = &row->row(false);
  sub->prop(ptr, "active", UI_ITEM_NONE, "", ICON_NONE);

  sub = &row->row(false);
  sub->active_set(RNA_boolean_get(ptr, "active"));
  sub->op("LOGIC_OT_sensor_remove", "", ICON_X);
}

static void draw_sensor_internal_header(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *box, *split, *sub, *row;

  box = &layout->box();
  box->active_set(RNA_boolean_get(ptr, "active"));
  split = &box->split(0.45f, false);

  row = &split->row(true);
  row->prop(ptr, "use_pulse_true_level", UI_ITEM_NONE, "", ICON_TRIA_UP);  // CHOOSE BETTER ICON
  row->prop(ptr, "use_pulse_false_level", UI_ITEM_NONE, "", ICON_TRIA_DOWN);  // CHOOSE BETTER ICON

  sub = &row->row(false);
  sub->active_set(
                    (RNA_boolean_get(ptr, "use_pulse_true_level") ||
                     RNA_boolean_get(ptr, "use_pulse_false_level")));
  sub->prop(ptr, "tick_skip", UI_ITEM_NONE, IFACE_("Skip"), ICON_NONE);

  row = &split->row(true);
  row->prop(ptr, "use_level", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
  row->prop(ptr, "use_tap", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

  split->prop(ptr, "invert", ITEM_R_TOGGLE, IFACE_("Invert"), ICON_NONE);
}
/* sensors in alphabetical order */

static void draw_sensor_actuator(blender::ui::Layout *layout, PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;

  PointerRNA settings_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_GameObjectSettings, ob);
  layout->prop_search(
                 ptr,
                 "actuator",
                 &settings_ptr,
                 "actuators",
                 std::nullopt,
                 ICON_ACTION);  // CHOOSE BETTER ICON
}

static void draw_sensor_armature(blender::ui::Layout *layout, PointerRNA *ptr)
{
  bSensor *sens = (bSensor *)ptr->data;
  bArmatureSensor *as = (bArmatureSensor *)sens->data;
  Object *ob = (Object *)ptr->owner_id;
  blender::ui::Layout *row;

  if (ob->type != OB_ARMATURE) {
    layout->label(IFACE_("Sensor only available for armatures"), ICON_NONE);
    return;
  }

  if (ob->pose) {
    PointerRNA pchan_ptr;
    PropertyRNA *bones_prop;

    PointerRNA pose_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Pose, ob->pose);
    bones_prop = RNA_struct_find_property(&pose_ptr, "bones");

    layout->prop_search(ptr, "bone", &pose_ptr, "bones", std::nullopt, ICON_BONE_DATA);

    if (RNA_property_collection_lookup_string(&pose_ptr, bones_prop, as->posechannel, &pchan_ptr))
      layout->prop_search(
                     ptr,
                     "constraint",
                     &pchan_ptr,
                     "constraints",
                     std::nullopt,
                     ICON_CONSTRAINT_BONE);
  }
  row = &layout->row(true);
  row->prop(ptr, "test_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "test_type") != SENS_ARM_STATE_CHANGED)
    row->prop(ptr, "value", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_sensor_collision(blender::ui::Layout *layout, PointerRNA *ptr, bContext *C)
{
  blender::ui::Layout *row, *split;

  PointerRNA main_ptr = RNA_main_pointer_create(CTX_data_main(C));

  split = &layout->split(0.3f, false);
  row = &split->row(true);
  row->prop(ptr, "use_pulse", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
  row->prop(ptr, "use_material", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

  switch (RNA_boolean_get(ptr, "use_material")) {
    case SENS_COLLISION_PROPERTY:
      split->prop(ptr, "property", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case SENS_COLLISION_MATERIAL:
      split->prop_search(
          ptr, "material", &main_ptr, "materials", std::nullopt, ICON_MATERIAL_DATA);
      break;
  }
}

static void draw_sensor_delay(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row;

  row = &layout->row(false);

  row->prop(ptr, "delay", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "duration", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "use_repeat", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_sensor_joystick(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *col, *row, *split;

  layout->prop(ptr, "joystick_index", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  split = &layout->split(0.75f, false);
  row = &split->row(false);
  row->prop(ptr, "event_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  switch (RNA_enum_get(ptr, "event_type")) {
    case SENS_JOY_BUTTON:
      split->prop(ptr, "use_all_events", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      col = &layout->column(false);
      col->active_set(RNA_boolean_get(ptr, "use_all_events") == false);
      col->prop(ptr, "button_number", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case SENS_JOY_AXIS:
      split->prop(ptr, "use_all_events", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      col = &layout->column(false);
      col->prop(ptr, "axis_number", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      col->active_set(RNA_boolean_get(ptr, "use_all_events") == false);
      col->prop(ptr, "axis_direction", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      col->prop(ptr, "axis_threshold", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case SENS_JOY_AXIS_SINGLE:
      col = &layout->column(false);
      col->prop(ptr, "single_axis_number", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      col->prop(ptr, "axis_threshold", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case SENS_JOY_SHOULDER_TRIGGER:
      col = &layout->column(false);
      col->prop(ptr, "axis_trigger_number", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      col->prop(ptr, "axis_threshold", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
  }
}

static void draw_actuator_vibration(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row;
  row = &layout->row(false);

  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, 0);

  switch (RNA_enum_get(ptr, "mode")) {
    case ACT_VIBRATION_PLAY: {
      row->prop(ptr, "joy_index", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row = &layout->row(false);
      row->prop(ptr, "joy_strength_left", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "joy_strength_right", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row = &layout->row(false);
      row->prop(ptr, "joy_duration", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    }
    case ACT_VIBRATION_STOP: {
      row->prop(ptr, "joy_index", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    }
  }
}

static void draw_sensor_keyboard(blender::ui::Layout *layout, PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  blender::ui::Layout *row, *col;

  row = &layout->row(false);
  row->label(CTX_IFACE_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Key:"), ICON_NONE);
  col = &row->column(false);
  col->active_set(RNA_boolean_get(ptr, "use_all_keys") == false);
  col->prop(ptr, "key", ITEM_R_EVENT, "", ICON_NONE);
  col = &row->column(false);
  col->prop(ptr, "use_all_keys", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

  col = &layout->column(false);
  col->active_set(RNA_boolean_get(ptr, "use_all_keys") == false);
  row = &col->row(false);
  row->label(IFACE_("First Modifier:"), ICON_NONE);
  row->prop(ptr, "modifier_key_1", ITEM_R_EVENT, "", ICON_NONE);

  row = &col->row(false);
  row->label(IFACE_("Second Modifier:"), ICON_NONE);
  row->prop(ptr, "modifier_key_2", ITEM_R_EVENT, "", ICON_NONE);

  PointerRNA settings_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_GameObjectSettings, ob);
  layout->prop_search(ptr, "log", &settings_ptr, "properties", std::nullopt, ICON_NONE);
  layout->prop_search(ptr, "target", &settings_ptr, "properties", std::nullopt, ICON_NONE);
}

static void draw_sensor_message(blender::ui::Layout *layout, PointerRNA *ptr)
{
  layout->prop(ptr, "subject", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_sensor_mouse(blender::ui::Layout *layout, PointerRNA *ptr, bContext *C)
{
  blender::ui::Layout *split, *split2;

  split = &layout->split(0.8f, false);
  split->prop(ptr, "mouse_event", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (RNA_enum_get(ptr, "mouse_event") == BL_SENS_MOUSE_MOUSEOVER_ANY) {
    split->prop(ptr, "use_pulse", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

    split = &layout->split(0.3f, false);
    split->prop(ptr, "use_material", UI_ITEM_NONE, "", ICON_NONE);

    split2 = &split->split(0.7f, false);
    if (RNA_enum_get(ptr, "use_material") == SENS_RAY_PROPERTY) {
      split2->prop(ptr, "property", UI_ITEM_NONE, "", ICON_NONE);
    }
    else {
      PointerRNA main_ptr = RNA_main_pointer_create(CTX_data_main(C));
      split2->prop_search(ptr, "material", &main_ptr, "materials", "", ICON_MATERIAL_DATA);
    }
    split2->prop(ptr, "use_x_ray", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

    split = &layout->split(0.3, false);
    split->prop(ptr, "mask", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void draw_sensor_near(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row;

  layout->prop(ptr, "property", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(true);
  row->prop(ptr, "distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "reset_distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_sensor_property(blender::ui::Layout *layout, PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;

  blender::ui::Layout *row;
  layout->prop(ptr, "evaluation_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  PointerRNA settings_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_GameObjectSettings, ob);
  layout->prop_search(ptr, "property", &settings_ptr, "properties", std::nullopt, ICON_NONE);

  switch (RNA_enum_get(ptr, "evaluation_type")) {
    case SENS_PROP_INTERVAL:
      row = &layout->row(false);
      row->prop(ptr, "value_min", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "value_max", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case SENS_PROP_EQUAL:
    case SENS_PROP_NEQUAL:
    case SENS_PROP_LESSTHAN:
    case SENS_PROP_GREATERTHAN:
      layout->prop(ptr, "value", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case SENS_PROP_CHANGED:
      break;
  }
}

static void draw_sensor_radar(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row;

  layout->prop(ptr, "property", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout->prop(ptr, "axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  row->prop(ptr, "angle", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_sensor_random(blender::ui::Layout *layout, PointerRNA *ptr)
{
  layout->prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_sensor_ray(blender::ui::Layout *layout, PointerRNA *ptr, bContext *C)
{
  blender::ui::Layout *split, *row;

  PointerRNA main_ptr = RNA_main_pointer_create(CTX_data_main(C));
  split = &layout->split(0.3f, false);
  split->prop(ptr, "ray_type", UI_ITEM_NONE, "", ICON_NONE);
  switch (RNA_enum_get(ptr, "ray_type")) {
    case SENS_RAY_PROPERTY:
      split->prop(ptr, "property", UI_ITEM_NONE, "", ICON_NONE);
      break;
    case SENS_RAY_MATERIAL:
      split->prop_search(ptr, "material", &main_ptr, "materials", "", ICON_MATERIAL_DATA);
      break;
  }

  split = &layout->split(0.3, false);
  split->prop(ptr, "axis", UI_ITEM_NONE, "", ICON_NONE);
  row = &split->row(false);
  row->prop(ptr, "range", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "use_x_ray", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
  split = &layout->split(0.3, false);
  split->prop(ptr, "mask", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_sensor_movement(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row;
  layout->prop(ptr, "axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row = &layout->row(false);
  row->prop(ptr, "use_local", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
  row->prop(ptr, "threshold", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_brick_sensor(blender::ui::Layout *layout, PointerRNA *ptr, bContext *C)
{
  blender::ui::Layout *box;

  if (!RNA_boolean_get(ptr, "show_expanded"))
    return;

  draw_sensor_internal_header(layout, ptr);

  box = &layout->box();
  box->active_set(RNA_boolean_get(ptr, "active"));

  switch (RNA_enum_get(ptr, "type")) {

    case SENS_ACTUATOR:
      draw_sensor_actuator(box, ptr);
      break;
    case SENS_ALWAYS:
      break;
    case SENS_ARMATURE:
      draw_sensor_armature(box, ptr);
      break;
    case SENS_COLLISION:
      draw_sensor_collision(box, ptr, C);
      break;
    case SENS_DELAY:
      draw_sensor_delay(box, ptr);
      break;
    case SENS_JOYSTICK:
      draw_sensor_joystick(box, ptr);
      break;
    case SENS_KEYBOARD:
      draw_sensor_keyboard(box, ptr);
      break;
    case SENS_MESSAGE:
      draw_sensor_message(box, ptr);
      break;
    case SENS_MOUSE:
      draw_sensor_mouse(box, ptr, C);
      break;
    case SENS_NEAR:
      draw_sensor_near(box, ptr);
      break;
    case SENS_PROPERTY:
      draw_sensor_property(box, ptr);
      break;
    case SENS_RADAR:
      draw_sensor_radar(box, ptr);
      break;
    case SENS_RANDOM:
      draw_sensor_random(box, ptr);
      break;
    case SENS_MOVEMENT:
      draw_sensor_movement(box, ptr);
      break;
    case SENS_RAY:
      draw_sensor_ray(box, ptr, C);
      break;
  }
}

/* Controller code */
static void draw_controller_header(blender::ui::Layout *layout, PointerRNA *ptr, int xco, int width, int yco)
{
  blender::ui::Layout *box, *row, *sub, *row2, *sub2;
  bController *cont = (bController *)ptr->data;

  char state[9];
  char short_state[7];
  BLI_snprintf(state, sizeof(state), "State %d", RNA_int_get(ptr, "states"));
  BLI_snprintf(short_state, sizeof(short_state), "Sta %d", RNA_int_get(ptr, "states"));

  box = &layout->box();
  row = &box->row(false);

  sub = &row->row(false);
  sub->active_set(RNA_boolean_get(ptr, "active"));
  sub->prop(ptr, "show_expanded", ITEM_R_NO_BG, "", ICON_NONE);
  if (RNA_boolean_get(ptr, "show_expanded")) {
    sub->prop(ptr, "type", UI_ITEM_NONE, "", ICON_NONE);
    sub->prop(ptr, "name", UI_ITEM_NONE, "", ICON_NONE);
    row2 = &box->row(false);
    sub2 = &row2->split(0.4f, true);
    sub2->active_set(RNA_boolean_get(ptr, "active"));
    sub2->label(IFACE_("Controller visible at: "), ICON_NONE);
    uiDefBlockBut(layout->block(),
                  controller_state_mask_menu,
                  cont,
                  state,
                  (short)(xco + width - 44),
                  yco,
                  22 + 22,
                  UI_UNIT_Y,
                  IFACE_("Set controller state index (from 1 to 30)"));
  }
  else {
    sub->label(IFACE_(controller_name(cont->type)), ICON_NONE);
    sub->label(cont->name, ICON_NONE);
    sub->label(short_state, ICON_NONE);
  }

  sub = &row->row(false);
  sub->active_set(RNA_boolean_get(ptr, "active"));
  sub->prop(ptr, "use_priority", UI_ITEM_NONE, "", ICON_NONE);

  sub = &row->row(true);
  sub->active_set(RNA_boolean_get(ptr, "active"));
  PointerRNA op_ptr = sub->op("LOGIC_OT_controller_move", "", ICON_TRIA_UP);    // up
  RNA_enum_set(&op_ptr, "direction", 1);
  op_ptr = sub->op("LOGIC_OT_controller_move", "", ICON_TRIA_DOWN);  // down
  RNA_enum_set(&op_ptr, "direction", 2);

  sub = &row->row(false);
  sub->prop(ptr, "active", UI_ITEM_NONE, "", ICON_NONE);

  sub = &row->row(false);
  sub->active_set(RNA_boolean_get(ptr, "active"));
  sub->op("LOGIC_OT_controller_remove","", ICON_X);
}

static void draw_controller_expression(blender::ui::Layout *layout, PointerRNA *ptr)
{
  layout->prop(ptr, "expression", UI_ITEM_NONE, "", ICON_NONE);
}

static void draw_controller_python(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *split, *sub;

  split = &layout->split(0.3, true);
  split->prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
  if (RNA_enum_get(ptr, "mode") == CONT_PY_SCRIPT) {
    split->prop(ptr, "text", UI_ITEM_NONE, "", ICON_NONE);
  }
  else {
    sub = &split->split(0.8f, false);
    sub->prop(ptr, "module", UI_ITEM_NONE, "", ICON_NONE);
    sub->prop(ptr, "use_debug", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
  }
}

static void draw_controller_state(blender::ui::Layout */*layout*/, PointerRNA */*ptr*/)
{
}

static void draw_brick_controller(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *box;

  if (!RNA_boolean_get(ptr, "show_expanded"))
    return;

  box = &layout->box();
  box->active_set(RNA_boolean_get(ptr, "active"));

  draw_controller_state(box, ptr);

  switch (RNA_enum_get(ptr, "type")) {
    case CONT_LOGIC_AND:
      break;
    case CONT_LOGIC_OR:
      break;
    case CONT_EXPRESSION:
      draw_controller_expression(box, ptr);
      break;
    case CONT_PYTHON:
      draw_controller_python(box, ptr);
      break;
    case CONT_LOGIC_NAND:
      break;
    case CONT_LOGIC_NOR:
      break;
    case CONT_LOGIC_XOR:
      break;
    case CONT_LOGIC_XNOR:
      break;
  }
}

/* Actuator code */
static void draw_actuator_header(blender::ui::Layout *layout, PointerRNA *ptr, PointerRNA *logic_ptr)
{
  blender::ui::Layout *box, *row, *sub;
  bActuator *act = (bActuator *)ptr->data;

  box = &layout->box();
  row = &box->row(false);

  sub = &row->row(false);
  sub->active_set(RNA_boolean_get(ptr, "active"));
  sub->prop(ptr, "show_expanded", ITEM_R_NO_BG, "", ICON_NONE);
  if (RNA_boolean_get(ptr, "show_expanded")) {
    sub->prop(ptr, "type", UI_ITEM_NONE, "", ICON_NONE);
    sub->prop(ptr, "name", UI_ITEM_NONE, "", ICON_NONE);
  }
  else {
    sub->label(IFACE_(actuator_name(act->type)), ICON_NONE);
    sub->label(act->name, ICON_NONE);
  }

  sub = &row->row(false);
  sub->active_set(
                    (((RNA_boolean_get(logic_ptr, "show_actuators_active_states") &&
                       RNA_boolean_get(ptr, "show_expanded")) ||
                      RNA_boolean_get(ptr, "pin")) &&
                     RNA_boolean_get(ptr, "active")));
  sub->prop(ptr, "pin", ITEM_R_NO_BG, "", ICON_NONE);

  sub = &row->row(true);
  sub->active_set(RNA_boolean_get(ptr, "active"));
  PointerRNA op_ptr = sub->op("LOGIC_OT_actuator_move", "", ICON_TRIA_UP);    // up
  RNA_enum_set(&op_ptr, "direction", 1);
  op_ptr = sub->op("LOGIC_OT_actuator_move", "", ICON_TRIA_DOWN);  // down
  RNA_enum_set(&op_ptr, "direction", 2);

  sub = &row->row(false);
  sub->prop(ptr, "active", UI_ITEM_NONE, "", ICON_NONE);

  sub = &row->row(false);
  sub->active_set(RNA_boolean_get(ptr, "active"));
  sub->op("LOGIC_OT_actuator_remove","", ICON_X);
}

static void draw_actuator_action(blender::ui::Layout *layout, PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  blender::ui::Layout *row, *sub;

  PointerRNA settings_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_GameObjectSettings, ob);

  row = &layout->row(false);
  row->prop(ptr, "play_mode", UI_ITEM_NONE, "", ICON_NONE);

  sub = &row->row(true);
  sub->prop(ptr, "use_force", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
  sub->prop(ptr, "use_additive", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

  row = &sub->column(false);
  row->active_set(
                    (RNA_boolean_get(ptr, "use_additive") || RNA_boolean_get(ptr, "use_force")));
  row->prop(ptr, "use_local", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  row->prop(ptr, "action", UI_ITEM_NONE, "", ICON_NONE);
  row->prop(ptr, "use_continue_last_frame", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  if ((RNA_enum_get(ptr, "play_mode") == ACT_ACTION_FROM_PROP))
    row->prop_search(ptr, "property", &settings_ptr, "properties", std::nullopt, ICON_NONE);

  else {
    row->prop(ptr, "frame_start", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    row->prop(ptr, "frame_end", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  row->prop(ptr, "apply_to_children", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  row->prop(ptr, "frame_blend_in", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "priority", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  row->prop(ptr, "layer", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "layer_weight", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "blend_mode", UI_ITEM_NONE, "", ICON_NONE);

  layout->prop_search(ptr, "frame_property", &settings_ptr, "properties", std::nullopt, ICON_NONE);

#ifdef __NLA_ACTION_BY_MOTION_ACTUATOR
  layout->prop("stride_length", UI_ITEM_NONE, std::nullopt, ICON_NONE);
#endif
}

static void draw_actuator_armature(blender::ui::Layout *layout, PointerRNA *ptr)
{
  bActuator *act = (bActuator *)ptr->data;
  bArmatureActuator *aa = (bArmatureActuator *)act->data;
  Object *ob = (Object *)ptr->owner_id;
  bConstraint *constraint = nullptr;
  PointerRNA pose_ptr, pchan_ptr;
  PropertyRNA *bones_prop = nullptr;

  if (ob->type != OB_ARMATURE) {
    layout->label(IFACE_("Actuator only available for armatures"), ICON_NONE);
    return;
  }

  if (ob->pose) {
    pose_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Pose, ob->pose);
    bones_prop = RNA_struct_find_property(&pose_ptr, "bones");
  }

  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  switch (RNA_enum_get(ptr, "mode")) {
    case ACT_ARM_RUN:
      break;
    case ACT_ARM_ENABLE:
    case ACT_ARM_DISABLE:
      if (ob->pose) {
        layout->prop_search(ptr, "bone", &pose_ptr, "bones", std::nullopt, ICON_BONE_DATA);

        if (RNA_property_collection_lookup_string(
                &pose_ptr, bones_prop, aa->posechannel, &pchan_ptr))
          layout->prop_search(
              ptr, "constraint", &pchan_ptr, "constraints", std::nullopt, ICON_CONSTRAINT_BONE);
      }
      break;
    case ACT_ARM_SETTARGET:
      if (ob->pose) {
        layout->prop_search(ptr, "bone", &pose_ptr, "bones", std::nullopt, ICON_BONE_DATA);

        if (RNA_property_collection_lookup_string(
                &pose_ptr, bones_prop, aa->posechannel, &pchan_ptr))
          layout->prop_search(
              ptr, "constraint", &pchan_ptr, "constraints", std::nullopt, ICON_CONSTRAINT_BONE);
      }

      layout->prop(ptr, "target", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      /* show second target only if the constraint supports it */
      get_armature_bone_constraint(ob, aa->posechannel, aa->constraint, &constraint);
      if (constraint && constraint->type == CONSTRAINT_TYPE_KINEMATIC) {
        layout->prop(ptr, "secondary_target", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
      break;
    case ACT_ARM_SETWEIGHT:
      if (ob->pose) {
        layout->prop_search(ptr, "bone", &pose_ptr, "bones", std::nullopt, ICON_BONE_DATA);

        if (RNA_property_collection_lookup_string(
                &pose_ptr, bones_prop, aa->posechannel, &pchan_ptr))
          layout->prop_search(
              ptr, "constraint", &pchan_ptr, "constraints", std::nullopt, ICON_CONSTRAINT_BONE);
      }

      layout->prop(ptr, "weight", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case ACT_ARM_SETINFLUENCE:
      if (ob->pose) {
        layout->prop_search(ptr, "bone", &pose_ptr, "bones", std::nullopt, ICON_BONE_DATA);

        if (RNA_property_collection_lookup_string(
                &pose_ptr, bones_prop, aa->posechannel, &pchan_ptr))
          layout->prop_search(
              ptr, "constraint", &pchan_ptr, "constraints", std::nullopt, ICON_CONSTRAINT_BONE);
      }

      layout->prop(ptr, "influence", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
  }
}

static void draw_actuator_camera(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row;
  layout->prop(ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  row->prop(ptr, "height", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(true);
  row->prop(ptr, "min", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "max", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout->prop(ptr, "damping", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_actuator_constraint(blender::ui::Layout *layout, PointerRNA *ptr, bContext *C)
{
  blender::ui::Layout *row, *col, *sub, *split;

  PointerRNA main_ptr = RNA_main_pointer_create(CTX_data_main(C));

  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  switch (RNA_enum_get(ptr, "mode")) {
    case ACT_CONST_TYPE_LOC:
      layout->prop(ptr, "limit", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      row = &layout->row(true);
      row->prop(ptr, "limit_min", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "limit_max", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      layout->prop(ptr, "damping", ITEM_R_SLIDER, std::nullopt, ICON_NONE);
      break;

    case ACT_CONST_TYPE_DIST:
      split = &layout->split(0.8, false);
      split->prop(ptr, "direction", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row = &split->row(true);
      row->prop(ptr, "use_local", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      row->prop(ptr, "use_normal", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      row = &layout->row(false);
      col = &row->column(true);
      col->label(IFACE_("Range:"), ICON_NONE);
      col->prop(ptr, "range", UI_ITEM_NONE, "", ICON_NONE);

      col = &row->column(true);
      col->prop(ptr, "use_force_distance", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      sub = &col->column(false);
      sub->active_set(RNA_boolean_get(ptr, "use_force_distance") == true);
      sub->prop(ptr, "distance", UI_ITEM_NONE, "", ICON_NONE);

      layout->prop(ptr, "damping", ITEM_R_SLIDER, std::nullopt, ICON_NONE);

      split = &layout->split(0.15f, false);
      split->prop(ptr, "use_material_detect", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      if (RNA_boolean_get(ptr, "use_material_detect"))
        split->prop_search(ptr, "material", &main_ptr, "materials", std::nullopt, ICON_MATERIAL_DATA);
      else
        split->prop(ptr, "property", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      split = &layout->split(0.15, false);
      split->prop(ptr, "use_persistent", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      row = &split->row(true);
      row->prop(ptr, "time", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "damping_rotation", ITEM_R_SLIDER, std::nullopt, ICON_NONE);
      break;

    case ACT_CONST_TYPE_ORI:
      layout->prop(ptr, "direction_axis_pos", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      row = &layout->row(true);
      row->prop(ptr, "damping", ITEM_R_SLIDER, std::nullopt, ICON_NONE);
      row->prop(ptr, "time", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      row = &layout->row(false);
      row->prop(ptr, "rotation_max", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      row = &layout->row(true);
      row->prop(ptr, "angle_min", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "angle_max", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;

    case ACT_CONST_TYPE_FH:
      split = &layout->split(0.75, false);
      row = &split->row(false);
      row->prop(ptr, "fh_damping", ITEM_R_SLIDER, std::nullopt, ICON_NONE);

      row->prop(ptr, "fh_height", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      split->prop(ptr, "use_fh_paralel_axis", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      row = &layout->row(false);
      row->prop(ptr, "direction_axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      split = &row->split(0.9f, false);
      split->prop(ptr, "fh_force", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      split->prop(ptr, "use_fh_normal", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      split = &layout->split(0.15, false);
      split->prop(ptr, "use_material_detect", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      if (RNA_boolean_get(ptr, "use_material_detect"))
        split->prop_search(ptr, "material", &main_ptr, "materials", std::nullopt, ICON_MATERIAL_DATA);
      else
        split->prop(ptr, "property", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      split = &layout->split(0.15, false);
      split->prop(ptr, "use_persistent", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      row = &split->row(false);
      row->prop(ptr, "time", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "damping_rotation", ITEM_R_SLIDER, std::nullopt, ICON_NONE);
      break;
  }
}

static void draw_actuator_edit_object(blender::ui::Layout *layout, PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  blender::ui::Layout *row, *split, *sub;
  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  switch (RNA_enum_get(ptr, "mode")) {
    case ACT_EDOB_ADD_OBJECT:
      row = &layout->row(false);
      row->prop(ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "time", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      split = &layout->split(0.9, false);
      row = &split->row(false);
      row->prop(ptr, "linear_velocity", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      split->prop(ptr, "use_local_linear_velocity", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      split = &layout->split(0.9, false);
      row = &split->row(false);
      row->prop(ptr, "angular_velocity", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      split->prop(ptr, "use_local_angular_velocity", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      row = &layout->row(false);
      row->prop(ptr, "use_object_duplicate", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      break;
    case ACT_EDOB_END_OBJECT:
      break;
    case ACT_EDOB_REPLACE_MESH:
      if (ob->type != OB_MESH) {
        layout->label(IFACE_("Mode only available for mesh objects"), ICON_NONE);
        break;
      }
      split = &layout->split(0.6, false);
      split->prop(ptr, "mesh", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row = &split->row(false);
      row->prop(ptr, "use_replace_display_mesh", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      row->prop(ptr, "use_replace_physics_mesh", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      break;
    case ACT_EDOB_TRACK_TO:
      split = &layout->split(0.5, false);
      split->prop(ptr, "track_object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      sub = &split->split(0.7f, false);
      sub->prop(ptr, "time", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      sub->prop(ptr, "use_3d_tracking", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      row = &layout->row(false);
      row->prop(ptr, "up_axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      row = &layout->row(false);
      row->prop(ptr, "track_axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case ACT_EDOB_DYNAMICS:
      /*if (ob->type != OB_MESH) {
        layout->label(IFACE_("Mode only available for mesh objects"), ICON_NONE);
        break;
      }*/
      layout->prop(ptr, "dynamic_operation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      if (RNA_enum_get(ptr, "dynamic_operation") == ACT_EDOB_SET_MASS)
        layout->prop(ptr, "mass", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      if (RNA_enum_get(ptr, "dynamic_operation") == ACT_EDOB_RESTORE_PHY) {
        layout->prop(ptr, "children_recursive_restore", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
      if (RNA_enum_get(ptr, "dynamic_operation") == ACT_EDOB_SUSPEND_PHY) {
        layout->prop(ptr, "children_recursive_suspend", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        layout->prop(ptr, "free_constraints", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
      break;
  }
}

static void draw_actuator_filter_2d(blender::ui::Layout *layout, PointerRNA *ptr)
{
  // blender::ui::Layout *row, *split;

  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  switch (RNA_enum_get(ptr, "mode")) {
    case ACT_2DFILTER_CUSTOMFILTER:
      layout->prop(ptr, "filter_pass", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout->prop(ptr, "glsl_shader", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    /*case ACT_2DFILTER_MOTIONBLUR:
      split = &layout->split(0.75f, true);
      row = &split->row(false);
      row->active_set(RNA_boolean_get(ptr, "use_motion_blur") == true);
      row->prop(ptr, "motion_blur_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      split->prop(ptr, "use_motion_blur", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      break;*/
    default:  // all other 2D Filters
      layout->prop(ptr, "filter_pass", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
  }
}

static void draw_actuator_game(blender::ui::Layout *layout, PointerRNA *ptr)
{
  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (ELEM(RNA_enum_get(ptr, "mode"), ACT_GAME_LOAD, ACT_GAME_SCREENSHOT))
    layout->prop(ptr, "filename", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_actuator_message(blender::ui::Layout *layout, PointerRNA *ptr, bContext *C)
{
  Object *ob;
  blender::ui::Layout *row;

  PointerRNA main_ptr = RNA_main_pointer_create(CTX_data_main(C));

  ob = (Object *)ptr->owner_id;
  PointerRNA settings_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_GameObjectSettings, ob);

  layout->prop_search(ptr, "to_property", &main_ptr, "objects", std::nullopt, ICON_OBJECT_DATA);
  layout->prop(ptr, "subject", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(true);
  row->prop(ptr, "body_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (RNA_enum_get(ptr, "body_type") == ACT_MESG_MESG)
    row->prop(ptr, "body_message", UI_ITEM_NONE, "", ICON_NONE);
  else  // mode == ACT_MESG_PROP
    row->prop_search(ptr, "body_property", &settings_ptr, "properties", "", ICON_NONE);
}

static void draw_actuator_motion(blender::ui::Layout *layout, PointerRNA *ptr)
{
  Object *ob;
  blender::ui::Layout *split, *row, *col, *sub;
  int physics_type;
  bool angular;

  ob = (Object *)ptr->owner_id;
  PointerRNA settings_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_GameObjectSettings, ob);
  physics_type = RNA_enum_get(&settings_ptr, "physics_type");

  angular = (RNA_enum_get(ptr, "servo_mode") == ACT_SERVO_ANGULAR);

  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  switch (RNA_enum_get(ptr, "mode")) {
    case ACT_OBJECT_NORMAL:
      split = &layout->split(0.9, false);
      row = &split->row(false);
      row->prop(ptr, "offset_location", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      split->prop(ptr, "use_local_location", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      split = &layout->split(0.9, false);
      row = &split->row(false);
      row->prop(ptr, "offset_rotation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      split->prop(ptr, "use_local_rotation", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      if (ELEM(physics_type, OB_BODY_TYPE_DYNAMIC, OB_BODY_TYPE_RIGID, OB_BODY_TYPE_SOFT)) {
        layout->label(IFACE_("Dynamic Object Settings:"), ICON_NONE);
        split = &layout->split(0.9, false);
        row = &split->row(false);
        row->prop(ptr, "force", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        split->prop(ptr, "use_local_force", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

        split = &layout->split(0.9, false);
        row = &split->row(false);
        row->prop(ptr, "torque", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        split->prop(ptr, "use_local_torque", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

        split = &layout->split(0.9, false);
        row = &split->row(false);
        row->prop(ptr, "linear_velocity", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        row = &split->row(true);
        row->prop(ptr, "use_local_linear_velocity", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
        row->prop(ptr, "use_add_linear_velocity", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

        split = &layout->split(0.9, false);
        row = &split->row(false);
        row->prop(ptr, "angular_velocity", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        split->prop(ptr, "use_local_angular_velocity", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

        layout->prop(ptr, "damping", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
      break;
    case ACT_OBJECT_SERVO:
      layout->prop(ptr, "reference_object", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      layout->prop(ptr, "servo_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      split = &layout->split(0.9, false);
      row = &split->row(false);
      if (angular) {
        row->prop(ptr, "angular_velocity", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        split->prop(ptr, "use_local_angular_velocity", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      }
      else {
        row->prop(ptr, "linear_velocity", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        split->prop(ptr, "use_local_linear_velocity", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      }

      row = &layout->row(false);
      col = &row->column(false);
      col->prop(ptr, "use_servo_limit_x", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      sub = &col->column(true);
      sub->active_set(RNA_boolean_get(ptr, "use_servo_limit_x") == true);
      sub->prop(ptr, "force_max_x", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      sub->prop(ptr, "force_min_x", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      col = &row->column(false);
      col->prop(ptr, "use_servo_limit_y", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      sub = &col->column(true);
      sub->active_set(RNA_boolean_get(ptr, "use_servo_limit_y") == true);
      sub->prop(ptr, "force_max_y", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      sub->prop(ptr, "force_min_y", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      col = &row->column(false);
      col->prop(ptr, "use_servo_limit_z", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      sub = &col->column(true);
      sub->active_set(RNA_boolean_get(ptr, "use_servo_limit_z") == true);
      sub->prop(ptr, "force_max_z", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      sub->prop(ptr, "force_min_z", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      // XXXACTUATOR missing labels from original 2.49 ui (e.g. Servo, Min, Max, Fast)
      // Layout designers willing to help on that, please compare with 2.49 ui
      // (since the old code is going to be deleted ... soon)

      col = &layout->column(true);
      col->prop(ptr, "proportional_coefficient", ITEM_R_SLIDER, std::nullopt, ICON_NONE);
      col->prop(ptr, "integral_coefficient", ITEM_R_SLIDER, std::nullopt, ICON_NONE);
      col->prop(ptr, "derivate_coefficient", ITEM_R_SLIDER, std::nullopt, ICON_NONE);
      break;
    case ACT_OBJECT_CHARACTER:
      split = &layout->split(0.9, false);
      row = &split->row(false);
      row->prop(ptr, "offset_location", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row = &split->row(true);
      row->prop(ptr, "use_local_location", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      row->prop(ptr, "use_add_character_location", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      split = &layout->split(0.9, false);
      row = &split->row(false);
      row->prop(ptr, "offset_rotation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      split->prop(ptr, "use_local_rotation", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);

      split = &layout->split(0.9, false);
      row = &split->row(false);
      split = &row->split(0.7, false);
      split->label("", ICON_NONE); /*Just use this for some spacing */
      split->prop(ptr, "use_character_jump", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      break;
  }
}

static void draw_actuator_parent(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row, *sub;

  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (RNA_enum_get(ptr, "mode") == ACT_PARENT_SET) {
    layout->prop(ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    row = &layout->row(false);
    row->prop(ptr, "use_compound", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    sub = &row->row(false);
    sub->active_set(RNA_boolean_get(ptr, "use_compound") == true);
    sub->prop(ptr, "use_ghost", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void draw_actuator_property(blender::ui::Layout *layout, PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  bActuator *act = (bActuator *)ptr->data;
  bPropertyActuator *pa = (bPropertyActuator *)act->data;
  Object *ob_from = pa->ob;

  blender::ui::Layout *row, *sub;

  PointerRNA settings_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_GameObjectSettings, ob);

  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout->prop_search(ptr, "property", &settings_ptr, "properties", std::nullopt, ICON_NONE);

  switch (RNA_enum_get(ptr, "mode")) {
    case ACT_PROP_TOGGLE:
    case ACT_PROP_LEVEL:
      break;
    case ACT_PROP_ADD:
      layout->prop(ptr, "value", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case ACT_PROP_ASSIGN:
      layout->prop(ptr, "value", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case ACT_PROP_COPY:
      row = &layout->row(false);
      row->prop(ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      if (ob_from) {
        PointerRNA obj_settings_ptr = RNA_pointer_create_discrete(
            (ID *)ob_from, &RNA_GameObjectSettings, ob_from);
        row->prop_search(
            ptr, "object_property", &obj_settings_ptr, "properties", std::nullopt, ICON_NONE);
      }
      else {
        sub = &row->row(false);
        sub->active_set(false);
        sub->prop(ptr, "object_property", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
      break;
  }
}

static void draw_actuator_random(blender::ui::Layout *layout, PointerRNA *ptr)
{
  Object *ob;
  blender::ui::Layout *row;

  ob = (Object *)ptr->owner_id;
  PointerRNA settings_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_GameObjectSettings, ob);

  row = &layout->row(false);

  row->prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "distribution", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  row->prop_search(ptr, "property", &settings_ptr, "properties", std::nullopt, ICON_NONE);

  row = &layout->row(false);

  switch (RNA_enum_get(ptr, "distribution")) {
    case ACT_RANDOM_BOOL_CONST:
      row->prop(ptr, "use_always_true", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      break;

    case ACT_RANDOM_BOOL_UNIFORM:
      row->label(IFACE_("Choose between true and false, 50% chance each"), ICON_NONE);
      break;

    case ACT_RANDOM_BOOL_BERNOUILLI:
      row->prop(ptr, "chance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;

    case ACT_RANDOM_INT_CONST:
      row->prop(ptr, "int_value", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;

    case ACT_RANDOM_INT_UNIFORM:
      row->prop(ptr, "int_min", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "int_max", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;

    case ACT_RANDOM_INT_POISSON:
      row->prop(ptr, "int_mean", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;

    case ACT_RANDOM_FLOAT_CONST:
      row->prop(ptr, "float_value", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;

    case ACT_RANDOM_FLOAT_UNIFORM:
      row->prop(ptr, "float_min", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "float_max", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;

    case ACT_RANDOM_FLOAT_NORMAL:
      row->prop(ptr, "float_mean", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "standard_derivation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;

    case ACT_RANDOM_FLOAT_NEGATIVE_EXPONENTIAL:
      row->prop(ptr, "half_life_time", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
  }
}

static void draw_actuator_scene(blender::ui::Layout *layout, PointerRNA *ptr)
{
  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  switch (RNA_enum_get(ptr, "mode")) {
    case ACT_SCENE_CAMERA:
      layout->prop(ptr, "camera", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case ACT_SCENE_RESTART:
      break;
    default:  // ACT_SCENE_SET|ACT_SCENE_ADD_FRONT|ACT_SCENE_ADD_BACK|ACT_SCENE_REMOVE|ACT_SCENE_SUSPEND|ACT_SCENE_RESUME
      layout->prop(ptr, "scene", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
  }
}

static void draw_actuator_collection(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row;
  row = &layout->row(false);
  row->prop(ptr, "collection", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row = &layout->row(false);
  row->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row = &layout->row(true);
  switch (RNA_enum_get(ptr, "mode")) {
    case ACT_COLLECTION_SUSPEND:
      row->prop(ptr, "use_logic", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "use_physics", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "use_render", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case ACT_COLLECTION_RESUME:
      row->prop(ptr, "use_logic", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "use_physics", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row->prop(ptr, "use_render", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case ACT_COLLECTION_ADD_OVERLAY:
      row->prop(ptr, "camera", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case ACT_COLLECTION_REMOVE_OVERLAY:
      break;
    default:
      break;
  }
}

static void draw_actuator_sound(blender::ui::Layout *layout, PointerRNA *ptr, bContext *C)
{
  blender::ui::Layout *row, *col;

  template_id(layout,
               C,
               ptr,
               "sound",
               nullptr,
               "SOUND_OT_open",
               nullptr,
               TEMPLATE_ID_FILTER_ALL,
               false,
               "");
  if (!RNA_pointer_get(ptr, "sound").data) {
    layout->label(IFACE_("Select a sound from the list or load a new one"), ICON_NONE);
    return;
  }
  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  row->prop(ptr, "volume", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "pitch", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  row->prop(ptr, "use_sound_3d", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "use_preload", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  col = &layout->column(false);
  col->active_set(RNA_boolean_get(ptr, "use_sound_3d") == true);

  row = &col->row(false);
  row->prop(ptr, "gain_3d_min", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "gain_3d_max", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &col->row(false);
  row->prop(ptr, "distance_3d_reference", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "distance_3d_max", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &col->row(false);
  row->prop(ptr, "rolloff_factor_3d", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "cone_outer_gain_3d", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &col->row(false);
  row->prop(ptr, "cone_outer_angle_3d", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "cone_inner_angle_3d", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_actuator_state(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *split;
  Object *ob = (Object *)ptr->owner_id;
  PointerRNA settings_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_GameObjectSettings, ob);

  split = &layout->split(0.35, false);
  split->prop(ptr, "operation", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  template_layers(split, ptr, "states", &settings_ptr, "used_states", 0);
}

static void draw_actuator_visibility(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row;
  row = &layout->row(false);

  row->prop(ptr, "use_visible", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  //row->prop(ptr, "use_occlusion", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "apply_to_children", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_actuator_steering(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row;
  blender::ui::Layout *col;

  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout->prop(ptr, "target", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout->prop(ptr, "navmesh", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  row->prop(ptr, "distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "velocity", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row = &layout->row(false);
  row->prop(ptr, "acceleration", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  row->prop(ptr, "turn_speed", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  row = &layout->row(false);
  col = &row->column(false);
  col->prop(ptr, "facing", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col = &row->column(false);
  col->prop(ptr, "facing_axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (!RNA_boolean_get(ptr, "facing")) {
    col->active_set(false);
  }
  col = &row->column(false);
  col->prop(ptr, "normal_up", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (!RNA_pointer_get(ptr, "navmesh").data) {
    col->active_set(false);
  }

  row = &layout->row(false);
  col = &row->column(false);
  col->prop(ptr, "self_terminated", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col = &row->column(false);
  col->prop(ptr, "lock_z_velocity", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "mode") == ACT_STEERING_PATHFOLLOWING) {
    row = &layout->row(false);
    col = &row->column(false);
    col->prop(ptr, "update_period", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col = &row->column(false);
    col->prop(ptr, "show_visualization", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    row = &layout->row(false);
    col = &row->column(false);
    col->prop(ptr, "path_lerp_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void draw_actuator_mouse(blender::ui::Layout *layout, PointerRNA *ptr)
{
  blender::ui::Layout *row, *col, *subcol, *split, *subsplit;

  layout->prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, 0);

  switch (RNA_enum_get(ptr, "mode")) {
    case ACT_MOUSE_VISIBILITY:
      row = &layout->row(0);
      row->prop(ptr, "visible", ITEM_R_TOGGLE, std::nullopt, 0);
      break;

    case ACT_MOUSE_LOOK:
      /* X axis */
      row = &layout->row(0);
      col = &row->column(1);

      col->prop(ptr, "use_axis_x", ITEM_R_TOGGLE, std::nullopt, 0);

      subcol = &col->column(1);
      subcol->active_set(RNA_boolean_get(ptr, "use_axis_x") == 1);
      subcol->prop(ptr, "sensitivity_x", UI_ITEM_NONE, std::nullopt, 0);
      subcol->prop(ptr, "threshold_x", UI_ITEM_NONE, std::nullopt, 0);

      subcol->prop(ptr, "min_x", UI_ITEM_NONE, std::nullopt, 0);
      subcol->prop(ptr, "max_x", UI_ITEM_NONE, std::nullopt, 0);

      subcol->prop(ptr, "object_axis_x", UI_ITEM_NONE, std::nullopt, 0);

      /* Y Axis */
      col = &row->column(1);

      col->prop(ptr, "use_axis_y", ITEM_R_TOGGLE, std::nullopt, 0);

      subcol = &col->column(1);
      subcol->active_set(RNA_boolean_get(ptr, "use_axis_y") == 1);
      subcol->prop(ptr, "sensitivity_y", UI_ITEM_NONE, std::nullopt, 0);
      subcol->prop(ptr, "threshold_y", UI_ITEM_NONE, std::nullopt, 0);

      subcol->prop(ptr, "min_y", UI_ITEM_NONE, std::nullopt, 0);
      subcol->prop(ptr, "max_y", UI_ITEM_NONE, std::nullopt, 0);

      subcol->prop(ptr, "object_axis_y", UI_ITEM_NONE, std::nullopt, 0);

      /* Lower options */
      row = &layout->row(0);
      split = &row->split(0.5, 0);

      subsplit = &split->split(0.5, 1);
      subsplit->active_set(RNA_boolean_get(ptr, "use_axis_x") == 1);
      subsplit->prop(ptr, "local_x", ITEM_R_TOGGLE, std::nullopt, 0);
      subsplit->prop(ptr, "reset_x", ITEM_R_TOGGLE, std::nullopt, 0);

      subsplit = &split->split(0.5, 1);
      subsplit->active_set(RNA_boolean_get(ptr, "use_axis_y") == 1);
      subsplit->prop(ptr, "local_y", ITEM_R_TOGGLE, std::nullopt, 0);
      subsplit->prop(ptr, "reset_y", ITEM_R_TOGGLE, std::nullopt, 0);

      break;
  }
}

static void draw_brick_actuator(blender::ui::Layout *layout, PointerRNA *ptr, bContext *C)
{
  blender::ui::Layout *box;

  if (!RNA_boolean_get(ptr, "show_expanded"))
    return;

  box = &layout->box();
  box->active_set(RNA_boolean_get(ptr, "active"));

  switch (RNA_enum_get(ptr, "type")) {
    case ACT_ACTION:
      draw_actuator_action(box, ptr);
      break;
    case ACT_ARMATURE:
      draw_actuator_armature(box, ptr);
      break;
    case ACT_CAMERA:
      draw_actuator_camera(box, ptr);
      break;
    case ACT_CONSTRAINT:
      draw_actuator_constraint(box, ptr, C);
      break;
    case ACT_EDIT_OBJECT:
      draw_actuator_edit_object(box, ptr);
      break;
    case ACT_2DFILTER:
      draw_actuator_filter_2d(box, ptr);
      break;
    case ACT_GAME:
      draw_actuator_game(box, ptr);
      break;
    case ACT_MESSAGE:
      draw_actuator_message(box, ptr, C);
      break;
    case ACT_OBJECT:
      draw_actuator_motion(box, ptr);
      break;
    case ACT_PARENT:
      draw_actuator_parent(box, ptr);
      break;
    case ACT_PROPERTY:
      draw_actuator_property(box, ptr);
      break;
    case ACT_RANDOM:
      draw_actuator_random(box, ptr);
      break;
    case ACT_SCENE:
      draw_actuator_scene(box, ptr);
      break;
    case ACT_COLLECTION:
      draw_actuator_collection(box, ptr);
      break;
    case ACT_SOUND:
      draw_actuator_sound(box, ptr, C);
      break;
    case ACT_STATE:
      draw_actuator_state(box, ptr);
      break;
    case ACT_VIBRATION:
      draw_actuator_vibration(box, ptr);
      break;
    case ACT_VISIBILITY:
      draw_actuator_visibility(box, ptr);
      break;
    case ACT_STEERING:
      draw_actuator_steering(box, ptr);
      break;
    case ACT_MOUSE:
      draw_actuator_mouse(box, ptr);
      break;
  }
}

void logic_buttons(bContext *C, ARegion *region)
{
  SpaceLogic *slogic = CTX_wm_space_logic(C);
  Object *ob = CTX_data_active_object(C);
  ID **idar;
  blender::ui::Layout *layout, *row, *box;
  Block *block;
  Button *but;
  char uiblockstr[32];
  short a, count;
  int xco, yco, width, height;

  if (ob == nullptr)
    return;

  PointerRNA logic_ptr = RNA_pointer_create_discrete(
      reinterpret_cast<ID *>(CTX_wm_screen(C)), &RNA_SpaceLogicEditor, slogic);
  idar = get_selected_and_linked_obs(C, &count, slogic->scaflag);

  BLI_snprintf(uiblockstr, sizeof(uiblockstr), "buttonswin %p", (void *)region);
  block = block_begin(C, region, uiblockstr, blender::ui::EmbossType::Emboss);
  block_func_handle_set(block, do_logic_buts, nullptr);
  block_bounds_set_normal(block, U.widget_unit / 2);

  /* loop over all objects and set visible/linked flags for the logic bricks */
  for (a = 0; a < count; a++) {
    bActuator *act;
    bSensor *sens;
    bController *cont;
    int iact;
    short flag;

    ob = (Object *)idar[a];

    /* clean ACT_LINKED and ACT_VISIBLE of all potentially visible actuators so that we can
     * determine which is actually linked/visible */
    act = (bActuator *)ob->actuators.first;
    while (act) {
      act->flag &= ~(ACT_LINKED | ACT_VISIBLE);
      act = act->next;
    }
    /* same for sensors */
    sens = (bSensor *)ob->sensors.first;
    while (sens) {
      sens->flag &= ~(SENS_VISIBLE);
      sens = sens->next;
    }

    /* mark the linked and visible actuators */
    cont = (bController *)ob->controllers.first;
    while (cont) {
      flag = ACT_LINKED;

      /* this controller is visible, mark all its actuator */
      if ((ob->scaflag & OB_ALLSTATE) || (ob->state & cont->state_mask))
        flag |= ACT_VISIBLE;

      for (iact = 0; iact < cont->totlinks; iact++) {
        act = cont->links[iact];
        if (act)
          act->flag |= flag;
      }
      cont = cont->next;
    }
  }

  /* ****************** Controllers ****************** */

  xco = 20 * U.widget_unit;
  yco = -U.widget_unit / 2;
  width = 17 * U.widget_unit;
  layout = &blender::ui::block_layout(block,
                                      blender::ui::LayoutDirection::Vertical,
                                      blender::ui::LayoutType::Panel,
                                      xco,
                                      yco,
                                      width,
                                      20,
                                      0,
                                      style_get());
  row = &layout->row(true);

  uiDefBlockBut(block,
                controller_menu,
                nullptr,
                IFACE_("Controllers"),
                xco - U.widget_unit / 2,
                yco,
                width,
                UI_UNIT_Y,
                ""); /* replace this with blender::ui::Layout stuff later */

  row->prop(&logic_ptr, "show_controllers_selected_objects", UI_ITEM_NONE, IFACE_("Sel"), ICON_NONE);
  row->prop(&logic_ptr, "show_controllers_active_object", UI_ITEM_NONE, IFACE_("Act"), ICON_NONE);
  row->prop(&logic_ptr, "show_controllers_linked_controller", UI_ITEM_NONE, IFACE_("Link"), ICON_NONE);

  for (a = 0; a < count; a++) {
    bController *cont;
    blender::ui::Layout *split, *subsplit, *col;

    ob = (Object *)idar[a];

    /* only draw the controller common header if "use_visible" */
    if ((ob->scavisflag & OB_VIS_CONT) == 0) {
      continue;
    }

    /* Drawing the Controller Header common to all Selected Objects */

    PointerRNA settings_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_GameObjectSettings, ob);

    split = &layout->split(0.05f, false);
    split->prop(&settings_ptr, "show_state_panel", ITEM_R_NO_BG, "", ICON_DISCLOSURE_TRI_RIGHT);

    row = &split->row(true);
    but = uiDefButBitS(block,
                 ButtonType::Toggle,
                 OB_SHOWCONT,
                 ob->id.name + 2,
                 (short)(xco - U.widget_unit / 2),
                 yco,
                 (short)(width - 1.5f * U.widget_unit),
                 UI_UNIT_Y,
                 &ob->scaflag,
                 0,
                 31,
                 TIP_("Object name, click to show/hide controllers"));
    button_retval_set(but, B_REDR);

    PointerRNA object_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Object, ob);
    row->context_ptr_set("object", &object_ptr);
    row->op_menu_enum(C, "LOGIC_OT_controller_add", "type", IFACE_("Add Controller"), ICON_NONE);

    if (RNA_boolean_get(&settings_ptr, "show_state_panel")) {

      box = &layout->box();
      split = &box->split(0.2f, false);

      col = &split->column(false);
      col->label(IFACE_("Visible"), ICON_NONE);
      col->label(IFACE_("Initial"), ICON_NONE);

      subsplit = &split->split(0.85f, false);
      col = &subsplit->column(false);
      row = &col->row(false);
      row->active_set(RNA_boolean_get(&settings_ptr, "use_all_states") == false);
      uiTemplateGameStates(row, &settings_ptr, "states_visible", &settings_ptr, "used_states", 0);
      row = &col->row(false);
      uiTemplateGameStates(row, &settings_ptr, "states_initial", &settings_ptr, "used_states", 0);

      col = &subsplit->column(false);
      col->prop(&settings_ptr, "use_all_states", ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      col->prop(&settings_ptr, "show_debug_state", UI_ITEM_NONE, "", ICON_NONE);
    }

    /* End of Drawing the Controller Header common to all Selected Objects */

    if ((ob->scaflag & OB_SHOWCONT) == 0)
      continue;

    layout->separator();

    for (cont = (bController *)ob->controllers.first; cont; cont = cont->next) {
      PointerRNA ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Controller, cont);

      if (!(ob->scaflag & OB_ALLSTATE) && !(ob->state & cont->state_mask))
        continue;

      /* use two nested splits to align inlinks/links properly */
      split = &layout->split(0.05f, false);

      /* put inlink button to the left */
      col = &split->column(false);
      col->active_set(RNA_boolean_get(&ptr, "active"));
      col->alignment_set(blender::ui::LayoutAlign::Left);
      but = uiDefIconBut(block,
                         ButtonType::Inlink,
                         ICON_LINKED,
                         0,
                         0,
                         UI_UNIT_X,
                         UI_UNIT_Y,
                         cont,
                         LINK_CONTROLLER,
                         0,
                         "");  // CHOOSE BETTER ICON
      if (!RNA_boolean_get(&ptr, "active")) {
        but->upbgeflag |= UI_BUT_SCA_LINK_GREY;
      }

      // col = &split->column(true);
      /* nested split for middle and right columns */
      subsplit = &split->split(0.95f, false);

      col = &subsplit->column(true);
      col->context_ptr_set("controller", &ptr);

      /* should make UI template for controller header.. function will do for now */
      draw_controller_header(col, &ptr, xco, width, yco);

      /* draw the brick contents */
      draw_brick_controller(col, &ptr);

      /* put link button to the right */
      col = &subsplit->column(false);
      col->active_set(RNA_boolean_get(&ptr, "active"));
      col->alignment_set(blender::ui::LayoutAlign::Left);
      but = uiDefIconBut(block,
                         ButtonType::Link,
                         ICON_LINKED,
                         0,
                         0,
                         UI_UNIT_X,
                         UI_UNIT_Y,
                         nullptr,
                         0,
                         0,
                         "");  // CHOOSE BETTER ICON
      if (!RNA_boolean_get(&ptr, "active")) {
        but->upbgeflag |= UI_BUT_SCA_LINK_GREY;
      }

      UI_but_link_set(
          but, nullptr, (void ***)&(cont->links), &cont->totlinks, LINK_CONTROLLER, LINK_ACTUATOR);
    }
  }
  yco = blender::ui::block_layout_resolve(block).y; /* stores final height in yco */
  height = yco;

  /* ****************** Sensors ****************** */

  xco = U.widget_unit / 2;
  yco = -U.widget_unit / 2;
  width = 17 * U.widget_unit;
  layout = &blender::ui::block_layout(block,
                                      blender::ui::LayoutDirection::Vertical,
                                      blender::ui::LayoutType::Panel,
                                      xco,
                                      yco,
                                      width,
                                      20,
                                      0,
                                      style_get());
  row = &layout->row(true);

  uiDefBlockBut(block,
                sensor_menu,
                nullptr,
                IFACE_("Sensors"),
                xco - U.widget_unit / 2,
                yco,
                15 * U.widget_unit,
                UI_UNIT_Y,
                ""); /* replace this with blender::ui::Layout stuff later */

  row->prop(&logic_ptr, "show_sensors_selected_objects", UI_ITEM_NONE, IFACE_("Sel"), ICON_NONE);
  row->prop(&logic_ptr, "show_sensors_active_object", UI_ITEM_NONE, IFACE_("Act"), ICON_NONE);
  row->prop(&logic_ptr, "show_sensors_linked_controller", UI_ITEM_NONE, IFACE_("Link"), ICON_NONE);
  row->prop(&logic_ptr, "show_sensors_active_states", UI_ITEM_NONE, IFACE_("State"), ICON_NONE);

  for (a = 0; a < count; a++) {
    bSensor *sens;

    ob = (Object *)idar[a];

    /* only draw the sensor common header if "use_visible" */
    if ((ob->scavisflag & OB_VIS_SENS) == 0)
      continue;

    row = &layout->row(true);
    but = uiDefButBitS(block,
                 ButtonType::Toggle,
                 OB_SHOWSENS,
                 ob->id.name + 2,
                 (short)(xco - U.widget_unit / 2),
                 yco,
                 (short)(width - 1.5f * U.widget_unit),
                 UI_UNIT_Y,
                 &ob->scaflag,
                 0,
                 31,
                 TIP_("Object name, click to show/hide sensors"));
    button_retval_set(but, B_REDR);

    PointerRNA object_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Object, ob);
    row->context_ptr_set("object", &object_ptr);
    row->op_menu_enum(C, "LOGIC_OT_sensor_add", "type", IFACE_("Add Sensor"), ICON_NONE);

    if ((ob->scaflag & OB_SHOWSENS) == 0)
      continue;

    layout->separator();

    for (sens = (bSensor *)ob->sensors.first; sens; sens = sens->next) {
      PointerRNA ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Sensor, sens);

      if ((ob->scaflag & OB_ALLSTATE) || !(slogic->scaflag & BUTS_SENS_STATE) ||
          (sens->totlinks ==
           0) || /* always display sensor without links so that is can be edited */
          (sens->flag & SENS_PIN &&
           slogic->scaflag & BUTS_SENS_STATE) || /* states can hide some sensors, pinned sensors
                                                    ignore the visible state */
          (is_sensor_linked(block, sens))) {  // gotta check if the current state is visible or not
        blender::ui::Layout *split, *col;

        /* make as visible, for move operator */
        sens->flag |= SENS_VISIBLE;

        split = &layout->split(0.95f, false);
        col = &split->column(true);
        col->context_ptr_set("sensor", &ptr);

        /* should make UI template for sensor header.. function will do for now */
        draw_sensor_header(col, &ptr, &logic_ptr);

        /* draw the brick contents */
        draw_brick_sensor(col, &ptr, C);

        /* put link button to the right */
        col = &split->column(false);
        col->active_set(RNA_boolean_get(&ptr, "active"));
        but = uiDefIconBut(block,
                           ButtonType::Link,
                           ICON_LINKED,
                           0,
                           0,
                           UI_UNIT_X,
                           UI_UNIT_Y,
                           nullptr,
                           0,
                           0,
                           "");  // CHOOSE BETTER ICON
        if (!RNA_boolean_get(&ptr, "active")) {
          but->upbgeflag |= UI_BUT_SCA_LINK_GREY;
        }

        /* use old-school uiButtons for links for now */
        UI_but_link_set(
            but, nullptr, (void ***)&sens->links, &sens->totlinks, LINK_SENSOR, LINK_CONTROLLER);
      }
    }
  }
  yco = blender::ui::block_layout_resolve(block).y; /* stores final height in yco */
  height = std::min(height, yco);

  /* ****************** Actuators ****************** */

  xco = 40 * U.widget_unit;
  yco = -U.widget_unit / 2;
  width = 17 * U.widget_unit;
  layout = &blender::ui::block_layout(block,
                                      blender::ui::LayoutDirection::Vertical,
                                      blender::ui::LayoutType::Panel,
                                      xco,
                                      yco,
                                      width,
                                      20,
                                      0,
                                      style_get());
  row = &layout->row(true);

  uiDefBlockBut(block,
                actuator_menu,
                nullptr,
                IFACE_("Actuators"),
                xco - U.widget_unit / 2,
                yco,
                15 * U.widget_unit,
                UI_UNIT_Y,
                ""); /* replace this with blender::ui::Layout stuff later */

  row->prop(&logic_ptr, "show_actuators_selected_objects", UI_ITEM_NONE, IFACE_("Sel"), ICON_NONE);
  row->prop(&logic_ptr, "show_actuators_active_object", UI_ITEM_NONE, IFACE_("Act"), ICON_NONE);
  row->prop(&logic_ptr, "show_actuators_linked_controller", UI_ITEM_NONE, IFACE_("Link"), ICON_NONE);
  row->prop(&logic_ptr, "show_actuators_active_states", UI_ITEM_NONE, IFACE_("State"), ICON_NONE);

  for (a = 0; a < count; a++) {
    bActuator *act;

    ob = (Object *)idar[a];

    /* only draw the actuator common header if "use_visible" */
    if ((ob->scavisflag & OB_VIS_ACT) == 0) {
      continue;
    }

    row = &layout->row(true);
    but = uiDefButBitS(block,
                 ButtonType::Toggle,
                 OB_SHOWACT,
                 ob->id.name + 2,
                 (short)(xco - U.widget_unit / 2),
                 yco,
                 (short)(width - 1.5f * U.widget_unit),
                 UI_UNIT_Y,
                 &ob->scaflag,
                 0,
                 31,
                 TIP_("Object name, click to show/hide actuators"));
    button_retval_set(but, B_REDR);
    PointerRNA object_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Object, ob);
    row->context_ptr_set("object", &object_ptr);
    row->op_menu_enum(C, "LOGIC_OT_actuator_add", "type", IFACE_("Add Actuator"), ICON_NONE);

    if ((ob->scaflag & OB_SHOWACT) == 0)
      continue;

    layout->separator();

    for (act = (bActuator *)ob->actuators.first; act; act = act->next) {

      PointerRNA ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Actuator, act);

      if ((ob->scaflag & OB_ALLSTATE) || !(slogic->scaflag & BUTS_ACT_STATE) ||
          !(act->flag &
            ACT_LINKED) || /* always display actuators without links so that is can be edited */
          (act->flag & ACT_VISIBLE) || /* this actuator has visible connection, display it */
          (act->flag & ACT_PIN &&
           slogic->scaflag & BUTS_ACT_STATE) /* states can hide some sensors, pinned sensors ignore
                                                the visible state */
      ) {                                    // gotta check if the current state is visible or not
        blender::ui::Layout *split, *col;

        /* make as visible, for move operator */
        act->flag |= ACT_VISIBLE;

        split = &layout->split(0.05f, false);

        /* put inlink button to the left */
        col = &split->column(false);
        col->active_set(RNA_boolean_get(&ptr, "active"));
        but = uiDefIconBut(block,
                           ButtonType::Inlink,
                           ICON_LINKED,
                           0,
                           0,
                           UI_UNIT_X,
                           UI_UNIT_Y,
                           act,
                           LINK_ACTUATOR,
                           0,
                           "");  // CHOOSE BETTER ICON
        if (!RNA_boolean_get(&ptr, "active")) {
          but->upbgeflag |= UI_BUT_SCA_LINK_GREY;
        }

        col = &split->column(true);
        col->context_ptr_set("actuator", &ptr);

        /* should make UI template for actuator header.. function will do for now */
        draw_actuator_header(col, &ptr, &logic_ptr);

        /* draw the brick contents */
        draw_brick_actuator(col, &ptr, C);
      }
    }
  }
  yco = blender::ui::block_layout_resolve(block).y; /* stores final height in yco */
  height = std::min(height, yco);

  view2d_totRect_set(&region->v2d, 57.5f * U.widget_unit, height - U.widget_unit);

  /* set the view */
  view2d_view_ortho(&region->v2d);

  UI_block_links_compose(block);

  block_end(C, block);
  block_draw(C, block);

  /* restore view matrix */
  view2d_view_restore(C);

  if (idar)
    MEM_freeN(idar);
}

} // namespace blender
