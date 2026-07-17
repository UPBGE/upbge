/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_NodeStateArena.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstddef>
#include <vector>

class LN_NodeStateArena {
 public:
  void Clear()
  {
    m_bytes.clear();
  }

  void Resize(size_t size)
  {
    m_bytes.resize(size, 0);
  }

  unsigned char *Data()
  {
    return m_bytes.data();
  }

  const unsigned char *Data() const
  {
    return m_bytes.data();
  }

  size_t Size() const
  {
    return m_bytes.size();
  }

 private:
  std::vector<unsigned char> m_bytes;
};
