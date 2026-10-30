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

/** \file DNA_property_types.h
 *  \ingroup DNA
 *  \since mar-2001
 *  \author nzc
 *  \attention Renderrecipe and scene decription. The fact that there is a
 *             hierarchy here is a bit strange, and not desirable.
 *
 * #bProperty type is specifically for use by Objects game-logic.
 */

#pragma once

#include "DNA_listBase.h"

namespace blender {

struct Object;

/* ********************* PROPERTY ************************ */

typedef struct bProperty {
  struct bProperty *next, *prev;
  char name[64]; /* MAX_NAME */
  short type, flag;
  int data;   /* data should be 4 bytes to store int,float stuff; Choice: the selected option */
  void *poin; /* references data unless its a string which is malloc'd */
  /** Vector (XYZ) and Color (RGBA) value. */
  float vec[4];
  /** Object value. */
  struct Object *object;
  /** Choice options (their names), List items, and Table rows (#GPROP_ROW, whose items are the
   * cells: one per column, in column order). */
  ListBase items;
  /** Table columns: name, type and the value of new cells. */
  ListBase columns;
  /** List: the type of its items. */
  short item_type;
  char _pad0[2];
  /** Selected row of the item and column lists in the UI. */
  int active_item;
  int active_column;
  char _pad1[4];
  /** List and Table: the name of the scene's #GameInventoryRules this list follows; empty: the
   * defaults. */
  char inventory_rules[64];
} bProperty;

/* property->type XXX Game Property, not RNA */
#define GPROP_BOOL 0
#define GPROP_INT 1
#define GPROP_FLOAT 2
#define GPROP_STRING 3
/* 4 was an old Vector type: never reuse it. */
#define GPROP_TIME 5
#define GPROP_VECTOR 6
#define GPROP_COLOR 7
#define GPROP_OBJECT 8
#define GPROP_CHOICE 9
#define GPROP_LIST 10
#define GPROP_TABLE 11
/** A Table row; never a property of its own. */
#define GPROP_ROW 12

/* property->flag */
#define PROP_DEBUG 1
#define PROP_SAVE_GAME (1 << 1)

#define MAX_PROPSTRING 128

}  // namespace blender
