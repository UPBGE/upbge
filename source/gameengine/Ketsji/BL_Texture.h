
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
	GPUTexture *m_gputex;

	struct {
		unsigned int bindcode;
		float colfac;
		float alphafac;
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

	static PyObject *pyattr_get_colorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_colorfac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_alphafac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_alphafac(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);

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
