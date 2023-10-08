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

/** \file DEV_EventConsumer.h
 *  \ingroup device
 */

#pragma once

#include "GHOST_IEventConsumer.hh"

class DEV_InputDevice;
class GHOST_ISystem;

class RAS_ICanvas;

class DEV_EventConsumer : public GHOST_IEventConsumer {
 private:
  DEV_InputDevice *m_device;
  RAS_ICanvas *m_canvas;

  void HandleWindowEvent(GHOST_TEventType type);
  void HandleKeyEvent(GHOST_TEventDataPtr data, bool down);
  void HandleCursorEvent(GHOST_TEventDataPtr data, GHOST_IWindow *window);
  void HandleWheelEvent(GHOST_TEventDataPtr data);
  void HandleButtonEvent(GHOST_TEventDataPtr data, bool down);

 public:
  DEV_EventConsumer(GHOST_ISystem *system, DEV_InputDevice *device, RAS_ICanvas *canvas);
  virtual ~DEV_EventConsumer();

  /// Function called by GHOST to process all events.
  virtual bool processEvent(const GHOST_IEvent *event);
};
