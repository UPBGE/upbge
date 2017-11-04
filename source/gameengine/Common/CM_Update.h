#ifndef __CM_UPDATE_H__
#define __CM_UPDATE_H__

#include <vector>

template <typename Category>
class CM_UpdateServer;

template <typename Category>
class CM_UpdateClient
{
friend class CM_UpdateServer<Category>;

private:
	bool m_invalid;
	unsigned int m_filter;

public:
	CM_UpdateClient(unsigned int filter)
		:m_invalid(false),
		m_filter(filter)
	{
	}

	~CM_UpdateClient() = default;

	bool GetInvalid() const
	{
		return m_invalid;
	}

	void ClearInvalid()
	{
		m_invalid = false;
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
	virtual ~CM_UpdateServer() = default;

	void AddUpdateClient(ClientType *client)
	{
		m_clients.push_back(client);
	}

	void RemoveUpdateClient(ClientType *client)
	{
		CM_ListRemoveIfFound(m_clients, client);
	}

	void SetUpdateInvalid(unsigned int flag)
	{
		for (ClientType *client : m_clients) {
			client->m_invalid |= (flag & client->m_filter);
		}
	}
};

#endif  // __CM_UPDATE_H__
