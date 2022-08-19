/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GHOST_NDOFManager.h"

// Event capture is handled within the NDOF manager on Macintosh,
// so there's no need for SystemCocoa to look for them.

class GHOST_NDOFManagerCocoa : public GHOST_NDOFManager {
 public:
  GHOST_NDOFManagerCocoa(GHOST_System &);
  ~GHOST_NDOFManagerCocoa();

  bool available();
};
