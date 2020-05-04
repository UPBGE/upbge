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
 * LoopbackNetworkDeviceInterface derived from NG_NetworkDeviceInterface
 */

/** \file gameengine/Network/NET_Client.cpp
 *  \ingroup ge_network
 */

#include "NET_Client.h"


NET_ClientInterface::NET_ClientInterface()
{
  m_currentList = 0;
  this->Offline();

  /* Initializing Enet */
  if (enet_initialize () != 0){
	fprintf (stderr, "An error occurred while initializing Game Engine Network.\n");
	//XXXENET do something useful
  }
  else {
	fprintf (stderr, "Game Engine Network Initialized.\n");
  }
}

NET_ClientInterface::~NET_ClientInterface()
{
  if (client){
	enet_host_destroy(client);
  }
  enet_deinitialize();
}

bool NET_ClientInterface::Connect(char *address, unsigned int port,
											char *password, unsigned int channels,
											unsigned int timeout)
{
  int l = 0;

  /* Create client host */
  fprintf(stderr,"Establishing connection to ENet...\n");
  m_client = enet_host_create(NULL, 1, 2, 0, 0); //XXXENET Incoming & outgoing bandwidth must be set by user. Unlimited by default
  if (m_client == NULL){
	fprintf(stderr,"An error occurred while trying to create an ENet client host.\n");
	return false;
  }
  else {
	fprintf(stderr,"ENet host created.\n");
  }

  /* Setup address and port */
  enet_address_set_host(&m_enet_address, address);
  m_enet_address.port = port;

  /* Initiate the connection, allocating channels */
  m_peer = enet_host_connect(m_client, &m_enet_address, 2, 0); //XXXENET Channels must be set by user. 1 Channel by default
  if (n_peer == NULL){
	fprintf(stderr,"No available peers for initiating an ENet connection.\n");
	return false;
  }

  /* Wait up to N miliseconds for the connection attempt to succeed. */
  while (l == 0){
	while (enet_host_service(m_client, &m_event, timeout) > 0){
	  switch(m_event.type){
		case ENET_EVENT_TYPE_CONNECT:
		  char ip_conn[16];
		  ENetPacket *packet;
		  enet_address_get_host (&m_enet_address, ip_conn, 16);
		  fprintf(stderr,"Connection to %s:%u succeeded.\n", ip_conn, m_enet_address.port);
		  enet_host_flush(m_client);
		  packet = enet_packet_create("testing", strlen("testing") + 1, ENET_PACKET_FLAG_RELIABLE);
		  enet_peer_send(m_peer, 1, packet); // Packet send by channel 1 (default)
		  enet_host_flush(m_client); //Needed for conexion event
		  break;
		case ENET_EVENT_TYPE_RECEIVE:
		  fprintf (stderr,"A packet of length %u containing %s was received from server.\n", m_event.packet->dataLength, m_event.packet->data);
		  enet_packet_destroy(m_event.packet);
		  sleep(1);
		  enet_host_flush(m_client);
		  l = 1;
		  break;
		default:
		  break;
	  }
	}
  }
  return true;
}


bool NET_ClientInterface::Disconnect(void)
{
  fprintf(stderr,"Disconnect false");
  return true;
}

int NET_ClientInterface::GetNetworkVersion()
{
  return ENET_VERSION;
}

// perhaps this should go to the shared/common implementation too
void NET_ClientInterface::NextFrame()
{
  // TODO
}

void NET_ClientInterface::SendNetworkMessage(NET_Message* netmsg)
{
  // TODO
}

vector<NET_Message*> NET_ClientInterface::RetrieveNetworkMessages()
{
	vector<NET_Message*> messages;
	
	// TODO
	return messages;
}

