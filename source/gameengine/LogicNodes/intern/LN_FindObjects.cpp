/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Find Objects' index (LN_FindObjects.h): the tick's poses and a uniform grid per source. The
 * answers depend only on the poses and the request, never on the order the cells are visited
 * (ties go to the object earlier in the scene), so every thread and every run shape gets the
 * same answer.
 */

#include "LN_FindObjects.h"

#include "KX_GameObject.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <limits>

namespace {

bool Finite(const MT_Vector3 &v)
{
  return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

}  // namespace

void LN_FindIndex::Begin(const uint64_t tick, const std::vector<KX_GameObject *> &objects)
{
  m_tick = tick;
  m_objects.clear();
  m_positions.clear();
  m_forwards.clear();
  m_indexOf.clear();
  m_grids.clear();
  m_objects.reserve(objects.size());
  m_positions.reserve(objects.size());
  m_forwards.reserve(objects.size());
  m_indexOf.reserve(objects.size());
  for (KX_GameObject *object : objects) {
    if (object == nullptr || m_indexOf.count(object) != 0) {
      continue;
    }
    const MT_Vector3 position = object->NodeGetWorldPosition();
    if (!Finite(position)) {
      continue;
    }
    const MT_Matrix3x3 orientation = object->NodeGetWorldOrientation();
    MT_Vector3 forward(orientation[0][1], orientation[1][1], orientation[2][1]);
    const float length = forward.length();
    forward = length > 0.0f && std::isfinite(length) ? forward / length :
                                                       MT_Vector3(0.0f, 1.0f, 0.0f);
    m_indexOf.emplace(object, uint32_t(m_objects.size()));
    m_objects.push_back(object);
    m_positions.push_back(position);
    m_forwards.push_back(forward);
  }
}

void LN_FindIndex::AddSource(const LN_FindSourceKey &key,
                             const std::function<bool(KX_GameObject *object)> &member)
{
  if (FindSource(key) != UINT32_MAX) {
    return;
  }
  Grid grid;
  grid.key = key;
  std::vector<uint32_t> members;
  for (uint32_t index = 0; index < m_objects.size(); index++) {
    if (member(m_objects[index])) {
      members.push_back(index);
    }
  }
  if (!members.empty()) {
    MT_Vector3 low = m_positions[members.front()], high = low;
    for (const uint32_t index : members) {
      const MT_Vector3 &p = m_positions[index];
      for (int axis = 0; axis < 3; axis++) {
        low[axis] = std::min(low[axis], p[axis]);
        high[axis] = std::max(high[axis], p[axis]);
      }
    }
    grid.low = low;
    grid.high = high;
    /* A cell of about the members' spacing (a flat world measured over its plane), so a cell
     * holds a few of them; no more cells than about four a member. */
    float extent[3];
    for (int axis = 0; axis < 3; axis++) {
      extent[axis] = high[axis] - low[axis];
    }
    float sorted[3] = {extent[0], extent[1], extent[2]};
    std::sort(sorted, sorted + 3);
    const float n = float(members.size());
    float cell = sorted[0] < 0.1f * sorted[2] ?
                     2.0f * std::sqrt(std::max(sorted[1], 1.0f) * std::max(sorted[2], 1.0f) / n) :
                     2.0f * std::cbrt(sorted[0] * sorted[1] * sorted[2] / n);
    if (!(cell >= 0.25f) || !std::isfinite(cell)) {
      cell = 0.25f;
    }
    const auto cells_for = [&](const float size) {
      double cells = 1.0;
      for (int axis = 0; axis < 3; axis++) {
        cells *= std::floor(double(extent[axis]) / double(size)) + 1.0;
      }
      return cells;
    };
    while (std::isfinite(cell) && cells_for(cell) > 4.0 * n + 64.0) {
      cell *= 2.0f;
    }
    grid.origin = low;
    const bool one_cell = !std::isfinite(cell) || !std::isfinite(extent[0]) ||
                          !std::isfinite(extent[1]) || !std::isfinite(extent[2]);
    if (one_cell) {
      /* Members spread past what a float measures (finite ends a float apart, 2026-11-17): one
       * cell holds them all (the search then scans them). */
      grid.cell = FLT_MAX;
      grid.dims[0] = grid.dims[1] = grid.dims[2] = 1;
    }
    else {
      grid.cell = cell;
      for (int axis = 0; axis < 3; axis++) {
        grid.dims[axis] = int32_t(std::floor(extent[axis] / cell)) + 1;
      }
    }
    const size_t cell_count = size_t(grid.dims[0]) * size_t(grid.dims[1]) *
                              size_t(grid.dims[2]);
    std::vector<uint32_t> cell_of(members.size());
    grid.cell_start.assign(cell_count + 1, 0);
    for (size_t k = 0; k < members.size(); k++) {
      const MT_Vector3 &p = m_positions[members[k]];
      int32_t c[3];
      for (int axis = 0; axis < 3; axis++) {
        c[axis] = one_cell ? 0 :
                             std::clamp(int32_t(std::floor((p[axis] - low[axis]) / cell)),
                                        0,
                                        grid.dims[axis] - 1);
      }
      cell_of[k] = uint32_t(c[0] + grid.dims[0] * (c[1] + grid.dims[1] * c[2]));
      grid.cell_start[cell_of[k] + 1]++;
    }
    for (size_t c = 0; c < cell_count; c++) {
      grid.cell_start[c + 1] += grid.cell_start[c];
    }
    grid.members.resize(members.size());
    std::vector<uint32_t> fill(grid.cell_start.begin(), grid.cell_start.end() - 1);
    /* Members in object order within each cell (the order makes no answer: ties go by order). */
    for (size_t k = 0; k < members.size(); k++) {
      grid.members[fill[cell_of[k]]++] = members[k];
    }
  }
  m_grids.push_back(std::move(grid));
}

uint32_t LN_FindIndex::FindSource(const LN_FindSourceKey &key) const
{
  for (uint32_t index = 0; index < m_grids.size(); index++) {
    if (m_grids[index].key == key) {
      return index;
    }
  }
  return UINT32_MAX;
}

bool LN_FindIndex::PoseOf(const KX_GameObject *object,
                          MT_Vector3 &r_position,
                          MT_Vector3 &r_forward) const
{
  const auto found = m_indexOf.find(object);
  if (found == m_indexOf.end()) {
    return false;
  }
  r_position = m_positions[found->second];
  r_forward = m_forwards[found->second];
  return true;
}

template<class ForEach>
void LN_FindIndex::Collect(ForEach for_each,
                           const LN_FindRequest &request,
                           LN_FindAnswer &r_answer) const
{
  const bool bounded = request.radius > 0.0f && std::isfinite(request.radius);
  const float radius_squared = bounded ? request.radius * request.radius : 0.0f;
  const bool cone = request.cos_half_angle > -1.0f;
  float best_squared = 0.0f;
  uint32_t best = UINT32_MAX;
  std::vector<uint32_t> picked;
  for_each([&](const uint32_t index) {
    const KX_GameObject *object = m_objects[index];
    if (object == request.exclude_a || object == request.exclude_b) {
      return;
    }
    const MT_Vector3 d = m_positions[index] - request.center;
    const float squared = d.x() * d.x() + d.y() * d.y() + d.z() * d.z();
    if (bounded && !(squared <= radius_squared)) {
      return;
    }
    if (cone && squared > 0.0f) {
      const float along = d.x() * request.forward.x() + d.y() * request.forward.y() +
                          d.z() * request.forward.z();
      if (along < request.cos_half_angle * std::sqrt(squared)) {
        return;
      }
    }
    r_answer.count++;
    switch (request.pick) {
      case LN_FindPick::Nearest:
        if (best == UINT32_MAX || squared < best_squared ||
            (squared == best_squared && index < best))
        {
          best = index;
          best_squared = squared;
        }
        break;
      case LN_FindPick::Farthest:
        if (best == UINT32_MAX || squared > best_squared ||
            (squared == best_squared && index < best))
        {
          best = index;
          best_squared = squared;
        }
        break;
      case LN_FindPick::Random:
      case LN_FindPick::All:
        picked.push_back(index);
        break;
    }
  });
  const auto hit = [&](const uint32_t index) {
    LN_FindHit h;
    h.object = m_objects[index];
    h.position = m_positions[index];
    const MT_Vector3 d = h.position - request.center;
    h.distance = std::sqrt(d.x() * d.x() + d.y() * d.y() + d.z() * d.z());
    h.order = index;
    return h;
  };
  if (request.pick == LN_FindPick::Random && !picked.empty()) {
    std::sort(picked.begin(), picked.end());
    const float unit = request.random_unit >= 0.0f && request.random_unit < 1.0f ?
                           request.random_unit :
                           0.0f;
    best = picked[std::min(size_t(unit * float(picked.size())), picked.size() - 1)];
  }
  if (request.pick == LN_FindPick::All && !picked.empty()) {
    r_answer.list.reserve(picked.size());
    for (const uint32_t index : picked) {
      r_answer.list.push_back(hit(index));
    }
    std::sort(r_answer.list.begin(),
              r_answer.list.end(),
              [](const LN_FindHit &a, const LN_FindHit &b) {
                return a.distance < b.distance || (a.distance == b.distance && a.order < b.order);
              });
    best = r_answer.list.front().order;
  }
  if (best != UINT32_MAX) {
    r_answer.found = true;
    r_answer.best = hit(best);
  }
}

void LN_FindIndex::Search(const uint32_t source,
                          const LN_FindRequest &request,
                          LN_FindAnswer &r_answer) const
{
  r_answer = LN_FindAnswer();
  if (source >= m_grids.size() || !Finite(request.center)) {
    return;
  }
  const Grid &grid = m_grids[source];
  if (grid.members.empty()) {
    return;
  }
  const auto all_members = [&](const auto &visit) {
    for (const uint32_t index : grid.members) {
      visit(index);
    }
  };
  /* The members in the cells a box of half size `half` around the center touches (all of them
   * when that is as many cells as members). */
  const auto box = [&](const float half) {
    return [&grid, &request, half](const auto &visit) {
      int32_t from[3], to[3];
      double cells = 1.0;
      for (int axis = 0; axis < 3; axis++) {
        const float lo = (request.center[axis] - half - grid.origin[axis]) / grid.cell;
        const float hi = (request.center[axis] + half - grid.origin[axis]) / grid.cell;
        if (!std::isfinite(lo) || !std::isfinite(hi)) {
          /* (Past what a float measures: every member; the scan measures each.) */
          for (const uint32_t index : grid.members) {
            visit(index);
          }
          return;
        }
        if (hi < 0.0f || lo > float(grid.dims[axis])) {
          return;
        }
        from[axis] = std::clamp(int32_t(std::floor(lo)), 0, grid.dims[axis] - 1);
        to[axis] = std::clamp(int32_t(std::floor(hi)), 0, grid.dims[axis] - 1);
        cells *= double(to[axis] - from[axis] + 1);
      }
      if (cells > double(grid.members.size())) {
        for (const uint32_t index : grid.members) {
          visit(index);
        }
        return;
      }
      for (int32_t z = from[2]; z <= to[2]; z++) {
        for (int32_t y = from[1]; y <= to[1]; y++) {
          const uint32_t row = uint32_t(grid.dims[0] * (y + grid.dims[1] * z));
          const uint32_t begin = grid.cell_start[row + uint32_t(from[0])];
          const uint32_t end = grid.cell_start[row + uint32_t(to[0]) + 1];
          for (uint32_t k = begin; k < end; k++) {
            visit(grid.members[k]);
          }
        }
      }
    };
  };
  const bool bounded = request.radius > 0.0f && std::isfinite(request.radius);
  if (bounded) {
    Collect(box(request.radius), request, r_answer);
    return;
  }
  if (request.pick == LN_FindPick::Nearest && !request.need_count) {
    /* Rings: the nearest within a growing radius is the nearest of all. */
    float span = 0.0f;
    for (int axis = 0; axis < 3; axis++) {
      span = std::max(span,
                      std::max(std::fabs(request.center[axis] - grid.low[axis]),
                               std::fabs(request.center[axis] - grid.high[axis])));
    }
    const float reach_all = span * 1.7320509f + grid.cell;
    for (float radius = grid.cell; radius < reach_all; radius *= 2.0f) {
      LN_FindRequest within = request;
      within.radius = radius;
      LN_FindAnswer answer;
      Collect(box(radius), within, answer);
      if (answer.found) {
        r_answer = answer;
        r_answer.count = 1;
        return;
      }
    }
  }
  Collect(all_members, request, r_answer);
}

void LN_FindIndex::SearchObjects(const std::vector<KX_GameObject *> &objects,
                                 const LN_FindRequest &request,
                                 LN_FindAnswer &r_answer) const
{
  r_answer = LN_FindAnswer();
  if (!Finite(request.center)) {
    return;
  }
  std::vector<uint32_t> indices;
  indices.reserve(objects.size());
  for (const KX_GameObject *object : objects) {
    const auto found = m_indexOf.find(object);
    if (found != m_indexOf.end()) {
      indices.push_back(found->second);
    }
  }
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  Collect(
      [&](const auto &visit) {
        for (const uint32_t index : indices) {
          visit(index);
        }
      },
      request,
      r_answer);
}
