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

/** \file NET_Client.h
 *  \ingroup ge_network
 */
#ifndef __NET_CLIENT_H__
#define __NET_CLIENT_H__

/* undef SendMessage Macro (WinUser.h) to avoid
conflicts with NET_Client::SendMessage */
#ifdef WIN32
#  undef SendMessage
#endif

#include <map>
#include <string>
#include <vector>

#include "enet/enet.h"


class SCA_IObject;

class NET_ClientInterface
{
public:
  struct NET_Message {
	// Receiver object(s) name.
	std::string to;
	// Sender game object.
	SCA_IObject *from;
	// Message subject, used as filter.
	std::string subject;
	// Message body.
	std::string body;
  };

private:
  /** List of all messages, filtered by receiver object(s) name and subject name.
   * We use two lists, one handle sended message in the current frame and the other
   * is used for handle message sended in the last frame for sensors.
   */
  std::map<std::string, std::map<std::string, std::vector<NET_Message>>> m_messages[2];

  /** Since we use two list for the current and last frame we have to switch of
   * current message list each frame. This value is only 0 or 1.
   */
  unsigned short m_currentList;

  //Enet stuff
  ENetHost *m_client;
  ENetPeer *m_peer;

  ENetAddress m_enet_address;
  ENetEvent m_event;
  enet_uint32 m_client_id;

public:
  NET_ClientInterface();
  virtual ~NET_ClientInterface();

  /**
   * Clear message buffer
   */
  void NextFrame();

  bool Connect(char *address, unsigned int port, char *password,
			   unsigned int localport, unsigned int timeout);

  bool Disconnect(void);

  int GetNetworkVersion(void);

  void SendNetworkMessage(class NET_Message* msg);
  std::vector<NET_Message*> RetrieveNetworkMessages();
};

#endif  /* __NET_CLIENT_H__ */
