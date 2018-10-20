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

#include <functional>

class BL_Converter;
class KX_KetsjiEngine;
class KX_Scene;

class KX_LibLoadStatus : public EXP_PyObjectPlus
{
	Py_Header
public:
	using ConvertFunction = std::function<void (BL_Converter&, BL_SceneConverter&)>;

private:
	KX_Scene *m_mergeScene;
	std::vector<BL_SceneConverter> m_sceneConverters;
	std::string m_libname;
	ConvertFunction m_convertFunction;

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
	KX_LibLoadStatus(const std::vector<KX_Scene *>& scenes, KX_Scene *mergeScene, const ConvertFunction& function,
			const BL_Resource::Library& libraryId, const std::string& path);

	/// Called when the libload is done.
	void Finish();
	void RunFinishCallback();
	void RunProgressCallback();

	KX_Scene *GetMergeScene() const;
	std::vector<BL_SceneConverter>& GetSceneConverters();
	const ConvertFunction& GetConvertFunction() const;

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
