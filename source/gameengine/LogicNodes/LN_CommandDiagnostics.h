/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_CommandDiagnostics.h
 *  \ingroup logicnodes
 */

#pragma once

#include <string>
#include <vector>

#include "LN_CommandBuffer.h"

const char *LN_CommandTypeName(LN_CommandBuffer::CommandType type);
const char *LN_CommandSubsystemName(LN_CommandBuffer::CommandSubsystem subsystem);
std::string LN_DescribeCommandForDiagnostics(const LN_CommandBuffer::Command &command);
std::string LN_DescribeCommandFailure(const LN_CommandBuffer::Command &command,
                                      const std::string &reason);
/** Emit each fully contextualized command failure only once per process. Thread-safe. */
void LN_WarnCommandFailureOnce(const LN_CommandBuffer::Command &command,
                               const std::string &reason);
/** Emit `message` as a warning once per process (the set LN_WarnCommandFailureOnce keeps; a
 * message without a payload, so a value changing every step warns once). Thread-safe. True
 * when it was printed now. */
bool LN_WarnOnce(const std::string &message);
/** Tests: forget the warnings issued, so a run prints its own whatever ran before it. */
void LN_ResetCommandFailureWarningsForTests();
/** Tests: the warnings issued since the last reset (LN_WarnCommandFailureOnce, LN_WarnOnce). */
std::vector<std::string> LN_IssuedWarningsForTests();
