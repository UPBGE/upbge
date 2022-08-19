/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Tristan Porteries */

/** \file gameengine/SceneGraph/SG_Familly.cpp
 *  \ingroup bgesg
 */

#include "SG_Familly.h"

CM_ThreadSpinLock &SG_Familly::GetMutex()
{
  return m_mutex;
}
