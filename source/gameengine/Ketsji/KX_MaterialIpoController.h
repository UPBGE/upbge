
/** \file KX_MaterialIpoController.h
 *  \ingroup ketsji
 */

#ifndef __KX_MATERIALIPOCONTROLLER_H__
#define __KX_MATERIALIPOCONTROLLER_H__



#include "SG_Controller.h"
#include "SG_Node.h"
#include "SG_Interpolator.h"
#include "mathfu.h"

class RAS_IPolyMaterial;

class KX_MaterialIpoController : public SG_Controller, public mt::SimdClassAllocator
{
public:
	mt::vec4			m_rgba;
	mt::vec3			m_specrgb;
	float			m_hard;
	float			m_spec;
	float			m_ref;
	float			m_emit;
	float			m_ambient;
	float			m_alpha;
	float			m_specAlpha;

private:
	RAS_IPolyMaterial *m_material;

public:
	KX_MaterialIpoController(RAS_IPolyMaterial *polymat) : 
				m_material(polymat)
		{}
	virtual ~KX_MaterialIpoController() = default;
	virtual bool Update();
};

#endif /* __KX_MATERIALIPOCONTROLLER_H__ */
