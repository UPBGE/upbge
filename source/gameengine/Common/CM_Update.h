#ifndef __CM_UPDATE_H__
#define __CM_UPDATE_H__

#include "CM_List.h"

#include <vector>

template <typename Category>
class CM_UpdateServer;

template <typename Category>
class CM_UpdateClient
{
friend class CM_UpdateServer<Category>;

private:
	unsigned int m_invalid;
	unsigned int m_filter;
	CM_UpdateServer<Category> *m_server;

public:
	CM_UpdateClient(unsigned int filter, unsigned int invalid)
		:m_invalid(invalid),
		m_filter(filter),
		m_server(nullptr)
	{
	}

	CM_UpdateClient(unsigned int filter)
		:CM_UpdateClient(filter, false)
	{
	}

	~CM_UpdateClient()
	{
		if (m_server) {
			m_server->RemoveUpdateClient(this);
		}
	}

	unsigned int GetInvalid() const
	{
		return m_invalid;
	}

	void ClearInvalid()
	{
		m_invalid = 0;
	}

	unsigned int GetInvalidAndClear()
	{
		const unsigned int invalid = m_invalid;
		m_invalid = 0;
		return invalid;
	}
};

template <typename Category>
class CM_UpdateServer
{
public:
	using ClientType = CM_UpdateClient<Category>;

private:
	std::vector<ClientType *> m_clients;

public:
	CM_UpdateServer() = default;
	virtual ~CM_UpdateServer()
	{
		for (ClientType *client : m_clients) {
			client->m_server = nullptr;
		}
	}

	void MoveUpdateClient(ClientType *client, unsigned int invalid)
	{
		if (client->m_server) {
			client->m_server->RemoveUpdateClient(client);
		}

		client->m_invalid |= invalid;
		AddUpdateClient(client);
	}

	void AddUpdateClient(ClientType *client)
	{
		m_clients.push_back(client);
		client->m_server = this;
	}

	void RemoveUpdateClient(ClientType *client)
	{
		CM_ListRemoveIfFound(m_clients, client);
		client->m_server = nullptr;
	}

	void NotifyUpdate(unsigned int flag)
	{
		for (ClientType *client : m_clients) {
			client->m_invalid |= (flag & client->m_filter);
		}
	}
};

#endif  // __CM_UPDATE_H__
