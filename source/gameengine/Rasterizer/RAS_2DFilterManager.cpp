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

#include "RAS_2DFilterManager.h"

#include "DRW_render.h"

#include "CM_Message.h"
#include "RAS_2DFilter.h"
#include "RAS_FrameBuffer.h"

extern "C" {
extern char datatoc_RAS_Blur2DFilter_glsl[];
extern char datatoc_RAS_Sharpen2DFilter_glsl[];
extern char datatoc_RAS_Dilation2DFilter_glsl[];
extern char datatoc_RAS_Erosion2DFilter_glsl[];
extern char datatoc_RAS_Laplacian2DFilter_glsl[];
extern char datatoc_RAS_Sobel2DFilter_glsl[];
extern char datatoc_RAS_Prewitt2DFilter_glsl[];
extern char datatoc_RAS_GrayScale2DFilter_glsl[];
extern char datatoc_RAS_Sepia2DFilter_glsl[];
extern char datatoc_RAS_Invert2DFilter_glsl[];
}

RAS_2DFilterManager::RAS_2DFilterManager()
{
}

RAS_2DFilterManager::~RAS_2DFilterManager()
{
  for (const RAS_PassTo2DFilter::value_type &pair : m_filters) {
    RAS_2DFilter *filter = pair.second;
    delete filter;
  }
}

RAS_2DFilter *RAS_2DFilterManager::AddFilter(RAS_2DFilterData &filterData)
{
  RAS_2DFilter *filter = CreateFilter(filterData);

  m_filters[filterData.filterPassIndex] = filter;
  // By default enable the filter.
  filter->SetEnabled(true);

  return filter;
}

void RAS_2DFilterManager::RemoveFilterPass(unsigned int passIndex)
{
  m_filters.erase(passIndex);
}

RAS_2DFilter *RAS_2DFilterManager::GetFilterPass(unsigned int passIndex)
{
  RAS_PassTo2DFilter::iterator it = m_filters.find(passIndex);
  return (it != m_filters.end()) ? it->second : nullptr;
}

RAS_FrameBuffer *RAS_2DFilterManager::RenderFilters(RAS_Rasterizer *rasty,
                                                    RAS_ICanvas *canvas,
                                                    RAS_FrameBuffer *inputfb,
                                                    RAS_FrameBuffer *targetfb,
                                                    KX_Scene *scene)
{
  if (m_filters.empty()) {
    // No filters, discard.
    GPU_framebuffer_bind(targetfb->GetFrameBuffer());
    rasty->DrawFrameBuffer(inputfb, targetfb);
    return targetfb;
  }

  /* Set ogl states */
  /*rasty->Disable(RAS_Rasterizer::RAS_CULL_FACE);
  rasty->Disable(RAS_Rasterizer::RAS_DEPTH_TEST);
  rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);
  rasty->Disable(RAS_Rasterizer::RAS_BLEND);
  rasty->Disable(RAS_Rasterizer::RAS_ALPHA_TEST);*/

  GPU_face_culling(GPU_CULL_NONE);
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_depth_mask(false);
  GPU_blend(GPU_BLEND_NONE);

  rasty->SetLines(false);

  RAS_FrameBuffer *previousfb = inputfb;

  /* Set source off screen to RAS_FrameBuffer_FILTER0 in case of multisample and blit,
   * else keep the original source off screen. */
  // if (inputfb->GetSamples()) {
  //	previousfb = rasty->GetFrameBuffer(RAS_Rasterizer::RAS_FrameBuffer_FILTER0);
  //	// No need to bind previousfb because a blit is proceeded.
  //	rasty->DrawOffScreen(inputfb, previousfb);
  //}

  // The filter color input off screen, changed for each filters.
  RAS_FrameBuffer *colorfb;
  // The filter depth input off scree, unchanged for each filters.
  RAS_FrameBuffer *depthfb = previousfb;

  // Used to know if a filter is the last of the container.
  RAS_PassTo2DFilter::const_iterator pend = std::prev(m_filters.end());

  for (RAS_PassTo2DFilter::iterator begin = m_filters.begin(), it = begin, end = m_filters.end();
       it != end;
       ++it) {
    RAS_2DFilter *filter = it->second;

    /* Assign the previous off screen to the input off screen. At the first render it's the
     * input off screen sent to RenderFilters. */
    colorfb = previousfb;

    RAS_FrameBuffer *ftargetfb;
    // Computing the filter targeted off screen.
    if (it == pend) {
      // Render to the targeted off screen for the last filter.
      ftargetfb = targetfb;
    }
    else {
      // Else render to the next off screen compared to the input off screen.
      ftargetfb = rasty->GetFrameBuffer(RAS_Rasterizer::NextFilterFrameBuffer(colorfb->GetType()));
    }

    /* Get the output off screen of the filter, could be the same as the input off screen
     * if no modifications were made or the targeted off screen.
     * This output off screen is used for the next filter as input off screen */
    previousfb = filter->Start(rasty, canvas, depthfb, colorfb, ftargetfb);
    filter->End();
  }

  // The last filter doesn't use its own off screen and didn't render to the targeted off screen ?
  if (previousfb != targetfb) {
    // Render manually to the targeted off screen as the last filter didn't do it for us.
    GPU_framebuffer_bind(targetfb->GetFrameBuffer());
    rasty->DrawFrameBuffer(previousfb, targetfb);
  }

  /*rasty->Enable(RAS_Rasterizer::RAS_DEPTH_TEST);
  rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
  rasty->Enable(RAS_Rasterizer::RAS_CULL_FACE);*/
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPU_depth_mask(true);
  GPU_face_culling(GPU_CULL_BACK);

  return targetfb;
}

RAS_2DFilter *RAS_2DFilterManager::CreateFilter(RAS_2DFilterData &filterData)
{
  RAS_2DFilter *result = nullptr;
  std::string shaderSource;
  switch (filterData.filterMode) {
    case RAS_2DFilterManager::FILTER_MOTIONBLUR:
      break;
    case RAS_2DFilterManager::FILTER_BLUR:
      shaderSource = datatoc_RAS_Blur2DFilter_glsl;
      break;
    case RAS_2DFilterManager::FILTER_SHARPEN:
      shaderSource = datatoc_RAS_Sharpen2DFilter_glsl;
      break;
    case RAS_2DFilterManager::FILTER_DILATION:
      shaderSource = datatoc_RAS_Dilation2DFilter_glsl;
      break;
    case RAS_2DFilterManager::FILTER_EROSION:
      shaderSource = datatoc_RAS_Erosion2DFilter_glsl;
      break;
    case RAS_2DFilterManager::FILTER_LAPLACIAN:
      shaderSource = datatoc_RAS_Laplacian2DFilter_glsl;
      break;
    case RAS_2DFilterManager::FILTER_SOBEL:
      shaderSource = datatoc_RAS_Sobel2DFilter_glsl;
      break;
    case RAS_2DFilterManager::FILTER_PREWITT:
      shaderSource = datatoc_RAS_Prewitt2DFilter_glsl;
      break;
    case RAS_2DFilterManager::FILTER_GRAYSCALE:
      shaderSource = datatoc_RAS_GrayScale2DFilter_glsl;
      break;
    case RAS_2DFilterManager::FILTER_SEPIA:
      shaderSource = datatoc_RAS_Sepia2DFilter_glsl;
      break;
    case RAS_2DFilterManager::FILTER_INVERT:
      shaderSource = datatoc_RAS_Invert2DFilter_glsl;
      break;
  }
  if (shaderSource.empty()) {
    if (filterData.filterMode == RAS_2DFilterManager::FILTER_CUSTOMFILTER) {
      result = NewFilter(filterData);
    }
    else {
      CM_Error("cannot create filter for mode: " << filterData.filterMode << ".");
    }
  }
  else {
    filterData.shaderText = shaderSource;
    result = NewFilter(filterData);
  }
  return result;
}
