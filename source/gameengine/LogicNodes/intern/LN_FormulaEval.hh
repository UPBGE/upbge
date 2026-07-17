/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_FormulaEval.hh
 *  \ingroup logicnodes
 */

#pragma once

#include <string>

namespace LN {

/** Evaluate uplogic-style math formulas with variables `a` and `b`. */
float EvaluateFormula(const std::string &formula, float a, float b);

}  // namespace LN
