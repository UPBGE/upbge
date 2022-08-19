/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup GHOST
 */

/**
 * Copyright (C) 2001 NaN Technologies B.V.
 */

#include "GHOST_ModifierKeys.h"

GHOST_ModifierKeys::GHOST_ModifierKeys()
{
  clear();
}

GHOST_ModifierKeys::~GHOST_ModifierKeys()
{
}

GHOST_TKey GHOST_ModifierKeys::getModifierKeyCode(GHOST_TModifierKey mask)
{
  GHOST_TKey key;
  switch (mask) {
    case GHOST_kModifierKeyLeftShift:
      key = GHOST_kKeyLeftShift;
      break;
    case GHOST_kModifierKeyRightShift:
      key = GHOST_kKeyRightShift;
      break;
    case GHOST_kModifierKeyLeftAlt:
      key = GHOST_kKeyLeftAlt;
      break;
    case GHOST_kModifierKeyRightAlt:
      key = GHOST_kKeyRightAlt;
      break;
    case GHOST_kModifierKeyLeftControl:
      key = GHOST_kKeyLeftControl;
      break;
    case GHOST_kModifierKeyRightControl:
      key = GHOST_kKeyRightControl;
      break;
    case GHOST_kModifierKeyOS:
      key = GHOST_kKeyOS;
      break;
    default:
      // Should not happen
      key = GHOST_kKeyUnknown;
      break;
  }
  return key;
}

bool GHOST_ModifierKeys::get(GHOST_TModifierKey mask) const
{
  switch (mask) {
    case GHOST_kModifierKeyLeftShift:
      return m_LeftShift;
    case GHOST_kModifierKeyRightShift:
      return m_RightShift;
    case GHOST_kModifierKeyLeftAlt:
      return m_LeftAlt;
    case GHOST_kModifierKeyRightAlt:
      return m_RightAlt;
    case GHOST_kModifierKeyLeftControl:
      return m_LeftControl;
    case GHOST_kModifierKeyRightControl:
      return m_RightControl;
    case GHOST_kModifierKeyOS:
      return m_OS;
    default:
      return false;
  }
}

void GHOST_ModifierKeys::set(GHOST_TModifierKey mask, bool down)
{
  switch (mask) {
    case GHOST_kModifierKeyLeftShift:
      m_LeftShift = down;
      break;
    case GHOST_kModifierKeyRightShift:
      m_RightShift = down;
      break;
    case GHOST_kModifierKeyLeftAlt:
      m_LeftAlt = down;
      break;
    case GHOST_kModifierKeyRightAlt:
      m_RightAlt = down;
      break;
    case GHOST_kModifierKeyLeftControl:
      m_LeftControl = down;
      break;
    case GHOST_kModifierKeyRightControl:
      m_RightControl = down;
      break;
    case GHOST_kModifierKeyOS:
      m_OS = down;
      break;
    default:
      break;
  }
}

void GHOST_ModifierKeys::clear()
{
  m_LeftShift = false;
  m_RightShift = false;
  m_LeftAlt = false;
  m_RightAlt = false;
  m_LeftControl = false;
  m_RightControl = false;
  m_OS = false;
}

bool GHOST_ModifierKeys::equals(const GHOST_ModifierKeys &keys) const
{
  return (m_LeftShift == keys.m_LeftShift) && (m_RightShift == keys.m_RightShift) &&
         (m_LeftAlt == keys.m_LeftAlt) && (m_RightAlt == keys.m_RightAlt) &&
         (m_LeftControl == keys.m_LeftControl) && (m_RightControl == keys.m_RightControl) &&
         (m_OS == keys.m_OS);
}
