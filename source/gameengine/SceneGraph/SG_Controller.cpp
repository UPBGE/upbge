/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file gameengine/SceneGraph/SG_Controller.cpp
 *  \ingroup bgesg
 */

#include "SG_Controller.h"

void SG_Controller::SetNode(SG_Node *node)
{
  m_node = node;  // no checks yet ?
}

void SG_Controller::ClearNode()
{
  m_node = nullptr;
}
