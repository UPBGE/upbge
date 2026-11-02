/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_JobPool.cpp
 *  \ingroup logicnodes
 */

#include "LN_JobPool.h"

#include "LN_LeanProgram.h"

LN_JobPool::~LN_JobPool()
{
  SetHelperCount(0);
}

void LN_JobPool::SetHelperCount(const uint32_t helpers)
{
  if (helpers == m_threads.size()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopping = true;
  }
  m_wake.notify_all();
  for (std::thread &thread : m_threads) {
    thread.join();
  }
  m_threads.clear();
  m_stopping = false;
  m_threads.reserve(helpers);
  /* A new helper takes only jobs started after it exists (no helper runs now: the field is
   * ours). Starting from 0, it took the last finished job for a new one and miscounted the
   * busy helpers. */
  const uint64_t serial = m_jobSerial;
  for (uint32_t index = 0; index < helpers; index++) {
    m_threads.emplace_back(&LN_JobPool::HelperLoop, this, index + 1, serial);
  }
}

void LN_JobPool::WorkOn(const uint32_t thread)
{
  while (true) {
    const size_t index = m_next.fetch_add(1, std::memory_order_relaxed);
    if (index >= m_count) {
      return;
    }
    (*m_item)(index, thread);
  }
}

void LN_JobPool::HelperLoop(const uint32_t thread, uint64_t seen_serial)
{
  /* A helper only reads lean programs; the main thread makes them (plan phase A16). */
  LN_SetLeanHelperThread(true);
  while (true) {
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_wake.wait(lock, [&] { return m_stopping || m_jobSerial != seen_serial; });
      if (m_stopping) {
        return;
      }
      seen_serial = m_jobSerial;
      m_helperPasses++;
    }
    WorkOn(thread);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_busyHelpers--;
      if (m_busyHelpers == 0) {
        m_done.notify_one();
      }
    }
  }
}

void LN_JobPool::Run(const size_t count, const ItemFunction &item)
{
  if (count == 0) {
    return;
  }
  if (m_threads.empty() || count == 1) {
    for (size_t index = 0; index < count; index++) {
      item(index, 0);
    }
    return;
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_item = &item;
    m_count = count;
    m_next.store(0, std::memory_order_relaxed);
    m_busyHelpers = uint32_t(m_threads.size());
    m_jobSerial++;
  }
  m_wake.notify_all();
  WorkOn(0);
  /* The lock gives the helpers' writes to the caller (and the next job's setup to them). */
  std::unique_lock<std::mutex> lock(m_mutex);
  m_done.wait(lock, [&] { return m_busyHelpers == 0; });
  m_item = nullptr;
  m_count = 0;
}
