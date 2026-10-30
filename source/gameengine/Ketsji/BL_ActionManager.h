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

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>

#include "DNA_action_types.h"

// Currently, we use the max value of a short.
// We should switch to unsigned short; doesn't make sense to support negative layers.
// This will also give us 64k layers instead of 32k.
#define MAX_ACTION_LAYERS 32767

class BL_Action;

enum class BL_ActionPlaybackStatus : uint8_t {
  Playing = 0,
  Completed,
  Interrupted,
};

/**
 * Stable identity and terminal state for one accepted action playback.
 *
 * The action manager owns the mutable instance while the playback is current. Deferred systems
 * retain a const shared reference, allowing the exact request to outlive replacement, stopping,
 * or destruction of the action layer without retaining the game object itself.
 */
class BL_ActionPlayback {
 public:
  uint64_t GetId() const
  {
    return m_id;
  }

  short GetLayer() const
  {
    return m_layer;
  }

  BL_ActionPlaybackStatus GetStatus() const
  {
    return m_status;
  }

 private:
  BL_ActionPlayback(uint64_t id, short layer) : m_id(id), m_layer(layer)
  {
  }

  uint64_t m_id = 0;
  short m_layer = 0;
  BL_ActionPlaybackStatus m_status = BL_ActionPlaybackStatus::Playing;

  friend class BL_ActionManager;
};

struct BL_ActionPlayResult {
  bool started = false;
  std::shared_ptr<const BL_ActionPlayback> playback;
};

/**
 * BL_ActionManager is responsible for handling a KX_GameObject's actions.
 */
class BL_ActionManager {
 private:
  typedef std::map<short, BL_Action *> BL_ActionMap;

  class KX_GameObject *m_obj;
  BL_ActionMap m_layers;
  std::map<short, std::shared_ptr<BL_ActionPlayback>> m_playbacks;
  uint64_t m_nextPlaybackId = 1;

  // Suspend action update?
  bool m_suspended;

  /**
   * Check if an action exists
   */
  BL_Action *GetAction(short layer);
  void FinishPlayback(short layer, BL_ActionPlaybackStatus status);
  BL_ActionPlayResult PlayActionInternal(const std::string &name,
                                         float start,
                                         float end,
                                         short layer,
                                         short priority,
                                         float blendin,
                                         short play_mode,
                                         float layer_weight,
                                         short ipo_flags,
                                         float playback_speed,
                                         short blend_mode,
                                         bool track_playback);

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
  /** Play an action and return an exact lifecycle token when the request is accepted. */
  BL_ActionPlayResult PlayActionTracked(const std::string &name,
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
  blender::bAction *GetCurrentAction(short layer);

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
  void RebaseTime(float previous_time, float current_time, bool use_pause_clock);
};
