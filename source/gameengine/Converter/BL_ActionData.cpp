#include "BL_ActionData.h"

extern "C" {
#  include "DNA_action_types.h"
#  include "DNA_anim_types.h"
#  include "BKE_fcurve.h"
}

BL_ActionData::BL_ActionData(bAction *action)
	:m_action(action)
{
	for (FCurve *fcu = (FCurve *)action->curves.first; fcu; fcu = fcu->next) {
		if (fcu->rna_path) {
			m_interpolators.emplace_back(fcu);
		}
	}
}

std::string BL_ActionData::GetName() const
{
	return m_action->id.name + 2;
}

bAction *BL_ActionData::GetAction() const
{
	return m_action;
}

BL_ScalarInterpolator *BL_ActionData::GetScalarInterpolator(const std::string& rna_path, int array_index)
{
	for (BL_ScalarInterpolator &interp : m_interpolators) {
		FCurve *fcu = interp.GetFCurve();
		if (array_index == fcu->array_index && rna_path == fcu->rna_path) {
			return &interp;
		}
	}
	return nullptr;
}
