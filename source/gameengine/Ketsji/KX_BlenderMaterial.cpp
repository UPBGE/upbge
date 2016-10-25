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

#include "BL_Shader.h"
#include "BL_BlenderShader.h"

#include "EXP_ListWrapper.h"

#include "MT_Matrix4x4.h"

#include "RAS_BucketManager.h"
#include "RAS_IRasterizer.h"
#include "RAS_MeshUser.h"

#include "GPU_draw.h"
#include "GPU_material.h" // for GPU_BLEND_SOLID

#include "DNA_texture_types.h"
#include "DNA_material_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_scene_types.h"

KX_BlenderMaterial::KX_BlenderMaterial(
		KX_Scene *scene,
		Material *mat,
		GameSettings *game,
		MTFace *mtface,
		int lightlayer)
	:RAS_IPolyMaterial(mat->id.name, game),
	m_material(mat),
	m_shader(NULL),
	m_blenderShader(NULL),
	m_scene(scene),
	m_userDefBlend(false),
	m_constructed(false),
	m_lightLayer(lightlayer)
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

	m_alphablend = mat->game.alpha_blend;

	m_mtexPoly = new MTexPoly();
	memset(m_mtexPoly, 0, sizeof(MTexPoly));

	if (mtface) {
		ME_MTEXFACE_CPY(m_mtexPoly, mtface);
	}

	short storage = game->storage;
	// If the material storage is set to RAS_STORE_SCENE then we use the storage set in scene.
	if (storage == GAME_STORAGE_SCENE) {
		Scene *blenderScene = scene->GetBlenderScene();
		storage = blenderScene->gm.raster_storage;
		m_flag |= (blenderScene->gm.flag & GAME_DISPLAY_LISTS) ? RAS_DISPLAYLISTS : 0;
	}
	else {
		m_flag |= (game->storage_flag & GEMAT_DISPLAY_LISTS) ? RAS_DISPLAYLISTS : 0;
	}

	switch (storage) {
		case GAME_STORAGE_VA:
		{
			m_storageType = RAS_IRasterizer::RAS_STORAGE_VA;
			break;
		}
		case GAME_STORAGE_VBO:
		{
			m_storageType = RAS_IRasterizer::RAS_STORAGE_VBO;
			break;
		}
	};

	// with ztransp enabled, enforce alpha blending mode
	if ((mat->mode & MA_TRANSP) && (mat->mode & MA_ZTRANSP) && (m_alphablend == GEMAT_SOLID)) {
		m_alphablend = GEMAT_ALPHA;
	}

	m_rasMode |= (mat->game.flag & GEMAT_BACKCULL) ? 0 : RAS_TWOSIDED;
	m_rasMode |= (mat->material_type == MA_TYPE_WIRE) ? RAS_WIRE : 0;
	m_rasMode |= (mat->mode2 & MA_DEPTH_TRANSP) ? RAS_DEPTH_ALPHA : 0;

	// always zsort alpha + add
	if (ELEM(m_alphablend, GEMAT_ALPHA, GEMAT_ALPHA_SORT, GEMAT_ADD) && (m_alphablend != GEMAT_CLIP)) {
		m_rasMode |= RAS_ALPHA;
		m_rasMode |= (mat && (mat->game.alpha_blend & GEMAT_ALPHA_SORT)) ? RAS_ZSORT : 0;
	}

	// RAS_IPolyMaterial variables...
	m_flag |= ((mat->mode & MA_SHLESS) != 0) ? 0 : RAS_MULTILIGHT;
	m_flag |= RAS_BLENDERGLSL;
	m_flag |= ((mat->mode2 & MA_CASTSHADOW) != 0) ? RAS_CASTSHADOW : 0;
	m_flag |= ((mat->mode & MA_ONLYCAST) != 0) ? RAS_ONLYSHADOW : 0;
	m_flag |= ((m_material->shade_flag & MA_OBCOLOR) != 0) ? RAS_OBJECTCOLOR : 0;
}

KX_BlenderMaterial::~KX_BlenderMaterial()
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

	delete m_mtexPoly;

	// cleanup work
	if (m_constructed) {
		// clean only if material was actually used
		OnExit();
	}
}

MTexPoly *KX_BlenderMaterial::GetMTexPoly() const
{
	// fonts on polys
	return m_mtexPoly;
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

const STR_String& KX_BlenderMaterial::GetTextureName() const
{
	static const STR_String empty = ""; // hack to return a reference.
	return (m_textures[0] ? m_textures[0]->GetName() : empty);
}

Material *KX_BlenderMaterial::GetBlenderMaterial() const
{
	return m_material;
}

Image *KX_BlenderMaterial::GetBlenderImage() const
{
	return (m_mtexPoly ? m_mtexPoly->tpage : NULL);
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
	if (m_blenderShader)
		m_blenderShader->ReloadMaterial();
}

void KX_BlenderMaterial::InitTextures()
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

void KX_BlenderMaterial::OnConstruction()
{
	if (m_constructed) {
		// when material are reused between objects
		return;
	}

	SetBlenderGLSLShader();

	InitTextures();

	m_blendFunc[0] = 0;
	m_blendFunc[1] = 0;
	m_constructed = true;
}

void KX_BlenderMaterial::EndFrame(RAS_IRasterizer *rasty)
{
	rasty->SetAlphaBlend(GPU_BLEND_SOLID);
	RAS_Texture::DesactiveTextures();
}

void KX_BlenderMaterial::OnExit()
{
	if (m_shader) {
		delete m_shader;
		m_shader = NULL;
	}
	if (m_blenderShader) {
		delete m_blenderShader;
		m_blenderShader = NULL;
	}

	/* used to call with 'm_material->tface' but this can be a freed array,
	 * see: [#30493], so just call with NULL, this is best since it clears
	 * the 'lastface' pointer in GPU too - campbell */
	GPU_set_tpage(NULL, 1, m_alphablend);
}


void KX_BlenderMaterial::SetShaderData(RAS_IRasterizer *ras)
{
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
		ras->Enable(RAS_IRasterizer::RAS_BLEND);
		ras->SetBlendFunc((RAS_IRasterizer::BlendFunc)m_blendFunc[0], (RAS_IRasterizer::BlendFunc)m_blendFunc[1]);
	}
}

void KX_BlenderMaterial::SetBlenderShaderData(RAS_IRasterizer *ras)
{
	// Don't set the alpha blend here because ActivateMeshSlot do it.
	m_blenderShader->SetProg(true, ras->GetTime(), ras);
}

void KX_BlenderMaterial::ActivateShaders(RAS_IRasterizer *rasty)
{
	SetShaderData(rasty);

	if (IsWire()) {
		rasty->SetCullFace(false);
	}
	else if (IsCullFace()) {
		rasty->SetCullFace(true);
	}
	else {
		rasty->SetCullFace(false);
	}

	ActivateGLMaterials(rasty);
	ActivateTexGen(rasty);
}

void KX_BlenderMaterial::ActivateBlenderShaders(RAS_IRasterizer *rasty)
{
	SetBlenderShaderData(rasty);

	if (IsWire()) {
		rasty->SetCullFace(false);
	}
	else if (IsCullFace()) {
		rasty->SetCullFace(true);
	}
	else {
		rasty->SetCullFace(false);
	}

	ActivateGLMaterials(rasty);
	m_blenderShader->SetAttribs(rasty);
}

void KX_BlenderMaterial::Activate(RAS_IRasterizer *rasty)
{
	if (m_shader && m_shader->Ok()) {
		ActivateShaders(rasty);
	}
	else if (m_blenderShader && m_blenderShader->Ok()) {
		ActivateBlenderShaders(rasty);
	}
}

void KX_BlenderMaterial::Desactivate(RAS_IRasterizer *rasty)
{
	if (m_shader && m_shader->Ok()) {
		m_shader->SetProg(false);
		for (unsigned short i = 0; i < RAS_Texture::MaxUnits; i++) {
			if (m_textures[i] && m_textures[i]->Ok()) {
				m_textures[i]->DisableTexture();
			}
		}
	}
	else if (m_blenderShader && m_blenderShader->Ok()) {
		m_blenderShader->SetProg(false);
	}
	// Make sure no one will use the attributs set by this material.
	rasty->ClearTexCoords();
	rasty->ClearAttribs();
}

bool KX_BlenderMaterial::UseInstancing() const
{
	if (m_shader && m_shader->Ok()) {
		return false;
	}
	else if (m_blenderShader) {
		return m_blenderShader->UseInstancing();
	}
	// The material is in conversion, we use the blender material flag then.
	return m_material->shade_flag & MA_INSTANCING;
}

void KX_BlenderMaterial::ActivateInstancing(RAS_IRasterizer *rasty, void *matrixoffset, void *positionoffset, void *coloroffset, unsigned int stride)
{
	if (m_blenderShader) {
		m_blenderShader->ActivateInstancing(matrixoffset, positionoffset, coloroffset, stride);
	}

	/* Because the geometry instancing use setting for all instances we use the original alpha blend.
	 * This requierd that the user use "alpha blend" mode if he will use mutate object color alpha.
	 */
	rasty->SetAlphaBlend(m_alphablend);
}

void KX_BlenderMaterial::DesactivateInstancing()
{
	if (m_blenderShader) {
		m_blenderShader->DesactivateInstancing();
	}
}

bool KX_BlenderMaterial::UsesLighting(RAS_IRasterizer *rasty) const
{
	if (!RAS_IPolyMaterial::UsesLighting(rasty))
		return false;

	if (m_shader && m_shader->Ok())
		return true;
	else if (m_blenderShader && m_blenderShader->Ok())
		return false;
	else
		return true;
}

void KX_BlenderMaterial::ActivateMeshSlot(RAS_MeshSlot *ms, RAS_IRasterizer *rasty)
{
	if (m_shader && m_shader->Ok()) {
		m_shader->Update(rasty, MT_Matrix4x4(ms->m_meshUser->GetMatrix()));
	}
	else if (m_blenderShader) {
		m_blenderShader->Update(ms, rasty);

		/* we do blend modes here, because they can change per object
		 * with the same material due to obcolor/obalpha */
		int alphablend = m_blenderShader->GetAlphaBlend();
		if (ELEM(alphablend, GEMAT_SOLID, GEMAT_ALPHA, GEMAT_ALPHA_SORT) && m_alphablend != GEMAT_SOLID)
			alphablend = m_alphablend;

		rasty->SetAlphaBlend(alphablend);
	}
}

void KX_BlenderMaterial::ActivateGLMaterials(RAS_IRasterizer *rasty) const
{
	if (m_shader || !m_blenderShader) {
		rasty->SetSpecularity(m_material->specr * m_material->spec, m_material->specg * m_material->spec,
							  m_material->specb * m_material->spec, m_material->spec);
		rasty->SetShinyness(((float)m_material->har) / 4.0f);
		rasty->SetDiffuse(m_material->r * m_material->ref + m_material->emit, m_material->g * m_material->ref + m_material->emit,
						  m_material->b * m_material->ref + m_material->emit, 1.0f);
		rasty->SetEmissive(m_material->r * m_material->emit, m_material->g * m_material->emit,
						   m_material->b * m_material->emit, 1.0f);
		rasty->SetAmbient(m_material->amb);
	}

	rasty->SetPolygonOffset(-m_material->zoffs, 0.0f);
}


void KX_BlenderMaterial::ActivateTexGen(RAS_IRasterizer *ras) const
{
	if (m_shader->GetAttribute() == BL_Shader::SHD_TANGENT) {
		RAS_IRasterizer::TexCoGenList attribs(2);
		attribs[0] = RAS_IRasterizer::RAS_TEXCO_DISABLE;
		attribs[1] = RAS_IRasterizer::RAS_TEXTANGENT;

		ras->SetAttribs(attribs);
	}

	RAS_IRasterizer::TexCoGenList texcos(RAS_Texture::MaxUnits);
	for (int i = 0; i < RAS_Texture::MaxUnits; i++) {
		RAS_Texture *texture = m_textures[i];
		/* Here textures can return false to Ok() because we're looking only at
		 * texture attributs and not texture bind id like for the binding and
		 * unbinding of textures. A NULL BL_Texture means that the cooresponding
		 * mtex is NULL too (see InitTextures).*/
		if (texture) {
			MTex *mtex = texture->GetMTex();
			if (mtex->texco & (TEXCO_OBJECT | TEXCO_REFL)) {
				texcos[i] = RAS_IRasterizer::RAS_TEXCO_GEN;
			}
			else if (mtex->texco & (TEXCO_ORCO | TEXCO_GLOB)) {
				texcos[i] = RAS_IRasterizer::RAS_TEXCO_ORCO;
			}
			else if (mtex->texco & TEXCO_UV) {
				texcos[i] = RAS_IRasterizer::RAS_TEXCO_UV;
			}
			else if (mtex->texco & TEXCO_NORM) {
				texcos[i] = RAS_IRasterizer::RAS_TEXCO_NORM;
			}
			else if (mtex->texco & TEXCO_TANGENT) {
				texcos[i] = RAS_IRasterizer::RAS_TEXTANGENT;
			}
		}
		else {
			texcos[i] = RAS_IRasterizer::RAS_TEXCO_DISABLE;
		}
	}
	ras->SetTexCoords(texcos);
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
	m_material->amb = (float)(ambient);
	m_material->har = (float)(hard);
	m_material->emit = (float)(emit);
	m_material->spec = (float)(spec);
	m_material->ref = (float)(ref);
	m_material->spectra = (float)specalpha;
}

const RAS_IRasterizer::AttribLayerList KX_BlenderMaterial::GetAttribLayers(const RAS_MeshObject::LayerList& layers) const
{
	if (m_blenderShader && m_blenderShader->Ok()) {
		return m_blenderShader->GetAttribLayers(layers);
	}

	static const RAS_IRasterizer::AttribLayerList attribLayers;
	return attribLayers;
}

void KX_BlenderMaterial::Replace_IScene(SCA_IScene *val)
{
	m_scene = static_cast<KX_Scene *>(val);

	OnConstruction();
}

void KX_BlenderMaterial::SetBlenderGLSLShader()
{
	if (!m_blenderShader)
		m_blenderShader = new BL_BlenderShader(m_scene, m_material, m_lightLayer);

	if (!m_blenderShader->Ok()) {
		delete m_blenderShader;
		m_blenderShader = NULL;
	}
}

STR_String& KX_BlenderMaterial::GetName()
{
	return m_name;
}

#ifdef USE_MATHUTILS

#define MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR 1
#define MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR 2

static unsigned char mathutils_kxblendermaterial_color_cb_index = -1; /* index for our callbacks */

static int mathutils_kxblendermaterial_generic_check(BaseMathObject *bmo)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>BGE_PROXY_REF(bmo->cb_user);
	if (!self)
		return -1;

	return 0;
}

static int mathutils_kxblendermaterial_color_get(BaseMathObject *bmo, int subtype)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>BGE_PROXY_REF(bmo->cb_user);
	if (!self)
		return -1;

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
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>BGE_PROXY_REF(bmo->cb_user);
	if (!self)
		return -1;

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
	if (mathutils_kxblendermaterial_color_get(bmo, subtype) == -1)
		return -1;
	return 0;
}

static int mathutils_kxblendermaterial_color_set_index(BaseMathObject *bmo, int subtype, int index)
{
	float f = bmo->data[index];

	/* lazy, avoid repeateing the case statement */
	if (mathutils_kxblendermaterial_color_get(bmo, subtype) == -1)
		return -1;

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


void KX_BlenderMaterial_Mathutils_Callback_Init()
{
	// register mathutils callbacks, ok to run more than once.
	mathutils_kxblendermaterial_color_cb_index = Mathutils_RegisterCallback(&mathutils_kxblendermaterial_color_cb);
}

#endif // USE_MATHUTILS

#ifdef WITH_PYTHON

PyMethodDef KX_BlenderMaterial::Methods[] =
{
	KX_PYMETHODTABLE(KX_BlenderMaterial, getShader),
	KX_PYMETHODTABLE( KX_BlenderMaterial, getTextureBindcode),
	KX_PYMETHODTABLE(KX_BlenderMaterial, setBlending),
	{NULL, NULL} //Sentinel
};

PyAttributeDef KX_BlenderMaterial::Attributes[] = {
	KX_PYATTRIBUTE_RO_FUNCTION("shader", KX_BlenderMaterial, pyattr_get_shader),
	KX_PYATTRIBUTE_RO_FUNCTION("textures", KX_BlenderMaterial, pyattr_get_textures),
	KX_PYATTRIBUTE_RW_FUNCTION("blending", KX_BlenderMaterial, pyattr_get_blending, pyattr_set_blending),
	KX_PYATTRIBUTE_RW_FUNCTION("alpha", KX_BlenderMaterial, pyattr_get_alpha, pyattr_set_alpha),
	KX_PYATTRIBUTE_RW_FUNCTION("hardness", KX_BlenderMaterial, pyattr_get_hardness, pyattr_set_hardness),
	KX_PYATTRIBUTE_RW_FUNCTION("specularIntensity", KX_BlenderMaterial, pyattr_get_specular_intensity, pyattr_set_specular_intensity),
	KX_PYATTRIBUTE_RW_FUNCTION("specularColor", KX_BlenderMaterial, pyattr_get_specular_color, pyattr_set_specular_color),
	KX_PYATTRIBUTE_RW_FUNCTION("diffuseIntensity", KX_BlenderMaterial, pyattr_get_diffuse_intensity, pyattr_set_diffuse_intensity),
	KX_PYATTRIBUTE_RW_FUNCTION("diffuseColor", KX_BlenderMaterial, pyattr_get_diffuse_color, pyattr_set_diffuse_color),
	KX_PYATTRIBUTE_RW_FUNCTION("emit", KX_BlenderMaterial, pyattr_get_emit, pyattr_set_emit),
	KX_PYATTRIBUTE_RW_FUNCTION("ambient", KX_BlenderMaterial, pyattr_get_ambient, pyattr_set_ambient),
	KX_PYATTRIBUTE_RW_FUNCTION("specularAlpha", KX_BlenderMaterial, pyattr_get_specular_alpha, pyattr_set_specular_alpha),

	{NULL} //Sentinel
};

PyTypeObject KX_BlenderMaterial::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
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

PyObject *KX_BlenderMaterial::pyattr_get_shader(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return self->PygetShader(NULL, NULL);
}

static int kx_blender_material_get_textures_size_cb(void *self_v)
{
	return RAS_Texture::MaxUnits;
}

static PyObject *kx_blender_material_get_textures_item_cb(void *self_v, int index)
{
	BL_Texture *tex = (BL_Texture *)((KX_BlenderMaterial *)self_v)->GetTexture(index);
	PyObject *item = NULL;
	if (tex) {
		item = tex->GetProxy();
	}
	else {
		item = Py_None;
		Py_INCREF(Py_None);
	}
	return item;
}

static const char *kx_blender_material_get_textures_item_name_cb(void *self_v, int index)
{
	BL_Texture *tex = (BL_Texture *)((KX_BlenderMaterial *)self_v)->GetTexture(index);
	return (tex ? tex->GetName().ReadPtr() : "");
}

PyObject *KX_BlenderMaterial::pyattr_get_textures(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	return (new CListWrapper(self_v,
							 ((KX_BlenderMaterial *)self_v)->GetProxy(),
							 NULL,
							 kx_blender_material_get_textures_size_cb,
							 kx_blender_material_get_textures_item_cb,
							 kx_blender_material_get_textures_item_name_cb,
							 NULL))->NewProxy(true);
}

PyObject *KX_BlenderMaterial::pyattr_get_blending(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	unsigned int *bfunc = self->GetBlendFunc();
	return Py_BuildValue("(ll)", (long int)bfunc[0], (long int)bfunc[1]);
}

PyObject *KX_BlenderMaterial::pyattr_get_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->alpha);
}

int KX_BlenderMaterial::pyattr_set_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->alpha = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_specular_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->spectra);
}

int KX_BlenderMaterial::pyattr_set_specular_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->spectra = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_hardness(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyLong_FromLong(self->GetBlenderMaterial()->har);
}

int KX_BlenderMaterial::pyattr_set_hardness(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	int val = PyLong_AsLong(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = int: KX_BlenderMaterial, expected a int", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 1, 511);

	self->GetBlenderMaterial()->har = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_specular_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->spec);
}

int KX_BlenderMaterial::pyattr_set_specular_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->spec = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_specular_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(BGE_PROXY_FROM_REF(self_v), mathutils_kxblendermaterial_color_cb_index, MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR);
#else
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	Material *mat = self->GetBlenderMaterial();
	return PyColorFromVector(MT_Vector3(mat->specr, mat->specg, mat->specb);
#endif
}

int KX_BlenderMaterial::pyattr_set_specular_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	MT_Vector3 color;
	if (!PyVecTo(value, color))
		return PY_SET_ATTR_FAIL;

	Material *mat = self->GetBlenderMaterial();
	mat->specr = color[0];
	mat->specg = color[1];
	mat->specb = color[2];
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_diffuse_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->ref);
}

int KX_BlenderMaterial::pyattr_set_diffuse_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->ref = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_diffuse_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(BGE_PROXY_FROM_REF(self_v), mathutils_kxblendermaterial_color_cb_index, MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR);
#else
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	Material *mat = self->GetBlenderMaterial();
	return PyColorFromVector(MT_Vector3(mat->r, mat->g, mat->b));
#endif
}

int KX_BlenderMaterial::pyattr_set_diffuse_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	MT_Vector3 color;
	if (!PyVecTo(value, color))
		return PY_SET_ATTR_FAIL;

	Material *mat = self->GetBlenderMaterial();
	mat->r = color[0];
	mat->g = color[1];
	mat->b = color[2];
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_emit(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->emit);
}

int KX_BlenderMaterial::pyattr_set_emit(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 2.0f);

	self->GetBlenderMaterial()->emit = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_ambient(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->amb);
}

int KX_BlenderMaterial::pyattr_set_ambient(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name);
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->amb = val;
	return PY_SET_ATTR_SUCCESS;
}

int KX_BlenderMaterial::pyattr_set_blending(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	PyObject *obj = self->PysetBlending(value, NULL);
	if (obj)
	{
		Py_DECREF(obj);
		return 0;
	}
	return -1;
}

KX_PYMETHODDEF_DOC(KX_BlenderMaterial, getShader, "getShader()")
{
	// returns Py_None on error
	// the calling script will need to check

	if (!m_shader) {
		m_shader = new BL_Shader();
		if (!m_shader->GetError()) {
			// Set the material to use custom shader.
			m_flag &= ~RAS_BLENDERGLSL;
			m_scene->GetBucketManager()->ReleaseDisplayLists(this);
		}
	}

	if (!m_shader->GetError()) {
		return m_shader->GetProxy();
	}
	// We have a shader but invalid.
	else {
		// decref all references to the object
		// then delete it!
		// We will then go back to fixed functionality
		// for this material
		delete m_shader; /* will handle python de-referencing */
		m_shader = NULL;
	}
	Py_RETURN_NONE;
}

static const unsigned int GL_array[11] = {
	RAS_IRasterizer::RAS_ZERO,
	RAS_IRasterizer::RAS_ONE,
	RAS_IRasterizer::RAS_SRC_COLOR,
	RAS_IRasterizer::RAS_ONE_MINUS_SRC_COLOR,
	RAS_IRasterizer::RAS_DST_COLOR,
	RAS_IRasterizer::RAS_ONE_MINUS_DST_COLOR,
	RAS_IRasterizer::RAS_SRC_ALPHA,
	RAS_IRasterizer::RAS_ONE_MINUS_SRC_ALPHA,
	RAS_IRasterizer::RAS_DST_ALPHA,
	RAS_IRasterizer::RAS_ONE_MINUS_DST_ALPHA,
	RAS_IRasterizer::RAS_SRC_ALPHA_SATURATE
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
			return NULL;
		}
		m_userDefBlend = true;
		Py_RETURN_NONE;
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_BlenderMaterial, getTextureBindcode, "getTextureBindcode(texslot)")
{
	ShowDeprecationWarning("material.getTextureBindcode(texslot)", "material.textures[texslot].bindCode");
	unsigned int texslot;
	if (!PyArg_ParseTuple(args, "i:texslot", &texslot)) {
		PyErr_SetString(PyExc_ValueError, "material.getTextureBindcode(texslot): KX_BlenderMaterial, expected an int.");
		return NULL;
	}
	Image *ima = GetTexture(texslot)->GetImage();
	if (ima) {
		unsigned int *bindcode = ima->bindcode;
		return PyLong_FromLong(*bindcode);
	}
	PyErr_SetString(PyExc_ValueError, "material.getTextureBindcode(texslot): KX_BlenderMaterial, invalid texture slot.");
	return NULL;
}

bool ConvertPythonToMaterial(PyObject *value, KX_BlenderMaterial **material, bool py_none_ok, const char *error_prefix)
{
	if (value == NULL) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer NULL, should never happen", error_prefix);
		*material = NULL;
		return false;
	}

	if (value == Py_None) {
		*material = NULL;

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
		if (mat == NULL) {
			PyErr_Format(PyExc_SystemError, "%s, " BGE_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		*material = mat;
		return true;
	}

	*material = NULL;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_BlenderMaterial, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_BlenderMaterial or a string", error_prefix);
	}

	return false;
}

#endif // WITH_PYTHON
