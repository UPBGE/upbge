#ifndef __RAS_SCREEN_PASS_COLLECTION_H__
#define __RAS_SCREEN_PASS_COLLECTION_H__

#include <vector>

class RAS_ScreenPass;
class RAS_Rasterizer;
class RAS_ICanvas;
class RAS_OffScreen;

class RAS_ScreenPassCollection
{
private:
	std::vector<RAS_ScreenPass *> m_passes;

	RAS_Rasterizer *m_rasty;
	RAS_ICanvas *m_canvas;

	/// The filter color input off screen, changed for each pass.
	RAS_OffScreen *m_colorOfs;
	/// The filter depth input off scree, unchanged for each pass.
	RAS_OffScreen *m_depthOfs;

	RAS_OffScreen *m_outputOfs;

public:
	RAS_ScreenPassCollection(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *input, RAS_OffScreen *output);

	void AddPass(RAS_ScreenPass *pass);
	RAS_OffScreen *Execute();
};

#endif  // __RAS_SCREEN_PASS_COLLECTION_H__
