/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file SG_QList.h
 *  \ingroup bgesg
 */

#pragma once

#include "SG_DList.h"

/**
 * Double-Double circular linked list
 * For storing an object is two lists simultaneously
 */
class SG_QList : public SG_DList {
 protected:
  SG_QList *m_fqlink;
  SG_QList *m_bqlink;

 public:
  template<typename T> class iterator {
   private:
    SG_QList &m_head;
    T *m_current;

   public:
    typedef iterator<T> _myT;
    iterator(SG_QList &head, SG_QList *current = nullptr) : m_head(head)
    {
      m_current = (T *)current;
    }
    ~iterator()
    {
    }

    void begin()
    {
      m_current = (T *)m_head.QPeek();
    }
    void back()
    {
      m_current = (T *)m_head.QBack();
    }
    bool end()
    {
      return (m_current == (T *)m_head.Self());
    }
    bool add_back(T *item)
    {
      return m_current->QAddBack(item);
    }
    T *operator*()
    {
      return m_current;
    }
    _myT &operator++()
    {
      m_current = (T *)m_current->QPeek();
      return *this;
    }
    _myT &operator--()
    {
      // no check on nullptr! make sure you don't try to increment beyond end
      m_current = (T *)m_current->QBack();
      return *this;
    }
  };

  SG_QList() : SG_DList()
  {
    m_fqlink = m_bqlink = this;
  }
  SG_QList(const SG_QList &other) : SG_DList()
  {
    m_fqlink = m_bqlink = this;
  }
  virtual ~SG_QList()
  {
    QDelink();
  }

  inline bool QEmpty()  // Check for empty queue
  {
    return (m_fqlink == this);
  }
  bool QAddBack(SG_QList *item)  // Add to the back
  {
    if (!item->QEmpty()) {
      return false;
    }
    item->m_bqlink = m_bqlink;
    item->m_fqlink = this;
    m_bqlink->m_fqlink = item;
    m_bqlink = item;
    return true;
  }
  bool QAddFront(SG_QList *item)  // Add to the back
  {
    if (!item->Empty()) {
      return false;
    }
    item->m_fqlink = m_fqlink;
    item->m_bqlink = this;
    m_fqlink->m_bqlink = item;
    m_fqlink = item;
    return true;
  }
  SG_QList *QRemove()  // Remove from the front
  {
    if (QEmpty()) {
      return nullptr;
    }
    SG_QList *item = m_fqlink;
    m_fqlink = item->m_fqlink;
    m_fqlink->m_bqlink = this;
    item->m_fqlink = item->m_bqlink = item;
    return item;
  }
  bool QDelink()  // Remove from the middle
  {
    if (QEmpty()) {
      return false;
    }
    m_bqlink->m_fqlink = m_fqlink;
    m_fqlink->m_bqlink = m_bqlink;
    m_fqlink = m_bqlink = this;
    return true;
  }
  inline SG_QList *QPeek()  // Look at front without removing
  {
    return m_fqlink;
  }
  inline SG_QList *QBack()  // Look at front without removing
  {
    return m_bqlink;
  }
};
