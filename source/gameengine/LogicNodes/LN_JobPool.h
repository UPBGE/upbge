/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_JobPool.h
 *  \ingroup logicnodes
 *
 * A small pool of helper threads for Logic Nodes (2026-10-31, step 7). The threads sleep on a
 * condition variable between jobs: no spinning, unlike Blender's task pool, whose 31 workers
 * spun after every tick of the removed Parallel Tree Execution. The calling thread works on the
 * job too, so a job with H helpers runs on H + 1 threads.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class LN_JobPool {
 public:
  /** `item(index, thread)`: `thread` is 0 for the calling thread, 1..helpers for the helpers. */
  using ItemFunction = std::function<void(size_t index, uint32_t thread)>;

  LN_JobPool() = default;
  ~LN_JobPool();
  LN_JobPool(const LN_JobPool &) = delete;
  LN_JobPool &operator=(const LN_JobPool &) = delete;

  /** Starts or stops helpers so the pool has `helpers` of them (0: everything on the caller).
   * Never while a job runs. */
  void SetHelperCount(uint32_t helpers);
  uint32_t GetHelperCount() const
  {
    return uint32_t(m_threads.size());
  }

  /** Calls `item` once for every index below `count`, on the calling thread and the helpers,
   * and returns when all are done. Items are handed out in increasing order, one at a time.
   * Not reentrant: an item must not run another job. */
  void Run(size_t count, const ItemFunction &item);

  /** Tests: how many times a helper has taken a job (each helper takes each job once). */
  uint64_t GetHelperPassesForTests()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_helperPasses;
  }

 private:
  /** `seen_serial`: the job serial when the helper was made (it waits for a later one). */
  void HelperLoop(uint32_t thread, uint64_t seen_serial);
  void WorkOn(uint32_t thread);

  std::vector<std::thread> m_threads;
  std::mutex m_mutex;
  std::condition_variable m_wake;
  std::condition_variable m_done;
  /** Counts the jobs started, so a helper takes each job once. */
  uint64_t m_jobSerial = 0;
  bool m_stopping = false;
  const ItemFunction *m_item = nullptr;
  size_t m_count = 0;
  std::atomic<size_t> m_next{0};
  /** Helpers still working on the current job. */
  uint32_t m_busyHelpers = 0;
  uint64_t m_helperPasses = 0;
};
