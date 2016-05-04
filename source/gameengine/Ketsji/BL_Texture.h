
/** \file BL_Texture.h
 *  \ingroup ketsji
 */

#ifndef __BL_TEXTURE_H__
#define __BL_TEXTURE_H__

#include "SCA_IObject.h"

struct MTex;
struct Image;
struct GPUTexture;

class BL_Texture : public CValue
{
	Py_Header
private:
	int m_bindCode;
	MTex *m_mtex;
	STR_String m_mtexName;
	GPUTexture *m_gpuTex;

	struct {
		unsigned int bindcode;
		float colintensfac;
		float colfac;
		float alphafac;
		float specintensfac;
		float speccolorfac;
		float hardnessfac;
		float emitfac;
		float mirrorfac;
		float normalfac;
		float parallaxbumpfac;
		float parallaxstepfac;
		float lodbias;
	} m_savedData;

public:
	BL_Texture(MTex *mtex, bool cubemap);
	~BL_Texture();

	// stuff for cvalue related things
	virtual CValue *Calc(VALUE_OPERATOR op, CValue *val);
	virtual CValue *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val);
	virtual const STR_String& GetText();
	virtual double GetNumber();
	virtual STR_String& GetName();
	virtual void SetName(const char *name); // Set the name of the value
	virtual CValue *GetReplica();

	bool Ok();

	MTex *GetMTex();
	Image *GetImage();

	unsigned int GetTextureType() const;

	enum {MaxUnits = 8};

	void ActivateTexture(int unit);
	void DisableTexture();
	unsigned int swapTexture(unsigned int bindcode);

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_diffuse_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_diffuse_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_diffuse_factor(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_diffuse_factor(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_factor(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_factor(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_hardness(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_hardness(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_emit(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_emit(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_mirror(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_mirror(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_normal(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_normal(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_parallax_bump(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_parallax_bump(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_parallax_step(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_parallax_step(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_lod_bias(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_lod_bias(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_bind_code(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_bind_code(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);

#endif  // WITH_PYTHON

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:BL_Texture")
#endif
};

#endif // __BL_TEXTURE_H__
