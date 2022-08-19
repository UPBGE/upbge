/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 * \brief Class to represent a group node. This node can contains several children.
 * \brief It also contains a transform matrix indicating the transform state of the underlying
 * children.
 */

#include "NodeGroup.h"

namespace Freestyle {

void NodeGroup::AddChild(Node *iChild)
{
  if (nullptr == iChild) {
    return;
  }

  _Children.push_back(iChild);
  iChild->addRef();
}

int NodeGroup::destroy()
{
  /** Node::destroy makes a release on the object and then returns the reference counter.
   *  If the reference counter is equal to 0, that means that nobody else is linking this node
   * group and that we can destroy the whole underlying tree. Else, one or several Node link this
   * node group, and we only returns the reference counter decremented by Node::destroy();
   */
  int refThis = Node::destroy();

  // if refThis != 0, we can't destroy the tree
  if (0 != refThis) {
    return refThis;
  }

  // If we are here, that means that nobody else needs our NodeGroup and we can destroy it.
  int refCount = 0;
  vector<Node *>::iterator node;

  for (node = _Children.begin(); node != _Children.end(); ++node) {
    refCount = (*node)->destroy();
    if (0 == refCount) {
      delete (*node);
    }
  }

  _Children.clear();

  return refThis;
}

void NodeGroup::accept(SceneVisitor &v)
{
  v.visitNodeGroup(*this);

  v.visitNodeGroupBefore(*this);
  for (vector<Node *>::iterator node = _Children.begin(), end = _Children.end(); node != end;
       ++node) {
    (*node)->accept(v);
  }
  v.visitNodeGroupAfter(*this);
}

void NodeGroup::DetachChildren()
{
  vector<Node *>::iterator node;

  for (node = _Children.begin(); node != _Children.end(); ++node) {
    (*node)->release();
  }

  _Children.clear();
}

void NodeGroup::DetachChild(Node *iChild)
{
  /* int found = 0; */ /* UNUSED */
  vector<Node *>::iterator node;

  for (node = _Children.begin(); node != _Children.end(); ++node) {
    if ((*node) == iChild) {
      (*node)->release();
      _Children.erase(node);
      /* found = 1; */ /* UNUSED */
      break;
    }
  }
}

void NodeGroup::RetrieveChildren(vector<Node *> &oNodes)
{
  oNodes = _Children;
}

const BBox<Vec3r> &NodeGroup::UpdateBBox()
{
  vector<Node *>::iterator node;
  clearBBox();
  for (node = _Children.begin(); node != _Children.end(); ++node) {
    AddBBox((*node)->UpdateBBox());
  }

  return Node::UpdateBBox();
}

} /* namespace Freestyle */
