
/** \file BL_Shader.h
 *  \ingroup ketsji
 */

#ifndef __BL_SHADER_H__
#define __BL_SHADER_H__

#include "EXP_PyObjectPlus.h"
#include "RAS_Shader.h"

class BL_Shader : public PyObjectPlus, public virtual RAS_Shader
{
	Py_Header
public:
	BL_Shader();
	virtual ~BL_Shader();

	// Python interface
#ifdef WITH_PYTHON
	virtual PyObject *py_repr()
	{
		return PyUnicode_FromFormat("BL_Shader\n\tvertex shader:%s\n\n\tfragment shader%s\n\n", m_progs[VERTEX_PROGRAM].ReadPtr(), m_progs[FRAGMENT_PROGRAM].ReadPtr());
	}

	static PyObject *pyattr_get_enabled(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_enabled(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);

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
