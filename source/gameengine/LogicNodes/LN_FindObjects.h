/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Find Objects (2026-11-16): the nearest, farthest, a random one or all of a set of objects
 * around a center, within a radius and a view cone. Every reader of one tick sees the objects
 * where they were when the tick started: LN_FindIndex holds their poses and a uniform grid for
 * each source the tick's programs search (all objects, a collection's, the objects with a game
 * property), made on the main thread before any tree records and only read while they do, so
 * the search is the same on any thread and in any order (wide runs, column passes, warm-ups on
 * the job pool). A List source is searched object by object through the same poses.
 */

#pragma once

#include "MT_Vector3.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class KX_GameObject;

/** Find Objects' Source (custom1). */
enum class LN_FindSource : uint8_t { All = 0, Collection = 1, Property = 2, List = 3 };
/** Find Objects' Pick (custom2). */
enum class LN_FindPick : uint8_t { Nearest = 0, Farthest = 1, Random = 2, All = 3 };

/** A set of objects the index keeps a grid for (a List has none). */
struct LN_FindSourceKey {
  LN_FindSource source = LN_FindSource::All;
  /** Collection: the collection's name (found in the game's Main when the grid is made);
   * Property: the game property's name. */
  std::string name;

  bool operator==(const LN_FindSourceKey &other) const
  {
    return source == other.source && name == other.name;
  }
};

/** Find Objects' query settings in LN_QueryExpression::int_value: the source, the pick and the
 * program's find source (LN_Program::AddFindSource). */
inline int32_t LN_FindSettings(const LN_FindSource source,
                               const LN_FindPick pick,
                               const uint32_t source_index)
{
  return int32_t(uint32_t(source) | (uint32_t(pick) << 8) | ((source_index & 0xffffu) << 16));
}
inline LN_FindSource LN_FindSettingsSource(const int32_t settings)
{
  return LN_FindSource(uint32_t(settings) & 0xffu);
}
inline LN_FindPick LN_FindSettingsPick(const int32_t settings)
{
  return LN_FindPick((uint32_t(settings) >> 8) & 0xffu);
}
inline uint32_t LN_FindSettingsSourceIndex(const int32_t settings)
{
  return (uint32_t(settings) >> 16) & 0xffffu;
}

/** One search. */
struct LN_FindRequest {
  MT_Vector3 center = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** The view cone's axis (unit length); only read when `cos_half_angle` is above -1. */
  MT_Vector3 forward = MT_Vector3(0.0f, 1.0f, 0.0f);
  /** Not above 0: any distance. */
  float radius = 0.0f;
  /** The cosine of half the view angle; -1 or less: all around. */
  float cos_half_angle = -1.0f;
  LN_FindPick pick = LN_FindPick::Nearest;
  /** Random's number in [0, 1). */
  float random_unit = 0.0f;
  /** Count is read: Nearest at any distance then looks at every object (else its rings stop at
   * the nearest, and the answer's count is 1). */
  bool need_count = true;
  /** Objects never found (the center object and the tree's own object with Exclude Self). */
  const KX_GameObject *exclude_a = nullptr;
  const KX_GameObject *exclude_b = nullptr;
};

struct LN_FindHit {
  KX_GameObject *object = nullptr;
  MT_Vector3 position = MT_Vector3(0.0f, 0.0f, 0.0f);
  float distance = 0.0f;
  /** Its place among the tick's objects (ties go to the earlier). */
  uint32_t order = 0;
};

struct LN_FindAnswer {
  bool found = false;
  LN_FindHit best;
  /** How many objects are within the radius and the cone. */
  int32_t count = 0;
  /** All: every one of them, nearest first. */
  std::vector<LN_FindHit> list;
};

/** The tick's poses and grids. */
class LN_FindIndex {
 public:
  /** Starts tick `tick` over `objects` (the active objects in scene order): their world
   * positions and forward (+Y) axes now. Main thread. */
  void Begin(uint64_t tick, const std::vector<KX_GameObject *> &objects);
  /** Makes the grid of `key` (once a tick): the tick's objects `member` keeps. Main thread. */
  void AddSource(const LN_FindSourceKey &key,
                 const std::function<bool(KX_GameObject *object)> &member);
  uint64_t GetTick() const
  {
    return m_tick;
  }
  /** The source's grid this tick, or UINT32_MAX. */
  uint32_t FindSource(const LN_FindSourceKey &key) const;
  /** An object's pose when the tick started (false: not one of the tick's objects). */
  bool PoseOf(const KX_GameObject *object, MT_Vector3 &r_position, MT_Vector3 &r_forward) const;
  /** Searches source `source`'s grid. */
  void Search(uint32_t source, const LN_FindRequest &request, LN_FindAnswer &r_answer) const;
  /** Searches `objects` (a List source; objects not of the tick are left out). */
  void SearchObjects(const std::vector<KX_GameObject *> &objects,
                     const LN_FindRequest &request,
                     LN_FindAnswer &r_answer) const;
  /** How many objects the tick holds. */
  size_t GetObjectCount() const
  {
    return m_objects.size();
  }

 private:
  struct Grid {
    LN_FindSourceKey key;
    /** Members (indices of m_objects) sorted by cell; cell c holds [cell_start[c],
     * cell_start[c + 1]). */
    std::vector<uint32_t> members;
    std::vector<uint32_t> cell_start;
    MT_Vector3 origin = MT_Vector3(0.0f, 0.0f, 0.0f);
    float cell = 1.0f;
    int32_t dims[3] = {1, 1, 1};
    /** The members' bounds. */
    MT_Vector3 low = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 high = MT_Vector3(0.0f, 0.0f, 0.0f);
  };
  /** Collects the candidates of `request` among `indices` into the answer. */
  template<class ForEach>
  void Collect(ForEach for_each, const LN_FindRequest &request, LN_FindAnswer &r_answer) const;

  uint64_t m_tick = UINT64_MAX;
  std::vector<KX_GameObject *> m_objects;
  std::vector<MT_Vector3> m_positions;
  std::vector<MT_Vector3> m_forwards;
  std::unordered_map<const KX_GameObject *, uint32_t> m_indexOf;
  std::vector<Grid> m_grids;
};
