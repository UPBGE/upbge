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
}

#include "CM_Message.h"

#define BGE_FONT_RES 100

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
                             Object *ob)
	:KX_GameObject(sgReplicationInfo, callbacks),
	m_object(ob),
	m_dpi(72),
	m_resolution(1.0f),
	m_rasterizer(rasterizer)
{
	Curve *text = static_cast<Curve *> (ob->data);
	m_fsize = text->fsize;
	m_line_spacing = text->linedist;
	m_offset = mt::vec3(text->xof, text->yof, 0.0f);

	m_fontid = GetFontId(text->vfont);

	m_boundingBox = new RAS_BoundingBox(boundingBoxManager);

	SetText(text->str);
}

KX_FontObject::~KX_FontObject()
{
	//remove font from the scene list
	//it's handled in KX_Scene::NewRemoveObject
}

EXP_Value *KX_FontObject::GetReplica()
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
	m_defaultMeshUser.reset(new RAS_TextUser(&m_clientInfo, m_boundingBox));

	RAS_BucketManager *bucketManager = GetScene()->GetBucketManager();
	RAS_DisplayArrayBucket *arrayBucket = bucketManager->GetTextDisplayArrayBucket();

	m_defaultMeshUser->NewMeshSlot(arrayBucket);

	SetCurrentMeshUser(m_defaultMeshUser.get());
}

void KX_FontObject::UpdateBuckets()
{
	RAS_TextUser *textUser = static_cast<RAS_TextUser *>(m_currentMeshUser);

	// Update datas and add mesh slot to be rendered only if the object is not culled.
	if (m_sgNode->IsDirty(SG_Node::DIRTY_RENDER)) {
		m_defaultMeshUser->SetMatrix(mt::mat4::FromAffineTransform(NodeGetWorldTransform()));
		m_defaultMeshUser->SetFrontFace(!IsNegativeScaling());
		m_sgNode->ClearDirty(SG_Node::DIRTY_RENDER);
	}

	// HARDCODED MULTIPLICATION FACTOR - this will affect the render resolution directly
	const float RES = BGE_FONT_RES * m_resolution;

	const float size = fabs(m_fsize * NodeGetWorldScaling()[0] * RES);
	const float aspect = m_fsize / size;

	// Account for offset
	mt::vec3 offset = NodeGetWorldOrientation() * m_offset * NodeGetWorldScaling();
	// Orient the spacing vector
	mt::vec3 spacing = NodeGetWorldOrientation() * mt::vec3(0.0f, m_fsize * m_line_spacing, 0.0f) * NodeGetWorldScaling()[1];

	textUser->SetLayer(m_layer);
	textUser->SetColor(m_objectColor);
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

	mt::vec2 min;
	mt::vec2 max;
	GetTextAabb(min, max);
	m_boundingBox->SetAabb(mt::vec3(min.x, min.y, 0.0f), mt::vec3(max.x, max.y, 0.0f));
}

void KX_FontObject::UpdateTextFromProperty()
{
	// Allow for some logic brick control
	EXP_Value *prop = GetProperty("Text");
	if (prop && prop->GetText() != m_text) {
		SetText(prop->GetText());
	}
}

const mt::vec2 KX_FontObject::GetTextDimensions()
{
	mt::vec2 min;
	mt::vec2 max;
	GetTextAabb(min, max);

	// Scale the width and height by the object's scale
	const mt::vec3& scale = NodeGetLocalScaling();

	return mt::vec2((max.x - min.x) * fabs(scale.x), (max.y - min.y) * fabs(scale.y));
}

void KX_FontObject::GetTextAabb(mt::vec2& min, mt::vec2& max)
{
	const float RES = BGE_FONT_RES * m_resolution;

	const float size = m_fsize * RES;
	const float aspect = m_fsize / size;
	const float lineSpacing = m_line_spacing / aspect;

	BLF_size(m_fontid, size, m_dpi);

	min = mt::vec2(FLT_MAX);
	max = mt::vec2(-FLT_MAX);

	for (unsigned short i = 0, count = m_texts.size(); i < count; ++i) {
		rctf box;
		const std::string& text = m_texts[i];
		BLF_boundbox(m_fontid, text.c_str(), text.size(), &box);
		min.x = std::min(min.x, box.xmin);
		min.y = std::min(min.y, box.ymin - lineSpacing * i);
		max.x = std::max(max.x, box.xmax);
		max.y = std::max(max.y, box.ymax - lineSpacing * i);
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
	if (BKE_vfont_is_builtin(vfont)) {
		fontid = BLF_load("default");
		return fontid;
	}

	// convert from relative to absolute
	char expanded[FILE_MAX];
	BLI_strncpy(expanded, vfont->name, FILE_MAX);

	char libpath[FILE_MAX];
	// Use library path if available and ensure it is absolute.
	if (vfont->id.lib) {
		BLI_strncpy(libpath, vfont->id.lib->name, FILE_MAX);
		BLI_path_abs(libpath, KX_GetMainPath().c_str());
	}
	else {
		BLI_strncpy(libpath, KX_GetMainPath().c_str(), FILE_MAX);
	}
	BLI_path_abs(expanded, libpath);

	fontid = BLF_load(expanded);

	// fallback
	if (fontid == -1) {
		fontid = BLF_load("default");
		CM_Warning("failed loading font \"" << vfont->name << "\"");
	}

	return fontid;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python Integration Hooks					                                 */
/* ------------------------------------------------------------------------- */

PyTypeObject KX_FontObject::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_FontObject",
	sizeof(EXP_PyObjectPlus_Proxy),
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
	EXP_PYATTRIBUTE_RW_FUNCTION("text", KX_FontObject, pyattr_get_text, pyattr_set_text),
	EXP_PYATTRIBUTE_RO_FUNCTION("dimensions", KX_FontObject, pyattr_get_dimensions),
	EXP_PYATTRIBUTE_FLOAT_RW("size", 0.0001f, 40.0f, KX_FontObject, m_fsize),
	EXP_PYATTRIBUTE_FLOAT_RW("resolution", 0.1f, 50.0f, KX_FontObject, m_resolution),
	/* EXP_PYATTRIBUTE_INT_RW("dpi", 0, 10000, false, KX_FontObject, m_dpi), */// no real need for expose this I think
	EXP_PYATTRIBUTE_NULL    //Sentinel
};

PyObject *KX_FontObject::pyattr_get_text(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_FontObject *self = static_cast<KX_FontObject *>(self_v);
	return PyUnicode_FromStdString(self->m_text);
}

int KX_FontObject::pyattr_set_text(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_FontObject *self = static_cast<KX_FontObject *>(self_v);
	if (!PyUnicode_Check(value)) {
		return PY_SET_ATTR_FAIL;
	}
	const char *chars = _PyUnicode_AsString(value);

	/* Allow for some logic brick control */
	EXP_Value *tprop = self->GetProperty("Text");
	if (tprop) {
		EXP_Value *newstringprop = new EXP_StringValue(std::string(chars), "Text");
		self->SetProperty("Text", newstringprop);
		newstringprop->Release();
	}
	else {
		self->SetText(std::string(chars));
	}

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_FontObject::pyattr_get_dimensions(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_FontObject *self = static_cast<KX_FontObject *>(self_v);
	return PyObjectFrom(self->GetTextDimensions());
}

#endif // WITH_PYTHON
