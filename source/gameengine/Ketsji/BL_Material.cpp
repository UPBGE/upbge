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

/** \file gameengine/Ketsji/BL_Material.cpp
 *  \ingroup ketsji
 */

#include "BL_Material.h"
#include "BL_MaterialShader.h"

#include "KX_Scene.h"
#include "KX_PyMath.h"
#include "KX_MaterialShader.h"

#include "EXP_ListWrapper.h"

#include "RAS_OverrideShader.h"
#include "RAS_BucketManager.h"
#include "RAS_Rasterizer.h"
#include "RAS_MeshUser.h"

#include "GPU_draw.h"
#include "GPU_material.h" // for GPU_BLEND_SOLID

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

BL_Material::BL_Material(Material *mat, const std::string& name, KX_Scene *scene)
	:RAS_IMaterial(name),
	m_scene(scene),
	m_material(mat),
	m_customShader(nullptr),
	m_blenderShader(nullptr),
	m_userDefBlend(false)
{
	// Save material data to restore on exit
	m_savedData.r = m_material->r;
	m_savedData.g = m_material->g;
	m_savedData.b = m_material->b;
	m_savedData.a = m_material->alpha;
	m_savedData.specr = m_material->specr;
	m_savedData.specg = m_material->specg;
	m_savedData.specb = m_material->specb;
	m_savedData.spec = m_material->spec;
	m_savedData.ref = m_material->ref;
	m_savedData.hardness = m_material->har;
	m_savedData.emit = m_material->emit;
	m_savedData.ambient = m_material->amb;
	m_savedData.specularalpha = m_material->spectra;

	m_alphaBlend = mat->game.alpha_blend;

	// with ztransp enabled, enforce alpha blending mode
	if ((mat->mode & MA_TRANSP) && (mat->mode & MA_ZTRANSP) && (m_alphaBlend == GEMAT_SOLID)) {
		m_alphaBlend = GEMAT_ALPHA;
	}

	m_zoffset = mat->zoffs;

	m_rasMode |= (mat->game.flag & GEMAT_INVISIBLE) ? 0 : RAS_VISIBLE;
	m_rasMode |= (mat->game.flag & GEMAT_NOPHYSICS) ? 0 : RAS_COLLIDER;
	m_rasMode |= (mat->game.flag & GEMAT_BACKCULL) ? 0 : RAS_TWOSIDED;
	m_rasMode |= (mat->material_type == MA_TYPE_WIRE) ? RAS_WIRE : 0;
	m_rasMode |= (mat->mode2 & MA_DEPTH_TRANSP) ? RAS_DEPTH_ALPHA : 0;

	if (ELEM(m_alphaBlend, GEMAT_CLIP, GEMAT_ALPHA_TO_COVERAGE)) {
		m_rasMode |= RAS_ALPHA_SHADOW;
	}
	// always zsort alpha + add
	else if (ELEM(m_alphaBlend, GEMAT_ALPHA, GEMAT_ALPHA_SORT, GEMAT_ADD)) {
		m_rasMode |= RAS_ALPHA;
		m_rasMode |= (mat && (mat->game.alpha_blend & GEMAT_ALPHA_SORT)) ? RAS_ZSORT : 0;
	}

	switch (mat->game.face_orientation) {
		case GEMAT_NORMAL:
		{
			m_drawingMode = RAS_NORMAL;
			break;
		}
		case GEMAT_BILLBOARD:
		{
			m_drawingMode = RAS_BILLBOARD;
			break;
		}
		case GEMAT_HALO:
		{
			m_drawingMode = RAS_HALO;
			break;
		}
		case GEMAT_SHADOW:
		{
			m_drawingMode = RAS_SHADOW;
			break;
		}
	}

	m_flag |= ((mat->mode & MA_SHLESS) != 0) ? 0 : RAS_MULTILIGHT;
	m_flag |= ((mat->mode2 & MA_CASTSHADOW) != 0) ? RAS_CASTSHADOW : 0;
	m_flag |= ((mat->mode & MA_ONLYCAST) != 0) ? RAS_ONLYSHADOW : 0;

	m_blendFunc[0] = RAS_Rasterizer::RAS_ZERO;
	m_blendFunc[1] = RAS_Rasterizer::RAS_ZERO;
}

BL_Material::~BL_Material()
{
	// Restore Blender material data
	m_material->r = m_savedData.r;
	m_material->g = m_savedData.g;
	m_material->b = m_savedData.b;
	m_material->alpha = m_savedData.a;
	m_material->specr = m_savedData.specr;
	m_material->specg = m_savedData.specg;
	m_material->specb = m_savedData.specb;
	m_material->spec = m_savedData.spec;
	m_material->ref = m_savedData.ref;
	m_material->har = m_savedData.hardness;
	m_material->emit = m_savedData.emit;
	m_material->amb = m_savedData.ambient;
	m_material->spectra = m_savedData.specularalpha;

	/* used to call with 'm_material->tface' but this can be a freed array,
	 * see: [#30493], so just call with nullptr, this is best since it clears
	 * the 'lastface' pointer in GPU too - campbell */
	GPU_set_tpage(nullptr, 1, m_alphaBlend);
}

bool BL_Material::GetUserBlend() const
{
	return m_userDefBlend;
}

const RAS_Rasterizer::BlendFunc (&BL_Material::GetBlendFunc() const)[2]
{
	return m_blendFunc;
}

RAS_IMaterialShader *BL_Material::GetShader(RAS_Rasterizer::DrawType drawingMode) const
{
	static const RAS_OverrideShader::Type overrideShaderType[RAS_Rasterizer::RAS_DRAW_MAX][RAS_IMaterialShader::GEOM_MAX] = {
		// {GEOM_NORMAL, GEOM_INSTANCING}
		// RAS_WIREFRAME
		{RAS_OverrideShader::RAS_OVERRIDE_SHADER_BLACK,
		 RAS_OverrideShader::RAS_OVERRIDE_SHADER_BLACK_INSTANCING},
		// RAS_TEXTURED, Never overrided.
		{RAS_OverrideShader::RAS_OVERRIDE_SHADER_BLACK,
		 RAS_OverrideShader::RAS_OVERRIDE_SHADER_BLACK_INSTANCING},
		// RAS_SHADOW
		{RAS_OverrideShader::RAS_OVERRIDE_SHADER_BLACK,
		 RAS_OverrideShader::RAS_OVERRIDE_SHADER_BLACK_INSTANCING},
		// RAS_SHADOW_VARIANCE
		{RAS_OverrideShader::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE,
		 RAS_OverrideShader::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING}
	};

	if (m_customShader && m_customShader->Ok()) {
		switch (drawingMode) {
			case RAS_Rasterizer::RAS_TEXTURED:
			{
				return m_customShader.get();
			}
			default:
			{
				return RAS_OverrideShader::GetShader(overrideShaderType[drawingMode][RAS_IMaterialShader::GEOM_NORMAL]);
			}
		}
	}
	else if (m_blenderShader && m_blenderShader->Ok()) {
		switch (drawingMode) {
			case RAS_Rasterizer::RAS_TEXTURED:
			{
				return m_blenderShader.get();
			}
			default:
			{
				return RAS_OverrideShader::GetShader(overrideShaderType[drawingMode][m_blenderShader->GetGeomMode()]);
			}
		}
	}

	return nullptr;
}

const std::string BL_Material::GetTextureName() const
{
	return (m_textures[0] ? m_textures[0]->GetName() : "");
}

Material *BL_Material::GetBlenderMaterial() const
{
	return m_material;
}

SCA_IScene *BL_Material::GetScene() const
{
	return m_scene;
}

void BL_Material::ReloadMaterial()
{
	BLI_assert(m_scene);

	if (m_blenderShader) {
		// If shader exists reload it.
		m_blenderShader->ReloadMaterial();
	}
	else {
		// Create shader.
		m_blenderShader.reset(new BL_MaterialShader(m_scene, this, m_material, m_alphaBlend));

		if (!m_blenderShader->Ok()) {
			m_blenderShader.reset(nullptr);
		}
	}
}

void BL_Material::Prepare()
{
	UpdateTextures();
}

void BL_Material::ReplaceScene(KX_Scene *scene)
{
	m_scene = scene;
}

void BL_Material::InitTextures()
{
	// for each unique material...
	int i;
	for (i = 0; i < RAS_Texture::MaxUnits; i++) {
		MTex *mtex = m_material->mtex[i];
		if (mtex && mtex->tex) {
			BL_Texture *texture = new BL_Texture(mtex);
			m_textures[i] = texture;
		}
	}
}

void BL_Material::EndFrame(RAS_Rasterizer *rasty)
{
	rasty->SetAlphaBlend(GPU_BLEND_SOLID);
	RAS_Texture::DesactiveTextures();
}

void BL_Material::UpdateTextures()
{
	/** We make sure that all gpu textures are the same in material textures here
	 * than in gpu material. This is dones in a separated loop because the texture
	 * regeneration can overide opengl bind settings of the previous texture.
	 */
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; i++) {
		RAS_Texture *tex = m_textures[i];
		if (tex && tex->Ok()) {
			tex->CheckValidTexture();
		}
	}
}

void BL_Material::ApplyTextures()
{
	// for each enabled unit
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; i++) {
		if (m_textures[i] && m_textures[i]->Ok()) {
			m_textures[i]->ActivateTexture(i);
		}
	}
}

void BL_Material::UpdateIPO(const mt::vec4 &rgba,
                                   const mt::vec3 &specrgb,
                                   float hard,
                                   float spec,
                                   float ref,
                                   float emit,
                                   float ambient,
                                   float alpha,
                                   float specalpha)
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
	m_material->amb = (float)(ambient);
	m_material->har = (float)(hard);
	m_material->emit = (float)(emit);
	m_material->spec = (float)(spec);
	m_material->ref = (float)(ref);
	m_material->spectra = (float)specalpha;
}

std::string BL_Material::GetName()
{
	return m_name;
}

#ifdef USE_MATHUTILS

#define MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR 1
#define MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR 2

static unsigned char mathutils_kxblendermaterial_color_cb_index = -1; /* index for our callbacks */

static int mathutils_kxblendermaterial_generic_check(BaseMathObject *bmo)
{
	BL_Material *self = static_cast<BL_Material *>EXP_PROXY_REF(bmo->cb_user);
	if (!self) {
		return -1;
	}

	return 0;
}

static int mathutils_kxblendermaterial_color_get(BaseMathObject *bmo, int subtype)
{
	BL_Material *self = static_cast<BL_Material *>EXP_PROXY_REF(bmo->cb_user);
	if (!self) {
		return -1;
	}

	Material *mat = self->GetBlenderMaterial();

	switch (subtype) {
		case MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR:
		{
			bmo->data[0] = mat->r;
			bmo->data[1] = mat->g;
			bmo->data[2] = mat->b;
			break;
		}
		case MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR:
		{
			bmo->data[0] = mat->specr;
			bmo->data[1] = mat->specg;
			bmo->data[2] = mat->specb;
			break;
		}
	}

	return 0;
}

static int mathutils_kxblendermaterial_color_set(BaseMathObject *bmo, int subtype)
{
	BL_Material *self = static_cast<BL_Material *>EXP_PROXY_REF(bmo->cb_user);
	if (!self) {
		return -1;
	}

	Material *mat = self->GetBlenderMaterial();

	switch (subtype) {
		case MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR:
		{
			mat->r = bmo->data[0];
			mat->g = bmo->data[1];
			mat->b = bmo->data[2];
			break;
		}
		case MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR:
		{
			mat->specr = bmo->data[0];
			mat->specg = bmo->data[1];
			mat->specb = bmo->data[2];
			break;
		}
	}

	return 0;
}

static int mathutils_kxblendermaterial_color_get_index(BaseMathObject *bmo, int subtype, int index)
{
	/* lazy, avoid repeteing the case statement */
	if (mathutils_kxblendermaterial_color_get(bmo, subtype) == -1) {
		return -1;
	}
	return 0;
}

static int mathutils_kxblendermaterial_color_set_index(BaseMathObject *bmo, int subtype, int index)
{
	float f = bmo->data[index];

	/* lazy, avoid repeateing the case statement */
	if (mathutils_kxblendermaterial_color_get(bmo, subtype) == -1) {
		return -1;
	}

	bmo->data[index] = f;
	return mathutils_kxblendermaterial_color_set(bmo, subtype);
}

static Mathutils_Callback mathutils_kxblendermaterial_color_cb = {
	mathutils_kxblendermaterial_generic_check,
	mathutils_kxblendermaterial_color_get,
	mathutils_kxblendermaterial_color_set,
	mathutils_kxblendermaterial_color_get_index,
	mathutils_kxblendermaterial_color_set_index
};


void BL_Material_Mathutils_Callback_Init()
{
	// register mathutils callbacks, ok to run more than once.
	mathutils_kxblendermaterial_color_cb_index = Mathutils_RegisterCallback(&mathutils_kxblendermaterial_color_cb);
}

#endif // USE_MATHUTILS

#ifdef WITH_PYTHON

PyMethodDef BL_Material::Methods[] =
{
	EXP_PYMETHODTABLE(BL_Material, getShader),
	EXP_PYMETHODTABLE(BL_Material, getTextureBindcode),
	EXP_PYMETHODTABLE(BL_Material, setBlending),
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef BL_Material::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("shader", BL_Material, pyattr_get_shader),
	EXP_PYATTRIBUTE_RO_FUNCTION("textures", BL_Material, pyattr_get_textures),
	EXP_PYATTRIBUTE_RW_FUNCTION("blending", BL_Material, pyattr_get_blending, pyattr_set_blending),
	EXP_PYATTRIBUTE_RW_FUNCTION("alpha", BL_Material, pyattr_get_alpha, pyattr_set_alpha),
	EXP_PYATTRIBUTE_RW_FUNCTION("hardness", BL_Material, pyattr_get_hardness, pyattr_set_hardness),
	EXP_PYATTRIBUTE_RW_FUNCTION("specularIntensity", BL_Material, pyattr_get_specular_intensity, pyattr_set_specular_intensity),
	EXP_PYATTRIBUTE_RW_FUNCTION("specularColor", BL_Material, pyattr_get_specular_color, pyattr_set_specular_color),
	EXP_PYATTRIBUTE_RW_FUNCTION("diffuseIntensity", BL_Material, pyattr_get_diffuse_intensity, pyattr_set_diffuse_intensity),
	EXP_PYATTRIBUTE_RW_FUNCTION("diffuseColor", BL_Material, pyattr_get_diffuse_color, pyattr_set_diffuse_color),
	EXP_PYATTRIBUTE_RW_FUNCTION("emit", BL_Material, pyattr_get_emit, pyattr_set_emit),
	EXP_PYATTRIBUTE_RW_FUNCTION("ambient", BL_Material, pyattr_get_ambient, pyattr_set_ambient),
	EXP_PYATTRIBUTE_RW_FUNCTION("specularAlpha", BL_Material, pyattr_get_specular_alpha, pyattr_set_specular_alpha),

	EXP_PYATTRIBUTE_NULL //Sentinel
};

PyTypeObject BL_Material::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"BL_Material",
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
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyObject *BL_Material::pyattr_get_shader(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	return self->PygetShader(nullptr, nullptr);
}

unsigned int BL_Material::py_get_textures_size()
{
	return RAS_Texture::MaxUnits;
}

PyObject *BL_Material::py_get_textures_item(unsigned int index)
{
	BL_Texture *tex = static_cast<BL_Texture *>(m_textures[index]);
	PyObject *item = nullptr;
	if (tex) {
		item = tex->GetProxy();
	}
	else {
		Py_RETURN_NONE;
	}
	return item;
}

std::string BL_Material::py_get_textures_item_name(unsigned int index)
{
	BL_Texture *tex = static_cast<BL_Texture *>(m_textures[index]);
	return (tex ? tex->GetName() : "");
}

PyObject *BL_Material::pyattr_get_textures(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<BL_Material, &BL_Material::py_get_textures_size, &BL_Material::py_get_textures_item,
				nullptr, &BL_Material::py_get_textures_item_name>(self_v))->NewProxy(true);
}

PyObject *BL_Material::pyattr_get_blending(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	const RAS_Rasterizer::BlendFunc *bfunc = self->GetBlendFunc();
	return Py_BuildValue("(ll)", (long int)bfunc[0], (long int)bfunc[1]);
}

PyObject *BL_Material::pyattr_get_alpha(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->alpha);
}

int BL_Material::pyattr_set_alpha(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: BL_Material, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->alpha = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Material::pyattr_get_specular_alpha(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->spectra);
}

int BL_Material::pyattr_set_specular_alpha(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: BL_Material, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->spectra = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Material::pyattr_get_hardness(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	return PyLong_FromLong(self->GetBlenderMaterial()->har);
}

int BL_Material::pyattr_set_hardness(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	int val = PyLong_AsLong(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = int: BL_Material, expected a int", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 1, 511);

	self->GetBlenderMaterial()->har = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Material::pyattr_get_specular_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->spec);
}

int BL_Material::pyattr_set_specular_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: BL_Material, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->spec = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Material::pyattr_get_specular_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(EXP_PROXY_FROM_REF(self_v), mathutils_kxblendermaterial_color_cb_index, MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR);
#else
	BL_Material *self = static_cast<BL_Material *>(self_v);
	Material *mat = self->GetBlenderMaterial();
	return PyColorFromVector(mt::vec3(mat->specr, mat->specg, mat->specb));
#endif
}

int BL_Material::pyattr_set_specular_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	mt::vec3 color;
	if (!PyVecTo(value, color)) {
		return PY_SET_ATTR_FAIL;
	}

	Material *mat = self->GetBlenderMaterial();
	mat->specr = color[0];
	mat->specg = color[1];
	mat->specb = color[2];
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Material::pyattr_get_diffuse_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->ref);
}

int BL_Material::pyattr_set_diffuse_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: BL_Material, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->ref = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Material::pyattr_get_diffuse_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(EXP_PROXY_FROM_REF(self_v), mathutils_kxblendermaterial_color_cb_index, MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR);
#else
	BL_Material *self = static_cast<BL_Material *>(self_v);
	Material *mat = self->GetBlenderMaterial();
	return PyColorFromVector(mt::vec3(mat->r, mat->g, mat->b));
#endif
}

int BL_Material::pyattr_set_diffuse_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	mt::vec3 color;
	if (!PyVecTo(value, color)) {
		return PY_SET_ATTR_FAIL;
	}

	Material *mat = self->GetBlenderMaterial();
	mat->r = color[0];
	mat->g = color[1];
	mat->b = color[2];
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Material::pyattr_get_emit(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->emit);
}

int BL_Material::pyattr_set_emit(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: BL_Material, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 2.0f);

	self->GetBlenderMaterial()->emit = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *BL_Material::pyattr_get_ambient(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->amb);
}

int BL_Material::pyattr_set_ambient(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: BL_Material, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->amb = val;
	return PY_SET_ATTR_SUCCESS;
}

int BL_Material::pyattr_set_blending(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Material *self = static_cast<BL_Material *>(self_v);
	PyObject *obj = self->PysetBlending(value, nullptr);
	if (obj) {
		Py_DECREF(obj);
		return 0;
	}
	return -1;
}

EXP_PYMETHODDEF_DOC(BL_Material, getShader, "getShader()")
{
	// returns Py_None on error
	// the calling script will need to check

	if (!m_customShader) {
		m_customShader.reset(new KX_MaterialShader(this, m_flag & RAS_MULTILIGHT, m_alphaBlend));
	}

	if (!m_customShader->GetError()) {
		return m_customShader->GetProxy();
	}
	// We have a shader but invalid.
	else {
		m_customShader.reset(nullptr);
	}
	Py_RETURN_NONE;
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

EXP_PYMETHODDEF_DOC(BL_Material, setBlending, "setBlending(bge.logic.src, bge.logic.dest)")
{
	unsigned int b[2];
	if (PyArg_ParseTuple(args, "ii:setBlending", &b[0], &b[1])) {
		bool value_found[2] = {false, false};
		for (int i = 0; i < 11; i++) {
			if (b[0] == GL_array[i]) {
				value_found[0] = true;
				m_blendFunc[0] = (RAS_Rasterizer::BlendFunc)b[0];
			}
			if (b[1] == GL_array[i]) {
				value_found[1] = true;
				m_blendFunc[1] = (RAS_Rasterizer::BlendFunc)b[1];
			}
			if (value_found[0] && value_found[1]) {
				break;
			}
		}
		if (!value_found[0] || !value_found[1]) {
			PyErr_SetString(PyExc_ValueError, "material.setBlending(int, int): BL_Material, invalid enum.");
			return nullptr;
		}
		m_userDefBlend = true;
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Material, getTextureBindcode, "getTextureBindcode(texslot)")
{
	EXP_ShowDeprecationWarning("material.getTextureBindcode(texslot)", "material.textures[texslot].bindCode");
	unsigned int texslot;
	if (!PyArg_ParseTuple(args, "i:texslot", &texslot)) {
		PyErr_SetString(PyExc_ValueError, "material.getTextureBindcode(texslot): BL_Material, expected an int.");
		return nullptr;
	}
	Image *ima = GetTexture(texslot)->GetImage();
	if (ima) {
		unsigned int *bindcode = ima->bindcode;
		return PyLong_FromLong(*bindcode);
	}
	PyErr_SetString(PyExc_ValueError, "material.getTextureBindcode(texslot): BL_Material, invalid texture slot.");
	return nullptr;
}

bool ConvertPythonToMaterial(PyObject *value, BL_Material **material, bool py_none_ok, const char *error_prefix)
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
			PyErr_Format(PyExc_TypeError, "%s, expected BL_Material or a BL_Material name, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &BL_Material::Type)) {
		BL_Material *mat = static_cast<BL_Material *>EXP_PROXY_REF(value);

		/* sets the error */
		if (mat == nullptr) {
			PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		*material = mat;
		return true;
	}

	*material = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a BL_Material, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a BL_Material or a string", error_prefix);
	}

	return false;
}

#endif // WITH_PYTHON
