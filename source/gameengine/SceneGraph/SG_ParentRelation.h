/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file SG_ParentRelation.h
 *  \ingroup bgesg
 * \page SG_ParentRelationPage SG_ParentRelation
 *
 * \section SG_ParentRelationSection SG_ParentRelation
 *
 * This is an abstract interface class to the Scene Graph library.
 * It allows you to specify how child nodes react to parent nodes.
 * Normally a child will use it's parent's transforms to compute
 * it's own global transforms. How this is performed depends on
 * the type of relation. For example if the parent is a vertex
 * parent to this child then the child should not inherit any
 * rotation information from the parent. Or if the parent is a
 * 'slow parent' to this child then the child should react
 * slowly to changes in the parent's position. The exact relation
 * is left for you to implement by filling out this interface
 * with concrete examples.
 *
 * There is exactly one SG_ParentRelation per SG_Node. Subclasses
 * should not be value types and should be allocated on the heap.
 *
 */

#pragma once

class SG_Node;

class SG_ParentRelation {
 public:
  /**
   * Update the childs local and global coordinates
   * based upon the parents global coordinates.
   * You must also handle the case when this node has no
   * parent (parent == nullptr). Usually you should just
   * copy the local coordinates of the child to the
   * world coordinates.
   */
  virtual bool UpdateChildCoordinates(SG_Node *child,
                                      const SG_Node *parent,
                                      bool &parentUpdated) = 0;

  virtual ~SG_ParentRelation()
  {
  }

  /**
   * You must provide a way of duplicating an
   * instance of an SG_ParentRelation. This should
   * return a pointer to a new duplicate allocated
   * on the heap. Responsibility for deleting the
   * duplicate resides with the caller of this method.
   */
  virtual SG_ParentRelation *NewCopy() = 0;

  /**
   * Vertex Parent Relation are special: they don't propagate rotation
   */
  virtual bool IsVertexRelation()
  {
    return false;
  }

  /**
   * Need this to see if we are able to adjust time-offset from the python api
   */
  virtual bool IsSlowRelation()
  {
    return false;
  }

 protected:
  /**
   * Protected constructors
   * this class is not meant to be instantiated.
   */

  SG_ParentRelation()
  {
  }

  /**
   * Copy construction should not be implemented
   */
  SG_ParentRelation(const SG_ParentRelation &);
};
