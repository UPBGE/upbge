/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file SG_DList.h
 *  \ingroup bgesg
 */

#pragma once

#include <stdlib.h>

/**
 * Double circular linked list
 */
class SG_DList {
 protected:
  SG_DList *m_flink;
  SG_DList *m_blink;

 public:
  template<typename T> class iterator {
   private:
    SG_DList &m_head;
    T *m_current;

   public:
    typedef iterator<T> _myT;
    iterator(SG_DList &head) : m_head(head), m_current(nullptr)
    {
    }

    ~iterator()
    {
    }

    void begin()
    {
      m_current = (T *)m_head.Peek();
    }
    void back()
    {
      m_current = (T *)m_head.Back();
    }
    bool end()
    {
      return (m_current == (T *)m_head.Self());
    }
    bool add_back(T *item)
    {
      return m_current->AddBack(item);
    }
    T *operator*()
    {
      return m_current;
    }
    _myT &operator++()
    {
      // no check of nullptr! make sure you don't try to increment beyond end
      m_current = (T *)m_current->Peek();
      return *this;
    }
    _myT &operator--()
    {
      // no check of nullptr! make sure you don't try to increment beyond end
      m_current = (T *)m_current->Back();
      return *this;
    }
  };

  template<typename T> class const_iterator {
   private:
    const SG_DList &m_head;
    const T *m_current;

   public:
    typedef const_iterator<T> _myT;
    const_iterator(const SG_DList &head) : m_head(head), m_current(nullptr)
    {
    }

    ~const_iterator()
    {
    }

    void begin()
    {
      m_current = (const T *)m_head.Peek();
    }
    void back()
    {
      m_current = (const T *)m_head.Back();
    }
    bool end()
    {
      return (m_current == (const T *)m_head.Self());
    }
    const T *operator*()
    {
      return m_current;
    }
    _myT &operator++()
    {
      // no check of nullptr! make sure you don't try to increment beyond end
      m_current = (const T *)m_current->Peek();
      return *this;
    }
    _myT &operator--()
    {
      // no check of nullptr! make sure you don't try to increment beyond end
      m_current = (const T *)m_current->Back();
      return *this;
    }
  };

  SG_DList()
  {
    m_flink = m_blink = this;
  }
  SG_DList(const SG_DList &other)
  {
    m_flink = m_blink = this;
  }
  virtual ~SG_DList()
  {
    Delink();
  }

  inline bool Empty()  // Check for empty queue
  {
    return (m_flink == this);
  }
  bool AddBack(SG_DList *item)  // Add to the back
  {
    if (!item->Empty()) {
      return false;
    }
    item->m_blink = m_blink;
    item->m_flink = this;
    m_blink->m_flink = item;
    m_blink = item;
    return true;
  }
  bool AddFront(SG_DList *item)  // Add to the back
  {
    if (!item->Empty()) {
      return false;
    }
    item->m_flink = m_flink;
    item->m_blink = this;
    m_flink->m_blink = item;
    m_flink = item;
    return true;
  }
  SG_DList *Remove()  // Remove from the front
  {
    if (Empty()) {
      return nullptr;
    }
    SG_DList *item = m_flink;
    m_flink = item->m_flink;
    m_flink->m_blink = this;
    item->m_flink = item->m_blink = item;
    return item;
  }
  bool Delink()  // Remove from the middle
  {
    if (Empty()) {
      return false;
    }
    m_blink->m_flink = m_flink;
    m_flink->m_blink = m_blink;
    m_flink = m_blink = this;
    return true;
  }
  inline SG_DList *Peek()  // Look at front without removing
  {
    return m_flink;
  }
  inline SG_DList *Back()  // Look at front without removing
  {
    return m_blink;
  }
  inline SG_DList *Self()
  {
    return this;
  }
  inline const SG_DList *Peek() const  // Look at front without removing
  {
    return (const SG_DList *)m_flink;
  }
  inline const SG_DList *Back() const  // Look at front without removing
  {
    return (const SG_DList *)m_blink;
  }
  inline const SG_DList *Self() const
  {
    return this;
  }
};

/**
 * SG_DListHead : Template class that implements copy constructor to duplicate list automatically
 *                The elements of the list must have themselves a copy constructor.
 */
template<typename T> class SG_DListHead : public SG_DList {
 public:
  typedef SG_DListHead<T> _myT;
  SG_DListHead() : SG_DList()
  {
  }
  SG_DListHead(const _myT &other) : SG_DList()
  {
    // copy the list, assuming objects of type T
    const_iterator<T> eit(other);
    T *elem;
    for (eit.begin(); !eit.end(); ++eit) {
      elem = (*eit)->GetReplica();
      AddBack(elem);
    }
  }
  virtual ~SG_DListHead()
  {
  }
  T *Remove()
  {
    return static_cast<T *>(SG_DList::Remove());
  }
};
