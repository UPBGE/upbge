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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Device/DEV_EventConsumer.cpp
 *  \ingroup device
 */

#include "DEV_EventConsumer.h"

#include "BLI_string_utf8.h"
#include "GHOST_ISystem.hh"

#include "DEV_InputDevice.h"
#include "RAS_ICanvas.h"

DEV_EventConsumer::DEV_EventConsumer(GHOST_ISystem *system,
                                     DEV_InputDevice *device,
                                     RAS_ICanvas *canvas)
    : m_device(device), m_canvas(canvas)
{
  // Setup the default mouse position.
  int cursorx, cursory;
  system->getCursorPosition(cursorx, cursory);
  int x, y;
  m_canvas->ConvertMousePosition(cursorx, cursory, x, y, true);
  m_device->ConvertMoveEvent(x, y);
}

DEV_EventConsumer::~DEV_EventConsumer()
{
}

void DEV_EventConsumer::HandleWindowEvent(GHOST_TEventType type)
{
  m_device->ConvertWindowEvent(type);
}

void DEV_EventConsumer::HandleKeyEvent(GHOST_TEventDataPtr data, bool down)
{
  GHOST_TEventKeyData *keyData = (GHOST_TEventKeyData *)data;
  unsigned int unicode = BLI_str_utf8_as_unicode_safe(keyData->utf8_buf);
  m_device->ConvertKeyEvent(keyData->key, down, unicode);
  // See d6fef73ef110eb43756b7b87c2cba80abae3b39f if issue
}

void DEV_EventConsumer::HandleCursorEvent(GHOST_TEventDataPtr data, GHOST_IWindow *window)
{
  GHOST_TEventCursorData *cursorData = (GHOST_TEventCursorData *)data;
  int x, y;
  m_canvas->ConvertMousePosition(cursorData->x, cursorData->y, x, y, false);

  m_device->ConvertMoveEvent(x, y);
}

void DEV_EventConsumer::HandleWheelEvent(GHOST_TEventDataPtr data)
{
  GHOST_TEventWheelData *wheelData = (GHOST_TEventWheelData *)data;

  m_device->ConvertWheelEvent(wheelData->z);
}

void DEV_EventConsumer::HandleButtonEvent(GHOST_TEventDataPtr data, bool down)
{
  GHOST_TEventButtonData *buttonData = (GHOST_TEventButtonData *)data;

  m_device->ConvertButtonEvent(buttonData->button, down);
}

bool DEV_EventConsumer::processEvent(const GHOST_IEvent *event)
{
  GHOST_TEventDataPtr eventData = ((GHOST_IEvent *)event)->getData();
  switch (event->getType()) {
    case GHOST_kEventButtonDown: {
      HandleButtonEvent(eventData, true);
      break;
    }

    case GHOST_kEventButtonUp: {
      HandleButtonEvent(eventData, false);
      break;
    }

    case GHOST_kEventWheel: {
      HandleWheelEvent(eventData);
      break;
    }

    case GHOST_kEventCursorMove: {
      HandleCursorEvent(eventData, event->getWindow());
      break;
    }

    case GHOST_kEventKeyDown: {
      HandleKeyEvent(eventData, true);
      break;
    }
    case GHOST_kEventKeyUp: {
      HandleKeyEvent(eventData, false);
      break;
    }
    case GHOST_kEventWindowSize:
    case GHOST_kEventWindowClose:
    case GHOST_kEventQuitRequest: {
      HandleWindowEvent(event->getType());
      break;
    }
    default:
      break;
  }

  return true;
}
