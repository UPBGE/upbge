
/** \file KX_MaterialIpoController.h
 *  \ingroup ketsji
 */

#ifndef __KX_MATERIALIPOCONTROLLER_H__
#define __KX_MATERIALIPOCONTROLLER_H__



#include "SG_Controller.h"
#include "SG_Node.h"
#include "KX_IInterpolator.h"

class RAS_IPolyMaterial;

class KX_MaterialIpoController : public SG_Controller
{
public:
	MT_Vector4			m_rgba;
	MT_Vector3			m_specrgb;
	MT_Scalar			m_hard;
	MT_Scalar			m_spec;
	MT_Scalar			m_ref;
	MT_Scalar			m_emit;
	MT_Scalar			m_ambient;
	MT_Scalar			m_alpha;
	MT_Scalar			m_specAlpha;

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
