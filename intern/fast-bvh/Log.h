#ifndef Log_h
#define Log_h

#include <cstdio>

#define LOG_LEVEL 100

#if LOG_LEVEL > 2
 #define LOG_INFO(...) { printf("[Info] " __VA_ARGS__); printf("\n"); }
#else
 #define LOG_INFO(...)
#endif

#if LOG_LEVEL > 2
 #define LOG_STAT(...) { printf("[Statistic] " __VA_ARGS__); printf("\n"); }
#else
 #define LOG_STAT(...) ;
#endif

#if LOG_LEVEL > 1
 #define LOG_WARNING(...) { printf("[Warning] " __VA_ARGS__); printf("\n"); }
#else
 #define LOG_WARNING(...) ;
#endif

#if LOG_LEVEL > 0
 #define LOG_ERROR(...) { printf( "[Error] " __VA_ARGS__); printf("\n"); }
#else
 #define LOG_ERROR(...) ;
#endif


#endif
