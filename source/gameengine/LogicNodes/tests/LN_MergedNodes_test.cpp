/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Merged Logic Nodes (2026-11-07, NOD_logic_merged.hh): one node with a dropdown stands for
 * several older nodes, and each option compiles exactly as its older node. The editor half (the
 * merged node's sockets are the older node's) is checked in Blender by
 * tests/python/logic_nodes_descriptor_socket_audit.py; this file checks the game half, plus the
 * outputs added on the way (Exists, Time's Delta Factor, Animation Status' Done) and Get Child's
 * empty Parent. */

#include "testing/testing.h"

#include <cstring>
#include <set>
#include <string>

#include "KX_ObjectReferenceValue.h"
#include "LN_NodeRegistry.h"
#include "LN_SemRig.h"
#include "LN_StrictDump.h"

#include "nodes/NOD_logic_merged.hh"

using namespace ln_test;
using namespace ln_test::sem;
using blender::nodes::logic::MergedNode;
using blender::nodes::logic::NodeMetadata;
using blender::nodes::logic::PinMetadata;
using blender::nodes::logic::PinType;

namespace {

/** How a node's inputs are fed and its settings set for one compile. */
struct Shape {
  /** Number, bool, text and generic inputs from live property reads (else their defaults). */
  bool live = false;
  int16_t custom1 = 0;
  int16_t custom2 = 0;
};

/** The ID a node's own picker gets (the sound nodes play a sound by its name). */
blender::ID &PickerID()
{
  static blender::ID id = [] {
    blender::ID made = {};
    SetCString(made.name, "SOMergedSound");
    return made;
  }();
  return id;
}

/** The collection a Collection pin gets (the overlay nodes need one). */
blender::ID &CollectionID()
{
  static blender::ID id = [] {
    blender::ID made = {};
    SetCString(made.name, "GRMergedCollection");
    return made;
  }();
  return id;
}

/** A tree around `idname` in `shape`: its flow from On Logic + Physics Step, its outputs used
 * (data into Print's value, flow into Print's flow). With `merged`, the node is then turned into
 * that merged node holding `option`, with the merged node's extra outputs (what the editor makes:
 * the older node's sockets, then the extras). Returns the program listing, or the compile
 * report when it failed ("FAILED\n" + report). */
std::string CompileAround(const char *idname,
                          const Shape &shape,
                          const MergedNode *merged,
                          const int option,
                          bool *r_compiled = nullptr)
{
  TreeBuilder b;
  b.SetRealSockets(true);
  GenNode &root = b.Add("LogicNativeOnFixedUpdate");
  GenNode &node = b.Add(idname, shape.custom1, shape.custom2);
  node.node.id = &PickerID();
  const NodeMetadata &meta = *node.meta;
  for (const PinMetadata &pin : node.in_pins) {
    if (pin.type == PinType::Execution) {
      b.Link(root, "Out", node, pin.identifier);
      continue;
    }
    if (pin.type == PinType::Collection) {
      b.Asset(node, pin.identifier, &CollectionID());
      continue;
    }
    if (!shape.live) {
      continue;
    }
    const char *source = nullptr;
    const char *property = nullptr;
    switch (pin.type) {
      case PinType::Float:
        source = "LogicNativeGetGamePropertyFloat";
        property = "f";
        break;
      case PinType::Int:
        source = "LogicNativeGetGamePropertyInt";
        property = "i";
        break;
      case PinType::Bool:
      case PinType::Condition:
        source = "LogicNativeGetGamePropertyBool";
        property = "b";
        break;
      case PinType::String:
        source = "LogicNativeGetGamePropertyString";
        property = "s";
        break;
      case PinType::Generic:
        source = "LogicNativeGetGameProperty";
        property = "g";
        break;
      default:
        break;
    }
    if (source == nullptr) {
      continue;
    }
    GenNode &read = b.Add(source);
    b.String(read, "Property", property);
    b.Link(read, "Value", node, pin.identifier);
  }
  /* The older node's own outputs are used; the merged node's extra outputs (Exists) are added
   * but left unlinked: unlinked, they must change nothing (they have their own test). */
  const size_t own_outputs = node.out_pins.size();
  if (merged != nullptr) {
    for (const PinMetadata &pin : merged->extra_outputs) {
      b.AddOutputPin(node, pin);
    }
  }
  for (size_t index = 0; index < own_outputs; index++) {
    const PinMetadata pin = node.out_pins[index];
    GenNode &print = b.Add("LogicNativePrint");
    if (pin.type == PinType::Execution) {
      b.Link(node, pin.identifier, print, "Flow");
    }
    else {
      b.Link(root, "Out", print, "Flow");
      b.Link(node, pin.identifier, print, "Message");
    }
  }
  (void)meta;
  if (merged != nullptr) {
    SetCString(node.node.idname, merged->idname);
    node.node.custom4 = float(option);
  }
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  if (r_compiled != nullptr) {
    *r_compiled = program != nullptr;
  }
  return program != nullptr ? LN_DumpProgramText(*program) : "FAILED\n" + report;
}

}  // namespace

/* The table names nodes that exist, its merged nodes are no older node, and the registry gives a
 * merged node the definition of the older node its option is (the first one for a value out of
 * range). */
TEST(LN_MergedNodes, TheTableNamesRegisteredNodesAndTheRegistryResolvesOptions)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  std::set<std::string> merged_idnames;
  for (const MergedNode &merged : blender::nodes::logic::merged_nodes()) {
    EXPECT_TRUE(merged_idnames.insert(merged.idname).second) << merged.idname;
    EXPECT_EQ(blender::nodes::logic::find_logic_node_metadata(merged.idname), nullptr)
        << merged.idname << " must not be an older node";
    EXPECT_GE(merged.options.size(), 2) << merged.idname;
    std::set<std::string> identifiers;
    for (const int option : merged.options.index_range()) {
      const char *older = merged.options[option].idname;
      EXPECT_TRUE(identifiers.insert(merged.options[option].identifier).second);
      ASSERT_NE(registry.FindNodeDefinition(older), nullptr) << older;
      int found_option = -1;
      EXPECT_EQ(blender::nodes::logic::find_merged_node_of_option(older, &found_option), &merged)
          << older;
      EXPECT_EQ(found_option, option) << older;
      blender::bNode node = {};
      SetCString(node.idname, merged.idname);
      node.custom4 = float(option);
      const LN_NodeDefinition *definition = registry.FindNodeDefinitionForNode(node);
      ASSERT_NE(definition, nullptr);
      EXPECT_EQ(definition->idname, older);
      EXPECT_EQ(definition->kind, registry.FindNodeDefinition(older)->kind);
      EXPECT_EQ(definition->outputs.size(),
                registry.FindNodeDefinition(older)->outputs.size() +
                    size_t(merged.extra_outputs.size()));
    }
    for (const float out_of_range : {-1.0f, float(merged.options.size()), 99.0f}) {
      blender::bNode node = {};
      SetCString(node.idname, merged.idname);
      node.custom4 = out_of_range;
      EXPECT_EQ(registry.FindNodeDefinitionForNode(node)->idname, merged.options[0].idname);
    }
  }
  EXPECT_EQ(merged_idnames.size(), 32u);
  /* An older node keeps its own definition. */
  blender::bNode older = {};
  SetCString(older.idname, "LogicNativeRaycastAll");
  older.custom4 = 0.0f;
  EXPECT_EQ(registry.FindNodeDefinitionForNode(older)->idname, "LogicNativeRaycastAll");
  EXPECT_EQ(blender::nodes::logic::find_merged_node_of_option("LogicNativeClampValue"), nullptr);
  /* The nodes another node does exactly were removed (2026-11-08). */
  for (const char *removed : {"LogicNativeLimitRange", "LogicNativeHasProperty",
                              "LogicNativeRandomFloat", "LogicNativeMakeList"})
  {
    EXPECT_EQ(registry.FindNodeDefinition(removed), nullptr) << removed;
  }
}

/* Every option of every merged node compiles exactly as its older node: the same tree with the
 * older node and with the merged node holding the option gives the same program listing, byte
 * for byte, in several shapes (inputs at their defaults or from live reads, settings 0 to 2). A
 * shape the older node cannot compile in fails the same way. Every option compiles in at least
 * one shape, so no option is compared only as two failures. */
TEST(LN_MergedNodes, EveryOptionCompilesExactlyAsItsOlderNode)
{
  /* custom2 15 is Gamepad Button's left trigger (its Strength output exists for triggers). */
  const Shape shapes[] = {
      {false, 0, 0}, {true, 0, 0}, {true, 1, 0}, {true, 2, 1}, {false, 1, 2}, {true, 0, 15}};
  size_t compared = 0;
  for (const MergedNode &merged : blender::nodes::logic::merged_nodes()) {
    for (const int option : merged.options.index_range()) {
      const char *older = merged.options[option].idname;
      bool compiled_somewhere = false;
      std::string last_report;
      for (const Shape &shape : shapes) {
        bool compiled = false;
        const std::string expected = CompileAround(older, shape, nullptr, 0, &compiled);
        last_report = expected;
        const std::string actual = CompileAround(older, shape, &merged, option);
        EXPECT_EQ(actual, expected)
            << merged.idname << " option " << merged.options[option].identifier << " ("
            << older << ") live=" << shape.live << " custom1=" << shape.custom1
            << " custom2=" << shape.custom2;
        compiled_somewhere |= compiled;
        compared++;
      }
      EXPECT_TRUE(compiled_somewhere) << older << " compiled in no shape:\n" << last_report;
    }
  }
  EXPECT_EQ(compared, 87u * std::size(shapes));
}

/* Get Game Property's Exists says whether the object has the property (what the Has Property node
 * said until 2026-11-08; checked against it then), for the owner (the snapshot's exists flag of
 * the same reference) and for another object (ObjectGamePropertyExists, a live read), in every
 * type, a text property read as a number included, and an Object property holding nothing
 * (2026-11-08: another object's, and the Any option's, called it missing). */
TEST(LN_MergedNodes, ExistsSaysWhetherTheObjectHasTheProperty)
{
  const MergedNode *merged = blender::nodes::logic::find_merged_node(
      "LogicNativeGetGamePropertyValue");
  ASSERT_NE(merged, nullptr);
  /* `other`: the property of that object, else (nullptr) the owner's. */
  const auto program_of = [&](const int option, const char *other) {
    TreeBuilder b;
    GenNode &root = b.Add("LogicNativeOnFixedUpdate");
    const char *older = merged->options[option].idname;
    GenNode &read = b.Add(older);
    b.String(read, "Property", "hp");
    for (const PinMetadata &pin : merged->extra_outputs) {
      b.AddOutputPin(read, pin);
    }
    SetCString(read.node.idname, merged->idname);
    read.node.custom4 = float(option);
    if (other != nullptr) {
      GenNode &find = b.Add("LogicNativeObjectByName");
      b.String(find, "Name", other);
      b.Link(find, "Object", read, "Object");
    }
    GenNode &write_exists = b.Add("LogicNativeSetGamePropertyBool");
    b.String(write_exists, "Property", "exists");
    b.Link(root, "Out", write_exists, "Flow");
    b.Link(read, "Exists", write_exists, "Value");
    std::string report;
    std::shared_ptr<LN_Program> program = b.Compile(&report);
    EXPECT_NE(program, nullptr) << report;
    return program;
  };
  for (const int option : merged->options.index_range()) {
    for (const char *other : {static_cast<const char *>(nullptr), "Obj1", "Obj2", "Obj0"}) {
      const std::shared_ptr<LN_Program> program = program_of(option, other);
      if (program == nullptr) {
        continue;
      }
      SemRig rig(program, kProduction, 4);
      /* Obj1, Obj2 and Obj3 have it (Obj2 an Object property holding nothing, Obj3 text); Obj0
       * does not. */
      rig.SetFloat(rig.Object(1), "hp", 2.5f);
      rig.Object(2).SetProperty("hp",
                                new KX_ObjectReferenceValue(static_cast<KX_GameObject *>(nullptr)));
      rig.SetString(rig.Object(3), "hp", "five");
      for (int step = 0; step < 2; step++) {
        rig.Step();
        for (uint32_t owner = 0; owner < 4; owner++) {
          KX_GameObject &object = rig.Object(owner);
          ASSERT_TRUE(rig.Has(object, "exists"));
          const bool expected = other != nullptr ? std::string(other) != "Obj0" : owner != 0;
          EXPECT_EQ(rig.Float(object, "exists") != 0.0f, expected)
              << merged->options[option].identifier << " other="
              << (other != nullptr ? other : "owner") << " Obj" << owner;
        }
      }
    }
  }
}

namespace {

/** The line of section `family` ("value", "bool", "float", "int"...) at `index` in a program
 * listing ("  [index] Family::Kind fields"). */
std::string ListingLine(const std::string &listing, const std::string &family, const size_t index)
{
  const size_t section = listing.find("\n" + family + " (");
  if (section == std::string::npos) {
    return "";
  }
  const std::string tag = "\n  [" + std::to_string(index) + "] ";
  const size_t next_section = listing.find("\n", listing.find("\n", section + 1) + 1);
  size_t at = listing.find(tag, section);
  (void)next_section;
  if (at == std::string::npos) {
    return "";
  }
  at += tag.size();
  return listing.substr(at, listing.find('\n', at) - at);
}

/** What Print reads: its value expression, then the typed expression that value wraps
 * (Value::FromFloat input0=K -> float [K]), then that one's int input when it has one. Indexes of
 * unused expressions differ between two programs; these lines do not. */
std::string ReadByPrint(const std::string &listing)
{
  const size_t print = listing.find("Op::Print");
  const size_t value_at = listing.find("value_expr_index=", print);
  const size_t value_index = std::stoul(listing.substr(value_at + 17));
  const std::string value = ListingLine(listing, "value", value_index);
  std::string text = value.substr(0, value.find(' '));
  const size_t from = value.find("::From");
  const size_t input = value.find("input0=");
  if (from != std::string::npos && input != std::string::npos) {
    std::string family = value.substr(from + 6, value.find(' ', from) - from - 6);
    for (char &c : family) {
      c = char(std::tolower(c));
    }
    const std::string typed = ListingLine(listing, family, std::stoul(value.substr(input + 7)));
    text += " <- " + typed;
    const size_t int_at = typed.find("int_expr_index=");
    if (int_at != std::string::npos) {
      text += " <- " + ListingLine(listing, "int", std::stoul(typed.substr(int_at + 15)));
    }
  }
  return text;
}

}  // namespace

/* Time's Delta Factor and Animation Status' Done read the expressions the Delta Factor and Action
 * Done nodes read (the same expression on the same inputs: compared with those nodes until they
 * were removed on 2026-11-08); Done reads a linked Layer. */
TEST(LN_MergedNodes, NewOutputsReadTheirExpressions)
{
  const auto listing = [](const char *idname, const char *output, const bool live_layer) {
    TreeBuilder b;
    GenNode &root = b.Add("LogicNativeOnFixedUpdate");
    GenNode &node = b.Add(idname);
    if (live_layer) {
      GenNode &read = b.Add("LogicNativeGetGamePropertyInt");
      b.String(read, "Property", "layer");
      b.Link(read, "Value", node, "Layer");
    }
    GenNode &print = b.Add("LogicNativePrint");
    b.Link(root, "Out", print, "Flow");
    b.Link(node, output, print, "Message");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    EXPECT_NE(program, nullptr) << idname << ": " << report;
    return program != nullptr ? LN_DumpProgramText(*program) : report;
  };
  const std::string delta_factor = ReadByPrint(listing("LogicNativeTime", "Delta Factor", false));
  EXPECT_NE(delta_factor.find("Float::SnapshotDeltaFactor"), std::string::npos) << delta_factor;
  const std::string done = ReadByPrint(listing("LogicNativeAnimationStatus", "Done", false));
  const std::string done_live = ReadByPrint(listing("LogicNativeAnimationStatus", "Done", true));
  EXPECT_NE(done.find("Bool::ActionDone"), std::string::npos) << done;
  EXPECT_NE(done_live.find("Bool::ActionDone"), std::string::npos) << done_live;
  EXPECT_NE(done, done_live) << "a linked Layer is read";
}

/* Get Child with an empty Parent finds the owner's child (it found nothing before 2026-11-07),
 * as Get Child By Name and Get Parent do. */
TEST(LN_MergedNodes, GetChildWithAnEmptyParentIsTheOwnersChild)
{
  TreeBuilder b;
  GenNode &root = b.Add("LogicNativeOnFixedUpdate");
  GenNode &child = b.Add("LogicNativeGetChild");
  b.Int(child, "Index", 0);
  GenNode &none = b.Add("LogicNativeNotNone");
  b.Link(child, "Child Object", none, "Value");
  GenNode &write = b.Add("LogicNativeSetGamePropertyBool");
  b.String(write, "Property", "found");
  b.Link(root, "Out", write, "Flow");
  b.Link(none, "If Not None", write, "Value");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  SemRig rig(program, kProduction, 2);
  rig.AddObject("Kid", MT_Vector3(0.0f, 0.0f, 0.0f));
  rig.Parent(2, 0); /* Obj0 has a child, Obj1 has none. */
  rig.Step();
  EXPECT_EQ(rig.Float(rig.Object(0), "found"), 1.0f);
  EXPECT_EQ(rig.Float(rig.Object(1), "found"), 0.0f);
}
