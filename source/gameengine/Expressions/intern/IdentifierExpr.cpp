/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Expressions/IdentifierExpr.cpp
 *  \ingroup expressions
 */

#include "EXP_IdentifierExpr.h"

EXP_IdentifierExpr::EXP_IdentifierExpr(const std::string &identifier, EXP_Value *id_context)
    : m_identifier(identifier)
{
  if (id_context) {
    m_idContext = id_context->AddRef();
  }
  else {
    m_idContext = nullptr;
  }
}

EXP_IdentifierExpr::~EXP_IdentifierExpr()
{
  if (m_idContext) {
    m_idContext->Release();
  }
}

EXP_Value *EXP_IdentifierExpr::Calculate()
{
  EXP_Value *result = nullptr;
  if (m_idContext) {
    result = m_idContext->FindIdentifier(m_identifier);
  }

  return result;
}

unsigned char EXP_IdentifierExpr::GetExpressionID()
{
  return CIDENTIFIEREXPRESSIONID;
}
