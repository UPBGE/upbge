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
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_ObstacleSimulation.cpp
 *  \ingroup ketsji
 *
 * Simulation for obstacle avoidance behavior
 */

#include "KX_ObstacleSimulation.h"
#include "KX_NavMeshObject.h"
#include "KX_Globals.h"
#include "DNA_object_types.h"
#include "BLI_math.h"

namespace
{
inline float perp(const mt::vec2& a, const mt::vec2& b)
{
	return a.x * b.y - a.y * b.x;
}

inline float sqr(float x)
{
	return x * x;
}
inline float lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}
inline float clamp(float a, float mn, float mx)
{
	return a < mn ? mn : (a > mx ? mx : a);
}
}

static int sweepCircleCircle(const mt::vec2 &pos0, const float r0, const mt::vec2 &v,
                             const mt::vec2 &pos1, const float r1,
                             float& tmin, float& tmax)
{
	static const float EPS = 0.0001f;
	mt::vec2 c0(pos0.x, pos0.y);
	mt::vec2 c1(pos1.x, pos1.y);
	mt::vec2 s = c1 - c0;
	float r = r0 + r1;
	float c = s.LengthSquared() - r * r;
	float a = v.LengthSquared();
	if (a < EPS) {
		return 0;           // not moving

	}
	// Overlap, calc time to exit.
	float b = mt::dot(v, s);
	float d = b * b - a * c;
	if (d < 0.0f) {
		return 0;           // no intersection.
	}
	tmin = (b - sqrtf(d)) / a;
	tmax = (b + sqrtf(d)) / a;
	return 1;
}

static int sweepCircleSegment(const mt::vec2 &pos0, const float r0, const mt::vec2 &v,
                              const mt::vec2& pa, const mt::vec2 &pb, const float sr,
                              float& tmin, float &tmax)
{
	// equation parameters
	mt::vec2 c0(pos0.x, pos0.y);
	mt::vec2 sa(pa.x, pa.y);
	mt::vec2 sb(pb.x, pb.y);
	mt::vec2 L = sb - sa;
	mt::vec2 H = c0 - sa;
	float radius = r0 + sr;
	float l2 = L.LengthSquared();
	float r2 = radius * radius;
	float dl = perp(v, L);
	float hl = perp(H, L);
	float a = dl * dl;
	float b = 2.0f * hl * dl;
	float c = hl * hl - (r2 * l2);
	float d = (b * b) - (4.0f * a * c);

	// infinite line missed by infinite ray.
	if (d < 0.0f) {
		return 0;
	}

	d = sqrtf(d);
	tmin = (-b - d) / (2.0f * a);
	tmax = (-b + d) / (2.0f * a);

	// line missed by ray range.
	/*	if (tmax < 0.0f || tmin > 1.0f)
	   return 0;*/

	// find what part of the ray was collided.
	mt::vec2 Pedge;
	Pedge = c0 + v * tmin;
	H = Pedge - sa;
	float e0 = mt::dot(H, L) / l2;
	Pedge = c0 + v * tmax;
	H = Pedge - sa;
	float e1 = mt::dot(H, L) / l2;

	if (e0 < 0.0f || e1 < 0.0f) {
		float ctmin, ctmax;
		if (sweepCircleCircle(pos0, r0, v, pa, sr, ctmin, ctmax)) {
			if (e0 < 0.0f && ctmin > tmin) {
				tmin = ctmin;
			}
			if (e1 < 0.0f && ctmax < tmax) {
				tmax = ctmax;
			}
		}
		else {
			return 0;
		}
	}

	if (e0 > 1.0f || e1 > 1.0f) {
		float ctmin, ctmax;
		if (sweepCircleCircle(pos0, r0, v, pb, sr, ctmin, ctmax)) {
			if (e0 > 1.0f && ctmin > tmin) {
				tmin = ctmin;
			}
			if (e1 > 1.0f && ctmax < tmax) {
				tmax = ctmax;
			}
		}
		else {
			return 0;
		}
	}

	return 1;
}

static bool inBetweenAngle(float a, float amin, float amax, float& t)
{
	if (amax < amin) {
		amax += (float)M_PI * 2;
	}
	if (a < amin - (float)M_PI) {
		a += (float)M_PI * 2;
	}
	if (a > amin + (float)M_PI) {
		a -= (float)M_PI * 2;
	}
	if (a >= amin && a < amax) {
		t = (a - amin) / (amax - amin);
		return true;
	}
	return false;
}

static float interpolateToi(float a, const float *dir, const float *toi, const int ntoi)
{
	for (int i = 0; i < ntoi; ++i)
	{
		int next = (i + 1) % ntoi;
		float t;
		if (inBetweenAngle(a, dir[i], dir[next], t)) {
			return lerp(toi[i], toi[next], t);
		}
	}
	return 0;
}

KX_ObstacleSimulation::KX_ObstacleSimulation(float levelHeight, bool enableVisualization)
	:m_levelHeight(levelHeight)
	,   m_enableVisualization(enableVisualization)
{

}

KX_ObstacleSimulation::~KX_ObstacleSimulation()
{
	for (size_t i = 0; i < m_obstacles.size(); i++)
	{
		KX_Obstacle *obs = m_obstacles[i];
		delete obs;
	}
	m_obstacles.clear();
}
KX_Obstacle *KX_ObstacleSimulation::CreateObstacle(KX_GameObject *gameobj)
{
	KX_Obstacle *obstacle = new KX_Obstacle();
	obstacle->m_gameObj = gameobj;

	obstacle->vel = mt::zero2;
	obstacle->pvel = mt::zero2;
	obstacle->dvel = mt::zero2;
	obstacle->nvel = mt::zero2;
	for (int i = 0; i < VEL_HIST_SIZE; ++i) {
		obstacle->hvel[i] = mt::zero2;
	}
	obstacle->hhead = 0;

	m_obstacles.push_back(obstacle);
	return obstacle;
}

void KX_ObstacleSimulation::AddObstacleForObj(KX_GameObject *gameobj)
{
	KX_Obstacle *obstacle = CreateObstacle(gameobj);
	struct Object *blenderobject = gameobj->GetBlenderObject();
	obstacle->m_type = KX_OBSTACLE_OBJ;
	obstacle->m_shape = KX_OBSTACLE_CIRCLE;
	obstacle->m_rad = blenderobject->obstacleRad;
}

void KX_ObstacleSimulation::AddObstaclesForNavMesh(KX_NavMeshObject *navmeshobj)
{
	dtStatNavMesh *navmesh = navmeshobj->GetNavMesh();
	if (navmesh) {
		int npoly = navmesh->getPolyCount();
		for (int pi = 0; pi < npoly; pi++)
		{
			const dtStatPoly *poly = navmesh->getPoly(pi);

			for (int i = 0, j = (int)poly->nv - 1; i < (int)poly->nv; j = i++)
			{
				if (poly->n[j]) {
					continue;
				}
				const float *vj = navmesh->getVertex(poly->v[j]);
				const float *vi = navmesh->getVertex(poly->v[i]);

				KX_Obstacle *obstacle = CreateObstacle(navmeshobj);
				obstacle->m_type = KX_OBSTACLE_NAV_MESH;
				obstacle->m_shape = KX_OBSTACLE_SEGMENT;
				obstacle->m_pos = mt::vec3(vj[0], vj[2], vj[1]);
				obstacle->m_pos2 = mt::vec3(vi[0], vi[2], vi[1]);
				obstacle->m_rad = 0;
			}
		}
	}
}

void KX_ObstacleSimulation::DestroyObstacleForObj(KX_GameObject *gameobj)
{
	for (size_t i = 0; i < m_obstacles.size(); )
	{
		if (m_obstacles[i]->m_gameObj == gameobj) {
			KX_Obstacle *obstacle = m_obstacles[i];
			m_obstacles[i] = m_obstacles.back();
			m_obstacles.pop_back();
			delete obstacle;
		}
		else {
			i++;
		}
	}
}

void KX_ObstacleSimulation::UpdateObstacles()
{
	for (size_t i = 0; i < m_obstacles.size(); i++)
	{
		if (m_obstacles[i]->m_type == KX_OBSTACLE_NAV_MESH || m_obstacles[i]->m_shape == KX_OBSTACLE_SEGMENT) {
			continue;
		}

		KX_Obstacle *obs = m_obstacles[i];
		obs->m_pos = obs->m_gameObj->NodeGetWorldPosition();
		obs->vel = obs->m_gameObj->GetLinearVelocity().xy();

		// Update velocity history and calculate perceived (average) velocity.
		obs->hvel[obs->hhead] = obs->vel;
		obs->hhead = (obs->hhead + 1) % VEL_HIST_SIZE;
		obs->pvel = mt::zero2;
		for (int j = 0; j < VEL_HIST_SIZE; ++j) {
			obs->pvel += obs->hvel[j];
		}
		obs->pvel *= 1.0f / VEL_HIST_SIZE;
	}
}

KX_Obstacle *KX_ObstacleSimulation::GetObstacle(KX_GameObject *gameobj)
{
	for (size_t i = 0; i < m_obstacles.size(); i++)
	{
		if (m_obstacles[i]->m_gameObj == gameobj) {
			return m_obstacles[i];
		}
	}

	return nullptr;
}

void KX_ObstacleSimulation::AdjustObstacleVelocity(KX_Obstacle *activeObst, KX_NavMeshObject *activeNavMeshObj,
                                                   mt::vec3& velocity, float maxDeltaSpeed, float maxDeltaAngle)
{
}

void KX_ObstacleSimulation::DrawObstacles()
{
	if (!m_enableVisualization) {
		return;
	}
	static const mt::vec4 bluecolor(0.0f, 0.0f, 1.0f, 1.0f);
	static const int SECTORS_NUM = 32;
	for (size_t i = 0; i < m_obstacles.size(); i++)
	{
		if (m_obstacles[i]->m_shape == KX_OBSTACLE_SEGMENT) {
			mt::vec3 p1 = m_obstacles[i]->m_pos;
			mt::vec3 p2 = m_obstacles[i]->m_pos2;
			//apply world transform
			if (m_obstacles[i]->m_type == KX_OBSTACLE_NAV_MESH) {
				KX_NavMeshObject *navmeshobj = static_cast<KX_NavMeshObject *>(m_obstacles[i]->m_gameObj);
				p1 = navmeshobj->TransformToWorldCoords(p1);
				p2 = navmeshobj->TransformToWorldCoords(p2);
			}

			KX_RasterizerDrawDebugLine(p1, p2, bluecolor);
		}
		else if (m_obstacles[i]->m_shape == KX_OBSTACLE_CIRCLE) {
			const float radius = m_obstacles[i]->m_rad;
			const mt::vec3& pos = m_obstacles[i]->m_pos;
			const float delta = M_PI * 2.0f / SECTORS_NUM;
			for (unsigned short i = 0; i < SECTORS_NUM; ++i) {
				const float t1 = delta * i;
				const float t2 = delta * (i + 1);
				const mt::vec3 p1 = mt::vec3(cosf(t1), sinf(t1), 0.0f) * radius + pos;
				const mt::vec3 p2 = mt::vec3(cosf(t2), sinf(t2), 0.0f) * radius + pos;
				KX_RasterizerDrawDebugLine(p1, p2, bluecolor);
			}
		}
	}
}

static mt::vec3 nearestPointToObstacle(mt::vec3& pos, KX_Obstacle *obstacle)
{
	switch (obstacle->m_shape) {
		case KX_OBSTACLE_SEGMENT:
		{
			mt::vec3 ab = obstacle->m_pos2 - obstacle->m_pos;
			if (!mt::FuzzyZero(ab)) {
				const float dist = ab.Length();
				mt::vec3 abdir = ab.Normalized();
				mt::vec3 v = pos - obstacle->m_pos;
				float proj = mt::dot(abdir, v);
				CLAMP(proj, 0, dist);
				mt::vec3 res = obstacle->m_pos + abdir * proj;
				return res;
			}
			ATTR_FALLTHROUGH;
		}
		case KX_OBSTACLE_CIRCLE:
		default:
			return obstacle->m_pos;
	}
}

static bool filterObstacle(KX_Obstacle *activeObst, KX_NavMeshObject *activeNavMeshObj, KX_Obstacle *otherObst,
                           float levelHeight)
{
	//filter obstacles by type
	if ((otherObst == activeObst) ||
	    (otherObst->m_type == KX_OBSTACLE_NAV_MESH && otherObst->m_gameObj != activeNavMeshObj)) {
		return false;
	}

	//filter obstacles by position
	mt::vec3 p = nearestPointToObstacle(activeObst->m_pos, otherObst);
	if (fabsf(activeObst->m_pos.z - p.z) > levelHeight) {
		return false;
	}

	return true;
}

///////////*********TOI_rays**********/////////////////
KX_ObstacleSimulationTOI::KX_ObstacleSimulationTOI(float levelHeight, bool enableVisualization)
	:KX_ObstacleSimulation(levelHeight, enableVisualization),
	m_maxSamples(32),
	m_minToi(0.0f),
	m_maxToi(0.0f),
	m_velWeight(1.0f),
	m_curVelWeight(1.0f),
	m_toiWeight(1.0f),
	m_collisionWeight(1.0f)
{
}


void KX_ObstacleSimulationTOI::AdjustObstacleVelocity(KX_Obstacle *activeObst, KX_NavMeshObject *activeNavMeshObj,
                                                      mt::vec3& velocity, float maxDeltaSpeed, float maxDeltaAngle)
{
	int nobs = m_obstacles.size();
	int obstidx = std::find(m_obstacles.begin(), m_obstacles.end(), activeObst) - m_obstacles.begin();
	if (obstidx == nobs) {
		return;
	}

	activeObst->dvel = velocity.xy();

	//apply RVO
	sampleRVO(activeObst, activeNavMeshObj, maxDeltaAngle);

	// Fake dynamic constraint.
	mt::vec2 dv = activeObst->nvel - activeObst->vel;
	const float ds = dv.Length();
	if (ds > maxDeltaSpeed || ds < -maxDeltaSpeed) {
		dv *= fabsf(maxDeltaSpeed / ds);
	}
	const mt::vec2 vel = activeObst->vel + dv;

	velocity.x = vel.x;
	velocity.y = vel.y;
}

///////////*********TOI_rays**********/////////////////
static const int AVOID_MAX_STEPS = 128;
struct TOICircle {
	TOICircle() :n(0), minToi(0), maxToi(1)
	{
	}
	float toi[AVOID_MAX_STEPS];     // Time of impact (seconds)
	float toie[AVOID_MAX_STEPS];    // Time of exit (seconds)
	float dir[AVOID_MAX_STEPS];     // Direction (radians)
	int n;                          // Number of samples
	float minToi, maxToi;           // Min/max TOI (seconds)
};

KX_ObstacleSimulationTOI_rays::KX_ObstacleSimulationTOI_rays(float levelHeight, bool enableVisualization) :
	KX_ObstacleSimulationTOI(levelHeight, enableVisualization)
{
	m_maxSamples = 32;
	m_minToi = 0.5f;
	m_maxToi = 1.2f;
	m_velWeight = 4.0f;
	m_toiWeight = 1.0f;
	m_collisionWeight = 100.0f;
}


void KX_ObstacleSimulationTOI_rays::sampleRVO(KX_Obstacle *activeObst, KX_NavMeshObject *activeNavMeshObj,
                                              const float maxDeltaAngle)
{
	mt::vec2 vel = activeObst->dvel;
	float vmax = (float)vel.Length();
	float odir = (float)atan2(vel.y, vel.x);

	mt::vec2 ddir = vel;
	ddir.Normalize();

	float bestScore = FLT_MAX;
	float bestDir = odir;
	float bestToi = 0;

	TOICircle tc;
	tc.n = m_maxSamples;
	tc.minToi = m_minToi;
	tc.maxToi = m_maxToi;

	const int iforw = m_maxSamples / 2;
	const float aoff = (float)iforw / (float)m_maxSamples;

	size_t nobs = m_obstacles.size();
	for (int iter = 0; iter < m_maxSamples; ++iter)
	{
		// Calculate sample velocity
		const float ndir = ((float)iter / (float)m_maxSamples) - aoff;
		const float dir = odir + ndir * (float)M_PI * 2.0f;
		mt::vec2 svel;
		svel.x = cosf(dir) * vmax;
		svel.y = sinf(dir) * vmax;

		// Find min time of impact and exit amongst all obstacles.
		float tmin = m_maxToi;
		float tmine = 0.0f;
		for (int i = 0; i < nobs; ++i)
		{
			KX_Obstacle *ob = m_obstacles[i];
			bool res = filterObstacle(activeObst, activeNavMeshObj, ob, m_levelHeight);
			if (!res) {
				continue;
			}

			float htmin, htmax;

			if (ob->m_shape == KX_OBSTACLE_CIRCLE) {
				mt::vec2 vab;
				if (ob->vel.Length() < 0.01f * 0.01f) {
					// Stationary, use VO
					vab = svel;
				}
				else {
					// Moving, use RVO
					vab = (2.0f * svel) - vel - mt::vec2(ob->vel);
				}

				if (!sweepCircleCircle(activeObst->m_pos.xy(), activeObst->m_rad,
				                       vab, ob->m_pos.xy(), ob->m_rad, htmin, htmax)) {
					continue;
				}
			}
			else if (ob->m_shape == KX_OBSTACLE_SEGMENT) {
				mt::vec3 p1 = ob->m_pos;
				mt::vec3 p2 = ob->m_pos2;
				//apply world transform
				if (ob->m_type == KX_OBSTACLE_NAV_MESH) {
					KX_NavMeshObject *navmeshobj = static_cast<KX_NavMeshObject *>(ob->m_gameObj);
					p1 = navmeshobj->TransformToWorldCoords(p1);
					p2 = navmeshobj->TransformToWorldCoords(p2);
				}

				if (!sweepCircleSegment(activeObst->m_pos.xy(), activeObst->m_rad, svel,
				                        p1.xy(), p2.xy(), ob->m_rad, htmin, htmax)) {
					continue;
				}
			}
			else {
				continue;
			}

			if (htmin > 0.0f) {
				// The closest obstacle is somewhere ahead of us, keep track of nearest obstacle.
				if (htmin < tmin) {
					tmin = htmin;
				}
			}
			else if (htmax > 0.0f) {
				// The agent overlaps the obstacle, keep track of first safe exit.
				if (htmax > tmine) {
					tmine = htmax;
				}
			}
		}

		// Calculate sample penalties and final score.
		const float apen = m_velWeight * fabsf(ndir);
		const float tpen = m_toiWeight * (1.0f / (0.0001f + tmin / m_maxToi));
		const float cpen = m_collisionWeight * (tmine / m_minToi) * (tmine / m_minToi);
		const float score = apen + tpen + cpen;

		// Update best score.
		if (score < bestScore) {
			bestDir = dir;
			bestToi = tmin;
			bestScore = score;
		}

		tc.dir[iter] = dir;
		tc.toi[iter] = tmin;
		tc.toie[iter] = tmine;
	}

	if (activeObst->vel.Length() > 0.1f) {
		// Constrain max turn rate.
		float cura = atan2(activeObst->vel.x, activeObst->vel.y);
		float da = bestDir - cura;
		if (da < -M_PI) {
			da += (float)M_PI * 2;
		}
		if (da > M_PI) {
			da -= (float)M_PI * 2;
		}
		if (da < -maxDeltaAngle) {
			bestDir = cura - maxDeltaAngle;
			bestToi = std::min(bestToi, interpolateToi(bestDir, tc.dir, tc.toi, tc.n));
		}
		else if (da > maxDeltaAngle) {
			bestDir = cura + maxDeltaAngle;
			bestToi = std::min(bestToi, interpolateToi(bestDir, tc.dir, tc.toi, tc.n));
		}
	}

	// Adjust speed when time of impact is less than min TOI.
	if (bestToi < m_minToi) {
		vmax *= bestToi / m_minToi;
	}

	// New steering velocity.
	activeObst->nvel.x = cosf(bestDir) * vmax;
	activeObst->nvel.y = sinf(bestDir) * vmax;
}

///////////********* TOI_cells**********/////////////////

static void processSamples(KX_Obstacle *activeObst, KX_NavMeshObject *activeNavMeshObj,
                           KX_Obstacles& obstacles,  float levelHeight, const float vmax,
                           mt::vec2 *spos, const float cs, const int nspos, mt::vec2& res,
                           float maxToi, float velWeight, float curVelWeight, float sideWeight,
                           float toiWeight)
{
	res = mt::zero2;

	const float ivmax = 1.0f / vmax;
	const mt::vec2 activeObstPos = activeObst->m_pos.xy();

	float minPenalty = FLT_MAX;

	for (int n = 0; n < nspos; ++n)
	{
		const mt::vec2& vcand = spos[n];

		// Find min time of impact and exit amongst all obstacles.
		float tmin = maxToi;
		float side = 0;
		int nside = 0;

		for (int i = 0; i < obstacles.size(); ++i)
		{
			KX_Obstacle *ob = obstacles[i];
			bool found = filterObstacle(activeObst, activeNavMeshObj, ob, levelHeight);
			if (!found) {
				continue;
			}
			float htmin, htmax;

			if (ob->m_shape == KX_OBSTACLE_CIRCLE) {
				// Moving, use RVO
				const mt::vec2 vab = vcand * 2.0f - activeObst->vel - ob->vel;

				// Side
				// NOTE: dp, and dv are constant over the whole calculation,
				// they can be precomputed per object.
				const mt::vec2 pb = ob->m_pos.xy();

				const float orig[2] = {0, 0};
				const mt::vec2 dp = (pb - activeObstPos).Normalized();
				const mt::vec2 dv = ob->dvel - activeObst->dvel;
				mt::vec2 np;

				/* TODO: use line_point_side_v2 */
				if (area_tri_signed_v2(orig, dp.Data(), dv.Data()) < 0.01f) {
					np[0] = -dp[1];
					np[1] = dp[0];
				}
				else {
					np[0] = dp[1];
					np[1] = -dp[0];
				}

				side += clamp(std::min(mt::dot(dp, vab),
				                       mt::dot(np, vab)) * 2.0f, 0.0f, 1.0f);
				nside++;

				if (!sweepCircleCircle(activeObst->m_pos.xy(), activeObst->m_rad,
				                       mt::vec2(vab), ob->m_pos.xy(), ob->m_rad, htmin, htmax)) {
					continue;
				}

				// Handle overlapping obstacles.
				if (htmin < 0.0f && htmax > 0.0f) {
					// Avoid more when overlapped.
					htmin = -htmin * 0.5f;
				}
			}
			else if (ob->m_shape == KX_OBSTACLE_SEGMENT) {
				mt::vec3 p1 = ob->m_pos;
				mt::vec3 p2 = ob->m_pos2;
				//apply world transform
				if (ob->m_type == KX_OBSTACLE_NAV_MESH) {
					KX_NavMeshObject *navmeshobj = static_cast<KX_NavMeshObject *>(ob->m_gameObj);
					p1 = navmeshobj->TransformToWorldCoords(p1);
					p2 = navmeshobj->TransformToWorldCoords(p2);
				}
				const mt::vec2 p = p1.xy();
				const mt::vec2 q = p2.xy();

				// NOTE: the segments are assumed to come from a navmesh which is shrunken by
				// the agent radius, hence the use of really small radius.
				// This can be handle more efficiently by using seg-seg test instead.
				// If the whole segment is to be treated as obstacle, use agent->rad instead of 0.01f!
				const float r = 0.01f; // agent->rad
				if (dist_squared_to_line_segment_v2(activeObstPos.Data(), p.Data(), q.Data()) < sqr(r + ob->m_rad)) {
					const mt::vec2 sdir = q - p;
					const mt::vec2 snorm(sdir.y, -sdir.x);
					// If the velocity is pointing towards the segment, no collision.
					if (mt::dot(snorm, vcand) < 0.0f) {
						continue;
					}
					// Else immediate collision.
					htmin = 0.0f;
					htmax = 10.0f;
				}
				else {
					if (!sweepCircleSegment(mt::vec2(activeObstPos), r, mt::vec2(vcand),
					                        mt::vec2(p), mt::vec2(q), ob->m_rad, htmin, htmax)) {
						continue;
					}
				}

				// Avoid less when facing walls.
				htmin *= 2.0f;
			}
			else {
				continue;
			}

			if (htmin >= 0.0f) {
				// The closest obstacle is somewhere ahead of us, keep track of nearest obstacle.
				if (htmin < tmin) {
					tmin = htmin;
				}
			}
		}

		// Normalize side bias, to prevent it dominating too much.
		if (nside) {
			side /= nside;
		}

		const float vpen = velWeight * (vcand - activeObst->dvel).Length() * ivmax;
		const float vcpen = curVelWeight * (vcand - activeObst->vel).Length() * ivmax;
		const float spen = sideWeight * side;
		const float tpen = toiWeight * (1.0f / (0.1f + tmin / maxToi));

		const float penalty = vpen + vcpen + spen + tpen;

		if (penalty < minPenalty) {
			minPenalty = penalty;
			res = vcand;
		}
	}
}

void KX_ObstacleSimulationTOI_cells::sampleRVO(KX_Obstacle *activeObst, KX_NavMeshObject *activeNavMeshObj,
                                               const float maxDeltaAngle)
{
	activeObst->nvel = mt::zero2;
	const float vmax = activeObst->dvel.Length();

	mt::vec2 *spos = new mt::vec2[m_maxSamples];
	int nspos = 0;

	if (!m_adaptive) {
		const mt::vec2 cv = activeObst->dvel * m_bias;
		const float vrange = vmax * (1 - m_bias);
		const float cs = 1.0f / (float)m_sampleRadius * vrange;

		for (int y = -m_sampleRadius; y <= m_sampleRadius; ++y)
		{
			for (int x = -m_sampleRadius; x <= m_sampleRadius; ++x)
			{
				if (nspos < m_maxSamples) {
					const mt::vec2 v = cv + (float)(x + 0.5f) * cs;
					if (v.LengthSquared() > sqr(vmax + cs / 2)) {
						continue;
					}
					spos[nspos] = v;
					nspos++;
				}
			}
		}
		processSamples(activeObst, activeNavMeshObj, m_obstacles, m_levelHeight, vmax, spos, cs / 2,
		               nspos,  activeObst->nvel, m_maxToi, m_velWeight, m_curVelWeight, m_collisionWeight, m_toiWeight);
	}
	else {
		const int rad = 4;
		// First sample location.
		mt::vec2 res = activeObst->dvel * m_bias;
		float cs = vmax * (2 - m_bias * 2) / (float)(rad - 1);

		for (int k = 0; k < 5; ++k)
		{
			const float half = (rad - 1) * cs * 0.5f;

			nspos = 0;
			for (int y = 0; y < rad; ++y)
			{
				for (int x = 0; x < rad; ++x)
				{
					const mt::vec2 v_xy = res + mt::vec2(x, y) * cs - half;
					if (v_xy.LengthSquared() > sqr(vmax + cs / 2)) {
						continue;
					}

					spos[nspos] = v_xy;
					nspos++;
				}
			}

			processSamples(activeObst, activeNavMeshObj, m_obstacles, m_levelHeight, vmax, spos, cs / 2,
			               nspos,  res, m_maxToi, m_velWeight, m_curVelWeight, m_collisionWeight, m_toiWeight);

			cs *= 0.5f;
		}
		activeObst->nvel = res;
	}

	delete[] spos;
}

KX_ObstacleSimulationTOI_cells::KX_ObstacleSimulationTOI_cells(float levelHeight, bool enableVisualization)
	:KX_ObstacleSimulationTOI(levelHeight, enableVisualization)
	,   m_bias(0.4f)
	,   m_adaptive(true)
	,   m_sampleRadius(15)
{
	m_maxSamples = (m_sampleRadius * 2 + 1) * (m_sampleRadius * 2 + 1) + 100;
	m_maxToi = 1.5f;
	m_velWeight = 2.0f;
	m_curVelWeight = 0.75f;
	m_toiWeight = 2.5f;
	m_collisionWeight = 0.75f; //side_weight
}
