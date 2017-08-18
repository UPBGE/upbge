#include "RAS_ScreenPassCollection.h"
#include "RAS_ScreenPass.h"
#include "RAS_Rasterizer.h"
#include "RAS_OffScreen.h"

RAS_ScreenPassCollection::RAS_ScreenPassCollection(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *input, RAS_OffScreen *output)
	:m_rasty(rasty),
	m_canvas(canvas),
	m_colorOfs(input),
	m_depthOfs(input),
	m_outputOfs(output)
{
}

void RAS_ScreenPassCollection::AddPass(RAS_ScreenPass *pass)
{
	m_passes.push_back(pass);
}

RAS_OffScreen *RAS_ScreenPassCollection::Execute()
{
	// Return input off screen if there's no passes.
	if (m_passes.size() == 0) {
		return m_colorOfs;
	}

	m_rasty->Disable(RAS_Rasterizer::RAS_CULL_FACE);
	m_rasty->Disable(RAS_Rasterizer::RAS_DEPTH_TEST);
	m_rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);
	m_rasty->Disable(RAS_Rasterizer::RAS_BLEND);
	m_rasty->Disable(RAS_Rasterizer::RAS_ALPHA_TEST);

	m_rasty->SetLines(false);

	/* Set source off screen to RAS_OFFSCREEN_FILTER0 in case of multisample and blit,
	 * else keep the original source off screen. */
	if (m_colorOfs->GetSamples()) {
		RAS_OffScreen *ofs = m_rasty->GetOffScreen(RAS_Rasterizer::RAS_OFFSCREEN_FILTER0);
		// No need to bind previousofs because a blit is proceeded.
		m_rasty->DrawOffScreen(m_colorOfs, ofs);
		m_colorOfs = m_depthOfs = ofs;
	}

	// Used to know if a pass is the last of the list.
	RAS_ScreenPass *lastPass = m_passes.back();

	for (RAS_ScreenPass *pass : m_passes) {
		RAS_OffScreen *targetofs;
		// Computing the pass targeted off screen.
		if (pass == lastPass) {
			// Render to the targeted off screen for the last pass.
			targetofs = m_outputOfs;
		}
		else {
			// Else render to the next off screen compared to the input off screen.
			targetofs = m_rasty->GetOffScreen(RAS_Rasterizer::NextFilterOffScreen(m_colorOfs->GetType()));
		}

		/* Get the output off screen of the pass, could be the same as the input off screen
		 * if no modifications were made or the targeted off screen.
		 * This output off screen is used for the next pass as input off screen */
		m_colorOfs = pass->Draw(m_rasty, m_canvas, m_depthOfs, m_colorOfs, targetofs);
	}

	// The last pass uses its own off screen and didn't render to the targeted off screen ?
	if (m_colorOfs != m_outputOfs) {
		// Render manually to the targeted off screen as the last pass didn't do it for us.
		m_outputOfs->Bind();
		m_rasty->DrawOffScreen(m_colorOfs, m_outputOfs);
	}

	m_rasty->Enable(RAS_Rasterizer::RAS_DEPTH_TEST);
	m_rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
	m_rasty->Enable(RAS_Rasterizer::RAS_CULL_FACE);

	return m_outputOfs;
}
