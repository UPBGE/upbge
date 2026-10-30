/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 */

#include "LN_SoundGroups.h"

#include <algorithm>
#include <cmath>

LN_SoundGroup LN_SoundGroupFromInt(const int32_t value, const bool allow_master)
{
  const int32_t last = allow_master ? int32_t(LN_SoundGroup::Master) : int32_t(LN_SoundGroup::UI);
  return value >= 0 && value <= last ? LN_SoundGroup(value) : LN_SoundGroup::Effects;
}

const char *LN_SoundGroupName(const LN_SoundGroup group)
{
  switch (group) {
    case LN_SoundGroup::Effects:
      return "Effects";
    case LN_SoundGroup::Music:
      return "Music";
    case LN_SoundGroup::Voice:
      return "Voice";
    case LN_SoundGroup::Ambience:
      return "Ambience";
    case LN_SoundGroup::UI:
      return "UI";
    case LN_SoundGroup::Master:
      return "Master";
  }
  return "Effects";
}

bool LN_SoundGroupVolumes::Step(Group &group, const double now)
{
  if (!group.fading) {
    return false;
  }
  const double progress = group.duration > 0.0 ?
                              std::clamp((now - group.start) / group.duration, 0.0, 1.0) :
                              1.0;
  const float volume = progress >= 1.0 ?
                           group.to :
                           group.from + (group.to - group.from) * float(progress);
  group.fading = progress < 1.0;
  const bool changed = volume != group.volume;
  group.volume = volume;
  return changed;
}

void LN_SoundGroupVolumes::Set(const LN_SoundGroup group,
                               const float volume,
                               const float fade,
                               const double now)
{
  Group &target = m_groups[size_t(LN_SoundGroupFromInt(int32_t(group), true))];
  /* Halfway through a fade, the new one starts where the volume is now. */
  Step(target, now);
  const float wanted = std::isfinite(volume) ? std::max(volume, 0.0f) : 0.0f;
  if (!(fade > 0.0f) || !std::isfinite(fade)) {
    target.volume = wanted;
    target.fading = false;
    return;
  }
  target.from = target.volume;
  target.to = wanted;
  target.start = now;
  target.duration = double(fade);
  target.fading = true;
}

bool LN_SoundGroupVolumes::Advance(const double now)
{
  bool changed = false;
  for (Group &group : m_groups) {
    changed |= Step(group, now);
  }
  return changed;
}

float LN_SoundGroupVolumes::Volume(const LN_SoundGroup group) const
{
  return m_groups[size_t(LN_SoundGroupFromInt(int32_t(group), true))].volume;
}

bool LN_SoundGroupVolumes::Fading() const
{
  return std::any_of(
      m_groups.begin(), m_groups.end(), [](const Group &group) { return group.fading; });
}

void LN_SoundGroupVolumes::Reset()
{
  m_groups = {};
}
