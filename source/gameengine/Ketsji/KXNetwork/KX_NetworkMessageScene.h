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

/** \file KX_NetworkMessageScene.h
 *  \ingroup ketsjinet
 *  \brief Ketsji Logic Extension: Network Message Scene class
 */
#ifndef __KX_NETWORKMESSAGESCENE_H__
#define __KX_NETWORKMESSAGESCENE_H__

/* undef SendMessage Macro (WinUser.h) to avoid
conflicts with KX_NetworkMessageScene::SendMessage */
#ifdef WIN32
#  undef SendMessage
#endif

#include "KX_NetworkMessageManager.h"
#include <string>
#include <map>
#include <vector>

class KX_GameObject;

class KX_NetworkMessageScene
{
private:
	KX_NetworkMessageManager *m_messageManager;

public:
	KX_NetworkMessageScene(KX_NetworkMessageManager *messageManager);
	virtual ~KX_NetworkMessageScene();

	/** Send A message to an object(s) name.
	 * \param to The object(s) name, in case of duplicated object all objects
	 * with the same name will receive the message.
	 * \param from The sender game object.
	 * \param subject The message subject, used as filter for receiver object(s).
	 * \param message The body of the message.
	 */
	void SendMessage(std::string to, KX_GameObject *from, std::string subject, std::string body);

	/** Get all messages for a given receiver object name and message subject.
	 * \param to The object(s) name.
	 * \param subject The message subject/filter.
	 */
	const std::vector<KX_NetworkMessageManager::Message> FindMessages(std::string to, std::string subject);
};

#endif // __KX_NETWORKMESSAGESCENE_H__
