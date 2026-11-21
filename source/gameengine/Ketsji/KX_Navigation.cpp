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
#include <unordered_set>

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
#include "KX_NavJumpLinks.h"
#include "KX_NavMesh.h"
#include "KX_NavMeshObject.h"
#include "KX_NavOrca.h"
#include "KX_NavOrca3D.h"
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
/** A way through a crowd ahead is searched again at most this often (a crowd at one door spreads
 * to another as it gathers). */
constexpr int kCrowdReplanSteps = 60;
/** Agents a job takes at once (jobs of single agents cost more to hand out than they do). */
constexpr size_t kAgentsPerJob = 32;
/** Far agents (Settings::detail_distance) avoid others every this many steps. */
constexpr int kFarAvoidSteps = 4;
/** Flying and swimming (phase 8): air map sizes at most; flyers' searches a step at most; the
 * nodes one search may look at; steps between looks at a flyer's way ahead (staggered by
 * agent); a way ending short is searched again this often. */
constexpr int kMaxAirMaps = 4;
constexpr int kMaxAirSearchesPerStep = 16;
constexpr int kAirSearchNodes = 20000;
constexpr int kAirCheckSteps = 15;
constexpr int kAirPartialReplanSteps = 60;
/** A flyer outside free air with none near climbs straight up out of it this far at most (the
 * searches snap their ends as far). Water volumes are looked at again this often (rising
 * water, a moved tank). */
constexpr float kAirUpReach = 64.0f;
constexpr int kWaterCheckSteps = 30;
/** Plan item 11: an obstacle at rest this many steps is solid in the air maps (one waking is
 * free at once; flyers dodge it while it moves). Their pieces' numbers start here (the level's
 * are its bodies' places). */
constexpr uint32_t kAirSolidSteps = 10;
constexpr int32_t kAirSolidPieceBase = 1 << 28;
/** Going round a group standing in the way (polite agents): after standing blocked this long
 * (seconds), looking this far ahead, turning by the least of these angles that clears it, the
 * way straight again once clear by this margin, at most this long a try; never within this of
 * the reach where a crowd at its goal counts as arrived. */
constexpr float kRoundAfter = 0.5f;
constexpr float kRoundLook = 3.0f;
constexpr float kRoundAngles[] = {30.0f, 60.0f, 90.0f, 120.0f};
constexpr float kRoundClearMargin = 0.1f;
constexpr float kRoundMaxTime = 8.0f;
constexpr float kRoundGoalMargin = 1.0f;
/** Keeping to the right (lanes, 2026-11-19): agents coming the other way within this far ahead,
 * this near its line (beyond touching), make a moving agent drift right, at most this much of
 * its speed. */
constexpr float kKeepRightAhead = 10.0f;
constexpr float kKeepRightLine = 0.3f;
constexpr float kKeepRightMost = 1.0f;
/** Two polite agents heading opposite ways pressing face to face (head_on_time): after this
 * long (seconds) they shrink for each other in ORCA, to kHeadOnSmallest of their size over
 * kHeadOnShrinkTime more, and slip past (Planet Coaster's way out of head-on gridlock: in the
 * head-on test one NPC of 80 stood for good without it). Agents going the same way never shrink
 * for each other (a jam at a door keeps its spacing). Dense agents press past instead. */
constexpr float kHeadOnAfter = 0.75f;
constexpr float kHeadOnShrinkTime = 1.5f;
constexpr float kHeadOnSmallest = 0.35f;
/** Corners (2026-11-19). A polite agent aims off the corner it turns round, on the turn's outer
 * side, by this much of its radius for each unit of |in - out| (a right angle: 1.41), at least
 * kCornerAimLeast: aimed at the corner itself (a point on the navmesh's wall), avoidance let it
 * near the wall's line only as fast as the distance left shrank, and it crawled the last
 * decimetres of every corner (DetourCrowd's anticipated turns aim wide the same way; Dense aims
 * a whole radius off). Walls stop holding it this near a corner (kx_orca::Params). */
constexpr float kCornerAim = 0.25f;
constexpr float kCornerAimLeast = 0.03f;
constexpr float kWallCornerRelease = 0.03f;

/** How much of their size two agents keep for each other: 1, or less for a pair heading opposite
 * ways (`a`, `b` unit or zero), each in front of the other, by the longer pressing of the two
 * (`pressed`, seconds: the same for both). */
static float head_on_size(const MT_Vector3 &a_heading,
                          const MT_Vector3 &b_heading,
                          const MT_Vector3 &a_to_b,
                          const float pressed)
{
  if (pressed <= kHeadOnAfter || a_heading.length2() < 0.5f || b_heading.length2() < 0.5f ||
      float(a_heading.dot(b_heading)) > -0.5f || float(a_to_b.dot(a_heading)) <= 0.0f)
  {
    return 1.0f;
  }
  const float t = std::min((pressed - kHeadOnAfter) / kHeadOnShrinkTime, 1.0f);
  return 1.0f - (1.0f - kHeadOnSmallest) * t;
}

/** A jump or a drop costs this many times its length (2026-11-19): walking is preferred unless
 * the jump saves real walking (the links found are only where it does). */
constexpr float kJumpCost = 1.5f;
/** A closed Wait link costs this many metres more (2026-11-20): an NPC goes round when that is
 * at most this much longer, else it waits at the link (a door it waits at). */
constexpr float kClosedWaitCost = 10.0f;
/** A link longer than Detour holds (a teleporter) is taken instead of a walk that reaches the goal
 * too when it is at least this many metres shorter (2026-11-20). */
constexpr float kLongLinkShortcut = 5.0f;
/** NPCs within this many metres of an open link between frames avoid each other whatever they
 * stand on (E4, 2026-11-20: crowding at gangways and lift doors). */
constexpr float kGangwayCrowd = 3.0f;
/** When Failed's "Stuck" (plan item 8): no progress for this long (the stall measure), not waiting
 * at a closed link. */
constexpr float kStuckSeconds = 3.0f;
/** Its "Unreachable": standing this long at the end of a partial way. */
constexpr float kUnreachableSeconds = 1.0f;
/** Its "Off Navmesh": no polygon under it in this many steps running. */
constexpr int kOffNavmeshSteps = 10;
/** Item 12: navigating Characters inside each other walk out of the overlap in this time. */
constexpr float kPassThroughSeparationTime = 0.15f;
/** Plan item 4: how far below and above a link's end on no floor its floor is looked for. */
constexpr float kLinkSnapBelow = 1.5f;
constexpr float kLinkSnapAbove = 0.5f;
/** Plan item 10: chasers of one target stand on at most this many rings round it (the rest press
 * in as before), and their places are given out again when the target moved this far since. */
constexpr int kChaseRings = 6;
constexpr float kChaseRingMove = 0.5f;
/** A chaser whose place lies more than this far round the ring from where it is (radians) goes
 * round, outside the ring, this much at a time, once within kChaseOrbitNear metres of the ring. */
constexpr float kChaseOrbitStep = 0.9f;
constexpr float kChaseOrbitNear = 3.0f;
/** A chaser kept from its place this long, within a body of it, has arrived as near as it gets. */
constexpr float kChasePressSeconds = 3.0f;
/** Going round the rings and got nowhere this long (seconds): the other way round. */
constexpr float kChaseTurnRoundStall = 1.0f;
/** A target moving slower than this (m/s) for kChaseStillSteps steps stands: its chasers get
 * places round it. */
constexpr float kChaseStillSpeed = 0.5f;
constexpr uint32_t kChaseStillSteps = 6;

/** An NPC's jump limits (2026-11-19). Unlimited (not `limited`): every link, as before. A
 * Character's come from its jump (`physics`: its jump speed, gravity and a run's speed on the
 * plan); Set Navigation Attribute's are set (the widest gap, the highest jump up above its climb,
 * the deepest drop). */
struct KX_NavJumpLimits {
  bool limited = false;
  bool physics = false;
  float jump_speed = 0.0f, plan_speed = 0.0f, gravity = 9.81f;
  float gap = 0.0f, up = 0.0f, drop = 0.0f, climb = 0.0f;

  bool operator==(const KX_NavJumpLimits &o) const
  {
    return limited == o.limited && physics == o.physics && jump_speed == o.jump_speed &&
           plan_speed == o.plan_speed && gravity == o.gravity && gap == o.gap && up == o.up &&
           drop == o.drop && climb == o.climb;
  }
  bool operator!=(const KX_NavJumpLimits &o) const
  {
    return !(*this == o);
  }
  bool Allows(const KX_NavLinkNeeds &needs) const
  {
    if (!limited) {
      return true;
    }
    if (needs.rise < -climb && -needs.rise > drop + 1e-4f) {
      return false;
    }
    if (physics) {
      return KX_NavJumpReachable(
          jump_speed, plan_speed, gravity, climb, needs.travel, needs.rise);
    }
    return needs.gap <= gap + 1e-4f && needs.rise <= climb + up + 1e-4f;
  }
};

/** Closed areas, crowded polygons and jump limits on top of Detour's area costs. */
class KX_NavFilter : public dtQueryFilter {
 public:
  /** Bit per area id. */
  uint64_t closed_areas = 0;
  /** Jump links (2026-11-19): the NPC's limits (links it can't make are closed to it), where
   * to read a link's needs, the areas jumps and drops are in (they cost kJumpCost x). */
  KX_NavJumpLimits jump;
  const KX_Navigation *navigation = nullptr;
  int jump_area = -1, drop_area = -1;
  /** Links the NPC jumped and missed (its own list), closed until their step; `now` the step. */
  const std::vector<std::pair<uint32_t, uint64_t>> *failed = nullptr;
  uint64_t now = 0;
  /** Density-aware routes (2026-11-19): crowded polygons of the agent's navmesh this step, each
   * with the part its cost grows by (KX_Navigation::CountCrowding; read-only while searching). */
  const std::unordered_map<uint64_t, float> *crowding = nullptr;
  /** Closed Wait links it gave up waiting at (2026-11-20): their numbers and the closing it gave
   * up on (KX_NavClosedLink::generation): closed to it as Go Round links are, until they close
   * again. */
  std::vector<std::pair<uint32_t, uint32_t>> gave_up;
  /** On a vehicle (E2): its type's link numbers -> the vehicle's own links' (null: none). */
  const std::vector<std::pair<uint32_t, uint32_t>> *id_map = nullptr;

  /** A link number of the navmesh it walks as its own link's (a vehicle copy's own). */
  uint32_t OwnLinkId(const uint32_t id) const
  {
    if (id_map != nullptr) {
      const uint32_t bare = id & ~kNavLinkReverse;
      for (const std::pair<uint32_t, uint32_t> &m : *id_map) {
        if (m.first == bare) {
          return m.second | (id & kNavLinkReverse);
        }
      }
    }
    return id;
  }

  /** The link of an off-mesh connection polygon (its own number), or 0. */
  static uint32_t LinkIdOf(const dtMeshTile *tile, const dtPoly *poly)
  {
    const int index = int(poly - tile->polys) - tile->header->offMeshBase;
    if (index >= 0 && index < tile->header->offMeshConCount) {
      return tile->offMeshCons[index].userId;
    }
    return 0;
  }

  /** A closed link (2026-11-20): closed to it (Go Round, or a Wait link it gave up on). */
  bool ClosedToIt(const uint32_t navmesh_id) const
  {
    const uint32_t id = OwnLinkId(navmesh_id);
    const KX_NavClosedLink *closed = navigation->FindClosedLink(id & ~kNavLinkReverse);
    if (closed == nullptr) {
      return false;
    }
    if (!closed->wait) {
      return true;
    }
    for (const std::pair<uint32_t, uint32_t> &g : gave_up) {
      if (g.first == (id & ~kNavLinkReverse) && g.second == closed->generation) {
        return true;
      }
    }
    return false;
  }

  bool passFilter(const dtPolyRef ref, const dtMeshTile *tile, const dtPoly *poly) const override
  {
    if ((closed_areas >> poly->getArea()) & 1u) {
      return false;
    }
    if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION && navigation != nullptr &&
        navigation->HasClosedLinks() && ClosedToIt(LinkIdOf(tile, poly)))
    {
      return false;
    }
    if ((jump.limited || (failed != nullptr && !failed->empty())) &&
        poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
    {
      const int index = int(poly - tile->polys) - tile->header->offMeshBase;
      if (index >= 0 && index < tile->header->offMeshConCount) {
        const uint32_t id = tile->offMeshCons[index].userId;
        KX_NavLinkNeeds needs;
        if (jump.limited && navigation != nullptr && navigation->LinkNeeds(id, needs) &&
            !jump.Allows(needs))
        {
          return false;
        }
        if (failed != nullptr) {
          for (const std::pair<uint32_t, uint64_t> &f : *failed) {
            if (f.first == id && f.second > now) {
              return false;
            }
          }
        }
      }
    }
    return dtQueryFilter::passFilter(ref, tile, poly);
  }

  float getCost(const float *pa,
                const float *pb,
                const dtPolyRef prevRef,
                const dtMeshTile *prevTile,
                const dtPoly *prevPoly,
                const dtPolyRef curRef,
                const dtMeshTile *curTile,
                const dtPoly *curPoly,
                const dtPolyRef nextRef,
                const dtMeshTile *nextTile,
                const dtPoly *nextPoly) const override
  {
    float cost = dtQueryFilter::getCost(
        pa, pb, prevRef, prevTile, prevPoly, curRef, curTile, curPoly, nextRef, nextTile, nextPoly);
    if (curPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
      if (int(curPoly->getArea()) == jump_area || int(curPoly->getArea()) == drop_area) {
        cost *= kJumpCost;
      }
      /* A closed Wait link (2026-11-20) is taken when going round costs more than waiting
       * (counted as kClosedWaitCost metres). */
      if (navigation != nullptr && navigation->HasClosedLinks() &&
          navigation->FindClosedLink(OwnLinkId(LinkIdOf(curTile, curPoly)) & ~kNavLinkReverse) !=
              nullptr)
      {
        cost += kClosedWaitCost;
      }
    }
    if (crowding != nullptr && !crowding->empty()) {
      auto it = crowding->find(uint64_t(curRef));
      if (it != crowding->end()) {
        cost *= 1.0f + it->second;
      }
    }
    return cost;
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
  /** Its size checked against the scene's navmesh (plan item 9: problems reported once). */
  bool size_checked = false;
  /** No polygon under it within the snap extents (item 8's "Off Navmesh"); told once. */
  bool off_navmesh = false;
  /** It gave up waiting at a closed link this step (item 8's "Gave Up"). */
  bool gave_up = false;
  /** The reason When Failed pulsed for this order (KX_NAV_FAILURE_*; none: not yet). */
  uint8_t failure_reported = 0;
  /** Seconds it has stood at the end of a partial way (item 8's "Unreachable"). */
  float partial_end_time = 0.0f;
  bool off_navmesh_reported = false;
  int off_navmesh_steps = 0;
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
  /** Flying or swimming (phase 8): the sphere it fills, its width and its height (a tall
   * jellyfish scraped ceilings by its width alone); without bounds, the radius. */
  float air_radius = 0.5f;
  /** What it can walk (its navmesh's class): a Character's own climb and slope (2026-11-19). */
  KX_NavAgentSize size;

  KX_NavMeshObject *navmesh_object = nullptr;
  KX_NavMesh *navmesh = nullptr;
  KX_NavFilter filter;
  /** Set Navigation Attribute's Jump Limits (2026-11-19), when set (else its own: a
   * Character's from its jump, anything else none). */
  bool jump_set = false;
  KX_NavJumpLimits jump_own;
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
  /** How long its path was when found (through its polygons' middles), and how far it walked
   * since: a change of the navmesh nearer than that may open a shorter way (2026-11-19). */
  float path_estimate = 0.0f;
  float traveled = 0.0f;
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
  /** Pursue with Polite avoidance, others after the same target (plan item 10, 2026-11-20): its
   * place round the target (an angle on the plan and a ring's radius round where the target will
   * be), arrived within `chase_reach` of it. Given out by AssignChaserPlaces. */
  bool chase_place = false;
  float chase_angle = 0.0f;
  float chase_radius = 0.0f;
  float chase_reach = 0.0f;
  /** How far out it goes round the rings to a place on the far side (outside the last one). */
  float chase_orbit = 0.0f;
  /** Which way round it goes (1 counterclockwise, -1 clockwise; 0: not chosen yet): chosen once
   * for a place, the shorter way unless a wall stands in it (a target with its back to a wall). */
  int chase_side = 0;
  /** The step it last turned round (got nowhere going round one way: the other way). */
  uint64_t chase_side_step = 0;
  /** (AssignChaserPlaces: whether it had a place in the step before.) */
  bool chase_place_kept = false;
  /** This step: at its place (else, arrived by catching its target, it settles there making way:
   * toward `chase_settle`, its place or a point round the rings on the way). */
  bool chase_settled = false;
  MT_Vector3 chase_settle = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** Plan item 11 (2026-11-20): a flyer landed on a vehicle's deck rides it (its frame, carried
   * as a sliding walker is; a Dynamic one gets the deck's velocity) until a goal off that deck
   * makes it take off; the hull spheres it is let through this step (its last approach from
   * above, its own deck, its take-off); the deck it is coming down on. */
  KX_GameObject *riding = nullptr;
  KX_GameObject *through_hull = nullptr;
  KX_GameObject *landing_on = nullptr;
  /** Seconds it wanted to move and got nowhere since it stood at `stall_anchor` (it goes round a
   * standing group). */
  float stall_time = 0.0f;
  MT_Vector3 stall_anchor = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool stall_anchored = false;
  float stall_since = 0.0f;
  /** Seconds it wanted to move and hardly moved in each step (Dense: it squeezes; a crowd at
   * its goal: it arrived). */
  float press_time = 0.0f;
  /** The goal it pressed toward (a goal that jumps starts the count again). */
  MT_Vector3 press_goal = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** How fast it really walked, smoothed (KX_NavAgentInfo::speed), and whether it stands. */
  float smooth_speed = 0.0f;
  bool standing = true;
  /** Avoidance Dense (2026-11-19): pushed this step though it arrived (it is moved back off the
   * spot it was shoved from, and others may shove it again). */
  bool dense_pushed = false;
  /** A chaser at its place round its target (plan item 10) steps aside for one passing (Polite:
   * ORCA with no wish of its own), then walks back: moved this step though it arrived. */
  bool making_way = false;
  /** Density-aware routes: a crowd on its way ahead (checked every kCrowdReplanSteps since its
   * search; it searches again for a freer way), and its own crowd's extra cost a meter. */
  bool crowded_ahead = false;
  float crowd_cost = 0.0f;
  /** Flying or swimming (Navigate's Move, phase 8, 2026-11-19): asked to (`flies`), its air map
   * (null: it walks, or it cannot fly: a Character), whether it swims (keeps to water) and floats
   * (a Dynamic body swimming: steers across only, its height left to the buoyancy), its way (the
   * corners still ahead from `air_next`), the goal the way was searched for, whether it gets
   * there. */
  bool flies = false;
  KX_NavAirMap *air = nullptr;
  bool water = false;
  bool floating = false;
  std::vector<MT_Vector3> air_path;
  size_t air_next = 0;
  MT_Vector3 air_path_goal = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool air_complete = false;
  bool air_needs_search = true;
  int air_steps_since_search = 0;
  /** Searches in a row that ended short (each waits twice as long: at most 10 s). */
  int air_short_searches = 0;
  bool air_warned = false;
  /** Walls and ceilings (Navigate's Climb, phase 9, 2026-11-19): it crawls (`air` then is the
   * map of its width with its shell, the air_* way over the shell), the surfaces it may use (a
   * KX_NavAirMap::SurfaceKind mask) and their costs; the surface under it this step (the level's
   * nearest point and the way out of it; on_surface false: none near), how far its origin
   * stays from the surface (its bounds' bottom); a Character's gravity before it climbed. */
  bool crawls = false;
  int crawl_kinds = 0;
  KX_NavAirMap::SurfaceCosts crawl_costs;
  bool on_surface = false;
  MT_Vector3 surface_point = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 surface_normal = MT_Vector3(0.0f, 0.0f, 1.0f);
  /** The way straight to its next corner (the last step's): the surface it stands on is the one
   * this runs along best (SupportSurface). */
  MT_Vector3 crawl_heading = MT_Vector3(0.0f, 0.0f, 0.0f);
  float crawl_offset = 0.3f;
  bool gravity_saved = false;
  MT_Vector3 saved_gravity = MT_Vector3(0.0f, 0.0f, -9.81f);
  /** The turn standing it on its surface (and Look At round the surface's normal), worked out
   * on the helpers (TurnAgents). */
  MT_Matrix3x3 crawl_turn = MT_Matrix3x3::Identity();
  bool crawl_turn_ok = false;
  /** Going round a group standing in its way (polite agents, 2026-11-19): the side (+1 left,
   * -1 right, 0 not), for how long, and whether it tried the other side already. */
  int8_t round_side = 0;
  bool round_flipped = false;
  float round_time = 0.0f;
  /** Seconds it has been pressing face to face against one heading the other way (falls back
   * twice as fast once free): the two shrink for each other by it (head_on_size). */
  float head_on_time = 0.0f;
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
  /** On a link (an off-mesh connection): from start to end over `link_duration`. The link's
   * object and kind as it was taken (links come and go while playing: its crossing keeps them,
   * the object cleared when it goes). */
  bool on_link = false;
  bool link_started = false;
  KX_GameObject *link_object = nullptr;
  std::string link_kind;
  /** Detour's id of the link it crosses (KX_Navigation::LinkNeeds / LinkKind). */
  uint32_t link_id = 0;
  MT_Vector3 link_start = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 link_end = MT_Vector3(0.0f, 0.0f, 0.0f);
  float link_time = 0.0f;
  float link_duration = 0.0f;
  /** A jump or a drop: over its arc, its top `link_clearance` over the higher end. */
  bool link_jump = false;
  float link_clearance = 0.0f;
  /** A physics jump (2026-11-19, Scene > Navigation Mesh > Jumps Physics): a Character or a
   * body moved as Dynamic jumps a jump link itself. Launched once in HandOver (`link_launch`,
   * upward `link_up_speed`), steered toward the landing through the air (`link_steer`: walk
   * direction or velocity), landed when it stands again (a Character) or stops falling; on the
   * link's end it walks on, elsewhere its way is searched again and the link is closed to it
   * for a while (`failed_links`). `link_from`: where it was at the launch. */
  bool link_physics = false;
  bool link_launch = false;
  float link_up_speed = 0.0f;
  float link_plan_speed = 0.0f;
  MT_Vector3 link_from = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 link_steer = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool link_left_ground = false;
  bool link_fell = false;
  /** Its crossing ended this step (On Navigation Link's On Link End); how: on the link's end. */
  bool link_ended = false;
  bool link_landed = false;
  /** Links it jumped and missed, closed to it until the step given. */
  std::vector<std::pair<uint32_t, uint64_t>> failed_links;
  /** Read in PrepareAgents (main thread): it can launch (a Character that can, or a body moved
   * as Dynamic), it stands on the ground (a Character), the strength of its gravity. */
  bool can_launch = false;
  bool grounded = false;
  float gravity = 9.81f;
  /** The vehicle it stands on (2026-11-19; null: the world): chosen when its corridor starts,
   * changed by a crossing. The deck's velocity where it stands over the coming step (a body
   * moved as Dynamic adds it to its own). */
  KX_GameObject *frame = nullptr;
  MT_Vector3 frame_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** Its frame is known (chosen under it, or by a crossing): kept until something else moves
   * it (a teleport). */
  bool frame_known = false;
  /** Its way to a goal on another frame (frame link numbers, each with its way: from its end
   * when reversed), chosen at `route_step` for the goal's frame `route_goal_frame`; routing:
   * walking it now (the goal it walks to is the next link's start; its own goal is
   * `final_goal`); waiting at a closed link's start (its object). */
  std::vector<std::pair<uint32_t, bool>> route;
  uint64_t route_step = 0;
  KX_GameObject *route_goal_frame = nullptr;
  bool routing = false;
  MT_Vector3 final_goal = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool waiting = false;
  KX_GameObject *waiting_at = nullptr;
  /** Waiting at a closed Wait link of its navmesh (2026-11-20, a door): its number and for how
   * long (it gives up after the link's Give Up After). */
  uint32_t wait_link = 0;
  float wait_seconds = 0.0f;
  /** Crossing a frame link: its ends in their frames' own spaces (they move with them). On
   * frame: a link inside its vehicle (E2), its ends in the vehicle's space, no frame changed. */
  bool link_frames = false;
  bool link_on_frame = false;
  bool link_frames_landing = false;
  KX_GameObject *link_from_frame = nullptr;
  KX_GameObject *link_to_frame = nullptr;
  MT_Vector3 link_start_local = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 link_end_local = MT_Vector3(0.0f, 0.0f, 0.0f);
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
  /** Avoidance Dense (2026-11-19): pressing and pushing; its weight (2 ^ ((priority - 50) /
   * 12.5)). */
  bool dense = false;
  float weight = 1.0f;
  /** Its object (obstacles: crowds may push a Character back). */
  KX_GameObject *object = nullptr;
  /** An agent standing still (KX_NavAgent::standing): it counts twice in a crowd. */
  bool standing = false;
  /** The way an agent wanted to go last step (the plane, unit; zero: none): who meets whom
   * head-on, also while both stand pressed. */
  MT_Vector3 heading = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** The vehicle an agent stands on (2026-11-19; null: the world): agents avoid only those on
   * the same one (their velocities are over their own decks), unless both are near a link
   * between frames (E4, 2026-11-20: a gangway): then by their velocities over each other's deck
   * (`frame_velocity`: the deck's own where it stands). */
  const KX_GameObject *frame = nullptr;
  bool near_frame_link = false;
  MT_Vector3 frame_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** A part of a vehicle's hull (E6, 2026-11-20): only flyers and swimmers avoid it (the air map
   * holds only what stands still; walkers walk its deck). */
  bool air_only = false;
};

/** A grid of who is where (the plane, x and y). */
/** The first ring a neighbour search looks in (KX_Navigation::FindNeighbours). */
constexpr float kFirstNeighbourRing = 2.0f;

/** The eighth of the circle a direction (the plane) points into: 0..7 counterclockwise from +x
 * (octant folding, no atan2); -1 for none. */
int heading_octant(float x, float y)
{
  if (x * x + y * y < 1e-12f) {
    return -1;
  }
  int octant = 0;
  if (y < 0.0f) {
    x = -x;
    y = -y;
    octant += 4;
  }
  if (x <= 0.0f) {
    const float t = x;
    x = y;
    y = -t;
    octant += 2;
  }
  if (x <= y) {
    const float t = y - x;
    x = x + y;
    y = t;
    octant += 1;
  }
  return octant;
}

struct BodyGrid {
  /* (2 m: a dense crowd's searches look at fewer bodies; the answers do not depend on it.) */
  float cell = 2.0f;
  struct Cell {
    std::vector<uint32_t> list;
    /** Bit per eighth of the circle the moving agents in it head into (keep_right skips a cell
     * where none heads against it). */
    uint8_t headings = 0;
  };
  std::unordered_map<uint64_t, Cell> cells;
  /** The largest body's radius this step. */
  float max_radius = 0.0f;

  static uint64_t Key(const int x, const int y)
  {
    return (uint64_t(uint32_t(x)) << 32) | uint64_t(uint32_t(y));
  }
  void Build(const std::vector<Body> &bodies)
  {
    for (auto &item : cells) {
      item.second.list.clear();
      item.second.headings = 0;
    }
    max_radius = 0.0f;
    for (uint32_t i = 0; i < bodies.size(); i++) {
      const Body &body = bodies[i];
      const int x = int(std::floor(body.position[0] / cell));
      const int y = int(std::floor(body.position[1] / cell));
      Cell &c = cells[Key(x, y)];
      c.list.push_back(i);
      if (body.agent != UINT32_MAX && !body.standing) {
        const int octant = heading_octant(float(body.heading[0]), float(body.heading[1]));
        if (octant >= 0) {
          c.headings |= uint8_t(1u << octant);
        }
      }
      max_radius = std::max(max_radius, body.radius);
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
        for (const uint32_t i : it->second.list) {
          f(i);
        }
      }
    }
  }
};

/** Density-aware routes (2026-11-19): each agent's own crowd, the agents within kCrowdRadius of
 * it (its neighbours, at its height; it and every one standing still counting twice: a group
 * standing in the way blocks it, one walking along only slows it), kCrowdedAgents or more of
 * them; a navmesh polygon costs what its most crowded agent says (a packed blob in a big room's
 * polygon counts as packed: its count spread over the polygon's 15 m2 counted as none; agents
 * on a deck above are another polygon). Searches pay one look-up a polygon (measuring along
 * each searched segment cost 10,000 NPCs 35 ms a step). */
constexpr int kCrowdedAgents = 3;
constexpr float kCrowdRadius = 1.0f;
/** A crowded meter costs at most this much more (a jam: about 9 meters' walk). */
constexpr float kCrowdMaxCost = 8.0f;
/** Polygons of its way ahead looked at for crowds (its own left out), and the extra cost a meter
 * there that makes it search again. */
constexpr int kCrowdLookAhead = 24;
constexpr float kCrowdReplanCost = 0.5f;

/** How much more a meter costs at `density` agents a square meter: walking at the speed people
 * keep in it (Weidmann's fundamental diagram: free below about one a square meter, half speed
 * at two, a standstill at 5.4), so the cost is the time lost. */
float crowd_extra_cost(const float density)
{
  constexpr float kJamDensity = 5.4f, kGamma = 1.913f;
  if (density <= 0.0f) {
    return 0.0f;
  }
  if (density >= kJamDensity) {
    return kCrowdMaxCost;
  }
  const float speed = 1.0f - std::exp(-kGamma * (1.0f / density - 1.0f / kJamDensity));
  if (speed <= 1.0f / (1.0f + kCrowdMaxCost)) {
    return kCrowdMaxCost;
  }
  return 1.0f / speed - 1.0f;
}

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
  /** Obstacles on a vehicle: which and where they were in its space in the step before (E3). */
  std::unordered_map<const KX_GameObject *, std::pair<const KX_GameObject *, MT_Vector3>>
      obstacle_local;
  std::unordered_map<const KX_GameObject *, MT_Vector3> obstacle_local_ahead;
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

/** How far `p` lies from a changed place (the plane; 0 inside it). */
float place_distance(const MT_Vector3 &p, const KX_NavChangedPlace &place)
{
  const float dx = std::max(std::max(place.min[0] - float(p[0]), float(p[0]) - place.max[0]),
                            0.0f);
  const float dy = std::max(std::max(place.min[1] - float(p[1]), float(p[1]) - place.max[1]),
                            0.0f);
  return std::sqrt(dx * dx + dy * dy);
}

bool arrived_at(const MT_Vector3 &position,
                const MT_Vector3 &goal,
                const float arrive,
                const float height)
{
  return KX_NavArrivedAt(position, goal, arrive, height);
}

/** A vehicle's hull for flyers and swimmers (E6, 2026-11-20): spheres along its longer side
 * whose union holds its box (its own space), corners included: each 1.25 x the cross section's
 * half diagonal, spaced so their rims meet at the box's corners. `top`: the highest point of
 * their union over the box's middle (plan item 11: a flyer lands from above it). */
struct HullSpheres {
  int along = 0;
  float radius = 0.1f, spacing = 0.1f;
  int count = 1;
  float top = 0.0f;
};
HullSpheres hull_spheres(const MT_Vector3 &lo, const MT_Vector3 &hi)
{
  HullSpheres h;
  const MT_Vector3 half = (hi - lo) * 0.5f, middle = (hi + lo) * 0.5f;
  h.along = half[0] >= half[1] ? 0 : 1;
  const float cross = std::sqrt(float(half[1 - h.along] * half[1 - h.along] + half[2] * half[2]));
  h.radius = std::max(1.25f * cross, 0.1f);
  h.spacing = std::max(2.0f * std::sqrt(h.radius * h.radius - cross * cross), 0.1f);
  const float length = float(half[h.along]) * 2.0f;
  h.count = std::max(1, int(std::ceil(length / h.spacing)));
  const float reach = h.count > 1 ? h.radius :
                                    std::sqrt(h.radius * h.radius +
                                              float(half[h.along] * half[h.along]));
  h.top = float(middle[2]) + reach;
  return h;
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
  settings.auto_cells = rd.auto_cells != 0;
  settings.build_radius = rd.build_radius > 0.0f ? rd.build_radius : 30.0f;
  settings.remove_radius = rd.remove_radius > 0.0f ?
                               std::max(rd.remove_radius, settings.build_radius) :
                               settings.build_radius * 1.6f;
  settings.detail_distance = std::max(rd.detail_distance, 0.0f);
  settings.level_height = bscene->gm.levelHeight;
  settings.visualize = (bscene->gm.flag & GAME_SHOW_OBSTACLE_SIMULATION) != 0;
  settings.avoid_physics = rd.avoid_physics_objects != 0;
  settings.physics_min_size = rd.physics_min_size > 0.0f ? rd.physics_min_size : 0.5f;
  settings.crowd_pushes = rd.crowd_push_characters != 0;
  settings.jump_links = rd.jump_links == blender::NAVMESH_JUMP_LINKS_AUTO;
  settings.jump_gap = rd.jump_gap > 0.0f ? rd.jump_gap : 3.0f;
  settings.jump_up = rd.jump_up > 0.0f ? rd.jump_up : 1.5f;
  settings.jump_drop = rd.jump_drop > 0.0f ? rd.jump_drop : 4.0f;
  settings.jump_spacing = rd.jump_spacing > 0.0f ? rd.jump_spacing : 2.0f;
  settings.physics_jumps = rd.link_crossing == blender::NAVMESH_LINK_PHYSICS;
  settings.vis_range = std::max(rd.vis_range, 0.0f);
  settings.vis_max_lines = rd.vis_max_lines > 0 ? rd.vis_max_lines : 20000;
  settings.vis_hide = int(rd.vis_hide);
  return settings;
}

KX_Navigation::KX_Navigation(KX_Scene *scene)
    : m_scene(scene), m_step(std::make_unique<KX_NavStep>()), m_build(nullptr, &KX_NavBuildFree)
{
  m_settings = KX_NavigationSettingsOfScene(scene);
  NormalizeBuildMode(m_settings);
  std::unique_ptr<KX_NavFilter> filter = std::make_unique<KX_NavFilter>();
  filter->setIncludeFlags(0xffff);
  filter->setExcludeFlags(0);
  filter->navigation = this;
  m_queryFilter = std::move(filter);
  const char *profile = getenv("UPBGE_NAV_PROFILE");
  m_profile = profile != nullptr && profile[0] != '\0' && profile[0] != '0';
}

KX_Navigation::~KX_Navigation() = default;

const char *KX_NavFailureText(const uint8_t failure)
{
  switch (failure) {
    case KX_NAV_FAILURE_NO_PATH:
      return "No Path";
    case KX_NAV_FAILURE_UNREACHABLE:
      return "Unreachable";
    case KX_NAV_FAILURE_OFF_NAVMESH:
      return "Off Navmesh";
    case KX_NAV_FAILURE_STUCK:
      return "Stuck";
    case KX_NAV_FAILURE_GAVE_UP:
      return "Gave Up";
    default:
      return "";
  }
}

void KX_Navigation::NormalizeBuildMode(Settings &settings)
{
  /* Auto (2026-11-20): Off until something needs the scene's navmesh (`WantSceneNavMesh`), then
   * the whole level. */
  if (settings.build_mode == blender::NAVMESH_BUILD_AUTO) {
    settings.auto_build = true;
    settings.build_mode = blender::NAVMESH_BUILD_OFF;
  }
}

void KX_Navigation::WantSceneNavMesh()
{
  if (m_settings.auto_build && m_settings.build_mode == blender::NAVMESH_BUILD_OFF) {
    m_settings.build_mode = blender::NAVMESH_BUILD_LEVEL;
  }
}

void KX_Navigation::ReportProblem(const char *kind,
                                  const std::string &object,
                                  const std::string &text,
                                  const bool setup)
{
  /* (Any thread: the step's jobs report too.) */
  std::lock_guard<std::mutex> lock(m_problemsMutex);
  const std::string key = std::string(kind) + '\x1f' + object;
  auto found = m_problemIndex.find(key);
  if (found != m_problemIndex.end()) {
    m_problems[found->second].count++;
    return;
  }
  m_problemIndex.emplace(key, m_problems.size());
  KX_NavProblem problem;
  problem.kind = kind;
  problem.object = object;
  problem.text = text;
  problem.setup = setup;
  problem.count = 1;
  m_problems.push_back(std::move(problem));
  if (setup) {
    CM_Warning("navigation: " << text);
  }
}

std::vector<KX_NavProblem> KX_Navigation::GetProblems() const
{
  std::lock_guard<std::mutex> lock(m_problemsMutex);
  return m_problems;
}

namespace {

std::string report_text(const std::string &text)
{
  std::string out = "\"";
  for (const unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    }
    else if (c < 0x20) {
      char code[8];
      snprintf(code, sizeof(code), "\\u%04x", unsigned(c));
      out += code;
    }
    else {
      out += char(c);
    }
  }
  return out + "\"";
}

std::string report_number(const double value)
{
  char text[32];
  snprintf(text, sizeof(text), "%.4g", std::isfinite(value) ? value : 0.0);
  return text;
}

}  // namespace

std::string KX_Navigation::ReportJson() const
{
  const std::vector<KX_NavProblem> problems = GetProblems();
  if (m_agents.empty() && problems.empty() && !m_build) {
    return std::string();
  }
  const char *build = m_settings.auto_build ?
                          "auto" :
                      m_settings.build_mode == blender::NAVMESH_BUILD_LEVEL ? "whole_level" :
                      m_settings.build_mode == blender::NAVMESH_BUILD_AROUND_NPCS ? "around_npcs" :
                                                                                     "off";
  std::string json = "{\"build\": \"" + std::string(build) + "\"";
  json += ", \"built\": " + std::string(m_build ? "true" : "false");
  if (m_build) {
    json += ", \"cell_size\": " + report_number(m_settings.cell_size) +
            ", \"cell_height\": " + report_number(m_settings.cell_height) +
            ", \"auto_cells\": " + (m_settings.auto_cells ? "true" : "false") +
            ", \"cell_reason\": " + report_text(m_autoCellReason);
  }
  /* The NPCs on the scene's navmesh by the size they are built for (whole cells), and by state. */
  struct Size {
    float treated = 0.0f;
    float smallest = 0.0f, largest = 0.0f;
    int npcs = 0;
    bool own = true;
  };
  std::vector<Size> sizes;
  int npcs = 0, walking = 0, waiting = 0, arrived = 0, failed = 0, flying = 0, climbing = 0,
      other_navmesh = 0;
  const float cell = std::max(m_settings.cell_size, 1e-3f);
  for (const std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    const KX_NavAgent &agent = *holder;
    npcs++;
    const KX_NavAgentInfo &info = agent.info;
    if (info.failed || info.failure != KX_NAV_FAILURE_NONE) {
      failed++;
    }
    else if (info.waiting) {
      waiting++;
    }
    else if (info.arrived) {
      arrived++;
    }
    else if (info.moving) {
      walking++;
    }
    if (agent.flies) {
      flying++;
      continue;
    }
    if (agent.crawls) {
      climbing++;
      continue;
    }
    if (agent.request.navmesh != nullptr || !agent.request.use_scene_navmesh) {
      other_navmesh++;
      continue;
    }
    const float r = agent.size.radius > 0.0f ? agent.size.radius : m_settings.agent_radius;
    const float treated = float(std::max(1, int(std::ceil(r / cell - 1e-4f)))) * cell;
    auto found = std::find_if(sizes.begin(), sizes.end(), [&](const Size &size) {
      return std::fabs(size.treated - treated) < cell * 0.25f;
    });
    if (found == sizes.end()) {
      Size size;
      size.treated = treated;
      size.smallest = size.largest = r;
      size.own = !m_build || KX_NavBuildHasOwnClass(*m_build, agent.size);
      sizes.push_back(size);
      found = sizes.end() - 1;
    }
    found->npcs++;
    found->smallest = std::min(found->smallest, r);
    found->largest = std::max(found->largest, r);
    found->own = found->own && (!m_build || KX_NavBuildHasOwnClass(*m_build, agent.size));
  }
  std::sort(sizes.begin(), sizes.end(), [](const Size &a, const Size &b) {
    return a.treated < b.treated;
  });
  json += ", \"sizes\": [";
  for (size_t i = 0; i < sizes.size(); i++) {
    const Size &size = sizes[i];
    json += std::string(i ? ", " : "") + "{\"radius\": " + report_number(size.treated) +
            ", \"smallest\": " + report_number(size.smallest) +
            ", \"largest\": " + report_number(size.largest) +
            ", \"npcs\": " + std::to_string(size.npcs) +
            ", \"own\": " + (size.own ? "true" : "false") + "}";
  }
  json += "]";
  json += ", \"npcs\": " + std::to_string(npcs) + ", \"walking\": " + std::to_string(walking) +
          ", \"waiting\": " + std::to_string(waiting) + ", \"arrived\": " +
          std::to_string(arrived) + ", \"failed\": " + std::to_string(failed) +
          ", \"flying\": " + std::to_string(flying) + ", \"climbing\": " +
          std::to_string(climbing) + ", \"other_navmesh\": " + std::to_string(other_navmesh);
  json += ", \"steps\": " + std::to_string(m_reportSteps) + ", \"step_ms\": " +
          report_number(m_reportSteps ? m_reportSeconds * 1000.0 / double(m_reportSteps) : 0.0);
  json += ", \"problems\": [";
  for (size_t i = 0; i < problems.size(); i++) {
    const KX_NavProblem &problem = problems[i];
    json += std::string(i ? ", " : "") + "{\"kind\": " + report_text(problem.kind) +
            ", \"object\": " + report_text(problem.object) + ", \"text\": " +
            report_text(problem.text) + ", \"count\": " + std::to_string(problem.count) +
            ", \"setup\": " + (problem.setup ? "true" : "false") + "}";
  }
  json += "]}";
  return json;
}

void KX_Navigation::SetSettings(const Settings &settings)
{
  m_settings = settings;
  NormalizeBuildMode(m_settings);
  m_build.reset();
  m_obstacleCandidatesDirty = true;
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
  /* Its size: its mesh's bounds on the plane (scaled), else the scene's agent (then its
   * collision shape's, below). */
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
      agent->air_radius = std::max({agent->radius, hz * 0.5f, 0.05f});
      /* Crawling: its origin stays as far from the surface as from its bounds' bottom (an origin
       * at the feet: on the surface; in the middle: half its height out). */
      agent->crawl_offset = std::max(-bounds->min[2] * sz, 0.02f);
    }
    else {
      agent->air_radius = agent->radius;
      agent->crawl_offset = std::min(agent->radius, agent->height * 0.5f);
    }
  }
  else {
    agent->air_radius = agent->radius;
    agent->crawl_offset = std::min(agent->radius, agent->height * 0.5f);
  }
  /* Plan item 2 (2026-11-20): its size is its collision shape's (the capsule a Character moves
   * with, a physics body's shape), the mesh's above only without a body: a sword or a cape in the
   * mesh made it wide and closed doors to it. Physics > Navigation's NPC Radius overrides the
   * radius (more room than its body: a horse with a small capsule). */
  if (const PHY_IPhysicsController *controller = object->GetPhysicsController()) {
    MT_Vector3 lo, hi;
    if (controller->GetShapeLocalBounds(lo, hi)) {
      const float hx = float(hi[0] - lo[0]) * 0.5f, hy = float(hi[1] - lo[1]) * 0.5f;
      const float hz = float(hi[2] - lo[2]);
      if (hx > 1e-3f && hy > 1e-3f) {
        agent->radius = std::max(hx, hy);
      }
      if (hz > 1e-3f) {
        agent->height = hz;
      }
      agent->air_radius = std::max({agent->radius, agent->height * 0.5f, 0.05f});
    }
  }
  if (object->GetNavSettings().npc_radius > 0.0f) {
    agent->radius = object->GetNavSettings().npc_radius;
    agent->air_radius = std::max(agent->air_radius, agent->radius);
  }
  /* A body crawls at its collision shape's height (a Character's capsule holds its middle half
   * its height off the surface, whatever its mesh), measured standing as it is made. */
  if (const PHY_IPhysicsController *controller = object->GetPhysicsController()) {
    MT_Vector3 lo, hi;
    if (controller->GetWorldBounds(lo, hi) && hi[2] > lo[2]) {
      agent->crawl_offset = std::max(
          float(object->NodeGetWorldPosition()[2] - lo[2]), 0.02f);
    }
  }
  agent->position = object->NodeGetWorldPosition();
  agent->size.radius = agent->radius;
  KX_NavAgent *raw = agent.get();
  m_agentByObject[object] = raw;
  m_agents.push_back(std::move(agent));
  /* An NPC is no floor (2026-11-19): a static body of its own the navmesh was built with (it
   * stood there at the start) is taken out of it, as an NPC that is made later never goes in. */
  if (m_build) {
    PHY_IPhysicsEnvironment *physics = Physics();
    MT_Vector3 lo, hi;
    if (physics != nullptr && object->GetPhysicsController() != nullptr &&
        physics->GetStaticBodyBounds(object->GetPhysicsController(), lo, hi))
    {
      NoteStaticChanged(lo, hi);
    }
  }
  m_movedPieces.erase(object);
  return *raw;
}

void KX_Navigation::Submit(const KX_NavRequest &request)
{
  if (request.object == nullptr) {
    return;
  }
  m_pendingRequests.push_back(request);
}

/** Vehicles (2026-11-19; see "Vehicles" below): a shape's navmeshes, a vehicle's views and moves. */
/** A vehicle standing still this many steps finds its docking links. */
constexpr uint32_t kDockStillSteps = 30;

struct KX_NavVehicleType {
  uint64_t key = 0;
  KX_NavGeometry geometry;
  KX_NavBuildState *build = nullptr;
  /** Links inside it (E2, 2026-11-20): both ends on the vehicle, in its own space, in a fixed
   * order (their numbers the first vehicle's of the shape). */
  std::vector<KX_NavLink> links;
  /** One vehicle's own copy (E3: obstacles stand on it, carved into its tiles alone). */
  bool private_copy = false;
  /** Its shape's bounds in its own space. */
  MT_Vector3 bounds_min = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 bounds_max = MT_Vector3(0.0f, 0.0f, 0.0f);
  ~KX_NavVehicleType()
  {
    if (build != nullptr) {
      KX_NavBuildFree(build);
    }
  }
};

struct KX_NavVehicle {
  KX_GameObject *object = nullptr;
  KX_NavVehicleType *type = nullptr;
  /** Its views of the type's navmeshes (one a size class). */
  std::unordered_map<const KX_NavMesh *, std::unique_ptr<KX_NavMesh>> views;
  /** Its pose expected now (at the last step's end: the pose then moved on by its velocities
   * over the step), the move from it to the pose now (this step's start) and the move expected
   * over the coming step (from now). */
  MT_Transform expected;
  bool has_expected = false;
  MT_Transform moved;
  MT_Transform ahead;
  /** How fast it moved over the last step (m/s, at its origin) and the steps it stood still
   * since (docking links). */
  float speed = 0.0f;
  uint32_t still_steps = 0;
  /** Docking links found while it stands still (none while it moves). */
  std::vector<KX_NavFrameLink> docks;
  bool docked = false;
  /** Its shape is read again at the next step (2026-11-20: an area of it changed, Rebuild Area
   * round it, its links inside changed). */
  bool reshape = false;
  /** Links inside it (E2, 2026-11-20): both ends on it, in its own space, in a fixed order, and
   * their key (its shape's type keys on them too); its type's link numbers -> its own links'
   * (a copy crossing the first's link tells its own). */
  std::vector<KX_NavLink> internal;
  uint64_t links_key = 0;
  std::vector<std::pair<uint32_t, uint32_t>> id_map;
  /** Obstacles standing on it this step (E3), in its own space (a private copy of its type's
   * tiles carves them). */
  std::vector<KX_NavCarveObstacle> obstacles;
  /** Its velocity at its origin over the last step (a jump onto it aims where it will be). */
  MT_Vector3 linear_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
};

void KX_Navigation::ForgetObject(const KX_GameObject *object)
{
  if (object == nullptr) {
    return;
  }
  std::erase_if(m_pendingRequests, [object](const KX_NavRequest &request) {
    return request.object == object;
  });
  /* A vehicle gone (2026-11-19): NPCs on it stand on the world again (its views go with it;
   * its shape's navmeshes stay for copies). */
  if (FindVehicleRecord(object) != nullptr) {
    for (std::unique_ptr<KX_NavAgent> &agent : m_agents) {
      if (agent->frame == object) {
        agent->frame = nullptr;
        agent->navmesh = nullptr;
        agent->corridor_ready = false;
        agent->boundary_ready = false;
      }
    }
    std::erase_if(m_vehicles, [object](const std::unique_ptr<KX_NavVehicle> &vehicle) {
      return vehicle->object == object;
    });
  }
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
  std::erase(m_physicsObstacleCandidates, object);
  m_step->obstacle_positions.erase(object);
  m_step->obstacle_local.erase(object);
  m_step->obstacle_local_ahead.erase(object);
  std::erase_if(m_step->carve,
                [object](const KX_NavCarveObstacle &o) { return o.object == object; });
  /* The bodies refer to agents by index: rebuilt at the next step. */
  m_step->bodies.clear();
  /* Its links: read again at the next step (the object list changed): the tiles holding them
   * are made again without them. */
  for (KX_NavLink &link : m_links) {
    if (link.object == object) {
      link.object = nullptr;
    }
    if (link.end_object == object) {
      link.end_object = nullptr;
    }
  }
  m_linkObjects.erase(object);
  m_testLinkSeen.erase(object);
  /* (Its number goes with it: a new object at the same address is another link.) */
  m_linkIds.erase(object);
  for (std::unique_ptr<KX_NavAgent> &agent : m_agents) {
    if (agent->link_object == object) {
      agent->link_object = nullptr;
    }
    if (agent->waiting_at == object) {
      agent->waiting_at = nullptr;
    }
    /* (What nodes read until the next step: never a removed object.) */
    if (agent->info.link_object == object) {
      agent->info.link_object = nullptr;
    }
    if (agent->info.waiting_at == object) {
      agent->info.waiting_at = nullptr;
    }
    if (agent->info.vehicle == object) {
      agent->info.vehicle = nullptr;
    }
    if (agent->riding == object) {
      agent->riding = nullptr;
    }
    if (agent->through_hull == object) {
      agent->through_hull = nullptr;
    }
    if (agent->landing_on == object) {
      agent->landing_on = nullptr;
    }
  }
  /* A level piece that moved and went before it came to rest: the navmesh still has it where
   * it was (its removal notes where it is now). */
  auto moved = m_movedPieces.find(object);
  if (moved != m_movedPieces.end()) {
    if (moved->second.had_bounds) {
      NoteStaticChanged(moved->second.before_min, moved->second.before_max);
    }
    m_movedPieces.erase(moved);
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
  KX_NavAgentSize size;
  size.radius = agent_radius;
  return NavMeshFor(navmesh_object, size);
}

KX_NavMesh *KX_Navigation::NavMeshFor(KX_NavMeshObject *navmesh_object,
                                      const KX_NavAgentSize &size)
{
  if (navmesh_object != nullptr) {
    return navmesh_object->GetNavMesh();
  }
  if (m_testNavMesh) {
    return m_testNavMesh.get();
  }
  if (m_settings.build_mode == 0) {
    return nullptr;
  }
  EnsureBuild();
  return m_build ? KX_NavBuildNavMesh(*m_build, size) : nullptr;
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
  if (!m_build && m_settings.auto_build) {
    /* Auto (2026-11-20): asked by a query (any thread), built at the next step. */
    m_sceneNavMeshAsked.store(true, std::memory_order_relaxed);
  }
  return m_build ? KX_NavBuildFindNavMesh(*m_build, 0.0f) : nullptr;
}

void KX_Navigation::SetSceneNavMeshForTests(std::unique_ptr<KX_NavMesh> navmesh)
{
  m_testNavMesh = std::move(navmesh);
}

void KX_Navigation::SetLinksForTests(std::vector<KX_NavLink> links)
{
  /* The tiles holding the old ones and the new ones are made again (a test changing them while
   * playing), unless only their open state changed. */
  for (size_t i = 0; i < links.size(); i++) {
    if (links[i].user_id == 0) {
      links[i].user_id = uint32_t(i + 1);
    }
    SnapLinkEnds(links[i]);
  }
  if (m_build) {
    auto same_place = [&](const KX_NavLink &link) {
      for (const KX_NavLink &old : m_links) {
        if (old.user_id == link.user_id) {
          return (old.start - link.start).length2() < 1e-6f &&
                 (old.end - link.end).length2() < 1e-6f && old.radius == link.radius &&
                 old.both_ways == link.both_ways;
        }
      }
      return false;
    };
    for (const KX_NavLink &link : m_links) {
      bool kept = false;
      for (const KX_NavLink &other : links) {
        kept = kept || (other.user_id == link.user_id && same_place(other));
      }
      if (!kept) {
        KX_NavBuildRelinkAt(*m_build, link.start);
        KX_NavBuildRelinkAt(*m_build, link.end);
      }
    }
    for (const KX_NavLink &link : links) {
      if (!same_place(link)) {
        KX_NavBuildRelinkAt(*m_build, link.start);
        KX_NavBuildRelinkAt(*m_build, link.end);
      }
    }
  }
  m_linksForTests = true;
  AdoptLinks(std::move(links));
  /* (Their objects' settings as the test set them: changed later, they change the links.) */
  m_testLinkSeen.clear();
  for (const KX_NavLink &link : m_links) {
    if (link.object != nullptr) {
      m_testLinkSeen[link.object] = {link.object->GetNavSettings().link_enabled,
                                     link.object->GetNavLinkEnd()};
    }
  }
  m_linkSettingsSerial = KX_NavSettingsSerial();
}

void KX_Navigation::AdoptLinks(std::vector<KX_NavLink> fresh)
{
  /* Closed links (2026-11-20): searches leave Go Round ones out (no tile is made again) and
   * charge Wait ones; where one opened or closed, the navmeshes are told (an NPC whose way may
   * pass there checks it: a closed link's way searched again, an opened one's shortcut taken). */
  std::unordered_map<uint32_t, KX_NavClosedLink> closed;
  std::vector<KX_NavChangedPlace> places;
  auto place_of = [](const KX_NavLink &link) {
    KX_NavChangedPlace place;
    for (int i = 0; i < 2; i++) {
      place.min[i] = float(std::min(link.start[i], link.end[i])) - link.radius;
      place.max[i] = float(std::max(link.start[i], link.end[i])) + link.radius;
    }
    return place;
  };
  for (const KX_NavLink &link : fresh) {
    auto before = m_closedLinks.find(link.user_id);
    const bool was_closed = before != m_closedLinks.end();
    if (link.enabled) {
      if (was_closed) {
        places.push_back(place_of(link));
      }
      continue;
    }
    KX_NavClosedLink entry;
    entry.wait = link.wait;
    entry.wait_time = link.wait_time;
    entry.generation = was_closed ? before->second.generation : m_nextClosedGeneration++;
    if (!was_closed || before->second.wait != link.wait) {
      places.push_back(place_of(link));
    }
    closed[link.user_id] = entry;
  }
  m_closedLinks = std::move(closed);
  m_links = std::move(fresh);
  m_linksNeedCheck = true;
  /* Their objects and the objects above them (never physics obstacles). */
  m_linkObjects.clear();
  for (const KX_NavLink &link : m_links) {
    for (KX_GameObject *end : {link.object, link.end_object}) {
      for (KX_GameObject *o = end; o != nullptr; o = o->GetParent()) {
        m_linkObjects.insert(o);
      }
    }
  }
  m_obstacleCandidatesDirty = true;
  IndexLinks();
  if (places.empty()) {
    return;
  }
  if (m_testNavMesh) {
    m_testNavMesh->NoteTilesChanged(&places);
  }
  if (m_build) {
    KX_NavBuildNoteWaysChanged(*m_build, places);
  }
  for (const std::unique_ptr<KX_NavVehicleType> &type : m_vehicleTypes) {
    if (type->build != nullptr) {
      KX_NavBuildNoteWaysChanged(*type->build, places);
    }
  }
}

const dtQueryFilter *KX_Navigation::QueryFilter() const
{
  return m_queryFilter.get();
}

int KX_Navigation::FindAreaId(const std::string &name) const
{
  if (name.empty() || name == "ground" || name == "Ground") {
    return 0;
  }
  for (size_t i = 0; i < m_areaNames.size(); i++) {
    if (m_areaNames[i] == name) {
      return int(i) + 1;
    }
  }
  return -1;
}

float KX_Navigation::SpotAskerHeight(const KX_GameObject *asker) const
{
  /* Its collision shape's height (standing: a crouched Character's controller keeps the standing
   * shape), else its mesh's; nothing of its own (an Empty, or an NPC known only by the scene's
   * agent height): 0, the fixed heights. Any thread: reads only. */
  if (asker == nullptr) {
    return 0.0f;
  }
  KX_GameObject *object = const_cast<KX_GameObject *>(asker);
  MT_Vector3 lo, hi;
  if (const PHY_IPhysicsController *controller = object->GetPhysicsController()) {
    if (controller->GetShapeLocalBounds(lo, hi)) {
      return float(hi[2] - lo[2]);
    }
  }
  if (const blender::Object *blender_object = object->GetBlenderObject()) {
    if (blender_object->type == blender::OB_MESH) {
      if (const std::optional<blender::Bounds<blender::float3>> bounds = BKE_object_boundbox_get(
              blender_object))
      {
        return (bounds->max[2] - bounds->min[2]) *
               std::fabs(float(object->NodeGetWorldScaling()[2]));
      }
    }
  }
  return 0.0f;
}

void KX_Navigation::FindSpots(
    const KX_NavMesh &navmesh,
    const KX_NavSpotQuery &query,
    const std::function<bool(const MT_Vector3 &, const MT_Vector3 &)> &clear_line,
    std::vector<KX_NavSpot> &r_spots) const
{
  /* Plan E8 (2026-11-20). Bounded: at most kMaxSpotSamples places tried, two rays each, one
   * walking search for all of them (Detour's round a circle from `from`: each polygon's way
   * cost). */
  constexpr int kMaxSpotSamples = 128;
  r_spots.clear();
  const int samples = std::clamp(query.samples, 1, kMaxSpotSamples);
  const float radius = std::max(query.radius, 0.0f);
  int area = -1;
  if (!query.area.empty()) {
    area = FindAreaId(query.area);
    if (area < 0) {
      return;
    }
  }
  thread_local std::unordered_map<KX_NavPolyRef, KX_NavMesh::WalkCost> costs;
  const float reach = 1.5f * (float(flat(query.center - query.from).length()) + radius) + 5.0f;
  if (!navmesh.WalkCosts(query.from, reach, QueryFilter(), costs)) {
    return;
  }
  struct Kept {
    KX_NavSpot spot;
    int index;
  };
  thread_local std::vector<Kept> kept;
  kept.clear();
  const MT_Vector3 up(0.0f, 0.0f, 1.0f);
  /* The heights its lines test (2026-11-20, plan item 13): the asking NPC's (its middle crouching
   * 0.55 x its height, its eyes standing 0.85 x, a crouching chest 0.45 x: a dog hides behind a
   * low wall a man is seen over), today's 1.0 / 1.5 / 0.8 m (a 1.8 m NPC's) for an asker without
   * a body (a squad's Empty asking for its soldiers). */
  float body_height = kSpotBodyHeight, eye_height = kSpotEyeHeight,
        cover_height = kSpotCoverHeight;
  const float asker_height = SpotAskerHeight(query.asker);
  if (asker_height > 0.1f) {
    body_height = 0.55f * asker_height;
    eye_height = 0.85f * asker_height;
    cover_height = 0.45f * asker_height;
  }
  for (int i = 0; i < samples; i++) {
    /* A sunflower's spiral: the disc covered evenly, the same points every time. */
    const float r = radius * std::sqrt((float(i) + 0.5f) / float(samples));
    const float a = float(i) * 2.39996323f;
    const MT_Vector3 p = query.center + MT_Vector3(r * std::cos(a), r * std::sin(a), 0.0f);
    MT_Vector3 point;
    KX_NavPolyRef ref = 0;
    if (!navmesh.NearestPoint(p, point, &ref, QueryFilter()) ||
        float(flat(point - p).length()) > 1.0f)
    {
      continue;
    }
    bool near_kept = false;
    for (const Kept &k : kept) {
      near_kept = near_kept || float(flat(k.spot.point - point).length2()) < 0.09f;
    }
    if (near_kept) {
      continue;
    }
    auto cost = costs.find(ref);
    if (cost == costs.end()) {
      continue;
    }
    if (area >= 0 && navmesh.PolygonArea(ref) != area) {
      continue;
    }
    if (query.not_taken) {
      bool taken = false;
      for (const std::unique_ptr<KX_NavAgent> &agent : m_agents) {
        if (agent->object == query.asker) {
          continue;
        }
        const KX_NavAgentInfo &info = agent->info;
        const MT_Vector3 standing = agent->object->NodeGetWorldPosition();
        taken = taken || distance_2d(standing, point) < query.spacing ||
                (info.active && distance_2d(info.goal, point) < query.spacing);
        if (taken) {
          break;
        }
      }
      if (taken) {
        continue;
      }
    }
    if (query.has_look) {
      if (query.visibility != 0) {
        /* Hidden From: the middle of a body crouching there (1 m up); Seen By: the eyes of one
         * standing there (1.5 m up), so a wall of about 1.2 to 1.4 m is cover, hides one
         * crouching and is shot over standing (half cover). One height for both left that a
         * band of a few centimetres, below the navmesh's own height error (it lies up to a
         * cell above the ground: heights are from it). */
        const bool seen = clear_line(
            query.look_from, point + up * (query.visibility == 2 ? eye_height : body_height));
        if (seen != (query.visibility == 2)) {
          continue;
        }
      }
      if (query.cover) {
        MT_Vector3 toward = flat(query.look_from - point);
        if (toward.length2() > 1e-8f) {
          toward.normalize();
          const MT_Vector3 start = point + up * cover_height;
          if (clear_line(start, start + toward * std::max(query.cover_distance, 0.1f))) {
            continue;
          }
        }
      }
    }
    Kept k;
    k.index = i;
    k.spot.point = point;
    /* Its walk: straight when nothing stops a straight walk (exact), else an estimate (the way
     * to the polygon's entry, then straight on; the best ones get their true walk below). */
    float fraction = 1.0f;
    MT_Vector3 hit, normal;
    if (!navmesh.Raycast(query.from, point, fraction, hit, normal, QueryFilter())) {
      k.spot.walk = float((point - query.from).length());
    }
    else {
      k.spot.walk = std::max(cost->second.cost + float((point - cost->second.entry).length()),
                             float(flat(point - query.from).length()));
    }
    kept.push_back(k);
  }
  const auto score_of = [&](const KX_NavSpot &spot) {
    return -query.walk_weight * spot.walk -
           (query.has_look ? query.look_weight * float(flat(spot.point - query.look_from).length()) :
                             0.0f);
  };
  const auto by_score = [](const Kept &a, const Kept &b) {
    if (a.spot.score != b.spot.score) {
      return a.spot.score > b.spot.score;
    }
    return a.index < b.index;
  };
  for (Kept &k : kept) {
    k.spot.score = score_of(k.spot);
  }
  std::stable_sort(kept.begin(), kept.end(), by_score);
  /* The best few by their true walk (a path's corners: the estimate through edges' middles
   * overstates big polygons), then ranked again. */
  if (query.walk_weight != 0.0f) {
    constexpr size_t kRefined = 8;
    thread_local std::vector<MT_Vector3> corners;
    for (size_t i = 0; i < std::min(kRefined, kept.size()); i++) {
      bool complete = false;
      corners.clear();
      if (navmesh.FindPath(query.from, kept[i].spot.point, corners, &complete, QueryFilter()) &&
          complete)
      {
        float length = 0.0f;
        MT_Vector3 last = query.from;
        for (const MT_Vector3 &c : corners) {
          length += float((c - last).length());
          last = c;
        }
        kept[i].spot.walk = length;
        kept[i].spot.score = score_of(kept[i].spot);
      }
    }
    std::stable_sort(kept.begin(), kept.end(), by_score);
  }
  r_spots.reserve(kept.size());
  for (const Kept &k : kept) {
    r_spots.push_back(k.spot);
  }
}

void KX_Navigation::IndexLinks()
{
  m_linkIndexById.clear();
  m_jumpNeeds.clear();
  for (size_t i = 0; i < m_links.size(); i++) {
    const KX_NavLink &link = m_links[i];
    m_linkIndexById[link.user_id] = i;
    if (!KX_NavLinkKindIsJump(link.kind)) {
      continue;
    }
    /* A jump's needs, each way (the build gives the way back kNavLinkReverse). Its ends are
     * where the game maker put them: its gap is the jump. */
    KX_NavLinkNeeds needs;
    needs.gap = float(flat(link.end - link.start).length());
    needs.travel = needs.gap;
    needs.rise = float(link.end[2] - link.start[2]);
    m_jumpNeeds[link.user_id] = needs;
    needs.rise = -needs.rise;
    m_jumpNeeds[link.user_id | kNavLinkReverse] = needs;
  }
}

bool KX_Navigation::LinkNeeds(const uint32_t id, KX_NavLinkNeeds &r_needs) const
{
  if (id >= kNavFoundLinkBase && id < kNavLinkReverse) {
    if (m_build == nullptr) {
      return false;
    }
    const KX_NavLinkInfo *info = KX_NavBuildFindLink(*m_build, id);
    if (info == nullptr) {
      return false;
    }
    r_needs.gap = info->gap;
    r_needs.travel = float(flat(info->end - info->start).length());
    r_needs.rise = float(info->end[2] - info->start[2]);
    return true;
  }
  auto it = m_jumpNeeds.find(id);
  if (it == m_jumpNeeds.end()) {
    return false;
  }
  r_needs = it->second;
  return true;
}

std::string KX_Navigation::LinkKind(const uint32_t id) const
{
  if (id >= kNavFoundLinkBase && id < kNavLinkReverse) {
    const KX_NavLinkInfo *info = m_build ? KX_NavBuildFindLink(*m_build, id) : nullptr;
    return info != nullptr && info->drop ? "drop" : "jump";
  }
  auto found = m_linkIndexById.find(id & ~kNavLinkReverse);
  if (found != m_linkIndexById.end() && found->second < m_links.size()) {
    return m_links[found->second].kind;
  }
  return "jump";
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
  /* (On whatever the load put it on: a deck, the quay; its way over frames chosen again.) */
  agent.frame_known = false;
  agent.route.clear();
  agent.route_step = 0;
  agent.waiting = false;
}

void KX_Navigation::SetObstacle(KX_GameObject *object, const bool enabled)
{
  if (object != nullptr) {
    const bool was = IsObstacle(object);
    m_obstacleOverrides[object] = enabled;
    m_obstacleCandidatesDirty = true;
    /* A static object that becomes an obstacle leaves the level the navmesh is built of (it is
     * carved instead), and one that stops being one is part of it again (2026-11-19). */
    PHY_IPhysicsEnvironment *physics = Physics();
    MT_Vector3 lo, hi;
    if (was != enabled && m_build && physics != nullptr &&
        object->GetPhysicsController() != nullptr && FindAgent(object) == nullptr &&
        physics->GetStaticBodyBounds(object->GetPhysicsController(), lo, hi))
    {
      NoteStaticChanged(lo, hi);
    }
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

void KX_Navigation::NoteLevelChangedNearLinks(const MT_Vector3 &aabb_min,
                                              const MT_Vector3 &aabb_max)
{
  /* (A floor under a link's end built, moved or gone: its end finds its floor again.) */
  const float reach = std::max(kLinkSnapBelow, m_settings.agent_climb) + 1.0f;
  for (const KX_NavLink &link : m_links) {
    for (const MT_Vector3 *p : {&link.start, &link.end}) {
      bool inside = true;
      for (int i = 0; i < 3; i++) {
        inside = inside && (*p)[i] >= aabb_min[i] - reach && (*p)[i] <= aabb_max[i] + reach;
      }
      if (inside) {
        m_linkSnapsStale = true;
        return;
      }
    }
  }
}

void KX_Navigation::RebuildArea(const MT_Vector3 &center, const MT_Vector3 &half_size)
{
  NoteLevelChangedNearLinks(center - half_size, center + half_size);
  if (m_build) {
    KX_NavBuildMarkDirty(*m_build, center - half_size, center + half_size);
  }
  /* (The air maps too: the level read again there.) */
  if (!m_airMaps.empty()) {
    m_airChanges.emplace_back(center - half_size, center + half_size);
  }
}

void KX_Navigation::NoteStaticChanged(const MT_Vector3 &aabb_min, const MT_Vector3 &aabb_max)
{
  NoteLevelChangedNearLinks(aabb_min, aabb_max);
  if (m_build) {
    KX_NavBuildMarkDirty(*m_build, aabb_min, aabb_max);
  }
  if (!m_airMaps.empty()) {
    m_airChanges.emplace_back(aabb_min, aabb_max);
  }
}

void KX_Navigation::NoteAreaChanged(KX_GameObject *object)
{
  /* Its surfaces are made again with the new area: its vehicle's shape, or the level's tiles
   * where it is. */
  if (KX_NavVehicle *vehicle = FindVehicleRecord(VehicleOf(object))) {
    vehicle->reshape = true;
  }
  else {
    NoteLevelBoundsOf(object, false);
  }
}

void KX_Navigation::NoteLevelBoundsOf(KX_GameObject *object, const bool children)
{
  PHY_IPhysicsEnvironment *physics = Physics();
  if (object == nullptr || physics == nullptr) {
    return;
  }
  MT_Vector3 lo, hi;
  if (object->GetPhysicsController() != nullptr &&
      physics->GetStaticBodyBounds(object->GetPhysicsController(), lo, hi))
  {
    NoteStaticChanged(lo, hi);
  }
  if (children) {
    for (KX_GameObject *child : object->GetChildren()) {
      NoteLevelBoundsOf(child, true);
    }
  }
}

bool KX_Navigation::IsExcludedFromLevel(const KX_GameObject *object) const
{
  /* (A vehicle and its parts: their own navmesh, 2026-11-19.) */
  return object != nullptr && (IsObstacle(object) || FindAgent(object) != nullptr ||
                               VehicleOf(object) != nullptr);
}

namespace {

/** An obstacle's outline (2026-11-20, plan item 5): its collision shape's (else its mesh's
 * bounds) in the world, a round shape standing upright (a sphere, a capsule, a cylinder within
 * 15 degrees of up, a ball however turned) a circle (`round`, radius half[0]), anything else (a
 * box, a hull, a mesh, a round one lying: a log) the box round its turned corners, turned about
 * up only (its yaw from its own x seen from above, else its y); Keep Away added round it on the
 * plane. No shape and no mesh (an Empty): a circle of Keep Away (at least 5 cm), `height` tall. Before: the mesh's bounds turned
 * by yaw only (a tilted log kept its upright footprint), a physics object's world box, every
 * obstacle avoided as one circle of the panel's Radius. */
struct ObstacleShape {
  MT_Vector3 center = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** On the plane in its yaw's frame (x, y), and half its height (z). */
  MT_Vector3 half = MT_Vector3(0.0f, 0.0f, 0.0f);
  float yaw = 0.0f;
  bool round = false;
  /** Without Keep Away (Avoid Physics Objects' size check). */
  float own_size = 0.0f;
};

constexpr float kUprightCos = 0.9659f; /* cos(15 degrees) */

ObstacleShape obstacle_shape(KX_GameObject *object, const float height)
{
  ObstacleShape shape;
  const MT_Vector3 position = object->NodeGetWorldPosition();
  shape.center = position;
  const float keep = std::max(object->GetNavSettings().keep_away, 0.0f);
  MT_Vector3 lo(0.0f, 0.0f, 0.0f), hi(0.0f, 0.0f, 0.0f);
  bool have = false, round = false;
  if (const PHY_IPhysicsController *controller = object->GetPhysicsController()) {
    if (controller->GetShapeLocalBounds(lo, hi) && (hi - lo).length2() > 1e-8f) {
      have = true;
      bool is_round = false;
      round = controller->GetShapeIsRound(is_round) && is_round;
    }
  }
  if (!have) {
    if (const blender::Object *blender_object = object->GetBlenderObject()) {
      if (const std::optional<blender::Bounds<blender::float3>> bounds = BKE_object_boundbox_get(
              blender_object))
      {
        const MT_Vector3 &scale = object->NodeGetWorldScaling();
        for (int i = 0; i < 3; i++) {
          const float a = bounds->min[i] * float(scale[i]), b = bounds->max[i] * float(scale[i]);
          lo[i] = std::min(a, b);
          hi[i] = std::max(a, b);
        }
        have = (hi - lo).length2() > 1e-8f;
      }
    }
  }
  if (!have) {
    /* (An object without a Blender object is the tests' only: 1 m round, as before.) */
    const float radius = object->GetBlenderObject() == nullptr ? 1.0f + keep :
                                                                 std::max(keep, 0.05f);
    shape.round = true;
    shape.half = MT_Vector3(radius, radius, std::max(height, 0.1f) * 0.5f);
    return shape;
  }
  const MT_Matrix3x3 &turn = object->NodeGetWorldOrientation();
  const MT_Vector3 middle = (lo + hi) * 0.5f, extent = (hi - lo) * 0.5f;
  /* Its own up in the world (round shapes stand along it). */
  const float up_z = float(turn[2][2]);
  /* The turned corners (offsets from its origin). */
  MT_Vector3 corners[8];
  for (int k = 0; k < 8; k++) {
    const MT_Vector3 local(middle[0] + ((k & 1) ? extent[0] : -extent[0]),
                           middle[1] + ((k & 2) ? extent[1] : -extent[1]),
                           middle[2] + ((k & 4) ? extent[2] : -extent[2]));
    corners[k] = turn * local;
  }
  float z_lo = 1e30f, z_hi = -1e30f;
  for (const MT_Vector3 &c : corners) {
    z_lo = std::min(z_lo, float(c[2]));
    z_hi = std::max(z_hi, float(c[2]));
  }
  /* (A ball is round however it is turned: its bounds as wide every way.) */
  const float widest = float(std::max({extent[0], extent[1], extent[2]}));
  const bool ball = round && widest - float(std::min({extent[0], extent[1], extent[2]})) <=
                                 widest * 0.01f;
  if (round && (ball || std::fabs(up_z) >= kUprightCos)) {
    const MT_Vector3 world_middle = turn * middle;
    const float radius = float(std::max(extent[0], extent[1]));
    shape.round = true;
    shape.own_size = radius * 2.0f;
    shape.center = MT_Vector3(position[0] + world_middle[0],
                              position[1] + world_middle[1],
                              position[2] + (z_lo + z_hi) * 0.5f);
    shape.half = MT_Vector3(radius + keep, radius + keep, (z_hi - z_lo) * 0.5f);
    return shape;
  }
  /* Its yaw: its own x seen from above, else (x standing up) its y turned back a quarter. */
  const float xx = float(turn[0][0]), xy = float(turn[1][0]);
  if (xx * xx + xy * xy >= 0.09f) {
    shape.yaw = std::atan2(xy, xx);
  }
  else {
    shape.yaw = std::atan2(float(turn[1][1]), float(turn[0][1])) - float(M_PI) * 0.5f;
  }
  const float c = std::cos(shape.yaw), s = std::sin(shape.yaw);
  float u_lo = 1e30f, u_hi = -1e30f, v_lo = 1e30f, v_hi = -1e30f;
  for (const MT_Vector3 &corner : corners) {
    const float u = c * float(corner[0]) + s * float(corner[1]);
    const float v = -s * float(corner[0]) + c * float(corner[1]);
    u_lo = std::min(u_lo, u);
    u_hi = std::max(u_hi, u);
    v_lo = std::min(v_lo, v);
    v_hi = std::max(v_hi, v);
  }
  const float mu = (u_lo + u_hi) * 0.5f, mv = (v_lo + v_hi) * 0.5f;
  shape.center = MT_Vector3(position[0] + c * mu - s * mv,
                            position[1] + s * mu + c * mv,
                            position[2] + (z_lo + z_hi) * 0.5f);
  shape.half = MT_Vector3(
      (u_hi - u_lo) * 0.5f + keep, (v_hi - v_lo) * 0.5f + keep, (z_hi - z_lo) * 0.5f);
  shape.own_size = std::max(u_hi - u_lo, v_hi - v_lo);
  return shape;
}

/** A physics object the step may avoid (Avoid Physics Objects): Dynamic or Rigid Body with a
 * body in the world, or a Character. Its size is checked apart. */
bool is_physics_body(KX_GameObject *object)
{
  PHY_IPhysicsController *controller = object->GetPhysicsController();
  if (controller == nullptr || controller->IsPhysicsSuspended()) {
    return false;
  }
  if (controller->IsCharacter()) {
    return true;
  }
  return controller->IsDynamic() && !controller->IsDynamicsSuspended();
}

}  // namespace

bool KX_Navigation::IsPhysicsObstacle(KX_GameObject *object) const
{
  /* (A link's objects never are, 2026-11-20: a resting rigid-body ladder was carved out of the
   * navmesh, its box cutting away the ground its own link starts from.) */
  if (!m_settings.avoid_physics || object == nullptr || FindAgent(object) != nullptr ||
      IsObstacle(object) || !is_physics_body(object) || VehicleOf(object) != nullptr ||
      IsLinkObject(object))
  {
    return false;
  }
  return obstacle_shape(object, m_settings.agent_height).own_size >= m_settings.physics_min_size;
}

void KX_Navigation::TrackLevelChanges()
{
  /* 2026-11-19: the physics notes every static body about to move, turn, scale, take a new
   * shape or physics type, or enter or leave the world (once, where it was then). A level piece
   * is made again in the navmesh where it was and where it is once it stood still kStillSteps
   * steps: a drawbridge raised, a wall slid aside. One moving every step rebuilds nothing until
   * it stops (Create Obstacle avoids it meanwhile). An NPC's own static body is no level piece
   * (the step moves it every step). */
  PHY_IPhysicsEnvironment *physics = Physics();
  /* (Flyers' air maps follow the level too, with or without a navmesh built.) */
  if ((m_settings.build_mode == 0 && m_airMaps.empty()) || m_testNavMesh || physics == nullptr) {
    return;
  }
  if (!m_staticTracking) {
    physics->SetStaticChangeTracking(true);
    m_staticTracking = true;
  }
  std::vector<PHY_StaticBodyChange> changes;
  physics->TakeStaticBodyChanges(changes);
  for (const PHY_StaticBodyChange &change : changes) {
    if (change.object == nullptr) {
      /* Gone since (a removal notes where it was last; this is where the navmesh had it). */
      if (change.had_bounds) {
        NoteStaticChanged(change.before_min, change.before_max);
      }
      continue;
    }
    if (IsExcludedFromLevel(change.object)) {
      continue;
    }
    auto [it, inserted] = m_movedPieces.try_emplace(change.object);
    if (inserted) {
      it->second.had_bounds = change.had_bounds;
      it->second.before_min = change.before_min;
      it->second.before_max = change.before_max;
    }
    it->second.changed_step = m_stepCount;
  }
  /* (The tiles marked are a set: the order pieces come to rest in changes nothing.) */
  for (auto it = m_movedPieces.begin(); it != m_movedPieces.end();) {
    if (m_stepCount - it->second.changed_step < uint64_t(kStillSteps)) {
      ++it;
      continue;
    }
    if (it->second.had_bounds) {
      NoteStaticChanged(it->second.before_min, it->second.before_max);
    }
    KX_GameObject *object = const_cast<KX_GameObject *>(it->first);
    MT_Vector3 lo, hi;
    if (!IsExcludedFromLevel(object) && object->GetPhysicsController() != nullptr &&
        physics->GetStaticBodyBounds(object->GetPhysicsController(), lo, hi))
    {
      NoteStaticChanged(lo, hi);
    }
    it = m_movedPieces.erase(it);
  }
}

void KX_Navigation::UpdateLinks()
{
  /* 2026-11-19: links come, go and move while playing. Read again whenever the object list
   * changed (a ladder added, a bridge destroyed: its link object or its end gone); between, a
   * link whose ends moved is put where they came to rest (kStillSteps still). Each link keeps
   * its own number while it lives; only the tiles its start lies in (old and new) are made again,
   * from their layers. An NPC crossing a link that changed or went finishes the crossing it
   * began (it keeps the link's ends and kind). */
  if (m_linksForTests) {
    /* Tests: the links they set, changed as a game changes them (Set Navigation Attribute's
     * Link Enabled and Link End on a link's object). */
    const uint64_t serial = KX_NavSettingsSerial();
    if (serial == m_linkSettingsSerial) {
      return;
    }
    m_linkSettingsSerial = serial;
    std::vector<KX_NavLink> links;
    for (KX_NavLink link : m_links) {
      if (link.object != nullptr) {
        /* What the link object's settings say now, where they changed since the links were
         * set (the test's own fields stand until then). */
        auto seen = m_testLinkSeen.find(link.object);
        const bool enabled = link.object->GetNavSettings().link_enabled;
        KX_GameObject *end = link.object->GetNavLinkEnd();
        if (seen == m_testLinkSeen.end() || seen->second.first != enabled) {
          link.enabled = enabled;
        }
        if (seen == m_testLinkSeen.end() || seen->second.second != end) {
          link.end_object = end;
          if (end == nullptr) {
            continue;
          }
          link.end = end->NodeGetWorldPosition();
        }
      }
      links.push_back(link);
    }
    SetLinksForTests(std::move(links));
    return;
  }
  if (m_scene == nullptr) {
    return;
  }
  /* (Both ends' tiles: Detour keeps a link in its start's tile and joins its landing only to an
   * end tile built knowing of it: a new or moved end in another tile stayed unjoined.) */
  auto relink = [&](const KX_NavLink &link) {
    if (m_build) {
      KX_NavBuildRelinkAt(*m_build, link.start);
      KX_NavBuildRelinkAt(*m_build, link.end);
    }
  };
  const uint64_t generation = m_scene->GetObjectListGeneration();
  const uint64_t serial = KX_NavSettingsSerial();
  if (generation != m_linkListGeneration || serial != m_linkSettingsSerial || m_linkSnapsStale) {
    /* (Also when a setting changed while playing, 2026-11-20: a link opened, closed or turned
     * to another end; and when the level changed near an end: its floor found again.) */
    m_linkListGeneration = generation;
    m_linkSettingsSerial = serial;
    std::vector<KX_NavLink> fresh = KX_NavCollectLinks(m_scene);
    std::unordered_map<uint32_t, const KX_NavLink *> old_by_id;
    for (const KX_NavLink &link : m_links) {
      old_by_id[link.user_id] = &link;
    }
    std::unordered_map<const KX_GameObject *, uint32_t> ids;
    for (KX_NavLink &link : fresh) {
      auto known = m_linkIds.find(link.object);
      if (known != m_linkIds.end()) {
        link.user_id = known->second;
      }
      else {
        link.user_id = m_nextLinkId++;
      }
      ids[link.object] = link.user_id;
      auto old = old_by_id.find(link.user_id);
      /* Its ends onto their floor (plan item 4): as before where its objects stand where they
       * stood (no level query each time objects come and go), else found again. */
      if (old != old_by_id.end() && !m_linkSnapsStale &&
          (old->second->start - old->second->start_shift - link.start).length2() < 1e-12f &&
          (old->second->end - old->second->end_shift - link.end).length2() < 1e-12f &&
          old->second->radius == link.radius)
      {
        link.start = old->second->start;
        link.end = old->second->end;
        link.start_shift = old->second->start_shift;
        link.end_shift = old->second->end_shift;
      }
      else {
        SnapLinkEnds(link);
      }
      const bool same = old != old_by_id.end() &&
                        (old->second->start - link.start).length2() < 1e-6f &&
                        (old->second->end - link.end).length2() < 1e-6f &&
                        old->second->radius == link.radius &&
                        old->second->both_ways == link.both_ways;
      if (!same) {
        if (old != old_by_id.end()) {
          relink(*old->second);
        }
        relink(link);
      }
      if (old != old_by_id.end()) {
        old_by_id.erase(old);
      }
    }
    /* Gone: the tiles that held them. */
    for (const auto &[id, link] : old_by_id) {
      relink(*link);
    }
    m_linkIds = std::move(ids);
    m_linkSnapsStale = false;
    AdoptLinks(std::move(fresh));
    return;
  }
  for (KX_NavLink &link : m_links) {
    if (link.object == nullptr || link.end_object == nullptr) {
      continue;
    }
    const MT_Vector3 start = link.object->NodeGetWorldPosition();
    const MT_Vector3 end = link.end_object->NodeGetWorldPosition();
    if (!link.moving) {
      /* Off its place in the navmesh by more than 5 cm (its objects' places: the ends' moves
       * onto their floor taken off): it began to move. */
      if ((start - (link.start - link.start_shift)).length2() > 0.05f * 0.05f ||
          (end - (link.end - link.end_shift)).length2() > 0.05f * 0.05f)
      {
        link.moving = true;
        link.moved_step = m_stepCount;
        link.seen_start = start;
        link.seen_end = end;
      }
      continue;
    }
    if ((start - link.seen_start).length2() > 1e-6f || (end - link.seen_end).length2() > 1e-6f) {
      /* Still moving: the rest counts from now. */
      link.moved_step = m_stepCount;
      link.seen_start = start;
      link.seen_end = end;
      continue;
    }
    if (m_stepCount - link.moved_step >= uint64_t(kStillSteps)) {
      relink(link);
      link.start = start;
      link.end = end;
      link.start_shift = MT_Vector3(0.0f, 0.0f, 0.0f);
      link.end_shift = MT_Vector3(0.0f, 0.0f, 0.0f);
      SnapLinkEnds(link);
      link.moving = false;
      m_linksNeedCheck = true;
      relink(link);
    }
  }
}

namespace {

/** The point of triangle abc nearest to (px, py) seen from above (Ericson's closest point on a
 * triangle, in the plane). */
void nearest_on_triangle_2d(const float px,
                            const float py,
                            const MT_Vector3 &a,
                            const MT_Vector3 &b,
                            const MT_Vector3 &c,
                            float &r_x,
                            float &r_y)
{
  const float abx = float(b[0] - a[0]), aby = float(b[1] - a[1]);
  const float acx = float(c[0] - a[0]), acy = float(c[1] - a[1]);
  const float apx = px - float(a[0]), apy = py - float(a[1]);
  const float d1 = abx * apx + aby * apy;
  const float d2 = acx * apx + acy * apy;
  if (d1 <= 0.0f && d2 <= 0.0f) {
    r_x = float(a[0]);
    r_y = float(a[1]);
    return;
  }
  const float bpx = px - float(b[0]), bpy = py - float(b[1]);
  const float d3 = abx * bpx + aby * bpy;
  const float d4 = acx * bpx + acy * bpy;
  if (d3 >= 0.0f && d4 <= d3) {
    r_x = float(b[0]);
    r_y = float(b[1]);
    return;
  }
  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
    const float v = d1 / (d1 - d3);
    r_x = float(a[0]) + abx * v;
    r_y = float(a[1]) + aby * v;
    return;
  }
  const float cpx = px - float(c[0]), cpy = py - float(c[1]);
  const float d5 = abx * cpx + aby * cpy;
  const float d6 = acx * cpx + acy * cpy;
  if (d6 >= 0.0f && d5 <= d6) {
    r_x = float(c[0]);
    r_y = float(c[1]);
    return;
  }
  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
    const float w = d2 / (d2 - d6);
    r_x = float(a[0]) + acx * w;
    r_y = float(a[1]) + acy * w;
    return;
  }
  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    r_x = float(b[0]) + float(c[0] - b[0]) * w;
    r_y = float(b[1]) + float(c[1] - b[1]) * w;
    return;
  }
  const float denom = 1.0f / (va + vb + vc);
  const float v = vb * denom;
  const float w = vc * denom;
  r_x = float(a[0]) + abx * v + acx * w;
  r_y = float(a[1]) + aby * v + acy * w;
}

/** Whether `object` rides a vehicle (itself or a parent has Physics > Navigation > Vehicle):
 * its links are frame links, their ends read where their objects are. */
bool on_vehicle_object(KX_GameObject *object)
{
  for (KX_GameObject *o = object; o != nullptr; o = o->GetParent()) {
    if (o->GetNavSettings().vehicle) {
      return true;
    }
  }
  return false;
}

}  // namespace

void KX_Navigation::SnapLinkEnds(KX_NavLink &link)
{
  if ((link.object != nullptr && on_vehicle_object(link.object)) ||
      (link.end_object != nullptr && on_vehicle_object(link.end_object)))
  {
    return;
  }
  const float climb = std::max(m_settings.agent_climb, 0.0f);
  const float walkable_nz = std::cos(std::clamp(m_settings.agent_max_slope, 0.0f, 1.5707f)) -
                            1e-4f;
  const float radius = std::max(link.radius, 0.05f);
  for (const bool is_end : {false, true}) {
    MT_Vector3 &p = is_end ? link.end : link.start;
    const float px = float(p[0]), py = float(p[1]), pz = float(p[2]);
    const float below = std::max(kLinkSnapBelow, climb) + 0.05f;
    const float above = std::max(kLinkSnapAbove, climb) + 0.05f;
    KX_NavGeometry geometry;
    CollectLevel(MT_Vector3(px - radius, py - radius, pz - below),
                 MT_Vector3(px + radius, py + radius, pz + above),
                 geometry);
    if (geometry.tris.empty()) {
      continue;
    }
    bool on_floor = false;
    bool found_below = false, found_above = false;
    float floor_below = 0.0f, floor_above = 0.0f;
    const float *v = geometry.verts.data();
    for (size_t t = 0; t + 2 < geometry.tris.size(); t += 3) {
      /* (The build's triangles are wound for Recast: a, c, b is the game's order.) */
      const int ia = geometry.tris[t], ib = geometry.tris[t + 2], ic = geometry.tris[t + 1];
      const MT_Vector3 a(v[ia * 3], v[ia * 3 + 1], v[ia * 3 + 2]);
      const MT_Vector3 b(v[ib * 3], v[ib * 3 + 1], v[ib * 3 + 2]);
      const MT_Vector3 c(v[ic * 3], v[ic * 3 + 1], v[ic * 3 + 2]);
      MT_Vector3 n = (b - a).cross(c - a);
      const float length = float(n.length());
      if (length < 1e-12f) {
        continue;
      }
      n /= length;
      if (float(n[2]) < walkable_nz) {
        continue;
      }
      float qx, qy;
      nearest_on_triangle_2d(px, py, a, b, c, qx, qy);
      if ((qx - px) * (qx - px) + (qy - py) * (qy - py) > radius * radius) {
        continue;
      }
      const float h = float(a[2]) -
                      (float(n[0]) * (qx - float(a[0])) + float(n[1]) * (qy - float(a[1]))) /
                          float(n[2]);
      if (std::fabs(pz - h) <= climb) {
        on_floor = true;
        break;
      }
      if (h < pz && pz - h <= kLinkSnapBelow && (!found_below || h > floor_below)) {
        found_below = true;
        floor_below = h;
      }
      if (h > pz && h - pz <= kLinkSnapAbove && (!found_above || h < floor_above)) {
        found_above = true;
        floor_above = h;
      }
    }
    if (on_floor || (!found_below && !found_above)) {
      continue;
    }
    const float floor = found_below ? floor_below : floor_above;
    const MT_Vector3 shift(0.0f, 0.0f, floor - pz);
    p += shift;
    (is_end ? link.end_shift : link.start_shift) += shift;
    KX_GameObject *at = is_end ? link.end_object : link.object;
    char text[320];
    snprintf(text,
             sizeof(text),
             "link '%s' -> '%s': its %s ('%s') was %.2f m %s the floor, further than Max Climb "
             "(%.2f m): moved %s onto it",
             link.object ? link.object->GetName().c_str() : "?",
             link.end_object ? link.end_object->GetName().c_str() : "?",
             is_end ? "end" : "start",
             at ? at->GetName().c_str() : "?",
             std::fabs(floor - pz),
             found_below ? "above" : "below",
             climb,
             found_below ? "down" : "up");
    ReportProblem(is_end ? "link_end_moved" : "link_start_moved",
                  link.object ? link.object->GetName() : std::string(),
                  text,
                  false);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vehicles (2026-11-19)
 *
 * A vehicle is an object with Physics > Navigation > Vehicle on: a ship, a train car, a
 * lift's cab. Its shape (its bodies' triangles, its parts' and children's with bodies) is read
 * once in its own space (its turn and place undone, its size kept) and built into navmeshes
 * once (Whole Level, KX_NavBuild): copies of one shape share them (`KX_NavVehicleType`). Each
 * vehicle walks views of them that follow it (KX_NavMesh::ViewOf): an NPC standing on it (its
 * frame) has its corridor, walls and way in the vehicle's space while the vehicle moves and
 * turns. A sliding NPC is carried along by the vehicle's move: ahead in HandOver by the move
 * physics will give the vehicle in the coming step (its velocities), the rest at the next
 * step's start (a move by logic, before the step: none left behind); a Character goes along by
 * Jolt (what it stands on carries it), a body moved as Dynamic by the deck's velocity added to
 * its own. Vehicles are no part of the scene's level (nor obstacles to avoid).
 * \{ */

namespace {

MT_Transform pose_of(const KX_GameObject *object)
{
  return MT_Transform(object->NodeGetWorldPosition(), object->NodeGetWorldOrientation());
}

/** `transform` after `seconds` of its velocities (an angular velocity about the origin). */
MT_Transform pose_after(const MT_Transform &transform,
                        const MT_Vector3 &linear,
                        const MT_Vector3 &angular,
                        const float seconds)
{
  MT_Matrix3x3 basis = transform.getBasis();
  const float angle = float(angular.length()) * seconds;
  if (angle > 1e-7f) {
    const MT_Quaternion turn(angular / float(angular.length()), angle);
    basis = MT_Matrix3x3(turn) * basis;
  }
  return MT_Transform(transform.getOrigin() + linear * seconds, basis);
}

/** A vehicle's shape in its own space: the triangles of its bodies and of its children's with
 * bodies (not `skip`'s: NPCs, other vehicles), each object's area, convex leaves solid
 * pieces. */
void collect_vehicle_geometry(PHY_IPhysicsEnvironment *physics,
                              KX_GameObject *vehicle,
                              const std::function<int(const std::string &)> &area_id,
                              const std::function<bool(const KX_GameObject *)> &skip,
                              KX_NavGeometry &r_local)
{
  if (physics == nullptr) {
    return;
  }
  MT_Transform inverse;
  inverse.invert(pose_of(vehicle));
  int pieces = 0;
  std::function<void(KX_GameObject *)> visit = [&](KX_GameObject *object) {
    if (object != vehicle && skip(object)) {
      return;
    }
    if (PHY_IPhysicsController *controller = object->GetPhysicsController()) {
      const int area = area_id(object->GetNavSettings().area);
      const int base = pieces;
      int most = -1;
      physics->CollectBodyTriangles(
          controller,
          [&](const int piece, const MT_Vector3 &a, const MT_Vector3 &b, const MT_Vector3 &c) {
            const int first = int(r_local.verts.size() / 3);
            for (const MT_Vector3 *v : {&a, &b, &c}) {
              const MT_Vector3 local = inverse(*v);
              r_local.verts.push_back(float(local[0]));
              r_local.verts.push_back(float(local[1]));
              r_local.verts.push_back(float(local[2]));
            }
            /* (Recast's winding: KX_NavCollectPhysicsGeometry's.) */
            r_local.tris.push_back(first);
            r_local.tris.push_back(first + 2);
            r_local.tris.push_back(first + 1);
            r_local.areas.push_back(uint8_t(area));
            r_local.pieces.push_back(piece >= 0 ? base + piece : -1);
            most = std::max(most, piece);
          });
      pieces = base + most + 1;
    }
    for (KX_GameObject *child : object->GetChildren()) {
      visit(child);
    }
  };
  visit(vehicle);
}

/** A shape's key: its triangles to the millimetre (copies of a shape share their navmeshes). */
uint64_t geometry_key(const KX_NavGeometry &geometry)
{
  uint64_t hash = 1469598103934665603ull;
  auto mix = [&](const uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  for (const float v : geometry.verts) {
    mix(uint64_t(int64_t(std::lround(double(v) * 1000.0))));
  }
  for (const int t : geometry.tris) {
    mix(uint64_t(t));
  }
  for (const uint8_t a : geometry.areas) {
    mix(a);
  }
  return hash;
}

}  // namespace

void KX_Navigation::SetVehiclesForTests(std::vector<KX_GameObject *> vehicles)
{
  /* (They are vehicles by their settings, as in a game: Set Navigation Attribute may turn one
   * off while the test plays.) */
  for (KX_GameObject *object : vehicles) {
    KX_NavObjectSettings settings = object->GetNavSettings();
    settings.vehicle = true;
    object->SetNavSettingsForTests(settings, object->GetNavLinkEnd());
  }
  m_vehiclesForTests = true;
  m_vehicleObjectsForTests = std::move(vehicles);
  m_vehicleListGeneration = UINT64_MAX;
}

void KX_Navigation::SetVehicleGeometryForTests(
    std::function<void(KX_GameObject *, KX_NavGeometry &)> geometry)
{
  m_vehicleGeometryForTests = std::move(geometry);
}

size_t KX_Navigation::VehicleTypeCountForTests() const
{
  return m_vehicleTypes.size();
}

KX_NavVehicle *KX_Navigation::FindVehicleRecord(const KX_GameObject *vehicle) const
{
  if (vehicle == nullptr) {
    return nullptr;
  }
  for (const std::unique_ptr<KX_NavVehicle> &holder : m_vehicles) {
    if (holder->object == vehicle) {
      return holder.get();
    }
  }
  return nullptr;
}

KX_GameObject *KX_Navigation::VehicleOf(const KX_GameObject *object) const
{
  if (m_vehicles.empty()) {
    return nullptr;
  }
  for (KX_GameObject *o = const_cast<KX_GameObject *>(object); o != nullptr; o = o->GetParent()) {
    if (KX_NavVehicle *vehicle = FindVehicleRecord(o)) {
      return vehicle->object;
    }
  }
  return nullptr;
}

KX_NavMesh *KX_Navigation::VehicleNavMeshFor(KX_GameObject *vehicle_object,
                                             const KX_NavAgentSize &size)
{
  KX_NavVehicle *vehicle = FindVehicleRecord(vehicle_object);
  if (vehicle == nullptr || vehicle->type == nullptr || vehicle->type->build == nullptr) {
    return nullptr;
  }
  KX_NavMesh *source = KX_NavBuildNavMesh(*vehicle->type->build, size);
  if (source == nullptr) {
    return nullptr;
  }
  std::unique_ptr<KX_NavMesh> &view = vehicle->views[source];
  if (!view) {
    view = KX_NavMesh::ViewOf(*source, vehicle->object);
  }
  return view.get();
}

KX_GameObject *KX_Navigation::AgentFrame(const KX_GameObject *object) const
{
  const KX_NavAgent *agent = FindAgent(object);
  return agent ? agent->frame : nullptr;
}

KX_NavVehicleType *KX_Navigation::NewVehicleType(const uint64_t key,
                                                KX_NavGeometry geometry,
                                                const std::vector<KX_NavLink> &links)
{
  std::unique_ptr<KX_NavVehicleType> type = std::make_unique<KX_NavVehicleType>();
  type->key = key;
  type->geometry = std::move(geometry);
  type->links = links;
  for (size_t i = 0; i + 2 < type->geometry.verts.size(); i += 3) {
    const MT_Vector3 v(type->geometry.verts[i], type->geometry.verts[i + 1],
                       type->geometry.verts[i + 2]);
    for (int a = 0; a < 3; a++) {
      type->bounds_min[a] = i == 0 ? v[a] : std::min(type->bounds_min[a], v[a]);
      type->bounds_max[a] = i == 0 ? v[a] : std::max(type->bounds_max[a], v[a]);
    }
  }
  Settings settings = m_settings;
  settings.build_mode = blender::NAVMESH_BUILD_LEVEL;
  settings.avoid_physics = false;
  KX_NavVehicleType *raw = type.get();
  type->build = KX_NavBuildCreate(
      settings, [raw](const MT_Vector3 &, const MT_Vector3 &, KX_NavGeometry &r_geometry) {
        r_geometry = raw->geometry;
      });
  KX_NavBuildSetLinkAreas(*type->build, uint8_t(m_jumpArea), uint8_t(m_dropArea));
  m_vehicleTypes.push_back(std::move(type));
  return raw;
}

void KX_Navigation::MapVehicleLinks(KX_NavVehicle &vehicle)
{
  /* Its type's links are the first vehicle's of the shape (the same places, in the same order):
   * each one's number -> its own link's. */
  vehicle.id_map.clear();
  if (vehicle.type == nullptr || vehicle.type->links.size() != vehicle.internal.size()) {
    return;
  }
  for (size_t i = 0; i < vehicle.internal.size(); i++) {
    vehicle.id_map.emplace_back(vehicle.type->links[i].user_id, vehicle.internal[i].user_id);
  }
}

void KX_Navigation::RetypeVehicle(KX_NavVehicle &vehicle, KX_NavVehicleType *type)
{
  if (m_profile && type != nullptr) {
    CM_Debug("navigation: vehicle \"" << vehicle.object->GetName() << "\" walks a shape of "
                                       << type->geometry.tris.size() / 3 << " triangles, "
                                       << type->links.size() << " links inside"
                                       << (type->private_copy ? " (its own copy)" : ""));
  }
  vehicle.type = type;
  MapVehicleLinks(vehicle);
  vehicle.views.clear();
  vehicle.docks.clear();
  vehicle.docked = false;
  /* Its NPCs' navmeshes are the new type's (their corridors and walls found again). */
  for (std::unique_ptr<KX_NavAgent> &agent : m_agents) {
    if (agent->frame == vehicle.object) {
      agent->navmesh = nullptr;
      agent->corridor_ready = false;
      agent->boundary_ready = false;
    }
  }
}

void KX_Navigation::UpdateVehicles(const float dt, LN_JobPool *pool)
{
  /* Read again when the object list or a navigation setting changed (2026-11-20: Physics >
   * Navigation > Vehicle, Set Navigation Attribute's Vehicle): vehicles kept by object (their
   * moves go on), new ones typed by their shape. */
  bool reread = false;
  std::vector<KX_GameObject *> objects;
  const uint64_t serial = KX_NavSettingsSerial();
  if (m_vehiclesForTests) {
    if (m_vehicleListGeneration == UINT64_MAX || serial != m_vehicleSettingsSerial) {
      m_vehicleListGeneration = 0;
      m_vehicleSettingsSerial = serial;
      for (KX_GameObject *object : m_vehicleObjectsForTests) {
        if (object->GetNavSettings().vehicle) {
          objects.push_back(object);
        }
      }
      reread = true;
    }
  }
  else if (m_scene != nullptr) {
    const uint64_t generation = m_scene->GetObjectListGeneration();
    if (generation != m_vehicleListGeneration || serial != m_vehicleSettingsSerial) {
      m_vehicleListGeneration = generation;
      m_vehicleSettingsSerial = serial;
      reread = true;
      for (KX_GameObject *object : *m_scene->GetObjectList()) {
        if (object->GetNavSettings().vehicle) {
          objects.push_back(object);
        }
      }
    }
  }
  /* A vehicle's shape: read in its own space, typed by it (copies share a type's navmeshes). */
  auto type_vehicle = [&](KX_NavVehicle &vehicle, const std::vector<KX_GameObject *> &all) {
    KX_GameObject *object = vehicle.object;
    vehicle.type = nullptr;
    KX_NavGeometry local;
    if (m_vehicleGeometryForTests) {
      m_vehicleGeometryForTests(object, local);
    }
    else {
      collect_vehicle_geometry(
          Physics(),
          object,
          [this](const std::string &name) { return AreaId(name); },
          [this, &all](const KX_GameObject *o) {
            return FindAgent(o) != nullptr || std::find(all.begin(), all.end(), o) != all.end();
          },
          local);
    }
    /* (Its links inside are part of the shape: copies with the same links share it.) */
    const uint64_t key = geometry_key(local) ^ (vehicle.links_key * 0x9e3779b97f4a7c15ull);
    for (const std::unique_ptr<KX_NavVehicleType> &type : m_vehicleTypes) {
      if (type->key == key && !type->private_copy) {
        vehicle.type = type.get();
        break;
      }
    }
    if (vehicle.type == nullptr) {
      if (local.tris.empty()) {
        ReportProblem("vehicle_no_shape",
                      vehicle.object->GetName(),
                      "'" + vehicle.object->GetName() +
                          "' is a Vehicle (Physics > Navigation) but has no collision shape to "
                          "walk on: give it, or its children, a physics body with collision "
                          "bounds");
        return;
      }
      vehicle.type = NewVehicleType(key, std::move(local), vehicle.internal);
    }
    MapVehicleLinks(vehicle);
  };
  if (reread) {
    std::vector<std::unique_ptr<KX_NavVehicle>> kept;
    for (KX_GameObject *object : objects) {
      std::unique_ptr<KX_NavVehicle> vehicle;
      for (std::unique_ptr<KX_NavVehicle> &old : m_vehicles) {
        if (old && old->object == object) {
          vehicle = std::move(old);
          break;
        }
      }
      if (!vehicle) {
        vehicle = std::make_unique<KX_NavVehicle>();
        vehicle->object = object;
        type_vehicle(*vehicle, objects);
        /* A level piece no more (its own navmesh): the tiles where it stood (a static body's;
         * none before the first build). */
        NoteLevelBoundsOf(object, true);
      }
      kept.push_back(std::move(vehicle));
    }
    /* Vehicles no more (Set Navigation Attribute's Vehicle off, or gone): a level piece again
     * where it still is. */
    bool changed = kept.size() != m_vehicles.size();
    for (std::unique_ptr<KX_NavVehicle> &old : m_vehicles) {
      if (old && old->object != nullptr) {
        NoteLevelBoundsOf(old->object, true);
        changed = true;
      }
    }
    m_vehicles = std::move(kept);
    /* NPCs on a vehicle gone: on the world again, their ways searched from where they are. When
     * vehicles came or went, every NPC's frame is chosen again under it (one may stand on a
     * deck that is a vehicle again). */
    for (std::unique_ptr<KX_NavAgent> &agent : m_agents) {
      if (agent->frame != nullptr && FindVehicleRecord(agent->frame) == nullptr) {
        agent->frame = nullptr;
        agent->navmesh = nullptr;
        agent->corridor_ready = false;
        agent->boundary_ready = false;
      }
      if (changed && !agent->on_link) {
        agent->frame_known = false;
      }
    }
  }
  /* Shapes read again (an area of a vehicle changed, Rebuild Area round it, its links inside
   * changed): its NPCs' navmeshes are its new type's. */
  bool retyped = reread;
  for (std::unique_ptr<KX_NavVehicle> &holder : m_vehicles) {
    KX_NavVehicle &vehicle = *holder;
    if (!vehicle.reshape) {
      continue;
    }
    vehicle.reshape = false;
    retyped = true;
    std::vector<KX_GameObject *> all;
    for (const std::unique_ptr<KX_NavVehicle> &v : m_vehicles) {
      all.push_back(v->object);
    }
    type_vehicle(vehicle, all);
    RetypeVehicle(vehicle, vehicle.type);
  }
  /* Types no vehicle walks any more (a shape changed, a vehicle gone) are freed with their
   * navmeshes (no view or NPC holds them: retyping dropped those). */
  if (retyped) {
    std::erase_if(m_vehicleTypes, [this](const std::unique_ptr<KX_NavVehicleType> &type) {
      return std::none_of(m_vehicles.begin(),
                          m_vehicles.end(),
                          [&](const std::unique_ptr<KX_NavVehicle> &v) {
                            return v->type == type.get();
                          });
    });
  }
  if (m_vehicles.empty()) {
    return;
  }
  /* Each vehicle's move since the pose expected at the last step's end (logic moved it, or
   * physics other than expected), and the move expected over the coming step (physics: its
   * velocities; logic moves it before the next step: none expected). */
  const float step = std::max(dt, 1e-4f);
  for (std::unique_ptr<KX_NavVehicle> &holder : m_vehicles) {
    KX_NavVehicle &vehicle = *holder;
    const MT_Transform now = pose_of(vehicle.object);
    if (vehicle.has_expected) {
      MT_Transform inverse;
      inverse.invert(vehicle.expected);
      vehicle.moved = now * inverse;
    }
    else {
      vehicle.moved.setIdentity();
    }
    MT_Vector3 linear(0.0f, 0.0f, 0.0f), angular(0.0f, 0.0f, 0.0f);
    PHY_IPhysicsController *controller = vehicle.object->GetPhysicsController();
    if (controller != nullptr && (controller->IsDynamic() || controller->IsKinematic())) {
      linear = vehicle.object->GetLinearVelocity(false);
      angular = vehicle.object->GetAngularVelocity(false);
    }
    MT_Transform inverse_now;
    inverse_now.invert(now);
    vehicle.ahead = pose_after(now, linear, angular, step) * inverse_now;
    vehicle.linear_velocity = vehicle.has_expected ?
                                  (now.getOrigin() - vehicle.expected.getOrigin()) / step + linear :
                                  linear;
    const float moved_by = vehicle.has_expected ?
                               float((now.getOrigin() - vehicle.expected.getOrigin()).length()) :
                               0.0f;
    vehicle.speed = std::max(moved_by / step, float(linear.length()));
    vehicle.still_steps = vehicle.speed < 0.05f && float(angular.length()) < 0.02f ?
                              vehicle.still_steps + 1 :
                              0;
  }
  (void)pool;
}

void KX_Navigation::UpdateVehicleNavMeshes(LN_JobPool *pool)
{
  if (m_vehicles.empty()) {
    return;
  }
  /* A vehicle with obstacles standing on it (E3, 2026-11-20) gets its own copy of its type (the
   * shared tiles stay the copies' own): made once, at its first obstacle. */
  for (std::unique_ptr<KX_NavVehicle> &holder : m_vehicles) {
    KX_NavVehicle &vehicle = *holder;
    if (vehicle.obstacles.empty() || vehicle.type == nullptr || vehicle.type->private_copy) {
      continue;
    }
    KX_NavVehicleType *own = NewVehicleType(0, vehicle.type->geometry, vehicle.type->links);
    own->private_copy = true;
    RetypeVehicle(vehicle, own);
  }
  /* Their navmeshes: the default size and every NPC's (built once, Whole Level), with the links
   * inside them, a private copy carving its vehicle's obstacles. */
  static const std::vector<KX_NavCarveObstacle> no_obstacles;
  for (std::unique_ptr<KX_NavVehicleType> &type : m_vehicleTypes) {
    if (type->build == nullptr) {
      continue;
    }
    const std::vector<KX_NavCarveObstacle> *obstacles = &no_obstacles;
    if (type->private_copy) {
      for (const std::unique_ptr<KX_NavVehicle> &vehicle : m_vehicles) {
        if (vehicle->type == type.get()) {
          obstacles = &vehicle->obstacles;
          break;
        }
      }
    }
    KX_NavBuildNavMesh(*type->build, 0.0f);
    for (const std::unique_ptr<KX_NavAgent> &agent : m_agents) {
      if (agent->requested) {
        KX_NavBuildNavMesh(*type->build, agent->size);
      }
    }
    KX_NavBuildUpdate(*type->build, {}, {}, {}, *obstacles, type->links, pool);
  }
}

void KX_Navigation::EndVehicleStep()
{
  if (m_vehicles.empty()) {
    return;
  }
  /* Slides on a vehicle (asked to move or standing there) go ahead with the move physics will
   * give the vehicle in the coming step, turned as it turns: drawn where the deck is. The rest
   * of its move (logic's, physics other than expected) at the next step's start. */
  for (std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    KX_NavAgent &agent = *holder;
    if (agent.frame == nullptr || agent.drive != KX_NavAgent::Drive::Slide || agent.on_link) {
      continue;
    }
    KX_NavVehicle *vehicle = FindVehicleRecord(agent.frame);
    if (vehicle == nullptr) {
      continue;
    }
    KX_GameObject *object = agent.object;
    const MT_Vector3 p = object->NodeGetWorldPosition();
    const MT_Vector3 q = vehicle->ahead(p);
    if ((q - p).length2() > 0.0f) {
      object->ApplyMovement(q - p, false);
      object->NodeSetGlobalOrientation(vehicle->ahead.getBasis() *
                                       object->NodeGetWorldOrientation());
    }
    agent.last_written = object->NodeGetWorldPosition();
    agent.has_written = true;
  }
  for (std::unique_ptr<KX_NavVehicle> &holder : m_vehicles) {
    KX_NavVehicle &vehicle = *holder;
    vehicle.expected = vehicle.ahead * pose_of(vehicle.object);
    vehicle.has_expected = true;
  }
}

KX_GameObject *KX_Navigation::FrameAt(const MT_Vector3 &p,
                                      const KX_NavAgentSize &size,
                                      bool &r_found)
{
  /* The highest navmesh under the point (half a metre above it to 2.5 m below, beside it by
   * no more than a body): a vehicle's deck above the sea floor, a cab's floor above the
   * shaft's. */
  r_found = false;
  KX_GameObject *best = nullptr;
  float best_height = -FLT_MAX;
  const float beside = std::max(size.radius, 0.5f) + 0.25f;
  auto consider = [&](KX_NavMesh *navmesh, KX_GameObject *frame) {
    MT_Vector3 point;
    if (navmesh == nullptr || !navmesh->NearestPoint(p, point)) {
      return;
    }
    const float dz = float(p[2] - point[2]);
    const MT_Vector3 off(point[0] - p[0], point[1] - p[1], 0.0f);
    if (float(off.length()) < beside && dz > -0.5f && dz < 2.5f &&
        float(point[2]) > best_height)
    {
      best_height = float(point[2]);
      best = frame;
      r_found = true;
    }
  };
  consider(NavMeshFor(nullptr, size), nullptr);
  for (std::unique_ptr<KX_NavVehicle> &vehicle : m_vehicles) {
    consider(VehicleNavMeshFor(vehicle->object, size), vehicle->object);
  }
  return best;
}

KX_GameObject *KX_Navigation::FrameUnder(KX_NavAgent &agent,
                                         PHY_ICharacter *character,
                                         bool &r_found)
{
  r_found = true;
  if (m_vehicles.empty()) {
    return nullptr;
  }
  /* A Character: what it stands on. */
  if (character != nullptr && character->OnGround()) {
    return VehicleOf(character->GetGroundObject());
  }
  return FrameAt(agent.position, agent.size, r_found);
}

void KX_Navigation::UpdateFrameLinks(const float dt)
{
  /* Links whose ends stand on different frames go round Detour (it joins one navmesh only):
   * their ends where their objects are now, open while at most their reach apart and not
   * moving apart faster than 0.5 m/s (a ship casting off). A link inside one vehicle has no
   * place yet (its navmesh is built without links). */
  m_frameLinks.clear();
  m_worldLinks.clear();
  m_hasLongLinks = false;
  std::unordered_map<const KX_GameObject *, std::vector<KX_NavLink>> inside;
  /* (The tile grid decides which links Detour holds: made before the first build's tiles.) */
  if (!m_links.empty() && m_settings.build_mode != 0 && !m_testNavMesh) {
    EnsureBuild();
  }
  for (const KX_NavLink &link : m_links) {
    KX_GameObject *start_frame = m_vehicles.empty() ? nullptr : VehicleOf(link.object);
    KX_GameObject *end_frame = m_vehicles.empty() ? nullptr : VehicleOf(link.end_object);
    if (start_frame == nullptr && end_frame == nullptr) {
      /* A link longer than Detour joins (2026-11-20: its end beyond its start's neighbouring
       * tiles, a teleporter across the level) goes round Detour as a link between frames
       * whose frames are both the world: open while Link Enabled, never moving apart. */
      if (m_build == nullptr || KX_NavBuildLinkFits(*m_build, link.start, link.end)) {
        m_worldLinks.push_back(link);
        continue;
      }
      KX_NavFrameLink frame_link;
      frame_link.link = link;
      frame_link.start = link.start;
      frame_link.end = link.end;
      frame_link.open = link.enabled;
      m_frameLinks.push_back(std::move(frame_link));
      m_hasLongLinks = true;
      continue;
    }
    if (link.object == nullptr || link.end_object == nullptr) {
      continue;
    }
    if (start_frame == end_frame) {
      /* Inside one vehicle (E2, 2026-11-20: a ladder between two decks): part of its shape's
       * navmesh, in its own space. */
      MT_Transform inverse;
      inverse.invert(pose_of(start_frame));
      KX_NavLink local = link;
      local.start = inverse(link.object->NodeGetWorldPosition());
      local.end = inverse(link.end_object->NodeGetWorldPosition());
      inside[start_frame].push_back(local);
      continue;
    }
    KX_NavFrameLink frame_link;
    frame_link.link = link;
    frame_link.start_frame = start_frame;
    frame_link.end_frame = end_frame;
    frame_link.start = link.object->NodeGetWorldPosition();
    frame_link.end = link.end_object->NodeGetWorldPosition();
    const float gap = float((frame_link.end - frame_link.start).length());
    bool parting = false;
    auto seen = m_frameLinkSeen.find(link.user_id);
    if (seen != m_frameLinkSeen.end()) {
      const float before = float((seen->second.second - seen->second.first).length());
      parting = (gap - before) / std::max(dt, 1e-4f) > 0.5f;
    }
    m_frameLinkSeen[link.user_id] = {frame_link.start, frame_link.end};
    /* (Link Enabled off closes it too, 2026-11-20: Go Round, or Wait at it.) */
    frame_link.open = link.enabled && gap <= link.reach && !parting;
    m_frameLinks.push_back(std::move(frame_link));
  }
  /* Each vehicle's links inside: in a fixed order (by place), their key; a change reads its
   * shape again at the next step (its type keys on them: copies with the same links share). */
  for (std::unique_ptr<KX_NavVehicle> &holder : m_vehicles) {
    KX_NavVehicle &vehicle = *holder;
    std::vector<KX_NavLink> links;
    auto found = inside.find(vehicle.object);
    if (found != inside.end()) {
      links = std::move(found->second);
    }
    auto q = [](const float v) { return int64_t(std::llround(v * 20.0f)); };
    std::sort(links.begin(), links.end(), [&](const KX_NavLink &a, const KX_NavLink &b) {
      for (int i = 0; i < 3; i++) {
        if (q(float(a.start[i])) != q(float(b.start[i]))) {
          return q(float(a.start[i])) < q(float(b.start[i]));
        }
      }
      for (int i = 0; i < 3; i++) {
        if (q(float(a.end[i])) != q(float(b.end[i]))) {
          return q(float(a.end[i])) < q(float(b.end[i]));
        }
      }
      return a.user_id < b.user_id;
    });
    uint64_t key = links.empty() ? 0 : 0xcbf29ce484222325ull;
    auto mix = [&key](const uint64_t v) { key = (key ^ v) * 0x100000001b3ull; };
    for (const KX_NavLink &link : links) {
      for (int i = 0; i < 3; i++) {
        mix(uint64_t(q(float(link.start[i]))));
        mix(uint64_t(q(float(link.end[i]))));
      }
      mix(uint64_t(q(link.radius)));
      mix(link.both_ways ? 1 : 2);
      mix(std::hash<std::string>()(link.kind));
    }
    if (key != vehicle.links_key) {
      vehicle.links_key = key;
      vehicle.internal = std::move(links);
      vehicle.reshape = true;
    }
    else {
      /* (The same places: its own numbers as now, its type's mapping to them.) */
      vehicle.internal = std::move(links);
      MapVehicleLinks(vehicle);
    }
  }
  /* Docking links: found once a vehicle stood still half a second, gone when it moves. */
  for (std::unique_ptr<KX_NavVehicle> &holder : m_vehicles) {
    KX_NavVehicle &vehicle = *holder;
    if (vehicle.still_steps == 0) {
      vehicle.docks.clear();
      vehicle.docked = false;
      continue;
    }
    if (!vehicle.docked && vehicle.still_steps >= kDockStillSteps) {
      FindDocks(vehicle);
      vehicle.docked = true;
    }
    const MT_Transform pose = pose_of(vehicle.object);
    for (const KX_NavFrameLink &dock : vehicle.docks) {
      KX_NavFrameLink frame_link = dock;
      frame_link.start = pose(dock.start_local);
      frame_link.open = true;
      m_frameLinks.push_back(std::move(frame_link));
    }
  }
}

void KX_Navigation::FindDocks(KX_NavVehicle &vehicle)
{
  /* Its deck's open edges every 2 m (the default size's navmesh), each looking outward up to
   * 2.5 m for the world's navmesh at the deck's height within the climb (both navmeshes keep a
   * body's width from their edges: a 1 m gap between deck and quay is about 2.2 m between
   * them); one link a 1.5 m stretch of edge, both ways, of kind "step". */
  vehicle.docks.clear();
  const KX_NavAgentSize size;
  KX_NavMesh *deck = VehicleNavMeshFor(vehicle.object, size);
  KX_NavMesh *world = NavMeshFor(nullptr, size);
  if (deck == nullptr || world == nullptr) {
    return;
  }
  const MT_Transform pose = pose_of(vehicle.object);
  MT_Transform inverse;
  inverse.invert(pose);
  const float climb = m_settings.agent_climb;
  std::vector<MT_Vector3> kept;
  deck->ForEachWallEdge([&](const MT_Vector3 &local_a, const MT_Vector3 &local_b) {
    const MT_Vector3 a = pose(local_a), b = pose(local_b);
    const MT_Vector3 along = flat(b - a);
    const float length = float(along.length());
    if (length < 0.2f) {
      return;
    }
    const int samples = std::max(1, int(length / 2.0f));
    for (int i = 0; i < samples; i++) {
      const MT_Vector3 p = a + (b - a) * ((float(i) + 0.5f) / float(samples));
      bool near_kept = false;
      for (const MT_Vector3 &k : kept) {
        near_kept = near_kept || float(flat(k - p).length()) < 1.5f;
      }
      if (near_kept) {
        continue;
      }
      /* Outward: the side the deck is not on. */
      MT_Vector3 out(along[1] / length, -along[0] / length, 0.0f);
      MT_Vector3 probe;
      if (deck->NearestPoint(p + out * 0.3f, probe) &&
          float(flat(probe - (p + out * 0.3f)).length()) < 0.05f)
      {
        out = -out;
      }
      for (float d = 0.2f; d <= 2.5f + 1e-4f; d += 0.2f) {
        const MT_Vector3 q = p + out * d;
        MT_Vector3 landing;
        if (!world->NearestPoint(q, landing) || float(flat(landing - q).length()) > 0.1f ||
            std::fabs(float(landing[2] - p[2])) > climb + 0.1f)
        {
          continue;
        }
        KX_NavFrameLink dock;
        dock.automatic = true;
        dock.start_frame = vehicle.object;
        dock.end_frame = nullptr;
        dock.start_local = inverse(p - out * 0.05f);
        dock.end = landing + out * 0.05f;
        dock.link.kind = "step";
        dock.link.both_ways = true;
        dock.link.user_id = m_nextDockId++;
        vehicle.docks.push_back(dock);
        kept.push_back(p);
        break;
      }
    }
  });
}

float KX_Navigation::ChooseFrameRoute(KX_NavAgent &agent, KX_GameObject *goal_frame)
{
  /* The way over at most two frame links (a ship from the dock, a lift: floor -> cab ->
   * floor) that is shortest in straight lines (a closed link costs 15 m more: an open one is
   * taken when not much longer). When the goal is on the agent's own frame (it does not reach
   * it there), the second link must land where the goal is reached. */
  agent.route.clear();
  agent.route_step = m_stepCount;
  agent.route_goal_frame = goal_frame;
  struct Way {
    const KX_NavFrameLink *link;
    bool reversed;
    MT_Vector3 from, to;
    KX_GameObject *from_frame, *to_frame;
  };
  std::vector<Way> ways;
  for (const KX_NavFrameLink &l : m_frameLinks) {
    /* Closed to it (2026-11-20, Link Enabled off): a Go Round link, or a Wait link it gave up
     * waiting at; a jump onto a vehicle it missed (E5) for a while. */
    if (!l.link.enabled && agent.filter.navigation != nullptr &&
        agent.filter.ClosedToIt(l.link.user_id))
    {
      continue;
    }
    if (std::any_of(agent.failed_links.begin(),
                    agent.failed_links.end(),
                    [&](const std::pair<uint32_t, uint64_t> &f) {
                      return f.first == l.link.user_id && f.second > m_stepCount;
                    }))
    {
      continue;
    }
    ways.push_back({&l, false, l.start, l.end, l.start_frame, l.end_frame});
    if (l.link.both_ways) {
      ways.push_back({&l, true, l.end, l.start, l.end_frame, l.start_frame});
    }
  }
  constexpr float kClosed = 15.0f;
  auto cost = [&](const Way &w) { return w.link->open ? 0.0f : kClosed; };
  const MT_Vector3 &p = agent.position;
  const MT_Vector3 &goal = agent.final_goal;
  float best = FLT_MAX;
  /* Whether the goal is reached from where a way lands, on the goal frame's navmesh (a lift's
   * door to the wrong floor lands near the goal in a straight line only). Each landing asked
   * once. */
  KX_NavMesh *goal_navmesh = goal_frame != nullptr ? VehicleNavMeshFor(goal_frame, agent.size) :
                                                     NavMeshFor(nullptr, agent.size);
  std::unordered_map<const Way *, bool> lands;
  auto lands_well = [&](const Way &w) {
    if (goal_navmesh == nullptr) {
      return true;
    }
    auto it = lands.find(&w);
    if (it != lands.end()) {
      return it->second;
    }
    std::vector<MT_Vector3> corners;
    bool complete = false;
    const bool ok = goal_navmesh->FindPath(w.to, goal, corners, &complete, &agent.filter) &&
                    complete;
    lands[&w] = ok;
    return ok;
  };
  /* Whether the agent walks to a way's start on its own frame (a door on another floor is near
   * in a straight line only). Each start asked once. */
  std::unordered_map<const Way *, bool> starts;
  auto starts_well = [&](const Way &w) {
    auto it = starts.find(&w);
    if (it != starts.end()) {
      return it->second;
    }
    std::vector<MT_Vector3> corners;
    bool complete = false;
    const bool ok = agent.navmesh != nullptr &&
                    agent.navmesh->FindPath(p, w.from, corners, &complete, &agent.filter) &&
                    complete;
    starts[&w] = ok;
    return ok;
  };
  /* One link (2026-11-20: also from the agent's own frame to it, a long link of the world: a
   * teleporter). */
  for (const Way &w : ways) {
    if (w.from_frame == agent.frame && w.to_frame == goal_frame) {
      const float c = float((w.from - p).length()) + float((goal - w.to).length()) + cost(w);
      if (c < best && starts_well(w) && lands_well(w)) {
        best = c;
        agent.route = {{w.link->link.user_id, w.reversed}};
      }
    }
  }
  for (const Way &a : ways) {
    if (a.from_frame != agent.frame || a.to_frame == agent.frame) {
      continue;
    }
    for (const Way &b : ways) {
      if (b.link == a.link || b.from_frame != a.to_frame || b.to_frame != goal_frame) {
        continue;
      }
      const float c = float((a.from - p).length()) + float((b.from - a.to).length()) +
                      float((goal - b.to).length()) + cost(a) + cost(b);
      if (c < best && starts_well(a) && lands_well(b)) {
        best = c;
        agent.route = {{a.link->link.user_id, a.reversed}, {b.link->link.user_id, b.reversed}};
      }
    }
  }
  return best;
}

void KX_Navigation::RouteAcrossFrames(KX_NavAgent &agent)
{
  agent.waiting = false;
  agent.waiting_at = nullptr;
  agent.routing = false;
  if (m_frameLinks.empty() || !agent.has_goal || agent.navmesh == nullptr || agent.flies ||
      agent.crawls || agent.on_link)
  {
    agent.route.clear();
    return;
  }
  agent.final_goal = agent.goal;
  /* The goal's frame: a Target Object's vehicle, else the frame under the goal. */
  bool found = false;
  KX_GameObject *goal_frame = agent.request.target != nullptr ?
                                  VehicleOf(agent.request.target) :
                                  FrameAt(agent.goal, agent.size, found);
  /* Its own frame reaches the goal: no route (searched at least once: a way that ends short). */
  const bool reaches_here = goal_frame == agent.frame &&
                            !(agent.corridor_ready && !agent.path_complete &&
                              agent.steps_since_search >= 1);
  if (reaches_here && agent.route.empty()) {
    /* Unless a long link of its frame (2026-11-20, a teleporter) is clearly shorter than the
     * walk left: looked at once a second (straight lines, as the routes are chosen). */
    if (!m_hasLongLinks || !agent.corridor_ready || agent.steps_since_search < 1 ||
        (agent.route_step != 0 && m_stepCount - agent.route_step < 60))
    {
      return;
    }
    const float walk = std::max(agent.path_estimate - agent.traveled,
                                float(flat(agent.goal - agent.position).length()));
    const float via = ChooseFrameRoute(agent, goal_frame);
    if (agent.route.empty() || via + kLongLinkShortcut >= walk) {
      agent.route.clear();
      return;
    }
  }
  /* Chosen when first needed, when the goal's frame changes and every second (links open and
   * close; none found: tried again a second later, never every step). */
  if (agent.route_step == 0 || agent.route_goal_frame != goal_frame ||
      m_stepCount - agent.route_step >= 60)
  {
    ChooseFrameRoute(agent, goal_frame);
  }
  if (agent.route.empty()) {
    return;
  }
  const auto [id, reversed] = agent.route.front();
  const KX_NavFrameLink *link = nullptr;
  for (const KX_NavFrameLink &l : m_frameLinks) {
    if (l.link.user_id == id) {
      link = &l;
      break;
    }
  }
  if (link == nullptr || (reversed ? link->end_frame : link->start_frame) != agent.frame) {
    /* Gone, or it no longer starts where the agent is: chosen again next step. */
    agent.route.clear();
    agent.route_step = 0;
    return;
  }
  const MT_Vector3 from = reversed ? link->end : link->start;
  const MT_Vector3 to = reversed ? link->start : link->end;
  agent.routing = true;
  agent.goal = from;
  agent.arrived = false;
  /* (Never a name `near`: <windows.h> defines it as nothing.) */
  const float at_start = std::max(agent.radius, 0.3f) + 0.25f;
  if (!arrived_at(agent.position, from, at_start, agent.height)) {
    return;
  }
  if (!link->open) {
    /* Closed: it waits at the start (it stands; Get Navigation Info's Waiting). Closed by Link
     * Enabled with a Give Up After (2026-11-20): after that long it takes another way. */
    agent.waiting = true;
    agent.waiting_at = reversed ? link->link.end_object : link->link.object;
    if (agent.wait_link != id) {
      agent.wait_link = id;
      agent.wait_seconds = 0.0f;
    }
    agent.wait_seconds += m_step->last_dt;
    const KX_NavClosedLink *closed = link->link.enabled ? nullptr : FindClosedLink(id);
    if (closed != nullptr && closed->wait_time > 0.0f && agent.wait_seconds >= closed->wait_time)
    {
      agent.filter.gave_up.emplace_back(id, closed->generation);
      agent.gave_up = true;
      agent.route.clear();
      agent.route_step = 0;
      agent.waiting = false;
      agent.waiting_at = nullptr;
      agent.wait_link = 0;
      agent.wait_seconds = 0.0f;
    }
    return;
  }
  /* Over it, carried: its ends follow their frames while it crosses. */
  KX_GameObject *from_frame = reversed ? link->end_frame : link->start_frame;
  KX_GameObject *to_frame = reversed ? link->start_frame : link->end_frame;
  auto local_of = [](KX_GameObject *frame, const MT_Vector3 &world) {
    if (frame == nullptr) {
      return world;
    }
    MT_Transform inverse;
    inverse.invert(pose_of(frame));
    return inverse(world);
  };
  MT_Vector3 landing = to;
  if (KX_NavMesh *navmesh = to_frame != nullptr ? VehicleNavMeshFor(to_frame, agent.size) :
                                                  NavMeshFor(nullptr, agent.size))
  {
    MT_Vector3 snapped;
    if (navmesh->NearestPoint(to, snapped)) {
      landing = snapped;
    }
  }
  MT_Vector3 take_off = agent.position;
  take_off[2] -= agent.height_offset;
  agent.on_link = true;
  agent.link_started = true;
  agent.link_id = id;
  agent.link_object = reversed ? link->link.end_object : link->link.object;
  agent.link_kind = link->link.kind;
  agent.link_start = take_off;
  agent.link_end = landing;
  agent.link_time = 0.0f;
  agent.link_jump = KX_NavLinkKindIsJump(agent.link_kind);
  agent.link_clearance = agent.link_jump ? KX_NavJumpClearance(take_off, landing) : 0.0f;
  agent.link_physics = false;
  agent.link_launch = false;
  agent.link_landed = false;
  const float distance = float((landing - take_off).length());
  agent.link_duration = link->link.duration > 0.0f ?
                            link->link.duration :
                            (link->link.kind == "teleport" ?
                                 0.0f :
                                 distance / std::max(agent.request.speed, 0.1f));
  /* A jump or a drop onto or off a vehicle (E5, 2026-11-20), Jumps: Physics: a Character or a
   * body moved as Dynamic jumps it itself, the throw aimed where the landing will be when it
   * lands (its frame's velocity times the flight), steered toward the moving landing in the
   * air (StepLink). A missed one is closed to it a while (failed_links) and its way chosen
   * again. */
  if (m_settings.physics_jumps && agent.link_jump && agent.can_launch &&
      !(link->link.duration > 0.0f))
  {
    MT_Vector3 moving(0.0f, 0.0f, 0.0f);
    if (const KX_NavVehicle *vehicle = to_frame ? FindVehicleRecord(to_frame) : nullptr) {
      moving = flat(vehicle->linear_velocity);
    }
    const MT_Vector3 from(agent.position[0], agent.position[1], take_off[2]);
    const float g = agent.gravity;
    MT_Vector3 aim = landing;
    float up_speed = 0.0f, plan_speed = 0.0f, air_time = 0.0f;
    bool ok = false;
    for (int pass = 0; pass < 3; pass++) {
      ok = KX_NavJumpLaunch(
          from, aim, KX_NavJumpClearance(from, aim), g, up_speed, plan_speed, air_time);
      if (!ok) {
        break;
      }
      aim = landing + moving * air_time;
    }
    if (ok) {
      agent.link_physics = true;
      agent.link_launch = true;
      agent.link_up_speed = up_speed;
      agent.link_plan_speed = plan_speed;
      agent.link_duration = air_time;
      agent.link_from = agent.position;
      agent.link_left_ground = false;
      agent.link_fell = false;
    }
  }
  agent.link_frames = true;
  agent.link_from_frame = from_frame;
  agent.link_to_frame = to_frame;
  agent.link_start_local = local_of(from_frame, take_off);
  agent.link_end_local = local_of(to_frame, landing);
  agent.route.erase(agent.route.begin());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The step
 * \{ */

void KX_Navigation::Step(const float dt, LN_JobPool *pool)
{
  m_stepCount++;
  /* Auto (2026-11-20): a Navmesh Query found no scene navmesh in the step before: built now. */
  if (m_sceneNavMeshAsked.exchange(false, std::memory_order_relaxed)) {
    WantSceneNavMesh();
  }
  if (m_agents.empty() && m_pendingRequests.empty() && m_settings.build_mode == 0 &&
      !m_settings.visualize && m_airMaps.empty())
  {
    /* A scene nothing navigates in (no NPC ever asked, no navmesh to build, no air map asked
     * for by Navmesh Query): nothing to do. */
    std::lock_guard<std::mutex> lock(m_airSizesMutex);
    if (m_airSizesAsked.empty()) {
      HandOverNavigatingBodies();
      return;
    }
  }
  const float step = std::max(dt, 1e-4f);
  /* The play report's cost a step (item 9): two clock reads a step that does anything. */
  const auto step_started = std::chrono::steady_clock::now();
  /* UPBGE_NAV_PROFILE: each phase timed (the clock read only then). */
  auto mark = m_profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  auto lap = [&](const int phase) {
    if (m_profile) {
      const auto now = std::chrono::steady_clock::now();
      m_profileMs[phase] += std::chrono::duration<double, std::milli>(now - mark).count();
      mark = now;
    }
  };
  /* The scene's gravity (a jump's time in the air; read here, on the main thread). */
  if (PHY_IPhysicsEnvironment *physics = Physics()) {
    MT_Vector3 gravity(0.0f, 0.0f, 0.0f);
    physics->GetGravity(gravity);
    m_gravity = std::max(float(gravity.length()), 0.01f);
  }
  ApplyRequests();
  /* (Vehicles first: they are no part of the scene's level, nor obstacles; links touching them
   * are links between frames.) */
  UpdateVehicles(step, pool);
  UpdateLinks();
  UpdateFrameLinks(step);
  TrackLevelChanges();
  UpdateAirMaps(pool);
  lap(0);
  /* Obstacles first: the still ones are carved into this step's navmesh (those on a vehicle
   * into its own). */
  GatherObstacles();
  UpdateAirSolids();
  UpdateVehicleNavMeshes(pool);
  lap(1);
  UpdateSceneNavMesh(pool);
  lap(2);
  if (!m_agents.empty()) {
    PrepareAgents(step, pool);
    SenseSurfaces(pool);
    lap(3);
    FindNeighbours(step, pool);
    lap(4);
    SyncCorridors(pool);
    CountCrowding(pool);
    PlanPaths(pool);
    PlanAir(pool);
    lap(5);
    SteerAgents(step, pool);
    lap(6);
    AvoidAgents(step, pool);
    SolveDense(step, pool);
    lap(7);
    HandOver(step, pool);
    SeparateSliding(pool);
    lap(8);
    TurnAgents(step, pool);
    lap(9);
    WriteInfo();
    lap(10);
  }
  EndVehicleStep();
  HandOverNavigatingBodies();
  if (m_profile && ++m_profileSteps == 300) {
    const double *t = m_profileMs;
    const double n = double(m_profileSteps);
    /* The scene navmesh's tiles over these steps (2026-11-19): built (voxelized), dropped,
     * restored (Around NPCs), made again for links. */
    KX_NavBuildStats stats;
    if (m_build) {
      stats = KX_NavBuildGetStats(*m_build);
    }
    CM_Debug("navigation step (ms, average of 300 steps, " << m_agents.size() << " agents): requests "
             << t[0] / n << ", obstacles " << t[1] / n << ", navmesh build " << t[2] / n
             << ", agents " << t[3] / n << ", neighbours " << t[4] / n << ", paths "
             << t[5] / n << ", steering " << t[6] / n << ", avoidance " << t[7] / n
             << ", moves " << t[8] / n << ", turns " << t[9] / n << ", info " << t[10] / n
             << "; tiles built " << stats.tiles_built - m_profileTiles[0] << ", dropped "
             << stats.tiles_removed - m_profileTiles[1] << ", restored "
             << stats.tiles_restored - m_profileTiles[2] << ", relinked "
             << stats.tiles_relinked - m_profileTiles[3] << ", alive " << stats.tiles_alive);
    m_profileTiles[0] = stats.tiles_built;
    m_profileTiles[1] = stats.tiles_removed;
    m_profileTiles[2] = stats.tiles_restored;
    m_profileTiles[3] = stats.tiles_relinked;
    std::fill(std::begin(m_profileMs), std::end(m_profileMs), 0.0);
    m_profileSteps = 0;
  }
  m_reportSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                   step_started)
                         .count();
  m_reportSteps++;

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
        /* A vehicle whose origin is in the box (E3, 2026-11-20: a ramp folded down, cargo
         * loaded): its shape read again and built as a new one. */
        for (std::unique_ptr<KX_NavVehicle> &vehicle : m_vehicles) {
          const MT_Vector3 d = vehicle->object->NodeGetWorldPosition() - request.destination;
          if (std::fabs(float(d[0])) <= float(request.offset[0]) &&
              std::fabs(float(d[1])) <= float(request.offset[1]) &&
              std::fabs(float(d[2])) <= float(request.offset[2]))
          {
            vehicle->reshape = true;
          }
        }
        continue;
      case KX_NavRequest::Command::SetJumpLimits: {
        KX_NavAgent &agent = GetOrCreateAgent(request.object);
        agent.jump_set = request.enabled;
        agent.jump_own = KX_NavJumpLimits();
        agent.jump_own.limited = request.enabled;
        agent.jump_own.gap = std::max(float(request.offset[0]), 0.0f);
        agent.jump_own.up = std::max(float(request.offset[1]), 0.0f);
        agent.jump_own.drop = std::max(float(request.offset[2]), 0.0f);
        continue;
      }
      case KX_NavRequest::Command::SetVehicle:
        /* (Read again at this step's UpdateVehicles: NPCs on a vehicle that stops being one
         * stand on the world again, its body is a level piece again.) */
        request.object->SetNavVehicle(request.enabled);
        continue;
      case KX_NavRequest::Command::SetLinkEnabled:
        request.object->SetNavLinkEnabled(request.enabled);
        continue;
      case KX_NavRequest::Command::SetLinkEnd:
        request.object->SetNavLinkEnd(request.target);
        continue;
      case KX_NavRequest::Command::SetArea: {
        const bool changed = request.object->GetNavSettings().area != request.area;
        request.object->SetNavArea(request.area);
        if (changed) {
          NoteAreaChanged(request.object);
        }
        continue;
      }
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
    if (!agent.asked_before || agent.request.issuer != request.issuer ||
        agent.request.tag != request.tag)
    {
      /* A new order (it was not asked in the step before, or another node, or another of its
       * points, asks now): its arrival pulses When Reached, also where it already stands; a
       * failure pulses When Failed again. */
      agent.reach_armed = true;
      agent.failure_reported = KX_NAV_FAILURE_NONE;
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
    /* Auto (2026-11-20): a walker on the scene's navmesh has it built in this very step (the
     * build comes after the requests). */
    if (agent.request.navmesh == nullptr && agent.request.use_scene_navmesh &&
        agent.request.move == KX_NavRequest::Move::Walk &&
        agent.request.climb == KX_NavRequest::Climb::Floors)
    {
      WantSceneNavMesh();
    }
  }
  m_pendingRequests.clear();
}

void KX_Navigation::PrepareAgents(const float dt, LN_JobPool *pool)
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
  AssignChaserPlaces(dt);
  for (size_t index = 0; index < m_agents.size(); index++) {
    KX_NavAgent &agent = *m_agents[index];
    KX_GameObject *object = agent.object;
    MT_Vector3 position = object->NodeGetWorldPosition();
    /* On a vehicle (2026-11-19): a slide nothing else moved goes along with the vehicle's move
     * since the last step's end (what was not expected then: logic moved it), turned as it
     * turned; its last position is seen where the deck took it (its velocity is the one over
     * the deck); the deck's velocity where it stands over the coming step. */
    agent.frame_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
    if (KX_NavVehicle *vehicle = agent.frame ? FindVehicleRecord(agent.frame) : nullptr) {
      if (agent.has_written && agent.drive == KX_NavAgent::Drive::Slide &&
          (position - agent.last_written).length2() <= 1e-6f)
      {
        const MT_Vector3 carried = vehicle->moved(position);
        object->ApplyMovement(carried - position, false);
        object->NodeSetGlobalOrientation(vehicle->moved.getBasis() *
                                         object->NodeGetWorldOrientation());
        position = object->NodeGetWorldPosition();
        agent.last_written = position;
      }
      if (agent.has_previous) {
        agent.position = vehicle->moved(agent.position);
      }
      agent.frame_velocity = (vehicle->ahead(position) - position) / std::max(dt, 1e-4f);
    }
    /* Moved by someone else (a teleport, Set Position): the navmesh height again, the corridor
     * from here. */
    if (agent.has_written && agent.drive == KX_NavAgent::Drive::Slide &&
        (position - agent.last_written).length2() > 1e-6f)
    {
      agent.offset_known = false;
      agent.corridor_ready = false;
      agent.boundary_ready = false;
      /* (On whatever it was put on.) */
      agent.frame_known = false;
    }
    agent.has_written = false;
    agent.previous_position = agent.has_previous ? agent.position : position;
    agent.position = position;
    agent.has_previous = true;

    agent.last_drive = agent.drive;
    agent.drive = KX_NavAgent::Drive::Slide;
    PHY_ICharacter *character = nullptr;
    agent.can_launch = false;
    agent.grounded = false;
    agent.gravity = m_gravity;
    if (PHY_IPhysicsController *controller = object->GetPhysicsController()) {
      if (controller->IsCharacter() && physics != nullptr &&
          (character = physics->GetCharacterController(object)) != nullptr)
      {
        agent.drive = KX_NavAgent::Drive::Walk;
        /* Physics jumps (2026-11-19): what the step's crossing reads on the helpers. */
        agent.can_launch = character->CanLaunch();
        agent.grounded = character->OnGround();
        agent.gravity = std::max(float(character->GetGravity().length()), 0.01f);
      }
      else if (agent.request.move_dynamic && controller->IsDynamic()) {
        agent.drive = KX_NavAgent::Drive::Velocity;
        agent.can_launch = true;
        agent.gravity = std::max(m_gravity * controller->GetGravityFactor(), 0.01f);
      }
    }
    /* Links it missed: closed until their time (then forgotten). */
    if (!agent.failed_links.empty()) {
      const uint64_t now = m_stepCount;
      agent.failed_links.erase(
          std::remove_if(agent.failed_links.begin(),
                         agent.failed_links.end(),
                         [now](const std::pair<uint32_t, uint64_t> &f) { return f.second <= now; }),
          agent.failed_links.end());
    }
    agent.filter.failed = &agent.failed_links;
    agent.filter.now = m_stepCount;
    /* What it can walk (2026-11-19): a Character the highest ledge it climbs (its Step Height,
     * or the edge its round bottom rides up: PHY_ICharacter::GetClimbHeight, its own shape's)
     * and the slopes up to its Max Slope (90 degrees and Jolt's "no limit" 0: the scene's). The
     * navmesh keeps the scene's limits where the Character's are not lower (KX_NavBuild takes
     * the lower of the two). */
    agent.size = KX_NavAgentSize();
    agent.size.radius = agent.radius;
    if (character != nullptr) {
      const float slope = character->GetMaxSlope();
      const bool no_limit = !(slope > 0.0f) || slope >= 1.5621f; /* 89.5 degrees */
      agent.size.climb = std::max(character->GetClimbHeight(), 1e-3f);
      agent.size.max_slope = no_limit ? 0.0f : slope;
    }

    /* Its jump limits (2026-11-19): set by Set Navigation Attribute, else a Character's own
     * (its jump speed and gravity, a run's speed on the plan: 1.5 x its walk, 3 m/s at least;
     * the scene's deepest drop), else none. A change searches its way again. */
    {
      KX_NavJumpLimits limits;
      const float climb = agent.size.climb > 0.0f ? agent.size.climb : m_settings.agent_climb;
      if (agent.jump_set) {
        limits = agent.jump_own;
        limits.climb = climb;
      }
      else if (character != nullptr) {
        limits.limited = true;
        limits.physics = true;
        limits.jump_speed = std::max(character->GetJumpSpeed(), 0.0f);
        limits.gravity = std::max(float(character->GetGravity().length()), 0.01f);
        limits.plan_speed = std::max(agent.request.speed * 1.5f, 3.0f);
        limits.drop = m_settings.jump_drop;
        limits.climb = climb;
      }
      if (limits != agent.filter.jump) {
        agent.filter.jump = limits;
        agent.needs_search = true;
      }
      agent.filter.navigation = this;
      agent.filter.jump_area = m_jumpArea;
      agent.filter.drop_area = m_dropArea;
    }
    /* Flying or swimming (phase 8): the air map of its size, no navmesh. A Character cannot (it
     * falls by its own gravity): it stands, warned once. */
    agent.flies = agent.requested && agent.request.move != KX_NavRequest::Move::Walk;
    KX_NavAirMap *air = nullptr;
    if (agent.flies) {
      if (agent.drive == KX_NavAgent::Drive::Walk) {
        if (!agent.air_warned) {
          ReportProblem("character_cannot_fly",
                        object->GetName(),
                        "'" + object->GetName() +
                            "' is a Character, it cannot fly or swim (Navigate's Movement): "
                            "give it Dynamic physics, or none");
          agent.air_warned = true;
        }
      }
      else {
        air = AirMapFor(agent.air_radius, pool);
      }
    }
    /* Walls and ceilings (phase 9): walking with Climb Walls or Ceilings, it crawls on the shell
     * of the air map of its width (no navmesh): floors and walls, ceilings too. Its costs and
     * closed surfaces are its area costs "wall" and "ceiling" (Set Navigation Attribute). A
     * Character climbs by its gravity turned toward the surface; given back when it stops. */
    agent.crawls = agent.requested && !agent.flies &&
                   agent.request.climb != KX_NavRequest::Climb::Floors;
    if (agent.crawls) {
      air = SurfaceMapFor(agent.radius, pool);
      const int wall = AreaId("wall"), ceiling = AreaId("ceiling");
      agent.crawl_kinds = KX_NavAirMap::SurfaceFloor;
      if (!((agent.filter.closed_areas >> wall) & 1u)) {
        agent.crawl_kinds |= KX_NavAirMap::SurfaceWall;
      }
      if (agent.request.climb == KX_NavRequest::Climb::Ceilings &&
          !((agent.filter.closed_areas >> ceiling) & 1u))
      {
        agent.crawl_kinds |= KX_NavAirMap::SurfaceCeiling;
      }
      agent.crawl_costs.floor = std::max(agent.filter.getAreaCost(0), 1e-3f);
      agent.crawl_costs.wall = std::max(agent.filter.getAreaCost(wall), 1e-3f);
      agent.crawl_costs.ceiling = std::max(agent.filter.getAreaCost(ceiling), 1e-3f);
    }
    if (agent.gravity_saved && (!agent.crawls || agent.drive != KX_NavAgent::Drive::Walk)) {
      if (character != nullptr) {
        character->SetGravity(agent.saved_gravity);
      }
      agent.gravity_saved = false;
    }
    if (air != agent.air) {
      agent.air = air;
      agent.air_path.clear();
      agent.air_next = 0;
      agent.air_needs_search = true;
    }
    agent.water = agent.flies && agent.request.move == KX_NavRequest::Move::Swim;
    /* A Dynamic swimmer floats (a boat, a duck: the water holds it, it steers across only),
     * unless its Gravity Factor is 0 (a fish the water neither lifts nor sinks: it dives, moved
     * in three dimensions as a flyer). */
    agent.floating = false;
    if (agent.water && agent.drive == KX_NavAgent::Drive::Velocity) {
      const PHY_IPhysicsController *controller = object->GetPhysicsController();
      agent.floating = controller == nullptr || controller->GetGravityFactor() != 0.0f;
    }

    /* How it moved (flying, not floating: in space; else along the ground). */
    const bool in_space = agent.air != nullptr && !agent.floating;
    const auto motion = [in_space](const MT_Vector3 &v) { return in_space ? v : flat(v); };
    /* Its velocity: how it moved in the last step (a jump of a teleport counts as standing). */
    MT_Vector3 moved = position - agent.previous_position;
    const float limit = std::max(agent.request.speed * 3.0f, 1.0f) * dt;
    agent.velocity = moved.length() > limit ? MT_Vector3(0.0f, 0.0f, 0.0f) : moved / dt;
    if (moved.length() <= limit) {
      agent.traveled += float(motion(moved).length());
    }
    /* Its walking speed for animations (2026-11-19): the plane (a flyer's: in space), smoothed
     * over about 0.2 s, 0 while it stands (below 0.1 m/s; walking again above 0.25 m/s: no
     * flicker in a jam). */
    const float actual = float(motion(agent.velocity).length());
    agent.smooth_speed += (actual - agent.smooth_speed) * std::min(dt / 0.2f, 1.0f);
    if (agent.standing ? agent.smooth_speed > 0.25f : agent.smooth_speed < 0.1f) {
      agent.standing = !agent.standing;
    }
    /* Stalled: it wanted to move (the last step's wish, avoidance may have said stand) and got
     * nowhere: the seconds since it was last farther than a third of a second's walk (0.1 m at
     * least) from where it stood. Its speed of one step said walking for an NPC shaking 3 mm
     * back and forth at a wall corner for good (ORCA's flutter), so nothing helped it on. (A
     * flyer climbing straight up has not stalled.) */
    const float wanted = float(motion(agent.desired).length());
    if (agent.requested && wanted > 0.05f) {
      const float enough = std::max(0.3f * agent.request.speed, 0.1f);
      if (!agent.stall_anchored ||
          float(motion(position - agent.stall_anchor).length()) > enough)
      {
        agent.stall_anchor = position;
        agent.stall_anchored = true;
        agent.stall_since = 0.0f;
      }
      else {
        agent.stall_since += dt;
      }
      /* (Less the time walking there takes: one walking freely has not stalled.) */
      agent.stall_time = std::max(
          agent.stall_since - enough / std::max(agent.request.speed, 0.01f), 0.0f);
    }
    else {
      agent.stall_anchored = false;
      agent.stall_since = 0.0f;
      agent.stall_time = 0.0f;
    }
    /* Pressing: hardly moving in this step (Dense agents squeeze by it, and near its goal it
     * counts as arrived by it: by stall_time a crowd creeping along squeezed too, and one pressed
     * at a corridor's mouth near its goal stood there as arrived, blocking the rest). */
    const float speed_now = float(motion(agent.velocity).length());
    if (agent.requested && wanted > 0.05f &&
        speed_now < std::max(0.1f * agent.request.speed, 0.05f))
    {
      agent.press_time += dt;
    }
    else if (!agent.requested || speed_now >= std::max(0.3f * agent.request.speed, 0.1f)) {
      agent.press_time = 0.0f;
    }

    /* The navmesh it walks: a navmesh object's, else the scene's or (2026-11-19) the vehicle's
     * it stands on (its frame: chosen when its corridor starts, changed by a crossing). */
    KX_NavMesh *navmesh = agent.navmesh;
    if (agent.requested) {
      const bool walks = !agent.flies && !agent.crawls;
      if (walks && agent.request.navmesh == nullptr && agent.request.use_scene_navmesh) {
        /* (A Character goes where it stands: carried off a deck by its physics, or walking
         * over a gangway that is a floor too.) */
        bool grounded_elsewhere = false;
        if (character != nullptr && agent.frame_known && !agent.on_link &&
            character->OnGround())
        {
          grounded_elsewhere = VehicleOf(character->GetGroundObject()) != agent.frame;
        }
        if (!agent.frame_known || grounded_elsewhere) {
          bool found = false;
          KX_GameObject *frame = FrameUnder(agent, character, found);
          if (found) {
            if (frame != agent.frame) {
              agent.route.clear();
            }
            agent.frame = frame;
            agent.frame_known = true;
          }
        }
        navmesh = agent.frame != nullptr ? VehicleNavMeshFor(agent.frame, agent.size) :
                                           NavMeshFor(nullptr, agent.size);
      }
      else {
        /* (A flyer riding a vehicle, plan item 11: its frame.) */
        agent.frame = agent.flies && !agent.crawls ? agent.riding : nullptr;
        navmesh = walks && agent.request.navmesh != nullptr ?
                      NavMeshFor(agent.request.navmesh, agent.size) :
                      nullptr;
      }
    }
    if (navmesh != agent.navmesh) {
      agent.navmesh = navmesh;
      agent.navmesh_object = agent.request.navmesh;
      agent.corridor_ready = false;
      agent.boundary_ready = false;
    }
    /* On a vehicle (E2): its type's links are the vehicle's own ones by this mapping. */
    {
      const KX_NavVehicle *vehicle = agent.frame != nullptr ? FindVehicleRecord(agent.frame) :
                                                              nullptr;
      agent.filter.id_map = vehicle != nullptr && !vehicle->id_map.empty() ? &vehicle->id_map :
                                                                             nullptr;
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
      /* Vehicles (2026-11-19): a goal on another frame, or not reached on its own: the way
       * over frame links (it walks to the next link's start, waits there while closed). */
      RouteAcrossFrames(agent);
    }
    else {
      agent.routing = false;
      agent.waiting = false;
      agent.route.clear();
      agent.route_step = 0;
    }
  }
}

float KX_Navigation::FootprintRadiusOf(KX_GameObject *object) const
{
  if (object == nullptr) {
    return 0.0f;
  }
  if (const KX_NavAgent *agent = FindAgent(object)) {
    return agent->radius;
  }
  MT_Vector3 lo, hi;
  if (const PHY_IPhysicsController *controller = object->GetPhysicsController()) {
    if (controller->GetShapeLocalBounds(lo, hi)) {
      return 0.5f * std::max(float(hi[0] - lo[0]), float(hi[1] - lo[1]));
    }
  }
  if (const blender::Object *blender_object = object->GetBlenderObject()) {
    if (blender_object->type == blender::OB_MESH) {
      if (const std::optional<blender::Bounds<blender::float3>> bounds = BKE_object_boundbox_get(
              blender_object))
      {
        const MT_Vector3 &scale = object->NodeGetWorldScaling();
        return 0.5f * std::max((bounds->max[0] - bounds->min[0]) * std::fabs(float(scale[0])),
                               (bounds->max[1] - bounds->min[1]) * std::fabs(float(scale[1])));
      }
    }
  }
  return 0.0f;
}

void KX_Navigation::AssignChaserPlaces(const float dt)
{
  /* Plan item 10 (2026-11-20): NPCs that Pursue one target with Polite avoidance spread round it
   * (a pack round its prey, soldiers round a tank) instead of queueing up behind each other on
   * the side they came from. Rings round the target, the nearest filled first by the nearest
   * chasers, each taking the free place nearest its own side; a place in a wall or behind one
   * (seen from the target along the navmesh) is not given out. The places are kept while the
   * same chasers chase and the target stays within kChaseRingMove of where they were given
   * (then given again: a running target). A lone chaser, Go To, Dense (a horde piling on), Off,
   * flyers and crawlers are left as before. Main thread, in agent order. */
  struct Group {
    KX_GameObject *target;
    std::vector<KX_NavAgent *> chasers;
  };
  std::vector<Group> groups;
  std::unordered_map<const KX_GameObject *, size_t> group_of;
  for (const std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    KX_NavAgent &agent = *holder;
    agent.chase_place_kept = agent.chase_place;
    agent.chase_place = false;
    const KX_NavRequest &request = agent.request;
    if (!agent.requested || request.mode != KX_NavRequest::Mode::Pursue ||
        request.target == nullptr || request.target == agent.object || !request.avoid ||
        request.dense || request.move != KX_NavRequest::Move::Walk ||
        request.climb != KX_NavRequest::Climb::Floors)
    {
      continue;
    }
    const auto [it, added] = group_of.emplace(request.target, groups.size());
    if (added) {
      groups.push_back({request.target, {}});
    }
    groups[it->second].chasers.push_back(&agent);
  }
  if (groups.empty() && m_chaseRings.empty()) {
    return;
  }
  auto turn_between = [](const float a, const float b) {
    float d = std::fmod(a - b, 6.2831853f);
    if (d > 3.14159265f) {
      d -= 6.2831853f;
    }
    else if (d < -3.14159265f) {
      d += 6.2831853f;
    }
    return std::fabs(d);
  };
  std::unordered_map<const KX_GameObject *, ChaseRing> kept;
  for (const Group &group : groups) {
    const size_t n = group.chasers.size();
    if (n < 2) {
      continue;
    }
    const MT_Vector3 target = group.target->NodeGetWorldPosition();
    const KX_NavMesh *navmesh = nullptr;
    for (const KX_NavAgent *chaser : group.chasers) {
      if (chaser->navmesh != nullptr) {
        navmesh = chaser->navmesh;
        break;
      }
    }
    ChaseRing ring;
    ring.chasers.reserve(n);
    for (const KX_NavAgent *chaser : group.chasers) {
      ring.chasers.push_back(chaser->order);
    }
    auto old = m_chaseRings.find(group.target);
    /* (A target on the move: chased as one chaser chases, no places; standing, places.) */
    const bool known = old != m_chaseRings.end();
    const float speed = known ? old->second.speed * 0.8f +
                                    0.2f * distance_2d(old->second.last, target) /
                                        std::max(dt, 1e-4f) :
                                0.0f;
    /* (Places stay until it walks, kChaseStillSpeed; given once it stood a moment under 60% of
     * it.) */
    const bool had_places = known && old->second.still_steps >= kChaseStillSteps;
    uint32_t still_steps = 0;
    if (known && speed < kChaseStillSpeed * (had_places ? 1.0f : 0.6f)) {
      still_steps = std::min(old->second.still_steps + 1, kChaseStillSteps);
    }
    if (still_steps < kChaseStillSteps) {
      ChaseRing waiting;
      waiting.last = target;
      waiting.still_steps = still_steps;
      waiting.speed = speed;
      kept.emplace(group.target, std::move(waiting));
      continue;
    }
    /* (A chaser shut in, not at its place, got nowhere for a while.) */
    bool shut_in = false;
    if (known && m_stepCount > old->second.given_step + 120) {
      for (const KX_NavAgent *chaser : group.chasers) {
        /* (chase_place was cleared above: the step before's is kept.) */
        shut_in = shut_in || (chaser->chase_place_kept && !chaser->chase_settled &&
                              chaser->stall_time > 2.0f * kChaseTurnRoundStall);
      }
    }
    if (old != m_chaseRings.end() && old->second.chasers == ring.chasers && !shut_in &&
        distance_2d(old->second.target_at, target) <= kChaseRingMove &&
        (old->second.checked || navmesh == nullptr))
    {
      ring = std::move(old->second);
    }
    else {
      ring.given_step = m_stepCount;
      ring.target_at = target;
      ring.checked = navmesh != nullptr;
      ring.places.assign(n, ChaseRing::Place());
      float body = 0.0f, reach = FLT_MAX;
      for (const KX_NavAgent *chaser : group.chasers) {
        body = std::max(body, chaser->radius);
        reach = std::min(reach, std::max(chaser->request.arrive_distance, 0.0f));
      }
      body = std::max(body, 0.05f);
      /* Places a body and a half apart on the ring (shoulders a body apart: one walks in
       * between two standing), the first ring half a body out from the target's side, or most of the way to Reach Threshold
       * (chasers that reach far, archers, stand round it at their reach); each further ring as
       * far out again. */
      const float spacing = 3.0f * body;
      float radius = std::max(1.5f * body + FootprintRadiusOf(group.target), 0.9f * reach);
      std::vector<float> angle_of(n), distance_of(n);
      std::vector<size_t> by_distance(n);
      for (size_t i = 0; i < n; i++) {
        const MT_Vector3 d = group.chasers[i]->object->NodeGetWorldPosition() - target;
        angle_of[i] = std::atan2(float(d[1]), float(d[0]));
        distance_of[i] = std::sqrt(float(d[0] * d[0] + d[1] * d[1]));
        by_distance[i] = i;
      }
      std::stable_sort(by_distance.begin(), by_distance.end(), [&](const size_t a, const size_t b) {
        return distance_of[a] < distance_of[b];
      });
      /* Seen from the target along the navmesh (a target off every navmesh: no check). */
      MT_Vector3 from = target;
      const KX_NavMesh *check = navmesh;
      if (check != nullptr && !check->NearestPoint(target, from)) {
        check = nullptr;
      }
      size_t next = 0;
      float outermost = radius;
      std::vector<float> free_angles, spread;
      std::vector<char> used;
      for (int ring_index = 0; ring_index < kChaseRings && next < n;
           ring_index++, radius += spacing)
      {
        const int capacity = std::max(int(6.2831853f * radius / spacing), 1);
        const size_t wanted = std::min(size_t(capacity), n - next);
        /* Its first place faces the side its chasers come from. */
        float cx = 0.0f, cy = 0.0f;
        for (size_t k = next; k < next + wanted; k++) {
          cx += std::cos(angle_of[by_distance[k]]);
          cy += std::sin(angle_of[by_distance[k]]);
        }
        const float base = cx * cx + cy * cy > 1e-6f ? std::atan2(cy, cx) :
                                                       angle_of[by_distance[next]];
        free_angles.clear();
        for (int k = 0; k < capacity; k++) {
          const float angle = base + 6.2831853f * float(k) / float(capacity);
          if (check != nullptr) {
            const MT_Vector3 point = target +
                                     MT_Vector3(std::cos(angle), std::sin(angle), 0.0f) * radius;
            MT_Vector3 snapped, hit, normal;
            float fraction = 1.0f;
            if (!check->NearestPoint(point, snapped) ||
                distance_2d(snapped, point) > 0.5f * body ||
                check->Raycast(from, snapped, fraction, hit, normal))
            {
              continue;
            }
          }
          free_angles.push_back(angle);
        }
        if (free_angles.empty()) {
          continue;
        }
        const size_t take = std::min(free_angles.size(), n - next);
        /* Fewer chasers than places: spread over the places there are. */
        if (take < free_angles.size()) {
          spread.resize(take);
          for (size_t k = 0; k < take; k++) {
            spread[k] = free_angles[k * free_angles.size() / take];
          }
          free_angles.swap(spread);
        }
        /* The arrival: within a body of its place, half the narrowest gap between the places
         * given at most (never past its Reach Threshold: a long one, 1.2 m, let chasers stop on
         * their side of a place round the far side). */
        float narrowest = 6.2831853f;
        for (size_t a = 0; a < free_angles.size(); a++) {
          for (size_t b = a + 1; b < free_angles.size(); b++) {
            narrowest = std::min(narrowest, turn_between(free_angles[a], free_angles[b]));
          }
        }
        const float gap = narrowest * radius;
        used.assign(free_angles.size(), 0);
        for (size_t k = 0; k < take; k++) {
          const size_t i = by_distance[next + k];
          size_t best = 0;
          float best_turn = FLT_MAX;
          for (size_t j = 0; j < free_angles.size(); j++) {
            if (used[j]) {
              continue;
            }
            const float turn = turn_between(free_angles[j], angle_of[i]);
            if (turn < best_turn - 1e-6f) {
              best_turn = turn;
              best = j;
            }
          }
          used[best] = 1;
          ChaseRing::Place &place = ring.places[i];
          place.placed = true;
          place.angle = free_angles[best];
          place.radius = radius;
          place.reach = std::min({0.5f * gap,
                                  std::max(body, 0.3f),
                                  std::max(group.chasers[i]->request.arrive_distance, 0.0f)});
        }
        next += take;
        outermost = radius;
      }
      /* Going round to a place on the far side: outside every ring (between two rings a body
       * does not pass). */
      for (ChaseRing::Place &place : ring.places) {
        place.orbit = outermost + spacing;
      }
    }
    ring.last = target;
    ring.still_steps = still_steps;
    ring.speed = speed;
    for (size_t i = 0; i < n; i++) {
      const ChaseRing::Place &place = ring.places[i];
      KX_NavAgent &chaser = *group.chasers[i];
      if (!chaser.chase_place_kept || chaser.chase_angle != place.angle ||
          chaser.chase_radius != place.radius)
      {
        chaser.chase_side = 0;
      }
      chaser.chase_place_kept = place.placed;
      chaser.chase_place = place.placed;
      chaser.chase_angle = place.angle;
      chaser.chase_radius = place.radius;
      chaser.chase_reach = place.reach;
      chaser.chase_orbit = place.orbit;
    }
    kept.emplace(group.target, std::move(ring));
  }
  m_chaseRings.swap(kept);
}

void KX_Navigation::ResolveGoal(KX_NavAgent &agent)
{
  const KX_NavRequest &request = agent.request;
  const MT_Vector3 &position = agent.position;
  /* (A chaser with a place round its target: arrived within its place's reach.) */
  float arrive = std::max(request.arrive_distance, 0.0f);
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
  /* Flying (not floating): distances, ways away and predictions in space (a bat high above the
   * player is far from it); else along the ground. */
  const bool in_space = agent.air != nullptr && !agent.floating;
  auto distance_to = [&](const MT_Vector3 &p) -> float {
    return in_space ? float((p - position).length()) : distance_2d(position, p);
  };
  /* Crawling (phase 9): near along its surface (the last step's), and off it no farther than
   * its shell (its way runs a radius out from the surface, its body at its own height). */
  const bool crawl_measure = agent.crawls && agent.on_surface && agent.air != nullptr;
  auto crawl_near = [&](const MT_Vector3 &offset, const float reach) {
    const float off = float(offset.dot(agent.surface_normal));
    const MT_Vector3 along = offset - agent.surface_normal * off;
    return float(along.length()) <= reach &&
           std::fabs(off) <= agent.radius + 2.0f * agent.air->Voxel() + reach;
  };
  /* A point `distance` away from `threat`, as far along the navmesh as it goes (straight away,
   * else turned up to 90 degrees each way). */
  auto away_point = [&](const MT_Vector3 &threat, const float distance) -> MT_Vector3 {
    MT_Vector3 away = in_space ? position - threat : flat(position - threat);
    if (away.length2() < 1e-8f) {
      away = MT_Vector3(1.0f, 0.0f, 0.0f);
    }
    away.normalize();
    const float want = std::max(distance - distance_to(threat), 1.0f) + arrive;
    if (agent.crawls && agent.air != nullptr) {
      /* Crawling: the surface point nearest straight away (turned round the vertical when there
       * is none near), on the surfaces it may use. */
      static const float kCrawlTurns[] = {0.0f, 0.785398f, -0.785398f, 1.570796f, -1.570796f};
      for (const float turn : kCrawlTurns) {
        const float c = std::cos(turn), s = std::sin(turn);
        const MT_Vector3 dir(away[0] * c - away[1] * s, away[0] * s + away[1] * c, away[2]);
        MT_Vector3 point;
        if (agent.air->NearestShell(
                position + dir * want, std::max(2.0f, want * 0.5f), agent.crawl_kinds, point))
        {
          return point;
        }
      }
      return position;
    }
    if (agent.air != nullptr) {
      /* Flying: away (up or down too), the first of the ways turned round the vertical the air
       * lets it fly, else the free point nearest straight away. */
      static const float kAirTurns[] = {0.0f, 0.785398f, -0.785398f, 1.570796f, -1.570796f};
      for (const float turn : kAirTurns) {
        const float c = std::cos(turn), s = std::sin(turn);
        const MT_Vector3 dir(away[0] * c - away[1] * s, away[0] * s + away[1] * c, away[2]);
        const MT_Vector3 end = position + dir * want;
        if (agent.air->LineFree(position, end, agent.water)) {
          return end;
        }
      }
      MT_Vector3 free;
      return agent.air->NearestFree(position + away * want, want, free, agent.water) ? free :
                                                                                     position;
    }
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
  /* Arrival is judged at `arrive_at`: the goal, but a chaser going round its target's ring heads
   * for a point on the way to its place (plan item 10). */
  MT_Vector3 arrive_at(0.0f, 0.0f, 0.0f);
  bool going_round = false;
  /* (A chaser with a place: where the target will be.) */
  MT_Vector3 chase_target(0.0f, 0.0f, 0.0f);
  bool chasing_place = false;
  switch (request.mode) {
    case KX_NavRequest::Mode::GoTo:
      goal = target_position();
      break;
    case KX_NavRequest::Mode::Wander: {
      bool next_point = !agent.has_wander_point;
      if (!next_point && agent.air != nullptr) {
        /* Flying: there in space (floating: across the water), or at the end of a way that
         * could not reach it (a point in a sealed space: as near as it gets, then another). */
        const float reach = std::max(arrive, agent.radius);
        auto close_enough = [&](MT_Vector3 offset) {
          if (agent.floating) {
            offset[2] = 0.0f;
          }
          return crawl_measure ? crawl_near(offset, reach) : float(offset.length()) <= reach;
        };
        next_point = close_enough(agent.wander_point - position);
        if (!next_point && !agent.air_needs_search && !agent.air_complete &&
            float((agent.air_path_goal - agent.wander_point).length()) < 1e-3f)
        {
          next_point = close_enough(agent.air_path.empty() ? MT_Vector3(0.0f, 0.0f, 0.0f) :
                                                     agent.air_path.back() - position);
        }
      }
      else if (!next_point) {
        next_point = arrived_at(
            position, agent.wander_point, std::max(arrive, agent.radius), agent.height);
      }
      if (next_point) {
        const uint64_t seed = mix64(request.seed ^ mix64(agent.wander_count++));
        MT_Vector3 point;
        bool found = false;
        if (agent.navmesh != nullptr) {
          found = agent.navmesh->RandomPoint(
              request.destination, std::max(request.radius, 0.1f), seed, point);
        }
        else if (agent.air != nullptr) {
          /* Flying: a point of the ball round home (swimming in water, floating: its disc),
           * the free point nearest it. */
          const float u = float(seed >> 40) * (1.0f / 16777216.0f);
          const float v = float((seed >> 16) & 0xffffff) * (1.0f / 16777216.0f);
          const float w = float(mix64(seed) >> 40) * (1.0f / 16777216.0f);
          const float r = std::max(request.radius, 0.0f) * std::cbrt(u);
          const float a = v * 6.2831853f;
          const float z = agent.floating ? 0.0f : 2.0f * w - 1.0f;
          const float ring = std::sqrt(std::max(1.0f - z * z, 0.0f));
          const MT_Vector3 wanted = request.destination +
                                    MT_Vector3(r * ring * std::cos(a), r * ring * std::sin(a), r * z);
          /* (Crawling: the nearest point of a surface it may use.) */
          found = agent.crawls ? agent.air->NearestShell(wanted,
                                                         std::max(2.0f, request.radius * 0.5f),
                                                         agent.crawl_kinds,
                                                         point) :
                                 agent.air->NearestFree(
                                     wanted, std::max(2.0f, request.radius * 0.5f), point,
                                     agent.water);
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
      if (distance_to(threat) >= request.radius) {
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
      const MT_Vector3 velocity = target_velocity();
      const MT_Vector3 predicted = target + (in_space ? velocity : flat(velocity)) * time;
      if (request.mode == KX_NavRequest::Mode::Pursue) {
        goal = predicted;
        if (agent.chase_place) {
          /* Its place round where the target will be (plan item 10). One on the far side is gone
           * round to outside the ring (straight it walked into the target and the chasers already
           * standing there, and stood behind them): a point a step round from where it is. */
          const MT_Vector3 place = predicted + MT_Vector3(std::cos(agent.chase_angle),
                                                          std::sin(agent.chase_angle),
                                                          0.0f) *
                                                   agent.chase_radius;
          arrive = agent.chase_reach;
          goal = place;
          arrive_at = place;
          chase_target = predicted;
          chasing_place = true;
          const MT_Vector3 out = flat(position - predicted);
          const float out_length = float(out.length());
          if (out_length > 1e-3f && out_length < agent.chase_orbit + kChaseOrbitNear) {
            const float now = std::atan2(float(out[1]), float(out[0]));
            float turn = std::fmod(agent.chase_angle - now, 6.2831853f);
            if (turn > 3.14159265f) {
              turn -= 6.2831853f;
            }
            else if (turn < -3.14159265f) {
              turn += 6.2831853f;
            }
            if (std::fabs(turn) > kChaseOrbitStep) {
              const float orbit = std::max(agent.chase_orbit, out_length);
              auto round_point = [&](const int side) {
                const float toward = now + float(side) * kChaseOrbitStep;
                return predicted + MT_Vector3(std::cos(toward), std::sin(toward), 0.0f) * orbit;
              };
              if (agent.chase_side == 0) {
                /* The shorter way, unless a wall stands in it and not in the other. */
                const int shorter = turn > 0.0f ? 1 : -1;
                agent.chase_side = shorter;
                if (agent.navmesh != nullptr) {
                  float fraction = 1.0f;
                  MT_Vector3 hit, normal;
                  if (agent.navmesh->Raycast(
                          position, round_point(shorter), fraction, hit, normal) &&
                      !agent.navmesh->Raycast(
                          position, round_point(-shorter), fraction, hit, normal))
                  {
                    agent.chase_side = -shorter;
                  }
                }
              }
              /* Shut in going round one way (the ring's places taken beside a wall): the other
               * way, at most every two seconds. */
              if (agent.stall_time > kChaseTurnRoundStall &&
                  m_stepCount > agent.chase_side_step + 120)
              {
                agent.chase_side = -agent.chase_side;
                agent.chase_side_step = m_stepCount;
              }
              goal = round_point(agent.chase_side);
              going_round = true;

            }
          }
        }
      }
      else if (distance_to(predicted) >= request.radius) {
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
      /* With a Target Object: where it now is (2026-11-19: the node's hidden Destination, the
       * world's origin, was taken: every flock flew there). */
      goal = request.goal ? target_position() : position;
      break;
  }
  /* Decks (plan item 11, 2026-11-20): a flyer whose goal is on a vehicle's deck comes down on
   * it from above (over its hull first, then straight down through it) and rides it once there;
   * riding, a goal off that deck makes it take off straight up out of the hull first. A goal
   * above the deck (beyond a flyer's height over it) or beside the vehicle is flown to as
   * before; swimmers and crawlers never land. */
  agent.through_hull = nullptr;
  KX_GameObject *deck_of_goal = nullptr;
  if (agent.flies && !agent.water && !agent.crawls && agent.air != nullptr && !done &&
      !m_vehicles.empty())
  {
    const bool lands = request.mode == KX_NavRequest::Mode::GoTo ||
                       request.mode == KX_NavRequest::Mode::Pursue ||
                       request.mode == KX_NavRequest::Mode::Formation;
    const float clearance = agent.air_radius + 0.3f;
    KX_NavVehicle *deck = nullptr;
    MT_Transform deck_pose;
    if (lands) {
      for (const std::unique_ptr<KX_NavVehicle> &vehicle : m_vehicles) {
        if (vehicle->type == nullptr) {
          continue;
        }
        const MT_Transform pose = pose_of(vehicle->object);
        MT_Transform inverse;
        inverse.invert(pose);
        const MT_Vector3 local = inverse(goal);
        const MT_Vector3 &lo = vehicle->type->bounds_min, &hi = vehicle->type->bounds_max;
        if (local[0] >= lo[0] && local[0] <= hi[0] && local[1] >= lo[1] && local[1] <= hi[1] &&
            local[2] >= hi[2] - 0.5f && local[2] <= hi[2] + std::max(1.0f, 2.0f * agent.air_radius))
        {
          deck = vehicle.get();
          deck_pose = pose;
          break;
        }
      }
    }
    deck_of_goal = deck != nullptr ? deck->object : nullptr;
    bool taking_off = false;
    if (agent.riding != nullptr && agent.riding != deck_of_goal) {
      if (KX_NavVehicle *from = FindVehicleRecord(agent.riding);
          from != nullptr && from->type != nullptr)
      {
        const MT_Transform pose = pose_of(from->object);
        MT_Transform inverse;
        inverse.invert(pose);
        const MT_Vector3 local = inverse(position);
        const MT_Vector3 &lo = from->type->bounds_min, &hi = from->type->bounds_max;
        const float top = hull_spheres(lo, hi).top + clearance;
        if (float(local[2]) < top - 0.05f && local[0] >= lo[0] - clearance &&
            local[0] <= hi[0] + clearance && local[1] >= lo[1] - clearance &&
            local[1] <= hi[1] + clearance)
        {
          arrive_at = goal;
          goal = pose(MT_Vector3(local[0], local[1], top));
          going_round = true;
          taking_off = true;
          agent.through_hull = agent.riding;
        }
      }
      if (!taking_off) {
        agent.riding = nullptr;
      }
    }
    if (agent.landing_on != deck_of_goal) {
      agent.landing_on = nullptr;
    }
    if (!taking_off && deck != nullptr) {
      if (agent.riding == deck->object) {
        /* On its own deck. */
        agent.through_hull = deck->object;
      }
      else {
        MT_Transform inverse;
        inverse.invert(deck_pose);
        const MT_Vector3 local_goal = inverse(goal), local = inverse(position);
        const float off = std::hypot(float(local[0] - local_goal[0]),
                                     float(local[1] - local_goal[1]));
        /* Coming down once above the place (kept while it stays roughly above: a turning deck
         * moves the place under it). */
        const float above = std::max(agent.air_radius, 0.5f);
        if (off <= above || (agent.landing_on == deck->object && off <= 3.0f * above)) {
          agent.landing_on = deck->object;
          agent.through_hull = deck->object;
          /* (Carried with the deck from this step's end on: it comes down on the place.) */
          agent.frame = deck->object;
        }
        else {
          const float top = hull_spheres(deck->type->bounds_min, deck->type->bounds_max).top +
                            clearance;
          arrive_at = goal;
          goal = deck_pose(
              MT_Vector3(local_goal[0], local_goal[1], std::max(top, float(local_goal[2]))));
          going_round = true;
        }
      }
    }
  }
  agent.goal = goal;
  agent.has_goal = true;
  if (!going_round) {
    arrive_at = goal;
  }
  if (done) {
    agent.arrived = true;
  }
  else if (agent.air != nullptr && request.mode != KX_NavRequest::Mode::Wander &&
           (request.mode != KX_NavRequest::Mode::Flock || request.goal))
  {
    /* Flying: within reach in space (floating: across the water); or at the end of a way that
     * reaches as near as the air lets (a goal on the ground: just above it). Crawling: within
     * reach along its surface (the last step's), and off it no farther than its shell (its way
     * runs a radius out, its body at its own height). */
    /* (Riding its goal's deck and arrived in the step before: a little more, the deck's turns
     * carried it a hair off; it flew back and forth at the reach's rim, When Reached each time.) */
    const float reach = std::max(arrive, 0.05f) *
                        (deck_of_goal != nullptr && agent.riding == deck_of_goal &&
                                 agent.info.arrived ?
                             1.25f :
                             1.0f);
    auto within = [&](MT_Vector3 offset) {
      if (agent.floating) {
        offset[2] = 0.0f;
      }
      return crawl_measure ? crawl_near(offset, reach) : float(offset.length()) <= reach;
    };
    agent.arrived = within(arrive_at - position);
    /* (Not for a place on a deck: free air in the map whatever the deck, and the way's end,
     * searched for where it was a moment ago, would be taken for it.) */
    if (!agent.arrived && !going_round && deck_of_goal == nullptr && agent.air_complete &&
        !agent.air_path.empty() &&
        float((agent.air_path_goal - goal).length()) <= std::max(arrive, 0.5f))
    {
      agent.arrived = within(agent.air_path.back() - position);
    }
    /* Arrived on a deck (plan item 11): it rides it. */
    if (agent.arrived && deck_of_goal != nullptr && !going_round) {
      agent.riding = deck_of_goal;
      agent.landing_on = nullptr;
      /* (Carried from this step's end on.) */
      agent.frame = deck_of_goal;
    }
  }
  else if (request.mode != KX_NavRequest::Mode::Wander &&
           (request.mode != KX_NavRequest::Mode::Flock || request.goal))
  {
    agent.arrived = arrived_at(position, arrive_at, arrive, agent.height);
    if (chasing_place) {
      /* A chaser with a place: arrived at it, or by catching its target as a lone chaser does
       * (within its Reach Threshold of where the target will be: When Reached is the catch; a
       * wolf after a running player caught nothing, its place round the player moving on);
       * caught, it settles to its place from there (AvoidAgents, making way). */
      agent.chase_settled = agent.arrived && !going_round;
      agent.chase_settle = goal;
      if (!agent.arrived && arrived_at(position,
                                       chase_target,
                                       std::max(request.arrive_distance, 0.0f),
                                       agent.height))
      {
        agent.arrived = true;
      }
    }
    /* A goal off the navmesh (the middle of a chest, a table, a pillar): reached at the navmesh's
     * point nearest to it once its way leads there, as flyers at the end of their way (Unreal
     * tests arrival against the destination projected onto the navmesh). It stood beside the
     * pillar for good and When Reached never came; a fast one arrived by pressing, a slow one
     * never (2026-11-19). Only a point within the usual snap reach of the goal, on a way searched
     * for this goal. */
    /* (Not a chaser's place round its target: its places are on the navmesh, and its way's end,
     * searched for a place a step before, was taken for the place it moved on to.) */
    if (!agent.arrived && !agent.chase_place && agent.navmesh != nullptr && agent.corridor_ready &&
        agent.path_complete && float((agent.path_goal - goal).length()) <= std::max(arrive, 0.5f))
    {
      const MT_Vector3 end = agent.navmesh->ToWorld(agent.corridor.getTarget());
      const MT_Vector3 off = goal - end;
      if (std::fabs(float(off[0])) <= KX_NavMesh::kSnapExtents[0] &&
          std::fabs(float(off[1])) <= KX_NavMesh::kSnapExtents[2] &&
          std::fabs(float(off[2])) <= KX_NavMesh::kSnapExtents[1])
      {
        agent.arrived = arrived_at(position, end, arrive, agent.height);
      }
    }
    /* As near as it gets: near its goal, kept from it for a second by others standing there (a
     * horde round the player, a queue at a counter). The press counts for the goal it pressed
     * toward: a new one starts it again (2026-11-19: creeping into a point with a small Reach
     * Threshold counted as pressing, and the next point within the crowd reach, 9 m for a 3 m
     * platform, 2.5 m for a walker, was arrived at once and skipped; found by the examples).
     * Only a fixed place's (Go To with no Target Object, Follow Path): a goal that moves by itself
     * (a pursued player) keeps the count of the horde pressing round it. */
    if (request.mode == KX_NavRequest::Mode::GoTo && request.target == nullptr &&
        float((goal - agent.press_goal).length()) > std::max(arrive, 0.5f))
    {
      agent.press_time = 0.0f;
    }
    agent.press_goal = goal;
    /* (A chaser with a place round its target: the others make way, so its place is its own;
     * slowly squeezing in between two of them is no crowd at its goal. Kept from it for longer,
     * by a body's width: a body that does not make way, a player standing there.) */
    const float crowd_reach = agent.chase_place ?
                                  arrive + 2.0f * agent.radius :
                                  arrive + std::max(2.0f, agent.radius * 6.0f);
    const float press_needed = agent.chase_place ? kChasePressSeconds : 1.0f;
    if (!agent.arrived && !going_round && agent.press_time >= press_needed &&
        arrived_at(position, arrive_at, crowd_reach, agent.height))
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
      /* Tiles were built again (a level piece added, removed or moved, an obstacle carved or
       * freed): the walls it keeps for avoidance are read again, and a corridor through a
       * changed tile is cut where it stops being valid and searched again. Its own polygon may
       * be gone too (the tile under it): it starts from the nearest one now, as DetourCrowd
       * does (given its old one, every search failed and it stood still for good).
       *
       * 2026-11-19: the places that changed decide how much is checked. One whose way round
       * (here to the place, the place to the goal, straight) is not longer than the way left
       * may lie on it: the whole corridor is checked (it was the first 16 polygons only), and
       * one clearly shorter may open a shortcut (a gate opened far away): it searches again.
       * Changes far from its way cost it nothing. */
      agent.boundary_ready = false;
      bool near_way = false, shorter = false;
      if (agent.corridor_ready) {
        const float *target = agent.corridor.getTarget();
        const float straight = dtVdist2D(pos, target);
        const float left = std::max(agent.path_estimate - agent.traveled, straight);
        const MT_Vector3 there = agent.navmesh->ToWorld(target);
        const bool known = agent.navmesh->ChangesSince(
            agent.tile_stamp, [&](const KX_NavChangedPlace &place) {
              const float via = place_distance(agent.position, place) +
                                place_distance(there, place);
              near_way = near_way || via <= left * 1.1f + 2.0f;
              shorter = shorter || via < left * 0.9f - 1.0f;
            });
        if (!known) {
          near_way = true;
          shorter = left > straight * 1.1f + 1.0f;
        }
      }
      const int check = near_way ? agent.corridor.getPathCount() :
                                   std::min(16, agent.corridor.getPathCount());
      if (agent.corridor_ready && !agent.corridor.isValid(check, query, &agent.filter)) {
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
      else if (shorter && agent.requested) {
        agent.needs_search = true;
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
        agent.off_navmesh = false;
        agent.off_navmesh_steps = 0;
      }
      else {
        agent.corridor.reset(0, pos);
        /* Off every floor NPCs walk (item 8's reason "Off Navmesh"; item 9 tells it once), for
         * kOffNavmeshSteps: a size class made in this step (a Character's own climb, known
         * after the step's build) has no tiles yet. */
        agent.off_navmesh_steps = agent.requested ? agent.off_navmesh_steps + 1 : 0;
        agent.off_navmesh = agent.off_navmesh_steps >= kOffNavmeshSteps;
        if (agent.off_navmesh && !agent.off_navmesh_reported) {
          agent.off_navmesh_reported = true;
          const MT_Vector3 at = agent.object->NodeGetWorldPosition();
          char text[256];
          snprintf(text,
                   sizeof(text),
                   "'%s' at (%.1f, %.1f, %.1f) is off every floor NPCs walk (more than 2 m across "
                   "or 4 m up or down from the navigation mesh): it cannot walk from there",
                   agent.object->GetName().c_str(),
                   float(at[0]),
                   float(at[1]),
                   float(at[2]));
          ReportProblem("npc_off_navmesh", agent.object->GetName(), text);
        }
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

/** A path's length through its polygons' middles (Recast's plane): not the walked way, but near
 * it and never much shorter on a winding path. */
static float path_length_estimate(const dtNavMeshQuery *query,
                                  const float *start,
                                  const dtPolyRef *path,
                                  const int npath,
                                  const float *target)
{
  const dtNavMesh *mesh = query->getAttachedNavMesh();
  float length = 0.0f;
  float previous[3];
  dtVcopy(previous, start);
  for (int i = 0; i < npath; i++) {
    const dtMeshTile *tile = nullptr;
    const dtPoly *poly = nullptr;
    if (dtStatusFailed(mesh->getTileAndPolyByRef(path[i], &tile, &poly)) || poly->vertCount == 0)
    {
      continue;
    }
    float middle[3] = {0.0f, 0.0f, 0.0f};
    for (int v = 0; v < poly->vertCount; v++) {
      dtVadd(middle, middle, &tile->verts[poly->verts[v] * 3]);
    }
    dtVscale(middle, middle, 1.0f / float(poly->vertCount));
    length += dtVdist2D(previous, middle);
    dtVcopy(previous, middle);
  }
  return length + dtVdist2D(previous, target);
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
  agent.path_estimate = path_length_estimate(query, start, path, kept, target);
  agent.traveled = 0.0f;
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
      else if (agent.crowded_ahead) {
        /* A crowd gathered on its way: perhaps another way is freer now. */
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
  /* Around NPCs (2026-11-19): a goal where nothing is built yet (no NPC of its size near it):
   * toward it, the built point nearest the way there (on the straight line, within its own
   * built area), a partial way searched again as it goes (kPartialReplanSteps) while the tiles
   * ahead are built. It stood still for good ("No Path"; found by the examples). */
  bool toward_only = false;
  if (goal_ref == 0 && agent.request.navmesh == nullptr &&
      m_settings.build_mode == blender::NAVMESH_BUILD_AROUND_NPCS)
  {
    const MT_Vector3 offset = agent.goal - agent.position;
    const MT_Vector3 flat(offset[0], offset[1], 0.0f);
    const float distance = float(flat.length());
    if (distance > 1e-3f) {
      const float reach = std::max(m_settings.build_radius * 0.8f, 1.0f);
      const MT_Vector3 ahead = agent.position + flat * (std::min(reach, distance) / distance);
      float toward[3];
      agent.navmesh->ToNav(ahead, toward);
      const float around[3] = {reach, KX_NavMesh::kSnapExtents[1] * 4.0f, reach};
      query->findNearestPoly(toward, around, &agent.filter, &goal_ref, goal_near);
      toward_only = goal_ref != 0;
    }
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
  if (toward_only) {
    /* (Only on the way: searched again before it gets there.) */
    agent.path_complete = false;
  }
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
    KX_NavJumpLimits jump;
    bool operator==(const Key &o) const
    {
      return navmesh == o.navmesh && ref == o.ref && closed == o.closed && jump == o.jump;
    }
  };
  std::vector<std::pair<Key, KX_NavAgent *>> keyed;
  keyed.reserve(count);
  dtNavMeshQuery *query = nullptr;
  KX_NavMesh *query_mesh = nullptr;
  for (size_t i = 0; i < count; i++) {
    KX_NavAgent *agent = due[i];
    Key key{agent->navmesh, 0, agent->filter.closed_areas, agent->filter.jump};
    if (count >= kMinShared && (!area_costs_default(*agent) || !agent->failed_links.empty() ||
                                !agent->filter.gave_up.empty()))
    {
      /* Its own costs, or links closed to it: its own search. */
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
  /* Agents that search their own way do so after every agent read its way from the Dijkstra's
   * tree: their searches use the same query's nodes (2026-11-19: one in the middle of the
   * group wiped the tree, every later agent read a broken way, partial and short, and stood at
   * a ledge for up to 5 s before its next search). */
  thread_local std::vector<KX_NavAgent *> own;
  own.clear();
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
      own.push_back(agent);
      continue;
    }
    /* The search ran from the goal over links as they go from there: a link on the way back
     * may be one way only (a drop: 2026-11-19, the links the build finds are all one way), so
     * such an agent searches its own way (given the shared way, Detour finds no corners over the
     * link turned round: it walks to the ledge and stands there until the corridor's topology
     * repair). */
    bool over_link = false;
    for (int i = 0; i < npath && !over_link; i++) {
      const dtMeshTile *tile = nullptr;
      const dtPoly *poly = nullptr;
      agent->navmesh->Mesh()->getTileAndPolyByRefUnsafe(path[size_t(i)], &tile, &poly);
      over_link = poly != nullptr && poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION;
    }
    if (over_link) {
      own.push_back(agent);
      continue;
    }
    /* The search ran from the goal: the agent's way is the reverse. It ends at the agent's own
     * goal (2026-11-19: it ended at the first agent's, so a crowd whose goals lay in one big
     * polygon, a hall's places, all walked to one point and jammed there): its nearest point of
     * the shared polygon. */
    std::reverse(path.begin(), path.begin() + npath);
    float own_goal[3], own_near[3];
    agent->navmesh->ToNav(agent->goal, own_goal);
    bool over = false;
    if (dtStatusFailed(query->closestPointOnPoly(goal_ref, own_goal, own_near, &over))) {
      dtVcopy(own_near, goal_near);
    }
    adopt_path(*agent, query, DT_SUCCESS, path.data(), npath, goal_ref, own_near);
  }
  for (KX_NavAgent *agent : own) {
    SearchAgentPath(*agent);
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
  /* The link by its own number (its place in the list changes as links come and go; a jump's
   * way back has kNavLinkReverse; links the build found are in its table). */
  const KX_NavLink *link = nullptr;
  uint32_t id = 0;
  if (connection != nullptr) {
    /* (On a vehicle, its own link: its type's are the first copy's, E2.) */
    id = agent.filter.OwnLinkId(connection->userId);
    auto found = m_linkIndexById.find(id & ~kNavLinkReverse);
    if (id < kNavFoundLinkBase && found != m_linkIndexById.end() &&
        found->second < m_links.size())
    {
      link = &m_links[found->second];
    }
  }
  agent.on_link = true;
  agent.link_started = true;
  agent.link_id = id;
  agent.link_object = link ? link->object : nullptr;
  agent.link_kind = link ? link->kind : LinkKind(id);
  agent.link_start = agent.navmesh->ToWorld(start);
  agent.link_end = agent.navmesh->ToWorld(end);
  agent.link_time = 0.0f;
  /* On a vehicle (E2, 2026-11-20): its ends move with the deck while it crosses (in the
   * vehicle's own space: the navmesh's). */
  agent.link_on_frame = agent.frame != nullptr;
  if (agent.link_on_frame) {
    agent.link_from_frame = agent.frame;
    agent.link_to_frame = agent.frame;
    agent.link_start_local = KX_NavMesh::NavToLocal(start);
    agent.link_end_local = KX_NavMesh::NavToLocal(end);
  }
  /* A jump or a drop flies the arc the build tested (KX_NavJumpArc: its top a little over the
   * higher end, 2026-11-19; a drop went straight through its ledge's corner), as long as a
   * throw under the scene's gravity takes; the rest go straight at the NPC's speed. */
  agent.link_jump = KX_NavLinkKindIsJump(agent.link_kind);
  agent.link_clearance = agent.link_jump ?
                             KX_NavJumpClearance(agent.link_start, agent.link_end) :
                             0.0f;
  const float distance = float((agent.link_end - agent.link_start).length());
  float up_speed = 0.0f, plan_speed = 0.0f, air_time = 0.0f;
  if (link && link->duration > 0.0f) {
    agent.link_duration = link->duration;
  }
  else if (link && link->kind == "teleport") {
    agent.link_duration = 0.0f;
  }
  else if (agent.link_jump && KX_NavJumpLaunch(agent.link_start,
                                               agent.link_end,
                                               agent.link_clearance,
                                               agent.gravity,
                                               up_speed,
                                               plan_speed,
                                               air_time))
  {
    agent.link_duration = air_time;
  }
  else {
    agent.link_duration = distance / std::max(agent.request.speed, 0.1f);
  }
  /* A physics jump (2026-11-19): a Character or a body moved as Dynamic jumps a jump or a drop
   * itself when the scene says so (Jumps: Physics) and the link sets no time of its own. From
   * where it stands (it starts a link a radius or two before its start), at the start's height,
   * the throw whose top is a little over the higher end; a Character judged by its own jump
   * launches at most at its jump speed (a lower, faster throw: its way took only links it
   * makes). */
  agent.link_physics = false;
  agent.link_launch = false;
  agent.link_landed = false;
  if (m_settings.physics_jumps && agent.link_jump && agent.can_launch &&
      !(link && link->duration > 0.0f))
  {
    const MT_Vector3 from(agent.position[0], agent.position[1], agent.link_start[2]);
    const float clearance = KX_NavJumpClearance(from, agent.link_end);
    const float g = agent.gravity;
    if (KX_NavJumpLaunch(from, agent.link_end, clearance, g, up_speed, plan_speed, air_time)) {
      const KX_NavJumpLimits &limits = agent.filter.jump;
      if (limits.physics && limits.jump_speed > 0.0f && up_speed > limits.jump_speed) {
        up_speed = limits.jump_speed;
        const float top = up_speed * up_speed / (2.0f * g);
        const float rise = float(agent.link_end[2] - from[2]);
        air_time = up_speed / g + std::sqrt(2.0f * std::max(top - rise, 0.0f) / g);
        plan_speed = float(flat(agent.link_end - from).length()) / std::max(air_time, 1e-3f);
      }
      agent.link_physics = true;
      agent.link_launch = true;
      agent.link_up_speed = up_speed;
      agent.link_plan_speed = plan_speed;
      agent.link_duration = air_time;
      agent.link_from = agent.position;
      agent.link_left_ground = false;
      agent.link_fell = false;
    }
  }
  return true;
}

/** A missed physics jump keeps its link closed to the NPC this long (s). */
constexpr float kMissedLinkSeconds = 10.0f;

void KX_Navigation::StepLink(KX_NavAgent &agent, const float dt)
{
  agent.link_time += dt;
  if (agent.link_frames || agent.link_on_frame) {
    /* A link between frames (2026-11-19): its ends where their frames took them. */
    if (agent.link_from_frame != nullptr) {
      agent.link_start = pose_of(agent.link_from_frame)(agent.link_start_local);
    }
    if (agent.link_to_frame != nullptr) {
      agent.link_end = pose_of(agent.link_to_frame)(agent.link_end_local);
    }
  }
  agent.next_corner = agent.link_end;
  agent.remaining = float((agent.link_end - agent.position).length());
  if (agent.link_physics) {
    /* Through the air by itself: launched in the HandOver of its first step (link_started),
     * steered toward the landing so as to reach it at the throw's time, landed when it stands
     * again (a Character) or stops falling, or given up a second and a half after the throw's
     * time. On the link's end (within a radius and 0.75 m, its height as the link's): it walks
     * on from the corridor's place there. Elsewhere (knocked off, short of the ledge): the link
     * is closed to it for kMissedLinkSeconds and its way is searched again from where it is. */
    const bool launched = !agent.link_started;
    if (launched) {
      agent.link_left_ground = agent.link_left_ground || !agent.grounded;
      agent.link_fell = agent.link_fell || agent.velocity[2] < -0.3f;
    }
    bool landed = false;
    if (launched && agent.link_time > 0.1f) {
      landed = agent.drive == KX_NavAgent::Drive::Walk ?
                   agent.link_left_ground && agent.grounded :
                   agent.link_fell && agent.velocity[2] > -0.3f;
    }
    if (landed || agent.link_time > agent.link_duration + 1.5f) {
      const float off = float(flat(agent.position - agent.link_end).length());
      const float rose = float(agent.position[2] - agent.link_from[2]);
      const float wanted = float(agent.link_end[2] - agent.link_start[2]);
      const float climb = agent.size.climb > 0.0f ? agent.size.climb : m_settings.agent_climb;
      agent.link_landed = off <= agent.radius + 0.75f && std::fabs(rose - wanted) <= climb + 0.5f;
      agent.on_link = false;
      agent.link_physics = false;
      agent.link_ended = true;
      agent.link_on_frame = false;
      agent.desired = MT_Vector3(0.0f, 0.0f, 0.0f);
      agent.link_steer = MT_Vector3(0.0f, 0.0f, 0.0f);
      if (agent.link_frames) {
        /* A jump between frames (E5, 2026-11-20): on the other frame now if it landed there,
         * else its frame found again under it. */
        agent.link_frames = false;
        agent.frame = agent.link_landed ? agent.link_to_frame : agent.frame;
        agent.frame_known = agent.link_landed;
        agent.route.clear();
        agent.route_step = 0;
        agent.corridor_ready = false;
        agent.boundary_ready = false;
      }
      if (!agent.link_landed) {
        agent.failed_links.emplace_back(
            agent.link_id, m_stepCount + uint64_t(kMissedLinkSeconds / std::max(dt, 1e-3f)));
        agent.corridor_ready = false;
        agent.offset_known = false;
        agent.needs_search = true;
      }
      return;
    }
    const MT_Vector3 to_end = flat(agent.link_end - agent.position);
    const float left = std::max(agent.link_duration - agent.link_time, 0.1f);
    MT_Vector3 steer = to_end / left;
    /* A landing on a moving vehicle (E2, E5): where it will be when it lands (the deck's own
     * velocity on top of the way to its landing now). */
    if ((agent.link_frames || agent.link_on_frame) && agent.link_to_frame != nullptr) {
      if (const KX_NavVehicle *vehicle = FindVehicleRecord(agent.link_to_frame)) {
        steer += flat(vehicle->linear_velocity);
      }
    }
    const float most = std::max(agent.link_plan_speed * 1.5f, std::max(agent.request.speed, 1.0f));
    if (float(steer.length()) > most) {
      steer *= most / float(steer.length());
    }
    agent.link_steer = steer;
    agent.desired = steer;
    return;
  }
  /* Carried along the link: a jump or a drop flies its arc, the rest go straight. */
  const float t = agent.link_duration > 0.0f ?
                      std::min(agent.link_time / agent.link_duration, 1.0f) :
                      1.0f;
  MT_Vector3 p = agent.link_jump ?
                     KX_NavJumpArc(agent.link_start, agent.link_end, agent.link_clearance, t) :
                     agent.link_start + (agent.link_end - agent.link_start) * t;
  p[2] += agent.height_offset;
  agent.link_position = p;
  agent.desired = (p - agent.position) / std::max(dt, 1e-4f);
  if (t >= 1.0f) {
    agent.on_link = false;
    agent.link_ended = true;
    agent.link_landed = true;
    agent.link_on_frame = false;
    /* The corridor stands at the link's end: walking on from there. Over a link between frames
     * it stands on the other frame now: its way from there on that frame's navmesh. */
    if (agent.link_frames) {
      /* (This step writes the landing as a carried step: its corridor is the other frame's,
       * still where it took off.) */
      agent.link_frames = false;
      agent.link_frames_landing = true;
      agent.frame = agent.link_to_frame;
      agent.frame_known = true;
      agent.corridor_ready = false;
      agent.boundary_ready = false;
      agent.offset_known = false;
      agent.needs_search = true;
    }
  }
}

static MT_Vector3 flock_pull(const KX_NavAgent &agent, const std::vector<Body> &bodies);
static MT_Vector3 round_standing_group(KX_NavAgent &agent,
                                       const MT_Vector3 &dir,
                                       float look,
                                       const std::vector<Body> &bodies,
                                       float dt);
static MT_Vector3 keep_right(const KX_NavAgent &agent,
                             const MT_Vector3 &desired,
                             const std::vector<Body> &bodies,
                             const BodyGrid &grid,
                             float level);

void KX_Navigation::SteerAgents(const float dt, LN_JobPool *pool)
{
  (void)dt;
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    agent.desired = MT_Vector3(0.0f, 0.0f, 0.0f);
    agent.remaining = 0.0f;
    agent.next_corner = agent.has_goal ? agent.goal : agent.position;
    agent.link_ended = false;
    if (!agent.requested || !agent.has_goal || agent.arrived || agent.on_link) {
      agent.round_side = 0;
      agent.round_flipped = false;
      agent.head_on_time = 0.0f;
    }
    if (!agent.requested || !agent.has_goal) {
      return;
    }
    const float speed = std::max(agent.request.speed, 0.0f);
    const bool flock = agent.request.mode == KX_NavRequest::Mode::Flock;
    MT_Vector3 desired(0.0f, 0.0f, 0.0f);
    /* (A link between frames starts in PrepareAgents: this is its first step.) */
    agent.link_started = agent.on_link && agent.link_frames && agent.link_time == 0.0f;
    if (agent.on_link) {
      StepLink(agent, dt);
      return;
    }
    if (agent.waiting) {
      /* At a closed link to another frame: it stands and waits. */
      agent.next_corner = agent.goal;
      return;
    }
    if (agent.arrived) {
      agent.remaining = 0.0f;
    }
    else if (flock && !agent.request.goal) {
      /* Only the flock's own pull below. */
    }
    else if (agent.air != nullptr) {
      SteerAir(agent);
      desired = agent.desired;
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
        /* A closed Wait link (2026-11-20, a door): it stands at its start until it opens (Get
         * Navigation Info's Waiting), or takes another way after the link's Give Up After. */
        if (HasClosedLinks()) {
          const dtOffMeshConnection *connection =
              agent.navmesh->Mesh()->getOffMeshConnectionByRef(dtPolyRef(polys[ncorners - 1]));
          const uint32_t navmesh_id = connection != nullptr ? connection->userId : 0;
          const uint32_t id = agent.filter.OwnLinkId(navmesh_id) & ~kNavLinkReverse;
          if (const KX_NavClosedLink *closed = id != 0 ? FindClosedLink(id) : nullptr) {
            if (agent.filter.ClosedToIt(navmesh_id)) {
              /* Closed to it (Go Round, or a wait it gave up), still on its way before the new
               * search: it never crosses; it stands until its way is searched again. */
              agent.needs_search = true;
              agent.next_corner = agent.navmesh->ToWorld(&corners[(ncorners - 1) * 3]);
              agent.desired = MT_Vector3(0.0f, 0.0f, 0.0f);
              return;
            }
            if (agent.wait_link != id) {
              agent.wait_link = id;
              agent.wait_seconds = 0.0f;
            }
            agent.wait_seconds += dt;
            if (closed->wait_time > 0.0f && agent.wait_seconds >= closed->wait_time) {
              /* Given up: another way (the link closed to it until it closes again). */
              agent.filter.gave_up.emplace_back(id, closed->generation);
              agent.gave_up = true;
              agent.needs_search = true;
              agent.wait_link = 0;
              agent.wait_seconds = 0.0f;
              agent.next_corner = agent.navmesh->ToWorld(&corners[(ncorners - 1) * 3]);
              agent.desired = MT_Vector3(0.0f, 0.0f, 0.0f);
              return;
            }
            else {
              /* Waiting at the link's end it stands at (a link both ways is taken from
               * either). */
              agent.waiting = true;
              agent.waiting_at = nullptr;
              agent.next_corner = agent.navmesh->ToWorld(&corners[(ncorners - 1) * 3]);
              auto found = m_linkIndexById.find(id);
              if (found != m_linkIndexById.end() && found->second < m_links.size()) {
                const KX_NavLink &link = m_links[found->second];
                /* (A link on a vehicle: its ends where its objects are now.) */
                const MT_Vector3 start = link.object ? link.object->NodeGetWorldPosition() :
                                                       link.start;
                const MT_Vector3 end = link.end_object ? link.end_object->NodeGetWorldPosition() :
                                                         link.end;
                agent.waiting_at = (agent.next_corner - start).length2() <=
                                           (agent.next_corner - end).length2() ?
                                       link.object :
                                       link.end_object;
              }
              agent.remaining = dtVdist2D(pos, &corners[(ncorners - 1) * 3]);
              agent.desired = MT_Vector3(0.0f, 0.0f, 0.0f);
              return;
            }
          }
        }
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
        /* Avoidance Dense (2026-11-19): aim a body's radius off the corner, on the turn's outer
         * side (the corner is the wall's tip: a packed crowd aiming at it pressed itself into the
         * wall beside a door, while the door's middle stayed free). */
        MT_Vector3 aim = agent.next_corner;
        if (ncorners >= 2 &&
            (flags[0] & (DT_STRAIGHTPATH_END | DT_STRAIGHTPATH_OFFMESH_CONNECTION)) == 0)
        {
          const MT_Vector3 after = agent.navmesh->ToWorld(&corners[3]);
          MT_Vector3 in = flat(agent.next_corner - world_pos), out = flat(after - agent.next_corner);
          if (in.length2() > 1e-8f && out.length2() > 1e-8f) {
            in.normalize();
            out.normalize();
            const MT_Vector3 outer = in - out;
            const float turn = float(outer.length());
            if (turn > 1e-3f) {
              /* Polite (kCornerAim): a little off the corner, more for a sharper turn. */
              const float off = agent.request.dense ?
                                    agent.radius :
                                    std::max(kCornerAim * agent.radius * turn, kCornerAimLeast);
              aim = agent.next_corner + outer / turn * off;
            }
          }
        }
        MT_Vector3 dir = flat(aim - world_pos);
        const float dist = float(dir.length());
        if (dist > 1e-4f) {
          dir /= dist;
        }
        /* A polite agent stuck behind a group standing in its way goes round it (far from its
         * goal only: a horde round the player, a queue at a counter wait). */
        const float crowd_reach = agent.request.arrive_distance +
                                  std::max(2.0f, agent.radius * 6.0f);
        if (!agent.request.dense && agent.request.avoid && dist > 1e-4f &&
            remaining > crowd_reach + kRoundGoalMargin)
        {
          dir = round_standing_group(agent, dir, dist, m_step->bodies, dt);
        }
        else {
          agent.round_side = 0;
          agent.round_flipped = false;
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
    if (agent.request.avoid && !agent.arrived && agent.navmesh != nullptr) {
      desired = keep_right(agent, desired, m_step->bodies, m_step->grid, m_settings.level_height);
    }
    /* Pressing face to face against one heading the other way: hardly moving, or already
     * shrinking (slipping past, it goes on shrinking while one is close in front). */
    bool pressed = false;
    const MT_Vector3 heading = flat(desired);
    if (agent.request.avoid && heading.length2() > 1e-6f &&
        (float(flat(agent.velocity).length()) < 0.5f * speed ||
         agent.head_on_time > kHeadOnAfter))
    {
      const MT_Vector3 way = heading.normalized();
      for (const uint32_t b : agent.neighbours) {
        const Body &other = m_step->bodies[b];
        const MT_Vector3 rel = flat(other.position - agent.position);
        if (other.agent != UINT32_MAX && float(other.heading.dot(way)) < -0.5f &&
            float(rel.dot(way)) > 0.0f &&
            float(rel.length2()) < (agent.radius + other.radius) * (agent.radius + other.radius) *
                                       1.5625f)
        {
          pressed = true;
          break;
        }
      }
    }
    agent.head_on_time = pressed ? agent.head_on_time + dt :
                                   std::max(agent.head_on_time - 2.0f * dt, 0.0f);
    agent.desired = desired;
  });
}

static MT_Vector3 round_standing_group(KX_NavAgent &agent,
                                       const MT_Vector3 &dir,
                                       const float look,
                                       const std::vector<Body> &bodies,
                                       const float dt)
{
  /* Inside one navmesh polygon a path is a straight line, so a search cannot lead round a group
   * standing in the open, and ORCA, its way straight into the group, finds no side to slip by:
   * the agent stood there for good. Once it stood kRoundAfter seconds blocked by standing agents
   * it turns to the side fewer of them are on, by the least of kRoundAngles that clears them,
   * until its straight way is clear; blocked again there (a wall), it tries the other side once
   * (Bug algorithms' boundary following, on the step's neighbours only). */
  auto blocked = [&](const MT_Vector3 &d, const float reach, const float margin) {
    for (const uint32_t b : agent.neighbours) {
      const Body &other = bodies[b];
      if (other.agent == UINT32_MAX || !other.standing) {
        continue;
      }
      const MT_Vector3 rel = flat(other.position - agent.position);
      const float along = float(rel.dot(d));
      if (along <= 0.0f || along > reach + other.radius) {
        continue;
      }
      const float side = std::fabs(float(rel[0] * d[1] - rel[1] * d[0]));
      if (side < agent.radius + other.radius + margin) {
        return true;
      }
    }
    return false;
  };
  const float reach = std::min(look, kRoundLook);
  if (agent.round_side == 0) {
    agent.round_flipped = false;
    if (agent.stall_time < kRoundAfter || !blocked(dir, reach, 0.0f)) {
      return dir;
    }
    /* Away from where more of them stand (left of the way: positive). */
    float left = 0.0f;
    for (const uint32_t b : agent.neighbours) {
      const Body &other = bodies[b];
      if (other.agent == UINT32_MAX || !other.standing) {
        continue;
      }
      const MT_Vector3 rel = flat(other.position - agent.position);
      if (rel.dot(dir) > 0.0f) {
        left += float(dir[0] * rel[1] - dir[1] * rel[0]);
      }
    }
    agent.round_side = left > 0.0f ? -1 : 1;
    agent.round_time = 0.0f;
  }
  agent.round_time += dt;
  /* Done: its way is clear (a margin: no flicker at the group's edge); or long enough. */
  if (!blocked(dir, reach, kRoundClearMargin) || agent.round_time > kRoundMaxTime) {
    agent.round_side = 0;
    return dir;
  }
  if (agent.stall_time >= kRoundAfter * 2.0f && agent.round_time > kRoundAfter * 2.0f &&
      !agent.round_flipped)
  {
    agent.round_side = int8_t(-agent.round_side);
    agent.round_flipped = true;
    agent.round_time = 0.0f;
  }
  const float sign = float(agent.round_side);
  MT_Vector3 turned = dir;
  for (const float degrees : kRoundAngles) {
    const float a = sign * degrees * float(M_PI) / 180.0f;
    const float c = std::cos(a), s = std::sin(a);
    turned = MT_Vector3(dir[0] * c - dir[1] * s, dir[0] * s + dir[1] * c, 0.0f);
    if (!blocked(turned, kRoundLook, 0.0f)) {
      break;
    }
  }
  return turned;
}

static MT_Vector3 keep_right(const KX_NavAgent &agent,
                             const MT_Vector3 &desired,
                             const std::vector<Body> &bodies,
                             const BodyGrid &grid,
                             const float level)
{
  /* Two crowds meeting head-on (four abreast in a corridor four wide) stood face to face for
   * good, Polite and Dense alike: nobody had room to step aside. People keep to one side and
   * lanes form before they meet (Detour's avoidance has a side bias for it): agents coming the
   * other way on its line within kKeepRightAhead make it drift right, the nearer the more. A
   * flow one way only sees nobody coming: unchanged. (The bodies near the way ahead from the
   * neighbour grid: an agent's nearest neighbours in a crowd are its own.) */
  const float speed = float(flat(desired).length());
  if (speed < 1e-3f) {
    return desired;
  }
  const MT_Vector3 dir = flat(desired) / speed;
  const MT_Vector3 right(dir[1], -dir[0], 0.0f);
  /* Only the grid cells along the strip ahead (its length, as wide as anyone on its line may
   * be), nearest first, and only those where somebody heads against it (more than 90 degrees
   * off its way: their eighths of the circle 2..6 from its own): in a crowd going one way it
   * looks at no body at all (10,000 meeting head-on in the open spent 35 ms a step on a square
   * of grid cells). It stops once the pull is the most it gives. */
  const int own = heading_octant(float(dir[0]), float(dir[1]));
  uint8_t against = 0;
  for (int k = 2; k <= 6; k++) {
    against |= uint8_t(1u << ((own + k) & 7));
  }
  const float half_width = agent.radius + grid.max_radius + kKeepRightLine;
  /* Rows of points 0.7 of a cell apart along it, across it at its edges and middle (finer where
   * the strip is wider than a cell); a cell seen again is one of the last rows'. */
  const int lanes = std::max(3, int(std::ceil(2.0f * half_width / (grid.cell * 0.7f))) + 1);
  uint64_t strip[96];
  int count = 0;
  const float step = grid.cell * 0.7f;
  for (float along = 0.0f; along <= kKeepRightAhead + step * 0.5f && count < 96; along += step) {
    for (int lane = 0; lane < lanes; lane++) {
      const float across = -half_width + 2.0f * half_width * float(lane) / float(lanes - 1);
      const MT_Vector3 p = agent.position + dir * std::min(along, kKeepRightAhead) +
                           right * across;
      const uint64_t key = BodyGrid::Key(int(std::floor(p[0] / grid.cell)),
                                         int(std::floor(p[1] / grid.cell)));
      bool seen = false;
      for (int i = std::max(0, count - 3 * lanes); i < count && !seen; i++) {
        seen = strip[i] == key;
      }
      if (!seen && count < 96) {
        strip[count++] = key;
      }
    }
  }
  const float most = kKeepRightMost / 0.5f;
  float pull = 0.0f;
  for (int i = 0; i < count && pull < most; i++) {
    auto it = grid.cells.find(strip[i]);
    if (it == grid.cells.end() || (it->second.headings & against) == 0) {
      continue;
    }
    for (const uint32_t b : it->second.list) {
      const Body &other = bodies[b];
      if (other.agent == UINT32_MAX || other.standing ||
          std::fabs(float(other.position[2] - agent.position[2])) >
              (level > 0.0f ? level : std::max(agent.height, other.height)))
      {
        continue;
      }
      const MT_Vector3 rel = flat(other.position - agent.position);
      const float along = float(rel.dot(dir));
      if (along <= 0.0f || along > kKeepRightAhead) {
        continue;
      }
      /* Heading the other way (more against it than across): its wish, not how it moves (in a
       * jam at a door those pushed back move backward and looked like a crowd coming the other
       * way: the flow drifted aside, Dense through a 2.4 m door fell from 5.4 to 4.7 a
       * second). */
      if (other.heading.length2() < 0.5f || float(other.heading.dot(dir)) > -0.5f) {
        continue;
      }
      const float side = std::fabs(float(rel.dot(right)));
      if (side > agent.radius + other.radius + kKeepRightLine) {
        continue;
      }
      pull += 1.0f - along / kKeepRightAhead;
    }
  }
  if (pull <= 0.0f) {
    return desired;
  }
  MT_Vector3 turned = dir + right * std::min(pull * 0.5f, kKeepRightMost);
  turned.normalize();
  return turned * speed + MT_Vector3(0.0f, 0.0f, float(desired[2]));
}

static MT_Vector3 flock_pull(const KX_NavAgent &agent, const std::vector<Body> &bodies)
{
  /* Boids among the agents of its group within reach: apart from the near ones, the same way
   * as all, toward their middle. */
  const KX_NavRequest &request = agent.request;
  const float speed = std::max(request.speed, 0.0f);
  const float reach = std::max(request.radius, agent.radius * 4.0f);
  /* (Flying, or crawling: in space, a flock of birds holds together in height too; walking or
   * floating: along the ground.) */
  const bool in_space = (agent.flies && !agent.floating) || agent.crawls;
  const auto plane = [in_space](const MT_Vector3 &v) { return in_space ? v : flat(v); };
  MT_Vector3 apart(0.0f, 0.0f, 0.0f), heading(0.0f, 0.0f, 0.0f), middle(0.0f, 0.0f, 0.0f);
  int count = 0;
  for (const uint32_t b : agent.neighbours) {
    const Body &other = bodies[b];
    if (other.agent == UINT32_MAX || other.group != request.group) {
      continue;
    }
    const MT_Vector3 offset = plane(agent.position - other.position);
    const float dist = float(offset.length());
    if (dist > reach || dist < 1e-5f) {
      continue;
    }
    const float personal = (agent.radius + other.radius) * 2.0f;
    if (dist < personal) {
      apart += offset / dist * ((personal - dist) / personal);
    }
    heading += plane(other.velocity);
    middle += other.position;
    count++;
  }
  if (count == 0) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  heading /= float(count);
  middle /= float(count);
  MT_Vector3 together = plane(middle - agent.position);
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
  for (std::unique_ptr<KX_NavVehicle> &vehicle : m_vehicles) {
    vehicle->obstacles.clear();
  }
  std::unordered_map<const KX_GameObject *, MT_Vector3> seen;
  auto add = [&](KX_GameObject *object, const bool physics) {
    bool moved = false;
    AddObstacleBody(object, physics, moved);
    seen[object] = object->NodeGetWorldPosition();
  };
  /* Avoid Physics Objects (2026-11-19): the physics objects big enough, in the world, no NPC and
   * not marked already (a marked one is avoided as marked). */
  auto add_physics = [&](KX_GameObject *object) {
    if (IsPhysicsObstacle(object)) {
      add(object, true);
    }
  };
  if (m_scene != nullptr) {
    if (m_obstacleCandidatesDirty ||
        m_obstacleListGeneration != m_scene->GetObjectListGeneration())
    {
      m_obstacleCandidates.clear();
      m_physicsObstacleCandidates.clear();
      for (KX_GameObject *object : *m_scene->GetObjectList()) {
        if (IsObstacle(object)) {
          m_obstacleCandidates.push_back(object);
        }
        else if (m_settings.avoid_physics) {
          /* Whether it moves as a body, its size: at each step (cheap for the few kept). */
          const PHY_IPhysicsController *controller = object->GetPhysicsController();
          if (controller != nullptr &&
              (const_cast<PHY_IPhysicsController *>(controller)->IsDynamic() ||
               controller->IsCharacter()))
          {
            m_physicsObstacleCandidates.push_back(object);
          }
        }
      }
      m_obstacleListGeneration = m_scene->GetObjectListGeneration();
      m_obstacleCandidatesDirty = false;
    }
    for (KX_GameObject *object : m_obstacleCandidates) {
      if (FindAgent(object) == nullptr) {
        add(object, false);
      }
    }
    for (KX_GameObject *object : m_physicsObstacleCandidates) {
      add_physics(object);
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
      add(object, false);
    }
    /* Tests: the physics objects they name, in their order. */
    for (KX_GameObject *object : m_physicsObstacleCandidates) {
      add_physics(object);
    }
  }
  m_step->obstacle_positions.swap(seen);
}

void KX_Navigation::SetPhysicsObjectsForTests(std::vector<KX_GameObject *> objects)
{
  m_physicsObstacleCandidates = std::move(objects);
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
    /* (A flyer: its sphere.) */
    body.radius = agent.flies && agent.air != nullptr && !agent.floating ? agent.air_radius :
                                                                            agent.radius;
    body.height = agent.height;
    body.agent = i;
    body.avoids = agent.requested && agent.request.avoid && (!agent.arrived || agent.chase_place);
    body.priority = agent.request.priority;
    body.group = agent.request.group;
    body.dense = agent.requested && agent.request.avoid && agent.request.dense;
    body.weight = std::exp2((float(agent.request.priority) - 50.0f) / 12.5f);
    body.object = agent.object;
    body.frame = agent.frame;
    body.frame_velocity = agent.frame_velocity;
    if (!m_frameLinks.empty()) {
      for (const KX_NavFrameLink &link : m_frameLinks) {
        if (link.open && ((agent.position - link.start).length2() < kGangwayCrowd * kGangwayCrowd ||
                          (agent.position - link.end).length2() < kGangwayCrowd * kGangwayCrowd))
        {
          body.near_frame_link = true;
          break;
        }
      }
    }
    body.standing = agent.standing;
    const MT_Vector3 wish = flat(agent.desired);
    if (wish.length2() > 1e-6f) {
      body.heading = wish.normalized();
    }
    bodies.push_back(body);
  }
  bodies.insert(bodies.end(), m_step->obstacles.begin(), m_step->obstacles.end());
  /* Vehicles' hulls for flyers and swimmers (E6, 2026-11-20): spheres along each one's longer
   * side (its shape's bounds), moving with it (a boat avoided by fish and gulls). */
  const bool any_flyer = std::any_of(m_agents.begin(),
                                     m_agents.end(),
                                     [](const std::unique_ptr<KX_NavAgent> &a) {
                                       return a->flies && a->air != nullptr;
                                     });
  if (any_flyer) {
    for (const std::unique_ptr<KX_NavVehicle> &vehicle : m_vehicles) {
      if (vehicle->type == nullptr) {
        continue;
      }
      const MT_Vector3 &lo = vehicle->type->bounds_min, &hi = vehicle->type->bounds_max;
      const MT_Vector3 half = (hi - lo) * 0.5f, middle = (hi + lo) * 0.5f;
      const HullSpheres spheres = hull_spheres(lo, hi);
      const int along = spheres.along;
      const float radius = spheres.radius, spacing = spheres.spacing;
      const float length = float(half[along]) * 2.0f;
      const int count = spheres.count;
      const float step_along = count > 1 ? (length - spacing) / float(count - 1) : 0.0f;
      const MT_Transform pose = pose_of(vehicle->object);
      for (int k = 0; k < count; k++) {
        MT_Vector3 local = middle;
        if (count > 1) {
          local[along] = float(lo[along]) + spacing * 0.5f + step_along * float(k);
        }
        Body hull;
        hull.position = pose(local);
        hull.velocity = vehicle->linear_velocity;
        hull.radius = count > 1 ? radius :
                                  std::sqrt(radius * radius + float(half[along] * half[along]));
        hull.height = float(half[2]) * 2.0f;
        hull.object = vehicle->object;
        hull.air_only = true;
        bodies.push_back(hull);
      }
    }
  }
  m_step->grid.Build(bodies);
}

void KX_Navigation::AddObstacleBody(KX_GameObject *object, const bool physics, bool &r_moved)
{
  const MT_Vector3 position = object->NodeGetWorldPosition();
  const auto &positions = m_step->obstacle_positions;
  auto previous = positions.find(object);
  r_moved = previous != positions.end() && (position - previous->second).length2() > 1e-6f;
  const MT_Vector3 velocity = previous != positions.end() ?
                                  (position - previous->second) /
                                      std::max(m_step->last_dt, 1e-4f) :
                                  MT_Vector3(0.0f, 0.0f, 0.0f);
  /* Its outline (2026-11-20, plan item 5): its collision shape (else its mesh), Keep Away round
   * it (the panel's old Radius, obstacleRad, is the logic bricks' and no longer read). */
  const ObstacleShape shape = obstacle_shape(object, m_settings.agent_height);
  const MT_Vector3 center = shape.center, half = shape.half;
  const float yaw = shape.yaw;
  const float height = std::max(float(half[2]) * 2.0f, 0.1f);
  /* Its avoidance bodies: one circle round it, or a row of them along a box more than twice as
   * long as wide (a cart, a log: avoided along its length, not as one circle as wide as it is
   * long), each wider than the box's half width so that their union holds its corners. */
  const size_t first_body = m_step->obstacles.size();
  const float along = float(std::max(half[0], half[1]));
  const float across = float(std::min(half[0], half[1]));
  if (!shape.round && along > 2.0f * across && across > 1e-3f) {
    const int axis = half[0] >= half[1] ? 0 : 1;
    const float radius = 1.25f * across;
    const float spacing = 2.0f * std::sqrt(radius * radius - across * across);
    const float length = along * 2.0f;
    const int count = std::max(2, int(std::ceil(length / spacing)));
    const float step = (length - spacing) / float(count - 1);
    const float c = std::cos(yaw), sn = std::sin(yaw);
    const MT_Vector3 direction = axis == 0 ? MT_Vector3(c, sn, 0.0f) : MT_Vector3(-sn, c, 0.0f);
    for (int k = 0; k < count; k++) {
      Body body;
      body.position = center + direction * (-along + spacing * 0.5f + step * float(k));
      body.velocity = velocity;
      body.radius = std::max(radius, 0.05f);
      body.height = height;
      body.object = object;
      m_step->obstacles.push_back(body);
    }
  }
  else {
    Body body;
    body.position = center;
    body.velocity = velocity;
    body.radius = std::max(along, 0.05f);
    body.height = height;
    body.object = object;
    m_step->obstacles.push_back(body);
  }
  /* A still obstacle is carved into the scene's navmesh; a moving one only avoided (Unity's
   * rule: carving a moving obstacle at every step would make every path search again). A
   * physics object only while Jolt keeps it asleep (a ball at the top of its bounce stands
   * still for a step), a Character never (it never sleeps; a standing player is no hole). */
  bool carve = !r_moved;
  if (physics) {
    PHY_IPhysicsController *controller = object->GetPhysicsController();
    carve = controller != nullptr && !controller->IsCharacter() && controller->IsAsleep();
  }
  /* On a vehicle (E3, 2026-11-20: parented to it, or standing on its deck): carved into the
   * vehicle's own navmesh in its space, while it stands still there (it moves with the deck). */
  if (!m_vehicles.empty()) {
    KX_GameObject *frame = VehicleOf(object);
    auto seen = m_step->obstacle_local.find(object);
    if (frame == nullptr) {
      /* Standing on a deck: the navmesh under it (its own carved hole hides that once carved:
       * then still on the deck it stood on while within the deck's bounds). */
      bool found = false;
      KX_NavAgentSize size;
      KX_GameObject *under = FrameAt(
          MT_Vector3(center[0], center[1], center[2] - half[2] + 0.1f), size, found);
      frame = found ? under : nullptr;
      if (frame == nullptr && seen != m_step->obstacle_local.end()) {
        KX_GameObject *before = const_cast<KX_GameObject *>(seen->second.first);
        const KX_NavVehicle *was = FindVehicleRecord(before);
        if (was != nullptr && was->type != nullptr) {
          MT_Transform inverse;
          inverse.invert(pose_of(before));
          const MT_Vector3 local = inverse(center);
          const MT_Vector3 &lo = was->type->bounds_min, &hi = was->type->bounds_max;
          if (local[0] >= lo[0] && local[0] <= hi[0] && local[1] >= lo[1] &&
              local[1] <= hi[1] && local[2] >= lo[2] - 0.5f && local[2] <= hi[2] + 3.0f)
          {
            frame = before;
          }
        }
      }
    }
    if (KX_NavVehicle *vehicle = frame != nullptr ? FindVehicleRecord(frame) : nullptr) {
      MT_Transform inverse;
      inverse.invert(pose_of(frame));
      const MT_Matrix3x3 &turn = frame->NodeGetWorldOrientation();
      /* Its place and turn in the deck's space (the turn as a point a metre along it: still means
       * neither moved nor turned there). */
      const MT_Vector3 local = inverse(center);
      const float local_yaw = yaw - std::atan2(float(turn[1][0]), float(turn[0][0]));
      const MT_Vector3 local_ahead = local + MT_Vector3(std::cos(local_yaw), std::sin(local_yaw), 0.0f);
      auto ahead = m_step->obstacle_local_ahead.find(object);
      const bool still = seen != m_step->obstacle_local.end() && seen->second.first == frame &&
                         (local - seen->second.second).length2() <= 1e-6f &&
                         ahead != m_step->obstacle_local_ahead.end() &&
                         (local_ahead - ahead->second).length2() <= 1e-4f;
      /* Its avoidance bodies: on this deck, their velocity over it (a crate parented to a
       * turning ship stands still there; its world velocity made the NPCs beside it stop). */
      const MT_Vector3 deck_velocity = seen != m_step->obstacle_local.end() &&
                                               seen->second.first == frame ?
                                           turn * ((local - seen->second.second) /
                                                   std::max(m_step->last_dt, 1e-4f)) :
                                           MT_Vector3(0.0f, 0.0f, 0.0f);
      for (size_t k = first_body; k < m_step->obstacles.size(); k++) {
        Body &on_deck_body = m_step->obstacles[k];
        on_deck_body.frame = frame;
        const MT_Vector3 world_velocity = on_deck_body.velocity;
        on_deck_body.velocity = deck_velocity;
        on_deck_body.frame_velocity = world_velocity - deck_velocity;
      }
      m_step->obstacle_local[object] = {frame, local};
      m_step->obstacle_local_ahead[object] = local_ahead;
      const bool carve_here = physics ? carve : still;
      if (carve_here) {
        KX_NavCarveObstacle on_deck;
        on_deck.object = object;
        on_deck.center = local;
        on_deck.half_size = half;
        on_deck.yaw = shape.round ? 0.0f : local_yaw;
        on_deck.round = shape.round;
        vehicle->obstacles.push_back(on_deck);
      }
      return;
    }
  }
  if (carve) {
    KX_NavCarveObstacle carve;
    carve.object = object;
    carve.center = center;
    carve.half_size = half;
    carve.yaw = yaw;
    carve.round = shape.round;
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
        if (other.agent == uint32_t(index) ||
            (other.air_only && (!agent.flies || other.object == agent.through_hull)))
        {
          return;
        }
        /* (Agents on other frames only near a link between frames, both: E4, 2026-11-20.) */
        if (other.agent != UINT32_MAX && other.frame != agent.frame &&
            !(other.near_frame_link && bodies[index].near_frame_link))
        {
          return;
        }
        const float dz = std::fabs(float(other.position[2] - agent.position[2]));
        /* (A flyer's or a crawler's neighbours are those near in space: above and below too.) */
        const bool in_space = agent.flies || agent.crawls;
        const float max_dz = in_space ? range :
                                        (level > 0.0f ? level :
                                                        std::max(agent.height, other.height));
        if (dz > max_dz) {
          return;
        }
        const float d = in_space ? float((other.position - agent.position).length()) :
                                      distance_2d(other.position, agent.position);
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

/** The navmesh's walls near `agent` (Detour's local boundary), refreshed as it moves (its own
 * corridor and query: any thread). False: no navmesh under it. */
/** A navmesh wall's way in (the plane, the world; unit, zero for a point): the side DetourCrowd
 * takes for its inside (dtTriArea2D of a point and the wall not below 0), from the wall's own
 * winding, never from where an agent stands (one on the line got it wrong). */
static MT_Vector3 wall_inward(const KX_NavMesh &navmesh, const float *a, const float *b)
{
  const float dx = b[0] - a[0], dz = b[2] - a[2];
  const float length = std::sqrt(dx * dx + dz * dz);
  if (length < 1e-6f) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  const float mid[3] = {(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f};
  float probe[3] = {mid[0] - dz / length * 0.05f, mid[1], mid[2] + dx / length * 0.05f};
  if (dtTriArea2D(probe, a, b) < 0.0f) {
    probe[0] = mid[0] + dz / length * 0.05f;
    probe[2] = mid[2] - dx / length * 0.05f;
  }
  const MT_Vector3 in = flat(navmesh.ToWorld(probe) - navmesh.ToWorld(mid));
  return in.length2() > 1e-12f ? in.normalized() : MT_Vector3(0.0f, 0.0f, 0.0f);
}

static bool update_boundary(KX_NavAgent &agent)
{
  if (agent.navmesh == nullptr || agent.corridor.getFirstPoly() == 0) {
    return false;
  }
  dtNavMeshQuery *query = agent.navmesh->ThreadQuery();
  if (query == nullptr) {
    return false;
  }
  const float *pos = agent.corridor.getPos();
  const float range = std::clamp(agent.radius * 6.0f + 1.0f, 2.0f, 8.0f);
  if (!agent.boundary_ready ||
      dtVdist2DSqr(pos, agent.boundary_center) > range * range * 0.0625f ||
      !agent.boundary.isValid(query, &agent.filter))
  {
    agent.boundary.update(agent.corridor.getFirstPoly(), pos, range, query, &agent.filter);
    dtVcopy(agent.boundary_center, pos);
    agent.boundary_ready = true;
  }
  return true;
}

/** Each wall end where the walkable side wraps round (a corner a way turns round): another wall
 * of the list meets it there and leads off to this wall's back (a wall going on into the walkable
 * side makes an inner corner). An end no other wall of the list meets stays unknown (Detour's
 * local boundary keeps the nearest walls only): not a corner. */
static void mark_convex_wall_ends(std::vector<kx_orca::Wall> &walls)
{
  constexpr float kSame = 1e-3f;
  for (kx_orca::Wall &wall : walls) {
    if (kx_orca::abs_sq(wall.inward) < 0.5f) {
      continue;
    }
    for (int end = 0; end < 2; end++) {
      const kx_orca::Vec2 at = end == 0 ? wall.a : wall.b;
      bool convex = false, inner = false;
      for (const kx_orca::Wall &other : walls) {
        if (&other == &wall) {
          continue;
        }
        kx_orca::Vec2 away;
        if (kx_orca::abs_sq(other.a - at) < kSame * kSame) {
          away = other.b - at;
        }
        else if (kx_orca::abs_sq(other.b - at) < kSame * kSame) {
          away = other.a - at;
        }
        else {
          continue;
        }
        const float side = kx_orca::dot(away, wall.inward);
        if (side < -1e-4f) {
          convex = true;
        }
        else {
          inner = true;
        }
      }
      (end == 0 ? wall.a_convex : wall.b_convex) = convex && !inner;
    }
  }
}

void KX_Navigation::AvoidAgents(const float dt, LN_JobPool *pool)
{
  const kx_orca::Params params{2.0f, 0.5f, dt, 0.0f, kWallCornerRelease};
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    agent.new_velocity = agent.desired;
    agent.making_way = false;
    /* A chaser standing at its place round its target makes way (plan item 10, 2026-11-20): one
     * walking to a place on the far side was shut in for good by the ring standing still. */
    const bool makes_way = agent.requested && agent.arrived && agent.chase_place &&
                           agent.request.avoid && !agent.request.dense && agent.air == nullptr &&
                           !agent.on_link && !agent.link_started;
    if (!agent.requested || (agent.arrived && !makes_way)) {
      agent.new_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
      return;
    }
    if (!agent.request.avoid || agent.on_link || agent.link_started) {
      return;
    }
    if (agent.air != nullptr) {
      /* Flying or swimming: in three dimensions. */
      AvoidAir(agent, dt);
      return;
    }
    if (agent.request.dense) {
      /* Avoidance Dense: SolveDense, next. */
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
      /* (One on another frame, near a link between frames, E4: its velocity over this agent's
       * deck.) */
      /* (Velocities are over each body's own deck, `frame_velocity` the deck's own there: a crate
       * on this agent's deck stands still for it however the ship turns; 2026-11-20.) */
      const MT_Vector3 v = other.frame == agent.frame ?
                               other.velocity :
                               other.velocity + other.frame_velocity - agent.frame_velocity;
      n.velocity = kx_orca::Vec2(float(v[0]), float(v[1]));
      n.radius = other.radius;
      /* Stuck face to face with one heading the other way: both shrink for each other. */
      if (other.agent != UINT32_MAX) {
        const float size = head_on_size(
            m_step->bodies[index].heading,
            other.heading,
            flat(other.position - agent.position),
            std::max(agent.head_on_time, m_agents[other.agent]->head_on_time));
        if (size < 1.0f) {
          /* (ORCA reads only the two radii's sum.) */
          n.radius = (agent.radius + other.radius) * size - agent.radius;
        }
      }
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
    /* The navmesh's walls nearby (Detour's local boundary, refreshed as the agent moves), each
     * with its way in and its corners (kx_orca::build_lines leaves out a wall the agent stands
     * behind, and lets go near a corner: 2026-11-19, measured in KX_Navigation.
     * AnNpcRoundsWallCornersWithoutStopping; an older try at leaving such walls out, whose way in
     * came from the agent's side, slowed a runner through pillars by a quarter). */
    if (update_boundary(agent)) {
      for (int i = 0; i < agent.boundary.getSegmentCount(); i++) {
        const float *s = agent.boundary.getSegment(i);
        const MT_Vector3 a = agent.navmesh->ToWorld(&s[0]);
        const MT_Vector3 b = agent.navmesh->ToWorld(&s[3]);
        const MT_Vector3 in = wall_inward(*agent.navmesh, &s[0], &s[3]);
        walls.push_back({kx_orca::Vec2(float(a[0]), float(a[1])),
                         kx_orca::Vec2(float(b[0]), float(b[1])),
                         kx_orca::Vec2(float(in[0]), float(in[1]))});
      }
      mark_convex_wall_ends(walls);
    }
    const kx_orca::Vec2 velocity(float(agent.commanded[0]), float(agent.commanded[1]));
    /* A tiny turn of its own (from its number): agents meeting exactly head-on would otherwise
     * both wait (ORCA's symmetric stand-off; RVO2's examples perturb the same way). */
    const float turn = (float(mix64(agent.order + 1) >> 40) * (1.0f / 16777216.0f) - 0.5f) * 0.08f;
    const float ct = std::cos(turn), st = std::sin(turn);
    /* (Making way: no wish of its own, or, caught its target away from its place, toward its
     * place at half its speed.) */
    kx_orca::Vec2 settle(0.0f, 0.0f);
    if (makes_way && !agent.chase_settled) {
      const MT_Vector3 d = flat(agent.chase_settle - agent.position);
      const float dist = float(d.length());
      if (dist > 1e-3f) {
        const float pace = std::min(0.5f * agent.request.speed, dist / std::max(dt, 1e-4f));
        settle = kx_orca::Vec2(float(d[0]) * pace / dist, float(d[1]) * pace / dist);
      }
    }
    const kx_orca::Vec2 preferred =
        makes_way ? settle :
                    kx_orca::Vec2(float(agent.desired[0]) * ct - float(agent.desired[1]) * st,
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
      chosen = makes_way ? MT_Vector3(0.0f, 0.0f, 0.0f) : agent.desired;
    }
    if (makes_way) {
      chosen[2] = 0.0f;
      agent.making_way = std::hypot(float(chosen[0]), float(chosen[1])) > 0.05f;
      if (!agent.making_way) {
        chosen = MT_Vector3(0.0f, 0.0f, 0.0f);
      }
    }
    agent.new_velocity = chosen;
  });
}

/* -------------------------------------------------------------------- */
/** \name Packed crowds (phase 7, 2026-11-19)
 * \{ */

namespace {

/** Avoidance Dense: rounds of the position solve a step (two sub-steps of four, Weiss et al.'s
 * "few iterations, several sub-steps"), their over-relaxation, and the look-ahead: agents about
 * to meet within kDenseLookAhead seconds steer sideways (only sideways: they slide past instead
 * of pushing back). */
constexpr int kDenseRounds = 8;
constexpr float kDenseOverRelaxation = 1.2f;
constexpr float kDenseLookAhead = 1.0f;
constexpr float kDenseSteer = 0.5f;
/** A polite agent (or one not avoiding) is this much heavier for a Dense one: it makes way. */
constexpr float kDensePoliteWeight = 4.0f;
/** Stuck pressing for this long (seconds), a Dense agent squeezes: its body counts this much
 * smaller (Planet Coaster's way out of a head-on standoff). */
constexpr float kDenseSqueezeAfter = 1.0f;
constexpr float kDenseSqueeze = 0.8f;
/** Crowds Push Characters: the part of the crowd's press a Character gets as a push, and the
 * largest speed it gets from it in one step. */
constexpr float kCrowdPushBack = 0.02f;
constexpr float kCrowdPushMaxSpeed = 2.0f;
/** A Jolt Character's mass for pushes (JoltCharacter's kMass). */
constexpr float kCharacterMass = 70.0f;

struct Vec2 {
  float x = 0.0f, y = 0.0f;
};

/** A navmesh wall of a Dense agent this step (the plane): its ends and the way in. */
struct DenseWall {
  Vec2 a, b, inward;
};
/** How far inside its walls a Dense agent's middle stays. */
constexpr float kDenseWallMargin = 0.01f;

}  // namespace

void KX_Navigation::CountCrowding(LN_JobPool *pool)
{
  /* Density-aware routes: each agent's own crowd (on the helpers, from its neighbours), then
   * each polygon the most crowded of its agents (on the main thread; a maximum: the same in any
   * order). Searches take the less crowded of two ways: a crowd spreads over several doors. An
   * agent following a path looks at its way ahead every kCrowdReplanSteps since its search. */
  for (auto &[navmesh, crowded] : m_crowding) {
    if (!crowded.empty()) {
      crowded.clear();
    }
  }
  const std::vector<Body> &bodies = m_step->bodies;
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    agent.crowd_cost = 0.0f;
    /* (Standing agents count too: one that never had to search stands in its polygon all the
     * same.) */
    if (agent.navmesh == nullptr || agent.corridor.getFirstPoly() == 0 || agent.on_link) {
      return;
    }
    int count = bodies[index].standing ? 2 : 1;
    for (const uint32_t b : agent.neighbours) {
      const Body &other = bodies[b];
      if (other.agent != UINT32_MAX &&
          distance_2d(other.position, agent.position) <= kCrowdRadius)
      {
        count += other.standing ? 2 : 1;
      }
    }
    if (count >= kCrowdedAgents) {
      agent.crowd_cost = crowd_extra_cost(float(count) / (float(M_PI) * kCrowdRadius *
                                                          kCrowdRadius));
    }
  });
  for (const std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    const KX_NavAgent &agent = *holder;
    if (agent.crowd_cost > 0.0f) {
      if (const dtPolyRef ref = agent.corridor.getFirstPoly()) {
        float &cost = m_crowding[agent.navmesh][uint64_t(ref)];
        cost = std::max(cost, agent.crowd_cost);
      }
    }
  }
  std::vector<uint32_t> &look = m_crowdLookScratch;
  look.clear();
  for (uint32_t i = 0; i < m_agents.size(); i++) {
    KX_NavAgent &agent = *m_agents[i];
    agent.filter.crowding = nullptr;
    agent.crowded_ahead = false;
    if (agent.navmesh == nullptr) {
      continue;
    }
    auto it = m_crowding.find(agent.navmesh);
    if (it == m_crowding.end() || it->second.empty()) {
      continue;
    }
    agent.filter.crowding = &it->second;
    /* PlanPaths counts this step next: its count then is this one. */
    const int since = agent.steps_since_search + 1;
    if (since >= kCrowdReplanSteps && since % kCrowdReplanSteps == 0 && agent.requested &&
        agent.has_goal && !agent.arrived && !agent.on_link && agent.corridor_ready)
    {
      look.push_back(i);
    }
  }
  RunJobs(pool, look.size(), [&](const size_t k) {
    KX_NavAgent &agent = *m_agents[look[k]];
    agent.crowded_ahead = CrowdedAhead(agent);
  });
}

float KX_Navigation::CrowdExtraCostForTests(const MT_Vector3 &p)
{
  KX_NavMesh *navmesh = GetSceneNavMesh();
  if (navmesh == nullptr) {
    return 0.0f;
  }
  auto it = m_crowding.find(navmesh);
  if (it == m_crowding.end()) {
    return 0.0f;
  }
  float nav[3], nearest[3];
  navmesh->ToNav(p, nav);
  dtPolyRef ref = 0;
  navmesh->ThreadQuery()->findNearestPoly(
      nav, KX_NavMesh::kSnapExtents, &navmesh->DefaultFilter(), &ref, nearest);
  auto cost = it->second.find(uint64_t(ref));
  return cost == it->second.end() ? 0.0f : cost->second;
}

float KX_Navigation::StallSecondsForTests(const KX_GameObject *object) const
{
  const KX_NavAgent *agent = FindAgent(object);
  return agent != nullptr ? agent->stall_time : -1.0f;
}

bool KX_Navigation::CrowdedAhead(KX_NavAgent &agent) const
{
  /* Its way ahead: the corridor's polygons after its own, one costing kCrowdReplanCost more. */
  if (agent.filter.crowding == nullptr || !agent.corridor_ready) {
    return false;
  }
  const dtPolyRef *path = agent.corridor.getPath();
  const int ahead = std::min(agent.corridor.getPathCount(), kCrowdLookAhead);
  for (int i = 1; i < ahead; i++) {
    auto it = agent.filter.crowding->find(uint64_t(path[i]));
    if (it != agent.filter.crowding->end() && it->second >= kCrowdReplanCost) {
      return true;
    }
  }
  return false;
}

void KX_Navigation::SolveDense(const float dt, LN_JobPool *pool)
{
  /* Avoidance Dense: position-based crowds (Weiss, Litteneker, Jiang, Terzopoulos 2017, without
   * their GPU). Each Dense agent first moves where it wants to go (its velocity changing at most
   * as ORCA's does, steered sideways from agents about to meet it), then kDenseRounds rounds push
   * every overlapping pair apart, the lighter more (weight from Priority; a polite agent counts
   * kDensePoliteWeight heavier, an obstacle, the player with Avoid Physics Objects included, never
   * moves); the slide along the navmesh (a body: Jolt) keeps it off the walls. Its velocity is how
   * far it got. Each round
   * reads the round before only (Jacobi): the same on any thread count. A Dense agent that
   * arrived takes part too: shoved, it moves (and walks back to its spot). */
  std::vector<uint32_t> dense;
  for (uint32_t i = 0; i < m_agents.size(); i++) {
    KX_NavAgent &agent = *m_agents[i];
    agent.dense_pushed = false;
    if (agent.requested && agent.has_goal && agent.request.avoid && agent.request.dense &&
        !agent.on_link && !agent.link_started && !agent.flies && !agent.crawls)
    {
      dense.push_back(i);
    }
  }
  if (dense.empty()) {
    return;
  }
  const std::vector<Body> &bodies = m_step->bodies;
  const size_t n = dense.size();
  /* Body index -> its place among the Dense agents (agents are the first bodies, in order). */
  std::vector<int32_t> slot(bodies.size(), -1);
  for (size_t k = 0; k < n; k++) {
    slot[dense[k]] = int32_t(k);
  }
  /* Where the others will be at the step's end: polite agents where they mean to go, obstacles
   * where they are going. */
  std::vector<Vec2> fixed(bodies.size());
  for (size_t b = 0; b < bodies.size(); b++) {
    const Body &body = bodies[b];
    MT_Vector3 end = body.position + body.velocity * dt;
    if (body.agent != UINT32_MAX && body.agent < m_agents.size()) {
      end = m_agents[body.agent]->position + m_agents[body.agent]->new_velocity * dt;
    }
    fixed[b] = {float(end[0]), float(end[1])};
  }
  std::vector<Vec2> start(n), x(n), next(n);
  std::vector<float> squeeze(n, 1.0f);
  std::vector<std::vector<DenseWall>> walls(n);
  RunJobs(pool, n, [&](const size_t k) {
    KX_NavAgent &agent = *m_agents[dense[k]];
    start[k] = {float(agent.position[0]), float(agent.position[1])};
    const float speed = std::max(agent.request.speed, 0.0f);
    /* The velocity it wants, changing at most as fast as ORCA's (no flicker). */
    Vec2 v = {float(agent.desired[0]), float(agent.desired[1])};
    const Vec2 told = {float(agent.commanded[0]), float(agent.commanded[1])};
    const float max_change = (8.0f + speed * 2.0f) * dt;
    Vec2 change = {v.x - told.x, v.y - told.y};
    const float change_len = std::hypot(change.x, change.y);
    if (change_len > max_change && change_len > 0.0f) {
      v = {told.x + change.x * (max_change / change_len), told.y + change.y * (max_change / change_len)};
    }
    /* Look ahead: about to meet another within kDenseLookAhead seconds, steer sideways (to the
     * side it already passes on; exactly head-on, each to its own right, as keep_right: they
     * part, and the lanes keep_right makes are not undone). */
    const float want = std::hypot(v.x, v.y);
    Vec2 steer;
    for (const uint32_t b : agent.neighbours) {
      const Body &other = bodies[b];
      const Vec2 p = {float(other.position[0] - agent.position[0]),
                      float(other.position[1] - agent.position[1])};
      const Vec2 u = {v.x - float(other.velocity[0]), v.y - float(other.velocity[1])};
      const float reach = agent.radius + other.radius;
      const float pp = p.x * p.x + p.y * p.y, pu = p.x * u.x + p.y * u.y,
                  uu = u.x * u.x + u.y * u.y;
      if (pp <= reach * reach || uu < 1e-8f || pu <= 0.0f) {
        continue;
      }
      const float disc = pu * pu - uu * (pp - reach * reach);
      if (disc < 0.0f) {
        continue;
      }
      const float t = (pu - std::sqrt(disc)) / uu;
      if (t <= 0.0f || t >= kDenseLookAhead) {
        continue;
      }
      const float len = std::sqrt(pp);
      Vec2 left = {-p.y / len, p.x / len};
      const float side = p.x * u.y - p.y * u.x;
      if (side <= 0.0f) {
        left = {-left.x, -left.y};
      }
      const float strength = want * kDenseSteer * (1.0f - t / kDenseLookAhead);
      steer.x += left.x * strength;
      steer.y += left.y * strength;
    }
    v.x += steer.x;
    v.y += steer.y;
    const float len = std::hypot(v.x, v.y);
    if (len > speed && len > 0.0f) {
      v = {v.x * speed / len, v.y * speed / len};
    }
    x[k] = {start[k].x + v.x * dt, start[k].y + v.y * dt};
    /* Stuck pressing for a while: it squeezes. */
    squeeze[k] = agent.press_time >= kDenseSqueezeAfter ? kDenseSqueeze : 1.0f;
    /* Its walls: the navmesh's edges near it it stands inside of (DetourCrowd's side test),
     * each with the way in. */
    walls[k].clear();
    if (update_boundary(agent)) {
      float here[3];
      agent.navmesh->ToNav(agent.position, here);
      for (int i = 0; i < agent.boundary.getSegmentCount(); i++) {
        const float *seg = agent.boundary.getSegment(i);
        if (dtTriArea2D(here, &seg[0], &seg[3]) < 0.0f) {
          continue;
        }
        const MT_Vector3 a3 = agent.navmesh->ToWorld(&seg[0]);
        const MT_Vector3 b3 = agent.navmesh->ToWorld(&seg[3]);
        DenseWall wall;
        wall.a = {float(a3[0]), float(a3[1])};
        wall.b = {float(b3[0]), float(b3[1])};
        const Vec2 ab = {wall.b.x - wall.a.x, wall.b.y - wall.a.y};
        const float len = std::hypot(ab.x, ab.y);
        if (len <= 1e-4f) {
          continue;
        }
        /* Its way in from the navmesh's own winding (by the agent's side of it, one standing on
         * the line, where the slide along the navmesh leaves it, was pushed out: two crowds
         * meeting carried an NPC along a corridor's edge for good). */
        const MT_Vector3 in = wall_inward(*agent.navmesh, &seg[0], &seg[3]);
        if (in.length2() < 0.5f) {
          continue;
        }
        wall.inward = {float(in[0]), float(in[1])};
        walls[k].push_back(wall);
      }
    }
  });
  /* Where each wanted to be (the crowd's press on a Character, below). */
  const std::vector<Vec2> wanted = x;
  for (int round = 0; round < kDenseRounds; round++) {
    RunJobs(pool, n, [&](const size_t k) {
      const uint32_t index = dense[k];
      const KX_NavAgent &agent = *m_agents[index];
      const Body &self = bodies[index];
      const float own = 1.0f / std::max(self.weight, 1e-3f);
      Vec2 total;
      int count = 0;
      for (const uint32_t b : agent.neighbours) {
        const Body &other = bodies[b];
        const int32_t other_slot = slot[b];
        const Vec2 there = other_slot >= 0 ? x[size_t(other_slot)] : fixed[b];
        float theirs = 0.0f;
        if (other_slot >= 0) {
          theirs = 1.0f / std::max(other.weight, 1e-3f);
        }
        else if (other.agent != UINT32_MAX) {
          theirs = 1.0f / (std::max(other.weight, 1e-3f) * kDensePoliteWeight);
        }
        /* (No head-on shrink here, nor a sidestep of face-to-face pairs: two crowds meeting
         * head-on passed sooner without either, keep_right doing it; measured 2026-11-19.) */
        const float squeezed = std::min(squeeze[k], other_slot >= 0 ? squeeze[size_t(other_slot)] :
                                                                      1.0f);
        const float reach = (agent.radius + other.radius) * squeezed;
        const float dx = x[k].x - there.x, dy = x[k].y - there.y;
        const float dist = std::hypot(dx, dy);
        if (dist >= reach) {
          continue;
        }
        /* Exactly on top of each other: apart along a fixed side by their order. */
        Vec2 dir = dist > 1e-5f ? Vec2{dx / dist, dy / dist} :
                                  Vec2{(b < index) ? 1.0f : -1.0f, 0.0f};
        const float share = own / (own + theirs);
        total.x += dir.x * (reach - dist) * share;
        total.y += dir.y * (reach - dist) * share;
        count++;
      }
      /* The navmesh's walls (its edges, already its radius in from the real walls: no radius to
       * keep from them, which shut a door one body wide): it may not cross one it stands inside
       * of (DetourCrowd's side test), where it passes along it. A push into a wall turns into a
       * slide along it, so a pressing crowd flows round corners into a door. */
      for (const DenseWall &wall : walls[k]) {
        const Vec2 ab = {wall.b.x - wall.a.x, wall.b.y - wall.a.y};
        const float ab2 = ab.x * ab.x + ab.y * ab.y;
        const float t = ((x[k].x - wall.a.x) * ab.x + (x[k].y - wall.a.y) * ab.y) / ab2;
        if (t < 0.0f || t > 1.0f) {
          continue;
        }
        const float inside = (x[k].x - wall.a.x) * wall.inward.x +
                             (x[k].y - wall.a.y) * wall.inward.y;
        if (inside >= kDenseWallMargin) {
          continue;
        }
        total.x += wall.inward.x * (kDenseWallMargin - inside);
        total.y += wall.inward.y * (kDenseWallMargin - inside);
        count++;
      }
      const float scale = count > 0 ? kDenseOverRelaxation / float(count) : 0.0f;
      next[k] = {x[k].x + total.x * scale, x[k].y + total.y * scale};
    });
    x.swap(next);
  }
  RunJobs(pool, n, [&](const size_t k) {
    KX_NavAgent &agent = *m_agents[dense[k]];
    Vec2 v = {(x[k].x - start[k].x) / dt, (x[k].y - start[k].y) / dt};
    /* Its velocity changes at most as fast as its wish may (what is left of a shove is pushed
     * apart in the next steps): the pushes' whole correction in one step shook a packed crowd
     * sideways at walking speed while nobody went through the door. */
    const Vec2 told = {float(agent.commanded[0]), float(agent.commanded[1])};
    const float max_change = (8.0f + std::max(agent.request.speed, 0.0f) * 2.0f) * dt;
    const Vec2 change = {v.x - told.x, v.y - told.y};
    const float change_len = std::hypot(change.x, change.y);
    if (change_len > max_change && change_len > 0.0f) {
      v = {told.x + change.x * (max_change / change_len), told.y + change.y * (max_change / change_len)};
    }
    /* Shoved, it may go faster than it walks, not without bound. */
    const float limit = std::max(agent.request.speed, 0.5f) * 2.0f;
    const float len = std::hypot(v.x, v.y);
    if (len > limit && len > 0.0f) {
      v = {v.x * limit / len, v.y * limit / len};
    }
    if (!std::isfinite(v.x) || !std::isfinite(v.y)) {
      v = {0.0f, 0.0f};
    }
    agent.new_velocity = MT_Vector3(v.x, v.y, agent.arrived ? 0.0f : float(agent.desired[2]));
    agent.dense_pushed = agent.arrived && std::hypot(v.x, v.y) > 0.05f;
  });
  if (!m_settings.crowd_pushes) {
    return;
  }
  /* Crowds Push Characters: a Character that is no NPC (an obstacle with Avoid Physics Objects)
   * pressed by Dense agents gets the crowd's press as a knock-back: how far each wanted to go
   * into it (the solve then kept it out), by its weight; in agent order, written in obstacle
   * order (main thread). */
  PHY_IPhysicsEnvironment *physics = Physics();
  std::vector<Vec2> press(bodies.size());
  for (size_t k = 0; k < n; k++) {
    const KX_NavAgent &agent = *m_agents[dense[k]];
    for (const uint32_t b : agent.neighbours) {
      const Body &other = bodies[b];
      if (other.agent != UINT32_MAX || other.object == nullptr) {
        continue;
      }
      const float dx = fixed[b].x - wanted[k].x, dy = fixed[b].y - wanted[k].y;
      const float dist = std::hypot(dx, dy);
      const float reach = agent.radius + other.radius;
      if (dist >= reach || dist <= 1e-5f) {
        continue;
      }
      const float weight = bodies[dense[k]].weight;
      press[b].x += dx / dist * (reach - dist) * weight;
      press[b].y += dy / dist * (reach - dist) * weight;
    }
  }
  for (size_t b = 0; b < bodies.size(); b++) {
    if (press[b].x == 0.0f && press[b].y == 0.0f) {
      continue;
    }
    KX_GameObject *object = bodies[b].object;
    PHY_IPhysicsController *controller = object->GetPhysicsController();
    if (controller == nullptr || !controller->IsCharacter() || physics == nullptr ||
        physics->GetCharacterController(object) == nullptr)
    {
      continue;
    }
    Vec2 dv = {press[b].x * kCrowdPushBack / dt, press[b].y * kCrowdPushBack / dt};
    const float len = std::hypot(dv.x, dv.y);
    if (len > kCrowdPushMaxSpeed) {
      dv = {dv.x * kCrowdPushMaxSpeed / len, dv.y * kCrowdPushMaxSpeed / len};
    }
    controller->ApplyImpulse(object->NodeGetWorldPosition(),
                             MT_Vector3(dv.x, dv.y, 0.0f) * kCharacterMass,
                             false);
  }
}

/** \} */

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

/** A crawler's turn this step (phase 9): its up (the local axis Look At turns round, Rot Axis)
 * toward its surface's normal, fast enough to round an edge at its speed; then, with Look At
 * while it moves, its front toward the way it goes, round that normal, at its Rot Speed. False:
 * already so. */
static bool crawl_turn(const KX_NavAgent &agent, const float dt, MT_Matrix3x3 &r_turn)
{
  const MT_Matrix3x3 orientation = agent.object->NodeGetWorldOrientation();
  MT_Vector3 local_up(0.0f, 0.0f, 0.0f);
  local_up[std::clamp(agent.request.rot_axis, 0, 2)] = 1.0f;
  const int front_index = std::clamp(agent.request.front_axis, 0, 5);
  MT_Vector3 local_front(0.0f, 0.0f, 0.0f);
  local_front[front_index % 3] = front_index >= 3 ? -1.0f : 1.0f;
  const MT_Vector3 n = agent.surface_normal;
  MT_Vector3 up = orientation * local_up;
  if (up.length2() < 1e-12f || n.length2() < 1e-12f) {
    return false;
  }
  up.normalize();
  /* Up to the normal: a quarter turn over an edge of its own height at its speed, at least
   * 8 radians a second. */
  const float edge = std::max(agent.crawl_offset, 0.05f);
  const float max_up = std::max(8.0f, std::max(agent.request.speed, 0.0f) / edge * 1.5f) * dt;
  MT_Matrix3x3 turn = MT_Matrix3x3::Identity();
  bool turned = false;
  const MT_Vector3 cross = up.cross(n);
  const float s = float(cross.length()), c = float(up.dot(n));
  const float angle = std::atan2(s, c);
  if (angle > 1e-4f) {
    MT_Vector3 axis = s > 1e-6f ? cross / s : (orientation * local_front).normalized();
    if (s <= 1e-6f && axis.length2() < 0.5f) {
      axis = MT_Vector3(1.0f, 0.0f, 0.0f);
    }
    turn = MT_Matrix3x3(MT_Quaternion(axis, MT_Scalar(std::min(angle, max_up))));
    turned = true;
  }
  /* Look At round its (new) up, while it moves (another Rotating Object: TurnAgents' own way). */
  const bool own = agent.request.rotating_object == nullptr ||
                   agent.request.rotating_object == agent.object;
  if (own && agent.request.look_at && agent.has_goal && !agent.arrived) {
    const MT_Matrix3x3 stood = turn * orientation;
    MT_Vector3 up1 = stood * local_up;
    up1.normalize();
    MT_Vector3 heading = agent.new_velocity - up1 * float(agent.new_velocity.dot(up1));
    MT_Vector3 front = stood * local_front;
    front -= up1 * float(front.dot(up1));
    if (heading.length2() > 1e-4f && front.length2() > 1e-8f) {
      heading.normalize();
      front.normalize();
      const MT_Vector3 side = front.cross(heading);
      float look = std::atan2(float(side.dot(up1)), float(front.dot(heading)));
      const float max_look = std::max(agent.request.rot_speed, 0.0f) * dt;
      look = std::copysign(std::min(std::fabs(look), max_look), look);
      if (std::fabs(look) > 1e-6f) {
        turn = MT_Matrix3x3(MT_Quaternion(up1, MT_Scalar(look))) * turn;
        turned = true;
      }
    }
  }
  r_turn = turn;
  return turned;
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
    if (agent.crawls && agent.on_surface && (agent.request.rotating_object == nullptr ||
                                             agent.request.rotating_object == agent.object))
    {
      /* (Turned with its stand on the surface: crawl_turn.) */
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
    agent.crawl_turn_ok = false;
    if (agent.crawls && agent.on_surface && agent.requested) {
      agent.crawl_turn_ok = crawl_turn(agent, dt, agent.crawl_turn);
    }
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
    if (agent.crawl_turn_ok) {
      agent.object->ApplyRotation(agent.crawl_turn, false);
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

/* -------------------------------------------------------------------- */
/** \name Flying and swimming (phase 8, 2026-11-19)
 * \{ */

void KX_Navigation::CollectLevel(const MT_Vector3 &aabb_min,
                                 const MT_Vector3 &aabb_max,
                                 KX_NavGeometry &r_geometry)
{
  if (m_geometryForTests) {
    m_geometryForTests(aabb_min, aabb_max, r_geometry);
    return;
  }
  if (m_scene == nullptr) {
    return;
  }
  KX_NavCollectSceneGeometry(
      m_scene,
      aabb_min,
      aabb_max,
      [this](const std::string &name) { return AreaId(name); },
      [this](const KX_GameObject *object) { return IsExcludedFromLevel(object); },
      r_geometry);
}

/** An air solid's box in the world (its turned corners'). */
static void air_solid_bounds(const MT_Vector3 &center,
                             const MT_Vector3 &half,
                             const float yaw,
                             const bool round,
                             MT_Vector3 &r_min,
                             MT_Vector3 &r_max)
{
  float ex, ey;
  if (round) {
    /* (The prism round the circle.) */
    ex = ey = float(half[0]) / std::cos(3.14159265f / 8.0f);
  }
  else {
    const float c = std::fabs(std::cos(yaw)), s = std::fabs(std::sin(yaw));
    ex = c * float(half[0]) + s * float(half[1]);
    ey = s * float(half[0]) + c * float(half[1]);
  }
  const MT_Vector3 extent(ex, ey, float(half[2]));
  r_min = center - extent;
  r_max = center + extent;
}

void KX_Navigation::CollectAirLevel(const MT_Vector3 &aabb_min,
                                    const MT_Vector3 &aabb_max,
                                    KX_NavGeometry &r_geometry)
{
  CollectLevel(aabb_min, aabb_max, r_geometry);
  if (m_airSolids.empty()) {
    return;
  }
  /* In the order of their places (a hash map has none), each a closed convex piece: a turned box,
   * or for a round one an eight-sided prism round its circle. */
  std::vector<const AirSolid *> solids;
  for (const auto &[object, solid] : m_airSolids) {
    if (!solid.solid) {
      continue;
    }
    MT_Vector3 lo, hi;
    air_solid_bounds(solid.center, solid.half, solid.yaw, solid.round, lo, hi);
    if (hi[0] < aabb_min[0] || lo[0] > aabb_max[0] || hi[1] < aabb_min[1] ||
        lo[1] > aabb_max[1] || hi[2] < aabb_min[2] || lo[2] > aabb_max[2])
    {
      continue;
    }
    solids.push_back(&solid);
  }
  std::sort(solids.begin(), solids.end(), [](const AirSolid *a, const AirSolid *b) {
    for (int axis = 0; axis < 3; axis++) {
      if (a->center[axis] != b->center[axis]) {
        return a->center[axis] < b->center[axis];
      }
    }
    return false;
  });
  KX_NavGeometry &g = r_geometry;
  if (g.pieces.size() < g.tris.size() / 3) {
    g.pieces.resize(g.tris.size() / 3, -1);
  }
  int32_t piece = kAirSolidPieceBase;
  /* A face given counterclockwise seen from outside, written as KX_NavCollectSceneGeometry
   * writes (the points reversed: Recast's handedness). */
  auto triangle = [&](const MT_Vector3 &a, const MT_Vector3 &b, const MT_Vector3 &c) {
    const int base = int(g.verts.size() / 3);
    for (const MT_Vector3 *v : {&a, &c, &b}) {
      g.verts.push_back(float((*v)[0]));
      g.verts.push_back(float((*v)[1]));
      g.verts.push_back(float((*v)[2]));
    }
    g.tris.push_back(base);
    g.tris.push_back(base + 1);
    g.tris.push_back(base + 2);
    g.areas.push_back(0);
    g.pieces.push_back(piece);
  };
  for (const AirSolid *solid : solids) {
    const int sides = solid->round ? 8 : 4;
    /* The ring of corners round the middle (counterclockwise from above), its bottom and top. */
    MT_Vector3 bottom[8], top[8];
    for (int k = 0; k < sides; k++) {
      float x, y;
      if (solid->round) {
        /* (The prism holds the circle: its corners a little outside.) */
        const float r = float(solid->half[0]) / std::cos(3.14159265f / 8.0f);
        const float a = 6.2831853f * float(k) / 8.0f;
        x = r * std::cos(a);
        y = r * std::sin(a);
      }
      else {
        static const float sx[4] = {-1.0f, 1.0f, 1.0f, -1.0f}, sy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        const float lx = sx[k] * float(solid->half[0]), ly = sy[k] * float(solid->half[1]);
        const float c = std::cos(solid->yaw), s = std::sin(solid->yaw);
        x = c * lx - s * ly;
        y = s * lx + c * ly;
      }
      bottom[k] = solid->center + MT_Vector3(x, y, -float(solid->half[2]));
      top[k] = solid->center + MT_Vector3(x, y, float(solid->half[2]));
    }
    for (int k = 1; k + 1 < sides; k++) {
      triangle(top[0], top[k], top[k + 1]);
      triangle(bottom[0], bottom[k + 1], bottom[k]);
    }
    for (int k = 0; k < sides; k++) {
      const int n = (k + 1) % sides;
      triangle(bottom[k], bottom[n], top[n]);
      triangle(bottom[k], top[n], top[k]);
    }
    piece++;
  }
}

void KX_Navigation::UpdateAirSolids()
{
  /* Plan item 11 (2026-11-20). Only while there are air maps (a map made later takes the
   * obstacles at rest from then on). */
  if (m_airMaps.empty()) {
    m_airSolids.clear();
    return;
  }
  const uint64_t now = m_stepCount;
  auto change = [&](const AirSolid &solid) {
    MT_Vector3 lo, hi;
    air_solid_bounds(solid.center, solid.half, solid.yaw, solid.round, lo, hi);
    m_airChanges.emplace_back(lo, hi);
  };
  for (const KX_NavCarveObstacle &carve : m_step->carve) {
    /* (Never an NPC's own body.) */
    if (FindAgent(carve.object) != nullptr) {
      continue;
    }
    AirSolid &solid = m_airSolids[carve.object];
    const bool same = solid.seen_step != 0 &&
                      (solid.center - carve.center).length2() <= 1e-4f &&
                      (solid.half - carve.half_size).length2() <= 1e-6f &&
                      std::fabs(solid.yaw - carve.yaw) <= 1e-3f && solid.round == carve.round;
    if (!same) {
      if (solid.solid) {
        change(solid);
      }
      solid.center = carve.center;
      solid.half = carve.half_size;
      solid.yaw = carve.yaw;
      solid.round = carve.round;
      solid.still_steps = 0;
      solid.solid = false;
    }
    else if (!solid.solid && ++solid.still_steps >= kAirSolidSteps) {
      solid.solid = true;
      change(solid);
    }
    solid.seen_step = now;
  }
  /* Moving again (woken, carried off) or gone: free at once. */
  for (auto it = m_airSolids.begin(); it != m_airSolids.end();) {
    if (it->second.seen_step != now) {
      if (it->second.solid) {
        change(it->second);
      }
      it = m_airSolids.erase(it);
    }
    else {
      ++it;
    }
  }
}

void KX_Navigation::CollectWater(std::vector<KX_NavWaterVolume> &r_water) const
{
  /* The Buoyancy Volume sensors: each its bounds (its mesh's, scaled and turned). */
  r_water.clear();
  if (m_hasWaterForTests) {
    r_water = m_waterForTests;
    return;
  }
  if (m_scene == nullptr) {
    return;
  }
  for (KX_GameObject *object : *m_scene->GetObjectList()) {
    const PHY_IPhysicsController *controller = object->GetPhysicsController();
    if (controller == nullptr || !controller->IsBuoyancyVolume()) {
      continue;
    }
    MT_Vector3 half(0.5f, 0.5f, 0.5f), middle(0.0f, 0.0f, 0.0f);
    if (const blender::Object *blender_object = object->GetBlenderObject()) {
      if (const std::optional<blender::Bounds<blender::float3>> bounds = BKE_object_boundbox_get(
              blender_object))
      {
        for (int axis = 0; axis < 3; axis++) {
          half[axis] = (bounds->max[axis] - bounds->min[axis]) * 0.5f;
          middle[axis] = (bounds->max[axis] + bounds->min[axis]) * 0.5f;
        }
      }
    }
    const MT_Vector3 scale = object->NodeGetWorldScaling();
    KX_NavWaterVolume volume;
    for (int axis = 0; axis < 3; axis++) {
      half[axis] *= std::fabs(float(scale[axis]));
      middle[axis] *= float(scale[axis]);
    }
    volume.orientation = object->NodeGetWorldOrientation();
    volume.center = object->NodeGetWorldPosition() + volume.orientation * middle;
    volume.half = half;
    r_water.push_back(volume);
  }
}

KX_NavAirMap *KX_Navigation::AirMapFor(const float radius, LN_JobPool *pool)
{
  /* A map as large or a little larger (a quarter and 5 cm) is its own; else a new one (at most
   * kMaxAirMaps sizes: the level read once each), else the nearest larger, else the largest. */
  const float r = std::max(radius, 0.05f);
  KX_NavAirMap *larger = nullptr, *largest = nullptr;
  for (const std::unique_ptr<KX_NavAirMap> &map : m_airMaps) {
    if (map->Radius() >= r - 1e-4f && map->Radius() <= r * 1.25f + 0.05f) {
      return map.get();
    }
    if (map->Radius() >= r && (larger == nullptr || map->Radius() < larger->Radius())) {
      larger = map.get();
    }
    if (largest == nullptr || map->Radius() > largest->Radius()) {
      largest = map.get();
    }
  }
  if (int(m_airMaps.size()) < kMaxAirMaps) {
    KX_NavAirMap::Settings settings;
    settings.radius = r;
    /* (Half the radius: a gap a flyer fits through needs its width and one voxel.) */
    settings.voxel = std::clamp(r * 0.5f, 0.2f, 1.0f);
    settings.headroom = 30.0f;
    std::unique_ptr<KX_NavAirMap> map = std::make_unique<KX_NavAirMap>(settings);
    KX_NavGeometry geometry;
    CollectAirLevel(MT_Vector3(-1e6f, -1e6f, -1e6f), MT_Vector3(1e6f, 1e6f, 1e6f), geometry);
    if (m_airMaps.empty()) {
      CollectWater(m_water);
    }
    map->Build(geometry, m_water, pool);
    m_airMaps.push_back(std::move(map));
    /* Level changes from now on are noted (TrackLevelChanges turns it on only at the next
     * step: a piece removed before then was missed). */
    PHY_IPhysicsEnvironment *physics = Physics();
    if (!m_staticTracking && !m_testNavMesh && physics != nullptr) {
      physics->SetStaticChangeTracking(true);
      m_staticTracking = true;
    }
    return m_airMaps.back().get();
  }
  if (larger == nullptr && !m_airSizeWarned) {
    /* (Its map's gaps may be too narrow for it: it may touch walls on the way.) */
    char text[200];
    snprintf(text,
             sizeof(text),
             "a flyer of radius %.2f m is larger than every air map (at most %d sizes; the "
             "largest %.2f m): it flies by the largest",
             r,
             int(kMaxAirMaps),
             largest->Radius());
    ReportProblem("flyer_sizes_full", "", text);
    m_airSizeWarned = true;
  }
  return larger != nullptr ? larger : largest;
}

static bool same_water(const std::vector<KX_NavWaterVolume> &a,
                       const std::vector<KX_NavWaterVolume> &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if ((a[i].center - b[i].center).length2() > 1e-6f ||
        (a[i].half - b[i].half).length2() > 1e-6f)
    {
      return false;
    }
    for (int row = 0; row < 3; row++) {
      if ((a[i].orientation[row] - b[i].orientation[row]).length2() > 1e-8f) {
        return false;
      }
    }
  }
  return true;
}

const KX_NavAirMap *KX_Navigation::ExistingAirMap(const float radius) const
{
  const float r = std::max(radius, 0.05f);
  const KX_NavAirMap *larger = nullptr, *largest = nullptr;
  for (const std::unique_ptr<KX_NavAirMap> &map : m_airMaps) {
    if (map->Radius() >= r - 1e-4f && map->Radius() <= r * 1.25f + 0.05f) {
      return map.get();
    }
    if (map->Radius() >= r && (larger == nullptr || map->Radius() < larger->Radius())) {
      larger = map.get();
    }
    if (largest == nullptr || map->Radius() > largest->Radius()) {
      largest = map.get();
    }
  }
  return larger != nullptr ? larger : largest;
}

const KX_NavAirMap *KX_Navigation::AirMapForQuery(const float radius)
{
  const float r = std::max(radius, 0.05f);
  const KX_NavAirMap *map = ExistingAirMap(r);
  if (map == nullptr || !(map->Radius() >= r - 1e-4f && map->Radius() <= r * 1.25f + 0.05f)) {
    /* (Queries may run on the helpers: the map is made at the next step.) */
    std::lock_guard<std::mutex> lock(m_airSizesMutex);
    if (std::find(m_airSizesAsked.begin(), m_airSizesAsked.end(), r) == m_airSizesAsked.end()) {
      m_airSizesAsked.push_back(r);
    }
  }
  return map;
}

void KX_Navigation::UpdateAirMaps(LN_JobPool *pool)
{
  std::vector<float> asked;
  {
    std::lock_guard<std::mutex> lock(m_airSizesMutex);
    asked.swap(m_airSizesAsked);
  }
  std::sort(asked.begin(), asked.end());
  for (const float r : asked) {
    AirMapFor(r, pool);
  }
  /* A map whose box no longer holds the level or the water (a piece added beside the level or
   * above its headroom, water moved out, a map made before any level): built whole again, a
   * new box (the bricks outside a box are never looked at); its flyers search again. */
  std::vector<KX_NavAirMap *> whole;
  auto rebuild_whole = [&](KX_NavAirMap *map) {
    if (std::find(whole.begin(), whole.end(), map) != whole.end()) {
      return;
    }
    whole.push_back(map);
    KX_NavGeometry geometry;
    CollectAirLevel(MT_Vector3(-1e6f, -1e6f, -1e6f), MT_Vector3(1e6f, 1e6f, 1e6f), geometry);
    map->Build(geometry, m_water, pool);
    for (const std::unique_ptr<KX_NavAgent> &agent : m_agents) {
      if (agent->air == map) {
        agent->air_needs_search = true;
      }
    }
  };
  /* The water again (it rose, its tank moved, a pool appeared or went): every map takes it, the
   * swimmers search again. */
  if (!m_airMaps.empty() && m_stepCount % uint64_t(kWaterCheckSteps) == 0) {
    std::vector<KX_NavWaterVolume> water;
    CollectWater(water);
    if (!same_water(water, m_water)) {
      m_water = std::move(water);
      for (const std::unique_ptr<KX_NavAirMap> &map : m_airMaps) {
        bool held = true;
        for (const KX_NavWaterVolume &volume : m_water) {
          MT_Vector3 extent(0.0f, 0.0f, 0.0f);
          for (int axis = 0; axis < 3; axis++) {
            for (int k = 0; k < 3; k++) {
              extent[axis] += std::fabs(float(volume.orientation[axis][k])) *
                              float(volume.half[k]);
            }
          }
          held = held && map->Holds(volume.center - extent, volume.center + extent);
        }
        if (held) {
          map->SetWater(m_water);
        }
        else {
          rebuild_whole(map.get());
        }
      }
      for (const std::unique_ptr<KX_NavAgent> &agent : m_agents) {
        if (agent->air != nullptr && agent->water) {
          agent->air_needs_search = true;
        }
      }
    }
  }
  if (m_airChanges.empty()) {
    return;
  }
  /* Level pieces added, removed or come to rest moved (NoteStaticChanged): each map built again
   * there; flyers whose way passes there search again, and every way that ended short (a door
   * may have opened). */
  for (const std::unique_ptr<KX_NavAgent> &agent : m_agents) {
    if (agent->air != nullptr && !agent->air_complete) {
      agent->air_needs_search = true;
      agent->air_short_searches = 0;
    }
  }
  for (const auto &[lo, hi] : m_airChanges) {
    for (const std::unique_ptr<KX_NavAirMap> &map : m_airMaps) {
      if (std::find(whole.begin(), whole.end(), map.get()) != whole.end()) {
        continue;
      }
      if (!map->Holds(lo, hi)) {
        rebuild_whole(map.get());
        continue;
      }
      const float grow = map->Radius() + 2.0f * map->Voxel() + 0.5f;
      const MT_Vector3 margin(grow, grow, grow);
      KX_NavGeometry geometry;
      CollectAirLevel(lo - margin, hi + margin, geometry);
      map->Rebuild(lo, hi, geometry, pool);
    }
    const MT_Vector3 near_lo = lo - MT_Vector3(3.0f, 3.0f, 3.0f);
    const MT_Vector3 near_hi = hi + MT_Vector3(3.0f, 3.0f, 3.0f);
    for (const std::unique_ptr<KX_NavAgent> &holder : m_agents) {
      KX_NavAgent &agent = *holder;
      if (agent.air == nullptr || agent.air_needs_search) {
        continue;
      }
      MT_Vector3 last = agent.position;
      for (size_t i = agent.air_next; i < agent.air_path.size(); i++) {
        const MT_Vector3 &p = agent.air_path[i];
        bool overlap = true;
        for (int axis = 0; axis < 3 && overlap; axis++) {
          overlap = std::max(last[axis], p[axis]) >= near_lo[axis] &&
                    std::min(last[axis], p[axis]) <= near_hi[axis];
        }
        if (overlap) {
          agent.air_needs_search = true;
          break;
        }
        last = p;
      }
    }
  }
  m_airChanges.clear();
  /* (Maps with surfaces built again in places: their crawlers' true surface reads the whole
   * level, collected once for all of them.) */
  bool surfaces = false;
  for (const std::unique_ptr<KX_NavAirMap> &map : m_airMaps) {
    surfaces = surfaces || (map->HasSurfaces() &&
                            std::find(whole.begin(), whole.end(), map.get()) == whole.end());
  }
  if (surfaces) {
    KX_NavGeometry geometry;
    CollectAirLevel(MT_Vector3(-1e6f, -1e6f, -1e6f), MT_Vector3(1e6f, 1e6f, 1e6f), geometry);
    for (const std::unique_ptr<KX_NavAirMap> &map : m_airMaps) {
      if (map->HasSurfaces() && std::find(whole.begin(), whole.end(), map.get()) == whole.end()) {
        map->SetSurfaceGeometry(geometry);
      }
    }
  }
}

void KX_Navigation::PlanAir(LN_JobPool *pool)
{
  /* Flyers' ways: one without a way, whose goal moved off its way's end, whose way ends short
   * (every kAirPartialReplanSteps), whose way ahead was found blocked (SteerAir) or crossed a
   * changed place (UpdateAirMaps); at most kMaxAirSearchesPerStep a step, taken in turn from
   * where the last step stopped; each search on the helpers, reading its map only (the same on
   * any thread count). */
  std::vector<KX_NavAgent *> due;
  const size_t n = m_agents.size();
  size_t last_taken = SIZE_MAX;
  for (size_t k = 0; k < n; k++) {
    const size_t index = (m_airSearchCursor + k) % n;
    KX_NavAgent &agent = *m_agents[index];
    if (agent.air == nullptr) {
      continue;
    }
    agent.air_steps_since_search++;
    /* (Through a deck's hull, plan item 11: straight for its goal, no way searched.) */
    if (!agent.requested || !agent.has_goal || agent.arrived || agent.through_hull != nullptr) {
      continue;
    }
    /* (A search that found no way, or a way ending short, is tried again only after
     * kAirPartialReplanSteps, twice as long after each in a row that ended short: at most
     * 10 s; never every step. A goal no way reaches cost 1,000 flyers searching every second
     * 400 ms a step.) */
    const float moved = float((agent.goal - agent.air_path_goal).length());
    const int wait = std::min(kAirPartialReplanSteps << std::min(agent.air_short_searches, 4),
                              600);
    if (agent.air_needs_search || moved > std::max(0.5f, agent.request.arrive_distance) ||
        (!agent.air_complete && agent.air_steps_since_search >= wait))
    {
      if (int(due.size()) < kMaxAirSearchesPerStep) {
        due.push_back(&agent);
        last_taken = index;
      }
    }
  }
  if (last_taken != SIZE_MAX && int(due.size()) == kMaxAirSearchesPerStep) {
    m_airSearchCursor = (last_taken + 1) % std::max<size_t>(n, 1);
  }
  /* (One search a job: a search costs far more than handing a job out.) */
  auto search = [&](const size_t i) {
    KX_NavAgent &agent = *due[i];
    std::vector<MT_Vector3> path;
    bool complete = false;
    /* (Crawling: over the shell of the surfaces it may use, from its body's surface.) */
    const bool found =
        agent.crawls ?
            agent.air->FindSurfacePath(agent.on_surface ? agent.surface_point +
                                                              agent.surface_normal * agent.radius :
                                                          agent.position,
                                       agent.goal,
                                       agent.crawl_kinds,
                                       agent.crawl_costs,
                                       path,
                                       &complete,
                                       4.0f,
                                       kAirSearchNodes) :
            agent.air->FindPath(
                agent.position, agent.goal, path, &complete, agent.water, kAirSearchNodes);
    if (!found) {
      path.clear();
      complete = false;
    }
    agent.air_short_searches = complete ? 0 : agent.air_short_searches + 1;
    agent.air_path = std::move(path);
    agent.air_next = 0;
    agent.air_complete = complete;
    agent.air_path_goal = agent.goal;
    agent.air_needs_search = false;
    agent.air_steps_since_search = 0;
  };
  if (pool == nullptr || pool->GetHelperCount() == 0 || due.size() < 2) {
    for (size_t i = 0; i < due.size(); i++) {
      search(i);
    }
  }
  else {
    pool->Run(due.size(), [&](const size_t i, uint32_t /*thread*/) { search(i); });
  }
}

void KX_Navigation::SteerAir(KX_NavAgent &agent)
{
  /* Along its way: corners passed within reach are done; every kAirCheckSteps (staggered by
   * agent) a corner seen past the next is flown to straight, and a leg ahead no longer free (a
   * level piece moved in) makes it search again. */
  const MT_Vector3 &position = agent.position;
  const std::vector<MT_Vector3> &path = agent.air_path;
  const float speed = std::max(agent.request.speed, 0.0f);
  /* Crawling (phase 9): its way runs along the shell, a radius out from the surface (a voxel's
   * rounding more), while its body keeps its own height: it aims at where its body is at each
   * corner (the corner put onto the true surface at its height: aiming at the shell's corner it
   * stopped short of a wall, under it, for good), and what it sees along the shell is seen from
   * its body lifted onto the shell. */
  const bool crawls = agent.crawls;
  const float reach = std::max(crawls ? agent.radius : agent.air_radius, 0.3f);
  const auto aim = [&](const MT_Vector3 &corner) {
    if (!crawls) {
      return corner;
    }
    MT_Vector3 point, normal;
    const MT_Vector3 hint = agent.surface_normal;
    if (!agent.air->NearestSurface(
            corner, agent.radius + 2.0f * agent.air->Voxel() + 0.5f, point, normal, &hint))
    {
      return corner;
    }
    return point + normal * agent.crawl_offset;
  };
  if (agent.through_hull != nullptr && !crawls) {
    /* Plan item 11: coming down on its goal's deck, on its own deck, taking off: straight for
     * the goal (free air whatever the deck: the air map holds no vehicle; a way searched for
     * where a moving deck's place was lagged behind it), carried with the deck (its frame), its
     * own move over the deck slowing near the goal. */
    MT_Vector3 dir = agent.goal - position;
    const float dist = float(dir.length());
    agent.next_corner = agent.goal;
    agent.remaining = dist;
    agent.desired = MT_Vector3(0.0f, 0.0f, 0.0f);
    if (dist < 1e-4f) {
      return;
    }
    const float slow = std::max(agent.air_radius * 2.0f, agent.request.arrive_distance * 2.0f);
    const float scale = std::clamp(dist / std::max(slow, 1e-3f), 0.2f, 1.0f);
    agent.desired = dir * (speed * scale / dist);
    return;
  }
  const MT_Vector3 seen_from = crawls && agent.on_surface ?
                                   agent.surface_point +
                                       agent.surface_normal * (agent.radius + agent.air->Voxel()) :
                                   position;
  const auto clear = [&](const MT_Vector3 &a, const MT_Vector3 &b) {
    return crawls ? agent.air->SurfaceLineFree(a, b, agent.crawl_kinds) :
                    agent.air->LineFree(a, b, agent.water);
  };
  while (agent.air_next + 1 < path.size() &&
         float((aim(path[agent.air_next]) - position).length()) <= reach)
  {
    agent.air_next++;
  }
  /* (A crawler skips no corner where its surface turns: from the floor a corner up a wall is
   * "seen" along the wall's shell, and aimed at from the floor it crept toward the wall's foot
   * pressed into the floor.) */
  const auto same_surface = [&](const MT_Vector3 &a, const MT_Vector3 &b) {
    if (!crawls) {
      return true;
    }
    const float surface_reach = agent.radius + 2.0f * agent.air->Voxel() + 0.5f;
    MT_Vector3 pa, na, pb, nb;
    return agent.air->NearestSurface(a, surface_reach, pa, na) &&
           agent.air->NearestSurface(b, surface_reach, pb, nb) && float(na.dot(nb)) > 0.9f;
  };
  const int phase = int((m_stepCount + agent.order) % uint64_t(kAirCheckSteps));
  if (phase == 0 && agent.air_next + 1 < path.size() &&
      same_surface(path[agent.air_next], path[agent.air_next + 1]) &&
      clear(seen_from, path[agent.air_next + 1]))
  {
    agent.air_next++;
  }
  /* (A crawler's leg ahead is checked by the level's change notes only: from its body on a
   * corner, the shell's next corner round an edge is not straight.) */
  if (!crawls && phase == kAirCheckSteps / 2 && agent.air_next < path.size() &&
      !clear(position, path[agent.air_next]))
  {
    agent.air_needs_search = true;
  }
  agent.desired = MT_Vector3(0.0f, 0.0f, 0.0f);
  if (path.empty()) {
    /* No way (yet): it waits (a search is due). */
    agent.next_corner = agent.goal;
    agent.remaining = float((agent.goal - position).length());
    return;
  }
  const MT_Vector3 target = aim(path[std::min(agent.air_next, path.size() - 1)]);
  float remaining = float((target - position).length());
  for (size_t i = agent.air_next + 1; i < path.size(); i++) {
    remaining += float((path[i] - path[i - 1]).length());
  }
  agent.next_corner = target;
  agent.remaining = remaining;
  MT_Vector3 dir = target - position;
  if (agent.floating) {
    dir[2] = 0.0f;
  }
  if (crawls) {
    agent.crawl_heading = dir;
  }
  if (crawls && agent.on_surface) {
    /* Along the plane under it toward the corner (a corner up a wall: to the wall's foot; down a
     * wall to the floor: down it, at its pace); at the junction its surface changes to the one
     * the way to the corner runs along (SenseSurfaces, by crawl_heading) and it goes on along
     * that. Straight for the corner only when nothing of it lies along the plane. (Straight
     * whenever the corner lay mostly off the plane, the floor or the wall ate the move and a flat
     * crawler crept: 14 s down a 1 m face in the game; a Character's walk is across its up:
     * straight is nothing for it.) */
    const MT_Vector3 along = dir - agent.surface_normal * float(dir.dot(agent.surface_normal));
    const float length = float(dir.length());
    if (float(along.length()) > 1e-3f) {
      dir = along.normalized() * length;
    }
  }
  const float dist = float(dir.length());
  if (dist < 1e-4f) {
    return;
  }
  /* Slowing for the way's end (not for its corners). */
  float scale = 1.0f;
  if (agent.air_next + 1 >= path.size()) {
    const float size = crawls ? agent.radius : agent.air_radius;
    const float slow = std::max(size * 2.0f, agent.request.arrive_distance * 2.0f);
    scale = std::clamp(remaining / std::max(slow, 1e-3f), 0.2f, 1.0f);
  }
  agent.desired = dir * (speed * scale / dist);
}

void KX_Navigation::AvoidAir(KX_NavAgent &agent, const float dt)
{
  /* Three-dimensional ORCA (RVO2-3D's method) among its neighbours, flyers or not, each a
   * sphere; a longer look ahead than walkers' (3 s: flyers keep their speed); its velocity
   * changing at most as walkers' do. Walls: the way keeps off them, the move through free air
   * (AirSlideTarget) stops at them. */
  thread_local std::vector<kx_orca3d::Neighbour> neighbours;
  thread_local std::vector<kx_orca3d::Plane> planes, scratch;
  neighbours.clear();
  const auto v3 = [](const MT_Vector3 &v) {
    return kx_orca3d::Vec3(float(v[0]), float(v[1]), float(v[2]));
  };
  for (const uint32_t b : agent.neighbours) {
    const Body &other = m_step->bodies[b];
    kx_orca3d::Neighbour n;
    n.position = v3(other.position);
    n.velocity = v3(other.velocity);
    n.radius = other.radius;
    if (!other.avoids) {
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
  const kx_orca3d::Params params{3.0f, dt};
  const float max_speed = std::max(agent.request.speed, float(agent.desired.length()));
  const kx_orca3d::Vec3 result = kx_orca3d::solve(v3(agent.position),
                                                  v3(agent.commanded),
                                                  v3(agent.desired),
                                                  agent.crawls ? agent.radius : agent.air_radius,
                                                  max_speed,
                                                  neighbours.data(),
                                                  neighbours.size(),
                                                  params,
                                                  planes,
                                                  scratch);
  MT_Vector3 chosen(result.x, result.y, result.z);
  if (agent.floating) {
    chosen[2] = 0.0f;
  }
  if (agent.crawls && agent.on_surface) {
    /* Crawling: it dodges along its surface, never off it (the way's own climb kept: in a
     * corner the wall's up is off the floor's plane). */
    const MT_Vector3 dodge = chosen - agent.desired;
    chosen = agent.desired + dodge - agent.surface_normal * float(dodge.dot(agent.surface_normal));
  }
  const float accel = 8.0f + agent.request.speed * 2.0f;
  const MT_Vector3 change = chosen - agent.commanded;
  const float change_len = float(change.length());
  if (change_len > accel * dt && change_len > 0.0f) {
    chosen = agent.commanded + change * (accel * dt / change_len);
  }
  const float chosen_len = float(chosen.length());
  if (chosen_len > max_speed && chosen_len > 0.0f) {
    chosen *= max_speed / chosen_len;
  }
  if (!std::isfinite(float(chosen[0])) || !std::isfinite(float(chosen[1])) ||
      !std::isfinite(float(chosen[2])))
  {
    chosen = agent.desired;
  }
  agent.new_velocity = chosen;
}

bool KX_Navigation::AirSlideTarget(const KX_NavAgent &agent,
                                   const MT_Vector3 &wanted,
                                   MT_Vector3 &r_target) const
{
  /* Through free air: the whole move, else the move without one axis, else along one axis
   * (it slides along a wall or the floor), else it stays. Outside free air (it started inside
   * a wall's reach, a fish above the water) it heads for the nearest free point; with none near
   * (placed beside the level, below its top: no way round there) straight up out of it. */
  const MT_Vector3 &from = agent.position;
  const KX_NavAirMap &air = *agent.air;
  const MT_Vector3 d = wanted - from;
  r_target = from;
  if (!air.Free(from, agent.water)) {
    MT_Vector3 out;
    if (air.NearestFree(from, 2.0f + agent.air_radius * 2.0f, out, agent.water, kAirUpReach)) {
      const MT_Vector3 to_out = out - from;
      const float step = std::max(float(d.length()), 0.02f);
      const float len = float(to_out.length());
      r_target = len <= step ? out : from + to_out * (step / len);
    }
    return true;
  }
  if (air.LineFree(from, wanted, agent.water)) {
    r_target = wanted;
    return true;
  }
  const MT_Vector3 tries[6] = {MT_Vector3(d[0], d[1], 0.0f),
                               MT_Vector3(d[0], 0.0f, d[2]),
                               MT_Vector3(0.0f, d[1], d[2]),
                               MT_Vector3(d[0], 0.0f, 0.0f),
                               MT_Vector3(0.0f, d[1], 0.0f),
                               MT_Vector3(0.0f, 0.0f, d[2])};
  for (const MT_Vector3 &t : tries) {
    if (t.length2() > 1e-12f && air.LineFree(from, from + t, agent.water)) {
      r_target = from + t;
      return true;
    }
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Walls and ceilings (phase 9, 2026-11-19)
 * \{ */

KX_NavAirMap *KX_Navigation::SurfaceMapFor(const float radius, LN_JobPool *pool)
{
  KX_NavAirMap *map = AirMapFor(radius, pool);
  if (map != nullptr && !map->HasSurfaces()) {
    /* (A floor is as steep as the scene's walkable slope: Scene > Navigation Mesh.) */
    const float slope = m_settings.agent_max_slope > 0.0f ? m_settings.agent_max_slope :
                                                            0.785398f;
    KX_NavGeometry geometry;
    CollectAirLevel(MT_Vector3(-1e6f, -1e6f, -1e6f), MT_Vector3(1e6f, 1e6f, 1e6f), geometry);
    map->EnableSurfaces(geometry, std::cos(std::min(slope, 1.5f)), pool);
  }
  return map;
}

/** How near a surface is to touch a crawler: its body's height off it; a body that collides (a
 * Dynamic moved by its velocity, a Character) is stopped by a wall at its side too, so its
 * width counts there (2026-11-19: a beetle longer than tall pressed on a wall's foot for good,
 * its nose 0.35 m from the middle, the wall never touching it; found by the examples). */
static float crawl_touch(const KX_NavAgent &agent)
{
  const float side = agent.drive != KX_NavAgent::Drive::Slide ? agent.radius : 0.0f;
  return std::max(agent.crawl_offset, side) + 0.05f;
}

void KX_Navigation::SenseSurfaces(LN_JobPool *pool)
{
  /* Each crawler's surface: of the surfaces touching it, the one its way (the last step's)
   * runs along (SupportSurface: a capsule touches the wall beside it nearer than the floor
   * under it); none near, the nearest up to 4 m (one placed in the air finds the surface below
   * it). The way out of it toward the crawler (the last step's normal decides on the surface
   * itself). Read-only on the helpers. */
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    if (!agent.crawls || agent.air == nullptr) {
      agent.on_surface = false;
      return;
    }
    const MT_Vector3 hint = agent.surface_normal;
    const float touch_reach = agent.crawl_offset + agent.radius + 0.5f;
    /* (By the way straight to its corner, not its wish along the plane: at a wall's foot with
     * its corner up the wall and along, the wish runs along the floor and the wall alike, and a
     * crowd walked the foot in an L, meeting head-on in its strip; a Character's walk runs along
     * its surface always, down a face too, and only the corner's way tells it the floor is
     * next.) */
    agent.on_surface = agent.air->SupportSurface(agent.position,
                                                 touch_reach,
                                                 crawl_touch(agent),
                                                 agent.crawl_heading,
                                                 agent.surface_point,
                                                 agent.surface_normal,
                                                 &hint) ||
                       agent.air->NearestSurface(agent.position,
                                                 std::max(touch_reach, 4.0f),
                                                 agent.surface_point,
                                                 agent.surface_normal,
                                                 &hint);
    if (!agent.on_surface) {
      agent.surface_normal = MT_Vector3(0.0f, 0.0f, 1.0f);
    }
  });
}

bool KX_Navigation::CrawlTarget(const KX_NavAgent &agent,
                                const MT_Vector3 &wanted,
                                MT_Vector3 &r_target) const
{
  /* The move, then its body back at its height from the surface there (round a convex edge
   * the nearest point is the edge: it wraps round it; into a corner the wall is nearer: it goes
   * up it). Onto a ceiling it may not use, or off every surface: it stays. One that came off its
   * surface (placed in the air, pushed off) goes back at its speed, never at once. */
  r_target = agent.position;
  if (agent.air == nullptr) {
    return false;
  }
  const MT_Vector3 hint = agent.surface_normal;
  MT_Vector3 point, normal;
  const float touch_reach = agent.crawl_offset + agent.radius + 0.5f;
  if (!agent.air->SupportSurface(wanted,
                                 touch_reach,
                                 crawl_touch(agent),
                                 wanted - agent.position,
                                 point,
                                 normal,
                                 &hint) &&
      !agent.air->NearestSurface(wanted, std::max(touch_reach, 4.0f), point, normal, &hint))
  {
    return true;
  }
  if (!(agent.crawl_kinds & KX_NavAirMap::SurfaceCeiling) && float(normal[2]) <= -0.5f) {
    return true;
  }
  MT_Vector3 placed = point + normal * agent.crawl_offset;
  /* Its middle kept clear of every other surface by its width or height, the smaller (a wall
   * beside a flat crawler on the floor stops it, never takes it in). */
  const float clearance = std::min(agent.radius, agent.crawl_offset) - 1e-3f;
  MT_Vector3 side_point, side_normal;
  if (clearance > 0.0f &&
      agent.air->NearestSurface(placed, clearance, side_point, side_normal, &normal))
  {
    const float gap = float((placed - side_point).length());
    placed += side_normal * (clearance - gap);
  }
  const float step = std::max(float((wanted - agent.position).length()), 0.02f);
  const float jump = float((placed - wanted).length());
  r_target = jump > step * 2.0f ? wanted + (placed - wanted) * (step / jump) : placed;
  return true;
}

/** \} */

void KX_Navigation::SetWaterForTests(std::vector<KX_NavWaterVolume> water)
{
  /* (Taken as a scene's Buoyancy Volumes are: by the first map, then by the check every
   * kWaterCheckSteps.) */
  m_waterForTests = std::move(water);
  m_hasWaterForTests = true;
}

KX_NavAirMap *KX_Navigation::AirMapForTests(const float radius)
{
  return AirMapFor(radius, nullptr);
}

void KX_Navigation::SetAgentSizeForTests(KX_GameObject *object,
                                         const float radius,
                                         const float crawl_offset)
{
  KX_NavAgent &agent = GetOrCreateAgent(object);
  agent.radius = radius;
  agent.air_radius = radius;
  agent.size.radius = radius;
  agent.crawl_offset = crawl_offset;
}

float KX_Navigation::AgentRadiusOf(const KX_GameObject *object) const
{
  /* (Asked by the air queries: the sphere a flyer of its size fills.) */
  if (const KX_NavAgent *agent = FindAgent(object)) {
    return agent->air_radius;
  }
  return m_settings.agent_radius > 0.0f ? m_settings.agent_radius : 0.5f;
}

bool KX_Navigation::PassesThroughOthers(const KX_NavAgent &agent)
{
  if (!(agent.requested || agent.on_link) || agent.arrived || agent.finished || agent.waiting) {
    return false;
  }
  PHY_IPhysicsController *controller = agent.object->GetPhysicsController();
  return controller != nullptr && (controller->IsCharacter() || !controller->IsDynamic());
}

MT_Vector3 KX_Navigation::PassThroughSeparation(const size_t index) const
{
  /* Item 12 (2026-11-20): navigating Characters pass through each other's bodies, so physics no
   * longer keeps them apart: overlapping another one, each walks out of half the overlap in
   * kPassThroughSeparationTime (as the push apart moves sliding NPCs out of theirs), at most at
   * its walking speed. From the step's start places (the same on any number of threads). Without
   * it two Characters in a squeeze stayed 10+ cm inside each other for up to 2 s, sliding NPCs
   * at most 0.15 s. */
  const KX_NavAgent &agent = *m_agents[index];
  const std::vector<Body> &bodies = m_step->bodies;
  if (index >= bodies.size() || !PassesThroughOthers(agent)) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  MT_Vector3 push(0.0f, 0.0f, 0.0f);
  const MT_Vector3 position = bodies[index].position;
  for (const uint32_t b : agent.neighbours) {
    const Body &other = bodies[b];
    if (other.agent == UINT32_MAX || other.agent == index) {
      continue;
    }
    const KX_NavAgent &them = *m_agents[other.agent];
    if (them.drive != KX_NavAgent::Drive::Walk || !PassesThroughOthers(them) ||
        them.frame != agent.frame)
    {
      continue;
    }
    const MT_Vector3 offset = flat(position - other.position);
    const float dist = float(offset.length());
    /* (Full radii, also in Dense: with SolveDense's squeezed reach the deep overlaps stayed,
     * 38 pair-steps over 20 cm in the Character door test against 1; 2026-11-20.) */
    const float overlap = agent.radius + them.radius - dist;
    if (overlap <= 0.0f) {
      continue;
    }
    const MT_Vector3 dir = dist > 1e-4f ? offset / dist :
                                          MT_Vector3(other.agent < index ? 1.0f : -1.0f, 0, 0);
    push += dir * (overlap * 0.5f);
  }
  MT_Vector3 velocity = push / kPassThroughSeparationTime;
  const float most = std::max(agent.request.speed, 0.5f);
  const float length = float(velocity.length());
  if (length > most) {
    velocity *= most / length;
  }
  return velocity;
}

void KX_Navigation::HandOverNavigatingBodies()
{
  /* Item 12 (2026-11-20): the bodies of the NPCs moved by the navigation in the coming physics
   * step that physics cannot push (Characters, Kinematic and Static NPCs; a Dynamic NPC is pushed
   * by the others' mass as before): a Character among them passes through the others. Standing
   * ones (arrived, waiting at a closed link, not asked) are met as before. */
  m_navigatingScratch.clear();
  for (const std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    if (PassesThroughOthers(*holder)) {
      m_navigatingScratch.push_back(holder->object->GetPhysicsController());
    }
  }
  if (m_navigatingScratch.empty() && !m_navigatingHandedOver) {
    return;
  }
  if (PHY_IPhysicsEnvironment *physics = Physics()) {
    physics->SetNavigatingBodies(m_navigatingScratch);
  }
  m_navigatingHandedOver = !m_navigatingScratch.empty();
}

float KX_Navigation::AgentWalkRadiusOf(const KX_GameObject *object) const
{
  if (const KX_NavAgent *agent = FindAgent(object)) {
    return agent->radius;
  }
  return m_settings.agent_radius > 0.0f ? m_settings.agent_radius : 0.5f;
}

bool KX_Navigation::ChasePlaceOf(const KX_GameObject *object,
                                 float &r_angle,
                                 float &r_radius) const
{
  const KX_NavAgent *agent = FindAgent(object);
  if (agent == nullptr || !agent->chase_place) {
    return false;
  }
  r_angle = agent->chase_angle;
  r_radius = agent->chase_radius;
  return true;
}

bool KX_Navigation::AirNearestPoint(const MT_Vector3 &p,
                                    const float radius,
                                    const bool water,
                                    MT_Vector3 &r_point)
{
  const KX_NavAirMap *map = AirMapForQuery(radius);
  return map != nullptr &&
         map->NearestFree(p, std::max(4.0f, radius * 4.0f), r_point, water);
}

bool KX_Navigation::AirLineClear(const MT_Vector3 &a,
                                 const MT_Vector3 &b,
                                 const float radius,
                                 const bool water)
{
  const KX_NavAirMap *map = AirMapForQuery(radius);
  return map != nullptr && map->LineFree(a, b, water);
}

bool KX_Navigation::AirFindPath(const MT_Vector3 &a,
                                const MT_Vector3 &b,
                                const float radius,
                                const bool water,
                                std::vector<MT_Vector3> &r_points,
                                bool *r_complete)
{
  r_points.clear();
  if (r_complete != nullptr) {
    *r_complete = false;
  }
  const KX_NavAirMap *map = AirMapForQuery(radius);
  return map != nullptr && map->FindPath(a, b, r_points, r_complete, water, kAirSearchNodes);
}

/** \} */

void KX_Navigation::HandOver(const float dt, LN_JobPool *pool)
{
  PHY_IPhysicsEnvironment *physics = Physics();
  /* The slides' Detour work on the helpers (each agent's own corridor), the writes below in
   * agent order (2026-11-18: 10,000 sliding NPCs spent 2 ms here on the main thread). */
  RunJobs(pool, m_agents.size(), [&](const size_t index) {
    KX_NavAgent &agent = *m_agents[index];
    agent.slide_ok = false;
    if (agent.requested && agent.has_goal &&
        (!agent.arrived || agent.dense_pushed || agent.making_way) &&
        !agent.on_link && !agent.link_started && agent.drive == KX_NavAgent::Drive::Slide)
    {
      const MT_Vector3 wanted = agent.position + agent.new_velocity * dt;
      agent.slide_ok = agent.crawls          ? CrawlTarget(agent, wanted, agent.slide_target) :
                       agent.air != nullptr ? AirSlideTarget(agent, wanted, agent.slide_target) :
                                              SlideTarget(agent, wanted, true, agent.slide_target);
    }
  });
  for (size_t index = 0; index < m_agents.size(); index++) {
    KX_NavAgent &agent = *m_agents[index];
    KX_GameObject *object = agent.object;
    const bool active = agent.requested && agent.has_goal;
    /* What it was told last step ends when it stops being asked (or arrives; Avoidance Dense:
     * unless the crowd shoved it this step). */
    const bool moving = active && (!agent.arrived || agent.dense_pushed || agent.making_way);
    if (active && agent.crawls && agent.air != nullptr && physics != nullptr &&
        agent.drive != KX_NavAgent::Drive::Slide)
    {
      /* Crawling with a body (phase 9), moving or arrived and still asked (it holds on): a
       * Dynamic body by its velocity along the surface, its gravity of the step undone and drawn
       * back to its height from the surface; a Character by its walk, its gravity turned toward
       * the surface (given back when it stops crawling: PrepareAgents). */
      const MT_Vector3 along = moving ? agent.new_velocity : MT_Vector3(0.0f, 0.0f, 0.0f);
      if (agent.drive == KX_NavAgent::Drive::Velocity) {
        MT_Vector3 gravity(0.0f, 0.0f, 0.0f);
        physics->GetGravity(gravity);
        const PHY_IPhysicsController *controller = object->GetPhysicsController();
        const float factor = controller != nullptr ? controller->GetGravityFactor() : 1.0f;
        MT_Vector3 v = along - gravity * (dt * factor);
        if (agent.on_surface) {
          const float dist = float((agent.position - agent.surface_point).length());
          v += agent.surface_normal *
               std::clamp((agent.crawl_offset - dist) * 0.5f / std::max(dt, 1e-4f), -3.0f, 3.0f);
        }
        object->setLinearVelocity(v, false);
      }
      else if (PHY_ICharacter *character = physics->GetCharacterController(object)) {
        if (agent.on_surface) {
          if (!agent.gravity_saved) {
            agent.saved_gravity = character->GetGravity();
            agent.gravity_saved = true;
          }
          const MT_Vector3 turned = agent.surface_normal *
                                    -float(agent.saved_gravity.length());
          if ((turned - character->GetGravity()).length2() > 1e-6f) {
            character->SetGravity(turned);
          }
        }
        character->SetWalkDirection(along);
      }
      agent.was_active = true;
      if (!moving) {
        agent.new_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
      }
      continue;
    }
    if (!moving && active && agent.flies && agent.air != nullptr && !agent.floating &&
        agent.drive == KX_NavAgent::Drive::Velocity && physics != nullptr)
    {
      /* A Dynamic flyer arrived, still asked: it hovers there (its gravity of this step undone;
       * no longer asked, it is a body like any other and falls). */
      MT_Vector3 gravity(0.0f, 0.0f, 0.0f);
      physics->GetGravity(gravity);
      const PHY_IPhysicsController *controller = object->GetPhysicsController();
      const float factor = controller != nullptr ? controller->GetGravityFactor() : 1.0f;
      /* (Riding a deck, plan item 11: with the deck where it is.) */
      object->setLinearVelocity(agent.frame_velocity + gravity * (-dt * factor), false);
      agent.was_active = true;
      agent.new_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
      continue;
    }
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
    if (agent.link_physics && (agent.on_link || agent.link_started)) {
      /* A physics jump (2026-11-19): launched once, then steered through the air; nothing
       * moves it but its physics. */
      if (agent.drive == KX_NavAgent::Drive::Walk && physics != nullptr) {
        if (PHY_ICharacter *character = physics->GetCharacterController(object)) {
          character->SetWalkDirection(agent.link_steer);
          if (agent.link_launch) {
            character->Launch(agent.link_up_speed);
          }
        }
      }
      else if (agent.drive == KX_NavAgent::Drive::Velocity) {
        MT_Vector3 v = object->GetLinearVelocity(false);
        v[0] = agent.link_steer[0];
        v[1] = agent.link_steer[1];
        if (agent.link_launch) {
          v[2] = agent.link_up_speed;
        }
        object->setLinearVelocity(v, false);
      }
      agent.link_launch = false;
      agent.new_velocity = agent.link_steer;
      continue;
    }
    if (agent.on_link || agent.link_started || agent.link_frames_landing) {
      /* Over a link it is carried (a Character included: it walks on from the end). */
      agent.link_frames_landing = false;
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
    if (agent.flies) {
      /* Flying or swimming: a Dynamic body by its velocity (its gravity of this step undone: it
       * gets the velocity first, then moves; floating, its height left to the buoyancy), else
       * through free air. A Character (it cannot) stands. */
      if (agent.air == nullptr) {
        if (agent.drive == KX_NavAgent::Drive::Walk && physics != nullptr) {
          if (PHY_ICharacter *character = physics->GetCharacterController(object)) {
            character->SetWalkDirection(MT_Vector3(0.0f, 0.0f, 0.0f));
          }
        }
      }
      else if (agent.drive == KX_NavAgent::Drive::Velocity) {
        /* (On a deck, plan item 11: over it, the deck's velocity where it is added.) */
        MT_Vector3 v = agent.new_velocity + agent.frame_velocity;
        if (agent.floating) {
          v[2] = object->GetLinearVelocity(false)[2];
        }
        else if (physics != nullptr) {
          MT_Vector3 gravity(0.0f, 0.0f, 0.0f);
          physics->GetGravity(gravity);
          const PHY_IPhysicsController *controller = object->GetPhysicsController();
          const float factor = controller != nullptr ? controller->GetGravityFactor() : 1.0f;
          v -= gravity * (dt * factor);
        }
        object->setLinearVelocity(v, false);
      }
      else if (agent.slide_ok) {
        WriteSlide(agent, agent.slide_target);
      }
      continue;
    }
    switch (agent.drive) {
      case KX_NavAgent::Drive::Walk:
        if (physics != nullptr) {
          if (PHY_ICharacter *character = physics->GetCharacterController(object)) {
            character->SetWalkDirection(flat(agent.new_velocity) + PassThroughSeparation(index));
          }
        }
        break;
      case KX_NavAgent::Drive::Velocity: {
        /* (Over its deck on a vehicle: the deck's velocity where it stands added.) */
        const MT_Vector3 v = object->GetLinearVelocity(false);
        object->setLinearVelocity(MT_Vector3(agent.new_velocity[0] + agent.frame_velocity[0],
                                             agent.new_velocity[1] + agent.frame_velocity[1],
                                             v[2]),
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
    /* (Not one carried over a link: its push slid along the navmesh it left and put it back at
     * the link's start, 2026-11-20, a crowd at a gangway.) */
    if (agent.requested && agent.drive == KX_NavAgent::Drive::Slide && agent.has_written &&
        agent.request.avoid && !agent.request.dense && !agent.flies && !agent.crawls &&
        !agent.on_link && !agent.link_ended)
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
        /* Half each between two that both give way, all of it against one that won't (a Dense
         * agent moved by its own solve takes no part here). */
        const bool shared = other.agent != UINT32_MAX && other.avoids &&
                            m_agents[other.agent]->drive == KX_NavAgent::Drive::Slide &&
                            !m_agents[other.agent]->request.dense;
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
    /* (Its wait at a closed link counts only while it waits there step after step.) */
    if (!agent.waiting) {
      agent.wait_link = 0;
      agent.wait_seconds = 0.0f;
    }
    /* Waits it gave up on that no longer hold (the link opened, or closed again since). */
    if (!agent.filter.gave_up.empty()) {
      std::erase_if(agent.filter.gave_up, [this](const std::pair<uint32_t, uint32_t> &g) {
        const KX_NavClosedLink *closed = FindClosedLink(g.first);
        return closed == nullptr || closed->generation != g.second;
      });
    }
    KX_NavAgentInfo &info = agent.info;
    const bool link_started = info.link_started;
    (void)link_started;
    info.active = agent.requested && agent.has_goal;
    /* An Until Arrived order that arrived keeps Arrived on until a new order. (On the way over
     * frames: never arrived before the goal's frame.) */
    info.arrived = ((info.active && agent.arrived) || agent.finished) && !agent.routing;
    /* When Reached: once an arrival; again after it walked off, or for a new order. */
    if (info.active && !info.arrived) {
      agent.reach_armed = true;
    }
    info.reached = info.arrived && agent.reach_armed;
    if (info.reached) {
      agent.reach_armed = false;
      info.reached_issuer = agent.request.issuer;
      info.reached_tag = agent.request.tag;
    }
    info.moving = info.active && !agent.arrived && agent.new_velocity.length2() > 1e-8f;
    if (agent.air != nullptr) {
      /* Flying or swimming: its way through the air. */
      info.has_path = !agent.air_path.empty();
      info.path_complete = info.has_path && agent.air_complete;
      info.no_path = info.active && !agent.air_needs_search && agent.air_path.empty();
    }
    else {
      info.has_path = agent.corridor_ready && agent.navmesh != nullptr;
      info.path_complete = info.has_path && agent.path_complete;
      info.no_path = agent.no_path || (info.active && agent.navmesh == nullptr && !agent.flies &&
                                       (agent.request.navmesh != nullptr ||
                                        m_settings.build_mode != 0));
    }
    /* When Failed and Reason (plan item 8, 2026-11-20): why its order cannot be done; When
     * Failed pulses once an order and reason. Unreachable: it stands as near as it gets (When
     * Reached pulses too). */
    /* At the end of a partial way (the goal beyond every polygon it can walk to: an island, a
     * sealed room) it stands without pressing on: a second there is Unreachable. (Around NPCs a
     * partial way ahead is searched again as tiles come: only standing at its end counts.) */
    bool at_partial_end = false;
    if (agent.air == nullptr && !agent.crawls && info.active && !info.arrived && info.has_path &&
        !info.path_complete && agent.corridor_ready && agent.navmesh != nullptr &&
        !agent.on_link && !agent.waiting)
    {
      const MT_Vector3 end = agent.navmesh->ToWorld(agent.corridor.getTarget());
      at_partial_end = distance_2d(agent.position, end) <=
                       std::max(agent.request.arrive_distance, agent.radius) + 0.15f;
    }
    agent.partial_end_time = at_partial_end ? agent.partial_end_time + m_step->last_dt : 0.0f;
    uint8_t failure = KX_NAV_FAILURE_NONE;
    if (agent.gave_up) {
      failure = KX_NAV_FAILURE_GAVE_UP;
    }
    else if (info.active && !info.arrived) {
      if (agent.off_navmesh) {
        failure = KX_NAV_FAILURE_OFF_NAVMESH;
      }
      else if (info.no_path) {
        failure = KX_NAV_FAILURE_NO_PATH;
      }
      else if (agent.partial_end_time >= kUnreachableSeconds) {
        failure = KX_NAV_FAILURE_UNREACHABLE;
      }
      else if (agent.stall_time > kStuckSeconds && !agent.waiting && !agent.on_link) {
        failure = KX_NAV_FAILURE_STUCK;
      }
    }
    else if (info.active && info.arrived && info.has_path && !info.path_complete) {
      failure = KX_NAV_FAILURE_UNREACHABLE;
    }
    agent.gave_up = false;
    info.failure = failure;
    info.failed = failure != KX_NAV_FAILURE_NONE && failure != agent.failure_reported;
    if (info.failed) {
      agent.failure_reported = failure;
      info.failed_issuer = agent.request.issuer;
    }
    info.position = agent.position;
    info.next_corner = agent.next_corner;
    info.goal = agent.routing                        ? agent.final_goal :
                agent.has_goal || agent.finished ? agent.goal :
                                                   agent.position;
    /* Vehicles (2026-11-19): what it stands on, a closed link it waits at. */
    info.vehicle = agent.frame;
    info.waiting = agent.waiting;
    info.waiting_at = agent.waiting ? agent.waiting_at : nullptr;
    info.velocity = agent.new_velocity;
    info.desired_velocity = agent.desired;
    info.speed = agent.standing ? 0.0f : agent.smooth_speed;
    info.remaining_distance = agent.remaining;
    info.surface_normal = agent.crawls ? (agent.on_surface ? agent.surface_normal :
                                                             MT_Vector3(0.0f, 0.0f, 0.0f)) :
                          agent.flies ? MT_Vector3(0.0f, 0.0f, 0.0f) :
                                        MT_Vector3(0.0f, 0.0f, 1.0f);
    info.on_link = agent.on_link || agent.link_started;
    info.link_started = agent.link_started;
    info.link_ended = agent.link_ended;
    info.link_landed = agent.link_ended && agent.link_landed;
    if (info.on_link || info.link_ended) {
      info.link_object = agent.link_object;
      info.link_kind = agent.link_kind;
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
          [this](const KX_GameObject *object) { return IsExcludedFromLevel(object); },
          r_geometry);
    };
  }
  if (source) {
    if (m_settings.auto_cells) {
      /* Auto Cells (2026-11-20): from the smallest NPC on the scene's navmesh now (else the
       * fallback size): its radius / 2 within 0.1 .. 0.3 m, so its radius is whole cells and a
       * door 1.5 NPCs wide stays open (at 0.2 m cells a 0.3 m NPC counted as 0.4 m: a 0.9 m door
       * was closed). The cell height is half the cell. */
      float smallest = 0.0f;
      const KX_NavAgent *from = nullptr;
      for (const std::unique_ptr<KX_NavAgent> &agent : m_agents) {
        if (agent->request.navmesh != nullptr || !agent->request.use_scene_navmesh ||
            agent->request.move != KX_NavRequest::Move::Walk ||
            agent->request.climb != KX_NavRequest::Climb::Floors || agent->radius <= 0.0f)
        {
          continue;
        }
        if (from == nullptr || agent->radius < smallest) {
          smallest = agent->radius;
          from = agent.get();
        }
      }
      const float radius = from != nullptr ? smallest :
                           m_settings.agent_radius > 0.0f ? m_settings.agent_radius :
                                                            0.5f;
      m_settings.cell_size = AutoCellSize(radius);
      m_settings.cell_height = m_settings.cell_size * 0.5f;
      char text[160];
      if (from != nullptr) {
        snprintf(text,
                 sizeof(text),
                 "%.2f m: for '%s', radius %.2f m",
                 m_settings.cell_size,
                 from->object->GetName().c_str(),
                 radius);
      }
      else {
        snprintf(text,
                 sizeof(text),
                 "%.2f m: for the fallback NPC size, radius %.2f m",
                 m_settings.cell_size,
                 radius);
      }
      m_autoCellReason = text;
    }
    m_build.reset(KX_NavBuildCreate(m_settings, std::move(source)));
    /* Jumps and drops in areas of their own (priced, closed per NPC). */
    m_jumpArea = AreaId("jump");
    m_dropArea = AreaId("drop");
    KX_NavBuildSetLinkAreas(*m_build, uint8_t(m_jumpArea), uint8_t(m_dropArea));
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
  /* The default size always (queries read it), every agent class asked for (built below). */
  KX_NavBuildNavMesh(*m_build, 0.0f);
  std::vector<MT_Vector3> positions;
  std::vector<KX_NavAgentSize> sizes;
  std::vector<uint64_t> ids;
  positions.reserve(m_agents.size());
  sizes.reserve(m_agents.size());
  ids.reserve(m_agents.size());
  for (const std::unique_ptr<KX_NavAgent> &agent : m_agents) {
    /* Agents walking a navmesh object build nothing; nor do flyers, swimmers and crawlers (the
     * air map). */
    if (agent->request.navmesh != nullptr || !agent->request.use_scene_navmesh ||
        agent->flies || agent->crawls)
    {
      continue;
    }
    if (agent->requested) {
      KX_NavBuildNavMesh(*m_build, agent->size);
    }
    /* On a vehicle under way (2026-11-20): nothing of the world under it (a ship at sea built
     * the seabed's tiles all along its voyage); only where it may step off: its vehicle's links'
     * world ends and its goal (each a number of its own). Standing still (docked), as any NPC:
     * the world round it (its docking links are found there). */
    if (agent->frame != nullptr && m_settings.build_mode == blender::NAVMESH_BUILD_AROUND_NPCS) {
      const KX_NavVehicle *vehicle = FindVehicleRecord(agent->frame);
      if (vehicle != nullptr && vehicle->still_steps == 0) {
        uint64_t extra = 0;
        auto add = [&](const MT_Vector3 &p) {
          positions.push_back(p);
          sizes.push_back(agent->size);
          ids.push_back((uint64_t(1) << 63) | (agent->order << 8) | (extra++ & 0xff));
        };
        for (const KX_NavFrameLink &link : m_frameLinks) {
          if (link.automatic) {
            continue;
          }
          if (link.start_frame == agent->frame && link.end_frame == nullptr) {
            add(link.end);
          }
          else if (link.end_frame == agent->frame && link.start_frame == nullptr) {
            add(link.start);
          }
        }
        if (agent->routing) {
          add(agent->final_goal);
        }
        continue;
      }
    }
    positions.push_back(agent->object->NodeGetWorldPosition());
    sizes.push_back(agent->size);
    ids.push_back(agent->order);
  }
  /* (Links between frames go round Detour: the world's only.) */
  KX_NavBuildUpdate(*m_build,
                    positions,
                    sizes,
                    ids,
                    m_step->carve,
                    m_worldLinks,
                    pool);
  CheckSceneNavMeshProblems();
}

float KX_Navigation::AutoCellSize(const float radius)
{
  return std::clamp(radius * 0.5f, 0.1f, 0.3f);
}

float KX_Navigation::BuiltCellSize() const
{
  return m_build ? m_settings.cell_size : 0.0f;
}

void KX_Navigation::CheckSceneNavMeshProblems()
{
  /* (Plan item 9, 2026-11-20: the setup mistakes the scene's navmesh shows, each once.) */
  if (!m_build || m_settings.build_mode == blender::NAVMESH_BUILD_AROUND_NPCS) {
    return;
  }
  const KX_NavMesh *level = KX_NavBuildFindNavMesh(*m_build, 0.0f);
  if (m_settings.auto_build && !m_autoEmptyChecked && level != nullptr) {
    m_autoEmptyChecked = true;
    if (level->PolygonCount() == 0) {
      ReportProblem("nothing_to_walk_on",
                    "",
                    "the scene's navigation mesh is empty: no static object with a collision "
                    "shape to walk on (Physics type Static, a floor)");
    }
  }
  const float cell = m_settings.cell_size;
  for (const std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    KX_NavAgent &agent = *holder;
    if (agent.size_checked || !agent.requested || agent.request.navmesh != nullptr ||
        !agent.request.use_scene_navmesh || agent.flies || agent.crawls)
    {
      continue;
    }
    agent.size_checked = true;
    const std::string name = agent.object->GetName();
    const float r = agent.size.radius > 0.0f ? agent.size.radius : m_settings.agent_radius;
    char text[320];
    /* (What to do about it: Auto Cells stop at their finest cell, the build's cost.) */
    const char *advice = m_settings.auto_cells ?
                             "Auto Cells go no finer than 0.10 m (the build's cost); its NPC "
                             "Radius (Physics > Navigation) may be set larger" :
                             "turn on Auto Cells or lower Cell Size (Navigation Mesh > Advanced)";
    if (r < cell * 0.999f) {
      snprintf(text,
               sizeof(text),
               "'%s' (radius %.2f m) is smaller than the navigation mesh's cell (%.2f m): it "
               "keeps %.2f m from walls; %s",
               name.c_str(),
               r,
               cell,
               cell,
               advice);
      ReportProblem("npc_smaller_than_cell", name, text);
    }
    else {
      const float treated = std::ceil(r / cell - 1e-4f) * cell;
      if (treated >= r * 1.25f) {
        snprintf(text,
                 sizeof(text),
                 "'%s' (radius %.2f m) is treated as %.2f m wide on each side at %.2f m cells: "
                 "doors narrower than about %.1f m are closed to it; %s",
                 name.c_str(),
                 r,
                 treated,
                 cell,
                 2.0f * treated + cell,
                 m_settings.auto_cells ? "other NPCs on this navigation mesh are smaller (Auto "
                                         "Cells follow the smallest)" :
                                         advice);
        ReportProblem("npc_rounded_up", name, text);
      }
    }
    if (!KX_NavBuildHasOwnClass(*m_build, agent.size)) {
      snprintf(text,
               sizeof(text),
               "'%s' (radius %.2f m) shares the navigation mesh of another NPC size: at most six "
               "sizes get their own, so it may not fit everywhere its own size would",
               name.c_str(),
               r);
      ReportProblem("npc_sizes_full", name, text);
    }
  }
  /* Hand-placed links whose ends are on no floor (further than Max Climb up or down, or beyond
   * the link's Radius): Detour never joins them. Checked on the default size's navmesh, once a
   * link (a link moved is checked again: `m_linksCheckedSerial`). */
  if (level != nullptr && m_linksNeedCheck) {
    m_linksNeedCheck = false;
    for (const KX_NavLink &link : m_links) {
      if (link.object == nullptr || link.end_object == nullptr ||
          (!m_vehicles.empty() &&
           (VehicleOf(link.object) != nullptr || VehicleOf(link.end_object) != nullptr)))
      {
        continue;
      }
      for (const bool end : {false, true}) {
        const MT_Vector3 &p = end ? link.end : link.start;
        MT_Vector3 nearest;
        const bool found = level->NearestPoint(p, nearest);
        const float across = found ? distance_2d(nearest, p) : 1e9f;
        const float up = found ? std::fabs(float(nearest[2] - p[2])) : 1e9f;
        if (across <= std::max(link.radius, 0.05f) + 0.05f &&
            up <= m_settings.agent_climb + m_settings.cell_height)
        {
          continue;
        }
        KX_GameObject *at = end ? link.end_object : link.object;
        char text[320];
        if (found) {
          snprintf(text,
                   sizeof(text),
                   "link '%s' -> '%s': its %s ('%s') is %.2f m %s the nearest floor NPCs walk "
                   "(%.2f m across): it is never used (put it on the floor, within Max Climb "
                   "%.2f m and the link's Radius %.2f m)",
                   link.object->GetName().c_str(),
                   link.end_object->GetName().c_str(),
                   end ? "end" : "start",
                   at->GetName().c_str(),
                   up,
                   p[2] > nearest[2] ? "above" : "below",
                   across,
                   m_settings.agent_climb,
                   link.radius);
        }
        else {
          snprintf(text,
                   sizeof(text),
                   "link '%s' -> '%s': its %s ('%s') has no floor NPCs walk near it: it is "
                   "never used",
                   link.object->GetName().c_str(),
                   link.end_object->GetName().c_str(),
                   end ? "end" : "start",
                   at->GetName().c_str());
        }
        ReportProblem(end ? "link_end_off_floor" : "link_start_off_floor",
                      link.object->GetName(),
                      text);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

bool KX_Navigation::DrawLine(const MT_Vector3 &a,
                             const MT_Vector3 &b,
                             const MT_Vector4 &color) const
{
  if (m_drawnLines >= m_drawBudget) {
    return false;
  }
  m_drawnLines++;
  if (m_lineSinkForTests) {
    m_lineSinkForTests(a, b, color);
  }
  else {
    KX_RasterizerDrawDebugLine(a, b, color);
  }
  return true;
}

void KX_Navigation::DrawCircle(const MT_Vector3 &center,
                               const float radius,
                               const MT_Vector4 &color,
                               const MT_Vector3 &normal,
                               const int sectors) const
{
  /* (As lines: they count in the budget, tests see them.) */
  MT_Vector3 n = normal.length2() > 1e-8f ? normal.normalized() : MT_Vector3(0, 0, 1);
  MT_Vector3 u = std::fabs(float(n[2])) < 0.9f ? n.cross(MT_Vector3(0, 0, 1)) :
                                                  n.cross(MT_Vector3(1, 0, 0));
  u.normalize();
  const MT_Vector3 v = n.cross(u);
  MT_Vector3 last = center + u * radius;
  for (int i = 1; i <= sectors; i++) {
    const float t = 2.0f * float(M_PI) * float(i) / float(sectors);
    const MT_Vector3 p = center + (u * std::cos(t) + v * std::sin(t)) * radius;
    if (!DrawLine(last, p, color)) {
      return;
    }
    last = p;
  }
}

void KX_Navigation::SetDebugLineSinkForTests(
    std::function<void(const MT_Vector3 &, const MT_Vector3 &, const MT_Vector4 &)> sink)
{
  m_lineSinkForTests = std::move(sink);
}

namespace {

/** Visualization's colours (2026-11-20). */
const MT_Vector4 kAgentColor(0.2f, 0.8f, 1.0f, 1.0f);
const MT_Vector4 kVelocityColor(1.0f, 0.9f, 0.1f, 1.0f);
const MT_Vector4 kPathColor(0.3f, 1.0f, 0.3f, 1.0f);
const MT_Vector4 kObstacleColor(1.0f, 0.4f, 0.2f, 1.0f);
const MT_Vector4 kCarveColor(1.0f, 0.15f, 0.1f, 1.0f);
const MT_Vector4 kWallColor(0.0f, 1.0f, 1.0f, 1.0f);
const MT_Vector4 kVehicleColor(0.6f, 1.0f, 0.6f, 1.0f);
const MT_Vector4 kClosedColor(0.5f, 0.5f, 0.5f, 1.0f);
const MT_Vector4 kWaitColor(1.0f, 0.1f, 0.1f, 1.0f);
const MT_Vector4 kOpenFrameColor(0.1f, 1.0f, 0.2f, 1.0f);
const MT_Vector4 kDockColor(0.2f, 0.9f, 0.9f, 1.0f);

/** An area's colour: ground a dim blue, each named one its own hue (golden-angle steps). */
MT_Vector4 area_color(const int area)
{
  if (area <= 0) {
    return MT_Vector4(0.15f, 0.45f, 0.8f, 1.0f);
  }
  const float h = std::fmod(float(area) * 0.618034f, 1.0f) * 6.0f;
  const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
  float r = 0, g = 0, b = 0;
  switch (int(h)) {
    case 0: r = 1; g = x; break;
    case 1: r = x; g = 1; break;
    case 2: g = 1; b = x; break;
    case 3: g = x; b = 1; break;
    case 4: r = x; b = 1; break;
    default: r = 1; b = x; break;
  }
  return MT_Vector4(0.25f + 0.75f * r, 0.25f + 0.75f * g, 0.25f + 0.75f * b, 1.0f);
}

/** A hand-placed link's colour by its kind. */
MT_Vector4 link_kind_color(const std::string &kind)
{
  if (kind == "jump") {
    return MT_Vector4(1.0f, 0.55f, 0.1f, 1.0f);
  }
  if (kind == "drop") {
    return MT_Vector4(1.0f, 0.2f, 0.9f, 1.0f);
  }
  if (kind == "ladder") {
    return MT_Vector4(1.0f, 0.95f, 0.2f, 1.0f);
  }
  if (kind == "door") {
    return MT_Vector4(0.85f, 0.55f, 0.3f, 1.0f);
  }
  if (kind == "teleport") {
    return MT_Vector4(0.65f, 0.35f, 1.0f, 1.0f);
  }
  if (kind == "gangway") {
    return MT_Vector4(1.0f, 1.0f, 1.0f, 1.0f);
  }
  return MT_Vector4(0.5f, 0.8f, 1.0f, 1.0f);
}

}  // namespace

void KX_Navigation::DrawLinkLine(const MT_Vector3 &start,
                                 const MT_Vector3 &end,
                                 const std::string &kind,
                                 const MT_Vector4 &color,
                                 const bool dashed,
                                 const float radius) const
{
  /* Jumps and drops over their arc, the rest straight; a closed one dashed; its start a ring
   * (its radius). */
  const bool arc = KX_NavLinkKindIsJump(kind);
  const float clearance = arc ? KX_NavJumpClearance(start, end) : 0.0f;
  MT_Vector3 last = start;
  constexpr int kPieces = 12;
  for (int i = 1; i <= kPieces; i++) {
    const float t = float(i) / float(kPieces);
    const MT_Vector3 p = arc ? KX_NavJumpArc(start, end, clearance, t) :
                               start + (end - start) * t;
    if (!dashed || (i % 2) == 1) {
      if (!DrawLine(last, p, color)) {
        return;
      }
    }
    last = p;
  }
  DrawCircle(start, std::max(radius, 0.1f), color, MT_Vector3(0, 0, 1), 12);
}

void KX_Navigation::DrawAgents() const
{
  /* Visualization (2026-11-20, Scene > Navigation Mesh > Visualization): what it shows, within
   * its range of the active camera (0: everywhere), at most its lines a step: the NPCs and their
   * ways first (never cut), obstacles, links, vehicles' navmeshes, then the scene's navmesh
   * (cut first in a huge level). */
  const int hide = m_settings.vis_hide;
  m_drawnLines = 0;
  m_drawBudget = size_t(std::max(m_settings.vis_max_lines, 100));
  const MT_Vector3 up(0.0f, 0.0f, 1.0f);
  MT_Vector3 center = m_cameraForTests;
  if (!m_hasCameraForTests && m_scene != nullptr) {
    KX_Camera *camera = m_scene->GetActiveCamera();
    center = camera ? camera->NodeGetWorldPosition() : MT_Vector3(0, 0, 0);
  }
  const float range = m_settings.vis_range;
  /* (Never a name `near`: <windows.h> defines it as nothing.) */
  const auto in_range = [&](const MT_Vector3 &p) {
    return range <= 0.0f || (p - center).length2() <= range * range;
  };
  for (const std::unique_ptr<KX_NavAgent> &holder : m_agents) {
    const KX_NavAgent &agent = *holder;
    const MT_Vector3 p = agent.object->NodeGetWorldPosition();
    if (!(hide & blender::NAVMESH_VIS_HIDE_AGENTS)) {
      if (agent.crawls) {
        /* A crawler: its circle on its surface. */
        DrawCircle(p, agent.radius, kAgentColor, agent.surface_normal, 16);
      }
      else if (agent.air != nullptr && !agent.floating) {
        /* A flyer or a diving swimmer: its sphere (three circles). */
        const float r = agent.air_radius;
        DrawCircle(p, r, kAgentColor, up, 16);
        DrawCircle(p, r, kAgentColor, MT_Vector3(1.0f, 0.0f, 0.0f), 16);
        DrawCircle(p, r, kAgentColor, MT_Vector3(0.0f, 1.0f, 0.0f), 16);
      }
      else {
        DrawCircle(p, agent.radius, agent.waiting ? kWaitColor : kAgentColor, up, 16);
      }
      if (agent.info.active) {
        DrawLine(p, p + agent.new_velocity, kVelocityColor);
      }
    }
    if (!agent.info.active || (hide & blender::NAVMESH_VIS_HIDE_PATHS)) {
      continue;
    }
    if (agent.air != nullptr && agent.air_next < agent.air_path.size()) {
      /* Its way through the air, corner after corner. */
      MT_Vector3 last = p;
      for (size_t i = agent.air_next; i < agent.air_path.size(); i++) {
        DrawLine(last, agent.air_path[i], kPathColor);
        last = agent.air_path[i];
      }
      if ((last - agent.goal).length2() > 1e-8f) {
        DrawLine(last, agent.goal, kPathColor * 0.6f);
      }
      continue;
    }
    DrawLine(p, agent.next_corner, kPathColor);
    if (agent.next_corner != agent.goal) {
      DrawLine(agent.next_corner, agent.goal, kPathColor * 0.6f);
    }
  }
  if (!(hide & blender::NAVMESH_VIS_HIDE_OBSTACLES)) {
    for (const Body &body : m_step->bodies) {
      if (body.agent == UINT32_MAX && !body.air_only && in_range(body.position)) {
        DrawCircle(body.position, body.radius, kObstacleColor, up, 16);
      }
    }
    /* The boxes cut out of the navmeshes: the scene's, and each vehicle's own (E3). */
    auto box = [&](const KX_NavCarveObstacle &o, const MT_Transform &frame) {
      const MT_Matrix3x3 turn(MT_Quaternion(up, o.yaw));
      MT_Vector3 corners[4];
      for (int i = 0; i < 4; i++) {
        const float sx = (i == 0 || i == 3) ? -1.0f : 1.0f, sy = i < 2 ? -1.0f : 1.0f;
        const MT_Vector3 local = o.center +
                                 turn * MT_Vector3(sx * float(o.half_size[0]),
                                                   sy * float(o.half_size[1]),
                                                   -float(o.half_size[2]));
        corners[i] = frame(local);
      }
      for (int i = 0; i < 4; i++) {
        DrawLine(corners[i], corners[(i + 1) % 4], kCarveColor);
      }
    };
    MT_Transform world;
    world.setIdentity();
    for (const KX_NavCarveObstacle &o : m_step->carve) {
      if (in_range(o.center)) {
        box(o, world);
      }
    }
    for (const std::unique_ptr<KX_NavVehicle> &vehicle : m_vehicles) {
      for (const KX_NavCarveObstacle &o : vehicle->obstacles) {
        box(o, pose_of(vehicle->object));
      }
    }
  }
  if (!(hide & blender::NAVMESH_VIS_HIDE_LINKS)) {
    /* Hand-placed links by their kind (closed: grey dashed, Wait: red dashed); links between
     * frames green open, red closed (docking places cyan); the jump links the build found:
     * jumps orange, drops magenta. */
    for (const KX_NavLink &link : m_links) {
      const MT_Vector3 start = link.object ? link.object->NodeGetWorldPosition() : link.start;
      const MT_Vector3 end = link.end_object ? link.end_object->NodeGetWorldPosition() : link.end;
      if (!in_range(start) && !in_range(end)) {
        continue;
      }
      const bool frame_link = std::any_of(m_frameLinks.begin(),
                                          m_frameLinks.end(),
                                          [&](const KX_NavFrameLink &f) {
                                            return f.link.user_id == link.user_id &&
                                                   (f.start_frame != nullptr ||
                                                    f.end_frame != nullptr);
                                          });
      if (frame_link) {
        continue;
      }
      const MT_Vector4 color = link.enabled ? link_kind_color(link.kind) :
                                              (link.wait ? kWaitColor : kClosedColor);
      DrawLinkLine(start, end, link.kind, color, !link.enabled, link.radius);
    }
    for (const KX_NavFrameLink &link : m_frameLinks) {
      if (link.start_frame == nullptr && link.end_frame == nullptr) {
        continue;
      }
      if (!in_range(link.start) && !in_range(link.end)) {
        continue;
      }
      const MT_Vector4 color = link.automatic ? kDockColor :
                                                (link.open ? kOpenFrameColor : kWaitColor);
      DrawLinkLine(link.start, link.end, link.link.kind, color, !link.open, link.link.radius);
    }
    if (m_build) {
      KX_NavBuildForEachFoundLink(*m_build, [&](uint32_t /*id*/, const KX_NavLinkInfo &link) {
        if (in_range(link.start)) {
          DrawLinkLine(link.start,
                       link.end,
                       link.drop ? "drop" : "jump",
                       link_kind_color(link.drop ? "drop" : "jump"),
                       false,
                       0.15f);
        }
      });
    }
  }
  if (!(hide & blender::NAVMESH_VIS_HIDE_VEHICLES)) {
    /* Each vehicle's navmesh on it (its walls; its NPCs' size class: the default). */
    for (const std::unique_ptr<KX_NavVehicle> &vehicle : m_vehicles) {
      if (!in_range(vehicle->object->NodeGetWorldPosition())) {
        continue;
      }
      for (const auto &[source, view] : vehicle->views) {
        if (view) {
          view->DrawAreas(
              [&](const MT_Vector3 &a, const MT_Vector3 &b, int area, bool wall) {
                return DrawLine(a, b, wall ? kVehicleColor : area_color(area) * 0.7f);
              },
              center,
              0.0f);
          break;
        }
      }
    }
  }
  if (!(hide & blender::NAVMESH_VIS_HIDE_NAVMESH)) {
    /* The scene's navmesh: walls bright, the walkable surfaces' edges by area. */
    const KX_NavMesh *navmesh = m_testNavMesh ? m_testNavMesh.get() :
                                (m_build ? KX_NavBuildFindNavMesh(*m_build, 0.0f) : nullptr);
    if (navmesh != nullptr) {
      navmesh->DrawAreas(
          [&](const MT_Vector3 &a, const MT_Vector3 &b, int area, bool wall) {
            return DrawLine(a, b, wall ? kWallColor : area_color(area) * 0.6f);
          },
          center,
          range);
    }
  }
}

/** \} */
