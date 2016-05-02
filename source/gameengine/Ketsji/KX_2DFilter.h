//This class is a subclass of RAS_2DFilter, it contains only the pytohn proxy and it is created only in the KX_2DFilterManager class which is also a subclass of RAS_2DFilterManager.
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

/** \file KX_2DFilter.h
*  \ingroup ketsji
*/

#ifndef __KX_2DFILTER_H__
#define __KX_2DFILTER_H__

#include "EXP_Value.h"
#include "RAS_2DFilter.h"
#include "SCA_IObject.h"
#include "BL_Texture.h"
#include "MT_Matrix4x4.h"
#include "MT_Matrix3x3.h"
#include "MT_Vector2.h"
#include "MT_Vector3.h"
#include "MT_Vector4.h"

class KX_2DFilter : public RAS_2DFilter, CValue
{
	Py_Header
private:
	STR_String m_name;

public:
	KX_2DFilter(RAS_2DFilterData& data, RAS_2DFilterManager *manager);
	virtual ~KX_2DFilter();

	// stuff for cvalue related things
	virtual CValue *Calc(VALUE_OPERATOR op, CValue *val);
	virtual CValue *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val);
	virtual const STR_String& GetText();
	virtual double GetNumber();
	virtual STR_String& GetName();
	virtual void SetName(const char *name); // Set the name of the value
	virtual CValue *GetReplica();

	void SetSampler(int loc, int unit);
	int GetUniformLocation(const char *name);
	void SetUniform(int uniform, const MT_Vector2 &vec);
	void SetUniform(int uniform, const MT_Vector3 &vec);
	void SetUniform(int uniform, const MT_Vector4 &vec);
	void SetUniform(int uniform, const MT_Matrix4x4 &vec, bool transpose = false);
	void SetUniform(int uniform, const MT_Matrix3x3 &vec, bool transpose = false);
	void SetUniform(int uniform, const float &val);
	void SetUniform(int uniform, const float *val, int len);
	void SetUniform(int uniform, const int *val, int len);
	void SetUniform(int uniform, const unsigned int &val);
	void SetUniform(int uniform, const int val);

	// -----------------------------------
	KX_PYMETHOD_DOC(KX_2DFilter, setUniform4f);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniform3f);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniform2f);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniform1f);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniform4i);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniform3i);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniform2i);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniform1i);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniformfv);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniformiv);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniformMatrix4);
	KX_PYMETHOD_DOC(KX_2DFilter, setUniformMatrix3);
	KX_PYMETHOD_DOC(KX_2DFilter, setSampler);

};

#endif  // __KX_2DFILTER_H__
