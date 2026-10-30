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

/** \file gameengine/Converter/BL_ConvertProperties.cpp
 *  \ingroup bgeconv
 */

#include "BL_ConvertProperties.h"

#include <sstream>

#include "BKE_game_persistence.hh"
#include "BKE_property.hh"
#include "BLI_listbase.h"
#include "BLI_utildefines.h"
#include "DNA_curve_types.h"

#include "CM_Message.h"
#include "EXP_ArrayValue.h"
#include "EXP_DictValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"
#include "EXP_Vector3Value.h"
#include "EXP_Vector4Value.h"
#include "KX_FontObject.h"
#include "KX_ObjectReferenceValue.h"
#include "SCA_TimeEventManager.h"

using namespace blender;

/* prototype */
void BL_ConvertTextProperty(blender::Object *object,
                            KX_FontObject *fontobj,
                            SCA_TimeEventManager *timemgr,
                            SCA_IScene *scene,
                            bool isInActiveLayer);

/** The game value of a property, also of List items and Table cells; nullptr for Timers. */
static EXP_Value *convert_property_value(const bProperty *prop)
{
  switch (prop->type) {
    case GPROP_BOOL:
      return new EXP_BoolValue(prop->data != 0);
    case GPROP_INT:
      return new EXP_IntValue(prop->data);
    case GPROP_FLOAT:
      return new EXP_FloatValue(*((const float *)&prop->data));
    case GPROP_STRING:
      return new EXP_StringValue(prop->poin ? static_cast<const char *>(prop->poin) : "", "");
    case GPROP_VECTOR:
      return new EXP_Vector3Value(MT_Vector3(prop->vec));
    case GPROP_COLOR:
      return new EXP_Vector4Value(MT_Vector4(prop->vec));
    case GPROP_OBJECT:
      return new KX_ObjectReferenceValue(prop->object);
    case GPROP_CHOICE: {
      /* The game sees the name of the selected option. */
      const bProperty *option = (const bProperty *)BLI_findlink(&prop->items, prop->data);
      return new EXP_StringValue(option ? option->name : "", "");
    }
    case GPROP_LIST: {
      EXP_ArrayValue *list = new EXP_ArrayValue();
      for (const bProperty *item = (const bProperty *)prop->items.first; item; item = item->next) {
        if (EXP_Value *value = convert_property_value(item)) {
          list->Append(value);
        }
      }
      return list;
    }
    case GPROP_TABLE: {
      /* A list with a dictionary per row, from column name to cell value. */
      EXP_ArrayValue *table = new EXP_ArrayValue();
      for (const bProperty *row = (const bProperty *)prop->items.first; row; row = row->next) {
        EXP_DictValue *entries = new EXP_DictValue();
        const bProperty *cell = (const bProperty *)row->items.first;
        for (const bProperty *column = (const bProperty *)prop->columns.first; column && cell;
             column = column->next, cell = cell->next)
        {
          if (EXP_Value *value = convert_property_value(cell)) {
            entries->Set(column->name, value);
          }
        }
        table->Append(entries);
      }
      return table;
    }
    default:
      return nullptr;
  }
}

void BL_ResolvePropertyObjects(blender::Object *object,
                               KX_GameObject *gameobj,
                               BL_SceneConverter &converter)
{
  for (const bProperty *prop = (const bProperty *)object->prop.first; prop; prop = prop->next) {
    if (ELEM(prop->type, GPROP_OBJECT, GPROP_LIST, GPROP_TABLE)) {
      if (EXP_Value *value = gameobj->GetProperty(prop->name)) {
        KX_ResolveObjectReferences(value, converter);
      }
    }
  }
}

void BL_ConvertProperties(blender::Object *object,
                          KX_GameObject *gameobj,
                          SCA_TimeEventManager *timemgr,
                          SCA_IScene *scene,
                          bool isInActiveLayer)
{

  bUUID persistence_uuid;
  BKE_game_persistence_identity_read(object->id, persistence_uuid);
  gameobj->SetPersistenceSourceUuid(persistence_uuid);
  gameobj->SetSaveGameObjectState((object->gameflag & OB_SAVE_GAME_OBJECT_STATE) != 0);

  bProperty *prop = (bProperty *)object->prop.first;
  EXP_Value *propval;
  bool show_debug_info;

  while (prop) {
    propval = nullptr;
    show_debug_info = bool(prop->flag & PROP_DEBUG);

    switch (prop->type) {
      case GPROP_TIME: {
        float floatprop = *((float *)&prop->data);

        EXP_Value *timeval = new EXP_FloatValue(floatprop);
        // set a subproperty called 'timer' so that
        // we can register the replica of this property
        // at the time a game object is replicated (AddObjectActuator triggers this)
        EXP_Value *bval = new EXP_BoolValue(true);
        timeval->SetProperty("timer", bval);
        bval->Release();
        if (isInActiveLayer) {
          timemgr->AddTimeProperty(timeval);
        }

        propval = timeval;
        gameobj->SetProperty(prop->name, timeval);
        break;
      }
      default: {
        propval = convert_property_value(prop);
        if (propval) {
          gameobj->SetProperty(prop->name, propval);
        }
        /* An inventory follows the scene's Inventory Rules it picks. */
        if (ELEM(prop->type, GPROP_LIST, GPROP_TABLE) && prop->inventory_rules[0] != '\0') {
          gameobj->SetInventoryRuleSet(prop->name, prop->inventory_rules);
        }
        break;
      }
    }

    if (propval) {
      if (prop->flag & PROP_SAVE_GAME) {
        KX_GameObject::SaveGamePropertyMetadata::Type type;
        bool timer = false;
        switch (prop->type) {
          case GPROP_BOOL:
            type = KX_GameObject::SaveGamePropertyMetadata::Type::Bool;
            break;
          case GPROP_INT:
            type = KX_GameObject::SaveGamePropertyMetadata::Type::Int;
            break;
          case GPROP_FLOAT:
            type = KX_GameObject::SaveGamePropertyMetadata::Type::Float;
            break;
          case GPROP_TIME:
            type = KX_GameObject::SaveGamePropertyMetadata::Type::Float;
            timer = true;
            break;
          case GPROP_VECTOR:
            type = KX_GameObject::SaveGamePropertyMetadata::Type::Vector3;
            break;
          case GPROP_COLOR:
            type = KX_GameObject::SaveGamePropertyMetadata::Type::Vector4;
            break;
          case GPROP_OBJECT:
            type = KX_GameObject::SaveGamePropertyMetadata::Type::Object;
            break;
          case GPROP_LIST:
          case GPROP_TABLE:
            type = KX_GameObject::SaveGamePropertyMetadata::Type::List;
            break;
          case GPROP_STRING:
          case GPROP_CHOICE:
          default:
            type = KX_GameObject::SaveGamePropertyMetadata::Type::String;
            break;
        }
        gameobj->AddSaveGameProperty(prop->name, type, timer);
      }
      if (show_debug_info && isInActiveLayer) {
        scene->AddDebugProperty(gameobj, prop->name);
      }
      // done with propval, release it
      propval->Release();
    }

#ifdef WITH_PYTHON
    /* Warn if we double up on attributes, this isn't quite right since it wont find inherited
     * attributes however there arnt many */
    for (PyAttributeDef *attrdef = KX_GameObject::Attributes; !attrdef->m_name.empty();
         attrdef++) {
      if (prop->name == attrdef->m_name) {
        CM_Warning("user defined property name \""
                   << prop->name << "\" is also a python attribute for object \""
                   << object->id.name + 2 << "\". Use ob[\"" << prop->name
                   << "\"] syntax to avoid conflict");
        break;
      }
    }
    for (PyMethodDef *methdef = KX_GameObject::Methods; methdef->ml_name; methdef++) {
      if (strcmp(prop->name, methdef->ml_name) == 0) {
        CM_Warning("user defined property name \""
                   << prop->name << "\" is also a python method for object \""
                   << object->id.name + 2 << "\". Use ob[\"" << prop->name
                   << "\"] syntax to avoid conflict");
        break;
      }
    }
    /* end warning check */
#endif  // WITH_PYTHON

    prop = prop->next;
  }
  // check if state needs to be debugged
  if (object->scaflag & OB_DEBUGSTATE && isInActiveLayer) {
    //  reserve name for object state
    scene->AddDebugProperty(gameobj, "__state__");
  }

  /* Font Objects need to 'copy' the Font blender::Object data body to ["Text"] */
  if (object->type == OB_FONT) {
    BL_ConvertTextProperty(object, (KX_FontObject *)gameobj, timemgr, scene, isInActiveLayer);
  }
}

void BL_ConvertTextProperty(blender::Object *object,
                            KX_FontObject *fontobj,
                            SCA_TimeEventManager *timemgr,
                            SCA_IScene *scene,
                            bool isInActiveLayer)
{
  EXP_Value *tprop = fontobj->GetProperty("Text");
  if (!tprop)
    return;
  bProperty *prop = BKE_bproperty_object_get(object, "Text");
  if (!prop)
    return;

  blender::Curve *curve = id_cast<blender::Curve *>(object->data);
  const std::string str = curve->str;
  std::stringstream stream(str);
  EXP_Value *propval = nullptr;

  switch (prop->type) {
    case GPROP_BOOL: {
      bool value;
      stream >> value;
      propval = new EXP_BoolValue(value != 0);
      break;
    }
    case GPROP_INT: {
      int value;
      stream >> value;
      propval = new EXP_IntValue(value);
      break;
    }
    case GPROP_FLOAT: {
      float floatprop;
      stream >> floatprop;
      propval = new EXP_FloatValue(floatprop);
      break;
    }
    case GPROP_STRING: {
      std::string textprop;
      stream >> textprop;
      propval = new EXP_StringValue(str, textprop.c_str());
      break;
    }
    case GPROP_TIME: {
      float floatprop;
      stream >> floatprop;

      propval = new EXP_FloatValue(floatprop);
      // set a subproperty called 'timer' so that
      // we can register the replica of this property
      // at the time a game object is replicated (AddObjectActuator triggers this)
      EXP_Value *bval = new EXP_BoolValue(true);
      propval->SetProperty("timer", bval);
      bval->Release();
      if (isInActiveLayer) {
        timemgr->AddTimeProperty(propval);
      }
      break;
    }
    default: {
      BLI_assert(0);
    }
  }

  /* check stream integrity */
  if (stream.bad()) {
    CM_Error("Failed to convert font property \"Text\"");
  }

  if (propval) {
    tprop->SetValue(propval);
    propval->Release();
  }
}
