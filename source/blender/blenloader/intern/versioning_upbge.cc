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
#include "BLI_utildefines.h"

#include <cstdio>

/* allow readfile to use deprecated functionality */
#define DNA_DEPRECATED_ALLOW

/* Define macros in `DNA_genfile.h`. */
#define DNA_GENFILE_VERSIONING_MACROS

#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_genfile.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_force_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_sdna_types.h"
#include "DNA_sensor_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_world_types.h"

#undef DNA_GENFILE_VERSIONING_MACROS

#include "BKE_main.hh"
#include "BKE_node.hh"

#include "BLI_listbase.h"
#include "BLI_math_base.h"

#include "BLO_readfile.hh"

#include "wm_event_types.hh"

#include "readfile.hh"

#include "MEM_guardedalloc.h"

void blo_do_versions_upbge(FileData *fd, Library */*lib*/, Main *bmain)
{
  /* UPBGE hack to force defaults in files saved in normal blender2.8 */
  if (!DNA_struct_member_exists(fd->filesdna, "Scene", "GameData", "gm")) {
    LISTBASE_FOREACH (Scene *, sce, &bmain->scenes) {
      /* game data */
      sce->gm.stereoflag = STEREO_NOSTEREO;
      sce->gm.stereomode = STEREO_ANAGLYPH;
      sce->gm.eyeseparation = 0.10;
      sce->gm.xplay = 1280;
      sce->gm.yplay = 720;
      sce->gm.samples_per_frame = 1;
      sce->gm.freqplay = 60;
      sce->gm.depth = 32;
      sce->gm.gravity = 9.8f;
      sce->gm.physicsEngine = WOPHY_BULLET;
      sce->gm.mode = WO_ACTIVITY_CULLING;
      sce->gm.occlusionRes = 128;
      sce->gm.ticrate = 60;
      sce->gm.maxlogicstep = 5;
      sce->gm.physubstep = 1;
      sce->gm.maxphystep = 5;
      sce->gm.lineardeactthreshold = 0.8f;
      sce->gm.angulardeactthreshold = 1.0f;
      sce->gm.deactivationtime = 2.0f;

      sce->gm.obstacleSimulation = OBSTSIMULATION_NONE;
      sce->gm.levelHeight = 2.f;

      sce->gm.recastData.cellsize = 0.3f;
      sce->gm.recastData.cellheight = 0.2f;
      sce->gm.recastData.agentmaxslope = M_PI_4;
      sce->gm.recastData.agentmaxclimb = 0.9f;
      sce->gm.recastData.agentheight = 2.0f;
      sce->gm.recastData.agentradius = 0.6f;
      sce->gm.recastData.edgemaxlen = 12.0f;
      sce->gm.recastData.edgemaxerror = 1.3f;
      sce->gm.recastData.regionminsize = 8.f;
      sce->gm.recastData.regionmergesize = 20.f;
      sce->gm.recastData.vertsperpoly = 6;
      sce->gm.recastData.detailsampledist = 6.0f;
      sce->gm.recastData.detailsamplemaxerror = 1.0f;
      sce->gm.recastData.partitioning = RC_PARTITION_WATERSHED;

      /* Blender key code for ESC */
      sce->gm.exitkey = 218;

      sce->gm.flag |= GAME_USE_UNDO;

      sce->gm.lodflag = SCE_LOD_USE_HYST;
      sce->gm.scehysteresis = 10;

      sce->gm.timeScale = 1.0f;
      sce->gm.pythonkeys[0] = EVT_LEFTCTRLKEY;
      sce->gm.pythonkeys[1] = EVT_LEFTSHIFTKEY;
      sce->gm.pythonkeys[2] = EVT_LEFTALTKEY;
      sce->gm.pythonkeys[3] = EVT_TKEY;

      if (sce->master_collection) {
        sce->master_collection->flag &= ~COLLECTION_HAS_OBJECT_CACHE_INSTANCED;
        sce->master_collection->flag |= COLLECTION_IS_SPAWNED;
      }

      sce->gm.erp = 0.2f;
      sce->gm.erp2 = 0.8f;
      sce->gm.cfm = 0.0f;

      sce->gm.logLevel = GAME_LOG_LEVEL_WARNING;
    }

    LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
      /* UPBGE defaults*/
      ob->mass = ob->inertia = 1.0f;
      ob->formfactor = 0.4f;
      ob->damping = 0.04f;
      ob->rdamping = 0.1f;
      ob->anisotropicFriction[0] = 1.0f;
      ob->anisotropicFriction[1] = 1.0f;
      ob->anisotropicFriction[2] = 1.0f;
      ob->gameflag = OB_PROP | OB_COLLISION;
      ob->gameflag2 = 0;
      ob->margin = 0.04f;
      ob->friction = 0.5f;
      ob->init_state = 1;
      ob->state = 1;
      ob->obstacleRad = 1.0f;
      ob->step_height = 0.15f;
      ob->jump_speed = 10.0f;
      ob->fall_speed = 55.0f;
      ob->max_jumps = 1;
      ob->max_slope = M_PI_2;
      ob->col_group = 0x01;
      ob->col_mask = 0xffff;

      ob->ccd_motion_threshold = 1.0f;
      ob->ccd_swept_sphere_radius = 0.9f;

      ob->lodfactor = 1.0f;
    }
    LISTBASE_FOREACH (Camera *, cam, &bmain->cameras) {
      cam->gameflag |= GAME_CAM_OBJECT_ACTIVITY_CULLING;
      cam->lodfactor = 1.0f;
    }
    /*LISTBASE_FOREACH (Light *, light, &bmain->lights) {
      light->mode |= LA_SOFT_SHADOWS;
    }*/
    LISTBASE_FOREACH (Collection *, collection, &bmain->collections) {
      collection->flag &= ~COLLECTION_HAS_OBJECT_CACHE_INSTANCED;
      collection->flag |= COLLECTION_IS_SPAWNED;
    }
  }
  if (DNA_struct_member_exists(fd->filesdna, "Scene", "GameData", "gm") &&
      !DNA_struct_member_exists(fd->filesdna, "Object", "float", "friction"))
  {
    LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
      if (ob->type == OB_MESH) {
        Mesh *me = (Mesh *)blo_do_versions_newlibadr(fd, &ob->id, ID_IS_LINKED(ob), ob->data);
        for (int i = 0; i < me->totcol; ++i) {
          Material *ma = (Material *)blo_do_versions_newlibadr(fd, &me->id, ID_IS_LINKED(me), me->mat[i]);
          if (ma) {
            ob->friction = ma->friction;
            ob->rolling_friction = 0.0f;
            ob->fh = ma->fh;
            ob->reflect = ma->reflect;
            ob->fhdist = ma->fhdist;
            ob->xyfrict = ma->xyfrict;
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

      LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
        LISTBASE_FOREACH (bSensor *, sensor, &ob->sensors) {
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
      LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
        LISTBASE_FOREACH (bSensor *, sensor, &ob->sensors) {
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
      LISTBASE_FOREACH (Scene *, sce, &bmain->scenes) {
        sce->gm.flag |= GAME_USE_UNDO;
      }
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 0)) {
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "float", "timeScale")) {
      LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
        scene->gm.timeScale = 1.0f;
      }
    }
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "short", "pythonkeys[4]")) {
      LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
        scene->gm.pythonkeys[0] = EVT_LEFTCTRLKEY;
        scene->gm.pythonkeys[1] = EVT_LEFTSHIFTKEY;
        scene->gm.pythonkeys[2] = EVT_LEFTALTKEY;
        scene->gm.pythonkeys[3] = EVT_TKEY;
      }
    }
    if (!DNA_struct_member_exists(fd->filesdna, "BulletSoftBody", "int", "bending_dist")) {
      LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
        if (ob->bsoft) {
          ob->bsoft->margin = 0.1f;
          ob->bsoft->collisionflags |= OB_BSB_COL_CL_RS;
          ob->bsoft->bending_dist = 2;
        }
      }
    }

    LISTBASE_FOREACH (Collection *, collection, &bmain->collections) {
      collection->flag |= COLLECTION_IS_SPAWNED;
    }
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      /* Old files do not have a master collection, but it will be created by
       * `BKE_collection_master_add()`. */
      if (scene->master_collection) {
        scene->master_collection->flag |= COLLECTION_IS_SPAWNED;
      }
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 1)) {
    if (!DNA_struct_member_exists(fd->filesdna, "Object", "float", "ccd_motion_threshold")) {
      LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
        ob->ccd_motion_threshold = 1.0f;
        ob->ccd_swept_sphere_radius = 0.9f;
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 2)) {
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "float", "erp")) {
      LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
        scene->gm.erp = 0.2f;
        scene->gm.erp2 = 0.8f;
        scene->gm.cfm = 0.0f;
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
      LISTBASE_FOREACH (Object *, object, &bmain->objects) {
        object->lodfactor = 1.0f;
      }
    }
    if (!DNA_struct_member_exists(fd->filesdna, "Camera", "float", "lodfactor")) {
      LISTBASE_FOREACH (Camera *, camera, &bmain->cameras) {
        camera->lodfactor = 1.0f;
      }
    }
  }

  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 7)) {
    LISTBASE_FOREACH (Collection *, collection, &bmain->collections) {
      collection->flag &= ~COLLECTION_HAS_OBJECT_CACHE_INSTANCED;
      collection->flag |= COLLECTION_IS_SPAWNED;
    }
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      /* Old files do not have a master collection, but it will be created by
       * `BKE_collection_master_add()`. */
      if (scene->master_collection) {
        scene->master_collection->flag &= ~COLLECTION_HAS_OBJECT_CACHE_INSTANCED;
        scene->master_collection->flag |= COLLECTION_IS_SPAWNED;
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 8)) {
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "short", "samples_per_frame")) {
      LISTBASE_FOREACH (Scene *, sce, &bmain->scenes) {
        sce->gm.samples_per_frame = 1;
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 9)) {
    if (!DNA_struct_member_exists(fd->filesdna, "GameData", "short", "logLevel")) {
      LISTBASE_FOREACH (Scene *, sce, &bmain->scenes) {
        sce->gm.logLevel = GAME_LOG_LEVEL_WARNING;
      }
    }
  }
  if (!MAIN_VERSION_UPBGE_ATLEAST(bmain, 30, 11)) {
    LISTBASE_FOREACH (Camera *, cam, &bmain->cameras) {
      /* Game overlay mouse control moved from flag to gameflag */
      if (cam->flag & (1 << 11)) {
        cam->gameflag |= GAME_CAM_OVERLAY_MOUSE_CONTROL;
      }
    }
  }
}
