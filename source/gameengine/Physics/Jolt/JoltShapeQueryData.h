/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file JoltShapeQueryData.h
 *  \ingroup physjolt
 */

#pragma once

#include "JoltPhysicsConfig.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include "MT_Vector2.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

struct JoltTriangleUV {
  std::array<MT_Vector2, 3> corners;
};

struct JoltMeshQueryData {
  std::vector<int32_t> polygonIndices;
  std::vector<JoltTriangleUV> triangleUVs;
};

class JoltShapeQueryData {
 public:
  using MeshDataPtr = std::shared_ptr<const JoltMeshQueryData>;

  const JoltMeshQueryData *FindMeshData(const JPH::Shape *leafShape) const
  {
    const auto it = m_meshData.find(leafShape);
    return it == m_meshData.end() ? nullptr : it->second.get();
  }

  void Add(const JPH::Shape *leafShape, MeshDataPtr meshData)
  {
    if (leafShape && meshData) {
      m_meshData[leafShape] = std::move(meshData);
    }
  }

  void Merge(const std::shared_ptr<const JoltShapeQueryData> &other)
  {
    if (!other) {
      return;
    }
    m_meshData.insert(other->m_meshData.begin(), other->m_meshData.end());
  }

  bool IsEmpty() const
  {
    return m_meshData.empty();
  }

 private:
  std::unordered_map<const JPH::Shape *, MeshDataPtr> m_meshData;
};

using JoltShapeQueryDataPtr = std::shared_ptr<const JoltShapeQueryData>;

inline JoltShapeQueryDataPtr JoltMergeShapeQueryData(
    const JoltShapeQueryDataPtr &first, const JoltShapeQueryDataPtr &second)
{
  if (!first) {
    return second;
  }
  if (!second || first == second) {
    return first;
  }

  std::shared_ptr<JoltShapeQueryData> merged = std::make_shared<JoltShapeQueryData>();
  merged->Merge(first);
  merged->Merge(second);
  return merged;
}
