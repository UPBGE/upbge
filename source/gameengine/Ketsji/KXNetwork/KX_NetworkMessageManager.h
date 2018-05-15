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
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_NetworkMessageManager.h
 *  \ingroup ketsjinet
 *  \brief Ketsji Logic Extension: Network Message Manager class
 */
#ifndef __KX_NETWORKMESSAGEMANAGER_H__
#define __KX_NETWORKMESSAGEMANAGER_H__

/* undef SendMessage Macro (WinUser.h) to avoid
conflicts with KX_NetworkMessageManager::SendMessage */
#ifdef WIN32
#  undef SendMessage
#endif

#include <string>
#include <map>
#include <vector>

class KX_GameObject;

class KX_NetworkMessageManager
{
public:
	struct Message
	{
		/// Receiver object(s) name.
		std::string to;
		/// Sender game object.
		KX_GameObject *from;
		/// Message subject, used as filter.
		std::string subject;
		/// Message body.
		std::string body;
	};

private:
	/** List of all messages, filtered by receiver object(s) name and subject name.
	 * We use two lists, one handle sended message in the current frame and the other
	 * is used for handle message sended in the last frame for sensors.
	 */
	std::map<std::string, std::map<std::string, std::vector<Message> > > m_messages[2];

	/** Since we use two list for the current and last frame we have to switch of
	 * current message list each frame. This value is only 0 or 1.
	 */
	unsigned short m_currentList;

public:
	KX_NetworkMessageManager();
	virtual ~KX_NetworkMessageManager();

	/** Add a message in the next message list.
	 * \param message The given message to add.
	 */
	void AddMessage(Message message);
	/** Get all messages for a given receiver object name and message subject.
	 * \param to The object(s) name.
	 * \param subject The message subject/filter.
	 */
	const std::vector<Message> GetMessages(std::string to, std::string subject);

	/// Clear all messages
	void ClearMessages();
};

#endif // __KX_NETWORKMESSAGEMANAGER_H__
