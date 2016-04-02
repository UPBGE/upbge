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

#include "glew-mx.h"

#include "KX_BlenderMaterial.h"
#include "BL_Material.h"
#include "BL_Shader.h"
#include "BL_BlenderShader.h"
#include "KX_Scene.h"
#include "KX_Light.h"
#include "KX_GameObject.h"
#include "KX_MeshProxy.h"
#include "KX_PyMath.h"

#include "EXP_ListWrapper.h"

#include "MT_Vector3.h"
#include "MT_Vector4.h"
#include "MT_Matrix4x4.h"

#include "RAS_BucketManager.h"
#include "RAS_MeshObject.h"
#include "RAS_IRasterizer.h"

#include "GPU_draw.h"
#include "GPU_material.h" // for GPU_BLEND_SOLID

#include "STR_HashedString.h"

#include "DNA_object_types.h"
#include "DNA_material_types.h"
#include "DNA_image_types.h"
#include "DNA_meshdata_types.h"
#include "BKE_mesh.h"
#include "BLI_utildefines.h"
#include "BLI_math.h"

#define spit(x) std::cout << x << std::endl;

KX_BlenderMaterial::KX_BlenderMaterial()
	: PyObjectPlus(),
	RAS_IPolyMaterial(),
	m_material(NULL),
	m_shader(NULL),
	m_blenderShader(NULL),
	m_scene(NULL),
	m_userDefBlend(false),
	m_modified(false),
	m_constructed(false)
{
	for (unsigned short i = 0; i < MAXTEX; ++i) {
		m_textures[i] = NULL;
	}
}

void KX_BlenderMaterial::Initialize(
    KX_Scene *scene,
    BL_Material *data,
    GameSettings *game,
    int lightlayer)
{
	RAS_IPolyMaterial::Initialize(
	    data->texname[0],
	    data->matname,
	    data->materialindex,
	    data->tile,
	    data->tilexrep[0],
	    data->tileyrep[0],
	    data->alphablend,
	    ((data->ras_mode & ALPHA) != 0),
	    ((data->ras_mode & ZSORT) != 0),
	    ((data->ras_mode & USE_LIGHT) != 0),
	    ((data->ras_mode & TEX)),
	    game
	    );
	Material *ma = data->material;

	// Save material data to restore on exit
	m_savedData.r = ma->r;
	m_savedData.g = ma->g;
	m_savedData.b = ma->b;
	m_savedData.a = ma->alpha;
	m_savedData.specr = ma->specr;
	m_savedData.specg = ma->specg;
	m_savedData.specb = ma->specb;
	m_savedData.spec = ma->spec;
	m_savedData.ref = ma->ref;
	m_savedData.hardness = ma->har;
	m_savedData.emit = ma->emit;

	m_material = data;
	m_shader = NULL;
	m_blenderShader = NULL;
	m_scene = scene;
	m_userDefBlend = false;
	m_modified = false;
	m_constructed = false;
	m_lightLayer = lightlayer;
	// --------------------------------
	// RAS_IPolyMaterial variables...
	m_flag |= ((m_material->ras_mode & USE_LIGHT) != 0) ? RAS_MULTILIGHT : 0;
	m_flag |= RAS_BLENDERGLSL;
	m_flag |= ((m_material->ras_mode & CAST_SHADOW) != 0) ? RAS_CASTSHADOW : 0;
	m_flag |= ((m_material->ras_mode & ONLY_SHADOW) != 0) ? RAS_ONLYSHADOW : 0;
	m_flag |= ((ma->shade_flag & MA_OBCOLOR) != 0) ? RAS_OBJECTCOLOR : 0;
}

KX_BlenderMaterial::~KX_BlenderMaterial()
{
	Material *ma = m_material->material;
	// Restore Blender material data
	ma->r = m_savedData.r;
	ma->g = m_savedData.g;
	ma->b = m_savedData.b;
	ma->alpha = m_savedData.a;
	ma->specr = m_savedData.specr;
	ma->specg = m_savedData.specg;
	ma->specb = m_savedData.specb;
	ma->spec = m_savedData.spec;
	ma->ref = m_savedData.ref;
	ma->har = m_savedData.hardness;
	ma->emit = m_savedData.emit;

	for (unsigned short i = 0; i < MAXTEX; ++i) {
		if (m_textures[i]) {
			delete m_textures[i];
		}
	}

	// cleanup work
	if (m_constructed) {
		// clean only if material was actually used
		OnExit();
	}
}

MTexPoly *KX_BlenderMaterial::GetMTexPoly() const
{
	// fonts on polys
	return &m_material->mtexpoly;
}

unsigned int *KX_BlenderMaterial::GetMCol() const
{
	// fonts on polys
	return m_material->rgb;
}

void KX_BlenderMaterial::GetMaterialRGBAColor(unsigned char *rgba) const
{
	if (m_material) {
		*rgba++ = (unsigned char)(m_material->matcolor[0] * 255.0f);
		*rgba++ = (unsigned char)(m_material->matcolor[1] * 255.0f);
		*rgba++ = (unsigned char)(m_material->matcolor[2] * 255.0f);
		*rgba++ = (unsigned char)(m_material->matcolor[3] * 255.0f);
	}
	else
		RAS_IPolyMaterial::GetMaterialRGBAColor(rgba);
}

Material *KX_BlenderMaterial::GetBlenderMaterial() const
{
	return m_material->material;
}

Image *KX_BlenderMaterial::GetBlenderImage() const
{
	return m_material->mtexpoly.tpage;
}

Scene *KX_BlenderMaterial::GetBlenderScene() const
{
	return m_scene->GetBlenderScene();
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
	for (i = 0; i < BL_Texture::GetMaxUnits(); i++) {
		Material *material = m_material->material;
		MTex *mtex = material->mtex[i];
		if (mtex) {
			bool mipmap = (m_material->flag[i] & MIPMAP) != 0;
			bool cubemap = (mtex->tex->type == TEX_ENVMAP && mtex->tex->env->stype == ENV_LOAD);
			BL_Texture *texture = new BL_Texture(mtex, cubemap, mipmap);
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
	GPU_set_tpage(NULL, 1, m_material->alphablend);
}


void KX_BlenderMaterial::SetShaderData(RAS_IRasterizer *ras)
{
	BLI_assert(GLEW_ARB_shader_objects && m_shader);

	int i;

	m_shader->SetProg(true);

	m_shader->ApplyShader();

	// for each enabled unit
	for (i = 0; i < BL_Texture::GetMaxUnits(); i++) {
		if (!m_textures[i] || !m_textures[i]->Ok()) {
			continue;
		}
		m_textures[i]->ActivateTexture(i);
	}

	if (!m_userDefBlend) {
		ras->SetAlphaBlend(m_material->alphablend);
	}
	else {
		ras->SetAlphaBlend(GPU_BLEND_SOLID);
		ras->SetAlphaBlend(-1); // indicates custom mode

		// tested to be valid enums
		glEnable(GL_BLEND);
		glBlendFunc(m_blendFunc[0], m_blendFunc[1]);
	}
}

void KX_BlenderMaterial::SetBlenderShaderData(RAS_IRasterizer *ras)
{
	// Don't set the alpha blend here because ActivateMeshSlot do it.
	m_blenderShader->SetProg(true, ras->GetTime(), ras);
}

void KX_BlenderMaterial::SetTexData(RAS_IRasterizer *ras)
{
	int mode = 0, i = 0;
	for (i = 0; i < BL_Texture::GetMaxUnits(); i++) {
		if (!m_textures[i] || !m_textures[i]->Ok()) {
			continue;
		}

		m_textures[i]->ActivateTexture(i);
		mode = m_material->mapping[i].mapping;

		if (mode & USEOBJ)
			SetObjectMatrixData(i, ras);

		if (!(mode & USEOBJ))
			SetTexMatrixData(i);
	}

	if (!m_userDefBlend) {
		ras->SetAlphaBlend(m_material->alphablend);
	}
	else {
		ras->SetAlphaBlend(GPU_BLEND_SOLID);
		ras->SetAlphaBlend(-1); // indicates custom mode

		glEnable(GL_BLEND);
		glBlendFunc(m_blendFunc[0], m_blendFunc[1]);
	}
}

void KX_BlenderMaterial::ActivateShaders(RAS_IRasterizer *rasty)
{
	SetShaderData(rasty);

	if (IsWire()) {
		rasty->SetCullFace(false);
	}
	else if (m_material->ras_mode & TWOSIDED) {
		rasty->SetCullFace(false);
	}
	else {
		rasty->SetCullFace(true);
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
	else if (m_material->ras_mode & TWOSIDED) {
		rasty->SetCullFace(false);
	}
	else {
		rasty->SetCullFace(true);
	}

	ActivateGLMaterials(rasty);
	m_blenderShader->SetAttribs(rasty);
}

void KX_BlenderMaterial::ActivateMat(RAS_IRasterizer *rasty)
{
	SetTexData(rasty);

	if (m_material->ras_mode & TWOSIDED)
		rasty->SetCullFace(false);
	else
		rasty->SetCullFace(true);

	if ((m_material->ras_mode & WIRE) || (rasty->GetDrawingMode() <= RAS_IRasterizer::RAS_WIREFRAME)) {
		if (m_material->ras_mode & WIRE)
			rasty->SetCullFace(false);
		rasty->SetLines(true);
	}
	else
		rasty->SetLines(false);
	ActivateGLMaterials(rasty);
	ActivateTexGen(rasty);
}

void KX_BlenderMaterial::Activate(RAS_IRasterizer *rasty)
{
	if (GLEW_ARB_shader_objects && (m_shader && m_shader->Ok())) {
		ActivateShaders(rasty);
	}
	else if (GLEW_ARB_shader_objects && (m_blenderShader && m_blenderShader->Ok())) {
		ActivateBlenderShaders(rasty);
	}
	else {
		ActivateMat(rasty);
	}
}
void KX_BlenderMaterial::Desactivate(RAS_IRasterizer *rasty)
{
	if (GLEW_ARB_shader_objects && (m_shader && m_shader->Ok())) {
		m_shader->SetProg(false);
		for (unsigned short i = 0; i < BL_Texture::GetMaxUnits(); i++) {
			if (m_textures[i] && m_textures[i]->Ok()) {
				m_textures[i]->DisableTexture();
			}
		}
	}
	else if (GLEW_ARB_shader_objects && (m_blenderShader && m_blenderShader->Ok())) {
		m_blenderShader->SetProg(false);
	}
	// Make sure no one will use the attributs set by this material.
	rasty->SetTexCoordNum(0);
	rasty->SetAttribNum(0);
}

bool KX_BlenderMaterial::IsWire() const
{
	return m_material->ras_mode & WIRE;
}

bool KX_BlenderMaterial::IsAlphaShadow() const
{
	return m_material->alphablend != GEMAT_SOLID;
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
	return m_material->material->shade_flag & MA_INSTANCING;
}

void KX_BlenderMaterial::ActivateInstancing(RAS_IRasterizer *rasty, void *matrixoffset, void *positionoffset, void *coloroffset, unsigned int stride)
{
	if (m_blenderShader) {
		m_blenderShader->ActivateInstancing(matrixoffset, positionoffset, coloroffset, stride);
	}

	/* Because the geometry instancing use setting for all instances we use the original alpha blend.
	 * This requierd that the user use "alpha blend" mode if he will use mutate object color alpha.
	 */
	rasty->SetAlphaBlend(m_material->alphablend);
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
	if (m_shader && GLEW_ARB_shader_objects) {
		m_shader->Update(ms, rasty);
	}
	else if (m_blenderShader && GLEW_ARB_shader_objects) {
		m_blenderShader->Update(ms, rasty);

		/* we do blend modes here, because they can change per object
		 * with the same material due to obcolor/obalpha */
		int alphablend = m_blenderShader->GetAlphaBlend();
		if (ELEM(alphablend, GEMAT_SOLID, GEMAT_ALPHA, GEMAT_ALPHA_SORT) && m_material->alphablend != GEMAT_SOLID)
			alphablend = m_material->alphablend;

		rasty->SetAlphaBlend(alphablend);
	}
}

void KX_BlenderMaterial::ActivateGLMaterials(RAS_IRasterizer *rasty) const
{
	if (m_shader || !m_blenderShader) {
		rasty->SetSpecularity(
		    m_material->speccolor[0] * m_material->spec_f,
		    m_material->speccolor[1] * m_material->spec_f,
		    m_material->speccolor[2] * m_material->spec_f,
		    m_material->spec_f
		    );

		rasty->SetShinyness(m_material->hard);

		rasty->SetDiffuse(
		    m_material->matcolor[0] * m_material->ref + m_material->emit,
		    m_material->matcolor[1] * m_material->ref + m_material->emit,
		    m_material->matcolor[2] * m_material->ref + m_material->emit,
		    1.0f);

		rasty->SetEmissive(
		    m_material->matcolor[0] * m_material->emit,
		    m_material->matcolor[1] * m_material->emit,
		    m_material->matcolor[2] * m_material->emit,
		    1.0f);

		rasty->SetAmbient(m_material->amb);
	}

	if (m_material->material)
		rasty->SetPolygonOffset(-m_material->material->zoffs, 0.0f);
}


void KX_BlenderMaterial::ActivateTexGen(RAS_IRasterizer *ras) const
{
	ras->SetAttribNum(0);
	if (m_shader && GLEW_ARB_shader_objects) {
		if (m_shader->GetAttribute() == BL_Shader::SHD_TANGENT) {
			ras->SetAttrib(RAS_IRasterizer::RAS_TEXCO_DISABLE, 0);
			ras->SetAttrib(RAS_IRasterizer::RAS_TEXTANGENT, 1);
			ras->SetAttribNum(2);
		}
	}

	ras->SetTexCoordNum(m_material->num_enabled);

	for (int i = 0; i < BL_Texture::GetMaxUnits(); i++) {
		int mode = m_material->mapping[i].mapping;

		if (mode & (USEREFL | USEOBJ))
			ras->SetTexCoord(RAS_IRasterizer::RAS_TEXCO_GEN, i);
		else if (mode & USEORCO)
			ras->SetTexCoord(RAS_IRasterizer::RAS_TEXCO_ORCO, i);
		else if (mode & USENORM)
			ras->SetTexCoord(RAS_IRasterizer::RAS_TEXCO_NORM, i);
		else if (mode & USEUV)
			ras->SetTexCoord(RAS_IRasterizer::RAS_TEXCO_UV, i);
		else if (mode & USETANG)
			ras->SetTexCoord(RAS_IRasterizer::RAS_TEXTANGENT, i);
		else
			ras->SetTexCoord(RAS_IRasterizer::RAS_TEXCO_DISABLE, i);
	}
}

void KX_BlenderMaterial::SetTexMatrixData(int i)
{
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();

	if (GLEW_ARB_texture_cube_map &&
	    m_textures[i]->GetTextureType() == GL_TEXTURE_CUBE_MAP_ARB &&
	    m_material->mapping[i].mapping & USEREFL)
	{
		glScalef(m_material->mapping[i].scale[0], -m_material->mapping[i].scale[1], -m_material->mapping[i].scale[2]);
	}
	else {
		glScalef(m_material->mapping[i].scale[0], m_material->mapping[i].scale[1], m_material->mapping[i].scale[2]);
	}
	glTranslatef(m_material->mapping[i].offsets[0], m_material->mapping[i].offsets[1], m_material->mapping[i].offsets[2]);

	glMatrixMode(GL_MODELVIEW);
}

static void GetProjPlane(BL_Material *mat, int index, int num, float *param)
{
	param[0] = param[1] = param[2] = param[3] = 0.0f;
	if (mat->mapping[index].projplane[num] == PROJX)
		param[0] = 1.0f;
	else if (mat->mapping[index].projplane[num] == PROJY)
		param[1] = 1.0f;
	else if (mat->mapping[index].projplane[num] == PROJZ)
		param[2] = 1.0f;
}

void KX_BlenderMaterial::SetObjectMatrixData(int i, RAS_IRasterizer *ras)
{
	KX_GameObject *obj = (KX_GameObject *)m_scene->GetObjectList()->FindValue(m_material->mapping[i].objconame);

	if (!obj) {
		return;
	}

	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
	glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);

	GLenum plane = GL_EYE_PLANE;

	// figure plane gen
	float proj[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	GetProjPlane(m_material, i, 0, proj);
	glTexGenfv(GL_S, plane, proj);

	GetProjPlane(m_material, i, 1, proj);
	glTexGenfv(GL_T, plane, proj);

	GetProjPlane(m_material, i, 2, proj);
	glTexGenfv(GL_R, plane, proj);

	glEnable(GL_TEXTURE_GEN_S);
	glEnable(GL_TEXTURE_GEN_T);
	glEnable(GL_TEXTURE_GEN_R);

	const MT_Matrix4x4& mvmat = ras->GetViewMatrix();

	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glScalef(m_material->mapping[i].scale[0], m_material->mapping[i].scale[1], m_material->mapping[i].scale[2]);

	MT_Vector3 pos = obj->NodeGetWorldPosition();
	MT_Vector4 matmul = MT_Vector4(pos[0], pos[1], pos[2], 1.0f);
	MT_Vector4 t = mvmat * matmul;

	glTranslatef((float)(-t[0]), (float)(-t[1]), (float)(-t[2]));

	glMatrixMode(GL_MODELVIEW);
}

// ------------------------------------
void KX_BlenderMaterial::UpdateIPO(
    MT_Vector4 rgba,
    MT_Vector3 specrgb,
    MT_Scalar hard,
    MT_Scalar spec,
    MT_Scalar ref,
    MT_Scalar emit,
    MT_Scalar alpha)
{
	// only works one deep now

	// GLSL							Multitexture				Input
	m_material->material->specr = m_material->speccolor[0] = (float)(specrgb)[0];
	m_material->material->specg = m_material->speccolor[1] = (float)(specrgb)[1];
	m_material->material->specb = m_material->speccolor[2] = (float)(specrgb)[2];
	m_material->material->r = m_material->matcolor[0] = (float)(rgba[0]);
	m_material->material->g = m_material->matcolor[1] = (float)(rgba[1]);
	m_material->material->b = m_material->matcolor[2] = (float)(rgba[2]);
	m_material->material->alpha = m_material->alpha = (float)(rgba[3]);
	m_material->material->har = m_material->hard = (float)(hard);
	m_material->material->emit = m_material->emit = (float)(emit);
	m_material->material->spec = m_material->spec_f = (float)(spec);
	m_material->material->ref = m_material->ref = (float)(ref);
}

void KX_BlenderMaterial::Replace_IScene(SCA_IScene *val)
{
	m_scene = static_cast<KX_Scene *>(val);

	OnConstruction();
}

BL_Material *KX_BlenderMaterial::GetBLMaterial()
{
	return m_material;
}

void KX_BlenderMaterial::SetBlenderGLSLShader()
{
	if (!m_blenderShader)
		m_blenderShader = new BL_BlenderShader(m_scene, m_material->material, m_material, m_lightLayer);

	if (!m_blenderShader->Ok()) {
		delete m_blenderShader;
		m_blenderShader = NULL;
	}
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

	switch (subtype) {
		case MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR:
		{
			copy_v3_v3(bmo->data, self->GetBLMaterial()->matcolor);
			break;
		}
		case MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR:
		{
			copy_v3_v3(bmo->data, self->GetBLMaterial()->speccolor);
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

	switch (subtype) {
		case MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR:
		{
			BL_Material *mat = self->GetBLMaterial();
			copy_v3_v3(mat->matcolor, bmo->data);
			mat->material->r = bmo->data[0];
			mat->material->g = bmo->data[1];
			mat->material->b = bmo->data[2];
			break;
		}
		case MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR:
		{
			BL_Material *mat = self->GetBLMaterial();
			copy_v3_v3(mat->speccolor, bmo->data);
			mat->material->specr = bmo->data[0];
			mat->material->specg = bmo->data[1];
			mat->material->specb = bmo->data[2];
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
	&PyObjectPlus::Type,
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
	return MAXTEX;
}

static PyObject *kx_blender_material_get_textures_item_cb(void *self_v, int index)
{
	BL_Texture *tex = ((KX_BlenderMaterial *)self_v)->GetTex(index);
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
	BL_Texture *tex = ((KX_BlenderMaterial *)self_v)->GetTex(index);
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
	return PyFloat_FromDouble(self->GetBLMaterial()->alpha);
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

	BL_Material *mat = self->GetBLMaterial();
	mat->alpha = mat->material->alpha = val;
	return PY_SET_ATTR_SUCCESS;
}
PyObject *KX_BlenderMaterial::pyattr_get_hardness(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyLong_FromLong(self->GetBLMaterial()->hard);
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

	BL_Material *mat = self->GetBLMaterial();
	mat->hard = mat->material->har = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_specular_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBLMaterial()->spec_f);
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

	BL_Material *mat = self->GetBLMaterial();
	mat->spec_f = mat->material->spec = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_specular_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(BGE_PROXY_FROM_REF(self_v), mathutils_kxblendermaterial_color_cb_index, MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR);
#else
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyColorFromVector(MT_Vector3(self->GetBLMaterial()->speccolor));
#endif
}

int KX_BlenderMaterial::pyattr_set_specular_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	MT_Vector3 color;
	if (!PyVecTo(value, color))
		return PY_SET_ATTR_FAIL;

	BL_Material *mat = self->GetBLMaterial();
	color.getValue(mat->speccolor);
	mat->material->specr = color[0];
	mat->material->specg = color[1];
	mat->material->specb = color[2];
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_diffuse_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBLMaterial()->ref);
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

	BL_Material *mat = self->GetBLMaterial();
	mat->ref = mat->material->ref = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_diffuse_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(BGE_PROXY_FROM_REF(self_v), mathutils_kxblendermaterial_color_cb_index, MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR);
#else
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyColorFromVector(MT_Vector3(self->GetBLMaterial()->matcolor));
#endif
}

int KX_BlenderMaterial::pyattr_set_diffuse_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	MT_Vector3 color;
	if (!PyVecTo(value, color))
		return PY_SET_ATTR_FAIL;

	BL_Material *mat = self->GetBLMaterial();
	color.getValue(mat->matcolor);
	mat->material->r = color[0];
	mat->material->g = color[1];
	mat->material->b = color[2];
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_emit(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBLMaterial()->emit);
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

	BL_Material *mat = self->GetBLMaterial();
	mat->emit = mat->material->emit = val;
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
	if (!GLEW_ARB_fragment_shader) {
		if (!m_modified)
			spit("Fragment shaders not supported");

		m_modified = true;
		Py_RETURN_NONE;
	}

	if (!GLEW_ARB_vertex_shader) {
		if (!m_modified)
			spit("Vertex shaders not supported");

		m_modified = true;
		Py_RETURN_NONE;
	}

	if (!GLEW_ARB_shader_objects) {
		if (!m_modified)
			spit("GLSL not supported");
		m_modified = true;
		Py_RETURN_NONE;
	}
	else {
		// returns Py_None on error
		// the calling script will need to check

		if (!m_shader && !m_modified) {
			m_shader = new BL_Shader();
			m_modified = true;

			// Using a custom shader, make sure to initialize textures
			InitTextures();
		}

		if (m_shader && !m_shader->GetError()) {
			m_flag &= ~RAS_BLENDERGLSL;
			m_material->SetSharedMaterial(true);
			m_scene->GetBucketManager()->ReleaseDisplayLists(this);
			return m_shader->GetProxy();
		}
		else {
			// decref all references to the object
			// then delete it!
			// We will then go back to fixed functionality
			// for this material
			if (m_shader) {
				delete m_shader; /* will handle python de-referencing */
				m_shader = NULL;
			}
		}
		Py_RETURN_NONE;
	}
	PyErr_SetString(PyExc_ValueError, "material.getShader(): KX_BlenderMaterial, GLSL Error");
	return NULL;
}

static const unsigned int GL_array[11] = {
	GL_ZERO,
	GL_ONE,
	GL_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR,
	GL_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA,
	GL_SRC_ALPHA_SATURATE
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
	unsigned int texslot;
	if (!PyArg_ParseTuple(args, "i:texslot", &texslot)) {
		PyErr_SetString(PyExc_ValueError, "material.getTextureBindcode(texslot): KX_BlenderMaterial, expected an int.");
		return NULL;
	}
	Image *ima = GetImage(texslot);
	if (ima) {
		unsigned int *bindcode = ima->bindcode;
		return PyLong_FromLong(*bindcode);
	}
	PyErr_SetString(PyExc_ValueError, "material.getTextureBindcode(texslot): KX_BlenderMaterial, invalid texture slot.");
	return NULL;
}

#endif // WITH_PYTHON
