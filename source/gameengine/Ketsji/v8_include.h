/*
 * Include V8 with Windows/Blender macro workarounds.
 * Use this as the first #include in .cpp files that need V8, so that
 * v8-unwinder.h and other V8 headers are not broken by pre-existing macros.
 */
#ifndef KETSJI_V8_INCLUDE_H
#define KETSJI_V8_INCLUDE_H

#if defined(_WIN32) && defined(_MSC_VER)
/* Undef macros that may conflict with V8 (v8-unwinder.h enum/struct names) */
#  ifdef StateTag
#    undef StateTag
#  endif
#  ifdef RegisterState
#    undef RegisterState
#  endif
#  ifdef MemoryRange
#    undef MemoryRange
#  endif
#  ifdef EmbedderStateTag
#    undef EmbedderStateTag
#  endif
#  ifdef IDLE
#    undef IDLE
#  endif
#  ifdef GC
#    undef GC
#  endif
#  ifdef JS
#    undef JS
#  endif
#  ifdef PARSER
#    undef PARSER
#  endif
#  ifdef OTHER
#    undef OTHER
#  endif
#  ifdef EXTERNAL
#    undef EXTERNAL
#  endif
#  ifdef LOGGING
#    undef LOGGING
#  endif
#  ifdef COMPILER
#    undef COMPILER
#  endif
#endif

#include <v8.h>

#endif
