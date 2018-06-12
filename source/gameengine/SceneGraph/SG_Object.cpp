#include "SG_Object.h"
#include "SG_Node.h"

SG_Object::SG_Object()
	:m_node(nullptr)
{
}

SG_Object::SG_Object(const SG_Object& other)
	:m_node(nullptr)
{
}

SG_Object::~SG_Object() = default;

SG_Node *SG_Object::GetNode() const
{
	return m_node;
}

void SG_Object::SetNode(SG_Node *node)
{
	m_node = node;
}

SG_BBox &SG_Object::GetAabb()
{
	return m_aabb;
}

const SG_BBox &SG_Object::GetAabb() const
{
	return m_aabb;
}

bool SG_Object::GetCulled() const
{
	return m_culled;
}

void SG_Object::SetCulled(bool culled)
{
	m_culled = culled;
}

