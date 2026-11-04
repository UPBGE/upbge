#pragma once

#include <chrono>

class CM_Clock {
 public:
  using Rep = std::chrono::nanoseconds::rep;

 private:
  /* steady_clock: libstdc++ aliases high_resolution_clock to system_clock, which is not
   * monotonic, so wall-clock steps (NTP, manual changes) would leak into frame timing. */
  std::chrono::steady_clock::time_point m_start;
  std::chrono::steady_clock m_clock;

 public:
  CM_Clock();

  void Reset();

  double GetTimeSecond() const;
  Rep GetTimeNano() const;
};
