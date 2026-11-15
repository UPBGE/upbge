/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The raycast recommendations (2026-11-11) end to end: Logic Nodes trees (built by idname, run in
 * the Logic Nodes test rig) whose rays, shape casts and sweeps hit bodies of a real Jolt world
 * (LN_SetPhysicsEnvironmentForTests). Hit Part, Hit Material, Shape Cast's Overlap, Projectile
 * Ray's new inputs and outputs, Line of Sight, Sweep Object and the Visualize panel. Mouse Ray and
 * Camera Ray need a camera and a canvas: the game probe carries them.
 */

#include "testing/testing.h"

#include <Jolt/Jolt.h>
JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "EXP_Vector3Value.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "LN_CommandDiagnostics.h"
#include "LN_RuntimeTree.h"
#include "LN_SemRig.h"
#include "LN_StrictDump.h"
#include "SG_Node.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

/* -------------------------------------------------------------------- */
/** \name A Jolt world for the rig's objects
 * \{ */

/** A body of a rig object: its controller and client info live here. */
struct Body {
  Body(JoltPhysicsEnvironment &environment,
       KX_GameObject &object,
       const JPH::RefConst<JPH::Shape> &shape,
       const JPH::EMotionType motion,
       const bool sensor,
       const unsigned short layer,
       const JoltShapeQueryDataPtr &query_data)
      : environment(environment),
        object(object),
        client_info(&object,
                    sensor                                ? KX_ClientObjectInfo::SENSOR :
                    motion == JPH::EMotionType::Static ? KX_ClientObjectInfo::STATIC :
                                                            KX_ClientObjectInfo::ACTOR)
  {
    const JoltBroadPhaseLayer category = motion == JPH::EMotionType::Static ? JOLT_BP_STATIC :
                                                                              JOLT_BP_DYNAMIC;
    JPH::BodyCreationSettings settings(
        shape,
        JoltMath::ToJoltR(object.NodeGetWorldPosition()),
        JoltMath::ToJolt(object.NodeGetWorldOrientation().getRotation()),
        motion,
        JoltMakeObjectLayer(layer, layer, category));
    settings.mIsSensor = sensor;
    body_id = environment.GetBodyInterface().CreateAndAddBody(settings,
                                                              JPH::EActivation::DontActivate);
    controller.SetEnvironment(&environment);
    controller.SetBodyID(body_id);
    controller.SetDynamic(motion == JPH::EMotionType::Dynamic);
    controller.SetKinematic(motion == JPH::EMotionType::Kinematic);
    controller.SetOriginalMotionType(motion);
    controller.SetBroadPhaseCategory(category);
    controller.SetShape(shape, query_data);
    controller.SetNewClientInfo(&client_info);
    controller.SetCollisionGroup(layer);
    object.SetPhysicsController(&controller);
    environment.GetBodyInterface().SetUserData(body_id,
                                               reinterpret_cast<JPH::uint64>(&client_info));
  }
  ~Body()
  {
    /* (The controller takes its body out of the world: a cleared body is not hit any more.) */
    object.SetPhysicsController(nullptr);
    controller.SetNewClientInfo(nullptr);
  }

  JoltPhysicsEnvironment &environment;
  KX_GameObject &object;
  KX_ClientObjectInfo client_info;
  JoltPhysicsController controller;
  JPH::BodyID body_id;
};

/** A Blender object and mesh holding material slots (the game object's materials), and the
 * Blender object a constant Object pin names. */
struct BlenderObject {
  explicit BlenderObject(const char *name, const std::vector<blender::Material *> &materials = {})
  {
    std::snprintf(object.id.name, sizeof(object.id.name), "OB%s", name);
    std::snprintf(mesh.id.name, sizeof(mesh.id.name), "ME%s", name);
    slots = materials;
    bits.assign(std::max<size_t>(materials.size(), 1), 0);
    object.type = blender::OB_MESH;
    object.data = reinterpret_cast<blender::ID *>(&mesh);
    object.totcol = short(materials.size());
    object.mat = materials.empty() ? nullptr : object_slots.data();
    object_slots.assign(std::max<size_t>(materials.size(), 1), nullptr);
    object.mat = object_slots.data();
    object.matbits = bits.data();
    mesh.totcol = short(materials.size());
    mesh.mat = slots.empty() ? nullptr : slots.data();
  }
  blender::Object object{};
  blender::Mesh mesh{};
  std::vector<blender::Material *> slots;
  std::vector<blender::Material *> object_slots;
  std::vector<char> bits;
};

struct Material {
  explicit Material(const char *name)
  {
    std::snprintf(material.id.name, sizeof(material.id.name), "MA%s", name);
  }
  blender::Material material{};
};

/** The world: queries of every tree ask it while it lives. Gravity none: nothing moves. */
class World {
 public:
  /** `max_bodies`: the room of Jolt's body, pair and contact tables (the benchmarks fill more);
   * `helpers`: the helper threads of Jolt's job system, beside the calling thread (1 as the
   * tests always had; 0: the caller alone, nothing runs in parallel). */
  explicit World(const int max_bodies = 256, const int helpers = 1)
      : environment(&scene, helpers, max_bodies, max_bodies, max_bodies, 8, false)
  {
    environment.GetPhysicsSystem()->SetGravity(JPH::Vec3::sZero());
    LN_SetPhysicsEnvironmentForTests(&environment);
  }
  ~World()
  {
    bodies.clear();
    LN_SetPhysicsEnvironmentForTests(nullptr);
  }

  /** A box (Blender half extents) for `object`, where it is. */
  Body &Box(KX_GameObject &object,
            const MT_Vector3 &half,
            const JPH::EMotionType motion = JPH::EMotionType::Static,
            const bool sensor = false,
            const unsigned short layer = 1)
  {
    const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(
        JPH::Vec3(half.x(), half.z(), half.y()), 0.0f);
    return Add(object, shape, motion, sensor, layer, nullptr);
  }

  Body &Add(KX_GameObject &object,
            const JPH::RefConst<JPH::Shape> &shape,
            const JPH::EMotionType motion,
            const bool sensor,
            const unsigned short layer,
            const JoltShapeQueryDataPtr &query_data)
  {
    bodies.push_back(
        std::make_unique<Body>(environment, object, shape, motion, sensor, layer, query_data));
    return *bodies.back();
  }

  blender::Scene scene{};
  JoltPhysicsEnvironment environment;
  std::deque<std::unique_ptr<Body>> bodies;
};

/** A unit quad in the object's XY plane (Blender), its two triangles (user data 0: x + y < 1 in
 * Jolt's x and z, 1: the other half) with material slots 0 and 1. */
JPH::RefConst<JPH::Shape> QuadWithTwoSlots(JoltShapeQueryDataPtr &r_query_data)
{
  JPH::VertexList vertices = {JPH::Float3(0.0f, 0.0f, 0.0f),
                              JPH::Float3(0.0f, 0.0f, 1.0f),
                              JPH::Float3(1.0f, 0.0f, 0.0f),
                              JPH::Float3(1.0f, 0.0f, 1.0f)};
  JPH::IndexedTriangleList triangles = {JPH::IndexedTriangle(0, 1, 2, 0, 0),
                                        JPH::IndexedTriangle(2, 1, 3, 0, 1)};
  JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
  settings.mPerTriangleUserData = true;
  const JPH::RefConst<JPH::Shape> shape = settings.Create().Get();
  std::shared_ptr<JoltMeshQueryData> mesh_data = std::make_shared<JoltMeshQueryData>();
  mesh_data->triangleMaterials = {0, 1};
  std::shared_ptr<JoltShapeQueryData> query_data = std::make_shared<JoltShapeQueryData>();
  query_data->Add(shape.GetPtr(), mesh_data);
  r_query_data = query_data;
  return shape;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Trees: [On Logic + Physics Step] -> the node -> one Set Game Property per output
 * \{ */

class Tree {
 public:
  Tree()
  {
    /* The editor's sockets for the Collision Layers (Mask) pins. */
    b.SetRealSockets(true);
    step = &b.Add("LogicNativeOnFixedUpdate");
    flow = step;
    flow_pin = "Out";
  }

  GenNode &Add(const char *idname, const int16_t c1 = 0, const int16_t c2 = 0)
  {
    return b.Add(idname, c1, c2);
  }

  /** `node.pin` (a Float, Int read as Float, or Bool read as 0/1) into the owner's property. */
  void Store(GenNode &node, const char *pin, const char *property, const char *setter)
  {
    GenNode &writer = b.Add(setter);
    b.String(writer, "Property", property);
    b.Link(*flow, flow_pin, writer, "Flow");
    b.Link(node, pin, writer, "Value");
    flow = &writer;
    flow_pin = "Done";
  }
  void StoreFloat(GenNode &node, const char *pin, const char *property)
  {
    Store(node, pin, property, "LogicNativeSetGamePropertyFloat");
  }
  void StoreBool(GenNode &node, const char *pin, const char *property)
  {
    Store(node, pin, property, "LogicNativeSetGamePropertyBool");
  }
  /** Text: an Object output gives its name, a String its text. */
  void StoreText(GenNode &node, const char *pin, const char *property)
  {
    Store(node, pin, property, "LogicNativeSetGamePropertyString");
  }
  /** Item `index` of a list output, as text. */
  void StoreItem(GenNode &node, const char *pin, const int index, const char *property)
  {
    GenNode &item = b.Add("LogicNativeListGetItem");
    b.Link(node, pin, item, "List");
    b.Int(item, "Index", index);
    StoreText(item, "Value", property);
  }
  void StoreCount(GenNode &node, const char *pin, const char *property)
  {
    GenNode &length = b.Add("LogicNativeListLength");
    b.Link(node, pin, length, "List");
    Store(length, "Length", property, "LogicNativeSetGamePropertyInt");
  }
  /** A Vector output into "<property>X", "<property>Y", "<property>Z". */
  void StoreVector(GenNode &node, const char *pin, const std::string &property)
  {
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(node, pin, split, "Vector");
    for (const char *axis : {"X", "Y", "Z"}) {
      names.push_back(property + axis);
      StoreFloat(split, axis, names.back().c_str());
    }
  }
  /** A list of objects named by constant Object pins (Get Object into List From Items' Objects,
   * a multi-input). */
  GenNode &ObjectList(const std::vector<blender::Object *> &objects)
  {
    GenNode &list = b.Add("LogicNativeListFromItems", 10);
    for (blender::Object *object : objects) {
      GenNode &get = b.Add("LogicNativeFindObject");
      b.ObjectNamed(get, "Object", object);
      b.Link(get, "Object", list, "Objects");
    }
    return list;
  }

  std::shared_ptr<LN_Program> Compile()
  {
    std::string report;
    std::shared_ptr<LN_Program> program = b.Compile(&report);
    EXPECT_NE(program, nullptr) << report;
    return program;
  }

  TreeBuilder b;
  GenNode *step = nullptr;
  GenNode *flow = nullptr;
  const char *flow_pin = nullptr;
  std::deque<std::string> names;
};

MT_Vector3 ReadVector(SemRig &rig, KX_GameObject &object, const std::string &property)
{
  return MT_Vector3(rig.Float(object, (property + "X").c_str()),
                    rig.Float(object, (property + "Y").c_str()),
                    rig.Float(object, (property + "Z").c_str()));
}

void ExpectVector(const MT_Vector3 &actual, const MT_Vector3 &expected, const float tolerance)
{
  EXPECT_NEAR(actual.x(), expected.x(), tolerance);
  EXPECT_NEAR(actual.y(), expected.y(), tolerance);
  EXPECT_NEAR(actual.z(), expected.z(), tolerance);
}

/** Every mode of the rig: the casts are the same in each. */
const std::vector<SemMode> &Modes()
{
  static const std::vector<SemMode> modes = {kProduction, kMemoOff, kProfiled};
  return modes;
}

/** \} */

/** A compound "Tank" (at x = 5) of a small root box 2 below its origin and a "Turret" child box
 * (half 0.5) at its origin, with the Turret bound as its compound child. */
struct Tank {
  Tank(SemRig &rig, World &world, const MT_Vector3 &position)
  {
    tank_index = rig.ObjectCount();
    rig.AddObject("Tank", position);
    turret_index = rig.ObjectCount();
    rig.AddObject("Turret", position);
    rig.Parent(turret_index, tank_index);
    KX_GameObject &tank = rig.Object(tank_index);
    KX_GameObject &turret = rig.Object(turret_index);
    Body &body = world.Box(tank, MT_Vector3(0.1f, 0.1f, 0.1f), JPH::EMotionType::Dynamic);
    const JPH::uint32 binding = body.controller.RegisterCompoundChildBinding(
        &turret, JPH::Vec3::sZero(), JPH::Quat::sIdentity());
    JPH::StaticCompoundShapeSettings compound;
    compound.AddShape(JoltMath::ToJolt(MT_Vector3(0.0f, 0.0f, -2.0f)),
                      JPH::Quat::sIdentity(),
                      new JPH::BoxShape(JPH::Vec3::sReplicate(0.1f)),
                      0);
    compound.AddShape(
        JPH::Vec3::sZero(), JPH::Quat::sIdentity(), new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f)),
        binding);
    body.controller.SetShapePreservingMassPropertiesAndCenterOfMass(compound.Create().Get(),
                                                                   nullptr);
    valid = binding != 0;
  }
  size_t tank_index = 0;
  size_t turret_index = 0;
  bool valid = false;
};

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Hit Part and Hit Material
 * \{ */

TEST(JoltLogicNodes, RaycastGivesTheCompoundChildHitAsHitPartAndEachHitsMaterial)
{
  Material wood("Wood");
  BlenderObject crate_blender("Crate", {&wood.material});
  Tree tree;
  GenNode &ray = tree.Add("LogicNativeRaycast", 1 /* Direction + Distance */);
  tree.b.Vector(ray, "Direction", 1.0f, 0.0f, 0.0f);
  tree.b.Float(ray, "Max Distance", 50.0f);
  tree.StoreText(ray, "Picked Object", "object"); /* (identifiers: Hit Object is "Picked Object") */
  tree.StoreText(ray, "Hit Part", "part");
  tree.StoreText(ray, "Hit Material", "material");
  GenNode &all = tree.Add("LogicNativeRaycastAll", 1);
  tree.b.Vector(all, "Direction", 1.0f, 0.0f, 0.0f);
  tree.b.Float(all, "Max Distance", 50.0f);
  tree.StoreCount(all, "Hit Parts", "parts");
  tree.StoreItem(all, "Hit Parts", 0, "part0");
  tree.StoreItem(all, "Hit Parts", 1, "part1");
  tree.StoreItem(all, "Hit Objects", 0, "object0");
  tree.StoreItem(all, "Hit Materials", 0, "material0");
  tree.StoreItem(all, "Hit Materials", 1, "material1");
  /* The same ray with no Hit Part / Hit Material linked asks the physics for neither. */
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  EXPECT_NE(program->GetDependencySummary().ray_query_detail_requirements &
                LN_RAY_QUERY_DETAIL_PART,
            0);
  EXPECT_NE(program->GetDependencySummary().ray_query_detail_requirements &
                LN_RAY_QUERY_DETAIL_MATERIAL,
            0);

  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    /* After the rig: its bodies go first (a game object deletes its controller). */
    World world;
    Tank tank(rig, world, MT_Vector3(5.0f, 0.0f, 0.0f));
    ASSERT_TRUE(tank.valid);
    KX_GameObject &crate = rig.AddObject("Crate", MT_Vector3(10.0f, 0.0f, 0.0f));
    crate.SetBlenderObject(&crate_blender.object);
    world.Box(crate, MT_Vector3(0.5f, 0.5f, 0.5f));
    KX_GameObject &owner = rig.Object(0);
    rig.Step();
    EXPECT_EQ(rig.Text(owner, "object"), "Tank");
    EXPECT_EQ(rig.Text(owner, "part"), "Turret");
    /* The Turret has no material: empty. */
    EXPECT_EQ(rig.Text(owner, "material"), "");
    EXPECT_EQ(rig.Float(owner, "parts"), 2.0f);
    EXPECT_EQ(rig.Text(owner, "object0"), "Tank");
    EXPECT_EQ(rig.Text(owner, "part0"), "Turret");
    /* A plain body is its own part; a box gives its first material. */
    EXPECT_EQ(rig.Text(owner, "part1"), "Crate");
    EXPECT_EQ(rig.Text(owner, "material0"), "");
    EXPECT_EQ(rig.Text(owner, "material1"), "Wood");
    crate.SetBlenderObject(nullptr);
  }
}

TEST(JoltLogicNodes, HitMaterialIsTheMaterialOfTheHitTrianglesSlot)
{
  Material grass("Grass");
  Material stone("Stone");
  BlenderObject floor_blender("Floor", {&grass.material, &stone.material});
  Tree tree;
  /* Two rays straight down, onto each triangle of the quad; a shape cast onto the second. */
  GenNode &first = tree.Add("LogicNativeRaycast", 0 /* End Point */);
  tree.b.Vector(first, "Origin", 0.25f, 0.25f, 1.0f);
  tree.b.Vector(first, "Destination", 0.25f, 0.25f, -1.0f);
  tree.b.Bool(first, "Hit Backfaces", true);
  tree.StoreText(first, "Hit Material", "first");
  tree.Store(first, "Face Index", "first_face", "LogicNativeSetGamePropertyInt");
  GenNode &second = tree.Add("LogicNativeRaycast", 0);
  tree.b.Vector(second, "Origin", 0.75f, 0.75f, 1.0f);
  tree.b.Vector(second, "Destination", 0.75f, 0.75f, -1.0f);
  tree.b.Bool(second, "Hit Backfaces", true);
  tree.StoreText(second, "Hit Material", "second");
  GenNode &cast = tree.Add("LogicNativeShapeCast", 0 /* Sphere */);
  tree.b.Vector(cast, "Origin", 0.8f, 0.8f, 1.0f);
  tree.b.Vector(cast, "Destination", 0.8f, 0.8f, -1.0f);
  tree.b.Float(cast, "Radius", 0.05f);
  tree.b.Bool(cast, "Hit Backfaces", true);
  tree.StoreText(cast, "Hit Material", "cast");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    /* After the rig: its bodies go first (a game object deletes its controller). */
    World world;
    KX_GameObject &floor = rig.AddObject("Floor", MT_Vector3(0.0f, 0.0f, 0.0f));
    floor.SetBlenderObject(&floor_blender.object);
    JoltShapeQueryDataPtr query_data;
    /* Jolt's (x, z) are Blender's (x, -y): the quad lies at y in [-1, 0]; turn it to y >= 0. */
    floor.NodeSetGlobalOrientation(MT_Matrix3x3(MT_Vector3(0.0f, 0.0f, float(M_PI_2))));
    floor.NodeUpdateGS(0.0);
    const JPH::RefConst<JPH::Shape> quad = QuadWithTwoSlots(query_data);
    world.Add(floor, quad, JPH::EMotionType::Static, false, 1, query_data);
    rig.Step();
    KX_GameObject &owner = rig.Object(0);
    /* Which half each point is on depends on the turn; the two rays meet the two slots. */
    const std::string a = rig.Text(owner, "first");
    const std::string c = rig.Text(owner, "second");
    EXPECT_TRUE((a == "Grass" && c == "Stone") || (a == "Stone" && c == "Grass")) << a << " " << c;
    EXPECT_EQ(rig.Text(owner, "cast"), c);
    /* Face Index was not kept for this mesh (only the slots): -1. */
    EXPECT_EQ(rig.Float(owner, "first_face"), -1.0f);
    floor.SetBlenderObject(nullptr);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape Cast: Overlap
 * \{ */

TEST(JoltLogicNodes, ShapeCastOverlapListsWhatTheShapeTouchesWhereItIs)
{
  Tree tree;
  GenNode &all = tree.Add("LogicNativeShapeCastAll", 0 /* Sphere */, 1 /* Overlap */);
  tree.b.Vector(all, "Origin", 0.0f, 0.0f, 0.0f);
  /* Ignored in Overlap. */
  tree.b.Vector(all, "Destination", 100.0f, 0.0f, 0.0f);
  tree.b.Float(all, "Radius", 2.0f);
  tree.StoreCount(all, "Hit Objects", "count");
  GenNode &nearest = tree.Add("LogicNativeShapeCast", 0, 1);
  tree.b.Vector(nearest, "Origin", 0.0f, 0.0f, 0.0f);
  tree.b.Vector(nearest, "Destination", 100.0f, 0.0f, 0.0f);
  tree.b.Float(nearest, "Radius", 2.0f);
  tree.StoreBool(nearest, "Has Result", "hit");
  tree.StoreBool(nearest, "Started Overlapping", "inside");
  /* The same node swept (custom2 0) reaches the far box too. */
  GenNode &sweep = tree.Add("LogicNativeShapeCastAll", 0, 0);
  tree.b.Vector(sweep, "Origin", 0.0f, 0.0f, 0.0f);
  tree.b.Vector(sweep, "Destination", 100.0f, 0.0f, 0.0f);
  tree.b.Float(sweep, "Radius", 2.0f);
  tree.StoreCount(sweep, "Hit Objects", "swept");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    /* After the rig: its bodies go first (a game object deletes its controller). */
    World world;
    world.Box(rig.AddObject("Near", MT_Vector3(1.0f, 0.0f, 0.0f)), MT_Vector3(0.5f, 0.5f, 0.5f));
    world.Box(rig.AddObject("Side", MT_Vector3(0.0f, 2.2f, 0.0f)), MT_Vector3(0.5f, 0.5f, 0.5f));
    world.Box(rig.AddObject("Far", MT_Vector3(20.0f, 0.0f, 0.0f)), MT_Vector3(0.5f, 0.5f, 0.5f));
    rig.Step();
    KX_GameObject &owner = rig.Object(0);
    EXPECT_EQ(rig.Float(owner, "count"), 2.0f);
    EXPECT_EQ(rig.Float(owner, "hit"), 1.0f);
    EXPECT_EQ(rig.Float(owner, "inside"), 1.0f);
    EXPECT_EQ(rig.Float(owner, "swept"), 3.0f);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Projectile Ray
 * \{ */

namespace {

/** A Projectile Ray from (0, 0, 1) heading for (1, 0, 2) at 10 (45 degrees up, along X), in
 * custom gravity (0, 0, -10), steps of 0.01 s, with its outputs stored. */
GenNode &Throw(Tree &tree, const char *prefix, const float radius = 0.0f, const float max_time = 0.0f)
{
  GenNode &ray = tree.Add("LogicNativeProjectileRay", 1 /* Custom Gravity */);
  tree.b.Vector(ray, "Origin", 0.0f, 0.0f, 1.0f);
  tree.b.Vector(ray, "Aim", 1.0f, 0.0f, 2.0f);
  tree.b.Float(ray, "Power", 10.0f);
  tree.b.Float(ray, "Distance", 100.0f);
  tree.b.Float(ray, "Resolution", 0.99f);
  tree.b.Float(ray, "Radius", radius);
  tree.b.Float(ray, "Max Time", max_time);
  tree.b.Vector(ray, "Gravity", 0.0f, 0.0f, -10.0f);
  const std::string p(prefix);
  tree.names.push_back(p + "hit");
  tree.StoreBool(ray, "RESULT", tree.names.back().c_str());
  tree.names.push_back(p + "object");
  tree.StoreText(ray, "PICKED_OBJECT", tree.names.back().c_str());
  tree.names.push_back(p + "time");
  tree.StoreFloat(ray, "Flight Time", tree.names.back().c_str());
  tree.names.push_back(p + "distance");
  tree.StoreFloat(ray, "Distance", tree.names.back().c_str());
  tree.names.push_back(p + "fraction");
  tree.StoreFloat(ray, "Fraction", tree.names.back().c_str());
  tree.StoreVector(ray, "POINT", p + "point");
  tree.StoreVector(ray, "Impact Velocity", p + "velocity");
  tree.names.push_back(p + "points");
  tree.StoreCount(ray, "PARABOLA", tree.names.back().c_str());
  return ray;
}

}  // namespace

TEST(JoltLogicNodes, ProjectileRayGivesFlightTimeImpactVelocityAndItsPath)
{
  Tree tree;
  Throw(tree, "a_");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  /* Where the throw lands on z = 0: 1 + 7.0711 t - 5 t^2 = 0. */
  const float vz = 10.0f / std::sqrt(2.0f);
  const float landing = (vz + std::sqrt(vz * vz + 20.0f)) / 10.0f;
  /* Its path length: the arc's (the segments are 0.01 s chords). */
  float arc = 0.0f;
  for (int i = 0; i < 20000; i++) {
    const float t0 = landing * float(i) / 20000.0f;
    const float t1 = landing * float(i + 1) / 20000.0f;
    const MT_Vector3 p0(vz * t0, 0.0f, 1.0f + vz * t0 - 5.0f * t0 * t0);
    const MT_Vector3 p1(vz * t1, 0.0f, 1.0f + vz * t1 - 5.0f * t1 * t1);
    arc += (p1 - p0).length();
  }
  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    /* After the rig: its bodies go first (a game object deletes its controller). */
    World world;
    world.Box(rig.AddObject("Ground", MT_Vector3(0.0f, 0.0f, -1.0f)),
              MT_Vector3(100.0f, 100.0f, 1.0f));
    rig.Step();
    KX_GameObject &owner = rig.Object(0);
    EXPECT_EQ(rig.Float(owner, "a_hit"), 1.0f);
    EXPECT_EQ(rig.Text(owner, "a_object"), "Ground");
    EXPECT_NEAR(rig.Float(owner, "a_time"), landing, 1.0e-3f);
    ExpectVector(ReadVector(rig, owner, "a_point"), MT_Vector3(vz * landing, 0.0f, 0.0f), 2.0e-3f);
    ExpectVector(ReadVector(rig, owner, "a_velocity"),
                 MT_Vector3(vz, 0.0f, vz - 10.0f * landing),
                 1.0e-2f);
    EXPECT_NEAR(rig.Float(owner, "a_distance"), arc, 1.0e-2f);
    EXPECT_NEAR(rig.Float(owner, "a_fraction"), arc / 100.0f, 1.0e-4f);
    /* The origin, one point a segment, the hit. */
    EXPECT_NEAR(rig.Float(owner, "a_points"), std::ceil(landing / 0.01f) + 1.0f, 1.0f);
  }
}

namespace {

/** A straight throw (no gravity) from the origin along +X at 10, outputs stored with `prefix`. */
GenNode &StraightThrow(Tree &tree,
                       const char *prefix,
                       const float radius = 0.0f,
                       const float max_time = 0.0f)
{
  GenNode &ray = tree.Add("LogicNativeProjectileRay", 1 /* Custom Gravity */);
  tree.b.Vector(ray, "Origin", 0.0f, 0.0f, 0.0f);
  tree.b.Vector(ray, "Aim", 1.0f, 0.0f, 0.0f);
  tree.b.Float(ray, "Power", 10.0f);
  tree.b.Float(ray, "Distance", 100.0f);
  tree.b.Float(ray, "Resolution", 0.9f);
  tree.b.Float(ray, "Radius", radius);
  tree.b.Float(ray, "Max Time", max_time);
  tree.b.Vector(ray, "Gravity", 0.0f, 0.0f, 0.0f);
  const std::string p(prefix);
  tree.names.push_back(p + "hit");
  tree.StoreBool(ray, "RESULT", tree.names.back().c_str());
  tree.names.push_back(p + "blocked");
  tree.StoreBool(ray, "Blocked", tree.names.back().c_str());
  tree.names.push_back(p + "object");
  tree.StoreText(ray, "PICKED_OBJECT", tree.names.back().c_str());
  tree.names.push_back(p + "time");
  tree.StoreFloat(ray, "Flight Time", tree.names.back().c_str());
  tree.names.push_back(p + "distance");
  tree.StoreFloat(ray, "Distance", tree.names.back().c_str());
  tree.StoreVector(ray, "POINT", p + "point");
  return ray;
}

}  // namespace

TEST(JoltLogicNodes, ProjectileRayRadiusMaxTimeFiltersAndGravityChoice)
{
  BlenderObject wall_blender("Wall");
  BlenderObject gate_blender("Gate");
  Tree tree;
  StraightThrow(tree, "thin_");
  StraightThrow(tree, "ball_", 0.3f);
  StraightThrow(tree, "short_", 0.0f, 0.5f);
  GenNode &ignoring = StraightThrow(tree, "ignore_");
  tree.b.ObjectNamed(ignoring, "Ignore Object", &wall_blender.object);
  GenNode &sensing = StraightThrow(tree, "sense_");
  tree.b.Bool(sensing, "Include Sensors", true);
  GenNode &listing = StraightThrow(tree, "list_");
  tree.b.Bool(listing, "Include Sensors", true);
  tree.b.Link(tree.ObjectList({&wall_blender.object, &gate_blender.object}),
              "Objects",
              listing,
              "Ignore Objects");
  GenNode &property = StraightThrow(tree, "property_");
  tree.b.String(property, "Property", "target");
  GenNode &xray = StraightThrow(tree, "xray_");
  tree.b.String(xray, "Property", "target");
  tree.b.Bool(xray, "X-Ray", true);
  /* Scene gravity: none in the rig, so the engine's default (0, 0, -9.8). */
  GenNode &scene = tree.Add("LogicNativeProjectileRay", 0 /* Scene Gravity */);
  tree.b.Vector(scene, "Origin", -50.0f, 0.0f, 1.0f);
  tree.b.Vector(scene, "Aim", -49.0f, 0.0f, 2.0f);
  tree.b.Float(scene, "Power", 10.0f);
  tree.b.Float(scene, "Distance", 100.0f);
  tree.b.Float(scene, "Resolution", 0.99f);
  tree.StoreFloat(scene, "Flight Time", "scene_time");
  /* Done follows Flow. */
  GenNode &flowed = tree.Add("LogicNativeProjectileRay", 1);
  tree.b.Vector(flowed, "Aim", 1.0f, 0.0f, 0.0f);
  tree.b.Link(*tree.step, "Out", flowed, "Flow");
  GenNode &done = tree.Add("LogicNativeSetGamePropertyBool");
  tree.b.String(done, "Property", "done");
  tree.b.Bool(done, "Value", true);
  tree.b.Link(flowed, "Done", done, "Flow");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    /* After the rig: its bodies go first (a game object deletes its controller). */
    World world;
    KX_GameObject &owner = rig.Object(0);
    rig.SetBool(owner, "done", false);
    world.Box(rig.AddObject("Bar", MT_Vector3(5.0f, 0.0f, 0.25f)), MT_Vector3(0.1f, 2.0f, 0.1f));
    KX_GameObject &wall = rig.AddObject("Wall", MT_Vector3(8.0f, 0.0f, 0.0f));
    world.Box(wall, MT_Vector3(0.1f, 5.0f, 5.0f));
    world.Box(rig.AddObject("Gate", MT_Vector3(3.0f, 0.0f, 0.0f)),
              MT_Vector3(0.1f, 2.0f, 2.0f),
              JPH::EMotionType::Static,
              true);
    KX_GameObject &back = rig.AddObject("Back", MT_Vector3(12.0f, 0.0f, 0.0f));
    world.Box(back, MT_Vector3(0.1f, 5.0f, 5.0f));
    rig.SetBool(back, "target", true);
    world.Box(rig.AddObject("Ground", MT_Vector3(-50.0f, 0.0f, -1.0f)),
              MT_Vector3(20.0f, 20.0f, 1.0f));
    rig.Step();

    /* Thin: under the bar, through the sensor, onto the wall's face at x = 7.9. */
    EXPECT_EQ(rig.Float(owner, "thin_hit"), 1.0f);
    EXPECT_EQ(rig.Text(owner, "thin_object"), "Wall");
    EXPECT_NEAR(rig.Float(owner, "thin_time"), 0.79f, 1.0e-4f);
    EXPECT_NEAR(rig.Float(owner, "thin_distance"), 7.9f, 1.0e-4f);
    /* A ball of 0.3 meets the bar's lower edge (x 4.9, z 0.15) with its center at
     * 4.9 - sqrt(0.3^2 - 0.15^2); the path ends at the center, the hit point on the bar. */
    const float ball_center = 4.9f - std::sqrt(0.09f - 0.0225f);
    EXPECT_EQ(rig.Text(owner, "ball_object"), "Bar");
    EXPECT_NEAR(rig.Float(owner, "ball_distance"), ball_center, 2.0e-3f);
    ExpectVector(ReadVector(rig, owner, "ball_point"), MT_Vector3(4.9f, 0.0f, 0.15f), 1.0e-2f);
    /* Max Time 0.5 s: the path ends at x = 5, nothing hit. */
    EXPECT_EQ(rig.Float(owner, "short_hit"), 0.0f);
    EXPECT_NEAR(rig.Float(owner, "short_time"), 0.5f, 1.0e-5f);
    EXPECT_NEAR(rig.Float(owner, "short_distance"), 5.0f, 1.0e-4f);
    /* Ignore Object: past the wall onto the back. */
    EXPECT_EQ(rig.Text(owner, "ignore_object"), "Back");
    /* Include Sensors: the gate. */
    EXPECT_EQ(rig.Text(owner, "sense_object"), "Gate");
    /* Ignore Objects (wall and gate) with sensors: the back. */
    EXPECT_EQ(rig.Text(owner, "list_object"), "Back");
    /* Property "target" without X-Ray: the wall blocks; with X-Ray: the back. */
    EXPECT_EQ(rig.Float(owner, "property_hit"), 0.0f);
    EXPECT_EQ(rig.Float(owner, "property_blocked"), 1.0f);
    EXPECT_EQ(rig.Float(owner, "xray_hit"), 1.0f);
    EXPECT_EQ(rig.Text(owner, "xray_object"), "Back");
    /* Scene gravity: -9.8 (not the custom -10). */
    const float vz = 10.0f / std::sqrt(2.0f);
    EXPECT_NEAR(rig.Float(owner, "scene_time"), (vz + std::sqrt(vz * vz + 2.0f * 9.8f)) / 9.8f, 1.0e-3f);
    EXPECT_EQ(rig.Float(owner, "done"), 1.0f);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Line of Sight
 * \{ */

namespace {

GenNode &Look(Tree &tree,
              const char *prefix,
              blender::Object *target,
              const int16_t eye_mode = 0,
              const int16_t target_mode = 0)
{
  GenNode &look = tree.Add("LogicNativeLineOfSight", eye_mode, target_mode);
  if (target != nullptr) {
    tree.b.ObjectNamed(look, "Target", target);
  }
  const std::string p(prefix);
  tree.names.push_back(p + "visible");
  tree.StoreBool(look, "Visible", tree.names.back().c_str());
  tree.names.push_back(p + "blocker");
  tree.StoreText(look, "Blocker", tree.names.back().c_str());
  tree.names.push_back(p + "distance");
  tree.StoreFloat(look, "Distance", tree.names.back().c_str());
  tree.names.push_back(p + "angle");
  tree.StoreFloat(look, "Angle", tree.names.back().c_str());
  return look;
}

}  // namespace

TEST(JoltLogicNodes, LineOfSightSeesWhatIsInRangeInViewAndNotBehindAWall)
{
  BlenderObject player("Player");
  BlenderObject right("Right");
  BlenderObject behind("Behind");
  BlenderObject far("Far");
  BlenderObject wall("Wall");
  BlenderObject left("Left");
  Tree tree;
  /* The guard (owner, at the origin, facing +Y) and a player 10 ahead, a wall between. */
  Look(tree, "wall_", &player.object);
  GenNode &ignored = Look(tree, "ignored_", &player.object);
  tree.b.Link(tree.ObjectList({&wall.object}), "Objects", ignored, "Ignore Objects");
  GenNode &masked = Look(tree, "masked_", &player.object);
  tree.b.Int(masked, "Mask", 1); /* the wall is on layer 2 */
  /* Over the wall: the eye and the target 3 up (the wall is 2 high). */
  GenNode &over = Look(tree, "over_", &player.object);
  tree.b.Vector(over, "Eye Offset", 0.0f, 0.0f, 3.0f);
  tree.b.Vector(over, "Target Offset", 0.0f, 0.0f, 3.0f);
  /* Right of the guard: 90 degrees, outside a 90 degree view; in a 360 one. */
  Look(tree, "right_", &right.object);
  GenNode &around = Look(tree, "around_", &right.object);
  tree.b.Float(around, "View Angle", float(2.0 * M_PI));
  /* Behind, with no facing (Forward zero): seen all around. */
  GenNode &blind = Look(tree, "nofacing_", &behind.object);
  tree.b.Vector(blind, "Forward", 0.0f, 0.0f, 0.0f);
  /* Too far for 50; no limit with 0. */
  Look(tree, "far_", &far.object);
  GenNode &unlimited = Look(tree, "unlimited_", &far.object);
  tree.b.Float(unlimited, "Max Distance", 0.0f);
  /* A sensor between the guard and "Left": passed, unless Include Sensors. */
  GenNode &left_look = Look(tree, "left_", &left.object);
  tree.b.Vector(left_look, "Forward", -1.0f, 0.0f, 0.0f);
  GenNode &left_sensor = Look(tree, "leftsensor_", &left.object);
  tree.b.Vector(left_sensor, "Forward", -1.0f, 0.0f, 0.0f);
  tree.b.Bool(left_sensor, "Include Sensors", true);
  /* Positions: from (0, 0.6, 0) (just ahead of the guard's body, which a position does not
   * pass) along +Y to (0, 4, 0) (before the wall) and (0, 20, 0). */
  GenNode &near_point = Look(tree, "nearpoint_", nullptr, 1, 1);
  tree.b.Vector(near_point, "Eye Position", 0.0f, 0.6f, 0.0f);
  tree.b.Vector(near_point, "Target Position", 0.0f, 4.0f, 0.0f);
  GenNode &behind_point = Look(tree, "behindpoint_", nullptr, 1, 1);
  tree.b.Vector(behind_point, "Eye Position", 0.0f, -1.0f, 0.0f);
  tree.b.Vector(behind_point, "Target Position", 0.0f, 4.0f, 0.0f);
  GenNode &far_point = Look(tree, "farpoint_", nullptr, 1, 1);
  tree.b.Vector(far_point, "Eye Position", 0.0f, 0.6f, 0.0f);
  tree.b.Vector(far_point, "Target Position", 0.0f, 20.0f, 0.0f);
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    /* After the rig: its bodies go first (a game object deletes its controller). */
    World world;
    KX_GameObject &guard = rig.Object(0);
    /* The guard's and the player's own bodies never block. */
    world.Box(guard, MT_Vector3(0.5f, 0.5f, 1.0f));
    world.Box(rig.AddObject("Player", MT_Vector3(0.0f, 10.0f, 0.0f)), MT_Vector3(0.5f, 0.5f, 1.0f));
    world.Box(rig.AddObject("Wall", MT_Vector3(0.0f, 5.0f, 0.0f)),
              MT_Vector3(3.0f, 0.1f, 2.0f),
              JPH::EMotionType::Static,
              false,
              2);
    world.Box(rig.AddObject("Right", MT_Vector3(10.0f, 0.0f, 0.0f)), MT_Vector3(0.5f, 0.5f, 1.0f));
    rig.AddObject("Behind", MT_Vector3(0.0f, -10.0f, 0.0f));
    rig.AddObject("Far", MT_Vector3(0.0f, 60.0f, 30.0f));
    rig.AddObject("Left", MT_Vector3(-10.0f, 0.0f, 0.0f));
    world.Box(rig.AddObject("Fog", MT_Vector3(-5.0f, 0.0f, 0.0f)),
              MT_Vector3(0.1f, 3.0f, 3.0f),
              JPH::EMotionType::Static,
              true);
    rig.Step();

    EXPECT_EQ(rig.Float(guard, "wall_visible"), 0.0f);
    EXPECT_EQ(rig.Text(guard, "wall_blocker"), "Wall");
    EXPECT_NEAR(rig.Float(guard, "wall_distance"), 10.0f, 1.0e-5f);
    EXPECT_NEAR(rig.Float(guard, "wall_angle"), 0.0f, 1.0e-3f);
    EXPECT_EQ(rig.Float(guard, "ignored_visible"), 1.0f);
    EXPECT_EQ(rig.Float(guard, "masked_visible"), 1.0f);
    EXPECT_EQ(rig.Float(guard, "over_visible"), 1.0f);
    EXPECT_EQ(rig.Float(guard, "right_visible"), 0.0f);
    EXPECT_NE(rig.Text(guard, "right_blocker"), "Wall");
    EXPECT_NEAR(rig.Float(guard, "right_angle"), 90.0f, 1.0e-3f);
    EXPECT_EQ(rig.Float(guard, "around_visible"), 1.0f);
    EXPECT_EQ(rig.Float(guard, "nofacing_visible"), 1.0f);
    EXPECT_NEAR(rig.Float(guard, "nofacing_angle"), 0.0f, 1.0e-5f);
    EXPECT_EQ(rig.Float(guard, "far_visible"), 0.0f);
    EXPECT_NEAR(rig.Float(guard, "far_distance"), std::sqrt(60.0f * 60.0f + 30.0f * 30.0f), 1.0e-3f);
    EXPECT_NEAR(rig.Float(guard, "far_angle"), float(std::atan2(30.0, 60.0) * 180.0 / M_PI), 1.0e-2f);
    /* No limit: seen, over the wall (the line is 2.5 up there). */
    EXPECT_EQ(rig.Float(guard, "unlimited_visible"), 1.0f);
    EXPECT_EQ(rig.Float(guard, "left_visible"), 1.0f);
    EXPECT_EQ(rig.Float(guard, "leftsensor_visible"), 0.0f);
    EXPECT_EQ(rig.Text(guard, "leftsensor_blocker"), "Fog");
    EXPECT_EQ(rig.Float(guard, "nearpoint_visible"), 1.0f);
    EXPECT_NEAR(rig.Float(guard, "nearpoint_distance"), 3.4f, 1.0e-5f);
    /* From behind the guard: its body is in the way (a position eye passes no object). */
    EXPECT_EQ(rig.Float(guard, "behindpoint_visible"), 0.0f);
    EXPECT_EQ(rig.Text(guard, "behindpoint_blocker"), "Obj0");
    EXPECT_EQ(rig.Float(guard, "farpoint_visible"), 0.0f);
    EXPECT_EQ(rig.Text(guard, "farpoint_blocker"), "Wall");
  }
}

TEST(JoltLogicNodes, LineOfSightVisualizeDrawsSeenAndHiddenInTheirColoursForItsDuration)
{
  BlenderObject player("Player");
  Tree tree;
  GenNode &look = Look(tree, "v_", &player.object);
  tree.b.Bool(look, "Visualize", true);
  tree.b.Float(look, "Duration", 0.5f);
  tree.b.Float(look, "View Angle", float(2.0 * M_PI)); /* no cone lines */
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction);
  /* After the rig: its bodies go first (a game object deletes its controller). */
  World world;
  KX_GameObject &guard = rig.Object(0);
  KX_GameObject &target = rig.AddObject("Player", MT_Vector3(0.0f, 10.0f, 0.0f));
  LN_DebugLinesForTests lines;
  LN_TimedDebugLines timed;
  LN_SetDebugLinesForTests(&lines);
  LN_SetTimedDebugLinesForTests(&timed);
  rig.Step();
  /* One line, drawn at each of the four reads of the node's outputs (the check is made again
   * for each instruction that reads it); kept once. */
  ASSERT_EQ(lines.size(), 4u);
  for (const auto &line : lines) {
    EXPECT_EQ(line.second, 0.5f);
    /* Seen: green by default. */
    EXPECT_EQ(line.first.color, MT_Vector4(0.1f, 1.0f, 0.2f, 1.0f));
    EXPECT_EQ(line.first.to, MT_Vector3(0.0f, 10.0f, 0.0f));
  }
  EXPECT_EQ(timed.Count(), 1u);
  /* Drawn again while its half second lasts, then gone. */
  lines.clear();
  timed.Draw(0.25);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].second, -1.0f);
  lines.clear();
  timed.Draw(0.6);
  EXPECT_TRUE(lines.empty());
  EXPECT_EQ(timed.Count(), 0u);
  /* Hidden behind a wall: red, to the wall. */
  world.Box(rig.AddObject("Wall", MT_Vector3(0.0f, 5.0f, 0.0f)), MT_Vector3(3.0f, 0.1f, 2.0f));
  lines.clear();
  rig.Step();
  ASSERT_GE(lines.size(), 1u);
  EXPECT_EQ(lines[0].first.color, MT_Vector4(1.0f, 0.05f, 0.02f, 1.0f));
  EXPECT_NEAR(lines[0].first.to.y(), 4.9f, 1.0e-4f);
  LN_SetDebugLinesForTests(nullptr);
  LN_SetTimedDebugLinesForTests(nullptr);
  (void)guard;
  (void)target;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sweep Object
 * \{ */

namespace {

GenNode &Sweep(Tree &tree, const char *prefix, const MT_Vector3 &destination)
{
  GenNode &sweep = tree.Add("LogicNativeSweepObject");
  tree.b.Vector(sweep, "Destination", destination.x(), destination.y(), destination.z());
  const std::string p(prefix);
  tree.names.push_back(p + "hit");
  tree.StoreBool(sweep, "Has Result", tree.names.back().c_str());
  tree.names.push_back(p + "blocked");
  tree.StoreBool(sweep, "Blocked", tree.names.back().c_str());
  tree.names.push_back(p + "object");
  tree.StoreText(sweep, "Hit Object", tree.names.back().c_str());
  tree.names.push_back(p + "part");
  tree.StoreText(sweep, "Hit Part", tree.names.back().c_str());
  tree.names.push_back(p + "fraction");
  tree.StoreFloat(sweep, "Safe Fraction", tree.names.back().c_str());
  tree.names.push_back(p + "distance");
  tree.StoreFloat(sweep, "Distance", tree.names.back().c_str());
  tree.names.push_back(p + "inside");
  tree.StoreBool(sweep, "Started Overlapping", tree.names.back().c_str());
  tree.StoreVector(sweep, "Safe Position", p + "safe");
  return sweep;
}

}  // namespace

TEST(JoltLogicNodes, SweepObjectMovesTheObjectsOwnShapeAndSaysHowFarItFits)
{
  BlenderObject wall("Wall");
  Tree tree;
  Sweep(tree, "plain_", MT_Vector3(6.0f, 0.0f, 0.0f));
  GenNode &ignoring = Sweep(tree, "ignore_", MT_Vector3(6.0f, 0.0f, 0.0f));
  tree.b.Link(tree.ObjectList({&wall.object}), "Objects", ignoring, "Ignore Objects");
  GenNode &local = Sweep(tree, "local_", MT_Vector3(0.0f, -6.0f, 0.0f));
  tree.b.Bool(local, "Local", true);
  GenNode &turned = tree.Add("LogicNativeSweepObject", 1 /* Custom Rotation */);
  tree.b.Vector(turned, "Destination", 6.0f, 0.0f, 0.0f);
  tree.b.Rotation(turned, "Rotation", 0.0f, 0.0f, 0.0f);
  tree.StoreVector(turned, "Safe Position", "turned_safe");
  GenNode &property = Sweep(tree, "property_", MT_Vector3(6.0f, 0.0f, 0.0f));
  tree.b.String(property, "Property", "solid");
  GenNode &xray = Sweep(tree, "xray_", MT_Vector3(6.0f, 0.0f, 0.0f));
  tree.b.String(xray, "Property", "solid");
  tree.b.Bool(xray, "X-Ray", true);
  Sweep(tree, "free_", MT_Vector3(0.0f, 0.0f, 6.0f));
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    /* After the rig: its bodies go first (a game object deletes its controller). */
    World world;
    KX_GameObject &mover = rig.Object(0);
    /* The mover: a long box (half 1 along X, 0.5 across), turned a quarter about Z: in the world
     * its long side is along Y. Its own child with a body of its own sits 1 ahead: passed. */
    mover.NodeSetGlobalOrientation(MT_Matrix3x3(MT_Vector3(0.0f, 0.0f, float(M_PI_2))));
    mover.NodeUpdateGS(0.0);
    world.Box(mover, MT_Vector3(1.0f, 0.5f, 0.5f), JPH::EMotionType::Dynamic);
    const size_t shield_index = rig.ObjectCount();
    KX_GameObject &shield = rig.AddObject("Shield", MT_Vector3(1.0f, 0.0f, 0.0f));
    rig.Parent(shield_index, 0);
    world.Box(shield, MT_Vector3(0.2f, 0.2f, 0.2f));
    world.Box(rig.AddObject("Wall", MT_Vector3(3.0f, 0.0f, 0.0f)), MT_Vector3(0.5f, 3.0f, 3.0f));
    KX_GameObject &back = rig.AddObject("Back", MT_Vector3(5.0f, 0.0f, 0.0f));
    world.Box(back, MT_Vector3(0.1f, 3.0f, 3.0f));
    rig.SetBool(back, "solid", true);
    rig.Step();

    /* Turned, the mover is 0.5 deep along X: it touches the wall (x 2.5) at x = 2, and stops a
     * millimetre short. */
    EXPECT_EQ(rig.Float(mover, "plain_hit"), 1.0f);
    EXPECT_EQ(rig.Text(mover, "plain_object"), "Wall");
    EXPECT_EQ(rig.Text(mover, "plain_part"), "Wall");
    EXPECT_NEAR(rig.Float(mover, "plain_distance"), 1.999f, 1.0e-3f);
    EXPECT_NEAR(rig.Float(mover, "plain_fraction"), 1.999f / 6.0f, 2.0e-4f);
    ExpectVector(ReadVector(rig, mover, "plain_safe"), MT_Vector3(1.999f, 0.0f, 0.0f), 1.0e-3f);
    EXPECT_EQ(rig.Float(mover, "plain_inside"), 0.0f);
    /* The wall ignored: the back at x 4.9. */
    EXPECT_EQ(rig.Text(mover, "ignore_object"), "Back");
    ExpectVector(ReadVector(rig, mover, "ignore_safe"), MT_Vector3(4.399f, 0.0f, 0.0f), 1.0e-3f);
    /* Local (0, -6, 0) of the turned mover is world +X: the same as plain. */
    ExpectVector(ReadVector(rig, mover, "local_safe"), MT_Vector3(1.999f, 0.0f, 0.0f), 1.0e-3f);
    /* Custom Rotation (none, in the world): the long side along X, 1 deep: it touches at 1.5. */
    ExpectVector(ReadVector(rig, mover, "turned_safe"), MT_Vector3(1.499f, 0.0f, 0.0f), 1.0e-3f);
    /* Property "solid" without X-Ray: the wall blocks; with X-Ray: the back. */
    EXPECT_EQ(rig.Float(mover, "property_hit"), 0.0f);
    EXPECT_EQ(rig.Float(mover, "property_blocked"), 1.0f);
    EXPECT_EQ(rig.Text(mover, "xray_object"), "Back");
    /* Up and free: the whole way. */
    EXPECT_EQ(rig.Float(mover, "free_hit"), 0.0f);
    EXPECT_EQ(rig.Float(mover, "free_fraction"), 1.0f);
    ExpectVector(ReadVector(rig, mover, "free_safe"), MT_Vector3(0.0f, 0.0f, 6.0f), 1.0e-5f);

    /* Inside the wall already: a hit at once, no way to go. */
    mover.NodeSetWorldPosition(MT_Vector3(2.6f, 0.0f, 0.0f));
    mover.NodeUpdateGS(0.0);
    rig.Step();
    EXPECT_EQ(rig.Float(mover, "plain_hit"), 1.0f);
    EXPECT_EQ(rig.Float(mover, "plain_inside"), 1.0f);
    EXPECT_EQ(rig.Float(mover, "plain_fraction"), 0.0f);
    ExpectVector(ReadVector(rig, mover, "plain_safe"), MT_Vector3(2.6f, 0.0f, 0.0f), 1.0e-5f);
  }
}

TEST(JoltLogicNodes, SweepObjectRefusesATriangleMeshAndGivesTheCompoundPartHit)
{
  Tree tree;
  Sweep(tree, "s_", MT_Vector3(10.0f, 0.0f, 0.0f));
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  {
    SemRig rig(program, kProduction);
    /* After the rig: its bodies go first (a game object deletes its controller). */
    World world;
    KX_GameObject &mover = rig.Object(0);
    world.Box(mover, MT_Vector3(0.5f, 0.5f, 0.5f), JPH::EMotionType::Dynamic);
    Tank tank(rig, world, MT_Vector3(5.0f, 0.0f, 0.0f));
    ASSERT_TRUE(tank.valid);
    rig.Step();
    EXPECT_EQ(rig.Text(mover, "s_object"), "Tank");
    EXPECT_EQ(rig.Text(mover, "s_part"), "Turret");
  }
  {
    SemRig rig(program, kProduction);
    /* After the rig: its bodies go first (a game object deletes its controller). */
    World world;
    KX_GameObject &mover = rig.Object(0);
    JoltShapeQueryDataPtr query_data;
    world.Add(mover, QuadWithTwoSlots(query_data), JPH::EMotionType::Static, false, 1, nullptr);
    world.Box(rig.AddObject("Wall", MT_Vector3(3.0f, 0.0f, 0.0f)), MT_Vector3(0.5f, 3.0f, 3.0f));
    rig.SetCapturePrints(false);
    testing::internal::CaptureStdout();
    rig.Step();
    const std::string printed = testing::internal::GetCapturedStdout();
    EXPECT_NE(printed.find("Sweep Object cannot move"), std::string::npos) << printed;
    EXPECT_EQ(rig.Float(mover, "s_hit"), 0.0f);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Visualize: Duration, Hit Color, Miss Color
 * \{ */

TEST(JoltLogicNodes, RaycastVisualizeUsesItsColoursAndKeepsItsLinesForItsDuration)
{
  Tree tree;
  GenNode &ray = tree.Add("LogicNativeRaycast", 1);
  tree.b.Vector(ray, "Direction", 1.0f, 0.0f, 0.0f);
  tree.b.Float(ray, "Max Distance", 20.0f);
  tree.b.Bool(ray, "Visualize", true);
  tree.b.Float(ray, "Duration", 1.0f);
  tree.StoreBool(ray, "Has Result", "hit");
  /* Linked colours (Value Color nodes): read while the game runs. */
  GenNode &hit_color = tree.Add("LogicNativeValueColor");
  tree.b.OutputColor(hit_color, "Value", 0.0f, 0.0f, 1.0f, 1.0f);
  tree.b.Link(hit_color, "Value", ray, "Hit Color");
  GenNode &miss_color = tree.Add("LogicNativeValueColor");
  tree.b.OutputColor(miss_color, "Value", 0.0f, 1.0f, 0.0f, 1.0f);
  tree.b.Link(miss_color, "Value", ray, "Miss Color");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction);
  /* After the rig: its bodies go first (a game object deletes its controller). */
  World world;
  KX_GameObject &wall = rig.AddObject("Wall", MT_Vector3(5.0f, 0.0f, 0.0f));
  world.Box(wall, MT_Vector3(0.5f, 3.0f, 3.0f));
  LN_DebugLinesForTests lines;
  LN_TimedDebugLines timed;
  LN_SetDebugLinesForTests(&lines);
  LN_SetTimedDebugLinesForTests(&timed);
  rig.Step();
  EXPECT_EQ(rig.Float(rig.Object(0), "hit"), 1.0f);
  /* The hit's frame (its own colours; the normal at most 1 m, never the whole cast), then the ray
   * in the Hit Color, drawn last: a head-on hit's normal runs back along the ray. */
  ASSERT_EQ(lines.size(), 4u);
  const LN_DebugLine &ray_line = lines.back().first;
  EXPECT_EQ(ray_line.color, MT_Vector4(0.0f, 0.0f, 1.0f, 1.0f));
  EXPECT_NEAR(ray_line.from.x(), 0.0f, 1.0e-4f);
  EXPECT_NEAR(ray_line.to.x(), 4.5f, 1.0e-4f);
  EXPECT_EQ(lines.back().second, 1.0f);
  for (size_t index = 0; index + 1 < lines.size(); index++) {
    EXPECT_LE((lines[index].first.to - lines[index].first.from).length(), 1.0f + 1.0e-4f);
    EXPECT_NE(lines[index].first.color, ray_line.color);
  }
  const size_t first_step_lines = lines.size();
  EXPECT_EQ(timed.Count(), first_step_lines);
  /* Without the wall: the Miss Color, to the end. */
  world.bodies.clear();
  lines.clear();
  rig.Step();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].first.color, MT_Vector4(0.0f, 1.0f, 0.0f, 1.0f));
  EXPECT_NEAR(lines[0].first.to.x(), 20.0f, 1.0e-4f);
  EXPECT_EQ(timed.Count(), first_step_lines + 1);
  /* Each step draws the lines still due: the rig's time is 1/60 a step. */
  lines.clear();
  timed.Draw(0.5);
  EXPECT_EQ(lines.size(), first_step_lines + 1);
  lines.clear();
  timed.Draw(1.0 + 1.0 / 60.0 + 1.0e-6);
  EXPECT_TRUE(lines.empty());
  EXPECT_EQ(timed.Count(), 0u);
  LN_SetDebugLinesForTests(nullptr);
  LN_SetTimedDebugLinesForTests(nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Explode or Implode Impulse's overlap (2026-11-12)
 * \{ */

/** OverlapSphereBodies (Explode or Implode Impulse's query since 2026-11-12) finds exactly the
 * bodies a zero-length sphere ShapeCast found before it, in random worlds of boxes, spheres, turned
 * boxes, one-sided mesh quads (some facing away from the center), static, kinematic, dynamic and
 * sensor bodies: the same set; and with more than its limit the same deepest ones, in order. */
TEST(JoltLogicNodes, ExplosionOverlapFindsTheBodiesTheZeroLengthShapeCastFound)
{
  uint32_t seed = 20261112u;
  const auto next = [&]() {
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) / float(1u << 24);
  };
  int at_limit = 0;
  int shortcut_free = 0;
  for (int world_index = 0; world_index < 12; world_index++) {
    SCOPED_TRACE(world_index);
    std::deque<PositionedGameObject> objects;
    /* After the objects: the bodies go first (a game object deletes its controller). */
    World world(700);
    const int count = world_index < 10 ? 60 : 600;
    for (int index = 0; index < count; index++) {
      const MT_Vector3 position(next() * 16.0f - 8.0f, next() * 16.0f - 8.0f, next() * 16.0f - 8.0f);
      objects.emplace_back(position);
      KX_GameObject &object = objects.back().object;
      object.SetName("B" + std::to_string(index));
      object.NodeSetGlobalOrientation(MT_Matrix3x3(MT_Vector3(next() * 3.0f, next() * 3.0f, 0.0f)));
      object.NodeUpdateGS(0.0);
      const float kind = next();
      const JPH::EMotionType motion = kind < 0.6f ? JPH::EMotionType::Dynamic :
                                      kind < 0.8f ? JPH::EMotionType::Kinematic :
                                                    JPH::EMotionType::Static;
      const bool sensor = next() < 0.1f;
      JPH::RefConst<JPH::Shape> shape;
      const float shape_kind = next();
      if (shape_kind < 0.4f) {
        shape = new JPH::BoxShape(JPH::Vec3(0.2f + next(), 0.2f + next(), 0.2f + next()), 0.05f);
      }
      else if (shape_kind < 0.7f || motion != JPH::EMotionType::Static) {
        shape = new JPH::SphereShape(0.2f + next());
      }
      else {
        /* A one-sided quad (static only): which way it faces decides a cast's back faces. */
        JPH::VertexList vertices = {JPH::Float3(-1.0f, 0.0f, -1.0f),
                                    JPH::Float3(-1.0f, 0.0f, 1.0f),
                                    JPH::Float3(1.0f, 0.0f, -1.0f),
                                    JPH::Float3(1.0f, 0.0f, 1.0f)};
        JPH::IndexedTriangleList triangles = {JPH::IndexedTriangle(0, 1, 2, 0),
                                              JPH::IndexedTriangle(2, 1, 3, 0)};
        shape = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create().Get();
      }
      world.Add(object, shape, motion, sensor, 1, nullptr);
    }
    world.environment.GetPhysicsSystem()->OptimizeBroadPhase();

    for (int query = 0; query < 20; query++) {
      const MT_Vector3 center(next() * 10.0f - 5.0f, next() * 10.0f - 5.0f, next() * 10.0f - 5.0f);
      const float radius = 0.5f + next() * (world_index < 10 ? 8.0f : 20.0f);
      const uint32_t limit = 256;
      PHY_IShapeCastFilterCallback accept_all(nullptr, nullptr);
      PHY_ShapeCastSettings settings;
      settings.type = PHY_ShapeCastType::Sphere;
      settings.origin = center;
      settings.destination = center;
      settings.radius = radius;
      settings.max_results = limit;
      std::vector<PHY_ShapeCastResult> cast;
      ASSERT_TRUE(world.environment.ShapeCast(settings, accept_all, cast));
      std::vector<PHY_IPhysicsController *> overlap;
      ASSERT_TRUE(
          world.environment.OverlapSphereBodies(center, radius, accept_all, limit, overlap));
      std::vector<PHY_IPhysicsController *> before;
      for (const PHY_ShapeCastResult &hit : cast) {
        before.push_back(hit.controller);
      }
      if (before.size() < limit) {
        /* The set: the order of the pushes is free (one each). */
        std::sort(before.begin(), before.end());
        std::vector<PHY_IPhysicsController *> after = overlap;
        std::sort(after.begin(), after.end());
        EXPECT_EQ(after, before) << "radius " << radius;
        shortcut_free += before.empty() ? 0 : 1;
      }
      else {
        /* The limit: the very same deepest ones, in the cast's order. */
        EXPECT_EQ(overlap, before) << "radius " << radius;
        at_limit++;
      }
    }
  }
  /* Both ways were taken. */
  EXPECT_GT(shortcut_free, 100);
  EXPECT_GT(at_limit, 10);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Keep Distance (2026-11-12)
 * \{ */

namespace {

/** [On Logic + Physics Step] -> Keep Distance (Measure From: the Radius 0.5 sphere, or the
 * object's shape; `mode` its Mode input, LN_KEEP_DISTANCE_MODE_*; Linear or Smooth), its outputs
 * stored as "near", "gap", "force", "normal" and "what" (they give the step before: read after
 * two steps). */
struct KeepDistanceTree {
  explicit KeepDistanceTree(const bool object_shape = false,
                            const int check_every = 1,
                            const int mode = LN_KEEP_DISTANCE_MODE_PUSH,
                            const bool smooth = false)
  {
    keep = &tree.Add("LogicNativeKeepDistance",
                     smooth ? LN_KEEP_DISTANCE_FALLOFF_SMOOTH : LN_KEEP_DISTANCE_FALLOFF_LINEAR,
                     object_shape ? 0 : 1);
    tree.b.Int(*keep, "Mode", mode);
    tree.b.Link(*tree.step, "Out", *keep, "Flow");
    tree.b.Float(*keep, "Radius", 0.5f);
    tree.b.Int(*keep, "Check Every", check_every);
    tree.StoreBool(*keep, "Near", "near");
    tree.StoreFloat(*keep, "Nearest Distance", "gap");
    tree.StoreVector(*keep, "Force", "force");
    tree.StoreVector(*keep, "Nearest Normal", "normal");
    tree.StoreText(*keep, "Nearest Object", "what");
  }
  Tree tree;
  GenNode *keep = nullptr;
};

/** The owner (object 0, at `position`) gets a dynamic ball of radius 0.5. */
Body &Ball(SemRig &rig, World &world, const MT_Vector3 &position = MT_Vector3(0, 0, 0))
{
  KX_GameObject &owner = rig.Object(0);
  owner.NodeSetWorldPosition(position);
  owner.NodeUpdateGS(0.0);
  return world.Add(owner, new JPH::SphereShape(0.5f), JPH::EMotionType::Dynamic, false, 1, nullptr);
}

/** A static wall (a 1 x 4 x 4 box) whose near face is at x = `face` (left of it when negative). */
Body &Wall(SemRig &rig, World &world, const char *name, const float face)
{
  const float center = face + (face >= 0.0f ? 0.5f : -0.5f);
  return world.Box(rig.AddObject(name, MT_Vector3(center, 0.0f, 0.0f)), MT_Vector3(0.5f, 2.0f, 2.0f));
}

MT_Vector3 Acceleration(SemRig &rig, Body &ball)
{
  return ReadVector(rig, rig.Object(0), "force") / float(ball.controller.GetMass());
}

}  // namespace

/** The push points away from the wall, Strength x (1 - gap / Distance) as an acceleration (Ignore
 * Mass): the ball (radius 0.5) 0.8 from a wall at Distance 1 and Strength 20 gets 4 m/s^2; the
 * outputs tell the gap, the side and the wall; the force reaches the body. */
TEST(JoltLogicNodes, KeepDistancePushesAwayFromAWallHarderTheCloser)
{
  KeepDistanceTree tree;
  const std::shared_ptr<LN_Program> program = tree.tree.Compile();
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    World world;
    Body &ball = Ball(rig, world);
    Wall(rig, world, "Wall", 1.3f);
    rig.Step();
    rig.Step();
    KX_GameObject &owner = rig.Object(0);
    EXPECT_EQ(rig.Float(owner, "near"), 1.0f);
    EXPECT_NEAR(rig.Float(owner, "gap"), 0.8f, 1e-4f);
    ExpectVector(Acceleration(rig, ball), MT_Vector3(-4.0f, 0.0f, 0.0f), 1e-3f);
    ExpectVector(ReadVector(rig, owner, "normal"), MT_Vector3(-1.0f, 0.0f, 0.0f), 1e-4f);
    EXPECT_EQ(rig.Text(owner, "what"), "Wall");
    EXPECT_NE(rig.Forces().find("Obj0 force"), std::string::npos);
  }
  /* Nearer (0.3 away): 14 m/s^2; nothing within Distance: no push, Nearest Distance -1. */
  for (const float face : {0.8f, 3.0f}) {
    SemRig rig(program, kProduction);
    World world;
    Body &ball = Ball(rig, world);
    Wall(rig, world, "Wall", face);
    rig.Step();
    rig.Step();
    const bool near = face < 1.5f;
    EXPECT_EQ(rig.Float(rig.Object(0), "near"), near ? 1.0f : 0.0f);
    EXPECT_NEAR(rig.Float(rig.Object(0), "gap"), near ? 0.3f : -1.0f, 1e-4f);
    ExpectVector(Acceleration(rig, ball), MT_Vector3(near ? -14.0f : 0.0f, 0.0f, 0.0f), 1e-3f);
    EXPECT_EQ(rig.Text(rig.Object(0), "what"), near ? "Wall" : "");
  }
}

/** Pull: toward the nearest surface only, Strength x (1 - gap / Distance): the ball between a
 * wall 0.6 to its left and one 0.8 to its right is pulled left at 8 m/s^2 (Push in the same place:
 * 2.67 right); past Distance nothing pulls. */
TEST(JoltLogicNodes, KeepDistancePullsTowardTheNearestSurfaceOnly)
{
  for (const int mode : {LN_KEEP_DISTANCE_MODE_PULL, LN_KEEP_DISTANCE_MODE_PUSH}) {
    KeepDistanceTree tree(false, 1, mode);
    const std::shared_ptr<LN_Program> program = tree.tree.Compile();
    ASSERT_NE(program, nullptr);
    for (const SemMode &sem_mode : Modes()) {
      SCOPED_TRACE(std::string(sem_mode.name) + (mode == LN_KEEP_DISTANCE_MODE_PULL ? " pull" :
                                                                                     " push"));
      SemRig rig(program, sem_mode);
      World world;
      Body &ball = Ball(rig, world);
      Wall(rig, world, "Right", 1.3f);
      Wall(rig, world, "Left", -1.1f);
      rig.Step();
      rig.Step();
      KX_GameObject &owner = rig.Object(0);
      EXPECT_EQ(rig.Float(owner, "near"), 1.0f);
      EXPECT_NEAR(rig.Float(owner, "gap"), 0.6f, 1e-4f);
      EXPECT_EQ(rig.Text(owner, "what"), "Left");
      ExpectVector(ReadVector(rig, owner, "normal"), MT_Vector3(1.0f, 0.0f, 0.0f), 1e-4f);
      ExpectVector(Acceleration(rig, ball),
                   MT_Vector3(mode == LN_KEEP_DISTANCE_MODE_PULL ? -8.0f : 20.0f * 0.2f * 0.4f / 0.6f,
                              0.0f,
                              0.0f),
                   1e-3f);
    }
  }
  KeepDistanceTree tree(false, 1, LN_KEEP_DISTANCE_MODE_PULL);
  const std::shared_ptr<LN_Program> program = tree.tree.Compile();
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction);
  World world;
  Body &ball = Ball(rig, world);
  Wall(rig, world, "Wall", 1.8f);
  rig.Step();
  rig.Step();
  EXPECT_EQ(rig.Float(rig.Object(0), "near"), 0.0f);
  EXPECT_NEAR(rig.Float(rig.Object(0), "gap"), -1.0f, 1e-4f);
  EXPECT_EQ(rig.Text(rig.Object(0), "what"), "");
  ExpectVector(Acceleration(rig, ball), MT_Vector3(0.0f, 0.0f, 0.0f), 1e-6f);
}

/** Hold (Distance 1, Reach 2): inside Distance it pushes as Push (0.8 from the wall: 4 m/s^2
 * away); farther it pulls back, Strength x (gap - Distance) / (Reach - Distance) (1.5: 10 toward
 * it; Smooth that squared: 5); past Reach it lets go. The outputs tell of everything within
 * Reach. Reach from a game property (a runtime link: the instruction's value field) moves it:
 * Reach 4, 2.5 from the wall: 10 toward it. With Check Every 4 the same. */
TEST(JoltLogicNodes, KeepDistanceHoldsAtDistancePullingBackOutToReach)
{
  struct Case {
    float face;
    bool smooth;
    float reach; /* < 0: the socket's default 2, a constant; else from the property "reach" */
    int every;
    float acceleration;
    float gap;
  };
  const Case cases[] = {{1.3f, false, -1.0f, 1, -4.0f, 0.8f},
                        {2.0f, false, -1.0f, 1, 10.0f, 1.5f},
                        {2.0f, true, -1.0f, 1, 5.0f, 1.5f},
                        {1.5f, false, -1.0f, 1, 0.0f, 1.0f},
                        {3.0f, false, -1.0f, 1, 0.0f, -1.0f},
                        {3.0f, false, 4.0f, 1, 10.0f, 2.5f},
                        {3.0f, false, 4.0f, 4, 10.0f, 2.5f},
                        {2.0f, false, -1.0f, 4, 10.0f, 1.5f}};
  for (const Case &c : cases) {
    SCOPED_TRACE(std::to_string(c.face) + (c.smooth ? " smooth" : "") + " reach " +
                 std::to_string(c.reach) + " every " + std::to_string(c.every));
    KeepDistanceTree tree(false, c.every, LN_KEEP_DISTANCE_MODE_HOLD, c.smooth);
    if (c.reach >= 0.0f) {
      GenNode &reach = tree.tree.b.Add("LogicNativeGetGamePropertyFloat");
      tree.tree.b.String(reach, "Property", "reach");
      tree.tree.b.Link(reach, "Value", *tree.keep, "Reach");
    }
    const std::shared_ptr<LN_Program> program = tree.tree.Compile();
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kProduction);
    World world;
    Body &ball = Ball(rig, world);
    if (c.reach >= 0.0f) {
      rig.SetFloat(rig.Object(0), "reach", c.reach);
    }
    Wall(rig, world, "Wall", c.face);
    rig.Step();
    rig.Step();
    KX_GameObject &owner = rig.Object(0);
    const bool within = c.gap >= 0.0f;
    EXPECT_EQ(rig.Float(owner, "near"), within ? 1.0f : 0.0f);
    EXPECT_NEAR(rig.Float(owner, "gap"), c.gap, 1e-3f);
    EXPECT_EQ(rig.Text(owner, "what"), within ? "Wall" : "");
    ExpectVector(Acceleration(rig, ball), MT_Vector3(c.acceleration, 0.0f, 0.0f), 2e-3f);
  }
}

/** The Mode is an input (2026-11-15, the user's call): a game property ("mode") switches it while
 * the game runs. Distance 1, Reach 2, Strength 20. A wall 0.8 away (inside Distance): 0 Push 4
 * m/s^2 away, 1 Pull 4 toward it, 2 Hold pushes as Push (4 away); below 0 counts as Push, above 2
 * as Hold. A wall 1.5 away (past Distance, within Reach): only Hold acts (10 toward it). With Check
 * Every 4 a switch from Push to Hold looks out to Reach at once (its range grew past the last
 * check's): whichever step of the 4 it comes on, two steps later the pull shows (the outputs
 * give the step before). In every mode of the rig. */
TEST(JoltLogicNodes, KeepDistanceModeSwitchesWhileTheGameRuns)
{
  const auto build = [](const int every) {
    KeepDistanceTree tree(false, every);
    GenNode &mode = tree.tree.b.Add("LogicNativeGetGamePropertyInt");
    tree.tree.b.String(mode, "Property", "mode");
    tree.tree.b.Link(mode, "Value", *tree.keep, "Mode");
    return tree.tree.Compile();
  };
  const std::shared_ptr<LN_Program> program = build(1);
  ASSERT_NE(program, nullptr);
  for (const SemMode &sem_mode : Modes()) {
    SCOPED_TRACE(sem_mode.name);
    SemRig rig(program, sem_mode);
    World world;
    Body &ball = Ball(rig, world);
    Wall(rig, world, "Wall", 1.3f);
    KX_GameObject &owner = rig.Object(0);
    const struct {
      int mode;
      float acceleration;
    } switches[] = {{0, -4.0f}, {1, 4.0f}, {2, -4.0f}, {1, 4.0f}, {-3, -4.0f}, {1, 4.0f},
                    {7, -4.0f}, {0, -4.0f}};
    for (const auto &s : switches) {
      SCOPED_TRACE("mode " + std::to_string(s.mode));
      rig.SetInt(owner, "mode", s.mode);
      rig.Step();
      rig.Step();
      ExpectVector(Acceleration(rig, ball), MT_Vector3(s.acceleration, 0.0f, 0.0f), 2e-3f);
    }
  }
  /* Past Distance, within Reach: Push and Pull leave it, Hold pulls it back. */
  {
    SemRig rig(program, kProduction);
    World world;
    Body &ball = Ball(rig, world);
    Wall(rig, world, "Wall", 2.0f);
    for (const auto &[mode, acceleration] : {std::pair(0, 0.0f),
                                              std::pair(2, 10.0f),
                                              std::pair(1, 0.0f),
                                              std::pair(7, 10.0f),
                                              std::pair(-3, 0.0f),
                                              std::pair(2, 10.0f)})
    {
      SCOPED_TRACE("far, mode " + std::to_string(mode));
      rig.SetInt(rig.Object(0), "mode", mode);
      rig.Step();
      rig.Step();
      ExpectVector(Acceleration(rig, ball), MT_Vector3(acceleration, 0.0f, 0.0f), 2e-3f);
      EXPECT_EQ(rig.Float(rig.Object(0), "near"), mode >= 2 ? 1.0f : 0.0f);
    }
  }
  const std::shared_ptr<LN_Program> every4 = build(4);
  ASSERT_NE(every4, nullptr);
  for (int before = 4; before < 8; before++) {
    SCOPED_TRACE("Check Every 4, switched after " + std::to_string(before) + " steps");
    SemRig rig(every4, kProduction);
    World world;
    Body &ball = Ball(rig, world);
    Wall(rig, world, "Wall", 2.0f);
    rig.SetInt(rig.Object(0), "mode", 0);
    for (int step = 0; step < before; step++) {
      rig.Step();
    }
    ExpectVector(Acceleration(rig, ball), MT_Vector3(0.0f, 0.0f, 0.0f), 1e-6f);
    rig.SetInt(rig.Object(0), "mode", 2);
    rig.Step();
    rig.Step();
    ExpectVector(Acceleration(rig, ball), MT_Vector3(10.0f, 0.0f, 0.0f), 2e-3f);
  }
}

/** The outputs tell of the cushion only, whatever Check Every looks ahead: the ball heading for a
 * wall 1.8 away at 30 m/s, Check Every 4 searches 1 + 30 x 4 / 60 + 0.05 = 3.05 far and finds it,
 * yet past Distance 1 nothing shows (Near off, Nearest Distance -1, no normal, no object), as with
 * Check Every 1; at Distance 2 both show it. */
TEST(JoltLogicNodes, KeepDistanceOutputsTellOfTheCushionOnly)
{
  for (const float distance : {1.0f, 2.0f}) {
    for (const int every : {1, 4}) {
      SCOPED_TRACE(std::to_string(distance) + " every " + std::to_string(every));
      KeepDistanceTree tree(false, every);
      tree.tree.b.Float(*tree.keep, "Distance", distance);
      tree.tree.b.Bool(*tree.keep, "Apply", false);
      const std::shared_ptr<LN_Program> program = tree.tree.Compile();
      ASSERT_NE(program, nullptr);
      SemRig rig(program, kProduction);
      World world;
      Body &ball = Ball(rig, world);
      ball.controller.SetLinearVelocity(MT_Vector3(30.0f, 0.0f, 0.0f), false);
      Wall(rig, world, "Wall", 2.3f);
      rig.Step();
      rig.Step();
      KX_GameObject &owner = rig.Object(0);
      const bool near = distance > 1.8f;
      EXPECT_EQ(rig.Float(owner, "near"), near ? 1.0f : 0.0f);
      EXPECT_NEAR(rig.Float(owner, "gap"), near ? 1.8f : -1.0f, 1e-3f);
      ExpectVector(
          ReadVector(rig, owner, "normal"), MT_Vector3(near ? -1.0f : 0.0f, 0.0f, 0.0f), 1e-4f);
      EXPECT_EQ(rig.Text(owner, "what"), near ? "Wall" : "");
    }
  }
}

/** Measure From Object's Shape: a box turned 45 degrees reaches the wall with its corner (0.707
 * from its center), the Sphere with its Radius. */
TEST(JoltLogicNodes, KeepDistanceMeasuresFromTheObjectsOwnShapeOrTheSphere)
{
  for (const bool object_shape : {true, false}) {
    KeepDistanceTree tree(object_shape);
    const std::shared_ptr<LN_Program> program = tree.tree.Compile();
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kProduction);
    World world;
    KX_GameObject &owner = rig.Object(0);
    owner.NodeSetGlobalOrientation(MT_Matrix3x3(MT_Vector3(0.0f, 0.0f, float(M_PI / 4.0))));
    owner.NodeUpdateGS(0.0);
    world.Box(owner, MT_Vector3(0.5f, 0.5f, 0.5f), JPH::EMotionType::Dynamic);
    Wall(rig, world, "Wall", 1.3f);
    rig.Step();
    rig.Step();
    EXPECT_NEAR(rig.Float(owner, "gap"), object_shape ? 1.3f - 0.5f * float(M_SQRT2) : 0.8f, 2e-3f)
        << object_shape;
  }
}

/** A volume, not rays: a 2 cm pole is found, and a one-sided mesh quad from either side. */
TEST(JoltLogicNodes, KeepDistanceFindsThinPolesAndBothSidesOfAMesh)
{
  KeepDistanceTree tree;
  const std::shared_ptr<LN_Program> program = tree.tree.Compile();
  ASSERT_NE(program, nullptr);
  {
    SemRig rig(program, kProduction);
    World world;
    Body &ball = Ball(rig, world);
    world.Box(rig.AddObject("Pole", MT_Vector3(1.0f, 0.0f, 0.0f)), MT_Vector3(0.01f, 0.01f, 2.0f));
    rig.Step();
    rig.Step();
    EXPECT_NEAR(rig.Float(rig.Object(0), "gap"), 0.49f, 1e-3f);
    EXPECT_EQ(rig.Text(rig.Object(0), "what"), "Pole");
    EXPECT_LT(Acceleration(rig, ball).x(), -10.0f);
  }
  for (const bool flipped : {false, true}) {
    SemRig rig(program, kProduction);
    World world;
    Ball(rig, world);
    /* A quad in the plane x = 1 (Jolt's x is Blender's x), facing +x or -x. */
    JPH::VertexList vertices = {JPH::Float3(0.0f, -1.0f, -1.0f),
                                JPH::Float3(0.0f, 1.0f, -1.0f),
                                JPH::Float3(0.0f, -1.0f, 1.0f),
                                JPH::Float3(0.0f, 1.0f, 1.0f)};
    JPH::IndexedTriangleList triangles =
        flipped ? JPH::IndexedTriangleList{JPH::IndexedTriangle(0, 2, 1, 0),
                                           JPH::IndexedTriangle(1, 2, 3, 0)} :
                  JPH::IndexedTriangleList{JPH::IndexedTriangle(0, 1, 2, 0),
                                           JPH::IndexedTriangle(2, 1, 3, 0)};
    world.Add(rig.AddObject("Sheet", MT_Vector3(1.0f, 0.0f, 0.0f)),
              JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create().Get(),
              JPH::EMotionType::Static,
              false,
              1,
              nullptr);
    rig.Step();
    rig.Step();
    EXPECT_NEAR(rig.Float(rig.Object(0), "gap"), 0.5f, 1e-3f) << flipped;
    EXPECT_EQ(rig.Text(rig.Object(0), "what"), "Sheet") << flipped;
  }
}

/** Between two walls the pushes balance; off the middle only the near wall (within Distance)
 * pushes: 0.5 away, 10 m/s^2. */
TEST(JoltLogicNodes, KeepDistanceBalancesBetweenSurfaces)
{
  KeepDistanceTree tree;
  const std::shared_ptr<LN_Program> program = tree.tree.Compile();
  ASSERT_NE(program, nullptr);
  for (const float offset : {0.0f, 0.3f}) {
    SemRig rig(program, kProduction);
    World world;
    Body &ball = Ball(rig, world, MT_Vector3(offset, 0.0f, 0.0f));
    Wall(rig, world, "Right", 1.3f);
    Wall(rig, world, "Left", -1.3f);
    rig.Step();
    rig.Step();
    EXPECT_EQ(rig.Float(rig.Object(0), "near"), 1.0f);
    ExpectVector(Acceleration(rig, ball),
                 MT_Vector3(offset == 0.0f ? 0.0f : -10.0f, 0.0f, 0.0f),
                 1e-3f);
  }
}

/** The filters: Property, Mask, Ignore Object, the object's own child, sensors (unless Include
 * Sensors). Each case: is the wall kept away from? */
TEST(JoltLogicNodes, KeepDistanceFiltersWhatItKeepsAwayFrom)
{
  enum Case { Property, PropertyHeld, Mask, IgnoreObject, Child, Sensor, SensorIncluded };
  for (const Case filter :
       {Property, PropertyHeld, Mask, IgnoreObject, Child, Sensor, SensorIncluded})
  {
    SCOPED_TRACE(int(filter));
    KeepDistanceTree tree;
    if (filter == Property || filter == PropertyHeld) {
      tree.tree.b.String(*tree.keep, "Property", "hard");
    }
    if (filter == Mask) {
      tree.tree.b.Int(*tree.keep, "Mask", 2);
    }
    if (filter == IgnoreObject) {
      GenNode &get = tree.tree.b.Add("LogicNativeObjectByName");
      tree.tree.b.String(get, "Name", "Wall");
      tree.tree.b.Link(get, "Object", *tree.keep, "Ignore Object");
    }
    if (filter == SensorIncluded) {
      tree.tree.b.Bool(*tree.keep, "Include Sensors", true);
    }
    const std::shared_ptr<LN_Program> program = tree.tree.Compile();
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kProduction);
    World world;
    Ball(rig, world);
    KX_GameObject &wall = rig.AddObject("Wall", MT_Vector3(1.8f, 0.0f, 0.0f));
    if (filter == Child) {
      rig.Parent(rig.ObjectCount() - 1, 0);
    }
    world.Box(wall,
              MT_Vector3(0.5f, 2.0f, 2.0f),
              JPH::EMotionType::Static,
              filter == Sensor || filter == SensorIncluded,
              1);
    if (filter == PropertyHeld) {
      rig.SetFloat(wall, "hard", 1.0f);
    }
    rig.Step();
    rig.Step();
    const bool kept_away = filter == PropertyHeld || filter == SensorIncluded;
    EXPECT_EQ(rig.Float(rig.Object(0), "near"), kept_away ? 1.0f : 0.0f);
  }
}

/** Check Every 4: between checks the surfaces found are remembered and measured again from the
 * object's move (a wall taken away still pushes until the next check); every step finds it at
 * once. */
TEST(JoltLogicNodes, KeepDistanceCheckEveryRemembersTheSurfacesBetweenChecks)
{
  for (const int every : {1, 4}) {
    SCOPED_TRACE(every);
    KeepDistanceTree tree(false, every);
    const std::shared_ptr<LN_Program> program = tree.tree.Compile();
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kProduction);
    World world;
    Ball(rig, world);
    Body &wall = Wall(rig, world, "Wall", 1.3f);
    rig.Step();
    rig.Step();
    EXPECT_NEAR(rig.Float(rig.Object(0), "gap"), 0.8f, 1e-4f);
    /* The ball moves 0.03 toward the wall (within the check's look-ahead): measured again. */
    rig.Object(0).NodeSetWorldPosition(MT_Vector3(0.03f, 0.0f, 0.0f));
    rig.Object(0).NodeUpdateGS(0.0);
    rig.Step();
    rig.Step();
    EXPECT_NEAR(rig.Float(rig.Object(0), "gap"), 0.77f, 1e-4f);
    /* Step 4: a check (the rig's steps count from 0; this tree's checks fall on 0, 4, 8). */
    rig.Step();
    /* The wall goes far away: found gone at once with every step, remembered until the next
     * check (step 8) with 4. */
    world.environment.GetBodyInterface().SetPosition(
        wall.body_id, JPH::RVec3(50.0f, 0.0f, 0.0f), JPH::EActivation::DontActivate);
    int steps_still_near = 0;
    for (int step = 0; step < 6; step++) {
      rig.Step();
      if (rig.Float(rig.Object(0), "near") == 1.0f) {
        steps_still_near++;
      }
    }
    if (every == 1) {
      EXPECT_EQ(steps_still_near, 1); /* (the step whose outputs read the check before) */
    }
    else {
      /* Steps 5, 6, 7 and 8 read a wall remembered since step 4. */
      EXPECT_EQ(steps_still_near, 4);
    }
    EXPECT_EQ(rig.Float(rig.Object(0), "near"), 0.0f);
  }
}

/** Damping brakes the movement toward the wall: falling toward it at 2 m/s adds
 * 2 x 1 x sqrt(20 / 1) x 2 x 0.2 = 3.578 m/s^2 to the 4 of the gap. */
TEST(JoltLogicNodes, KeepDistanceDampingBrakesTheApproach)
{
  KeepDistanceTree tree;
  const std::shared_ptr<LN_Program> program = tree.tree.Compile();
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction);
  World world;
  Body &ball = Ball(rig, world);
  Wall(rig, world, "Wall", 1.3f);
  world.environment.GetBodyInterface().SetLinearVelocity(ball.body_id, JPH::Vec3(2.0f, 0.0f, 0.0f));
  rig.Step();
  rig.Step();
  ExpectVector(Acceleration(rig, ball),
               MT_Vector3(-(4.0f + 2.0f * std::sqrt(20.0f) * 2.0f * 0.2f), 0.0f, 0.0f),
               1e-3f);
}

/** Many Keep Distance nodes: their queries run together on the physics threads (8 here: 7
 * helpers and the caller) and give every object exactly what one thread alone gives (no helpers:
 * the batch runs them one after the other, without jobs). */
TEST(JoltLogicNodes, KeepDistanceGivesTheSameOnOneThreadAndOnMany)
{
  KeepDistanceTree tree;
  const std::shared_ptr<LN_Program> program = tree.tree.Compile();
  ASSERT_NE(program, nullptr);
  constexpr uint32_t owners = 48;
  std::string results[2];
  for (const int helpers : {0, 7}) {
    SemRig rig(program, kProduction, owners);
    World world(owners + 16, helpers);
    for (uint32_t index = 0; index < owners; index++) {
      KX_GameObject &owner = rig.Object(index);
      const float angle = 2.0f * float(M_PI) * float(index) / float(owners);
      owner.NodeSetWorldPosition(MT_Vector3(1.6f * std::cos(angle), 1.6f * std::sin(angle), 0.0f));
      owner.NodeUpdateGS(0.0);
      world.Add(owner, new JPH::SphereShape(0.3f), JPH::EMotionType::Dynamic, false, 1, nullptr);
    }
    /* A room 5 x 5 around them. */
    for (const auto &[name, center, half] :
         {std::tuple{"East", MT_Vector3(3.0f, 0, 0), MT_Vector3(0.5f, 3, 2)},
          std::tuple{"West", MT_Vector3(-3.0f, 0, 0), MT_Vector3(0.5f, 3, 2)},
          std::tuple{"North", MT_Vector3(0, 3.0f, 0), MT_Vector3(3, 0.5f, 2)},
          std::tuple{"South", MT_Vector3(0, -3.0f, 0), MT_Vector3(3, 0.5f, 2)}})
    {
      world.Box(rig.AddObject(name, center), half);
    }
    world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
    rig.Step();
    rig.Step();
    std::string &dump = results[helpers == 0 ? 0 : 1];
    for (uint32_t index = 0; index < owners; index++) {
      KX_GameObject &owner = rig.Object(index);
      for (const char *field : {"forceX", "forceY", "forceZ", "gap"}) {
        dump += LN_DumpFloatBits(rig.Float(owner, field)) + " ";
      }
      dump += rig.Text(owner, "what") + "\n";
    }
  }
  EXPECT_EQ(results[0], results[1]);
  /* (The balls are nearer each other than the walls: a ball's nearest surface is a neighbour.) */
  EXPECT_NE(results[0].find(" Obj"), std::string::npos);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Benchmarks (DISABLED_: run with --gtest_also_run_disabled_tests)
 * \{ */

namespace {

double MillisecondsPerStep(SemRig &rig, const int steps)
{
  for (int warm = 0; warm < 5; warm++) {
    rig.Step();
  }
  const auto start = std::chrono::steady_clock::now();
  for (int step = 0; step < steps; step++) {
    rig.Step();
  }
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
             .count() /
         steps;
}

}  // namespace

/** Explode or Implode Impulse among N dynamic boxes, all inside its Radius: the cost of one step
 * of its tree (the query, the recorded pushes and the flush that applies them), against the same
 * tree without the node. UPBGE_LN_BENCH_STEPS sets the steps timed (default 400). */
TEST(JoltLogicNodes, DISABLED_ExplodeOrImplodeImpulseCost)
{
  const char *steps_text = std::getenv("UPBGE_LN_BENCH_STEPS");
  const int steps = steps_text ? std::max(1, std::atoi(steps_text)) : 400;
  for (const int count : {10, 100, 256, 1000}) {
    double ms[2] = {0.0, 0.0};
    for (const bool with_node : {false, true}) {
      Tree tree;
      if (with_node) {
        GenNode &blast = tree.Add("LogicNativeExplosionImpulse", 0 /* Constant */, 0 /* Position */);
        tree.b.Link(*tree.step, "Out", blast, "Flow");
        tree.b.Vector(blast, "Origin", 0.0f, 0.0f, 0.0f);
        tree.b.Float(blast, "Radius", 1000.0f);
        tree.b.Float(blast, "Strength", 1.0f);
      }
      else {
        /* The same event with a node that records nothing. */
        GenNode &print = tree.Add("LogicNativeValueFloat");
        (void)print;
      }
      const std::shared_ptr<LN_Program> program = tree.Compile();
      ASSERT_NE(program, nullptr);
      SemRig rig(program, kProduction);
      /* (The rig's text dump of every recorded command would be most of the time.) */
      rig.SetDump(false);
      World world(count + 64);
      const int side = int(std::ceil(std::cbrt(double(count))));
      for (int index = 0; index < count; index++) {
        const MT_Vector3 position(float(index % side) * 2.0f - side,
                                  float((index / side) % side) * 2.0f - side,
                                  float(index / (side * side)) * 2.0f + 3.0f);
        world.Box(rig.AddObject("Crate" + std::to_string(index), position),
                  MT_Vector3(0.25f, 0.25f, 0.25f),
                  JPH::EMotionType::Dynamic);
      }
      world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
      ms[with_node ? 1 : 0] = MillisecondsPerStep(rig, steps);
      if (with_node) {
        /* Where it goes: the trees' run (the query and the recorded pushes) and the flush. */
        rig.ResetTimes();
        for (int step = 0; step < steps; step++) {
          rig.Step();
        }
        std::printf("    capture %.4f (plan %.4f), record %.4f ms, flush %.4f ms a step\n",
                    rig.Times().capture_ms / steps,
                    rig.Times().plan_ms / steps,
                    rig.Times().record_ms / steps,
                    rig.Times().flush_ms / steps);
        /* The query alone: the zero-length shape cast the node asked before 2026-11-12, and the
         * overlap it asks now (the same bodies: ExplosionOverlapFinds...). */
        PHY_ShapeCastSettings settings;
        settings.type = PHY_ShapeCastType::Sphere;
        settings.radius = 1000.0f;
        settings.max_results = 256;
        std::vector<PHY_ShapeCastResult> results;
        PHY_IShapeCastFilterCallback accept_all(nullptr, nullptr);
        const auto cast_start = std::chrono::steady_clock::now();
        for (int step = 0; step < steps; step++) {
          world.environment.ShapeCast(settings, accept_all, results);
        }
        const double cast_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - cast_start)
                                   .count() /
                               steps;
        std::vector<PHY_IPhysicsController *> controllers;
        const auto overlap_start = std::chrono::steady_clock::now();
        for (int step = 0; step < steps; step++) {
          world.environment.OverlapSphereBodies(
              MT_Vector3(0.0f, 0.0f, 0.0f), 1000.0f, accept_all, 256, controllers);
        }
        const double overlap_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - overlap_start)
                                      .count() /
                                  steps;
        std::printf("    the query alone: the old shape cast %.4f ms (%zu hits), the overlap "
                    "%.4f ms (%zu bodies)\n",
                    cast_ms,
                    results.size(),
                    overlap_ms,
                    controllers.size());
      }
    }
    const double node_ms = ms[1] - ms[0];
    std::printf("Explode or Implode Impulse, %4d bodies pushed: step %.4f ms (empty tree %.4f), "
                "the node %.4f ms = %.2f us a body\n",
                count,
                ms[1],
                ms[0],
                node_ms,
                1000.0 * node_ms / std::min(count, 256));
  }
}

/** Keep Distance on N balls (radius 0.2, 0.8 apart in a grid: each within reach of its
 * neighbours), each its own tree: the step's cost (the trees' runs and the flush that answers
 * them) on 1, 2 and 8 physics threads, checking every step or every 4th, against the same trees
 * without the node. UPBGE_LN_BENCH_STEPS sets the steps timed (default 200). */
TEST(JoltLogicNodes, DISABLED_KeepDistanceCost)
{
  const char *steps_text = std::getenv("UPBGE_LN_BENCH_STEPS");
  const int steps = steps_text ? std::max(1, std::atoi(steps_text)) : 200;
  for (const uint32_t count : {100u, 1000u}) {
    double baseline = 0.0;
    for (const int every : {0 /* no node */, 1, 4}) {
      /* (Threads = Jolt's helpers + the calling thread.) */
      for (const int threads : {1, 2, 8}) {
        if (every == 0 && threads > 1) {
          continue;
        }
        Tree tree;
        if (every > 0) {
          GenNode &keep = tree.Add("LogicNativeKeepDistance", 0, 0 /* Object's Shape */);
          tree.b.Link(*tree.step, "Out", keep, "Flow");
          tree.b.Int(keep, "Check Every", every);
        }
        else {
          tree.Add("LogicNativeValueFloat");
        }
        const std::shared_ptr<LN_Program> program = tree.Compile();
        ASSERT_NE(program, nullptr);
        SemRig rig(program, kProduction, count);
        /* (The rig's command dump and force notes are text a game does not make.) */
        rig.SetDump(false);
        rig.SetObserveForces(false);
        World world(int(count) + 64, threads - 1);
        const int side = int(std::ceil(std::cbrt(double(count))));
        for (uint32_t index = 0; index < count; index++) {
          KX_GameObject &owner = rig.Object(index);
          owner.NodeSetWorldPosition(MT_Vector3(float(index % side) * 0.8f,
                                                float((index / side) % side) * 0.8f,
                                                float(index / (side * side)) * 0.8f));
          owner.NodeUpdateGS(0.0);
          world.Add(owner, new JPH::SphereShape(0.2f), JPH::EMotionType::Dynamic, false, 1, nullptr);
        }
        world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
        const double ms = MillisecondsPerStep(rig, steps);
        if (every == 0) {
          baseline = ms;
          std::printf("Keep Distance, %4u balls: the trees without the node %.4f ms a step\n",
                      count,
                      ms);
          continue;
        }
        std::printf("Keep Distance, %4u balls, Check Every %d, %d thread%s: %.4f ms a step, the "
                    "node %.4f ms = %.2f us a ball\n",
                    count,
                    every,
                    threads,
                    threads == 1 ? " " : "s",
                    ms,
                    ms - baseline,
                    1000.0 * (ms - baseline) / count);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Speed and Velocity at Point (2026-11-14)
 * \{ */

/** Get Object Attribute's Speed is the length of the body's velocity (Float::VectorLength of World
 * Linear Velocity), for one owner and for a crowd (a wide run reads the velocities on the main
 * thread and takes their lengths on the lanes: FLength). */
TEST(JoltLogicNodes, SpeedIsTheLengthOfTheBodysVelocity)
{
  Tree tree;
  GenNode &speed = tree.Add("LogicNativeGetObjectAttribute", 15);
  tree.StoreFloat(speed, "Speed", "speed");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : Modes()) {
    for (const uint32_t owners : {1u, 32u}) {
     for (const int columns : {0, 1, 4}) {
      SCOPED_TRACE(std::string(mode.name) + " x" + std::to_string(owners) + " columns " +
                   std::to_string(columns));
      SemRig rig(program, mode, owners);
      /* 0: every tree its column pass; 1: wide runs on this thread; 4: on 3 helpers too. */
      if (columns != 0) {
        rig.SetColumns(true, uint32_t(columns - 1));
      }
      World world(owners + 8);
      std::vector<MT_Vector3> velocities;
      for (uint32_t index = 0; index < owners; index++) {
        KX_GameObject &owner = rig.Object(index);
        owner.NodeSetWorldPosition(MT_Vector3(float(index) * 3.0f, 0.0f, 0.0f));
        owner.NodeUpdateGS(0.0);
        Body &body = world.Box(owner, MT_Vector3(0.5f, 0.5f, 0.5f), JPH::EMotionType::Dynamic);
        velocities.push_back(MT_Vector3(3.0f + float(index), -4.0f, 0.5f * float(index)));
        world.environment.GetBodyInterface().SetLinearVelocity(
            body.body_id, JoltMath::ToJolt(velocities.back()));
      }
      /* (The first step runs On Init: no lanes then.) */
      uint64_t lanes = 0;
      for (int step = 0; step < 3; step++) {
        rig.Step();
        lanes += rig.LastColumnLanes();
      }
      for (uint32_t index = 0; index < owners; index++) {
        EXPECT_NEAR(rig.Float(rig.Object(index), "speed"), velocities[index].length(), 1.0e-4f)
            << "owner " << index;
      }
      /* (UPBGE_LN_COLUMN_DIRECT=force runs every program as a direct program: no lanes to
       * count; the speeds are still checked.) */
      if (columns != 0 && owners > 1 &&
          LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force)
      {
        /* The crowd ran wide (the velocity read and FLength), not tree by tree. */
        EXPECT_GE(lanes, uint64_t(owners)) << rig.LastColumnRefusal();
      }
     }
    }
  }
}

/** Set Rigid Body Attribute's Linear Damping and Angular Damping (2026-11-15) each set one damping
 * and leave the other as it is; both in one step both apply (the command is never merged); below
 * 0 is 0; a node still holding the old Damping (3: the editor turns it into Linear Damping when a
 * file is read) sets both. The dampings come from game properties: runtime-linked inputs. */
TEST(JoltLogicNodes, LinearAndAngularDampingAreSetApart)
{
  struct Case {
    const char *name;
    std::vector<int16_t> attributes;
    float linear, angular;
    float expected_linear, expected_angular;
  };
  constexpr int16_t old_damping = int16_t(LN_RigidBodyAttribute::Damping);
  constexpr int16_t linear = int16_t(LN_RigidBodyAttribute::LinearDamping);
  constexpr int16_t angular = int16_t(LN_RigidBodyAttribute::AngularDamping);
  const Case cases[] = {
      {"linear only", {linear}, 3.0f, 9.0f, 3.0f, 0.05f},
      {"angular only", {angular}, 9.0f, 2.0f, 0.05f, 2.0f},
      {"both in one step", {linear, angular}, 1.5f, 0.5f, 1.5f, 0.5f},
      {"below 0 is 0", {angular, linear}, -1.0f, -2.0f, 0.0f, 0.0f},
      {"the old Damping sets both", {old_damping}, 4.0f, 6.0f, 4.0f, 6.0f},
  };
  for (const Case &c : cases) {
    Tree tree;
    for (const int16_t attribute : c.attributes) {
      GenNode &set = tree.Add("LogicNativeSetRigidBodyAttribute", attribute);
      for (const auto &[pin, property] : {std::pair{"Linear Damping", "linear"},
                                          std::pair{"Angular Damping", "angular"}})
      {
        GenNode &read = tree.Add("LogicNativeGetGamePropertyFloat");
        tree.b.String(read, "Property", property);
        tree.b.Link(read, "Value", set, pin);
      }
      tree.b.Link(*tree.flow, tree.flow_pin, set, "Flow");
      tree.flow = &set;
      tree.flow_pin = "Done";
    }
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr) << c.name;
    for (const SemMode &mode : Modes()) {
      SCOPED_TRACE(std::string(c.name) + ", " + mode.name);
      SemRig rig(program, mode, 1);
      World world;
      KX_GameObject &owner = rig.Object(0);
      Body &body = world.Box(owner, MT_Vector3(0.5f, 0.5f, 0.5f), JPH::EMotionType::Dynamic);
      body.controller.SetDamping(0.05f, 0.05f);
      rig.SetFloat(owner, "linear", c.linear);
      rig.SetFloat(owner, "angular", c.angular);
      rig.Step();
      rig.Step();
      EXPECT_NEAR(body.controller.GetLinearDamping(), c.expected_linear, 1.0e-6f);
      EXPECT_NEAR(body.controller.GetAngularDamping(), c.expected_angular, 1.0e-6f);
    }
  }
}

namespace {

using AxisLocks = std::array<bool, 6>;

AxisLocks LocksOf(const JoltPhysicsController &controller)
{
  AxisLocks locks;
  controller.GetRigidBodyAxisLocks(locks[0], locks[1], locks[2], locks[3], locks[4], locks[5]);
  return locks;
}

std::string LocksText(const AxisLocks &locks)
{
  std::string text = "T";
  for (int axis = 0; axis < 6; axis++) {
    text += axis == 3 ? " R" : "";
    text += locks[axis] ? "1" : "0";
  }
  return text;
}

/** The degrees of freedom Jolt has for the body now. */
JPH::EAllowedDOFs JoltDOFsOf(World &world, const Body &body)
{
  JPH::BodyLockRead lock(world.environment.GetPhysicsSystem()->GetBodyLockInterface(),
                         body.body_id);
  return lock.Succeeded() && lock.GetBody().GetMotionPropertiesUnchecked() != nullptr ?
             lock.GetBody().GetMotionPropertiesUnchecked()->GetAllowedDOFs() :
             JPH::EAllowedDOFs::None;
}

/** A Rigid Body box (physics rotation on) for `owner`, with `start` locks as the Physics panel
 * gives them. */
Body &LockedRigidBox(World &world, KX_GameObject &owner, const AxisLocks &start)
{
  Body &body = world.Box(owner, MT_Vector3(0.5f, 0.5f, 0.5f), JPH::EMotionType::Dynamic);
  body.controller.SetRuntimePhysicsTypeSupported(true);
  body.controller.SetRigidBody(true);
  EXPECT_TRUE(body.controller.SetRigidBodyAxisLocks(
      start[0], start[1], start[2], start[3], start[4], start[5]));
  return body;
}

/** One Set Rigid Body Attribute (Axis Locks) after the flow: Lock and the two rows (each axis not
 * 0 chosen), constants. */
GenNode &AxisLocksNode(Tree &tree, const bool lock, const MT_Vector3 &translation,
                       const MT_Vector3 &rotation)
{
  GenNode &set = tree.Add("LogicNativeSetRigidBodyAttribute",
                          int16_t(LN_RigidBodyAttribute::AxisLocks));
  tree.b.Bool(set, "Lock", lock);
  tree.b.Vector(set, "Lock Translation", translation.x(), translation.y(), translation.z());
  tree.b.Vector(set, "Lock Rotation", rotation.x(), rotation.y(), rotation.z());
  tree.b.Link(*tree.flow, tree.flow_pin, set, "Flow");
  tree.flow = &set;
  tree.flow_pin = "Done";
  return set;
}

}  // namespace

/* Set Rigid Body Attribute's Axis Locks (2026-11-15) changes each axis on its own: the chosen
 * axes (the rows' axes not 0) are locked or freed as Lock says, every other axis keeps its lock
 * (here the Physics panel's); two nodes of one step combine in order; nothing chosen changes
 * nothing; Lock and the rows may be linked. Jolt's degrees of freedom follow, in every mode. */
TEST(JoltLogicNodes, AxisLocksChangeEachAxisOnItsOwn)
{
  constexpr bool F = false;
  constexpr bool T = true;
  const MT_Vector3 none(0.0f, 0.0f, 0.0f);
  struct Case {
    const char *name;
    AxisLocks start;
    std::function<void(Tree &)> nodes;
    AxisLocks expected;
    /* The game property "lock" a linked Lock reads (both ways: a constant read instead of the
     * link is caught whatever the constant). */
    bool property_lock = false;
  };
  const auto linked = [&](Tree &tree) {
    GenNode &set = AxisLocksNode(tree, true, none, none);
    GenNode &lock = tree.Add("LogicNativeGetGamePropertyBool");
    tree.b.String(lock, "Property", "lock");
    tree.b.Link(lock, "Value", set, "Lock");
    GenNode &axes = tree.Add("LogicNativeGetGamePropertyVector");
    tree.b.String(axes, "Property", "axes");
    tree.b.Link(axes, "Value", set, "Lock Translation");
    tree.b.Link(axes, "Value", set, "Lock Rotation");
  };
  const Case cases[] = {
      {"a 2.5D body locks rotation X and Z",
       {F, T, F, F, F, F},
       [&](Tree &tree) { AxisLocksNode(tree, true, none, MT_Vector3(1.0f, 0.0f, 1.0f)); },
       {F, T, F, T, F, T}},
      {"one axis freed, the others kept",
       {F, T, F, T, F, T},
       [&](Tree &tree) { AxisLocksNode(tree, false, none, MT_Vector3(1.0f, 0.0f, 0.0f)); },
       {F, T, F, F, F, T}},
      {"two nodes of one step combine in order",
       {F, F, F, F, F, F},
       [&](Tree &tree) {
         AxisLocksNode(tree, true, MT_Vector3(1.0f, 1.0f, 1.0f), none);
         AxisLocksNode(tree, false, MT_Vector3(0.0f, 1.0f, 0.0f), none);
       },
       {T, F, T, F, F, F}},
      {"nothing chosen changes nothing",
       {F, T, F, F, F, T},
       [&](Tree &tree) { AxisLocksNode(tree, true, none, none); },
       {F, T, F, F, F, T}},
      {"Lock and the rows linked: free", {F, T, F, F, T, F}, linked, {F, F, F, F, F, F}, false},
      {"Lock and the rows linked: lock", {F, F, F, F, F, F}, linked, {F, T, F, F, T, F}, true},
  };
  for (const Case &c : cases) {
    Tree tree;
    c.nodes(tree);
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr) << c.name;
    for (const SemMode &mode : Modes()) {
      SCOPED_TRACE(std::string(c.name) + ", " + mode.name);
      SemRig rig(program, mode, 1);
      World world;
      KX_GameObject &owner = rig.Object(0);
      Body &body = LockedRigidBox(world, owner, c.start);
      /* (The linked cases: Lock from "lock", axes translation Y and rotation Y.) */
      rig.SetBool(owner, "lock", c.property_lock);
      EXP_Value *axes = new EXP_Vector3Value(0.0f, 1.0f, 0.0f);
      owner.SetProperty("axes", axes);
      axes->Release();
      rig.Step();
      rig.Step();
      EXPECT_EQ(LocksText(LocksOf(body.controller)), LocksText(c.expected));
      EXPECT_EQ(JoltDOFsOf(world, body), JoltGameStateAllowedDOFs(true, c.expected));
    }
  }
}

/* A body is never left without a free axis (2026-11-15; Jolt "will crash with a division by zero
 * later", bodies vanish): locking the last free axis is refused with a warning naming the node
 * (the locks stay, Jolt too); so is turning off physics rotation with every translation axis
 * locked (the body warns); a physics type change to Dynamic (no rotation) with every translation
 * axis locked drops the locks (warned), so the stored locks stay what Jolt has. The same locks
 * again change nothing: Jolt's mass properties are not computed again. */
TEST(JoltLogicNodes, AxisLocksNeverLeaveABodyWithoutAFreeAxis)
{
  constexpr bool F = false;
  constexpr bool T = true;
  const MT_Vector3 none(0.0f, 0.0f, 0.0f);
  {
    SCOPED_TRACE("locking the last free axis");
    Tree tree;
    AxisLocksNode(tree, true, none, MT_Vector3(0.0f, 0.0f, 1.0f));
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kProduction, 1);
    World world;
    const AxisLocks start = {T, T, T, T, T, F};
    Body &body = LockedRigidBox(world, rig.Object(0), start);
    LN_ResetCommandFailureWarningsForTests();
    rig.Step();
    rig.Step();
    /* (The rig keeps what its flushes print: the warnings.) */
    const std::string printed = rig.Printed();
    EXPECT_EQ(LocksText(LocksOf(body.controller)), LocksText(start));
    EXPECT_EQ(JoltDOFsOf(world, body), JoltGameStateAllowedDOFs(true, start));
    EXPECT_NE(JoltDOFsOf(world, body), JPH::EAllowedDOFs::None);
    EXPECT_NE(printed.find("no free axis"), std::string::npos) << printed;
    /* Warned once (two steps). */
    EXPECT_EQ(printed.find("no free axis"), printed.rfind("no free axis")) << printed;
  }
  {
    SCOPED_TRACE("turning off physics rotation with every translation axis locked");
    Tree tree;
    GenNode &set = tree.Add("LogicNativeSetRigidBodyAttribute",
                            int16_t(LN_RigidBodyAttribute::AllowPhysicsRotation));
    tree.b.Bool(set, "Enabled", false);
    tree.b.Link(*tree.flow, tree.flow_pin, set, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kProduction, 1);
    World world;
    const AxisLocks start = {T, T, T, F, F, F};
    Body &body = LockedRigidBox(world, rig.Object(0), start);
    rig.Step();
    const std::string printed = rig.Printed();
    EXPECT_TRUE(body.controller.GetRigidBodyRotationEnabled());
    EXPECT_EQ(JoltDOFsOf(world, body), JoltGameStateAllowedDOFs(true, start));
    EXPECT_NE(printed.find("physics rotation was refused"), std::string::npos) << printed;
  }
  /* (An owner for the bodies below: a rig object; no step runs.) */
  Tree idle_tree;
  AxisLocksNode(idle_tree, true, none, none);
  const std::shared_ptr<LN_Program> idle = idle_tree.Compile();
  ASSERT_NE(idle, nullptr);
  {
    SCOPED_TRACE("a physics type change to Dynamic with every translation axis locked");
    SemRig rig(idle, kProduction, 1);
    World world;
    KX_GameObject &owner = rig.Object(0);
    Body &body = LockedRigidBox(world, owner, {T, T, T, F, T, F});
    testing::internal::CaptureStdout();
    EXPECT_TRUE(body.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Dynamic));
    const std::string printed = testing::internal::GetCapturedStdout();
    EXPECT_FALSE(body.controller.GetRigidBodyRotationEnabled());
    EXPECT_EQ(LocksText(LocksOf(body.controller)), LocksText({F, F, F, F, F, F}));
    EXPECT_EQ(JoltDOFsOf(world, body), JoltGameStateAllowedDOFs(false, {F, F, F, F, F, F}));
    EXPECT_NE(printed.find("axis locks were dropped"), std::string::npos) << printed;
  }
  {
    SCOPED_TRACE("the same locks again: no recompute");
    SemRig rig(idle, kProduction, 1);
    World world;
    KX_GameObject &owner = rig.Object(0);
    const AxisLocks start = {F, T, F, T, F, F};
    Body &body = LockedRigidBox(world, owner, start);
    const auto inverse_inertia = [&]() {
      JPH::BodyLockRead lock(world.environment.GetPhysicsSystem()->GetBodyLockInterface(),
                             body.body_id);
      return lock.GetBody().GetMotionPropertiesUnchecked()->GetInverseInertiaDiagonal();
    };
    {
      /* A mark only a recompute of the mass properties takes away. */
      JPH::BodyLockWrite lock(world.environment.GetPhysicsSystem()->GetBodyLockInterface(),
                              body.body_id);
      lock.GetBody().GetMotionPropertiesUnchecked()->SetInverseInertia(
          JPH::Vec3(5.0f, 6.0f, 7.0f), JPH::Quat::sIdentity());
    }
    EXPECT_TRUE(body.controller.SetRigidBodyAxisLocks(
        start[0], start[1], start[2], start[3], start[4], start[5]));
    EXPECT_TRUE(inverse_inertia() == JPH::Vec3(5.0f, 6.0f, 7.0f));
    EXPECT_TRUE(body.controller.SetRigidBodyAxisLocks(F, T, F, F, F, F));
    EXPECT_FALSE(inverse_inertia() == JPH::Vec3(5.0f, 6.0f, 7.0f));
  }
}

/** Velocity at Point (Position): the body's movement plus its spin around its centre of mass
 * (Jolt's point velocity), for the owner, an object without a body of its own carried by its
 * parent's (the owner's), and an object no body carries (zero). Point is the Position. */
TEST(JoltLogicNodes, VelocityAtPointIsTheBodysMovementPlusItsSpin)
{
  BlenderObject tip_blender("Tip");
  BlenderObject loose_blender("Loose");
  Tree tree;
  const auto measure = [&](blender::Object *object, const char *prefix) {
    GenNode &at = tree.Add("LogicNativeVelocityAtPoint", 0);
    if (object != nullptr) {
      tree.b.ObjectNamed(at, "Object", object);
    }
    tree.b.Vector(at, "Position", 3.0f, 1.0f, 2.0f);
    tree.StoreVector(at, "Velocity", std::string(prefix) + "velocity");
    tree.StoreFloat(at, "Speed", tree.names.emplace_back(std::string(prefix) + "speed").c_str());
    tree.StoreVector(at, "Point", std::string(prefix) + "point");
  };
  measure(nullptr, "own_");
  measure(&tip_blender.object, "tip_");
  measure(&loose_blender.object, "loose_");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    World world;
    KX_GameObject &owner = rig.Object(0);
    Body &body = world.Box(owner, MT_Vector3(0.5f, 0.5f, 0.5f), JPH::EMotionType::Dynamic);
    const MT_Vector3 linear(1.0f, 2.0f, 0.0f);
    const MT_Vector3 angular(0.0f, 0.0f, 2.0f);
    world.environment.GetBodyInterface().SetLinearVelocity(body.body_id, JoltMath::ToJolt(linear));
    world.environment.GetBodyInterface().SetAngularVelocity(body.body_id,
                                                            JoltMath::ToJolt(angular));
    const size_t tip_index = rig.ObjectCount();
    KX_GameObject &tip = rig.AddObject("Tip", MT_Vector3(0.0f, 1.0f, 0.0f));
    tip.SetBlenderObject(&tip_blender.object);
    rig.Parent(tip_index, 0);
    KX_GameObject &loose = rig.AddObject("Loose", MT_Vector3(9.0f, 0.0f, 0.0f));
    loose.SetBlenderObject(&loose_blender.object);
    rig.Step();
    /* v + w x (p - centre of mass): (1, 2, 0) + (0, 0, 2) x (3, 1, 2) = (-1, 8, 0). */
    const MT_Vector3 point(3.0f, 1.0f, 2.0f);
    const MT_Vector3 expected = linear + angular.cross(point);
    for (const char *prefix : {"own_", "tip_"}) {
      SCOPED_TRACE(prefix);
      ExpectVector(ReadVector(rig, owner, std::string(prefix) + "velocity"), expected, 1.0e-4f);
      EXPECT_NEAR(rig.Float(owner, (std::string(prefix) + "speed").c_str()),
                  expected.length(),
                  1.0e-4f);
      ExpectVector(ReadVector(rig, owner, std::string(prefix) + "point"), point, 1.0e-5f);
    }
    ExpectVector(ReadVector(rig, owner, "loose_velocity"), MT_Vector3(0, 0, 0), 0.0f);
    EXPECT_EQ(rig.Float(owner, "loose_speed"), 0.0f);
    ExpectVector(ReadVector(rig, owner, "loose_point"), point, 1.0e-5f);
  }
}

/** Velocity at Point is measured where its route first reads it, not when the tick starts (no
 * warm-up, as the column kernel QPointVel measures it): after an Apply Movement of the owner that
 * a Get Distance from it applied, its Position (the owner's live World Position) and Point are the
 * moved ones. */
TEST(JoltLogicNodes, VelocityAtPointIsMeasuredWhereItsRouteReadsIt)
{
  Tree tree;
  GenNode &move = tree.Add("LogicNativeApplyMovement");
  tree.b.Vector(move, "Vector", 0.0f, 0.5f, 0.0f);
  tree.b.Link(*tree.flow, tree.flow_pin, move, "Flow");
  tree.flow = &move;
  tree.flow_pin = "Done";
  /* Get Distance from the owner (Object A empty) to a point: its read applies the move. */
  GenNode &distance = tree.Add("LogicNativeGetDistance", 0, 1);
  tree.b.Vector(distance, "Position B", 10.0f, 0.0f, 0.0f);
  tree.StoreFloat(distance, "Distance", "distance");
  GenNode &where = tree.Add("LogicNativeGetObjectAttribute", 0 /* World Position */);
  GenNode &at = tree.Add("LogicNativeVelocityAtPoint", 0, 0);
  tree.b.Link(where, "Vector", at, "Position");
  tree.StoreVector(at, "Point", "p");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    World world;
    KX_GameObject &owner = rig.Object(0);
    owner.NodeSetWorldPosition(MT_Vector3(0.0f, 1.0f, 0.0f));
    owner.NodeUpdateGS(0.0);
    world.Box(owner, MT_Vector3(0.5f, 0.5f, 0.5f), JPH::EMotionType::Dynamic);
    rig.Step();
    /* Moved from y 1 to 1.5 before the read (the warm-up would have measured at 1). */
    EXPECT_NEAR(rig.Float(owner, "distance"), std::sqrt(100.0f + 1.5f * 1.5f), 1.0e-4f);
    ExpectVector(ReadVector(rig, owner, "p"), MT_Vector3(0.0f, 1.5f, 0.0f), 1.0e-5f);
  }
}

/** Velocity at Point in a crowd: a wide run (the column kernel QPointVel, the VM's own code per
 * NPC) gives the column pass's numbers bit for bit, on one thread and on helpers. Variants: a
 * constant Position; the owner's live World Position after an Apply Movement of the owner on the
 * route (pending, unapplied in both); a named object's body; a Vertex Group mode (measured when
 * the run starts; the rig has no meshes: nothing measured, the plumbing compared). */
TEST(JoltLogicNodes, VelocityAtPointRunsWideAsItsColumnPass)
{
  BlenderObject marker_blender("Marker");
  const uint32_t owners = 300; /* two chunks */
  const char *const labels[] = {"constant Position",
                                "own live position after a move",
                                "a named object",
                                "a vertex group"};
  for (int variant = 0; variant < int(std::size(labels)); variant++) {
    SCOPED_TRACE(labels[variant]);
    Tree tree;
    if (variant == 1) {
      GenNode &move = tree.Add("LogicNativeApplyMovement");
      tree.b.Vector(move, "Vector", 0.0f, 0.5f, 0.0f);
      tree.b.Link(*tree.flow, tree.flow_pin, move, "Flow");
      tree.flow = &move;
      tree.flow_pin = "Done";
    }
    GenNode &at = tree.Add("LogicNativeVelocityAtPoint", variant == 3 ? 1 : 0, 0);
    switch (variant) {
      case 0:
        tree.b.Vector(at, "Position", 3.0f, 1.0f, 2.0f);
        break;
      case 1: {
        GenNode &where = tree.Add("LogicNativeGetObjectAttribute", 0 /* World Position */);
        tree.b.Link(where, "Vector", at, "Position");
        break;
      }
      case 2:
        tree.b.ObjectNamed(at, "Object", &marker_blender.object);
        tree.b.Vector(at, "Position", 1.0f, 0.0f, 0.0f);
        break;
      default:
        tree.b.String(at, "Vertex Group", "Tip");
        break;
    }
    tree.StoreVector(at, "Velocity", "v");
    tree.StoreFloat(at, "Speed", "speed");
    tree.StoreVector(at, "Point", "p");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    {
      /* The kernel, not the bridge: one QPointVel node (Velocity, Speed and Point read it), the
       * Vertex Group mode measured when the run starts. */
      std::string refusal;
      const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
          *program, LN_Event::OnFixedUpdate, &refusal, false);
      ASSERT_NE(columns, nullptr) << refusal;
      const std::string text = LN_DescribeColumnProgram(*columns);
      EXPECT_NE(text.find(" bridged=0 "), std::string::npos) << text;
      EXPECT_NE(text.find(" point_velocity_nodes=1 "), std::string::npos) << text;
      EXPECT_NE(text.find(variant == 3 ? " point_velocity_reads=1 " : " point_velocity_reads=0 "),
                std::string::npos)
          << text;
    }
    for (const SemMode &mode : Modes()) {
      SCOPED_TRACE(mode.name);
      std::string reference;
      for (const int columns : {0, 1, 4}) {
        SCOPED_TRACE("columns " + std::to_string(columns));
        SemRig rig(program, mode, owners);
        /* 0: every tree its column pass; 1: wide runs on this thread; 4: on 3 helpers too. */
        if (columns != 0) {
          rig.SetColumns(true, uint32_t(columns - 1));
        }
        World world(owners + 8);
        for (uint32_t index = 0; index < owners; index++) {
          KX_GameObject &owner = rig.Object(index);
          owner.NodeSetWorldPosition(MT_Vector3(float(index) * 3.0f, 1.0f, 0.0f));
          owner.NodeUpdateGS(0.0);
          Body &body = world.Box(owner, MT_Vector3(0.5f, 0.5f, 0.5f), JPH::EMotionType::Dynamic);
          JPH::BodyInterface &bodies = world.environment.GetBodyInterface();
          bodies.SetLinearVelocity(
              body.body_id, JoltMath::ToJolt(MT_Vector3(1.0f + float(index % 7), -2.0f, 0.5f)));
          bodies.SetAngularVelocity(
              body.body_id,
              JoltMath::ToJolt(MT_Vector3(0.1f * float(index % 5), 0.0f, 1.0f + float(index % 3))));
        }
        KX_GameObject &marker = rig.AddObject("Marker", MT_Vector3(-5.0f, 2.0f, 1.0f));
        marker.SetBlenderObject(&marker_blender.object);
        Body &marker_body = world.Box(marker, MT_Vector3(0.5f, 0.5f, 0.5f),
                                      JPH::EMotionType::Dynamic);
        world.environment.GetBodyInterface().SetAngularVelocity(
            marker_body.body_id, JoltMath::ToJolt(MT_Vector3(0.0f, 0.0f, 3.0f)));
        /* (The first step runs On Init: no lanes then.) */
        std::string text;
        uint64_t lanes = 0;
        for (int step = 0; step < 4; step++) {
          rig.Step();
          lanes += rig.LastColumnLanes();
          for (uint32_t index = 0; index < owners; index++) {
            KX_GameObject &owner = rig.Object(index);
            for (const char *name : {"vX", "vY", "vZ", "speed", "pX", "pY", "pZ"}) {
              const float value = rig.Float(owner, name);
              uint32_t bits;
              std::memcpy(&bits, &value, sizeof(bits));
              text += std::to_string(bits) + " ";
            }
            text += "\n";
          }
        }
        if (columns == 0) {
          reference = text;
          /* Something was measured (not the rig's empty vertex groups). */
          if (variant != 3) {
            EXPECT_NE(rig.Float(rig.Object(1), "speed"), 0.0f);
          }
        }
        else {
          EXPECT_TRUE(text == reference) << "a wide run's outputs differ from the column pass's";
          if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
            /* The crowd ran wide, not tree by tree. */
            EXPECT_GE(lanes, uint64_t(owners)) << rig.LastColumnRefusal();
          }
        }
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Casts in crowds: wide runs against column passes (2026-11-14)
 *
 * Line of Sight's column kernel (QSight) and the bridge's Line of Sight, Projectile Ray and Sweep
 * Object: a crowd of NPCs in a Jolt world gives every output bit for bit as each tree's column
 * pass does, on one thread and with helpers.
 * \{ */

namespace {

/** A guard crowd: `owners` NPCs on a grid around a Player at the origin, each with a box body and
 * its own facing, two walls, the NPCs' game properties set by `properties` (owner index). */
void PlaceGuardCrowd(SemRig &rig,
                     World &world,
                     const uint32_t owners,
                     const std::function<void(KX_GameObject &, uint32_t)> &properties)
{
  for (uint32_t index = 0; index < owners; index++) {
    KX_GameObject &owner = rig.Object(index);
    owner.NodeSetWorldPosition(MT_Vector3(float(index % 20) * 3.0f - 28.5f,
                                          float(index / 20) * 3.0f - 21.5f,
                                          float(index % 3) * 0.5f));
    MT_Matrix3x3 facing;
    facing.setEuler(MT_Vector3(0.0f, 0.0f, float(index) * 0.645f));
    owner.NodeSetLocalOrientation(facing);
    owner.NodeUpdateGS(0.0);
    world.Box(owner, MT_Vector3(0.4f, 0.4f, 0.9f));
    properties(owner, index);
  }
  world.Box(rig.AddObject("Player", MT_Vector3(0.0f, 0.0f, 0.5f)), MT_Vector3(0.5f, 0.5f, 1.0f));
  world.Box(rig.AddObject("WallX", MT_Vector3(-8.0f, 0.0f, 0.0f)), MT_Vector3(0.2f, 9.0f, 3.0f));
  world.Box(rig.AddObject("WallY", MT_Vector3(4.0f, 9.0f, 0.0f)), MT_Vector3(7.0f, 0.2f, 3.0f));
}

/** Every float game property `names` of the first `owners` objects, as bits. */
std::string CrowdBits(SemRig &rig, const uint32_t owners, const std::vector<const char *> &names)
{
  std::string text;
  for (uint32_t index = 0; index < owners; index++) {
    KX_GameObject &owner = rig.Object(index);
    for (const char *name : names) {
      const float value = rig.Float(owner, name);
      uint32_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      text += std::to_string(bits) + " ";
    }
    text += "\n";
  }
  return text;
}

/** A crowd of `owners` trees of `program` (each NPC's second tree after one of `first_trees`
 * when set) on the guard crowd's grid, `extra` placing more, `properties` setting each NPC's game
 * properties: in every mode, every tree's column pass against wide runs on this thread and with 3
 * helpers, the float properties `names` bit for bit over 4 steps. `r_lanes`: the fewest lanes a
 * wide setting ran; `check`: a look at the column passes' result (something happened). */
void ExpectCrowdAlike(const std::shared_ptr<LN_Program> &program,
                      const uint32_t owners,
                      const std::vector<const char *> &names,
                      const std::function<void(SemRig &, World &)> &extra,
                      const std::function<void(SemRig &, KX_GameObject &, uint32_t)> &properties,
                      const std::function<void(SemRig &)> &check,
                      uint64_t &r_lanes,
                      const std::shared_ptr<LN_Program> &first_trees = nullptr,
                      const std::vector<const char *> &texts = {})
{
  r_lanes = UINT64_MAX;
  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    std::string reference;
    for (const int columns : {0, 1, 4}) {
      SCOPED_TRACE("columns " + std::to_string(columns));
      SemRig rig(first_trees != nullptr ? first_trees : program, mode, owners);
      if (first_trees != nullptr) {
        rig.AddSecondTrees(program, owners);
      }
      if (columns != 0) {
        rig.SetColumns(true, uint32_t(columns - 1));
      }
      World world(owners + 16);
      PlaceGuardCrowd(rig, world, owners, [&](KX_GameObject &owner, const uint32_t index) {
        properties(rig, owner, index);
      });
      extra(rig, world);
      /* The lanes that ran in wide runs (not the planned ones: a lane given back to its column
       * pass is not counted). */
      const uint64_t ran_before = LN_ColumnLaneCount();
      std::string text;
      for (int step = 0; step < 4; step++) {
        rig.Step();
        text += CrowdBits(rig, owners, names);
        for (const char *name : texts) {
          for (uint32_t index = 0; index < owners; index++) {
            text += rig.Text(rig.Object(index), name) + " ";
          }
        }
      }
      if (columns == 0) {
        reference = text;
        check(rig);
      }
      else {
        EXPECT_TRUE(text == reference) << "a wide run's outputs differ from the column pass's";
        r_lanes = std::min(r_lanes, LN_ColumnLaneCount() - ran_before);
      }
    }
  }
}

}  // namespace

TEST(JoltLogicNodes, LineOfSightRunsWideAsItsColumnPass)
{
  BlenderObject player_blender("Player");
  const uint32_t owners = 300; /* two chunks */
  const char *const labels[] = {"a named target, constant settings",
                                "a range and an eye height per NPC",
                                "a target each NPC finds by name",
                                "positions per NPC",
                                "behind a Branch",
                                "after a move of the owner",
                                "Blocker and Visualize",
                                "a Player another tree moved this step",
                                "NPCs their own first trees moved this step",
                                "targets that are other NPCs"};
  /* A tree moving its owner every step (each NPC's first tree). */
  std::shared_ptr<LN_Program> mover;
  {
    Tree tree;
    GenNode &move = tree.Add("LogicNativeApplyMovement");
    tree.b.Vector(move, "Vector", 0.05f, 0.02f, 0.0f);
    tree.b.Link(*tree.step, "Out", move, "Flow");
    mover = tree.Compile();
    ASSERT_NE(mover, nullptr);
  }
  /* A Pusher's tree moving the Player by name, tree by tree (a Set Random Seed, which has no
   * column kernel, keeps it there): its move is pending in the step's buffer as the VM records
   * it, for the NPCs' looks to apply. */
  std::shared_ptr<LN_Program> player_mover;
  {
    Tree tree;
    GenNode &seed = tree.Add("LogicNativeSetRandomSeed");
    tree.b.Link(*tree.step, "Out", seed, "Flow");
    GenNode &move = tree.Add("LogicNativeApplyMovement");
    tree.b.ObjectNamed(move, "Object", &player_blender.object);
    tree.b.Vector(move, "Vector", 0.05f, 0.02f, 0.0f);
    tree.b.Link(seed, "Done", move, "Flow");
    player_mover = tree.Compile();
    ASSERT_NE(player_mover, nullptr);
    std::string refusal;
    EXPECT_EQ(LN_CompileColumnProgram(*player_mover, LN_Event::OnFixedUpdate, &refusal, false),
              nullptr);
  }
  for (int variant = 0; variant < int(std::size(labels)); variant++) {
    SCOPED_TRACE(labels[variant]);
    Tree tree;
    GenNode *from = tree.step;
    const char *from_pin = "Out";
    if (variant == 4) {
      /* Its Flow from an op that runs for some NPCs only (the Branch's True route): the cast
       * runs only where that op ran (the others' outputs are the disabled query's). */
      GenNode &alert = tree.Add("LogicNativeGetGamePropertyBool");
      tree.b.String(alert, "Property", "alert");
      GenNode &branch = tree.Add("LogicNativeBranch");
      tree.b.Link(*tree.step, "Out", branch, "Flow");
      tree.b.Link(alert, "Value", branch, "Condition");
      GenNode &mark = tree.Add("LogicNativeSetGamePropertyBool");
      tree.b.String(mark, "Property", "looked");
      tree.b.Bool(mark, "Value", true);
      tree.b.Link(branch, "True", mark, "Flow");
      from = &mark;
      from_pin = "Done";
    }
    if (variant == 5) {
      GenNode &move = tree.Add("LogicNativeApplyMovement");
      tree.b.Vector(move, "Vector", 0.0f, 0.1f, 0.0f);
      tree.b.Link(*tree.step, "Out", move, "Flow");
      from = &move;
      from_pin = "Done";
    }
    GenNode &look = tree.Add("LogicNativeLineOfSight", variant == 3 ? 1 : 0, variant == 3 ? 1 : 0);
    tree.b.Link(*from, from_pin, look, "Flow");
    tree.b.Float(look, "Max Distance", 30.0f);
    /* A wide view: many NPCs see the Player, the walls hide it from others. */
    tree.b.Float(look, "View Angle", 4.0f);
    switch (variant) {
      case 1: {
        GenNode &range = tree.Add("LogicNativeGetGamePropertyFloat");
        tree.b.String(range, "Property", "range");
        tree.b.Link(range, "Value", look, "Max Distance");
        GenNode &height = tree.Add("LogicNativeGetGamePropertyFloat");
        tree.b.String(height, "Property", "eyez");
        GenNode &offset = tree.Add("LogicNativeCombineXYZ");
        tree.b.Link(height, "Value", offset, "Z");
        tree.b.Link(offset, "Vector", look, "Eye Offset");
        tree.b.ObjectNamed(look, "Target", &player_blender.object);
        break;
      }
      case 2:
      case 9: {
        GenNode &name = tree.Add("LogicNativeGetGamePropertyString");
        tree.b.String(name, "Property", "target_name");
        GenNode &find = tree.Add("LogicNativeObjectByName");
        tree.b.Link(name, "Value", find, "Name");
        tree.b.Link(find, "Object", look, "Target");
        tree.b.Vector(look, "Target Offset", 0.0f, 0.0f, 0.5f);
        break;
      }
      case 3: {
        GenNode &x = tree.Add("LogicNativeGetGamePropertyFloat");
        tree.b.String(x, "Property", "ex");
        GenNode &y = tree.Add("LogicNativeGetGamePropertyFloat");
        tree.b.String(y, "Property", "ey");
        GenNode &eye = tree.Add("LogicNativeCombineXYZ");
        tree.b.Link(x, "Value", eye, "X");
        tree.b.Link(y, "Value", eye, "Y");
        /* Above the NPCs' bodies, toward a point over the Player (positions pass no body). */
        tree.b.Float(eye, "Z", 2.5f);
        tree.b.Link(eye, "Vector", look, "Eye Position");
        tree.b.Vector(look, "Target Position", 0.0f, 0.0f, 3.0f);
        tree.b.Vector(look, "Forward", 1.0f, 0.0f, 0.0f);
        tree.b.Float(look, "View Angle", 2.5f);
        break;
      }
      case 6: {
        tree.b.ObjectNamed(look, "Target", &player_blender.object);
        GenNode &draw = tree.Add("LogicNativeGetGamePropertyBool");
        tree.b.String(draw, "Property", "draw");
        tree.b.Link(draw, "Value", look, "Visualize");
        break;
      }
      default:
        tree.b.ObjectNamed(look, "Target", &player_blender.object);
        break;
    }
    tree.StoreBool(look, "Visible", "visible");
    tree.StoreFloat(look, "Distance", "distance");
    tree.StoreFloat(look, "Angle", "angle");
    if (variant == 7) {
      /* A second look at a target found by name (never the Player: a found object with a pending
       * move holds the run itself): the bridge is off for this program (no named object list of
       * its own), so the kernel's own note of the Player must hold the run until the Player's
       * pending move is applied. */
      GenNode &name = tree.Add("LogicNativeGetGamePropertyString");
      tree.b.String(name, "Property", "other_name");
      GenNode &find = tree.Add("LogicNativeObjectByName");
      tree.b.Link(name, "Value", find, "Name");
      GenNode &second = tree.Add("LogicNativeLineOfSight");
      tree.b.Link(*tree.step, "Out", second, "Flow");
      tree.b.Link(find, "Object", second, "Target");
      tree.b.Float(second, "View Angle", 4.0f);
      tree.StoreBool(second, "Visible", "visible2");
    }
    if (variant == 6) {
      tree.StoreText(look, "Blocker", "blocker");
    }
    /* Done: a write behind it. */
    GenNode &done = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(done, "Property", "done");
    tree.b.Float(done, "Value", 1.0f);
    tree.b.Link(look, "Done", done, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    {
      std::string refusal;
      const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
          *program, LN_Event::OnFixedUpdate, &refusal, false);
      if (variant == 5) {
        /* A cast from the owner after its move applies the move first: tree by tree. */
        EXPECT_EQ(columns, nullptr);
        EXPECT_NE(refusal.find("after a move of the owner"), std::string::npos) << refusal;
      }
      else {
        ASSERT_NE(columns, nullptr) << refusal;
        const std::string text = LN_DescribeColumnProgram(*columns);
        if (variant == 6) {
          /* Visualize draws (the main thread) and Blocker is an object: the bridge. */
          EXPECT_NE(text.find(" bridged=1 "), std::string::npos) << text;
          EXPECT_NE(text.find(" sight_nodes=0 "), std::string::npos) << text;
        }
        else {
          /* The kernel, not the bridge: QSight nodes for Visible, Distance, Angle and Done (one
           * per read site with its own route: the VM casts again at every op that reads it). */
          EXPECT_NE(text.find(" bridged=0 "), std::string::npos) << text;
          EXPECT_EQ(text.find(" sight_nodes=0 "), std::string::npos) << text;
        }
      }
    }
    for (const SemMode &mode : Modes()) {
      SCOPED_TRACE(mode.name);
      std::string reference;
      for (const int columns : {0, 1, 4}) {
        SCOPED_TRACE("columns " + std::to_string(columns));
        /* (Variant 8: each NPC's Line of Sight tree is its second, after the mover.) */
        SemRig rig(variant == 8 ? mover : program, mode, owners);
        if (variant == 8) {
          rig.AddSecondTrees(program, owners);
        }
        if (columns != 0) {
          rig.SetColumns(true, uint32_t(columns - 1));
        }
        World world(owners + 16);
        PlaceGuardCrowd(rig, world, owners, [&](KX_GameObject &owner, const uint32_t index) {
          rig.SetFloat(owner, "range", 6.0f + float(index % 5) * 8.0f);
          rig.SetFloat(owner, "eyez", float(index % 4) * 0.7f);
          /* Found targets: none another NPC but in variant 9 (an NPC's target that is another
           * NPC gives that run's NPCs back, so runs there are short); not periodic in a chunk's
           * 256 lanes. */
          const uint32_t pick = (index * 7) % 11;
          rig.SetString(owner,
                        "target_name",
                        variant == 9 && index % 3 == 0 ?
                            ("Obj" + std::to_string((index * 7 + 3) % owners)).c_str() :
                        pick < 4 ? "Player" :
                        pick < 6 ? "Nobody" :
                        pick < 8 ? "WallY" :
                                   "WallX");
          const MT_Vector3 &at = owner.NodeGetWorldPosition();
          rig.SetFloat(owner, "ex", at.x() + 0.6f);
          rig.SetFloat(owner, "ey", at.y());
          rig.SetBool(owner, "alert", index % 3 != 0);
          rig.SetBool(owner, "draw", index % 2 == 0);
          rig.SetString(owner, "other_name", pick < 5 ? "WallX" : pick < 8 ? "WallY" : "Nobody");
        });
        if (variant == 7) {
          /* The Player moves (a Pusher's tree, recorded first) before every NPC looks. */
          rig.AddObject("Pusher", MT_Vector3(40.0f, 40.0f, 0.0f));
          rig.AddTree(player_mover, rig.ObjectCount() - 1, 0, true);
        }
        uint64_t lanes = 0;
        std::string text;
        for (int step = 0; step < 4; step++) {
          rig.Step();
          lanes += rig.LastColumnLanes();
          text += CrowdBits(rig, owners, {"visible", "distance", "angle", "done", "visible2"});
          if (variant == 6) {
            for (uint32_t index = 0; index < owners; index++) {
              text += rig.Text(rig.Object(index), "blocker") + " ";
            }
          }
        }
        if (columns == 0) {
          reference = text;
          /* Seen and hidden both happen (some NPCs face the Player, some walls are between). */
          int seen = 0;
          for (uint32_t index = 0; index < owners; index++) {
            seen += rig.Float(rig.Object(index), "visible") != 0.0f ? 1 : 0;
          }
          EXPECT_GT(seen, 5);
          EXPECT_LT(seen, int(owners) - 5);
        }
        else {
          EXPECT_TRUE(text == reference) << "a wide run's outputs differ from the column pass's";
          if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force && variant != 5 &&
              variant != 8)
          {
            /* The crowd ran wide, not tree by tree (a found target that is another NPC gives
             * that NPC's run back; with the Player's move pending, the first NPCs look on the
             * VM, applying it; an NPC its own first tree moved looks on the VM). */
            EXPECT_GE(lanes, uint64_t(variant == 9 ? 1 : variant == 7 ? owners / 2 : owners))
                << rig.LastColumnRefusal();
          }
        }
      }
    }
  }
}

namespace {

/** A tree's flow for a crowd variant: the event's, or a Set Game Property Bool's Done behind a
 * Branch on the NPC's "alert" (a Flow from an op that runs for some NPCs only). */
std::pair<GenNode *, const char *> CrowdFlow(Tree &tree, const bool behind_op)
{
  if (!behind_op) {
    return {tree.step, "Out"};
  }
  GenNode &alert = tree.Add("LogicNativeGetGamePropertyBool");
  tree.b.String(alert, "Property", "alert");
  GenNode &branch = tree.Add("LogicNativeBranch");
  tree.b.Link(*tree.step, "Out", branch, "Flow");
  tree.b.Link(alert, "Value", branch, "Condition");
  GenNode &mark = tree.Add("LogicNativeSetGamePropertyBool");
  tree.b.String(mark, "Property", "acted");
  tree.b.Bool(mark, "Value", true);
  tree.b.Link(branch, "True", mark, "Flow");
  return {&mark, "Done"};
}

/** The compiled column program's description, or the refusal. */
std::string DescribeColumns(const LN_Program &program, std::string &r_refusal)
{
  const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
      program, LN_Event::OnFixedUpdate, &r_refusal, false);
  return columns != nullptr ? LN_DescribeColumnProgram(*columns) : std::string();
}

}  // namespace

TEST(JoltLogicNodes, ProjectileRayRunsWideAsItsColumnPass)
{
  BlenderObject player_blender("Player");
  const uint32_t owners = 300;
  const char *const labels[] = {"its Flow the event's",
                                "behind an op that runs for some NPCs",
                                "a Power per NPC",
                                "after a move of the owner",
                                "thrown from a named object",
                                "Visualize",
                                "NPCs their own first trees moved this step",
                                "tree by tree (a node without a kernel next to it)"};
  std::shared_ptr<LN_Program> mover;
  {
    Tree tree;
    GenNode &move = tree.Add("LogicNativeApplyMovement");
    tree.b.Vector(move, "Vector", 0.05f, 0.02f, 0.0f);
    tree.b.Link(*tree.step, "Out", move, "Flow");
    mover = tree.Compile();
    ASSERT_NE(mover, nullptr);
  }
  for (int variant = 0; variant < int(std::size(labels)); variant++) {
    SCOPED_TRACE(labels[variant]);
    Tree tree;
    std::pair<GenNode *, const char *> from = CrowdFlow(tree, variant == 1);
    if (variant == 7) {
      /* (Set Random Seed has no column kernel: every tree its column pass, its warm-up on the
       * pool all the same.) */
      GenNode &seed = tree.Add("LogicNativeSetRandomSeed");
      tree.b.Link(*from.first, from.second, seed, "Flow");
      from = {&seed, "Done"};
    }
    if (variant == 3) {
      GenNode &move = tree.Add("LogicNativeApplyMovement");
      tree.b.Vector(move, "Vector", 0.0f, 0.1f, 0.0f);
      tree.b.Link(*from.first, from.second, move, "Flow");
      from = {&move, "Done"};
    }
    GenNode &arc = tree.Add("LogicNativeProjectileRay");
    tree.b.Link(*from.first, from.second, arc, "Flow");
    tree.b.Bool(arc, "Local", true);
    tree.b.Vector(arc, "Origin", 0.0f, 0.5f, 0.5f);
    tree.b.Vector(arc, "Aim", 0.0f, 1.0f, 0.6f);
    tree.b.Float(arc, "Power", 12.0f);
    tree.b.Float(arc, "Distance", 40.0f);
    if (variant == 2) {
      GenNode &power = tree.Add("LogicNativeGetGamePropertyFloat");
      tree.b.String(power, "Property", "power");
      tree.b.Link(power, "Value", arc, "Power");
    }
    if (variant == 4) {
      tree.b.ObjectNamed(arc, "Caster", &player_blender.object);
    }
    if (variant == 5) {
      GenNode &draw = tree.Add("LogicNativeGetGamePropertyBool");
      tree.b.String(draw, "Property", "draw");
      tree.b.Link(draw, "Value", arc, "Visualize");
    }
    tree.StoreBool(arc, "RESULT", "hit");
    tree.StoreFloat(arc, "Distance", "distance");
    tree.StoreFloat(arc, "Flight Time", "time");
    tree.StoreVector(arc, "Impact Velocity", "v");
    tree.StoreVector(arc, "POINT", "p");
    GenNode &done = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(done, "Property", "done");
    tree.b.Float(done, "Value", 1.0f);
    tree.b.Link(arc, "Done", done, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    {
      /* The bridge (the VM's own cast, NPC by NPC): no longer tree by tree. Its arc reads the
       * caster's pose without applying a pending move, so a move of the owner before it keeps
       * the run. */
      std::string refusal;
      const std::string text = DescribeColumns(*program, refusal);
      if (variant == 7) {
        EXPECT_TRUE(text.empty()) << text;
      }
      else {
        ASSERT_FALSE(text.empty()) << refusal;
        EXPECT_NE(text.find(" bridged=1 "), std::string::npos) << text;
        EXPECT_NE(text.find(variant == 5 ? " main_thread=1 " : " main_thread=0 "),
                  std::string::npos)
            << text;
      }
    }
    uint64_t lanes = 0;
    ExpectCrowdAlike(
        program,
        owners,
        {"hit", "distance", "time", "vX", "vY", "vZ", "pX", "pY", "pZ", "done"},
        [&](SemRig &rig, World &world) {
          world.Box(rig.AddObject("Ground", MT_Vector3(0.0f, 0.0f, -1.5f)),
                    MT_Vector3(60.0f, 60.0f, 0.5f));
        },
        [&](SemRig &rig, KX_GameObject &owner, const uint32_t index) {
          rig.SetFloat(owner, "power", 4.0f + float((index * 7) % 11));
          rig.SetBool(owner, "alert", index % 3 != 0);
          rig.SetBool(owner, "draw", index % 2 == 0);
        },
        [&](SemRig &rig) {
          /* Arcs land on the ground, hit walls and NPCs, or fly out of reach (the Player's
           * throw is every NPC's). */
          int hits = 0;
          for (uint32_t index = 0; index < owners; index++) {
            hits += rig.Float(rig.Object(index), "hit") != 0.0f ? 1 : 0;
          }
          EXPECT_TRUE(variant == 4 || hits > 5) << hits;
        },
        lanes,
        variant == 6 ? mover : nullptr);
    if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force && variant != 7) {
      EXPECT_GE(lanes, uint64_t(owners)) << "the crowd ran tree by tree";
    }
  }
}

TEST(JoltLogicNodes, SweepObjectRunsWideAsItsColumnPass)
{
  BlenderObject player_blender("Player");
  const uint32_t owners = 300;
  const char *const labels[] = {"its Flow the event's",
                                "behind an op that runs for some NPCs",
                                "a destination per NPC",
                                "after a move of the owner",
                                "the named Player swept",
                                "Visualize",
                                "NPCs their own first trees moved this step"};
  std::shared_ptr<LN_Program> mover;
  {
    Tree tree;
    GenNode &move = tree.Add("LogicNativeApplyMovement");
    tree.b.Vector(move, "Vector", 0.05f, 0.02f, 0.0f);
    tree.b.Link(*tree.step, "Out", move, "Flow");
    mover = tree.Compile();
    ASSERT_NE(mover, nullptr);
  }
  for (int variant = 0; variant < int(std::size(labels)); variant++) {
    SCOPED_TRACE(labels[variant]);
    Tree tree;
    std::pair<GenNode *, const char *> from = CrowdFlow(tree, variant == 1);
    if (variant == 3) {
      GenNode &move = tree.Add("LogicNativeApplyMovement");
      tree.b.Vector(move, "Vector", 0.0f, 0.1f, 0.0f);
      tree.b.Link(*from.first, from.second, move, "Flow");
      from = {&move, "Done"};
    }
    GenNode &sweep = tree.Add("LogicNativeSweepObject");
    tree.b.Link(*from.first, from.second, sweep, "Flow");
    tree.b.Bool(sweep, "Local", true);
    tree.b.Vector(sweep, "Destination", 0.0f, 4.0f, 0.0f);
    if (variant == 2) {
      GenNode &reach = tree.Add("LogicNativeGetGamePropertyFloat");
      tree.b.String(reach, "Property", "reach");
      GenNode &destination = tree.Add("LogicNativeCombineXYZ");
      tree.b.Link(reach, "Value", destination, "Y");
      tree.b.Link(destination, "Vector", sweep, "Destination");
    }
    if (variant == 4) {
      tree.b.ObjectNamed(sweep, "Object", &player_blender.object);
    }
    if (variant == 5) {
      GenNode &draw = tree.Add("LogicNativeGetGamePropertyBool");
      tree.b.String(draw, "Property", "draw");
      tree.b.Link(draw, "Value", sweep, "Visualize");
    }
    tree.StoreBool(sweep, "Has Result", "hit");
    tree.StoreVector(sweep, "Safe Position", "s");
    tree.StoreFloat(sweep, "Safe Fraction", "fraction");
    tree.StoreFloat(sweep, "Distance", "distance");
    tree.StoreVector(sweep, "Hit Point", "p");
    GenNode &done = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(done, "Property", "done");
    tree.b.Float(done, "Value", 1.0f);
    tree.b.Link(sweep, "Done", done, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    {
      std::string refusal;
      const std::string text = DescribeColumns(*program, refusal);
      if (variant == 3) {
        /* The sweep from the owner applies the owner's move first: tree by tree. */
        EXPECT_TRUE(text.empty());
        EXPECT_NE(refusal.find("after a move of the owner"), std::string::npos) << refusal;
      }
      else {
        ASSERT_FALSE(text.empty()) << refusal;
        EXPECT_NE(text.find(" bridged=1 "), std::string::npos) << text;
        EXPECT_NE(text.find(variant == 5 ? " main_thread=1 " : " main_thread=0 "),
                  std::string::npos)
            << text;
      }
    }
    uint64_t lanes = 0;
    ExpectCrowdAlike(
        program,
        owners,
        {"hit", "sX", "sY", "sZ", "fraction", "distance", "pX", "pY", "pZ", "done"},
        [](SemRig &, World &) {},
        [&](SemRig &rig, KX_GameObject &owner, const uint32_t index) {
          rig.SetFloat(owner, "reach", 1.0f + float((index * 7) % 11) * 0.7f);
          rig.SetBool(owner, "alert", index % 3 != 0);
          rig.SetBool(owner, "draw", index % 2 == 0);
        },
        [&](SemRig &rig) {
          /* Some NPCs' boxes would hit another NPC or a wall, others move freely (the Player's
           * sweep is every NPC's). */
          int hits = 0;
          for (uint32_t index = 0; index < owners; index++) {
            hits += rig.Float(rig.Object(index), "hit") != 0.0f ? 1 : 0;
          }
          EXPECT_TRUE(variant == 4 || (hits > 5 && hits < int(owners) - 5)) << hits;
        },
        lanes,
        variant == 6 ? mover : nullptr);
    /* (An NPC its own first tree moved sweeps on the VM, its move applied first.) */
    if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force && variant != 3 && variant != 6) {
      EXPECT_GE(lanes, uint64_t(owners)) << "the crowd ran tree by tree";
    }
  }
}

/** Raycast's column kernel (QRay, 2026-11-14: its inputs column slots, its caster the owner, a
 * named object or one found while playing; before, constants from the owner only). */
TEST(JoltLogicNodes, RaycastRunsWideAsItsColumnPass)
{
  BlenderObject player_blender("Player");
  const uint32_t owners = 300;
  const char *const labels[] = {"Local, a constant Destination",
                                "a Destination per NPC",
                                "Direction mode, a Max Distance per NPC",
                                "cast from the named Player",
                                "cast from an object each NPC finds by name",
                                "behind an op that runs for some NPCs",
                                "after a move of the owner",
                                "between world points",
                                "a Property and an X-Ray per NPC",
                                "Hit Object (the bridge) and Blocked",
                                "NPCs their own first trees moved this step",
                                "cast from Get Owner"};
  std::shared_ptr<LN_Program> mover;
  {
    Tree tree;
    GenNode &move = tree.Add("LogicNativeApplyMovement");
    tree.b.Vector(move, "Vector", 0.05f, 0.02f, 0.0f);
    tree.b.Link(*tree.step, "Out", move, "Flow");
    mover = tree.Compile();
    ASSERT_NE(mover, nullptr);
  }
  for (int variant = 0; variant < int(std::size(labels)); variant++) {
    SCOPED_TRACE(labels[variant]);
    Tree tree;
    std::pair<GenNode *, const char *> from = CrowdFlow(tree, variant == 5);
    if (variant == 6) {
      GenNode &move = tree.Add("LogicNativeApplyMovement");
      tree.b.Vector(move, "Vector", 0.0f, 0.1f, 0.0f);
      tree.b.Link(*from.first, from.second, move, "Flow");
      from = {&move, "Done"};
    }
    GenNode &ray = tree.Add("LogicNativeRaycast", variant == 2 ? 1 : 0);
    tree.b.Link(*from.first, from.second, ray, "Flow");
    tree.b.Bool(ray, "Local", variant != 7);
    tree.b.Vector(ray, "Destination", 0.0f, 15.0f, 0.0f);
    switch (variant) {
      case 1: {
        GenNode &reach = tree.Add("LogicNativeGetGamePropertyFloat");
        tree.b.String(reach, "Property", "reach");
        GenNode &destination = tree.Add("LogicNativeCombineXYZ");
        tree.b.Link(reach, "Value", destination, "Y");
        tree.b.Float(destination, "Z", 0.3f);
        tree.b.Link(destination, "Vector", ray, "Destination");
        break;
      }
      case 2: {
        tree.b.Vector(ray, "Direction", 0.3f, 1.0f, 0.0f);
        GenNode &reach = tree.Add("LogicNativeGetGamePropertyFloat");
        tree.b.String(reach, "Property", "reach");
        tree.b.Link(reach, "Value", ray, "Max Distance");
        break;
      }
      case 3:
        tree.b.ObjectNamed(ray, "Caster", &player_blender.object);
        break;
      case 4: {
        GenNode &name = tree.Add("LogicNativeGetGamePropertyString");
        tree.b.String(name, "Property", "caster_name");
        GenNode &find = tree.Add("LogicNativeObjectByName");
        tree.b.Link(name, "Value", find, "Name");
        tree.b.Link(find, "Object", ray, "Caster");
        break;
      }
      case 7: {
        GenNode &x = tree.Add("LogicNativeGetGamePropertyFloat");
        tree.b.String(x, "Property", "ex");
        GenNode &y = tree.Add("LogicNativeGetGamePropertyFloat");
        tree.b.String(y, "Property", "ey");
        GenNode &origin = tree.Add("LogicNativeCombineXYZ");
        tree.b.Link(x, "Value", origin, "X");
        tree.b.Link(y, "Value", origin, "Y");
        tree.b.Float(origin, "Z", 0.2f);
        tree.b.Link(origin, "Vector", ray, "Origin");
        tree.b.Vector(ray, "Destination", 0.2f, 0.3f, 0.4f);
        break;
      }
      case 11: {
        GenNode &owner = tree.Add("LogicNativeGetOwner");
        tree.b.Link(owner, "Owner Object", ray, "Caster");
        break;
      }
      case 8: {
        tree.b.String(ray, "Property", "solid");
        GenNode &xray = tree.Add("LogicNativeGetGamePropertyBool");
        tree.b.String(xray, "Property", "xray");
        tree.b.Link(xray, "Value", ray, "X-Ray");
        break;
      }
      default:
        break;
    }
    tree.StoreBool(ray, "Has Result", "hit");
    tree.StoreBool(ray, "Blocked", "blocked");
    tree.StoreFloat(ray, "Distance", "distance");
    tree.StoreVector(ray, "Point", "p");
    tree.StoreVector(ray, "Normal", "n");
    if (variant == 9) {
      tree.StoreText(ray, "Picked Object", "what");
    }
    GenNode &done = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(done, "Property", "done");
    tree.b.Float(done, "Value", 1.0f);
    tree.b.Link(ray, "Done", done, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    {
      std::string refusal;
      const std::string text = DescribeColumns(*program, refusal);
      if (variant == 6) {
        /* A cast from the owner after its move applies the move first: tree by tree. */
        EXPECT_TRUE(text.empty()) << text;
        EXPECT_NE(refusal.find("after a move of the owner"), std::string::npos) << refusal;
      }
      else {
        ASSERT_FALSE(text.empty()) << refusal;
        /* The kernel (Has Result, Blocked, Distance, Point, Normal, Done); Hit Object, an
         * object, through the bridge. */
        EXPECT_EQ(text.find(" ray_nodes=0 "), std::string::npos) << text;
        EXPECT_NE(text.find(variant == 9 ? " bridged=1 " : " bridged=0 "), std::string::npos)
            << text;
      }
    }
    uint64_t lanes = 0;
    ExpectCrowdAlike(
        program,
        owners,
        {"hit", "blocked", "distance", "pX", "pY", "pZ", "nX", "nY", "nZ", "done"},
        [&](SemRig &rig, World &world) {
          /* The walls hold the Property "solid"; the Player does not (X-Ray passes it). */
          for (size_t index = 0; index < rig.ObjectCount(); index++) {
            const std::string name = rig.Object(index).GetName();
            if (name == "WallX" || name == "WallY") {
              rig.SetBool(rig.Object(index), "solid", true);
            }
          }
          world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
        },
        [&](SemRig &rig, KX_GameObject &owner, const uint32_t index) {
          const uint32_t pick = (index * 7) % 11;
          rig.SetFloat(owner, "reach", 2.0f + float(pick) * 1.7f);
          rig.SetString(owner,
                        "caster_name",
                        pick < 4 ? "Player" : pick < 7 ? "WallX" : pick < 9 ? "WallY" : "Nobody");
          const MT_Vector3 &at = owner.NodeGetWorldPosition();
          rig.SetFloat(owner, "ex", at.x() + 0.6f);
          rig.SetFloat(owner, "ey", at.y());
          rig.SetBool(owner, "alert", index % 3 != 0);
          rig.SetBool(owner, "xray", index % 2 == 0);
        },
        [&](SemRig &rig) {
          /* Hits and misses both (the named Player's cast is every NPC's; the world rays all end
           * in the Player, some stopped earlier by an NPC). */
          int hits = 0;
          for (uint32_t index = 0; index < owners; index++) {
            hits += rig.Float(rig.Object(index), "hit") != 0.0f ? 1 : 0;
          }
          EXPECT_TRUE(variant == 3 || (hits > 5 && (variant == 7 || hits < int(owners) - 5)))
              << hits;
        },
        lanes,
        variant == 10 ? mover : nullptr,
        variant == 9 ? std::vector<const char *>{"what"} : std::vector<const char *>{});
    if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force && variant != 6 && variant != 10) {
      EXPECT_GE(lanes, uint64_t(owners)) << "the crowd ran tree by tree";
    }
  }
}

/** The casts' family in one column kernel (QRay, 2026-11-14): Shape Cast (sphere, box, capsule,
 * overlap), the All kinds (their Hit Count and first hit; their lists through the bridge) and the
 * objects a cast passes (the owner and named objects). */
TEST(JoltLogicNodes, CastFamilyRunsWideAsItsColumnPass)
{
  BlenderObject player_blender("Player");
  BlenderObject wall_blender("WallX");
  const uint32_t owners = 300;
  const char *const labels[] = {"a sphere cast ahead",
                                "a box per NPC, turned",
                                "a capsule per NPC",
                                "an overlap per NPC",
                                "Shape Cast All: Hit Count and the first hit",
                                "Raycast All: Hit Count, Direction, End Point",
                                "a Raycast passing the Player, the wall and the owner",
                                "a sphere cast from the named Player",
                                "behind an op that runs for some NPCs",
                                "after a move of the owner",
                                "Shape Cast All's Hit Objects (the bridge)"};
  for (int variant = 0; variant < int(std::size(labels)); variant++) {
    SCOPED_TRACE(labels[variant]);
    Tree tree;
    std::pair<GenNode *, const char *> from = CrowdFlow(tree, variant == 8);
    if (variant == 9) {
      GenNode &move = tree.Add("LogicNativeApplyMovement");
      tree.b.Vector(move, "Vector", 0.0f, 0.1f, 0.0f);
      tree.b.Link(*from.first, from.second, move, "Flow");
      from = {&move, "Done"};
    }
    const bool raycast = variant == 5 || variant == 6;
    const bool all = variant == 4 || variant == 5 || variant == 10;
    const char *idname = raycast ? (all ? "LogicNativeRaycastAll" : "LogicNativeRaycast") :
                                   (all ? "LogicNativeShapeCastAll" : "LogicNativeShapeCast");
    const int16_t shape = variant == 1 ? 1 : variant == 2 ? 2 : 0;
    GenNode &cast = tree.Add(idname, shape, variant == 3 ? 1 : 0);
    tree.b.Link(*from.first, from.second, cast, "Flow");
    tree.b.Bool(cast, "Local", true);
    tree.b.Vector(cast, "Destination", 0.0f, 6.0f, 0.0f);
    if (!raycast) {
      tree.b.Float(cast, "Radius", 0.4f);
    }
    GenNode &size = tree.Add("LogicNativeGetGamePropertyFloat");
    tree.b.String(size, "Property", "size");
    switch (variant) {
      case 1: {
        GenNode &extents = tree.Add("LogicNativeCombineXYZ");
        tree.b.Link(size, "Value", extents, "X");
        tree.b.Float(extents, "Y", 0.3f);
        tree.b.Float(extents, "Z", 0.5f);
        tree.b.Link(extents, "Vector", cast, "Half Extents");
        /* Turned by the NPC's own amount (its "size" again). */
        GenNode &turn = tree.Add("LogicNativeEuler");
        tree.b.Link(size, "Value", turn, "Z");
        tree.b.Link(turn, "Rotation", cast, "Rotation");
        break;
      }
      case 2:
        tree.b.Link(size, "Value", cast, "Radius");
        tree.b.Float(cast, "Height", 1.5f);
        break;
      case 3:
        tree.b.Link(size, "Value", cast, "Radius");
        break;
      case 4:
      case 10:
        tree.b.Int(cast, "Max Results", 4);
        tree.b.Vector(cast, "Destination", 0.0f, 12.0f, 0.0f);
        break;
      case 5:
        tree.b.Int(cast, "Max Results", 3);
        tree.b.Vector(cast, "Destination", 0.0f, 25.0f, 0.0f);
        break;
      case 6: {
        tree.b.ObjectNamed(cast, "Ignore Object", &player_blender.object);
        GenNode &owner = tree.Add("LogicNativeGetOwner");
        GenNode &list = tree.Add("LogicNativeListFromItems", 10);
        GenNode &wall = tree.Add("LogicNativeFindObject");
        tree.b.ObjectNamed(wall, "Object", &wall_blender.object);
        tree.b.Link(wall, "Object", list, "Objects");
        tree.b.Link(owner, "Owner Object", list, "Objects");
        tree.b.Link(list, "Objects", cast, "Ignore Objects");
        tree.b.Vector(cast, "Destination", 0.0f, 25.0f, 0.0f);
        break;
      }
      case 7:
        tree.b.ObjectNamed(cast, "Caster", &player_blender.object);
        break;
      default:
        break;
    }
    tree.StoreBool(cast, "Has Result", "hit");
    tree.StoreBool(cast, "Blocked", "blocked");
    if (all) {
      tree.Store(cast, "Hit Count", "count", "LogicNativeSetGamePropertyInt");
    }
    if (!all) {
      tree.StoreFloat(cast, "Distance", "distance");
      tree.StoreVector(cast, "Point", "p");
      tree.StoreVector(cast, "Normal", "n");
    }
    if (!raycast && !all) {
      tree.StoreFloat(cast, "Fraction", "fraction");
      tree.StoreVector(cast, "Cast Position", "c");
      tree.StoreBool(cast, "Started Overlapping", "started");
    }
    if (raycast && all) {
      tree.StoreVector(cast, "Direction", "d");
      tree.StoreVector(cast, "End Point", "e");
    }
    if (variant == 10) {
      tree.StoreItem(cast, "Hit Objects", 0, "first");
    }
    GenNode &done = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(done, "Property", "done");
    tree.b.Float(done, "Value", 1.0f);
    tree.b.Link(cast, "Done", done, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    {
      std::string refusal;
      const std::string text = DescribeColumns(*program, refusal);
      if (variant == 9) {
        EXPECT_TRUE(text.empty()) << text;
        EXPECT_NE(refusal.find("after a move of the owner"), std::string::npos) << refusal;
      }
      else {
        ASSERT_FALSE(text.empty()) << refusal;
        EXPECT_EQ(text.find(" ray_nodes=0 "), std::string::npos) << text;
        EXPECT_NE(text.find(variant == 10 ? " bridged=1 " : " bridged=0 "), std::string::npos)
            << text;
      }
    }
    uint64_t lanes = 0;
    std::vector<const char *> names = {"hit", "blocked", "count", "distance", "pX", "pY", "pZ",
                                       "nX", "nY", "nZ", "fraction", "cX", "cY", "cZ", "started",
                                       "dX", "dY", "dZ", "eX", "eY", "eZ", "done"};
    ExpectCrowdAlike(
        program,
        owners,
        names,
        [&](SemRig &, World &world) { world.environment.GetPhysicsSystem()->OptimizeBroadPhase(); },
        [&](SemRig &rig, KX_GameObject &owner, const uint32_t index) {
          rig.SetFloat(owner, "size", 0.2f + float((index * 7) % 11) * 0.15f);
          rig.SetBool(owner, "alert", index % 3 != 0);
        },
        [&](SemRig &rig) {
          int hits = 0;
          for (uint32_t index = 0; index < owners; index++) {
            hits += rig.Float(rig.Object(index), "hit") != 0.0f ? 1 : 0;
          }
          /* (The Player's cast is every NPC's.) */
          EXPECT_TRUE(variant == 7 || (hits > 5 && hits < int(owners) - 5)) << hits;
        },
        lanes,
        nullptr,
        variant == 10 ? std::vector<const char *>{"first"} : std::vector<const char *>{});
    if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force && variant != 9) {
      EXPECT_GE(lanes, uint64_t(owners)) << "the crowd ran tree by tree";
    }
  }
}

/** Keep Distance in a crowd (2026-11-14): its op runs in the wide run (the bridge: the VM's
 * handler records each NPC's request into the chunk's buffer, the run's end moves them to the
 * step's), its outputs as a column node (QKeep) reading what the flush's end left in each tree. */
TEST(JoltLogicNodes, KeepDistanceRunsWideAsItsColumnPass)
{
  const uint32_t owners = 300;
  const char *const labels[] = {"Push",
                                "Hold, with Reach",
                                "Check Every 3",
                                "behind an op that runs for some NPCs",
                                "a Distance per NPC",
                                "Pull",
                                "the object's own shape",
                                "Nearest Object",
                                "a Mode per NPC (-1 to 3: Push, Pull, Hold and clamped)"};
  /* Each NPC's first tree moves it (each step's answers differ: a lost request would show), each
   * a little differently (its own speed from its "drift" property). */
  std::shared_ptr<LN_Program> mover;
  {
    Tree tree;
    GenNode &drift = tree.Add("LogicNativeGetGamePropertyFloat");
    tree.b.String(drift, "Property", "drift");
    GenNode &step = tree.Add("LogicNativeCombineXYZ");
    tree.b.Link(drift, "Value", step, "X");
    tree.b.Float(step, "Y", 0.07f);
    GenNode &move = tree.Add("LogicNativeApplyMovement");
    tree.b.Link(step, "Vector", move, "Vector");
    tree.b.Link(*tree.step, "Out", move, "Flow");
    mover = tree.Compile();
    ASSERT_NE(mover, nullptr);
  }
  for (int variant = 0; variant < int(std::size(labels)); variant++) {
    SCOPED_TRACE(labels[variant]);
    Tree tree;
    const std::pair<GenNode *, const char *> from = CrowdFlow(tree, variant == 3);
    GenNode &keep = tree.Add(
        "LogicNativeKeepDistance", LN_KEEP_DISTANCE_FALLOFF_LINEAR, variant == 6 ? 0 : 1);
    tree.b.Int(keep,
               "Mode",
               variant == 1 ? LN_KEEP_DISTANCE_MODE_HOLD :
               variant == 5 ? LN_KEEP_DISTANCE_MODE_PULL :
                              LN_KEEP_DISTANCE_MODE_PUSH);
    tree.b.Link(*from.first, from.second, keep, "Flow");
    tree.b.Float(keep, "Distance", 3.0f);
    tree.b.Float(keep, "Radius", 0.5f);
    if (variant == 1) {
      tree.b.Float(keep, "Reach", 5.0f);
    }
    if (variant == 2) {
      tree.b.Int(keep, "Check Every", 3);
    }
    if (variant == 4) {
      GenNode &distance = tree.Add("LogicNativeGetGamePropertyFloat");
      tree.b.String(distance, "Property", "keep");
      tree.b.Link(distance, "Value", keep, "Distance");
    }
    if (variant == 8) {
      /* (2026-11-15) The Mode from each NPC's "mode" property. */
      GenNode &mode = tree.Add("LogicNativeGetGamePropertyInt");
      tree.b.String(mode, "Property", "mode");
      tree.b.Link(mode, "Value", keep, "Mode");
      tree.b.Float(keep, "Reach", 5.0f);
    }
    /* (No Visualize here: its lines are drawn by the flush's end into the rasterizer, which the
     * rig has none of; the request carries the setting either way.) */
    tree.StoreBool(keep, "Near", "near");
    tree.StoreFloat(keep, "Nearest Distance", "gap");
    tree.StoreVector(keep, "Force", "force");
    tree.StoreVector(keep, "Nearest Normal", "normal");
    tree.StoreVector(keep, "Nearest Point", "point");
    if (variant == 7) {
      tree.StoreText(keep, "Nearest Object", "what");
    }
    GenNode &done = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(done, "Property", "done");
    tree.b.Float(done, "Value", 1.0f);
    tree.b.Link(keep, "Done", done, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    {
      /* The op bridged (on the main thread: its handler is no tree batch's), the outputs a
       * column node (Nearest Object, an object, through the bridge too). */
      std::string refusal;
      const std::string text = DescribeColumns(*program, refusal);
      ASSERT_FALSE(text.empty()) << refusal;
      EXPECT_NE(text.find(" bridged=1 "), std::string::npos) << text;
      EXPECT_NE(text.find(" keep_nodes=1 "), std::string::npos) << text;
    }
    uint64_t lanes = 0;
    ExpectCrowdAlike(
        program,
        owners,
        {"near",
         "gap",
         "forceX",
         "forceY",
         "forceZ",
         "normalX",
         "normalY",
         "normalZ",
         "pointX",
         "pointY",
         "pointZ",
         "done"},
        [](SemRig &, World &world) { world.environment.GetPhysicsSystem()->OptimizeBroadPhase(); },
        [&](SemRig &rig, KX_GameObject &owner, const uint32_t index) {
          rig.SetFloat(owner, "keep", 1.0f + float((index * 7) % 11) * 0.4f);
          rig.SetFloat(owner, "drift", 0.03f * float(index % 7) - 0.09f);
          rig.SetBool(owner, "alert", index % 3 != 0);
          rig.SetBool(owner, "draw", index % 2 == 0);
          rig.SetInt(owner, "mode", int((index * 3) % 5) - 1);
        },
        [&](SemRig &rig) {
          /* Neighbours and walls within reach of some, nothing near others. */
          int near = 0;
          for (uint32_t index = 0; index < owners; index++) {
            near += rig.Float(rig.Object(index), "near") != 0.0f ? 1 : 0;
          }
          EXPECT_GT(near, 5);
        },
        lanes,
        mover,
        variant == 7 ? std::vector<const char *>{"what"} : std::vector<const char *>{});
    if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
      EXPECT_GE(lanes, uint64_t(owners)) << "the crowd ran tree by tree";
    }
  }
}

/** What a crowd of NPC brains costs with each cast (2026-11-14): ms a step, every tree its column
 * pass (as before these casts ran wide: one such node kept its whole tree there) against wide runs
 * on this thread and with 7 helpers (Jolt's job system has 7 helpers in all three). Each brain:
 * the node, a Branch on its result, a property write on each route and a small move.
 * UPBGE_LN_BENCH_OWNERS (1000), UPBGE_LN_BENCH_STEPS (50). The rig's threaded times swing: judge
 * threads in the game. */
TEST(JoltLogicNodes, DISABLED_CastsInCrowdsCost)
{
  BlenderObject player_blender("Player");
  const char *owners_text = std::getenv("UPBGE_LN_BENCH_OWNERS");
  const uint32_t owners = owners_text ? uint32_t(std::max(1, std::atoi(owners_text))) : 1000;
  const char *steps_text = std::getenv("UPBGE_LN_BENCH_STEPS");
  const int steps = steps_text ? std::max(1, std::atoi(steps_text)) : 50;
  const char *const names[] = {
      "no cast", "Line of Sight", "Projectile Ray", "Sweep Object", "Keep Distance",
      "Raycast (QRay)", "Raycast (bridge)"};
  for (int which = 0; which < int(std::size(names)); which++) {
    Tree tree;
    GenNode *cast = nullptr;
    const char *condition = nullptr;
    const char *value = nullptr;
    switch (which) {
      case 0:
        cast = &tree.Add("LogicNativeGetGamePropertyBool");
        tree.b.String(*cast, "Property", "alert");
        condition = "Value";
        break;
      case 1:
        cast = &tree.Add("LogicNativeLineOfSight");
        tree.b.Link(*tree.step, "Out", *cast, "Flow");
        tree.b.ObjectNamed(*cast, "Target", &player_blender.object);
        tree.b.Float(*cast, "Max Distance", 30.0f);
        tree.b.Float(*cast, "View Angle", 4.0f);
        condition = "Visible";
        value = "Distance";
        break;
      case 2:
        cast = &tree.Add("LogicNativeProjectileRay");
        tree.b.Link(*tree.step, "Out", *cast, "Flow");
        tree.b.Bool(*cast, "Local", true);
        tree.b.Vector(*cast, "Aim", 0.0f, 1.0f, 0.6f);
        tree.b.Float(*cast, "Power", 12.0f);
        tree.b.Float(*cast, "Distance", 40.0f);
        condition = "RESULT";
        value = "Flight Time";
        break;
      case 3:
        cast = &tree.Add("LogicNativeSweepObject");
        tree.b.Link(*tree.step, "Out", *cast, "Flow");
        tree.b.Bool(*cast, "Local", true);
        tree.b.Vector(*cast, "Destination", 0.0f, 4.0f, 0.0f);
        condition = "Has Result";
        value = "Safe Fraction";
        break;
      case 4:
        cast = &tree.Add("LogicNativeKeepDistance", LN_KEEP_DISTANCE_FALLOFF_LINEAR, 1);
        tree.b.Link(*tree.step, "Out", *cast, "Flow");
        tree.b.Float(*cast, "Distance", 3.0f);
        condition = "Near";
        value = "Nearest Distance";
        break;
      default: {
        /* A Raycast ahead of the NPC: constant (the QRay kernel) or as far as its "reach"
         * property (computed: the bridge, on helpers). */
        cast = &tree.Add("LogicNativeRaycast");
        tree.b.Link(*tree.step, "Out", *cast, "Flow");
        tree.b.Bool(*cast, "Local", true);
        tree.b.Vector(*cast, "Destination", 0.0f, 15.0f, 0.0f);
        if (which == 6) {
          GenNode &reach = tree.Add("LogicNativeGetGamePropertyFloat");
          tree.b.String(reach, "Property", "reach");
          GenNode &destination = tree.Add("LogicNativeCombineXYZ");
          tree.b.Link(reach, "Value", destination, "Y");
          tree.b.Link(destination, "Vector", *cast, "Destination");
        }
        condition = "Has Result";
        value = "Distance";
        break;
      }
    }
    GenNode &branch = tree.Add("LogicNativeBranch");
    tree.b.Link(*tree.step, "Out", branch, "Flow");
    tree.b.Link(*cast, condition, branch, "Condition");
    GenNode &seen = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(seen, "Property", "seen");
    if (value != nullptr) {
      tree.b.Link(*cast, value, seen, "Value");
    }
    tree.b.Link(branch, "True", seen, "Flow");
    GenNode &move = tree.Add("LogicNativeApplyMovement");
    tree.b.Vector(move, "Vector", 0.0f, 0.001f, 0.0f);
    tree.b.Bool(move, "Local", true);
    tree.b.Link(seen, "Done", move, "Flow");
    GenNode &lost = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(lost, "Property", "seen");
    tree.b.Float(lost, "Value", -1.0f);
    tree.b.Link(branch, "False", lost, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    double column_passes = 0.0;
    for (const int columns : {0, 1, 8}) {
      SemRig rig(program, kProduction, owners);
      rig.SetDump(false);
      rig.SetObserveForces(false);
      if (columns != 0) {
        rig.SetColumns(true, uint32_t(columns - 1));
      }
      World world(int(owners) + 64, 7);
      for (uint32_t index = 0; index < owners; index++) {
        KX_GameObject &owner = rig.Object(index);
        owner.NodeSetWorldPosition(MT_Vector3(float(index % 40) * 3.0f - 58.5f,
                                              float(index / 40) * 3.0f - 36.0f,
                                              float(index % 3) * 0.5f));
        MT_Matrix3x3 facing;
        facing.setEuler(MT_Vector3(0.0f, 0.0f, float(index) * 0.645f));
        owner.NodeSetLocalOrientation(facing);
        owner.NodeUpdateGS(0.0);
        world.Box(owner, MT_Vector3(0.4f, 0.4f, 0.9f));
        rig.SetBool(owner, "alert", index % 3 != 0);
        rig.SetFloat(owner, "reach", 15.0f);
      }
      world.Box(rig.AddObject("Player", MT_Vector3(0.0f, 0.0f, 0.5f)), MT_Vector3(0.5f, 0.5f, 1.0f));
      world.Box(rig.AddObject("WallX", MT_Vector3(-8.0f, 0.0f, 0.0f)), MT_Vector3(0.2f, 9.0f, 3.0f));
      world.Box(rig.AddObject("Ground", MT_Vector3(0.0f, 0.0f, -1.5f)),
                MT_Vector3(80.0f, 80.0f, 0.5f));
      world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
      const double ms = MillisecondsPerStep(rig, steps);
      if (columns == 0) {
        column_passes = ms;
      }
      std::printf("%-15s %u NPCs, %-26s %8.3f ms a step%s\n",
                  names[which],
                  owners,
                  columns == 0 ? "tree by tree (before):" :
                  columns == 1 ? "wide runs, 1 thread:" :
                                 "wide runs, 8 threads:",
                  ms,
                  columns == 0 ? "" :
                                 (" (" + std::to_string(column_passes / std::max(ms, 1.0e-9)) +
                                  " x faster)")
                                     .c_str());
    }
  }
}

/** The cast kernels of 2026-11-14 evening (Shape Cast, Shape Cast All, Raycast All, Ignore
 * Objects: QRay) in an NPC brain: the cast, a Branch on its Has Result, the distance (or -1) kept,
 * a small step ahead. Each NPC's step timed (record and flush, real Jolt) tree by tree, wide
 * through the bridge (as before: UPBGE_LN_COLUMN_NATIVE_RAYS=0, 8 threads) and wide with the
 * kernel (1 and 8 threads); the median of UPBGE_LN_BENCH_ROUNDS (5) rounds of
 * UPBGE_LN_BENCH_STEPS (50) steps. UPBGE_LN_BENCH_OWNERS NPCs (1,000). */
TEST(JoltLogicNodes, DISABLED_CastKernelsInCrowdsCost)
{
  BlenderObject player_blender("Player");
  BlenderObject wall_blender("WallX");
  const char *owners_text = std::getenv("UPBGE_LN_BENCH_OWNERS");
  const uint32_t owners = owners_text ? uint32_t(std::max(1, std::atoi(owners_text))) : 1000;
  const char *steps_text = std::getenv("UPBGE_LN_BENCH_STEPS");
  const int steps = steps_text ? std::max(1, std::atoi(steps_text)) : 50;
  const char *rounds_text = std::getenv("UPBGE_LN_BENCH_ROUNDS");
  const int rounds = rounds_text ? std::max(1, std::atoi(rounds_text)) : 5;
  const char *const names[] = {"Shape Cast (sphere)",
                               "Shape Cast (box, turned)",
                               "Shape Cast All",
                               "Raycast All",
                               "Raycast, Ignore Objects"};
  const auto build = [&](const int which) {
    Tree tree;
    const bool raycast = which >= 3;
    const bool all = which == 2 || which == 3;
    const char *idname = raycast ? (all ? "LogicNativeRaycastAll" : "LogicNativeRaycast") :
                                   (all ? "LogicNativeShapeCastAll" : "LogicNativeShapeCast");
    GenNode &cast = tree.Add(idname, which == 1 ? 1 : 0);
    tree.b.Link(*tree.step, "Out", cast, "Flow");
    tree.b.Bool(cast, "Local", true);
    tree.b.Vector(cast, "Destination", 0.0f, 8.0f, 0.0f);
    if (!raycast) {
      tree.b.Float(cast, "Radius", 0.4f);
    }
    if (which == 1) {
      GenNode &size = tree.Add("LogicNativeGetGamePropertyFloat");
      tree.b.String(size, "Property", "size");
      GenNode &extents = tree.Add("LogicNativeCombineXYZ");
      tree.b.Link(size, "Value", extents, "X");
      tree.b.Float(extents, "Y", 0.3f);
      tree.b.Float(extents, "Z", 0.5f);
      tree.b.Link(extents, "Vector", cast, "Half Extents");
      GenNode &turn = tree.Add("LogicNativeEuler");
      tree.b.Link(size, "Value", turn, "Z");
      tree.b.Link(turn, "Rotation", cast, "Rotation");
    }
    if (all) {
      tree.b.Int(cast, "Max Results", 3);
      tree.b.Vector(cast, "Destination", 0.0f, 15.0f, 0.0f);
    }
    if (which == 4) {
      tree.b.ObjectNamed(cast, "Ignore Object", &player_blender.object);
      GenNode &owner = tree.Add("LogicNativeGetOwner");
      GenNode &list = tree.Add("LogicNativeListFromItems", 10);
      GenNode &wall = tree.Add("LogicNativeFindObject");
      tree.b.ObjectNamed(wall, "Object", &wall_blender.object);
      tree.b.Link(wall, "Object", list, "Objects");
      tree.b.Link(owner, "Owner Object", list, "Objects");
      tree.b.Link(list, "Objects", cast, "Ignore Objects");
      tree.b.Vector(cast, "Destination", 0.0f, 15.0f, 0.0f);
    }
    GenNode &branch = tree.Add("LogicNativeBranch");
    tree.b.Link(*tree.step, "Out", branch, "Flow");
    tree.b.Link(cast, "Has Result", branch, "Condition");
    GenNode &seen = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(seen, "Property", "seen");
    if (!all) {
      tree.b.Link(cast, "Distance", seen, "Value");
    }
    else {
      tree.b.Float(seen, "Value", 1.0f);
    }
    tree.b.Link(branch, "True", seen, "Flow");
    GenNode &move = tree.Add("LogicNativeApplyMovement");
    tree.b.Vector(move, "Vector", 0.0f, 0.001f, 0.0f);
    tree.b.Bool(move, "Local", true);
    tree.b.Link(seen, "Done", move, "Flow");
    GenNode &lost = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(lost, "Property", "seen");
    tree.b.Float(lost, "Value", -1.0f);
    tree.b.Link(branch, "False", lost, "Flow");
    return tree.Compile();
  };
  struct Mode {
    const char *label;
    int columns; /* 0: tree by tree; else 1 + helpers */
    bool kernel;
  };
  const Mode modes[] = {{"tree by tree", 0, true},
                        {"wide, bridge (before), 8 threads", 8, false},
                        {"wide, kernel, 1 thread", 1, true},
                        {"wide, kernel, 8 threads", 8, true}};
  for (int which = 0; which < int(std::size(names)); which++) {
    double before = 0.0;
    for (const Mode &mode : modes) {
      LN_SetColumnNativeRaysForTests(mode.kernel ? -1 : 0);
      std::vector<double> times;
      std::string described;
      for (int round = 0; round < rounds; round++) {
        const std::shared_ptr<LN_Program> program = build(which);
        ASSERT_NE(program, nullptr);
        if (round == 0 && mode.columns != 0) {
          std::string refusal;
          described = DescribeColumns(*program, refusal);
          ASSERT_FALSE(described.empty()) << refusal;
        }
        SemRig rig(program, kProduction, owners);
        rig.SetDump(false);
        rig.SetObserveForces(false);
        if (mode.columns != 0) {
          rig.SetColumns(true, uint32_t(mode.columns - 1));
        }
        World world(int(owners) + 64, 7);
        for (uint32_t index = 0; index < owners; index++) {
          KX_GameObject &owner = rig.Object(index);
          owner.NodeSetWorldPosition(MT_Vector3(float(index % 40) * 3.0f - 58.5f,
                                                float(index / 40) * 3.0f - 36.0f,
                                                float(index % 3) * 0.5f));
          MT_Matrix3x3 facing;
          facing.setEuler(MT_Vector3(0.0f, 0.0f, float(index) * 0.645f));
          owner.NodeSetLocalOrientation(facing);
          owner.NodeUpdateGS(0.0);
          world.Box(owner, MT_Vector3(0.4f, 0.4f, 0.9f));
          rig.SetFloat(owner, "size", 0.2f + float(index % 5) * 0.1f);
        }
        world.Box(rig.AddObject("Player", MT_Vector3(0.0f, 0.0f, 0.5f)),
                  MT_Vector3(0.5f, 0.5f, 1.0f));
        world.Box(rig.AddObject("WallX", MT_Vector3(-8.0f, 0.0f, 0.0f)),
                  MT_Vector3(0.2f, 9.0f, 3.0f));
        world.Box(rig.AddObject("Ground", MT_Vector3(0.0f, 0.0f, -1.5f)),
                  MT_Vector3(80.0f, 80.0f, 0.5f));
        world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
        times.push_back(MillisecondsPerStep(rig, steps));
      }
      std::sort(times.begin(), times.end());
      const double ms = times[times.size() / 2];
      if (mode.columns == 0) {
        before = ms;
      }
      const bool bridged = described.find(" bridged=0 ") == std::string::npos;
      std::printf("%-26s %u NPCs, %-34s %8.3f ms a step (%.2f x tree by tree)%s\n",
                  names[which],
                  owners,
                  mode.label,
                  ms,
                  before / std::max(ms, 1.0e-9),
                  mode.columns != 0 && mode.kernel && bridged ? "  [still bridged]" : "");
    }
  }
  LN_SetColumnNativeRaysForTests(-1);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The start-of-tick query warm-up (2026-11-14)
 *
 * LN_RuntimeTree::WarmQueryCache works out queries before the tick's ops run and keeps the result
 * for the tick. It used to take every query of the program on every tick: a Projectile Ray behind
 * a Delay's Done or a Once never cast (its Flow was false before the ops ran, and that answer was
 * kept), and a Tween feeding a ray's Flow moved on frame ticks too (twice as fast).
 * \{ */

namespace {

/** The program's Projectile Ray query (the last one). */
uint32_t ProjectileRayQuery(const LN_Program &program)
{
  const std::vector<LN_QueryExpression> &queries = program.GetQueryExpressions();
  for (uint32_t index = uint32_t(queries.size()); index-- > 0;) {
    if (queries[index].kind == LN_QueryExpressionKind::ProjectileRay) {
      return index;
    }
  }
  return LN_INVALID_INDEX;
}

/** A Projectile Ray straight along +X whose Done adds 1 to "casts". */
GenNode &CountedProjectileRay(Tree &tree)
{
  GenNode &ray = tree.Add("LogicNativeProjectileRay", 1);
  tree.b.Vector(ray, "Aim", 1.0f, 0.0f, 0.0f);
  GenNode &count = tree.Add("LogicNativeModifyProperty");
  tree.b.String(count, "Property", "casts");
  tree.b.Float(count, "Value", 1.0f);
  tree.b.Link(ray, "Done", count, "Flow");
  return ray;
}

}  // namespace

TEST(JoltLogicNodes, AProjectileRayCastsBehindEveryKindOfFlow)
{
  /* Over 10 steps the ray casts each time its Flow fires ("fires" counts that output). */
  const char *const labels[] = {
      "Delay's Done", "Once", "Branch's True", "Raycast's Done", "Set Game Property's Done"};
  for (int source = 0; source < int(std::size(labels)); source++) {
    SCOPED_TRACE(labels[source]);
    Tree tree;
    GenNode *from = nullptr;
    const char *pin = "Done";
    switch (source) {
      case 0:
        from = &tree.Add("LogicNativeDelay");
        tree.b.Float(*from, "Delay", 0.05f);
        pin = "Out";
        break;
      case 1:
        from = &tree.Add("LogicNativeOnce");
        pin = "Out";
        break;
      case 2:
        from = &tree.Add("LogicNativeBranch");
        tree.b.Bool(*from, "Condition", true);
        pin = "True";
        break;
      case 3:
        from = &tree.Add("LogicNativeRaycast");
        tree.b.Vector(*from, "Destination", 10.0f, 0.0f, 0.0f);
        break;
      default:
        from = &tree.Add("LogicNativeSetGamePropertyBool");
        tree.b.String(*from, "Property", "a");
        tree.b.Bool(*from, "Value", true);
        break;
    }
    tree.b.Link(*tree.step, "Out", *from, "Flow");
    GenNode &ray = CountedProjectileRay(tree);
    tree.b.Link(*from, pin, ray, "Flow");
    GenNode &fires = tree.Add("LogicNativeModifyProperty");
    tree.b.String(fires, "Property", "fires");
    tree.b.Float(fires, "Value", 1.0f);
    tree.b.Link(*from, pin, fires, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    for (const SemMode &mode : Modes()) {
      SCOPED_TRACE(mode.name);
      SemRig rig(program, mode);
      World world;
      KX_GameObject &owner = rig.Object(0);
      rig.SetFloat(owner, "casts", 0.0f);
      rig.SetFloat(owner, "fires", 0.0f);
      rig.SetBool(owner, "a", false);
      for (int step = 0; step < 10; step++) {
        rig.Step();
      }
      /* (Before the fix a Delay's Done and a Once let no cast through: 0.) */
      EXPECT_GE(rig.Float(owner, "fires"), 1.0f);
      EXPECT_EQ(rig.Float(owner, "casts"), rig.Float(owner, "fires"));
    }
  }
}

/** A Tween whose Done leads (through Translate's When Done) to a Projectile Ray's Flow, in a tree
 * that also runs on frames: the Tween moves only on the steps that read it. */
TEST(JoltLogicNodes, ATweenFeedingAProjectileRayKeepsItsSpeed)
{
  for (const bool with_ray : {false, true}) {
    SCOPED_TRACE(with_ray ? "with the ray" : "alone");
    Tree tree;
    GenNode &first = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(first, "Property", "u");
    tree.b.Link(*tree.step, "Out", first, "Flow");
    GenNode &tween = tree.Add("LogicNativeTweenValue");
    tree.b.Float(tween, "FromFloat", 0.0f);
    tree.b.Float(tween, "ToFloat", 1.0f);
    tree.b.Float(tween, "Duration", 0.5f);
    tree.b.Link(first, "Done", tween, "Forward");
    GenNode &store = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(store, "Property", "t");
    tree.b.Link(tween, "ResultFloat", store, "Value");
    tree.b.Link(*tree.step, "Out", store, "Flow");
    if (with_ray) {
      GenNode &move = tree.Add("LogicNativeTranslate");
      tree.b.Link(tween, "Done", move, "Flow");
      GenNode &ray = CountedProjectileRay(tree);
      tree.b.Link(move, "When Done", ray, "Flow");
    }
    /* A frame route too: the tree runs (and its warm-up) on frames. */
    GenNode &frame_event = tree.Add("LogicNativeOnUpdate");
    GenNode &frame_write = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(frame_write, "Property", "w");
    tree.b.Link(frame_event, "Out", frame_write, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    for (const SemMode &mode : Modes()) {
      SCOPED_TRACE(mode.name);
      SemRig rig(program, mode);
      World world;
      KX_GameObject &owner = rig.Object(0);
      rig.SetFloat(owner, "t", -1.0f);
      rig.SetFloat(owner, "casts", 0.0f);
      for (int frame = 0; frame < 6; frame++) {
        rig.Frame();
      }
      /* 6 steps of 1/60 s into a Tween of 0.5 s: 0.2 (a frame tick moving it gave 0.4). */
      EXPECT_NEAR(rig.Float(owner, "t"), 6.0f / 30.0f, 1.0e-5f);
      /* The Tween's Done runs a step while it moves: one cast each. */
      EXPECT_EQ(rig.Float(owner, "casts"), with_ray ? 6.0f : 0.0f);
    }
  }
}

/** A step route's Projectile Ray is not cast on frame ticks (its Visualize drew a line on each
 * frame the warm-up cast it, where no op read it). */
TEST(JoltLogicNodes, AStepRoutesProjectileRayIsNotCastOnFrames)
{
  Tree tree;
  GenNode &ray = CountedProjectileRay(tree);
  tree.b.Bool(ray, "Visualize", true);
  tree.b.Link(*tree.step, "Out", ray, "Flow");
  GenNode &frame_event = tree.Add("LogicNativeOnUpdate");
  GenNode &frame_write = tree.Add("LogicNativeSetGamePropertyFloat");
  tree.b.String(frame_write, "Property", "w");
  tree.b.Link(frame_event, "Out", frame_write, "Flow");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : Modes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    World world;
    KX_GameObject &owner = rig.Object(0);
    rig.SetFloat(owner, "casts", 0.0f);
    LN_DebugLinesForTests lines;
    LN_SetDebugLinesForTests(&lines);
    for (int frame = 0; frame < 3; frame++) {
      rig.Update();
    }
    EXPECT_TRUE(lines.empty()) << lines.size() << " lines drawn on frames";
    rig.Step();
    EXPECT_FALSE(lines.empty());
    EXPECT_EQ(rig.Float(owner, "casts"), 1.0f);
    LN_SetDebugLinesForTests(nullptr);
  }
}

/** Which events' warm-ups work a query out (LN_Program::GetQueryWarmEvents). */
TEST(JoltLogicNodes, TheWarmUpTakesOnlyQueriesItCanKeep)
{
  constexpr uint8_t kFrames = 1u << uint8_t(LN_Event::OnUpdate);
  constexpr uint8_t kSteps = 1u << uint8_t(LN_Event::OnFixedUpdate);
  const auto events_of = [](Tree &tree, const bool known = true) {
    const std::shared_ptr<LN_Program> program = tree.Compile();
    EXPECT_NE(program, nullptr);
    if (program == nullptr) {
      return uint8_t(0xff);
    }
    const uint32_t query = ProjectileRayQuery(*program);
    EXPECT_NE(query, LN_INVALID_INDEX);
    if (query == LN_INVALID_INDEX) {
      return uint8_t(0xff);
    }
    /* The walk knows every input of the ray (kKindInputSupplement: the fields the rig misses),
     * so the answer is the rule's, not the unknown walk's. */
    const std::string reach = LN_ColumnQueryReachForTests(*program, 'Q', query);
    EXPECT_EQ(reach.rfind("unknown", 0) == 0, !known) << reach;
    return program->GetQueryWarmEvents()[query];
  };
  /* The events alone (LN_QUERY_WARM_ON_HELPERS aside: the last case below). */
  const auto events_only = [&](Tree &tree, const bool known = true) {
    return uint8_t(events_of(tree, known) & ~LN_QUERY_WARM_ON_HELPERS);
  };
  {
    /* A step route's ray, in a tree with a frame route: on steps only (it was cast on frames
     * too, where no op reads it). (Projectile Ray needs Logic + Physics Steps.) */
    Tree tree;
    GenNode &ray = CountedProjectileRay(tree);
    tree.b.Link(*tree.step, "Out", ray, "Flow");
    GenNode &frame_event = tree.Add("LogicNativeOnUpdate");
    GenNode &frame_write = tree.Add("LogicNativeSetGamePropertyFloat");
    tree.b.String(frame_write, "Property", "w");
    tree.b.Link(frame_event, "Out", frame_write, "Flow");
    EXPECT_EQ(events_only(tree), kSteps);
    EXPECT_EQ(events_of(tree) & kFrames, 0);
  }
  {
    /* Behind a Delay: never (its Flow holds only once the Delay's op ran). */
    Tree tree;
    GenNode &delay = tree.Add("LogicNativeDelay");
    tree.b.Float(delay, "Delay", 0.05f);
    tree.b.Link(*tree.step, "Out", delay, "Flow");
    GenNode &ray = CountedProjectileRay(tree);
    tree.b.Link(delay, "Out", ray, "Flow");
    EXPECT_EQ(events_of(tree), 0);
  }
  {
    /* A Local a Flow Gate's Is Open gives (a kind keeping state): never (working it out at the
     * tick's start may set what the gate keeps where its route never reads it). */
    Tree tree;
    GenNode &gate = tree.Add("LogicNativeFlowGate");
    tree.b.Bool(gate, "Start Open", true);
    tree.b.Link(*tree.step, "Out", gate, "Enter");
    GenNode &ray = CountedProjectileRay(tree);
    tree.b.Link(gate, "Is Open", ray, "Local");
    tree.b.Link(*tree.step, "Out", ray, "Flow");
    EXPECT_EQ(events_of(tree), 0);
  }
  {
    /* An Aim a Tween gives: never (its result kinds read through the Tween's state, a walk
     * that cannot know them: not warmed either). */
    Tree tree;
    GenNode &tween = tree.Add("LogicNativeTweenValue");
    tree.b.Vector(tween, "FromVector", 1.0f, 0.0f, 0.0f);
    tree.b.Vector(tween, "ToVector", 0.0f, 1.0f, 0.0f);
    tree.b.Link(*tree.step, "Out", tween, "Forward");
    GenNode &ray = CountedProjectileRay(tree);
    tree.b.Link(tween, "ResultVector", ray, "Aim");
    tree.b.Link(*tree.step, "Out", ray, "Flow");
    EXPECT_EQ(events_of(tree, false), 0);
  }
  {
    /* 2026-11-14: a plain ray may be warmed on a helper (LN_WarmQueryCaches); one that may draw
     * (Visualize linked) only on the main thread. */
    Tree plain;
    GenNode &ray = CountedProjectileRay(plain);
    plain.b.Link(*plain.step, "Out", ray, "Flow");
    EXPECT_NE(events_of(plain) & LN_QUERY_WARM_ON_HELPERS, 0);
    Tree drawn;
    GenNode &drawing = CountedProjectileRay(drawn);
    drawn.b.Link(*drawn.step, "Out", drawing, "Flow");
    GenNode &draw = drawn.Add("LogicNativeGetGamePropertyBool");
    drawn.b.String(draw, "Property", "draw");
    drawn.b.Link(draw, "Value", drawing, "Visualize");
    EXPECT_EQ(events_only(drawn), kSteps);
    EXPECT_EQ(events_of(drawn) & LN_QUERY_WARM_ON_HELPERS, 0);
  }
}

/** \} */
