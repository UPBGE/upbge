#include "RAS_DisplayArrayStorage.h"
#include "RAS_StorageVbo.h"

RAS_DisplayArrayStorage::RAS_DisplayArrayStorage()
{
}

RAS_DisplayArrayStorage::~RAS_DisplayArrayStorage()
{
}

void RAS_DisplayArrayStorage::Construct(RAS_DisplayArray *array)
{
	m_vbo.reset(new RAS_StorageVbo(array));
}

RAS_StorageVbo *RAS_DisplayArrayStorage::GetVbo() const
{
	return m_vbo.get();
}

void RAS_DisplayArrayStorage::UpdateVertexData(unsigned int modifiedFlag)
{
	m_vbo->UpdateVertexData(modifiedFlag);
}

void RAS_DisplayArrayStorage::UpdateSize()
{
	m_vbo->UpdateSize();
}

unsigned int *RAS_DisplayArrayStorage::GetIndexMap()
{
	return m_vbo->GetIndexMap();
}

void RAS_DisplayArrayStorage::FlushIndexMap()
{
	m_vbo->FlushIndexMap();
}

void RAS_DisplayArrayStorage::IndexPrimitives()
{
	m_vbo->IndexPrimitives();
}

void RAS_DisplayArrayStorage::IndexPrimitivesInstancing(unsigned int numslots)
{
	m_vbo->IndexPrimitivesInstancing(numslots);
}

void RAS_DisplayArrayStorage::IndexPrimitivesBatching(const std::vector<intptr_t>& indices, const std::vector<int>& counts)
{
	m_vbo->IndexPrimitivesBatching(indices, counts);
}
