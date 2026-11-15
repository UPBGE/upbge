/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Get / Set Game Property's Vector, Color, Object and List types (2026-11-15). The owner's Vector
 * and Color are typed snapshot reads (Vector / Color::SnapshotGameProperty, kept in the snapshot's
 * vector cells); another object's are the value read converted (FromGenericValue). Both must give
 * what a model written here says, for every kind of property, after every kind of change. The
 * writes keep a property's kind (a vector stays a vector). The columns' kernel for the owner's
 * Vector is checked in LN_Columns_test (FPropAxis). */

#include "testing/testing.h"

#include <functional>
#include <string>
#include <vector>

#include "EXP_ArrayValue.h"
#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"
#include "EXP_Vector2Value.h"
#include "EXP_Vector3Value.h"
#include "EXP_Vector4Value.h"
#include "KX_ObjectReferenceValue.h"
#include "LN_CommandBuffer.h"
#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

const MT_Vector3 kZero(0.0f, 0.0f, 0.0f);
const MT_Vector4 kNoColor(0.0f, 0.0f, 0.0f, 1.0f);

/** A property and what Get Game Property's Vector and Color give for it (the model): a vector's
 * X, Y, Z (a four-number one's first three, a 2D one's with Z 0), a number on all three, a list's
 * first three numbers; a color as it is, a vector with alpha 1, a list's numbers (alpha 1 unless
 * a fourth). Nothing else is a vector or a color: the default then (zero; (0, 0, 0, 1), what
 * every color read falls back to, so that another object's read written by Set Game Property's
 * Color gives the same). */
struct Source {
  const char *what;
  /** Null: the object has no such property. */
  std::function<EXP_Value *()> make;
  MT_Vector3 vector;
  MT_Vector4 color;
};

EXP_Value *NumberList(const std::vector<float> &numbers)
{
  EXP_ArrayValue *list = new EXP_ArrayValue();
  for (const float number : numbers) {
    list->Append(new EXP_FloatValue(number));
  }
  return list;
}

std::vector<Source> Sources()
{
  return {
      {"missing", nullptr, kZero, kNoColor},
      {"Vector",
       [] { return new EXP_Vector3Value(1.5f, -2.0f, 3.25f); },
       MT_Vector3(1.5f, -2.0f, 3.25f),
       MT_Vector4(1.5f, -2.0f, 3.25f, 1.0f)},
      {"Color",
       [] { return new EXP_Vector4Value(0.1f, 0.2f, 0.3f, 0.4f); },
       MT_Vector3(0.1f, 0.2f, 0.3f),
       MT_Vector4(0.1f, 0.2f, 0.3f, 0.4f)},
      {"2D vector",
       [] { return new EXP_Vector2Value(5.0f, 6.0f); },
       MT_Vector3(5.0f, 6.0f, 0.0f),
       MT_Vector4(5.0f, 6.0f, 0.0f, 1.0f)},
      {"Int", [] { return new EXP_IntValue(7); }, MT_Vector3(7.0f, 7.0f, 7.0f), kNoColor},
      {"Float",
       [] { return new EXP_FloatValue(-2.5f); },
       MT_Vector3(-2.5f, -2.5f, -2.5f),
       kNoColor},
      {"Bool", [] { return new EXP_BoolValue(true); }, kZero, kNoColor},
      {"number text", [] { return new EXP_StringValue("12.5", ""); }, kZero, kNoColor},
      {"List of 3",
       [] { return NumberList({1.0f, 2.0f, 3.0f}); },
       MT_Vector3(1.0f, 2.0f, 3.0f),
       MT_Vector4(1.0f, 2.0f, 3.0f, 1.0f)},
      {"List of 4",
       [] { return NumberList({4.0f, 3.0f, 2.0f, 0.5f}); },
       MT_Vector3(4.0f, 3.0f, 2.0f),
       MT_Vector4(4.0f, 3.0f, 2.0f, 0.5f)},
      {"Object", [] { return new KX_ObjectReferenceValue(static_cast<KX_GameObject *>(nullptr)); },
       kZero,
       kNoColor},
  };
}

void Place(KX_GameObject &object, const char *name, const Source &source)
{
  object.RemoveProperty(name);
  if (source.make) {
    EXP_Value *value = source.make();
    object.SetProperty(name, value);
    value->Release();
  }
}

/** On every step: the owner's "p" read as a Vector and a Color, by the owner (empty Object) into
 * own_v / own_c and through Get Owner (another object's read) into other_v / other_c. */
std::shared_ptr<LN_Program> ReadEveryWay()
{
  TreeBuilder b;
  GenNode &root = b.Add("LogicNativeOnFixedUpdate");
  GenNode &owner = b.Add("LogicNativeGetOwner");
  GenNode *flow = &root;
  const char *out = "Out";
  const auto write = [&](const char *setter, const char *name, GenNode &read) {
    GenNode &set = b.Add(setter);
    b.String(set, "Property", name);
    b.Link(*flow, out, set, "Flow");
    b.Link(read, "Value", set, "Value");
    flow = &set;
    out = "Done";
  };
  for (const bool other : {false, true}) {
    GenNode &vector = b.Add("LogicNativeGetGamePropertyVector");
    b.String(vector, "Property", "p");
    GenNode &color = b.Add("LogicNativeGetGamePropertyColor");
    b.String(color, "Property", "p");
    if (other) {
      b.Link(owner, "Owner Object", vector, "Object");
      b.Link(owner, "Owner Object", color, "Object");
    }
    write("LogicNativeSetGamePropertyVector", other ? "other_v" : "own_v", vector);
    write("LogicNativeSetGamePropertyColor", other ? "other_c" : "own_c", color);
  }
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

void ExpectVector(KX_GameObject &object,
                  const char *name,
                  const MT_Vector3 &expected,
                  const std::string &where)
{
  EXP_Value *value = object.GetProperty(name);
  ASSERT_NE(value, nullptr) << where << " " << name;
  ASSERT_EQ(value->GetValueType(), VALUE_VECTOR3_TYPE) << where << " " << name;
  const MT_Vector3 &got = static_cast<EXP_Vector3Value *>(value)->GetVector3();
  EXPECT_TRUE(got == expected) << where << " " << name << ": " << value->GetText();
}

void ExpectColor(KX_GameObject &object,
                 const char *name,
                 const MT_Vector4 &expected,
                 const std::string &where)
{
  EXP_Value *value = object.GetProperty(name);
  ASSERT_NE(value, nullptr) << where << " " << name;
  ASSERT_EQ(value->GetValueType(), VALUE_VECTOR4_TYPE) << where << " " << name;
  const MT_Vector4 &got = static_cast<EXP_Vector4Value *>(value)->GetVector4();
  EXPECT_TRUE(got == expected) << where << " " << name << ": " << value->GetText();
}

/** A Logic Nodes write of `value` into `name` (in place where the kind allows: the property map
 * keeps its stamp, so a kept snapshot cell must read it again). */
void WriteInPlace(KX_GameObject &object, const char *name, const LN_Value &value)
{
  std::string reason;
  EXPECT_TRUE(LN_CommandBuffer::ApplyGamePropertyValue(&object, name, value, nullptr, reason))
      << reason;
}

}  // namespace

/* Every kind of property read as a Vector and as a Color, by the owner and as another object's,
 * in every rig mode and typed snapshot storage; then each owner's property replaced by the next
 * kind (a new stamp), then numbers and vectors changed in place (the same stamp). */
TEST(LN_TypedGameProperty, VectorAndColorReadEveryKindAsTheModelSays)
{
  const std::shared_ptr<LN_Program> program = ReadEveryWay();
  ASSERT_NE(program, nullptr);
  const std::vector<Source> sources = Sources();
  const size_t count = sources.size();
  for (const int storage : {0, 1, 2}) {
    LN_Snapshot::SetTypedSnapshotModeForTests(storage);
    const uint32_t differences = LN_Snapshot::TypedSnapshotVerifyDifferencesForTests();
    for (const SemMode &mode : AllModes()) {
      SemRig rig(program, mode, count);
      const auto check = [&](const std::vector<const Source *> &now, const std::string &when) {
        rig.Step();
        for (size_t owner = 0; owner < count; owner++) {
          const std::string where = std::string(mode.name) + " storage " +
                                    std::to_string(storage) + " " + when + " Obj" +
                                    std::to_string(owner) + " (" + now[owner]->what + ")";
          KX_GameObject &object = rig.Object(owner);
          ExpectVector(object, "own_v", now[owner]->vector, where);
          ExpectVector(object, "other_v", now[owner]->vector, where);
          ExpectColor(object, "own_c", now[owner]->color, where);
          ExpectColor(object, "other_c", now[owner]->color, where);
        }
      };
      std::vector<const Source *> now(count);
      for (size_t owner = 0; owner < count; owner++) {
        now[owner] = &sources[owner];
        Place(rig.Object(owner), "p", *now[owner]);
      }
      check(now, "placed");
      for (size_t owner = 0; owner < count; owner++) {
        now[owner] = &sources[(owner + 1) % count];
        Place(rig.Object(owner), "p", *now[owner]);
      }
      check(now, "replaced");
      /* In place: a vector, a color, numbers (Obj0 holds the Vector now, Obj1 the Color, Obj3 the
       * Int, Obj4 the Float). */
      Source vector{"Vector in place", nullptr, MT_Vector3(-1.0f, 0.5f, 9.0f),
                    MT_Vector4(-1.0f, 0.5f, 9.0f, 1.0f)};
      LN_Value value;
      value.exists = true;
      value.type = LN_ValueType::Vector;
      value.vector_value = vector.vector;
      WriteInPlace(rig.Object(0), "p", value);
      now[0] = &vector;
      Source color{"Color in place", nullptr, MT_Vector3(0.75f, 0.5f, 0.25f),
                   MT_Vector4(0.75f, 0.5f, 0.25f, 0.125f)};
      value.type = LN_ValueType::Color;
      value.color_value = color.color;
      WriteInPlace(rig.Object(1), "p", value);
      now[1] = &color;
      Source number{"number in place", nullptr, MT_Vector3(-4.0f, -4.0f, -4.0f), kNoColor};
      value.type = LN_ValueType::Float;
      value.float_value = -4.0f;
      WriteInPlace(rig.Object(3), "p", value);
      WriteInPlace(rig.Object(4), "p", value);
      now[3] = &number;
      now[4] = &number;
      check(now, "in place");
    }
    EXPECT_EQ(LN_Snapshot::TypedSnapshotVerifyDifferencesForTests(), differences)
        << "storage " << storage;
  }
  LN_Snapshot::SetTypedSnapshotModeForTests(-1);
}

/* The typed writes keep a property's kind: a Vector into a vector (or a new property), a Color
 * into a color or 4D vector; a Vector into a color or a Color into a vector is refused and leaves
 * the property as it was; a number property takes the length. (A Vector output linked into Set's
 * Color is refused by the compiler: "Data link type mismatch".) */
TEST(LN_TypedGameProperty, VectorAndColorWritesKeepThePropertysKind)
{
  TreeBuilder b;
  GenNode &root = b.Add("LogicNativeOnFixedUpdate");
  GenNode *flow = &root;
  const char *out = "Out";
  const auto then = [&](GenNode &set) {
    b.Link(*flow, out, set, "Flow");
    flow = &set;
    out = "Done";
  };
  for (const char *name : {"v_new", "v_vector", "v_color", "v_float"}) {
    GenNode &set = b.Add("LogicNativeSetGamePropertyVector");
    b.String(set, "Property", name);
    b.Vector(set, "Value", 1.0f, 2.0f, 3.0f);
    then(set);
  }
  for (const char *name : {"c_new", "c_color", "c_vector", "c_float"}) {
    GenNode &set = b.Add("LogicNativeSetGamePropertyColor");
    b.String(set, "Property", name);
    b.Color(set, "Value", 0.5f, 0.25f, 0.125f, 0.75f);
    then(set);
  }
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;

  for (const SemMode &mode : AllModes()) {
    SemRig rig(program, mode, 1);
    KX_GameObject &object = rig.Object(0);
    const auto own = [&](const char *name, EXP_Value *value) {
      object.SetProperty(name, value);
      value->Release();
    };
    own("v_vector", new EXP_Vector3Value(9.0f, 9.0f, 9.0f));
    own("v_color", new EXP_Vector4Value(9.0f, 9.0f, 9.0f, 9.0f));
    rig.SetFloat(object, "v_float", 9.0f);
    own("c_color", new EXP_Vector4Value(9.0f, 9.0f, 9.0f, 9.0f));
    own("c_vector", new EXP_Vector3Value(9.0f, 9.0f, 9.0f));
    rig.SetFloat(object, "c_float", 9.0f);
    rig.Step();
    const std::string where = mode.name;
    ExpectVector(object, "v_new", MT_Vector3(1.0f, 2.0f, 3.0f), where);
    ExpectVector(object, "v_vector", MT_Vector3(1.0f, 2.0f, 3.0f), where);
    ExpectColor(object, "v_color", MT_Vector4(9.0f, 9.0f, 9.0f, 9.0f), where);
    /* A number property stays a number and takes the value's number (Set Game Property's Any
     * the same, as EXP's GetNumber): a vector's or color's length. */
    EXPECT_EQ(rig.Float(object, "v_float"), MT_Vector3(1.0f, 2.0f, 3.0f).length()) << where;
    ExpectColor(object, "c_new", MT_Vector4(0.5f, 0.25f, 0.125f, 0.75f), where);
    ExpectColor(object, "c_color", MT_Vector4(0.5f, 0.25f, 0.125f, 0.75f), where);
    ExpectVector(object, "c_vector", MT_Vector3(9.0f, 9.0f, 9.0f), where);
    EXPECT_EQ(rig.Float(object, "c_float"), MT_Vector4(0.5f, 0.25f, 0.125f, 0.75f).length())
        << where;
  }
}

/* Get Game Property's Object gives the object an Object property holds (and drives a node's
 * Object input), a text naming one too (the input finds it by name); Set Game Property's Object
 * writes the linked object, and an empty Value clears the property (or makes an empty Object
 * property): never the owner. A text read is written as the text (a write takes what its Value
 * gives; only an Object input finds an object by name). */
TEST(LN_TypedGameProperty, ObjectReadsAndWritesTheObjectAPropertyHolds)
{
  TreeBuilder b;
  GenNode &root = b.Add("LogicNativeOnFixedUpdate");
  GenNode *flow = &root;
  const char *out = "Out";
  const auto then = [&](GenNode &set) {
    b.Link(*flow, out, set, "Flow");
    flow = &set;
    out = "Done";
  };
  /* The held object gets hit = 1. */
  GenNode &target = b.Add("LogicNativeGetGamePropertyObject");
  b.String(target, "Property", "target");
  GenNode &hit = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(hit, "Property", "hit");
  b.Float(hit, "Value", 1.0f);
  b.Link(target, "Value", hit, "Object");
  then(hit);
  /* A copy of the held object, the owner written, and two cleared. */
  GenNode &copy = b.Add("LogicNativeSetGamePropertyObject");
  b.String(copy, "Property", "copy");
  b.Link(target, "Value", copy, "Value");
  then(copy);
  GenNode &owner = b.Add("LogicNativeGetOwner");
  GenNode &me = b.Add("LogicNativeSetGamePropertyObject");
  b.String(me, "Property", "me");
  b.Link(owner, "Owner Object", me, "Value");
  then(me);
  for (const char *name : {"cleared", "empty_new"}) {
    GenNode &clear = b.Add("LogicNativeSetGamePropertyObject");
    b.String(clear, "Property", name);
    then(clear);
  }
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;

  for (const SemMode &mode : AllModes()) {
    SemRig rig(program, mode, 3);
    KX_GameObject &holder = rig.Object(0);
    KX_GameObject &named = rig.Object(1);
    KX_GameObject &held = rig.Object(2);
    const auto hold = [](KX_GameObject &object, const char *name, KX_GameObject *held_object) {
      EXP_Value *value = new KX_ObjectReferenceValue(held_object);
      object.SetProperty(name, value);
      value->Release();
    };
    hold(holder, "target", &held);
    hold(holder, "cleared", &held);
    /* Obj1 names Obj2 by text. */
    rig.SetString(named, "target", "Obj2");
    hold(named, "cleared", &held);
    rig.Step();
    const std::string where = mode.name;
    EXPECT_EQ(rig.Float(held, "hit", -1.0f), 1.0f) << where;
    EXPECT_FALSE(rig.Has(holder, "hit")) << where;
    EXPECT_FALSE(rig.Has(named, "hit")) << where;
    EXP_Value *copied = holder.GetProperty("copy");
    ASSERT_NE(copied, nullptr) << where;
    ASSERT_EQ(copied->GetValueType(), VALUE_OBJECT_TYPE) << where;
    EXPECT_EQ(static_cast<KX_ObjectReferenceValue *>(copied)->GetObject(), &held) << where;
    EXPECT_EQ(rig.Text(named, "copy"), "Obj2") << where;
    for (KX_GameObject *object : {&holder, &named}) {
      EXP_Value *me_value = object->GetProperty("me");
      ASSERT_NE(me_value, nullptr) << where;
      EXPECT_EQ(static_cast<KX_ObjectReferenceValue *>(me_value)->GetObject(), object) << where;
      for (const char *name : {"cleared", "empty_new"}) {
        EXP_Value *value = object->GetProperty(name);
        ASSERT_NE(value, nullptr) << where << " " << name;
        ASSERT_EQ(value->GetValueType(), VALUE_OBJECT_TYPE) << where << " " << name;
        EXPECT_EQ(static_cast<KX_ObjectReferenceValue *>(value)->GetObject(), nullptr)
            << where << " " << name;
      }
    }
    /* The holder with no target: hit nowhere, the copy cleared. */
    holder.RemoveProperty("target");
    named.RemoveProperty("target");
    held.RemoveProperty("hit");
    rig.Step();
    EXPECT_FALSE(rig.Has(held, "hit")) << where;
    EXPECT_EQ(static_cast<KX_ObjectReferenceValue *>(holder.GetProperty("copy"))->GetObject(),
              nullptr)
        << where;
  }
}

/* Get Game Property's List gives the list a List property holds (a Table's rows too) and Set
 * Game Property's List writes it: a copy, item for item; a list never goes into a number. */
TEST(LN_TypedGameProperty, ListCopiesAListProperty)
{
  TreeBuilder b;
  GenNode &root = b.Add("LogicNativeOnFixedUpdate");
  GenNode &read = b.Add("LogicNativeGetGamePropertyList");
  b.String(read, "Property", "bag");
  GenNode &copy = b.Add("LogicNativeSetGamePropertyList");
  b.String(copy, "Property", "copy");
  b.Link(root, "Out", copy, "Flow");
  b.Link(read, "Value", copy, "Value");
  GenNode &into_number = b.Add("LogicNativeSetGamePropertyList");
  b.String(into_number, "Property", "number");
  b.Link(copy, "Done", into_number, "Flow");
  b.Link(read, "Value", into_number, "Value");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  for (const SemMode &mode : AllModes()) {
    SemRig rig(program, mode, 1);
    KX_GameObject &object = rig.Object(0);
    EXP_ArrayValue *bag = new EXP_ArrayValue();
    bag->Append(new EXP_StringValue("sword", ""));
    bag->Append(new EXP_IntValue(3));
    bag->Append(new EXP_Vector3Value(1.0f, 2.0f, 3.0f));
    object.SetProperty("bag", bag);
    bag->Release();
    rig.SetFloat(object, "number", 5.0f);
    rig.Step();
    const std::string where = mode.name;
    EXP_Value *copied = object.GetProperty("copy");
    ASSERT_NE(copied, nullptr) << where;
    ASSERT_EQ(copied->GetValueType(), VALUE_ARRAY_TYPE) << where;
    EXPECT_NE(copied, object.GetProperty("bag")) << where;
    EXPECT_EQ(copied->GetText(), object.GetProperty("bag")->GetText()) << where;
    EXPECT_EQ(rig.Float(object, "number"), 5.0f) << where;
  }
}
