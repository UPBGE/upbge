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

/** \file gameengine/Ketsji/RAS_Shader.cpp
 *  \ingroup bgerast
 */

#include <iostream>
#include <cstring>
#include "RAS_Shader.h"
#include "RAS_IRasterizer.h"

#include "BLI_utildefines.h"
#include "MEM_guardedalloc.h"

#include "GPU_shader.h"

#define spit(x) std::cout << x << std::endl;

#define UNIFORM_MAX_LEN (int)sizeof(float) * 16

RAS_Shader::RAS_Uniform::RAS_Uniform(int data_size)
	:mLoc(-1),
	m_count(1),
	mDirty(true),
	mType(UNI_NONE),
	mTranspose(0),
	mDataLen(data_size)
{
#ifdef SORT_UNIFORMS
	BLI_assert((int)mDataLen <= UNIFORM_MAX_LEN);
	mData = (void *)MEM_mallocN(mDataLen, "shader-uniform-alloc");
#endif
}

RAS_Shader::RAS_Uniform::~RAS_Uniform()
{
#ifdef SORT_UNIFORMS
	if (mData) {
		MEM_freeN(mData);
		mData = NULL;
	}
#endif
}

void RAS_Shader::RAS_Uniform::Apply(RAS_Shader *shader)
{
#ifdef SORT_UNIFORMS
	BLI_assert(mType > UNI_NONE && mType < UNI_MAX && mData);

	if (!mDirty) {
		return;
	}

	GPUShader *gpushader = shader->GetGPUShader();
	switch (mType) {
		case UNI_FLOAT:
		{
			float *f = (float *)mData;
			GPU_shader_uniform_vector(gpushader, mLoc, 1, m_count, (float *)f);
			break;
		}
		case UNI_INT:
		{
			int *f = (int *)mData;
			GPU_shader_uniform_vector_int(gpushader, mLoc, 1, m_count, (int *)f);
			break;
		}
		case UNI_FLOAT2:
		{
			float *f = (float *)mData;
			GPU_shader_uniform_vector(gpushader, mLoc, 2, m_count, (float *)f);
			break;
		}
		case UNI_FLOAT3:
		{
			float *f = (float *)mData;
			GPU_shader_uniform_vector(gpushader, mLoc, 3, m_count, (float *)f);
			break;
		}
		case UNI_FLOAT4:
		{
			float *f = (float *)mData;
			GPU_shader_uniform_vector(gpushader, mLoc, 4, m_count, (float *)f);
			break;
		}
		case UNI_INT2:
		{
			int *f = (int *)mData;
			GPU_shader_uniform_vector_int(gpushader, mLoc, 2, m_count, (int *)f);
			break;
		}
		case UNI_INT3:
		{
			int *f = (int *)mData;
			GPU_shader_uniform_vector_int(gpushader, mLoc, 3, m_count, (int *)f);
			break;
		}
		case UNI_INT4:
		{
			int *f = (int *)mData;
			GPU_shader_uniform_vector_int(gpushader, mLoc, 4, m_count, (int *)f);
			break;
		}
		case UNI_MAT4:
		{
			float *f = (float *)mData;
			GPU_shader_uniform_vector(gpushader, mLoc, 16, m_count, (float *)f);
			break;
		}
		case UNI_MAT3:
		{
			float *f = (float *)mData;
			GPU_shader_uniform_vector(gpushader, mLoc, 9, m_count, (float *)f);
			break;
		}
	}
	mDirty = false;
#endif
}

void RAS_Shader::RAS_Uniform::SetData(int location, int type, unsigned int count, bool transpose)
{
#ifdef SORT_UNIFORMS
	mType = type;
	mLoc = location;
	m_count = count;
	mDirty = true;
#endif
}

int RAS_Shader::RAS_Uniform::GetLocation()
{
	return mLoc;
}

void *RAS_Shader::RAS_Uniform::getData()
{
	return mData;
}

bool RAS_Shader::Ok()const
{
	return (m_shader != 0 && mOk && mUse);
}

RAS_Shader::RAS_Shader()
	:m_shader(NULL),
	mOk(0),
	mUse(0),
	mAttr(0),
	m_vertProg(""),
	m_fragProg(""),
	mError(0),
	mDirty(true)
{
}

RAS_Shader::~RAS_Shader()
{
	ClearUniforms();

	DeleteShader();
}

void RAS_Shader::ClearUniforms()
{
	RAS_UniformVec::iterator it = mUniforms.begin();
	while (it != mUniforms.end()) {
		delete *it;
		it++;
	}
	mUniforms.clear();

	RAS_UniformVecDef::iterator itp = mPreDef.begin();
	while (itp != mPreDef.end()) {
		delete *itp;
		itp++;
	}
	mPreDef.clear();
}

RAS_Shader::RAS_Uniform *RAS_Shader::FindUniform(const int location)
{
#ifdef SORT_UNIFORMS
	RAS_UniformVec::iterator it = mUniforms.begin();
	while (it != mUniforms.end()) {
		if ((*it)->GetLocation() == location) {
			return *it;
		}
		it++;
	}
#endif
	return NULL;
}

void RAS_Shader::SetUniformfv(int location, int type, float *param, int size, unsigned int count, bool transpose)
{
#ifdef SORT_UNIFORMS
	RAS_Uniform *uni = FindUniform(location);

	if (uni) {
		memcpy(uni->getData(), param, size);
		uni->SetData(location, type, count, transpose);
	}
	else {
		uni = new RAS_Uniform(size);
		memcpy(uni->getData(), param, size);
		uni->SetData(location, type, count, transpose);
		mUniforms.push_back(uni);
	}

	mDirty = true;
#endif
}

void RAS_Shader::SetUniformiv(int location, int type, int *param, int size, unsigned int count, bool transpose)
{
#ifdef SORT_UNIFORMS
	RAS_Uniform *uni = FindUniform(location);

	if (uni) {
		memcpy(uni->getData(), param, size);
		uni->SetData(location, type, count, transpose);
	}
	else {
		uni = new RAS_Uniform(size);
		memcpy(uni->getData(), param, size);
		uni->SetData(location, type, count, transpose);
		mUniforms.push_back(uni);
	}

	mDirty = true;
#endif
}

void RAS_Shader::ApplyShader()
{
#ifdef SORT_UNIFORMS
	if (!mDirty) {
		return;
	}

	for (unsigned int i = 0; i < mUniforms.size(); i++) {
		mUniforms[i]->Apply(this);
	}

	mDirty = false;
#endif
}

void RAS_Shader::UnloadShader()
{
	//
}

void RAS_Shader::DeleteShader()
{
	if (m_shader) {
		GPU_shader_free(m_shader);
		m_shader = NULL;
		mOk = false;
	}
}

bool RAS_Shader::LinkProgram()
{
	if (mError) {
		goto programError;
	}

	if (m_vertProg.IsEmpty() || m_fragProg.IsEmpty()) {
		spit("Invalid GLSL sources");
		return false;
	}

	m_shader = GPU_shader_create_ex(m_vertProg.ReadPtr(), m_fragProg.ReadPtr(), NULL, NULL, NULL, 0, 0, 0, GPU_SHADER_FLAGS_SPECIAL_RESET_LINE);
	if (!m_shader) {
		goto programError;
	}

	mOk = 1;
	mError = 0;
	return true;

programError:
	mOk = 0;
	mUse = 0;
	mError = 1;
	return false;
}

void RAS_Shader::ValidateProgram()
{
	char *log = GPU_shader_validate(m_shader);
	if (log) {
		spit("---- GLSL Validation ----");
		spit(log);
		MEM_freeN(log);
	}
}

bool RAS_Shader::GetError()
{
	return mError;
}

unsigned int RAS_Shader::GetProg()
{
	return GPU_shader_program(m_shader);
}

GPUShader *RAS_Shader::GetGPUShader()
{
	return m_shader;
}

void RAS_Shader::SetSampler(int loc, int unit)
{
	GPU_shader_uniform_int(m_shader, loc, unit);
}

void RAS_Shader::SetProg(bool enable)
{
	if (m_shader && mOk && enable) {
		GPU_shader_bind(m_shader);
	}
	else {
		GPU_shader_unbind();
	}
}

void RAS_Shader::SetEnabled(bool enabled)
{
	mUse = enabled;
}

bool RAS_Shader::GetEnabled() const
{
	return mUse;
}

void RAS_Shader::Update(RAS_IRasterizer *rasty, const MT_Matrix4x4 model)
{
	if (!Ok() || !mPreDef.size()) {
		return;
	}

	const MT_Matrix4x4 &view = rasty->GetViewMatrix();

	RAS_UniformVecDef::iterator it;
	for (it = mPreDef.begin(); it != mPreDef.end(); it++) {
		RAS_DefUniform *uni = (*it);

		if (uni->mLoc == -1) {
			continue;
		}

		switch (uni->mType) {
			case MODELMATRIX:
			{
				SetUniform(uni->mLoc, model);
				break;
			}
			case MODELMATRIX_TRANSPOSE:
			{
				SetUniform(uni->mLoc, model, true);
				break;
			}
			case MODELMATRIX_INVERSE:
			{
				SetUniform(uni->mLoc, model.inverse());
				break;
			}
			case MODELMATRIX_INVERSETRANSPOSE:
			{
				SetUniform(uni->mLoc, model.inverse(), true);
				break;
			}
			case MODELVIEWMATRIX:
			{
				SetUniform(uni->mLoc, view * model);
				break;
			}
			case MODELVIEWMATRIX_TRANSPOSE:
			{
				MT_Matrix4x4 mat(view * model);
				SetUniform(uni->mLoc, mat, true);
				break;
			}
			case MODELVIEWMATRIX_INVERSE:
			{
				MT_Matrix4x4 mat(view * model);
				SetUniform(uni->mLoc, mat.inverse());
				break;
			}
			case MODELVIEWMATRIX_INVERSETRANSPOSE:
			{
				MT_Matrix4x4 mat(view * model);
				SetUniform(uni->mLoc, mat.inverse(), true);
				break;
			}
			case CAM_POS:
			{
				MT_Vector3 pos(rasty->GetCameraPosition());
				SetUniform(uni->mLoc, pos);
				break;
			}
			case VIEWMATRIX:
			{
				SetUniform(uni->mLoc, view);
				break;
			}
			case VIEWMATRIX_TRANSPOSE:
			{
				SetUniform(uni->mLoc, view, true);
				break;
			}
			case VIEWMATRIX_INVERSE:
			{
				SetUniform(uni->mLoc, view.inverse());
				break;
			}
			case VIEWMATRIX_INVERSETRANSPOSE:
			{
				SetUniform(uni->mLoc, view.inverse(), true);
				break;
			}
			case CONSTANT_TIMER:
			{
				SetUniform(uni->mLoc, (float)rasty->GetTime());
				break;
			}
			case EYE:
			{
				SetUniform(uni->mLoc, (rasty->GetEye() == RAS_IRasterizer::RAS_STEREO_LEFTEYE) ? 0.0f : 0.5f);
			}
			default:
				break;
		}
	}
}

int RAS_Shader::GetAttribute()
{
	return mAttr;
}

int RAS_Shader::GetAttribLocation(const char *name)
{
	return GPU_shader_get_attribute(m_shader, name);
}

void RAS_Shader::BindAttribute(const char *attr, int loc)
{
	GPU_shader_bind_attribute(m_shader, loc, attr);
}

int RAS_Shader::GetUniformLocation(const char *name, bool debug)
{
	BLI_assert(m_shader != NULL);
	int location = GPU_shader_get_uniform(m_shader, name);

	if (location == -1 && debug) {
		spit("Invalid uniform value: " << name << ".");
	}

	return location;
}

void RAS_Shader::SetUniform(int uniform, const MT_Vector2 &vec)
{
	float value[2];
	vec.getValue(value);
	GPU_shader_uniform_vector(m_shader, uniform, 2, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const MT_Vector3 &vec)
{
	float value[3];
	vec.getValue(value);
	GPU_shader_uniform_vector(m_shader, uniform, 3, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const MT_Vector4 &vec)
{
	float value[4];
	vec.getValue(value);
	GPU_shader_uniform_vector(m_shader, uniform, 4, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const unsigned int &val)
{
	GPU_shader_uniform_int(m_shader, uniform, val);
}

void RAS_Shader::SetUniform(int uniform, const int val)
{
	GPU_shader_uniform_int(m_shader, uniform, val);
}

void RAS_Shader::SetUniform(int uniform, const float &val)
{
	GPU_shader_uniform_float(m_shader, uniform, val);
}

void RAS_Shader::SetUniform(int uniform, const MT_Matrix4x4 &vec, bool transpose)
{
	float value[16];
	// note: getValue gives back column major as needed by OpenGL
	vec.getValue(value);
	GPU_shader_uniform_vector(m_shader, uniform, 16, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const MT_Matrix3x3 &vec, bool transpose)
{
	float value[9];
	value[0] = (float)vec[0][0];
	value[1] = (float)vec[1][0];
	value[2] = (float)vec[2][0];
	value[3] = (float)vec[0][1];
	value[4] = (float)vec[1][1];
	value[5] = (float)vec[2][1];
	value[6] = (float)vec[0][2];
	value[7] = (float)vec[1][2];
	value[8] = (float)vec[2][2];
	GPU_shader_uniform_vector(m_shader, uniform, 9, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const float *val, int len)
{
	if (len >= 2 && len <= 4) {
		GPU_shader_uniform_vector(m_shader, uniform, len, 1, (float *)val);
	}
	else {
		BLI_assert(0);
	}
}

void RAS_Shader::SetUniform(int uniform, const int *val, int len)
{
	if (len >= 2 && len <= 4) {
		GPU_shader_uniform_vector_int(m_shader, uniform, len, 1, (int *)val);
	}
	else {
		BLI_assert(0);
	}
}
