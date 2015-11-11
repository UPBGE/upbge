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
 * Ketsji Logic Extension: Network Message Scene generic implementation
 */

/** \file gameengine/Ketsji/KXNetwork/KX_NetworkMessageScene.cpp
 *  \ingroup ketsjinet
 */

#include "KX_NetworkMessageScene.h"
#include <iostream>

KX_NetworkMessageScene::KX_NetworkMessageScene()
	:m_currentList(0)
{
}

KX_NetworkMessageScene::~KX_NetworkMessageScene()
{
	ClearMessages();
}

void KX_NetworkMessageScene::SendMessage(STR_String to, SCA_IObject *from, STR_String subject, STR_String body)
{
	Message message;
	message.to = to;
	message.from = from;
	message.subject = subject;
	message.body = body;

	// Put the new message in map for the given receiver and subject.
	m_messages[m_currentList][to][subject].push_back(message);
}

const std::vector<KX_NetworkMessageScene::Message>& KX_NetworkMessageScene::FindMessages(STR_String to, STR_String subject)
{
	// Look at messages in the previous list.
	return m_messages[1 - m_currentList][to][subject];
}

void KX_NetworkMessageScene::ClearMessages()
{
	// Clear previous list.
	m_messages[1 - m_currentList].clear();
	m_currentList = 1 - m_currentList;
}
