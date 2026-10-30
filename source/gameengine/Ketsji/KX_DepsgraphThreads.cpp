/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file gameengine/Ketsji/KX_DepsgraphThreads.cpp
 *  \ingroup ketsji
 */

#include "KX_DepsgraphThreads.h"

#include <algorithm>
#include <limits>

void KX_DepsgraphThreads::StartTrial(const double main_thread_ms)
{
  m_useWorkers = true;
  m_fastUpdates = 0;
  m_triggerMs = main_thread_ms;
  m_trialUpdates = 0;
  m_trialMs = 0.0;
}

void KX_DepsgraphThreads::Decide(const bool workers, const bool close)
{
  m_checking = false;
  m_useWorkers = workers;
  m_fastUpdates = 0;
  m_sinceTrial = 0;
  if (!close) {
    m_notFasterMs = 0.0;
    m_closeWin = false;
    m_trialValidFor = FirstTrialValidUpdates;
    return;
  }
  /* The same close result again stands longer. */
  const bool same = workers ? m_closeWin : m_notFasterMs > 0.0;
  m_trialValidFor = same ? std::min(m_trialValidFor * 2, LastTrialValidUpdates) :
                           FirstTrialValidUpdates;
  m_closeWin = workers;
  m_notFasterMs = workers ? 0.0 : m_trialMs / TrialUpdates;
  m_mainAverageMs = m_checkMs / CheckUpdates;
}

void KX_DepsgraphThreads::Check(const double main_thread_ms)
{
  const double workers_ms = m_trialMs / TrialUpdates;
  if (++m_checkUpdates == 1) {
    /* The first update after a switch runs slower (2000 moving boxes: about 4 ms instead of
     * 3.5), so it is left out of the average; far faster is clear even with it. */
    if (workers_ms <= FarFasterShare * std::min(m_triggerMs, main_thread_ms)) {
      Decide(true, false);
    }
    return;
  }
  m_checkMs += main_thread_ms;
  if (m_checkUpdates <= CheckUpdates) {
    return;
  }
  Decide(workers_ms <= ClearlyFasterShare * (m_checkMs / CheckUpdates), true);
}

void KX_DepsgraphThreads::Record(const bool used_workers, const double milliseconds)
{
  if (!used_workers) {
    if (m_checking) {
      Check(milliseconds);
      return;
    }
    m_sinceTrial++;
    m_mainAverageMs = m_mainAverageMs > 0.0 ? 0.9 * m_mainAverageMs + 0.1 * milliseconds :
                                              milliseconds;
    if (milliseconds > SlowOnMainThreadMs) {
      if (m_retrying) {
        m_retryAfter = std::min(m_retryAfter * 2, LastRetryUpdates);
      }
      m_retrying = false;
      /* A recent trial found the workers no faster than the main thread is now: stay. A heavier
       * scene tries them again; the running average keeps one slow update from looking like one
       * (single updates vary by 30%). */
      const bool workers_not_faster = m_notFasterMs > 0.0 && m_sinceTrial <= m_trialValidFor &&
                                      m_notFasterMs > ClearlyFasterShare * m_mainAverageMs;
      if (!workers_not_faster) {
        StartTrial(milliseconds);
      }
      return;
    }
    if (m_retrying) {
      /* The main thread is fine again: a later slow update waits the shortest time. */
      m_retryAfter = FirstRetryUpdates;
    }
    m_retrying = false;
    return;
  }

  if (m_trialUpdates <= TrialUpdates) {
    /* The first worker update after a switch is left out too. */
    if (m_trialUpdates > 0) {
      m_trialMs += milliseconds;
    }
    if (++m_trialUpdates > TrialUpdates) {
      m_useWorkers = false;
      m_checking = true;
      m_checkUpdates = 0;
      m_checkMs = 0.0;
      return;
    }
  }
  else if (m_closeWin && ++m_sinceTrial > m_trialValidFor) {
    /* The workers won by a little: measure both again. */
    m_trialUpdates = 0;
    m_trialMs = 0.0;
    m_triggerMs = std::numeric_limits<double>::infinity();
  }

  m_fastUpdates = milliseconds < FastOnWorkersMs ? m_fastUpdates + 1 : 0;
  if (m_fastUpdates >= m_retryAfter) {
    m_useWorkers = false;
    m_retrying = true;
    m_fastUpdates = 0;
  }
}
