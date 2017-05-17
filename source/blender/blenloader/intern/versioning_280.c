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
 * Contributor(s): Dalai Felinto
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 */

/** \file blender/blenloader/intern/versioning_280.c
 *  \ingroup blenloader
 */

/* allow readfile to use deprecated functionality */
#define DNA_DEPRECATED_ALLOW

#include "DNA_object_types.h"
#include "DNA_camera_types.h"
#include "DNA_gpu_types.h"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_sensor_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"
#include "DNA_genfile.h"

#include "BKE_blender.h"
#include "BKE_collection.h"
#include "BKE_idprop.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_scene.h"

#include "BLI_listbase.h"
#include "BLI_mempool.h"
#include "BLI_string.h"

#include "BLO_readfile.h"
#include "readfile.h"

#include "MEM_guardedalloc.h"

#include "wm_event_types.h"

void do_versions_after_linking_280(Main *main)
{
	if (!MAIN_VERSION_ATLEAST(main, 280, 0)) {
		char version[48];
		BKE_blender_version_string(version, sizeof(version), main->versionfile, main->subversionfile, false, false);

		for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
			/* since we don't have access to FileData we check the (always valid) first render layer instead */
			if (scene->render_layers.first == NULL) {
				SceneCollection *sc_master = BKE_collection_master(scene);
				BLI_strncpy(sc_master->name, "Master Collection", sizeof(sc_master->name));

				SceneCollection *collections[20] = {NULL};
				bool is_visible[20];

				int lay_used = 0;
				for (int i = 0; i < 20; i++) {
					char name[MAX_NAME];

					BLI_snprintf(name, sizeof(collections[i]->name), "Collection %d [converted from %s]", i + 1, version);
					collections[i] = BKE_collection_add(scene, sc_master, name);

					is_visible[i] = (scene->lay & (1 << i));
				}

				for (Base *base = scene->base.first; base; base = base->next) {
					lay_used |= base->lay & ((1 << 20) - 1); /* ignore localview */

					for (int i = 0; i < 20; i++) {
						if ((base->lay & (1 << i)) != 0) {
							BKE_collection_object_add(scene, collections[i], base->object);
						}
					}
				}

				scene->active_layer = 0;

				if (!BKE_scene_uses_blender_game(scene)) {
					for (SceneRenderLayer *srl = scene->r.layers.first; srl; srl = srl->next) {

						SceneLayer *sl = BKE_scene_layer_add(scene, srl->name);
						BKE_scene_layer_engine_set(sl, scene->r.engine);

						if (srl->mat_override) {
							BKE_collection_override_datablock_add((LayerCollection *)sl->layer_collections.first, "material", (ID *)srl->mat_override);
						}

						if (srl->light_override && BKE_scene_uses_blender_internal(scene)) {
							/* not sure how we handle this, pending until we design the override system */
							TODO_LAYER_OVERRIDE;
						}

						if (srl->lay != scene->lay) {
							/* unlink master collection  */
							BKE_collection_unlink(sl, sl->layer_collections.first);

							/* add new collection bases */
							for (int i = 0; i < 20; i++) {
								if ((srl->lay & (1 << i)) != 0) {
									BKE_collection_link(sl, collections[i]);
								}
							}
						}

						/* for convenience set the same active object in all the layers */
						if (scene->basact) {
							sl->basact = BKE_scene_layer_base_find(sl, scene->basact->object);
						}

						/* TODO: passes, samples, mask_layesr, exclude, ... */
					}

					if (BLI_findlink(&scene->render_layers, scene->r.actlay)) {
						scene->active_layer = scene->r.actlay;
					}
				}

				SceneLayer *sl = BKE_scene_layer_add(scene, "Viewport");

				/* In this particular case we can safely assume the data struct */
				LayerCollection *lc = ((LayerCollection *)sl->layer_collections.first)->layer_collections.first;
				for (int i = 0; i < 20; i++) {
					if (!is_visible[i]) {
						lc->flag &= ~COLLECTION_VISIBLE;
					}
					lc = lc->next;
				}

				/* convert active base */
				if (scene->basact) {
					sl->basact = BKE_scene_layer_base_find(sl, scene->basact->object);
				}

				/* convert selected bases */
				for (Base *base = scene->base.first; base; base = base->next) {
					Base *ob_base = BKE_scene_layer_base_find(sl, base->object);
					if ((base->flag & SELECT) != 0) {
						if ((ob_base->flag & BASE_SELECTABLED) != 0) {
							ob_base->flag |= BASE_SELECTED;
						}
					}
					else {
						ob_base->flag &= ~BASE_SELECTED;
					}

					/* keep lay around for forward compatibility (open those files in 2.79) */
					ob_base->lay = base->lay;
				}

				/* TODO: copy scene render data to layer */

				/* Cleanup */
				for (int i = 0; i < 20; i++) {
					if ((lay_used & (1 << i)) == 0) {
						BKE_collection_remove(scene, collections[i]);
					}
				}

				/* Fallback name if only one layer was found in the original file */
				if (BLI_listbase_count_ex(&sc_master->scene_collections, 2) == 1) {
					BKE_collection_rename(scene, sc_master->scene_collections.first, "Default Collection");
				}

				/* remove bases once and for all */
				for (Base *base = scene->base.first; base; base = base->next) {
					id_us_min(&base->object->id);
				}
				BLI_freelistN(&scene->base);
				scene->basact = NULL;
			}
		}

		for (bScreen *screen = main->screen.first; screen; screen = screen->id.next) {
			for (ScrArea *sa = screen->areabase.first; sa; sa = sa->next) {
				for (SpaceLink *sl = sa->spacedata.first; sl; sl = sl->next) {
					if (sl->spacetype == SPACE_OUTLINER) {
						SpaceOops *soutliner = (SpaceOops *)sl;
						SceneLayer *layer = BKE_scene_layer_context_active(screen->scene);

						soutliner->outlinevis = SO_ACT_LAYER;

						if (BLI_listbase_count_ex(&layer->layer_collections, 2) == 1) {
							if (soutliner->treestore == NULL) {
								soutliner->treestore = BLI_mempool_create(
								        sizeof(TreeStoreElem), 1, 512, BLI_MEMPOOL_ALLOW_ITER);
							}

							/* Create a tree store element for the collection. This is normally
							 * done in check_persistent (outliner_tree.c), but we need to access
							 * it here :/ (expand element if it's the only one) */
							TreeStoreElem *tselem = BLI_mempool_calloc(soutliner->treestore);
							tselem->type = TSE_LAYER_COLLECTION;
							tselem->id = layer->layer_collections.first;
							tselem->nr = tselem->used = 0;
							tselem->flag &= ~TSE_CLOSED;
						}
					}
				}
			}
		}
	}
}

static void do_version_layer_collections_idproperties(ListBase *lb)
{
	IDPropertyTemplate val = {0};
	for (LayerCollection *lc = lb->first; lc; lc = lc->next) {
		lc->properties = IDP_New(IDP_GROUP, &val, ROOT_PROP);
		BKE_layer_collection_engine_settings_create(lc->properties);

		/* No overrides at first */
		for (IDProperty *prop = lc->properties->data.group.first; prop; prop = prop->next) {
			while (prop->data.group.first) {
				IDP_FreeFromGroup(prop, prop->data.group.first);
			}
		}

		/* Do it recursively */
		do_version_layer_collections_idproperties(&lc->layer_collections);
	}
}

void blo_do_versions_280(FileData *fd, Library *lib, Main *main)
{
	if (!MAIN_VERSION_ATLEAST(main, 280, 0)) {
		if (!DNA_struct_elem_find(fd->filesdna, "Scene", "ListBase", "render_layers")) {
			for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
				/* Master Collection */
				scene->collection = MEM_callocN(sizeof(SceneCollection), "Master Collection");
				BLI_strncpy(scene->collection->name, "Master Collection", sizeof(scene->collection->name));
			}
		}

		if (DNA_struct_elem_find(fd->filesdna, "LayerCollection", "ListBase", "engine_settings") &&
		    !DNA_struct_elem_find(fd->filesdna, "LayerCollection", "IDProperty", "properties"))
		{
			for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
				for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
					do_version_layer_collections_idproperties(&sl->layer_collections);
				}
			}
		}

	}

	if (!DNA_struct_elem_find(fd->filesdna, "GPUDOFSettings", "float", "ratio"))	{
		for (Camera *ca = main->camera.first; ca; ca = ca->id.next) {
			ca->gpu_dof.ratio = 1.0f;
		}
	}

	if (!DNA_struct_elem_find(fd->filesdna, "SceneLayer", "IDProperty", "*properties")) {
		for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
			for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
				IDPropertyTemplate val = {0};
				sl->properties = IDP_New(IDP_GROUP, &val, ROOT_PROP);
				BKE_scene_layer_engine_settings_create(sl->properties);
			}
		}
	}

	/* GAME ENGINE */

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

	if (!DNA_struct_elem_find(fd->filesdna, "Material", "short", "constflag")) {
		for (Material *ma = main->mat.first; ma; ma = ma->id.next) {
			ma->constflag |= MA_CONSTANT_TEXTURE | MA_CONSTANT_TEXTURE_UV;
		}
	}

	if (!DNA_struct_elem_find(fd->filesdna, "GameData", "short", "pythonkeys[4]")) {
		for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
			scene->gm.pythonkeys[0] = LEFTCTRLKEY;
			scene->gm.pythonkeys[1] = LEFTSHIFTKEY;
			scene->gm.pythonkeys[2] = LEFTALTKEY;
			scene->gm.pythonkeys[3] = TKEY;
		}
	}

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

	if (!DNA_struct_elem_find(fd->filesdna, "MTex", "float", "ior")) {
		for (Material *ma = main->mat.first; ma; ma = ma->id.next) {
			for (unsigned short a = 0; a < MAX_MTEX; ++a) {
				if (ma->mtex[a]) {
					ma->mtex[a]->ior = 1.0f;
				}
			}
		}
	}

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

	for (bScreen *sc = main->screen.first; sc; sc = sc->id.next) {
		for (ScrArea *sa = sc->areabase.first; sa; sa = sa->next) {
			for (SpaceLink *sl = sa->spacedata.first; sl; sl = sl->next) {
				if (sl->spacetype == SPACE_VIEW3D) {
					View3D *v3d = (View3D *)sl;
					v3d->flag3 = V3D_SHOW_MIST;
				}
			}
		}
	}

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
