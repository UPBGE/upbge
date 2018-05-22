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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/SCA_IInputDevice.cpp
 *  \ingroup gamelogic
 */


#include "BLI_utildefines.h"
#include "SCA_IInputDevice.h"

/** Initialize conversion table key to char (shifted too), this function is a long function but
 * is easier to maintain than key index conversion way.
 */
static std::map<SCA_IInputDevice::SCA_EnumInputs, std::pair<char, char> > createKeyToCharMap()
{
	std::map<SCA_IInputDevice::SCA_EnumInputs, std::pair<char, char> > map;

	map[SCA_IInputDevice::RETKEY] = std::make_pair('\n', '\n');
	map[SCA_IInputDevice::SPACEKEY] = std::make_pair(' ', ' ');
	map[SCA_IInputDevice::COMMAKEY] = std::make_pair(',', '<');
	map[SCA_IInputDevice::MINUSKEY] = std::make_pair('-', '_');
	map[SCA_IInputDevice::PERIODKEY] = std::make_pair('.', '>');
	map[SCA_IInputDevice::ZEROKEY] = std::make_pair('0', ')');
	map[SCA_IInputDevice::ONEKEY] = std::make_pair('1', '!');
	map[SCA_IInputDevice::TWOKEY] = std::make_pair('2', '@');
	map[SCA_IInputDevice::THREEKEY] = std::make_pair('3', '#');
	map[SCA_IInputDevice::FOURKEY] = std::make_pair('4', '$');
	map[SCA_IInputDevice::FIVEKEY] = std::make_pair('5', '%');
	map[SCA_IInputDevice::SIXKEY] = std::make_pair('6', '^');
	map[SCA_IInputDevice::SEVENKEY] = std::make_pair('7', '&');
	map[SCA_IInputDevice::EIGHTKEY] = std::make_pair('8', '*');
	map[SCA_IInputDevice::NINEKEY] = std::make_pair('9', '(');
	map[SCA_IInputDevice::AKEY] = std::make_pair('a', 'A');
	map[SCA_IInputDevice::BKEY] = std::make_pair('b', 'B');
	map[SCA_IInputDevice::CKEY] = std::make_pair('c', 'C');
	map[SCA_IInputDevice::DKEY] = std::make_pair('d', 'D');
	map[SCA_IInputDevice::EKEY] = std::make_pair('e', 'E');
	map[SCA_IInputDevice::FKEY] = std::make_pair('f', 'F');
	map[SCA_IInputDevice::GKEY] = std::make_pair('g', 'G');
	map[SCA_IInputDevice::HKEY_] = std::make_pair('h', 'H');
	map[SCA_IInputDevice::IKEY] = std::make_pair('i', 'I');
	map[SCA_IInputDevice::JKEY] = std::make_pair('j', 'J');
	map[SCA_IInputDevice::KKEY] = std::make_pair('k', 'K');
	map[SCA_IInputDevice::LKEY] = std::make_pair('l', 'L');
	map[SCA_IInputDevice::MKEY] = std::make_pair('m', 'M');
	map[SCA_IInputDevice::NKEY] = std::make_pair('n', 'N');
	map[SCA_IInputDevice::OKEY] = std::make_pair('o', 'O');
	map[SCA_IInputDevice::PKEY] = std::make_pair('p', 'P');
	map[SCA_IInputDevice::QKEY] = std::make_pair('q', 'Q');
	map[SCA_IInputDevice::RKEY] = std::make_pair('r', 'R');
	map[SCA_IInputDevice::SKEY] = std::make_pair('s', 'S');
	map[SCA_IInputDevice::TKEY] = std::make_pair('t', 'T');
	map[SCA_IInputDevice::UKEY] = std::make_pair('u', 'U');
	map[SCA_IInputDevice::VKEY] = std::make_pair('v', 'V');
	map[SCA_IInputDevice::WKEY] = std::make_pair('w', 'W');
	map[SCA_IInputDevice::XKEY] = std::make_pair('x', 'X');
	map[SCA_IInputDevice::YKEY] = std::make_pair('y', 'Y');
	map[SCA_IInputDevice::ZKEY] = std::make_pair('z', 'Z');
	map[SCA_IInputDevice::TABKEY] = std::make_pair('\t', '\t');
	map[SCA_IInputDevice::SEMICOLONKEY] = std::make_pair(';', ':');
	map[SCA_IInputDevice::QUOTEKEY] = std::make_pair('\'', '\"');
	map[SCA_IInputDevice::ACCENTGRAVEKEY] = std::make_pair('`', '~');
	map[SCA_IInputDevice::SLASHKEY] = std::make_pair('/', '?');
	map[SCA_IInputDevice::BACKSLASHKEY] = std::make_pair('\\', '|');
	map[SCA_IInputDevice::EQUALKEY] = std::make_pair('=', '+');
	map[SCA_IInputDevice::LEFTBRACKETKEY] = std::make_pair('[', '{');
	map[SCA_IInputDevice::RIGHTBRACKETKEY] = std::make_pair(']', '}');
	map[SCA_IInputDevice::PAD1] = std::make_pair('1', '1');
	map[SCA_IInputDevice::PAD2] = std::make_pair('2', '2');
	map[SCA_IInputDevice::PAD3] = std::make_pair('3', '3');
	map[SCA_IInputDevice::PAD4] = std::make_pair('4', '4');
	map[SCA_IInputDevice::PAD5] = std::make_pair('5', '5');
	map[SCA_IInputDevice::PAD6] = std::make_pair('6', '6');
	map[SCA_IInputDevice::PAD7] = std::make_pair('7', '7');
	map[SCA_IInputDevice::PAD9] = std::make_pair('8', '8');
	map[SCA_IInputDevice::PAD0] = std::make_pair('9', '9');
	map[SCA_IInputDevice::PADASTERKEY] = std::make_pair('*', '*');
	map[SCA_IInputDevice::PADPERIOD] = std::make_pair('.', '.');
	map[SCA_IInputDevice::PADSLASHKEY] = std::make_pair('/', '/');
	map[SCA_IInputDevice::PADMINUS] = std::make_pair('-', '-');
	map[SCA_IInputDevice::PADENTER] = std::make_pair('\n', '\n');
	map[SCA_IInputDevice::PADPLUSKEY] = std::make_pair('+', '+');

	return map;
}

std::map<SCA_IInputDevice::SCA_EnumInputs, std::pair<char, char> > SCA_IInputDevice::m_keyToChar = createKeyToCharMap();

SCA_IInputDevice::SCA_IInputDevice()
	:m_hookExitKey(false)
{
	for (int i = 0; i < SCA_IInputDevice::MAX_KEYS; ++i) {
		m_inputsTable[i] = std::move(SCA_InputEvent(i));
	}
}

SCA_IInputDevice::~SCA_IInputDevice()
{
	for (int i = 0; i < SCA_IInputDevice::MAX_KEYS; ++i) {
		m_inputsTable[i].InvalidateProxy();
	}
}

void SCA_IInputDevice::SetHookExitKey(bool hook)
{
	m_hookExitKey = hook;
}

bool SCA_IInputDevice::GetHookExitKey() const
{
	return m_hookExitKey;
}

void SCA_IInputDevice::ClearInputs()
{
	for (int i = 0; i < SCA_IInputDevice::MAX_KEYS; ++i) {
		m_inputsTable[i].Clear();
	}
	m_text.clear();
}

void SCA_IInputDevice::ReleaseMoveEvent()
{
	/* We raise the release mouse move event if:
	 *   - there are only one value from the last call to Clear()
	 *   - the last state was ACTIVE
	 * If the both are true then the ACTIVE come from the last call to ClearEvent must
	 * be removed of the status list to avoid setting the mouse active for two frames.
	 */
	SCA_EnumInputs eventTypes[4] = {
		MOUSEX,
		MOUSEY,
		WHEELUPMOUSE,
		WHEELDOWNMOUSE
	};

	for (unsigned short i = 0; i < 4; ++i) {
		SCA_InputEvent &event = m_inputsTable[eventTypes[i]];
		if ((event.m_values.size() == 1) && (event.m_status[event.m_status.size() - 1] == SCA_InputEvent::ACTIVE)) {
			event.m_status.pop_back();
			event.m_status.push_back(SCA_InputEvent::NONE);
			event.m_queue.push_back(SCA_InputEvent::JUSTRELEASED);
		}
	}
}

const std::wstring& SCA_IInputDevice::GetText() const
{
	return m_text;
}

const char SCA_IInputDevice::ConvertKeyToChar(SCA_IInputDevice::SCA_EnumInputs input, bool shifted)
{
	std::map<SCA_EnumInputs, std::pair<char, char> >::iterator it = m_keyToChar.find(input);
	if (it == m_keyToChar.end()) {
		return 0;
	}

	return (shifted) ? it->second.second : it->second.first;
}

SCA_InputEvent& SCA_IInputDevice::GetInput(SCA_IInputDevice::SCA_EnumInputs inputcode)
{
	return m_inputsTable[inputcode];
}
