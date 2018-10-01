#ifndef __CM_CLOCK_H__
#define __CM_CLOCK_H__

#include <chrono>

class CM_Clock
{
public:
	using Rep = std::chrono::nanoseconds::rep;

private:
	std::chrono::high_resolution_clock::time_point m_start;
	std::chrono::high_resolution_clock m_clock;

public:
	CM_Clock();

	void Reset();

	double GetTimeSecond() const;
	Rep GetTimeNano() const;
};

#endif  // __CM_CLOCK_H__
