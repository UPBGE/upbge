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

/** \file blender/editors/space_logic/logic_ops.c
 *  \ingroup splogic
 */

#include <cstddef>

#include "DNA_actuator_types.h"
#include "DNA_controller_types.h"
#include "DNA_object_types.h"
#include "DNA_python_proxy_types.h"
#include "DNA_scene_types.h"
#include "DNA_sensor_types.h"

#include "BLI_string.h"
#include "BLI_string_utils.hh"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_python_proxy.hh"
#include "BKE_sca.hh"

#include "ED_logic.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "logic_intern.hh"

/* ************* Generic Operator Helpers ************* */
static bool edit_sensor_poll(bContext *C)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "sensor", &RNA_Sensor);

  if (ptr.data && ID_IS_LINKED(ptr.owner_id))
    return 0;
  return true;
}

static bool edit_controller_poll(bContext *C)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "controller", &RNA_Controller);

  if (ptr.data && ID_IS_LINKED(ptr.owner_id))
    return 0;
  return true;
}

static bool edit_actuator_poll(bContext *C)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "actuator", &RNA_Actuator);

  if (ptr.data && ID_IS_LINKED(ptr.owner_id))
    return 0;
  return true;
}

static void edit_sensor_properties(wmOperatorType *ot)
{
  RNA_def_string(ot->srna, "sensor", nullptr, MAX_NAME, "Sensor", "Name of the sensor to edit");
  RNA_def_string(
      ot->srna, "object", nullptr, MAX_NAME, "Object", "Name of the object the sensor belongs to");
}

static int edit_sensor_invoke_properties(bContext *C, wmOperator *op)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "sensor", &RNA_Sensor);

  if (RNA_struct_property_is_set(op->ptr, "sensor") &&
      RNA_struct_property_is_set(op->ptr, "object"))
    return 1;

  if (ptr.data) {
    bSensor *sens = (bSensor *)ptr.data;
    Object *ob = (Object *)ptr.owner_id;

    RNA_string_set(op->ptr, "sensor", sens->name);
    RNA_string_set(op->ptr, "object", ob->id.name + 2);
    return 1;
  }

  return 0;
}

static Object *edit_object_property_get(bContext *C, wmOperator *op)
{
  char ob_name[MAX_NAME];
  Object *ob;

  RNA_string_get(op->ptr, "object", ob_name);

  /* if ob_name is valid try to find the object with this name
   * otherwise gets the active object */
  if (*ob_name)
    ob = (Object *)BLI_findstring(&(CTX_data_main(C)->objects), ob_name, offsetof(ID, name) + 2);
  else
    ob = blender::ed::object::context_active_object(C);

  return ob;
}

static bSensor *edit_sensor_property_get(bContext *C, wmOperator *op, Object **ob)
{
  char sensor_name[MAX_NAME];
  bSensor *sens;

  RNA_string_get(op->ptr, "sensor", sensor_name);

  *ob = edit_object_property_get(C, op);
  if (!*ob)
    return nullptr;

  sens = (bSensor *)BLI_findstring(&((*ob)->sensors), sensor_name, offsetof(bSensor, name));
  return sens;
}

static void edit_controller_properties(wmOperatorType *ot)
{
  RNA_def_string(
      ot->srna, "controller", nullptr, MAX_NAME, "Controller", "Name of the controller to edit");
  RNA_def_string(ot->srna,
                 "object",
                 nullptr,
                 MAX_NAME,
                 "Object",
                 "Name of the object the controller belongs to");
}

static int edit_controller_invoke_properties(bContext *C, wmOperator *op)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "controller", &RNA_Controller);

  if (RNA_struct_property_is_set(op->ptr, "controller") &&
      RNA_struct_property_is_set(op->ptr, "object"))
    return 1;

  if (ptr.data) {
    bController *cont = (bController *)ptr.data;
    Object *ob = (Object *)ptr.owner_id;

    RNA_string_set(op->ptr, "controller", cont->name);
    RNA_string_set(op->ptr, "object", ob->id.name + 2);
    return 1;
  }

  return 0;
}

static bController *edit_controller_property_get(bContext *C, wmOperator *op, Object **ob)
{
  char controller_name[MAX_NAME];
  bController *cont;

  RNA_string_get(op->ptr, "controller", controller_name);

  *ob = edit_object_property_get(C, op);
  if (!*ob)
    return nullptr;

  cont = (bController *)BLI_findstring(&((*ob)->controllers), controller_name, offsetof(bController, name));
  return cont;
}

static void edit_actuator_properties(wmOperatorType *ot)
{
  RNA_def_string(ot->srna, "actuator", nullptr, MAX_NAME, "Actuator", "Name of the actuator to edit");
  RNA_def_string(
      ot->srna, "object", nullptr, MAX_NAME, "Object", "Name of the object the actuator belongs to");
}

static int edit_actuator_invoke_properties(bContext *C, wmOperator *op)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "actuator", &RNA_Actuator);

  if (RNA_struct_property_is_set(op->ptr, "actuator") &&
      RNA_struct_property_is_set(op->ptr, "object"))
    return 1;

  if (ptr.data) {
    bActuator *act = (bActuator *)ptr.data;
    Object *ob = (Object *)ptr.owner_id;

    RNA_string_set(op->ptr, "actuator", act->name);
    RNA_string_set(op->ptr, "object", ob->id.name + 2);
    return 1;
  }

  return 0;
}

static bActuator *edit_actuator_property_get(bContext *C, wmOperator *op, Object **ob)
{
  char actuator_name[MAX_NAME];
  bActuator *act;

  RNA_string_get(op->ptr, "actuator", actuator_name);

  *ob = edit_object_property_get(C, op);
  if (!*ob)
    return nullptr;

  act = (bActuator *)BLI_findstring(&((*ob)->actuators), actuator_name, offsetof(bActuator, name));
  return act;
}

static int logicbricks_move_property_get(wmOperator *op)
{
  int type = RNA_enum_get(op->ptr, "direction");

  if (type == 1)
    return true;
  else
    return false;
}

static bool remove_component_poll(bContext *C)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "component", &RNA_PythonProxy);
  Object *ob = (ptr.owner_id) ? (Object *)ptr.owner_id :
                                blender::ed::object::context_active_object(C);

  if (!ob || ID_IS_LINKED(ob)) {
    return false;
  }

  if (ID_IS_OVERRIDE_LIBRARY(ob)) {
    CTX_wm_operator_poll_msg_set(
        C, "Cannot remove components coming from linked data in a library override");
    return false;
  }

  return true;
}

/* ************* Add/Remove Sensor Operator ************* */

static int sensor_remove_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  bSensor *sens = edit_sensor_property_get(C, op, &ob);

  if (!sens)
    return OPERATOR_CANCELLED;

  BLI_remlink(&(ob->sensors), sens);
  BKE_sca_free_sensor(sens);

  ED_undo_push_old(C, "sensor_remove_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static int sensor_remove_invoke(bContext *C, wmOperator *op, const wmEvent */*event*/)
{
  if (edit_sensor_invoke_properties(C, op)) {
    return sensor_remove_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

static void LOGIC_OT_sensor_remove(wmOperatorType *ot)
{
  ot->name = "Remove Sensor";
  ot->description = "Remove a sensor from the active object";
  ot->idname = "LOGIC_OT_sensor_remove";

  ot->invoke = sensor_remove_invoke;
  ot->exec = sensor_remove_exec;
  ot->poll = edit_sensor_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
  edit_sensor_properties(ot);
}

static int sensor_add_exec(bContext *C, wmOperator *op)
{
  Object *ob;
  bSensor *sens;
  PropertyRNA *prop;
  const char *sens_name;
  char name[MAX_NAME];
  int type = RNA_enum_get(op->ptr, "type");

  ob = edit_object_property_get(C, op);
  if (!ob)
    return OPERATOR_CANCELLED;

  sens = BKE_sca_new_sensor(type);
  BLI_addtail(&(ob->sensors), sens);

  /* set the sensor name based on rna type enum */
  PointerRNA sens_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Sensor, sens);
  prop = RNA_struct_find_property(&sens_ptr, "type");

  RNA_string_get(op->ptr, "name", name);
  if (*name) {
    BLI_strncpy(sens->name, name, sizeof(sens->name));
  }
  else {
    RNA_property_enum_name(C, &sens_ptr, prop, RNA_property_enum_get(&sens_ptr, prop), &sens_name);
    BLI_strncpy(sens->name, sens_name, sizeof(sens->name));
  }

  BLI_uniquename(
      &ob->sensors, sens, DATA_("Sensor"), '.', offsetof(bSensor, name), sizeof(sens->name));
  ob->scaflag |= OB_SHOWSENS;

  ED_undo_push_old(C, "sensor_add_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_sensor_add(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Add Sensor";
  ot->description = "Add a sensor to the active object";
  ot->idname = "LOGIC_OT_sensor_add";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = sensor_add_exec;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  /* properties */
  ot->prop = prop = RNA_def_enum(
      ot->srna, "type", rna_enum_dummy_NULL_items, SENS_ALWAYS, "Type", "Type of sensor to add");
  RNA_def_enum_funcs(prop, rna_Sensor_type_itemf);
  prop = RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the Sensor to add");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_string(
      ot->srna, "object", nullptr, MAX_NAME, "Object", "Name of the Object to add the Sensor to");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* ************* Add/Remove Controller Operator ************* */

static int controller_remove_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  bController *cont = edit_controller_property_get(C, op, &ob);

  if (!cont)
    return OPERATOR_CANCELLED;

  BLI_remlink(&(ob->controllers), cont);
  BKE_sca_unlink_controller(cont);
  BKE_sca_free_controller(cont);

  ED_undo_push_old(C, "controller_remove_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static int controller_remove_invoke(bContext *C, wmOperator *op, const wmEvent */*event*/)
{
  if (edit_controller_invoke_properties(C, op)) {
    return controller_remove_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

static void LOGIC_OT_controller_remove(wmOperatorType *ot)
{
  ot->name = "Remove Controller";
  ot->description = "Remove a controller from the active object";
  ot->idname = "LOGIC_OT_controller_remove";

  ot->invoke = controller_remove_invoke;
  ot->exec = controller_remove_exec;
  ot->poll = edit_controller_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
  edit_controller_properties(ot);
}

static int controller_add_exec(bContext *C, wmOperator *op)
{
  Object *ob;
  bController *cont;
  PropertyRNA *prop;
  const char *cont_name;
  int bit;
  char name[MAX_NAME];
  int type = RNA_enum_get(op->ptr, "type");

  ob = edit_object_property_get(C, op);
  if (!ob)
    return OPERATOR_CANCELLED;

  cont = BKE_sca_new_controller(type);
  BLI_addtail(&(ob->controllers), cont);

  /* set the controller name based on rna type enum */
  PointerRNA cont_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Controller, cont);
  prop = RNA_struct_find_property(&cont_ptr, "type");

  RNA_string_get(op->ptr, "name", name);
  if (*name) {
    BLI_strncpy(cont->name, name, sizeof(cont->name));
  }
  else {
    RNA_property_enum_name(C, &cont_ptr, prop, RNA_property_enum_get(&cont_ptr, prop), &cont_name);
    BLI_strncpy(cont->name, cont_name, sizeof(cont->name));
  }

  BLI_uniquename(&ob->controllers,
                 cont,
                 DATA_("Controller"),
                 '.',
                 offsetof(bController, name),
                 sizeof(cont->name));

  /* set the controller state mask from the current object state.
   * A controller is always in a single state, so select the lowest bit set
   * from the object state */
  for (bit = 0; bit < OB_MAX_STATES; bit++) {
    if (ob->state & (1 << bit))
      break;
  }
  cont->state_mask = (1 << bit);
  if (cont->state_mask == 0) {
    /* shouldn't happen, object state is never 0 */
    cont->state_mask = 1;
  }

  ob->scaflag |= OB_SHOWCONT;

  ED_undo_push_old(C, "controller_add_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_controller_add(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Add Controller";
  ot->description = "Add a controller to the active object";
  ot->idname = "LOGIC_OT_controller_add";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = controller_add_exec;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  /* properties */
  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          rna_enum_controller_type_items,
                          CONT_LOGIC_AND,
                          "Type",
                          "Type of controller to add");
  prop = RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the Controller to add");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_string(
      ot->srna, "object", nullptr, MAX_NAME, "Object", "Name of the Object to add the Controller to");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* ************* Add/Remove Actuator Operator ************* */

static int actuator_remove_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  bActuator *act = edit_actuator_property_get(C, op, &ob);

  if (!act)
    return OPERATOR_CANCELLED;

  BLI_remlink(&(ob->actuators), act);
  BKE_sca_unlink_actuator(act);
  BKE_sca_free_actuator(act);

  ED_undo_push_old(C, "actuator_remove_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static int actuator_remove_invoke(bContext *C, wmOperator *op, const wmEvent */*event*/)
{
  if (edit_actuator_invoke_properties(C, op)) {
    return actuator_remove_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

static void LOGIC_OT_actuator_remove(wmOperatorType *ot)
{
  ot->name = "Remove Actuator";
  ot->description = "Remove an actuator from the active object";
  ot->idname = "LOGIC_OT_actuator_remove";

  ot->invoke = actuator_remove_invoke;
  ot->exec = actuator_remove_exec;
  ot->poll = edit_actuator_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
  edit_actuator_properties(ot);
}

static int actuator_add_exec(bContext *C, wmOperator *op)
{
  Object *ob;
  bActuator *act;
  PropertyRNA *prop;
  const char *act_name;
  char name[MAX_NAME];
  int type = RNA_enum_get(op->ptr, "type");

  ob = edit_object_property_get(C, op);
  if (!ob)
    return OPERATOR_CANCELLED;

  act = BKE_sca_new_actuator(type);
  BLI_addtail(&(ob->actuators), act);

  /* set the actuator name based on rna type enum */
  PointerRNA act_ptr = RNA_pointer_create_discrete((ID *)ob, &RNA_Actuator, act);
  prop = RNA_struct_find_property(&act_ptr, "type");

  RNA_string_get(op->ptr, "name", name);
  if (*name) {
    BLI_strncpy(act->name, name, sizeof(act->name));
  }
  else {
    RNA_property_enum_name(C, &act_ptr, prop, RNA_property_enum_get(&act_ptr, prop), &act_name);
    BLI_strncpy(act->name, act_name, sizeof(act->name));
  }

  BLI_uniquename(
      &ob->actuators, act, DATA_("Actuator"), '.', offsetof(bActuator, name), sizeof(act->name));
  ob->scaflag |= OB_SHOWACT;

  ED_undo_push_old(C, "actuator_add_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_actuator_add(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Add Actuator";
  ot->description = "Add an actuator to the active object";
  ot->idname = "LOGIC_OT_actuator_add";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = actuator_add_exec;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  /* properties */
  ot->prop = prop = RNA_def_enum(ot->srna,
                                 "type",
                                 rna_enum_dummy_NULL_items,
                                 CONT_LOGIC_AND,
                                 "Type",
                                 "Type of actuator to add");
  RNA_def_enum_funcs(prop, rna_Actuator_type_itemf);
  prop = RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the Actuator to add");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_string(
      ot->srna, "object", nullptr, MAX_NAME, "Object", "Name of the Object to add the Actuator to");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* ************* Move Logic Bricks Operator ************* */
static const EnumPropertyItem logicbricks_move_direction[] = {
    {1, "UP", 0, "Move Up", ""}, {2, "DOWN", 0, "Move Down", ""}, {0, nullptr, 0, nullptr, nullptr}};

static int sensor_move_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  bSensor *sens = edit_sensor_property_get(C, op, &ob);
  int move_up = logicbricks_move_property_get(op);

  if (!sens)
    return OPERATOR_CANCELLED;

  BKE_sca_move_sensor(sens, ob, move_up);

  ED_undo_push_old(C, "sensor_move_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static int sensor_move_invoke(bContext *C, wmOperator *op, const wmEvent */*event*/)
{
  if (edit_sensor_invoke_properties(C, op)) {
    return sensor_move_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

static void LOGIC_OT_sensor_move(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Move Sensor";
  ot->description = "Move Sensor";
  ot->idname = "LOGIC_OT_sensor_move";

  /* api callbacks */
  ot->invoke = sensor_move_invoke;
  ot->exec = sensor_move_exec;
  ot->poll = edit_sensor_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  /* properties */
  edit_sensor_properties(ot);
  RNA_def_enum(
      ot->srna, "direction", logicbricks_move_direction, 1, "Direction", "Move Up or Down");
}

static int controller_move_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  bController *cont = edit_controller_property_get(C, op, &ob);
  int move_up = logicbricks_move_property_get(op);

  if (!cont)
    return OPERATOR_CANCELLED;

  BKE_sca_move_controller(cont, ob, move_up);

  ED_undo_push_old(C, "controller_move_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static int controller_move_invoke(bContext *C, wmOperator *op, const wmEvent */*event*/)
{
  if (edit_controller_invoke_properties(C, op)) {
    return controller_move_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

static void LOGIC_OT_controller_move(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Move Controller";
  ot->description = "Move Controller";
  ot->idname = "LOGIC_OT_controller_move";

  /* api callbacks */
  ot->invoke = controller_move_invoke;
  ot->exec = controller_move_exec;
  ot->poll = edit_controller_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  /* properties */
  edit_controller_properties(ot);
  RNA_def_enum(
      ot->srna, "direction", logicbricks_move_direction, 1, "Direction", "Move Up or Down");
}

static int actuator_move_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  bActuator *act = edit_actuator_property_get(C, op, &ob);
  int move_up = logicbricks_move_property_get(op);

  if (!act)
    return OPERATOR_CANCELLED;

  BKE_sca_move_actuator(act, ob, move_up);

  ED_undo_push_old(C, "actuator_move_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static int actuator_move_invoke(bContext *C, wmOperator *op, const wmEvent */*event*/)
{
  if (edit_actuator_invoke_properties(C, op)) {
    return actuator_move_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

static void LOGIC_OT_actuator_move(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Move Actuator";
  ot->description = "Move Actuator";
  ot->idname = "LOGIC_OT_actuator_move";

  /* api callbacks */
  ot->invoke = actuator_move_invoke;
  ot->exec = actuator_move_exec;
  ot->poll = edit_actuator_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  /* properties */
  edit_actuator_properties(ot);
  RNA_def_enum(
      ot->srna, "direction", logicbricks_move_direction, 1, "Direction", "Move Up or Down");
}

/* ************************ view ********************* */

static int logic_view_all_exec(bContext *C, wmOperator *op)
{
  ARegion *region = CTX_wm_region(C);
  rctf cur_new = region->v2d.tot;
  float aspect = BLI_rctf_size_y(&region->v2d.cur) / BLI_rctf_size_x(&region->v2d.cur);
  const int smooth_viewtx = WM_operator_smooth_viewtx_get(op);

  /* force the view2d code to zoom to width, not height */
  cur_new.ymin = cur_new.ymax - BLI_rctf_size_x(&cur_new) * aspect;

  UI_view2d_smooth_view(C, region, &cur_new, smooth_viewtx);

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_view_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "View All";
  ot->idname = "LOGIC_OT_view_all";
  ot->description = "Resize view so you can see all logic bricks";

  /* api callbacks */
  ot->exec = logic_view_all_exec;
  ot->poll = ED_operator_logic_active;

  /* flags */
  ot->flag = 0;
}

/* ********** flip a region alignment ********************* */

static int logic_region_flip_exec(bContext *C, wmOperator */*op*/)
{
  ScrArea *sa = CTX_wm_area(C);
  ARegion *region = logic_has_buttons_region(sa);

  if (region == nullptr)
    return OPERATOR_CANCELLED;

  if (region->alignment == RGN_ALIGN_RIGHT) {
    ED_region_toggle_hidden(C, region);
    region->alignment = RGN_ALIGN_LEFT;
  }
  else if (region->alignment == RGN_ALIGN_LEFT) {
    ED_region_toggle_hidden(C, region);
    region->alignment = RGN_ALIGN_RIGHT;
  }

  ED_area_tag_redraw(CTX_wm_area(C));
  WM_event_add_mousemove(CTX_wm_window(C));
  WM_event_add_notifier(C, NC_LOGIC, nullptr);
  ED_region_toggle_hidden(C, region);

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_region_flip(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Region flip";
  ot->idname = "LOGIC_OT_region_flip";
  ot->description = "Toggle the properties region's alignment (left/right)";

  /* api callbacks */
  ot->exec = logic_region_flip_exec;
  ot->poll = ED_operator_logic_active;

  /* flags */
  ot->flag = 0;
}

/* Custom object operators */
static int python_class_new_invoke(bContext *C, wmOperator *op, const wmEvent */*event*/)
{
  /* Better for user feedback. */
  return WM_operator_props_dialog_popup(C, op, 15 * UI_UNIT_X);
}

static int custom_object_register_exec(bContext *C, wmOperator *op)
{
  PythonProxy *pp;
  Object *ob = CTX_data_active_object(C);
  char import[MAX_NAME];

  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  RNA_string_get(op->ptr, "class_name", import);
  pp = BKE_custom_object_new(import, op->reports, C);

  if (!pp) {
    return OPERATOR_CANCELLED;
  }

  ob->custom_object = pp;

  ED_undo_push_old(C, "custom_object_register_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static int custom_object_create_exec(bContext *C, wmOperator *op)
{
  PythonProxy *pp;
  Object *ob = CTX_data_active_object(C);
  char import[MAX_NAME];

  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  RNA_string_get(op->ptr, "class_name", import);
  pp = BKE_custom_object_create_file(import, op->reports, C);

  if (!pp) {
    return OPERATOR_CANCELLED;
  }

  ob->custom_object = pp;

  ED_undo_push_old(C, "custom_object_create_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_custom_object_register(wmOperatorType *ot)
{
  ot->name = "Select Custom Object";
  ot->idname = "LOGIC_OT_custom_object_register";
  ot->description = "Use a custom KX_GameObject subclass for the selected object";

  /* api callbacks */
  ot->exec = custom_object_register_exec;
  ot->invoke = python_class_new_invoke;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *parm;
  parm = RNA_def_string(ot->srna,
                        "class_name",
                        "module.MyObject",
                        64,
                        "MyObject",
                        "The class name with module (module.ClassName)");
  RNA_def_parameter_flags(parm, (PropertyFlag)0, PARM_REQUIRED);
}

static void LOGIC_OT_custom_object_create(wmOperatorType *ot)
{
  ot->name = "Create Custom Object";
  ot->idname = "LOGIC_OT_custom_object_create";
  ot->description = "Create a KX_GameObject subclass and attach it to the selected object";

  /* api callbacks */
  ot->exec = custom_object_create_exec;
  ot->invoke = python_class_new_invoke;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *parm;
  parm = RNA_def_string(ot->srna,
                        "class_name",
                        "module.MyObject",
                        64,
                        "MyObject",
                        "The class name with module (module.ClassName)");
  RNA_def_parameter_flags(parm, (PropertyFlag)0, PARM_REQUIRED);
}

static int custom_object_remove_exec(bContext *C, wmOperator */*op*/)
{
  Object *ob = CTX_data_active_object(C);

  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  PythonProxy *pp = ob->custom_object;

  if (!pp) {
    return OPERATOR_CANCELLED;
  }

  ob->custom_object = nullptr;

  BKE_python_proxy_free(pp);

  ED_undo_push_old(C, "custom_object_remove_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_custom_object_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Custom Object";
  ot->description = "Remove this custom class from the object";
  ot->idname = "LOGIC_OT_custom_object_remove";

  /* api callbacks */
  ot->exec = custom_object_remove_exec;
  ot->poll = remove_component_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER;
}

static int custom_object_reload_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);

  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  PythonProxy *pp = ob->custom_object;

  if (!pp) {
    return OPERATOR_CANCELLED;
  }

  /* Try to create a new object */
  BKE_custom_object_reload(pp, op->reports, C);

  ED_undo_push_old(C, "custom_object_reload_exec");

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_custom_object_reload(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reload Object";
  ot->description = "Reload custom object from the source script";
  ot->idname = "LOGIC_OT_custom_object_reload";

  /* api callbacks */
  ot->exec = custom_object_reload_exec;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER;
}

/* Component operators */
static int component_register_exec(bContext *C, wmOperator *op)
{
  PythonProxy *pp;
  Object *ob = CTX_data_active_object(C);
  char import[MAX_NAME];

  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  RNA_string_get(op->ptr, "component_name", import);
  pp = BKE_python_component_new(import, op->reports, C);

  if (!pp) {
    return OPERATOR_CANCELLED;
  }

  BLI_addtail(&ob->components, pp);

  ED_undo_push_old(C, "component_register_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static int component_create_exec(bContext *C, wmOperator *op)
{
  PythonProxy *pp;
  Object *ob = CTX_data_active_object(C);
  char import[MAX_NAME];

  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  RNA_string_get(op->ptr, "component_name", import);
  pp = BKE_python_component_create_file(import, op->reports, C);

  if (!pp) {
    return OPERATOR_CANCELLED;
  }

  BLI_addtail(&ob->components, pp);

  ED_undo_push_old(C, "component_create_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_python_component_register(wmOperatorType *ot)
{
  ot->name = "Add Python Component";
  ot->idname = "LOGIC_OT_python_component_register";
  ot->description = "Add a Python component to the selected object";

  /* api callbacks */
  ot->exec = component_register_exec;
  ot->invoke = python_class_new_invoke;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *parm;
  parm = RNA_def_string(ot->srna,
                        "component_name",
                        "module.Component",
                        64,
                        "Component",
                        "The component class name with module (module.ComponentName)");
  RNA_def_parameter_flags(parm, (PropertyFlag)0, PARM_REQUIRED);
}

static void LOGIC_OT_python_component_create(wmOperatorType *ot)
{
  ot->name = "Create Python Component";
  ot->idname = "LOGIC_OT_python_component_create";
  ot->description = "Create a Python component to the selected object";

  /* api callbacks */
  ot->exec = component_create_exec;
  ot->invoke = python_class_new_invoke;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *parm;
  parm = RNA_def_string(ot->srna,
                        "component_name",
                        "module.Component",
                        64,
                        "Component",
                        "The component class name with module (module.ComponentName)");
  RNA_def_parameter_flags(parm, (PropertyFlag)0, PARM_REQUIRED);
}

static int component_remove_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  PythonProxy *pp = nullptr;
  int index = RNA_int_get(op->ptr, "index");

  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  pp = (PythonProxy *)BLI_findlink(&ob->components, index);

  if (!pp) {
    return OPERATOR_CANCELLED;
  }

  BLI_remlink(&ob->components, pp);
  BKE_python_proxy_free(pp);

  ED_undo_push_old(C, "component_remove_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_python_component_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Component";
  ot->description = "Remove this component from the object";
  ot->idname = "LOGIC_OT_python_component_remove";

  /* api callbacks */
  ot->exec = component_remove_exec;
  ot->poll = remove_component_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER;

  /* properties */
  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "Component index to remove", 0, INT_MAX);
}

static int component_move_up_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  PythonProxy *p1, *p2 = nullptr;
  int index = RNA_int_get(op->ptr, "index");

  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  p1 = (PythonProxy *)BLI_findlink(&ob->components, index);

  if (!p1 || index < 1) {
    return OPERATOR_CANCELLED;
  }

  p2 = (PythonProxy *)BLI_findlink(&ob->components, index - 1);

  if (!p2) {
    return OPERATOR_CANCELLED;
  }

  BLI_listbase_swaplinks(&ob->components, p1, p2);

  ED_undo_push_old(C, "component_move_up_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static bool component_move_up_poll(bContext *C)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "component", &RNA_PythonProxy);
  Object *ob = (ptr.owner_id) ? (Object *)ptr.owner_id :
                                blender::ed::object::context_active_object(C);

  if (!ob || ID_IS_LINKED(ob)) {
    return false;
  }

  if (ID_IS_OVERRIDE_LIBRARY(ob)) {
    CTX_wm_operator_poll_msg_set(
        C, "Cannot move component coming from linked data in a library override");
    return false;
  }

  int index = BLI_findindex(&ob->components, ptr.data);

  return index > 0;
}

static void LOGIC_OT_python_component_move_up(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Move Component Up";
  ot->description = "Move this component up in the list";
  ot->idname = "LOGIC_OT_python_component_move_up";

  /* api callbacks */
  ot->exec = component_move_up_exec;
  ot->poll = component_move_up_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER;

  /* properties */
  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "Component index to move", 0, INT_MAX);
}

static bool component_move_down_poll(bContext *C)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "component", &RNA_PythonProxy);
  Object *ob = (ptr.owner_id) ? (Object *)ptr.owner_id :
                                blender::ed::object::context_active_object(C);

  if (!ob || ID_IS_LINKED(ob)) {
    return false;
  }

  if (ID_IS_OVERRIDE_LIBRARY(ob)) {
    CTX_wm_operator_poll_msg_set(
        C, "Cannot move component coming from linked data in a library override");
    return false;
  }

  int count = BLI_listbase_count(&ob->components);
  int index = BLI_findindex(&ob->components, ptr.data);

  return index < count - 1;
}

static int component_move_down_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  PythonProxy *p1, *p2 = nullptr;
  int index = RNA_int_get(op->ptr, "index");

  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  p1 = (PythonProxy *)BLI_findlink(&ob->components, index);

  if (!p1) {
    return OPERATOR_CANCELLED;
  }

  int count = BLI_listbase_count(&ob->components);

  if (index >= count - 1) {
    return OPERATOR_CANCELLED;
  }

  p2 = (PythonProxy *)BLI_findlink(&ob->components, index + 1);

  if (!p2) {
    return OPERATOR_CANCELLED;
  }

  BLI_listbase_swaplinks(&ob->components, p1, p2);

  ED_undo_push_old(C, "component_move_down_exec");

  WM_event_add_notifier(C, NC_LOGIC, nullptr);

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_python_component_move_down(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Move Component Down";
  ot->description = "Move this component down in the list";
  ot->idname = "LOGIC_OT_python_component_move_down";

  /* api callbacks */
  ot->exec = component_move_down_exec;
  ot->poll = component_move_down_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER;

  /* properties */
  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "Component index to move", 0, INT_MAX);
}

static int component_reload_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  PythonProxy *pp = nullptr, *prev_pp = nullptr;
  int index = RNA_int_get(op->ptr, "index");

  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  if (index > 0) {
    prev_pp = (PythonProxy *)BLI_findlink(&ob->components, index - 1);
    pp = prev_pp->next;
  }
  else {
    /* pc is at the head */
    pp = (PythonProxy *)BLI_findlink(&ob->components, index);
  }

  if (!pp) {
    return OPERATOR_CANCELLED;
  }

  /* Try to create a new component */
  BKE_python_component_reload(pp, op->reports, C);

  ED_undo_push_old(C, "component_reload_exec");

  return OPERATOR_FINISHED;
}

static void LOGIC_OT_python_component_reload(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reload Component";
  ot->description = "Reload component from the source script";
  ot->idname = "LOGIC_OT_python_component_reload";

  /* api callbacks */
  ot->exec = component_reload_exec;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER;

  /* properties */
  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "Component index to reload", 0, INT_MAX);
}

/* ************************* */

/* ************************* */

void ED_operatortypes_logic()
{
  WM_operatortype_append(LOGIC_OT_sensor_remove);
  WM_operatortype_append(LOGIC_OT_sensor_add);
  WM_operatortype_append(LOGIC_OT_sensor_move);
  WM_operatortype_append(LOGIC_OT_controller_remove);
  WM_operatortype_append(LOGIC_OT_controller_add);
  WM_operatortype_append(LOGIC_OT_controller_move);
  WM_operatortype_append(LOGIC_OT_actuator_remove);
  WM_operatortype_append(LOGIC_OT_actuator_add);
  WM_operatortype_append(LOGIC_OT_actuator_move);
  WM_operatortype_append(LOGIC_OT_custom_object_register);
  WM_operatortype_append(LOGIC_OT_custom_object_reload);
  WM_operatortype_append(LOGIC_OT_custom_object_create);
  WM_operatortype_append(LOGIC_OT_custom_object_remove);
  WM_operatortype_append(LOGIC_OT_python_component_register);
  WM_operatortype_append(LOGIC_OT_python_component_reload);
  WM_operatortype_append(LOGIC_OT_python_component_create);
  WM_operatortype_append(LOGIC_OT_python_component_remove);
  WM_operatortype_append(LOGIC_OT_python_component_move_up);
  WM_operatortype_append(LOGIC_OT_python_component_move_down);
  WM_operatortype_append(LOGIC_OT_view_all);
  WM_operatortype_append(LOGIC_OT_region_flip);
}
