#ifndef __KX_RENDER_DATA_H__
#define __KX_RENDER_DATA_H__

#include "RAS_Rasterizer.h"
#include "RAS_FramingManager.h"
#include "RAS_OffScreen.h"
#include "SG_Frustum.h"

#include <functional>

class KX_Scene;
class KX_Camera;

/** \brief This file contains all the scheduling data describing the rendering proceeded in a frame.
 * KX_RenderSchedule is the main scheduler which for each eye (in case of stereo) contains a frame
 * and each of these frame contains the scenes and cameras.
 */

/** Info for off screen rendering of shadow and texture map.
 */
struct KX_TextureRenderSchedule
{
	enum Mode {
		MODE_NONE = 0,
		/// Render world background.
		MODE_RENDER_WORLD = (1 << 0),
		/// Update object lod.
		MODE_UPDATE_LOD = (1 << 1)
	}
	/// Rendering/update options.
	m_mode;

	/// Clear options to process at render begining.
	RAS_Rasterizer::ClearBit m_clearMode;
	/// Drawing mode.
	RAS_Rasterizer::DrawType m_drawingMode;

	/// View model matrix.
	mt::mat4 m_viewMatrix;
	/// View projection matrix.
	mt::mat4 m_progMatrix;
	/// View transform.
	mt::mat3x4 m_camTrans;
	/// View position.
	mt::vec3 m_position;

	/// Frustum culling info.
	SG_Frustum m_frustum;
	/// Visible layers to render.
	unsigned int m_visibleLayers;

	/// Distance factor used when computing lod.
	float m_lodFactor;

	/// Stereo eye.
	RAS_Rasterizer::StereoEye m_eye;
	/// Viewport index used while rendering this texture.
	unsigned short m_index;

	/// Custom function called before the rendering after matrix setup.
	std::function<void (RAS_Rasterizer *)> m_bind;
	/// Custom function called after rendering.
	std::function<void (RAS_Rasterizer *)> m_unbind;
};

using KX_TextureRenderScheduleList = std::vector<KX_TextureRenderSchedule>;

/** Info for camera rendering.
 */
struct KX_CameraRenderSchedule
{
	/// View model matrix.
	mt::mat4 m_viewMatrix;
	/// View projection matrix.
	mt::mat4 m_progMatrix;
	/// View transform.
	mt::mat3x4 m_camTrans;
	/// View position.
	mt::vec3 m_position;
	/// True if view is using negative scale.
	bool m_negScale;
	/// True if the projection is perspective.
	bool m_perspective;

	/// Camera passed to scene callbacks.
	KX_Camera *m_camera;
	/// Frame (screen area) frustum.
	RAS_FrameFrustum m_frameFrustum;
	/// Frustum culling info.
	SG_Frustum m_frustum;
	/// True if testing object culling.
	bool m_culling;

	/// Display area.
	RAS_Rect m_area;
	/// Viewport area.
	RAS_Rect m_viewport;

	/// Distance factor used when computing lod.
	float m_lodFactor;

	/// Rendering stereo mode.
	RAS_Rasterizer::StereoMode m_stereoMode;
	/// Stereo eye.
	RAS_Rasterizer::StereoEye m_eye;
	/// Stereo focal length.
	float m_focalLength;
	// Index of the camera in all the scene's cameras rendered.
	unsigned short m_index;
};

using KX_CameraRenderScheduleList = std::vector<KX_CameraRenderSchedule>;

/** Scene render info.
 * Contains cameras and textures schedulers.
 */
struct KX_SceneRenderSchedule
{
	KX_Scene *m_scene;
	KX_TextureRenderScheduleList m_textureSchedules;
	// Use multiple list of cameras in case of per eye stereo.
	KX_CameraRenderScheduleList m_cameraSchedules[RAS_Rasterizer::RAS_STEREO_MAXEYE];
};

using KX_SceneRenderScheduleList = std::vector<KX_SceneRenderSchedule>;

/** Info about usage of an off screen.
 * In case of stereo requiring compositing, two frames are used for one off screen
 * per eye. In case of regular render only one frame is used.
 */
struct KX_FrameRenderSchedule
{
	/// Off screen type targeted.
	RAS_OffScreen::Type m_ofsType;
	/// Eyes to render in this frame.
	std::vector<RAS_Rasterizer::StereoEye> m_eyes;
};

using KX_FrameRenderScheduleList = std::vector<KX_FrameRenderSchedule>;

/** Root render scheduler info.
 * Contains frame and scene schedulers.
 */
struct KX_RenderSchedule
{
	/// Frame border size and color.
	RAS_FrameSettings m_frameSettings;
	/// Rendering stereo mode.
	RAS_Rasterizer::StereoMode m_stereoMode;
	/// True if uses two frames for each stereo eye.
	bool m_renderPerEye;

	/// Scene info to render.
	KX_SceneRenderScheduleList m_sceneSchedules;
	/// Frame used to render.
	KX_FrameRenderScheduleList m_frameSchedules;
};

#endif  // __KX_RENDER_DATA_H__
