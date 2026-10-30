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

/** \file BL_ActionManager.cpp
 *  \ingroup ketsji
 */

#include "BL_ActionManager.h"

#include "BKE_lib_id.hh"

#include "BL_Action.h"

using namespace blender;


BL_ActionManager::BL_ActionManager(class KX_GameObject *obj) : m_obj(obj), m_suspended(false)
{
}

BL_ActionManager::~BL_ActionManager()
{
  for (const std::pair<const short, std::shared_ptr<BL_ActionPlayback>> &playback : m_playbacks) {
    if (playback.second != nullptr &&
        playback.second->m_status == BL_ActionPlaybackStatus::Playing)
    {
      playback.second->m_status = BL_ActionPlaybackStatus::Interrupted;
    }
  }
  m_playbacks.clear();

  BL_ActionMap::iterator it;

  for (it = m_layers.begin(); it != m_layers.end(); it++)
    delete it->second;

  m_layers.clear();
}

void BL_ActionManager::FinishPlayback(const short layer, const BL_ActionPlaybackStatus status)
{
  const auto item = m_playbacks.find(layer);
  if (item == m_playbacks.end() || item->second == nullptr ||
      item->second->m_status != BL_ActionPlaybackStatus::Playing)
  {
    return;
  }
  item->second->m_status = status;
}

BL_Action *BL_ActionManager::GetAction(short layer)
{
  BL_ActionMap::iterator it = m_layers.find(layer);

  return (it != m_layers.end()) ? it->second : 0;
}

float BL_ActionManager::GetActionFrame(short layer)
{
  BL_Action *action = GetAction(layer);

  return action ? action->GetFrame() : 0.f;
}

const std::string BL_ActionManager::GetActionName(short layer)
{
  BL_Action *action = GetAction(layer);
  return action ? action->GetName() : "";
}

void BL_ActionManager::SetActionFrame(short layer, float frame)
{
  BL_Action *action = GetAction(layer);

  if (action)
    action->SetFrame(frame);
}

blender::bAction *BL_ActionManager::GetCurrentAction(short layer)
{
  BL_Action *action = GetAction(layer);

  return action ? action->GetAction() : 0;
}

void BL_ActionManager::SetPlayMode(short layer, short mode)
{
  BL_Action *action = GetAction(layer);

  if (action)
    action->SetPlayMode(mode);
}

bool BL_ActionManager::PlayAction(const std::string &name,
                                  float start,
                                  float end,
                                  short layer,
                                  short priority,
                                  float blendin,
                                  short play_mode,
                                  float layer_weight,
                                  short ipo_flags,
                                  float playback_speed,
                                  short blend_mode)
{
  return PlayActionInternal(name,
                            start,
                            end,
                            layer,
                            priority,
                            blendin,
                            play_mode,
                            layer_weight,
                            ipo_flags,
                            playback_speed,
                            blend_mode,
                            false)
      .started;
}

BL_ActionPlayResult BL_ActionManager::PlayActionTracked(const std::string &name,
                                                        float start,
                                                        float end,
                                                        short layer,
                                                        short priority,
                                                        float blendin,
                                                        short play_mode,
                                                        float layer_weight,
                                                        short ipo_flags,
                                                        float playback_speed,
                                                        short blend_mode)
{
  return PlayActionInternal(name,
                            start,
                            end,
                            layer,
                            priority,
                            blendin,
                            play_mode,
                            layer_weight,
                            ipo_flags,
                            playback_speed,
                            blend_mode,
                            true);
}

BL_ActionPlayResult BL_ActionManager::PlayActionInternal(const std::string &name,
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
                                                         const bool track_playback)
{
  BL_ActionPlayResult result;

  // Only this method will create layer if non-existent
  BL_Action *action = GetAction(layer);
  if (!action) {
    action = new BL_Action(m_obj);
    m_layers[layer] = action;
  }

  // Disable layer blending on the first layer
  if (layer == 0)
    layer_weight = -1.f;

  const bool previous_done = action->IsDone();
  result.started = action->Play(name,
                                start,
                                end,
                                priority,
                                blendin,
                                play_mode,
                                layer_weight,
                                ipo_flags,
                                playback_speed,
                                blend_mode);
  if (!result.started) {
    /* Priority and duplicate rejections leave the previous playback alive. A missing action can
     * transition BL_Action to done while returning false; that request interrupted the previous
     * playback even though it failed to establish a replacement. */
    if (!previous_done && action->IsDone()) {
      FinishPlayback(layer, BL_ActionPlaybackStatus::Interrupted);
      m_playbacks.erase(layer);
    }
    return result;
  }

  FinishPlayback(layer,
                 previous_done ? BL_ActionPlaybackStatus::Completed :
                                 BL_ActionPlaybackStatus::Interrupted);
  m_playbacks.erase(layer);

  if (!track_playback) {
    return result;
  }

  /* Zero is reserved as an invalid identity. Exhaustion is practically unreachable, but skipping
   * it makes the token contract total even after unsigned wraparound. */
  if (m_nextPlaybackId == 0) {
    m_nextPlaybackId = 1;
  }
  std::shared_ptr<BL_ActionPlayback> playback(
      new BL_ActionPlayback(m_nextPlaybackId++, layer));
  m_playbacks[layer] = playback;
  result.playback = std::move(playback);
  return result;
}

void BL_ActionManager::StopAction(short layer)
{
  BL_Action *action = GetAction(layer);

  if (action) {
    FinishPlayback(layer,
                   action->IsDone() ? BL_ActionPlaybackStatus::Completed :
                                      BL_ActionPlaybackStatus::Interrupted);
    m_playbacks.erase(layer);
    m_layers.erase(layer);
    delete action;
  }
}

void BL_ActionManager::RemoveTaggedActions()
{
  for (BL_ActionMap::iterator it = m_layers.begin(); it != m_layers.end();) {
    if (IS_TAGGED(it->second->GetAction())) {
      const short layer = it->first;
      FinishPlayback(layer,
                     it->second->IsDone() ? BL_ActionPlaybackStatus::Completed :
                                            BL_ActionPlaybackStatus::Interrupted);
      m_playbacks.erase(layer);
      delete it->second;
      it = m_layers.erase(it);
    }
    else
      ++it;
  }
}

bool BL_ActionManager::IsActionDone(short layer)
{
  BL_Action *action = GetAction(layer);

  return action ? action->IsDone() : true;
}

void BL_ActionManager::Suspend()
{
  m_suspended = true;
}

void BL_ActionManager::Resume()
{
  m_suspended = false;
}

bool BL_ActionManager::IsSuspended() const
{
  return m_suspended;
}

void BL_ActionManager::Update(float curtime, bool applyToObject)
{
  for (const auto &pair : m_layers) {
    const bool was_done = pair.second->IsDone();
    pair.second->Update(curtime, applyToObject);
    if (!was_done && pair.second->IsDone()) {
      FinishPlayback(pair.first, BL_ActionPlaybackStatus::Completed);
    }
  }
  /* It's to sync children with parent SGNode after fcurve update */
  for (const auto &pair : m_layers) {
    pair.second->UpdateIPOs();
  }
}

void BL_ActionManager::RebaseTime(const float previous_time,
                                  const float current_time,
                                  const bool use_pause_clock)
{
  for (const std::pair<const short, BL_Action *> &layer : m_layers) {
    if (layer.second != nullptr) {
      layer.second->RebaseTime(previous_time, current_time, use_pause_clock);
    }
  }
}
