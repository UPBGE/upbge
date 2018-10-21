#include "RAS_OpenGLPixelBuffer.h"

#include <cstring>

RAS_OpenGLPixelBuffer::RAS_OpenGLPixelBuffer()
	:m_size(0) // TODO cas impossible a supprimer
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

const unsigned int *RAS_OpenGLPixelBuffer::Get()
{
	if (m_size == 0) {
		return nullptr;
	}

	unsigned int *pixels = new unsigned int[m_size];

	glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
	unsigned int *buffer = (unsigned int *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);

	memcpy(pixels, buffer, m_size);
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	return pixels;
}
