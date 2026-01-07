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

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BLI_listbase.h"
#include "DNA_material_types.h"

#include "BL_Shader.h"
#include "BL_Texture.h"
#include "EXP_ListWrapper.h"
#include "KX_KetsjiEngine.h"
#include "KX_MaterialShader.h"
#include "RAS_BucketManager.h"

#ifdef WITH_PYTHON
#  include "bpy_rna.hh"

using namespace blender;
#endif

KX_BlenderMaterial::KX_BlenderMaterial(RAS_Rasterizer *rasty,
                                       KX_Scene *scene,
                                       blender::Material *mat,
                                       const std::string &name,
                                       int lightlayer,
                                       bool converting_during_runtime)
    : RAS_IPolyMaterial(name),
      m_material(mat),
      m_shader(nullptr),
      m_rasterizer(rasty),
      m_scene(scene),
      m_userDefBlend(false),
      m_constructed(false),
      m_lightLayer(lightlayer)
{
  m_nodetree = nullptr;
  m_alphablend = mat->blend_method;
  m_nodetree = m_material->nodetree;
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

const std::string KX_BlenderMaterial::GetTextureName() const
{
  return (m_textures[0] ? m_textures[0]->GetName() : "");
}

blender::Material *KX_BlenderMaterial::GetBlenderMaterial() const
{
  return m_material;
}

blender::Scene *KX_BlenderMaterial::GetBlenderScene() const
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
  if (!m_nodetree) {
    // No node tree, so no textures
    return;
  }
  int i = 0;
  for (bNode &node : m_nodetree->nodes) {
    if ((node.type_legacy == SH_NODE_TEX_IMAGE) ||
        (node.typeinfo && node.typeinfo->idname == "ShaderNodeTexImage")) {
      blender::Image *ima = (blender::Image *)node.id;
      if (ima) {
        if (i < RAS_Texture::MaxUnits) {
          BL_Texture *texture = new BL_Texture(ima);
          m_textures[i] = texture;
          i++;
        }
      }
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

PyMethodDef KX_BlenderMaterial::Methods[] = {
    EXP_PYMETHODTABLE(KX_BlenderMaterial, getShader),
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef KX_BlenderMaterial::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("textures", KX_BlenderMaterial, pyattr_get_textures),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyTypeObject KX_BlenderMaterial::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_BlenderMaterial",
                                         sizeof(EXP_PyObjectPlus_Proxy),
                                         0,
                                         py_base_dealloc,
                                         0,
                                         0,
                                         0,
                                         0,
                                         py_base_repr,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         Methods,
                                         0,
                                         0,
                                         &EXP_Value::Type,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         py_base_new};

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

PyObject *KX_BlenderMaterial::pyattr_get_textures(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return (new EXP_ListWrapper(self_v,
                              ((KX_BlenderMaterial *)self_v)->GetProxy(),
                              nullptr,
                              kx_blender_material_get_textures_size_cb,
                              kx_blender_material_get_textures_item_cb,
                              kx_blender_material_get_textures_item_name_cb,
                              nullptr))
      ->NewProxy(true);
}

EXP_PYMETHODDEF_DOC(KX_BlenderMaterial, getShader, "getShader()")
{
  /* EEVEE: Any way to restore Custom shaders without bge rendering pipeline */
  if (!m_shader) {
    m_shader.reset(new KX_MaterialShader());
    // Set the material to use custom shader.
    m_scene->GetBucketManager()->UpdateShaders(this);
  }

  return m_shader->GetShader()->GetProxy();
}

bool ConvertPythonToMaterial(PyObject *value,
                             KX_BlenderMaterial **material,
                             bool py_none_ok,
                             const char *error_prefix)
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
      PyErr_Format(PyExc_TypeError,
                   "%s, expected KX_BlenderMaterial or a KX_BlenderMaterial name, None is invalid",
                   error_prefix);
      return false;
    }
  }

  if (PyObject_TypeCheck(value, &KX_BlenderMaterial::Type)) {
    KX_BlenderMaterial *mat = static_cast<KX_BlenderMaterial *> EXP_PROXY_REF(value);

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
    PyErr_Format(
        PyExc_TypeError, "%s, expect a KX_BlenderMaterial, a string or None", error_prefix);
  }
  else {
    PyErr_Format(PyExc_TypeError, "%s, expect a KX_BlenderMaterial or a string", error_prefix);
  }

  return false;
}

#endif  // WITH_PYTHON
