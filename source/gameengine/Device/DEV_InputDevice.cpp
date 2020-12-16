/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Device/DEV_InputDevice.cpp
 *  \ingroup device
 */

#include "DEV_InputDevice.h"


#include "GHOST_Types.h"

DEV_InputDevice::DEV_InputDevice()
{
  m_reverseKeyTranslateTable[GHOST_kKeyA] = AKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyB] = BKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyC] = CKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyD] = DKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyE] = EKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF] = FKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyG] = GKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyH] = HKEY_;
  m_reverseKeyTranslateTable[GHOST_kKeyI] = IKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyJ] = JKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyK] = KKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyL] = LKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyM] = MKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyN] = NKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyO] = OKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyP] = PKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyQ] = QKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyR] = RKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyS] = SKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyT] = TKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyU] = UKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyV] = VKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyW] = WKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyX] = XKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyY] = YKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyZ] = ZKEY;

  m_reverseKeyTranslateTable[GHOST_kKey0] = ZEROKEY;
  m_reverseKeyTranslateTable[GHOST_kKey1] = ONEKEY;
  m_reverseKeyTranslateTable[GHOST_kKey2] = TWOKEY;
  m_reverseKeyTranslateTable[GHOST_kKey3] = THREEKEY;
  m_reverseKeyTranslateTable[GHOST_kKey4] = FOURKEY;
  m_reverseKeyTranslateTable[GHOST_kKey5] = FIVEKEY;
  m_reverseKeyTranslateTable[GHOST_kKey6] = SIXKEY;
  m_reverseKeyTranslateTable[GHOST_kKey7] = SEVENKEY;
  m_reverseKeyTranslateTable[GHOST_kKey8] = EIGHTKEY;
  m_reverseKeyTranslateTable[GHOST_kKey9] = NINEKEY;

  // Middle keyboard area keys
  m_reverseKeyTranslateTable[GHOST_kKeyPause] = PAUSEKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyInsert] = INSERTKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyDelete] = DELKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyHome] = HOMEKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyEnd] = ENDKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyUpPage] = PAGEUPKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyDownPage] = PAGEDOWNKEY;

  // Arrow keys
  m_reverseKeyTranslateTable[GHOST_kKeyUpArrow] = UPARROWKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyDownArrow] = DOWNARROWKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyLeftArrow] = LEFTARROWKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyRightArrow] = RIGHTARROWKEY;

  // Function keys
  m_reverseKeyTranslateTable[GHOST_kKeyF1] = F1KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF2] = F2KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF3] = F3KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF4] = F4KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF5] = F5KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF6] = F6KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF7] = F7KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF8] = F8KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF9] = F9KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF10] = F10KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF11] = F11KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF12] = F12KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF13] = F13KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF14] = F14KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF15] = F15KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF16] = F16KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF17] = F17KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF18] = F18KEY;
  m_reverseKeyTranslateTable[GHOST_kKeyF19] = F19KEY;

  // Numpad keys
  m_reverseKeyTranslateTable[GHOST_kKeyNumpad0] = PAD0;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpad1] = PAD1;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpad2] = PAD2;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpad3] = PAD3;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpad4] = PAD4;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpad5] = PAD5;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpad6] = PAD6;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpad7] = PAD7;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpad8] = PAD8;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpad9] = PAD9;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpadAsterisk] = PADASTERKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpadPlus] = PADPLUSKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpadPeriod] = PADPERIOD;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpadMinus] = PADMINUS;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpadSlash] = PADSLASHKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyNumpadEnter] = PADENTER;

  // Other keys
  m_reverseKeyTranslateTable[GHOST_kKeyCapsLock] = CAPSLOCKKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyEsc] = ESCKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyTab] = TABKEY;
  m_reverseKeyTranslateTable[GHOST_kKeySpace] = SPACEKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyEnter] = RETKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyBackSpace] = BACKSPACEKEY;
  m_reverseKeyTranslateTable[GHOST_kKeySemicolon] = SEMICOLONKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyPeriod] = PERIODKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyComma] = COMMAKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyQuote] = QUOTEKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyAccentGrave] = ACCENTGRAVEKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyMinus] = MINUSKEY;
  m_reverseKeyTranslateTable[GHOST_kKeySlash] = SLASHKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyBackslash] = BACKSLASHKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyEqual] = EQUALKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyLeftBracket] = LEFTBRACKETKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyRightBracket] = RIGHTBRACKETKEY;

  m_reverseKeyTranslateTable[GHOST_kKeyOS] = OSKEY;

  // Modifier keys.
  m_reverseKeyTranslateTable[GHOST_kKeyLeftControl] = LEFTCTRLKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyRightControl] = RIGHTCTRLKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyLeftAlt] = LEFTALTKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyRightAlt] = RIGHTALTKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyLeftShift] = LEFTSHIFTKEY;
  m_reverseKeyTranslateTable[GHOST_kKeyRightShift] = RIGHTSHIFTKEY;

  // Mouse buttons.
  m_reverseButtonTranslateTable[GHOST_kButtonMaskMiddle] = MIDDLEMOUSE;
  m_reverseButtonTranslateTable[GHOST_kButtonMaskRight] = RIGHTMOUSE;
  m_reverseButtonTranslateTable[GHOST_kButtonMaskLeft] = LEFTMOUSE;
  m_reverseButtonTranslateTable[GHOST_kButtonMaskButton4] = BUTTON4MOUSE;
  m_reverseButtonTranslateTable[GHOST_kButtonMaskButton5] = BUTTON5MOUSE;
  m_reverseButtonTranslateTable[GHOST_kButtonMaskButton6] = BUTTON6MOUSE;
  m_reverseButtonTranslateTable[GHOST_kButtonMaskButton7] = BUTTON7MOUSE;

  // Window events.
  m_reverseWindowTranslateTable[GHOST_kEventWindowSize] = WINRESIZE;
  m_reverseWindowTranslateTable[GHOST_kEventQuitRequest] = WINQUIT;
  m_reverseWindowTranslateTable[GHOST_kEventWindowClose] = WINCLOSE;

#ifdef WITH_GAMEENGINE_CEGUI
  /* The reverse table. In order to not confuse ourselves, we
   * immediately convert all events that come in to CEGUI codes. */

  // standard keyboard
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyA] = CEGUI::Key::A;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyB] = CEGUI::Key::B;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyC] = CEGUI::Key::C;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyD] = CEGUI::Key::D;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyE] = CEGUI::Key::E;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF] = CEGUI::Key::F;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyG] = CEGUI::Key::G;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyH] = CEGUI::Key::H;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyI] = CEGUI::Key::I;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyJ] = CEGUI::Key::J;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyK] = CEGUI::Key::K;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyL] = CEGUI::Key::L;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyM] = CEGUI::Key::M;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyN] = CEGUI::Key::N;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyO] = CEGUI::Key::O;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyP] = CEGUI::Key::P;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyQ] = CEGUI::Key::Q;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyR] = CEGUI::Key::R;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyS] = CEGUI::Key::S;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyT] = CEGUI::Key::T;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyU] = CEGUI::Key::U;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyV] = CEGUI::Key::V;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyW] = CEGUI::Key::W;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyX] = CEGUI::Key::X;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyY] = CEGUI::Key::Y;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyZ] = CEGUI::Key::Z;

  m_ceguiReverseKeyTranslateTable[GHOST_kKey0] = CEGUI::Key::Zero;
  m_ceguiReverseKeyTranslateTable[GHOST_kKey1] = CEGUI::Key::One;
  m_ceguiReverseKeyTranslateTable[GHOST_kKey2] = CEGUI::Key::Two;
  m_ceguiReverseKeyTranslateTable[GHOST_kKey3] = CEGUI::Key::Three;
  m_ceguiReverseKeyTranslateTable[GHOST_kKey4] = CEGUI::Key::Four;
  m_ceguiReverseKeyTranslateTable[GHOST_kKey5] = CEGUI::Key::Five;
  m_ceguiReverseKeyTranslateTable[GHOST_kKey6] = CEGUI::Key::Six;
  m_ceguiReverseKeyTranslateTable[GHOST_kKey7] = CEGUI::Key::Seven;
  m_ceguiReverseKeyTranslateTable[GHOST_kKey8] = CEGUI::Key::Eight;
  m_ceguiReverseKeyTranslateTable[GHOST_kKey9] = CEGUI::Key::Nine;

  // Middle keyboard area keys
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyPause] = CEGUI::Key::Pause;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyInsert] = CEGUI::Key::Insert;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyDelete] = CEGUI::Key::Delete;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyHome] = CEGUI::Key::Home;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyEnd] = CEGUI::Key::End;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyUpPage] = CEGUI::Key::PageUp;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyDownPage] = CEGUI::Key::PageDown;

  // Arrow keys
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyLeftArrow] = CEGUI::Key::ArrowLeft;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyDownArrow] = CEGUI::Key::ArrowDown;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyRightArrow] = CEGUI::Key::ArrowRight;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyUpArrow] = CEGUI::Key::ArrowUp;

  // Function keys
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF1] = CEGUI::Key::F1;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF2] = CEGUI::Key::F2;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF3] = CEGUI::Key::F3;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF4] = CEGUI::Key::F4;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF5] = CEGUI::Key::F5;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF6] = CEGUI::Key::F6;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF7] = CEGUI::Key::F7;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF8] = CEGUI::Key::F8;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF9] = CEGUI::Key::F9;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF10] = CEGUI::Key::F10;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF11] = CEGUI::Key::F11;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF12] = CEGUI::Key::F12;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF13] = CEGUI::Key::F13;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF14] = CEGUI::Key::F14;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF15] = CEGUI::Key::F15;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF16] = CEGUI::Key::Unknown;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF17] = CEGUI::Key::Unknown;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF18] = CEGUI::Key::Unknown;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyF19] = CEGUI::Key::Unknown;

  // Numpad keys
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpad0] = CEGUI::Key::Numpad0;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpad1] = CEGUI::Key::Numpad1;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpad2] = CEGUI::Key::Numpad2;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpad3] = CEGUI::Key::Numpad3;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpad4] = CEGUI::Key::Numpad4;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpad5] = CEGUI::Key::Numpad5;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpad6] = CEGUI::Key::Numpad6;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpad7] = CEGUI::Key::Numpad7;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpad8] = CEGUI::Key::Numpad8;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpad9] = CEGUI::Key::Numpad9;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpadAsterisk] = CEGUI::Key::Multiply;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpadPlus] = CEGUI::Key::Add;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpadPeriod] = CEGUI::Key::Decimal;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpadMinus] = CEGUI::Key::Subtract;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpadSlash] = CEGUI::Key::Divide;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyNumpadEnter] = CEGUI::Key::NumpadEnter;

  // Other keys
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyCapsLock] = CEGUI::Key::Capital;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyEsc] = CEGUI::Key::Escape;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyTab] = CEGUI::Key::Tab;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyEnter] = CEGUI::Key::Return;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeySpace] = CEGUI::Key::Space;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyBackSpace] = CEGUI::Key::Backspace;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeySemicolon] = CEGUI::Key::Semicolon;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyPeriod] = CEGUI::Key::Period;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyComma] = CEGUI::Key::Comma;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyQuote] = CEGUI::Key::Apostrophe;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyAccentGrave] = CEGUI::Key::Grave;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyMinus] = CEGUI::Key::Minus;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeySlash] = CEGUI::Key::Slash;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyBackslash] = CEGUI::Key::Backslash;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyEqual] = CEGUI::Key::Equals;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyLeftBracket] = CEGUI::Key::LeftBracket;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyRightBracket] = CEGUI::Key::RightBracket;

  // Modifier keys.
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyLeftControl] = CEGUI::Key::LeftControl;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyRightControl] = CEGUI::Key::RightControl;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyLeftAlt] = CEGUI::Key::LeftAlt;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyRightAlt] = CEGUI::Key::RightAlt;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyLeftShift] = CEGUI::Key::LeftShift;
  m_ceguiReverseKeyTranslateTable[GHOST_kKeyRightShift] = CEGUI::Key::RightShift;
#endif
}

DEV_InputDevice::~DEV_InputDevice()
{
}

void DEV_InputDevice::ConvertKeyEvent(int incode, int val, unsigned int unicode)
{
  ConvertEvent(m_reverseKeyTranslateTable[incode], val, unicode);

#ifdef WITH_GAMEENGINE_CEGUI
  ConvertEvent(m_ceguiReverseKeyTranslateTable[incode], val, unicode);
#endif
}

void DEV_InputDevice::ConvertButtonEvent(int incode, int val)
{
  ConvertEvent(m_reverseButtonTranslateTable[incode], val, 0);
}

void DEV_InputDevice::ConvertWindowEvent(int incode)
{
  ConvertEvent(m_reverseWindowTranslateTable[incode], 1, 0);
}

void DEV_InputDevice::ConvertEvent(SCA_IInputDevice::SCA_EnumInputs type,
                                   int val,
                                   unsigned int unicode)
{
  SCA_InputEvent &event = m_inputsTable[type];

  if (event.m_values[event.m_values.size() - 1] != val) {
    // The key event value changed, we considerate it as the real event.
    event.m_status.push_back((val > 0) ? SCA_InputEvent::ACTIVE : SCA_InputEvent::NONE);
    event.m_queue.push_back((val > 0) ? SCA_InputEvent::JUSTACTIVATED :
                                        SCA_InputEvent::JUSTRELEASED);
    event.m_values.push_back(val);
    event.m_unicode = unicode;

    // Avoid pushing nullptr string character.
    if (val > 0 && unicode != 0) {
      m_text += (wchar_t)unicode;
    }
  }
}

void DEV_InputDevice::ConvertEvent(unsigned int type,
                                   int val,
                                   unsigned int unicode)
{
  SCA_InputEvent &event = m_inputsTable[type];

  if (event.m_values[event.m_values.size() - 1] != val) {
    // The key event value changed, we considerate it as the real event.
    event.m_status.push_back((val > 0) ? SCA_InputEvent::ACTIVE : SCA_InputEvent::NONE);
    event.m_queue.push_back((val > 0) ? SCA_InputEvent::JUSTACTIVATED :
                                        SCA_InputEvent::JUSTRELEASED);
    event.m_values.push_back(val);
    event.m_unicode = unicode;

    // Avoid pushing nullptr string character.
    if (val > 0 && unicode != 0) {
      m_text += (wchar_t)unicode;
    }
  }
}

void DEV_InputDevice::ConvertMoveEvent(int x, int y)
{
  SCA_InputEvent &xevent = m_inputsTable[MOUSEX];
  xevent.m_values.push_back(x);
  if (xevent.m_status[xevent.m_status.size() - 1] != SCA_InputEvent::ACTIVE) {
    xevent.m_status.push_back(SCA_InputEvent::ACTIVE);
    xevent.m_queue.push_back(SCA_InputEvent::JUSTACTIVATED);
  }

  SCA_InputEvent &yevent = m_inputsTable[MOUSEY];
  yevent.m_values.push_back(y);
  if (yevent.m_status[yevent.m_status.size() - 1] != SCA_InputEvent::ACTIVE) {
    yevent.m_status.push_back(SCA_InputEvent::ACTIVE);
    yevent.m_queue.push_back(SCA_InputEvent::JUSTACTIVATED);
  }
}

void DEV_InputDevice::ConvertWheelEvent(int z)
{
  SCA_InputEvent &event = m_inputsTable[(z > 0) ? WHEELUPMOUSE : WHEELDOWNMOUSE];
  event.m_values.push_back(z);
  if (event.m_status[event.m_status.size() - 1] != SCA_InputEvent::ACTIVE) {
    event.m_status.push_back(SCA_InputEvent::ACTIVE);
    event.m_queue.push_back(SCA_InputEvent::JUSTACTIVATED);
  }
}
