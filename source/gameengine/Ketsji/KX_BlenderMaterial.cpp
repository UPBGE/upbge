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

/** \file gameengine/Ketsji/KX_BlenderMaterial.cpp
 *  \ingroup ketsji
 */

#include "KX_BlenderMaterial.h"
#include "KX_Scene.h"
#include "KX_PyMath.h"

#include "KX_MaterialShader.h"
#include "BL_Shader.h"

#include "EXP_ListWrapper.h"

#include "RAS_BucketManager.h"
#include "RAS_Rasterizer.h"

extern "C" {
#  include "DNA_material_types.h"
#  include "eevee_private.h"
#  include "GPU_material.h"
#  include "../gpu/intern/gpu_codegen.h"
}

KX_BlenderMaterial::KX_BlenderMaterial(
		RAS_Rasterizer *rasty,
		KX_Scene *scene,
		Material *mat,
		const std::string& name,
		GameSettings *game,
		int lightlayer)
	:RAS_IPolyMaterial(name, game),
	m_material(mat),
	m_shader(nullptr),
	m_rasterizer(rasty),
	m_scene(scene),
	m_userDefBlend(false),
	m_constructed(false),
	m_lightLayer(lightlayer)
{
	m_alphablend = mat->blend_method;

	if (m_material->use_nodes && m_material->nodetree) {
		EEVEE_Data *vedata = EEVEE_engine_data_get();
		EEVEE_EffectsInfo *effects = vedata->stl->effects;
		const bool use_ssrefract = ((m_material->blend_flag & MA_BL_SS_REFRACTION) != 0) &&
                               ((effects->enabled_effects & EFFECT_REFRACT) != 0);
		m_gpuMat = EEVEE_material_mesh_get(scene->GetBlenderScene(), m_material, vedata,
			false, use_ssrefract);
	}
	else {
		m_gpuMat = nullptr;
	}
}

KX_BlenderMaterial::~KX_BlenderMaterial()
{
	// cleanup work
	if (m_constructed) {
		// clean only if material was actually used
		OnExit();
	}
}

RAS_MaterialShader *KX_BlenderMaterial::GetShader() const
{
	if (m_shader && m_shader->IsValid()) {
		return m_shader.get();
	}

	// Should never happen.
	BLI_assert(false);

	return nullptr;
}

void KX_BlenderMaterial::GetRGBAColor(unsigned char *rgba) const
{
	if (m_material) {
		*rgba++ = (unsigned char)(m_material->r * 255.0f);
		*rgba++ = (unsigned char)(m_material->g * 255.0f);
		*rgba++ = (unsigned char)(m_material->b * 255.0f);
		*rgba++ = (unsigned char)(m_material->alpha * 255.0f);
	}
	else
		RAS_IPolyMaterial::GetRGBAColor(rgba);
}

const std::string KX_BlenderMaterial::GetTextureName() const
{
	return (m_textures[0] ? m_textures[0]->GetName() : "");
}

Material *KX_BlenderMaterial::GetBlenderMaterial() const
{
	return m_material;
}

Scene *KX_BlenderMaterial::GetBlenderScene() const
{
	return m_scene->GetBlenderScene();
}

SCA_IScene *KX_BlenderMaterial::GetScene() const
{
	return m_scene;
}

void KX_BlenderMaterial::ReleaseMaterial()
{
}

void KX_BlenderMaterial::InitTextures()
{
	if (!m_gpuMat) {
		return;
	}
	GPUPass *gpupass = GPU_material_get_pass(m_gpuMat);

	if (!gpupass) {
		/* Shader compilation error */
		return;
	}

	/* Converting dynamic GPUInput to DRWUniform */
	ListBase *inputs = GPU_material_get_inputs(m_gpuMat);

	int i = 0;
	for (GPUInput *input = (GPUInput *)inputs->first; input; input = input->next) {
		/* Textures */
		if (input->ima) {
			BL_Texture *texture = new BL_Texture(input);
			m_textures[i] = texture;
			i++;
		}
	}
}

void KX_BlenderMaterial::OnConstruction()
{
	if (m_constructed) {
		// when material are reused between objects
		return;
	}

	InitTextures();

	m_blendFunc[0] = 0;
	m_blendFunc[1] = 0;
	m_constructed = true;
}

void KX_BlenderMaterial::EndFrame(RAS_Rasterizer *rasty)
{
	RAS_Texture::DesactiveTextures();
}

void KX_BlenderMaterial::OnExit()
{
	if (m_shader) {
		m_shader.reset(nullptr);
	}
}


void KX_BlenderMaterial::SetShaderData(RAS_Rasterizer *ras)
{
#if 0
	BLI_assert(m_shader);

	int i;

	m_shader->SetProg(true);

	m_shader->ApplyShader();

	/** We make sure that all gpu textures are the same in material textures here
	 * than in gpu material. This is dones in a separated loop because the texture
	 * regeneration can overide bind settings of the previous texture.
	 */
	for (i = 0; i < RAS_Texture::MaxUnits; i++) {
		if (m_textures[i] && m_textures[i]->Ok()) {
			m_textures[i]->CheckValidTexture();
		}
	}

	// for each enabled unit
	for (i = 0; i < RAS_Texture::MaxUnits; i++) {
		if (m_textures[i] && m_textures[i]->Ok()) {
			m_textures[i]->ActivateTexture(i);
		}
	}

	if (!m_userDefBlend) {
		ras->SetAlphaBlend(m_alphablend);
	}
	else {
		ras->SetAlphaBlend(GPU_BLEND_SOLID);
		ras->SetAlphaBlend(-1); // indicates custom mode

		// tested to be valid enums
		ras->Enable(RAS_Rasterizer::RAS_BLEND);
		ras->SetBlendFunc((RAS_Rasterizer::BlendFunc)m_blendFunc[0], (RAS_Rasterizer::BlendFunc)m_blendFunc[1]);
	}

	// Disable :
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; i++) {
		if (m_textures[i] && m_textures[i]->Ok()) {
			m_textures[i]->DisableTexture();
		}
	}
#endif
}

bool KX_BlenderMaterial::UsesLighting() const
{
	if (!RAS_IPolyMaterial::UsesLighting())
		return false;

	if (m_shader && m_shader->IsValid()) {
		return true;
	}
	else {
		return true;
	}
}

void KX_BlenderMaterial::UpdateIPO(
    MT_Vector4 rgba,
    MT_Vector3 specrgb,
    MT_Scalar hard,
    MT_Scalar spec,
    MT_Scalar ref,
    MT_Scalar emit,
	MT_Scalar ambient,
    MT_Scalar alpha,
	MT_Scalar specalpha)
{
	// only works one deep now

	// GLSL								Input
	m_material->specr = (float)(specrgb)[0];
	m_material->specg = (float)(specrgb)[1];
	m_material->specb = (float)(specrgb)[2];
	m_material->r = (float)(rgba[0]);
	m_material->g = (float)(rgba[1]);
	m_material->b = (float)(rgba[2]);
	m_material->alpha = (float)(rgba[3]);
}

void KX_BlenderMaterial::ReplaceScene(KX_Scene *scene)
{
	m_scene = scene;

	OnConstruction();
}

std::string KX_BlenderMaterial::GetName()
{
	return m_name;
}

#ifdef WITH_PYTHON

PyMethodDef KX_BlenderMaterial::Methods[] =
{
	KX_PYMETHODTABLE(KX_BlenderMaterial, getShader),
	KX_PYMETHODTABLE(KX_BlenderMaterial, setBlending),
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_BlenderMaterial::Attributes[] = {
	KX_PYATTRIBUTE_RO_FUNCTION("textures", KX_BlenderMaterial, pyattr_get_textures),
	KX_PYATTRIBUTE_NULL //Sentinel
};

PyTypeObject KX_BlenderMaterial::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_BlenderMaterial",
	sizeof(PyObjectPlus_Proxy),
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
	&CValue::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

static int kx_blender_material_get_textures_size_cb(void *self_v)
{
	return RAS_Texture::MaxUnits;
}

static PyObject *kx_blender_material_get_textures_item_cb(void *self_v, int index)
{
	BL_Texture *tex = (BL_Texture *)((KX_BlenderMaterial *)self_v)->GetTexture(index);
	PyObject *item = nullptr;
	if (tex) {
		item = tex->GetProxy();
	}
	else {
		item = Py_None;
		Py_INCREF(Py_None);
	}
	return item;
}

static const std::string kx_blender_material_get_textures_item_name_cb(void *self_v, int index)
{
	BL_Texture *tex = (BL_Texture *)((KX_BlenderMaterial *)self_v)->GetTexture(index);
	return (tex ? tex->GetName() : "");
}

PyObject *KX_BlenderMaterial::pyattr_get_textures(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	return (new CListWrapper(self_v,
							 ((KX_BlenderMaterial *)self_v)->GetProxy(),
							 nullptr,
							 kx_blender_material_get_textures_size_cb,
							 kx_blender_material_get_textures_item_cb,
							 kx_blender_material_get_textures_item_name_cb,
							 nullptr))->NewProxy(true);
}

KX_PYMETHODDEF_DOC(KX_BlenderMaterial, getShader, "getShader()")
{
	/* EEVEE: Any way to restore Custom shaders without bge rendering pipeline */
	if (!m_shader) {
		m_shader.reset(new KX_MaterialShader());
		// Set the material to use custom shader.
		m_flag &= ~RAS_BLENDERGLSL;
		m_scene->GetBucketManager()->UpdateShaders(this);
	}

	return m_shader->GetShader()->GetProxy();
}

static const unsigned int GL_array[11] = {
	RAS_Rasterizer::RAS_ZERO,
	RAS_Rasterizer::RAS_ONE,
	RAS_Rasterizer::RAS_SRC_COLOR,
	RAS_Rasterizer::RAS_ONE_MINUS_SRC_COLOR,
	RAS_Rasterizer::RAS_DST_COLOR,
	RAS_Rasterizer::RAS_ONE_MINUS_DST_COLOR,
	RAS_Rasterizer::RAS_SRC_ALPHA,
	RAS_Rasterizer::RAS_ONE_MINUS_SRC_ALPHA,
	RAS_Rasterizer::RAS_DST_ALPHA,
	RAS_Rasterizer::RAS_ONE_MINUS_DST_ALPHA,
	RAS_Rasterizer::RAS_SRC_ALPHA_SATURATE
};

KX_PYMETHODDEF_DOC(KX_BlenderMaterial, setBlending, "setBlending(bge.logic.src, bge.logic.dest)")
{
	unsigned int b[2];
	if (PyArg_ParseTuple(args, "ii:setBlending", &b[0], &b[1])) {
		bool value_found[2] = {false, false};
		for (int i = 0; i < 11; i++) {
			if (b[0] == GL_array[i]) {
				value_found[0] = true;
				m_blendFunc[0] = b[0];
			}
			if (b[1] == GL_array[i]) {
				value_found[1] = true;
				m_blendFunc[1] = b[1];
			}
			if (value_found[0] && value_found[1]) {
				break;
			}
		}
		if (!value_found[0] || !value_found[1]) {
			PyErr_SetString(PyExc_ValueError, "material.setBlending(int, int): KX_BlenderMaterial, invalid enum.");
			return nullptr;
		}
		m_userDefBlend = true;
		Py_RETURN_NONE;
	}
	return nullptr;
}

bool ConvertPythonToMaterial(PyObject *value, KX_BlenderMaterial **material, bool py_none_ok, const char *error_prefix)
{
	if (value == nullptr) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
		*material = nullptr;
		return false;
	}

	if (value == Py_None) {
		*material = nullptr;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected KX_BlenderMaterial or a KX_BlenderMaterial name, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &KX_BlenderMaterial::Type)) {
		KX_BlenderMaterial *mat = static_cast<KX_BlenderMaterial *>BGE_PROXY_REF(value);

		/* sets the error */
		if (mat == nullptr) {
			PyErr_Format(PyExc_SystemError, "%s, " BGE_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		*material = mat;
		return true;
	}

	*material = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_BlenderMaterial, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_BlenderMaterial or a string", error_prefix);
	}

	return false;
}

#endif // WITH_PYTHON
