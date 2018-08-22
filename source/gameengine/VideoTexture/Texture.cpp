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
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright (c) 2007 The Zdeno Ash Miklas
 *
 * This source file is part of VideoTexture library
 *
 * Contributor(s):
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/VideoTexture/Texture.cpp
 *  \ingroup bgevideotex
 */

// implementation

#include "EXP_PyObjectPlus.h"
#include <structmember.h>

#include "KX_GameObject.h"
#include "KX_LightObject.h"
#include "RAS_ILightObject.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_image_types.h"
#include "IMB_imbuf_types.h"
#include "BKE_image.h"

#include "MEM_guardedalloc.h"

#include "RAS_MaterialBucket.h"
#include "RAS_IMaterial.h"
#include "RAS_Texture.h"

#include "KX_KetsjiEngine.h"
#include "KX_Globals.h"
#include "KX_Mesh.h"
#include "Texture.h"
#include "ImageBase.h"
#include "Exception.h"

#include <memory.h>
#include "GPU_glew.h"

#include "CM_Message.h"

extern "C" {
	#include "IMB_imbuf.h"
}

static std::vector<Texture *> textures;

// macro for exception handling and logging
#define CATCH_EXCP catch (Exception & exp) \
	{ exp.report(); return nullptr; }

PyObject *Texture_close(Texture *self);

Texture::Texture()
	:m_actTex(0),
	m_orgTex(0),
	m_orgImg(0),
	m_orgSaved(false),
	m_imgBuf(nullptr),
	m_imgTexture(nullptr),
	m_matTexture(nullptr),
	m_scene(nullptr),
	m_mipmap(false),
	m_scaledImBuf(nullptr),
	m_lastClock(0.0),
	m_source(nullptr)
{
	textures.push_back(this);
}

Texture::~Texture()
{
	// release renderer
	Py_XDECREF(m_source);
	// close texture
	Close();
	// release scaled image buffer
	IMB_freeImBuf(m_scaledImBuf);
}

void Texture::DestructFromPython()
{
	std::vector<Texture *>::iterator it = std::find(textures.begin(), textures.end(), this);
	if (it != textures.end()) {
		textures.erase(it);
	}

	EXP_PyObjectPlus::DestructFromPython();
}

std::string Texture::GetName()
{
	return "Texture";
}

void Texture::FreeAllTextures(KX_Scene *scene)
{
	for (std::vector<Texture *>::iterator it = textures.begin(); it != textures.end(); ) {
		Texture *texture = *it;
		if (texture->m_scene != scene) {
			++it;
			continue;
		}

		it = textures.erase(it);
		texture->Release();
	}
}

void Texture::Close()
{
	if (m_orgSaved) {
		m_orgSaved = false;
		// restore original texture code
		if (m_useMatTexture) {
			m_matTexture->SetBindCode(m_orgTex);
			if (m_imgTexture) {
				// This is requierd for texture used in blender material.
				m_imgTexture->bindcode[TEXTARGET_TEXTURE_2D] = m_orgImg;
			}
		}
		else {
			m_imgTexture->bindcode[TEXTARGET_TEXTURE_2D] = m_orgImg;
			BKE_image_release_ibuf(m_imgTexture, m_imgBuf, nullptr);
			m_imgBuf = nullptr;
		}
		// drop actual texture
		if (m_actTex != 0) {
			glDeleteTextures(1, (GLuint *)&m_actTex);
			m_actTex = 0;
		}
	}
}

void Texture::SetSource(PyImage *source)
{
	BLI_assert(source != nullptr);
	Py_XDECREF(m_source);
	Py_INCREF(source);
	m_source = source;
}

// load texture
void loadTexture(unsigned int texId, unsigned int *texture, short *size,
                 bool mipmap, unsigned int internalFormat)
{
	// load texture for rendering
	glBindTexture(GL_TEXTURE_2D, texId);
	if (mipmap) {
		int i;
		ImBuf *ibuf;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		ibuf = IMB_allocFromBuffer(texture, nullptr, size[0], size[1]);

		IMB_makemipmap(ibuf, true);

		for (i = 0; i < ibuf->miptot; i++) {
			ImBuf *mip = IMB_getmipmap(ibuf, i);

			glTexImage2D(GL_TEXTURE_2D, i, internalFormat, mip->x, mip->y, 0, GL_RGBA, GL_UNSIGNED_BYTE, mip->rect);
		}
		IMB_freeImBuf(ibuf);
	}
	else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, size[0], size[1], 0, GL_RGBA, GL_UNSIGNED_BYTE, texture);
	}
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}


// get pointer to material
RAS_IMaterial *getMaterial(KX_GameObject *gameObj, short matID)
{
	// get pointer to texture image
	const std::vector<KX_Mesh *>& meshes = gameObj->GetMeshList();

	if (meshes.empty()) {
		return nullptr;
	}

	// get material from mesh
	KX_Mesh *mesh = meshes.front();
	RAS_MeshMaterial *meshMat = mesh->GetMeshMaterial(matID);
	if (meshMat && meshMat->GetBucket()) {
		// return pointer to polygon or blender material
		return meshMat->GetBucket()->GetMaterial();
	}

	// otherwise material was not found
	return nullptr;
}

// get material ID
short getMaterialID(PyObject *obj, const char *name)
{
	// search for material
	for (short matID = 0;; ++matID)
	{
		// get material
		KX_GameObject *gameObj;
		if (!ConvertPythonToGameObject(KX_GetActiveScene()->GetLogicManager(), obj, &gameObj, false, "")) {
			break;
		}

		RAS_IMaterial *mat = getMaterial(gameObj, matID);
		// if material is not available, report that no material was found
		if (mat == nullptr) {
			break;
		}
		// name is a material name if it starts with MA and a UV texture name if it starts with IM
		if (name[0] == 'I' && name[1] == 'M') {
			// if texture name matches
			if (mat->GetTextureName() == name) {
				return matID;
			}
		}
		else {
			// if material name matches
			if (mat->GetName() == name) {
				return matID;
			}
		}
	}
	// material was not found
	return -1;
}


// Texture object allocation
static PyObject *Texture_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	// allocate object
	Texture *self = new Texture();

	// return allocated object
	return self->NewProxy(true);
}

ExceptionID MaterialNotAvail;
ExpDesc MaterialNotAvailDesc(MaterialNotAvail, "Texture material is not available");

ExceptionID TextureNotAvail;
ExpDesc TextureNotAvailDesc(TextureNotAvail, "Texture is not available");

// Texture object initialization
static int Texture_init(PyObject *self, PyObject *args, PyObject *kwds)
{
	if (!EXP_PROXY_PYREF(self)) {
		return -1;
	}

	Texture *tex = (Texture *)EXP_PROXY_REF(self);

	// parameters - game object with video texture
	PyObject *obj = nullptr;
	// material ID
	short matID = 0;
	// texture ID
	short texID = 0;
	// texture object with shared texture ID
	Texture *texObj = nullptr;

	static const char *kwlist[] = {"gameObj", "materialID", "textureID", "textureObj", nullptr};

	// get parameters
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|hhO!",
	                                 const_cast<char **>(kwlist), &obj, &matID, &texID, &Texture::Type,
	                                 &texObj)) {
		return -1;
	}

	KX_GameObject *gameObj = nullptr;
	if (ConvertPythonToGameObject(KX_GetActiveScene()->GetLogicManager(), obj, &gameObj, false, "")) {
		// process polygon material or blender material
		try
		{
			tex->m_scene = gameObj->GetScene();
			// get pointer to texture image
			RAS_IMaterial *mat = getMaterial(gameObj, matID);
			KX_LightObject *lamp = nullptr;
			if (gameObj->GetGameObjectType() == SCA_IObject::OBJ_LIGHT) {
				lamp = (KX_LightObject *)gameObj;
			}

			if (mat != nullptr) {
				// get blender material texture
				tex->m_matTexture = mat->GetTexture(texID);
				if (!tex->m_matTexture) {
					THRWEXCP(TextureNotAvail, S_OK);
				}
				tex->m_imgTexture = tex->m_matTexture->GetImage();
				tex->m_useMatTexture = true;
			}
			else if (lamp != nullptr) {
				tex->m_imgTexture = lamp->GetLightData()->GetTextureImage(texID);
				tex->m_useMatTexture = false;
			}

			// check if texture is available, if not, initialization failed
			if (tex->m_imgTexture == nullptr && tex->m_matTexture == nullptr) {
				// throw exception if initialization failed
				THRWEXCP(MaterialNotAvail, S_OK);
			}

			// if texture object is provided
			if (texObj != nullptr) {
				// copy texture code
				tex->m_actTex = texObj->m_actTex;
				tex->m_mipmap = texObj->m_mipmap;
				if (texObj->m_source != nullptr) {
					tex->SetSource(texObj->m_source);
				}
			}
			else {
				// otherwise generate texture code
				glGenTextures(1, (GLuint *)&tex->m_actTex);
			}
		}
		catch (Exception & exp)
		{
			exp.report();
			return -1;
		}
	}
	// initialization succeded
	return 0;
}

// close added texture
EXP_PYMETHODDEF_DOC(Texture, close, "Close dynamic texture and restore original")
{
	// restore texture
	Close();
	Py_RETURN_NONE;
}

// refresh texture
EXP_PYMETHODDEF_DOC(Texture, refresh, "Refresh texture from source")
{
	// get parameter - refresh source
	PyObject *param;
	double ts = -1.0;

	if (!PyArg_ParseTuple(args, "O|d:refresh", &param, &ts) || !PyBool_Check(param)) {
		// report error
		PyErr_SetString(PyExc_TypeError, "The value must be a bool");
		return nullptr;
	}
	// some trick here: we are in the business of loading a texture,
	// no use to do it if we are still in the same rendering frame.
	// We find this out by looking at the engine current clock time
	KX_KetsjiEngine *engine = KX_GetActiveEngine();
	if (engine->GetClockTime() != m_lastClock) {
		m_lastClock = engine->GetClockTime();
		// set source refresh
		bool refreshSource = (param == Py_True);
		// try to proces texture from source
		try
		{
			// if source is available
			if (m_source != nullptr) {
				// check texture code
				if (!m_orgSaved) {
					m_orgSaved = true;
					// save original image code
					if (m_useMatTexture) {
						m_orgTex = m_matTexture->GetBindCode();
						m_matTexture->SetBindCode(m_actTex);
						if (m_imgTexture) {
							m_orgImg = m_imgTexture->bindcode[TEXTARGET_TEXTURE_2D];
							m_imgTexture->bindcode[TEXTARGET_TEXTURE_2D] = m_actTex;
						}
					}
					else {
						// Swapping will work only if the GPU has already loaded the image.
						// If not, it will delete and overwrite our texture on next render.
						// To avoid that, we acquire the image buffer now.
						// WARNING: GPU has a ImageUser to pass, we don't. Using nullptr
						// works on image file, not necessarily on other type of image.
						m_imgBuf = BKE_image_acquire_ibuf(m_imgTexture, nullptr, nullptr);
						m_orgImg = m_imgTexture->bindcode[TEXTARGET_TEXTURE_2D];
						m_imgTexture->bindcode[TEXTARGET_TEXTURE_2D] = m_actTex;
					}
				}

				// get texture
				unsigned int *texture = m_source->m_image->getImage(m_actTex, ts, m_mipmap);
				// if texture is available
				if (texture != nullptr) {
					// get texture size
					short *orgSize = m_source->m_image->getSize();
					// calc scaled sizes
					short size[2];
					if (GLEW_ARB_texture_non_power_of_two) {
						size[0] = orgSize[0];
						size[1] = orgSize[1];
					}
					else {
						size[0] = ImageBase::calcSize(orgSize[0]);
						size[1] = ImageBase::calcSize(orgSize[1]);
					}
					// scale texture if needed
					if (size[0] != orgSize[0] || size[1] != orgSize[1]) {
						IMB_freeImBuf(m_scaledImBuf);
						m_scaledImBuf = IMB_allocFromBuffer(texture, nullptr, orgSize[0], orgSize[1]);
						IMB_scaleImBuf(m_scaledImBuf, size[0], size[1]);
						// use scaled image instead original
						texture = m_scaledImBuf->rect;
					}
					// load texture for rendering
					loadTexture(m_actTex, texture, size, m_mipmap, m_source->m_image->GetInternalFormat());
				}
				// refresh texture source, if required
				if (refreshSource) {
					m_source->m_image->refresh();
				}
			}
		}
		CATCH_EXCP;
	}
	Py_RETURN_NONE;
}

// get OpenGL Bind Id
PyObject *Texture::pyattr_get_bindId(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	Texture *self = (Texture *)self_v;

	unsigned int id = self->m_actTex;
	return Py_BuildValue("h", id);
}

// get mipmap value
PyObject *Texture::pyattr_get_mipmap(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	Texture *self = (Texture *)self_v;

	// return true if flag is set, otherwise false
	if (self->m_mipmap) {
		Py_RETURN_TRUE;
	}
	else {
		Py_RETURN_FALSE;
	}
}

// set mipmap value
int Texture::pyattr_set_mipmap(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	Texture *self = (Texture *)self_v;

	// check parameter, report failure
	if (value == nullptr || !PyBool_Check(value)) {
		PyErr_SetString(PyExc_TypeError, "The value must be a bool");
		return -1;
	}
	// set mipmap
	self->m_mipmap = value == Py_True;
	// success
	return 0;
}

// get source object
PyObject *Texture::pyattr_get_source(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	Texture *self = (Texture *)self_v;

	// if source exists
	if (self->m_source != nullptr) {
		Py_INCREF(self->m_source);
		return reinterpret_cast<PyObject *>(self->m_source);
	}
	// otherwise return None
	Py_RETURN_NONE;
}

// set source object
int Texture::pyattr_set_source(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	Texture *self = (Texture *)self_v;
	// check new value
	if (value == nullptr || !pyImageTypes.in(Py_TYPE(value))) {
		// report value error
		PyErr_SetString(PyExc_TypeError, "Invalid type of value");
		return -1;
	}
	self->SetSource(reinterpret_cast<PyImage *>(value));
	// return success
	return 0;
}

// class Texture methods
PyMethodDef Texture::Methods[] = {
	EXP_PYMETHODTABLE(Texture, close),
	EXP_PYMETHODTABLE(Texture, refresh),
	{nullptr, nullptr}  // Sentinel
};

// class Texture attributes
PyAttributeDef Texture::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("mipmap", Texture, pyattr_get_mipmap, pyattr_set_mipmap),
	EXP_PYATTRIBUTE_RW_FUNCTION("source", Texture, pyattr_get_source, pyattr_set_source),
	EXP_PYATTRIBUTE_RO_FUNCTION("bindId", Texture, pyattr_get_bindId),
	EXP_PYATTRIBUTE_NULL
};


// class Texture declaration
PyTypeObject Texture::Type =
{
	PyVarObject_HEAD_INIT(nullptr, 0)
	"VideoTexture.Texture",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0,
	&imageBufferProcs,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_PyObjectPlus::Type,
	0, 0, 0, 0,
	(initproc)Texture_init,
	0,
	Texture_new
};
