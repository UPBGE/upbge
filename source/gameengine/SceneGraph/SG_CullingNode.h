#ifndef __SG_CULLING_OBJECT_H__
#define __SG_CULLING_OBJECT_H__

#include "SG_BBox.h"
#include "Object.h"

#include <vector>

/// \brief Node used for the culling of a scene graph node, then a scene object.
class SG_CullingNode : public bvh::Object
{
private:
	/// The bounding box of the node in the scene graph node transform space.
	SG_BBox m_aabb;
	/// The culling state from the last culling pass.
	bool m_culled;

public:
	SG_CullingNode();
	~SG_CullingNode() = default;

	SG_BBox& GetAabb();
	const SG_BBox& GetAabb() const;

	bool GetCulled() const;
	void SetCulled(bool culled);

  virtual bool getIntersection(
      const bvh::Ray& ray,
      bvh::IntersectionInfo* intersection)
    const
    {
		return false;
	}

  //! Return an object normal based on an intersection
  virtual mt::vec3 getNormal(const bvh::IntersectionInfo& I) const
  {
	  return mt::axisX3;
}

  //! Return a bounding box for this object
  virtual bvh::BBox getBBox() const
  {
	  return bvh::BBox(m_aabb.GetMin(), m_aabb.GetMax());
}

  //! Return the centroid for this object. (Used in BVH Sorting)
  virtual mt::vec3 getCentroid() const
  {
	  return m_aabb.GetCenter();
}
};

using SG_CullingNodeList = std::vector<SG_CullingNode *>;

#endif  // __SG_CULLING_OBJECT_H__
