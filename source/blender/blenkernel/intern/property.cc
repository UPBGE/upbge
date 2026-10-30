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
 * Contributor(s): ton roosendaal
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/property.c
 *  \ingroup bke
 *
 * This module deals with bProperty only,
 * they are used on blender objects in the game engine
 * (where they get converted into C++ classes - EXP_Value and subclasses)
 */

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BKE_property.hh"

#include <algorithm>

#include "BLI_listbase.h"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_utils.hh"

#include "DNA_object_types.h"
#include "DNA_property_types.h"

namespace blender {
void BKE_bproperty_free(bProperty *prop)
{

  if (prop->poin && prop->poin != &prop->data)
    MEM_delete_void(prop->poin);
  BKE_bproperty_free_list(&prop->items);
  BKE_bproperty_free_list(&prop->columns);
  MEM_delete(prop);
}

void BKE_bproperty_free_list(ListBase *lb)
{
  bProperty *prop;

  while ((prop = (bProperty *)BLI_pophead(lb))) {
    BKE_bproperty_free(prop);
  }
}

bProperty *BKE_bproperty_copy(const bProperty *prop)
{
  bProperty *propn;

  propn = (bProperty *)MEM_dupalloc(prop);
  if (prop->poin && prop->poin != &prop->data) {
    propn->poin = MEM_dupalloc_void(prop->poin);
  }
  else {
    propn->poin = &propn->data;
  }
  BLI_listbase_clear(&propn->items);
  BLI_listbase_clear(&propn->columns);
  BKE_bproperty_copy_list(&propn->items, &prop->items);
  BKE_bproperty_copy_list(&propn->columns, &prop->columns);

  return propn;
}

void BKE_bproperty_copy_list(ListBase *lbn, const ListBase *lbo)
{
  bProperty *prop, *propn;
  BKE_bproperty_free_list(lbn); /* in case we are copying to an object with props */
  prop = (bProperty *)lbo->first;
  while (prop) {
    propn = BKE_bproperty_copy(prop);
    BLI_addtail(lbn, propn);
    prop = prop->next;
  }
}

void BKE_bproperty_init(bProperty *prop)
{
  /* also use when property changes type */

  if (prop->poin && prop->poin != &prop->data)
    MEM_delete_void(prop->poin);
  prop->poin = nullptr;

  prop->data = 0;
  zero_v4(prop->vec);
  prop->object = nullptr;
  BKE_bproperty_free_list(&prop->items);
  BKE_bproperty_free_list(&prop->columns);
  prop->active_item = 0;
  prop->active_column = 0;

  switch (prop->type) {
    case GPROP_STRING:
      prop->poin = MEM_new_zeroed(MAX_PROPSTRING, "property string");
      break;
    case GPROP_COLOR:
      copy_v4_fl(prop->vec, 1.0f);
      prop->poin = &prop->data;
      break;
    case GPROP_CHOICE:
      /* Like Blender's Menu Switch node. */
      STRNCPY(BKE_bproperty_item_add(prop, false)->name, "A");
      STRNCPY(BKE_bproperty_item_add(prop, false)->name, "B");
      prop->active_item = 0;
      prop->poin = &prop->data;
      break;
    case GPROP_LIST:
      if (!ELEM(prop->item_type,
                GPROP_BOOL,
                GPROP_INT,
                GPROP_FLOAT,
                GPROP_STRING,
                GPROP_VECTOR,
                GPROP_COLOR,
                GPROP_OBJECT))
      {
        prop->item_type = GPROP_FLOAT;
      }
      prop->poin = &prop->data;
      break;
    default:
      prop->poin = &prop->data;
      break;
  }
}

static bProperty *new_value(const int type)
{
  bProperty *value = (bProperty *)MEM_new_zeroed(sizeof(bProperty), "property");
  value->type = type;
  BKE_bproperty_init(value);
  return value;
}

/** A cell for \a column: its type and the value new rows start with. */
static bProperty *new_cell(const bProperty *column)
{
  bProperty *cell = BKE_bproperty_copy(column);
  cell->name[0] = '\0';
  return cell;
}

ListBase *BKE_bproperty_owner_list(Object *ob, const bProperty *prop, bProperty **r_owner)
{
  if (r_owner) {
    *r_owner = nullptr;
  }
  if (BLI_findindex(&ob->prop, prop) != -1) {
    return &ob->prop;
  }
  for (bProperty *owner : ListBaseWrapper<bProperty>(ob->prop)) {
    if (BLI_findindex(&owner->columns, prop) != -1) {
      if (r_owner) {
        *r_owner = owner;
      }
      return &owner->columns;
    }
    if (BLI_findindex(&owner->items, prop) != -1) {
      if (r_owner) {
        *r_owner = owner;
      }
      return &owner->items;
    }
    for (bProperty *row : ListBaseWrapper<bProperty>(owner->items)) {
      if (BLI_findindex(&row->items, prop) != -1) {
        if (r_owner) {
          *r_owner = row;
        }
        return &row->items;
      }
    }
  }
  return nullptr;
}

bProperty *BKE_bproperty_item_add(bProperty *prop, const bool column)
{
  bProperty *item = nullptr;
  if (column && prop->type == GPROP_TABLE) {
    item = new_value(GPROP_FLOAT);
    STRNCPY(item->name, "Column");
    BLI_addtail(&prop->columns, item);
    BLI_uniquename(
        &prop->columns, item, "Column", '.', offsetof(bProperty, name), sizeof(item->name));
    BKE_bproperty_table_sync(prop);
    prop->active_column = BLI_listbase_count(&prop->columns) - 1;
    return item;
  }
  switch (prop->type) {
    case GPROP_CHOICE:
      item = new_value(GPROP_STRING);
      STRNCPY(item->name, "Option");
      BLI_addtail(&prop->items, item);
      BLI_uniquename(
          &prop->items, item, "Option", '.', offsetof(bProperty, name), sizeof(item->name));
      break;
    case GPROP_LIST:
      item = new_value(prop->item_type);
      BLI_addtail(&prop->items, item);
      break;
    case GPROP_TABLE:
      item = new_value(GPROP_ROW);
      BLI_addtail(&prop->items, item);
      BKE_bproperty_table_sync(prop);
      break;
    default:
      return nullptr;
  }
  prop->active_item = BLI_listbase_count(&prop->items) - 1;
  return item;
}

bool BKE_bproperty_item_remove(bProperty *prop, const bool column, const int index)
{
  ListBase *list = column ? &prop->columns : &prop->items;
  bProperty *item = (bProperty *)BLI_findlink(list, index);
  if (item == nullptr) {
    return false;
  }
  BLI_remlink(list, item);
  BKE_bproperty_free(item);
  if (column) {
    for (bProperty *row : ListBaseWrapper<bProperty>(prop->items)) {
      if (bProperty *cell = (bProperty *)BLI_findlink(&row->items, index)) {
        BLI_remlink(&row->items, cell);
        BKE_bproperty_free(cell);
      }
    }
  }
  if (prop->type == GPROP_CHOICE && prop->data >= index && prop->data > 0) {
    prop->data--;
  }
  int &active = column ? prop->active_column : prop->active_item;
  active = std::clamp(active, 0, std::max(0, BLI_listbase_count(list) - 1));
  return true;
}

bool BKE_bproperty_item_move(bProperty *prop, const bool column, const int index, const int step)
{
  ListBase *list = column ? &prop->columns : &prop->items;
  const int other = index + step;
  if (index < 0 || other < 0 || other >= BLI_listbase_count(list) ||
      !BLI_listbase_link_move(list, BLI_findlink(list, index), step))
  {
    return false;
  }
  if (column) {
    for (bProperty *row : ListBaseWrapper<bProperty>(prop->items)) {
      if (void *cell = BLI_findlink(&row->items, index)) {
        BLI_listbase_link_move(&row->items, cell, step);
      }
    }
  }
  if (prop->type == GPROP_CHOICE) {
    /* The selected option stays selected. */
    if (prop->data == index) {
      prop->data = other;
    }
    else if (prop->data == other) {
      prop->data = index;
    }
  }
  (column ? prop->active_column : prop->active_item) = other;
  return true;
}

void BKE_bproperty_list_set_item_type(bProperty *prop, const int type)
{
  prop->item_type = type;
  for (bProperty *item : ListBaseWrapper<bProperty>(prop->items)) {
    if (item->type != type) {
      item->type = type;
      BKE_bproperty_init(item);
    }
  }
}

void BKE_bproperty_table_sync(bProperty *table)
{
  const int column_count = BLI_listbase_count(&table->columns);
  for (bProperty *row : ListBaseWrapper<bProperty>(table->items)) {
    while (BLI_listbase_count(&row->items) > column_count) {
      BKE_bproperty_free((bProperty *)BLI_poptail(&row->items));
    }
    bProperty *cell = (bProperty *)row->items.first;
    for (const bProperty *column : ConstListBaseWrapper<bProperty>(table->columns)) {
      if (cell == nullptr) {
        BLI_addtail(&row->items, new_cell(column));
        continue;
      }
      if (cell->type != column->type) {
        cell->type = column->type;
        BKE_bproperty_init(cell);
      }
      cell = cell->next;
    }
  }
}

bProperty *BKE_bproperty_new(int type)
{
  bProperty *prop;

  prop = (bProperty *)MEM_new_zeroed(sizeof(bProperty), "property");
  prop->type = type;

  BKE_bproperty_init(prop);

  strcpy(prop->name, "prop");

  return prop;
}

bProperty *BKE_bproperty_object_get(Object *ob, const char *name)
{
  return (bProperty *)BLI_findstring(&ob->prop, name, offsetof(bProperty, name));
}

void BKE_bproperty_object_set(Object *ob, bProperty *propc)
{
  bProperty *prop;
  prop = BKE_bproperty_object_get(ob, propc->name);
  if (prop) {
    BLI_remlink(&ob->prop, prop);
    BKE_bproperty_free(prop);
  }
  BLI_addtail(&ob->prop, BKE_bproperty_copy(propc));
}

/* negative: prop is smaller
 * positive: prop is larger
 */
#if 0 /* UNUSED */
int BKE_bproperty_cmp(bProperty *prop, const char *str)
{
//	extern int Gdfra;		/* sector.c */
	float fvalue, ftest;
	
	switch (prop->type) {
		case GPROP_BOOL:
			if (BLI_strcasecmp(str, "true") == 0) {
				if (prop->data == 1) return 0;
				else return 1;
			}
			else if (BLI_strcasecmp(str, "false") == 0) {
				if (prop->data == 0) return 0;
				else return 1;
			}
		/* no break, do GPROP_int too! */
		
		case GPROP_INT:
			return prop->data - atoi(str);

		case GPROP_FLOAT:
		case GPROP_TIME:
			/* WARNING: untested for GPROP_TIME
			 * function isn't used currently */
			fvalue = *((float *)&prop->data);
			ftest = (float)atof(str);
			if (fvalue > ftest) return 1;
			else if (fvalue < ftest) return -1;
			return 0;

		case GPROP_STRING:
			return strcmp(prop->poin, str);
	}
	
	return 0;
}
#endif

void BKE_bproperty_set(bProperty *prop, const char *str)
{
  //	extern int Gdfra;		/* sector.c */

  switch (prop->type) {
    case GPROP_BOOL:
      if (BLI_strcasecmp(str, "true") == 0)
        prop->data = 1;
      else if (BLI_strcasecmp(str, "false") == 0)
        prop->data = 0;
      else
        prop->data = (atoi(str) != 0);
      break;
    case GPROP_INT:
      prop->data = atoi(str);
      break;
    case GPROP_FLOAT:
    case GPROP_TIME:
      *((float *)&prop->data) = (float)atof(str);
      break;
    case GPROP_STRING:
      strcpy((char *)prop->poin, str); /* TODO - check size? */
      break;
  }
}

void BKE_bproperty_add(bProperty *prop, const char *str)
{
  //	extern int Gdfra;		/* sector.c */

  switch (prop->type) {
    case GPROP_BOOL:
    case GPROP_INT:
      prop->data += atoi(str);
      break;
    case GPROP_FLOAT:
    case GPROP_TIME:
      *((float *)&prop->data) += (float)atof(str);
      break;
    case GPROP_STRING:
      /* strcpy(prop->poin, str); */
      break;
  }
}

/* reads value of property, sets it in chars in str */
void BKE_bproperty_set_valstr(bProperty *prop, char str[MAX_PROPSTRING])
{
  //	extern int Gdfra;		/* sector.c */

  if (str == nullptr)
    return;

  switch (prop->type) {
    case GPROP_BOOL:
    case GPROP_INT:
      sprintf(str, "%d", prop->data);
      break;
    case GPROP_FLOAT:
    case GPROP_TIME:
      sprintf(str, "%f", *((float *)&prop->data));
      break;
    case GPROP_STRING:
      BLI_strncpy(str, (const char *)prop->poin, MAX_PROPSTRING);
      break;
  }
}

#if 0 /* UNUSED */
void cp_property(bProperty *prop1, bProperty *prop2)
{
	char str[128];

	BKE_bproperty_set_valstr(prop2, str);

	BKE_bproperty_set(prop1, str);
}
#endif

}  // namespace blender
