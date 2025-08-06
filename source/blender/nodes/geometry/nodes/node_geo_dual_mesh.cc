/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_task.hh"

#include "BKE_attribute_math.hh"
#include "BKE_mesh.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_randomize.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_dual_mesh_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Mesh")
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Mesh to compute the dual of");
  b.add_output<decl::Geometry>("Dual Mesh").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Keep Boundaries")
      .default_value(false)
      .description(
          "Keep non-manifold boundaries of the input mesh in place by avoiding the dual "
          "transformation there");
}

enum class EdgeType : int8_t {
  Loose = 0,       /* No faces connected to it. */
  Boundary = 1,    /* An edge connected to exactly one face. */
  Normal = 2,      /* A normal edge (connected to two faces). */
  NonManifold = 3, /* An edge connected to more than two faces. */
};

static EdgeType get_edge_type_with_added_neighbor(EdgeType old_type)
{
  switch (old_type) {
    case EdgeType::Loose:
      return EdgeType::Boundary;
    case EdgeType::Boundary:
      return EdgeType::Normal;
    case EdgeType::Normal:
    case EdgeType::NonManifold:
      return EdgeType::NonManifold;
  }
  BLI_assert_unreachable();
  return EdgeType::Loose;
}

enum class VertexType : int8_t {
  Loose = 0,       /* Either no edges connected or only loose edges connected. */
  Normal = 1,      /* A normal vertex. */
  Boundary = 2,    /* A vertex on a boundary edge. */
  NonManifold = 3, /* A vertex on a non-manifold edge. */
};

static VertexType get_vertex_type_with_added_neighbor(VertexType old_type)
{
  switch (old_type) {
    case VertexType::Loose:
      return VertexType::Normal;
    case VertexType::Normal:
      return VertexType::Boundary;
    case VertexType::Boundary:
    case VertexType::NonManifold:
      return VertexType::NonManifold;
  }
  BLI_assert_unreachable();
  return VertexType::Loose;
}

/* Copy only where vertex_types is 'normal'. If keep boundaries is selected, also copy from
 * boundary vertices. */
template<typename T>
static void copy_data_based_on_vertex_types(Span<T> data,
                                            MutableSpan<T> r_data,
                                            const Span<VertexType> vertex_types,
                                            const bool keep_boundaries)
{
  if (keep_boundaries) {
    int out_i = 0;
    for (const int i : data.index_range()) {
      if (ELEM(vertex_types[i], VertexType::Normal, VertexType::Boundary)) {
        r_data[out_i] = data[i];
        out_i++;
      }
    }
  }
  else {
    int out_i = 0;
    for (const int i : data.index_range()) {
      if (vertex_types[i] == VertexType::Normal) {
        r_data[out_i] = data[i];
        out_i++;
      }
    }
  }
}

template<typename T>
static void copy_data_based_on_pairs(Span<T> data,
                                     MutableSpan<T> r_data,
                                     const Span<std::pair<int, int>> new_to_old_map)
{
  for (const std::pair<int, int> &pair : new_to_old_map) {
    r_data[pair.first] = data[pair.second];
  }
}

/**
 * Transfers the attributes from the original mesh to the new mesh using the following logic:
 * - If the attribute was on the face domain it is now on the point domain, and this is true
 *   for all faces, so we can just copy these.
 * - If the attribute was on the vertex domain there are three cases:
 *   - It was a 'bad' vertex so it is not in the dual mesh, and we can just ignore it
 *   - It was a normal vertex so it has a corresponding face in the dual mesh to which we can
 *     transfer.
 *   - It was a boundary vertex so it has a corresponding face, if keep_boundaries is true.
 *     Otherwise we can just ignore it.
 * - If the attribute was on the edge domain we lookup for the new edges which edge it originated
 *   from using `new_to_old_edges_map`. We have to do it in this reverse order, because there can
 *   be more edges in the new mesh if keep boundaries is on.
 * - We do the same thing for face corners as we do for edges.
 *
 * Some of the vertices (on the boundary) in the dual mesh don't come from faces, but from edges or
 * vertices. For these the `boundary_vertex_to_relevant_face_map` is used, which maps them to the
 * closest face.
 */
static void transfer_attributes(
    const Span<VertexType> vertex_types,
    const bool keep_boundaries,
    const Span<int> new_to_old_edges_map,
    const Span<int> new_to_old_face_corners_map,
    const Span<std::pair<int, int>> boundary_vertex_to_relevant_face_map,
    const AttributeFilter &attribute_filter,
    const AttributeAccessor src_attributes,
    MutableAttributeAccessor dst_attributes)
{
  /* Retrieve all attributes except for position which is handled manually.
   * Remove anonymous attributes that don't need to be propagated. */
  Set<StringRefNull> attribute_ids = src_attributes.all_ids();
  attribute_ids.remove("position");
  attribute_ids.remove(".edge_verts");
  attribute_ids.remove(".corner_vert");
  attribute_ids.remove(".corner_edge");
  attribute_ids.remove("sharp_face");
  attribute_ids.remove_if([&](const StringRef id) { return attribute_filter.allow_skip(id); });

  for (const StringRef id : attribute_ids) {
    GAttributeReader src = src_attributes.lookup(id);

    AttrDomain out_domain;
    if (src.domain == AttrDomain::Face) {
      out_domain = AttrDomain::Point;
    }
    else if (src.domain == AttrDomain::Point) {
      out_domain = AttrDomain::Face;
    }
    else {
      /* Edges and Face Corners. */
      out_domain = src.domain;
    }
    const bke::AttrType data_type = bke::cpp_type_to_attribute_type(src.varray.type());
    GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
        id, out_domain, data_type);
    if (!dst) {
      continue;
    }

    switch (src.domain) {
      case AttrDomain::Point: {
        const GVArraySpan src_span(*src);
        bke::attribute_math::convert_to_static_type(data_type, [&](auto dummy) {
          using T = decltype(dummy);
          copy_data_based_on_vertex_types(
              src_span.typed<T>(), dst.span.typed<T>(), vertex_types, keep_boundaries);
        });
        break;
      }
      case AttrDomain::Edge:
        bke::attribute_math::gather(*src, new_to_old_edges_map, dst.span);
        break;
      case AttrDomain::Face: {
        const GVArraySpan src_span(*src);
        dst.span.take_front(src_span.size()).copy_from(src_span);
        bke::attribute_math::convert_to_static_type(data_type, [&](auto dummy) {
          using T = decltype(dummy);
          if (keep_boundaries) {
            copy_data_based_on_pairs(
                src_span.typed<T>(), dst.span.typed<T>(), boundary_vertex_to_relevant_face_map);
          }
        });
        break;
      }
      case AttrDomain::Corner:
        bke::attribute_math::gather(*src, new_to_old_face_corners_map, dst.span);
        break;
      default:
        BLI_assert_unreachable();
    }
    dst.finish();
  }
}

/**
 * Calculates the boundaries of the mesh. Boundary faces are not computed since we don't need
 * them later on. We use the following definitions:
 * - An edge is on a boundary if it is connected to only one face.
 * - A vertex is on a boundary if it is on an edge on a boundary.
 */
static void calc_boundaries(const Mesh &mesh,
                            MutableSpan<VertexType> r_vertex_types,
                            MutableSpan<EdgeType> r_edge_types)
{
  BLI_assert(r_vertex_types.size() == mesh.verts_num);
  BLI_assert(r_edge_types.size() == mesh.edges_num);
  const Span<int2> edges = mesh.edges();
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_edges = mesh.corner_edges();

  r_vertex_types.fill(VertexType::Loose);
  r_edge_types.fill(EdgeType::Loose);

  /* Add up the number of faces connected to each edge. */
  for (const int i : IndexRange(mesh.faces_num)) {
    for (const int edge_i : corner_edges.slice(faces[i])) {
      r_edge_types[edge_i] = get_edge_type_with_added_neighbor(r_edge_types[edge_i]);
    }
  }

  /* Update vertices. */
  for (const int i : IndexRange(mesh.edges_num)) {
    const EdgeType edge_type = r_edge_types[i];
    if (edge_type == EdgeType::Loose) {
      continue;
    }
    const int2 &edge = edges[i];
    if (edge_type == EdgeType::Boundary) {
      r_vertex_types[edge[0]] = get_vertex_type_with_added_neighbor(r_vertex_types[edge[0]]);
      r_vertex_types[edge[1]] = get_vertex_type_with_added_neighbor(r_vertex_types[edge[1]]);
    }
    else if (edge_type >= EdgeType::NonManifold) {
      r_vertex_types[edge[0]] = VertexType::NonManifold;
      r_vertex_types[edge[1]] = VertexType::NonManifold;
    }
  }

  /* Normal verts are on a normal edge, and not on boundary edges or non-manifold edges. */
  for (const int i : IndexRange(mesh.edges_num)) {
    const EdgeType edge_type = r_edge_types[i];
    if (edge_type == EdgeType::Normal) {
      const int2 &edge = edges[i];
      if (r_vertex_types[edge[0]] == VertexType::Loose) {
        r_vertex_types[edge[0]] = VertexType::Normal;
      }
      if (r_vertex_types[edge[1]] == VertexType::Loose) {
        r_vertex_types[edge[1]] = VertexType::Normal;
      }
    }
  }
}

/**
 * Sorts the faces connected to the given vertex based on face adjacency. The ordering is
 * so such that the normals point in the same way as the original mesh. If the vertex is a
 * boundary vertex, the first and last face have a boundary edge connected to the vertex. The
 * `r_shared_edges` array at index i is set to the index of the shared edge between the i-th and
 * `(i+1)-th` sorted face. Similarly the `r_sorted_corners` array at index i is set to the
 * corner in the i-th sorted face. If the faces couldn't be sorted, `false` is returned.
 *
 * How the faces are sorted (see diagrams below):
 * (For this explanation we'll assume all faces are oriented clockwise)
 * (The vertex whose connected faces we need to sort is "v0")
 *
 * \code{.unparsed}
 *     Normal case:                    Boundary Vertex case:
 *       v1 ----- v2 ----- v3              |       |             |
 *       |   f3   |   f0   |               v2 ---- v4 --------- v3---
 *       |        |        |               |      /          ,-' |
 *       v8 ----- v0 ----- v4              | f0  /   f1   ,-'    |
 *       |   f2   |   f1   |               |    /      ,-'       |
 *       |        |        |               |   /    ,-'          |
 *       v7 ----- v6 ----- v5              |  /  ,-'     f2      |
 *                                         | /,-'                |
 *                                         v0 ------------------ v1---
 * \endcode
 *
 * - First we get the two corners of each face that have an edge which contains v0. A corner is
 *   simply a vertex followed by an edge. In this case for the face "f0" for example, we'd end up
 *   with the corners (v: v4, e: v4<->v0) and (v: v0, e: v0<->v2). Note that if the face was
 *   oriented counter-clockwise we'd end up with the corners (v: v0, e: v0<->v4) and (v: v2, e:
 *   v0<->v2) instead.
 * - Then we need to choose one face as our first. If "v0" is not on a boundary we can just
 *   choose any face. If it is on a boundary some more care needs to be taken. Here we need to
 *   pick a face which lies on the boundary (in the diagram either f0 or f2). To choose between
 *   the two we need the next step.
 * - In the normal case we use this face to set `shared_edge_i` which indicates the index of the
 *   shared edge between this face and the next one. There are two possible choices: v0<->v4 and
 *   v2<->v0. To choose we look at the corners. Since the edge v0<->v2 lies on the corner which has
 *   v0, we set `shared_edge_i` to the other edge (v0<->v4), such that the next face will be "f1"
 *   which is the next face in clockwise order.
 * - In the boundary vertex case, we do something similar, but we are also forced to choose the
 *   edge which is not on the boundary. If this doesn't line up with orientation of the face, we
 *   know we'll need to choose the other boundary face as our first face. If the orientations
 *   don't line up there as well, it means that the mesh normals are not consistent, and we just
 *   have to force an orientation for ourselves. (Imagine if f0 is oriented counter-clockwise and
 *   f2 is oriented clockwise for example)
 * - Next comes a loop where we look at the other faces and find the one which has the shared
 *   edge. Then we set the next shared edge to the other edge on the face connected to "v0", and
 *   continue. Because of the way we've chosen the first shared edge the order of the faces will
 *   have the same orientation as that of the first face.
 *   (In this case we'd have f0 -> f1 -> f2 -> f3 which also goes around clockwise).
 * - Every time we determine a shared edge, we can also add a corner to `r_sorted_corners`. This
 *   will simply be the corner which doesn't contain the shared edge.
 * - Finally if we are in the normal case we also need to add the last "shared edge" to close the
 *   loop.
 */
static bool sort_vertex_faces(const Span<int2> edges,
                              const OffsetIndices<int> faces,
                              const Span<int> corner_verts,
                              const Span<int> corner_edges,
                              const int vertex_index,
                              const bool boundary_vertex,
                              const Span<EdgeType> edge_types,
                              MutableSpan<int> connected_faces,
                              MutableSpan<int> r_shared_edges,
                              MutableSpan<int> r_sorted_corners)
{
  if (connected_faces.size() <= 2 && (!boundary_vertex || connected_faces.is_empty())) {
    return true;
  }

  /* For each face store the two corners whose edge contains the vertex. */
  Array<std::pair<int, int>, 16> face_vertex_corners(connected_faces.size());
  for (const int i : connected_faces.index_range()) {
    bool first_edge_done = false;
    for (const int corner : faces[connected_faces[i]]) {
      const int edge = corner_edges[corner];
      if (edges[edge][0] == vertex_index || edges[edge][1] == vertex_index) {
        if (!first_edge_done) {
          face_vertex_corners[i].first = corner;
          first_edge_done = true;
        }
        else {
          face_vertex_corners[i].second = corner;
          break;
        }
      }
    }
  }

  int shared_edge_i = -1;
  /* Determine first face and orientation. For now the orientation of the whole loop depends
   * on the one face we chose as first. It's probably not worth it to check every face in
   * the loop to determine the 'average' orientation. */
  if (boundary_vertex) {
    /* Our first face needs to be one which has a boundary edge. */
    for (const int i : connected_faces.index_range()) {
      const int corner_1 = face_vertex_corners[i].first;
      const int corner_2 = face_vertex_corners[i].second;
      if (edge_types[corner_edges[corner_1]] == EdgeType::Boundary &&
          corner_verts[corner_1] == vertex_index)
      {
        shared_edge_i = corner_edges[corner_2];
        r_sorted_corners[0] = face_vertex_corners[i].first;
        std::swap(connected_faces[i], connected_faces[0]);
        std::swap(face_vertex_corners[i], face_vertex_corners[0]);
        break;
      }
      if (edge_types[corner_edges[corner_2]] == EdgeType::Boundary &&
          corner_verts[corner_2] == vertex_index)
      {
        shared_edge_i = corner_edges[corner_1];
        r_sorted_corners[0] = face_vertex_corners[i].second;
        std::swap(connected_faces[i], connected_faces[0]);
        std::swap(face_vertex_corners[i], face_vertex_corners[0]);
        break;
      }
    }
    if (shared_edge_i == -1) {
      /* The rotation is inconsistent between the two faces on the boundary. Just choose one
       * of the face's orientation. */
      for (const int i : connected_faces.index_range()) {
        const int corner_1 = face_vertex_corners[i].first;
        const int corner_2 = face_vertex_corners[i].second;
        if (edge_types[corner_edges[corner_1]] == EdgeType::Boundary) {
          shared_edge_i = corner_edges[corner_2];
          r_sorted_corners[0] = face_vertex_corners[i].first;
          std::swap(connected_faces[i], connected_faces[0]);
          std::swap(face_vertex_corners[i], face_vertex_corners[0]);
          break;
        }
        if (edge_types[corner_edges[corner_2]] == EdgeType::Boundary) {
          shared_edge_i = corner_edges[corner_1];
          r_sorted_corners[0] = face_vertex_corners[i].second;
          std::swap(connected_faces[i], connected_faces[0]);
          std::swap(face_vertex_corners[i], face_vertex_corners[0]);
          break;
        }
      }
    }
  }
  else {
    /* Any face can be the first. Just need to check the orientation. */
    const int corner_1 = face_vertex_corners.first().first;
    const int corner_2 = face_vertex_corners.first().second;
    if (corner_verts[corner_1] == vertex_index) {
      shared_edge_i = corner_edges[corner_2];
      r_sorted_corners[0] = face_vertex_corners[0].first;
    }
    else {
      r_sorted_corners[0] = face_vertex_corners[0].second;
      shared_edge_i = corner_edges[corner_1];
    }
  }
  BLI_assert(shared_edge_i != -1);

  for (const int i : IndexRange(connected_faces.size() - 1)) {
    r_shared_edges[i] = shared_edge_i;

    /* Look at the other faces to see if it has this shared edge. */
    int j = i + 1;
    for (; j < connected_faces.size(); ++j) {
      const int corner_1 = face_vertex_corners[j].first;
      const int corner_2 = face_vertex_corners[j].second;

      if (corner_edges[corner_1] == shared_edge_i) {
        r_sorted_corners[i + 1] = face_vertex_corners[j].first;
        shared_edge_i = corner_edges[corner_2];
        break;
      }
      if (corner_edges[corner_2] == shared_edge_i) {
        r_sorted_corners[i + 1] = face_vertex_corners[j].second;
        shared_edge_i = corner_edges[corner_1];
        break;
      }
    }
    if (j == connected_faces.size()) {
      /* The vertex is not manifold because the faces around the vertex don't form a loop, and
       * hence can't be sorted. */
      return false;
    }

    std::swap(connected_faces[i + 1], connected_faces[j]);
    std::swap(face_vertex_corners[i + 1], face_vertex_corners[j]);
  }

  if (!boundary_vertex) {
    /* Shared edge between first and last face. */
    r_shared_edges.last() = shared_edge_i;
  }
  return true;
}

/**
 * Get the edge on the face that contains the given vertex and is a boundary edge.
 */
static void boundary_edge_on_face(const Span<int2> edges,
                                  const Span<int> face_edges,
                                  const int vertex_index,
                                  const Span<EdgeType> edge_types,
                                  int &r_edge)
{
  for (const int edge_i : face_edges) {
    if (edge_types[edge_i] == EdgeType::Boundary) {
      const int2 &edge = edges[edge_i];
      if (edge[0] == vertex_index || edge[1] == vertex_index) {
        r_edge = edge_i;
        return;
      }
    }
  }
}

/**
 * Get the two edges on the face that contain the given vertex and are boundary edges. The
 * orientation of the face is taken into account.
 */
static void boundary_edges_on_face(const IndexRange face,
                                   const Span<int2> edges,
                                   const Span<int> corner_verts,
                                   const Span<int> corner_edges,
                                   const int vertex_index,
                                   const Span<EdgeType> edge_types,
                                   int &r_edge1,
                                   int &r_edge2)
{
  bool edge1_done = false;
  /* This is set to true if the order in which we encounter the two edges is inconsistent with the
   * orientation of the face. */
  bool needs_swap = false;
  for (const int corner : face) {
    const int edge_i = corner_edges[corner];
    if (edge_types[edge_i] == EdgeType::Boundary) {
      const int2 &edge = edges[edge_i];
      if (edge[0] == vertex_index || edge[1] == vertex_index) {
        if (edge1_done) {
          if (needs_swap) {
            r_edge2 = r_edge1;
            r_edge1 = edge_i;
          }
          else {
            r_edge2 = edge_i;
          }
          return;
        }
        r_edge1 = edge_i;
        edge1_done = true;
        if (corner_verts[corner] == vertex_index) {
          needs_swap = true;
        }
      }
    }
  }
}

static void add_edge(const int old_edge_i,
                     const int v1,
                     const int v2,
                     Vector<int> &new_to_old_edges_map,
                     Vector<int2> &new_edges,
                     Vector<int> &corner_edges)
{
  const int new_edge_i = new_edges.size();
  new_to_old_edges_map.append(old_edge_i);
  new_edges.append({v1, v2});
  corner_edges.append(new_edge_i);
}

/* Returns true if the vertex is connected only to the two faces and is not on the boundary. */
static bool vertex_needs_dissolving(const int vertex,
                                    const int first_face_index,
                                    const int second_face_index,
                                    const Span<VertexType> vertex_types,
                                    const GroupedSpan<int> vert_to_face_map)
{
  /* Order is guaranteed to be the same because 2face verts that are not on the boundary are
   * ignored in `sort_vertex_faces`. */
  return (vertex_types[vertex] != VertexType::Boundary && vert_to_face_map[vertex].size() == 2 &&
          vert_to_face_map[vertex][0] == first_face_index &&
          vert_to_face_map[vertex][1] == second_face_index);
}

/**
 * Finds 'normal' vertices which are connected to only two faces and marks them to not be
 * used in the data-structures derived from the mesh. For each pair of faces which has such a
 * vertex, an edge is created for the dual mesh between the centers of those two faces. All
 * edges in the input mesh which contain such a vertex are marked as 'done' to prevent duplicate
 * edges being created. (See #94144)
 */
static void dissolve_redundant_verts(const Span<int2> edges,
                                     const OffsetIndices<int> faces,
                                     const Span<int> corner_edges,
                                     const GroupedSpan<int> vert_to_face_map,
                                     MutableSpan<VertexType> vertex_types,
                                     MutableSpan<int> old_to_new_edges_map,
                                     Vector<int2> &new_edges,
                                     Vector<int> &new_to_old_edges_map)
{
  const int vertex_num = vertex_types.size();
  for (const int vert_i : IndexRange(vertex_num)) {
    if (vert_to_face_map[vert_i].size() != 2 || vertex_types[vert_i] != VertexType::Normal) {
      continue;
    }
    const int first_face_index = vert_to_face_map[vert_i][0];
    const int second_face_index = vert_to_face_map[vert_i][1];
    const int new_edge_index = new_edges.size();
    bool edge_created = false;
    for (const int edge_i : corner_edges.slice(faces[first_face_index])) {
      const int2 &edge = edges[edge_i];
      bool mark_edge = false;
      if (vertex_needs_dissolving(
              edge[0], first_face_index, second_face_index, vertex_types, vert_to_face_map))
      {
        /* This vertex is now 'removed' and should be ignored elsewhere. */
        vertex_types[edge[0]] = VertexType::Loose;
        mark_edge = true;
      }
      if (vertex_needs_dissolving(
              edge[1], first_face_index, second_face_index, vertex_types, vert_to_face_map))
      {
        /* This vertex is now 'removed' and should be ignored elsewhere. */
        vertex_types[edge[1]] = VertexType::Loose;
        mark_edge = true;
      }
      if (mark_edge) {
        if (!edge_created) {
          /* The vertex indices in the dual mesh are the face indices of the input mesh. */
          new_to_old_edges_map.append(edge_i);
          new_edges.append({first_face_index, second_face_index});
          edge_created = true;
        }
        old_to_new_edges_map[edge_i] = new_edge_index;
      }
    }
  }
}

/**
 * Calculate the barycentric dual of a mesh. The dual is only "dual" in terms of connectivity,
 * i.e. applying the function twice will give the same vertices, edges, and faces, but not the
 * same positions. When the option "Keep Boundaries" is selected the connectivity is no
 * longer dual.
 *
 * For the dual mesh of a manifold input mesh:
 * - The vertices are at the centers of the faces of the input mesh.
 * - The edges connect the two vertices created from the two faces next to the edge in the input
 *   mesh.
 * - The faces are at the vertices of the input mesh.
 *
 * Some special cases are needed for boundaries and non-manifold geometry.
 */
static Mesh *calc_dual_mesh(const Mesh &src_mesh,
                            const bool keep_boundaries,
                            const AttributeFilter &attribute_filter)
{
  const Span<float3> src_positions = src_mesh.vert_positions();
  const Span<int2> src_edges = src_mesh.edges();
  const OffsetIndices src_faces = src_mesh.faces();
  const Span<int> src_corner_verts = src_mesh.corner_verts();
  const Span<int> src_corner_edges = src_mesh.corner_edges();

  Array<VertexType> vertex_types(src_mesh.verts_num);
  Array<EdgeType> edge_types(src_mesh.edges_num);
  calc_boundaries(src_mesh, vertex_types, edge_types);
  /* Stores the indices of the faces connected to the vertex. Because the faces are looped
   * over in order of their indices, the face's indices will be sorted in ascending order.
   * (This can change once they are sorted using `sort_vertex_faces`). */
  Array<int> vert_to_face_indices = src_mesh.vert_to_face_map().data;
  const OffsetIndices<int> vert_to_face_offsets = src_mesh.vert_to_face_map().offsets;

  Array<Array<int>> vertex_shared_edges(src_mesh.verts_num);
  Array<Array<int>> vertex_corners(src_mesh.verts_num);
  threading::parallel_for(src_positions.index_range(), 512, [&](IndexRange range) {
    for (const int i : range) {
      if (vertex_types[i] == VertexType::Loose || vertex_types[i] >= VertexType::NonManifold ||
          (!keep_boundaries && vertex_types[i] == VertexType::Boundary))
      {
        /* Bad vertex that we can't work with. */
        continue;
      }
      MutableSpan<int> corner_indices = vert_to_face_indices.as_mutable_span().slice(
          vert_to_face_offsets[i]);
      Array<int> sorted_corners(corner_indices.size());
      bool vertex_ok = true;
      if (vertex_types[i] == VertexType::Normal) {
        Array<int> shared_edges(corner_indices.size());
        vertex_ok = sort_vertex_faces(src_edges,
                                      src_faces,
                                      src_corner_verts,
                                      src_corner_edges,
                                      i,
                                      false,
                                      edge_types,
                                      corner_indices,
                                      shared_edges,
                                      sorted_corners);
        vertex_shared_edges[i] = std::move(shared_edges);
      }
      else {
        Array<int> shared_edges(corner_indices.size() - 1);
        vertex_ok = sort_vertex_faces(src_edges,
                                      src_faces,
                                      src_corner_verts,
                                      src_corner_edges,
                                      i,
                                      true,
                                      edge_types,
                                      corner_indices,
                                      shared_edges,
                                      sorted_corners);
        vertex_shared_edges[i] = std::move(shared_edges);
      }
      if (!vertex_ok) {
        /* The sorting failed which means that the vertex is non-manifold and should be ignored
         * further on. */
        vertex_types[i] = VertexType::NonManifold;
        continue;
      }
      vertex_corners[i] = std::move(sorted_corners);
    }
  });

  const GroupedSpan<int> vert_to_face_map(vert_to_face_offsets, vert_to_face_indices);

  Vector<float3> vert_positions(src_mesh.faces_num);
  for (const int i : src_faces.index_range()) {
    const IndexRange face = src_faces[i];
    vert_positions[i] = bke::mesh::face_center_calc(src_positions, src_corner_verts.slice(face));
  }

  Array<int> boundary_edge_midpoint_index;
  if (keep_boundaries) {
    /* Only initialize when we actually need it. */
    boundary_edge_midpoint_index.reinitialize(src_mesh.edges_num);
    /* We need to add vertices at the centers of boundary edges. */
    for (const int i : IndexRange(src_mesh.edges_num)) {
      if (edge_types[i] == EdgeType::Boundary) {
        const int2 &edge = src_edges[i];
        const float3 mid = math::midpoint(src_positions[edge[0]], src_positions[edge[1]]);
        boundary_edge_midpoint_index[i] = vert_positions.size();
        vert_positions.append(mid);
      }
    }
  }

  Vector<int> face_sizes;
  Vector<int> corner_verts;
  Vector<int> corner_edges;
  Vector<int2> new_edges;
  /* These are used to transfer attributes. */
  Vector<int> new_to_old_face_corners_map;
  Vector<int> new_to_old_edges_map;
  /* Stores the index of the vertex in the dual and the face it should get the attribute from. */
  Vector<std::pair<int, int>> boundary_vertex_to_relevant_face_map;
  /* Since each edge in the dual (except the ones created with keep boundaries) comes from
   * exactly one edge in the original, we can use this array to keep track of whether it still
   * needs to be created or not. If it's not -1 it gives the index in `new_edges` of the dual
   * edge. The edges coming from preserving the boundaries only get added once anyway, so we
   * don't need a hash-map for that. */
  Array<int> old_to_new_edges_map(src_mesh.edges_num);
  old_to_new_edges_map.fill(-1);

  /* This is necessary to prevent duplicate edges from being created, but will likely not do
   * anything for most meshes. */
  dissolve_redundant_verts(src_edges,
                           src_faces,
                           src_corner_edges,
                           vert_to_face_map,
                           vertex_types,
                           old_to_new_edges_map,
                           new_edges,
                           new_to_old_edges_map);

  for (const int i : IndexRange(src_mesh.verts_num)) {
    if (vertex_types[i] == VertexType::Loose || vertex_types[i] >= VertexType::NonManifold ||
        (!keep_boundaries && vertex_types[i] == VertexType::Boundary))
    {
      /* Bad vertex that we can't work with. */
      continue;
    }

    Vector<int> corner_indices = vert_to_face_map[i];
    Span<int> shared_edges = vertex_shared_edges[i];
    Span<int> sorted_corners = vertex_corners[i];
    if (vertex_types[i] == VertexType::Normal) {
      if (corner_indices.size() <= 2) {
        /* We can't make a face from 2 vertices. */
        continue;
      }

      /* Add edges in the loop. */
      for (const int i : shared_edges.index_range()) {
        const int old_edge_i = shared_edges[i];
        if (old_to_new_edges_map[old_edge_i] == -1) {
          /* This edge has not been created yet. */
          new_to_old_edges_map.append(old_edge_i);
          old_to_new_edges_map[old_edge_i] = new_edges.size();
          new_edges.append({corner_indices[i], corner_indices[(i + 1) % corner_indices.size()]});
        }
        corner_edges.append(old_to_new_edges_map[old_edge_i]);
      }

      new_to_old_face_corners_map.extend(sorted_corners);
    }
    else {
      /**
       * The code handles boundary vertices like the vertex marked "V" in the diagram below.
       * The first thing that happens is ordering the faces f1,f2 and f3 (stored in
       * corner_indices), together with their shared edges e3 and e4 (which get stored in
       * shared_edges). The ordering could end up being clockwise or counterclockwise, for this
       * we'll assume that the ordering f1->f2->f3 is chosen. After that we add the edges in
       * between the faces, in this case the edges f1--f2, and f2--f3. Now we need to merge
       * these with the boundary edges e1 and e2. To do this we create an edge from f3 to the
       * midpoint of e2 (computed in a previous step), from this midpoint to V, from V to the
       * midpoint of e1 and from the midpoint of e1 to f1.
       *
       * \code{.unparsed}
       *       |       |             |                    |       |            |
       *       v2 ---- v3 --------- v4---                 v2 ---- v3 -------- v4---
       *       | f3   /          ,-' |                    |      /          ,-'|
       *       |     /   f2   ,-'    |                    |     /        ,-'   |
       *    e2 |    /e3    ,-' e4    |       ====>       M1-f3-/--f2-.,-'      |
       *       |   /    ,-'          |       ====>        |   /    ,-'\        |
       *       |  /  ,-'     f1      |                    |  /  ,-'    f1      |
       *       | /,-'                |                    | /,-'        |      |
       *       V-------------------- v5---                V------------M2----- v5---
       * \endcode
       */

      /* Add the edges in between the faces. */
      for (const int i : shared_edges.index_range()) {
        const int old_edge_i = shared_edges[i];
        if (old_to_new_edges_map[old_edge_i] == -1) {
          /* This edge has not been created yet. */
          new_to_old_edges_map.append(old_edge_i);
          old_to_new_edges_map[old_edge_i] = new_edges.size();
          new_edges.append({corner_indices[i], corner_indices[i + 1]});
        }
        corner_edges.append(old_to_new_edges_map[old_edge_i]);
      }

      new_to_old_face_corners_map.extend(sorted_corners);

      /* Add the vertex and the midpoints of the two boundary edges to the loop. */

      /* Get the boundary edges. */
      int edge1;
      int edge2;
      if (corner_indices.size() >= 2) {
        /* The first boundary edge is at the end of the chain of faces. */
        boundary_edge_on_face(src_edges,
                              src_corner_edges.slice(src_faces[corner_indices.last()]),
                              i,
                              edge_types,
                              edge1);
        boundary_edge_on_face(src_edges,
                              src_corner_edges.slice(src_faces[corner_indices.first()]),
                              i,
                              edge_types,
                              edge2);
      }
      else {
        /* If there is only one face both edges are in that face. */
        boundary_edges_on_face(src_faces[corner_indices[0]],
                               src_edges,
                               src_corner_verts,
                               src_corner_edges,
                               i,
                               edge_types,
                               edge1,
                               edge2);
      }

      const int last_face_center = corner_indices.last();
      corner_indices.append(boundary_edge_midpoint_index[edge1]);
      new_to_old_face_corners_map.append(sorted_corners.last());
      const int first_midpoint = corner_indices.last();
      if (old_to_new_edges_map[edge1] == -1) {
        add_edge(edge1,
                 last_face_center,
                 first_midpoint,
                 new_to_old_edges_map,
                 new_edges,
                 corner_edges);
        old_to_new_edges_map[edge1] = new_edges.size() - 1;
        boundary_vertex_to_relevant_face_map.append(std::pair(first_midpoint, last_face_center));
      }
      else {
        corner_edges.append(old_to_new_edges_map[edge1]);
      }
      corner_indices.append(vert_positions.size());
      /* This is sort of arbitrary, but interpolating would be a lot harder to do. */
      new_to_old_face_corners_map.append(sorted_corners.first());
      boundary_vertex_to_relevant_face_map.append(
          std::pair(corner_indices.last(), last_face_center));
      vert_positions.append(src_positions[i]);
      const int boundary_vertex = corner_indices.last();
      add_edge(
          edge1, first_midpoint, boundary_vertex, new_to_old_edges_map, new_edges, corner_edges);

      corner_indices.append(boundary_edge_midpoint_index[edge2]);
      new_to_old_face_corners_map.append(sorted_corners.first());
      const int second_midpoint = corner_indices.last();
      add_edge(
          edge2, boundary_vertex, second_midpoint, new_to_old_edges_map, new_edges, corner_edges);

      if (old_to_new_edges_map[edge2] == -1) {
        const int first_face_center = corner_indices.first();
        add_edge(edge2,
                 second_midpoint,
                 first_face_center,
                 new_to_old_edges_map,
                 new_edges,
                 corner_edges);
        old_to_new_edges_map[edge2] = new_edges.size() - 1;
        boundary_vertex_to_relevant_face_map.append(std::pair(second_midpoint, first_face_center));
      }
      else {
        corner_edges.append(old_to_new_edges_map[edge2]);
      }
    }

    face_sizes.append(corner_indices.size());
    for (const int j : corner_indices) {
      corner_verts.append(j);
    }
  }
  Mesh *mesh_out = BKE_mesh_new_nomain(
      vert_positions.size(), new_edges.size(), face_sizes.size(), corner_verts.size());
  bke::mesh_smooth_set(*mesh_out, false);

  transfer_attributes(vertex_types,
                      keep_boundaries,
                      new_to_old_edges_map,
                      new_to_old_face_corners_map,
                      boundary_vertex_to_relevant_face_map,
                      attribute_filter,
                      src_mesh.attributes(),
                      mesh_out->attributes_for_write());

  mesh_out->vert_positions_for_write().copy_from(vert_positions);
  mesh_out->edges_for_write().copy_from(new_edges);

  if (mesh_out->faces_num > 0) {
    MutableSpan<int> dst_face_offsets = mesh_out->face_offsets_for_write();
    dst_face_offsets.drop_back(1).copy_from(face_sizes);
    offset_indices::accumulate_counts_to_offsets(dst_face_offsets);
  }
  mesh_out->corner_verts_for_write().copy_from(corner_verts);
  mesh_out->corner_edges_for_write().copy_from(corner_edges);

  return mesh_out;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  const bool keep_boundaries = params.extract_input<bool>("Keep Boundaries");
  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (const Mesh *mesh = geometry_set.get_mesh()) {
      Mesh *new_mesh = calc_dual_mesh(
          *mesh, keep_boundaries, params.get_attribute_filter("Dual Mesh"));
      geometry::debug_randomize_mesh_order(new_mesh);
      geometry_set.replace_mesh(new_mesh);
    }
  });
  params.set_output("Dual Mesh", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeDualMesh", GEO_NODE_DUAL_MESH);
  ntype.ui_name = "Dual Mesh";
  ntype.ui_description = "Convert Faces into vertices and vertices into faces";
  ntype.enum_name_legacy = "DUAL_MESH";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_dual_mesh_cc
