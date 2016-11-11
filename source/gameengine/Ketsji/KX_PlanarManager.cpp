/*
* ***** BEGIN GPL LICENSE BLOCK *****
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software Foundation,
* Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
* Contributor(s): Ulysse Martin, Tristan Porteries, Martins Upitis.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** file gameengine/Ketsji/KX_PlanarManager.cpp
*  ingroup ketsji
*/

#include "KX_PlanarManager.h"
#include "KX_Camera.h"
#include "KX_Scene.h"
#include "KX_Globals.h"
#include "KX_Planar.h"

#include "EXP_ListValue.h"

#include "RAS_IRasterizer.h"
#include "RAS_Texture.h"

#include "DNA_texture_types.h"

KX_PlanarManager::KX_PlanarManager(KX_Scene *scene)
	:m_scene(scene)
{
	const RAS_CameraData& camdata = RAS_CameraData();
	m_camera = new KX_Camera(m_scene, KX_Scene::m_callbacks, camdata, true, true);
	m_camera->SetName("__planar_cam__");
}

KX_PlanarManager::~KX_PlanarManager()
{
	for (std::vector<KX_Planar *>::iterator it = m_planars.begin(), end = m_planars.end(); it != end; ++it) {
		delete *it;
	}

	m_camera->Release();
}

void KX_PlanarManager::AddPlanar(RAS_Texture *texture, KX_GameObject *gameobj, RAS_IPolyMaterial *polymat, short type, int width, int height)
{
	/* Don't Add Planar several times for the same texture. If the texture is shared by several objects,
	 * we just add a "textureUser" to signal that the planar texture will be shared by several objects.
	 */
	for (std::vector<KX_Planar *>::iterator it = m_planars.begin(), end = m_planars.end(); it != end; ++it) {
		KX_Planar *planar = *it;
		const std::vector<RAS_Texture *>& textures = planar->GetTextureUsers();
		for (std::vector<RAS_Texture *>::const_iterator it = textures.begin(), end = textures.end(); it != end; ++it) {
			if ((*it)->GetTex() == texture->GetTex()) {
				planar->AddTextureUser(texture);
				return;
			}
		}
	}

	Tex *tex = texture->GetTex();
	KX_Planar *kxplanar = new KX_Planar(tex, gameobj, polymat, type, width, height);
	kxplanar->AddTextureUser(texture);
	texture->SetPlanar(kxplanar);
	m_planars.push_back(kxplanar);
}

void KX_PlanarManager::RenderPlanar(RAS_IRasterizer *rasty, KX_Planar *planar)
{
	KX_GameObject *mirror = planar->GetMirrorObject();
	KX_GameObject *observer = m_scene->GetActiveCamera();

	// Doesn't need (or can) update.
	if (!planar->NeedUpdate() || !planar->GetEnabled() || !mirror) {
		return;
	}

	// mirror mode, compute camera position and orientation
	// convert mirror position and normal in world space
	const MT_Matrix3x3 & mirrorObjWorldOri = mirror->GetSGNode()->GetWorldOrientation();
	const MT_Vector3 & mirrorObjWorldPos = mirror->GetSGNode()->GetWorldPosition();
	const MT_Vector3 & mirrorObjWorldScale = mirror->GetSGNode()->GetWorldScaling();
	MT_Vector3 mirrorWorldPos =
		mirrorObjWorldPos + mirrorObjWorldScale * (mirrorObjWorldOri * planar->GetMirrorPos());
	MT_Vector3 mirrorWorldZ = mirrorObjWorldOri * planar->GetMirrorZ();
	// get observer world position
	const MT_Vector3 & observerWorldPos = observer->GetSGNode()->GetWorldPosition();
	// get plane D term = mirrorPos . normal
	MT_Scalar mirrorPlaneDTerm = mirrorWorldPos.dot(mirrorWorldZ);
	// compute distance of observer to mirror = D - observerPos . normal
	MT_Scalar observerDistance = mirrorPlaneDTerm - observerWorldPos.dot(mirrorWorldZ);
	// if distance < 0.01 => observer is on wrong side of mirror, don't render
	if (observerDistance < 0.01f) {
		return;
	}

	MT_Matrix3x3 m1 = mirror->NodeGetWorldOrientation();
	MT_Matrix3x3 m2 = m1;
	m2.invert();

	static const MT_Matrix3x3 r180 = MT_Matrix3x3(-1.0f, 0.0f, 0.0f,
	                                               0.0f, 1.0f, 0.0f,
	                                               0.0f, 0.0f, -1.0f);
	
	static const MT_Matrix3x3 unmir = MT_Matrix3x3(-1.0f, 0.0f, 0.0f,
	                                                0.0f, 1.0f, 0.0f,
	                                                0.0f, 0.0f, 1.0f);

	MT_Matrix3x3 ori = observer->NodeGetWorldOrientation();
	MT_Vector3 cameraWorldPos = observerWorldPos;

	if (planar->GetPlanarType() == TEX_PLANAR_REFLECTION) {
		cameraWorldPos = (observerWorldPos - mirror->GetSGNode()->GetWorldPosition()) * m1;
		cameraWorldPos = mirror->GetSGNode()->GetWorldPosition() + cameraWorldPos * r180 * unmir * m2;
		ori.transpose();
		ori = ori * m1 * r180 * unmir * m2;
		ori.transpose();
	}

	// Set Render camera position and orientation
	m_camera->GetSGNode()->SetLocalPosition(cameraWorldPos);
	m_camera->GetSGNode()->SetLocalOrientation(ori);

	m_camera->GetSGNode()->UpdateWorldData(0.0);

	// Begin rendering stuff
	planar->BeginRender();
	planar->BindFace(rasty);

	//rasty->BeginFrame(KX_GetActiveEngine()->GetClockTime());

	rasty->SetViewport(0, 0, planar->GetWidth(), planar->GetHeight());
	//rasty->SetScissor(0, 0, planar->GetWidth(), planar->GetHeight());

	//m_scene->GetWorldInfo()->UpdateWorldSettings(rasty);
	//rasty->SetAuxilaryClientInfo(m_scene);
	//rasty->DisplayFog();

	/* When we update clipstart or clipend values,
	* or if the projection matrix is not computed yet,
	* we have to compute projection matrix.
	*/
	if (planar->GetInvalidProjectionMatrix()) {
		const float clipstart = planar->GetClipStart();
		const float clipend = planar->GetClipEnd();
		const MT_Matrix4x4& proj = rasty->GetFrustumMatrix(-clipstart, clipstart, -clipstart, clipstart, clipstart, clipend, 1.0f, true);
		planar->SetProjectionMatrix(proj);
		planar->SetInvalidProjectionMatrix(false);
	}

	MT_Matrix4x4 projmat = planar->GetProjectionMatrix();

	m_camera->SetProjectionMatrix(projmat);


	MT_Transform camtrans(m_camera->GetWorldToCamera());
	MT_Matrix4x4 viewmat(camtrans);

	rasty->SetViewMatrix(viewmat, m_camera->NodeGetWorldOrientation(), m_camera->NodeGetWorldPosition(), m_camera->NodeGetLocalScaling(), m_camera->GetCameraData()->m_perspective);
	m_camera->SetModelviewMatrix(viewmat);


	m_scene->CalculateVisibleMeshes(rasty, m_camera, ~planar->GetIgnoreLayers());

	KX_GetActiveEngine()->UpdateAnimations(m_scene);

	planar->EnableClipPlane(mirrorWorldZ, mirrorPlaneDTerm, planar->GetPlanarType());

	for (std::vector<KX_Planar *>::iterator it = m_planars.begin(), end = m_planars.end(); it != end; ++it) {
		KX_Planar *p = *it;
		if (p->GetPlanarType() == TEX_PLANAR_REFRACTION) {
			p->GetMirrorObject()->SetVisible(false, false);
		}
		else {
			if (p != planar && planar->GetCullReflections()) {
				p->GetMirrorObject()->SetVisible(false, false);
			}
		}
	}

	// Now the objects are culled and we can render the scene.
	m_scene->GetWorldInfo()->RenderBackground(rasty);

	m_scene->RenderBuckets(camtrans, rasty);

	planar->EndRender();

	for (std::vector<KX_Planar *>::iterator it = m_planars.begin(), end = m_planars.end(); it != end; ++it) {
		KX_Planar *p = *it;
		if (p->GetPlanarType() == TEX_PLANAR_REFRACTION) {
			p->GetMirrorObject()->SetVisible(true, false);
		}
		else {
			if (p != planar && planar->GetCullReflections()) {
				p->GetMirrorObject()->SetVisible(true, false);
			}
		}
	}

	planar->DisableClipPlane(planar->GetPlanarType());

}

void KX_PlanarManager::Render(RAS_IRasterizer *rasty)
{
	if (m_planars.size() == 0 || rasty->GetDrawingMode() != RAS_IRasterizer::RAS_TEXTURED) {
		return;
	}

	// Disable scissor to not bother with scissor box.
	rasty->Disable(RAS_IRasterizer::RAS_SCISSOR_TEST);

	// Copy current stereo mode.
	const RAS_IRasterizer::StereoMode steremode = rasty->GetStereoMode();
	// Disable stereo for realtime planar.
	rasty->SetStereoMode(RAS_IRasterizer::RAS_STEREO_NOSTEREO);

	for (std::vector<KX_Planar *>::iterator it = m_planars.begin(), end = m_planars.end(); it != end; ++it) {
		RenderPlanar(rasty, *it);
	}

	// Restore previous stereo mode.
	rasty->SetStereoMode(steremode);

	rasty->Enable(RAS_IRasterizer::RAS_SCISSOR_TEST);
}
