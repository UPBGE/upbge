/**
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
 * Contributor(s): Mitchell Stokes, Diego Lopes, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifndef DNA_COMPONENT_TYPES_H
#define DNA_COMPONENT_TYPES_H

#include "DNA_listBase.h"

typedef struct ComponentProperty {
	struct ComponentProperty *next, *prev;
	char name[32];
	short type, pad;
	int data;
	void *ptr, *ptr2;
} ComponentProperty;

typedef struct PythonComponent {
	struct PythonComponent *next, *prev;
	ListBase properties;
	char name[64];
	char module[64];
} PythonComponent;


/* ComponentProperty.type */
#define CPROP_TYPE_INT         0
#define CPROP_TYPE_FLOAT       1
#define CPROP_TYPE_STRING      2
#define CPROP_TYPE_BOOLEAN     3
#define CPROP_TYPE_SET         4

#endif // DNA_COMPONENT_TYPES_H
