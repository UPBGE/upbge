/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "KX_DepsgraphThreads.h"

#include <algorithm>

namespace {

/** A scene whose updates take `main_ms` on the main thread and `workers_ms` with the workers. */
struct Scene {
  double main_ms;
  double workers_ms;
};

/** Runs `updates` updates the way the guard picks; returns how many ran on the main thread. */
int Play(KX_DepsgraphThreads &threads, const Scene scene, const int updates)
{
  int on_main = 0;
  for (int i = 0; i < updates; i++) {
    const bool workers = threads.UseWorkers();
    on_main += workers ? 0 : 1;
    threads.Record(workers, workers ? scene.workers_ms : scene.main_ms);
  }
  return on_main;
}

/** Updates of `scene` on the workers until the guard leaves them: how many. */
int WorkerRun(KX_DepsgraphThreads &threads, const Scene scene)
{
  int count = 0;
  while (threads.UseWorkers() && count < 100000) {
    threads.Record(true, scene.workers_ms);
    count++;
  }
  return count;
}

/** Updates of `scene` on the main thread until the guard tries the workers: how many. */
int MainRun(KX_DepsgraphThreads &threads, const Scene scene)
{
  int count = 0;
  while (!threads.UseWorkers() && count < 100000) {
    threads.Record(false, scene.main_ms);
    count++;
  }
  return count;
}

/** The updates a trial takes on the workers and then on the main thread (each phase leaves its
 * first update out of the average). */
constexpr int Trial = KX_DepsgraphThreads::TrialUpdates + 1;
constexpr int Checks = KX_DepsgraphThreads::CheckUpdates + 1;

TEST(KX_DepsgraphThreads, StartsOnTheMainThread)
{
  KX_DepsgraphThreads threads;
  EXPECT_FALSE(threads.UseWorkers());
}

TEST(KX_DepsgraphThreads, SmallUpdatesStayOnTheMainThread)
{
  KX_DepsgraphThreads threads;
  EXPECT_EQ(Play(threads, {0.12, 0.3}, 1000), 1000);
  EXPECT_EQ(Play(threads, {KX_DepsgraphThreads::SlowOnMainThreadMs, 0.3}, 1000), 1000);
}

TEST(KX_DepsgraphThreads, ASlowMainThreadUpdateTriesTheWorkers)
{
  KX_DepsgraphThreads threads;
  threads.Record(false, 22.0);
  EXPECT_TRUE(threads.UseWorkers());
}

TEST(KX_DepsgraphThreads, HeavyGeometryNodesStayOnTheWorkers)
{
  /* Geometry nodes reading a moving object: 22 ms alone, 2.7 ms with the workers. Two updates run
   * on the main thread: the one that started the trial and one check (far faster decides at
   * once, and is never tried again). */
  KX_DepsgraphThreads threads;
  EXPECT_EQ(Play(threads, {22.0, 2.7}, 100000), 2);
  EXPECT_TRUE(threads.UseWorkers());
}

TEST(KX_DepsgraphThreads, ManyMovingObjectsStayOnTheMainThread)
{
  /* 2000 moving boxes: 3.34 ms alone, 3.35 ms with the workers and 5 more cores busy. A new trial
   * now and then (later each time) costs little. */
  KX_DepsgraphThreads threads;
  const Scene boxes = {3.34, 3.35};
  EXPECT_GT(Play(threads, boxes, 20000), 20000 * 98 / 100);
  EXPECT_FALSE(threads.UseWorkers());
}

TEST(KX_DepsgraphThreads, WorkersClearlyFasterKeepTheUpdates)
{
  /* 6000 moving boxes: 35.7 ms alone, 30.3 ms with the workers (15% faster). A close win is
   * measured again now and then, later each time. */
  KX_DepsgraphThreads threads;
  EXPECT_LE(Play(threads, {35.7, 30.3}, 10000), 10000 / 100);
  EXPECT_TRUE(threads.UseWorkers());
}

TEST(KX_DepsgraphThreads, ACloseWinIsMeasuredAgainLaterEachTime)
{
  KX_DepsgraphThreads threads;
  const Scene scene = {10.0, 8.5};
  Play(threads, scene, 1 + Trial + Checks);
  ASSERT_TRUE(threads.UseWorkers());
  int wait = KX_DepsgraphThreads::FirstTrialValidUpdates;
  for (int round = 0; round < 8; round++) {
    /* `wait` updates, then a trial's worker updates, then its main-thread checks. */
    EXPECT_EQ(WorkerRun(threads, scene), wait + 1 + Trial);
    EXPECT_EQ(Play(threads, scene, Checks), Checks);
    ASSERT_TRUE(threads.UseWorkers());
    wait = std::min(wait * 2, KX_DepsgraphThreads::LastTrialValidUpdates);
  }
}

TEST(KX_DepsgraphThreads, TenPercentFasterIsTheLine)
{
  KX_DepsgraphThreads faster;
  Play(faster, {10.0, 9.0}, 1 + Trial + Checks);
  EXPECT_TRUE(faster.UseWorkers());

  KX_DepsgraphThreads not_faster;
  Play(not_faster, {10.0, 9.1}, 1 + Trial + Checks);
  EXPECT_FALSE(not_faster.UseWorkers());
}

TEST(KX_DepsgraphThreads, AOneOffSlowUpdateDoesNotKeepTheWorkers)
{
  /* The game's first update evaluates everything (20 ms); then the scene takes 3.34 ms alone and
   * 3.35 ms with the workers. The trial's main-thread updates show the workers are no faster. */
  KX_DepsgraphThreads threads;
  threads.Record(false, 20.0);
  ASSERT_TRUE(threads.UseWorkers());
  const Scene boxes = {3.34, 3.35};
  EXPECT_EQ(WorkerRun(threads, boxes), Trial);
  EXPECT_EQ(Play(threads, boxes, 100), 100);
  EXPECT_FALSE(threads.UseWorkers());
}

TEST(KX_DepsgraphThreads, TheFirstUpdateAfterASwitchIsLeftOut)
{
  /* 2000 moving boxes as measured: steady 3.6 ms alone and 3.4 ms with the workers (not 10%
   * faster), but the first update after each switch takes 6 ms alone and 5 ms with them. */
  KX_DepsgraphThreads threads;
  bool last_workers = false;
  int on_workers = 0;
  for (int i = 0; i < 5000; i++) {
    const bool workers = threads.UseWorkers();
    const bool first = workers != last_workers;
    threads.Record(workers, workers ? (first ? 5.0 : 3.4) : (first ? 6.0 : 3.6));
    on_workers += workers ? 1 : 0;
    last_workers = workers;
  }
  EXPECT_FALSE(threads.UseWorkers());
  EXPECT_LT(on_workers, 5000 / 50);
}

TEST(KX_DepsgraphThreads, AHeavierSceneTriesTheWorkersAgain)
{
  KX_DepsgraphThreads threads;
  Play(threads, {3.34, 3.35}, 100);
  ASSERT_FALSE(threads.UseWorkers());
  /* The workers' 3.35 ms is clearly faster than this: the running average sees it at once. */
  EXPECT_EQ(MainRun(threads, {10.0, 3.35}), 1);
  /* A scene only a little heavier takes a few updates. */
  KX_DepsgraphThreads slower;
  Play(slower, {3.34, 3.35}, 100);
  EXPECT_EQ(MainRun(slower, {5.0, 3.35}), 3);
}

TEST(KX_DepsgraphThreads, OneSlowUpdateDoesNotStartATrial)
{
  KX_DepsgraphThreads threads;
  const Scene boxes = {3.6, 3.4};
  Play(threads, boxes, 100);
  ASSERT_FALSE(threads.UseWorkers());
  /* Single updates vary: a 30% slower one is no heavier scene. */
  threads.Record(false, 4.7);
  EXPECT_FALSE(threads.UseWorkers());
  EXPECT_EQ(Play(threads, boxes, 100), 100);
}

TEST(KX_DepsgraphThreads, WorkersNoFasterAreTriedAgainLaterEachTime)
{
  KX_DepsgraphThreads threads;
  const Scene boxes = {3.34, 3.35};
  threads.Record(false, boxes.main_ms);
  int wait = KX_DepsgraphThreads::FirstTrialValidUpdates;
  for (int round = 0; round < 8; round++) {
    ASSERT_EQ(WorkerRun(threads, boxes), Trial);
    /* The trial's checks, then `wait` updates; the next one tries the workers. */
    EXPECT_EQ(MainRun(threads, boxes), Checks + wait + 1);
    wait = std::min(wait * 2, KX_DepsgraphThreads::LastTrialValidUpdates);
  }
  EXPECT_EQ(wait, KX_DepsgraphThreads::LastTrialValidUpdates);
}

TEST(KX_DepsgraphThreads, ADifferentResultResetsTheTrialWait)
{
  KX_DepsgraphThreads threads;
  const Scene boxes = {3.34, 3.35};
  Play(threads, boxes, 1 + Trial + Checks);
  MainRun(threads, boxes);
  Play(threads, boxes, Trial + Checks);
  ASSERT_EQ(MainRun(threads, boxes), KX_DepsgraphThreads::FirstTrialValidUpdates * 2 + 1);
  /* The trial that update started fails again; then a heavy moment the workers win at once. */
  Play(threads, boxes, Trial + Checks);
  ASSERT_FALSE(threads.UseWorkers());
  threads.Record(false, 40.0);
  Play(threads, {40.0, 2.0}, Trial + 1);
  ASSERT_TRUE(threads.UseWorkers());
  /* The scene gets light, then the boxes again: a failed trial waits the shortest time again. */
  WorkerRun(threads, {40.0, 0.1});
  ASSERT_FALSE(threads.UseWorkers());
  Play(threads, boxes, 1 + Trial + Checks);
  ASSERT_FALSE(threads.UseWorkers());
  EXPECT_EQ(MainRun(threads, boxes), KX_DepsgraphThreads::FirstTrialValidUpdates + 1);
}

TEST(KX_DepsgraphThreads, FastWorkerUpdatesTryTheMainThreadAgain)
{
  /* The scene got light while on the workers. */
  KX_DepsgraphThreads threads;
  Play(threads, {5.0, 0.1}, Trial + 2);
  ASSERT_TRUE(threads.UseWorkers());
  EXPECT_EQ(WorkerRun(threads, {5.0, 0.1}), KX_DepsgraphThreads::FirstRetryUpdates);
}

TEST(KX_DepsgraphThreads, OneSlowWorkerUpdateRestartsTheCount)
{
  KX_DepsgraphThreads threads;
  Play(threads, {5.0, 0.1}, Trial + 2);
  for (int i = 0; i < KX_DepsgraphThreads::FirstRetryUpdates - 1; i++) {
    threads.Record(true, 0.1);
  }
  threads.Record(true, 0.5);
  EXPECT_EQ(WorkerRun(threads, {5.0, 0.1}), KX_DepsgraphThreads::FirstRetryUpdates);
}

TEST(KX_DepsgraphThreads, SlowAgainOnTheMainThreadWaitsTwiceAsLong)
{
  /* Fast with the workers but slow without: each retry of the main thread starts a new trial,
   * then the workers keep the updates twice as long before the next retry. */
  KX_DepsgraphThreads threads;
  const Scene scene = {1.2, 0.1};
  threads.Record(false, scene.main_ms);
  int wait = KX_DepsgraphThreads::FirstRetryUpdates;
  for (int round = 0; round < 4; round++) {
    ASSERT_EQ(WorkerRun(threads, scene), Trial);
    threads.Record(false, scene.main_ms);
    ASSERT_TRUE(threads.UseWorkers());
    EXPECT_EQ(WorkerRun(threads, scene), wait);
    threads.Record(false, scene.main_ms);
    wait *= 2;
  }
}

TEST(KX_DepsgraphThreads, TheRetryWaitStopsGrowing)
{
  KX_DepsgraphThreads threads;
  const Scene scene = {1.2, 0.1};
  threads.Record(false, scene.main_ms);
  int last = 0;
  for (int round = 0; round < 20; round++) {
    WorkerRun(threads, scene);
    threads.Record(false, scene.main_ms);
    last = WorkerRun(threads, scene);
    threads.Record(false, scene.main_ms);
  }
  EXPECT_EQ(last, KX_DepsgraphThreads::LastRetryUpdates);
}

TEST(KX_DepsgraphThreads, AFastMainThreadUpdateResetsTheRetryWait)
{
  KX_DepsgraphThreads threads;
  const Scene scene = {1.2, 0.1};
  Play(threads, scene, Trial + 2);
  WorkerRun(threads, scene);
  threads.Record(false, scene.main_ms);
  WorkerRun(threads, scene);
  threads.Record(false, scene.main_ms);
  EXPECT_EQ(WorkerRun(threads, scene), KX_DepsgraphThreads::FirstRetryUpdates * 2);
  /* The scene got light again: the main thread stays, and a later heavy moment waits the
   * shortest time again. */
  threads.Record(false, 0.1);
  EXPECT_FALSE(threads.UseWorkers());
  threads.Record(false, 3.0);
  WorkerRun(threads, scene);
  threads.Record(false, 3.0);
  EXPECT_EQ(WorkerRun(threads, scene), KX_DepsgraphThreads::FirstRetryUpdates);
}

}  // namespace
