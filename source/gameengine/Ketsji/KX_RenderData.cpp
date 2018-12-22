#include "KX_RenderData.h"
#include "KX_Camera.h"

KX_CameraRenderData::KX_CameraRenderData(KX_Camera *rendercam, KX_Camera *cullingcam, const RAS_Rect& area,
		const RAS_Rect& viewport, RAS_Rasterizer::StereoMode stereoMode, RAS_Rasterizer::StereoEye eye, unsigned short index)
	:m_renderCamera(rendercam),
	m_cullingCamera(cullingcam),
	m_area(area),
	m_viewport(viewport),
	m_stereoMode(stereoMode),
	m_eye(eye),
	m_index(index)
{
	m_renderCamera->AddRef();
}

KX_CameraRenderData::KX_CameraRenderData(const KX_CameraRenderData& other)
	:m_renderCamera(CM_AddRef(other.m_renderCamera)),
	m_cullingCamera(other.m_cullingCamera),
	m_area(other.m_area),
	m_viewport(other.m_viewport),
	m_stereoMode(other.m_stereoMode),
	m_eye(other.m_eye),
	m_index(other.m_index)
{
}

KX_CameraRenderData::~KX_CameraRenderData()
{
	m_renderCamera->Release();
}

KX_SceneRenderData::KX_SceneRenderData(KX_Scene *scene)
	:m_scene(scene)
{
}

KX_FrameRenderData::KX_FrameRenderData(RAS_OffScreen::Type ofsType, const std::vector<RAS_Rasterizer::StereoEye>& eyes)
	:m_ofsType(ofsType),
	m_eyes(eyes)
{
}

KX_RenderData::KX_RenderData(RAS_Rasterizer::StereoMode stereoMode, bool renderPerEye)
	:m_stereoMode(stereoMode),
	m_renderPerEye(renderPerEye)
{
}
