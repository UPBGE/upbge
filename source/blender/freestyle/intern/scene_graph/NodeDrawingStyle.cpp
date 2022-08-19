/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 * \brief Class to define a Drawing Style to be applied to the underlying children. Inherits from
 * NodeGroup.
 */

#include "NodeDrawingStyle.h"

namespace Freestyle {

void NodeDrawingStyle::accept(SceneVisitor &v)
{
  v.visitNodeDrawingStyle(*this);

  v.visitNodeDrawingStyleBefore(*this);
  v.visitDrawingStyle(_DrawingStyle);
  for (vector<Node *>::iterator node = _Children.begin(), end = _Children.end(); node != end;
       ++node) {
    (*node)->accept(v);
  }
  v.visitNodeDrawingStyleAfter(*this);
}

} /* namespace Freestyle */
