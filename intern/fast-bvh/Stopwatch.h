#ifndef Stopwatch_h
#define Stopwatch_h

#include <sys/time.h>

class Stopwatch {
  private:
    double start;
    double _stopwatch() const
    {
      struct timeval time;
      gettimeofday(&time, 0 );
      return 1.0 * time.tv_sec + time.tv_usec / (double)1e6;
    }
  public:
    Stopwatch() { reset(); }
    void reset() { start = _stopwatch(); }
    double read() const { return _stopwatch() - start; }
};

#endif
