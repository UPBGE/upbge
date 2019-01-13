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
#include "RAS_OffScreen.h"
#include "DNA_scene_types.h"

#include "BKE_image.h"
#include "BKE_global.h"
#include "BKE_main.h"

#include "BLI_task.h"
#include "BLI_path_util.h"
#include "BLI_string.h"

#include "MEM_guardedalloc.h"

#include "GPU_framebuffer.h"

extern "C" {
#  include "IMB_imbuf.h"
#  include "IMB_imbuf_types.h"
}

#include "CM_Message.h"

#include <stdlib.h> // for free()

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
 * \param taskdata Must point to a ScreenshotTaskData object. This function takes ownership
 *                 of all pointers in the ScreenshotTaskData, and frees them.
 */
void save_screenshot_thread_func(TaskPool *__restrict pool, void *taskdata, int threadid);

const int RAS_ICanvas::swapInterval[RAS_ICanvas::SWAP_CONTROL_MAX] = {
	0, // VSYNC_OFF
	1, // VSYNC_ON
	-1 // VSYNC_ADAPTIVE
};

RAS_ICanvas::RAS_ICanvas(const RAS_OffScreen::AttachmentList& attachments, int numSamples)
	:m_samples(numSamples),
	m_attachments(attachments),
	m_swapControl(VSYNC_OFF),
	m_frame(1)
{
	m_taskscheduler = BLI_task_scheduler_create(TASK_SCHEDULER_AUTO_THREADS);
	m_taskpool = BLI_task_pool_create(m_taskscheduler, nullptr);
}

RAS_ICanvas::~RAS_ICanvas()
{
	if (m_taskpool) {
		BLI_task_pool_work_and_wait(m_taskpool);
		BLI_task_pool_free(m_taskpool);
		m_taskpool = nullptr;
	}

	if (m_taskscheduler) {
		BLI_task_scheduler_free(m_taskscheduler);
		m_taskscheduler = nullptr;
	}
}

void RAS_ICanvas::SetSwapControl(SwapControl control)
{
	m_swapControl = control;
}

RAS_ICanvas::SwapControl RAS_ICanvas::GetSwapControl() const
{
	return m_swapControl;
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
	return m_area.GetWidth();
}

int RAS_ICanvas::GetHeight() const
{
	return m_area.GetHeight();
}

int RAS_ICanvas::GetMaxX() const
{
	return m_area.GetMaxX();
}

int RAS_ICanvas::GetMaxY() const
{
	return m_area.GetMaxY();
}


float RAS_ICanvas::GetMouseNormalizedX(int x) const
{
	return float(x) / m_area.GetMaxX();
}

float RAS_ICanvas::GetMouseNormalizedY(int y) const
{
	return float(y) / m_area.GetMaxY();
}

const RAS_Rect &RAS_ICanvas::GetArea() const
{
	return m_area;
}

const int *RAS_ICanvas::GetViewPort() const
{
	return m_viewport;
}

void RAS_ICanvas::FlushScreenshots(RAS_Rasterizer *rasty)
{
	for (const Screenshot& screenshot : m_screenshots) {
		SaveScreeshot(screenshot, rasty);
	}

	m_screenshots.clear();
}

RAS_OffScreen *RAS_ICanvas::GetOffScreen(RAS_OffScreen::Type type)
{
	return m_offScreens[type].get();
}

void RAS_ICanvas::AddScreenshot(const std::string& path, int x, int y, int width, int height, ImageFormatData *format)
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

void save_screenshot_thread_func(TaskPool *__restrict UNUSED(pool), void *taskdata, int UNUSED(threadid))
{
	ScreenshotTaskData *task = static_cast<ScreenshotTaskData *>(taskdata);

	/* create and save imbuf */
	ImBuf *ibuf = IMB_allocImBuf(task->dumpsx, task->dumpsy, 24, 0);
	ibuf->rect = task->dumprect;

	BKE_imbuf_write_as(ibuf, task->path, task->im_format, false);

	ibuf->rect = nullptr;
	IMB_freeImBuf(ibuf);
	// Dumprect is allocated in RAS_OpenGLRasterizer::MakeScreenShot with malloc(), we must use free() then.
	free(task->dumprect);
	MEM_freeN(task->im_format);
}


void RAS_ICanvas::SaveScreeshot(const Screenshot& screenshot, RAS_Rasterizer *rasty)
{
	unsigned int *pixels = rasty->MakeScreenshot(screenshot.x, screenshot.y, screenshot.width, screenshot.height);
	if (!pixels) {
		CM_Error("cannot allocate pixels array");
		return;
	}

	/* Save the actual file in a different thread, so that the
	 * game engine can keep running at full speed. */
	ScreenshotTaskData *task = (ScreenshotTaskData *)MEM_mallocN(sizeof(ScreenshotTaskData), "screenshot-data");
	task->dumprect = pixels;
	task->dumpsx = screenshot.width;
	task->dumpsy = screenshot.height;
	task->im_format = screenshot.format;

	BLI_strncpy(task->path, screenshot.path.c_str(), FILE_MAX);
	BLI_path_frame(task->path, m_frame, 0);
	m_frame++;
	BKE_image_path_ensure_ext_from_imtype(task->path, task->im_format->imtype);

	BLI_task_pool_push(m_taskpool,
	                   save_screenshot_thread_func,
	                   task,
	                   true, // free task data
	                   TASK_PRIORITY_LOW);
}

void RAS_ICanvas::UpdateOffScreens()
{
	const unsigned int width = GetWidth();
	const unsigned int height = GetHeight();
	for (unsigned short i = 0; i < RAS_OffScreen::RAS_OFFSCREEN_MAX; ++i) {
		RAS_OffScreen::Type type = (RAS_OffScreen::Type)i;
		// Check if the off screen type can support samples.
		const bool sampleofs = ELEM(type, RAS_OffScreen::RAS_OFFSCREEN_EYE_LEFT0, RAS_OffScreen::RAS_OFFSCREEN_EYE_RIGHT0);

		/* Some GPUs doesn't support high multisample value with GL_RGBA16F or GL_RGBA32F.
		 * To avoid crashing we check if the off screen was created and if not decremente
		 * the multisample value and try to create the off screen to find a supported value.
		 */
		for (int samples = m_samples; samples >= 0; --samples) {
			RAS_OffScreen *ofs = new RAS_OffScreen(width, height, sampleofs ? samples : 0, m_attachments, type);
			if (!ofs->GetValid()) {
				delete ofs;
				continue;
			}

			m_offScreens[type].reset(ofs);
			m_samples = samples;
			break;
		}

		/* Creating an off screen restore the default frame buffer object.
		 * We have to rebind the last off screen. */
		RAS_OffScreen *lastOffScreen = RAS_OffScreen::GetLastOffScreen();
		if (lastOffScreen) {
			lastOffScreen->Bind();
		}
	}
}
