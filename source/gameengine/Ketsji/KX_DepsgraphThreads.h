/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_DepsgraphThreads.h
 *  \ingroup ketsji
 *
 * Whether the game's depsgraph updates use Blender's worker threads. An update that starts tasks
 * wakes all of them (31 on a 32-core computer) and each spins about a millisecond after its work:
 * about 2 cores per update at 60 fps, whatever the update's size. The few objects a frame moves
 * are evaluated faster on the main thread alone (Blender's single-thread evaluation). An update
 * that is slow there (many objects reading a moving one: geometry nodes, modifiers, constraints,
 * thousands of moving objects) starts a trial: some updates with the workers, then some on the
 * main thread again (the update that started it may have been a one-off: the game's first frames,
 * many objects added at once). The workers stay only when they were clearly faster; 2000 moving
 * boxes take about as long either way, and the workers then keep 5 more cores busy for nothing.
 * A close result is tried again later, later each time it holds; after a while of fast worker
 * updates the main thread is tried again too.
 */

#pragma once

class KX_DepsgraphThreads {
 public:
  /** A main-thread update slower than this starts a trial of the workers (ms). */
  static constexpr double SlowOnMainThreadMs = 1.0;
  /** The workers stay only when their updates take at most this share of the main thread's
   * time (10% faster). */
  static constexpr double ClearlyFasterShare = 0.9;
  /** At most this share (twice as fast or more), one main-thread update decides, and the result
   * is never tried again (heavy geometry nodes: 22 ms alone, 2.7 ms with the workers). */
  static constexpr double FarFasterShare = 0.5;
  /** Worker updates averaged in a trial, then main-thread updates averaged, each after one update
   * left out (the first after a switch runs slower). */
  static constexpr int TrialUpdates = 16;
  static constexpr int CheckUpdates = 8;
  /** Updates a close result stands for before the next trial: the first time, and at most (it
   * doubles each time the trial gives the same result). */
  static constexpr int FirstTrialValidUpdates = 240;
  static constexpr int LastTrialValidUpdates = 7680;
  /** Worker updates faster than this count toward trying the main thread again (ms). */
  static constexpr double FastOnWorkersMs = 0.25;
  /** Fast worker updates in a row before the main thread is tried again: the first time, and
   * at most (it doubles each time the main thread is slow again). */
  static constexpr int FirstRetryUpdates = 60;
  static constexpr int LastRetryUpdates = 3840;

  /** Whether the next update evaluates with Blender's worker threads. */
  bool UseWorkers() const
  {
    return m_useWorkers;
  }

  /** An update that evaluated something took `milliseconds`, with the workers or without. */
  void Record(bool used_workers, double milliseconds);

 private:
  void StartTrial(double main_thread_ms);
  void Check(double main_thread_ms);
  void Decide(bool workers, bool close);

  bool m_useWorkers = false;
  /** The main thread is being tried again after the workers. */
  bool m_retrying = false;
  int m_fastUpdates = 0;
  int m_retryAfter = FirstRetryUpdates;

  /** The main-thread update that started the trial (ms; none for a trial run again). */
  double m_triggerMs = 0.0;
  /** Worker updates of the running trial (the left-out one included; more than TrialUpdates when
   * none runs) and their sum, then its main-thread updates and their sum (ms). */
  int m_trialUpdates = TrialUpdates + 1;
  double m_trialMs = 0.0;
  bool m_checking = false;
  int m_checkUpdates = 0;
  double m_checkMs = 0.0;
  /** The last close result: the workers' time when they were not clearly faster (ms, else 0),
   * whether they were close winners, the updates since and how many it stands for. */
  double m_notFasterMs = 0.0;
  bool m_closeWin = false;
  int m_sinceTrial = 0;
  int m_trialValidFor = FirstTrialValidUpdates;
  /** Running average of main-thread updates (ms), from the last trial's check on. */
  double m_mainAverageMs = 0.0;
};
