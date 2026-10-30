#include "CM_Clock.h"

CM_Clock::CM_Clock()
{
  Reset();
}

void CM_Clock::Reset()
{
  m_start = std::chrono::steady_clock::now();
}

double CM_Clock::GetTimeSecond() const
{
  return GetTimeNano() * 1e-9;
}

CM_Clock::Rep CM_Clock::GetTimeNano() const
{
  const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_start).count();
}
