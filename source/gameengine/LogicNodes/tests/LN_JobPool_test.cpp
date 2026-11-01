/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "LN_JobPool.h"

namespace {

/* Every item runs exactly once per job, on the caller or a helper, whatever the helper count;
 * a job's writes are all seen by the caller when Run returns. */
TEST(LN_JobPool, EveryItemRunsOnceAndItsWritesAreSeenAfterTheJob)
{
  for (const uint32_t helpers : {0u, 1u, 3u, 8u}) {
    SCOPED_TRACE("helpers " + std::to_string(helpers));
    LN_JobPool pool;
    pool.SetHelperCount(helpers);
    EXPECT_EQ(pool.GetHelperCount(), helpers);
    for (const size_t count : {size_t(0), size_t(1), size_t(2), size_t(17), size_t(1000)}) {
      for (int job = 0; job < 20; job++) {
        std::vector<uint32_t> runs(count, 0);
        std::vector<uint32_t> threads(count, 99);
        pool.Run(count, [&](const size_t index, const uint32_t thread) {
          runs[index]++;
          threads[index] = thread;
        });
        for (size_t index = 0; index < count; index++) {
          ASSERT_EQ(runs[index], 1u) << "count " << count << " item " << index;
          EXPECT_LE(threads[index], helpers);
        }
      }
    }
  }
}

/* With helpers, the items really spread over several threads, each with its own index. */
TEST(LN_JobPool, HelpersShareTheWork)
{
  LN_JobPool pool;
  pool.SetHelperCount(3);
  std::atomic<uint32_t> running{0};
  std::atomic<uint32_t> most_at_once{0};
  std::vector<std::thread::id> ids(64);
  pool.Run(64, [&](const size_t index, const uint32_t /*thread*/) {
    const uint32_t now = running.fetch_add(1) + 1;
    uint32_t seen = most_at_once.load();
    while (now > seen && !most_at_once.compare_exchange_weak(seen, now)) {
    }
    ids[index] = std::this_thread::get_id();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    running.fetch_sub(1);
  });
  const std::set<std::thread::id> distinct(ids.begin(), ids.end());
  EXPECT_GE(distinct.size(), 2u);
  EXPECT_GE(most_at_once.load(), 2u);
}

/* Helpers made after jobs have run wait for the next job: they took the last finished one for a
 * new one and miscounted the busy helpers, so a job could return before its slow items ended
 * (review finding, 2026-10-31). */
TEST(LN_JobPool, NewHelpersWaitForTheNextJob)
{
  LN_JobPool pool;
  uint64_t passes = 0;
  for (int round = 0; round < 20; round++) {
    const uint32_t helpers = 2 + uint32_t(round % 3);
    pool.SetHelperCount(helpers);
    /* Time for a new helper to wake up: it must not take the finished job. */
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_EQ(pool.GetHelperPassesForTests(), passes) << "round " << round;
    std::atomic<uint32_t> finished{0};
    pool.Run(8, [&](const size_t, uint32_t) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      finished.fetch_add(1);
    });
    ASSERT_EQ(finished.load(), 8u) << "round " << round;
    passes += helpers;
    ASSERT_EQ(pool.GetHelperPassesForTests(), passes) << "round " << round;
  }
}

/* Changing the helper count between jobs keeps the pool working. */
TEST(LN_JobPool, HelperCountChangesBetweenJobs)
{
  LN_JobPool pool;
  for (const uint32_t helpers : {2u, 0u, 5u, 1u, 5u, 0u}) {
    pool.SetHelperCount(helpers);
    std::atomic<size_t> sum{0};
    pool.Run(100, [&](const size_t index, uint32_t) { sum += index; });
    EXPECT_EQ(sum.load(), size_t(4950));
  }
}

}  // namespace
