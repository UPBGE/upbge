/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file SG_Controller.h
 *  \ingroup bgesg
 */

#pragma once

#include <stddef.h>

class SG_Node;

/**
 * A scenegraph controller
 */
class SG_Controller {
 public:
  SG_Controller() : m_node(nullptr)
  {
  }

  virtual ~SG_Controller()
  {
  }

  virtual bool Update(double time) = 0;

  virtual void SetNode(SG_Node *node);

  void ClearNode();

  virtual void SetSimulatedTime(double time) = 0;

  virtual SG_Controller *GetReplica(SG_Node *destnode) = 0;

  /**
   * Hacky way of passing options to specific controllers
   * \param option An integer identifying the option.
   * \param value  The value of this option.
   * \attention This has been placed here to give sca-elements
   * \attention some control over the controllers. This is
   * \attention necessary because the identity of the controller
   * \attention is lost on the way here.
   */
  virtual void SetOption(int option, int value) = 0;

  /**
   * Option-identifiers: SG_CONTR_<controller-type>_<option>.
   * Options only apply to a specific controller type. The
   * semantics are defined by whoever uses the setting.
   */
  enum SG_Controller_option {
    SG_CONTR_NODEF = 0,
    SG_CONTR_IPO_IPO_AS_FORCE,
    SG_CONTR_IPO_IPO_ADD,
    SG_CONTR_IPO_LOCAL,
    SG_CONTR_IPO_RESET,
    SG_CONTR_CAMIPO_LENS,
    SG_CONTR_CAMIPO_CLIPEND,
    SG_CONTR_CAMIPO_CLIPSTART,
    SG_CONTR_MAX
  };

 protected:
  SG_Node *m_node;
};
