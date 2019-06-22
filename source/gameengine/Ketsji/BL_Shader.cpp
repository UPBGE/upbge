/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/BL_Shader.cpp
 *  \ingroup ketsji
 */


#include "BL_Shader.h"

#include "RAS_MeshSlot.h"
#include "RAS_MeshUser.h"
#include "RAS_IMaterial.h"

#include "KX_PyMath.h"
#include "KX_PythonInit.h"
#include "KX_GameObject.h"

#include "DNA_material_types.h"
#include "DNA_texture_types.h"

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#endif  // WITH_PYTHON

#include "CM_Message.h"

BL_Shader::BL_Shader(CM_UpdateServer<RAS_IMaterial> *materialUpdateServer)
	:m_attr(SHD_NONE),
	m_materialUpdateServer(materialUpdateServer)
{
#ifdef WITH_PYTHON
	for (unsigned short i = 0; i < CALLBACKS_MAX; ++i) {
		m_callbacks[i] = PyList_New(0);
	}
#endif  // WITH_PYTHON
}

BL_Shader::~BL_Shader()
{
#ifdef WITH_PYTHON
	for (unsigned short i = 0; i < CALLBACKS_MAX; ++i) {
		Py_XDECREF(m_callbacks[i]);
	}
#endif  // WITH_PYTHON
}

bool BL_Shader::LinkProgram()
{
	// Can be null in case of filter shaders.
	if (m_materialUpdateServer) {
		// Notify all clients tracking this shader that shader is recompiled and attributes are invalidated.
		m_materialUpdateServer->NotifyUpdate(RAS_IMaterial::SHADER_MODIFIED | RAS_IMaterial::ATTRIBUTES_MODIFIED);
	}

	return RAS_Shader::LinkProgram();
}

std::string BL_Shader::GetName() const
{
	return "BL_Shader";
}

std::string BL_Shader::GetText() const
{
	return "BL_Shader\n\tvertex shader:" + m_progs[VERTEX_PROGRAM] + "\n\n\tfragment shader" + m_progs[FRAGMENT_PROGRAM] + "\n\n";
}


#ifdef WITH_PYTHON

PyObject *BL_Shader::GetCallbacks(BL_Shader::CallbacksType type)
{
	return m_callbacks[type];
}

void BL_Shader::SetCallbacks(BL_Shader::CallbacksType type, PyObject *callbacks)
{
	Py_XDECREF(m_callbacks[type]);
	Py_INCREF(callbacks);
	m_callbacks[type] = callbacks;
}

#endif  // WITH_PYTHON

RAS_AttributeArray::AttribList BL_Shader::GetAttribs(const RAS_Mesh::LayersInfo& layersInfo,
                                                     RAS_Texture *const textures[RAS_Texture::MaxUnits]) const
{
	RAS_AttributeArray::AttribList attribs;
	// Initialize textures attributes.
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; ++i) {
		RAS_Texture *texture = textures[i];
		/* Here textures can return false to Ok() because we're looking only at
		 * texture attributes and not texture bind id like for the binding and
		 * unbinding of textures. A nullptr RAS_Texture means that the corresponding
		 * mtex is nullptr too (see KX_BlenderMaterial::InitTextures).*/
		if (texture) {
			MTex *mtex = texture->GetMTex();
			if (mtex->texco & (TEXCO_OBJECT | TEXCO_REFL)) {
				attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_POS, true, 0});
			}
			else if (mtex->texco & (TEXCO_ORCO | TEXCO_GLOB)) {
				attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_POS, true, 0});
			}
			else if (mtex->texco & TEXCO_UV) {
				// UV layer not specified, use default layer.
				if (strlen(mtex->uvname) == 0) {
					attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_UV, true, layersInfo.activeUv});
				}

				// Search for the UV layer index used by the texture.
				for (const RAS_Mesh::Layer& layer : layersInfo.uvLayers) {
					if (layer.name == mtex->uvname) {
						attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_UV, true, layer.index});
						break;
					}
				}
			}
			else if (mtex->texco & TEXCO_NORM) {
				attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_NORM, true, 0});
			}
			else if (mtex->texco & TEXCO_TANGENT) {
				attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_TANGENT, true, 0});
			}
		}
	}

	if (m_attr == SHD_TANGENT) {
		attribs.push_back({1, RAS_AttributeArray::RAS_ATTRIB_TANGENT, false, 0});
	}

	return attribs;
}

void BL_Shader::BindProg()
{
#ifdef WITH_PYTHON
	if (PyList_GET_SIZE(m_callbacks[CALLBACKS_BIND]) > 0) {
		EXP_RunPythonCallBackList(m_callbacks[CALLBACKS_BIND], nullptr, 0, 0);
	}
#endif  // WITH_PYTHON

	RAS_Shader::BindProg();
}

void BL_Shader::Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser)
{
#ifdef WITH_PYTHON
	if (PyList_GET_SIZE(m_callbacks[CALLBACKS_OBJECT]) > 0) {
		KX_GameObject *gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)meshUser->GetClientObject());
		PyObject *args[] = {gameobj->GetProxy()};
		EXP_RunPythonCallBackList(m_callbacks[CALLBACKS_OBJECT], args, 0, ARRAY_SIZE(args));
	}
#endif  // WITH_PYTHON

	RAS_Shader::Update(rasty, meshUser->GetMatrix());
}

#ifdef WITH_PYTHON
PyMethodDef BL_Shader::Methods[] = {
	// creation
	EXP_PYMETHODTABLE(BL_Shader, setSource),
	EXP_PYMETHODTABLE(BL_Shader, setSourceList),
	EXP_PYMETHODTABLE(BL_Shader, delSource),
	EXP_PYMETHODTABLE(BL_Shader, getVertexProg),
	EXP_PYMETHODTABLE(BL_Shader, getFragmentProg),
	EXP_PYMETHODTABLE(BL_Shader, validate),
	// access functions
	EXP_PYMETHODTABLE(BL_Shader, isValid),
	EXP_PYMETHODTABLE(BL_Shader, setUniformEyef),
	EXP_PYMETHODTABLE(BL_Shader, setUniform1f),
	EXP_PYMETHODTABLE(BL_Shader, setUniform2f),
	EXP_PYMETHODTABLE(BL_Shader, setUniform3f),
	EXP_PYMETHODTABLE(BL_Shader, setUniform4f),
	EXP_PYMETHODTABLE(BL_Shader, setUniform1i),
	EXP_PYMETHODTABLE(BL_Shader, setUniform2i),
	EXP_PYMETHODTABLE(BL_Shader, setUniform3i),
	EXP_PYMETHODTABLE(BL_Shader, setUniform4i),
	EXP_PYMETHODTABLE(BL_Shader, setAttrib),
	EXP_PYMETHODTABLE(BL_Shader, setUniformfv),
	EXP_PYMETHODTABLE(BL_Shader, setUniformiv),
	EXP_PYMETHODTABLE(BL_Shader, setUniformDef),
	EXP_PYMETHODTABLE(BL_Shader, setSampler),
	EXP_PYMETHODTABLE(BL_Shader, setUniformMatrix4),
	EXP_PYMETHODTABLE(BL_Shader, setUniformMatrix3),
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef BL_Shader::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("enabled", BL_Shader, pyattr_get_enabled, pyattr_set_enabled),
	EXP_PYATTRIBUTE_RW_FUNCTION("bindCallbacks", BL_Shader, pyattr_get_callbacks, pyattr_set_callbacks),
	EXP_PYATTRIBUTE_RW_FUNCTION("objectCallbacks", BL_Shader, pyattr_get_callbacks, pyattr_set_callbacks),
	EXP_PYATTRIBUTE_NULL //Sentinel
};

PyTypeObject BL_Shader::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"BL_Shader",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_PyObjectPlus::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyObject *BL_Shader::pyattr_get_enabled(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Shader *self = static_cast<BL_Shader *>(self_v);
	return PyBool_FromLong(self->GetEnabled());
}

int BL_Shader::pyattr_set_enabled(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Shader *self = static_cast<BL_Shader *>(self_v);
	int param = PyObject_IsTrue(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "shader.enabled = bool: BL_Shader, expected True or False");
		return PY_SET_ATTR_FAIL;
	}

	self->SetEnabled(param);
	return PY_SET_ATTR_SUCCESS;
}

static std::map<const std::string, BL_Shader::CallbacksType> callbacksTable = {
	{"bindCallbacks", BL_Shader::CALLBACKS_BIND},
	{"objectCallbacks", BL_Shader::CALLBACKS_OBJECT}
};

PyObject *BL_Shader::pyattr_get_callbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Shader *self = static_cast<BL_Shader *>(self_v);
	PyObject *callbacks = self->GetCallbacks(callbacksTable[attrdef->m_name]);
	Py_INCREF(callbacks);
	return callbacks;
}

int BL_Shader::pyattr_set_callbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Shader *self = static_cast<BL_Shader *>(self_v);
	if (!PyList_CheckExact(value)) {
		PyErr_Format(PyExc_AttributeError, "shader.%s = bool: BL_Shader, expected a list", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->SetCallbacks(callbacksTable[attrdef->m_name], value);
	return PY_SET_ATTR_SUCCESS;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setSource, " setSource(vertexProgram, fragmentProgram, apply)")
{
	if (m_shader) {
		// already set...
		Py_RETURN_NONE;
	}

	char *v, *f;
	int apply = 0;

	if (PyArg_ParseTuple(args, "ssi:setSource", &v, &f, &apply)) {
		m_progs[VERTEX_PROGRAM] = std::string(v);
		m_progs[FRAGMENT_PROGRAM] = std::string(f);
		m_progs[GEOMETRY_PROGRAM] = "";

		if (LinkProgram()) {
			m_use = apply != 0;
			Py_RETURN_NONE;
		}

		m_progs[VERTEX_PROGRAM] = "";
		m_progs[FRAGMENT_PROGRAM] = "";
		m_use = false;
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setSourceList, " setSourceList(sources, apply)")
{
	if (m_shader) {
		// already set...
		Py_RETURN_NONE;
	}

	PyObject *pydict;
	int apply = 0;

	if (!PyArg_ParseTuple(args, "O!i:setSourceList", &PyDict_Type, &pydict, &apply)) {
		return nullptr;
	}

	bool error = false;
	static const char *progname[MAX_PROGRAM] = {"vertex", "fragment", "geometry"};
	static const bool optional[MAX_PROGRAM] = {false, false, true};

	for (unsigned short i = 0; i < MAX_PROGRAM; ++i) {
		PyObject *pyprog = PyDict_GetItemString(pydict, progname[i]);
		if (!optional[i]) {
			if (!pyprog) {
				error = true;
				PyErr_Format(PyExc_SystemError, "setSourceList(sources, apply): BL_Shader, non optional %s program missing", progname[i]);
				break;
			}
			else if (!PyUnicode_Check(pyprog)) {
				error = true;
				PyErr_Format(PyExc_SystemError, "setSourceList(sources, apply): BL_Shader, non optional %s program is not a string", progname[i]);
				break;
			}
		}
		if (pyprog) {
			m_progs[i] = std::string(_PyUnicode_AsString(pyprog));
		}
	}

	if (error) {
		for (unsigned short i = 0; i < MAX_PROGRAM; ++i) {
			m_progs[i] = "";
		}
		m_use = false;
		return nullptr;
	}

	if (LinkProgram()) {
		m_use = apply != 0;
	}

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(BL_Shader, delSource, "delSource( )")
{
	ClearUniforms();
	DeleteShader();
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(BL_Shader, isValid, "isValid()")
{
	return PyBool_FromLong(m_shader != nullptr);
}

EXP_PYMETHODDEF_DOC(BL_Shader, getVertexProg, "getVertexProg( )")
{
	return PyUnicode_FromStdString(m_progs[VERTEX_PROGRAM]);
}

EXP_PYMETHODDEF_DOC(BL_Shader, getFragmentProg, "getFragmentProg( )")
{
	return PyUnicode_FromStdString(m_progs[FRAGMENT_PROGRAM]);
}

EXP_PYMETHODDEF_DOC(BL_Shader, validate, "validate()")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	if (!m_shader) {
		PyErr_SetString(PyExc_TypeError, "shader.validate(): BL_Shader, invalid shader object");
		return nullptr;
	}

	ValidateProgram();

	Py_RETURN_NONE;
}


EXP_PYMETHODDEF_DOC(BL_Shader, setSampler, "setSampler(name, index)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int index = -1;

	if (PyArg_ParseTuple(args, "si:setSampler", &uniform, &index)) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			if (index >= RAS_Texture::MaxUnits || index < 0) {
				CM_Warning("invalid texture sample index: " << index);
			}
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT, &index, (sizeof(int)), 1);
#else
			SetUniform(loc, index);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

/// access functions
EXP_PYMETHODDEF_DOC(BL_Shader, setUniform1f, "setUniform1f(name, fx)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float value = 0.0f;

	if (PyArg_ParseTuple(args, "sf:setUniform1f", &uniform, &value)) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformfv(loc, RAS_Uniform::UNI_FLOAT, &value, sizeof(float), 1);
#else
			SetUniform(loc, (float)value);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform2f, "setUniform2f(name, fx, fy)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float array[2] = {0.0f, 0.0f};

	if (PyArg_ParseTuple(args, "sff:setUniform2f", &uniform, &array[0], &array[1])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformfv(loc, RAS_Uniform::UNI_FLOAT2, array, (sizeof(float) * 2), 1);
#else
			SetUniform(loc, array, 2);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform3f, "setUniform3f(name, fx,fy,fz) ")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float array[3] = {0.0f, 0.0f, 0.0f};

	if (PyArg_ParseTuple(args, "sfff:setUniform3f", &uniform, &array[0], &array[1], &array[2])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformfv(loc, RAS_Uniform::UNI_FLOAT3, array, (sizeof(float) * 3), 1);
#else
			SetUniform(loc, array, 3);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform4f, "setUniform4f(name, fx,fy,fz, fw) ")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float array[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	if (PyArg_ParseTuple(args, "sffff:setUniform4f", &uniform, &array[0], &array[1], &array[2], &array[3])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformfv(loc, RAS_Uniform::UNI_FLOAT4, array, (sizeof(float) * 4), 1);
#else
			SetUniform(loc, array, 4);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniformEyef, "setUniformEyef(name)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}
	const char *uniform;
	if (PyArg_ParseTuple(args, "s:setUniformEyef", &uniform)) {
		int loc = GetUniformLocation(uniform);
		if (loc != -1) {
			bool defined = false;
			for (RAS_DefUniform *defuni : m_preDef) {
				if (defuni->m_loc == loc) {
					defined = true;
					break;
				}
			}

			if (defined) {
				Py_RETURN_NONE;
			}

			RAS_DefUniform *uni = new RAS_DefUniform();
			uni->m_loc = loc;
			uni->m_type = EYE;
			uni->m_flag = 0;
			m_preDef.push_back(uni);
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform1i, "setUniform1i(name, ix)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int value = 0;

	if (PyArg_ParseTuple(args, "si:setUniform1i", &uniform, &value)) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT, &value, sizeof(int), 1);
#else
			SetUniform(loc, (int)value);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform2i, "setUniform2i(name, ix, iy)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int array[2] = {0, 0};

	if (PyArg_ParseTuple(args, "sii:setUniform2i", &uniform, &array[0], &array[1])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT2, array, sizeof(int) * 2, 1);
#else
			SetUniform(loc, array, 2);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform3i, "setUniform3i(name, ix,iy,iz) ")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int array[3] = {0, 0, 0};

	if (PyArg_ParseTuple(args, "siii:setUniform3i", &uniform, &array[0], &array[1], &array[2])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT3, array, sizeof(int) * 3, 1);
#else
			SetUniform(loc, array, 3);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform4i, "setUniform4i(name, ix,iy,iz, iw) ")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int array[4] = {0, 0, 0, 0};

	if (PyArg_ParseTuple(args, "siiii:setUniform4i", &uniform, &array[0], &array[1], &array[2], &array[3])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT4, array, sizeof(int) * 4, 1);
#else
			SetUniform(loc, array, 4);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniformfv, "setUniformfv(float (list2 or list3 or list4))")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform = "";
	PyObject *listPtr = nullptr;
	float array_data[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	if (PyArg_ParseTuple(args, "sO:setUniformfv", &uniform, &listPtr)) {
		int loc = GetUniformLocation(uniform);
		if (loc != -1) {
			if (PySequence_Check(listPtr)) {
				unsigned int list_size = PySequence_Size(listPtr);

				for (unsigned int i = 0; (i < list_size && i < 4); i++) {
					PyObject *item = PySequence_GetItem(listPtr, i);
					array_data[i] = (float)PyFloat_AsDouble(item);
					Py_DECREF(item);
				}

				switch (list_size) {
					case 2:
					{
						float array2[2] = {array_data[0], array_data[1]};
#ifdef SORT_UNIFORMS
						SetUniformfv(loc, RAS_Uniform::UNI_FLOAT2, array2, sizeof(float) * 2, 1);
#else
						SetUniform(loc, array2, 2);
#endif
						Py_RETURN_NONE;
						break;
					}
					case 3:
					{
						float array3[3] = {array_data[0], array_data[1], array_data[2]};
#ifdef SORT_UNIFORMS
						SetUniformfv(loc, RAS_Uniform::UNI_FLOAT3, array3, sizeof(float) * 3, 1);
#else
						SetUniform(loc, array3, 3);
#endif
						Py_RETURN_NONE;
						break;
					}
					case 4:
					{
						float array4[4] = {array_data[0], array_data[1], array_data[2], array_data[3]};
#ifdef SORT_UNIFORMS
						SetUniformfv(loc, RAS_Uniform::UNI_FLOAT4, array4, sizeof(float) * 4, 1);
#else
						SetUniform(loc, array4, 4);
#endif
						Py_RETURN_NONE;
						break;
					}
					default:
					{
						PyErr_SetString(PyExc_TypeError,
						                "shader.setUniform4i(name, ix,iy,iz, iw): BL_Shader. invalid list size");
						return nullptr;
						break;
					}
				}
			}
		}
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniformiv, "setUniformiv(uniform_name, (list2 or list3 or list4))")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform = "";
	PyObject *listPtr = nullptr;
	int array_data[4] = {0, 0, 0, 0};

	if (!PyArg_ParseTuple(args, "sO:setUniformiv", &uniform, &listPtr)) {
		return nullptr;
	}

	int loc = GetUniformLocation(uniform);

	if (loc == -1) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformiv(...): BL_Shader, first string argument is not a valid uniform value");
		return nullptr;
	}

	if (!PySequence_Check(listPtr)) {
		PyErr_SetString(PyExc_TypeError, "shader.setUniformiv(...): BL_Shader, second argument is not a sequence");
		return nullptr;
	}

	unsigned int list_size = PySequence_Size(listPtr);

	for (unsigned int i = 0; (i < list_size && i < 4); i++) {
		PyObject *item = PySequence_GetItem(listPtr, i);
		array_data[i] = PyLong_AsLong(item);
		Py_DECREF(item);
	}

	if (PyErr_Occurred()) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformiv(...): BL_Shader, one or more values in the list is not an int");
		return nullptr;
	}

	// Sanity checks done!
	switch (list_size) {
		case 2:
		{
			int array2[2] = {array_data[0], array_data[1]};
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT2, array2, sizeof(int) * 2, 1);
#else
			SetUniform(loc, array2, 2);
#endif
			Py_RETURN_NONE;
			break;
		}
		case 3:
		{
			int array3[3] = {array_data[0], array_data[1], array_data[2]};
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT3, array3, sizeof(int) * 3, 1);
#else
			SetUniform(loc, array3, 3);
#endif
			Py_RETURN_NONE;
			break;
		}
		case 4:
		{
			int array4[4] = {array_data[0], array_data[1], array_data[2], array_data[3]};
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT4, array4, sizeof(int) * 4, 1);
#else
			SetUniform(loc, array4, 4);
#endif
			Py_RETURN_NONE;
			break;
		}
		default:
		{
			PyErr_SetString(PyExc_TypeError,
			                "shader.setUniformiv(...): BL_Shader, second argument, invalid list size, expected an int "
			                "list between 2 and 4");
			return nullptr;
			break;
		}
	}
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniformMatrix4,
                    "setUniformMatrix4(uniform_name, mat-4x4, transpose(row-major=true, col-major=false)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	PyObject *matrix = nullptr;
	int transp = 0; // python use column major by default, so no transpose....

	if (!PyArg_ParseTuple(args, "sO|i:setUniformMatrix4", &uniform, &matrix, &transp)) {
		return nullptr;
	}

	int loc = GetUniformLocation(uniform);

	if (loc == -1) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformMatrix4(...): BL_Shader, first string argument is not a valid uniform value");
		return nullptr;
	}

	mt::mat4 mat;

	if (!PyMatTo(matrix, mat)) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformMatrix4(...): BL_Shader, second argument cannot be converted into a 4x4 matrix");
		return nullptr;
	}

	// Sanity checks done!
#ifdef SORT_UNIFORMS
	SetUniformfv(loc, RAS_Uniform::UNI_MAT4, (float *)mat.Data(), (sizeof(float) * 16), 1, (transp != 0));
#else
	SetUniform(loc, mat, (transp != 0));
#endif
	Py_RETURN_NONE;
}


EXP_PYMETHODDEF_DOC(BL_Shader, setUniformMatrix3,
                    "setUniformMatrix3(uniform_name, list[3x3], transpose(row-major=true, col-major=false)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	PyObject *matrix = nullptr;
	int transp = 0; // python use column major by default, so no transpose....

	if (!PyArg_ParseTuple(args, "sO|i:setUniformMatrix3", &uniform, &matrix, &transp)) {
		return nullptr;
	}

	int loc = GetUniformLocation(uniform);

	if (loc == -1) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformMatrix3(...): BL_Shader, first string argument is not a valid uniform value");
		return nullptr;
	}

	mt::mat3 mat;

	if (!PyMatTo(matrix, mat)) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformMatrix3(...): BL_Shader, second argument cannot be converted into a 3x3 matrix");
		return nullptr;
	}

#ifdef SORT_UNIFORMS
	float matr[9];
	mat.Pack(matr);
	SetUniformfv(loc, RAS_Uniform::UNI_MAT3, matr, (sizeof(float) * 9), 1, (transp != 0));
#else
	SetUniform(loc, mat, (transp != 0));
#endif
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setAttrib, "setAttrib(enum)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	int attr = 0;

	if (!PyArg_ParseTuple(args, "i:setAttrib", &attr)) {
		return nullptr;
	}

	attr = SHD_TANGENT; // user input is ignored for now, there is only 1 attr

	if (!m_shader) {
		PyErr_SetString(PyExc_ValueError, "shader.setAttrib() BL_Shader, invalid shader object");
		return nullptr;
	}

	// Avoid redundant attributes reconstruction.
	if (attr == m_attr) {
		Py_RETURN_NONE;
	}

	m_attr = (AttribTypes)attr;

	// Can be null in case of filter shaders.
	if (m_materialUpdateServer) {
		// Notify all clients tracking this shader that attributes are modified.
		m_materialUpdateServer->NotifyUpdate(RAS_IMaterial::ATTRIBUTES_MODIFIED);
	}

	BindAttribute("Tangent", m_attr);
	Py_RETURN_NONE;
}


EXP_PYMETHODDEF_DOC(BL_Shader, setUniformDef, "setUniformDef(name, enum)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int nloc = 0;
	if (PyArg_ParseTuple(args, "si:setUniformDef", &uniform, &nloc)) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			bool defined = false;
			for (RAS_DefUniform *defuni : m_preDef) {
				if (defuni->m_loc == loc) {
					defined = true;
					break;
				}
			}

			if (defined) {
				Py_RETURN_NONE;
			}

			RAS_DefUniform *uni = new RAS_DefUniform();
			uni->m_loc = loc;
			uni->m_type = nloc;
			uni->m_flag = 0;
			m_preDef.push_back(uni);
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

#endif // WITH_PYTHON
