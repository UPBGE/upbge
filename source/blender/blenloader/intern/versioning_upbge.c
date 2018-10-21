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

#include "BLI_utildefines.h"
#include "BLI_compiler_attrs.h"

#include <stdio.h>

/* allow readfile to use deprecated functionality */
#define DNA_DEPRECATED_ALLOW

#include "DNA_genfile.h"
#include "DNA_material_types.h"
#include "DNA_object_force_types.h"
#include "DNA_object_types.h"
#include "DNA_camera_types.h"
#include "DNA_sdna_types.h"
#include "DNA_sensor_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_screen_types.h"
#include "DNA_mesh_types.h"
#include "DNA_material_types.h"
#include "DNA_world_types.h"

#include "BKE_main.h"
#include "BKE_node.h"

#include "BLI_math_base.h"

#include "BLO_readfile.h"

#include "wm_event_types.h"

#include "readfile.h"

#include "MEM_guardedalloc.h"

void blo_do_versions_upbge(FileData *fd, Library *lib, Main *main)
{
	//printf("UPBGE: open file from version : %i, subversion : %i\n", main->upbgeversionfile, main->upbgesubversionfile);
	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 0, 1)) {
		if (!DNA_struct_elem_find(fd->filesdna, "bRaySensor", "int", "mask")) {
			bRaySensor *raySensor;

			for (Object *ob = main->object.first; ob; ob = ob->id.next) {
				for (bSensor* sensor = ob->sensors.first; sensor != NULL; sensor = (bSensor *)sensor->next) {
					if (sensor->type == SENS_RAY) {
						raySensor = (bRaySensor *)sensor->data;
						raySensor->mask = 0xFFFF;//all one, 'cause this was the previous behavior
					}
				}
			}
		}
		for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
			/* Previous value of GAME_GLSL_NO_ENV_LIGHTING was 1 << 18, it was conflicting
			 * with GAME_SHOW_BOUNDING_BOX. To fix this issue, we replace 1 << 18 by
			 * 1 << 21 (the new value) when the file come from blender not UPBGE.
			 */
			if (scene->gm.flag & (1 << 18)) {
				scene->gm.flag |= GAME_GLSL_NO_ENV_LIGHTING;
				/* Disable bit 18 */
				scene->gm.flag &= ~(1 << 18);
			}
		}
	}

	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 0, 6)) {
		if (!DNA_struct_elem_find(fd->filesdna, "Material", "short", "constflag")) {
			for (Material *ma = main->mat.first; ma; ma = ma->id.next) {
				ma->constflag |= MA_CONSTANT_TEXTURE | MA_CONSTANT_TEXTURE_UV;
			}
		}
	}

	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 0, 9)) {
		if (!DNA_struct_elem_find(fd->filesdna, "GameData", "short", "pythonkeys[4]")) {
			for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
				scene->gm.pythonkeys[0] = LEFTCTRLKEY;
				scene->gm.pythonkeys[1] = LEFTSHIFTKEY;
				scene->gm.pythonkeys[2] = LEFTALTKEY;
				scene->gm.pythonkeys[3] = TKEY;
			}
		}
	}
	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 1, 0)) {
		if (!DNA_struct_elem_find(fd->filesdna, "Material", "float", "depthtranspfactor")) {
			for (Material *ma = main->mat.first; ma; ma = ma->id.next) {
				ma->depthtranspfactor = 1.0f;
			}
		}

		if (!DNA_struct_elem_find(fd->filesdna, "EnvMap", "short", "flag")) {
			for (Tex *tex = main->tex.first; tex; tex = tex->id.next) {
				if (tex->env) {
					tex->env->flag |= ENVMAP_AUTO_UPDATE;
				}
			}
		}
	}

	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 1, 1)) {
		if (!DNA_struct_elem_find(fd->filesdna, "MTex", "float", "ior")) {
			for (Material *ma = main->mat.first; ma; ma = ma->id.next) {
				for (unsigned short a = 0; a < MAX_MTEX; ++a) {
					if (ma->mtex[a]) {
						ma->mtex[a]->ior = 1.0f;
					}
				}
			}
		}
	}

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

	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 1, 5)) {
		if (!DNA_struct_elem_find(fd->filesdna, "Object", "float", "lodfactor")) {
			for (Object *ob = main->object.first; ob; ob = ob->id.next) {
				ob->lodfactor = 1.0f;
			}
		}
		if (!DNA_struct_elem_find(fd->filesdna, "Camera", "float", "lodfactor")) {
			for (Camera *ca = main->camera.first; ca; ca = ca->id.next) {
				ca->lodfactor = 1.0f;
			}
		}
		if (!DNA_struct_elem_find(fd->filesdna, "EnvMap", "float", "lodfactor")) {
			for (Tex *tex = main->tex.first; tex; tex = tex->id.next) {
				if (tex->env) {
					tex->env->lodfactor = 1.0f;
				}
			}
		}
	}

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

	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 1, 7)) {
		if (!DNA_struct_elem_find(fd->filesdna, "GameData", "float", "timeScale")) {
			for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
				scene->gm.timeScale = 1.0f;
			}
		}

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

		if (!DNA_struct_elem_find(fd->filesdna, "bMouseSensor", "int", "mask")) {
			for (Object *ob = main->object.first; ob; ob = ob->id.next) {
				for (bSensor *sensor = ob->sensors.first; sensor; sensor = (bSensor *)sensor->next) {
					if (sensor->type == SENS_MOUSE) {
						bMouseSensor *mouseSensor = (bMouseSensor *)sensor->data;
						// All one, because this was the previous behavior.
						mouseSensor->mask = 0xFFFF;
					}
				}
			}
		}
	}

	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 2, 1)) {
		if (!DNA_struct_elem_find(fd->filesdna, "BulletSoftBody", "int", "bending_dist")) {
			for (Object *ob = main->object.first; ob; ob = ob->id.next) {
				if (ob->bsoft) {
					ob->bsoft->bending_dist = 2;
				}
			}
		}
	}

	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 2, 2)) {
		for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
			scene->gm.mode |= WO_ACTIVITY_CULLING;
		}

		for (Camera *camera = main->camera.first; camera; camera = camera->id.next) {
			camera->gameflag |= GAME_CAM_OBJECT_ACTIVITY_CULLING;
			camera->gameviewport.rightratio = 1.0f;
			camera->gameviewport.topratio = 1.0f;
		}
	}

	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 2, 2)) {
		if (!DNA_struct_elem_find(fd->filesdna, "Object", "float", "max_slope")) {
			for (Object *ob = main->object.first; ob; ob = ob->id.next) {
				ob->max_slope = M_PI_2;
			}
		}
	}

	if (!MAIN_VERSION_UPBGE_ATLEAST(main, 2, 4)) {
		FOREACH_NODETREE(main, ntree, id) {
			if (ntree->type == NTREE_SHADER) {
				for (bNode *node = ntree->nodes.first; node; node = node->next) {
					switch (node->type) {
						case 194:
						{
							node->type = SH_NODE_OBJECT;
							break;
						}
						case 195:
						{
							node->type = SH_NODE_TIME;
							break;
						}
						case 196:
						{
							node->type = SH_NODE_PARALLAX;
							break;
						}
					}
				}
			}
		} FOREACH_NODETREE_END

		if (!DNA_struct_elem_find(fd->filesdna, "Lamp", "float", "cutoff")) {
			for (Lamp *lamp = main->lamp.first; lamp; lamp = lamp->id.next) {
				lamp->radius = 8.0f;
				lamp->cutoff = 0.001f;
			}
		}

		for (bScreen *sc = main->screen.first; sc; sc = sc->id.next) {
			for (ScrArea *sa = sc->areabase.first; sa; sa = sa->next) {
				for (SpaceLink *sl = sa->spacedata.first; sl; sl = sl->next) {
					if (sl->spacetype == SPACE_VIEW3D) {
						View3D *v3d = (View3D *)sl;
						v3d->flag2 |= V3D_SHOW_MIST;
					}
				}
			}
		}

		if (!DNA_struct_elem_find(fd->filesdna, "GameData", "short", "colorManagement")) {
			for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
				scene->gm.colorManagement = GAME_COLOR_MANAGEMENT_SRGB;
			}
		}

		if (!DNA_struct_elem_find(fd->filesdna, "MTex", "short", "colorManagement")) {
			for (Material *ma = main->mat.first; ma; ma = ma->id.next) {
				for (unsigned short a = 0; a < MAX_MTEX; ++a) {
					if (ma->mtex[a]) {
						ma->mtex[a]->colorManagement = GAME_COLOR_MANAGEMENT_SRGB;
					}
				}
			}

			for (World *wo = main->world.first; wo; wo = wo->id.next) {
				for (unsigned short a = 0; a < MAX_MTEX; ++a) {
					if (wo->mtex[a]) {
						wo->mtex[a]->colorManagement = GAME_COLOR_MANAGEMENT_SRGB;
					}
				}
			}
		}
	}
}
