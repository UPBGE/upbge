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

/** \file gameengine/GamePlayer/common/GH_EventConsumer.cpp
 *  \ingroup player
 */


#include "GH_EventConsumer.h"
#include "GH_InputDevice.h"

#include "GHOST_IEvent.h"
#include "GHOST_IWindow.h"

#include "BLI_string_utf8.h"

#include <iostream>

GH_EventConsumer::GH_EventConsumer(GH_InputDevice *device)
	:m_device(device)
{
}

GH_EventConsumer::~GH_EventConsumer()
{
}

void GH_EventConsumer::HandleKeyEvent(GHOST_TEventDataPtr data, bool down)
{
	GHOST_TEventKeyData *keyData = (GHOST_TEventKeyData *)data;
	unsigned int unicode = keyData->utf8_buf[0] ? BLI_str_utf8_as_unicode(keyData->utf8_buf) : keyData->ascii;

	m_device->ConvertEvent(keyData->key, down, unicode);
}

void GH_EventConsumer::HandleCursorEvent(GHOST_TEventDataPtr data, GHOST_IWindow *window)
{
	GHOST_TEventCursorData *cursorData = (GHOST_TEventCursorData *)data;
	GHOST_TInt32 x, y;
	window->screenToClient(cursorData->x, cursorData->y, x, y);

	m_device->ConvertMoveEvent(x, y);
}

void GH_EventConsumer::HandleButtonEvent(GHOST_TEventDataPtr data, bool down)
{
	GHOST_TEventButtonData *buttonData = (GHOST_TEventButtonData *)data;

	m_device->ConvertEvent(buttonData->button, down, 0);
}

bool GH_EventConsumer::processEvent(GHOST_IEvent *event)
{
	GHOST_TEventDataPtr eventData = ((GHOST_IEvent*)event)->getData();
	switch (event->getType()) {
		case GHOST_kEventButtonDown:
			HandleButtonEvent(eventData, true);
			break;

		case GHOST_kEventButtonUp:
			HandleButtonEvent(eventData, false);
			break;

		case GHOST_kEventWheel:
			/* TODO
			 	bool handled = false;
	BLI_assert(event);
	if (m_mouse) 
	{
		GHOST_TEventDataPtr eventData = ((GHOST_IEvent*)event)->getData();
		GHOST_TEventWheelData* wheelData = static_cast<GHOST_TEventWheelData*>(eventData);
		GPC_MouseDevice::TButtonId button;
		if (wheelData->z > 0)
			button = GPC_MouseDevice::buttonWheelUp;
		else
			button = GPC_MouseDevice::buttonWheelDown;
		m_mouse->ConvertButtonEvent(button, true);
		handled = true;
	}*/
			break;

		case GHOST_kEventCursorMove:
		{
			HandleCursorEvent(eventData, event->getWindow());
			break;
		}

		case GHOST_kEventKeyDown:
		{
			HandleKeyEvent(eventData, true);
			break;
		}
		case GHOST_kEventKeyUp:
		{
			HandleKeyEvent(eventData, false);
			break;
		}
		/*case GHOST_kEventWindowSize:
			{
			GHOST_IWindow* window = event->getWindow();
			if (!m_system->validWindow(window)) break;
			if (m_canvas) {
				GHOST_Rect bnds;
				window->getClientBounds(bnds);
				m_canvas->Resize(bnds.getWidth(), bnds.getHeight());
				m_ketsjiengine->Resize();
			}
			}
			break;*/
		/*case GHOST_kEventWindowClose:
		case GHOST_kEventQuit:
			m_exitRequested = KX_EXIT_REQUEST_OUTSIDE;
			break;*/
		default:
			break;
	}

	return true;
}
