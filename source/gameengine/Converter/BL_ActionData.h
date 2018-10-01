#ifndef __BL_ACTION_DATA_H__
#define __BL_ACTION_DATA_H__

#include "BL_Resource.h"
#include "BL_ScalarInterpolator.h"

#include <string>

struct bAction;

/** Data related to a blender animation.
 */
class BL_ActionData : public BL_Resource
{
private:
	/// The blender action.
	bAction *m_action;
	/// The interpolators for each curve (FCurve) of the action.
	std::vector<BL_ScalarInterpolator> m_interpolators;

public:
	BL_ActionData(bAction *action);
	~BL_ActionData() = default;

	std::string GetName() const;
	bAction *GetAction() const;

	BL_ScalarInterpolator *GetScalarInterpolator(const std::string& rna_path, int array_index);
};

#endif  // __BL_ACTION_DATA_H__
