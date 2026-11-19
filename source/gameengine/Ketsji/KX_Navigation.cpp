/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_Navigation.cpp
 *  \ingroup ketsji
 */

#include "KX_Navigation.h"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <functional>

#include "BKE_object.hh"
#include "CM_Message.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DetourLocalBoundary.h"
#include "EXP_ListValue.h"
#include "KX_Camera.h"
#include "KX_Detour.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_NavBuild.h"
#include "KX_NavMesh.h"
#include "KX_NavMeshObject.h"
#include "KX_NavOrca.h"
#include "KX_Scene.h"
#include "LN_JobPool.h"
#include "PHY_ICharacter.h"
#include "PHY_IPhysicsController.h"
#include "PHY_IPhysicsEnvironment.h"

/* -------------------------------------------------------------------- */
/** \name Agents
 * \{ */

namespace {

/** Polygons one agent's corridor holds (a longer path is followed in parts). */
constexpr int kCorridorPolygons = 256;
/** Corners looked at ahead. */
constexpr int kCorners = 4;
/** Each agent's path is shortened by a local search this often (staggered by agent). */
constexpr int kTopologySteps = 30;
/** A path that ends short of its goal is searched again this often while followed. */
constexpr int kPartialReplanSteps = 30;
/** Agents a job takes at once (jobs of single agents cost more to hand out than they do). */
constexpr size_t kAgentsPerJob = 32;
/** Far agents (Settings::detail_distance) avoid others every this many steps. */
constexpr int kFarAvoidSteps = 4;

/** Closed areas and crowded polygons on top of Detour's area costs. */
class KX_NavFilter : public dtQueryFilter {
 public:
  /** Bit per area id. */
  uint64_t closed_areas = 0;

  bool passFilter(const dtPolyRef ref, const dtMeshTile *tile, const dtPoly *poly) const override
  {
    if ((closed_areas >> poly->getArea()) & 1u) {
      return false;
    }
    return dtQueryFilter::passFilter(ref, tile, poly);
  }
};

}  // namespace

struct KX_NavAgent {
  enum class Drive : uint8_t {
    /** Moved along the navmesh by its position (no body, or a body not moved as Dynamic). */
    Slide,
    /** A Character: its walking velocity (Jolt moves it and collides). */
    Walk,
    /** A dynamic body moved as Dynamic: its velocity. */
    Velocity,
  };

  KX_GameObject *object = nullptr;
  uint64_t order = 0;

  KX_NavRequest request;
  bool requested = false;
  /** The step's slide, worked out on the helpers before the writes (HandOver,
   * SeparateSliding). */
  MT_Vector3 slide_target = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool slide_ok = false;
  /** Look At's turn of the step (TurnAgents): worked out on the helpers, written in order. */
  MT_Vector3 turn_axis = MT_Vector3(0.0f, 0.0f, 1.0f);
  float turn_angle = 0.0f;
  bool turn_ok = false;
  /** Steps the last request still holds without being asked again (KX_NavRequest::hold_steps). */
  uint32_t hold_left = 0;
  /** Asked in the step before (a request now starts a new order when it was not). */
  bool asked_before = false;
  /** Its Until Arrived order ended by arriving: it stands at the goal, Arrived stays on until a
   * new order. */
  bool finished = false;
  /** The next arrival pulses When Reached (KX_NavAgentInfo::reached). */
  bool reach_armed = true;
  bool was_active = false;
  Drive drive = Drive::Slide;
  Drive last_drive = Drive::Slide;

  float radius = 0.5f;
  float height = 2.0f;

  KX_NavMeshObject *navmesh_object = nullptr;
  KX_NavMesh *navmesh = nullptr;
  KX_NavFilter filter;
  dtPathCorridor corridor;
  dtLocalBoundary boundary;
  bool corridor_ready = false;
  /** Every area costs 1 (its paths may take Detour's shortcuts). */
  bool default_costs = true;
  bool needs_search = false;
  bool path_complete = false;
  bool no_path = false;
  int search_wait = 0;
  int steps_since_search = 0;
  uint64_t tile_stamp = 0;
  /** The goal the corridor leads to (the game's space). */
  MT_Vector3 path_goal = MT_Vector3(0.0f, 0.0f, 0.0f);
  float boundary_center[3] = {0.0f, 0.0f, 0.0f};
  bool boundary_ready = false;

  /** At the step's start, and the step before. */
  MT_Vector3 position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 previous_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool has_previous = false;
  MT_Vector3 velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** Sliding agents: how high the object stands above the navmesh surface. */
  float height_offset = 0.0f;
  bool offset_known = false;
  MT_Vector3 last_written = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool has_written = false;

  /** This step. */
  MT_Vector3 goal = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool has_goal = false;
  bool arrived = false;
  /** Seconds it wanted to move and could not (a crowd at its goal). */
  float stall_time = 0.0f;
  MT_Vector3 desired = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 new_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** The velocity it was told last step (what others avoid, what its next change starts from):
   * a body's own may differ (it hit something). */
  MT_Vector3 commanded = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 next_corner = MT_Vector3(0.0f, 0.0f, 0.0f);
  float remaining = 0.0f;
  bool far_away = false;

  /** Wander's point and how many it picked. */
  MT_Vector3 wander_point = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool has_wander_point = false;
  uint64_t wander_count = 0;
  /** On a link (an off-mesh connection): from start to end over `link_duration`. */
  bool on_link = false;
  bool link_started = false;
  int link_index = -1;
  MT_Vector3 link_start = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 link_end = MT_Vector3(0.0f, 0.0f, 0.0f);
  float link_time = 0.0f;
  float link_duration = 0.0f;
  /** Where the link puts it this step. */
  MT_Vector3 link_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** Neighbours found this step (indices into the step's bodies), for flocking. */
  std::vector<uint32_t> neighbours;

  KX_NavAgentInfo info;

  KX_NavAgent()
  {
    corridor.init(kCorridorPolygons);
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);
  }
};

namespace {

/** Something agents avoid this step: an agent, or an obstacle. */
struct Body {
  MT_Vector3 position;
  MT_Vector3 velocity;
  float radius = 0.5f;
  float height = 2.0f;
  /** An agent (index), or an obstacle (UINT32_MAX). */
  uint32_t agent = UINT32_MAX;
  /** Moving by navigation and avoiding: shares the dodge by priority. */
  bool avoids = false;
  int priority = 50;
  int group = 0;
};

/** A grid of who is where (the plane, x and y). */
/** The first ring a neighbour search looks in (KX_Navigation::FindNeighbours). */
constexpr float kFirstNeighbourRing = 2.0f;

struct BodyGrid {
  /* (2 m: a dense crowd's searches look at fewer bodies; the answers do not depend on it.) */
  float cell = 2.0f;
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells;

  static uint64_t Key(const int x, const int y)
  {
    return (uint64_t(uint32_t(x)) << 32) | uint64_t(uint32_t(y));
  }
  void Build(const std::vector<Body> &bodies)
  {
    for (auto &item : cells) {
      item.second.clear();
    }
    for (uint32_t i = 0; i < bodies.size(); i++) {
      const int x = int(std::floor(bodies[i].position[0] / cell));
      const int y = int(std::floor(bodies[i].position[1] / cell));
      cells[Key(x, y)].push_back(i);
    }
  }
  /** Bodies within `range` of `p` (the plane), any order. */
  template<class F> void Near(const MT_Vector3 &p, const float range, F &&f) const
  {
    const int x0 = int(std::floor((p[0] - range) / cell)),
              x1 = int(std::floor((p[0] + range) / cell));
    const int y0 = int(std::floor((p[1] - range) / cell)),
              y1 = int(std::floor((p[1] + range) / cell));
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        auto it = cells.find(Key(x, y));
        if (it == cells.end()) {
          continue;
        }
        for (const uint32_t i : it->second) {
          f(i);
        }
      }
    }
  }
};

}  // namespace

/** What one step works on besides the agents. */
struct KX_NavStep {
  std::vector<Body> bodies;
  BodyGrid grid;
  /** This step's obstacles (bodies), and the still ones to carve into the scene's navmesh. */
  std::vector<Body> obstacles;
  std::vector<KX_NavCarveObstacle> carve;
  /** Where each obstacle was in the step before (its velocity). */
  std::unordered_map<const KX_GameObject *, MT_Vector3> obstacle_positions;
  float last_dt = 1.0f / 60.0f;
};

namespace {

float distance_2d(const MT_Vector3 &a, const MT_Vector3 &b)
{
  const float dx = float(a[0] - b[0]), dy = float(a[1] - b[1]);
  return std::sqrt(dx * dx + dy * dy);
}

MT_Vector3 flat(const MT_Vector3 &v)
{
  return MT_Vector3(v[0], v[1], 0.0f);
}

bool arrived_at(const MT_Vector3 &position,
                const MT_Vector3 &goal,
                const float arrive,
                const float height)
{
  return KX_NavArrivedAt(position, goal, arrive, height);
}

/** A stable number from an object pointer (wander seeds): never its address, which changes
 * between plays. */
uint64_t mix64(uint64_t z)
{
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // namespace

/** \} */

/* -------------------------------------------------------------------- */
/** \name The navigation
 * \{ */

KX_Navigation::Settings KX_NavigationSettingsOfScene(const KX_Scene *scene)
{
  KX_Navigation::Settings settings;
  if (scene == nullptr) {
    return settings;
  }
  const blender::Scene *bscene = scene->GetBlenderScene();
  if (bscene == nullptr) {
    return settings;
  }
  const blender::RecastData &rd = bscene->gm.recastData;
  settings.cell_size = std::max(rd.cellsize, 0.01f);
  settings.cell_height = std::max(rd.cellheight, 0.01f);
  settings.agent_radius = std::max(rd.agentradius, 0.0f);
  settings.agent_height = std::max(rd.agentheight, 0.1f);
  settings.agent_climb = std::max(rd.agentmaxclimb, 0.0f);
  settings.agent_max_slope = rd.agentmaxslope;
  settings.region_min_size = rd.regionminsize;
  settings.region_merge_size = rd.regionmergesize;
  settings.edge_max_len = rd.edgemaxlen;
  settings.edge_max_error = rd.edgemaxerror;
  settings.verts_per_poly = std::clamp(rd.vertsperpoly, 3, 6);
  settings.detail_sample_dist = rd.detailsampledist;
  settings.detail_sample_max_error = rd.detailsamplemaxerror;
  settings.partitioning = rd.partitioning;
  settings.build_mode = rd.build_mode;
  settings.build_radius = rd.build_radius > 0.0f ? rd.build_radius : 30.0f;
  settings.remove_radius = rd.remove_radius > 0.0f ?
                               std::max(rd.remove_radius, settings.build_radius) :
                               settings.build_radius * 1.6f;
  settings.detail_distance = std::max(rd.detail_distance, 0.0f);
  settings.level_height = bscene->gm.levelHeight;
  settings.visualize = (bscene->gm.flag & GAME_SHOW_OBSTACLE_SIMULATION) != 0;
  return settings;
}

KX_Navigation::KX_Navigation(KX_Scene *scene)
    : m_scene(scene), m_step(std::make_unique<KX_NavStep>()), m_build(nullptr, &KX_NavBuildFree)
{
  m_settings = KX_NavigationSettingsOfScene(scene);
  const char *profile = getenv("UPBGE_NAV_PROFILE");
  m_profile = profile != nullptr && profile[0] != '\0' && profile[0] != '0';
}

KX_Navigation::~KX_Navigation() = default;

void KX_Navigation::SetSettings(const Settings &settings)
{
  m_settings = settings;
  m_build.reset();
}

void KX_Navigation::SetPhysicsEnvironmentForTests(PHY_IPhysicsEnvironment *environment)
{
  m_physicsForTests = environment;
}

PHY_IPhysicsEnvironment *KX_Navigation::Physics() const
{
  if (m_physicsForTests != nullptr) {
    return m_physicsForTests;
  }
  return m_scene ? m_scene->GetPhysicsEnvironment() : nullptr;
}

KX_NavAgent *KX_Navigation::FindAgent(const KX_GameObject *object) const
{
  auto it = m_agentByObject.find(object);
  return it == m_agentByObject.end() ? nullptr : it->second;
}

KX_NavAgent &KX_Navigation::GetOrCreateAgent(KX_GameObject *object)
{
  if (KX_NavAgent *agent = FindAgent(object)) {
    return *agent;
  }
  std::unique_ptr<KX_NavAgent> agent = std::make_unique<KX_NavAgent>();
  agent->object = object;
  agent->order = m_nextAgentOrder++;
  /* Its size: its mesh's bounds on the plane (scaled), else the scene's agent. */
  agent->radius = m_settings.agent_radius > 0.0f ? m_settings.agent_radius : 0.5f;
  agent->height = m_settings.agent_height;
  if (const blender::Object *blender_object = object->GetBlenderObject()) {
    if (const std::optional<blender::Bounds<blender::float3>> bounds = BKE_object_boundbox_get(
            blender_object))
    {
      const MT_Vector3 &scale = object->NodeGetWorldScaling();
      const float sx = std::fabs(float(scale[0])), sy = std::fabs(float(scale[1])),
                  sz = std::fabs(float(scale[2]));
      const float hx = (bounds->max[0] - bounds->min[0]) * 0.5f * sx;
      const float hy = (bounds->max[1] - bounds->min[1]) * 0.5f * sy;
      const float hz = (bounds->max[2] - bounds->min[2]) * sz;
      if (hx > 1e-3f && hy > 1e-3f) {
        agent->radius = std::max(hx, hy);
      }
      if (hz > 1e-3f) {
        agent->height = hz;
      }
    }
  }
  agent->position = object->NodeGetWorldPosition();
  KX_NavAgent *raw = agent.get();
  m_agentByObject[object] = raw;
  m_agents.push_back(std::move(agent));
  return *raw;
}

void KX_Navigation::Submit(const KX_NavRequest &request)
{
  if (request.object == nullptr) {
    return;
  }
  m_pendingRequests.push_back(request);
}

void KX_Navigation::ForgetObject(const KX_GameObject *object)
{
  if (object == nullptr) {
    return;
  }
  std::erase_if(m_pendingRequests, [object](const KX_NavRequest &request) {
    return request.object == object;
  });
  for (KX_NavRequest &request : m_pendingRequests) {
    if (request.target == object) {
      request.target = nullptr;
    }
    if (request.rotating_object == object) {
      request.rotating_object = nullptr;
      request.look_at = false;
    }
    if (request.navmesh == object) {
      request.navmesh = nullptr;
    }
  }
  for (std::unique_ptr<KX_NavAgent> &agent : m_agents) {
    if (agent->request.target == object) {
      agent->request.target = nullptr;
      if (agent->request.until_arrived && agent->request.mode != KX_NavRequest::Mode::Wander) {
        /* A held order after (or away from) an object that went ends: it would walk to the
         * Destination field instead. A link it crosses is finished first. */
        agent->request.until_arrived = false;
        agent->hold_left = 0;
        if (!agent->on_link) {
          agent->requested = false;
        }
      }
    }
    if (agent->request.rotating_object == object) {
      agent->request.rotating_object = nullptr;
      agent->request.look_at = false;
    }
    if (agent->navmesh_object == object || agent->request.navmesh == object) {
      agent->request.navmesh = nullptr;
      agent->navmesh_object = nullptr;
      agent->navmesh = nullptr;
      agent->corridor_ready = false;
      agent->boundary_ready = false;
    }
  }
  m_obstacleOverrides.erase(object);
  std::erase(m_obstacleCandidates, object);
  m_step->obstacle_positions.erase(object);
  std::erase_if(m_step->carve,
                [object](const KX_NavCarveObstacle &o) { return o.object == object; });
  /* The bodies refer to agents by index: rebuilt at the next step. */
  m_step->bodies.clear();
  for (KX_NavLink &link : m_links) {
    if (link.object == object) {
      link.object = nullptr;
    }
  }
  auto it = m_agentByObject.find(object);
  if (it == m_agentByObject.end()) {
    return;
  }
  KX_NavAgent *agent = it->second;
  m_agentByObject.erase(it);
  std::erase_if(m_agents, [agent](const std::unique_ptr<KX_NavAgent> &a) {
    return a.get() == agent;
  });
}

bool KX_Navigation::GetAgentInfo(const KX_GameObject *object, KX_NavAgentInfo &r_info) const
{
  if (const KX_NavAgent *agent = FindAgent(object)) {
    r_info = agent->info;
    return true;
  }
  return false;
}

KX_NavMesh *KX_Navigation::NavMeshFor(KX_NavMeshObject *navmesh_object, const float agent_radius)
{
  if (navmesh_object != nullptr) {
    return navmesh_object->GetNavMesh();
  }
  return GetSceneNavMesh(agent_radius);
}

KX_NavMesh *KX_Navigation::GetSceneNavMesh(const float agent_radius)
{
  if (m_testNavMesh) {
    return m_testNavMesh.get();
  }
  if (m_settings.build_mode == 0) {
    return nullptr;
  }
  EnsureBuild();
  return m_build ? KX_NavBuildNavMesh(*m_build, agent_radius) : nullptr;
}

const KX_NavMesh *KX_Navigation::FindSceneNavMesh() const
{
  if (m_testNavMesh) {
    return m_testNavMesh.get();
  }
  return m_build ? KX_NavBuildFindNavMesh(*m_build, 0.0f) : nullptr;
}

void KX_Navigation::SetSceneNavMeshForTests(std::unique_ptr<KX_NavMesh> navmesh)
{
  m_testNavMesh = std::move(navmesh);
}

void KX_Navigation::SetLinksForTests(std::vector<KX_NavLink> links)
{
  m_links = std::move(links);
  m_linksCollected = true;
}

void KX_Navigation::SetCameraForTests(const MT_Vector3 &position)
{
  m_hasCameraForTests = true;
  m_cameraForTests = position;
}

void KX_Navigation::SetGeometrySourceForTests(
    std::function<void(const MT_Vector3 &, const MT_Vector3 &, KX_NavGeometry &)> source)
{
  m_geometryForTests = std::move(source);
  m_build.reset();
}

void KX_Navigation::RunJobs(LN_JobPool *pool,
                            const size_t count,
                            const std::function<void(size_t)> &item)
{
  if (count == 0) {
    return;
  }
  const size_t jobs = (count + kAgentsPerJob - 1) / kAgentsPerJob;
  if (pool == nullptr || pool->GetHelperCount() == 0 || jobs < 2) {
    for (size_t i = 0; i < count; i++) {
      item(i);
    }
    return;
  }
  pool->Run(jobs, [&](const size_t job, uint32_t /*thread*/) {
    const size_t end = std::min(count, (job + 1) * kAgentsPerJob);
    for (size_t i = job * kAgentsPerJob; i < end; i++) {
      item(i);
    }
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Obstacles, areas, links
 * \{ */

bool KX_Navigation::GetHeldOrder(const KX_GameObject *object, KX_NavRequest &r_request) const
{
  const KX_NavAgent *agent = FindAgent(object);
  if (agent == nullptr || !agent->requested || !agent->request.until_arrived || agent->arrived) {
    return false;
  }
  r_request = agent->request;
  return true;
}

void KX_Navigation::RestoreHeldOrder(KX_GameObject *object, const KX_NavRequest *request)
{
  if (object == nullptr) {
    return;
  }
  if (request == nullptr) {
    if (KX_NavAgent *agent = FindAgent(object)) {
      if (agent->request.until_arrived) {
        agent->request.until_arrived = false;
        agent->requested = false;
        agent->hold_left = 0;
      }
      agent->finished = false;
    }
    return;
  }
  KX_NavAgent &agent = GetOrCreateAgent(object);
  agent.request = *request;
  agent.request.object = object;
  agent.request.command = KX_NavRequest::Command::Move;
  agent.request.until_arrived = true;
  agent.requested = true;
  agent.hold_left = 0;
  agent.finished = false;
  agent.reach_armed = true;
  agent.arrived = false;
  /* Where it now stands (Load Game put it there): a new corridor and walls. */
  agent.corridor_ready = false;
  agent.boundary_ready = false;
  agent.has_wander_point = false;
  agent.on_link = false;
  agent.link_started = false;
  agent.offset_known = false;
  agent.has_written = false;
  agent.has_previous = false;
}

void KX_Navigation::SetObstacle(KX_GameObject *object, const bool enabled)
{
  if (object != nullptr) {
    m_obstacleOverrides[object] = enabled;
    m_obstacleCandidatesDirty = true;
  }
}

bool KX_Navigation::IsObstacle(const KX_GameObject *object) const
{
  if (object == nullptr) {
    return false;
  }
  auto it = m_obstacleOverrides.find(object);
  if (it != m_obstacleOverrides.end()) {
    return it->second;
  }
  const blender::Object *blender_object = const_cast<KX_GameObject *>(object)->GetBlenderObject();
  return blender_object != nullptr && (blender_object->gameflag & blender::OB_HASOBSTACLE) != 0;
}

int KX_Navigation::AreaId(const std::string &name)
{
  if (name.empty() || name == "ground" || name == "Ground") {
    return 0;
  }
  for (size_t i = 0; i < m_areaNames.size(); i++) {
    if (m_areaNames[i] == name) {
      return int(i) + 1;
    }
  }
  if (m_areaNames.size() >= 62) {
    return 0;
  }
  m_areaNames.push_back(name);
  return int(m_areaNames.size());
}

void KX_Navigation::SetAreaCost(KX_GameObject *agent_object,
                                const std::string &area,
                                const float cost,
                                const bool closed)
{
  if (agent_object == nullptr) {
    return;
  }
  const int id = AreaId(area);
  KX_NavAgent &agent = GetOrCreateAgent(agent_object);
  agent.filter.setAreaCost(id, std::max(cost, 0.001f));
  agent.default_costs = true;
  for (int i = 0; i < DT_MAX_AREAS; i++) {
    agent.default_costs = agent.default_costs && agent.filter.getAreaCost(i) == 1.0f;
  }
  if (closed) {
    agent.filter.closed_areas |= uint64_t(1) << id;
  }
  else {
    agent.filter.closed_areas &= ~(uint64_t(1) << id);
  }
  /* The way may change: search again. */
  agent.needs_search = true;
}

void KX_Navigation::RebuildArea(const MT_Vector3 &center, const MT_Vector3 &half_size)
{
  if (m_build) {
    KX_NavBuildMarkDirty(*m_build, center - half_size, center + half_size);
  }
}

void KX_Navigation::NoteStaticChanged(const MT_Vector3 &aabb_min, const MT_Vector3 &aabb_max)
{
  if (m_build) {
    KX_NavBuildMarkDirty(*m_build, aabb_min, aabb_max);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The step
 * \{ */

void KX_Navigation::Step(const float dt, LN_JobPool *pool)
{
  m_stepCount++;
  if (m_agents.empty() && m_pendingRequests.empty() && m_settings.build_mode == 0 &&
      !m_settings.visualize)
  {
    /* A scene nothing navigates in (no NPC ever asked, no navmesh to build): nothing to do. */
    return;
  }
  const float step = std::max(dt, 1e-4f);
  /* UPBGE_NAV_PROFILE: each phase timed (the clock read only then). */
  auto mark = m_profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  auto lap = [&](const int phase) {
    if (m_profile) {
      const auto now = std::chrono::steady_clock::now();
      m_profileMs[phase] += std::chrono::duration<double, std::milli>(now - mark).count();
      mark = now;
    }
  };
  ApplyRequests();
  CollectLinks();
  lap(0);
  /* Obstacles first: the still ones are carved into this step's navmesh. */
  GatherObstacles();
  lap(1);
  UpdateSceneNavMesh(pool);
  lap(2);
  if (!m_agents.empty()) {
    PrepareAgents(step);
    lap(3);
    FindNeighbours(step, pool);
    lap(4);
    SyncCorridors(pool);
    PlanPaths(pool);
    lap(5);
    SteerAgents(step, pool);
    lap(6);
    AvoidAgents(step, pool);
    lap(7);
    HandOver(step, pool);
    SeparateSliding(pool);
    lap(8);
    TurnAgents(step, pool);
    lap(9);
    WriteInfo();
    lap(10);
  }
  if (m_profile && ++m_profileSteps == 300) {
    const double *t = m_profileMs;
    const double n = double(m_profileSteps);
    CM_Debug("navigation step (ms, average of 300 steps, " << m_agents.size() << " agents): requests "
             << t[0] / n << ", obstacles " << t[1] / n << ", navmesh build " << t[2] / n
             << ", agents " << t[3] / n << ", neighbours " << t[4] / n << ", paths "
             << t[5] / n << ", steering " << t[6] / n << ", avoidance " << t[7] / n
             << ", moves " << t[8] / n << ", turns " << t[9] / n << ", info " << t[10] / n);
    std::fill(std::begin(m_profileMs), std::end(m_profileMs), 0.0);
    m_profileSteps = 0;
  }

  m_step->last_dt = step;
  if (m_settings.visualize) {
    DrawAgents();
  }
  else if (m_scene != nullptr) {
    /* Navigate's Visualize: that NPC's way only. */
    const MT_Vector4 color(1.0f, 0.0f, 0.0f, 1.0f);
    for (const std::unique_ptr<KX_NavAgent> &agent : m_agents) {
      if (agent->requested && agent->request.visualize && agent->info.active) {
        const MT_Vector3 p = agent->object->NodeGetWorldPosition();
        KX_RasterizerDrawDebugLine(p, agent->info.next_corner, color);
        if ((agent->info.next_corner - agent->info.goal).length2() > 1e-8f) {
          KX_RasterizerDrawDebugLine(agent->info.next_corner, agent->info.goal, color);
        }
      }
    }
  }
}

void KX_Navigation::ApplyRequests()
{
  for (std::unique_ptr<KX_NavAgent> &agent : m_agents) {
    agent->asked_before = agent->requested;
    if (!agent->requested) {
      continue;
    }
    if (agent->on_link) {
      /* Carried over a link to its end whatever it is asked (stopped in the air, it hung there):
       * then the rules below. */
    }
    else if (agent->hold_left > 0) {
      /* Asked every N steps (Every N Steps), the request holds in between. */
      agent->hold_left--;
    }
    else if (agent->request.until_arrived && !agent->arrived) {
      /* Until Arrived: the order holds. */
    }
    else {
      /* An Until Arrived order that arrived ends: it stands there, Arrived on. */
      agent->finished = agent->request.until_arrived && agent->arrived;
      agent->requested = false;
      agent->hold_left = 0;
    }
  }
  /* In the order they were recorded: the last one for an object wins (the flush hands them over
   * in order already: sorted only when not). */
  const auto before = [](const KX_NavRequest &a, const KX_NavRequest &b) {
    if (a.sort_key != b.sort_key) {
      return a.sort_key < b.sort_key;
    }
    return a.record_sequence < b.record_sequence;
  };
  if (!std::is_sorted(m_pendingRequests.begin(), m_pendingRequests.end(), before)) {
    std::stable_sort(m_pendingRequests.begin(), m_pendingRequests.end(), before);
  }
  for (KX_NavRequest &request : m_pendingRequests) {
    switch (request.command) {
      case KX_NavRequest::Command::Move:
        break;
      case KX_NavRequest::Command::SetObstacle:
        SetObstacle(request.object, request.enabled);
        continue;
      case KX_NavRequest::Command::SetAreaCost:
        SetAreaCost(request.object, request.area, request.cost, false);
        continue;
      case KX_NavRequest::Command::SetAreaClosed: {
        /* Its cost kept: only closed or open. */
        KX_NavAgent &agent = GetOrCreateAgent(request.object);
        const int id = AreaId(request.area);
        SetAreaCost(request.object, request.area, agent.filter.getAreaCost(id), request.enabled);
        continue;
      }
      case KX_NavRequest::Command::RebuildArea:
        RebuildArea(request.destination, request.offset);
        continue;
      case KX_NavRequest::Command::Stop:
        if (KX_NavAgent *agent = FindAgent(request.object)) {
          /* Its order ends (a link it crosses is finished first: the carry keeps it asked). */
          agent->request.until_arrived = false;
          agent->hold_left = 0;
          agent->finished = false;
          if (!agent->on_link) {
            agent->requested = false;
          }
        }
        continue;
    }
    KX_NavAgent &agent = GetOrCreateAgent(request.object);
    if (!agent.asked_before) {
      /* A new order (it was not asked in the step before): its arrival pulses When Reached,
       * also where it already stands. */
      agent.reach_armed = true;
    }
    agent.finished = false;
    if (agent.request.navmesh != request.navmesh) {
      agent.corridor_ready = false;
      agent.boundary_ready = false;
    }
    if (agent.request.mode != request.mode) {
      agent.has_wander_point = false;
    }
    agent.request = std::move(request);
    agent.requested = true;
    agent.hold_left = std::max<uint32_t>(agent.request.hold_steps, 1u) - 1u;
  }
  m_pendingRequests.clear();
}

void KX_Navigation::PrepareAgents(const float dt)
{
  PHY_IPhysicsEnvironment *physics = Physics();
  std::vector<MT_Vector3> cameras;
  if (m_settings.detail_distance > 0.0f) {
    if (m_hasCameraForTests) {
      cameras.push_back(m_cameraForTests);
    }
    else if (m_scene != nullptr) {
      if (KX_Camera *camera = m_scene->GetActiveCamera()) {
        cameras.push_back(camera->NodeGetWorldPosition());
      }
    }
  }
  for (size_t index = 0; index < m_agents.size(); index++) {
    KX_NavAgent &agent = *m_agents[index];
    KX_GameObject *object = agent.object;
    const MT_Vector3 position = object->NodeGetWorldPosition();
    /* Moved by someone else (a teleport, Set Position): the navmesh height again, the corridor
     * from here. */
    if (agent.has_written && agent.drive == KX_NavAgent::Drive::Slide &&
        (position - agent.last_written).length2() > 1e-6f)
    {
      agent.offset_known = false;
      agent.corridor_ready = false;
      agent.boundary_ready = false;
    }
    agent.has_written = false;
    agent.previous_position = agent.has_previous ? agent.position : position;
    agent.position = position;
    agent.has_previous = true;
    /* Its velocity: how it moved in the last step (a jump of a teleport counts as standing). */
    MT_Vector3 moved = position - agent.previous_position;
    const float limit = std::max(agent.request.speed * 3.0f, 1.0f) * dt;
    agent.velocity = moved.length() > limit ? MT_Vector3(0.0f, 0.0f, 0.0f) : moved / dt;
    /* Stalled: it wanted to move (the last step's wish, avoidance may have said stand) and
     * hardly did. */
    const float wanted = float(flat(agent.desired).length());
    if (agent.requested && wanted > 0.05f &&
        float(flat(agent.velocity).length()) < std::max(0.1f * agent.request.speed, 0.05f))
    {
      agent.stall_time += dt;
    }
    else if (!agent.requested || float(flat(agent.velocity).length()) >=
                                     std::max(0.3f * agent.request.speed, 0.1f))
    {
      agent.stall_time = 0.0f;
    }

    agent.last_drive = agent.drive;
    agent.drive = KX_NavAgent::Drive::Slide;
    if (PHY_IPhysicsController *controller = object->GetPhysicsController()) {
      if (controller->IsCharacter() && physics != nullptr &&
          physics->GetCharacterController(object) != nullptr)
      {
        agent.drive = KX_NavAgent::Drive::Walk;
      }
      else if (agent.request.move_dynamic && controller->IsDynamic()) {
        agent.drive = KX_NavAgent::Drive::Velocity;
      }
    }

    /* The navmesh it walks. */
    KX_NavMesh *navmesh = agent.navmesh;
    if (agent.requested) {
      navmesh = agent.request.navmesh != nullptr || agent.request.use_scene_navmesh ?
                    NavMeshFor(agent.request.navmesh, agent.radius) :
                    nullptr;
    }
    if (navmesh != agent.navmesh) {
      agent.navmesh = navmesh;
      agent.navmesh_object = agent.request.navmesh;
      agent.corridor_ready = false;
      agent.boundary_ready = false;
    }
    if (agent.navmesh != nullptr && agent.navmesh->GetTileStamp() != agent.tile_stamp) {
      /* Tiles changed: the corridor is checked (SyncCorridors), the walls found again. */
      agent.boundary_ready = false;
    }

    agent.far_away = false;
    if (!cameras.empty()) {
      float nearest = FLT_MAX;
      for (const MT_Vector3 &camera : cameras) {
        nearest = std::min(nearest, float((camera - position).length()));
      }
      agent.far_away = nearest > m_settings.detail_distance;
    }

    agent.has_goal = false;
    agent.arrived = false;
    if (agent.requested) {
      ResolveGoal(agent);
    }
  }
}

void KX_Navigation::ResolveGoal(KX_NavAgent &agent)
{
  const KX_NavRequest &request = agent.request;
  const MT_Vector3 &position = agent.position;
  const float arrive = std::max(request.arrive_distance, 0.0f);
  auto target_position = [&]() -> MT_Vector3 {
    return request.target ? request.target->NodeGetWorldPosition() : request.destination;
  };
  auto target_velocity = [&]() -> MT_Vector3 {
    if (request.target == nullptr) {
      return MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    if (const KX_NavAgent *other = FindAgent(request.target)) {
      return other->velocity;
    }
    return request.target->GetLinearVelocity(false);
  };
  /* A point `distance` away from `threat`, as far along the navmesh as it goes (straight away,
   * else turned up to 90 degrees each way). */
  auto away_point = [&](const MT_Vector3 &threat, const float distance) -> MT_Vector3 {
    MT_Vector3 away = flat(position - threat);
    if (away.length2() < 1e-8f) {
      away = MT_Vector3(1.0f, 0.0f, 0.0f);
    }
    away.normalize();
    const float want = std::max(distance - distance_2d(position, threat), 1.0f) + arrive;
    if (agent.navmesh == nullptr) {
      return position + away * want;
    }
    MT_Vector3 best = position;
    float best_score = -FLT_MAX;
    static const float kTurns[] = {0.0f, 0.785398f, -0.785398f, 1.570796f, -1.570796f};
    for (const float turn : kTurns) {
      const float c = std::cos(turn), s = std::sin(turn);
      const MT_Vector3 dir(away[0] * c - away[1] * s, away[0] * s + away[1] * c, 0.0f);
      const MT_Vector3 end = position + dir * want;
      float fraction = 1.0f;
      MT_Vector3 hit, normal;
      MT_Vector3 point = end;
      if (agent.navmesh->Raycast(position, end, fraction, hit, normal)) {
        point = position + (hit - position) * 0.95f;
      }
      const float score = distance_2d(point, threat);
      if (score > best_score + 1e-4f) {
        best_score = score;
        best = point;
      }
    }
    return best;
  };

  MT_Vector3 goal = request.destination;
  bool done = false;
  switch (request.mode) {
    case KX_NavRequest::Mode::GoTo:
      goal = target_position();
      break;
    case KX_NavRequest::Mode::Wander: {
      if (!agent.has_wander_point ||
          arrived_at(position, agent.wander_point, std::max(arrive, agent.radius), agent.height))
      {
        const uint64_t seed = mix64(request.seed ^ mix64(agent.wander_count++));
        MT_Vector3 point;
        bool found = false;
        if (agent.navmesh != nullptr) {
          found = agent.navmesh->RandomPoint(
              request.destination, std::max(request.radius, 0.1f), seed, point);
        }
        if (!found) {
          /* No navmesh: a point of the disc around home. */
          const float u = float(seed >> 40) * (1.0f / 16777216.0f);
          const float v = float((seed >> 16) & 0xffffff) * (1.0f / 16777216.0f);
          const float r = std::max(request.radius, 0.0f) * std::sqrt(u);
          const float a = v * 6.2831853f;
          point = request.destination + MT_Vector3(r * std::cos(a), r * std::sin(a), 0.0f);
        }
        agent.wander_point = point;
        agent.has_wander_point = true;
      }
      goal = agent.wander_point;
      break;
    }
    case KX_NavRequest::Mode::Flee: {
      const MT_Vector3 threat = target_position();
      if (distance_2d(position, threat) >= request.radius) {
        done = true;
        goal = position;
      }
      else {
        goal = away_point(threat, request.radius);
      }
      break;
    }
    case KX_NavRequest::Mode::Pursue:
    case KX_NavRequest::Mode::Evade: {
      const MT_Vector3 target = target_position();
      const float distance = float((target - position).length());
      const float time = std::min(distance / std::max(request.speed, 0.1f), 3.0f);
      const MT_Vector3 predicted = target + flat(target_velocity()) * time;
      if (request.mode == KX_NavRequest::Mode::Pursue) {
        goal = predicted;
      }
      else if (distance_2d(position, predicted) >= request.radius) {
        done = true;
        goal = position;
      }
      else {
        goal = away_point(predicted, request.radius);
      }
      break;
    }
    case KX_NavRequest::Mode::Formation:
      if (request.target != nullptr) {
        goal = request.target->NodeGetWorldPosition() +
               request.target->NodeGetWorldOrientation() * request.offset;
      }
      else {
        goal = request.destination + request.offset;
      }
      break;
    case KX_NavRequest::Mode::Flock:
      goal = request.goal ? request.destination : position;
      break;
  }
  agent.goal = goal;
  agent.has_goal = true;
  if (done) {
    agent.arrived = true;
  }
  else if (request.mode != KX_NavRequest::Mode::Wander &&
           (request.mode != KX_NavRequest::Mode::Flock || request.goal))
  {
    agent.arrived = arrived_at(position, goal, arrive, agent.height);
    /* As near as it gets: near its goal, kept from it for a second by others standing there (a
     * horde round the player, a queue at a counter). */
    const float crowd_reach = arrive + std::max(2.0f, agent.radius * 6.0f);
    if (!agent.arrived && agent.stall_time >= 1.0f &&
        arrived_at(position, goal, crowd_reach, agent.height))
    {
      agent.arrived = true;
    }
  }
}

void KX_Navigation::SyncCorridors(LN_JobPool *pool)
{
  /* Each agent with a navmesh: its corridor starts where it now is; a teleport (or a push off
   * the navmesh) starts it again. */
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    if (agent.navmesh == nullptr) {
      agent.corridor_ready = false;
      return;
    }
    if (agent.on_link) {
      /* Carried over a link: its corridor already stands at the link's end. Compared with the
       * NPC in the air it was started again on the floor it left, and after landing the slide
       * put the NPC back there (a platform standing on the floor: the floor's edge is near). */
      return;
    }
    dtNavMeshQuery *query = agent.navmesh->ThreadQuery();
    if (query == nullptr) {
      return;
    }
    float pos[3];
    agent.navmesh->ToNav(agent.position, pos);
    if (agent.navmesh->GetTileStamp() != agent.tile_stamp) {
      /* Tiles were built again (a level piece added or removed, an obstacle carved): the walls
       * it keeps for avoidance are read again, and a corridor through a changed tile is cut
       * where it stops being valid and searched again. Its own polygon may be gone too (the
       * tile under it): it starts from the nearest one now, as DetourCrowd does (given its old
       * one, every search failed and it stood still for good). */
      agent.boundary_ready = false;
      if (agent.corridor_ready && !agent.corridor.isValid(16, query, &agent.filter)) {
        dtPolyRef safe_ref = agent.corridor.getFirstPoly();
        float safe_pos[3];
        dtVcopy(safe_pos, agent.corridor.getPos());
        if (!query->isValidPolyRef(safe_ref, &agent.filter)) {
          safe_ref = 0;
          query->findNearestPoly(pos, KX_NavMesh::kSnapExtents, &agent.filter, &safe_ref, safe_pos);
        }
        if (safe_ref != 0) {
          agent.corridor.trimInvalidPath(safe_ref, safe_pos, query, &agent.filter);
        }
        else {
          agent.corridor_ready = false;
        }
        agent.needs_search = agent.requested;
      }
      agent.tile_stamp = agent.navmesh->GetTileStamp();
    }
    if (agent.corridor_ready) {
      agent.corridor.movePosition(pos, query, &agent.filter);
      const float *p = agent.corridor.getPos();
      const float dx = p[0] - pos[0], dz = p[2] - pos[2];
      if (dx * dx + dz * dz > 1.5f * 1.5f || agent.corridor.getPathCount() == 0) {
        agent.corridor_ready = false;
      }
    }
    if (!agent.corridor_ready) {
      dtPolyRef ref = 0;
      float nearest[3];
      query->findNearestPoly(pos, KX_NavMesh::kSnapExtents, &agent.filter, &ref, nearest);
      if (ref != 0) {
        agent.corridor.reset(ref, nearest);
        agent.needs_search = agent.requested;
        agent.path_complete = false;
      }
      else {
        agent.corridor.reset(0, pos);
      }
      agent.boundary_ready = false;
    }
    /* Sliding agents keep their height above the surface they stand on. */
    if (!agent.offset_known && agent.corridor.getFirstPoly() != 0) {
      float surface[3];
      dtVcopy(surface, agent.corridor.getPos());
      float h = 0.0f;
      if (dtStatusSucceed(query->getPolyHeight(agent.corridor.getFirstPoly(), surface, &h))) {
        surface[1] = h;
      }
      agent.height_offset = float(agent.position[2] - agent.navmesh->ToWorld(surface)[2]);
      agent.offset_known = true;
    }
  });
}

static void adopt_path(KX_NavAgent &agent,
                              dtNavMeshQuery *query,
                              const dtStatus status,
                              const dtPolyRef *path,
                              const int npath,
                              const dtPolyRef goal_ref,
                              const float *goal_near)
{
  if (dtStatusFailed(status) || npath == 0) {
    agent.no_path = true;
    agent.path_complete = false;
    return;
  }
  agent.no_path = false;
  const int kept = std::min(npath, kCorridorPolygons);
  agent.path_complete = path[npath - 1] == goal_ref && kept == npath &&
                        !dtStatusDetail(status, DT_PARTIAL_RESULT);
  float target[3];
  dtVcopy(target, goal_near);
  if (!agent.path_complete) {
    bool over = false;
    query->closestPointOnPoly(path[kept - 1], goal_near, target, &over);
  }
  const dtPolyRef start_ref = agent.corridor.getFirstPoly();
  float start[3];
  dtVcopy(start, agent.corridor.getPos());
  agent.corridor.reset(start_ref, start);
  agent.corridor.setCorridor(target, path, kept);
  agent.corridor_ready = true;
}

void KX_Navigation::PlanPaths(LN_JobPool *pool)
{
  std::vector<KX_NavAgent *> due;
  for (std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    KX_NavAgent &agent = *holder;
    agent.steps_since_search++;
    if (!agent.requested || !agent.has_goal || agent.arrived || agent.navmesh == nullptr ||
        agent.corridor.getFirstPoly() == 0)
    {
      continue;
    }
    if (!agent.corridor_ready) {
      agent.needs_search = true;
    }
    else if (!agent.needs_search) {
      const float moved = float((agent.goal - agent.path_goal).length());
      if (moved > std::max(0.25f, agent.request.arrive_distance * 0.5f)) {
        agent.needs_search = true;
      }
      else if (!agent.path_complete && agent.steps_since_search >= kPartialReplanSteps) {
        /* A partial path: search again from nearer the goal. */
        agent.needs_search = true;
      }
    }
    if (agent.needs_search) {
      due.push_back(&agent);
    }
  }
  /* The longest waiting first, then the oldest agent: never by thread or time. */
  std::stable_sort(due.begin(), due.end(), [](const KX_NavAgent *a, const KX_NavAgent *b) {
    if (a->search_wait != b->search_wait) {
      return a->search_wait > b->search_wait;
    }
    return a->order < b->order;
  });
  const size_t count = std::min(due.size(), size_t(kMaxSearchesPerStep));
  for (size_t i = count; i < due.size(); i++) {
    due[i]->search_wait++;
  }
  m_lastSearchCount = int(count);
  /* Goals shared by many agents (a horde after the player): one search from the goal serves all
   * of them (a Dijkstra over the navmesh, each path read off its tree). */
  std::vector<std::vector<KX_NavAgent *>> groups;
  std::vector<KX_NavAgent *> single;
  GroupSharedGoals(due, count, groups, single);

  RunJobs(pool, single.size(), [&](const size_t i) { SearchAgentPath(*single[i]); });
  /* Each group in one job: the Dijkstra's tree lives in that thread's query object. */
  if (!groups.empty()) {
    if (pool == nullptr || pool->GetHelperCount() == 0 || groups.size() < 2) {
      for (std::vector<KX_NavAgent *> &group : groups) {
        SearchShared(group);
      }
    }
    else {
      pool->Run(groups.size(),
                [&](const size_t g, uint32_t /*thread*/) { SearchShared(groups[g]); });
    }
  }
}

void KX_Navigation::SearchAgentPath(KX_NavAgent &agent)
{
  agent.needs_search = false;
  agent.search_wait = 0;
  agent.steps_since_search = 0;
  agent.path_goal = agent.goal;
  dtNavMeshQuery *query = agent.navmesh->ThreadQuery();
  if (query == nullptr) {
    return;
  }
  thread_local std::vector<dtPolyRef> path;
  path.resize(KX_NavMesh::kMaxPathPolygons);
  float goal[3], goal_near[3];
  agent.navmesh->ToNav(agent.goal, goal);
  dtPolyRef goal_ref = 0;
  query->findNearestPoly(goal, KX_NavMesh::kSnapExtents, &agent.filter, &goal_ref, goal_near);
  if (goal_ref == 0) {
    /* A goal far off the navmesh: look wider before giving up. */
    const float wide[3] = {KX_NavMesh::kSnapExtents[0] * 4.0f,
                           KX_NavMesh::kSnapExtents[1] * 4.0f,
                           KX_NavMesh::kSnapExtents[2] * 4.0f};
    query->findNearestPoly(goal, wide, &agent.filter, &goal_ref, goal_near);
  }
  if (goal_ref == 0) {
    agent.no_path = true;
    agent.path_complete = false;
    agent.corridor_ready = false;
    return;
  }
  const dtPolyRef start_ref = agent.corridor.getFirstPoly();
  if (!query->isValidPolyRef(start_ref, &agent.filter)) {
    /* Its polygon went: the next step finds it the nearest one and searches again. */
    agent.corridor_ready = false;
    return;
  }
  float start[3];
  dtVcopy(start, agent.corridor.getPos());
  int npath = 0;
  const dtStatus status = query->findPath(start_ref,
                                          goal_ref,
                                          start,
                                          goal_near,
                                          &agent.filter,
                                          path.data(),
                                          &npath,
                                          KX_NavMesh::kMaxPathPolygons);
  adopt_path(agent, query, status, path.data(), npath, goal_ref, goal_near);
}

static bool area_costs_default(const KX_NavAgent &agent);

void KX_Navigation::GroupSharedGoals(std::vector<KX_NavAgent *> &due,
                                     const size_t count,
                                     std::vector<std::vector<KX_NavAgent *>> &r_groups,
                                     std::vector<KX_NavAgent *> &r_single)
{
  /* Agents of one navmesh and filter whose goals snap to the same polygon. */
  constexpr size_t kMinShared = 8;
  struct Key {
    KX_NavMesh *navmesh;
    dtPolyRef ref;
    uint64_t closed;
    bool operator==(const Key &o) const
    {
      return navmesh == o.navmesh && ref == o.ref && closed == o.closed;
    }
  };
  std::vector<std::pair<Key, KX_NavAgent *>> keyed;
  keyed.reserve(count);
  dtNavMeshQuery *query = nullptr;
  KX_NavMesh *query_mesh = nullptr;
  for (size_t i = 0; i < count; i++) {
    KX_NavAgent *agent = due[i];
    Key key{agent->navmesh, 0, agent->filter.closed_areas};
    if (count >= kMinShared && !area_costs_default(*agent)) {
      /* Its own costs: its own search. */
    }
    else if (count >= kMinShared) {
      if (query_mesh != agent->navmesh) {
        query_mesh = agent->navmesh;
        query = agent->navmesh->ThreadQuery();
      }
      if (query != nullptr) {
        float goal[3], snapped[3];
        agent->navmesh->ToNav(agent->goal, goal);
        query->findNearestPoly(goal, KX_NavMesh::kSnapExtents, &agent->filter, &key.ref, snapped);
      }
    }
    keyed.emplace_back(key, agent);
  }
  std::vector<bool> taken(keyed.size(), false);
  for (size_t i = 0; i < keyed.size(); i++) {
    if (taken[i] || keyed[i].first.ref == 0) {
      continue;
    }
    std::vector<KX_NavAgent *> group;
    for (size_t j = i; j < keyed.size(); j++) {
      if (!taken[j] && keyed[j].first == keyed[i].first) {
        group.push_back(keyed[j].second);
      }
    }
    if (group.size() >= kMinShared) {
      for (size_t j = i; j < keyed.size(); j++) {
        if (keyed[j].first == keyed[i].first) {
          taken[j] = true;
        }
      }
      r_groups.push_back(std::move(group));
    }
  }
  for (size_t i = 0; i < keyed.size(); i++) {
    if (!taken[i]) {
      r_single.push_back(keyed[i].second);
    }
  }
  m_lastSharedGroupCount = int(r_groups.size());
}

static bool area_costs_default(const KX_NavAgent &agent)
{
  for (int i = 0; i < DT_MAX_AREAS; i++) {
    if (agent.filter.getAreaCost(i) != 1.0f) {
      return false;
    }
  }
  return true;
}

void KX_Navigation::SearchShared(std::vector<KX_NavAgent *> &group)
{
  /* One Dijkstra from the goal, over every polygon the group's agents may reach; each agent's
   * path is its polygon's way back to the goal, turned around. Walking costs are the same both
   * ways (area costs by distance), so it is the path a search from the agent finds. */
  KX_NavAgent &first = *group.front();
  dtNavMeshQuery *query = first.navmesh->ThreadQuery();
  if (query == nullptr) {
    for (KX_NavAgent *agent : group) {
      SearchAgentPath(*agent);
    }
    return;
  }
  float goal[3], goal_near[3];
  first.navmesh->ToNav(first.goal, goal);
  dtPolyRef goal_ref = 0;
  query->findNearestPoly(goal, KX_NavMesh::kSnapExtents, &first.filter, &goal_ref, goal_near);
  if (goal_ref == 0) {
    for (KX_NavAgent *agent : group) {
      SearchAgentPath(*agent);
    }
    return;
  }
  /* As far as the farthest agent, and a little more. */
  float reach = 0.0f;
  for (KX_NavAgent *agent : group) {
    reach = std::max(reach, float((agent->position - first.goal).length()));
  }
  thread_local std::vector<dtPolyRef> result_refs, result_parents, path;
  thread_local std::vector<float> result_costs;
  constexpr int kMaxResult = 8192;
  result_refs.resize(kMaxResult);
  result_parents.resize(kMaxResult);
  result_costs.resize(kMaxResult);
  path.resize(KX_NavMesh::kMaxPathPolygons);
  int nresult = 0;
  query->findPolysAroundCircle(goal_ref,
                               goal_near,
                               reach * 1.5f + 10.0f,
                               &first.filter,
                               result_refs.data(),
                               result_parents.data(),
                               result_costs.data(),
                               &nresult,
                               kMaxResult);
  m_sharedSearches.fetch_add(1, std::memory_order_relaxed);
  for (KX_NavAgent *agent : group) {
    agent->needs_search = false;
    agent->search_wait = 0;
    agent->steps_since_search = 0;
    agent->path_goal = agent->goal;
    const dtPolyRef start_ref = agent->corridor.getFirstPoly();
    int npath = 0;
    if (dtStatusFailed(query->getPathFromDijkstraSearch(
            start_ref, path.data(), &npath, KX_NavMesh::kMaxPathPolygons)) ||
        npath == 0)
    {
      /* Not reached by the search (too far, another island): its own. */
      SearchAgentPath(*agent);
      continue;
    }
    /* The search ran from the goal: the agent's way is the reverse. */
    std::reverse(path.begin(), path.begin() + npath);
    adopt_path(*agent, query, DT_SUCCESS, path.data(), npath, goal_ref, goal_near);
  }
}

bool KX_Navigation::StartLink(KX_NavAgent &agent, const KX_NavPolyRef link_ref, void *query_ptr)
{
  dtNavMeshQuery *query = static_cast<dtNavMeshQuery *>(query_ptr);
  dtPolyRef refs[2];
  float start[3], end[3];
  if (!agent.corridor.moveOverOffmeshConnection(dtPolyRef(link_ref), refs, start, end, query)) {
    return false;
  }
  const dtOffMeshConnection *connection = agent.navmesh->Mesh()->getOffMeshConnectionByRef(
      dtPolyRef(link_ref));
  const int index = connection ? int(connection->userId) - 1 : -1;
  agent.on_link = true;
  agent.link_started = true;
  agent.link_index = index >= 0 && index < int(m_links.size()) ? index : -1;
  agent.link_start = agent.navmesh->ToWorld(start);
  agent.link_end = agent.navmesh->ToWorld(end);
  agent.link_time = 0.0f;
  const KX_NavLink *link = agent.link_index >= 0 ? &m_links[size_t(agent.link_index)] : nullptr;
  const float distance = float((agent.link_end - agent.link_start).length());
  if (link && link->duration > 0.0f) {
    agent.link_duration = link->duration;
  }
  else if (link && link->kind == "teleport") {
    agent.link_duration = 0.0f;
  }
  else {
    agent.link_duration = distance / std::max(agent.request.speed, 0.1f);
  }
  return true;
}

void KX_Navigation::StepLink(KX_NavAgent &agent, const float dt)
{
  /* Along the link: a jump arcs (its height a quarter of its length, at least half a metre,
   * over the higher end), the rest go straight. */
  agent.link_time += dt;
  const float t = agent.link_duration > 0.0f ?
                      std::min(agent.link_time / agent.link_duration, 1.0f) :
                      1.0f;
  MT_Vector3 p = agent.link_start + (agent.link_end - agent.link_start) * t;
  const KX_NavLink *link = agent.link_index >= 0 ? &m_links[size_t(agent.link_index)] : nullptr;
  if (link == nullptr || link->kind == "jump") {
    const float length = float((agent.link_end - agent.link_start).length());
    const float rise = std::max(0.5f, length * 0.25f);
    p[2] += 4.0f * rise * t * (1.0f - t);
  }
  p[2] += agent.height_offset;
  agent.link_position = p;
  agent.next_corner = agent.link_end;
  agent.remaining = float((agent.link_end - agent.position).length());
  agent.desired = (p - agent.position) / std::max(dt, 1e-4f);
  if (t >= 1.0f) {
    agent.on_link = false;
    /* The corridor stands at the link's end: walking on from there. */
  }
}

static MT_Vector3 flock_pull(const KX_NavAgent &agent, const std::vector<Body> &bodies);

void KX_Navigation::SteerAgents(const float dt, LN_JobPool *pool)
{
  (void)dt;
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    agent.desired = MT_Vector3(0.0f, 0.0f, 0.0f);
    agent.remaining = 0.0f;
    agent.next_corner = agent.has_goal ? agent.goal : agent.position;
    if (!agent.requested || !agent.has_goal) {
      return;
    }
    const float speed = std::max(agent.request.speed, 0.0f);
    const bool flock = agent.request.mode == KX_NavRequest::Mode::Flock;
    MT_Vector3 desired(0.0f, 0.0f, 0.0f);
    agent.link_started = false;
    if (agent.on_link) {
      StepLink(agent, dt);
      return;
    }
    if (agent.arrived) {
      agent.remaining = 0.0f;
    }
    else if (flock && !agent.request.goal) {
      /* Only the flock's own pull below. */
    }
    else if (agent.navmesh != nullptr && agent.corridor_ready) {
      dtNavMeshQuery *query = agent.navmesh->ThreadQuery();
      if (query == nullptr) {
        return;
      }
      float corners[kCorners * 3];
      unsigned char flags[kCorners];
      dtPolyRef polys[kCorners];
      int ncorners = agent.corridor.findCorners(
          corners, flags, polys, kCorners, query, &agent.filter);
      /* A straighter way when the second corner can be seen (Detour's crowd does the same).
       * Not for an agent with its own area costs: Detour's shortcuts look at walls only and
       * would cross an area it pays to keep off. */
      if (agent.default_costs) {
        if (ncorners > 0) {
          const float *ahead = &corners[std::min(1, ncorners - 1) * 3];
          agent.corridor.optimizePathVisibility(
              ahead, agent.radius * 30.0f, query, &agent.filter);
        }
        if ((m_stepCount + agent.order) % kTopologySteps == 0) {
          agent.corridor.optimizePathTopology(query, &agent.filter);
        }
      }
      ncorners = agent.corridor.findCorners(
          corners, flags, polys, kCorners, query, &agent.filter);
      const float *pos = agent.corridor.getPos();
      /* At a link's start: over it (Detour's crowd starts when that close). */
      if (ncorners > 0 && (flags[ncorners - 1] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) &&
          dtVdist2D(pos, &corners[(ncorners - 1) * 3]) < std::max(agent.radius * 2.25f, 0.5f))
      {
        if (StartLink(agent, polys[ncorners - 1], query)) {
          StepLink(agent, dt);
          return;
        }
      }
      if (ncorners > 0) {
        agent.next_corner = agent.navmesh->ToWorld(&corners[0]);
        /* The way left: the corners, then straight to the corridor's end. */
        float remaining = 0.0f;
        const float *previous = pos;
        for (int i = 0; i < ncorners; i++) {
          remaining += dtVdist2D(previous, &corners[i * 3]);
          previous = &corners[i * 3];
        }
        if ((flags[ncorners - 1] & DT_STRAIGHTPATH_END) == 0) {
          remaining += dtVdist2D(previous, agent.corridor.getTarget());
        }
        agent.remaining = remaining;
        const MT_Vector3 world_pos = agent.position;
        MT_Vector3 dir = flat(agent.next_corner - world_pos);
        const float dist = float(dir.length());
        if (dist > 1e-4f) {
          dir /= dist;
        }
        /* Slow down for the goal itself (not for corners). */
        const bool final_corner = (flags[0] & DT_STRAIGHTPATH_END) != 0;
        const float slow_radius = std::max(agent.radius * 2.0f,
                                           agent.request.arrive_distance * 2.0f);
        float scale = 1.0f;
        if (final_corner && agent.path_complete) {
          scale = std::clamp(remaining / std::max(slow_radius, 1e-3f), 0.2f, 1.0f);
        }
        desired = dir * (speed * scale);
      }
      else {
        agent.remaining = distance_2d(agent.position, agent.goal);
      }
    }
    else if (agent.navmesh == nullptr) {
      /* No navmesh: straight at the goal (also up and down, as before navmeshes). */
      MT_Vector3 dir = agent.goal - agent.position;
      const float dist = float(dir.length());
      agent.remaining = dist;
      if (dist > 1e-4f) {
        desired = dir / dist * speed;
      }
    }

    if (flock) {
      desired += flock_pull(agent, m_step->bodies);
      const float len = float(desired.length());
      if (len > speed && len > 0.0f) {
        desired *= speed / len;
      }
    }
    agent.desired = desired;
  });
}

static MT_Vector3 flock_pull(const KX_NavAgent &agent, const std::vector<Body> &bodies)
{
  /* Boids among the agents of its group within reach: apart from the near ones, the same way
   * as all, toward their middle. */
  const KX_NavRequest &request = agent.request;
  const float speed = std::max(request.speed, 0.0f);
  const float reach = std::max(request.radius, agent.radius * 4.0f);
  MT_Vector3 apart(0.0f, 0.0f, 0.0f), heading(0.0f, 0.0f, 0.0f), middle(0.0f, 0.0f, 0.0f);
  int count = 0;
  for (const uint32_t b : agent.neighbours) {
    const Body &other = bodies[b];
    if (other.agent == UINT32_MAX || other.group != request.group) {
      continue;
    }
    const MT_Vector3 offset = flat(agent.position - other.position);
    const float dist = float(offset.length());
    if (dist > reach || dist < 1e-5f) {
      continue;
    }
    const float personal = (agent.radius + other.radius) * 2.0f;
    if (dist < personal) {
      apart += offset / dist * ((personal - dist) / personal);
    }
    heading += flat(other.velocity);
    middle += other.position;
    count++;
  }
  if (count == 0) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  heading /= float(count);
  middle /= float(count);
  MT_Vector3 together = flat(middle - agent.position);
  const float tl = float(together.length());
  if (tl > 1e-4f) {
    together /= tl;
  }
  const float hl = float(heading.length());
  if (hl > 1e-4f) {
    heading /= hl;
  }
  return (apart * request.separation + heading * request.alignment +
          together * request.cohesion) *
         speed;
}

void KX_Navigation::GatherObstacles()
{
  /* Objects with Create Obstacle (or set so by a node) that are no agents: avoided, and carved
   * into the scene's navmesh while they stand still. */
  m_step->obstacles.clear();
  m_step->carve.clear();
  std::unordered_map<const KX_GameObject *, MT_Vector3> seen;
  auto add = [&](KX_GameObject *object) {
    bool moved = false;
    AddObstacleBody(object, moved);
    seen[object] = object->NodeGetWorldPosition();
  };
  if (m_scene != nullptr) {
    if (m_obstacleCandidatesDirty ||
        m_obstacleListGeneration != m_scene->GetObjectListGeneration())
    {
      m_obstacleCandidates.clear();
      for (KX_GameObject *object : *m_scene->GetObjectList()) {
        if (IsObstacle(object)) {
          m_obstacleCandidates.push_back(object);
        }
      }
      m_obstacleListGeneration = m_scene->GetObjectListGeneration();
      m_obstacleCandidatesDirty = false;
    }
    for (KX_GameObject *object : m_obstacleCandidates) {
      if (FindAgent(object) == nullptr) {
        add(object);
      }
    }
  }
  else {
    std::vector<KX_GameObject *> objects;
    for (const auto &[object, enabled] : m_obstacleOverrides) {
      if (enabled && FindAgent(object) == nullptr) {
        objects.push_back(const_cast<KX_GameObject *>(object));
      }
    }
    /* A hash map has no order: by place (tests only; the scene's list is ordered). */
    std::sort(objects.begin(), objects.end(), [](KX_GameObject *a, KX_GameObject *b) {
      const MT_Vector3 pa = a->NodeGetWorldPosition(), pb = b->NodeGetWorldPosition();
      if (pa[0] != pb[0]) {
        return pa[0] < pb[0];
      }
      return pa[1] < pb[1];
    });
    for (KX_GameObject *object : objects) {
      add(object);
    }
  }
  m_step->obstacle_positions.swap(seen);
}

void KX_Navigation::GatherBodies()
{
  std::vector<Body> &bodies = m_step->bodies;
  bodies.clear();
  for (uint32_t i = 0; i < m_agents.size(); i++) {
    const KX_NavAgent &agent = *m_agents[i];
    Body body;
    body.position = agent.position;
    /* Agents avoid each other by what they were told (ORCA's reciprocity), obstacles by how they
     * move. */
    body.velocity = agent.commanded;
    body.radius = agent.radius;
    body.height = agent.height;
    body.agent = i;
    body.avoids = agent.requested && agent.request.avoid && !agent.arrived;
    body.priority = agent.request.priority;
    body.group = agent.request.group;
    bodies.push_back(body);
  }
  bodies.insert(bodies.end(), m_step->obstacles.begin(), m_step->obstacles.end());
  m_step->grid.Build(bodies);
}

void KX_Navigation::AddObstacleBody(KX_GameObject *object, bool &r_moved)
{
  const MT_Vector3 position = object->NodeGetWorldPosition();
  const auto &positions = m_step->obstacle_positions;
  auto previous = positions.find(object);
  r_moved = previous != positions.end() && (position - previous->second).length2() > 1e-6f;
  Body body;
  body.position = position;
  body.velocity = previous != positions.end() ?
                      (position - previous->second) / std::max(m_step->last_dt, 1e-4f) :
                      MT_Vector3(0.0f, 0.0f, 0.0f);
  const blender::Object *blender_object = object->GetBlenderObject();
  float radius = blender_object ? blender_object->obstacleRad : 1.0f;
  float height = m_settings.agent_height;
  MT_Vector3 half(radius, radius, height * 0.5f);
  MT_Vector3 center = position;
  float yaw = 0.0f;
  if (blender_object != nullptr) {
    const MT_Matrix3x3 &orientation = object->NodeGetWorldOrientation();
    yaw = std::atan2(float(orientation[1][0]), float(orientation[0][0]));
    if (const std::optional<blender::Bounds<blender::float3>> bounds = BKE_object_boundbox_get(
            blender_object))
    {
      const MT_Vector3 &scale = object->NodeGetWorldScaling();
      half = MT_Vector3((bounds->max[0] - bounds->min[0]) * 0.5f * std::fabs(float(scale[0])),
                        (bounds->max[1] - bounds->min[1]) * 0.5f * std::fabs(float(scale[1])),
                        (bounds->max[2] - bounds->min[2]) * 0.5f * std::fabs(float(scale[2])));
      height = float(half[2] * 2.0f);
      /* The box's middle, wherever the object's origin is. */
      const MT_Vector3 middle((bounds->max[0] + bounds->min[0]) * 0.5f * float(scale[0]),
                              (bounds->max[1] + bounds->min[1]) * 0.5f * float(scale[1]),
                              (bounds->max[2] + bounds->min[2]) * 0.5f * float(scale[2]));
      center = position + orientation * middle;
    }
  }
  if (radius <= 0.0f) {
    radius = float(std::max(half[0], half[1]));
  }
  body.radius = std::max(radius, 0.05f);
  body.height = std::max(height, 0.1f);
  body.position = center;
  m_step->obstacles.push_back(body);
  /* A still obstacle is carved into the scene's navmesh; a moving one only avoided (Unity's
   * rule: carving a moving obstacle at every step would make every path search again). */
  if (!r_moved) {
    KX_NavCarveObstacle carve;
    carve.object = object;
    carve.center = center;
    carve.half_size = half;
    carve.yaw = yaw;
    m_step->carve.push_back(carve);
  }
}

void KX_Navigation::FindNeighbours(const float dt, LN_JobPool *pool)
{
  (void)dt;
  GatherBodies();
  const float level = m_settings.level_height;
  const std::vector<Body> &bodies = m_step->bodies;
  const BodyGrid &grid = m_step->grid;
  /* Each agent's nearest bodies (avoidance, flocking, the push apart). */
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    agent.neighbours.clear();
    if (!agent.requested) {
      return;
    }
    const float speed = std::max(agent.request.speed, 0.0f);
    float range = std::clamp(speed * 2.0f + agent.radius * 2.0f + 1.0f, 2.0f, 15.0f);
    if (agent.request.mode == KX_NavRequest::Mode::Flock) {
      range = std::max(range,
                       std::min(std::max(agent.request.radius, agent.radius * 4.0f), 30.0f));
    }
    const size_t want = agent.request.mode == KX_NavRequest::Mode::Flock ?
                            size_t(kMaxNeighbours * 3) :
                            size_t(kMaxNeighbours);
    /* Rings outward (2026-11-18: a dense crowd had ~700 bodies within the whole range, all
     * sorted, for the 10 nearest; 10,000 NPCs spent 90 ms a step here): once a ring holds
     * `want` bodies, the nearest `want` are surely in it (any other is farther), so the answer
     * is the one the whole range gives. The last ring is the whole range, as before. */
    thread_local std::vector<std::pair<float, uint32_t>> found;
    float ring = std::min(range, kFirstNeighbourRing);
    for (;;) {
      found.clear();
      const bool whole = ring >= range;
      grid.Near(agent.position, ring, [&](const uint32_t b) {
        const Body &other = bodies[b];
        if (other.agent == uint32_t(index)) {
          return;
        }
        const float dz = std::fabs(float(other.position[2] - agent.position[2]));
        const float max_dz = level > 0.0f ? level : std::max(agent.height, other.height);
        if (dz > max_dz) {
          return;
        }
        const float d = distance_2d(other.position, agent.position);
        if (whole ? d - other.radius > range : d > ring) {
          return;
        }
        found.emplace_back(d, b);
      });
      if (whole || found.size() >= want) {
        break;
      }
      ring = std::min(ring * 2.0f, range);
    }
    /* Nearest first; equal distances by body order: the same on any thread. */
    const size_t keep = std::min(found.size(), want);
    std::partial_sort(found.begin(), found.begin() + keep, found.end());
    for (size_t i = 0; i < keep; i++) {
      agent.neighbours.push_back(found[i].second);
    }
  });
}

void KX_Navigation::AvoidAgents(const float dt, LN_JobPool *pool)
{
  const kx_orca::Params params{2.0f, 0.5f, dt};
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    agent.new_velocity = agent.desired;
    if (!agent.requested || agent.arrived) {
      agent.new_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
      return;
    }
    if (!agent.request.avoid || agent.on_link || agent.link_started) {
      return;
    }
    /* Far from every camera: the avoidance only every few steps (the velocity it wants in
     * between). */
    if (agent.far_away && (m_stepCount + agent.order) % kFarAvoidSteps != 0) {
      return;
    }
    thread_local std::vector<kx_orca::Neighbour> neighbours;
    thread_local std::vector<kx_orca::Wall> walls;
    thread_local std::vector<kx_orca::Line> lines, scratch;
    neighbours.clear();
    walls.clear();
    const kx_orca::Vec2 position(float(agent.position[0]), float(agent.position[1]));
    for (const uint32_t b : agent.neighbours) {
      const Body &other = m_step->bodies[b];
      kx_orca::Neighbour n;
      n.position = kx_orca::Vec2(float(other.position[0]), float(other.position[1]));
      n.velocity = kx_orca::Vec2(float(other.velocity[0]), float(other.velocity[1]));
      n.radius = other.radius;
      if (!other.avoids) {
        /* It won't dodge: all of it is ours. */
        n.responsibility = 1.0f;
      }
      else if (other.priority == agent.request.priority) {
        n.responsibility = 0.5f;
      }
      else {
        n.responsibility = other.priority > agent.request.priority ? 0.8f : 0.2f;
      }
      neighbours.push_back(n);
    }
    /* The navmesh's walls nearby (Detour's local boundary, refreshed as the agent moves). */
    if (agent.navmesh != nullptr && agent.corridor.getFirstPoly() != 0) {
      dtNavMeshQuery *query = agent.navmesh->ThreadQuery();
      const float *pos = agent.corridor.getPos();
      const float range = std::clamp(agent.radius * 6.0f + 1.0f, 2.0f, 8.0f);
      if (query != nullptr &&
          (!agent.boundary_ready || dtVdist2DSqr(pos, agent.boundary_center) > range * range * 0.0625f ||
           !agent.boundary.isValid(query, &agent.filter)))
      {
        agent.boundary.update(agent.corridor.getFirstPoly(), pos, range, query, &agent.filter);
        dtVcopy(agent.boundary_center, pos);
        agent.boundary_ready = true;
      }
      for (int i = 0; i < agent.boundary.getSegmentCount(); i++) {
        const float *s = agent.boundary.getSegment(i);
        const MT_Vector3 a = agent.navmesh->ToWorld(&s[0]);
        const MT_Vector3 b = agent.navmesh->ToWorld(&s[3]);
        walls.push_back({kx_orca::Vec2(float(a[0]), float(a[1])),
                         kx_orca::Vec2(float(b[0]), float(b[1]))});
      }
    }
    const kx_orca::Vec2 velocity(float(agent.commanded[0]), float(agent.commanded[1]));
    /* A tiny turn of its own (from its number): agents meeting exactly head-on would otherwise
     * both wait (ORCA's symmetric stand-off; RVO2's examples perturb the same way). */
    const float turn = (float(mix64(agent.order + 1) >> 40) * (1.0f / 16777216.0f) - 0.5f) * 0.08f;
    const float ct = std::cos(turn), st = std::sin(turn);
    const kx_orca::Vec2 preferred(float(agent.desired[0]) * ct - float(agent.desired[1]) * st,
                                  float(agent.desired[0]) * st + float(agent.desired[1]) * ct);
    const float max_speed = std::max(agent.request.speed, float(flat(agent.desired).length()));
    const kx_orca::Vec2 result = kx_orca::solve(position,
                                                velocity,
                                                preferred,
                                                agent.radius,
                                                max_speed,
                                                neighbours.data(),
                                                neighbours.size(),
                                                walls.data(),
                                                walls.size(),
                                                params,
                                                lines,
                                                scratch);
    MT_Vector3 chosen(result.x, result.y, agent.desired[2]);
    /* Velocity changes at most so fast (no flicker between two choices), from what it was told
     * (never from how a body moved: a push would feed back into its walking). */
    const float accel = 8.0f + agent.request.speed * 2.0f;
    MT_Vector3 change = flat(chosen - agent.commanded);
    const float max_change = accel * dt;
    const float change_len = float(change.length());
    if (change_len > max_change && change_len > 0.0f) {
      const MT_Vector3 limited = flat(agent.commanded) + change * (max_change / change_len);
      chosen = MT_Vector3(limited[0], limited[1], chosen[2]);
    }
    const float chosen_len = float(flat(chosen).length());
    if (chosen_len > max_speed && chosen_len > 0.0f) {
      const float s = max_speed / chosen_len;
      chosen = MT_Vector3(chosen[0] * s, chosen[1] * s, chosen[2]);
    }
    if (!std::isfinite(float(chosen[0])) || !std::isfinite(float(chosen[1]))) {
      chosen = agent.desired;
    }
    agent.new_velocity = chosen;
  });
}

/** Rotate To's turn (LN_CommandBuffer's RotateToward with a max angular step): `object`'s local
 * `front_axis` toward `direction` around its local `rot_axis`, at most `max_step` radians. */
static bool turn_angle(const KX_GameObject *object,
                        const MT_Vector3 &direction,
                        const int rot_axis,
                        const int front_axis,
                        const float max_step,
                        MT_Vector3 &r_axis,
                        float &r_angle)
{
  if (direction.length2() < 1e-10f || max_step <= 0.0f) {
    return false;
  }
  const MT_Vector3 to_target = direction.normalized();
  const MT_Matrix3x3 orientation = const_cast<KX_GameObject *>(object)->NodeGetWorldOrientation();
  MT_Vector3 local_rot_axis(0.0f, 0.0f, 0.0f);
  local_rot_axis[std::clamp(rot_axis, 0, 2)] = 1.0f;
  const MT_Vector3 rot_axis_world = (orientation * local_rot_axis).normalized();
  const int front_index = std::clamp(front_axis, 0, 5);
  MT_Vector3 local_front(0.0f, 0.0f, 0.0f);
  local_front[front_index % 3] = front_index >= 3 ? -1.0f : 1.0f;
  const MT_Vector3 front_world = orientation * local_front;
  auto project = [](const MT_Vector3 &v, const MT_Vector3 &n) { return v - n * v.dot(n); };
  MT_Vector3 desired = project(to_target, rot_axis_world);
  MT_Vector3 front = project(front_world, rot_axis_world);
  if (desired.length2() < 1e-10f || front.length2() < 1e-10f) {
    return false;
  }
  desired.normalize();
  front.normalize();
  const MT_Vector3 cross = front.cross(desired);
  const float dot = front.dot(desired);
  float angle;
  if (cross.length2() < 1e-10f) {
    if (dot >= 0.0f) {
      return false;
    }
    angle = std::min(float(M_PI), max_step);
  }
  else {
    angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
    if (cross.dot(rot_axis_world) < 0.0f) {
      angle = -angle;
    }
    angle = std::copysign(std::min(std::fabs(angle), max_step), angle);
  }
  if (std::fabs(angle) < 1.0e-6f) {
    /* Facing the way already (most walking NPCs, most steps): nothing to write. */
    return false;
  }
  r_axis = rot_axis_world;
  r_angle = angle;
  return true;
}

void KX_Navigation::TurnAgents(const float dt, LN_JobPool *pool)
{
  /* Look At: toward the velocity just handed over (on a link, toward its end), only while it
   * walks: an NPC that arrived or stands keeps facing where it faced. The angles on the helpers
   * (the objects only read), the turns written in agent order; a Rotating Object other than
   * the NPC's own may be turned by several agents: worked out at its write, in order. */
  const auto heading_of = [](const KX_NavAgent &agent, MT_Vector3 &r_heading) {
    if (!agent.requested || !agent.has_goal || agent.arrived || !agent.request.look_at) {
      return false;
    }
    r_heading = agent.new_velocity;
    if (agent.on_link || agent.link_started) {
      r_heading = agent.link_end - agent.position;
    }
    r_heading[2] = 0.0f;
    return r_heading.length2() >= 1e-4f;
  };
  const auto own = [](const KX_NavAgent &agent) {
    return agent.request.rotating_object == nullptr ||
           agent.request.rotating_object == agent.object;
  };
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    agent.turn_ok = false;
    MT_Vector3 heading;
    if (own(agent) && heading_of(agent, heading)) {
      agent.turn_ok = turn_angle(agent.object,
                                 heading,
                                 agent.request.rot_axis,
                                 agent.request.front_axis,
                                 std::max(agent.request.rot_speed, 0.0f) * dt,
                                 agent.turn_axis,
                                 agent.turn_angle);
    }
  });
  for (std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    KX_NavAgent &agent = *holder;
    if (!own(agent)) {
      MT_Vector3 heading;
      agent.turn_ok = heading_of(agent, heading) &&
                      turn_angle(agent.request.rotating_object,
                                 heading,
                                 agent.request.rot_axis,
                                 agent.request.front_axis,
                                 std::max(agent.request.rot_speed, 0.0f) * dt,
                                 agent.turn_axis,
                                 agent.turn_angle);
    }
    if (agent.turn_ok) {
      KX_GameObject *turned = own(agent) ? agent.object : agent.request.rotating_object;
      /* A turn in world space, as the slide's ApplyMovement: the scene graph (parented too)
       * and the body. */
      turned->ApplyRotation(
          MT_Matrix3x3(MT_Quaternion(agent.turn_axis, MT_Scalar(agent.turn_angle))), false);
    }
  }
}

void KX_Navigation::HandOver(const float dt, LN_JobPool *pool)
{
  PHY_IPhysicsEnvironment *physics = Physics();
  /* The slides' Detour work on the helpers (each agent's own corridor), the writes below in
   * agent order (2026-11-18: 10,000 sliding NPCs spent 2 ms here on the main thread). */
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    agent.slide_ok = false;
    if (agent.requested && agent.has_goal && !agent.arrived && !agent.on_link &&
        !agent.link_started && agent.drive == KX_NavAgent::Drive::Slide)
    {
      agent.slide_ok = SlideTarget(
          agent, agent.position + agent.new_velocity * dt, true, agent.slide_target);
    }
  });
  for (std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    KX_NavAgent &agent = *holder;
    KX_GameObject *object = agent.object;
    const bool active = agent.requested && agent.has_goal;
    /* What it was told last step ends when it stops being asked (or arrives). */
    const bool moving = active && !agent.arrived;
    if (!moving) {
      if (agent.was_active) {
        if (agent.last_drive == KX_NavAgent::Drive::Walk && physics != nullptr) {
          if (PHY_ICharacter *character = physics->GetCharacterController(object)) {
            character->SetWalkDirection(MT_Vector3(0.0f, 0.0f, 0.0f));
          }
        }
        else if (agent.last_drive == KX_NavAgent::Drive::Velocity) {
          const MT_Vector3 v = object->GetLinearVelocity(false);
          object->setLinearVelocity(MT_Vector3(0.0f, 0.0f, v[2]), false);
        }
      }
      agent.was_active = false;
      agent.new_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
      continue;
    }
    agent.was_active = true;
    if (agent.on_link || agent.link_started) {
      /* Over a link it is carried (a Character included: it walks on from the end). */
      if (agent.drive == KX_NavAgent::Drive::Walk && physics != nullptr) {
        if (PHY_ICharacter *character = physics->GetCharacterController(object)) {
          character->SetWalkDirection(MT_Vector3(0.0f, 0.0f, 0.0f));
        }
      }
      const MT_Vector3 delta = agent.link_position - object->NodeGetWorldPosition();
      object->ApplyMovement(delta, false);
      agent.last_written = object->NodeGetWorldPosition();
      agent.has_written = true;
      continue;
    }
    switch (agent.drive) {
      case KX_NavAgent::Drive::Walk:
        if (physics != nullptr) {
          if (PHY_ICharacter *character = physics->GetCharacterController(object)) {
            character->SetWalkDirection(flat(agent.new_velocity));
          }
        }
        break;
      case KX_NavAgent::Drive::Velocity: {
        const MT_Vector3 v = object->GetLinearVelocity(false);
        object->setLinearVelocity(MT_Vector3(agent.new_velocity[0], agent.new_velocity[1], v[2]),
                                  false);
        break;
      }
      case KX_NavAgent::Drive::Slide:
        if (agent.slide_ok) {
          WriteSlide(agent, agent.slide_target);
        }
        break;
    }
  }
}

bool KX_Navigation::SlideTarget(KX_NavAgent &agent,
                                const MT_Vector3 &wanted,
                                const bool clamp_goal,
                                MT_Vector3 &r_target) const
{
  MT_Vector3 target = wanted;
  if (agent.navmesh != nullptr && agent.corridor.getFirstPoly() != 0) {
    dtNavMeshQuery *query = agent.navmesh->ThreadQuery();
    if (query == nullptr) {
      return false;
    }
    float npos[3];
    agent.navmesh->ToNav(wanted, npos);
    agent.corridor.movePosition(npos, query, &agent.filter);
    float p[3];
    dtVcopy(p, agent.corridor.getPos());
    float h = 0.0f;
    if (dtStatusSucceed(query->getPolyHeight(agent.corridor.getFirstPoly(), p, &h))) {
      p[1] = h;
    }
    target = agent.navmesh->ToWorld(p);
    target[2] += agent.height_offset;
  }
  else if (clamp_goal && agent.has_goal) {
    /* No navmesh: never past the goal's arrive distance (as Navigate always moved). */
    const MT_Vector3 to_goal = agent.goal - agent.position;
    const float dist = float(to_goal.length());
    const MT_Vector3 step = wanted - agent.position;
    const float step_len = float(step.length());
    const float room = std::max(dist - agent.request.arrive_distance, 0.0f);
    if (step_len > room && step_len > 0.0f) {
      target = agent.position + step * (room / step_len);
    }
  }
  r_target = target;
  return true;
}

void KX_Navigation::WriteSlide(KX_NavAgent &agent, const MT_Vector3 &target)
{
  KX_GameObject *object = agent.object;
  const MT_Vector3 current = object->NodeGetWorldPosition();
  const MT_Vector3 delta = target - current;
  if (delta.length2() > 0.0f) {
    object->ApplyMovement(delta, false);
  }
  agent.last_written = object->NodeGetWorldPosition();
  agent.has_written = true;
}

void KX_Navigation::SeparateSliding(LN_JobPool *pool)
{
  std::vector<Body> &bodies = m_step->bodies;
  /* Sliding agents that still overlap (ORCA lets a crowd press together) are pushed apart along
   * the navmesh, twice; bodies of physics are kept apart by the physics. */
  std::vector<size_t> sliding;
  for (size_t i = 0; i < m_agents.size(); i++) {
    const KX_NavAgent &agent = *m_agents[i];
    if (agent.requested && agent.drive == KX_NavAgent::Drive::Slide && agent.has_written &&
        agent.request.avoid)
    {
      sliding.push_back(i);
    }
  }
  if (sliding.size() < 1) {
    return;
  }
  std::vector<MT_Vector3> push(m_agents.size());
  for (int iteration = 0; iteration < 2; iteration++) {
    /* Where everyone is now (the moves just written). */
    for (size_t b = 0; b < bodies.size(); b++) {
      if (bodies[b].agent != UINT32_MAX) {
        bodies[b].position = m_agents[bodies[b].agent]->object->NodeGetWorldPosition();
      }
    }
    RunJobs(pool, sliding.size(), [&](const size_t s) {
      const size_t index = sliding[s];
      KX_NavAgent &agent = *m_agents[index];
      MT_Vector3 total(0.0f, 0.0f, 0.0f);
      const MT_Vector3 position = bodies[index].position;
      for (const uint32_t b : agent.neighbours) {
        const Body &other = bodies[b];
        const MT_Vector3 offset = flat(position - other.position);
        const float dist = float(offset.length());
        const float overlap = agent.radius + other.radius - dist;
        if (overlap <= 0.0f) {
          continue;
        }
        MT_Vector3 dir = dist > 1e-4f ? offset / dist :
                                        MT_Vector3(other.agent < index ? 1.0f : -1.0f, 0.0f, 0.0f);
        /* Half each between two that both give way, all of it against one that won't. */
        const bool shared = other.agent != UINT32_MAX && other.avoids &&
                            m_agents[other.agent]->drive == KX_NavAgent::Drive::Slide;
        total += dir * (overlap * (shared ? 0.5f : 1.0f));
      }
      push[index] = total;
    });
    /* The pushes' slides on the helpers (each agent's own corridor; the objects only read),
     * then written in agent order. */
    RunJobs(pool, sliding.size(), [&](const size_t s) {
      KX_NavAgent &agent = *m_agents[sliding[s]];
      agent.slide_ok = false;
      if (push[sliding[s]].length2() > 1e-8f) {
        agent.position = agent.object->NodeGetWorldPosition();
        agent.slide_ok = SlideTarget(
            agent, agent.position + push[sliding[s]], false, agent.slide_target);
      }
    });
    bool any = false;
    for (const size_t index : sliding) {
      if (push[index].length2() > 1e-8f) {
        KX_NavAgent &agent = *m_agents[index];
        if (agent.slide_ok) {
          WriteSlide(agent, agent.slide_target);
        }
        any = true;
      }
      push[index] = MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    if (!any) {
      break;
    }
  }
}

void KX_Navigation::WriteInfo()
{
  for (std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    KX_NavAgent &agent = *holder;
    agent.commanded = agent.new_velocity;
    KX_NavAgentInfo &info = agent.info;
    const bool link_started = info.link_started;
    (void)link_started;
    info.active = agent.requested && agent.has_goal;
    /* An Until Arrived order that arrived keeps Arrived on until a new order. */
    info.arrived = (info.active && agent.arrived) || agent.finished;
    /* When Reached: once an arrival; again after it walked off, or for a new order. */
    if (info.active && !info.arrived) {
      agent.reach_armed = true;
    }
    info.reached = info.arrived && agent.reach_armed;
    if (info.reached) {
      agent.reach_armed = false;
    }
    info.moving = info.active && !agent.arrived && agent.new_velocity.length2() > 1e-8f;
    info.has_path = agent.corridor_ready && agent.navmesh != nullptr;
    info.path_complete = info.has_path && agent.path_complete;
    info.no_path = agent.no_path || (info.active && agent.navmesh == nullptr &&
                                     (agent.request.navmesh != nullptr ||
                                      m_settings.build_mode != 0));
    info.position = agent.position;
    info.next_corner = agent.next_corner;
    info.goal = agent.has_goal || agent.finished ? agent.goal : agent.position;
    info.velocity = agent.new_velocity;
    info.desired_velocity = agent.desired;
    info.remaining_distance = agent.remaining;
    info.on_link = agent.on_link || agent.link_started;
    info.link_started = agent.link_started;
    if (info.on_link && agent.link_index >= 0 && agent.link_index < int(m_links.size())) {
      info.link_object = m_links[size_t(agent.link_index)].object;
      info.link_kind = m_links[size_t(agent.link_index)].kind;
    }
    else {
      info.link_object = nullptr;
      info.link_kind.clear();
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The scene's own navmesh
 * \{ */

void KX_Navigation::EnsureBuild()
{
  if (m_build || m_settings.build_mode == 0) {
    return;
  }
  KX_NavGeometrySource source = m_geometryForTests;
  if (!source && m_scene != nullptr) {
    KX_Scene *scene = m_scene;
    source = [scene, this](const MT_Vector3 &aabb_min,
                           const MT_Vector3 &aabb_max,
                           KX_NavGeometry &r_geometry) {
      KX_NavCollectSceneGeometry(
          scene,
          aabb_min,
          aabb_max,
          [this](const std::string &name) { return AreaId(name); },
          [this](const KX_GameObject *object) { return IsObstacle(object); },
          r_geometry);
    };
  }
  if (source) {
    m_build.reset(KX_NavBuildCreate(m_settings, std::move(source)));
  }
}

void KX_Navigation::UpdateSceneNavMesh(LN_JobPool *pool)
{
  if (m_testNavMesh || m_settings.build_mode == 0) {
    return;
  }
  EnsureBuild();
  if (!m_build) {
    return;
  }
  /* The default size always (queries read it), every agent size asked for (built below). */
  KX_NavBuildNavMesh(*m_build, 0.0f);
  std::vector<MT_Vector3> positions;
  std::vector<float> radii;
  positions.reserve(m_agents.size());
  radii.reserve(m_agents.size());
  for (const std::unique_ptr<KX_NavAgent> &agent : m_agents) {
    /* Agents walking a navmesh object build nothing. */
    if (agent->request.navmesh != nullptr || !agent->request.use_scene_navmesh) {
      continue;
    }
    if (agent->requested) {
      KX_NavBuildNavMesh(*m_build, agent->radius);
    }
    positions.push_back(agent->object->NodeGetWorldPosition());
    radii.push_back(agent->radius);
  }
  KX_NavBuildUpdate(*m_build, positions, radii, m_step->carve, m_links, pool);
}

void KX_Navigation::CollectLinks()
{
  if (m_linksCollected || m_scene == nullptr) {
    return;
  }
  m_linksCollected = true;
  m_links = KX_NavCollectLinks(m_scene);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

void KX_Navigation::DrawAgents() const
{
  const MT_Vector4 agent_color(0.2f, 0.8f, 1.0f, 1.0f);
  const MT_Vector4 velocity_color(1.0f, 0.9f, 0.1f, 1.0f);
  const MT_Vector4 path_color(0.3f, 1.0f, 0.3f, 1.0f);
  const MT_Vector4 obstacle_color(1.0f, 0.4f, 0.2f, 1.0f);
  const MT_Vector3 up(0.0f, 0.0f, 1.0f);
  for (const std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    const KX_NavAgent &agent = *holder;
    const MT_Vector3 p = agent.object->NodeGetWorldPosition();
    KX_RasterizerDrawDebugCircle(p, agent.radius, agent_color, up, 16);
    if (!agent.info.active) {
      continue;
    }
    KX_RasterizerDrawDebugLine(p, p + agent.new_velocity, velocity_color);
    KX_RasterizerDrawDebugLine(p, agent.next_corner, path_color);
    if (agent.next_corner != agent.goal) {
      KX_RasterizerDrawDebugLine(agent.next_corner, agent.goal, path_color * 0.6f);
    }
  }
  for (const Body &body : m_step->bodies) {
    if (body.agent == UINT32_MAX) {
      KX_RasterizerDrawDebugCircle(body.position, body.radius, obstacle_color, up, 16);
    }
  }
  /* The scene's navmesh near the active camera. */
  if (m_scene != nullptr && m_build) {
    KX_Camera *camera = m_scene->GetActiveCamera();
    const MT_Vector3 center = camera ? camera->NodeGetWorldPosition() : MT_Vector3(0, 0, 0);
    const MT_Vector4 mesh_color(0.0f, 1.0f, 1.0f, 1.0f);
    KX_NavBuildForEachNavMesh(*m_build, [&](const KX_NavMesh &navmesh) {
      navmesh.Draw(
          KX_NavMesh::DrawMode::Walls,
          [&](const MT_Vector3 &a, const MT_Vector3 &b) {
            KX_RasterizerDrawDebugLine(a, b, mesh_color);
          },
          center,
          60.0f);
    });
  }
}

/** \} */
