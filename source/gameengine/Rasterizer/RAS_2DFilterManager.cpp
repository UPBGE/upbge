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
 * Contributor(s): Pierluigi Grassi, Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_2DFilterManager.cpp
 *  \ingroup bgerast
 */

#include "RAS_ICanvas.h"
#include "RAS_2DFilterManager.h"
#include "RAS_2DFilter.h"
#include <iostream>

#include "glew-mx.h"

#define STRINGIFY(A) #A
#include "RAS_OpenGLFilters/RAS_Blur2DFilter.h"
#include "RAS_OpenGLFilters/RAS_Sharpen2DFilter.h"
#include "RAS_OpenGLFilters/RAS_Dilation2DFilter.h"
#include "RAS_OpenGLFilters/RAS_Erosion2DFilter.h"
#include "RAS_OpenGLFilters/RAS_Laplacian2DFilter.h"
#include "RAS_OpenGLFilters/RAS_Sobel2DFilter.h"
#include "RAS_OpenGLFilters/RAS_Prewitt2DFilter.h"
#include "RAS_OpenGLFilters/RAS_GrayScale2DFilter.h"
#include "RAS_OpenGLFilters/RAS_Sepia2DFilter.h"
#include "RAS_OpenGLFilters/RAS_Invert2DFilter.h"


RAS_2DFilterManager::RAS_2DFilterManager(RAS_ICanvas *canvas)
	:m_canvas(canvas)
{
}

RAS_2DFilterManager::~RAS_2DFilterManager()
{
	for (RAS_PassTo2DFilter::iterator it = m_filters.begin(), end = m_filters.end(); it != end; ++it) {
		RAS_2DFilter *filter = it->second;
		delete filter;
	}
}

void RAS_2DFilterManager::PrintShaderError(unsigned int shaderUid, const char *title, const char *shaderCode)
{
	std::cout << "2D Filter GLSL Shader: " << title << " error." << std::endl;

	// Copied value from BL_Shader MAX_LOG_LEN.
	const int MAX_LOG_CHAR_COUNT = 262144;
	GLint logSize = 0;
	glGetShaderiv(shaderUid, GL_INFO_LOG_LENGTH, &logSize);

	if(logSize != 0) {
		GLsizei infoLogRetSize = 0;
		if (logSize > MAX_LOG_CHAR_COUNT) {
			logSize = MAX_LOG_CHAR_COUNT;
		}
		char *logCharBuffer = (char *)malloc(sizeof(char) * logSize);

		glGetInfoLogARB(shaderUid, logSize, &infoLogRetSize, logCharBuffer);
		std::cout << logCharBuffer << std::endl;

		free(logCharBuffer);
	}
}

RAS_2DFilter *RAS_2DFilterManager::AddFilter(RAS_2DFilterData& filterData)
{
	RAS_2DFilter *filter = CreateFilter(filterData);

	m_filters[filterData.filterPassIndex] = filter;

	return filter;
}

void RAS_2DFilterManager::EnableFilterPass(unsigned int passIndex)
{
	RAS_2DFilter *filter = GetFilterPass(passIndex);
	if (filter) {
		filter->SetEnabled(true);
	}
}

void RAS_2DFilterManager::DisableFilterPass(unsigned int passIndex)
{
	RAS_2DFilter *filter = GetFilterPass(passIndex);
	if (filter) {
		filter->SetEnabled(false);
	}
}

void RAS_2DFilterManager::RemoveFilterPass(unsigned int passIndex)
{
	m_filters.erase(passIndex);
}

RAS_2DFilter *RAS_2DFilterManager::GetFilterPass(unsigned int passIndex)
{
	RAS_PassTo2DFilter::iterator it = m_filters.find(passIndex);
	return (it != m_filters.end()) ? it->second : NULL;
}

void RAS_2DFilterManager::RenderFilters()
{
	for (RAS_PassTo2DFilter::iterator it = m_filters.begin(), end = m_filters.end(); it != end; ++it) {
		RAS_2DFilter *filter = it->second;
		filter->Start();
		filter->End();
	}
}

RAS_ICanvas *RAS_2DFilterManager::GetCanvas()
{
	return m_canvas;
}

RAS_2DFilter *RAS_2DFilterManager::CreateFilter(RAS_2DFilterData& filterData)
{
	RAS_2DFilter *result = NULL;
	const char *shaderSource = NULL;
	switch(filterData.filterMode) {
		case RAS_2DFilterManager::FILTER_MOTIONBLUR:
			break;
		case RAS_2DFilterManager::FILTER_BLUR:
			shaderSource = BlurFragmentShader;
			break;
		case RAS_2DFilterManager::FILTER_SHARPEN:
			shaderSource = SharpenFragmentShader;
			break;
		case RAS_2DFilterManager::FILTER_DILATION:
			shaderSource = DilationFragmentShader;
			break;
		case RAS_2DFilterManager::FILTER_EROSION:
			shaderSource = ErosionFragmentShader;
			break;
		case RAS_2DFilterManager::FILTER_LAPLACIAN:
			shaderSource = LaplacianFragmentShader;
			break;
		case RAS_2DFilterManager::FILTER_SOBEL:
			shaderSource = SobelFragmentShader;
			break;
		case RAS_2DFilterManager::FILTER_PREWITT:
			shaderSource = PrewittFragmentShader;
			break;
		case RAS_2DFilterManager::FILTER_GRAYSCALE:
			shaderSource = GrayScaleFragmentShader;
			break;
		case RAS_2DFilterManager::FILTER_SEPIA:
			shaderSource = SepiaFragmentShader;
			break;
		case RAS_2DFilterManager::FILTER_INVERT:
			shaderSource = InvertFragmentShader;
			break;
	}
	if (!shaderSource) {
		if(filterData.filterMode == RAS_2DFilterManager::FILTER_CUSTOMFILTER) {
			result = new RAS_2DFilter(filterData, this);
		}
		else {
			std::cout << "Cannot create filter for mode: " << filterData.filterMode << "." << std::endl;
		}
	}
	else {
		filterData.shaderText = shaderSource;
		result = new RAS_2DFilter(filterData, this);
	}
	return result;
}
