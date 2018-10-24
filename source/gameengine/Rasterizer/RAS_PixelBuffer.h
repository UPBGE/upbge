#ifndef __RAS_PIXEL_BUFFER_H__
#define __RAS_PIXEL_BUFFER_H__

#include <memory>

class RAS_OpenGLPixelBuffer;

class RAS_PixelBuffer
{
private:
	const std::unique_ptr<RAS_OpenGLPixelBuffer> m_impl;

public:
	RAS_PixelBuffer();
	~RAS_PixelBuffer();

	// Copy screen image in described area to this pixel buffer.
	void Copy(int x, int y, int width, int height);
	// Get pixel buffer pointer.
	unsigned int *Map();
	// Release pixel buffer pointer.
	void Unmap();
};

#endif  // __RAS_PIXEL_BUFFER_H__
