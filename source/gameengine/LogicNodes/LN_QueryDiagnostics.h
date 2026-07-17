/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_QueryDiagnostics.h
 *  \ingroup logicnodes
 */

#pragma once

#include <string>

#include "LN_Types.h"

const char *LN_QueryDiagnosticStatusName(LN_QueryDiagnosticStatus status);
std::string LN_DescribePhysicsQueryResult(const LN_PhysicsQueryResult &result);
