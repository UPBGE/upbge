/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_FrameScheduler.h
 *  \ingroup ketsji
 *
 * Plans the main loop's passes in fixed-step mode: whether a pass draws a frame, the time that
 * frame shows (its frame time), and how many fixed steps run before it. It works on one clock in
 * seconds and is plain arithmetic, so tests drive it with a made-up clock.
 *
 * A frame shows the game at its frame time: the steps due by that time run right before it is
 * drawn, whatever moment the loop actually woke up at. Frame times come from the FPS Limit's
 * deadlines, from the screen's refresh while vsync sets the pace, or from the clock. Only these
 * planned numbers are compared, so wake-up noise can't give one frame 0 steps and the next 2 (the
 * stutter seen after a resume, when the steps and the FPS Limit tick together). At game start and
 * at resume, the steps are lined up with the frame times; after a hitch, the FPS Limit's rhythm is
 * lined up with the steps instead, so no game time is lost (a computer slower than the limit keeps
 * real speed). That is the local clock's choice; a network clock would replace it with its own.
 */

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

/** Measures the screen's refresh period from the times of frames the screen paced (vsync). */
class KX_RefreshEstimator {
 public:
  /** Recent frames whose median interval is the refresh period. */
  static constexpr int Frames = 60;

  void Reset();
  /** A frame pass while vsync paced the loop. */
  void AddFrame(double time);
  /** Median interval of the last frames when most of them are one refresh apart, else 0. */
  double Period() const;

 private:
  /** Frame times, oldest first, at most Frames + 1. */
  std::vector<double> m_times;
};

class KX_FrameScheduler {
 public:
  /** What decides when frames are drawn. */
  enum class Pacing : uint8_t {
    /** The FPS Limit: frames at its fixed rate. */
    Limit,
    /** No FPS Limit, vsync on: a frame every pass, the screen sets the pace. */
    Vsync,
    /** No FPS Limit and no vsync: a frame every pass, as fast as possible. */
    Free,
  };

  struct Settings {
    /** One fixed step, in game seconds. */
    double step = 1.0 / 60.0;
    /** Steps one pass may run at most; the rest of a long hitch is dropped. */
    int max_steps = 5;
    /** FPS Limit: frames at `limit_rate`. */
    bool limit = false;
    double limit_rate = 60.0;
    /** The screen shows a new frame only at a refresh. */
    bool vsync = true;
    /** How many steps frame times may run off the clock while vsync paces: frame times follow
     * the screen's refresh and ignore wake-up noise up to this (Godot's physics jitter fix and
     * delta smoothing). 0 turns it off: frame times are the clock. */
    double jitter_fix = 0.5;
  };

  /** The plan of one pass. */
  struct Pass {
    bool frame = false;
    /** The time the pass advances the game to; a frame shows the game at this time. */
    double time = 0.0;
    /** Fixed steps due by `time`. */
    int steps = 0;
    /** Clock time this pass admits, and the game time it admits (scaled and clamped). */
    double real_delta = 0.0;
    double scaled_delta = 0.0;
    /** Share of a step left after the last step at `time`: 0 when the steps are lined up with
     * the frames. */
    double leftover = 0.0;
    /** Where between the last two steps a frame drawn now shows the game (Smooth Motion): the
     * leftover, plus how late a late FPS Limit frame is drawn after its time. Step counts keep the
     * planned time (no noise), drawing follows the clock (a busy computer still looks smooth). */
    double interpolation = 0.0;
  };

  /** New settings. A new pacing starts its rhythm at the next pass; a new step lines up again. */
  void Configure(const Settings &settings);
  const Settings &GetSettings() const;
  /** A new game, or timing that can't continue (the clock jumped back): no leftover time, the
   * next pass draws a frame and the steps line up with it. The tick count is kept. */
  void Reset(double now);
  /** The tick count starts again from 0 (a new game). */
  void ResetTick();

  /** Plans one pass at clock time `now`; `time_scale` is 0 while the game is paused. */
  Pass BeginPass(double now, double time_scale);
  /** The steps of the last pass that ran: a pause asked for during a step stops the rest. */
  void EndPass(int steps_run);
  /** A resume lines the steps up with the frame times again (the game was paused, so nobody sees
   * the leftover time go). */
  void SetPaused(bool paused);

  /** When the next pass has work (a frame deadline or a step), or `now` when every pass draws a
   * frame (the screen or nothing sets the pace). */
  double NextWorkTime(double now) const;

  /** Fixed steps run since the game started. */
  uint64_t GetTick() const;
  Pacing GetPacing() const;
  /** Steps run before the last frame was drawn (since the frame before it). */
  int GetLastFrameSteps() const;
  /** The last frame's time, and how far it was from the clock when it was planned. */
  double GetLastFrameTime() const;
  double GetLastFrameClockOffset() const;
  /** The measured refresh rate (Hz) while vsync paces, 0 while unknown. */
  double GetRefreshRate() const;

 private:
  /** Frame time of a pass while the FPS Limit paces. */
  bool LimitedFrame(double now, double &r_time, bool &r_hitch);
  /** From a frame to the next FPS Limit deadline, lined up with the steps: the last step boundary
   * within one period (with a whole ratio, every later frame falls on a step boundary). */
  double AlignedDeadlineDelay() const;
  /** Frame time of a pass while vsync paces: the screen's refresh grid, ignoring wake-up noise. */
  double VsyncFrameTime(double now, bool &r_resync);

  Settings m_settings;
  bool m_started = false;
  /** Clock time up to which time has been admitted. */
  double m_advancedTo = 0.0;
  /** Game time since the last step boundary; slightly negative after a step ran a bit early. */
  double m_accumulator = 0.0;
  double m_timeScale = 1.0;
  bool m_paused = false;
  /** The next frame lines the steps up with its frame time. */
  bool m_alignPending = true;
  uint64_t m_tick = 0;

  /** Limited frames: the next frame's deadline (< 0 when none is planned yet). */
  double m_deadline = -1.0;

  /** Vsync frames: the screen's refresh grid, tracked from the measured frames. */
  KX_RefreshEstimator m_refresh;
  bool m_vsyncClockValid = false;
  double m_vsyncPeriod = 0.0;
  /** The tracked refresh phase (frame times themselves stay on whole refreshes of the period). */
  double m_vsyncTrack = 0.0;

  /** Frames. */
  bool m_lastPassFrame = false;
  double m_lastFrameTime = 0.0;
  double m_lastFrameOffset = 0.0;
  int m_stepsSinceFrame = 0;
  int m_lastFrameSteps = 0;
};

/**
 * How many fixed steps the drawn frames of the last second showed, for the profile overlay's
 * Steps/frame line: "1:60" while every frame shows one step, "0:12 1:36 2:12" while frames
 * stutter (frame times stay even, so the frame-time numbers can't show it).
 */
class KX_FrameStepCounts {
 public:
  /** Frames with 0, 1, 2, and 3 or more steps. */
  static constexpr int Kinds = 4;
  /** Seconds of frames counted. */
  static constexpr double Window = 1.0;

  void Reset();
  /** A drawn frame at `time` (its frame time, in seconds) that showed `steps` steps. */
  void AddFrame(double time, int steps);
  const std::array<int, Kinds> &GetCounts() const;
  /** The counts that aren't 0, "steps:frames" each ("3+" for 3 or more); "-" before any frame. */
  std::string Text() const;

 private:
  /** Frame time and kind of the counted frames, oldest first. */
  std::deque<std::pair<double, int>> m_frames;
  std::array<int, Kinds> m_counts = {};
};

/**
 * What paces the frames, for the profile overlay: "FPS Limit 60", "Vsync, frames 100.0 Hz" (the
 * measured even rate of the frames, "Vsync" while they aren't even), "Free" or "Variable
 * timestep", from the pacing names of bge.logic.getFramePacingInfo() ("LIMIT", "VSYNC", "FREE",
 * "VARIABLE").
 */
std::string KX_FramePacingText(const std::string &pacing, double frame_limit, double refresh_rate);
