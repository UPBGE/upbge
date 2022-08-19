/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbdds
 */

/*
 * This file is based on a similar file from the NVIDIA texture tools
 * (http://nvidia-texture-tools.googlecode.com/)
 *
 * Original license from NVIDIA follows.
 */

/* This code is in the public domain -- <castanyo@yahoo.es>. */

#include <Color.h>
#include <Image.h>

#include <cstdio> /* printf */

Image::Image() : m_width(0), m_height(0), m_format(Format_RGB), m_data(nullptr)
{
}

Image::~Image()
{
  free();
}

void Image::allocate(uint w, uint h)
{
  free();
  m_width = w;
  m_height = h;
  m_data = new Color32[w * h];
}

void Image::free()
{
  delete[] m_data;
  m_data = nullptr;
}

uint Image::width() const
{
  return m_width;
}

uint Image::height() const
{
  return m_height;
}

const Color32 *Image::scanline(uint h) const
{
  if (h >= m_height) {
    printf("DDS: scanline beyond dimensions of image\n");
    return m_data;
  }
  return m_data + h * m_width;
}

Color32 *Image::scanline(uint h)
{
  if (h >= m_height) {
    printf("DDS: scanline beyond dimensions of image\n");
    return m_data;
  }
  return m_data + h * m_width;
}

const Color32 *Image::pixels() const
{
  return m_data;
}

Color32 *Image::pixels()
{
  return m_data;
}

const Color32 &Image::pixel(uint idx) const
{
  if (idx >= m_width * m_height) {
    printf("DDS: pixel beyond dimensions of image\n");
    return m_data[0];
  }
  return m_data[idx];
}

Color32 &Image::pixel(uint idx)
{
  if (idx >= m_width * m_height) {
    printf("DDS: pixel beyond dimensions of image\n");
    return m_data[0];
  }
  return m_data[idx];
}

Image::Format Image::format() const
{
  return m_format;
}

void Image::setFormat(Image::Format f)
{
  m_format = f;
}
