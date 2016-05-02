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
 *  \ingroup ketsji
 */

#include "glew-mx.h"

#include <iostream>
#include <cstring>
#include "RAS_Shader.h"
#include "RAS_IRasterizer.h"

#include "BLI_utildefines.h"
#include "MEM_guardedalloc.h"


#define spit(x) std::cout << x << std::endl;

#define UNIFORM_MAX_LEN (int)sizeof(float) * 16
#define MAX_LOG_LEN 262144 // bounds

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

	switch (mType) {
		case UNI_FLOAT:
		{
			float *f = (float *)mData;
			glUniform1fvARB(mLoc, m_count, (GLfloat *)f);
			break;
		}
		case UNI_INT:
		{
			int *f = (int *)mData;
			glUniform1ivARB(mLoc, m_count, (GLint *)f);
			break;
		}
		case UNI_FLOAT2:
		{
			float *f = (float *)mData;
			glUniform2fvARB(mLoc, m_count, (GLfloat *)f);
			break;
		}
		case UNI_FLOAT3:
		{
			float *f = (float *)mData;
			glUniform3fvARB(mLoc, m_count, (GLfloat *)f);
			break;
		}
		case UNI_FLOAT4:
		{
			float *f = (float *)mData;
			glUniform4fvARB(mLoc, m_count, (GLfloat *)f);
			break;
		}
		case UNI_INT2:
		{
			int *f = (int *)mData;
			glUniform2ivARB(mLoc, m_count, (GLint *)f);
			break;
		}
		case UNI_INT3:
		{
			int *f = (int *)mData;
			glUniform3ivARB(mLoc, m_count, (GLint *)f);
			break;
		}
		case UNI_INT4:
		{
			int *f = (int *)mData;
			glUniform4ivARB(mLoc, m_count, (GLint *)f);
			break;
		}
		case UNI_MAT4:
		{
			float *f = (float *)mData;
			glUniformMatrix4fvARB(mLoc, m_count, mTranspose ? GL_TRUE : GL_FALSE, (GLfloat *)f);
			break;
		}
		case UNI_MAT3:
		{
			float *f = (float *)mData;
			glUniformMatrix3fvARB(mLoc, m_count, mTranspose ? GL_TRUE : GL_FALSE, (GLfloat *)f);
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
	return (mShader != 0 && mOk && mUse);
}

RAS_Shader::RAS_Shader()
	:mShader(0),
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
	if (mShader) {
		glDeleteObjectARB(mShader);
		mShader = 0;
		mOk = false;
	}
}

bool RAS_Shader::LinkProgram()
{
	int vertlen = 0, fraglen = 0, proglen = 0;
	int vertstatus = 0, fragstatus = 0, progstatus = 0;
	unsigned int tmpVert = 0, tmpFrag = 0, tmpProg = 0;
	int char_len = 0;
	char *logInf = NULL;
	const char *vertProg = m_vertProg.ReadPtr();
	const char *fragProg = m_fragProg.ReadPtr();

	if (mError) {
		goto programError;
	}

	if (m_vertProg.IsEmpty() || m_fragProg.IsEmpty()) {
		spit("Invalid GLSL sources");
		return false;
	}

	if (!GLEW_ARB_fragment_shader) {
		spit("Fragment shaders not supported");
		return false;
	}

	if (!GLEW_ARB_vertex_shader) {
		spit("Vertex shaders not supported");
		return false;
	}

	// -- vertex shader ------------------
	tmpVert = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB);
	glShaderSourceARB(tmpVert, 1, (const char **)&vertProg, 0);
	glCompileShaderARB(tmpVert);
	glGetObjectParameterivARB(tmpVert, GL_OBJECT_INFO_LOG_LENGTH_ARB, (GLint *)&vertlen);

	// print info if any
	if (vertlen > 0 && vertlen < MAX_LOG_LEN) {
		logInf = (char *)MEM_mallocN(vertlen, "vert-log");
		glGetInfoLogARB(tmpVert, vertlen, (GLsizei *)&char_len, logInf);

		if (char_len > 0) {
			spit("---- Vertex Shader Error ----");
			spit(logInf);
		}

		MEM_freeN(logInf);
		logInf = 0;
	}

	// check for compile errors
	glGetObjectParameterivARB(tmpVert, GL_OBJECT_COMPILE_STATUS_ARB, (GLint *)&vertstatus);
	if (!vertstatus) {
		spit("---- Vertex shader failed to compile ----");
		goto programError;
	}

	// -- fragment shader ----------------
	tmpFrag = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);
	glShaderSourceARB(tmpFrag, 1, (const char **)&fragProg, 0);
	glCompileShaderARB(tmpFrag);
	glGetObjectParameterivARB(tmpFrag, GL_OBJECT_INFO_LOG_LENGTH_ARB, (GLint *)&fraglen);

	if (fraglen > 0 && fraglen < MAX_LOG_LEN) {
		logInf = (char *)MEM_mallocN(fraglen, "frag-log");
		glGetInfoLogARB(tmpFrag, fraglen, (GLsizei *)&char_len, logInf);

		if (char_len > 0) {
			spit("---- Fragment Shader Error ----");
			spit(logInf);
		}

		MEM_freeN(logInf);
		logInf = 0;
	}

	glGetObjectParameterivARB(tmpFrag, GL_OBJECT_COMPILE_STATUS_ARB, (GLint *)&fragstatus);

	if (!fragstatus) {
		spit("---- Fragment shader failed to compile ----");
		goto programError;
	}

	// -- program ------------------------
	// set compiled vert/frag shader & link
	tmpProg = glCreateProgramObjectARB();
	glAttachObjectARB(tmpProg, tmpVert);
	glAttachObjectARB(tmpProg, tmpFrag);
	glLinkProgramARB(tmpProg);
	glGetObjectParameterivARB(tmpProg, GL_OBJECT_INFO_LOG_LENGTH_ARB, (GLint *)&proglen);
	glGetObjectParameterivARB(tmpProg, GL_OBJECT_LINK_STATUS_ARB, (GLint *)&progstatus);

	if (proglen > 0 && proglen < MAX_LOG_LEN) {
		logInf = (char *)MEM_mallocN(proglen, "prog-log");
		glGetInfoLogARB(tmpProg, proglen, (GLsizei *)&char_len, logInf);

		if (char_len > 0) {
			spit("---- GLSL Program ----");
			spit(logInf);
		}

		MEM_freeN(logInf);
		logInf = 0;
	}

	if (!progstatus) {
		spit("---- GLSL program failed to link ----");
		goto programError;
	}

	// set
	mShader = tmpProg;
	glDeleteObjectARB(tmpVert);
	glDeleteObjectARB(tmpFrag);
	mOk = 1;
	mError = 0;
	return true;

programError:
	if (tmpVert) {
		glDeleteObjectARB(tmpVert);
		tmpVert = 0;
	}

	if (tmpFrag) {
		glDeleteObjectARB(tmpFrag);
		tmpFrag = 0;
	}

	if (tmpProg) {
		glDeleteObjectARB(tmpProg);
		tmpProg = 0;
	}

	mOk = 0;
	mUse = 0;
	mError = 1;
	return false;
}

void RAS_Shader::ValidateProgram()
{
	int stat = 0;
	glValidateProgramARB(mShader);
	glGetObjectParameterivARB(mShader, GL_OBJECT_VALIDATE_STATUS_ARB, (GLint *)&stat);

	if (stat > 0 && stat < MAX_LOG_LEN) {
		int char_len = 0;
		char *logInf = (char *)MEM_mallocN(stat, "validate-log");

		glGetInfoLogARB(mShader, stat, (GLsizei *)&char_len, logInf);

		if (char_len > 0) {
			spit("---- GLSL Validation ----");
			spit(logInf);
		}
		MEM_freeN(logInf);
		logInf = NULL;
	}
}

bool RAS_Shader::GetError()
{
	return mError;
}

unsigned int RAS_Shader::GetProg()
{
	return mShader;
}

void RAS_Shader::SetSampler(int loc, int unit)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		glUniform1iARB(loc, unit);
	}
}

void RAS_Shader::SetProg(bool enable)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		if (mShader != 0 && mOk && enable) {
			glUseProgramObjectARB(mShader);
		}
		else {
			glUseProgramObjectARB(0);
		}
	}
}


void RAS_Shader::Update(RAS_IRasterizer *rasty, const MT_Matrix4x4 model)
{
	if (!Ok() || !mPreDef.size()) {
		return;
	}

	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
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
					MT_Matrix4x4 viewinv = view;
					SetUniform(uni->mLoc, view.inverse());
					break;
				}
				case VIEWMATRIX_INVERSETRANSPOSE:
				{
					MT_Matrix4x4 viewinv = view;
					SetUniform(uni->mLoc, view.inverse(), true);
					break;
				}
				case CONSTANT_TIMER:
				{
					SetUniform(uni->mLoc, (float)rasty->GetTime());
					break;
				}
				default:
					break;
			}
		}
	}
}

int RAS_Shader::GetAttribute()
{
	return mAttr;
}


int RAS_Shader::GetAttribLocation(const char *name)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		return glGetAttribLocationARB(mShader, name);
	}

	return -1;
}

void RAS_Shader::BindAttribute(const char *attr, int loc)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		glBindAttribLocationARB(mShader, loc, attr);
	}
}

int RAS_Shader::GetUniformLocation(const char *name, bool debug)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		BLI_assert(mShader != 0);
		int location = glGetUniformLocationARB(mShader, name);

		if (location == -1 && debug) {
			spit("Invalid uniform value: " << name << ".");
		}

		return location;
	}
	return -1;
}

void RAS_Shader::SetUniform(int uniform, const MT_Vector2 &vec)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		float value[2];
		vec.getValue(value);
		glUniform2fvARB(uniform, 1, value);
	}
}

void RAS_Shader::SetUniform(int uniform, const MT_Vector3 &vec)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		float value[3];
		vec.getValue(value);
		glUniform3fvARB(uniform, 1, value);
	}
}

void RAS_Shader::SetUniform(int uniform, const MT_Vector4 &vec)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		float value[4];
		vec.getValue(value);
		glUniform4fvARB(uniform, 1, value);
	}
}

void RAS_Shader::SetUniform(int uniform, const unsigned int &val)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		glUniform1iARB(uniform, val);
	}
}

void RAS_Shader::SetUniform(int uniform, const int val)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		glUniform1iARB(uniform, val);
	}
}

void RAS_Shader::SetUniform(int uniform, const float &val)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		glUniform1fARB(uniform, val);
	}
}

void RAS_Shader::SetUniform(int uniform, const MT_Matrix4x4 &vec, bool transpose)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		float value[16];
		// note: getValue gives back column major as needed by OpenGL
		vec.getValue(value);
		glUniformMatrix4fvARB(uniform, 1, transpose ? GL_TRUE : GL_FALSE, value);
	}
}

void RAS_Shader::SetUniform(int uniform, const MT_Matrix3x3 &vec, bool transpose)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
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
		glUniformMatrix3fvARB(uniform, 1, transpose ? GL_TRUE : GL_FALSE, value);
	}
}

void RAS_Shader::SetUniform(int uniform, const float *val, int len)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		if (len == 2) {
			glUniform2fvARB(uniform, 1, (GLfloat *)val);
		}
		else if (len == 3) {
			glUniform3fvARB(uniform, 1, (GLfloat *)val);
		}
		else if (len == 4) {
			glUniform4fvARB(uniform, 1, (GLfloat *)val);
		}
		else {
			BLI_assert(0);
		}
	}
}

void RAS_Shader::SetUniform(int uniform, const int *val, int len)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		if (len == 2) {
			glUniform2ivARB(uniform, 1, (GLint *)val);
		}
		else if (len == 3) {
			glUniform3ivARB(uniform, 1, (GLint *)val);
		}
		else if (len == 4) {
			glUniform4ivARB(uniform, 1, (GLint *)val);
		}
		else {
			BLI_assert(0);
		}
	}
}
