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
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "KX_NavAir.h"
#include "MT_Vector3.h"
#include "MT_Vector4.h"

class dtQueryFilter;
class KX_GameObject;
class KX_NavMesh;
class KX_NavMeshObject;
class KX_Scene;
class LN_JobPool;
class PHY_ICharacter;
class PHY_IPhysicsController;
class PHY_IPhysicsEnvironment;
struct KX_NavAgent;
struct KX_NavBuildState;
struct KX_NavGeometry;
struct KX_NavVehicle;
struct KX_NavVehicleType;
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
    /** `object`'s jump limits (2026-11-19, Set Navigation Attribute's Jump Limits): `offset`
     * holds the widest gap, the highest jump up (above its climb) and the deepest drop;
     * `enabled` off: its own again (a Character's from its jump, anything else none). */
    SetJumpLimits,
    /** 2026-11-20 (Set Navigation Attribute's Vehicle, Link Enabled, Link End, Area): `object` a
     * vehicle (`enabled`) or not; its link open (`enabled`) or closed; its link to `target`
     * (null: no link); its surfaces' `area`. */
    SetVehicle,
    SetLinkEnabled,
    SetLinkEnd,
    SetArea,
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
  /** Move a Dynamic or Rigid Body NPC by its velocity (2026-11-20: the default; false: Navigate's
   * Slide, it slides as an object without physics). A Character always walks. */
  bool move_dynamic = true;
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
  /** Avoidance Dense (2026-11-19): in packed crowds the NPC presses and pushes (position-based,
   * by weight: Priority) instead of stepping politely aside (ORCA). */
  bool dense = false;
  /** How it moves (2026-11-19): walking the navmesh, flying through the air, swimming in water. */
  enum class Move : uint8_t { Walk = 0, Fly, Swim };
  Move move = Move::Walk;
  /** Where it walks (2026-11-19): floors only, walls too, walls and ceilings too (crawlers). */
  enum class Climb : uint8_t { Floors = 0, Walls, Ceilings };
  Climb climb = Climb::Floors;
  /** Who gave the order (2026-11-19): the Navigate or Follow Path node's identity (a hash of its
   * tree's and its own name, never 0); 0: none known (an order saved before). When Reached
   * pulses only on the node whose order arrived (an NPC driven by two Navigate nodes pulsed both
   * at any arrival), and another node's order is a new order. `tag`: which of its orders
   * (Follow Path: the point's place + 1, with LN_FOLLOW_PATH_LAST_POINT on the last one). */
  uint32_t issuer = 0;
  uint32_t tag = 0;
  /** The recording tree's place: requests are taken in this order, the last for an object
   * wins. */
  uint64_t sort_key = 0;
  uint64_t record_sequence = 0;
};

/** What a jump link asks of an NPC (2026-11-19): how far across on the plan, and how far up
 * (down: negative) from its start to its end. */
struct KX_NavLinkNeeds {
  /** The space between the ledges (Set Navigation Attribute's limits judge it). */
  float gap = 0.0f;
  /** The jump on the plan from end to end (a Character's jump must carry it so far). */
  float travel = 0.0f;
  float rise = 0.0f;
};

/** What an agent can walk (2026-11-19): its radius, the highest ledge it climbs and the steepest
 * slope (radians) it walks. 0 or less: the scene's (Scene > Navigation Mesh). A navmesh is made
 * for each such class (KX_NavBuild); a Character's own Step Height and Max Slope may ask a
 * stricter one than the scene's. */
struct KX_NavAgentSize {
  float radius = 0.0f;
  float climb = 0.0f;
  float max_slope = 0.0f;
};

/** Why an order cannot be done (Navigate's When Failed, Get Navigation Info's Reason; plan item
 * 8, 2026-11-20). */
enum : uint8_t {
  KX_NAV_FAILURE_NONE = 0,
  KX_NAV_FAILURE_NO_PATH,
  KX_NAV_FAILURE_UNREACHABLE,
  KX_NAV_FAILURE_OFF_NAVMESH,
  KX_NAV_FAILURE_STUCK,
  KX_NAV_FAILURE_GAVE_UP,
};
/** The words of a reason ("" for none). */
const char *KX_NavFailureText(uint8_t failure);

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
  /** The order it reached last (KX_NavRequest::issuer and tag, 2026-11-19): When Reached
   * pulses on that node only. */
  uint32_t reached_issuer = 0;
  uint32_t reached_tag = 0;
  /** Why its order cannot be done now (KX_NAV_FAILURE_*), and whether When Failed pulses this
   * step (once an order and reason) for the order of `failed_issuer`. */
  uint8_t failure = 0;
  bool failed = false;
  uint32_t failed_issuer = 0;
  /** Where it stood at the step's start. */
  MT_Vector3 position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 next_corner = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 goal = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 desired_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** How fast it really walked (2026-11-19, the plane), smoothed over about 0.2 s; exactly 0 while
   * it stands (below 0.1 m/s, walking again above 0.25 m/s): animations in jams. */
  float speed = 0.0f;
  /** Along the path, to the goal. */
  float remaining_distance = 0.0f;
  /** The surface it walks or climbs on (phase 9): its normal (straight up on a navigation mesh;
   * none, zero, while it flies or swims, or a crawler off every surface). */
  MT_Vector3 surface_normal = MT_Vector3(0.0f, 0.0f, 1.0f);
  /** On a navigation link (jump, ladder, door, teleport): its object and kind; started: the
   * step it began. Ended (2026-11-19): the step its crossing ended (object and kind still
   * given), landed: on the link's end (a physics jump may land elsewhere). */
  bool on_link = false;
  bool link_started = false;
  bool link_ended = false;
  bool link_landed = false;
  KX_GameObject *link_object = nullptr;
  std::string link_kind;
  /** Vehicles (2026-11-19): the vehicle it stands on (null: the world); waiting at the start
   * of a link to another frame that is closed (a ship away, the cab elsewhere), its object. */
  KX_GameObject *vehicle = nullptr;
  bool waiting = false;
  KX_GameObject *waiting_at = nullptr;
};

/** A navigation link (Detour's off-mesh connection): an object whose Physics > Navigation > Link
 * goes To another object. */
struct KX_NavLink {
  KX_GameObject *object = nullptr;
  /** The object at its other end. */
  KX_GameObject *end_object = nullptr;
  MT_Vector3 start, end;
  float radius = 0.5f;
  bool both_ways = true;
  /** jump, ladder, door, teleport, or anything the game names. */
  std::string kind = "jump";
  /** Seconds to cross (0: from the kind: a teleport at once, the rest at the agent's speed). */
  float duration = 0.0f;
  /** A link between a vehicle and the world or another vehicle (2026-11-19: a gangway, a
   * lift's door; Reach): open while its ends are at most this far apart. */
  float reach = 1.5f;
  /** Open (2026-11-20: Open at Start, Set Navigation Attribute's Link Enabled). Closed: NPCs
   * search ways without it, or (`wait`) take it and wait at its start until it opens, giving up
   * after `wait_time` seconds (0: never). */
  bool enabled = true;
  bool wait = false;
  float wait_time = 0.0f;
  /** Its own number while it lives (2026-11-19: links come and go while playing; the navmesh's
   * off-mesh connections carry it). 0: the list's place + 1. */
  uint32_t user_id = 0;
  /** Its ends moved and have not come to rest yet (the navmesh still has it at start, end):
   * where they were seen last, and the last step they moved. */
  bool moving = false;
  uint64_t moved_step = 0;
  MT_Vector3 seen_start = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 seen_end = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** How far `start` / `end` were moved onto their floor (2026-11-20, plan item 4: an end on no
   * floor, KX_Navigation::SnapLinkEnds); zero: where its object is. */
  MT_Vector3 start_shift = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 end_shift = MT_Vector3(0.0f, 0.0f, 0.0f);
};

/** A link between frames (2026-11-19): a link whose ends stand on different vehicles, or on a
 * vehicle and the world (a gangway, a lift's door; Detour's links join one navmesh only).
 * Crossed carried, its ends where their objects are at every step; open while they are at most
 * `reach` apart and not moving apart (a ship gone, the cab at another floor: closed, NPCs wait
 * at its start). `start_frame` / `end_frame`: the vehicles (null: the world). */
struct KX_NavFrameLink {
  KX_NavLink link;
  KX_GameObject *start_frame = nullptr;
  KX_GameObject *end_frame = nullptr;
  /** Its ends this step, and whether it is open. */
  MT_Vector3 start = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 end = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool open = false;
  /** Found by the navigation (a vehicle standing still beside the world's navmesh: a docking
   * place, no objects): its start in its vehicle's space, its end in the world. */
  bool automatic = false;
  MT_Vector3 start_local = MT_Vector3(0.0f, 0.0f, 0.0f);
};

/** Navmesh Query's Find Spot (2026-11-20, plan E8: "find a good spot", Unreal's EQS made small):
 * a place to go round `center` within `radius` (`samples` points tried, a sunflower's spiral),
 * reachable on foot from `from`; judged against the one at `look_from` when `has_look` (an
 * enemy's eye): `visibility` 0 any, 1 hidden from it, 2 seen by it (a straight line from its eye
 * to the point at a body's height), `cover`: a wall between within `cover_distance`; not taken
 * (`not_taken`: no other NPC standing or going within `spacing`); inside `area` (empty: any).
 * Each kept place's score: - walk_weight x its walk from `from` - look_weight x its distance
 * from the look (a negative weight: farther is better). */
struct KX_NavSpotQuery {
  MT_Vector3 center = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 from = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 look_from = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool has_look = false;
  float radius = 10.0f;
  int samples = 24;
  int visibility = 0;
  bool cover = false;
  float cover_distance = 1.5f;
  float walk_weight = 1.0f;
  float look_weight = 0.0f;
  bool not_taken = true;
  float spacing = 1.0f;
  std::string area;
  /** The NPC asking (never in its own way). */
  const KX_GameObject *asker = nullptr;
};

/** Find Spot's heights above a place: Hidden From tests the line to a crouching body's middle,
 * Seen By to a standing NPC's eyes, Cover a line toward the look at a crouching chest. These are
 * a 1.8 m NPC's, used for an asker without a body of its own; an NPC's are 0.55 / 0.85 / 0.45 of
 * its height (`SpotAskerHeight`, plan item 13). */
constexpr float kSpotBodyHeight = 1.0f;
constexpr float kSpotEyeHeight = 1.5f;
constexpr float kSpotCoverHeight = 0.8f;

/** A place Find Spot kept: where, its walk from the query's `from`, its score. */
/** A problem the navigation found (2026-11-20, plan item 9): printed once when it is a setup error
 * (`setup`), listed with its count in the Dashboard's Navigation section (the play report). */
struct KX_NavProblem {
  std::string kind;
  std::string object;
  std::string text;
  int count = 0;
  bool setup = true;
};

struct KX_NavSpot {
  MT_Vector3 point = MT_Vector3(0.0f, 0.0f, 0.0f);
  float walk = 0.0f;
  float score = 0.0f;
};

/** Docking links' numbers start here (hand-placed and jump links' stay below). */
constexpr uint32_t kNavDockLinkBase = 0x10000000u;

/** A closed hand-placed link (2026-11-20, Link Enabled off): Go Round (left out of every search)
 * or Wait (taken; NPCs wait at its start, each giving up after `wait_time` seconds, 0: never).
 * `generation` counts its closings (an NPC that gave up waits again at the next one). */
struct KX_NavClosedLink {
  bool wait = false;
  float wait_time = 0.0f;
  uint32_t generation = 0;
};

class KX_Navigation {
 public:
  /** Searches a step at most (counted, never timed: the same on every machine). */
  static constexpr int kMaxSearchesPerStep = 128;
  /** At most this many neighbours are avoided by each agent (the nearest). */
  static constexpr int kMaxNeighbours = 10;
  /** Wall segments each agent avoids (the nearest, Detour's local boundary). */
  static constexpr int kMaxWalls = 8;
  /** A level piece or a link that moved is put into the navmesh where it stands once it stood
   * still this many steps (a piece moving every step never rebuilds tiles every step). */
  static constexpr int kStillSteps = 10;

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
    /** Build at game start: 0 off, 1 the whole level, 2 around NPCs, 3 Auto (2026-11-20: the
     * whole level the first time something needs the scene's navmesh; `build_mode` reads 0 until
     * then and 1 after, `auto_build` says it is Auto: `NormalizeBuildMode`). */
    int build_mode = 0;
    bool auto_build = false;
    /** Auto Cells (2026-11-20): the cell size worked out from the smallest NPC when the build is
     * made (`AutoCellSize`: its radius / 2 within 0.1 .. 0.3 m, the cell height half of it). */
    bool auto_cells = false;
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
    /** Navigation Mesh's Avoid Physics Objects (2026-11-19): Dynamic and Rigid Body objects and
     * Characters that are no NPC, of at least `physics_min_size` (their widest side), are
     * obstacles too: avoided while they move, carved while Jolt keeps them asleep. */
    bool avoid_physics = false;
    float physics_min_size = 0.5f;
    /** Crowds Push Characters (2026-11-19): a Dense crowd pressing a Character that is no NPC
     * (the player, with Avoid Physics Objects) pushes it back (a knock-back). */
    bool crowd_pushes = false;
    /** Jump Links Automatic (2026-11-19): the build finds jumps across gaps up to `jump_gap`
     * (on the plan), up ledges up to `jump_up` above the climb, drops down to `jump_drop`, one
     * link per `jump_spacing` of a ledge (KX_NavBuild). */
    bool jump_links = false;
    float jump_gap = 3.0f, jump_up = 1.5f, jump_drop = 4.0f, jump_spacing = 2.0f;
    /** Jumps Physics (2026-11-19): Characters and Dynamic bodies cross jump and drop links by a
     * real jump (else every NPC is carried along a fixed arc). */
    bool physics_jumps = false;
    /** Visualization (2026-11-20): the navmesh and links drawn within `vis_range` of the active
     * camera (0: everywhere), at most `vis_max_lines` lines a step, without what `vis_hide`
     * (blender::NAVMESH_VIS_HIDE_*) leaves out. */
    float vis_range = 60.0f;
    int vis_max_lines = 20000;
    int vis_hide = 0;
  };

  explicit KX_Navigation(KX_Scene *scene);
  ~KX_Navigation();
  KX_Navigation(const KX_Navigation &) = delete;
  KX_Navigation &operator=(const KX_Navigation &) = delete;

  void SetSettings(const Settings &settings);
  /** Build mode Auto (2026-11-20) read as Off with `auto_build` until wanted. */
  static void NormalizeBuildMode(Settings &settings);
  /** Auto: the scene's navmesh is wanted (an order on it, a query asking for it): built from now
   * on as Whole Level. Main thread. */
  void WantSceneNavMesh();
  /** A problem for the game maker (any thread): printed once (setup errors), counted. */
  void ReportProblem(const char *kind,
                     const std::string &object,
                     const std::string &text,
                     bool setup = true);
  std::vector<KX_NavProblem> GetProblems() const;
  /** The play report's "navigation" object (item 9; the Logic Nodes sidebar > Dashboard >
   * Navigation reads it): the build, the cell used and why, the NPC sizes built and how many NPCs
   * each, the NPCs by state, the cost a step, the problems. Empty when nothing navigated. Main
   * thread. */
  std::string ReportJson() const;
  /** Auto Cells: the cell for an NPC of this radius (radius / 2 within 0.1 .. 0.3 m). */
  static float AutoCellSize(float radius);
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
  KX_NavMesh *NavMeshFor(KX_NavMeshObject *navmesh_object, const KX_NavAgentSize &size);
  /** The scene's own navmesh for the default agent size (built at game start or around NPCs),
   * null when there is none. Main thread (it may make the size's navmesh). */
  KX_NavMesh *GetSceneNavMesh(float agent_radius = 0.0f);
  /** The scene's own navmesh for the default agent size if it exists: any thread, while no Step
   * runs (queries). */
  const KX_NavMesh *FindSceneNavMesh() const;
  /** The cell size the scene's navmesh was built with, and why (Auto Cells: the NPC it came
   * from), for the play report. */
  float BuiltCellSize() const;
  const std::string &AutoCellReason() const
  {
    return m_autoCellReason;
  }

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

  /** Areas (Physics > Navigation > Area of level objects): a name's id, created at its first
   * use (1..62; 0 is the ground). */
  int AreaId(const std::string &name);
  /** An agent's cost of walking an area (1: as the ground), or closes it (`closed`). */
  void SetAreaCost(KX_GameObject *agent_object, const std::string &area, float cost, bool closed);
  /** Rebuilds the scene's navmesh in a box (level pieces changed by the game). */
  void RebuildArea(const MT_Vector3 &center, const MT_Vector3 &half_size);
  /** A static body was added, removed or moved (the physics, KX_Scene): its tiles rebuild. */
  void NoteStaticChanged(const MT_Vector3 &aabb_min, const MT_Vector3 &aabb_max);
  /** `object`'s area changed (Set Navigation Attribute, Load Game): its surfaces are made again
   * (the level's tiles, or its vehicle's shape). */
  void NoteAreaChanged(KX_GameObject *object);
  /** NoteStaticChanged where `object`'s static body is (and its children's). */
  void NoteLevelBoundsOf(KX_GameObject *object, bool children);
  /** Not part of the level the scene's navmesh is built of: an obstacle (carved instead) or an
   * NPC (its own static body is no floor). */
  bool IsExcludedFromLevel(const KX_GameObject *object) const;
  /** An object the step avoids as a physics object (Avoid Physics Objects; main thread). */
  bool IsPhysicsObstacle(KX_GameObject *object) const;

  const std::vector<KX_NavLink> &GetLinks() const
  {
    return m_links;
  }
  /** Links between frames (vehicles, the world) this step (2026-11-19). */
  const std::vector<KX_NavFrameLink> &GetFrameLinks() const
  {
    return m_frameLinks;
  }
  /** What the jump link with Detour's id `id` asks (a hand-placed link of kind "jump" or
   * "drop", either way, or one the build found): false for any other link. Any thread while no
   * Step changes the links. */
  bool LinkNeeds(uint32_t id, KX_NavLinkNeeds &r_needs) const;
  /** The kind of the link with Detour's id `id` ("jump" when unknown). */
  std::string LinkKind(uint32_t id) const;
  /** A hand-placed link closed now (by its own number, kNavLinkReverse cleared), else null. Read
   * by searches on any thread (changed only between steps). */
  const KX_NavClosedLink *FindClosedLink(uint32_t id) const
  {
    if (m_closedLinks.empty()) {
      return nullptr;
    }
    auto it = m_closedLinks.find(id);
    return it != m_closedLinks.end() ? &it->second : nullptr;
  }
  bool HasClosedLinks() const
  {
    return !m_closedLinks.empty();
  }
  /** One of a link's objects (its start, its end, an object above them): never a physics
   * obstacle (a resting ladder carved out of the navmesh cut away its own foot). */
  bool IsLinkObject(const KX_GameObject *object) const
  {
    return m_linkObjects.count(object) != 0;
  }
  /** A filter for queries that are no NPC's (Navmesh Query's paths): every area at its cost,
   * closed links left out. */
  const dtQueryFilter *QueryFilter() const;
  /** Find Spot (KX_NavSpotQuery): the places kept on `navmesh`, best first (ties: the earlier
   * sample). Any thread while the navigation does not step (it reads the navmesh with the
   * thread's query, the NPCs' last step); `clear_line(a, b)`: nothing the physics holds blocks
   * a straight line (asked at most twice a sample). */
  void FindSpots(const KX_NavMesh &navmesh,
                 const KX_NavSpotQuery &query,
                 const std::function<bool(const MT_Vector3 &, const MT_Vector3 &)> &clear_line,
                 std::vector<KX_NavSpot> &r_spots) const;
  /** An area's id by its name without making one (ground and empty 0; -1: no such area). */
  int FindAreaId(const std::string &name) const;

  /** Find Spot's asker's height (item 13): its collision shape's, else its mesh's; 0: none. */
  float SpotAskerHeight(const KX_GameObject *asker) const;
  /** Tests: a navmesh standing for the scene's own (the default size). */
  void SetSceneNavMeshForTests(std::unique_ptr<KX_NavMesh> navmesh);
  /** Tests: the links (else the scene's, gathered at the first step). Their ends find their
   * floor as the scene's do (SnapLinkEnds) when the level is known then. */
  void SetLinksForTests(std::vector<KX_NavLink> links);
  /** Plan item 4 (2026-11-20): an end of a world link lying on no floor (no walkable level
   * surface within the link's Radius across and Max Climb up or down: Detour would never join
   * it) moves straight down onto the floor below it within 1.5 m, else up onto one within
   * 0.5 m (`start_shift` / `end_shift`; each move listed in the problems, not printed). Ends on
   * a floor stay exactly where they are. Main thread (the level's triangles). */
  void SnapLinkEnds(KX_NavLink &link);
  /** Tests: makes the step pretend a camera stands here (level of detail). */
  void SetCameraForTests(const MT_Vector3 &position);
  /** Tests: Visualization's lines into `sink` (the rasterizer otherwise), and how many the
   * last step drew. */
  void SetDebugLineSinkForTests(
      std::function<void(const MT_Vector3 &, const MT_Vector3 &, const MT_Vector4 &)> sink);
  size_t GetDrawnLinesForTests() const
  {
    return m_drawnLines;
  }
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
  /** Tests: the physics objects Avoid Physics Objects looks at (a scene's: its object list). */
  /** Density-aware routes: how much more a meter costs in the scene navmesh's polygon at `p`
   * this step (after a Step; 0 without crowds). */
  float CrowdExtraCostForTests(const MT_Vector3 &p);
  /** Seconds the object's agent wanted to move and got nowhere (-1: no agent). */
  float StallSecondsForTests(const KX_GameObject *object) const;
  /** Flying and swimming (phase 8): the water volumes instead of the scene's Buoyancy Volume
   * sensors; the air map a flyer of `radius` uses (made when first needed; null: none). */
  void SetWaterForTests(std::vector<KX_NavWaterVolume> water);
  KX_NavAirMap *AirMapForTests(float radius);
  /** Walls and ceilings (phase 9): an agent's width and its body's height from the surface (as
   * a mesh's bounds give them; test objects have none). */
  void SetAgentSizeForTests(KX_GameObject *object, float radius, float crawl_offset);

  /** The air maps for Navmesh Query (phase 8; any thread, read-only: only maps already made
   * answer, null otherwise, and the map of that size is made at the next step): the free point
   * nearest `p` for a flyer of `radius` (in water: `water`), whether a straight flight from `a`
   * to `b` is clear, the way from `a` to `b` (its corners). */
  bool AirNearestPoint(const MT_Vector3 &p, float radius, bool water, MT_Vector3 &r_point);
  /** The size the navigation gives `object` in the air (the sphere its agent fills; not an agent:
   * the scene's radius). */
  float AgentRadiusOf(const KX_GameObject *object) const;
  /** Its radius on a navmesh (2026-11-20, plan item 2: its collision shape's, or its NPC
   * Radius), not an agent: the scene's fallback. */
  float AgentWalkRadiusOf(const KX_GameObject *object) const;
  /** Plan item 10: the place round its target the last step gave a chaser (an angle on the plan
   * and a ring's radius); false: none (alone, Go To, Dense, ...). */
  bool ChasePlaceOf(const KX_GameObject *object, float &r_angle, float &r_radius) const;
  bool AirLineClear(const MT_Vector3 &a, const MT_Vector3 &b, float radius, bool water);
  bool AirFindPath(const MT_Vector3 &a,
                   const MT_Vector3 &b,
                   float radius,
                   bool water,
                   std::vector<MT_Vector3> &r_points,
                   bool *r_complete);
  void SetPhysicsObjectsForTests(std::vector<KX_GameObject *> objects);
  /** Tests: the level's triangles for the scene's own navmesh (else the physics' static
   * bodies). */
  void SetGeometrySourceForTests(
      std::function<void(const MT_Vector3 &, const MT_Vector3 &, KX_NavGeometry &)> source);
  /** Vehicles (2026-11-19): objects with Physics > Navigation > Vehicle (a ship, a train car,
   * an elevator cab). Each walks a navmesh of its own, built once from its shape in its own
   * space (copies of one shape share it) and following it while it moves; an NPC stands on the
   * world or on one vehicle (its frame), carried along with it. Tests: these vehicles (else the
   * scene's vehicles), each one's shape in its own space (else its bodies'). */
  void SetVehiclesForTests(std::vector<KX_GameObject *> vehicles);
  void SetVehicleGeometryForTests(
      std::function<void(KX_GameObject *vehicle, KX_NavGeometry &r_local)> geometry);
  /** The navmesh of `vehicle` for agents of `size` (main thread; null: no such vehicle). */
  KX_NavMesh *VehicleNavMeshFor(KX_GameObject *vehicle, const KX_NavAgentSize &size);
  /** The vehicle `object`'s agent stands on (null: the world, or no agent). */
  KX_GameObject *AgentFrame(const KX_GameObject *object) const;
  /** The vehicle `object` is (or is part of): null when none. */
  KX_GameObject *VehicleOf(const KX_GameObject *object) const;
  /** Tests: vehicle shapes built (copies of one shape share one). */
  size_t VehicleTypeCountForTests() const;

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
  /** Level pieces the physics says changed (moved, turned, scaled, in or out of the world):
   * their tiles once they stand still. */
  void TrackLevelChanges();
  /** Links read again when the object list changed; links whose ends moved, where they came to
   * rest. */
  void UpdateLinks();
  void IndexLinks();
  void PrepareAgents(float dt, LN_JobPool *pool);
  /** Plan item 10: NPCs pursuing one target with Polite avoidance get places round it. */
  void AssignChaserPlaces(float dt);
  /** How far an object reaches out on the plan from its origin: an agent's radius, else its
   * collision shape's, else its mesh's; 0: nothing known. */
  float FootprintRadiusOf(KX_GameObject *object) const;
  void ResolveGoal(KX_NavAgent &agent);
  void FindNeighbours(float dt, LN_JobPool *pool);
  void GatherObstacles();
  void GatherBodies();
  void AddObstacleBody(KX_GameObject *object, bool physics, bool &r_moved);
  void SyncCorridors(LN_JobPool *pool);
  void PlanPaths(LN_JobPool *pool);
  /** Flying and swimming (phase 8): the air map of a flyer's size (made now when missing: at
   * most kMaxAirMaps sizes, else the nearest larger, else the largest); the ways of the flyers
   * that need one, on the helpers; following them; 3D avoidance; the moves through free air. */
  KX_NavAirMap *AirMapFor(float radius, LN_JobPool *pool);
  const KX_NavAirMap *ExistingAirMap(float radius) const;
  void CollectWater(std::vector<KX_NavWaterVolume> &r_water) const;
  void UpdateAirMaps(LN_JobPool *pool);
  void PlanAir(LN_JobPool *pool);
  void SteerAir(KX_NavAgent &agent);
  void AvoidAir(KX_NavAgent &agent, float dt);
  bool AirSlideTarget(const KX_NavAgent &agent, const MT_Vector3 &wanted, MT_Vector3 &r_target) const;
  /** Walls and ceilings (phase 9): the air map of a crawler's width with its surfaces (made now
   * when missing); each crawler's surface this step (on the helpers); where a crawler's move
   * puts it (its body back at its height from the surface there; false: it stays). */
  KX_NavAirMap *SurfaceMapFor(float radius, LN_JobPool *pool);
  void SenseSurfaces(LN_JobPool *pool);
  bool CrawlTarget(const KX_NavAgent &agent, const MT_Vector3 &wanted, MT_Vector3 &r_target) const;
  /** The level's triangles in a box (the geometry source the scene's navmesh reads). */
  void CollectLevel(const MT_Vector3 &aabb_min, const MT_Vector3 &aabb_max, KX_NavGeometry &r_geometry);
  /** The air maps' level (plan item 11): the level's triangles and the solid resting obstacles,
   * each a closed convex piece. */
  void CollectAirLevel(const MT_Vector3 &aabb_min,
                       const MT_Vector3 &aabb_max,
                       KX_NavGeometry &r_geometry);
  /** Plan item 11: the step's resting obstacles against the air maps' solids. */
  void UpdateAirSolids();
  /** For a query: a map near `radius` (else asked for, made at the next step), or the nearest
   * larger, or the largest; null when there is none at all. */
  const KX_NavAirMap *AirMapForQuery(float radius);
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
  /** Density-aware routes (2026-11-19): this step's crowds, read by its searches (a way through
   * a crowd costs the time lost in it) and by CrowdedAhead. */
  void CountCrowding(LN_JobPool *pool);
  /** A crowd on the agent's way ahead: it may search again for a freer way. */
  bool CrowdedAhead(KX_NavAgent &agent) const;
  /** Avoidance Dense (2026-11-19): position-based crowds instead of ORCA for Dense agents. */
  void SolveDense(float dt, LN_JobPool *pool);
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
  /** Plan item 9 (2026-11-20): the scene navmesh's setup problems (nothing to walk on, NPCs
   * rounded up or sharing a size, links off the floor), each reported once. */
  void CheckSceneNavMeshProblems();
  void DrawAgents() const;
  /** Visualization's lines and circles (as lines): into the rasterizer, or a test's sink, at
   * most the step's budget (false: spent). */
  bool DrawLine(const MT_Vector3 &a, const MT_Vector3 &b, const MT_Vector4 &color) const;
  void DrawCircle(const MT_Vector3 &center,
                  float radius,
                  const MT_Vector4 &color,
                  const MT_Vector3 &normal,
                  int sectors) const;
  void DrawLinkLine(const MT_Vector3 &start,
                    const MT_Vector3 &end,
                    const std::string &kind,
                    const MT_Vector4 &color,
                    bool dashed,
                    float radius) const;
  mutable size_t m_drawnLines = 0;
  mutable size_t m_drawBudget = 0;
  std::function<void(const MT_Vector3 &, const MT_Vector3 &, const MT_Vector4 &)>
      m_lineSinkForTests;
  void RunJobs(LN_JobPool *pool, size_t count, const std::function<void(size_t)> &item);

  KX_Scene *m_scene;
  Settings m_settings;
  PHY_IPhysicsEnvironment *m_physicsForTests = nullptr;
  std::vector<std::unique_ptr<KX_NavAgent>> m_agents;
  std::unordered_map<const KX_GameObject *, KX_NavAgent *> m_agentByObject;
  uint64_t m_nextAgentOrder = 0;
  /** Plan item 10: the places given out round each pursued target, kept while its chasers stay
   * the same and it stays within kChaseRingMove of where they were given. */
  struct ChaseRing {
    std::vector<uint64_t> chasers;
    MT_Vector3 target_at = MT_Vector3(0.0f, 0.0f, 0.0f);
    /** Checked against a navmesh (none known when given: given again once one is). */
    bool checked = false;
    /** Where the target was at the last step, and the steps it has stood (moved under
     * kChaseStillSpeed) since: places are given round a target that stands; one on the move is
     * chased as a lone chaser chases (places round it moved on with it and were never reached:
     * a wolf pack after a walking player caught nothing). */
    MT_Vector3 last = MT_Vector3(0.0f, 0.0f, 0.0f);
    uint32_t still_steps = 0;
    /** Its speed smoothed over about five steps (a shove of a few millimetres is no walk). */
    float speed = 0.0f;
    /** The step the places were given (given again when a chaser got nowhere for a while: shut
     * in by the others, it takes the free place nearest it then; at most every two seconds). */
    uint64_t given_step = 0;
    struct Place {
      bool placed = false;
      float angle = 0.0f, radius = 0.0f, reach = 0.0f, orbit = 0.0f;
    };
    std::vector<Place> places;
  };
  std::unordered_map<const KX_GameObject *, ChaseRing> m_chaseRings;
  std::vector<KX_NavRequest> m_pendingRequests;
  uint64_t m_stepCount = 0;
  /** UPBGE_NAV_PROFILE=1: each phase's time summed over 300 steps, then printed (CM_Debug). */
  bool m_profile = false;
  double m_profileMs[12] = {};
  uint32_t m_profileSteps = 0;
  uint64_t m_profileTiles[4] = {};
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
   * override changed (a scan of every object each step cost big scenes): marked ones, then
   * (Avoid Physics Objects) physics objects. */
  std::vector<KX_GameObject *> m_obstacleCandidates;
  std::vector<KX_GameObject *> m_physicsObstacleCandidates;
  uint64_t m_obstacleListGeneration = UINT64_MAX;
  bool m_obstacleCandidatesDirty = true;
  std::vector<std::string> m_areaNames;
  std::vector<KX_NavLink> m_links;
  /** The links' places in m_links by their own numbers; each link object's number (kept while
   * it has a link). */
  std::unordered_map<uint32_t, size_t> m_linkIndexById;
  /** What each hand-placed jump link asks, either way (the way back's id with kNavLinkReverse). */
  std::unordered_map<uint32_t, KX_NavLinkNeeds> m_jumpNeeds;
  /** The areas of jump and drop links ("jump", "drop"). */
  int m_jumpArea = 0, m_dropArea = 0;
  /** The scene's gravity (m/s2), read at each step's start (9.81 without physics). */
  float m_gravity = 9.81f;
  /** Vehicles (2026-11-19): each object with Vehicle on, its shape's type (the navmeshes
   * built once for every copy of a shape), read again when the object list changes. */
  std::vector<std::unique_ptr<KX_NavVehicle>> m_vehicles;
  std::vector<std::unique_ptr<KX_NavVehicleType>> m_vehicleTypes;
  uint64_t m_vehicleListGeneration = UINT64_MAX;
  bool m_vehiclesForTests = false;
  std::vector<KX_GameObject *> m_vehicleObjectsForTests;
  std::function<void(KX_GameObject *, KX_NavGeometry &)> m_vehicleGeometryForTests;
  /** Vehicles: read again when the object list changed, their moves since the last step, their
   * navmeshes built (main thread; the tiles on the helpers). */
  void UpdateVehicles(float dt, LN_JobPool *pool);
  /** Their navmeshes built, with their links and (a private copy) their obstacles (after
   * GatherObstacles). */
  void UpdateVehicleNavMeshes(LN_JobPool *pool);
  /** A new type of vehicle shape (its navmeshes built from `geometry`, with `links` inside). */
  KX_NavVehicleType *NewVehicleType(uint64_t key,
                                    KX_NavGeometry geometry,
                                    const std::vector<KX_NavLink> &links);
  /** `vehicle` walks `type` from now on (its views, docks and NPCs' navmeshes found again). */
  void RetypeVehicle(KX_NavVehicle &vehicle, KX_NavVehicleType *type);
  /** Its type's link numbers -> its own links'. */
  void MapVehicleLinks(KX_NavVehicle &vehicle);
  /** Each vehicle's pose for the next step's start (its expected one: KX_NavVehicle). */
  void EndVehicleStep();
  KX_NavVehicle *FindVehicleRecord(const KX_GameObject *vehicle) const;
  /** Which vehicle an agent stands on (null: the world): a Character's ground's, else the
   * vehicle whose navmesh lies nearest under it, above the world's. `r_found`: some navmesh
   * (or a Character's ground) told. */
  KX_GameObject *FrameUnder(KX_NavAgent &agent, PHY_ICharacter *character, bool &r_found);
  /** The frame whose navmesh lies under `point` (half a metre above it to 2.5 m below; the
   * highest), for agents of `size`. */
  KX_GameObject *FrameAt(const MT_Vector3 &point, const KX_NavAgentSize &size, bool &r_found);
  /** Links between frames: split from the world's, their ends and open state this step. */
  void UpdateFrameLinks(float dt);
  /** An agent whose goal is on another frame, or whose own frame does not reach it: its way
   * over frame links (the goal it walks to now: the next link's start), waiting at a closed
   * one, crossing an open one. */
  void RouteAcrossFrames(KX_NavAgent &agent);
  /** Its way over at most two frame links (agent.route; empty: none), and its cost (straight
   * lines, FLT_MAX: none). */
  float ChooseFrameRoute(KX_NavAgent &agent, KX_GameObject *goal_frame);
  /** Some link of the world is longer than Detour holds (routed as frame links). */
  bool m_hasLongLinks = false;
  std::vector<KX_NavFrameLink> m_frameLinks;
  /** The links Detour takes (no frame links). */
  std::vector<KX_NavLink> m_worldLinks;
  /** Each frame link's ends at the last step (by its number): whether they move apart. */
  std::unordered_map<uint32_t, std::pair<MT_Vector3, MT_Vector3>> m_frameLinkSeen;
  /** A vehicle standing still: its docking links (its deck's open edges beside the world's
   * navmesh, a step apart in height). */
  void FindDocks(KX_NavVehicle &vehicle);
  uint32_t m_nextDockId = kNavDockLinkBase;
  std::unordered_map<const KX_GameObject *, uint32_t> m_linkIds;
  uint32_t m_nextLinkId = 1;
  uint64_t m_linkListGeneration = UINT64_MAX;
  bool m_linksForTests = false;
  /** Tests: each link object's Link Enabled and end when the links were set. */
  std::unordered_map<const KX_GameObject *, std::pair<bool, KX_GameObject *>> m_testLinkSeen;
  /** 2026-11-20: the scene's navigation settings serial the links and the vehicles were read at
   * (a setting changed while playing: read again). */
  uint64_t m_linkSettingsSerial = UINT64_MAX;
  uint64_t m_vehicleSettingsSerial = UINT64_MAX;
  /** Closed links by number (FindClosedLink), the next closing's generation. */
  std::unordered_map<uint32_t, KX_NavClosedLink> m_closedLinks;
  uint32_t m_nextClosedGeneration = 1;
  /** The links' objects and the objects above them (IsLinkObject). */
  std::unordered_set<const KX_GameObject *> m_linkObjects;
  /** The links read again (tests or the scene): closed ones listed, link objects found, the
   * navmeshes told where a link opened or closed (ways through it are checked). */
  void AdoptLinks(std::vector<KX_NavLink> fresh);
  std::unique_ptr<dtQueryFilter> m_queryFilter;
  /** The physics notes static bodies that change (TrackLevelChanges). */
  bool m_staticTracking = false;
  /** Level pieces that changed and have not come to rest: where they were (when they were a
   * level piece), the last step they changed. */
  struct MovedPiece {
    bool had_bounds = false;
    MT_Vector3 before_min, before_max;
    uint64_t changed_step = 0;
  };
  std::unordered_map<const KX_GameObject *, MovedPiece> m_movedPieces;
  /** Density-aware routes: each navmesh's crowded polygons this step and how much more a meter
   * there costs (Detour's 64-bit polygon reference), and the agents looking at their way ahead
   * this step. */
  std::unordered_map<const KX_NavMesh *, std::unordered_map<uint64_t, float>> m_crowding;
  std::vector<uint32_t> m_crowdLookScratch;
  /** Flying and swimming: the air maps (one a flyer size), the water they hold, the water for
   * tests, the places a level piece changed in (their air bricks are built again), sizes
   * Navmesh Query asked for (made at the next step; the queries may run on the helpers), where
   * the flyers' searches go on from next step, whether a flyer too big for every map was
   * warned of. */
  std::vector<std::unique_ptr<KX_NavAirMap>> m_airMaps;
  std::vector<KX_NavWaterVolume> m_water;
  std::vector<KX_NavWaterVolume> m_waterForTests;
  bool m_hasWaterForTests = false;
  bool m_airSizeWarned = false;
  std::vector<std::pair<MT_Vector3, MT_Vector3>> m_airChanges;
  /** Plan item 11 (2026-11-20): obstacles at rest (an Obstacle object standing still, an Avoid
   * Physics Objects body asleep: the step's carved ones) are solid in the air maps once at rest
   * kAirSolidSteps steps (a collapsing house's rubble: flyers plan round it, not only dodge it);
   * the box of one coming to rest or waking goes to m_airChanges. */
  struct AirSolid {
    MT_Vector3 center = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 half = MT_Vector3(0.0f, 0.0f, 0.0f);
    float yaw = 0.0f;
    bool round = false;
    uint32_t still_steps = 0;
    bool solid = false;
    uint64_t seen_step = 0;
  };
  std::unordered_map<const KX_GameObject *, AirSolid> m_airSolids;
  std::vector<float> m_airSizesAsked;
  std::mutex m_airSizesMutex;
  /** Auto build (2026-11-20): a query found no scene navmesh (any thread). */
  mutable std::atomic<bool> m_sceneNavMeshAsked{false};
  bool m_autoEmptyChecked = false;
  /** The links' ends checked against the scene's navmesh (item 9; again after the list
   * changed). */
  bool m_linksNeedCheck = true;
  /** The level changed near a link's end: the ends find their floor again (item 4). */
  bool m_linkSnapsStale = false;
  /** Item 12: the navigating bodies handed to physics at each step's end (none: not again). */
  void HandOverNavigatingBodies();
  /** Moved by the navigation in this step and not pushable by physics (item 12). */
  static bool PassesThroughOthers(const KX_NavAgent &agent);
  /** A walking Character's way out of the navigating Characters it is inside (item 12). */
  MT_Vector3 PassThroughSeparation(size_t index) const;
  std::vector<PHY_IPhysicsController *> m_navigatingScratch;
  bool m_navigatingHandedOver = false;
  /** The play report's cost (item 9): seconds of the steps that did anything, and how many. */
  double m_reportSeconds = 0.0;
  uint64_t m_reportSteps = 0;
  void NoteLevelChangedNearLinks(const MT_Vector3 &aabb_min, const MT_Vector3 &aabb_max);
  std::string m_autoCellReason;
  /** Problems (item 9). */
  mutable std::mutex m_problemsMutex;
  std::vector<KX_NavProblem> m_problems;
  std::unordered_map<std::string, size_t> m_problemIndex;
  size_t m_airSearchCursor = 0;
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
  /* (0.1 mm more: an NPC moved straight stops exactly `arrive` from its goal, and the rounding of
   * its place, 16 - 15.9 = 0.10000038, kept it from arriving: it stood a second, until the
   * crowd's "as near as it gets"; 2026-11-19, the examples' platform.) */
  const float reach = arrive + 1e-4f;
  return dx * dx + dy * dy <= reach * reach &&
         (dz < 0.0f ? -dz : dz) <= (height > 2.0f ? height : 2.0f) + arrive;
}

/** Settings from a scene's game settings (Navigation Mesh and Obstacle Simulation panels). */
KX_Navigation::Settings KX_NavigationSettingsOfScene(const KX_Scene *scene);
