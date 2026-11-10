/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Where the rays of Raycast from Mesh start (2026-11-10). A face ray starts at the centre of a
 * Blender face (the dot Edit Mode draws) and points along its normal; a vertex ray starts at a
 * Blender vertex and points along its vertex normal (the corner angle weighted average of the
 * faces around it, as Blender's normals overlay shows it, for flat and smooth shading alike).
 * Faces are Blender's (quads and n-gons), never the game mesh's triangles, so a ray's face is the
 * face the node's Face Indices output names.
 *
 * Positions are read through a function: the runtime gives the game mesh's start shape (the mesh
 * as it was converted when the game started), tests give Blender positions. Normals are computed
 * here from those positions with Blender's own formulas (Newell's method for faces,
 * `bke::mesh::normals_calc_verts` for vertices), only for the sampled elements, so a query never
 * makes Blender compute normals of the whole mesh.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BLI_function_ref.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

#include "MT_Vector3.h"

namespace blender {
struct Mesh;
}

/** One ray's start on the mesh and its direction, in the mesh's own space. */
struct LN_MeshRaySample {
  MT_Vector3 position;
  MT_Vector3 normal;
};

/** A mesh's faces as Blender stores them, its vertex positions read through `position`. */
struct LN_MeshRayGeometry {
  blender::OffsetIndices<int> faces;
  blender::Span<int> corner_verts;
  /** Read by vertex samples only. */
  blender::GroupedSpan<int> vert_to_face_map;
  /** False when the vertex has no position (out of range, not in the game mesh, not finite). */
  blender::FunctionRef<bool(int vert, blender::float3 &r_position)> position;
};

/**
 * The ray of face `face`: Blender's face centre (`bke::mesh::face_center_calc`) and face normal
 * (Newell's method, as `Mesh::face_normals`). False for a face with fewer than three corners, a
 * position missing, or no area (Blender would give it +Z; no ray is better than a made-up one).
 */
bool LN_MeshFaceRaySample(const LN_MeshRayGeometry &geometry,
                          int face,
                          LN_MeshRaySample &r_sample);

/**
 * The ray of vertex `vert`: its position and Blender's vertex normal (as
 * `bke::mesh::normals_calc_verts`). False for a vertex no face uses (a loose vertex has no
 * direction), a position missing, or faces whose normals cancel out.
 */
bool LN_MeshVertexRaySample(const LN_MeshRayGeometry &geometry,
                            int vert,
                            LN_MeshRaySample &r_sample);

/**
 * At most `max_rays` samples of the elements `0 .. count - 1`, in element order: the elements are
 * cut into `min(count, max_rays)` even bins and each bin gives its middle element, or the nearest
 * one around it that `sample` accepts. Every element casts when `count <= max_rays`; otherwise an
 * even spread in element order, the same every time (3 of 4 faces: faces 0, 1 and 2). Regular
 * meshes read exactly the picked elements, however big the mesh.
 */
std::vector<LN_MeshRaySample> LN_PickMeshRaySamples(
    uint64_t count,
    size_t max_rays,
    blender::FunctionRef<bool(uint64_t index, LN_MeshRaySample &r_sample)> sample);

/* -------------------------------------------------------------------- */
/** \name Vertex Group input (2026-11-10)
 *
 * The node's Vertex Group names a vertex group, or a Boolean or Float attribute, of the Blender
 * mesh the rays come from. Vertices cast when their value is above 0. Faces cast when all their
 * corners' values are above 0 (Blender's rule: selecting a vertex group in Edit Mode selects
 * exactly those faces); a face attribute picks faces by its own value. Members are in Blender's
 * index order, and Max Rays spreads over them only.
 * \{ */

/** The elements whose value is above 0 (a NaN is not), in order. */
std::vector<int> LN_MeshRaySelectByValue(blender::Span<float> values);

/** The faces all of whose corners' vertex values are above 0, in order. */
std::vector<int> LN_MeshRaySelectFacesByVertices(blender::OffsetIndices<int> faces,
                                                 blender::Span<int> corner_verts,
                                                 blender::Span<float> vertex_values);

/**
 * The vertices (or faces) of `mesh` in the group or attribute `name`. False when the mesh has no
 * such vertex group or attribute, or one that cannot be read as a number (a string attribute).
 */
bool LN_MeshRaySelectGroup(const blender::Mesh &mesh,
                           const std::string &name,
                           bool faces,
                           std::vector<int> &r_members);

/**
 * The scene's remembered group members, so a query scans its mesh once, not every step. Keyed by
 * the session UID of the Blender object or mesh the game mesh came from (Blender never gives a
 * UID twice in a session, so a removed object's list never answers for another), the group and
 * the domain; an entry holds while the mesh's vertex, face and corner counts do. The weights are
 * the ones of the mesh when the game started (a weight changed while playing is not seen), as the
 * rays keep the starting shape. Lives as long as the scene's Logic Nodes manager: a new game reads
 * the weights again. Thread safe.
 */
class LN_MeshRayGroupCache {
 public:
  /**
   * The members of `name` in `mesh`, found once per key. Null when the mesh has no such group:
   * warned once per key, naming `object_name`. `source_uid` 0 (no session UID) is never kept.
   */
  std::shared_ptr<const std::vector<int>> Members(uint32_t source_uid,
                                                  bool source_is_object,
                                                  const blender::Mesh &mesh,
                                                  const std::string &name,
                                                  bool faces,
                                                  const std::string &object_name);

  /** How many times a group was looked up in a mesh (tests: a second query reads no mesh). */
  uint64_t ScanCount() const;

 private:
  struct Entry {
    int verts_num = 0;
    int faces_num = 0;
    int corners_num = 0;
    /** Null: the mesh has no such group. */
    std::shared_ptr<const std::vector<int>> members;
  };

  mutable std::mutex m_mutex;
  std::unordered_map<std::string, Entry> m_entries;
  std::unordered_set<std::string> m_warned;
  uint64_t m_scanCount = 0;
};

/** \} */
