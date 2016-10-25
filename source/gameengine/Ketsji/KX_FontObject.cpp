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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_FontObject.cpp
 *  \ingroup ketsji
 */

#include "KX_FontObject.h"
#include "DNA_curve_types.h"
#include "DNA_vfont_types.h"
#include "KX_Scene.h"
#include "KX_Globals.h"
#include "KX_PythonInit.h"
#include "KX_TextMaterial.h"
#include "KX_PyMath.h"
#include "BLI_math.h"
#include "EXP_StringValue.h"
#include "RAS_IRasterizer.h"
#include "RAS_BucketManager.h"
#include "RAS_MaterialBucket.h"
#include "RAS_TextUser.h"

/* paths needed for font load */
#include "BLI_blenlib.h"
#include "BKE_global.h"
#include "BKE_font.h"
#include "BKE_main.h"
#include "DNA_packedFile_types.h"

extern "C" {
#include "BLF_api.h"
}

#include "CM_Message.h"

#define BGE_FONT_RES 100

/* proptotype */
int GetFontId(VFont *font);

static std::vector<STR_String> split_string(STR_String str)
{
	std::vector<STR_String> text = std::vector<STR_String>();

	// Split the string upon new lines
	int begin = 0, end = 0;
	while (end < str.Length()) {
		if (str.GetAt(end) == '\n') {
			text.push_back(str.Mid(begin, end - begin));
			begin = end + 1;
		}
		end++;
	}
	// Now grab the last line
	text.push_back(str.Mid(begin, end - begin));

	return text;
}

KX_FontObject::KX_FontObject(void *sgReplicationInfo,
                             SG_Callbacks callbacks,
                             RAS_IRasterizer *rasterizer,
                             Object *ob,
                             bool do_color_management)
	:KX_GameObject(sgReplicationInfo, callbacks),
	m_object(ob),
	m_dpi(72),
	m_resolution(1.0f),
	m_rasterizer(rasterizer),
	m_do_color_management(do_color_management)
{
	Curve *text = static_cast<Curve *> (ob->data);
	m_text = split_string(text->str);
	m_fsize = text->fsize;
	m_line_spacing = text->linedist;
	m_offset = MT_Vector3(text->xof, text->yof, 0.0f);

	m_fontid = GetFontId(text->vfont);
}

KX_FontObject::~KX_FontObject()
{
	//remove font from the scene list
	//it's handled in KX_Scene::NewRemoveObject
}

CValue *KX_FontObject::GetReplica()
{
	KX_FontObject *replica = new KX_FontObject(*this);
	replica->ProcessReplica();
	return replica;
}

void KX_FontObject::ProcessReplica()
{
	KX_GameObject::ProcessReplica();
}

void KX_FontObject::AddMeshUser()
{
	m_meshUser = new RAS_TextUser(m_pClient_info);

	// set the part of the mesh slot that never change
	float *fl = GetOpenGLMatrixPtr()->getPointer();
	m_meshUser->SetMatrix(fl);

	RAS_BucketManager *bucketManager = GetScene()->GetBucketManager();
	bool created = false;
	RAS_MaterialBucket *bucket = bucketManager->FindBucket(GetTextMaterial(), created);

	// If the material bucket is just created then we add a new mesh slot.
	if (created) {
		RAS_TexVertFormat format;
		format.uvSize = 1;
		format.colorSize = 1;
		bucket->AddMesh(NULL, NULL, format);
	}

	/* We copy the original mesh slot which is at the begin of the list, if it's not the case it
	 * doesn't matter as the mesh slot are all similar exepted their mesh user pointer which is
	 * set to NULL in copy. By copying instead of adding a mesh slot we reuse the same display
	 * array bucket.
	 */
	RAS_MeshSlot *ms = bucket->CopyMesh(*bucket->msBegin());
	ms->SetMeshUser(m_meshUser);
	ms->SetDeformer(NULL);
	m_meshUser->AddMeshSlot(ms);
}

void KX_FontObject::UpdateBuckets()
{
	// Update datas and add mesh slot to be rendered only if the object is not culled.
	if (m_bVisible && m_meshUser) {
		if (m_pSGNode->IsDirty()) {
			GetOpenGLMatrix();
		}

		// Allow for some logic brick control
		if (GetProperty("Text")) {
			m_text = split_string(GetProperty("Text")->GetText());
		}

		// update the animated color
		GetObjectColor().getValue(m_color);

		// Font Objects don't use the glsl shader, this color management code is copied from gpu_shader_material.glsl
		float color[4];
		if (m_do_color_management) {
			linearrgb_to_srgb_v4(color, m_color);
		}
		else {
			copy_v4_v4(color, m_color);
		}


		// HARDCODED MULTIPLICATION FACTOR - this will affect the render resolution directly
		const float RES = BGE_FONT_RES * m_resolution;

		const float size = m_fsize * NodeGetWorldScaling()[0] * RES;
		const float aspect = m_fsize / size;

		// Account for offset
		MT_Vector3 offset = NodeGetWorldOrientation() * m_offset * NodeGetWorldScaling();
		// Orient the spacing vector
		MT_Vector3 spacing = MT_Vector3(0.0f, m_fsize * m_line_spacing, 0.0f);

		RAS_TextUser *textUser = (RAS_TextUser *)m_meshUser;

		textUser->SetColor(MT_Vector4(color));
		textUser->SetFrontFace(!m_bIsNegativeScaling);
		textUser->SetFontId(m_fontid);
		textUser->SetSize(size);
		textUser->SetDpi(m_dpi);
		textUser->SetAspect(aspect);
		textUser->SetOffset(offset);
		textUser->SetSpacing(spacing);
		textUser->SetTexts(m_text);
		textUser->ActivateMeshSlots();
	}
}

const MT_Vector2 KX_FontObject::GetTextDimensions()
{
	MT_Vector2 dimensions(0.0f, 0.0f);

	for (std::vector<STR_String>::iterator it = m_text.begin(); it != m_text.end(); ++it) {
		float w = 0.0f, h = 0.0f;
		const STR_String& text = *it;

		BLF_width_and_height(m_fontid, text.ReadPtr(), text.Length(), &w, &h);
		dimensions.x() = std::max(dimensions.x(), w);
		dimensions.y() += h + m_line_spacing;
	}

	// XXX: Quick hack to convert the size to BU
	dimensions /= 10.0f;

	// Scale the width and height by the object's scale
	const MT_Vector3& scale = NodeGetLocalScaling();
	dimensions.x() *= fabs(scale.x());
	dimensions.y() *= fabs(scale.y());

	return dimensions;
}

int GetFontId(VFont *vfont)
{
	PackedFile *packedfile = NULL;
	int fontid = -1;

	if (vfont->packedfile) {
		packedfile = vfont->packedfile;
		fontid = BLF_load_mem(vfont->name, (unsigned char *)packedfile->data, packedfile->size);

		if (fontid == -1) {
			CM_Error("packed font \"" << vfont->name << "\" could not be loaded");
			fontid = BLF_load("default");
		}
		return fontid;
	}

	// once we have packed working we can load the builtin font
	const char *filepath = vfont->name;
	if (BKE_vfont_is_builtin(vfont)) {
		fontid = BLF_load("default");

		/* XXX the following code is supposed to work (after you add get_builtin_packedfile to BKE_font.h )
		 * unfortunately it's crashing on blf_glyph.c:173 because gc->max_glyph_width is 0
		 */
		// packedfile=get_builtin_packedfile();
		// fontid= BLF_load_mem(font->name, (unsigned char*)packedfile->data, packedfile->size);
		// return fontid;

		return BLF_load("default");
	}

	// convert from absolute to relative
	char expanded[256]; // font names can be bigger than FILE_MAX (240)
	BLI_strncpy(expanded, filepath, 256);
	BLI_path_abs(expanded, KX_GetMainPath().ReadPtr());

	fontid = BLF_load(expanded);

	// fallback
	if (fontid == -1)
		fontid = BLF_load("default");

	return fontid;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python Integration Hooks					                                 */
/* ------------------------------------------------------------------------- */

PyTypeObject KX_FontObject::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_FontObject",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	&KX_GameObject::Sequence,
	&KX_GameObject::Mapping,
	0, 0, 0,
	NULL,
	NULL,
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&KX_GameObject::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_FontObject::Methods[] = {
	{NULL, NULL} //Sentinel
};

PyAttributeDef KX_FontObject::Attributes[] = {
	//KX_PYATTRIBUTE_STRING_RW("text", 0, 280, false, KX_FontObject, m_text[0]), //arbitrary limit. 280 = 140 unicode chars in unicode
	KX_PYATTRIBUTE_RW_FUNCTION("text", KX_FontObject, pyattr_get_text, pyattr_set_text),
	KX_PYATTRIBUTE_RO_FUNCTION("dimensions", KX_FontObject, pyattr_get_dimensions),
	KX_PYATTRIBUTE_FLOAT_RW("size", 0.0001f, 40.0f, KX_FontObject, m_fsize),
	KX_PYATTRIBUTE_FLOAT_RW("resolution", 0.1f, 50.0f, KX_FontObject, m_resolution),
	/* KX_PYATTRIBUTE_INT_RW("dpi", 0, 10000, false, KX_FontObject, m_dpi), */// no real need for expose this I think
	{NULL}    //Sentinel
};

PyObject *KX_FontObject::pyattr_get_text(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_FontObject *self = static_cast<KX_FontObject *>(self_v);
	STR_String str = STR_String();
	for (unsigned int i = 0; i < self->m_text.size(); ++i) {
		if (i != 0)
			str += '\n';
		str += self->m_text[i];
	}
	return PyUnicode_From_STR_String(str);
}

int KX_FontObject::pyattr_set_text(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_FontObject *self = static_cast<KX_FontObject *>(self_v);
	if (!PyUnicode_Check(value))
		return PY_SET_ATTR_FAIL;
	char *chars = _PyUnicode_AsString(value);

	/* Allow for some logic brick control */
	CValue *tprop = self->GetProperty("Text");
	if (tprop) {
		CValue *newstringprop = new CStringValue(STR_String(chars), "Text");
		self->SetProperty("Text", newstringprop);
		newstringprop->Release();
	}
	else {
		self->m_text = split_string(STR_String(chars));
	}

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_FontObject::pyattr_get_dimensions(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_FontObject *self = static_cast<KX_FontObject *>(self_v);
	return PyObjectFrom(self->GetTextDimensions());
}

#endif // WITH_PYTHON
