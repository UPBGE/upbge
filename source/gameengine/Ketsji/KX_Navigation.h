/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_Navigation.h
 *  \ingroup ketsji
 *  A scene's NPC navigation: its navmeshes, its agents (each NPC a navigation node moves) and
 *  the navigation step that moves them all.
 *
 *  Logic nodes only ask (Submit, a request a run, at the end of the Logic Nodes flush) and read
 *  (GetAgentInfo, the last step's outcome). Once every Logic + Physics step, after the flush and
 *  before physics, Step works for all agents at once:
 *    1. the step's requests (an agent not asked this step stands still);
 *    2. paths: agents whose goal or navmesh changed search again, at most kMaxSearchesPerStep a
 *       step, on the job pool's threads;
 *    3. corridors: each agent's polygons trimmed and repaired as it walks, its next corner and
 *       the velocity it wants (on the threads);
 *    4. who is near whom (a grid), then avoidance: ORCA against the neighbours' last velocities
 *       and the nearby navmesh walls (on the threads);
 *    5. the hand-over, in agent order on the main thread: a Character walks at the velocity, a
 *       body moved as Dynamic gets it, anything else slides along the navmesh.
 *  Every thread works on its own agents and reads only the step's start: the outcome is the same
 *  on any number of threads.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "MT_Vector3.h"
#include "MT_Vector4.h"

class KX_GameObject;
class KX_NavMesh;
class KX_NavMeshObject;
class KX_Scene;
class LN_JobPool;
class PHY_IPhysicsEnvironment;
struct KX_NavAgent;
struct KX_NavBuildState;
struct KX_NavGeometry;
struct KX_NavStep;

/** What a navigation node asks for one object in one run. */
struct KX_NavRequest {
  /** What the request does: move the NPC (Navigate, Follow Path), or change the navigation (Set
   * Navigation Attribute), in the order the trees recorded them. */
  enum class Command : uint8_t {
    Move = 0,
    /** `object` an obstacle (`enabled`) or not. */
    SetObstacle,
    /** `object`'s cost of walking `area`. */
    SetAreaCost,
    /** `object` may not walk `area` (`enabled`: closed). */
    SetAreaClosed,
    /** The scene's navmesh made again around `destination`, `offset` half its size. */
    RebuildArea,
    /** `object` stops: its held order (Until Arrived) ends; a link it crosses is finished
     * first (Navigate's Stop input). */
    Stop,
  };
  Command command = Command::Move;
  bool enabled = true;
  std::string area;
  float cost = 1.0f;

  enum class Mode : uint8_t {
    /** To `destination`, or after `target` when set. */
    GoTo = 0,
    /** Around `destination` (the home), to random reachable points within `radius`. */
    Wander,
    /** Away from `target` (else `destination`) until `radius` away. */
    Flee,
    /** Where `target` will be (its velocity ahead by the time it takes to get there). */
    Pursue,
    /** Away from where `target` will be. */
    Evade,
    /** To `offset` in `target`'s space (a leader's formation place). */
    Formation,
    /** With the agents of its `group` near it: apart, the same way, together; toward
     * `destination` when `goal` is set. */
    Flock,
  };

  KX_GameObject *object = nullptr;
  /** The navmesh object to walk on; null: the scene's own (built while playing), else none. */
  KX_NavMeshObject *navmesh = nullptr;
  Mode mode = Mode::GoTo;
  MT_Vector3 destination = MT_Vector3(0.0f, 0.0f, 0.0f);
  KX_GameObject *target = nullptr;
  MT_Vector3 offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  float speed = 1.0f;
  /** Arrived within this distance of the goal. */
  float arrive_distance = 0.5f;
  /** Wander: how far from home; Flee / Evade: how far away to get. */
  float radius = 10.0f;
  /** Flock: weights and group. */
  float separation = 1.0f, alignment = 1.0f, cohesion = 1.0f;
  int group = 0;
  bool goal = false;
  /** No navmesh object: walk the scene's own navmesh (when the scene builds one). Follow Path:
   * only its navmesh object. */
  bool use_scene_navmesh = true;
  /** Move a dynamic body by its velocity (else it slides, as a non-physics object). */
  bool move_dynamic = false;
  /** Avoid other NPCs and obstacles. */
  bool avoid = true;
  /** Higher: others make way (0..100, 50 by default). */
  int priority = 50;
  /** Navigate's Visualize: the step draws its way (the next corner, the goal). */
  bool visualize = false;
  /** Look At (2026-11-18): the step turns `rotating_object` (the NPC when null) toward the way
   * it walks, at most `rot_speed` radians a second, around its local axis `rot_axis` (0 X, 1 Y,
   * 2 Z), its local `front_axis` (0 X, 1 Y, 2 Z, 3 -X, 4 -Y, 5 -Z) facing the way: Rotate To's
   * rule, after the step moved it. */
  bool look_at = false;
  KX_GameObject *rotating_object = nullptr;
  int rot_axis = 2;
  int front_axis = 1;
  float rot_speed = 1.0f;
  /** Wander's random numbers: the node's (LN_RandomBits). */
  uint64_t seed = 0;
  /** Steps the request holds (2026-11-18): a route behind Every N Steps asks every N steps and
   * the NPC keeps going in between (Logic Nodes' phase_tick_stride); 1: this step only. */
  uint32_t hold_steps = 1;
  /** Navigate's Until Arrived (2026-11-18): the order holds until the NPC arrives (then it
   * stands, Arrived staying on), a new request replaces it or Stop ends it. Wander and Flock
   * never arrive: until stopped. */
  bool until_arrived = false;
  /** The recording tree's place: requests are taken in this order, the last for an object
   * wins. */
  uint64_t sort_key = 0;
  uint64_t record_sequence = 0;
};

/** What a node reads of an agent (the last navigation step's outcome). */
struct KX_NavAgentInfo {
  /** Asked to move in the last step. */
  bool active = false;
  /** Moving (asked, not arrived, with somewhere to go). */
  bool moving = false;
  bool arrived = false;
  /** A path was found; complete: it reaches the goal (else it ends nearest it). */
  bool has_path = false;
  bool path_complete = false;
  /** Asked for a goal no navmesh point is near (or no navmesh under the agent). */
  bool no_path = false;
  /** It arrived in the last step (Navigate's When Reached): once an arrival, again after it
   * walked off or got a new order. */
  bool reached = false;
  /** Where it stood at the step's start. */
  MT_Vector3 position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 next_corner = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 goal = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 desired_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** Along the path, to the goal. */
  float remaining_distance = 0.0f;
  /** On a navigation link (jump, ladder, door, teleport): its object and kind; started: the
   * step it began. */
  bool on_link = false;
  bool link_started = false;
  KX_GameObject *link_object = nullptr;
  std::string link_kind;
};

/** A navigation link (Detour's off-mesh connection): an object with the game property
 * `nav_link` naming the object at its other end. */
struct KX_NavLink {
  KX_GameObject *object = nullptr;
  MT_Vector3 start, end;
  float radius = 0.5f;
  bool both_ways = true;
  /** jump, ladder, door, teleport, or anything the game names. */
  std::string kind = "jump";
  /** Seconds to cross (0: from the kind: a teleport at once, the rest at the agent's speed). */
  float duration = 0.0f;
  uint32_t user_id = 0;
};

class KX_Navigation {
 public:
  /** Searches a step at most (counted, never timed: the same on every machine). */
  static constexpr int kMaxSearchesPerStep = 128;
  /** At most this many neighbours are avoided by each agent (the nearest). */
  static constexpr int kMaxNeighbours = 10;
  /** Wall segments each agent avoids (the nearest, Detour's local boundary). */
  static constexpr int kMaxWalls = 8;

  struct Settings {
    /** The scene's Navigation Mesh settings (also the editor's bake). */
    float cell_size = 0.3f, cell_height = 0.2f;
    float agent_radius = 0.6f, agent_height = 2.0f, agent_climb = 0.9f;
    /** Radians. */
    float agent_max_slope = 0.785398f;
    float region_min_size = 8.0f, region_merge_size = 20.0f;
    float edge_max_len = 12.0f, edge_max_error = 1.3f;
    int verts_per_poly = 6;
    float detail_sample_dist = 6.0f, detail_sample_max_error = 1.0f;
    int partitioning = 0;
    /** Build at game start: 0 off, 1 the whole level, 2 around NPCs. */
    int build_mode = 0;
    /** Around NPCs: the navmesh exists within `build_radius` of every agent, and is dropped past
     * `remove_radius`. */
    float build_radius = 30.0f, remove_radius = 50.0f;
    /** Agents farther than this from every camera avoid others only every few steps (0:
     * always). */
    float detail_distance = 0.0f;
    /** Obstacle Simulation's Level Height: agents more apart vertically ignore each other (0:
     * their height). */
    float level_height = 0.0f;
    /** Obstacle Simulation's Visualization. */
    bool visualize = false;
  };

  explicit KX_Navigation(KX_Scene *scene);
  ~KX_Navigation();
  KX_Navigation(const KX_Navigation &) = delete;
  KX_Navigation &operator=(const KX_Navigation &) = delete;

  void SetSettings(const Settings &settings);
  const Settings &GetSettings() const
  {
    return m_settings;
  }

  /** A navigation node's request (main thread, at the end of the Logic Nodes flush). */
  void Submit(const KX_NavRequest &request);
  /** The object is gone: its agent, requests naming it, and a navmesh it carried. */
  void ForgetObject(const KX_GameObject *object);

  /** The navigation step: after every Logic + Physics step's flush, before physics. `pool`:
   * the threads to use (null: the caller alone). */
  void Step(float dt, LN_JobPool *pool);
  /** Steps taken (tests, the agents' stagger). */
  uint64_t GetStepCount() const
  {
    return m_stepCount;
  }

  /** The last step's outcome for `object` (false: it has no agent). Any thread, while no Step
   * runs. */
  bool GetAgentInfo(const KX_GameObject *object, KX_NavAgentInfo &r_info) const;
  size_t GetAgentCount() const
  {
    return m_agents.size();
  }

  /** The navmesh an agent walks: `navmesh_object`'s, else the scene's for its size (null:
   * none). */
  KX_NavMesh *NavMeshFor(KX_NavMeshObject *navmesh_object, float agent_radius = 0.0f);
  /** The scene's own navmesh for the default agent size (built at game start or around NPCs),
   * null when there is none. Main thread (it may make the size's navmesh). */
  KX_NavMesh *GetSceneNavMesh(float agent_radius = 0.0f);
  /** The scene's own navmesh for the default agent size if it exists: any thread, while no Step
   * runs (queries). */
  const KX_NavMesh *FindSceneNavMesh() const;

  /** Obstacles (the physics panel's Create Obstacle, or Set Navigation Attribute's Obstacle):
   * avoided while they move, carved into the scene's navmesh while they stand still. */
  void SetObstacle(KX_GameObject *object, bool enabled);
  /** Save Game (2026-11-18): `object`'s held order (Until Arrived, not arrived nor stopped), as
   * it was asked. False: none. */
  bool GetHeldOrder(const KX_GameObject *object, KX_NavRequest &r_request) const;
  /** Load Game: `object` gets `request` as its held order again, or (`request` null) its held
   * order ends. Its path is searched again from where it now stands. */
  void RestoreHeldOrder(KX_GameObject *object, const KX_NavRequest *request);
  bool IsObstacle(const KX_GameObject *object) const;

  /** Areas (the `nav_area` game property of level objects): a name's id, created at its first
   * use (1..62; 0 is the ground). */
  int AreaId(const std::string &name);
  /** An agent's cost of walking an area (1: as the ground), or closes it (`closed`). */
  void SetAreaCost(KX_GameObject *agent_object, const std::string &area, float cost, bool closed);
  /** Rebuilds the scene's navmesh in a box (level pieces changed by the game). */
  void RebuildArea(const MT_Vector3 &center, const MT_Vector3 &half_size);
  /** A static body was added, removed or moved (the physics, KX_Scene): its tiles rebuild. */
  void NoteStaticChanged(const MT_Vector3 &aabb_min, const MT_Vector3 &aabb_max);

  const std::vector<KX_NavLink> &GetLinks() const
  {
    return m_links;
  }

  /** Tests: a navmesh standing for the scene's own (the default size). */
  void SetSceneNavMeshForTests(std::unique_ptr<KX_NavMesh> navmesh);
  /** Tests: the links (else the scene's, gathered at the first step). */
  void SetLinksForTests(std::vector<KX_NavLink> links);
  /** Tests: makes the step pretend a camera stands here (level of detail). */
  void SetCameraForTests(const MT_Vector3 &position);
  /** Tests: the searches started in the last step. */
  int GetLastSearchCount() const
  {
    return m_lastSearchCount;
  }
  /** Tests: obstacle carving, tile building. */
  KX_NavBuildState *GetBuildStateForTests()
  {
    return m_build.get();
  }
  /** The scene's own navmeshes' build (null when building is off or not started). */
  const KX_NavBuildState *GetBuildState() const
  {
    return m_build.get();
  }

 /** Tests: the physics world agents' Characters live in (else the scene's). */
  void SetPhysicsEnvironmentForTests(PHY_IPhysicsEnvironment *environment);
  /** Tests: the level's triangles for the scene's own navmesh (else the physics' static
   * bodies). */
  void SetGeometrySourceForTests(
      std::function<void(const MT_Vector3 &, const MT_Vector3 &, KX_NavGeometry &)> source);
  /** Tests: groups of agents that shared one search from their goal in the last step. */
  int GetLastSharedGroupCount() const
  {
    return m_lastSharedGroupCount;
  }
  uint64_t GetSharedSearchCount() const
  {
    return m_sharedSearches.load(std::memory_order_relaxed);
  }

 private:
  KX_NavAgent *FindAgent(const KX_GameObject *object) const;
  KX_NavAgent &GetOrCreateAgent(KX_GameObject *object);
  PHY_IPhysicsEnvironment *Physics() const;
  void ApplyRequests();
  void PrepareAgents(float dt);
  void ResolveGoal(KX_NavAgent &agent);
  void FindNeighbours(float dt, LN_JobPool *pool);
  void GatherObstacles();
  void GatherBodies();
  void AddObstacleBody(KX_GameObject *object, bool &r_moved);
  void SyncCorridors(LN_JobPool *pool);
  void PlanPaths(LN_JobPool *pool);
  void SearchAgentPath(KX_NavAgent &agent);
  void SearchShared(std::vector<KX_NavAgent *> &group);
  void GroupSharedGoals(std::vector<KX_NavAgent *> &due,
                        size_t count,
                        std::vector<std::vector<KX_NavAgent *>> &r_groups,
                        std::vector<KX_NavAgent *> &r_single);
  void SteerAgents(float dt, LN_JobPool *pool);
  bool StartLink(KX_NavAgent &agent, uint64_t link_ref, void *query);
  void StepLink(KX_NavAgent &agent, float dt);
  void AvoidAgents(float dt, LN_JobPool *pool);
  void HandOver(float dt, LN_JobPool *pool);
  /** Where a slide toward `wanted` ends: Detour only, the agent's own corridor (any thread).
   * False: no query object (it does not move). */
  bool SlideTarget(KX_NavAgent &agent,
                   const MT_Vector3 &wanted,
                   bool clamp_goal,
                   MT_Vector3 &r_target) const;
  /** The slide's move written (main thread, in agent order). */
  void WriteSlide(KX_NavAgent &agent, const MT_Vector3 &target);
  void SeparateSliding(LN_JobPool *pool);
  void TurnAgents(float dt, LN_JobPool *pool);
  void WriteInfo();
  void EnsureBuild();
  void UpdateSceneNavMesh(LN_JobPool *pool);
  void CollectLinks();
  void DrawAgents() const;
  void RunJobs(LN_JobPool *pool, size_t count, const std::function<void(size_t)> &item);

  KX_Scene *m_scene;
  Settings m_settings;
  PHY_IPhysicsEnvironment *m_physicsForTests = nullptr;
  std::vector<std::unique_ptr<KX_NavAgent>> m_agents;
  std::unordered_map<const KX_GameObject *, KX_NavAgent *> m_agentByObject;
  uint64_t m_nextAgentOrder = 0;
  std::vector<KX_NavRequest> m_pendingRequests;
  uint64_t m_stepCount = 0;
  /** UPBGE_NAV_PROFILE=1: each phase's time summed over 300 steps, then printed (CM_Debug). */
  bool m_profile = false;
  double m_profileMs[12] = {};
  uint32_t m_profileSteps = 0;
  int m_lastSearchCount = 0;
  int m_lastSharedGroupCount = 0;
  std::atomic<uint64_t> m_sharedSearches{0};
  /** The step's bodies (agents, then obstacles), their grid, the still obstacles to carve. */
  std::unique_ptr<KX_NavStep> m_step;
  /** The scene's own navmeshes (built while playing; tests set one). */
  std::unique_ptr<KX_NavBuildState, void (*)(KX_NavBuildState *)> m_build;
  std::unique_ptr<KX_NavMesh> m_testNavMesh;
  std::function<void(const MT_Vector3 &, const MT_Vector3 &, KX_NavGeometry &)>
      m_geometryForTests;
  /** Obstacles not set by the panel (Set Navigation Attribute), on or off. */
  std::unordered_map<const KX_GameObject *, bool> m_obstacleOverrides;
  /** The scene's objects that may be obstacles, found again only when the object list or an
   * override changed (a scan of every object each step cost big scenes). */
  std::vector<KX_GameObject *> m_obstacleCandidates;
  uint64_t m_obstacleListGeneration = UINT64_MAX;
  bool m_obstacleCandidatesDirty = true;
  std::vector<std::string> m_areaNames;
  std::vector<KX_NavLink> m_links;
  bool m_linksCollected = false;
  bool m_hasCameraForTests = false;
  MT_Vector3 m_cameraForTests = MT_Vector3(0.0f, 0.0f, 0.0f);
};

/** Arrived: within `arrive` on the plane (x, y) and within a height of it (an object's origin
 * stands above the navmesh). The navigation step and the nodes' first run share it. */
inline bool KX_NavArrivedAt(const MT_Vector3 &position,
                            const MT_Vector3 &goal,
                            const float arrive,
                            const float height)
{
  const float dx = float(position[0] - goal[0]), dy = float(position[1] - goal[1]);
  const float dz = float(position[2] - goal[2]);
  return dx * dx + dy * dy <= arrive * arrive &&
         (dz < 0.0f ? -dz : dz) <= (height > 2.0f ? height : 2.0f) + arrive;
}

/** Settings from a scene's game settings (Navigation Mesh and Obstacle Simulation panels). */
KX_Navigation::Settings KX_NavigationSettingsOfScene(const KX_Scene *scene);
