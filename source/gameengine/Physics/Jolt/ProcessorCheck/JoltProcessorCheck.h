/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file JoltProcessorCheck.h
 *  \ingroup physjolt
 *  \brief Whether this processor has the instructions Jolt was built with (AVX2 and FMA).
 *
 * Jolt is built with the processor's newer instruction sets for speed, and its start-up code
 * (constants made before main()) already uses them: on a processor without them the program would
 * stop with a crash before showing anything. JoltProcessorCheck.cpp runs before that code and says
 * what is missing instead. It lives in its own library, built without those instructions and told
 * which ones Jolt was built with (Jolt's JPH_USE_* definitions).
 */

#pragma once

#include <cstdint>

/** The processor's feature bits the check reads. */
struct JoltProcessorFeatures {
  /** CPUID leaf 1, ECX. */
  uint32_t basic_ecx = 0;
  /** CPUID leaf 7 (sub-leaf 0), EBX; 0 when the processor has no leaf 7. */
  uint32_t extended_ebx = 0;
  /** CPUID leaf 0x80000001, ECX; 0 when the processor has no such leaf. */
  uint32_t amd_ecx = 0;
  /** XGETBV 0: the register states the operating system saves; 0 without OSXSAVE. */
  uint64_t xcr0 = 0;
};

/** This processor's feature bits (all 0 on processors other than x86-64). */
JoltProcessorFeatures JoltReadProcessorFeatures();

/** The instruction sets this build of Jolt uses that `features` lack: names such as "AVX2", at most
 * `capacity` of them written into `missing`. Returns how many it lacks (0: it runs Jolt). */
int JoltMissingInstructions(const JoltProcessorFeatures &features,
                            const char **missing,
                            int capacity);

/** The instruction sets this build of Jolt uses, as JoltMissingInstructions names them. */
int JoltRequiredInstructions(const char **names, int capacity);

/** On a processor that can't run this build of Jolt: says which instructions it lacks (a message
 * box on Windows, the console elsewhere) and ends the program. Runs before main() on Linux and
 * Windows, and again when a scene creates its physics (the only check elsewhere). */
void JoltStopOnUnsupportedProcessor();
