/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "AppCanvas.h"
#include "AppConfig.h"
#include "AppView.h"
#include "Controller.h"

#include "../image/Image.h"
#include "../stroke/StrokeRenderer.h"
#include "../stroke/StyleModule.h"
#include "../system/TimeStamp.h"

#include "../system/StringUtils.h"
namespace Freestyle {

AppCanvas::AppCanvas()
{
  _pViewer = nullptr;
  _MapsPath = Config::Path::getInstance()->getMapsDir().c_str();
}

AppCanvas::AppCanvas(AppView *iViewer)
{
  _pViewer = iViewer;
}

AppCanvas::AppCanvas(const AppCanvas &iBrother) : Canvas(iBrother)
{
  _pViewer = iBrother._pViewer;
}

AppCanvas::~AppCanvas()
{
  _pViewer = nullptr;
}

void AppCanvas::setViewer(AppView *iViewer)
{
  _pViewer = iViewer;
}

int AppCanvas::width() const
{
  return _pViewer->width();
}

int AppCanvas::height() const
{
  return _pViewer->height();
}

BBox<Vec2i> AppCanvas::border() const
{
  return _pViewer->border();
}

float AppCanvas::thickness() const
{
  return _pViewer->thickness();
}

BBox<Vec3r> AppCanvas::scene3DBBox() const
{
  return _pViewer->scene3DBBox();
}

void AppCanvas::preDraw()
{
  Canvas::preDraw();
}

void AppCanvas::init()
{
#if 0
  static bool firsttime = true;
  if (firsttime) {
    _Renderer = new BlenderStrokeRenderer;
    if (!StrokeRenderer::loadTextures()) {
      cerr << "unable to load stroke textures" << endl;
      return;
    }
  }
#endif
}

void AppCanvas::postDraw()
{
  for (unsigned int i = 0; i < _StyleModules.size(); i++) {
    if (!_StyleModules[i]->getDisplayed() || !_Layers[i]) {
      continue;
    }
    _Layers[i]->ScaleThickness(thickness());
  }
  Canvas::postDraw();
}

void AppCanvas::Erase()
{
  Canvas::Erase();
}

// Abstract

void AppCanvas::readColorPixels(int x, int y, int w, int h, RGBImage &oImage) const
{
  float *rgb = new float[3 * w * h];
  memset(rgb, 0, sizeof(float[3]) * w * h);
  int xsch = width();
  int ysch = height();
  if (_pass_diffuse.buf) {
    int xmin = border().getMin().x();
    int ymin = border().getMin().y();
    int xmax = border().getMax().x();
    int ymax = border().getMax().y();
    int rectx = _pass_diffuse.width;
    int recty = _pass_diffuse.height;
    float xfac = ((float)rectx) / ((float)(xmax - xmin));
    float yfac = ((float)recty) / ((float)(ymax - ymin));
#if 0
    if (G.debug & G_DEBUG_FREESTYLE) {
      printf("readColorPixels %d x %d @ (%d, %d) in %d x %d [%d x %d] -- %d x %d @ %d%%\n",
             w,
             h,
             x,
             y,
             xsch,
             ysch,
             xmax - xmin,
             ymax - ymin,
             rectx,
             recty,
             (int)(xfac * 100.0f));
    }
#endif
    int ii, jj;
    for (int j = 0; j < h; j++) {
      jj = (int)((y - ymin + j) * yfac);
      if (jj < 0 || jj >= recty) {
        continue;
      }
      for (int i = 0; i < w; i++) {
        ii = (int)((x - xmin + i) * xfac);
        if (ii < 0 || ii >= rectx) {
          continue;
        }
        memcpy(rgb + (w * j + i) * 3, _pass_diffuse.buf + (rectx * jj + ii) * 3, sizeof(float[3]));
      }
    }
  }
  oImage.setArray(rgb, xsch, ysch, w, h, x, y, false);
}

void AppCanvas::readDepthPixels(int x, int y, int w, int h, GrayImage &oImage) const
{
  float *z = new float[w * h];
  memset(z, 0, sizeof(float) * w * h);
  int xsch = width();
  int ysch = height();
  if (_pass_z.buf) {
    int xmin = border().getMin().x();
    int ymin = border().getMin().y();
    int xmax = border().getMax().x();
    int ymax = border().getMax().y();
    int rectx = _pass_z.width;
    int recty = _pass_z.height;
    float xfac = ((float)rectx) / ((float)(xmax - xmin));
    float yfac = ((float)recty) / ((float)(ymax - ymin));
#if 0
    if (G.debug & G_DEBUG_FREESTYLE) {
      printf("readDepthPixels %d x %d @ (%d, %d) in %d x %d [%d x %d] -- %d x %d @ %d%%\n",
             w,
             h,
             x,
             y,
             xsch,
             ysch,
             xmax - xmin,
             ymax - ymin,
             rectx,
             recty,
             (int)(xfac * 100.0f));
    }
#endif
    int ii, jj;
    for (int j = 0; j < h; j++) {
      jj = (int)((y - ymin + j) * yfac);
      if (jj < 0 || jj >= recty) {
        continue;
      }
      for (int i = 0; i < w; i++) {
        ii = (int)((x - xmin + i) * xfac);
        if (ii < 0 || ii >= rectx) {
          continue;
        }
        z[w * j + i] = _pass_z.buf[rectx * jj + ii];
      }
    }
  }
  oImage.setArray(z, xsch, ysch, w, h, x, y, false);
}

void AppCanvas::RenderStroke(Stroke *iStroke)
{
  if (_basic) {
    iStroke->RenderBasic(_Renderer);
  }
  else {
    iStroke->Render(_Renderer);
  }
}

void AppCanvas::update()
{
}

} /* namespace Freestyle */
