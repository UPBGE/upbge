#include "RAS_AttributeArrayStorage.h"
#include "RAS_StorageVao.h"

RAS_AttributeArrayStorage::RAS_AttributeArrayStorage(RAS_IDisplayArray *array, RAS_DisplayArrayStorage *arrayStorage,
													 const RAS_AttributeArray::AttribList& attribList)
	:m_vao(new RAS_StorageVao(array, arrayStorage, attribList))
{
}

RAS_AttributeArrayStorage::~RAS_AttributeArrayStorage()
{
}

void RAS_AttributeArrayStorage::BindPrimitives()
{
	m_vao->BindPrimitives();
}

void RAS_AttributeArrayStorage::UnbindPrimitives()
{
	m_vao->UnbindPrimitives();
}


