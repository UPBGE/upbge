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

#pragma once

#include "BKE_lib_query.hh"

/** \file
 *  \ingroup bke
 */

struct Main;
struct Object;
struct bSensor;
struct bController;
struct bActuator;

/* Logicbricks */
void BKE_sca_link_logicbricks(void **poin, void ***ppoin, short *tot, short size);
void BKE_sca_unlink_logicbricks(void **poin, void ***ppoin, short *tot);
void BKE_sca_remap_data_postprocess_links_logicbricks_update(struct Main *bmain,
                                     struct Object *ob_old,
                                     struct Object *ob_new);
void BKE_sca_copy_logicbricks(struct Object *ob_new, const struct Object *ob, const int flag);

/* Controllers */
void BKE_sca_unlink_controller(struct bController *cont);
void BKE_sca_unlink_controllers(struct ListBase *lb);
void BKE_sca_free_controller(struct bController *cont);
void BKE_sca_free_controllers(struct ListBase *lb);
struct bController *BKE_sca_copy_controller(struct bController *cont, const int flag);
void BKE_sca_copy_controllers(struct ListBase *lbn, const struct ListBase *lbo, const int flag);
void BKE_sca_init_controller(struct bController *cont);
struct bController *BKE_sca_new_controller(int type);
void BKE_sca_move_controller(struct bController *cont_to_move, struct Object *ob, int move_up);

/* Actuators */
void BKE_sca_unlink_actuator(struct bActuator *act);
void BKE_sca_unlink_actuators(struct ListBase *lb);
void BKE_sca_free_actuator(struct bActuator *act);
void BKE_sca_free_actuators(struct ListBase *lb);
struct bActuator *BKE_sca_copy_actuator(struct bActuator *act);
void BKE_sca_copy_actuators(struct ListBase *lbn, const struct ListBase *lbo);
void BKE_sca_init_actuator(struct bActuator *act);
struct bActuator *BKE_sca_new_actuator(int type);
void BKE_sca_move_actuator(struct bActuator *act_to_move, struct Object *ob, int move_up);

/* Sensors */
void BKE_sca_free_sensor(struct bSensor *sens);
void BKE_sca_free_sensors(struct ListBase *lb);
struct bSensor *BKE_sca_copy_sensor(struct bSensor *sens, const int flag);
void BKE_sca_copy_sensors(struct ListBase *lbn, const struct ListBase *lbo, const int flag);
void BKE_sca_init_sensor(struct bSensor *sens);
struct bSensor *BKE_sca_new_sensor(int type);
void BKE_sca_move_sensor(struct bSensor *sens_to_move, struct Object *ob, int move_up);

/* Points */
void BKE_sca_clear_new_points_ob(struct Object *ob);
void BKE_sca_clear_new_points(void);
void BKE_sca_set_new_points_ob(struct Object *ob);
void BKE_sca_set_new_points(void);

/* States */
const char *BKE_sca_get_name_state(Object *ob, short bit);

/* Callback format for performing operations on ID-pointers for sensors/controllers/actuators. */
typedef void (*SCASensorIDFunc)(struct bSensor *sensor,
                                struct ID **idpoin,
                                void *userdata,
                                LibraryForeachIDCallbackFlag cb_flag);
typedef void (*SCAControllerIDFunc)(struct bController *controller,
                                    struct ID **idpoin,
                                    void *userdata,
                                    LibraryForeachIDCallbackFlag cb_flag);
typedef void (*SCAActuatorIDFunc)(struct bActuator *actuator,
                                  struct ID **idpoin,
                                  void *userdata,
                                  LibraryForeachIDCallbackFlag cb_flag);

void BKE_sca_sensors_id_loop(struct ListBase *senslist, SCASensorIDFunc func, void *userdata);
void BKE_sca_controllers_id_loop(struct ListBase *contlist,
                                 SCAControllerIDFunc func,
                                 void *userdata);
void BKE_sca_actuators_id_loop(struct ListBase *atclist, SCAActuatorIDFunc func, void *userdata);
