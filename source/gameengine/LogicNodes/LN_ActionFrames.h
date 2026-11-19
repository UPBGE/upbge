/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * On Action Frame (2026-11-16): whether an action layer passed a frame between two reads of the
 * layer, one a Logic + Physics step (Bool::ActionFrameReached). BL_Action plays the frames from
 * its start toward its end frame; Loop puts the frame back on the start frame at the end, Ping
 * Pong swaps the start and end frame there; a non-looping action stops on its end frame.
 */

#pragma once

#include <cstdint>
#include <string>

/** Play Action's and Set Action Layer's Layer Weight as BL_Action takes it (2026-11-16): above 0
 * (at most 1) how much of the layers below shows; 0 (or not a number): -1, the layer covers them
 * (every Play Action before the input). */
inline float LN_ActionLayerWeight(const float weight)
{
  return weight > 0.0f && weight == weight ? (weight < 1.0f ? weight : 1.0f) : -1.0f;
}

/** A move back by less than this many frames is rounding, not a loop or a restart (BL_Action
 * works its frame out of float times: a start time set again, a clock rebased). */
inline constexpr float LN_ACTION_FRAME_JITTER = 0.01f;

/** BL_Action's play modes (ACT_MODE_PLAY / LOOP / PING_PONG). */
enum class LN_ActionPlayMode : int16_t { Play = 0, Loop = 1, PingPong = 2 };

/** One read of an action layer. */
struct LN_ActionLayerRead {
  std::string action;
  float frame = 0.0f;
  float start = 0.0f;
  float end = 0.0f;
  int16_t play_mode = 0;
};

/** On Action Frame's state in one tree (LN_RuntimeTree::ActionFrameState): the layer as the step
 * before read it. */
struct LN_ActionFrameState {
  uint64_t last_update_tick = UINT64_MAX;
  bool has_previous = false;
  bool result = false;
  LN_ActionLayerRead previous;
};

/** Whether `frame` lies on the way from `from` to `to`: `to` included, `from` only when
 * `include_from` (the frames go either way). */
inline bool LN_ActionFramePassed(const float from,
                                 const float to,
                                 const float frame,
                                 const bool include_from)
{
  if (to >= from) {
    return (include_from ? frame >= from : frame > from) && frame <= to;
  }
  return (include_from ? frame <= from : frame < from) && frame >= to;
}

/** Whether the layer passed `frame` from `previous` (none: the action just started, or another
 * one) to `current`:
 * - the first read of an action: from its start frame to the frame now, the start included;
 * - frames on the way from start toward end: from the previous frame to this one;
 * - Ping Pong turned (start and end swapped): on to the old end, then back to this frame;
 * - a frame against the way: Loop wrapped (on to the end, then from the start, included);
 *   otherwise the action was started again (from its start frame, included); less than
 *   LN_ACTION_FRAME_JITTER back is no move. */
inline bool LN_ActionFrameCrossed(const LN_ActionLayerRead *previous,
                                  const LN_ActionLayerRead &current,
                                  const float frame)
{
  if (previous == nullptr || previous->action != current.action) {
    return LN_ActionFramePassed(current.start, current.frame, frame, true);
  }
  const float p = previous->frame;
  const float c = current.frame;
  if (LN_ActionPlayMode(current.play_mode) == LN_ActionPlayMode::PingPong &&
      previous->start == current.end && previous->end == current.start &&
      current.start != current.end)
  {
    /* Turned at the old end (the new start). */
    return LN_ActionFramePassed(p, previous->end, frame, false) ||
           LN_ActionFramePassed(current.start, c, frame, false);
  }
  const float way = current.end >= current.start ? 1.0f : -1.0f;
  const float moved = (c - p) * way;
  if (moved > 0.0f) {
    return LN_ActionFramePassed(p, c, frame, false);
  }
  if (!(moved < -LN_ACTION_FRAME_JITTER)) {
    return false;
  }
  if (LN_ActionPlayMode(current.play_mode) == LN_ActionPlayMode::Loop) {
    return LN_ActionFramePassed(p, current.end, frame, false) ||
           LN_ActionFramePassed(current.start, c, frame, true);
  }
  return LN_ActionFramePassed(current.start, c, frame, true);
}
