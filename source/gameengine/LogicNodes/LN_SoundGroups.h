/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Volume groups: Music, Effects, Voice, Ambience, UI and Master. Set Volume changes a group's
 * volume at once or over a fade; every sound of the group follows, also one already playing.
 * This is the arithmetic only: LN_CommandBuffer applies the volumes to the playing sounds.
 */

#pragma once

#include <array>
#include <cstdint>

#include "LN_Types.h"

/** The group a node setting names: out-of-range values are Effects, and Master only where
 * `allow_master` (Set Volume; a sound is never in Master). */
LN_SoundGroup LN_SoundGroupFromInt(int32_t value, bool allow_master);

/** The name players see in the editor ("Music"). */
const char *LN_SoundGroupName(LN_SoundGroup group);

/** A game starts or ends (LA_Launcher): every group back to full volume, the audio device's
 * volume as it was before the game (LN_CommandBuffer::ResetSoundGroups). */
void LN_ResetSoundGroups();

class LN_SoundGroupVolumes {
 public:
  /** Starts a change of `group`'s volume at `now` (seconds): at once when `fade` is 0 (or less),
   * else over `fade` seconds from the volume it has now (also halfway through another fade).
   * A negative or invalid volume is 0. */
  void Set(LN_SoundGroup group, float volume, float fade, double now);
  /** Moves the fading groups on to `now`. True when a volume changed. */
  bool Advance(double now);
  /** The group's volume now: 1 until Set changes it. */
  float Volume(LN_SoundGroup group) const;
  /** Some group is still fading. */
  bool Fading() const;
  /** Every group back to 1 (a new game). */
  void Reset();

 private:
  struct Group {
    float volume = 1.0f;
    float from = 1.0f;
    float to = 1.0f;
    double start = 0.0;
    double duration = 0.0;
    bool fading = false;
  };
  /** Moves one fading group on to `now`; true when its volume changed. */
  static bool Step(Group &group, double now);

  std::array<Group, LN_SOUND_GROUP_COUNT> m_groups;
};
