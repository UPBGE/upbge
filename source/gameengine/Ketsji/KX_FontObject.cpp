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

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BLI_string_utf8.h"
#include "DNA_curve_types.h"

#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"

using namespace blender;

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

KX_FontObject::KX_FontObject() : KX_GameObject(), m_object(nullptr), m_rasterizer(nullptr)
{
}

KX_FontObject::~KX_FontObject()
{
  // remove font from the scene list
  // it's handled in KX_Scene::NewRemoveObject
  if (m_ownCurve != nullptr) {
    /* A copy's own text data goes with it; its object is deleted next. */
    if (blender::Object *ob = GetBlenderObject()) {
      ob->data = &m_sharedCurve->id;
      id_us_plus(&m_sharedCurve->id);
    }
    BKE_id_delete(CTX_data_main(KX_GetActiveEngine()->GetContext()), m_ownCurve);
    return;
  }
  UpdateCurveText(m_backupText);  // eevee
}

KX_PythonProxy *KX_FontObject::NewInstance()
{
  return new KX_FontObject(*this);
}

void KX_FontObject::ProcessReplica()
{
  KX_GameObject::ProcessReplica();
  /* A copy can be shown for another player (a menu): filter its text again. */
  m_filterRevision = UINT64_MAX;
  /* A copy shares its source's text data, so a new text would show on every copy: it gets its own
   * (a full copy already has its own). A copy of a copy gives back the authored text data, which
   * lives for the whole game, not its source's own (deleted with the source). A dupli instance
   * draws its source's object itself, so it keeps that object's data. */
  blender::Curve *authored = m_sharedCurve;
  m_ownCurve = nullptr;
  m_sharedCurve = nullptr;
  blender::Object *ob = GetBlenderObject();
  blender::Curve *curve = ob && (ob->gameflag & OB_DUPLI_UPBGE) == 0 ?
                              id_cast<blender::Curve *>(ob->data) :
                              nullptr;
  if (curve != nullptr && ID_REAL_USERS(&curve->id) > 1) {
    m_sharedCurve = authored != nullptr ? authored : curve;
    m_ownCurve = id_cast<blender::Curve *>(
        BKE_id_copy(CTX_data_main(KX_GetActiveEngine()->GetContext()), &curve->id));
    id_us_min(&curve->id);
    /* ReplicateBlenderObject already tagged the relations update this needs. */
    ob->data = &m_ownCurve->id;
  }
}

void KX_FontObject::SetText(const std::string &text)
{
  m_text = text;
  m_texts = split_string(text);
}

void KX_FontObject::UpdateCurveText(std::string newText)  // eevee
{
  blender::Object *ob = GetBlenderObject();
  blender::Curve *cu = (blender::Curve *)ob->data;
  if (cu->str)
    MEM_delete(cu->str);
  if (cu->strinfo)
    MEM_delete(cu->strinfo);

  size_t len_bytes;
  size_t len_chars = BLI_strlen_utf8_ex(newText.c_str(), &len_bytes);

  cu->len_char32 = len_chars;
  cu->len = len_bytes;
  cu->strinfo = static_cast<CharInfo *>(MEM_new_zeroed((len_chars + 1) * sizeof(CharInfo), __func__));
  cu->str = static_cast<char *>(MEM_new_uninitialized(len_bytes + sizeof(char32_t), __func__));
  memcpy(cu->str, newText.c_str(), len_bytes + 1);

  GetScene()->AppendToIdsToUpdate(
      &ob->id, ID_RECALC_GEOMETRY, ob->gameflag & OB_OVERLAY_COLLECTION);
}

KX_FontObject::TextFilter KX_FontObject::s_textFilter = nullptr;
uint64_t KX_FontObject::s_textFilterRevision = 0;

void KX_FontObject::SetTextFilter(const TextFilter filter, const uint64_t revision)
{
  s_textFilter = filter;
  s_textFilterRevision = revision;
}

void KX_FontObject::UpdateTextFromProperty()
{
  // Allow for some logic brick control
  EXP_Value *prop = GetProperty("Text");
  if (prop != nullptr) {
    std::string source = prop->GetText();
    if (s_textFilter == nullptr || source.find('{') == std::string::npos ||
        (m_hasLiteralText && source == m_literalText))
    {
      if (source != m_text) {
        SetText(source);
        UpdateCurveText(m_text);  // eevee
      }
      m_sourceText = std::move(source);
      m_filterLive = false;
      return;
    }
    if (source == m_sourceText && m_filterRevision == s_textFilterRevision && !m_filterLive) {
      return;
    }
    m_sourceText = std::move(source);
  }
  else if (s_textFilter == nullptr ||
           (m_filterRevision == s_textFilterRevision && !m_filterLive) ||
           m_sourceText.find('{') == std::string::npos)
  {
    return;
  }
  /* A text with braces, such as "{Grab} Move" or "Gold: {Player.gold}": shown through the
   * filter. One with values is filtered on every update, but only set when it changed. */
  m_filterRevision = s_textFilterRevision;
  bool live = false;
  const std::string shown = s_textFilter(*this, m_sourceText, live);
  m_filterLive = live;
  if (shown != m_text) {
    SetText(shown);
    UpdateCurveText(m_text);  // eevee
  }
}

void KX_FontObject::SetRasterizer(RAS_Rasterizer *rasterizer)
{
  m_rasterizer = rasterizer;
}

void KX_FontObject::SetBlenderObject(blender::Object *obj)
{
  KX_GameObject::SetBlenderObject(obj);

  if (obj) {
    blender::Curve *text = id_cast<blender::Curve *>(obj->data);

    m_backupText = std::string(text->str);  // eevee
    m_sourceText = m_backupText;

    SetText(text->str);
  }
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python Integration Hooks					                                 */
/* ------------------------------------------------------------------------- */
PyObject *KX_FontObject::game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  KX_FontObject *obj = new KX_FontObject();

  PyObject *proxy = py_base_new(type, PyTuple_Pack(1, obj->GetProxy()), kwds);
  if (!proxy) {
    delete obj;
    return nullptr;
  }

  return proxy;
}

PyTypeObject KX_FontObject::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_FontObject",
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
                                    0,
                                    0,
                                    0,
                                    nullptr,
                                    nullptr,
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
                                    &KX_GameObject::Type,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    game_object_new};

PyMethodDef KX_FontObject::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef KX_FontObject::Attributes[] = {
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON
