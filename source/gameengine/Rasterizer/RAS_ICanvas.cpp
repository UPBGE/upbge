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

/** \file gameengine/Rasterizer/RAS_ICanvas.cpp
 *  \ingroup bgerast
 */

#include "RAS_ICanvas.h"

#include "BKE_image.hh"
#include "BKE_image_format.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_task.h"
#include "DNA_scene_types.h"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"
#include "MEM_guardedalloc.h"

#include "CM_Message.h"

// Task data for saving screenshots in a different thread.
struct ScreenshotTaskData {
  unsigned int *dumprect;
  int dumpsx;
  int dumpsy;
  char path[FILE_MAX];
  ImageFormatData *im_format;
};

/**
 * Function that actually performs the image compression and saving to disk of a screenshot.
 * Run in a separate thread by RAS_ICanvas::save_screenshot().
 *
 * @param taskdata Must point to a ScreenshotTaskData object. This function takes ownership
 *                 of all pointers in the ScreenshotTaskData, and frees them.
 */
void save_screenshot_thread_func(TaskPool *__restrict pool, void *taskdata, int threadid);

RAS_ICanvas::RAS_ICanvas(RAS_Rasterizer *rasty) : m_rasterizer(rasty), m_samples(0)
{
  m_taskpool = BLI_task_pool_create(nullptr, TASK_PRIORITY_LOW);
}

RAS_ICanvas::~RAS_ICanvas()
{
  if (m_taskpool) {
    BLI_task_pool_work_and_wait(m_taskpool);
    BLI_task_pool_free(m_taskpool);
    m_taskpool = nullptr;
  }
}

void RAS_ICanvas::SetSamples(int samples)
{
  m_samples = samples;
}

int RAS_ICanvas::GetSamples() const
{
  return m_samples;
}

int RAS_ICanvas::GetWidth() const
{
  return m_viewportArea.GetWidth();
}

int RAS_ICanvas::GetHeight() const
{
  return m_viewportArea.GetHeight();
}

float RAS_ICanvas::GetMouseNormalizedX(int x)
{
  return float(x) / GetWidth();
}

float RAS_ICanvas::GetMouseNormalizedY(int y)
{
  return float(y) / GetHeight();
}

const RAS_Rect &RAS_ICanvas::GetWindowArea() const
{
  return m_windowArea;
}

const RAS_Rect &RAS_ICanvas::GetViewportArea() const
{
  return m_viewportArea;
}

void RAS_ICanvas::FlushScreenshots()
{
  for (const Screenshot &screenshot : m_screenshots) {
    SaveScreeshot(screenshot);
  }

  m_screenshots.clear();
}

void RAS_ICanvas::AddScreenshot(
    const std::string &path, int x, int y, int width, int height, ImageFormatData *format)
{
  Screenshot screenshot;
  screenshot.path = path;
  screenshot.x = x;
  screenshot.y = y;
  screenshot.width = width;
  screenshot.height = height;
  screenshot.format = format;

  m_screenshots.push_back(screenshot);
}

void save_screenshot_thread_func(TaskPool *__restrict (pool),
                                 void *taskdata,
                                 int /*(threadid)*/)
{
  ScreenshotTaskData *task = static_cast<ScreenshotTaskData *>(taskdata);

  /* create and save imbuf */
  ImBuf *ibuf = IMB_allocImBuf(task->dumpsx, task->dumpsy, 24, 0);
  ibuf->byte_buffer.data = (uint8_t *)task->dumprect;

  BKE_imbuf_write_as(ibuf, task->path, task->im_format, false);

  ibuf->byte_buffer.data = nullptr;
  IMB_freeImBuf(ibuf);
  // Dumprect is allocated in RAS_OpenGLRasterizer::MakeScreenShot with malloc(), we must use
  // free() then.
  free(task->dumprect);
  MEM_freeN(task->im_format);
}

void RAS_ICanvas::SaveScreeshot(const Screenshot &screenshot)
{
  unsigned int *pixels = m_rasterizer->MakeScreenshot(
      screenshot.x, screenshot.y, screenshot.width, screenshot.height);
  if (!pixels) {
    CM_Error("cannot allocate pixels array");
    return;
  }

  /* Save the actual file in a different thread, so that the
   * game engine can keep running at full speed. */
  ScreenshotTaskData *task = (ScreenshotTaskData *)MEM_mallocN(sizeof(ScreenshotTaskData),
                                                               "screenshot-data");
  task->dumprect = pixels;
  task->dumpsx = screenshot.width;
  task->dumpsy = screenshot.height;
  task->im_format = screenshot.format;

  BLI_strncpy(task->path, screenshot.path.c_str(), FILE_MAX);
  BLI_path_frame(task->path, sizeof(task->path), m_frame, 0);
  m_frame++;
  BKE_image_path_ext_from_imtype_ensure(task->path, sizeof(task->path), task->im_format->imtype);

  BLI_task_pool_push(m_taskpool,
                     (TaskRunFunction)save_screenshot_thread_func,
                     task,
                     true,  // free task data
                     NULL);
}
