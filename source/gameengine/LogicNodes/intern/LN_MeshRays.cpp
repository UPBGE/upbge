/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 */

#include "LN_MeshRays.h"

#include <algorithm>
#include <cmath>

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_virtual_array.hh"
#include "DNA_mesh_types.h"

#include "CM_Message.h"

using blender::float3;
using blender::IndexRange;
using blender::Span;

static bool FloatsAreFinite(const float3 &value)
{
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

static MT_Vector3 ToMT(const float3 &value)
{
  return MT_Vector3(value.x, value.y, value.z);
}

/**
 * Newell's normal of a face, written as Blender's `normal_calc_ngon` (mesh_normals.cc), which
 * gives `Mesh::face_normals` for every face size. `r_degenerate` is set for a face without area;
 * its normal is then Blender's fallback +Z (vertex normals add it in as Blender does).
 */
static bool FaceNewellNormal(const LN_MeshRayGeometry &geometry,
                             const Span<int> face_verts,
                             float3 &r_normal,
                             bool &r_degenerate)
{
  float3 previous;
  if (!geometry.position(face_verts.last(), previous)) {
    return false;
  }
  r_normal = float3(0.0f);
  for (const int vert : face_verts) {
    float3 current;
    if (!geometry.position(vert, current)) {
      return false;
    }
    add_newell_cross_v3_v3v3(r_normal, previous, current);
    previous = current;
  }
  r_degenerate = normalize_v3(r_normal) == 0.0f;
  if (r_degenerate) {
    r_normal = float3(0.0f, 0.0f, 1.0f);
  }
  return true;
}

static bool FaceIsReadable(const LN_MeshRayGeometry &geometry, const int face)
{
  if (face < 0 || face >= geometry.faces.size()) {
    return false;
  }
  const IndexRange corners = geometry.faces[face];
  return corners.size() >= 3 && corners.one_after_last() <= geometry.corner_verts.size();
}

bool LN_MeshFaceRaySample(const LN_MeshRayGeometry &geometry,
                          const int face,
                          LN_MeshRaySample &r_sample)
{
  if (!FaceIsReadable(geometry, face)) {
    return false;
  }
  const Span<int> face_verts = geometry.corner_verts.slice(geometry.faces[face]);

  float3 normal;
  bool degenerate = false;
  if (!FaceNewellNormal(geometry, face_verts, normal, degenerate) || degenerate ||
      !FloatsAreFinite(normal))
  {
    return false;
  }

  /* The centre as `bke::mesh::face_center_calc` sums it: triangles and quads add then divide,
   * n-gons add each position times 1 / n. */
  float3 center(0.0f);
  const int size = int(face_verts.size());
  const float weight = 1.0f / float(size);
  for (const int vert : face_verts) {
    float3 position;
    if (!geometry.position(vert, position)) {
      return false;
    }
    if (size <= 4) {
      center += position;
    }
    else {
      center += position * weight;
    }
  }
  if (size <= 4) {
    center /= float(size);
  }
  if (!FloatsAreFinite(center)) {
    return false;
  }

  r_sample.position = ToMT(center);
  r_sample.normal = ToMT(normal);
  return true;
}

bool LN_MeshVertexRaySample(const LN_MeshRayGeometry &geometry,
                            const int vert,
                            LN_MeshRaySample &r_sample)
{
  if (vert < 0 || vert >= geometry.vert_to_face_map.size()) {
    return false;
  }
  const Span<int> vert_faces = geometry.vert_to_face_map[vert];
  if (vert_faces.is_empty()) {
    return false;
  }
  float3 position;
  if (!geometry.position(vert, position) || !FloatsAreFinite(position)) {
    return false;
  }

  /* `bke::mesh::normals_calc_verts`: each face's normal weighted by the vertex's corner angle in
   * that face. */
  float3 vert_normal(0.0f);
  for (const int face : vert_faces) {
    if (!FaceIsReadable(geometry, face)) {
      return false;
    }
    const IndexRange corners = geometry.faces[face];
    const Span<int> face_verts = geometry.corner_verts.slice(corners);
    float3 face_normal;
    bool degenerate = false;
    if (!FaceNewellNormal(geometry, face_verts, face_normal, degenerate)) {
      return false;
    }
    const blender::int2 adjacent = blender::bke::mesh::face_find_adjacent_verts(
        corners, geometry.corner_verts, vert);
    float3 previous;
    float3 next;
    if (!geometry.position(adjacent[0], previous) || !geometry.position(adjacent[1], next)) {
      return false;
    }
    const float3 dir_prev = blender::math::normalize(previous - position);
    const float3 dir_next = blender::math::normalize(next - position);
    const float factor = blender::math::safe_acos_approx(blender::math::dot(dir_prev, dir_next));
    vert_normal += face_normal * factor;
  }
  vert_normal = blender::math::normalize(vert_normal);
  if (!FloatsAreFinite(vert_normal) || blender::math::is_zero(vert_normal)) {
    return false;
  }

  r_sample.position = ToMT(position);
  r_sample.normal = ToMT(vert_normal);
  return true;
}

std::vector<LN_MeshRaySample> LN_PickMeshRaySamples(
    const uint64_t count,
    const size_t max_rays,
    const blender::FunctionRef<bool(uint64_t index, LN_MeshRaySample &r_sample)> sample)
{
  const size_t output_count = size_t(std::min<uint64_t>(count, uint64_t(max_rays)));
  std::vector<LN_MeshRaySample> samples;
  samples.reserve(output_count);
  if (output_count == 0) {
    return samples;
  }

  /* One sample from each even bin. Search outwards from the bin's middle only when that element
   * gives no ray; bins never overlap, so order and uniqueness hold. */
  const uint64_t bin_stride = count / uint64_t(output_count);
  const uint64_t bin_remainder = count % uint64_t(output_count);
  const auto bin_boundary = [&](const size_t index) {
    const uint64_t i = uint64_t(index);
    return bin_stride * i + (bin_remainder * i) / uint64_t(output_count);
  };
  for (size_t output_index = 0; output_index < output_count; output_index++) {
    const uint64_t bin_begin = bin_boundary(output_index);
    const uint64_t bin_end = bin_boundary(output_index + 1);
    const uint64_t center = bin_begin + (bin_end - bin_begin - 1u) / 2u;
    LN_MeshRaySample picked;
    if (sample(center, picked)) {
      samples.push_back(picked);
      continue;
    }
    for (uint64_t offset = 1; offset < bin_end - bin_begin; offset++) {
      const bool has_lower = offset <= center - bin_begin;
      const bool has_upper = offset < bin_end - center;
      if (has_lower && sample(center - offset, picked)) {
        samples.push_back(picked);
        break;
      }
      if (has_upper && sample(center + offset, picked)) {
        samples.push_back(picked);
        break;
      }
      if (!has_lower && !has_upper) {
        break;
      }
    }
  }
  return samples;
}

std::vector<int> LN_MeshRaySelectByValue(const Span<float> values)
{
  std::vector<int> members;
  for (const int index : values.index_range()) {
    if (values[index] > 0.0f) {
      members.push_back(index);
    }
  }
  return members;
}

std::vector<int> LN_MeshRaySelectFacesByVertices(const blender::OffsetIndices<int> faces,
                                                 const Span<int> corner_verts,
                                                 const Span<float> vertex_values)
{
  std::vector<int> members;
  for (const int face : faces.index_range()) {
    const IndexRange corners = faces[face];
    if (corners.is_empty() || corners.one_after_last() > corner_verts.size()) {
      continue;
    }
    bool all = true;
    for (const int vert : corner_verts.slice(corners)) {
      if (vert < 0 || vert >= vertex_values.size() || !(vertex_values[vert] > 0.0f)) {
        all = false;
        break;
      }
    }
    if (all) {
      members.push_back(face);
    }
  }
  return members;
}

bool LN_MeshRaySelectGroup(const blender::Mesh &mesh,
                           const std::string &name,
                           const bool faces,
                           std::vector<int> &r_members)
{
  using blender::bke::AttrDomain;
  const blender::bke::AttributeAccessor attributes = mesh.attributes();
  const std::optional<blender::bke::AttributeMetaData> meta = attributes.lookup_meta_data(name);
  if (!meta) {
    return false;
  }
  /* A face attribute picks faces by its own values; any other (a vertex group, a point, edge or
   * corner attribute) is read on the vertices, which Blender adapts to. */
  const bool by_face_value = faces && meta->domain == AttrDomain::Face;
  const blender::bke::AttributeReader<float> reader = attributes.lookup<float>(
      name, by_face_value ? AttrDomain::Face : AttrDomain::Point);
  if (!reader) {
    return false;
  }
  const blender::VArraySpan<float> values(*reader);
  if (!faces || by_face_value) {
    r_members = LN_MeshRaySelectByValue(values);
  }
  else {
    r_members = LN_MeshRaySelectFacesByVertices(mesh.faces(), mesh.corner_verts(), values);
  }
  return true;
}

std::shared_ptr<const std::vector<int>> LN_MeshRayGroupCache::Members(
    const uint32_t source_uid,
    const bool source_is_object,
    const blender::Mesh &mesh,
    const std::string &name,
    const bool faces,
    const std::string &object_name)
{
  const std::string key = std::to_string(source_uid) + (source_is_object ? ":o" : ":m") +
                          (faces ? ":f:" : ":v:") + name;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (source_uid != 0) {
    const auto found = m_entries.find(key);
    if (found != m_entries.end() && found->second.verts_num == mesh.verts_num &&
        found->second.faces_num == mesh.faces_num &&
        found->second.corners_num == mesh.corners_num)
    {
      return found->second.members;
    }
  }

  m_scanCount++;
  Entry entry;
  entry.verts_num = mesh.verts_num;
  entry.faces_num = mesh.faces_num;
  entry.corners_num = mesh.corners_num;
  std::vector<int> members;
  if (LN_MeshRaySelectGroup(mesh, name, faces, members)) {
    entry.members = std::make_shared<const std::vector<int>>(std::move(members));
  }
  else if (m_warned.insert(object_name + '\0' + name).second) {
    CM_Warning("Logic Nodes: Raycast from Mesh: the mesh of \"" << object_name
                                                                << "\" has no vertex group or "
                                                                   "attribute \""
                                                                << name
                                                                << "\"; it casts no rays");
  }
  std::shared_ptr<const std::vector<int>> result = entry.members;
  if (source_uid != 0) {
    m_entries[key] = std::move(entry);
  }
  return result;
}

uint64_t LN_MeshRayGroupCache::ScanCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_scanCount;
}
