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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "LN_Columns.h"
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
 * Completed after each of them. A Delay's Done is no part of the route: each run's Flow is a
 * countdown of its own (Queue), its Done one a step after the route (2026-11-17: the first
 * one's Done was lost, its route had already looked, and this test read 1). */
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
    EXPECT_EQ(rig.Float(owner, "after_delay"), 2.0f);
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

/* Each Object events run on the main thread (their state makes object handles): since 2026-11-14
 * the columns take them (their routes' blocks and repeats bridged on the main thread, the rest of
 * the tree wide); the tree batches still refuse them, by name. An Any Object node is refused by
 * neither for being one. */
TEST(LN_CollisionEvents, ColumnsTakeEachObjectEventsBatchesLeaveThemToTheColumnPass)
{
  std::string report;
  const std::shared_ptr<LN_Program> each = CountingTree(kEachObject, &report);
  ASSERT_NE(each, nullptr) << report;
  std::string refusal;
  const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
      *each, LN_Event::OnFixedUpdate, &refusal);
  ASSERT_NE(columns, nullptr) << refusal;
  const std::string text = LN_DescribeColumnProgram(*columns);
  EXPECT_NE(text.find(" bridged=1 "), std::string::npos) << text;
  EXPECT_NE(text.find(" main_thread=1 "), std::string::npos) << text;
  EXPECT_FALSE(LN_AnalyzeTreeBatchSafety(*each).eligible);
  EXPECT_FALSE(LN_ColumnRefusalHint("an On Collision Each Object route").empty());

  const std::shared_ptr<LN_Program> any = CountingTree(kAnyObject, &report);
  ASSERT_NE(any, nullptr) << report;
  refusal.clear();
  LN_CompileColumnProgram(*any, LN_Event::OnFixedUpdate, &refusal);
  EXPECT_EQ(refusal.find("Each Object"), std::string::npos) << refusal;
  EXPECT_EQ(LN_AnalyzeTreeBatchSafety(*any).reason.find("Each Object"), std::string::npos);
}

/* A crowd of owners with Each Object events (2026-11-14): wide runs (the routes' blocks and their
 * repeats through the VM's walk on the main thread, NPC by NPC) end every step as every tree's
 * column pass does, on one thread and with helpers: owners' and touched objects' counts, the run's
 * Contact Count, Point and Normal, objects entering, staying and leaving over six steps (the
 * counting tree of the page; many owners write one touched object's counts). */
TEST(LN_CollisionEvents, EachObjectEventsRunWideAsTheirColumnPasses)
{
  std::string report;
  const std::shared_ptr<LN_Program> program = CountingTree(kEachObject, &report);
  ASSERT_NE(program, nullptr) << report;
  constexpr uint32_t kOwners = 300;
  constexpr int kOthers = 12;
  std::string reference;
  for (const int columns : {0, 1, 4}) {
    SCOPED_TRACE("columns " + std::to_string(columns));
    SemRig rig(program, kProduction, kOwners);
    if (columns != 0) {
      rig.SetColumns(true, uint32_t(columns - 1));
    }
    std::vector<KX_GameObject *> others;
    for (int index = 0; index < kOthers; index++) {
      others.push_back(&rig.AddObject("Other" + std::to_string(index),
                                      MT_Vector3(float(index), 0.0f, 0.0f)));
    }
    for (uint32_t index = 0; index < kOwners; index++) {
      ZeroCounts(rig, rig.Object(index), others);
    }
    Contacts contacts;
    const uint64_t ran_before = LN_ColumnLaneCount();
    std::string text;
    for (int step = 0; step < 6; step++) {
      /* Each owner touches a window of the others that slides with the step (some enter, some
       * stay, some leave), its own number of contacts and points. */
      for (uint32_t index = 0; index < kOwners; index++) {
        std::vector<Touch> touching;
        const int first = int((index * 7 + uint32_t(step) * 2) % kOthers);
        const int count = int(index % 4);
        for (int k = 0; k < count; k++) {
          Touch touch;
          touch.other = others[size_t((first + k) % kOthers)];
          touch.count = 1 + int((index + uint32_t(k)) % 3);
          touch.point = MT_Vector3(float(index % 5), float(k), float(step));
          touch.normal = MT_Vector3(0.0f, 0.0f, k % 2 == 0 ? 1.0f : -1.0f);
          touch.owner_second = (index + uint32_t(k)) % 2 == 1;
          touching.push_back(touch);
        }
        contacts.Set(rig.Object(index), touching);
      }
      rig.Step();
      for (uint32_t index = 0; index < kOwners; index++) {
        text += Counts(rig, rig.Object(index), {}) + "\n";
      }
      text += Counts(rig, rig.Object(0), others) + "\n";
    }
    if (columns == 0) {
      reference = text;
      EXPECT_NE(text.find("| Other3: "), std::string::npos);
    }
    else {
      EXPECT_TRUE(text == reference) << "a wide run's counts differ from the column passes'";
      if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
        EXPECT_GE(LN_ColumnLaneCount() - ran_before, uint64_t(kOwners)) << rig.LastColumnRefusal();
      }
    }
  }
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

namespace {

/** An NPC's brain with a small On Collision part: every step its distance to the Target kept in
 * pf1, a step ahead or a turn (by its pb0), pf0 grown; On Collision's On Enter counts "hits". */
std::shared_ptr<LN_Program> BrainWithCollisions(const int16_t events, std::string *report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf1");
  b.Link(distance, "Distance", keep, "Value");
  b.Link(step, "Out", keep, "Flow");
  GenNode &walking = b.Add("LogicNativeGetGamePropertyBool");
  b.String(walking, "Property", "pb0");
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Link(walking, "Value", branch, "Condition");
  b.Link(keep, "Done", branch, "Flow");
  GenNode &walk = b.Add("LogicNativeApplyMovement");
  b.Vector(walk, "Vector", 0.0f, 0.01f, 0.0f);
  b.Link(branch, "True", walk, "Flow");
  GenNode &turn = b.Add("LogicNativeApplyRotation");
  b.Rotation(turn, "Rotation", 0.0f, 0.0f, 0.02f);
  b.Link(branch, "False", turn, "Flow");
  GenNode &grow = b.Add("LogicNativeModifyProperty");
  b.String(grow, "Property", "pf0");
  b.Float(grow, "Value", 0.1f);
  b.Link(step, "Out", grow, "Flow");
  GenNode &touch = b.Add("LogicNativeOnCollision", events);
  AddOne(b, touch, "On Enter", "hits", false);
  return b.Compile(report);
}

}  // namespace

/* A tool, not a check (--gtest_also_run_disabled_tests): the cost of a step of UPBGE_LN_BENCH_OWNERS
 * (1,000) balls, each touching 0 to 3 of 12 objects in a window that slides every step (some
 * enter, some stay, some leave), each running CountingTree (nothing but collision routes) or
 * BrainWithCollisions (an NPC brain with a small On Collision part), in Any Object and Each Object
 * mode, tree by tree (Each Object's only way before 2026-11-14) and in wide runs (1 and 8
 * threads). The median of UPBGE_LN_BENCH_ROUNDS (5) rounds of UPBGE_LN_BENCH_STEPS (50) timed
 * steps, with the median round's capture / record / flush; setting the contacts is not timed. */
TEST(LN_CollisionEvents, DISABLED_EachObjectEventsCost)
{
  const char *owners_text = std::getenv("UPBGE_LN_BENCH_OWNERS");
  const uint32_t owners = owners_text ? uint32_t(std::max(1, std::atoi(owners_text))) : 1000;
  const char *steps_text = std::getenv("UPBGE_LN_BENCH_STEPS");
  const int steps = steps_text ? std::max(1, std::atoi(steps_text)) : 50;
  const char *rounds_text = std::getenv("UPBGE_LN_BENCH_ROUNDS");
  const int rounds = rounds_text ? std::max(1, std::atoi(rounds_text)) : 5;
  constexpr int kOthers = 12;
  constexpr int kWarm = 5;
  for (const bool brain : {false, true}) {
  for (const int16_t events : {kAnyObject, kEachObject}) {
    double tree_by_tree = 0.0;
    for (const int columns : {0, 1, 8}) {
      std::vector<std::array<double, 4>> times;
      std::string lanes_text;
      for (int round = 0; round < rounds; round++) {
        std::string report;
        const std::shared_ptr<LN_Program> program = brain ? BrainWithCollisions(events, &report) :
                                                            CountingTree(events, &report);
        ASSERT_NE(program, nullptr) << report;
        SemRig rig(program, kProduction, owners);
        rig.SetDump(false);
        rig.SetObserveForces(false);
        if (columns != 0) {
          rig.SetColumns(true, uint32_t(columns - 1));
        }
        std::vector<KX_GameObject *> others;
        for (int index = 0; index < kOthers; index++) {
          others.push_back(&rig.AddObject("Other" + std::to_string(index),
                                          MT_Vector3(float(index), 0.0f, 0.0f)));
        }
        rig.AddObject("Target", MT_Vector3(3.0f, 1.0f, 0.5f));
        for (uint32_t index = 0; index < owners; index++) {
          KX_GameObject &owner = rig.Object(index);
          ZeroCounts(rig, owner, others);
          rig.SetFloat(owner, "hits", 0.0f);
          rig.SetFloat(owner, "pf0", 0.0f);
          rig.SetFloat(owner, "pf1", 0.0f);
          rig.SetBool(owner, "pb0", index % 2 == 0);
          owner.NodeSetWorldPosition(
              MT_Vector3(float(index % 40) * 2.0f, float(index / 40) * 2.0f, 0.0f));
          owner.NodeUpdateGS(0.0);
        }
        Contacts contacts;
        double total = 0.0;
        const uint64_t lanes_before = LN_ColumnLaneCount();
        for (int step = 0; step < kWarm + steps; step++) {
          for (uint32_t index = 0; index < owners; index++) {
            std::vector<Touch> touching;
            const int first = int((index * 7 + uint32_t(step) * 2) % kOthers);
            const int count = int(index % 4);
            for (int k = 0; k < count; k++) {
              Touch touch;
              touch.other = others[size_t((first + k) % kOthers)];
              touch.count = 1 + int((index + uint32_t(k)) % 3);
              touch.point = MT_Vector3(float(index % 5), float(k), float(step));
              touching.push_back(touch);
            }
            contacts.Set(rig.Object(index), touching);
          }
          if (step == kWarm) {
            rig.ResetTimes();
          }
          const auto start = std::chrono::steady_clock::now();
          rig.Step();
          if (step >= kWarm) {
            total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               start)
                         .count();
          }
        }
        const SemRig::PhaseTimes &phases = rig.Times();
        times.push_back({total / steps,
                         phases.capture_ms / steps,
                         phases.record_ms / steps,
                         phases.flush_ms / steps});
        if (round == 0 && columns != 0) {
          lanes_text = std::to_string(int((LN_ColumnLaneCount() - lanes_before) /
                                          uint64_t(kWarm + steps))) +
                       " lanes a step";
          std::string refusal;
          if (LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal) == nullptr) {
            lanes_text += " (refused: " + refusal + ")";
            /* UPBGE_LN_BENCH_DUMP=1: the refused program's text (its blocks and routes). */
            if (std::getenv("UPBGE_LN_BENCH_DUMP") != nullptr) {
              std::printf("%s\n", LN_DumpProgramText(*program).c_str());
            }
          }
        }
      }
      std::sort(times.begin(), times.end());
      const std::array<double, 4> &median = times[times.size() / 2];
      if (columns == 0) {
        tree_by_tree = median[0];
      }
      std::printf(
          "%-14s %-11s %u balls, %-21s %7.3f ms a step (%.2f x tree by tree; capture %.3f, "
          "record %.3f, flush %.3f)%s%s\n",
          brain ? "NPC brain," : "Counting tree,",
          events == kEachObject ? "Each Object" : "Any Object",
          owners,
          columns == 0 ? "tree by tree:" :
          columns == 1 ? "wide, 1 thread:" :
                         "wide, 8 threads:",
          median[0],
          tree_by_tree / std::max(median[0], 1.0e-9),
          median[1],
          median[2],
          median[3],
          lanes_text.empty() ? "" : "; ",
          lanes_text.c_str());
    }
  }
  }
}
