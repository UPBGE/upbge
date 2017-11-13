#ifndef __KX_CULLING_HANDLER_H__
#define __KX_CULLING_HANDLER_H__

#include "SG_Frustum.h"
#include <vector>

class KX_GameObject;

class KX_CullingHandler
{
private:
	/// List of all objects to render after the culling pass.
	std::vector<KX_GameObject *>& m_activeObjects;
	/// The camera frustum data.
	const SG_Frustum& m_frustum;

public:
	KX_CullingHandler(std::vector<KX_GameObject *>& objects, const SG_Frustum& frustum);
	~KX_CullingHandler() = default;

	/** Process the culling of a new object, if the culling succeeded the
	 * object is added in m_activeObjects.
	 */
	void Process(KX_GameObject *object);
};

#endif  // __KX_CULLING_HANDLER_H__
