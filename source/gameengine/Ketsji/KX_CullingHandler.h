#ifndef __KX_CULLING_HANDLER_H__
#define __KX_CULLING_HANDLER_H__

#include "SG_Frustum.h"
#include "SG_BBox.h"

#include "EXP_ListValue.h"

#ifdef WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#endif

class KX_GameObject;

class KX_CullingHandler
{
private:
	/// List of all objects to test.
	EXP_ListValue<KX_GameObject>& m_objects;
	/// The camera frustum data.
	const SG_Frustum& m_frustum;
	/// Layer to ignore some objects.
	int m_layer;


public:
	KX_CullingHandler(EXP_ListValue<KX_GameObject>& objects, const SG_Frustum& frustum, int layer);
	~KX_CullingHandler() = default;

	bool Test(const mt::mat3x4& trans, const mt::vec3& scale, const SG_BBox& aabb) const;

	/// Process the culling of all object and return a list of non-culled objects.
	std::vector<KX_GameObject *> Process();
};

#endif  // __KX_CULLING_HANDLER_H__
