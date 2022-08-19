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
 * Contributor(s): Mitchell Stokes.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file BL_ActionManager.h
 *  \ingroup ketsji
 */

#pragma once

#include <iostream>
#include <map>

// Currently, we use the max value of a short.
// We should switch to unsigned short; doesn't make sense to support negative layers.
// This will also give us 64k layers instead of 32k.
#define MAX_ACTION_LAYERS 32767

class BL_Action;

/**
 * BL_ActionManager is responsible for handling a KX_GameObject's actions.
 */
class BL_ActionManager {
 private:
  typedef std::map<short, BL_Action *> BL_ActionMap;

  class KX_GameObject *m_obj;
  BL_ActionMap m_layers;

  // Suspend action update?
  bool m_suspended;

  /**
   * Check if an action exists
   */
  BL_Action *GetAction(short layer);

 public:
  BL_ActionManager(class KX_GameObject *obj);
  ~BL_ActionManager();

  bool PlayAction(const std::string &name,
                  float start,
                  float end,
                  short layer = 0,
                  short priority = 0,
                  float blendin = 0.f,
                  short play_mode = 0,
                  float layer_weight = 0.f,
                  short ipo_flags = 0,
                  float playback_speed = 1.f,
                  short blend_mode = 0);
  /**
   * Gets the current frame of an action
   */
  float GetActionFrame(short layer);

  /**
   * Gets the name of the current action
   */
  const std::string GetActionName(short layer);

  /**
   * Sets the current frame of an action
   */
  void SetActionFrame(short layer, float frame);

  /**
   * Gets the currently running action on the given layer
   */
  struct bAction *GetCurrentAction(short layer);

  /**
   * Sets play mode of the action on the given layer
   */
  void SetPlayMode(short layer, short mode);

  /**
   * Stop playing the action on the given layer
   */
  void StopAction(short layer);

  /**
   * Remove playing tagged actions.
   */
  void RemoveTaggedActions();

  /**
   * Check if an action has finished playing
   */
  bool IsActionDone(short layer);

  void Suspend();
  void Resume();
  bool IsSuspended() const;

  /**
   * Update any running actions
   * \param curtime The current time used to compute the actions' frame.
   * \param applyToObject Set to true if the actions must transform the object, else it only
   * manages actions' frames.
   */
  void Update(float curtime, bool applyToObject);
};
