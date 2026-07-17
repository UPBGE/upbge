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
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 */

/** \file blender/blenloader/intern/versioning_upbge.c
 *  \ingroup blenloader
 */

#include "BLI_compiler_attrs.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include <cstdio>

/* allow readfile to use deprecated functionality */
#define DNA_DEPRECATED_ALLOW

/* Define macros in `DNA_genfile.h`. */
#define DNA_GENFILE_VERSIONING_MACROS

#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_genfile.h"
#include "DNA_key_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_force_types.h"
#include "DNA_object_types.h"
#include "DNA_node_types.h"
#include "DNA_rigidbody_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_sdna_types.h"
#include "DNA_sensor_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_world_types.h"

#undef DNA_GENFILE_VERSIONING_MACROS

#include "BKE_mesh_legacy_convert.hh"
#include "BKE_main.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_object.hh"

#include "versioning_common.hh"

#include "BLI_bounds.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"

#include "BLO_readfile.hh"

#include "wm_event_types.hh"

#include "readfile.hh"

#include "MEM_guardedalloc.h"

namespace blender {

static void upbge_vehicle_normalize_or_default(float vector[3], const float default_value[3])
{
  if (normalize_v3(vector) == 0.0f) {
    copy_v3_v3(vector, default_value);
  }
}

static void upbge_vehicle_derive_wheel_frame(const Object *chassis_ob,
                                             const Object *wheel_ob,
                                             float r_connection_point[3],
                                             float r_down_direction[3],
                                             float r_axle_direction[3],
                                             float r_down_world[3])
{
  const float default_down[3] = {0.0f, 0.0f, -1.0f};
  const float default_axle[3] = {1.0f, 0.0f, 0.0f};
  float chassis_mat[4][4];
  float chassis_inv[4][4];
  float wheel_mat[4][4];

  BKE_object_to_mat4(chassis_ob, chassis_mat);
  BKE_object_to_mat4(wheel_ob, wheel_mat);
  invert_m4_m4(chassis_inv, chassis_mat);

  copy_v3_v3(r_connection_point, wheel_mat[3]);
  mul_m4_v3(chassis_inv, r_connection_point);

  copy_v3_v3(r_down_world, default_down);
  mul_mat3_m4_v3(wheel_mat, r_down_world);
  upbge_vehicle_normalize_or_default(r_down_world, default_down);

  copy_v3_v3(r_down_direction, r_down_world);
  mul_mat3_m4_v3(chassis_inv, r_down_direction);
  upbge_vehicle_normalize_or_default(r_down_direction, default_down);

  copy_v3_v3(r_axle_direction, default_axle);
  mul_mat3_m4_v3(wheel_mat, r_axle_direction);
  upbge_vehicle_normalize_or_default(r_axle_direction, default_axle);
  mul_mat3_m4_v3(chassis_inv, r_axle_direction);
  upbge_vehicle_normalize_or_default(r_axle_direction, default_axle);
}

static float upbge_vehicle_resolve_wheel_radius(const Object *chassis_ob,
                                                const Object *wheel_ob,
                                                const float axle_direction[3],
                                                const float down_direction[3],
                                                float stored_radius)
{
  float radius = max_ff(stored_radius, 0.01f);
  if (!chassis_ob || !wheel_ob) {
    return radius;
  }

  std::optional<Bounds<float3>> bounds = BKE_object_boundbox_get(wheel_ob);
  if (!bounds) {
    return radius;
  }

  float chassis_mat[4][4];
  float wheel_mat[4][4];
  float axle_world[3];
  float radial_axis_a[3];
  float radial_axis_b[3];

  BKE_object_to_mat4(chassis_ob, chassis_mat);
  BKE_object_to_mat4(wheel_ob, wheel_mat);

  copy_v3_v3(axle_world, axle_direction);
  mul_mat3_m4_v3(chassis_mat, axle_world);
  upbge_vehicle_normalize_or_default(axle_world, (const float[3]){1.0f, 0.0f, 0.0f});

  copy_v3_v3(radial_axis_a, down_direction);
  mul_mat3_m4_v3(chassis_mat, radial_axis_a);
  madd_v3_v3fl(radial_axis_a, axle_world, -dot_v3v3(radial_axis_a, axle_world));
  if (normalize_v3(radial_axis_a) == 0.0f) {
    const float fallback_axis[3] = {fabsf(axle_world[0]) < 0.9f ? 1.0f : 0.0f,
                                    fabsf(axle_world[0]) < 0.9f ? 0.0f : 1.0f,
                                    0.0f};
    cross_v3_v3v3(radial_axis_a, axle_world, fallback_axis);
    upbge_vehicle_normalize_or_default(radial_axis_a, (const float[3]){0.0f, 0.0f, 1.0f});
  }

  cross_v3_v3v3(radial_axis_b, axle_world, radial_axis_a);
  upbge_vehicle_normalize_or_default(radial_axis_b, (const float[3]){0.0f, 0.0f, 1.0f});

  float wheel_origin[3];
  copy_v3_v3(wheel_origin, wheel_mat[3]);

  float max_radial_extent_a = 0.0f;
  float max_radial_extent_b = 0.0f;
  for (const float3 &corner : bounds::corners(*bounds)) {
    float world_corner[3] = {corner[0], corner[1], corner[2]};
    mul_m4_v3(wheel_mat, world_corner);
    sub_v3_v3(world_corner, wheel_origin);

    max_radial_extent_a = max_ff(max_radial_extent_a, fabsf(dot_v3v3(world_corner, radial_axis_a)));
    max_radial_extent_b = max_ff(max_radial_extent_b, fabsf(dot_v3v3(world_corner, radial_axis_b)));
  }

  const float resolved_radius = max_ff(max_radial_extent_a, max_radial_extent_b);
  return resolved_radius > 1.0e-6f ? resolved_radius : radius;
}

static void upbge_vehicle_offset_world_location(Object *ob, const float offset_world[3])
{
  float object_matrix[4][4];
  BKE_object_to_mat4(ob, object_matrix);
  add_v3_v3(object_matrix[3], offset_world);
  BKE_object_apply_mat4(ob, object_matrix, false, true);
}

static void upbge_vehicle_store_manual_hardpoint(GameVehicleSettings *ws,
                                                 const float wheel_center[3],
                                                 const float down_direction[3],
                                                 const float axle_direction[3],
                                                 float wheel_radius)
{
  copy_v3_v3(ws->connection_point, wheel_center);
  madd_v3_v3fl(ws->connection_point, down_direction, -wheel_radius);
  copy_v3_v3(ws->down_direction, down_direction);
  copy_v3_v3(ws->axle_direction, axle_direction);
  ws->wheel_rest_length = wheel_radius;
}

void blo_do_versions_upbge(FileData *fd, Library * /*lib*/, Main *bmain)
{
  /* UPBGE hack to force defaults in files saved in normal blender2.8 */
  if (!DNA_struct_member_exists(fd->filesdna, "Scene", "GameData", "gm")) {
    for (Scene &sce : bmain->scenes) {
      /* game data */
      sce.gm.stereoflag = STEREO_NOSTEREO;
      sce.gm.stereomode = STEREO_ANAGLYPH;
      sce.gm.eyeseparation = 0.10;
      sce.gm.xplay = 1280;
      sce.gm.yplay = 720;
      sce.gm.samples_per_frame = 1;
      sce.gm.freqplay = 60;
      sce.gm.depth = 32;
      sce.gm.gravity = 9.8f;
      sce.gm.physicsEngine = WOPHY_JOLT;
      sce.gm.mode = WO_ACTIVITY_CULLING;
      sce.gm.occlusionRes = 128;
      sce.gm.ticrate = 60;
      sce.gm.maxlogicstep = 5;
      sce.gm.physubstep = 1;
      sce.gm.maxphystep = 5;
      sce.gm.use_fixed_physics_timestep = 1;
      sce.gm.physics_tick_rate = 60;
      sce.gm.use_fixed_physics_interpolation = 0;
      sce.gm.use_fixed_fps_cap = 1;
      sce.gm.fixed_render_cap_rate = 60;
      sce.gm.lineardeactthreshold = 0.03f;
      sce.gm.angulardeactthreshold = 0.03f;
      sce.gm.deactivationtime = 0.5f;

      sce.gm.obstacleSimulation = OBSTSIMULATION_NONE;
      sce.gm.levelHeight = 2.f;

      sce.gm.recastData.cellsize = 0.3f;
      sce.gm.recastData.cellheight = 0.2f;
      sce.gm.recastData.agentmaxslope = M_PI_4;
      sce.gm.recastData.agentmaxclimb = 0.9f;
      sce.gm.recastData.agentheight = 2.0f;
      sce.gm.recastData.agentradius = 0.6f;
      sce.gm.recastData.edgemaxlen = 12.0f;
      sce.gm.recastData.edgemaxerror = 1.3f;
      sce.gm.recastData.regionminsize = 8.f;
      sce.gm.recastData.regionmergesize = 20.f;
      sce.gm.recastData.vertsperpoly = 6;
      sce.gm.recastData.detailsampledist = 6.0f;
      sce.gm.recastData.detailsamplemaxerror = 1.0f;
      sce.gm.recastData.partitioning = RC_PARTITION_WATERSHED;

      /* Blender key code for ESC */
      sce.gm.exitkey = 218;

      sce.gm.flag |= GAME_USE_UNDO;

      sce.gm.lodflag = SCE_LOD_USE_HYST;
      sce.gm.scehysteresis = 10;

      sce.gm.timeScale = 1.0f;
      sce.gm.pythonkeys[0] = EVT_LEFTCTRLKEY;
      sce.gm.pythonkeys[1] = EVT_LEFTSHIFTKEY;
      sce.gm.pythonkeys[2] = EVT_LEFTALTKEY;
      sce.gm.pythonkeys[3] = EVT_TKEY;

      if (sce.master_collection) {
        sce.master_collection->flag &= ~COLLECTION_HAS_OBJECT_CACHE_INSTANCED;
        sce.master_collection->flag |= COLLECTION_IS_SPAWNED;
      }

      sce.gm.erp = 0.2f;
      sce.gm.erp2 = 0.8f;
      sce.gm.cfm = 0.0f;

      sce.gm.logLevel = GAME_LOG_LEVEL_WARNING;
    }

    for (Object &ob : bmain->objects) {
      /* UPBGE defaults*/
      ob.mass = ob.inertia = 1.0f;
      ob.formfactor = 0.4f;
      ob.damping = 0.04f;
      ob.rdamping = 0.1f;
      ob.anisotropicFriction[0] = 1.0f;
      ob.anisotropicFriction[1] = 1.0f;
      ob.anisotropicFriction[2] = 1.0f;
      ob.gameflag = OB_PROP | OB_COLLISION;
      ob.gameflag2 = 0;
      ob.margin = 0.04f;
      ob.friction = 0.5f;
      ob.gravity_factor = 1.0f;
      ob.init_state = 1;
      ob.state = 1;
      ob.obstacleRad = 1.0f;
      ob.step_height = 0.15f;
      ob.jump_speed = 10.0f;
      ob.fall_speed = 55.0f;
      ob.max_jumps = 1;
      ob.max_slope = M_PI_2;
      ob.col_group = 0x01;
      ob.col_mask = 0xffff;

      ob.ccd_motion_threshold = 1.0f;
      ob.ccd_swept_sphere_radius = 0.9f;

      ob.lodfactor = 1.0f;
    }
    for (Camera &cam : bmain->cameras) {
      cam.gameflag |= GAME_CAM_OBJECT_ACTIVITY_CULLING;
      cam.lodfactor = 1.0f;
    }
    /*LISTBASE_FOREACH (Light *, light, &bmain->lights) {
      light->mode |= LA_SOFT_SHADOWS;
    }*/
    for (Collection &collection : bmain->collections) {
      collection.flag &= ~COLLECTION_HAS_OBJECT_CACHE_INSTANCED;
      collection.flag |= COLLECTION_IS_SPAWNED;
    }
  }
  if (!DNA_struct_member_exists(
          fd->filesdna, "GameData", "char", "use_fixed_physics_interpolation"))
  {
    for (Scene &scene : bmain->scenes) {
      scene.gm.use_fixed_physics_interpolation = scene.gm.physicsEngine == WOPHY_JOLT ? 0 : 1;
    }
  }
  if (DNA_struct_member_exists(fd->filesdna, "Scene", "GameData", "gm") &&
      !DNA_struct_member_exists(fd->filesdna, "Object", "float", "friction"))
  {
    for (Object &ob : bmain->objects) {
      if (ob.type == OB_MESH) {
        Mesh *me = (Mesh *)blo_do_versions_newlibadr(fd, &ob.id, ID_IS_LINKED(&ob), ob.data);
        for (int i = 0; i < me->totcol; ++i) {
          Material *ma = (Material *)blo_do_versions_newlibadr(
              fd, &me->id, ID_IS_LINKED(me), me->mat[i]);
          if (ma) {
            ob.friction = ma->friction;
            ob.rolling_friction = 0.0f;
            ob.fh = ma->fh;
            ob.reflect = ma->reflect;
            ob.fhdist = ma->fhdist;
            ob.xyfrict = ma->xyfrict;
            break;
          }
        }
      }
    }
  }
  /* UPBGE hack to force defaults in files saved in normal blender2.8 END */

  /* printf("UPBGE: open file from versionfile: %i, subversionfile: %i\n", main->upbgeversionfile,
   * main->upbgesubversionfile); */
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 0, 1)) {
    if (!DNA_struct_member_exists(fd->filesdna, "bRaySensor", "int", "mask")) {
      bRaySensor *raySensor;

      for (Object &ob : bmain->objects) {
        for (bSensor *sensor = (bSensor *)ob.sensors.first; sensor; sensor = sensor->next) {
          if (sensor->type == SENS_RAY) {
            raySensor = (bRaySensor *)sensor->data;
            /* All one, because this was the previous behavior */
            raySensor->mask = 0xFFFF;
          }
        }
      }
    }
  }

#if 0 /* XXX UPBGE | we need to increase upbge version and translate from do_versions_280 */
    if (!MAIN_VERSION_UPBGE_ATLEAST(main, 1, 2)) {
        if (!DNA_struct_elem_find(fd->filesdna, "Object", "float", "friction")) {
            for (Object *ob = main->object.first; ob; ob = ob->id.next) {
                if (ob->type == OB_MESH) {
                    Mesh *me = blo_do_versions_newlibadr(fd, lib, ob->data);
                    bool converted = false;
                    for (unsigned short i = 0; i < me->totcol; ++i) {
                        Material *ma = blo_do_versions_newlibadr(fd, lib, me->mat[i]);
                        if (ma) {
                            ob->friction = ma->friction;
                            ob->rolling_friction = ma->rolling_friction;
                            ob->fh = ma->fh;
                            ob->reflect = ma->reflect;
                            ob->fhdist = ma->fhdist;
                            ob->xyfrict = ma->xyfrict;
                            if (ma->dynamode & MA_FH_NOR) {
                                ob->dynamode |= OB_FH_NOR;
                            }
                            converted = true;
                            break;
                        }
                    }
                    /* There's no valid material, we use the settings from BKE_object_init. */
                    if (!converted) {
                        ob->friction = 0.5f;
                    }
                }
            }
        }
    }
#endif
#if 0 /* XXX UPBGE Pending clean-up of GameData */
    if (!MAIN_VERSION_UPBGE_ATLEAST(main, 1, 6)) {
        if (!DNA_struct_elem_find(fd->filesdna, "GameData", "short", "showBoundingBox")) {
            for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
                scene->gm.showBoundingBox = (scene->gm.flag & GAME_SHOW_BOUNDING_BOX) ? GAME_DEBUG_FORCE : GAME_DEBUG_DISABLE;
            }
        }
        if (!DNA_struct_elem_find(fd->filesdna, "GameData", "short", "showArmatures")) {
            for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
                scene->gm.showArmatures = (scene->gm.flag & GAME_SHOW_ARMATURES) ? GAME_DEBUG_ALLOW : GAME_DEBUG_DISABLE;
            }
        }
        if (!DNA_struct_elem_find(fd->filesdna, "GameData", "short", "showCameraFrustum")) {
            for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
                scene->gm.showCameraFrustum = GAME_DEBUG_ALLOW;
            }
        }
    }
#endif
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 1, 7)) {
#if 0 /* XXX UPBGE Pending recoveries */
        if (!DNA_struct_elem_find(fd->filesdna, "Camera", "short", "gameflag")) {
            for (Camera *camera = main->camera.first; camera; camera = camera->id.next) {
                /* Previous value of GAME_CAM_SHOW_FRUSTUM was 1 << 10, it was possibly conflicting
                 * with new flags. To fix this issue we use a separate flag value: gameflag.
                 */
                if (camera->flag & (1 << 10)) {
                    camera->gameflag |= GAME_CAM_SHOW_FRUSTUM;
                    /* Disable bit 10 */
                    camera->flag &= ~(1 << 10);
                }
            }
        }
#endif
    if (!DNA_struct_member_exists(fd->filesdna, "bMouseSensor", "int", "mask")) {
      for (Object &ob : bmain->objects) {
        for (bSensor *sensor = (bSensor *)ob.sensors.first; sensor; sensor = sensor->next) {
          if (sensor->type == SENS_MOUSE) {
            bMouseSensor *mouseSensor = (bMouseSensor *)sensor->data;
            /* All one, because this was the previous behavior */
            mouseSensor->mask = 0xFFFF;
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 3, 0)) {
    /* In this case we check against GameData to maintain previous behaviour */
    if (DNA_struct_member_exists(fd->filesdna, "Scene", "GameData", "gm")) {
      for (Scene &sce : bmain->scenes) {
        sce.gm.flag |= GAME_USE_UNDO;
      }
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 0)) {
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "float", "timeScale")) {
      for (Scene &scene : bmain->scenes) {
        scene.gm.timeScale = 1.0f;
      }
    }
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "short", "pythonkeys[4]")) {
      for (Scene &scene : bmain->scenes) {
        scene.gm.pythonkeys[0] = EVT_LEFTCTRLKEY;
        scene.gm.pythonkeys[1] = EVT_LEFTSHIFTKEY;
        scene.gm.pythonkeys[2] = EVT_LEFTALTKEY;
        scene.gm.pythonkeys[3] = EVT_TKEY;
      }
    }
    if (!DNA_struct_member_exists(fd->filesdna, "BulletSoftBody", "int", "bending_dist")) {
      for (Object &ob : bmain->objects) {
        if (ob.bsoft) {
          ob.bsoft->margin = 0.1f;
          ob.bsoft->collisionflags |= OB_BSB_COL_CL_RS;
          ob.bsoft->bending_dist = 2;
        }
      }
    }

    for (Collection &collection : bmain->collections) {
      collection.flag |= COLLECTION_IS_SPAWNED;
    }
    for (Scene &scene : bmain->scenes) {
      /* Old files do not have a master collection, but it will be created by
       * `BKE_collection_master_add()`. */
      if (scene.master_collection) {
        scene.master_collection->flag |= COLLECTION_IS_SPAWNED;
      }
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 1)) {
    if (!DNA_struct_member_exists(fd->filesdna, "Object", "float", "ccd_motion_threshold")) {
      for (Object &ob : bmain->objects) {
        ob.ccd_motion_threshold = 1.0f;
        ob.ccd_swept_sphere_radius = 0.9f;
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 2)) {
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "float", "erp")) {
      for (Scene &scene : bmain->scenes) {
        scene.gm.erp = 0.2f;
        scene.gm.erp2 = 0.8f;
        scene.gm.cfm = 0.0f;
      }
    }
  }

  /*if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 3)) {
    LISTBASE_FOREACH (Light *, light, &bmain->lights) {
      light->mode |= LA_SOFT_SHADOWS;
    }
  }*/

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 4)) {
    if (!DNA_struct_member_exists(fd->filesdna, "Object", "float", "lodfactor")) {
      for (Object &ob : bmain->objects) {
        ob.lodfactor = 1.0f;
      }
    }
    if (!DNA_struct_member_exists(fd->filesdna, "Camera", "float", "lodfactor")) {
      for (Camera &camera : bmain->cameras) {
        camera.lodfactor = 1.0f;
      }
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 7)) {
    for (Collection &collection : bmain->collections) {
      collection.flag &= ~COLLECTION_HAS_OBJECT_CACHE_INSTANCED;
      collection.flag |= COLLECTION_IS_SPAWNED;
    }
    for (Scene &scene : bmain->scenes) {
      /* Old files do not have a master collection, but it will be created by
       * `BKE_collection_master_add()`. */
      if (scene.master_collection) {
        scene.master_collection->flag &= ~COLLECTION_HAS_OBJECT_CACHE_INSTANCED;
        scene.master_collection->flag |= COLLECTION_IS_SPAWNED;
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 8)) {
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "short", "samples_per_frame")) {
      for (Scene &scene : bmain->scenes) {
        scene.gm.samples_per_frame = 1;
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 9)) {
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "short", "logLevel")) {
      for (Scene &scene : bmain->scenes) {
        scene.gm.logLevel = GAME_LOG_LEVEL_WARNING;
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 11)) {
    for (Camera &cam : bmain->cameras) {
      /* Game overlay mouse control moved from flag to gameflag */
      if (cam.flag & (1 << 11)) {
        cam.gameflag |= GAME_CAM_OVERLAY_MOUSE_CONTROL;
        cam.flag = static_cast<eCamera_Flag>(cam.flag & ~(1 << 11));
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 45, 1)) {
    for (Object &ob : bmain->objects) {
      /* OB_TRANSFLAG_OVERRIDE_GAME_PRIORITY moved from 1 << 14 to 1 << 31 */
      if (ob.transflag & (1 << 14)) {
        ob.transflag = static_cast<eObject_TransFlag>(ob.transflag | (1 << 31));
        ob.transflag = static_cast<eObject_TransFlag>(ob.transflag & ~(1 << 14));
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 50, 1)) {
    for (Object &ob : bmain->objects) {
      if (ob.body_type == OB_BOUND_EMPTY) {
        ob.body_type = OB_BOUND_BOX;
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 50, 4)) {
    for (Mesh &mesh : bmain->meshes) {
      BKE_mesh_legacy_recast_to_generic(&mesh);
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 50, 5)) {
    /* Migrate to mode-specific timing variables (Phase 2 refactoring) */
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "short", "fixed_render_cap_rate")) {
      for (Scene &scene : bmain->scenes) {
        /* Initialize fixed mode render cap rate from legacy ticrate
         * This ensures old .blend files continue to work with same behavior.
         * NOTE: fixed_logic_rate and fixed_max_logic_step removed - logic coupled to physics in fixed mode */
        scene.gm.fixed_render_cap_rate = scene.gm.ticrate;
      }
    }
  }
  if (!DNA_struct_member_exists(fd->filesdna, "GameData", "short", "jolt_physics_threads")) {
    for (Scene &scene : bmain->scenes) {
      scene.gm.jolt_physics_threads = -1;
      scene.gm.jolt_max_bodies = 65536;
      scene.gm.jolt_max_body_pairs = 65536;
      scene.gm.jolt_max_contact_constraints = 65536;
      scene.gm.jolt_temp_allocator_mb = 32;
    }
  }
  if (!DNA_struct_member_exists(
          fd->filesdna, "GameData", "short", "jolt_velocity_solver_iterations"))
  {
    for (Scene &scene : bmain->scenes) {
      scene.gm.jolt_velocity_solver_iterations = 10;
      scene.gm.jolt_position_solver_iterations = 2;
    }
  }
  if (!DNA_struct_member_exists(
          fd->filesdna, "Object", "short", "jolt_velocity_solver_iterations"))
  {
    for (Object &ob : bmain->objects) {
      ob.gameflag2 &= ~OB_JOLT_OVERRIDE_SOLVER_ITERATIONS;
      ob.jolt_velocity_solver_iterations = 10;
      ob.jolt_position_solver_iterations = 2;
    }
  }
  if (!DNA_struct_member_exists(fd->filesdna, "GameData", "short", "frame_time_graph_record_slot"))
  {
    for (Scene &scene : bmain->scenes) {
      scene.gm.frame_time_graph_visible_slots = 0;
      scene.gm.frame_time_graph_record_slot = -1;
      scene.gm.frame_time_graph_window = 2;
      scene.gm.frame_time_graph_axis = GAME_FRAME_TIME_GRAPH_AXIS_FRAMES;
      scene.gm.frame_time_graph_style = GAME_FRAME_TIME_GRAPH_STYLE_LINE;
      scene.gm.frame_time_graph_visible_domains = GAME_FRAME_TIME_GRAPH_DOMAIN_FRAME;
      BLI_strncpy(scene.gm.frame_time_graph_slot_names[0],
                  "Slot 1",
                  sizeof(scene.gm.frame_time_graph_slot_names[0]));
      BLI_strncpy(scene.gm.frame_time_graph_slot_names[1],
                  "Slot 2",
                  sizeof(scene.gm.frame_time_graph_slot_names[1]));
      BLI_strncpy(scene.gm.frame_time_graph_slot_names[2],
                  "Slot 3",
                  sizeof(scene.gm.frame_time_graph_slot_names[2]));
      BLI_strncpy(scene.gm.frame_time_graph_slot_names[3],
                  "Slot 4",
                  sizeof(scene.gm.frame_time_graph_slot_names[3]));
    }
  }
  if (!DNA_struct_member_exists(fd->filesdna, "GameData", "int", "frame_time_graph_max_samples"))
  {
    for (Scene &scene : bmain->scenes) {
      scene.gm.frame_time_graph_max_samples = 10000;
    }
  }
  if (!DNA_struct_member_exists(
          fd->filesdna, "GameData", "int", "frame_time_graph_visible_domains"))
  {
    for (Scene &scene : bmain->scenes) {
      scene.gm.frame_time_graph_visible_domains = GAME_FRAME_TIME_GRAPH_DOMAIN_FRAME;
    }
  }
  if (!DNA_struct_member_exists(
          fd->filesdna, "GameData", "char", "frame_time_graph_slot_names[4][64]"))
  {
    for (Scene &scene : bmain->scenes) {
      BLI_strncpy(scene.gm.frame_time_graph_slot_names[0],
                  "Slot 1",
                  sizeof(scene.gm.frame_time_graph_slot_names[0]));
      BLI_strncpy(scene.gm.frame_time_graph_slot_names[1],
                  "Slot 2",
                  sizeof(scene.gm.frame_time_graph_slot_names[1]));
      BLI_strncpy(scene.gm.frame_time_graph_slot_names[2],
                  "Slot 3",
                  sizeof(scene.gm.frame_time_graph_slot_names[2]));
      BLI_strncpy(scene.gm.frame_time_graph_slot_names[3],
                  "Slot 4",
                  sizeof(scene.gm.frame_time_graph_slot_names[3]));
    }
  }
  if (!DNA_struct_member_exists(fd->filesdna, "Object", "GameVehicleSettings", "*vehicle")) {
    for (Object &ob : bmain->objects) {
      ob.vehicle = nullptr;
      ob.gameflag2 &= ~OB_HAS_VEHICLE;
    }
  }

  /* Initialize chassis_roll_influence for files saved before it existed.
   * The default of 0.1 matches the old per-wheel wheel_roll_influence default,
   * preserving existing behavior. */
  if (!DNA_struct_member_exists(
          fd->filesdna, "GameVehicleSettings", "float", "chassis_roll_influence"))
  {
    for (Object &ob : bmain->objects) {
      if (ob.vehicle && ob.vehicle->vehicle_type == OB_VEHICLE_TYPE_CHASSIS) {
        ob.vehicle->chassis_roll_influence = 0.1f;
      }
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 52, 7)) {
    for (Object &ob : bmain->objects) {
      if (!ob.vehicle || ob.vehicle->vehicle_type != OB_VEHICLE_TYPE_CHASSIS) {
        continue;
      }

      if (ob.vehicle->lsd_preset == OB_VEHICLE_LSD_RACE) {
        /* The removed Race preset already wrote its resolved ratio into
         * limited_slip_ratio, so preserve that value and fall back to Custom. */
        ob.vehicle->lsd_preset = OB_VEHICLE_LSD_CUSTOM;
      }
    }
  }
}

void do_versions_after_linking_upbge(FileData *fd, Main *bmain)
{
  /* Vehicle migration depends on Object ID pointers being remapped first.
   * Running it in blo_do_versions_upbge() can dereference stale file addresses
   * when loading older blend files. */
  if (!DNA_struct_member_exists(fd->filesdna, "GameVehicleSettings", "int", "vehicle_type")) {
    for (Object &ob : bmain->objects) {
      if (!(ob.gameflag2 & OB_HAS_VEHICLE) || !ob.vehicle) {
        continue;
      }

      GameVehicleSettings *vs = ob.vehicle;
      vs->vehicle_type = OB_VEHICLE_TYPE_CHASSIS;

      for (GameVehicleWheel *wheel =
               static_cast<GameVehicleWheel *>(vs->legacy_wheels.first);
           wheel;
           wheel = wheel->next)
      {
        Object *wheel_obj = wheel->wheel_object;
        if (!wheel_obj) {
          continue;
        }

        if (!wheel_obj->vehicle) {
          wheel_obj->vehicle = MEM_new<GameVehicleSettings>("GameVehicleSettings");
        }
        GameVehicleSettings *ws = wheel_obj->vehicle;
        ws->vehicle_type = OB_VEHICLE_TYPE_WHEEL;
        ws->flags = vs->flags;
        ws->chassis_object = &ob;

        copy_v3_v3(ws->connection_point, wheel->connection_point);
        copy_v3_v3(ws->down_direction, wheel->down_direction);
        copy_v3_v3(ws->axle_direction, wheel->axle_direction);

        ws->wheel_engine_force = wheel->engine_force;
        ws->wheel_brake = wheel->brake;
        ws->wheel_steering = wheel->steering;

        ws->wheel_radius = wheel->wheel_radius;
        ws->wheel_width = wheel->wheel_width;
        ws->wheel_rest_length = wheel->wheel_rest_length;
        ws->wheel_friction_slip = wheel->wheel_friction_slip;
        ws->wheel_longitudinal_friction = wheel->wheel_friction_slip;
        ws->wheel_lateral_friction = wheel->wheel_friction_slip;
        ws->wheel_roll_influence = wheel->wheel_roll_influence;

        ws->suspension_stiffness = wheel->suspension_stiffness;
        ws->suspension_travel = wheel->suspension_travel;
        ws->damping_compression = wheel->damping_compression;
        ws->damping_relaxation = wheel->damping_relaxation;

        ws->wheel_flags = wheel->flags | OB_VEHICLE_WHEEL_COMBINE_FRICTION_AXES;
        ws->collision_mode = wheel->collision_mode;

        wheel_obj->gameflag2 |= OB_HAS_VEHICLE;
      }
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 52, 8)) {
    for (Object &ob : bmain->objects) {
      if (!ob.vehicle || ob.vehicle->vehicle_type != OB_VEHICLE_TYPE_WHEEL) {
        continue;
      }

      GameVehicleSettings *ws = ob.vehicle;
      Object *chassis_ob = ws->chassis_object;
      const bool use_transform = (ws->wheel_flags & OB_VEHICLE_WHEEL_USE_OBJECT_TRANSFORM) != 0;
      const bool auto_ride_height = (ws->wheel_flags & OB_VEHICLE_WHEEL_AUTO_RIDE_HEIGHT) != 0;

      float wheel_center[3];
      float down_direction[3];
      float axle_direction[3];
      float down_world[3] = {0.0f, 0.0f, -1.0f};

      if (use_transform && chassis_ob) {
        upbge_vehicle_derive_wheel_frame(
            chassis_ob, &ob, wheel_center, down_direction, axle_direction, down_world);

        if (!auto_ride_height) {
          float center_offset_world[3];
          copy_v3_v3(center_offset_world, down_world);
          mul_v3_fl(center_offset_world, ws->wheel_rest_length);
          upbge_vehicle_offset_world_location(&ob, center_offset_world);
          upbge_vehicle_derive_wheel_frame(
              chassis_ob, &ob, wheel_center, down_direction, axle_direction, down_world);
        }
      }
      else {
        const float default_down[3] = {0.0f, 0.0f, -1.0f};
        const float default_axle[3] = {1.0f, 0.0f, 0.0f};

        copy_v3_v3(wheel_center, ws->connection_point);
        copy_v3_v3(down_direction, ws->down_direction);
        upbge_vehicle_normalize_or_default(down_direction, default_down);
        copy_v3_v3(axle_direction, ws->axle_direction);
        upbge_vehicle_normalize_or_default(axle_direction, default_axle);

        if (!auto_ride_height) {
          madd_v3_v3fl(wheel_center, down_direction, ws->wheel_rest_length);
        }
      }

      float wheel_radius = ws->wheel_radius;
      if ((ws->wheel_flags & OB_VEHICLE_WHEEL_AUTO_RADIUS) && chassis_ob) {
        wheel_radius = upbge_vehicle_resolve_wheel_radius(
            chassis_ob, &ob, axle_direction, down_direction, wheel_radius);
      }
      wheel_radius = max_ff(wheel_radius, 0.01f);

      upbge_vehicle_store_manual_hardpoint(
          ws, wheel_center, down_direction, axle_direction, wheel_radius);
      ws->wheel_flags &= ~OB_VEHICLE_WHEEL_AUTO_RIDE_HEIGHT;
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 52, 9)) {
    for (Object &ob : bmain->objects) {
      if (!ob.vehicle || ob.vehicle->vehicle_type != OB_VEHICLE_TYPE_WHEEL) {
        continue;
      }

      GameVehicleSettings *ws = ob.vehicle;
      ws->wheel_flags |= OB_VEHICLE_WHEEL_COMBINE_FRICTION_AXES;
      ws->wheel_longitudinal_friction = ws->wheel_friction_slip;
      ws->wheel_lateral_friction = ws->wheel_friction_slip;
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 52, 10)) {
    for (Object &ob : bmain->objects) {
      if (!ob.vehicle || ob.vehicle->vehicle_type != OB_VEHICLE_TYPE_CHASSIS) {
        continue;
      }

      ob.vehicle->flags |= OB_VEHICLE_SIMPLE_DRIVE;
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 52, 11)) {
    for (Object &ob : bmain->objects) {
      if (!ob.vehicle || ob.vehicle->vehicle_type != OB_VEHICLE_TYPE_CHASSIS) {
        continue;
      }

      ob.vehicle->solver_iterations = 0;
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 52, 12)) {
    for (Object &ob : bmain->objects) {
      if (!ob.vehicle || ob.vehicle->vehicle_type != OB_VEHICLE_TYPE_CHASSIS) {
        continue;
      }

      ob.vehicle->max_pitch_roll_angle = (float)M_PI;
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 52, 13)) {
    if (!DNA_struct_member_exists(fd->filesdna, "BulletSoftBody", "float", "plasticThreshold")) {
      for (Object &ob : bmain->objects) {
        if (!ob.bsoft) {
          continue;
        }

        ob.bsoft->plasticThreshold = 0.1f;
        ob.bsoft->plasticStrength = 1.0f;
        ob.bsoft->plasticMaxDeform = 1.0f;
        ob.bsoft->plasticRepairRate = 0.0f;
      }
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 52, 16)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id) {
      if (node_tree->type != NTREE_LOGIC) {
        continue;
      }
      for (bNode &node : node_tree->nodes.items_reversed_mutable()) {
        if (STREQ(node.idname, "LogicNativeSendMessage") ||
            node.type_legacy == LN_NODE_SEND_MESSAGE)
        {
          version_node_remove(*node_tree, node);
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 52, 17)) {
    if (!DNA_struct_member_exists(
            fd->filesdna, "RigidBodyCon", "short", "jolt_velocity_solver_iterations"))
    {
      for (Object &ob : bmain->objects) {
        if (ob.rigidbody_constraint) {
          ob.rigidbody_constraint->jolt_velocity_solver_iterations = 10;
          ob.rigidbody_constraint->jolt_position_solver_iterations = 2;
        }
      }
    }

    FOREACH_NODETREE_BEGIN (bmain, node_tree, id) {
      if (node_tree->type != NTREE_LOGIC) {
        continue;
      }
      for (bNode &node : node_tree->nodes) {
        if (!STREQ(node.idname, "LogicNativeAddPhysicsConstraint")) {
          continue;
        }
        for (bNodeSocket &socket : node.inputs) {
          if (STREQ(socket.name, "Solver Iterations")) {
            STRNCPY(socket.name, "Velocity Solver Iterations");
          }
          if (STREQ(socket.identifier, "Solver Iterations")) {
            version_node_socket_identifier_set(socket, "Velocity Solver Iterations");
          }
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 52, 15)) {
    if (!DNA_struct_member_exists(fd->filesdna, "Object", "ListBase", "logic_node_bindings")) {
      for (Object &ob : bmain->objects) {
        ob.logic_node_bindings = {nullptr, nullptr};
      }
    }
  }
}

}  // namespace blender
