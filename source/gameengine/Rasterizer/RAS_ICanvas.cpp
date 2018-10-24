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

#include "DNA_scene_types.h" // For ImageFormatData.

#include "BKE_image.h"
#include "BKE_global.h"
#include "BKE_main.h"

#include "BLI_path_util.h"
#include "BLI_string.h"

#include "MEM_guardedalloc.h"

extern "C" {
#  include "IMB_imbuf.h"
#  include "IMB_imbuf_types.h"
}

#include "CM_Message.h"

const int RAS_ICanvas::swapInterval[RAS_ICanvas::SWAP_CONTROL_MAX] = {
	0, // VSYNC_OFF
	1, // VSYNC_ON
	-1 // VSYNC_ADAPTIVE
};

RAS_ICanvas::RAS_ICanvas(RAS_Rasterizer *rasty)
	:m_currentScreenshotQueue(0),
	m_samples(0),
	m_hdrType(RAS_Rasterizer::RAS_HDR_NONE),
	m_swapControl(VSYNC_OFF),
	m_frame(1)
{
	m_rasterizer = rasty;
}

RAS_ICanvas::~RAS_ICanvas()
{
	for (ScreenshotQueue& queue : m_screenshotsQueues) {
		queue.tasks.wait();
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

void RAS_ICanvas::SetHdrType(RAS_Rasterizer::HdrType type)
{
	m_hdrType = type;
}

RAS_Rasterizer::HdrType RAS_ICanvas::GetHdrType() const
{
	return m_hdrType;
}

class CompressImageTask
{
private:
	const unsigned short m_width;
	const unsigned short m_height;
	unsigned int *m_pixels;
	const std::string m_path;
	ImageFormatData *m_format;
	unsigned int m_frame;

public:
	CompressImageTask(const RAS_Rect& area, unsigned int *pixels, const std::string& path,
			ImageFormatData *format, unsigned  int frame)
		:m_width(area.GetWidth()),
		m_height(area.GetHeight()),
		m_pixels(pixels),
		m_path(path),
		m_format(format),
		m_frame(frame)
	{
	}

	void operator()() const
	{
		// Get path.
		char path[FILE_MAX];
		BLI_strncpy(path, m_path.c_str(), FILE_MAX);
		BLI_path_frame(path, m_frame, 0);
		BKE_image_path_ensure_ext_from_imtype(path, m_format->imtype);

		// Create and save imbuf.
		ImBuf *ibuf = IMB_allocImBuf(m_width, m_height, 24, 0);
		ibuf->rect = m_pixels;

		BKE_imbuf_write_as(ibuf, path, m_format, false);

		ibuf->rect = nullptr;
		IMB_freeImBuf(ibuf);

		MEM_freeN(m_format);
	}
};

void RAS_ICanvas::FlushScreenshots()
{
#if 0
	const RAS_Rect& area = GetWindowArea();
	unsigned int *pixels = m_rasterizer->MakeScreenshot(area.GetLeft(), area.GetBottom(), area.GetWidth(), area.GetHeight());

#else
	// Index of queue compressing images.
	const unsigned short imageQueueIndex = (m_currentScreenshotQueue + (NUM_SCREENSHOT_QUEUE) / 2) % NUM_SCREENSHOT_QUEUE;

	// Queue copying screen to buffer buffer.
	ScreenshotQueue& copyQueue = m_screenshotsQueues[m_currentScreenshotQueue];
	// Queue compressing images.
	ScreenshotQueue& imageQueue = m_screenshotsQueues[imageQueueIndex];

	// Wait until all compressions using the queue buffer are proceeded before copying a new buffer.
	copyQueue.tasks.wait();
	// Release the buffer pointer.
	copyQueue.buffer.Unmap();

	const RAS_Rect& area = GetWindowArea(); // TODO get member (render attachement branch)
	copyQueue.area = area;
	// Copy the data from current frame in an other buffer and process the image conversion on futur frame.
	copyQueue.buffer.Copy(area.GetLeft(), area.GetBottom(), area.GetWidth(), area.GetHeight());

	// Create image compression tasks.
	std::vector<Screenshot>& screenshots = imageQueue.screenshots;
	if (!screenshots.empty()) {
		// Obtain buffer pointer of image data.
		imageQueue.pixels = imageQueue.buffer.Map();

		for (const Screenshot& screenshot : screenshots) {
			// Create image compression task.
			const CompressImageTask task(imageQueue.area, imageQueue.pixels, screenshot.path, screenshot.format, m_frame);
			// Pass to next frame for paths using it.
			m_frame++;
			// Register the task.
			imageQueue.tasks.run(task);
		}

		screenshots.clear();
	}

	// Pass to the next buffer.
	m_currentScreenshotQueue = (m_currentScreenshotQueue + 1) % NUM_SCREENSHOT_QUEUE;
#endif
}

void RAS_ICanvas::AddScreenshot(const std::string& path, ImageFormatData *format)
{
	Screenshot screenshot;
	screenshot.path = path;
	screenshot.format = format;

	m_screenshotsQueues[m_currentScreenshotQueue].screenshots.push_back(screenshot);
}
