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
#include "DNA_object_types.h"
#include "DNA_sdna_types.h"
#include "DNA_sensor_types.h"
#include "DNA_space_types.h"

#include "BKE_main.h"

#include "BLO_readfile.h"

#include "readfile.h"

#include "MEM_guardedalloc.h"

void blo_do_versions_upbge(FileData *fd, Library *UNUSED(lib), Main *main)
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
	}
}
