/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The transform apply of the command buffer on a real scene graph (lean runtime plan, phase A8):
 * the function the flush and a read's early apply share applies the eight transform kinds with the
 * node's immediate-update guard (the node is not put in the scene's update list and taken out
 * again) or without it (UPBGE_LN_LEAN_FLUSH=0). Both must end in the very same scene graph: the
 * local and world poses of the object and its child, the world stamps, the modified flags and the
 * update list. A real KX_GameObject on a real SG_Node with a scheduling callback, a normal parent
 * relation for the child, and a test update list as the scene's. */

#include "testing/testing.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <string>

#include "KX_GameObject.h"
#include "KX_NodeRelationships.h"
#include "LN_CommandBuffer.h"
#include "LN_Rotation.h"
#include "LN_TestTrees.h"
#include "SG_Node.h"
#include "SG_QList.h"

using namespace ln_test;

namespace {

using CommandType = LN_CommandBuffer::CommandType;

bool ScheduleIntoTestList(SG_Node *node, void *, void *list)
{
  return node->Schedule(*static_cast<SG_QList *>(list));
}

bool RescheduleIntoTestList(SG_Node *node, void *, void *list)
{
  return node->Reschedule(*static_cast<SG_QList *>(list));
}

/** A root object with a child on its scene graph, scheduling into `list` like the scene does. */
struct SceneGraphPair {
  SceneGraphPair()
      : callbacks(nullptr, nullptr, nullptr, ScheduleIntoTestList, RescheduleIntoTestList),
        root_node(&root, &list, callbacks),
        child_node(&child, &list, callbacks)
  {
    root_node.SetParentRelation(new RootRelation());
    child_node.SetParentRelation(new KX_NormalParentRelation());
    root.SetSGNode(&root_node);
    child.SetSGNode(&child_node);
    root_node.AddChild(&child_node);
    root_node.SetLocalPosition(MT_Vector3(1.0f, 2.0f, 3.0f));
    root_node.SetLocalScale(MT_Vector3(2.0f, 2.0f, 2.0f));
    child_node.SetLocalPosition(MT_Vector3(0.5f, 0.0f, 1.0f));
    root.NodeUpdateGS(0.0);
    /* Start from a settled graph: nothing scheduled, nothing modified. */
    root_node.Delink();
    child_node.Delink();
  }
  ~SceneGraphPair()
  {
    root.SetSGNode(nullptr);
    child.SetSGNode(nullptr);
  }

  SG_QList list;
  SG_Callbacks callbacks;
  KX_GameObject root;
  KX_GameObject child;
  SG_Node root_node;
  SG_Node child_node;
};

std::string Bits(const float value)
{
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  char text[16];
  std::snprintf(text, sizeof(text), "%08x", bits);
  return text;
}

std::string Describe(const MT_Vector3 &vector)
{
  return Bits(vector.x()) + "," + Bits(vector.y()) + "," + Bits(vector.z());
}

std::string Describe(const MT_Matrix3x3 &matrix)
{
  std::string text;
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      text += Bits(matrix[row][column]) + (column == 2 ? "/" : ",");
    }
  }
  return text;
}

/** Everything about one node a guard might change. */
std::string Describe(SG_Node &node)
{
  return "lp=" + Describe(node.GetLocalPosition()) + " lr=" + Describe(node.GetLocalOrientation()) +
         " ls=" + Describe(node.GetLocalScale()) + " wp=" + Describe(node.GetWorldPosition()) +
         " wr=" + Describe(node.GetWorldOrientation()) +
         " ws=" + Describe(node.GetWorldScaling()) +
         " stamp=" + std::to_string(node.GetWorldStamp()) +
         " modified=" + std::to_string(node.IsModified()) +
         " scheduled=" + std::to_string(!node.Empty()) +
         " guard=" + std::to_string(node.IsImmediateUpdate()) + ";";
}

std::string Describe(SceneGraphPair &pair)
{
  return "root " + Describe(pair.root_node) + " child " + Describe(pair.child_node) + " list=" +
         std::to_string(!pair.list.Empty());
}

struct ModeGuard {
  ~ModeGuard()
  {
    LN_CommandBuffer::SetLeanFlushModeForTests(-1);
    SG_Node::SetHistoryFollowsUpdates(false);
  }
};

const CommandType kTransformKinds[] = {CommandType::SetWorldPosition,
                                       CommandType::SetLocalPosition,
                                       CommandType::SetWorldOrientation,
                                       CommandType::SetLocalOrientation,
                                       CommandType::SetWorldScale,
                                       CommandType::SetLocalScale,
                                       CommandType::ApplyMovement,
                                       CommandType::ApplyRotation};

/** One apply of a kind with values that move every part (a turn about all three axes). */
void ApplyKind(const CommandType type, KX_GameObject &object, const int round, const bool local)
{
  const float amount = 0.25f + 0.5f * float(round);
  const MT_Vector3 vector(amount, -2.0f * amount, 0.5f + amount);
  const LN_Rotation rotation = LN_Rotation::FromEulerXYZ(MT_Vector3(0.3f * amount, 0.2f, -0.4f));
  LN_CommandBuffer::ApplyTransformForTests(type, &object, vector, rotation, local);
}

}  // namespace

/* The eight kinds on the root and on the child, with and without the step history, from a graph
 * with nothing scheduled and from one whose node was scheduled by an earlier change, twice in a row
 * (the second apply meets the history the first made): the graph ends the same with the guard and
 * without it, down to the bit. */
TEST(LN_TransformApply, TheGuardLeavesTheSceneGraphExactlyAsTheUpdateListWayDoes)
{
  ModeGuard restore;
  size_t compared = 0;
  for (const bool history : {false, true}) {
    SG_Node::SetHistoryFollowsUpdates(history);
    for (const CommandType type : kTransformKinds) {
      for (const bool on_child : {false, true}) {
        for (const bool scheduled_before : {false, true}) {
          for (const bool local : {false, true}) {
            std::string results[2];
            for (const int mode : {0, 1}) {
              LN_CommandBuffer::SetLeanFlushModeForTests(mode);
              SceneGraphPair pair;
              KX_GameObject &object = on_child ? pair.child : pair.root;
              if (scheduled_before) {
                /* An earlier change that nothing updated yet: the node is in the list. */
                (on_child ? pair.child_node : pair.root_node).SetModified();
              }
              ApplyKind(type, object, 0, local);
              ApplyKind(type, object, 1, local);
              results[mode] = Describe(pair);
            }
            EXPECT_EQ(results[0], results[1])
                << "kind " << int(type) << (on_child ? " child" : " root")
                << (scheduled_before ? " scheduled before" : "") << (local ? " local" : "")
                << (history ? " with history" : "");
            compared++;
          }
        }
      }
    }
  }
  EXPECT_EQ(compared, 2u * 8u * 2u * 2u * 2u);
}

/* The guard is one node's: while a node applies a change, its child (or any other node) that is
 * changed is scheduled as always, and nothing is left on afterwards. */
TEST(LN_TransformApply, TheGuardBelongsToOneNodeAndIsGoneAfterwards)
{
  ModeGuard restore;
  LN_CommandBuffer::SetLeanFlushModeForTests(1);
  SceneGraphPair pair;
  ApplyKind(CommandType::SetLocalPosition, pair.root, 0, false);
  EXPECT_FALSE(pair.root_node.IsImmediateUpdate());
  EXPECT_FALSE(pair.child_node.IsImmediateUpdate());
  /* A change of another node made after the apply is scheduled as usual. */
  pair.child_node.SetModified();
  EXPECT_FALSE(pair.child_node.Empty());
  /* While one node holds the guard, another node's change still schedules it. */
  {
    SG_ImmediateUpdateScope scope(&pair.root_node);
    EXPECT_TRUE(pair.root_node.IsImmediateUpdate());
    EXPECT_FALSE(pair.child_node.IsImmediateUpdate());
    pair.child_node.Delink();
    pair.child_node.SetModified();
    EXPECT_FALSE(pair.child_node.Empty());
    pair.root_node.Delink();
    pair.root_node.SetModified();
    EXPECT_TRUE(pair.root_node.Empty()); /* the guarded node was not put in the list */
  }
  EXPECT_FALSE(pair.root_node.IsImmediateUpdate());
}

/* With the guard a node that was not scheduled is never in the update list, not even for a moment
 * (the point of the guard); without it the list is entered and left in the same call. */
TEST(LN_TransformApply, AGuardedChangeNeverEntersTheUpdateList)
{
  ModeGuard restore;
  LN_CommandBuffer::SetLeanFlushModeForTests(1);
  SceneGraphPair pair;
  struct Watch {
    static bool Schedule(SG_Node *node, void *, void *counter)
    {
      ++*static_cast<int *>(counter);
      return false;
    }
  };
  int scheduled = 0;
  SG_Callbacks counting(nullptr, nullptr, nullptr, Watch::Schedule, nullptr);
  SG_Node node(nullptr, &scheduled, counting);
  node.SetParentRelation(new RootRelation()); /* (this schedules once) */
  scheduled = 0;
  {
    SG_ImmediateUpdateScope scope(&node);
    node.SetLocalPosition(MT_Vector3(1.0f, 0.0f, 0.0f));
  }
  EXPECT_EQ(scheduled, 0);
  node.SetLocalPosition(MT_Vector3(2.0f, 0.0f, 0.0f));
  EXPECT_EQ(scheduled, 1);
}

/* A micro benchmark of the guard (DISABLED_: run with --gtest_also_run_disabled_tests): 10,000
 * root objects, each moved and turned (Apply Movement, Apply Rotation), by the update list way and
 * with the guard; best of 15. */
TEST(LN_TransformApply, DISABLED_GuardBench)
{
  ModeGuard restore;
  constexpr int count = 10000;
  SG_QList list;
  SG_Callbacks callbacks(nullptr, nullptr, nullptr, ScheduleIntoTestList, RescheduleIntoTestList);
  std::vector<std::unique_ptr<KX_GameObject>> objects;
  std::vector<std::unique_ptr<SG_Node>> nodes;
  for (int index = 0; index < count; index++) {
    objects.push_back(std::make_unique<KX_GameObject>());
    nodes.push_back(std::make_unique<SG_Node>(objects.back().get(), &list, callbacks));
    nodes.back()->SetParentRelation(new RootRelation());
    objects.back()->SetSGNode(nodes.back().get());
    nodes.back()->Delink();
  }
  const MT_Vector3 move(0.0f, 0.002f, 0.0f);
  const LN_Rotation turn = LN_Rotation::FromEulerXYZ(MT_Vector3(0.0f, 0.0f, 0.002f));
  for (const int mode : {0, 1}) {
    LN_CommandBuffer::SetLeanFlushModeForTests(mode);
    double best = 1.0e9;
    for (int round = 0; round < 15; round++) {
      const auto start = std::chrono::steady_clock::now();
      for (int index = 0; index < count; index++) {
        LN_CommandBuffer::ApplyTransformForTests(
            CommandType::ApplyMovement, objects[index].get(), move, turn, true);
        LN_CommandBuffer::ApplyTransformForTests(
            CommandType::ApplyRotation, objects[index].get(), move, turn, true);
      }
      best = std::min(best,
                      std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count());
    }
    std::printf("GUARD_BENCH mode %d: %.3f ms for %d x (move + turn)\n", mode, best, count);
  }
  for (std::unique_ptr<KX_GameObject> &object : objects) {
    object->SetSGNode(nullptr);
  }
}

/* Pending move marks (plan phase A33): with marks on or off, and in verify mode, a buffer says an
 * object has a pending move exactly when its index says so: a moved object, a child of a moved
 * parent through a hierarchy read, an object moved in a chunk joined to it, none after a read
 * applied the moves or after a flush; an untouched object reads none without the index. Verify mode
 * finds a lost mark (a pending move without its buffer's mark), which must never happen. */
TEST(LN_TransformApply, PendingMoveMarksAnswerAsTheIndex)
{
  struct VerifyScope {
    VerifyScope()
    {
      LN_CommandBuffer::SetPendingMarkModeForTests(2);
    }
    ~VerifyScope()
    {
      LN_CommandBuffer::SetPendingMarkModeForTests(-1);
    }
  } verify;
  const uint64_t differences_before = LN_CommandBuffer::PendingMarkVerifyDifferencesForTests();
  for (const bool marks : {false, true}) {
    SCOPED_TRACE(marks ? "marks" : "no marks");
    SceneGraphPair pair;
    BatchObject other(MT_Vector3(4.0f, 0.0f, 0.0f));
    BatchObject joined(MT_Vector3(-4.0f, 0.0f, 0.0f));
    LN_CommandBuffer buffer;
    buffer.SetMarksPendingMoves(marks);
    buffer.BeginRecording();
    EXPECT_FALSE(buffer.HasPendingTransforms(&pair.root, false));
    buffer.AppendApplyMovement(&pair.root, MT_Vector3(1.0f, 0.0f, 0.0f), false, 1, 0);
    EXPECT_TRUE(buffer.HasPendingTransforms(&pair.root, false));
    EXPECT_TRUE(buffer.HasPendingTransforms(&pair.child, true)) << "the parent moved";
    EXPECT_FALSE(buffer.HasPendingTransforms(&pair.child, false));
    EXPECT_FALSE(buffer.HasPendingTransforms(&other.object, true));
    EXPECT_FALSE(buffer.HasIndexedPendingTransforms(&other.object, true));
    /* A hierarchy read applies the parent's move: nothing is pending after it. */
    buffer.FlushPendingObjectHierarchyTransformCommands(&pair.child);
    EXPECT_FALSE(buffer.HasPendingTransforms(&pair.root, false));
    EXPECT_FALSE(buffer.HasPendingTransforms(&pair.child, true));
    /* A chunk's moves are the buffer's once joined. */
    LN_CommandBuffer chunk;
    chunk.BeginChunkRecording();
    chunk.AppendSetWorldPosition(&joined.object, MT_Vector3(0.0f, 1.0f, 0.0f), 2, 0);
    chunk.EndChunkRecording();
    buffer.AppendRecordedChunk(chunk);
    EXPECT_TRUE(buffer.HasPendingTransforms(&joined.object, false));
    buffer.PrepareForChunkReads();
    EXPECT_TRUE(buffer.HasIndexedPendingTransforms(&joined.object, false));
    buffer.EndRecording();
    buffer.Flush();
    /* A new content: the marks of the last one no longer count. */
    buffer.BeginRecording();
    EXPECT_FALSE(buffer.HasPendingTransforms(&joined.object, false));
    EXPECT_FALSE(buffer.HasPendingTransforms(&pair.root, true));
    buffer.EndRecording();
  }
  EXPECT_EQ(LN_CommandBuffer::PendingMarkVerifyDifferencesForTests(), differences_before);

  /* A lost mark is caught by verify mode, and the index's answer is given. */
  BatchObject lost(MT_Vector3(0.0f, 0.0f, 0.0f));
  LN_CommandBuffer buffer;
  buffer.SetMarksPendingMoves(true);
  buffer.BeginRecording();
  buffer.AppendApplyMovement(&lost.object, MT_Vector3(1.0f, 0.0f, 0.0f), false, 1, 0);
  lost.object.SetLogicNodesPendingMove(0);
  LN_CommandBuffer::SetPendingMarkDifferenceExpectedForTests(true);
  EXPECT_TRUE(buffer.HasPendingTransforms(&lost.object, false));
  LN_CommandBuffer::SetPendingMarkDifferenceExpectedForTests(false);
  EXPECT_GT(LN_CommandBuffer::PendingMarkVerifyDifferencesForTests(), differences_before);
  buffer.EndRecording();
}
