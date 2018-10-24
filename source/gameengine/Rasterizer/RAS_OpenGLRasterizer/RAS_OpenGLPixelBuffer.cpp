#include "RAS_OpenGLPixelBuffer.h"

#include <cstring>

RAS_OpenGLPixelBuffer::RAS_OpenGLPixelBuffer()
{
	glGenBuffers(1, &m_pbo);
}

RAS_OpenGLPixelBuffer::~RAS_OpenGLPixelBuffer()
{
	glDeleteBuffers(1, &m_pbo);
}

void RAS_OpenGLPixelBuffer::Copy(int x, int y, int width, int height)
{
	m_size = width * height * sizeof(unsigned int);

	glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
	glBufferData(GL_PIXEL_PACK_BUFFER, m_size, nullptr, GL_STREAM_READ);
	glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

unsigned int *RAS_OpenGLPixelBuffer::Map()
{
	glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
	unsigned int *buffer = (unsigned int *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	return buffer;
}

void RAS_OpenGLPixelBuffer::Unmap()
{
	glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}
