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

/** \file GH_EventConsumer.h
 *  \ingroup player
 */

#ifndef __GH_EVENTCONSUMER_H__
#define __GH_EVENTCONSUMER_H__

#include "GHOST_IEventConsumer.h"

class GH_InputDevice;

class GH_EventConsumer : public GHOST_IEventConsumer
{
private:
	GH_InputDevice *m_device;

	void HandleKeyEvent(GHOST_TEventDataPtr data, bool down);
	void HandleCursorEvent(GHOST_TEventDataPtr data, GHOST_IWindow *window);
	void HandleWheelEvent(GHOST_TEventDataPtr data);
	void HandleButtonEvent(GHOST_TEventDataPtr data, bool down);

public:
	GH_EventConsumer(GH_InputDevice *device);
	virtual ~GH_EventConsumer();

	virtual bool processEvent(GHOST_IEvent *event);
};

#endif  // __GH_EVENTCONSUMER_H__
