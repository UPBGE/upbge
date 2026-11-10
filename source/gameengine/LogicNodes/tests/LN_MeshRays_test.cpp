/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Where Raycast from Mesh's rays start (LN_MeshRays.h): Blender faces and vertices, checked
 * against Blender's own centre and normal functions. */

#include "testing/testing.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#include "BKE_attribute.hh"
#include "BKE_deform.hh"
#include "BKE_cpp_types.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "MEM_guardedalloc.h"

#include "LN_MeshRays.h"

namespace {

using blender::Array;
using blender::float3;
using blender::GroupedSpan;
using blender::OffsetIndices;
using blender::Span;

/** A mesh built from plain lists, read as Raycast from Mesh reads one. */
struct TestMesh {
  std::vector<float3> positions;
  std::vector<int> face_offsets;
  std::vector<int> corner_verts;
  Array<int> vert_to_face_offsets;
  Array<int> vert_to_face_indices;
  GroupedSpan<int> vert_to_face;
  /** Vertices whose position the reader cannot give (a vertex the game mesh lacks). */
  std::vector<int> missing;

  TestMesh(std::vector<float3> positions_in, const std::vector<std::vector<int>> &faces)
      : positions(std::move(positions_in))
  {
    face_offsets.push_back(0);
    for (const std::vector<int> &face : faces) {
      corner_verts.insert(corner_verts.end(), face.begin(), face.end());
      face_offsets.push_back(int(corner_verts.size()));
    }
    vert_to_face = blender::bke::mesh::build_vert_to_face_map(this->faces(),
                                                              corner_verts,
                                                              int(positions.size()),
                                                              vert_to_face_offsets,
                                                              vert_to_face_indices);
  }
  /* `Geometry()` refers to this object: never copied or moved. */
  TestMesh(const TestMesh &) = delete;
  TestMesh &operator=(const TestMesh &) = delete;

  OffsetIndices<int> faces() const
  {
    return OffsetIndices<int>(Span<int>(face_offsets));
  }

  bool Position(const int vert, float3 &r_position) const
  {
    if (vert < 0 || vert >= int(positions.size())) {
      return false;
    }
    for (const int gone : missing) {
      if (gone == vert) {
        return false;
      }
    }
    r_position = positions[vert];
    return true;
  }

  /** Geometry whose position function refers to this mesh: keep the mesh alive and unmoved. */
  LN_MeshRayGeometry Geometry() const
  {
    LN_MeshRayGeometry geometry;
    geometry.faces = faces();
    geometry.corner_verts = corner_verts;
    geometry.vert_to_face_map = vert_to_face;
    geometry.position = position_function;
    return geometry;
  }

  std::vector<float3> BlenderFaceNormals() const
  {
    std::vector<float3> normals(faces().size());
    blender::bke::mesh::normals_calc_faces(positions, faces(), corner_verts, normals);
    return normals;
  }

  std::vector<float3> BlenderVertexNormals() const
  {
    const std::vector<float3> face_normals = BlenderFaceNormals();
    std::vector<float3> normals(positions.size());
    blender::bke::mesh::normals_calc_verts(
        positions, faces(), corner_verts, vert_to_face, face_normals, normals);
    return normals;
  }

  float3 BlenderFaceCenter(const int face) const
  {
    return blender::bke::mesh::face_center_calc(
        positions, Span<int>(corner_verts).slice(faces()[face]));
  }

 private:
  struct PositionFunction {
    const TestMesh *mesh;
    bool operator()(const int vert, float3 &r_position) const
    {
      return mesh->Position(vert, r_position);
    }
  } position_function{this};
};

float3 F3(const MT_Vector3 &value)
{
  return float3(value.x(), value.y(), value.z());
}

void ExpectNear(const float3 &actual, const float3 &expected, const float tolerance = 1e-6f)
{
  EXPECT_NEAR(actual.x, expected.x, tolerance);
  EXPECT_NEAR(actual.y, expected.y, tolerance);
  EXPECT_NEAR(actual.z, expected.z, tolerance);
}

std::vector<LN_MeshRaySample> PickFaces(const TestMesh &mesh, const size_t max_rays)
{
  const LN_MeshRayGeometry geometry = mesh.Geometry();
  return LN_PickMeshRaySamples(
      uint64_t(mesh.faces().size()), max_rays, [&](const uint64_t face, LN_MeshRaySample &r) {
        return LN_MeshFaceRaySample(geometry, int(face), r);
      });
}

std::vector<LN_MeshRaySample> PickVertices(const TestMesh &mesh, const size_t max_rays)
{
  const LN_MeshRayGeometry geometry = mesh.Geometry();
  return LN_PickMeshRaySamples(
      uint64_t(mesh.positions.size()), max_rays, [&](const uint64_t vert, LN_MeshRaySample &r) {
        return LN_MeshVertexRaySample(geometry, int(vert), r);
      });
}

/** The user's test plane (RayCastFromMeshTest.blend, Plane.001): four quads, eight triangles. */
TestMesh UserPlane()
{
  return TestMesh({{-1, -1, 0},
                   {1, -1, 0},
                   {-1, 1, 0},
                   {1, 1, 0},
                   {-1, 0, 0},
                   {0, -1, 0},
                   {1, 0, 0},
                   {0, 1, 0},
                   {0, 0, 0}},
                  {{8, 6, 3, 7}, {4, 8, 7, 2}, {0, 5, 8, 4}, {5, 1, 6, 8}});
}

/** A cube of eight vertices and six quads, wound outwards (Blender's default cube). */
TestMesh Cube()
{
  return TestMesh({{1, 1, 1},
                   {1, 1, -1},
                   {1, -1, 1},
                   {1, -1, -1},
                   {-1, 1, 1},
                   {-1, 1, -1},
                   {-1, -1, 1},
                   {-1, -1, -1}},
                  {{0, 4, 6, 2}, {3, 2, 6, 7}, {7, 6, 4, 5}, {5, 1, 3, 7}, {1, 0, 2, 3}, {5, 4, 0, 1}});
}

}  // namespace

TEST(LN_MeshRays, FaceRaysStartAtTheFaceCentresBlenderShowsNotAtTriangleCentres)
{
  const TestMesh plane = UserPlane();
  const std::vector<LN_MeshRaySample> four = PickFaces(plane, 4);
  ASSERT_EQ(four.size(), 4u);
  const float3 centres[4] = {{0.5f, 0.5f, 0}, {-0.5f, 0.5f, 0}, {-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}};
  for (int i = 0; i < 4; i++) {
    ExpectNear(F3(four[i].position), centres[i]);
    ExpectNear(F3(four[i].normal), float3(0, 0, 1));
  }

  /* Three rays on four faces: faces 0, 1 and 2, each from its own centre. */
  const std::vector<LN_MeshRaySample> three = PickFaces(plane, 3);
  ASSERT_EQ(three.size(), 3u);
  for (int i = 0; i < 3; i++) {
    ExpectNear(F3(three[i].position), centres[i]);
  }

  /* As many rays as faces at most: 256 gives four, never one per triangle. */
  EXPECT_EQ(PickFaces(plane, 256).size(), 4u);
}

TEST(LN_MeshRays, FaceRaysMatchBlendersCentresAndNormalsForEveryFaceSize)
{
  /* A triangle, a bent (non-planar) quad, a pentagon and a tilted hexagon. */
  std::vector<float3> positions = {{0, 0, 0},
                                   {1, 0, 0},
                                   {0, 1, 0},
                                   /* quad */
                                   {2, 0, 0},
                                   {3, 0, 0.4f},
                                   {3, 1, 0},
                                   {2, 1, 0.3f},
                                   /* pentagon */
                                   {5, 0, 0},
                                   {6, 0.2f, 0},
                                   {6.3f, 1, 0},
                                   {5.5f, 1.6f, 0},
                                   {4.8f, 0.9f, 0}};
  for (int i = 0; i < 6; i++) {
    const float angle = float(i) * 1.04719755f; /* 60 degrees */
    positions.push_back(float3(8.0f + std::cos(angle), std::sin(angle), 0.5f * std::cos(angle)));
  }
  const TestMesh mesh(positions,
                      {{0, 1, 2}, {3, 4, 5, 6}, {7, 8, 9, 10, 11}, {12, 13, 14, 15, 16, 17}});
  const std::vector<float3> blender_normals = mesh.BlenderFaceNormals();
  const LN_MeshRayGeometry geometry = mesh.Geometry();
  for (int face = 0; face < 4; face++) {
    LN_MeshRaySample sample;
    ASSERT_TRUE(LN_MeshFaceRaySample(geometry, face, sample)) << "face " << face;
    ExpectNear(F3(sample.position), mesh.BlenderFaceCenter(face));
    ExpectNear(F3(sample.normal), blender_normals[face]);
  }
}

TEST(LN_MeshRays, VertexRaysFollowBlendersVertexNormalsOnAFlatShadedCube)
{
  /* Shading plays no part: a cube's corner ray points out of the corner, not along one of its
   * faces (the game mesh's flat-shaded vertex normal was one face's normal). */
  const TestMesh cube = Cube();
  const std::vector<float3> blender_normals = cube.BlenderVertexNormals();
  const std::vector<LN_MeshRaySample> samples = PickVertices(cube, 256);
  ASSERT_EQ(samples.size(), 8u);
  for (int vert = 0; vert < 8; vert++) {
    ExpectNear(F3(samples[vert].position), cube.positions[vert]);
    ExpectNear(F3(samples[vert].normal), blender_normals[vert]);
    ExpectNear(F3(samples[vert].normal), blender::math::normalize(cube.positions[vert]), 1e-5f);
  }

  /* Faces of the same cube point straight out of each side. */
  const std::vector<LN_MeshRaySample> faces = PickFaces(cube, 256);
  ASSERT_EQ(faces.size(), 6u);
  for (const LN_MeshRaySample &face : faces) {
    ExpectNear(F3(face.normal), F3(face.position));
  }
}

TEST(LN_MeshRays, VertexRaysMatchBlenderOnAnUnevenMesh)
{
  const TestMesh plane = UserPlane();
  TestMesh bent(plane.positions, {{8, 6, 3, 7}, {4, 8, 7, 2}, {0, 5, 8, 4}, {5, 1, 6, 8}});
  bent.positions[8] = float3(0.1f, -0.2f, 0.45f);
  bent.positions[3] = float3(1.0f, 1.2f, -0.3f);
  const std::vector<float3> blender_normals = bent.BlenderVertexNormals();
  const LN_MeshRayGeometry geometry = bent.Geometry();
  for (int vert = 0; vert < 9; vert++) {
    LN_MeshRaySample sample;
    ASSERT_TRUE(LN_MeshVertexRaySample(geometry, vert, sample)) << "vertex " << vert;
    ExpectNear(F3(sample.normal), blender_normals[vert]);
  }
}

TEST(LN_MeshRays, FacesWithoutAreaAndLooseOrMissingVerticesGiveNoRay)
{
  /* Face 1 has no area (its corners lie on a line); vertex 9 belongs to no face. */
  TestMesh mesh({{0, 0, 0},
                 {1, 0, 0},
                 {0, 1, 0},
                 {2, 0, 0},
                 {3, 0, 0},
                 {4, 0, 0},
                 {5, 0, 0},
                 {6, 0, 0},
                 {5, 1, 0},
                 {9, 9, 9}},
                {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}});
  const LN_MeshRayGeometry geometry = mesh.Geometry();
  LN_MeshRaySample sample;
  EXPECT_TRUE(LN_MeshFaceRaySample(geometry, 0, sample));
  EXPECT_FALSE(LN_MeshFaceRaySample(geometry, 1, sample));
  EXPECT_TRUE(LN_MeshFaceRaySample(geometry, 2, sample));
  EXPECT_FALSE(LN_MeshFaceRaySample(geometry, 3, sample));
  EXPECT_FALSE(LN_MeshFaceRaySample(geometry, -1, sample));
  EXPECT_FALSE(LN_MeshVertexRaySample(geometry, 9, sample));
  EXPECT_FALSE(LN_MeshVertexRaySample(geometry, 10, sample));

  /* One ray over three faces: the middle face gives none, so its bin takes the face before it. */
  const std::vector<LN_MeshRaySample> one = PickFaces(mesh, 1);
  ASSERT_EQ(one.size(), 1u);
  ExpectNear(F3(one[0].position), mesh.BlenderFaceCenter(0));
  /* Three rays: the empty face's bin has nothing else, so two rays. */
  EXPECT_EQ(PickFaces(mesh, 3).size(), 2u);

  /* A vertex the reader cannot place takes every face and vertex ray that needs it. */
  mesh.missing = {1};
  EXPECT_FALSE(LN_MeshFaceRaySample(mesh.Geometry(), 0, sample));
  EXPECT_FALSE(LN_MeshVertexRaySample(mesh.Geometry(), 0, sample));
  EXPECT_FALSE(LN_MeshVertexRaySample(mesh.Geometry(), 1, sample));
  EXPECT_TRUE(LN_MeshFaceRaySample(mesh.Geometry(), 2, sample));
}

TEST(LN_MeshRays, MaxRaysPicksAnEvenSpreadInElementOrder)
{
  const auto picked = [](const uint64_t count, const size_t max_rays) {
    std::vector<uint64_t> indices;
    const std::vector<LN_MeshRaySample> samples = LN_PickMeshRaySamples(
        count, max_rays, [&](const uint64_t index, LN_MeshRaySample &r_sample) {
          indices.push_back(index);
          r_sample.position = MT_Vector3(float(index), 0.0f, 0.0f);
          r_sample.normal = MT_Vector3(0.0f, 0.0f, 1.0f);
          return true;
        });
    EXPECT_EQ(samples.size(), indices.size());
    return indices;
  };
  EXPECT_EQ(picked(4, 3), (std::vector<uint64_t>{0, 1, 2}));
  EXPECT_EQ(picked(4, 4), (std::vector<uint64_t>{0, 1, 2, 3}));
  EXPECT_EQ(picked(4, 256), (std::vector<uint64_t>{0, 1, 2, 3}));
  EXPECT_EQ(picked(8, 3), (std::vector<uint64_t>{0, 3, 6}));
  EXPECT_EQ(picked(10, 4), (std::vector<uint64_t>{0, 3, 5, 8}));
  EXPECT_EQ(picked(0, 4), (std::vector<uint64_t>{}));
  EXPECT_EQ(picked(5, 1), (std::vector<uint64_t>{2}));
}

TEST(LN_MeshRays, SelectionByValueKeepsWhatIsAboveZero)
{
  const std::vector<float> values = {0.0f, 0.5f, -1.0f, std::nanf(""), 1.0f, 1e-6f};
  EXPECT_EQ(LN_MeshRaySelectByValue(values), (std::vector<int>{1, 4, 5}));

  /* Faces count only when every corner is in: the upper row of the user's plane. */
  const TestMesh plane = UserPlane();
  std::vector<float> upper(9, 0.0f);
  for (const int vert : {2, 3, 4, 6, 7, 8}) {
    upper[vert] = 1.0f;
  }
  EXPECT_EQ(LN_MeshRaySelectFacesByVertices(plane.faces(), plane.corner_verts, upper),
            (std::vector<int>{0, 1}));
  upper[8] = 0.0f; /* The centre vertex leaves: no face is wholly in any more. */
  EXPECT_EQ(LN_MeshRaySelectFacesByVertices(plane.faces(), plane.corner_verts, upper),
            (std::vector<int>{}));
}

namespace {

/** The user's plane as a Blender mesh (outside Main) with vertex groups "Upper" (the upper row's
 * vertices, weight 1; vertex 5 is in it with weight 0) and "Corner" (vertex 0, weight 0.3), and
 * a Boolean face attribute "front" on faces 2 and 3. Freed by the caller (BKE_id_free). */
blender::Mesh *WeightedPlane()
{
  using namespace blender;
  const TestMesh plane = UserPlane();
  Mesh *mesh = BKE_mesh_new_nomain(9, 0, 4, 16);
  mesh->vert_positions_for_write().copy_from(plane.positions);
  mesh->face_offsets_for_write().copy_from(plane.face_offsets);
  mesh->corner_verts_for_write().copy_from(plane.corner_verts);
  MutableSpan<MDeformVert> dverts = mesh->deform_verts_for_write();
  for (const int vert : {2, 3, 4, 6, 7, 8}) {
    BKE_defvert_add_index_notest(&dverts[vert], 0, 1.0f);
  }
  BKE_defvert_add_index_notest(&dverts[5], 0, 0.0f);
  BKE_defvert_add_index_notest(&dverts[0], 1, 0.3f);
  for (const char *name : {"Upper", "Corner"}) {
    bDeformGroup *group = MEM_new<bDeformGroup>(__func__);
    STRNCPY(group->name, name);
    BLI_addtail(&mesh->vertex_group_names, group);
  }
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<bool> front = attributes.lookup_or_add_for_write_span<bool>(
      "front", bke::AttrDomain::Face);
  front.span[2] = true;
  front.span[3] = true;
  front.finish();
  mesh->tag_topology_changed();
  return mesh;
}

/** Blender meshes need Blender's data types and ID types, set up once in the process. Not
 * `bke::BlenderGTestBase`: its teardown aborts in this build ("pointer not in memlist", Blender's
 * own ArmatureDeform tests too, 2026-11-10), which would end the run before later suites. */
class LN_MeshRaysBlender : public testing::Test {
 public:
  static void SetUpTestSuite()
  {
    static std::once_flag once;
    std::call_once(once, [] {
      blender::BKE_idtype_init();
      blender::BKE_cpp_types_init();
    });
  }
};

}  // namespace

TEST_F(LN_MeshRaysBlender, AVertexGroupOrAttributeOfABlenderMeshSelectsVerticesAndWholeFaces)
{
  blender::Mesh *mesh = WeightedPlane();
  std::vector<int> members;
  ASSERT_TRUE(LN_MeshRaySelectGroup(*mesh, "Upper", false, members));
  EXPECT_EQ(members, (std::vector<int>{2, 3, 4, 6, 7, 8}));
  ASSERT_TRUE(LN_MeshRaySelectGroup(*mesh, "Upper", true, members));
  EXPECT_EQ(members, (std::vector<int>{0, 1}));
  ASSERT_TRUE(LN_MeshRaySelectGroup(*mesh, "Corner", false, members));
  EXPECT_EQ(members, (std::vector<int>{0}));
  ASSERT_TRUE(LN_MeshRaySelectGroup(*mesh, "Corner", true, members));
  EXPECT_EQ(members, (std::vector<int>{}));
  /* A face attribute picks faces by its own value. */
  ASSERT_TRUE(LN_MeshRaySelectGroup(*mesh, "front", true, members));
  EXPECT_EQ(members, (std::vector<int>{2, 3}));
  EXPECT_FALSE(LN_MeshRaySelectGroup(*mesh, "Nope", false, members));
  EXPECT_FALSE(LN_MeshRaySelectGroup(*mesh, "Nope", true, members));
  BKE_id_free(nullptr, mesh);

  /* A face attribute on five sides of a cube: the sixth face is not in it, although each of its
   * corners touches a marked face (read through the vertices it would be). */
  using namespace blender;
  const TestMesh cube_lists = Cube();
  Mesh *cube = BKE_mesh_new_nomain(8, 0, 6, 24);
  cube->vert_positions_for_write().copy_from(cube_lists.positions);
  cube->face_offsets_for_write().copy_from(cube_lists.face_offsets);
  cube->corner_verts_for_write().copy_from(cube_lists.corner_verts);
  bke::SpanAttributeWriter<bool> sides =
      cube->attributes_for_write().lookup_or_add_for_write_span<bool>("sides",
                                                                       bke::AttrDomain::Face);
  for (const int face : {1, 2, 3, 4, 5}) {
    sides.span[face] = true;
  }
  sides.finish();
  cube->tag_topology_changed();
  ASSERT_TRUE(LN_MeshRaySelectGroup(*cube, "sides", true, members));
  EXPECT_EQ(members, (std::vector<int>{1, 2, 3, 4, 5}));
  BKE_id_free(nullptr, cube);
}

TEST_F(LN_MeshRaysBlender, TheGroupCacheScansAMeshOncePerSourceGroupAndDomain)
{
  blender::Mesh *mesh = WeightedPlane();
  LN_MeshRayGroupCache cache;
  const auto faces = cache.Members(7, true, *mesh, "Upper", true, "Plane");
  ASSERT_NE(faces, nullptr);
  EXPECT_EQ(*faces, (std::vector<int>{0, 1}));
  EXPECT_EQ(cache.Members(7, true, *mesh, "Upper", true, "Plane"), faces);
  EXPECT_EQ(cache.ScanCount(), 1u);
  /* Another domain, group, source or kind of source is its own entry. */
  EXPECT_EQ(*cache.Members(7, true, *mesh, "Upper", false, "Plane"),
            (std::vector<int>{2, 3, 4, 6, 7, 8}));
  EXPECT_EQ(cache.ScanCount(), 2u);
  EXPECT_NE(cache.Members(7, false, *mesh, "Upper", true, "Plane"), faces);
  EXPECT_NE(cache.Members(8, true, *mesh, "Upper", true, "Plane"), faces);
  EXPECT_EQ(cache.ScanCount(), 4u);
  /* A missing group is remembered as missing too. */
  EXPECT_EQ(cache.Members(7, true, *mesh, "Nope", true, "Plane"), nullptr);
  EXPECT_EQ(cache.Members(7, true, *mesh, "Nope", true, "Plane"), nullptr);
  EXPECT_EQ(cache.ScanCount(), 5u);
  /* No session UID: never kept. */
  cache.Members(0, true, *mesh, "Upper", true, "Plane");
  cache.Members(0, true, *mesh, "Upper", true, "Plane");
  EXPECT_EQ(cache.ScanCount(), 7u);

  /* A source whose topology changed (other counts) is scanned again. */
  blender::Mesh *smaller = blender::BKE_mesh_new_nomain(4, 0, 1, 4);
  smaller->face_offsets_for_write().copy_from({0, 4});
  smaller->corner_verts_for_write().copy_from({0, 1, 2, 3});
  EXPECT_EQ(cache.Members(7, true, *smaller, "Upper", true, "Plane"), nullptr);
  EXPECT_EQ(cache.ScanCount(), 8u);
  EXPECT_EQ(*cache.Members(7, true, *mesh, "Upper", true, "Plane"), (std::vector<int>{0, 1}));
  EXPECT_EQ(cache.ScanCount(), 9u);
  BKE_id_free(nullptr, smaller);
  BKE_id_free(nullptr, mesh);
}

/* Bench (--gtest_also_run_disabled_tests): the Vertex Group scan of a grid of N x N vertices
 * (half of them in the group), done once per mesh and group, against a remembered lookup. */
TEST_F(LN_MeshRaysBlender, DISABLED_BenchGroupScan)
{
  using namespace blender;
  for (const int side : {100, 317, 1000}) {
    const int verts = side * side;
    const int faces = (side - 1) * (side - 1);
    Mesh *mesh = BKE_mesh_new_nomain(verts, 0, faces, faces * 4);
    MutableSpan<int> offsets = mesh->face_offsets_for_write();
    MutableSpan<int> corners = mesh->corner_verts_for_write();
    int face = 0;
    for (int y = 0; y + 1 < side; y++) {
      for (int x = 0; x + 1 < side; x++, face++) {
        offsets[face] = face * 4;
        const int v = y * side + x;
        corners[face * 4 + 0] = v;
        corners[face * 4 + 1] = v + 1;
        corners[face * 4 + 2] = v + side + 1;
        corners[face * 4 + 3] = v + side;
      }
    }
    offsets[faces] = faces * 4;
    MutableSpan<MDeformVert> dverts = mesh->deform_verts_for_write();
    for (int v = 0; v < verts / 2; v++) {
      BKE_defvert_add_index_notest(&dverts[v], 0, 1.0f);
    }
    bDeformGroup *group = MEM_new<bDeformGroup>(__func__);
    STRNCPY(group->name, "Half");
    BLI_addtail(&mesh->vertex_group_names, group);
    mesh->tag_topology_changed();

    for (const bool by_faces : {false, true}) {
      LN_MeshRayGroupCache cache;
      const auto start = std::chrono::steady_clock::now();
      const auto members = cache.Members(1, true, *mesh, "Half", by_faces, "Grid");
      const auto scanned = std::chrono::steady_clock::now();
      constexpr int kLookups = 1000;
      for (int i = 0; i < kLookups; i++) {
        EXPECT_EQ(cache.Members(1, true, *mesh, "Half", by_faces, "Grid"), members);
      }
      const auto looked_up = std::chrono::steady_clock::now();
      const double scan_ms = std::chrono::duration<double, std::milli>(scanned - start).count();
      const double lookup_us =
          std::chrono::duration<double, std::micro>(looked_up - scanned).count() / kLookups;
      printf("LN_MESH_RAYS_BENCH %8d verts %-8s members %8zu  scan %.3f ms  lookup %.3f us\n",
             verts,
             by_faces ? "faces" : "vertices",
             members->size(),
             scan_ms,
             lookup_us);
    }
    BKE_id_free(nullptr, mesh);
  }
}
