/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * The queries' Visualize lines with a Duration (2026-11-11): kept with the time they expire and
 * drawn again at each Logic + Physics step until then (each step's picture is drawn anew). The
 * manager keeps one list per scene; the drawing is the engine's debug lines (no render code).
 */

#pragma once

#include <cstddef>
#include <deque>
#include <mutex>

#include "MT_Vector3.h"
#include "MT_Vector4.h"

/** One line a query's Visualize drew: world ends and its colour. */
struct LN_DebugLine {
  MT_Vector3 from = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 to = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector4 color = MT_Vector4(1.0f, 1.0f, 1.0f, 1.0f);
};

class LN_TimedDebugLines {
 public:
  /** The most kept at once: more push the oldest out (warned once). */
  static constexpr size_t kMaxLines = 65536;

  /** A line to draw again until the game time passes `expires_at`. Any thread. */
  void Add(const LN_DebugLine &line, double expires_at);
  /** Draws the lines still due at `now` (LN_RedrawDebugLine) and forgets the others. */
  void Draw(double now);
  size_t Count() const;
  void Clear();

 private:
  struct Timed {
    LN_DebugLine line;
    double expires_at = 0.0;
  };
  std::deque<Timed> m_lines;
  mutable std::mutex m_mutex;
  bool m_capWarned = false;
};

/** A timed line drawn again: to the rasterizer, and to the tests' list (LN_SetDebugLinesForTests)
 * with the duration -1. */
void LN_RedrawDebugLine(const LN_DebugLine &line);
