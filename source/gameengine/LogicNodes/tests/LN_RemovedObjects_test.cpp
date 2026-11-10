/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * A stored object that was removed is None (2026-11-10). Copies share their source's name, and a
 * value whose object was removed used to be looked up by that name: a stored copy became another
 * copy, so a later Remove Object or Modify Property hit an object nobody chose. Now a value made
 * for one object (a tree's reference, or a shared one in a global value or an event) is None once
 * that object is gone: for the object inputs, the None node, a conversion to Bool or text, Print,
 * a game property write and the columns' tree-property conversion alike. A value with only a name
 * (picked in the editor) is still found by its name, and so is a shared reference made before a
 * scene merge renumbered the handles.
 *
 * The rig has no scene: an object is "removed" as the manager tells the trees
 * (InvalidateObjectRef, or the manager's bulk removal) and renamed so that name lookups no longer
 * find it, as they would not find a removed object in the scene's lists. */

#include "testing/testing.h"

#include <deque>
#include <string>
#include <vector>

#include "KX_ObjectReferenceValue.h"
#include "LN_Manager.h"
#include "LN_SemRig.h"
#include "PHY_IPhysicsEnvironment.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

constexpr int16_t kEachObject = 1;

/** One contact pair a step, owner first (as in LN_CollisionEvents_test). */
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

  void Set(KX_GameObject &owner, const std::vector<KX_GameObject *> &touching)
  {
    std::vector<const PHY_CachedCollisionContact *> &list = m_map[&owner];
    list.clear();
    for (KX_GameObject *other : touching) {
      PHY_CachedCollisionContact &contact = m_storage.emplace_back();
      contact.object0 = &owner;
      contact.object1 = other;
      contact.contact_count = 1;
      list.push_back(&contact);
    }
  }

 private:
  std::deque<PHY_CachedCollisionContact> m_storage;
  LN_CollisionContactsForTests m_map;
};

/** The object's removal as the trees hear of it, and out of the scene's name lookups. */
void RemoveFromTrees(SemRig &rig, KX_GameObject &object)
{
  for (size_t index = 0; index < rig.TreeCount(); index++) {
    rig.Tree(index).InvalidateObjectRef(&object);
  }
  object.SetName("Removed");
}

std::string LastLine(const std::string &text)
{
  if (text.empty()) {
    return text;
  }
  const size_t end = text.back() == '\n' ? text.size() - 1 : text.size();
  const size_t start = text.rfind('\n', end - 1);
  return text.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1));
}

/** The Ground remembers the object that arrives (Set Tree Property "target" = Collided Object);
 * every step it reads "target": Modify Property "hits" on it, the None node into "gone", the
 * value as Bool into "alive", as text into "text", Print, and Set Object Property "ref". */
std::shared_ptr<LN_Program> RememberingTree(std::string *report)
{
  TreeBuilder b;
  GenNode &touch = b.Add("LogicNativeOnCollision", kEachObject);
  GenNode &keep = b.Add("LogicNativeSetTreeProperty");
  b.String(keep, "Property", "target");
  b.Link(touch, "Collided Object", keep, "Value");
  b.Link(touch, "On Enter", keep, "Flow");
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &read = b.Add("LogicNativeGetTreeProperty");
  b.String(read, "Property", "target");
  GenNode &hit = b.Add("LogicNativeModifyProperty");
  b.String(hit, "Property", "hits");
  b.Float(hit, "Value", 1.0f);
  b.Link(read, "Value", hit, "Object");
  b.Link(step, "Out", hit, "Flow");
  GenNode &none = b.Add("LogicNativeNone");
  b.Link(read, "Value", none, "Value");
  GenNode &gone = b.Add("LogicNativeSetGamePropertyBool");
  b.String(gone, "Property", "gone");
  b.Link(none, "If None", gone, "Value");
  b.Link(step, "Out", gone, "Flow");
  GenNode &alive = b.Add("LogicNativeSetGamePropertyBool");
  b.String(alive, "Property", "alive");
  b.Link(read, "Value", alive, "Value");
  b.Link(step, "Out", alive, "Flow");
  GenNode &text = b.Add("LogicNativeSetGamePropertyString");
  b.String(text, "Property", "text");
  b.Link(read, "Value", text, "Value");
  b.Link(step, "Out", text, "Flow");
  GenNode &print = b.Add("LogicNativePrint");
  b.Link(read, "Value", print, "Message");
  b.Link(step, "Out", print, "Flow");
  GenNode &store = b.Add("LogicNativeSetGameProperty");
  b.String(store, "Property", "ref");
  b.Link(read, "Value", store, "Value");
  b.Link(step, "Out", store, "Flow");
  return b.Compile(report);
}

void PrepareRemembering(SemRig &rig, KX_GameObject &owner, KX_GameObject &a, KX_GameObject &b)
{
  for (KX_GameObject *object : {&a, &b}) {
    rig.SetFloat(*object, "hits", 0.0f);
  }
  rig.SetBool(owner, "gone", false);
  rig.SetBool(owner, "alive", false);
  rig.SetString(owner, "text", "");
  owner.SetProperty("ref", new KX_ObjectReferenceValue(static_cast<KX_GameObject *>(nullptr)));
}

}  // namespace

/* The stored copy is removed while another copy of the same name lives: everything that reads the
 * stored value sees None, nothing reaches the other copy. In every mode of the rig. */
TEST(LN_RemovedObjects, AStoredCopyThatIsRemovedIsNoneNotAnotherCopy)
{
  std::string report;
  const std::shared_ptr<LN_Program> program = RememberingTree(&report);
  ASSERT_NE(program, nullptr) << report;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    KX_GameObject &owner = rig.Object(0);
    KX_GameObject &a = rig.AddObject("Spawn", MT_Vector3(1.0f, 0.0f, 0.0f));
    KX_GameObject &bb = rig.AddObject("Spawn", MT_Vector3(2.0f, 0.0f, 0.0f));
    PrepareRemembering(rig, owner, a, bb);
    Contacts contacts;
    contacts.Set(owner, {&a});
    rig.Step();
    contacts.Set(owner, {});
    rig.Step();
    const float a_hits = rig.Float(a, "hits");
    EXPECT_GE(a_hits, 1.0f);
    EXPECT_EQ(rig.Float(bb, "hits"), 0.0f);
    EXPECT_EQ(rig.Float(owner, "gone"), 0.0f);
    EXPECT_EQ(rig.Float(owner, "alive"), 1.0f);
    EXPECT_EQ(rig.Text(owner, "text"), "Spawn");
    EXPECT_EQ(LastLine(rig.Printed()), "Spawn");
    EXPECT_EQ(rig.Text(owner, "ref"), "Spawn");

    RemoveFromTrees(rig, a);
    rig.Step();
    EXPECT_EQ(rig.Float(a, "hits"), a_hits);
    EXPECT_EQ(rig.Float(bb, "hits"), 0.0f) << "the stored copy became the other copy";
    EXPECT_EQ(rig.Float(owner, "gone"), 1.0f);
    EXPECT_EQ(rig.Float(owner, "alive"), 0.0f);
    EXPECT_EQ(rig.Text(owner, "text"), "");
    EXPECT_EQ(LastLine(rig.Printed()), "None");
    EXPECT_EQ(rig.Text(owner, "ref"), "None");
  }
}

/* A stored list keeps its other objects; the removed one in it is None (Print, List Get Item). */
TEST(LN_RemovedObjects, ARemovedObjectInAStoredListIsNone)
{
  TreeBuilder b;
  GenNode &touch = b.Add("LogicNativeOnCollision", kEachObject);
  GenNode &keep = b.Add("LogicNativeSetTreeProperty");
  b.String(keep, "Property", "seen");
  b.Link(touch, "Collided Objects", keep, "Value");
  b.Link(touch, "On Enter", keep, "Flow");
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &read = b.Add("LogicNativeGetTreeProperty");
  b.String(read, "Property", "seen");
  GenNode &print = b.Add("LogicNativePrint");
  b.Link(read, "Value", print, "Message");
  b.Link(step, "Out", print, "Flow");
  GenNode &first = b.Add("LogicNativeListGetItem");
  b.Link(read, "Value", first, "List");
  GenNode &none = b.Add("LogicNativeNone");
  b.Link(first, "Value", none, "Value");
  GenNode &gone = b.Add("LogicNativeSetGamePropertyBool");
  b.String(gone, "Property", "first_gone");
  b.Link(none, "If None", gone, "Value");
  b.Link(step, "Out", gone, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    KX_GameObject &owner = rig.Object(0);
    KX_GameObject &a = rig.AddObject("Spawn", MT_Vector3(1.0f, 0.0f, 0.0f));
    KX_GameObject &bb = rig.AddObject("Spawn", MT_Vector3(2.0f, 0.0f, 0.0f));
    rig.SetBool(owner, "first_gone", false);
    Contacts contacts;
    contacts.Set(owner, {&a, &bb});
    rig.Step();
    rig.Step();
    EXPECT_EQ(LastLine(rig.Printed()), "[Spawn, Spawn]");
    EXPECT_EQ(rig.Float(owner, "first_gone"), 0.0f);
    RemoveFromTrees(rig, a);
    contacts.Set(owner, {&bb});
    rig.Step();
    EXPECT_EQ(LastLine(rig.Printed()), "[None, Spawn]");
    EXPECT_EQ(rig.Float(owner, "first_gone"), 1.0f);
  }
}

/* A value that is only a name (an object picked in the editor) is still found by its name: the
 * first object of that name, and the next one once the first is gone. */
TEST(LN_RemovedObjects, ANamedObjectIsStillFoundByItsName)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &hit = b.Add("LogicNativeModifyProperty");
  b.String(hit, "Property", "hits");
  b.Float(hit, "Value", 1.0f);
  b.ObjectNamed(hit, "Object", &diff::TargetBlenderObject());
  b.Link(step, "Out", hit, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    KX_GameObject &first = rig.AddObject("Target", MT_Vector3(1.0f, 0.0f, 0.0f));
    KX_GameObject &second = rig.AddObject("Target", MT_Vector3(2.0f, 0.0f, 0.0f));
    rig.SetFloat(first, "hits", 0.0f);
    rig.SetFloat(second, "hits", 0.0f);
    rig.Step();
    EXPECT_EQ(rig.Float(first, "hits"), 1.0f);
    EXPECT_EQ(rig.Float(second, "hits"), 0.0f);
    RemoveFromTrees(rig, first);
    rig.NoteObjectListChanged();
    rig.Step();
    EXPECT_EQ(rig.Float(first, "hits"), 1.0f);
    EXPECT_EQ(rig.Float(second, "hits"), 1.0f);
  }
}

/* Shared references (a global value): removed is None too. One made before a scene merge
 * renumbered the handles (RenewSharedRefDomain) is not taken as removed: it is found by its
 * name, as before. */
TEST(LN_RemovedObjects, ASharedReferenceIsNoneOnceItsObjectIsRemoved)
{
  TreeBuilder b;
  GenNode &touch = b.Add("LogicNativeOnCollision", kEachObject);
  GenNode &keep = b.Add("LogicNativeSetGlobalProperty");
  b.String(keep, "Category", "ln_removed_objects_test");
  b.String(keep, "Property", "target");
  b.Link(touch, "Collided Object", keep, "Value");
  b.Link(touch, "On Enter", keep, "Flow");
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &read = b.Add("LogicNativeGetGlobalProperty");
  b.String(read, "Category", "ln_removed_objects_test");
  b.String(read, "Property", "target");
  GenNode &hit = b.Add("LogicNativeModifyProperty");
  b.String(hit, "Property", "hits");
  b.Float(hit, "Value", 1.0f);
  b.Link(read, "Value", hit, "Object");
  b.Link(step, "Out", hit, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  auto manager = std::make_unique<LN_Manager>(*unused_scene); /* (outlives the rig's tree) */
  SemRig rig(program, kProduction);
  rig.Tree(0).SetLogicManager(manager.get());
  KX_GameObject &owner = rig.Object(0);
  KX_GameObject &a = rig.AddObject("Spawn", MT_Vector3(1.0f, 0.0f, 0.0f));
  KX_GameObject &bb = rig.AddObject("Spawn", MT_Vector3(2.0f, 0.0f, 0.0f));
  rig.SetFloat(a, "hits", 0.0f);
  rig.SetFloat(bb, "hits", 0.0f);
  Contacts contacts;
  contacts.Set(owner, {&a});
  rig.Step();
  contacts.Set(owner, {});
  rig.Step();
  const float a_hits = rig.Float(a, "hits");
  EXPECT_GE(a_hits, 1.0f);
  EXPECT_EQ(rig.Float(bb, "hits"), 0.0f);

  /* A is removed (the manager invalidates its handle and tells the trees). */
  manager->NotifyGameObjectsQueuedForBulkRemoval({&a});
  a.SetName("Removed");
  rig.Step();
  EXPECT_EQ(rig.Float(a, "hits"), a_hits);
  EXPECT_EQ(rig.Float(bb, "hits"), 0.0f) << "the removed global value became the other copy";

  /* B is stored, then a scene merge renumbers the handles: B lives, found by its name. */
  contacts.Set(owner, {&bb});
  rig.Step();
  contacts.Set(owner, {}); /* (no On Enter after the merge: the value stays the old one) */
  rig.Step();
  const float b_hits = rig.Float(bb, "hits");
  EXPECT_GE(b_hits, 1.0f);
  manager->RenumberSceneHandles();
  rig.Step();
  EXPECT_EQ(rig.Float(bb, "hits"), b_hits + 1.0f) << "a live object taken as removed by the merge";
  rig.Tree(0).SetLogicManager(nullptr);
}

/* A stored object shared after its removal (Set Global Property of it): the global value is None,
 * not a name the next reader finds again. */
TEST(LN_RemovedObjects, SharingARemovedObjectSharesNone)
{
  TreeBuilder b;
  GenNode &touch = b.Add("LogicNativeOnCollision", kEachObject);
  GenNode &keep = b.Add("LogicNativeSetTreeProperty");
  b.String(keep, "Property", "target");
  b.Link(touch, "Collided Object", keep, "Value");
  b.Link(touch, "On Enter", keep, "Flow");
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &read = b.Add("LogicNativeGetTreeProperty");
  b.String(read, "Property", "target");
  GenNode &share = b.Add("LogicNativeSetGlobalProperty");
  b.String(share, "Category", "ln_removed_objects_share");
  b.String(share, "Property", "copy");
  b.Link(read, "Value", share, "Value");
  b.Link(step, "Out", share, "Flow");
  GenNode &shared = b.Add("LogicNativeGetGlobalProperty");
  b.String(shared, "Category", "ln_removed_objects_share");
  b.String(shared, "Property", "copy");
  GenNode &hit = b.Add("LogicNativeModifyProperty");
  b.String(hit, "Property", "hits");
  b.Float(hit, "Value", 1.0f);
  b.Link(shared, "Value", hit, "Object");
  b.Link(share, "Done", hit, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  auto manager = std::make_unique<LN_Manager>(*unused_scene);
  SemRig rig(program, kProduction);
  rig.Tree(0).SetLogicManager(manager.get());
  KX_GameObject &owner = rig.Object(0);
  KX_GameObject &a = rig.AddObject("Spawn", MT_Vector3(1.0f, 0.0f, 0.0f));
  KX_GameObject &bb = rig.AddObject("Spawn", MT_Vector3(2.0f, 0.0f, 0.0f));
  rig.SetFloat(a, "hits", 0.0f);
  rig.SetFloat(bb, "hits", 0.0f);
  Contacts contacts;
  contacts.Set(owner, {&a});
  rig.Step();
  contacts.Set(owner, {});
  rig.Step();
  const float a_hits = rig.Float(a, "hits");
  EXPECT_GE(a_hits, 1.0f);
  /* Only the tree hears of the removal here: its own reference is the removed one it shares. */
  RemoveFromTrees(rig, a);
  rig.Step();
  EXPECT_EQ(rig.Float(a, "hits"), a_hits);
  EXPECT_EQ(rig.Float(bb, "hits"), 0.0f) << "the shared removed object became the other copy";
  rig.Tree(0).SetLogicManager(nullptr);
}

/* The columns' conversion of a tree property to Bool (VTreeConvert) says what the VM says of a
 * removed object: false. Lanes run in columns and as column passes end alike. */
TEST(LN_RemovedObjects, ColumnsReadARemovedStoredObjectAsTheVMDoes)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &read = b.Add("LogicNativeGetTreeProperty");
  b.String(read, "Property", "target");
  GenNode &alive = b.Add("LogicNativeSetGamePropertyBool");
  b.String(alive, "Property", "alive");
  b.Link(read, "Value", alive, "Value");
  b.Link(step, "Out", alive, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  uint32_t target_ref = LN_INVALID_INDEX;
  for (uint32_t index = 0; index < program->GetTreePropertyRefs().size(); index++) {
    if (program->GetTreePropertyRefs()[index].name == "target") {
      target_ref = index;
    }
  }
  ASSERT_NE(target_ref, LN_INVALID_INDEX);
  constexpr uint32_t kOwners = 6;
  std::string results[2];
  for (const bool columns : {false, true}) {
    SCOPED_TRACE(columns ? "columns" : "column passes");
    SemRig rig(program, kProduction, kOwners);
    rig.SetColumns(columns, 0, 1);
    KX_GameObject &victim = rig.AddObject("Spawn", MT_Vector3(1.0f, 0.0f, 0.0f));
    rig.AddObject("Spawn", MT_Vector3(2.0f, 0.0f, 0.0f));
    /* Even owners store the victim, odd ones the other copy. */
    for (uint32_t index = 0; index < kOwners; index++) {
      LN_RuntimeTree &tree = rig.Tree(index);
      KX_GameObject &stored = index % 2 == 0 ? victim : rig.Object(kOwners + 1);
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = "Spawn";
      value.runtime_ref = tree.MakeObjectRef(&stored, "Spawn");
      ASSERT_TRUE(tree.SetTreePropertyValue(target_ref, value));
      rig.SetBool(rig.Object(index), "alive", false);
    }
    rig.Step();
    RemoveFromTrees(rig, victim);
    rig.Step();
    /* (UPBGE_LN_COLUMN_DIRECT=force runs every tree as a column pass: no lanes to count, the
     * results are still compared.) */
    if (columns && LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
      EXPECT_GT(rig.LastColumnLanes(), 0u) << rig.LastColumnRefusal();
    }
    for (uint32_t index = 0; index < kOwners; index++) {
      EXPECT_EQ(rig.Float(rig.Object(index), "alive"), index % 2 == 0 ? 0.0f : 1.0f) << index;
      results[columns ? 1 : 0] += std::to_string(int(rig.Float(rig.Object(index), "alive")));
    }
  }
  EXPECT_EQ(results[0], results[1]);
}
