/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The lookup of constant Object inputs (lean runtime plan, phase A9): a constant object is found
 * once for the object list generation and not by name at every read (LN_RuntimeTree::
 * ResolveConstantObject). Whatever happens to the scene (a spawn after a miss, a removal, two
 * objects of one name, a menu copy rule that changes between reads, a tick that is not running,
 * an object only in the inactive list, another scene with the same generation) the answers must
 * be the old lookup's, step by step: a tree measures its distance to the constant "Target" every
 * step and stores the distance and whether there was one; the three modes (old lookup, cached,
 * verify) must store the very same. */

#include "testing/testing.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

std::shared_ptr<LN_Program> DistanceToTarget()
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
  GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(store, "Property", "d");
  b.Link(event, "Out", store, "Flow");
  b.Link(distance, "Distance", store, "Value");
  GenNode &valid = b.Add("LogicNativeSetGamePropertyBool");
  b.String(valid, "Property", "v");
  b.Link(store, "Done", valid, "Flow");
  b.Link(distance, "Valid", valid, "Value");
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

using Script = std::function<void(SemRig &rig, int step)>;

struct ModeGuard {
  ~ModeGuard()
  {
    LN_RuntimeTree::SetConstantObjectModeForTests(-1);
  }
};

/** The distance and whether there was one, after each step of the script, in `mode`. */
std::string Run(const std::shared_ptr<LN_Program> &program,
                const int mode,
                const int steps,
                const Script &script)
{
  LN_RuntimeTree::SetConstantObjectModeForTests(mode);
  SemRig rig(program, kMemoOff);
  /* A tree that has not stored yet reads as "none". */
  std::string log;
  for (int step = 0; step < steps; step++) {
    script(rig, step);
    rig.Step();
    KX_GameObject &owner = rig.Object(0);
    log += rig.Has(owner, "d") ? std::to_string(rig.Float(owner, "d")) : "-";
    log += rig.Has(owner, "v") ? (rig.Text(owner, "v") == "TRUE" ? "T" : "F") : "-";
    log += ";";
  }
  /* The rig's seam may have been swapped by the script: put its own back for the destructor. */
  LN_SetSceneAccessForTests(&rig.Access());
  return log;
}

/** The script's log in the three modes: the same, no difference found by verify. Returns the log. */
std::string Check(const char *what, const int steps, const Script &script)
{
  ModeGuard restore;
  const std::shared_ptr<LN_Program> program = DistanceToTarget();
  EXPECT_NE(program, nullptr);
  if (program == nullptr) {
    return "";
  }
  const uint32_t differences_before = LN_RuntimeTree::ConstantObjectVerifyDifferencesForTests();
  const std::string old_way = Run(program, 0, steps, script);
  const std::string cached = Run(program, 1, steps, script);
  const std::string verified = Run(program, 2, steps, script);
  EXPECT_EQ(cached, old_way) << what << " (cached against the old lookup)";
  EXPECT_EQ(verified, old_way) << what << " (verify against the old lookup)";
  EXPECT_EQ(LN_RuntimeTree::ConstantObjectVerifyDifferencesForTests(), differences_before) << what;
  return old_way;
}

}  // namespace

/* Spawn after a miss: the first steps find nothing (and the miss is kept), the step after the
 * spawn finds the object (the spawn changes the generation, which ends the kept miss). */
TEST(LN_ConstantObjects, ASpawnAfterAMissIsFound)
{
  const std::string log = Check("spawn after a miss", 6, [](SemRig &rig, const int step) {
    if (step == 3) {
      rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
    }
  });
  EXPECT_EQ(log.substr(0, 10), "0.000000F;") << log; /* no Target: no distance, not valid */
  EXPECT_EQ(log.substr(log.rfind(';', log.size() - 2) + 1), "5.000000T;") << log;
}

/* Removal: found for a while, gone after the object left the list. */
TEST(LN_ConstantObjects, ARemovedObjectIsNotFoundAnyMore)
{
  KX_GameObject *target = nullptr;
  const std::string log = Check("removal", 6, [&](SemRig &rig, const int step) {
    if (step == 0) {
      target = &rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
    }
    if (step == 3) {
      std::vector<KX_GameObject *> &objects = rig.Access().objects;
      objects.erase(std::find(objects.begin(), objects.end(), target));
      rig.NoteObjectListChanged();
    }
  });
  EXPECT_NE(log.find("5.000000T;"), std::string::npos) << log;
  EXPECT_NE(log.find("5.000000T;"), log.rfind("5.000000T;")) << "found for several steps: " << log;
  EXPECT_EQ(log.substr(log.rfind(';', log.size() - 2) + 1), "0.000000F;") << log;
}

/* Two objects of one name: the first of the list wins; when it leaves, the second does. */
TEST(LN_ConstantObjects, TheFirstObjectOfAnEqualNameWinsUntilItLeaves)
{
  KX_GameObject *first = nullptr;
  const std::string log = Check("duplicate names", 6, [&](SemRig &rig, const int step) {
    if (step == 0) {
      first = &rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
      rig.AddObject("Target", MT_Vector3(6.0f, 8.0f, 0.0f));
    }
    if (step == 3) {
      std::vector<KX_GameObject *> &objects = rig.Access().objects;
      objects.erase(std::find(objects.begin(), objects.end(), first));
      rig.NoteObjectListChanged();
    }
  });
  EXPECT_NE(log.find("5.000000T;"), std::string::npos) << log;
  EXPECT_EQ(log.substr(log.rfind(';', log.size() - 2) + 1), "10.000000T;") << log;
}

/* A menu copy rule that starts to apply with no change of the object list: the viewer's copy is
 * measured from then on (the rule is applied at every read). And one that finds no copy at all
 * (a copy removed by logic) reads as no object. */
TEST(LN_ConstantObjects, TheMenuCopyRuleIsAppliedAtEveryRead)
{
  KX_GameObject *copy = nullptr;
  int mode = 0; /* 0: no rule, 1: the copy, 2: none */
  const std::string log = Check("menu copy rule", 8, [&](SemRig &rig, const int step) {
    if (step == 0) {
      rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
      copy = &rig.AddObject("TargetCopy", MT_Vector3(0.0f, 0.0f, 2.0f));
      mode = 0;
      /* Set once: setting a rule publishes the seam again (a new generation), and the point here
       * is a rule that changes with no change of the object list. */
      rig.SetOverlayCounterpart(
          [&mode, &copy](KX_GameObject *object, KX_GameObject *) -> KX_GameObject * {
            if (mode == 0) {
              return object;
            }
            return mode == 1 ? copy : nullptr;
          });
    }
    if (step == 2) {
      mode = 1;
    }
    if (step == 5) {
      mode = 2;
    }
  });
  EXPECT_NE(log.find("5.000000T;"), std::string::npos) << log;
  EXPECT_NE(log.find("2.000000T;"), std::string::npos) << log;
}

/* A tick that is not running (the manager's name table answers only in a tick): the answers are
 * the same in and out of a tick, and the switch back and forth is followed. */
TEST(LN_ConstantObjects, TheAnswerFollowsWhetherATickIsRunning)
{
  const std::string log = Check("ticking", 8, [](SemRig &rig, const int step) {
    if (step == 0) {
      rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
    }
    rig.Access().ticking = step % 3 != 1;
  });
  EXPECT_NE(log.find("5.000000T;"), std::string::npos) << log;
}

/* An object only in the inactive list (a template) is not found by a constant, as before. */
TEST(LN_ConstantObjects, AnInactiveObjectIsNotFound)
{
  KX_GameObject *template_object = nullptr;
  const std::string log = Check("inactive", 5, [&](SemRig &rig, const int step) {
    if (step == 0) {
      template_object = &rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
      std::vector<KX_GameObject *> &objects = rig.Access().objects;
      objects.erase(std::find(objects.begin(), objects.end(), template_object));
      rig.Access().inactive.push_back(template_object);
      rig.NoteObjectListChanged();
    }
  });
  EXPECT_EQ(log.find("5.000000"), std::string::npos) << log;
}

/* Another scene (here another seam) with the very same generation number: the tree's memory is
 * of one scene, so the other scene's Target is measured when the tree is run in it. */
TEST(LN_ConstantObjects, AnotherSceneWithTheSameGenerationIsAnotherLookup)
{
  LN_SceneAccessForTests other;
  KX_GameObject *other_target = nullptr;
  std::unique_ptr<BatchObject> holder;
  const std::string log = Check("two scenes", 6, [&](SemRig &rig, const int step) {
    if (step == 0) {
      rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
      holder = std::make_unique<BatchObject>(MT_Vector3(6.0f, 8.0f, 0.0f));
      holder->object.SetName("Target");
      other_target = &holder->object;
      other = rig.Access();
      other.objects.clear();
      other.objects.push_back(&rig.Object(0));
      other.objects.push_back(other_target);
      /* The same generation number as the rig's scene. */
    }
    LN_SetSceneAccessForTests(step >= 3 ? &other : &rig.Access());
  });
  EXPECT_NE(log.find("5.000000T;"), std::string::npos) << log;
  EXPECT_NE(log.find("10.000000T;"), std::string::npos) << log;
}

/* A micro benchmark (DISABLED_): 2,000 trees measure their distance to the constant "Target" in
 * every step; the steps' time by the old lookup and with the cached one, best of 15. */
TEST(LN_ConstantObjects, DISABLED_LookupBench)
{
  ModeGuard restore;
  const std::shared_ptr<LN_Program> program = DistanceToTarget();
  ASSERT_NE(program, nullptr);
  for (const int mode : {0, 1}) {
    LN_RuntimeTree::SetConstantObjectModeForTests(mode);
    SemRig rig(program, kMemoOff, 2000);
    rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
    double best = 1.0e9;
    for (int round = 0; round < 15; round++) {
      const auto start = std::chrono::steady_clock::now();
      rig.Step();
      best = std::min(best,
                      std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count());
    }
    std::printf("CONST_BENCH mode %d: %.3f ms a step for 2000 trees\n", mode, best);
  }
}
