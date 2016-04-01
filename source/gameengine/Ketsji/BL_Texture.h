
/** \file BL_Texture.h
 *  \ingroup ketsji
 */

#ifndef __BL_TEXTURE_H__
#define __BL_TEXTURE_H__

#include "SCA_IObject.h"

struct MTex;
struct GPUTexture;

class BL_Texture : public CValue
{
	Py_Header
private:
	unsigned int m_bindcode;
	MTex *m_mtex;
	STR_String m_mtexname;
	GPUTexture *m_gputex;

	struct {
		unsigned int bindcode;
		float colintensfac;
		float colfac;
		float alphafac;
		float specintensfac;
		float speccolorfac;
		float spechardnessfac;
		float emitfac;
		float mirrorfac;
		float normalfac;
		float parallaxbumpfac;
		float parallaxstepfac;
	} m_savedData;

public:
	BL_Texture(MTex *mtex, bool cubemap, bool mipmap);
	~BL_Texture();

	// stuff for cvalue related things
	virtual CValue *Calc(VALUE_OPERATOR op, CValue *val);
	virtual CValue *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val);
	virtual const STR_String& GetText();
	virtual double GetNumber();
	virtual MTex *GetMTex()
	{
		return m_mtex;
	}
	virtual STR_String& GetName();
	virtual void SetName(const char *name); // Set the name of the value
	virtual CValue *GetReplica();

	// stuff for python integration

	KX_PYMETHOD(BL_Texture, GetNumTex);
	KX_PYMETHOD(BL_Texture, GetTextureName);

	static PyObject *pyattr_get_colorintensfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_colorintensfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_colorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_colorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_alphafac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_alphafac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specintensfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specintensfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_speccolorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_speccolorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_spechardnessfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_spechardnessfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_emitfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_emitfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_mirrorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_mirrorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_normalfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_normalfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_parallaxbumpfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_parallaxbumpfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_parallaxstepfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_parallaxstepfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	bool Ok();

	unsigned int GetTextureType() const;

	static int GetMaxUnits();

	void ActivateTexture(int unit);
	void DisableTexture();
	unsigned int swapTexture(unsigned int bindcode);

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:BL_Texture")
#endif
};

#endif // __BL_TEXTURE_H__
