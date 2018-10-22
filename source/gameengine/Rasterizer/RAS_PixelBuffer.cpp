#include "RAS_PixelBuffer.h"
#include "RAS_OpenGLPixelBuffer.h"

RAS_PixelBuffer::RAS_PixelBuffer()
	:m_impl(new RAS_OpenGLPixelBuffer())
{
}

RAS_PixelBuffer::~RAS_PixelBuffer()
{
}

void RAS_PixelBuffer::Copy(int x, int y, int width, int height)
{
	return m_impl->Copy(x, y, width, height);
}

const unsigned int *RAS_PixelBuffer::Map()
{
	return m_impl->Map();
}

void RAS_PixelBuffer::Unmap()
{
	m_impl->Unmap();
}
