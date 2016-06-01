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

/** \file GH_EventConsumer.h
 *  \ingroup ghost
 */

#ifndef __GH_EVENTCONSUMER_H__
#define __GH_EVENTCONSUMER_H__

#include "GHOST_IEventConsumer.h"

class GH_InputDevice;

class RAS_ICanvas;

class GH_EventConsumer : public GHOST_IEventConsumer
{
private:
	GH_InputDevice *m_device;
	RAS_ICanvas *m_canvas;

	void HandleWindowEvent(GHOST_TEventType type);
	void HandleKeyEvent(GHOST_TEventDataPtr data, bool down);
	void HandleCursorEvent(GHOST_TEventDataPtr data, GHOST_IWindow *window);
	void HandleWheelEvent(GHOST_TEventDataPtr data);
	void HandleButtonEvent(GHOST_TEventDataPtr data, bool down);

public:
	GH_EventConsumer(GH_InputDevice *device, RAS_ICanvas *canvas);
	virtual ~GH_EventConsumer();

	virtual bool processEvent(GHOST_IEvent *event);
};

#endif  // __GH_EVENTCONSUMER_H__
