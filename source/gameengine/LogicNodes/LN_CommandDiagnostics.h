/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_CommandDiagnostics.h
 *  \ingroup logicnodes
 */

#pragma once

#include <string>

#include "LN_CommandBuffer.h"

const char *LN_CommandTypeName(LN_CommandBuffer::CommandType type);
const char *LN_CommandSubsystemName(LN_CommandBuffer::CommandSubsystem subsystem);
std::string LN_DescribeCommandForDiagnostics(const LN_CommandBuffer::Command &command);
std::string LN_DescribeCommandFailure(const LN_CommandBuffer::Command &command,
                                      const std::string &reason);
