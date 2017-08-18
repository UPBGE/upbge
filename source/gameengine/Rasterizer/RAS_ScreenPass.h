#ifndef __RAS_SCREEN_PASS_H__
#define __RAS_SCREEN_PASS_H__

class RAS_Rasterizer;
class RAS_ICanvas;
class RAS_OffScreen;

class RAS_ScreenPass
{
public:
	RAS_ScreenPass() = default;
	virtual ~RAS_ScreenPass() = default;

	/** Draw a screen pass.
	 * \param rasty The used rasterizer to call draw commands.
	 * \param canvas The canvas containing screen viewport.
	 * \param detphofs The off screen used only for the depth texture input,
	 * the same for all passes of a scene.
	 * \param colorofs The off screen used only for the color texture input, unique per pass.
	 * \param targetofs The off screen used to draw the pass to.
	 * \return The off screen to use as input for the next pass.
	 */
	virtual RAS_OffScreen *Draw(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *detphofs,
			   RAS_OffScreen *colorofs, RAS_OffScreen *targetofs) = 0;
};

#endif  // __RAS_SCREEN_PASS_H__
