/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Tristan Porteries */

/** \file SG_Familly.h
 *  \ingroup bgesg
 */

#pragma once

#include "CM_Thread.h"

class SG_Familly {
 private:
  CM_ThreadSpinLock m_mutex;

 public:
  SG_Familly() = default;
  ~SG_Familly() = default;

  CM_ThreadSpinLock &GetMutex();
};
