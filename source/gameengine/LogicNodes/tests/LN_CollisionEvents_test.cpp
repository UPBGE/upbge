/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * On Collision's Events dropdown (2026-11-09): Each Object runs On Enter, On Stay and On Exit once
 * for every object that arrives, stays or leaves, in the same step, its Collided Object, Contact
 * Count, Point and Normal telling about that run's object; Any Object is the node as it was. And
 * the list of touching objects made with a short scan, then a map (a floor touched by thousands
 * of objects scanned the whole list for every contact).
 *
 * The rig has no physics: the contacts each owner reads come from LN_SetCollisionContactsForTests,
 * as the physics environment's per-step cache gives them (one entry per touching pair of shapes,
 * an object in several entries for a compound shape, the owner as either object of a pair). */

#include "testing/testing.h"

#include <deque>
#include <string>
#include <vector>

#include "LN_Manager.h"
#include "LN_SemRig.h"
#include "PHY_IPhysicsEnvironment.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

constexpr int16_t kAnyObject = 0;
constexpr int16_t kEachObject = 1;
/* Typecast Value's To String / To Float (custom1). */
constexpr int16_t kToString = 2;
constexpr int16_t kToFloat = 3;

/** One touching pair of shapes as the physics' cache lists it. */
struct Touch {
  KX_GameObject *other = nullptr;
  int count = 1;
  MT_Vector3 point = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** The normal as stored: from object0's side. The owner reads it negated when it is object1. */
  MT_Vector3 normal = MT_Vector3(0.0f, 0.0f, 1.0f);
  bool owner_second = false;
};

/** The contacts of this step, per owner (kept alive as long as the test runs). */
class Contacts {
 public:
  Contacts()
  {
    LN_SetCollisionContactsForTests(&m_map);
  }
  ~Contacts()
  {
    LN_SetCollisionContactsForTests(nullptr);
  }
  Contacts(const Contacts &) = delete;

  void Set(KX_GameObject &owner, const std::vector<Touch> &touching)
  {
    std::vector<const PHY_CachedCollisionContact *> &list = m_map[&owner];
    list.clear();
    for (const Touch &touch : touching) {
      PHY_CachedCollisionContact &contact = m_storage.emplace_back();
      contact.object0 = touch.owner_second ? touch.other : &owner;
      contact.object1 = touch.owner_second ? &owner : touch.other;
      contact.contact_count = touch.count;
      contact.points = {touch.point};
      contact.normals = {touch.normal};
      list.push_back(&contact);
    }
  }

 private:
  std::deque<PHY_CachedCollisionContact> m_storage;
  LN_CollisionContactsForTests m_map;
};

/** Modify Property (Add) of `property` by 1 (or by `value_node.value_pin`), on the owner or on the
 * node's Collided Object, run by `touch`'s output `flow`. */
GenNode &AddOne(TreeBuilder &b,
                GenNode &touch,
                const char *flow,
                const char *property,
                const bool on_collided,
                GenNode *value_node = nullptr,
                const char *value_pin = nullptr)
{
  GenNode &modify = b.Add("LogicNativeModifyProperty");
  b.String(modify, "Property", property);
  b.Float(modify, "Value", 1.0f);
  if (on_collided) {
    b.Link(touch, "Collided Object", modify, "Object");
  }
  if (value_node != nullptr) {
    b.Link(*value_node, value_pin, modify, "Value");
  }
  b.Link(touch, flow, modify, "Flow");
  return modify;
}

/** On Collision with `events`: each output's runs counted on the owner (enters, stays, exits)
 * and on the run's Collided Object (entered, stayed, exited); in On Enter also its Contact Count
 * (contacts), Point x (px) and Normal z (nz). */
std::shared_ptr<LN_Program> CountingTree(const int16_t events, std::string *report)
{
  TreeBuilder b;
  GenNode &touch = b.Add("LogicNativeOnCollision", events);
  AddOne(b, touch, "On Enter", "enters", false);
  AddOne(b, touch, "On Enter", "entered", true);
  AddOne(b, touch, "On Stay", "stays", false);
  AddOne(b, touch, "On Stay", "stayed", true);
  AddOne(b, touch, "On Exit", "exits", false);
  AddOne(b, touch, "On Exit", "exited", true);
  GenNode &count = b.Add("LogicNativeTypecast", kToFloat);
  b.Link(touch, "Contact Count", count, "Value");
  AddOne(b, touch, "On Enter", "contacts", true, &count, "Float");
  GenNode &point = b.Add("LogicNativeSeparateXYZ");
  b.Link(touch, "Point", point, "Vector");
  AddOne(b, touch, "On Enter", "px", true, &point, "X");
  GenNode &normal = b.Add("LogicNativeSeparateXYZ");
  b.Link(touch, "Normal", normal, "Vector");
  AddOne(b, touch, "On Enter", "nz", true, &normal, "Z");
  return b.Compile(report);
}

const char *const kOwnerCounts[] = {"enters", "stays", "exits"};
const char *const kObjectCounts[] = {"entered", "stayed", "exited", "contacts", "px", "nz"};

void ZeroCounts(SemRig &rig, KX_GameObject &owner, const std::vector<KX_GameObject *> &others)
{
  for (const char *name : kOwnerCounts) {
    rig.SetFloat(owner, name, 0.0f);
  }
  for (KX_GameObject *other : others) {
    for (const char *name : kObjectCounts) {
      rig.SetFloat(*other, name, 0.0f);
    }
  }
}

/** "enters stays exits | A: entered stayed exited contacts px nz | B: ..." */
std::string Counts(SemRig &rig, KX_GameObject &owner, const std::vector<KX_GameObject *> &others)
{
  std::string text;
  for (const char *name : kOwnerCounts) {
    text += std::to_string(int(rig.Float(owner, name))) + " ";
  }
  for (KX_GameObject *other : others) {
    text += "| " + other->GetName() + ":";
    for (const char *name : kObjectCounts) {
      text += " " + std::to_string(int(rig.Float(*other, name)));
    }
    text += " ";
  }
  return text;
}

}  // namespace

/* The step by step story of the page (On_Collision_Each_Object.html): two objects land at once, a
 * third arrives while they lie there, one leaves, the rest leave, the first comes back. Each
 * Object runs every event for every object; Point, Normal and Contact Count are the run's
 * object's (A touches with two shape pairs: 2 + 3 contacts, the first pair's point; B lists the
 * owner second, so its normal reads negated). In every mode of the rig. */
TEST(LN_CollisionEvents, EachObjectRunsAnEventForEveryObject)
{
  std::string report;
  const std::shared_ptr<LN_Program> program = CountingTree(kEachObject, &report);
  ASSERT_NE(program, nullptr) << report;
  EXPECT_EQ(program->GetCollisionEventNodeCount(), 1u);
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    KX_GameObject &owner = rig.Object(0);
    KX_GameObject &a = rig.AddObject("A", MT_Vector3(1.0f, 0.0f, 0.0f));
    KX_GameObject &b = rig.AddObject("B", MT_Vector3(2.0f, 0.0f, 0.0f));
    KX_GameObject &c = rig.AddObject("C", MT_Vector3(3.0f, 0.0f, 0.0f));
    const std::vector<KX_GameObject *> others = {&a, &b, &c};
    ZeroCounts(rig, owner, others);
    Contacts contacts;
    const Touch a1{&a, 2, MT_Vector3(1.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 1.0f)};
    const Touch a2{&a, 3, MT_Vector3(7.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 5.0f)};
    const Touch b1{&b, 1, MT_Vector3(2.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 1.0f), true};
    const Touch c1{&c, 4, MT_Vector3(3.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 1.0f)};

    contacts.Set(owner, {a1, b1, a2});
    rig.Step();
    EXPECT_EQ(Counts(rig, owner, others),
              "2 0 0 | A: 1 0 0 5 1 1 | B: 1 0 0 1 2 -1 | C: 0 0 0 0 0 0 ");

    contacts.Set(owner, {a1, b1, a2, c1});
    rig.Step();
    EXPECT_EQ(Counts(rig, owner, others),
              "3 2 0 | A: 1 1 0 5 1 1 | B: 1 1 0 1 2 -1 | C: 1 0 0 4 3 1 ");

    contacts.Set(owner, {c1, b1});
    rig.Step();
    EXPECT_EQ(Counts(rig, owner, others),
              "3 4 1 | A: 1 1 1 5 1 1 | B: 1 2 0 1 2 -1 | C: 1 1 0 4 3 1 ");

    contacts.Set(owner, {});
    rig.Step();
    EXPECT_EQ(Counts(rig, owner, others),
              "3 4 3 | A: 1 1 1 5 1 1 | B: 1 2 1 1 2 -1 | C: 1 1 1 4 3 1 ");

    rig.Step(); /* nothing touches, nothing left: no event */
    contacts.Set(owner, {a2});
    rig.Step();
    EXPECT_EQ(Counts(rig, owner, others),
              "4 4 3 | A: 2 1 1 8 8 6 | B: 1 2 1 1 2 -1 | C: 1 1 1 4 3 1 ");
  }
}

/* Any Object is the node as it was: one Enter for the first arrival (Collided Object the first
 * contact), nothing for a later one while something touches, one Stay a step, one Exit when the
 * last leaves (Collided Object the last step's first contact, the exit payload). */
TEST(LN_CollisionEvents, AnyObjectKeepsTheOldEvents)
{
  std::string report;
  const std::shared_ptr<LN_Program> program = CountingTree(kAnyObject, &report);
  ASSERT_NE(program, nullptr) << report;
  EXPECT_EQ(program->GetCollisionEventNodeCount(), 0u);
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    KX_GameObject &owner = rig.Object(0);
    KX_GameObject &a = rig.AddObject("A", MT_Vector3(1.0f, 0.0f, 0.0f));
    KX_GameObject &b = rig.AddObject("B", MT_Vector3(2.0f, 0.0f, 0.0f));
    KX_GameObject &c = rig.AddObject("C", MT_Vector3(3.0f, 0.0f, 0.0f));
    const std::vector<KX_GameObject *> others = {&a, &b, &c};
    ZeroCounts(rig, owner, others);
    Contacts contacts;
    const Touch a1{&a, 2, MT_Vector3(1.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 1.0f)};
    const Touch b1{&b, 1, MT_Vector3(2.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 1.0f), true};
    const Touch c1{&c, 4, MT_Vector3(3.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 1.0f)};

    /* Contact Count is the sum over every object, Point and Normal the first contact's. */
    contacts.Set(owner, {a1, b1});
    rig.Step();
    EXPECT_EQ(Counts(rig, owner, others),
              "1 0 0 | A: 1 0 0 3 1 1 | B: 0 0 0 0 0 0 | C: 0 0 0 0 0 0 ");
    contacts.Set(owner, {a1, b1, c1});
    rig.Step();
    EXPECT_EQ(Counts(rig, owner, others),
              "1 1 0 | A: 1 1 0 3 1 1 | B: 0 0 0 0 0 0 | C: 0 0 0 0 0 0 ");
    contacts.Set(owner, {c1, b1});
    rig.Step();
    EXPECT_EQ(Counts(rig, owner, others),
              "1 2 0 | A: 1 1 0 3 1 1 | B: 0 0 0 0 0 0 | C: 0 1 0 0 0 0 ");
    contacts.Set(owner, {});
    rig.Step();
    EXPECT_EQ(Counts(rig, owner, others),
              "1 2 1 | A: 1 1 0 3 1 1 | B: 0 0 0 0 0 0 | C: 0 1 1 0 0 0 ");
  }
}

/* Collided Object means the run's object only inside the node's own event routes: read by an
 * On Logic + Physics Step route of the same tree it stays the first contact, in the same steps.
 * The runs go in contact order (Collided Objects' order), each in a scope of its own. */
TEST(LN_CollisionEvents, CollidedObjectIsTheRunsObjectOnlyInsideTheEventRoutes)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &touch = b.Add("LogicNativeOnCollision", kEachObject);
  GenNode &name = b.Add("LogicNativeTypecast", kToString);
  b.Link(touch, "Collided Object", name, "Value");
  GenNode &first = b.Add("LogicNativeSetGamePropertyString");
  b.String(first, "Property", "first");
  b.Link(name, "String", first, "Value");
  b.Link(step, "Out", first, "Flow");
  /* Each run appends its object's name: the runs' order. */
  GenNode &append = b.Add("LogicNativeListAppend", 1);
  b.String(append, "Property", "arrivals");
  b.Link(name, "String", append, "Value");
  b.Link(touch, "On Enter", append, "Flow");
  GenNode &last = b.Add("LogicNativeSetGamePropertyString");
  b.String(last, "Property", "last_arrival");
  b.Link(name, "String", last, "Value");
  b.Link(touch, "On Enter", last, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    KX_GameObject &owner = rig.Object(0);
    KX_GameObject &a = rig.AddObject("A", MT_Vector3(1.0f, 0.0f, 0.0f));
    KX_GameObject &bb = rig.AddObject("B", MT_Vector3(2.0f, 0.0f, 0.0f));
    KX_GameObject &c = rig.AddObject("C", MT_Vector3(3.0f, 0.0f, 0.0f));
    KX_GameObject &d = rig.AddObject("D", MT_Vector3(4.0f, 0.0f, 0.0f));
    rig.SetString(owner, "first", "");
    rig.SetString(owner, "last_arrival", "");
    Contacts contacts;
    contacts.Set(owner, {Touch{&bb}, Touch{&a}});
    rig.Step();
    EXPECT_EQ(rig.Text(owner, "first"), "B");
    EXPECT_EQ(rig.Text(owner, "last_arrival"), "A");
    /* C and D arrive while B and A lie there: the step's route still reads the first contact. */
    contacts.Set(owner, {Touch{&bb}, Touch{&a}, Touch{&d}, Touch{&c}});
    rig.Step();
    EXPECT_EQ(rig.Text(owner, "first"), "B");
    EXPECT_EQ(rig.Text(owner, "last_arrival"), "C");
    EXPECT_EQ(rig.Text(owner, "arrivals"), "[B, A, D, C]");
  }
}

/* A Loop in an event route runs whole for each object (its frame restarts in each run), and its
 * Completed after each of them; a Delay's Done is no part of the route: it runs once. */
TEST(LN_CollisionEvents, ALoopInAnEventRouteRunsForEachObject)
{
  TreeBuilder b;
  GenNode &touch = b.Add("LogicNativeOnCollision", kEachObject);
  GenNode &loop = b.Add("LogicNativeLoop");
  b.Int(loop, "Count", 3);
  b.Link(touch, "On Enter", loop, "Flow");
  GenNode &body = b.Add("LogicNativeModifyProperty");
  b.String(body, "Property", "loops");
  b.Float(body, "Value", 1.0f);
  b.Link(touch, "Collided Object", body, "Object");
  b.Link(loop, "Loop", body, "Flow");
  GenNode &done = b.Add("LogicNativeModifyProperty");
  b.String(done, "Property", "done");
  b.Float(done, "Value", 1.0f);
  b.Link(touch, "Collided Object", done, "Object");
  b.Link(loop, "Completed", done, "Flow");
  GenNode &delay = b.Add("LogicNativeDelay");
  b.Float(delay, "Delay", 0.0f);
  b.Link(touch, "On Enter", delay, "Flow");
  GenNode &after = b.Add("LogicNativeModifyProperty");
  b.String(after, "Property", "after_delay");
  b.Float(after, "Value", 1.0f);
  b.Link(delay, "Out", after, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    KX_GameObject &owner = rig.Object(0);
    KX_GameObject &a = rig.AddObject("A", MT_Vector3(1.0f, 0.0f, 0.0f));
    KX_GameObject &bb = rig.AddObject("B", MT_Vector3(2.0f, 0.0f, 0.0f));
    for (KX_GameObject *object : {&a, &bb}) {
      rig.SetFloat(*object, "loops", 0.0f);
      rig.SetFloat(*object, "done", 0.0f);
    }
    rig.SetFloat(owner, "after_delay", 0.0f);
    Contacts contacts;
    contacts.Set(owner, {Touch{&a}, Touch{&bb}});
    for (int step = 0; step < 4; step++) {
      rig.Step();
    }
    EXPECT_EQ(rig.Float(a, "loops"), 3.0f);
    EXPECT_EQ(rig.Float(bb, "loops"), 3.0f);
    EXPECT_EQ(rig.Float(a, "done"), 1.0f);
    EXPECT_EQ(rig.Float(bb, "done"), 1.0f);
    EXPECT_EQ(rig.Float(owner, "after_delay"), 1.0f);
  }
}

/* A Projectile Ray is kept for the tick (and worked out before the trees run): inside an Each
 * Object route it is taken again for each run, so a ray cast from each Collided Object starts at
 * that object (the rig has no physics: the arc's first point is its origin). */
TEST(LN_CollisionEvents, AProjectileRayInAnEventRouteIsEachRunsOwn)
{
  TreeBuilder b;
  GenNode &touch = b.Add("LogicNativeOnCollision", kEachObject);
  GenNode &ray = b.Add("LogicNativeProjectileRay");
  b.Link(touch, "On Enter", ray, "Flow");
  b.Link(touch, "Collided Object", ray, "Caster");
  b.Bool(ray, "Local", true);
  GenNode &first = b.Add("LogicNativeListGetItem");
  b.Link(ray, "PARABOLA", first, "List");
  GenNode &text = b.Add("LogicNativeTypecast", kToString);
  b.Link(first, "Value", text, "Value");
  GenNode &write = b.Add("LogicNativeSetGamePropertyString");
  b.String(write, "Property", "origin");
  b.Link(touch, "Collided Object", write, "Object");
  b.Link(text, "String", write, "Value");
  b.Link(touch, "On Enter", write, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    KX_GameObject &owner = rig.Object(0);
    KX_GameObject &a = rig.AddObject("A", MT_Vector3(1.0f, 0.0f, 0.0f));
    KX_GameObject &bb = rig.AddObject("B", MT_Vector3(2.0f, 0.0f, 0.0f));
    rig.SetString(a, "origin", "");
    rig.SetString(bb, "origin", "");
    Contacts contacts;
    contacts.Set(owner, {Touch{&a}, Touch{&bb}});
    rig.Step();
    EXPECT_EQ(rig.Text(a, "origin"), "(1, 0, 0)");
    EXPECT_EQ(rig.Text(bb, "origin"), "(2, 0, 0)");
  }
}

/* Every owner keeps its own events (two trees of one program), and a crowd of objects past the
 * short scan: each object once, in contact order, its compound shape's contacts summed. Collided
 * Objects lists them once each in both modes. */
TEST(LN_CollisionEvents, ManyObjectsAndOwners)
{
  std::string report;
  TreeBuilder b;
  GenNode &touch = b.Add("LogicNativeOnCollision", kEachObject);
  AddOne(b, touch, "On Enter", "enters", false);
  GenNode &count = b.Add("LogicNativeTypecast", kToFloat);
  b.Link(touch, "Contact Count", count, "Value");
  AddOne(b, touch, "On Enter", "contacts", true, &count, "Float");
  AddOne(b, touch, "On Exit", "exits", false);
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &length = b.Add("LogicNativeListLength");
  b.Link(touch, "Collided Objects", length, "List");
  GenNode &listed = b.Add("LogicNativeSetGamePropertyInt");
  b.String(listed, "Property", "listed");
  b.Link(length, "Length", listed, "Value");
  b.Link(step, "Out", listed, "Flow");
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  constexpr int kCrowd = 40;
  SemRig rig(program, kProduction, 2);
  KX_GameObject &first = rig.Object(0);
  KX_GameObject &second = rig.Object(1);
  std::vector<KX_GameObject *> crowd;
  for (int index = 0; index < kCrowd; index++) {
    KX_GameObject &object = rig.AddObject("Crowd" + std::to_string(index),
                                          MT_Vector3(float(index), 0.0f, 0.0f));
    rig.SetFloat(object, "contacts", 0.0f);
    crowd.push_back(&object);
  }
  for (KX_GameObject *owner : {&first, &second}) {
    rig.SetFloat(*owner, "enters", 0.0f);
    rig.SetFloat(*owner, "exits", 0.0f);
    rig.SetInt(*owner, "listed", 0);
  }
  Contacts contacts;
  /* The first owner: every crowd object with two shape pairs (1 + 2 contacts), all first pairs,
   * then all second ones; the second owner: the even ones once. */
  std::vector<Touch> all;
  for (KX_GameObject *object : crowd) {
    all.push_back(Touch{object, 1});
  }
  for (KX_GameObject *object : crowd) {
    all.push_back(Touch{object, 2, MT_Vector3(0.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 1.0f),
                        true});
  }
  std::vector<Touch> even;
  for (int index = 0; index < kCrowd; index += 2) {
    even.push_back(Touch{crowd[index], 10});
  }
  contacts.Set(first, all);
  contacts.Set(second, even);
  rig.Step();
  EXPECT_EQ(rig.Float(first, "enters"), float(kCrowd));
  EXPECT_EQ(rig.Float(second, "enters"), float(kCrowd / 2));
  EXPECT_EQ(rig.Float(first, "listed"), float(kCrowd));
  EXPECT_EQ(rig.Float(second, "listed"), float(kCrowd / 2));
  for (int index = 0; index < kCrowd; index++) {
    EXPECT_EQ(rig.Float(*crowd[index], "contacts"), index % 2 == 0 ? 13.0f : 3.0f) << index;
  }
  /* Half the crowd leaves the first owner: 20 exits; nothing new enters. */
  std::vector<Touch> half(all.begin(), all.begin() + kCrowd / 2);
  contacts.Set(first, half);
  rig.Step();
  EXPECT_EQ(rig.Float(first, "enters"), float(kCrowd));
  EXPECT_EQ(rig.Float(first, "exits"), float(kCrowd / 2));
  EXPECT_EQ(rig.Float(first, "listed"), float(kCrowd / 2));
  EXPECT_EQ(rig.Float(second, "exits"), 0.0f);
}

/* A tree's object refs past the short scan come from a map: each object keeps its slot, a removed
 * object's slot goes to the next new object (the lowest emptied first, as the scan gave it), and
 * an old ref to a reused slot resolves to nothing. */
TEST(LN_CollisionEvents, ObjectRefsPastTheShortScanKeepTheirSlots)
{
  std::string report;
  const std::shared_ptr<LN_Program> program = CountingTree(kAnyObject, &report);
  ASSERT_NE(program, nullptr) << report;
  SemRig rig(program, kProduction);
  LN_RuntimeTree &tree = rig.Tree(0);
  std::vector<KX_GameObject *> objects;
  for (int index = 0; index < 40; index++) {
    objects.push_back(&rig.AddObject("Ref" + std::to_string(index), MT_Vector3(0.0f, 0.0f, 0.0f)));
  }
  std::vector<LN_RuntimeRef> refs;
  for (KX_GameObject *object : objects) {
    refs.push_back(tree.MakeObjectRef(object, object->GetName()));
  }
  for (size_t index = 0; index < objects.size(); index++) {
    EXPECT_EQ(tree.ResolveObjectRef(refs[index]), objects[index]) << index;
    const LN_RuntimeRef again = tree.MakeObjectRef(objects[index], objects[index]->GetName());
    EXPECT_EQ(again.slot, refs[index].slot) << index;
    EXPECT_EQ(again.generation, refs[index].generation) << index;
  }
  /* Objects 30, 5 and 20 go: the next new objects take slots 5, 20, 30 in that order. */
  for (const int gone : {30, 5, 20}) {
    tree.InvalidateObjectRef(objects[gone]);
    EXPECT_EQ(tree.ResolveObjectRef(refs[gone]), nullptr) << gone;
  }
  KX_GameObject &late0 = rig.AddObject("Late0", MT_Vector3(0.0f, 0.0f, 0.0f));
  KX_GameObject &late1 = rig.AddObject("Late1", MT_Vector3(0.0f, 0.0f, 0.0f));
  KX_GameObject &late2 = rig.AddObject("Late2", MT_Vector3(0.0f, 0.0f, 0.0f));
  KX_GameObject &late3 = rig.AddObject("Late3", MT_Vector3(0.0f, 0.0f, 0.0f));
  const LN_RuntimeRef ref0 = tree.MakeObjectRef(&late0, "Late0");
  const LN_RuntimeRef ref1 = tree.MakeObjectRef(&late1, "Late1");
  const LN_RuntimeRef ref2 = tree.MakeObjectRef(&late2, "Late2");
  const LN_RuntimeRef ref3 = tree.MakeObjectRef(&late3, "Late3");
  EXPECT_EQ(ref0.slot, refs[5].slot);
  EXPECT_EQ(ref1.slot, refs[20].slot);
  EXPECT_EQ(ref2.slot, refs[30].slot);
  EXPECT_EQ(ref3.slot, uint32_t(objects.size()));
  EXPECT_EQ(tree.ResolveObjectRef(ref0), &late0);
  EXPECT_EQ(tree.ResolveObjectRef(refs[5]), nullptr); /* (the old ref: another generation) */
  EXPECT_EQ(tree.ResolveObjectRef(ref3), &late3);
  for (size_t index = 0; index < objects.size(); index++) {
    if (index != 5 && index != 20 && index != 30) {
      EXPECT_EQ(tree.MakeObjectRef(objects[index], "").slot, refs[index].slot) << index;
    }
  }
}

/* Each Object events run on the main thread, one tree at a time: the columns and the tree batches
 * refuse them, by name. An Any Object node is refused by neither for being one. */
TEST(LN_CollisionEvents, ColumnsAndBatchesLeaveEachObjectEventsToTheColumnPass)
{
  std::string report;
  const std::shared_ptr<LN_Program> each = CountingTree(kEachObject, &report);
  ASSERT_NE(each, nullptr) << report;
  std::string refusal;
  EXPECT_EQ(LN_CompileColumnProgram(*each, LN_Event::OnFixedUpdate, &refusal), nullptr);
  EXPECT_EQ(refusal, "an On Collision Each Object route");
  EXPECT_FALSE(LN_AnalyzeTreeBatchSafety(*each).eligible);
  EXPECT_FALSE(LN_ColumnRefusalHint(refusal).empty());

  const std::shared_ptr<LN_Program> any = CountingTree(kAnyObject, &report);
  ASSERT_NE(any, nullptr) << report;
  refusal.clear();
  LN_CompileColumnProgram(*any, LN_Event::OnFixedUpdate, &refusal);
  EXPECT_EQ(refusal.find("Each Object"), std::string::npos) << refusal;
  EXPECT_EQ(LN_AnalyzeTreeBatchSafety(*any).reason.find("Each Object"), std::string::npos);
}

/* With the manager, objects are compared by its handles: an object removed while touching leaves
 * (On Exit, with no Collided Object), and an object at its address afterwards is a new one (it
 * enters). A scene merge renumbers the handles: the touching objects stay (no Exit, no Enter). */
TEST(LN_CollisionEvents, ObjectsAreComparedByTheManagersHandles)
{
  std::string report;
  const std::shared_ptr<LN_Program> program = CountingTree(kEachObject, &report);
  ASSERT_NE(program, nullptr) << report;
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  /* (Made before the rig: they outlive its trees.) */
  auto manager = std::make_unique<LN_Manager>(*unused_scene);
  auto merged = std::make_unique<LN_Manager>(*unused_scene);
  SemRig rig(program, kProduction);
  rig.Tree(0).SetLogicManager(manager.get());
  KX_GameObject &owner = rig.Object(0);
  KX_GameObject &a = rig.AddObject("A", MT_Vector3(1.0f, 0.0f, 0.0f));
  KX_GameObject &bb = rig.AddObject("B", MT_Vector3(2.0f, 0.0f, 0.0f));
  KX_GameObject &c = rig.AddObject("C", MT_Vector3(3.0f, 0.0f, 0.0f));
  const std::vector<KX_GameObject *> others = {&a, &bb, &c};
  ZeroCounts(rig, owner, others);
  Contacts contacts;
  contacts.Set(owner, {Touch{&a}, Touch{&bb}});
  rig.Step();
  EXPECT_EQ(Counts(rig, owner, others),
            "2 0 0 | A: 1 0 0 1 0 1 | B: 1 0 0 1 0 1 | C: 0 0 0 0 0 0 ");

  /* A scene merge: the identities held as objects, the handles renumbered (a new manager here),
   * made again. */
  rig.Tree(0).HoldCollisionIdentitiesAsObjects();
  rig.Tree(0).SetLogicManager(merged.get());
  rig.Tree(0).RenewCollisionIdentities();
  rig.Step();
  EXPECT_EQ(Counts(rig, owner, others),
            "2 2 0 | A: 1 1 0 1 0 1 | B: 1 1 0 1 0 1 | C: 0 0 0 0 0 0 ");

  /* A is removed while touching (its handle ends); here its address stays in the contacts, as a
   * new object made there would: A's old self leaves, unnamed, and the object there enters. */
  merged->NotifyGameObjectsQueuedForBulkRemoval({&a});
  rig.Step();
  EXPECT_EQ(Counts(rig, owner, others),
            "3 3 1 | A: 2 1 0 2 0 2 | B: 1 2 0 1 0 1 | C: 0 0 0 0 0 0 ");
  rig.Tree(0).SetLogicManager(nullptr);
}
