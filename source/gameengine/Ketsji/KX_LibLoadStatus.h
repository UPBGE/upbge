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
 * Contributor(s): Mitchell Stokes
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_LibLoadStatus.h
 *  \ingroup bgeconv
 */

#ifndef __KX_LIBLOADSTATUS_H__
#define __KX_LIBLOADSTATUS_H__

#include "EXP_PyObjectPlus.h"
#include "BL_SceneConverter.h"

class BL_Converter;
class KX_KetsjiEngine;
class KX_Scene;

class KX_LibLoadStatus : public EXP_PyObjectPlus
{
	Py_Header
private:
	BL_Converter *m_converter;
	KX_KetsjiEngine *m_engine;
	KX_Scene *m_mergescene;
	std::vector<KX_Scene *> m_scenes;
	std::vector<BL_SceneConverter> m_sceneConvertes;
	std::string m_libname;

	float m_progress;
	double m_starttime;
	double m_endtime;

	/// The current status of this libload, used by the scene converter.
	bool m_finished;

#ifdef WITH_PYTHON
	PyObject *m_finish_cb;
	PyObject *m_progress_cb;
#endif

public:
	KX_LibLoadStatus(BL_Converter *converter, KX_KetsjiEngine *engine, KX_Scene *merge_scene, const std::string& path);

	/// Called when the libload is done.
	void Finish();
	void RunFinishCallback();
	void RunProgressCallback();

	BL_Converter *GetConverter() const;
	KX_KetsjiEngine *GetEngine() const;
	KX_Scene *GetMergeScene() const;

	const std::vector<KX_Scene *>& GetScenes() const;
	void SetScenes(const std::vector<KX_Scene *>& scenes);
	const std::vector<BL_SceneConverter>& GetSceneConverters() const;
	void AddSceneConverter(BL_SceneConverter&& converter);

	bool IsFinished() const;

	void SetProgress(float progress);
	float GetProgress() const;
	void AddProgress(float progress);

#ifdef WITH_PYTHON
	static PyObject *pyattr_get_onfinish(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_onfinish(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_onprogress(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_onprogress(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	static PyObject *pyattr_get_timetaken(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
#endif
};

#endif  // __KX_LIBLOADSTATUS_H__
