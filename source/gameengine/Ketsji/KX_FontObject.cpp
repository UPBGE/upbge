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
#include "KX_PyMath.h"
#include "BLI_math.h"
#include "EXP_StringValue.h"
#include "RAS_Rasterizer.h"
#include "RAS_BucketManager.h"
#include "RAS_MaterialBucket.h"
#include "RAS_BoundingBox.h"
#include "RAS_TextUser.h"

/* paths needed for font load */
#include "BLI_blenlib.h"
#include "BKE_global.h"
#include "BKE_font.h"
#include "BKE_main.h"
#include "DNA_packedFile_types.h"

extern "C" {
#include "BLF_api.h"
#include "depsgraph/DEG_depsgraph.h"
#include "MEM_guardedalloc.h"
}

#include "CM_Message.h"

#define BGE_FONT_RES 100

#define MAX_BGE_TEXT_LEN 1024 //eevee

/* proptotype */
int GetFontId(VFont *font);

static std::vector<std::string> split_string(std::string str)
{
	std::vector<std::string> text = std::vector<std::string>();

	// Split the string upon new lines
	int begin = 0, end = 0;
	while (end < str.size()) {
		if (str[end] == '\n') {
			text.push_back(str.substr(begin, end - begin));
			begin = end + 1;
		}
		end++;
	}
	// Now grab the last line
	text.push_back(str.substr(begin, end - begin));

	return text;
}

KX_FontObject::KX_FontObject(void *sgReplicationInfo,
                             SG_Callbacks callbacks,
                             RAS_Rasterizer *rasterizer,
							 RAS_BoundingBoxManager *boundingBoxManager,
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
	m_fsize = text->fsize;
	m_line_spacing = text->linedist;
	m_offset = MT_Vector3(text->xof, text->yof, 0.0f);

	m_fontid = GetFontId(text->vfont);

	m_boundingBox = new RAS_BoundingBox(boundingBoxManager);

	SetText(text->str);

	m_backupText = std::string(text->str); //eevee
}

KX_FontObject::~KX_FontObject()
{
	//remove font from the scene list
	//it's handled in KX_Scene::NewRemoveObject
	UpdateCurveText(m_backupText); //eevee
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

	m_boundingBox = m_boundingBox->GetReplica();
}

void KX_FontObject::AddMeshUser()
{
	m_meshUser = new RAS_TextUser(m_pClient_info, m_boundingBox);

	NodeGetWorldTransform().getValue(m_meshUser->GetMatrix());

	RAS_BucketManager *bucketManager = GetScene()->GetBucketManager();
	RAS_DisplayArrayBucket *arrayBucket = bucketManager->GetTextDisplayArrayBucket();

	RAS_MeshSlot *ms = new RAS_MeshSlot(nullptr, m_meshUser, arrayBucket);
	m_meshUser->AddMeshSlot(ms);
}

void KX_FontObject::UpdateBuckets()
{
	// Update datas and add mesh slot to be rendered only if the object is not culled.
	if (m_pSGNode->IsDirty(SG_Node::DIRTY_RENDER)) {
		NodeGetWorldTransform().getValue(m_meshUser->GetMatrix());
		m_pSGNode->ClearDirty(SG_Node::DIRTY_RENDER);
	}

	// Font Objects don't use the glsl shader, this color management code is copied from gpu_shader_material.glsl
	float color[4];
	if (m_do_color_management) {
		linearrgb_to_srgb_v4(color, m_objectColor.getValue());
	}
	else {
		m_objectColor.getValue(color);
	}

	// HARDCODED MULTIPLICATION FACTOR - this will affect the render resolution directly
	const float RES = BGE_FONT_RES * m_resolution;

	const float size = fabs(m_fsize * NodeGetWorldScaling()[0] * RES);
	const float aspect = m_fsize / size;

	// Account for offset
	MT_Vector3 offset = NodeGetWorldOrientation() * m_offset * NodeGetWorldScaling();
	// Orient the spacing vector
	MT_Vector3 spacing = NodeGetWorldOrientation() * MT_Vector3(0.0f, m_fsize * m_line_spacing, 0.0f) * NodeGetWorldScaling()[1];

	RAS_TextUser *textUser = (RAS_TextUser *)m_meshUser;

	textUser->SetColor(MT_Vector4(color));
	textUser->SetFrontFace(!m_bIsNegativeScaling);
	textUser->SetFontId(m_fontid);
	textUser->SetSize(size);
	textUser->SetDpi(m_dpi);
	textUser->SetAspect(aspect);
	textUser->SetOffset(offset);
	textUser->SetSpacing(spacing);
	textUser->SetTexts(m_texts);
	textUser->ActivateMeshSlots();
}

void KX_FontObject::SetText(const std::string& text)
{
	m_text = text;
	m_texts = split_string(text);

	MT_Vector2 min;
	MT_Vector2 max;
	GetTextAabb(min, max);
	m_boundingBox->SetAabb(MT_Vector3(min.x(), min.y(), 0.0f), MT_Vector3(max.x(), max.y(), 0.0f));
}

void KX_FontObject::UpdateCurveText(std::string newText) //eevee
{
	Object *ob = GetBlenderObject();
	Curve *cu = (Curve *)ob->data;
	if (cu->str) MEM_freeN(cu->str);
	if (cu->strinfo) MEM_freeN(cu->strinfo);

	cu->len_wchar = strlen(newText.c_str());
	cu->len = BLI_strlen_utf8(newText.c_str());
	cu->strinfo = (CharInfo *)MEM_callocN((cu->len_wchar + 4) * sizeof(CharInfo), "texteditinfo");
	cu->str = (char *)MEM_mallocN(cu->len + sizeof(wchar_t), "str");
	BLI_strncpy(cu->str, newText.c_str(), MAX_BGE_TEXT_LEN);

	DEG_id_tag_update(&ob->id, OB_RECALC_DATA);
	DEG_id_tag_update(&ob->id, DEG_TAG_COPY_ON_WRITE);

	GetScene()->ResetTaaSamples();
}

void KX_FontObject::UpdateTextFromProperty()
{
	// Allow for some logic brick control
	CValue *prop = GetProperty("Text");
	if (prop && prop->GetText() != m_text) {
		SetText(prop->GetText());
		UpdateCurveText(m_text); //eevee
	}
}

const MT_Vector2 KX_FontObject::GetTextDimensions()
{
	MT_Vector2 min;
	MT_Vector2 max;
	GetTextAabb(min, max);

	// Scale the width and height by the object's scale
	const MT_Vector3& scale = NodeGetLocalScaling();

	return MT_Vector2((max.x() - min.x()) * fabs(scale.x()), (max.y() - min.y()) * fabs(scale.y()));
}

void KX_FontObject::GetTextAabb(MT_Vector2& min, MT_Vector2& max)
{
	const float RES = BGE_FONT_RES * m_resolution;

	const float size = m_fsize * RES;
	const float aspect = m_fsize / size;
	const float lineSpacing = m_line_spacing / aspect;

	BLF_size(m_fontid, size, m_dpi);

	for (unsigned short i = 0, size = m_texts.size(); i < size; ++i) {
		rctf box;
		const std::string& text = m_texts[i];
		BLF_boundbox(m_fontid, text.c_str(), text.size(), &box);
		if (i == 0) {
			min.x() = box.xmin;
			min.y() = box.ymin;
			max.x() = box.xmax;
			max.y() = box.ymax;
		}
		else {
			min.x() = std::min(min.x(), box.xmin);
			min.y() = std::min(min.y(), box.ymin - lineSpacing * i);
			max.x() = std::max(max.x(), box.xmax);
			max.y() = std::max(max.y(), box.ymax - lineSpacing * i);
		}
	}

	min *= aspect;
	max *= aspect;
}

int GetFontId(VFont *vfont)
{
	PackedFile *packedfile = nullptr;
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
		 * unfortunately it's crashing on blf_glyph.c:173 because gc->glyph_width_max is 0
		 */
		// packedfile=get_builtin_packedfile();
		// fontid= BLF_load_mem(font->name, (unsigned char*)packedfile->data, packedfile->size);
		// return fontid;

		return BLF_load("default");
	}

	// convert from absolute to relative
	char expanded[FILE_MAX];
	BLI_strncpy(expanded, filepath, FILE_MAX);
	BLI_path_abs(expanded, vfont->id.lib ? vfont->id.lib->name : KX_GetMainPath().c_str());

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
	PyVarObject_HEAD_INIT(nullptr, 0)
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
	nullptr,
	nullptr,
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
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_FontObject::Attributes[] = {
	KX_PYATTRIBUTE_RW_FUNCTION("text", KX_FontObject, pyattr_get_text, pyattr_set_text),
	KX_PYATTRIBUTE_RO_FUNCTION("dimensions", KX_FontObject, pyattr_get_dimensions),
	KX_PYATTRIBUTE_FLOAT_RW("size", 0.0001f, 40.0f, KX_FontObject, m_fsize),
	KX_PYATTRIBUTE_FLOAT_RW("resolution", 0.1f, 50.0f, KX_FontObject, m_resolution),
	/* KX_PYATTRIBUTE_INT_RW("dpi", 0, 10000, false, KX_FontObject, m_dpi), */// no real need for expose this I think
	KX_PYATTRIBUTE_NULL    //Sentinel
};

PyObject *KX_FontObject::pyattr_get_text(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_FontObject *self = static_cast<KX_FontObject *>(self_v);
	return PyUnicode_FromStdString(self->m_text);
}

int KX_FontObject::pyattr_set_text(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_FontObject *self = static_cast<KX_FontObject *>(self_v);
	if (!PyUnicode_Check(value))
		return PY_SET_ATTR_FAIL;
	const char *chars = _PyUnicode_AsString(value);

	/* Allow for some logic brick control */
	CValue *tprop = self->GetProperty("Text");
	if (tprop) {
		CValue *newstringprop = new CStringValue(std::string(chars), "Text");
		self->SetProperty("Text", newstringprop);
		newstringprop->Release();
	}
	else {
		self->SetText(std::string(chars));
	}

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_FontObject::pyattr_get_dimensions(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_FontObject *self = static_cast<KX_FontObject *>(self_v);
	return PyObjectFrom(self->GetTextDimensions());
}

#endif // WITH_PYTHON
