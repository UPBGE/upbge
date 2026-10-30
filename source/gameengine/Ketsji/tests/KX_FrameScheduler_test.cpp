/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cmath>
#include <random>
#include <vector>

#include "KX_FrameScheduler.h"

namespace {

/**
 * The main loop and a screen, on a made-up clock. A pass plans with the scheduler; a pass that
 * draws hands its frame to the screen. With vsync the screen holds two images (one shown, one
 * queued, as Blender's Vulkan FIFO swapchain): drawing waits while a frame is still queued, and
 * a frame is shown at the first refresh after it is ready. Between frames the loop sleeps until
 * the scheduler's next work time and wakes a little late (the system's timer).
 */
struct LoopSimulation {
  KX_FrameScheduler scheduler;
  std::mt19937 random{7};

  double now = 0.0;
  double refresh = 1.0 / 60.0;
  double refresh_phase = 0.0013;
  bool vsync = true;
  double render = 0.006;
  /** The loop wakes up to this late (Linux: microseconds; Windows' 1 ms timer: up to 2 ms). */
  double wake_late = 4e-6;
  /** The system hands frames back with this much noise. */
  double hand_over_noise = 0.0;
  double time_scale = 1.0;

  struct Frame {
    double time;
    int steps;
    double leftover;
    double interpolation;
    double begin;
    double end;
    double shown;
    /** Fixed steps run since the start, this frame's included. */
    uint64_t tick;
  };
  std::vector<Frame> frames;
  /** When the last handed frame reaches the screen. */
  double last_shown = -1.0;

  double Uniform(const double max)
  {
    return std::uniform_real_distribution<double>(0.0, max)(random);
  }

  double NextRefresh(const double time) const
  {
    return refresh_phase + std::ceil((time - refresh_phase) / refresh) * refresh;
  }

  void Pass()
  {
    const KX_FrameScheduler::Pass pass = scheduler.BeginPass(now, time_scale);
    scheduler.EndPass(pass.steps);
    if (pass.frame) {
      const int steps = scheduler.GetLastFrameSteps();
      const double begin = now;
      /* Two images: wait while the last frame is still queued for the screen. */
      const double free_image = vsync ? std::max(now, last_shown) : now;
      const double ready = free_image + render;
      const double shown = vsync ? NextRefresh(std::max(ready, last_shown + 0.5 * refresh)) :
                                   ready;
      const double end = ready + Uniform(hand_over_noise);
      last_shown = shown;
      frames.push_back({pass.time,
                        steps,
                        pass.leftover,
                        pass.interpolation,
                        begin,
                        end,
                        shown,
                        scheduler.GetTick()});
      now = end + 2e-6;
      return;
    }
    const double target = scheduler.NextWorkTime(now);
    now = std::max(now + 1e-6, target + Uniform(wake_late));
  }

  void RunFor(const double seconds)
  {
    const double end = now + seconds;
    while (now < end) {
      Pass();
    }
  }

  /** Frames drawn from `from_seconds` on. */
  std::vector<Frame> FramesSince(const double from_seconds) const
  {
    std::vector<Frame> result;
    for (const Frame &frame : frames) {
      if (frame.begin >= from_seconds) {
        result.push_back(frame);
      }
    }
    return result;
  }
};

KX_FrameScheduler::Settings LimitSettings(const double steps, const double limit)
{
  KX_FrameScheduler::Settings settings;
  settings.step = 1.0 / steps;
  settings.limit = true;
  settings.limit_rate = limit;
  return settings;
}

int CountFramesWithout(const std::vector<LoopSimulation::Frame> &frames, const int steps)
{
  int count = 0;
  for (const LoopSimulation::Frame &frame : frames) {
    if (frame.steps != steps) {
      count++;
    }
  }
  return count;
}

/* The FPS demo stutter: 60 steps and a 60 fps limit on one clock gave frames 0 or 2 steps
 * whenever the two lined up within wake-up noise. A frame now takes the steps due by its own
 * time, so every frame gets exactly one, however late the loop wakes. */
TEST(KX_FrameScheduler, EveryFrameGetsOneStepWhateverTheWakeUpNoise)
{
  for (const double wake_late : {4e-6, 0.002}) {
    LoopSimulation loop;
    loop.refresh = 1.0 / 100.0;
    loop.wake_late = wake_late;
    loop.scheduler.Configure(LimitSettings(60.0, 60.0));
    loop.RunFor(10.0);
    const std::vector<LoopSimulation::Frame> frames = loop.FramesSince(0.1);
    ASSERT_GT(frames.size(), 580u);
    EXPECT_EQ(CountFramesWithout(frames, 1), 0) << "wake late " << wake_late;
    /* Frame times keep the limit's rhythm exactly. */
    for (size_t i = 1; i < frames.size(); i++) {
      EXPECT_NEAR(frames[i].time - frames[i - 1].time, 1.0 / 60.0, 1e-9);
    }
    /* Game time follows the clock: about 600 steps in 10 seconds. */
    EXPECT_NEAR(double(loop.scheduler.GetTick()), 600.0, 2.0);
  }
}

/* Pausing and resuming at any moment, in a frame or between frames: the steps line up with the
 * frames again, so no resume can land on the edge. */
TEST(KX_FrameScheduler, ResumeLinesTheStepsUpWithTheFrames)
{
  LoopSimulation loop;
  loop.wake_late = 0.001;
  loop.scheduler.Configure(LimitSettings(60.0, 60.0));
  std::mt19937 random(11);
  for (int cycle = 0; cycle < 20; cycle++) {
    loop.RunFor(0.5 + std::uniform_real_distribution<double>(0.0, 0.7)(random));
    loop.time_scale = 0.0;
    loop.scheduler.SetPaused(true);
    /* Pause after a step, in the middle of a frame's rhythm. */
    loop.now += std::uniform_real_distribution<double>(0.0, 1.0 / 60.0)(random);
    loop.RunFor(0.3 + std::uniform_real_distribution<double>(0.0, 0.5)(random));
    loop.now += std::uniform_real_distribution<double>(0.0, 1.0 / 60.0)(random);
    loop.time_scale = 1.0;
    loop.scheduler.SetPaused(false);
    const size_t resumed_at = loop.frames.size();
    loop.RunFor(1.0);
    /* The first frame after the resume may run the step the resume moment owed; every frame
     * after it gets exactly one. */
    for (size_t i = resumed_at + 1; i < loop.frames.size(); i++) {
      EXPECT_EQ(loop.frames[i].steps, 1) << "cycle " << cycle << " frame " << i - resumed_at;
      /* Lined up exactly on the frames. */
      EXPECT_NEAR(loop.frames[i].leftover, 0.0, 1e-6);
    }
  }
}

TEST(KX_FrameScheduler, AHitchStartsTheRhythmAgain)
{
  LoopSimulation loop;
  loop.scheduler.Configure(LimitSettings(60.0, 60.0));
  loop.RunFor(1.0);
  /* The loop stalls for a tenth of a second (loading, a slow frame). */
  loop.now += 0.1;
  const size_t stalled_at = loop.frames.size();
  loop.RunFor(1.0);
  ASSERT_GT(loop.frames.size(), stalled_at + 50);
  /* The late frame catches up at most the steps a pass may run, then one step a frame. */
  EXPECT_LE(loop.frames[stalled_at].steps, 5);
  EXPECT_GE(loop.frames[stalled_at].steps, 5);
  for (size_t i = stalled_at + 1; i < loop.frames.size(); i++) {
    EXPECT_EQ(loop.frames[i].steps, 1);
  }
}

/* A computer slower than the FPS Limit (every frame takes longer than its period): frames get the
 * steps they are due by nature (1, 2, 1, 2... at 40 fps with 60 steps), the game keeps real speed,
 * and what a frame shows one step late (Smooth Motion Interpolate) moves on with the clock frame by
 * frame. Lining the steps up at each late frame used to drop the leftover: slow motion. */
TEST(KX_FrameScheduler, ASlowComputerKeepsRealSpeedAndSmoothDrawing)
{
  const double step = 1.0 / 60.0;
  for (const double render : {0.0185, 0.025, 0.04}) {
    LoopSimulation loop;
    loop.vsync = false;
    loop.render = render;
    loop.wake_late = 0.0005;
    loop.scheduler.Configure(LimitSettings(60.0, 60.0));
    loop.RunFor(10.0);
    EXPECT_NEAR(double(loop.scheduler.GetTick()), 600.0, 3.0) << "render " << render;
    const std::vector<LoopSimulation::Frame> frames = loop.FramesSince(0.5);
    ASSERT_GT(frames.size(), 200u);
    for (size_t i = 1; i < frames.size(); i++) {
      const double shown = (double(frames[i].tick) - 1.0 + frames[i].interpolation) * step;
      const double shown_before = (double(frames[i - 1].tick) - 1.0 +
                                   frames[i - 1].interpolation) *
                                  step;
      EXPECT_NEAR(shown - shown_before, frames[i].begin - frames[i - 1].begin, 1e-6)
          << "render " << render << " frame " << i;
    }
  }
}

TEST(KX_FrameScheduler, WholeRatiosKeepTheirPattern)
{
  /* Two steps a frame. */
  {
    LoopSimulation loop;
    loop.wake_late = 0.002;
    loop.scheduler.Configure(LimitSettings(120.0, 60.0));
    loop.RunFor(5.0);
    EXPECT_EQ(CountFramesWithout(loop.FramesSince(0.1), 2), 0);
  }
  /* Two frames a step: steps on every other frame, never two frames in a row. */
  {
    LoopSimulation loop;
    loop.refresh = 1.0 / 240.0;
    loop.wake_late = 0.0005;
    loop.scheduler.Configure(LimitSettings(60.0, 120.0));
    loop.RunFor(5.0);
    const std::vector<LoopSimulation::Frame> frames = loop.FramesSince(0.1);
    for (size_t i = 1; i < frames.size(); i++) {
      EXPECT_EQ(frames[i].steps + frames[i - 1].steps, 1) << i;
    }
  }
  /* Half speed (slow motion): a step every other frame. */
  {
    LoopSimulation loop;
    loop.time_scale = 0.5;
    loop.wake_late = 0.001;
    loop.scheduler.Configure(LimitSettings(60.0, 60.0));
    loop.RunFor(5.0);
    const std::vector<LoopSimulation::Frame> frames = loop.FramesSince(0.1);
    for (size_t i = 1; i < frames.size(); i++) {
      EXPECT_EQ(frames[i].steps + frames[i - 1].steps, 1) << i;
    }
  }
}

TEST(KX_FrameScheduler, StepsBetweenFramesRunOnTime)
{
  KX_FrameScheduler scheduler;
  scheduler.Configure(LimitSettings(120.0, 60.0));
  /* The first pass draws and lines up; the next step is due half a frame later, between the
   * frames, and the one after it with the next frame. */
  KX_FrameScheduler::Pass pass = scheduler.BeginPass(1.0, 1.0);
  scheduler.EndPass(pass.steps);
  EXPECT_TRUE(pass.frame);
  const double step = 1.0 / 120.0;
  EXPECT_NEAR(scheduler.NextWorkTime(1.0), 1.0 + step, 1e-9);
  pass = scheduler.BeginPass(1.0 + step, 1.0);
  scheduler.EndPass(pass.steps);
  EXPECT_FALSE(pass.frame);
  EXPECT_EQ(pass.steps, 1);
  EXPECT_NEAR(scheduler.NextWorkTime(1.0 + step), 1.0 + 2.0 * step, 1e-9);
}

TEST(KX_FrameScheduler, TheTickCountsTheStepsThatRan)
{
  KX_FrameScheduler scheduler;
  scheduler.Configure(LimitSettings(60.0, 60.0));
  scheduler.EndPass(scheduler.BeginPass(0.0, 1.0).steps);
  KX_FrameScheduler::Pass pass = scheduler.BeginPass(0.1, 1.0);
  EXPECT_TRUE(pass.frame);
  EXPECT_EQ(pass.steps, 5);
  /* A pause asked for during the second step stopped the rest. */
  scheduler.EndPass(2);
  EXPECT_EQ(scheduler.GetTick(), 2u);
  EXPECT_EQ(scheduler.GetLastFrameSteps(), 2);
  scheduler.ResetTick();
  EXPECT_EQ(scheduler.GetTick(), 0u);
}

TEST(KX_FrameScheduler, AClockSetBackALongWayStartsAgain)
{
  KX_FrameScheduler scheduler;
  scheduler.Configure(LimitSettings(60.0, 60.0));
  for (int i = 0; i < 100; i++) {
    const double now = 10.0 + i / 60.0;
    scheduler.EndPass(scheduler.BeginPass(now, 1.0).steps);
  }
  const KX_FrameScheduler::Pass pass = scheduler.BeginPass(2.0, 1.0);
  EXPECT_TRUE(pass.frame);
  EXPECT_EQ(pass.steps, 0);
  EXPECT_DOUBLE_EQ(pass.time, 2.0);
}

TEST(KX_RefreshEstimator, MeasuresThePeriodThroughNoiseAndMissedRefreshes)
{
  KX_RefreshEstimator estimator;
  std::mt19937 random(3);
  const double period = 1.0 / 99.99;
  double refresh_number = 0.0;
  EXPECT_EQ(estimator.Period(), 0.0);
  for (int i = 0; i < 200; i++) {
    /* Every 20th frame missed a refresh. */
    refresh_number += (i % 20 == 19) ? 2.0 : 1.0;
    const double noise = std::uniform_real_distribution<double>(-0.001, 0.001)(random);
    estimator.AddFrame(0.5 + refresh_number * period + noise);
  }
  EXPECT_NEAR(estimator.Period(), period, 0.005 * period);
  /* A long gap starts the grid again. */
  estimator.AddFrame(10.0);
  EXPECT_EQ(estimator.Period(), 0.0);
}

TEST(KX_RefreshEstimator, FramesNotPacedByTheScreenGiveNoPeriod)
{
  KX_RefreshEstimator estimator;
  std::mt19937 random(5);
  double time = 0.0;
  for (int i = 0; i < 300; i++) {
    time += std::uniform_real_distribution<double>(0.004, 0.03)(random);
    estimator.AddFrame(time);
  }
  EXPECT_EQ(estimator.Period(), 0.0);
}

/* No FPS Limit, vsync on: the screen sets the pace and frames come back with noise. Frame times
 * follow the screen's refresh grid (the jitter fix), so 60 steps on a 60 Hz screen are one step
 * a frame, and on a 59.94 Hz screen the rare catch-up is one clean double step, never a frame
 * without a step next to a frame with two. */
TEST(KX_FrameScheduler, VsyncPacingIgnoresWakeUpNoise)
{
  for (const double hz : {60.0, 59.94}) {
    LoopSimulation loop;
    loop.refresh = 1.0 / hz;
    loop.hand_over_noise = 0.0015;
    KX_FrameScheduler::Settings settings;
    settings.step = 1.0 / 60.0;
    settings.limit = false;
    settings.vsync = true;
    loop.scheduler.Configure(settings);
    loop.RunFor(40.0);
    const std::vector<LoopSimulation::Frame> frames = loop.FramesSince(3.0);
    ASSERT_GT(frames.size(), 2000u);
    int doubles = 0;
    for (size_t i = 1; i < frames.size(); i++) {
      EXPECT_FALSE(frames[i].steps == 0 && frames[i - 1].steps == 2) << hz << " frame " << i;
      EXPECT_FALSE(frames[i].steps == 2 && frames[i - 1].steps == 0) << hz << " frame " << i;
      EXPECT_LE(frames[i].steps, 2);
      doubles += frames[i].steps == 2;
    }
    if (hz == 60.0) {
      EXPECT_EQ(CountFramesWithout(frames, 1), 0);
    }
    else {
      /* 0.06 extra steps a second must come in: about two in 37 seconds. */
      EXPECT_LE(doubles, 4);
    }
    /* Frame times never run more than half a step off the clock. */
    EXPECT_LE(std::abs(loop.scheduler.GetLastFrameClockOffset()), 0.5 / 60.0 + 1e-9);
    EXPECT_NEAR(loop.scheduler.GetRefreshRate(), hz, 0.002 * hz);
  }
}

/* A 144 Hz screen with 60 steps: frames get 0 or 1 steps by nature, but on the screen's grid, in
 * an even pattern (every 12 frames hold 5 steps), not shuffled by the noise. */
TEST(KX_FrameScheduler, VsyncFrameTimesStayOnTheRefreshGrid)
{
  LoopSimulation loop;
  loop.refresh = 1.0 / 144.0;
  loop.render = 0.003;
  loop.hand_over_noise = 0.0015;
  KX_FrameScheduler::Settings settings;
  settings.limit = false;
  settings.vsync = true;
  loop.scheduler.Configure(settings);
  loop.RunFor(20.0);
  const std::vector<LoopSimulation::Frame> frames = loop.FramesSince(3.0);
  ASSERT_GT(frames.size(), 2000u);
  for (size_t i = 1; i < frames.size(); i++) {
    const double refreshes = (frames[i].time - frames[i - 1].time) * 144.0;
    EXPECT_NEAR(refreshes, std::round(refreshes), 0.01) << i;
  }
  for (size_t i = 12; i < frames.size(); i++) {
    int steps = 0;
    for (size_t j = i - 12; j < i; j++) {
      steps += frames[j].steps;
    }
    EXPECT_GE(steps, 4) << i;
    EXPECT_LE(steps, 6) << i;
  }
}

TEST(KX_FrameScheduler, VsyncResumeHasNoFlips)
{
  LoopSimulation loop;
  loop.hand_over_noise = 0.0015;
  KX_FrameScheduler::Settings settings;
  settings.limit = false;
  settings.vsync = true;
  loop.scheduler.Configure(settings);
  loop.RunFor(3.0);
  std::mt19937 random(13);
  for (int cycle = 0; cycle < 15; cycle++) {
    loop.time_scale = 0.0;
    loop.scheduler.SetPaused(true);
    loop.RunFor(0.2 + std::uniform_real_distribution<double>(0.0, 0.5)(random));
    loop.time_scale = 1.0;
    loop.scheduler.SetPaused(false);
    const size_t resumed_at = loop.frames.size();
    loop.RunFor(1.5);
    for (size_t i = resumed_at + 1; i < loop.frames.size(); i++) {
      EXPECT_EQ(loop.frames[i].steps, 1) << "cycle " << cycle << " frame " << i - resumed_at;
    }
  }
}

TEST(KX_FrameScheduler, NoJitterFixMeansFrameTimesAreTheClock)
{
  LoopSimulation loop;
  loop.hand_over_noise = 0.0015;
  KX_FrameScheduler::Settings settings;
  settings.limit = false;
  settings.vsync = true;
  settings.jitter_fix = 0.0;
  loop.scheduler.Configure(settings);
  loop.RunFor(3.0);
  for (const LoopSimulation::Frame &frame : loop.frames) {
    EXPECT_DOUBLE_EQ(frame.time, frame.begin);
  }
}

/* The overlay's Steps/frame line with the FPS Limit at the steps' rate: one step in each of 60
 * frames, the same count every frame (it is read every frame, so it must not flicker 59/60/61). */
TEST(KX_FrameStepCounts, AnEvenLimitReadsOneStepInEverySixtyFrames)
{
  LoopSimulation loop;
  loop.vsync = false;
  loop.wake_late = 0.002;
  loop.scheduler.Configure(LimitSettings(60.0, 60.0));
  loop.RunFor(4.0);
  KX_FrameStepCounts counts;
  for (const LoopSimulation::Frame &frame : loop.frames) {
    counts.AddFrame(frame.time, frame.steps);
    if (frame.time >= 1.5) {
      EXPECT_EQ(counts.Text(), "1:60") << frame.time;
    }
  }
}

/* 60 steps on a 100 Hz screen paced by vsync: 40 frames a second show no new step and 60 show
 * one (the pattern Smooth Motion Interpolate smooths), never two. */
TEST(KX_FrameStepCounts, VsyncAtAHundredHertzReadsFortyEmptyAndSixtyOneStepFrames)
{
  LoopSimulation loop;
  loop.refresh = 1.0 / 100.0;
  loop.render = 0.004;
  loop.hand_over_noise = 0.0015;
  KX_FrameScheduler::Settings settings;
  settings.limit = false;
  settings.vsync = true;
  loop.scheduler.Configure(settings);
  loop.RunFor(6.0);
  KX_FrameStepCounts counts;
  for (const LoopSimulation::Frame &frame : loop.frames) {
    counts.AddFrame(frame.time, frame.steps);
    if (frame.begin >= 3.0) {
      const std::array<int, KX_FrameStepCounts::Kinds> &kinds = counts.GetCounts();
      EXPECT_NEAR(kinds[0], 40, 1) << frame.begin;
      EXPECT_NEAR(kinds[1], 60, 1) << frame.begin;
      EXPECT_EQ(kinds[2] + kinds[3], 0) << frame.begin;
    }
  }
}

TEST(KX_FrameStepCounts, ShowsUnevenStepsAndForgetsFramesOlderThanASecond)
{
  KX_FrameStepCounts counts;
  EXPECT_EQ(counts.Text(), "-");
  /* The old stutter: 0, 2, 1, 1 steps, then a 5-step catch-up. */
  const int steps[] = {0, 2, 1, 1, 5};
  for (int i = 0; i < 5; i++) {
    counts.AddFrame(i / 60.0, steps[i]);
  }
  EXPECT_EQ(counts.Text(), "0:1 1:2 2:1 3+:1");
  /* A second later only the newest frame counts. */
  counts.AddFrame(4.0 / 60.0 + 1.0, 1);
  EXPECT_EQ(counts.Text(), "1:1");
  /* A clock set back starts the counts again. */
  counts.AddFrame(0.5, 0);
  EXPECT_EQ(counts.Text(), "0:1");
  counts.Reset();
  EXPECT_EQ(counts.Text(), "-");
}

TEST(KX_FrameStepCounts, PacingTextNamesWhatPacesTheFrames)
{
  EXPECT_EQ(KX_FramePacingText("LIMIT", 60.0, 0.0), "FPS Limit 60");
  EXPECT_EQ(KX_FramePacingText("VSYNC", 0.0, 100.0239), "Vsync, frames 100.0 Hz");
  EXPECT_EQ(KX_FramePacingText("VSYNC", 0.0, 0.0), "Vsync");
  EXPECT_EQ(KX_FramePacingText("FREE", 0.0, 0.0), "Free");
  EXPECT_EQ(KX_FramePacingText("VARIABLE", 0.0, 0.0), "Variable timestep");
}

}  // namespace
