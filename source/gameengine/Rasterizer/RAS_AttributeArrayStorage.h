#ifndef __RAS_ATTRIBUTE_ARRAY_STORAGE_H__
#define __RAS_ATTRIBUTE_ARRAY_STORAGE_H__

#include "RAS_AttributeArray.h"

class RAS_StorageVao;
struct RAS_DisplayArrayLayout;

class RAS_AttributeArrayStorage
{
private:
	std::unique_ptr<RAS_StorageVao> m_vao;

public:
	RAS_AttributeArrayStorage(const RAS_DisplayArrayLayout& layout, RAS_DisplayArrayStorage *arrayStorage,
							  const RAS_AttributeArray::AttribList& attribList);
	~RAS_AttributeArrayStorage();

	void BindPrimitives();
	void UnbindPrimitives();
};

#endif  // __RAS_ATTRIBUTE_ARRAY_STORAGE_H__
