#pragma once

#include <chrono>

/** The game's clock: seconds since Reset, from the steady clock. high_resolution_clock is the
 * wall clock in libstdc++ (Linux), which jumps when the system time is set; game timing must
 * never jump. On Windows and macOS high_resolution_clock already was the steady clock. */
class CM_Clock {
 public:
  using Rep = std::chrono::nanoseconds::rep;

 private:
  std::chrono::steady_clock::time_point m_start;

 public:
  CM_Clock();

  void Reset();

  double GetTimeSecond() const;
  Rep GetTimeNano() const;
};
