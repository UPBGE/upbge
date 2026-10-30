/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_FrameScheduler.cpp
 *  \ingroup ketsji
 */

#include "KX_FrameScheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

/** Frame times and step boundaries are sums of the same doubles: a step due this soon (rounding
 * only) is due now. */
constexpr double RoundingTolerance = 1e-9;
/** While vsync paces, the steps are lined up this share of a step before the frame times. The
 * measured refresh is never exactly the step rate: exactly on the frame times, a screen only a
 * hair faster than the steps would need its catch-up (a frame without a step) right away; this
 * far off, it comes once in minutes. With the FPS Limit, frame times are the same sums as the
 * steps, so they line up exactly and the loop wakes once a frame. */
constexpr double VsyncAlignMargin = 0.1;
/** Plausible refresh periods (seconds): 20 to 250 Hz. */
constexpr double MinRefreshPeriod = 1.0 / 250.0;
constexpr double MaxRefreshPeriod = 1.0 / 20.0;
/** The vsync frame clock follows the measured frames this much each frame: phase and period of a
 * critically damped loop (period gain = phase gain squared / 4). Frame times then move by a tenth
 * of the wake-up noise, well inside VsyncStepTolerance. */
constexpr double VsyncPhaseGain = 0.1;
constexpr double VsyncPeriodGain = 0.0025;
/** Seconds a frame may sit inside the step counts' window edge and still count as out of it. */
constexpr double StepCountWindowTolerance = 1e-6;

double median_of(std::vector<double> values)
{
  if (values.empty()) {
    return 0.0;
  }
  const size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + middle, values.end());
  return values[middle];
}

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Refresh estimator
 * \{ */

void KX_RefreshEstimator::Reset()
{
  m_times.clear();
}

void KX_RefreshEstimator::AddFrame(const double time)
{
  if (!m_times.empty()) {
    if (time <= m_times.back()) {
      return;
    }
    /* A long gap (a hitch, a frozen window): the refresh grid starts again. */
    if (time - m_times.back() > 0.25) {
      m_times.clear();
    }
  }
  if (int(m_times.size()) > Frames) {
    m_times.erase(m_times.begin());
  }
  m_times.push_back(time);
}

double KX_RefreshEstimator::Period() const
{
  if (int(m_times.size()) <= Frames) {
    return 0.0;
  }
  std::vector<double> intervals;
  intervals.reserve(Frames);
  for (size_t i = 1; i < m_times.size(); i++) {
    intervals.push_back(m_times[i] - m_times[i - 1]);
  }
  const double median = median_of(intervals);
  if (median < MinRefreshPeriod || median > MaxRefreshPeriod) {
    return 0.0;
  }
  /* Wake-up noise is milliseconds whatever the refresh rate: at 144 Hz it is a fifth of a frame. */
  const double near = std::max(0.12 * median, 0.002);
  int single = 0;
  double refreshes = 0.0;
  for (const double interval : intervals) {
    if (std::abs(interval - median) < near) {
      single++;
    }
    const double count = std::round(interval / median);
    if (count < 1.0) {
      return 0.0;
    }
    refreshes += count;
  }
  /* Mostly one refresh apart: the screen paces these frames. The whole span over the refreshes
   * in it is far more exact than any interval with its wake-up noise (a missed refresh counts
   * two). */
  if (single < (int(intervals.size()) * 4) / 5) {
    return 0.0;
  }
  return (m_times.back() - m_times.front()) / refreshes;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame scheduler
 * \{ */

void KX_FrameScheduler::Configure(const Settings &settings)
{
  Settings sanitized = settings;
  if (!(sanitized.step > 0.0) || !std::isfinite(sanitized.step)) {
    sanitized.step = 1.0 / 60.0;
  }
  sanitized.max_steps = std::max(1, sanitized.max_steps);
  if (!(sanitized.limit_rate > 0.0) || !std::isfinite(sanitized.limit_rate)) {
    sanitized.limit_rate = 60.0;
  }
  if (!std::isfinite(sanitized.jitter_fix) || sanitized.jitter_fix < 0.0) {
    sanitized.jitter_fix = 0.0;
  }

  const bool step_changed = sanitized.step != m_settings.step;
  const bool pacing_changed = sanitized.limit != m_settings.limit ||
                              sanitized.limit_rate != m_settings.limit_rate ||
                              sanitized.vsync != m_settings.vsync;
  m_settings = sanitized;

  if (step_changed) {
    m_accumulator = 0.0;
    m_alignPending = true;
  }
  if (pacing_changed) {
    /* A new rhythm starts at the next pass. */
    m_deadline = -1.0;
    m_vsyncClockValid = false;
    m_refresh.Reset();
    m_alignPending = true;
  }
}

const KX_FrameScheduler::Settings &KX_FrameScheduler::GetSettings() const
{
  return m_settings;
}

void KX_FrameScheduler::Reset(const double now)
{
  m_started = true;
  m_advancedTo = now;
  m_accumulator = 0.0;
  m_alignPending = true;
  m_deadline = -1.0;
  m_vsyncClockValid = false;
  m_vsyncPeriod = 0.0;
  m_refresh.Reset();
  m_lastPassFrame = false;
  m_lastFrameTime = now;
  m_lastFrameOffset = 0.0;
  m_stepsSinceFrame = 0;
  m_lastFrameSteps = 0;
}

void KX_FrameScheduler::ResetTick()
{
  m_tick = 0;
}

KX_FrameScheduler::Pacing KX_FrameScheduler::GetPacing() const
{
  if (m_settings.limit) {
    return Pacing::Limit;
  }
  return m_settings.vsync ? Pacing::Vsync : Pacing::Free;
}

bool KX_FrameScheduler::LimitedFrame(const double now, double &r_time, bool &r_hitch)
{
  const double period = 1.0 / m_settings.limit_rate;
  r_hitch = false;
  if (m_deadline < 0.0) {
    /* The first frame starts the rhythm. */
    r_time = now;
    m_deadline = now + period;
    r_hitch = true;
    return true;
  }
  if (now < m_deadline) {
    return false;
  }
  if (now >= m_deadline + period) {
    /* A whole frame late (a hitch): the rhythm starts again here rather than drawing the missed
     * frames back to back. */
    r_time = now;
    m_deadline = now + period;
    r_hitch = true;
    return true;
  }
  r_time = m_deadline;
  m_deadline += period;
  return true;
}

double KX_FrameScheduler::AlignedDeadlineDelay() const
{
  const double period = 1.0 / m_settings.limit_rate;
  if (!(m_timeScale > 0.0)) {
    return period;
  }
  const double step_seconds = m_settings.step / m_timeScale;
  /* Clock time from this frame to the next step boundary. */
  const double to_step = std::max(0.0, m_settings.step - m_accumulator) / m_timeScale;
  if (period >= step_seconds) {
    const double whole_steps = std::floor((period - to_step) / step_seconds + 1e-6);
    return to_step + std::max(0.0, whole_steps) * step_seconds;
  }
  /* Several frames a step: every few frames one falls on a step boundary. */
  const double delay = std::fmod(to_step, period);
  return delay > RoundingTolerance ? delay : period;
}

double KX_FrameScheduler::VsyncFrameTime(const double now, bool &r_resync)
{
  r_resync = false;
  const double measured = m_refresh.Period();
  const double max_offset = std::min(0.5 * measured, m_settings.jitter_fix * m_settings.step);
  if (measured <= 0.0 || !(max_offset > 0.0)) {
    /* Not measured (yet), not paced by the screen, or no jitter fix: frame times are the clock. */
    if (m_vsyncClockValid) {
      m_vsyncClockValid = false;
      r_resync = true;
    }
    return now;
  }
  if (!m_vsyncClockValid || std::abs(m_vsyncPeriod - measured) > 0.05 * measured) {
    m_vsyncClockValid = true;
    m_vsyncPeriod = measured;
    m_vsyncTrack = now;
    r_resync = true;
    return now;
  }
  /* Track the screen's refresh: a phase that follows the frames by a tenth of each difference,
   * and a period that follows the phase. */
  const double track_refreshes = std::max(1.0,
                                          std::round((now - m_vsyncTrack) / m_vsyncPeriod));
  const double track_error = now - (m_vsyncTrack + track_refreshes * m_vsyncPeriod);
  m_vsyncTrack += track_refreshes * m_vsyncPeriod + VsyncPhaseGain * track_error;
  m_vsyncPeriod += VsyncPeriodGain * track_error / track_refreshes;
  m_vsyncPeriod = std::clamp(m_vsyncPeriod, 0.95 * measured, 1.05 * measured);

  /* Frame times are whole refreshes of the tracked period after the last frame: no wake-up noise
   * reaches them, so the steps a frame gets follow the screen's rhythm exactly. */
  const double refreshes = std::max(1.0, std::round((now - m_lastFrameTime) / m_vsyncPeriod));
  const double time = m_lastFrameTime + refreshes * m_vsyncPeriod;
  if (std::abs(now - time) > max_offset) {
    /* Too far from the clock (a hitch, a changed screen): the grid starts again here. */
    m_vsyncTrack = now;
    r_resync = true;
    return now;
  }
  return time;
}

KX_FrameScheduler::Pass KX_FrameScheduler::BeginPass(const double now, const double time_scale)
{
  if (!m_started || !std::isfinite(now) || now < m_advancedTo - 1.0) {
    /* The first pass, or the clock went back a long way (an external clock set back). */
    Reset(std::isfinite(now) ? now : 0.0);
  }
  m_timeScale = std::isfinite(time_scale) ? std::max(0.0, time_scale) : 0.0;
  const double step = m_settings.step;

  Pass pass;
  bool hitch = false;
  switch (GetPacing()) {
    case Pacing::Limit:
      pass.frame = LimitedFrame(now, pass.time, hitch);
      break;
    case Pacing::Vsync:
      pass.frame = true;
      m_refresh.AddFrame(now);
      pass.time = VsyncFrameTime(now, hitch);
      break;
    case Pacing::Free:
      pass.frame = true;
      pass.time = now;
      break;
  }
  if (!pass.frame) {
    pass.time = now;
  }

  /* Admit time up to the pass time: a frame's own time, not the moment the loop woke up. */
  const double upto = std::max(pass.time, m_advancedTo);
  pass.real_delta = upto - m_advancedTo;
  m_advancedTo = upto;
  pass.scaled_delta = std::min(pass.real_delta * m_timeScale,
                               step * double(m_settings.max_steps));
  m_accumulator += pass.scaled_delta;

  const double threshold = step - RoundingTolerance;
  while (m_accumulator >= threshold && pass.steps < m_settings.max_steps) {
    m_accumulator -= step;
    pass.steps++;
  }
  if (m_accumulator >= step) {
    /* More than the steps one pass may run (a long hitch): the rest is dropped. */
    m_accumulator = std::fmod(m_accumulator, step);
  }

  if (pass.frame && m_timeScale > 0.0) {
    if (m_alignPending || (hitch && GetPacing() != Pacing::Limit)) {
      /* Line the steps up with this frame: the next step boundaries fall on the frame grid (just
       * before it while vsync paces). What changes is less than a step, at a moment the picture
       * is not continuous anyway (the start, a resume, new settings, a vsync hitch). */
      m_accumulator = GetPacing() == Pacing::Vsync && m_vsyncClockValid ?
                          VsyncAlignMargin * step :
                          0.0;
      m_alignPending = false;
    }
    else if (hitch) {
      /* A late FPS Limit frame: its rhythm moves onto the steps instead (the next deadline is a
       * step boundary), so no game time is lost and a computer slower than the limit, late every
       * frame, still runs at real speed. */
      m_deadline = pass.time + AlignedDeadlineDelay();
    }
  }
  else if (hitch) {
    /* Paused: the next frame that runs steps lines them up. */
    m_alignPending = true;
  }

  pass.leftover = std::clamp(m_accumulator / step, 0.0, 1.0);
  /* A late FPS Limit frame is drawn after its time: the steps stay on the planned time, the
   * drawing moves on by the lateness. */
  const double late = pass.frame && GetPacing() == Pacing::Limit ?
                          std::max(0.0, now - pass.time) * m_timeScale :
                          0.0;
  pass.interpolation = std::clamp((m_accumulator + late) / step, 0.0, 1.0);
  m_lastPassFrame = pass.frame;
  if (pass.frame) {
    m_lastFrameTime = pass.time;
    m_lastFrameOffset = pass.time - now;
  }
  return pass;
}

void KX_FrameScheduler::EndPass(const int steps_run)
{
  /* Fewer than planned when a pause asked for during a step stopped the rest. */
  const int run = std::max(0, steps_run);
  m_tick += uint64_t(run);
  m_stepsSinceFrame += run;
  if (m_lastPassFrame) {
    m_lastFrameSteps = m_stepsSinceFrame;
    m_stepsSinceFrame = 0;
  }
}

void KX_FrameScheduler::SetPaused(const bool paused)
{
  if (paused == m_paused) {
    return;
  }
  m_paused = paused;
  if (!paused) {
    /* The leftover time of the pause moment would put the steps anywhere between the frames. */
    m_accumulator = 0.0;
    m_alignPending = true;
  }
}

double KX_FrameScheduler::NextWorkTime(const double now) const
{
  if (GetPacing() != Pacing::Limit) {
    /* Every pass draws a frame: the screen (or nothing) sets the pace. */
    return now;
  }
  double next = m_deadline < 0.0 ? now : m_deadline;
  if (m_timeScale > 0.0) {
    const double step_time = m_advancedTo + std::max(0.0, m_settings.step - m_accumulator) /
                                                m_timeScale;
    next = std::min(next, step_time);
  }
  return next;
}

uint64_t KX_FrameScheduler::GetTick() const
{
  return m_tick;
}

int KX_FrameScheduler::GetLastFrameSteps() const
{
  return m_lastFrameSteps;
}

double KX_FrameScheduler::GetLastFrameTime() const
{
  return m_lastFrameTime;
}

double KX_FrameScheduler::GetLastFrameClockOffset() const
{
  return m_lastFrameOffset;
}

double KX_FrameScheduler::GetRefreshRate() const
{
  if (GetPacing() != Pacing::Vsync) {
    return 0.0;
  }
  const double period = m_vsyncClockValid ? m_vsyncPeriod : m_refresh.Period();
  return period > 0.0 ? 1.0 / period : 0.0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Steps per frame, for the profile overlay
 * \{ */

void KX_FrameStepCounts::Reset()
{
  m_frames.clear();
  m_counts.fill(0);
}

void KX_FrameStepCounts::AddFrame(const double time, const int steps)
{
  if (!std::isfinite(time)) {
    return;
  }
  if (!m_frames.empty() && time < m_frames.back().first) {
    /* The clock went back (a new game clock): count from here. */
    Reset();
  }
  const int kind = std::clamp(steps, 0, Kinds - 1);
  m_frames.emplace_back(time, kind);
  m_counts[kind]++;
  /* Frame times are sums of the same doubles: a frame exactly one window back is out, whatever
   * the rounding, so 60 even frames a second always count 60. */
  const double oldest = time - Window + StepCountWindowTolerance;
  while (!m_frames.empty() && m_frames.front().first <= oldest) {
    m_counts[m_frames.front().second]--;
    m_frames.pop_front();
  }
}

const std::array<int, KX_FrameStepCounts::Kinds> &KX_FrameStepCounts::GetCounts() const
{
  return m_counts;
}

std::string KX_FrameStepCounts::Text() const
{
  std::string text;
  for (int kind = 0; kind < Kinds; kind++) {
    if (m_counts[kind] == 0) {
      continue;
    }
    if (!text.empty()) {
      text += ' ';
    }
    text += std::to_string(kind);
    if (kind == Kinds - 1) {
      text += '+';
    }
    text += ':' + std::to_string(m_counts[kind]);
  }
  return text.empty() ? "-" : text;
}

std::string KX_FramePacingText(const std::string &pacing,
                               const double frame_limit,
                               const double refresh_rate)
{
  char text[64];
  if (pacing == "LIMIT") {
    std::snprintf(text, sizeof(text), "FPS Limit %.0f", frame_limit);
    return text;
  }
  if (pacing == "VSYNC") {
    if (!(refresh_rate > 0.0)) {
      return "Vsync";
    }
    /* The even rate frames come at, which is the screen's refresh only while the game keeps up
     * with it (a game slower than a 100 Hz screen shows its own steady rate here). */
    std::snprintf(text, sizeof(text), "Vsync, frames %.1f Hz", refresh_rate);
    return text;
  }
  if (pacing == "FREE") {
    return "Free";
  }
  return "Variable timestep";
}

/** \} */
