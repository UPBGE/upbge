/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_StateBlock.h
 *  \ingroup logicnodes
 *
 * One memory block for a runtime tree's per-object state (2026-10-31): every state array of a
 * tree lives in one allocation laid out by its program (LN_TreeStateLayout), instead of a list of
 * its own each. The arrays are reached through LN_StateSpan views into the block.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

/** A view of `size` elements of a state array in a tree's state block. The block owns them. */
template<typename T> class LN_StateSpan {
 public:
  LN_StateSpan() = default;
  LN_StateSpan(T *data, const uint32_t size) : m_data(data), m_size(size) {}

  uint32_t size() const
  {
    return m_size;
  }
  bool empty() const
  {
    return m_size == 0;
  }
  T *data() const
  {
    return m_data;
  }
  T &operator[](const size_t index) const
  {
    return m_data[index];
  }
  T *begin() const
  {
    return m_data;
  }
  T *end() const
  {
    return m_data + m_size;
  }

 private:
  T *m_data = nullptr;
  uint32_t m_size = 0;
};

/** The bytes of a state block, aligned to a cache line. Elements are constructed and destroyed
 * by the block's owner, which knows their types. */
class LN_StateBlockMemory {
 public:
  static constexpr size_t kAlignment = 64;

  LN_StateBlockMemory() = default;
  explicit LN_StateBlockMemory(const size_t size)
  {
    if (size != 0) {
      m_bytes = static_cast<std::byte *>(::operator new(size, std::align_val_t(kAlignment)));
      m_size = size;
    }
  }
  LN_StateBlockMemory(const LN_StateBlockMemory &) = delete;
  LN_StateBlockMemory &operator=(const LN_StateBlockMemory &) = delete;
  LN_StateBlockMemory(LN_StateBlockMemory &&other) noexcept
      : m_bytes(std::exchange(other.m_bytes, nullptr)), m_size(std::exchange(other.m_size, 0))
  {
  }
  LN_StateBlockMemory &operator=(LN_StateBlockMemory &&other) noexcept
  {
    if (this != &other) {
      Release();
      m_bytes = std::exchange(other.m_bytes, nullptr);
      m_size = std::exchange(other.m_size, 0);
    }
    return *this;
  }
  ~LN_StateBlockMemory()
  {
    Release();
  }

  std::byte *Data() const
  {
    return m_bytes;
  }
  size_t Size() const
  {
    return m_size;
  }

 private:
  void Release()
  {
    if (m_bytes != nullptr) {
      ::operator delete(m_bytes, std::align_val_t(kAlignment));
      m_bytes = nullptr;
      m_size = 0;
    }
  }

  std::byte *m_bytes = nullptr;
  size_t m_size = 0;
};
