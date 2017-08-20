
/** \file BL_Shader.h
 *  \ingroup ketsji
 */

#ifndef __BL_SHADER_H__
#define __BL_SHADER_H__

#include "EXP_Value.h"
#include "RAS_Shader.h"
#include "RAS_Texture.h" // For RAS_Texture::MaxUnits.

class RAS_MeshSlot;

class BL_Shader : public CValue, public virtual RAS_Shader
{
	Py_Header
public:
	enum CallbacksType {
		CALLBACKS_BIND = 0,
		CALLBACKS_OBJECT,
		CALLBACKS_MAX
	};

	enum AttribTypes {
		SHD_TANGENT = 1
	};

private:
#ifdef WITH_PYTHON
	PyObject *m_callbacks[CALLBACKS_MAX];
#endif  // WITH_PYTHON

public:
	BL_Shader();
	virtual ~BL_Shader();

	virtual std::string GetName();
	virtual std::string GetText();

	/// Initialize textures coordinates attributes using a list of textures.
	void InitTexCo(RAS_Texture *textures[RAS_Texture::MaxUnits]);

#ifdef WITH_PYTHON
	PyObject *GetCallbacks(CallbacksType type);
	void SetCallbacks(CallbacksType type, PyObject *callbacks);
#endif // WITH_PYTHON

	virtual void SetProg(bool enable);

	void SetAttribs(RAS_Rasterizer *rasty);

	/** Update the uniform shader for the current rendered mesh slot.
	 * The python callbacks are executed in this function and at the end
	 * RAS_Shader::Update(rasty, mat) is called.
	 */
	void Update(RAS_Rasterizer *rasty, RAS_MeshSlot *ms);

	// Python interface
#ifdef WITH_PYTHON

	static PyObject *pyattr_get_enabled(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_enabled(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_callbacks(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_callbacks(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	// -----------------------------------
	KX_PYMETHOD_DOC(BL_Shader, setSource);
	KX_PYMETHOD_DOC(BL_Shader, setSourceList);
	KX_PYMETHOD_DOC(BL_Shader, delSource);
	KX_PYMETHOD_DOC(BL_Shader, getVertexProg);
	KX_PYMETHOD_DOC(BL_Shader, getFragmentProg);
	KX_PYMETHOD_DOC(BL_Shader, setNumberOfPasses);
	KX_PYMETHOD_DOC(BL_Shader, isValid);
	KX_PYMETHOD_DOC(BL_Shader, validate);

	// -----------------------------------
	KX_PYMETHOD_DOC(BL_Shader, setUniform4f);
	KX_PYMETHOD_DOC(BL_Shader, setUniform3f);
	KX_PYMETHOD_DOC(BL_Shader, setUniform2f);
	KX_PYMETHOD_DOC(BL_Shader, setUniform1f);
	KX_PYMETHOD_DOC(BL_Shader, setUniform4i);
	KX_PYMETHOD_DOC(BL_Shader, setUniform3i);
	KX_PYMETHOD_DOC(BL_Shader, setUniform2i);
	KX_PYMETHOD_DOC(BL_Shader, setUniform1i);
	KX_PYMETHOD_DOC(BL_Shader, setUniformEyef);
	KX_PYMETHOD_DOC(BL_Shader, setUniformfv);
	KX_PYMETHOD_DOC(BL_Shader, setUniformiv);
	KX_PYMETHOD_DOC(BL_Shader, setUniformMatrix4);
	KX_PYMETHOD_DOC(BL_Shader, setUniformMatrix3);
	KX_PYMETHOD_DOC(BL_Shader, setUniformDef);
	KX_PYMETHOD_DOC(BL_Shader, setAttrib);
	KX_PYMETHOD_DOC(BL_Shader, setSampler);
#endif
};

#endif /* __BL_SHADER_H__ */
