#include "KX_MaterialShader.h"
#include "BL_Material.h"
#include "KX_GameObject.h"

#include "KX_Shader.h"

#include "RAS_MeshUser.h"

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#endif  // WITH_PYTHON

// Needed for GetAttribs using MTex.
#include "DNA_texture_types.h"
#include "DNA_material_types.h"

#include "GPU_material.h"

KX_MaterialShader::KX_MaterialShader(BL_Material *material, bool useLigthings, int alphaBlend)
	:m_material(material),
	m_useLightings(useLigthings),
	m_attr(SHD_NONE),
	m_alphaBlend(alphaBlend)
{
#ifdef WITH_PYTHON
	for (unsigned short i = 0; i < CALLBACKS_MAX; ++i) {
		m_callbacks[i] = PyList_New(0);
	}
#endif  // WITH_PYTHON
}

KX_MaterialShader::~KX_MaterialShader()
{
#ifdef WITH_PYTHON
	for (unsigned short i = 0; i < CALLBACKS_MAX; ++i) {
		Py_XDECREF(m_callbacks[i]);
	}
#endif  // WITH_PYTHON
}

bool KX_MaterialShader::LinkProgram()
{
	// Notify all clients tracking this shader that shader is recompiled and attributes are invalidated.
	m_material->NotifyUpdate(RAS_IMaterial::SHADER_MODIFIED | RAS_IMaterial::ATTRIBUTES_MODIFIED);

	return RAS_Shader::LinkProgram();
}

bool KX_MaterialShader::Ok() const
{
	return KX_Shader::Ok();
}

bool KX_MaterialShader::GetError() const
{
	return KX_Shader::GetError();
}

#ifdef WITH_PYTHON

PyObject *KX_MaterialShader::GetCallbacks(KX_MaterialShader::CallbacksType type)
{
	return m_callbacks[type];
}

void KX_MaterialShader::SetCallbacks(KX_MaterialShader::CallbacksType type, PyObject *callbacks)
{
	Py_XDECREF(m_callbacks[type]);
	Py_INCREF(callbacks);
	m_callbacks[type] = callbacks;
}

#endif  // WITH_PYTHON

void KX_MaterialShader::Prepare(RAS_Rasterizer *rasty)
{
}

void KX_MaterialShader::Activate(RAS_Rasterizer* rasty)
{
#ifdef WITH_PYTHON
	if (PyList_GET_SIZE(m_callbacks[CALLBACKS_BIND]) > 0) {
		EXP_RunPythonCallBackList(m_callbacks[CALLBACKS_BIND], nullptr, 0, 0);
	}
#endif  // WITH_PYTHON

	BindProg();
	ApplyShader();

	m_material->ActivateTextures();

	Material *ma = m_material->GetBlenderMaterial();
	rasty->SetSpecularity(ma->specr * ma->spec, ma->specg * ma->spec,
	                      ma->specb * ma->spec, ma->spec);
	rasty->SetShinyness(((float)ma->har) / 4.0f);
	rasty->SetDiffuse(ma->r * ma->ref + ma->emit, ma->g * ma->ref + ma->emit,
	                  ma->b * ma->ref + ma->emit, 1.0f);
	rasty->SetEmissive(ma->r * ma->emit, ma->g * ma->emit,
	                   ma->b * ma->emit, 1.0f);
	rasty->SetAmbient(ma->amb);
}

void KX_MaterialShader::Deactivate(RAS_Rasterizer* rasty)
{
	UnbindProg();

	m_material->DeactivateTextures();
}

void KX_MaterialShader::ActivateInstancing(RAS_Rasterizer *rasty, RAS_InstancingBuffer *buffer)
{
}

void KX_MaterialShader::ActivateMeshUser(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans)
{
#ifdef WITH_PYTHON
	if (PyList_GET_SIZE(m_callbacks[CALLBACKS_OBJECT]) > 0) {
		KX_GameObject *gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)meshUser->GetClientObject());
		PyObject *args[] = {gameobj->GetProxy()};
		EXP_RunPythonCallBackList(m_callbacks[CALLBACKS_OBJECT], args, 0, ARRAY_SIZE(args));
	}
#endif  // WITH_PYTHON

	RAS_Shader::Update(rasty, mt::mat4(meshUser->GetMatrix()));

	ApplyShader();
	// Update OpenGL lighting builtins.
	rasty->ProcessLighting(m_useLightings, camtrans);

	if (m_material->GetUserBlend()) {
		rasty->SetAlphaBlend(GPU_BLEND_SOLID);
		rasty->SetAlphaBlend(-1); // indicates custom mode

		// tested to be valid enums
		rasty->Enable(RAS_Rasterizer::RAS_BLEND);

		const RAS_Rasterizer::BlendFunc (&blendFunc)[2] = m_material->GetBlendFunc();
		rasty->SetBlendFunc(blendFunc[0], blendFunc[1]);
	}
	else {
		rasty->SetAlphaBlend(m_alphaBlend);
	}
}

const RAS_AttributeArray::AttribList KX_MaterialShader::GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const
{
	RAS_AttributeArray::AttribList attribs;
	// Initialize textures attributes.
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; ++i) {
		RAS_Texture *texture = m_material->GetTexture(i);
		/* Here textures can return false to Ok() because we're looking only at
		 * texture attributes and not texture bind id like for the binding and
		 * unbinding of textures. A nullptr RAS_Texture means that the corresponding
		 * mtex is nullptr too (see BL_Material::InitTextures).*/
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

RAS_InstancingBuffer::Attrib KX_MaterialShader::GetInstancingAttribs() const
{
	return RAS_InstancingBuffer::DEFAULT_ATTRIBS;
}

#ifdef WITH_PYTHON

PyMethodDef KX_MaterialShader::Methods[] = {
	EXP_PYMETHODTABLE(KX_MaterialShader, setAttrib),
	{nullptr, nullptr} // Sentinel
};

// TODO python doc
PyAttributeDef KX_MaterialShader::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("bindCallbacks", KX_MaterialShader, pyattr_get_callbacks, pyattr_set_callbacks),
	EXP_PYATTRIBUTE_RW_FUNCTION("objectCallbacks", KX_MaterialShader, pyattr_get_callbacks, pyattr_set_callbacks),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyTypeObject KX_MaterialShader::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_MaterialShader",
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
	&KX_Shader::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

static std::map<const std::string, KX_MaterialShader::CallbacksType> callbacksTable = {
	{"bindCallbacks", KX_MaterialShader::CALLBACKS_BIND},
	{"objectCallbacks", KX_MaterialShader::CALLBACKS_OBJECT}
};

PyObject *KX_MaterialShader::pyattr_get_callbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_MaterialShader *self = static_cast<KX_MaterialShader *>(self_v);
	PyObject *callbacks = self->GetCallbacks(callbacksTable[attrdef->m_name]);
	Py_INCREF(callbacks);
	return callbacks;
}

int KX_MaterialShader::pyattr_set_callbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_MaterialShader *self = static_cast<KX_MaterialShader *>(self_v);
	if (!PyList_CheckExact(value)) {
		PyErr_Format(PyExc_AttributeError, "shader.%s = bool: KX_MaterialShader, expected a list", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->SetCallbacks(callbacksTable[attrdef->m_name], value);
	return PY_SET_ATTR_SUCCESS;
}


EXP_PYMETHODDEF_DOC(KX_MaterialShader, setAttrib, "setAttrib(enum)")
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
		PyErr_SetString(PyExc_ValueError, "shader.setAttrib() KX_Shader, invalid shader object");
		return nullptr;
	}

	// Avoid redundant attributes reconstruction.
	if (attr == m_attr) {
		Py_RETURN_NONE;
	}

	m_attr = (AttribTypes)attr;

	// Notify all clients tracking this shader that attributes are modified.
	m_material->NotifyUpdate(RAS_IMaterial::ATTRIBUTES_MODIFIED);

	BindAttribute("Tangent", m_attr);
	Py_RETURN_NONE;
}

#endif  // WITH_PYTHON
