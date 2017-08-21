/*
 * Copyright 2016, Blender Foundation.
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
 * Contributor(s): Blender Institute
 *
 */

/** \file GE_engine.c
 *  \ingroup draw_engine
 */

#include "DRW_render.h"

#include "../eevee/eevee_private.h"
#include "../eevee/eevee_engine.h"

static void GE_engine_init(void *ved)
{
	draw_engine_eevee_type.engine_init(ved);
}

static void GE_cache_init(void *vedata)
{
	draw_engine_eevee_type.cache_init(vedata);
}

static void GE_cache_populate(void *vedata, Object *ob)
{
	draw_engine_eevee_type.cache_populate(vedata, ob);
}

static void GE_cache_finish(void *vedata)
{
	draw_engine_eevee_type.cache_finish(vedata);
}

static void GE_draw_background(void *vedata)
{
	draw_engine_eevee_type.draw_background(vedata);
}

static void GE_engine_free(void)
{
	draw_engine_eevee_type.engine_free();
}

static void GE_layer_collection_settings_create(RenderEngine *engine, IDProperty *props)
{
	DRW_engine_viewport_eevee_type.collection_settings_create(engine, props);
}

static void GE_scene_layer_settings_create(RenderEngine *engine, IDProperty *props)
{
	DRW_engine_viewport_eevee_type.render_settings_create(engine, props);
}

static const DrawEngineDataSize GE_data_size = DRW_VIEWPORT_DATA_SIZE(EEVEE_Data);

DrawEngineType draw_engine_game_type = {
	NULL, NULL,
	N_("Blender Game"),
	&GE_data_size,
	&GE_engine_init,
	&GE_engine_free,
	&GE_cache_init,
	&GE_cache_populate,
	&GE_cache_finish,
	&GE_draw_background,
	NULL//&GE_draw_scene
};

RenderEngineType DRW_engine_viewport_game_type = {
	NULL, NULL,
	"BLENDER_GAME", N_("Blender Game"), RE_INTERNAL | RE_USE_SHADING_NODES,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	&GE_layer_collection_settings_create, &GE_scene_layer_settings_create,
	&draw_engine_eevee_type,
	{NULL, NULL, NULL}
};
