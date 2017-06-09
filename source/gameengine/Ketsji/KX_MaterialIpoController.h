
/** \file KX_MaterialIpoController.h
 *  \ingroup ketsji
 */

#ifndef __KX_MATERIALIPOCONTROLLER_H__
#define __KX_MATERIALIPOCONTROLLER_H__



#include "SG_Controller.h"
#include "SG_Node.h"
#include "KX_IInterpolator.h"
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
	T_InterpolatorList	m_interpolators;
	bool				m_modified;

	double		        m_ipotime;
	RAS_IPolyMaterial *m_material;

public:
	KX_MaterialIpoController(RAS_IPolyMaterial *polymat) : 
				m_modified(true),
				m_ipotime(0.0),
				m_material(polymat)
		{}
	virtual ~KX_MaterialIpoController();
	virtual	SG_Controller*	GetReplica(class SG_Node* destnode);
	virtual bool Update(double time);
	virtual void SetSimulatedTime(double time) {
		m_ipotime = time;
		m_modified = true;
	}
	
		void
	SetOption(
		int option,
		int value
	) {
		// intentionally empty
	};


	void	AddInterpolator(KX_IInterpolator* interp);
};

#endif /* __KX_MATERIALIPOCONTROLLER_H__ */
