/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Plan C1 prototype: columns inside Logic Nodes (see LN_Columns.h). */

#include "LN_Columns.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "CM_Message.h"
#include "KX_GameObject.h"
#include "PHY_IPhysicsController.h"
#include "LN_CommandBuffer.h"
#include "LN_JobPool.h"
#include "LN_KindCatalog.h"
#include "LN_LeanProgram.h"
#include "LN_Program.h"
#include "LN_RuntimeTree.h"
#include "LN_Snapshot.h"
#include "LN_StrictDump.h"
#include "LN_TreeBatch.h"
#include "LN_TypeConversion.h"

namespace {

std::atomic<int> g_column_mode_override{-1};
std::atomic<uint64_t> g_column_lanes{0};
/** Column programs compiled (LN_ColumnRunner::CompiledCountForTests). */
std::atomic<uint64_t> g_column_compiles{0};
/** Lanes the lane map looked at (LN_ColumnRunner::LaneMapWorkForTests: once each a tick). */
std::atomic<uint64_t> g_column_lane_map_work{0};
/** Waves of short runs run (LN_ColumnRunner::WaveCountForTests). */
std::atomic<uint64_t> g_column_waves{0};
/** Lanes the bridge gave back to the VM (a pending move when their run started). */
std::atomic<uint64_t> g_column_bridge_fallbacks{0};
/** Runs split at a pushed lane (plan C2 fix 1, LN_ColumnPushedSplitCount). */
std::atomic<uint64_t> g_column_pushed_splits{0};
/** Segments of bridged runs the helpers worked through, and segments the main thread took
 * (plan C1-9). */
std::atomic<uint64_t> g_column_helper_segments{0};
/** Lanes given back to the VM because an object one found while playing is a lane's (plan C1-9). */
std::atomic<uint64_t> g_column_runtime_fallbacks{0};
/** Runs split lane by lane, and lanes given back to the VM, because their lanes depend on an
 * object with pending moves (plan C2 P2: a parented lane's ancestor, a named lane). */
std::atomic<uint64_t> g_column_depend_splits{0};
/** UPBGE_LN_COLUMN_NATIVE_RAYS' test override (column_native_rays_on). */
std::atomic<int> g_native_rays_override{-1};
std::atomic<uint64_t> g_column_depend_fallbacks{0};
std::atomic<uint64_t> g_column_main_segments{0};
std::atomic<uint64_t> g_column_program_serial{0};
/** Lanes whose own property writes were applied ahead of the flush (plan C1-11 C-4). */
std::atomic<uint64_t> g_column_pre_applied{0};
std::atomic<int> g_column_pre_apply_override{-1};
std::atomic<uint64_t> g_column_count_mismatches{0};
std::atomic<int> g_column_count_verify_override{-1};
/** UPBGE_LN_COLUMN_DIRECT's test override, and the lanes that ran a direct program (plan C2 P5). */
std::atomic<int> g_column_direct_override{-1};
std::atomic<uint64_t> g_column_direct_lanes{0};

enum class PreApplyMode : uint8_t { Off, Auto, Force };
PreApplyMode pre_apply_mode()
{
  const int override_mode = g_column_pre_apply_override.load(std::memory_order_relaxed);
  if (override_mode >= 0) {
    return PreApplyMode(override_mode);
  }
  static const PreApplyMode from_environment = [] {
    const char *value = std::getenv("UPBGE_LN_COLUMN_PREAPPLY");
    if (value == nullptr || value[0] == '\0' || std::strcmp(value, "1") == 0) {
      return PreApplyMode::Auto;
    }
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0) {
      return PreApplyMode::Off;
    }
    return std::strcmp(value, "force") == 0 ? PreApplyMode::Force : PreApplyMode::Auto;
  }();
  return from_environment;
}

constexpr uint32_t kNone = LN_INVALID_INDEX;
/** NPCs a chunk: what a thread works through node by node (its values stay in the cache). */
constexpr size_t kChunkLanes = 256;
/** Runs shorter than this record one tree after the other. 1 since plan C2 P1 (2026-11-06, its
 * D3 gate: a run of 1 among trees of other programs ties the old system for an 8-node tree and
 * is 10-26% faster from 11 nodes; groups of 2-15 objects 1.3-2x; the 11 smoke files and the FPS
 * demo equal within noise or faster); 16 before (UPBGE_LN_COLUMN_MIN_RUN=16). */
constexpr size_t kDefaultMinRunLanes = 1;

/** `n` values from `src` to `dst` (plan C2 P1: a single value as one assignment; a memcpy of a
 * length known only at run time is a library call). */
template<typename T> inline void CopyLanes(T *dst, const T *src, const size_t n)
{
  if (n == 1) {
    *dst = *src;
  }
  else {
    std::memcpy(dst, src, n * sizeof(T));
  }
}

bool finite3(const MT_Vector3 &v)
{
  return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

/** A set of object pointers made again every tick (2026-11-06, the farm game: 10,000 lanes' objects
 * a step through an unordered_set cost a hash node allocation each, ~25 ns a lane). Open
 * addressing, kept between ticks: Clear only moves the generation, slots of older generations
 * count as empty; no allocation once it has grown to a tick's size. */
class FlatPointerSet {
 public:
  /** Empties the set, with room for `expected` pointers without growing. */
  void Clear(const size_t expected = 0)
  {
    m_size = 0;
    m_generation++;
    size_t want = 16;
    while (want < expected * 2) {
      want *= 2;
    }
    if (want > m_slots.size()) {
      m_slots.assign(want, Slot());
      m_generation = 1;
    }
    else if (m_generation == 0) {
      /* (wrapped: every slot back to an old generation) */
      std::fill(m_slots.begin(), m_slots.end(), Slot());
      m_generation = 1;
    }
  }
  /** Adds `pointer`; false when it was in already. */
  bool Insert(const void *pointer)
  {
    if (m_slots.empty() || (m_size + 1) * 2 > m_slots.size()) {
      Grow();
    }
    const size_t mask = m_slots.size() - 1;
    for (size_t at = Hash(pointer) & mask;; at = (at + 1) & mask) {
      Slot &slot = m_slots[at];
      if (slot.generation != m_generation) {
        slot.pointer = pointer;
        slot.generation = m_generation;
        m_size++;
        return true;
      }
      if (slot.pointer == pointer) {
        return false;
      }
    }
  }
  bool Contains(const void *pointer) const
  {
    if (m_size == 0) {
      return false;
    }
    const size_t mask = m_slots.size() - 1;
    for (size_t at = Hash(pointer) & mask;; at = (at + 1) & mask) {
      const Slot &slot = m_slots[at];
      if (slot.generation != m_generation) {
        return false;
      }
      if (slot.pointer == pointer) {
        return true;
      }
    }
  }
  bool Empty() const
  {
    return m_size == 0;
  }

 private:
  struct Slot {
    const void *pointer = nullptr;
    uint32_t generation = 0;
  };
  static size_t Hash(const void *pointer)
  {
    uint64_t x = uint64_t(reinterpret_cast<uintptr_t>(pointer));
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return size_t(x);
  }
  void Grow()
  {
    std::vector<const void *> kept;
    kept.reserve(m_size);
    for (const Slot &slot : m_slots) {
      if (slot.generation == m_generation && m_size != 0) {
        kept.push_back(slot.pointer);
      }
    }
    m_slots.assign(std::max<size_t>(16, m_slots.size() * 2), Slot());
    m_generation = 1;
    m_size = 0;
    for (const void *pointer : kept) {
      Insert(pointer);
    }
  }
  std::vector<Slot> m_slots;
  size_t m_size = 0;
  uint32_t m_generation = 0;
};

/** The nodes a column program runs, each one loop over a chunk. */
enum class NodeOp : uint8_t {
  FConst,
  FProp,
  FAdd,
  FSub,
  FMul,
  FDiv,
  FSafeDiv,
  FMin,
  FMax,
  FAbs,
  FNeg,
  FClamp,
  FFromInt,
  FFromBool,
  FPow,
  FSign,
  FRound,
  FFloor,
  FCeil,
  FTrunc,
  FFract,
  FMod,
  FSin,
  FCos,
  FRad,
  FDeg,
  /** Threshold: `a` value, `b` threshold, `c` Else Zero (bool), `compare` the operation. */
  FThreshold,
  /** Ranged Threshold: `a` value, `b` min, `c` max, `compare` the operation. */
  FRanged,
  /** `c` the condition (bool): `a` when true, `b` when false. */
  FSelect,
  /** A value of the tick, the same for every NPC (`compare`: a Uniform). */
  FUniform,
  /** Random Float: `a` min, `b` max, `property_ref` the expression (the hash's seed). */
  FRandom,
  /** A tree property's field (`property_ref`), as it is when the pass starts. */
  FTreeProp,
  /** Get Distance: `out` the distance, `out2` the squared distance. */
  QDistance,
  /** Plan C2 P3: a Raycast (query `property_ref`) cast by the VM's own QueryKind once per NPC
   * where `d` lets it (`compare`: the gate + 1): `out` Has Result (bool), `out2` the distance,
   * `position_a` the hit position, `position_b` the hit normal (zero without a hit), as its
   * output kinds give them. */
  QRay,
  IConst,
  IProp,
  /** Typecast's float to integer (`compare`: the mode). */
  IFromFloat,
  IFromBool,
  IRandom,
  ISelect,
  ITreeProp,
  BConst,
  BProp,
  BNot,
  BAnd,
  BOr,
  BCmp,
  BFromInt,
  BFromFloat,
  /** A vector's length is not zero: `a`, `b`, `c` its axes. */
  BFromVector,
  BSelect,
  BTreeProp,
  /** `a` (int) equals `int_value`. */
  BIntEq,
  /** Normalize: `a`, `b`, `c` in; `out`, `out2`, `out3` out. */
  VNormalize,
  /** Get Object Attribute's vector (`compare`: the LN_ObjectAttributeId) of the owner (`end_a`
   * Owner, read live) or a named object (Object, read when the run starts). */
  VAttr,
  /** Get Axis Vector (`compare`: the axis): the owner's from its snapshot, a named object's live. */
  VAxis,
  /** Get Object Attribute's velocity: read on the main thread when the run starts (physics is
   * not read from the helpers), `property_ref` the program's velocity read. */
  VVel,
  /** Get Average Center Position whose source is the same for every NPC (plan C1-9): its centre
   * (`out`, `out2`, `out3`), count (IAvgCount: `out`) and Valid (BAvgValid: `out`), worked out
   * once when the run starts (`property_ref`: the program's average read). */
  VAvg,
  IAvgCount,
  BAvgValid,
  /** Plan C2 fix 1: Get Game Property (Bool / Int / Float) of a named object, the same for every
   * NPC and all the tick (game property writes wait for the flush): worked out once when the run
   * starts by the VM's own code (`property_ref`: the program's run invariant), `out`. */
  FRunInv,
  IRunInv,
  BRunInv,
  /** Once (`property_ref`: its state) with `a` the op's mask: `out` passed. */
  SOnce,
  /** Flow Router (`property_ref`: its state, `int_value` outputs) with `a` the op's mask: `out`
   * (int) the route taken, -1 when it did not route. */
  SFlowRouter,
  /** Flow Router's last route (`property_ref`: its state) when the pass starts, -1 for none. */
  SFlowLast,
  /** Repeat Limit (`property_ref`: its state) with `a` the op's mask and `b` the limit (int):
   * `out` accepted, `out2` blocked. */
  SRepeat,
  /** Every N Steps: gate `property_ref` lets its routes run this step (StepGateRuns). */
  SGate,
  /** Time Passed of gate `property_ref` (kNone: the event's delta). */
  FStepTime,
  /** The unscaled delta a block behind gate `property_ref` reads. */
  FGateUnscaled,
  /** Vector Rotate (`compare`: mode 0 Z, 1 fixed axis `int_value`, 2 axis input): `position_a`
   * the point, `position_b` the pivot, `vec_c` the axis, `a` the angle. */
  VRotate,
  /** Random Vector (`property_ref` the expression): `position_a` the axes. */
  VRandom,
  /** Has Property: the snapshot's game property `property_ref` exists. */
  BExists,
  /** Boolean Edge (`property_ref` its expression): `a` the condition, `d` where it is evaluated. */
  SEdge,
  /** Boolean Edge's falling side: the state of `property_ref` once `d` evaluated it. */
  SEdgeFalling,
  /** On Value Changed (`property_ref`, plan C2 fix 1) of a bool, int or float (`compare` 0, 1,
   * 2) in slot `a`, where `d`: the VM's own step (LN_RuntimeTree::ValueChangeStep) NPC by NPC on
   * the value FromBool / FromInt / FromFloat makes; `bool_value`: waits for a value. */
  SValueChanged,
  /** Store Value's Flow (StoreValueDone): stores `a` into Store Value `property_ref` where `d`. */
  SStoreDone,
  /** Store Value read (`property_ref`): its first read stores `a` when `bool_value` (where `d`). */
  FStoreValue,
  /** Flow Gate's Enter (TryFlowGate, `property_ref` the gate) with `a` the op's mask and `b` the
   * gate's Start Open: `out` passed. */
  SGateTry,
  /** Flow Gate's Open / Close / Toggle (SetFlowGate, `int_value` the control) with `a` the mask. */
  SGateSet,
  /** Flow Gate's Is Open read (`property_ref` the gate, `b` Start Open, `d` where evaluated). */
  SGateRead,
  /** A Delay, Timer, Wait for Next Frame, Pulsify, Barrier or Cooldown op (`compare`: TimeOp) of
   * time flow state `property_ref` for the NPCs of mask `a` (LN_RuntimeTree::TimeFlow*): `b` its
   * float input (duration, interval), `c` its Ignore Timescale, `d` a Barrier's condition;
   * `int_value` the gate of its route (-1: none), `bool_value` the instruction's UI route flag. */
  STimeOp,
  /** A read of time flow state `property_ref` (`compare`: TimeRead), `bool_value` the fallback. */
  STimeRead,
  /** A Cooldown's Remaining / Progress (`compare` 0 / 1) of state `property_ref`, `float_value` the
   * fallback. */
  FTimeRead,
  /** A constant rotation (`rotation`) in rotation slot `out`. */
  RConst,
  /** A rotation from Euler angles `a`, `b`, `c` (LN_Rotation::FromEulerXYZ). */
  RFromEuler,
  /** Vector To Rotation (`int_value` the axis): `position_a` the direction, `position_b` up; as a
   * rotation (RVecToRot, slot `out`) or as Euler angles (VVecToRot, `out`, `out2`, `out3`). */
  RVecToRot,
  VVecToRot,
  /** Get Object Attribute as a rotation (`compare`: the attribute; RotationKind's typed read) of
   * the owner (live) or a named object; `rotation` the fallback. */
  RAttr,
  /** The bridge (plan C1-8): an expression worked out by the VM's own kind functions, NPC by NPC
   * where `d` (kNone: everywhere) lets it, in its route's context (`compare`: the gate + 1, 0 none):
   * `property_ref` the expression, the fallback in `bool_value` / `int_value` / `float_value` /
   * `position_b` / `rotation` / `color`; out in `out` (vectors `out`..`out3`, colours `out4` too). */
  BKBool,
  BKInt,
  BKFloat,
  BKVector,
  BKRotation,
  BKColor,
  /** The bridge: op `property_ref` run by the VM's handler for the NPCs of mask `a` (`compare` the
   * gate + 1), its commands caught for write `int_value` (applied at the flush, in op order). */
  BOp,
  /** The stamp of an op (`property_ref` its stamp index) in this pass, after a bridged op. */
  SRan,
  /** Plan C1-9: Loop body block `property_ref` run by the VM's block walk for the NPCs of mask `a`
   * (`compare` the gate + 1), its commands caught for write `int_value`. */
  BLoop,
  /** Plan C1-9: loop frame `property_ref` completed in this pass (its Completed route runs). */
  SLoopDone,
  /** Repeat Limit's Remaining (`property_ref` its state, `a` the limit, `float_value` fallback). */
  IRepeatRemaining,
  /** Repeat Limit's Is Exhausted. */
  BRepeatExhausted,
  /** Move To's Reached (`property_ref` the op's stamp index). */
  SReached,
  /** Move To's record-time Reached check (`property_ref` the stamp index) with `a` the mask:
   * `position_a` target, `position_b` where the object is, `b` speed, `c` Stop Distance, `d` the
   * step's delta. */
  SMoveReached,
  /** Apply Force To Target (plan C2 P5) for the NPCs of mask `a`: `position_a` the target
   * position, `position_b` where the moved object is, `b` Force, `c` Reached Threshold, `d` Stop
   * Distance (LN_RuntimeTree::ColumnForceToTarget); notes Reached on stamp `property_ref`; `out`
   * the force is applied, `vec_c` the force. */
  MForceToTarget,
  /** An op's stamp (`property_ref` the stamp index) for the NPCs of mask `a`. */
  SStamp,
  /** Set Tree Property written now (`int_value` the tree write op) for the NPCs of mask `a`:
   * `b`, `c`, `d` its float, int and bool value. */
  STreeWrite,
  /** The step's delta behind gate `property_ref` (`compare` 1: as is, 0: not below 0). */
  FGateDelta,
  /** A tree property (`property_ref`) read as a generic value and converted (`compare`: 0 float,
   * 1 int (`int_value` the mode), 2 bool; `float_value` the float fallback): `a`, `b`, `c` its
   * fields and `d` the last write of the pass so far (-1: none, the stored value). */
  VTreeConvert,
  /** A tree property (`property_ref`) as a vector, from its value as it is at this place of the
   * pass (Set Tree Property writes at once): `compare` 0 = the vector read (a rotation's Euler
   * angles, else the vector field), 1 = a generic value's conversion (ValueAsVector, else
   * `position_b` the fallback). Plan C1-11. */
  VTreeProp,
  /** The object a tree property (`property_ref`) holds at this place of the pass, per NPC, into
   * the run's target column `int_value` (a write's object, plan C1-11). */
  OTreeObj,
};

/** STimeOp's ops. */
enum class TimeOp : uint8_t { NextFrame, Timer, Delay, Pulsify, Barrier, Cooldown, ResetCooldown };

/** STimeRead's reads (the kinds' code). */
enum class TimeRead : uint8_t {
  /** Timer Elapsed, Delay Done, Next Frame Done, Cooldown Completed: it pulsed this tick. */
  Pulse,
  PulsifyPulse,
  BarrierPassed,
  CooldownAccepted,
  CooldownBlocked,
  CooldownReady,
};

/** The values of a tick FUniform reads. */
enum class Uniform : uint8_t {
  UnscaledDeltaTime = 0,
  /** float(max(0, delta_time)) and float(delta_time) of the pass (ungated routes). */
  DeltaTime,
  DeltaTimeRaw,
  GravityX,
  GravityY,
  GravityZ,
  ElapsedTime,
  FrameDelta,
  FPS,
  DeltaFactor,
  TimeScale,
  Count
};

/** An end of a distance. Runtime (plan C1-9): an object each NPC finds while playing (its Object
 * game property, a tree property, Get Object By Name), resolved when the run starts. */
enum class EndKind : uint8_t { None = 0, Owner, Object, Position, Runtime };

/** A vector: three float slots (Combine XYZ needs no copy). */
struct VecSlots {
  uint32_t x = kNone, y = kNone, z = kNone;
};

struct Node {
  NodeOp op = NodeOp::FConst;
  uint8_t compare = 0;
  uint32_t out = kNone;
  uint32_t out2 = kNone;
  uint32_t out3 = kNone;
  uint32_t a = kNone, b = kNone, c = kNone, d = kNone;
  uint32_t property_ref = kNone;
  float float_value = 0.0f;
  int32_t int_value = 0;
  bool bool_value = false;
  /* RConst, BKRotation */
  LN_Rotation rotation;
  /* BKColor: its fourth output and fallback */
  uint32_t out4 = kNone;
  /** A bridged node (BK*, BOp) whose VM code may not run on a helper thread (plan C1-9: the
   * run takes it on the main thread, over every chunk in lane order, between the segments the
   * helpers run). */
  bool main_thread = false;
  /** A bridged node reading logic collisions: on the main thread too in a run where a lane's
   * object queries its collisions on demand (PHY_IPhysicsController). */
  bool collision_reads = false;
  float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  /* QDistance */
  EndKind end_a = EndKind::None, end_b = EndKind::None;
  uint32_t object_a = kNone, object_b = kNone;
  VecSlots position_a, position_b, vec_c;
};

/** Service: Set Visible, Set Object Color, Set Object Attribute's Teleport (object service
 * commands, never merged: they end a run of last-write-wins writes). */
/** Captured: the commands a bridged op recorded (plan C1-8), applied by the flush's own code. */
enum class WriteKind : uint8_t { Number, Modify, Transform, Motion, Service, Captured };

struct Write {
  WriteKind kind = WriteKind::Number;
  /** The op's mask (a bool slot): the op ran for that NPC. */
  uint32_t mask = kNone;
  uint32_t property_ref = kNone;
  /* Number */
  LN_ValueType number_type = LN_ValueType::Float;
  uint32_t value = kNone; /* a slot of the number's type */
  /* Modify */
  LN_PropertyModifyOp modify_op = LN_PropertyModifyOp::None;
  bool clamped = false;
  uint32_t operand = kNone, min = kNone, max = kNone; /* float slots */
  /* Transform (moves, velocities and forces) */
  LN_CommandBuffer::CommandType command = LN_CommandBuffer::CommandType::ApplyMovement;
  /** Set Transform Vector / Set Velocity Vector: the command when Local is on (`command` when off). */
  LN_CommandBuffer::CommandType local_command = LN_CommandBuffer::CommandType::ApplyMovement;
  bool type_from_local = false;
  bool is_rotation = false;
  /** A rotation worked out per NPC: its rotation slot and its column in the run (kNone: the
   * constant `rotation`). */
  uint32_t rot = kNone, out_rot = kNone;
  /** A move (IsObjectTransformCommand): a read may apply it early; velocities and forces never. */
  bool is_move = false;
  /** Transform, Number and Modify: the object it writes, an index into runtime_objects (each
   * NPC's, found when the run starts, or after it for an object only written: plan C1-9 E2,
   * C1-10), kNone: the owner. */
  uint32_t runtime_target = kNone;
  /** Number, Modify, Motion and Service: the object a tree property the program writes holds
   * where the op runs (an index into the run's target columns, filled by OTreeObj; plan C1-11),
   * kNone: none. */
  uint32_t target_column = kNone;
  /** Whether the command(s) it stands for are last-write-wins (the flush's merge), for `command`
   * and `local_command`. */
  bool last_write_wins = false;
  bool local_last_write_wins = false;
  /** Another write of the program may have its key (the same property, a command of the same
   * type): the flush's merge must see it. */
  bool may_collide = false;
  LN_Rotation rotation;
  VecSlots vector;
  /* Motion (Move To, Rotate To, Align Axis To Vector: ApplyMotionNow's fields) */
  VecSlots secondary;
  uint32_t scalar = kNone, int_a = kNone, int_b = kNone;
  bool motion_bool = false, motion_secondary_bool = false;
  /* Service: Set Visible's Visible and Include Children (bool slots), a colour's RGBA (float
   * slots) */
  uint32_t flag_a = kNone, flag_b = kNone;
  uint32_t color[4] = {kNone, kNone, kNone, kNone};
  /** The flush's gate counts its command as last-write-wins (its type's policy); the merge never
   * merges it (key_of has no key for its stream). */
  bool counts_last_write_wins = false;
  bool local_constant = false;
  uint32_t local = kNone; /* bool slot; kNone: local_constant */
  /* Where the run keeps this write's values (column numbers in the run's arrays). */
  uint32_t out_float = kNone, out_int = kNone, out_bool = kNone;
};

}  // namespace

struct LN_ColumnProgram {
  uint64_t serial = 0;
  uint64_t program_id = 0;
  LN_Event event = LN_Event::OnUpdate;
  std::string source_name;
  /** Constants: filled once per chunk scratch, not per chunk. */
  std::vector<Node> constants;
  std::vector<Node> nodes;
  std::vector<Write> writes;
  uint32_t float_slots = 0, int_slots = 0, bool_slots = 0;
  uint32_t out_float_columns = 0, out_int_columns = 0, out_bool_columns = 0;
  /** Rotations: slots of a chunk and columns of a run (LN_Rotation each). */
  uint32_t rot_slots = 0, out_rot_columns = 0;
  /** The value expressions of the named objects the distances measure (resolved when a run
   * starts; their pending moves applied then, as the run's first tree's read would). */
  std::vector<uint32_t> object_values;
  /** The value expressions of the objects each NPC finds while playing (EndKind::Runtime: its
   * Object game property, a tree property the program never writes, Get Object By Name of a
   * constant name, Get Child (By Name) of the owner), resolved per NPC when a run starts (plan
   * C1-9). */
  std::vector<uint32_t> runtime_objects;
  /** Per runtime object (plan C1-9 E2): a distance reads it (`runtime_measured`: neither it nor a
   * parent may be a lane's object, the VM's read would apply their moves); a live pose read reads
   * it not after a sure distance to it (`runtime_pose_unprotected`: no object whose pending moves
   * the run applies when it starts may be it or a parent of it); a move writes it
   * (`runtime_moved`: it must be no lane's object, nor a parent of an object whose read applies
   * pending moves: a turret on its NPC is fine); Apply Movement / Rotation moves it
   * (`runtime_relative`: a Frame Update refuses that on a dynamic body). */
  std::vector<uint8_t> runtime_measured, runtime_pose_unprotected, runtime_moved, runtime_relative;
  /** Per runtime object (plan C1-10): the same for every NPC (`runtime_invariant`: a named
   * object, Get Object By Name of a constant name, their parents; found once when the run
   * starts); only written (`runtime_after_run`: a property write's target, nothing reads it in
   * the run; found by the VM's code after the run, NPC by NPC, only where a write's op ran, as the
   * VM finds it at the op); found by a collision (`runtime_collision`: Collision Hit Object of
   * the owner, or a parent of it; always after the run). */
  std::vector<uint8_t> runtime_invariant, runtime_after_run, runtime_collision;
  /** Some runtime object is found by a collision (plan C1-10). */
  bool collision_sources = false;
  /** Writes' objects captured where their ops run (OTreeObj, plan C1-11). */
  uint32_t target_columns = 0;
  /** Some read of the program may apply pending moves (a distance or ray of the owner, of a named
   * object or of an object found while playing, Add Object's reference...): it may change what a
   * collision read sees, in another program of a group too (plan C1-10). */
  bool applies_moves = false;
  /** Add Object (its copy is made while the tree records: name lookups and collisions of other
   * trees may see it). */
  bool adds_objects = false;
  /** The run starts only when nothing its reads would apply is pending, as a bridged run (the
   * first NPCs play on the VM while a named object has some, the run goes back to the VM while a
   * lane's object or a measured found object has some): nothing is applied while its trees run,
   * in the VM either. A program reading live positions it cannot name (`live_positions`), or
   * finding a collision's object next to reads that apply pending moves (an apply may drop the
   * contacts the logic reads: plan C1-10). */
  bool quiet_start = false;
  /** Plan C2 fix 1: why the program starts quietly for its reads' order (ColumnCompiler::
   * NoteQuietReads; empty: not for that), e.g. a live pose read of the owner next to a distance
   * read from it. */
  std::string quiet_reads;
  std::vector<std::string> property_names;
  bool needs_owner_position = false;
  /** Get Axis Vector of the owner: the lanes capture their transform with their properties. */
  bool needs_owner_transform = false;
  /** Reads the tick's timing (elapsed time, frame delta, FPS...): the tick read context's. */
  bool needs_timing = false;
  /** Reads the tick's gravity (the tick read context's). */
  bool needs_gravity = false;
  /** When a distance reads the owner / a named object (each such read applies its pending moves
   * first): on every pass, or only when one of these step gates lets its route run. */
  struct ReadWhen {
    bool always = false;
    uint64_t gates = 0;
  };
  /** A distance that reads the owner or a named object: when it is read, and its ends (the VM
   * resolves end B only when end A is valid). */
  struct QueryRead {
    ReadWhen when;
    EndKind a = EndKind::None, b = EndKind::None;
    uint32_t a_object = kNone, b_object = kNone;
  };
  std::vector<QueryRead> query_reads;
  /** Apply Movement / Rotation of the owner: a Frame Update refuses them on a dynamic body. */
  bool relative_moves = false;
  /** Plan C2 fix 1: the reads worked out once a run (FRunInv / IRunInv / BRunInv): expression
   * `index` of family `family` ('F', 'I', 'B') with the fallback of its read site. */
  struct RunInvariant {
    char family = 'F';
    uint32_t index = kNone;
    float float_fallback = 0.0f;
    int32_t int_fallback = 0;
    bool bool_fallback = false;
  };
  std::vector<RunInvariant> run_invariants;
  /** The Get Average Center Position queries worked out once a run (VAvg / IAvgCount /
   * BAvgValid: their source is the same for every NPC). */
  std::vector<uint32_t> average_reads;
  /** Some node reads live positions of objects it cannot name (an average, bridged or not): the
   * run starts only when nothing it would apply is pending (as a bridged run), so no position
   * changes while the run's trees run, in the VM either (plan C1-9). */
  bool live_positions = false;
  /** Some write moves (or pushes) an object found while playing (plan C1-9 E2). */
  bool moves_others = false;
  /** The bridge (plan C1-8): some expression or op runs the VM's own code NPC by NPC. Its lanes
   * capture their whole snapshot, and a run falls back to the VM when a lane's object or an object
   * the program names has a pending move (`constant_objects`, resolved when the run starts). */
  bool bridged = false;
  /** Chunks run one after the other on the main thread: the program is not batch safe. Since
   * plan C1-9 only `UPBGE_LN_COLUMN_SEGMENTS=0` reads it: the run cuts the nodes into segments
   * instead, the helpers working through those whose nodes may run there (Node::main_thread). */
  bool on_main_thread = false;
  /** Why a bridged node stays on the main thread (the first one; empty: none does). */
  std::string main_thread_why;
  /** Some node is Node::collision_reads. */
  bool collision_reads = false;
  /** No read of the program applies its owner's pending moves (no distance from the owner, no
   * ray or shape cast from it, no Add Object at it): a bridged run takes lanes whose objects
   * have some (an earlier tree of the object, another tree's push), as the VM's pass leaves
   * them pending (plan C1-9). */
  bool owner_pending_ok = false;
  std::vector<uint32_t> constant_objects;
  /** The named objects whose reads may apply pending moves (`object_values`, and in a bridged
   * program the `constant_objects` but those only Get Child By Name's name hints): a moved
   * runtime object must be none of them nor a parent of one (plan C1-9 E2). */
  std::vector<uint32_t> applied_named;
  /** Velocities Get Object Attribute reads (VVel): of the owner (per NPC) or a named object
   * (`object`), read when the run starts. */
  struct VelocityRead {
    LN_ObjectAttributeId attribute = LN_ObjectAttributeId::None;
    bool owner = true;
    uint32_t object = kNone;
  };
  std::vector<VelocityRead> velocity_reads;
  uint32_t route_ops = 0;
  /** Per op: its stamp index (LN_LeanOp::state_index) and the mask of the NPCs it ran for (the VM
   * stamps every op that runs). */
  std::vector<uint32_t> op_state, op_mask;
  /** A tree property a pass writes: its fields' slots at the pass's end and the int slot of the
   * last write that ran (an index into tree_write_ops; -1 none). */
  struct TreeProperty {
    uint32_t ref = kNone;
    uint32_t f = kNone, i = kNone, b = kNone;
    uint32_t last_write = kNone;
  };
  std::vector<TreeProperty> tree_properties;
  /** A Set Tree Property op: the value it writes is a fresh value of `type` holding the field, or
   * `value` itself when `constant` (the property's default, a constant). */
  struct TreeWrite {
    LN_ValueType type = LN_ValueType::Float;
    bool constant = false;
    LN_Value value;
    uint32_t mask = kNone;
  };
  std::vector<TreeWrite> tree_write_ops;
  /** Some write may share its key with another (Write::may_collide). */
  bool any_collide = false;
  /** Some write that may be last-write-wins also may share its key with another of the pass, or
   * acts on an object found while playing: only then may a lane on an object nothing else writes
   * list a last-write-wins write when the buffer's own writes cannot merge (ListWrites' dry walk;
   * plan C2 P1: two Apply Movements of the owner made every lane walk its writes). */
  bool lists_unshared_last_write_wins = false;
  /** Some write acts on an object found while playing (runtime_target, target_column): always
   * listed for the flush's merge. */
  bool any_found_target_write = false;
  /** Some Number write is last-write-wins (another lane may write its key: written_keys). */
  bool any_last_write_wins_number = false;
  /** Some Number or Modify write acts on the owner (plan C1-11 C-4: applied ahead when safe). */
  bool own_property_writes = false;
  /** Plan C2 P3: a Raycast runs as a column node (QRay); its run starts quietly (nothing its
   * lanes' casts would apply may be pending: the cast cannot apply them where the VM does). */
  bool native_rays = false;
  /** The merge keys of its own last-write-wins writes, sorted (plan C2 P2: two trees of one object
   * writing one of them must be listed for the flush's merge): a Number write's property name, a
   * transform write's command type as "\x01<type>" (the key is the object and the type). */
  std::vector<std::string> own_last_write_wins_keys;
  /** Some Number or Modify write acts on an object found while playing. */
  bool found_property_writes = false;
  /** Some write is a bridged op's recorded commands. */
  bool captured_writes = false;
  /** Plan C2 P5: a direct program (LN_ColumnDirectMode). It has no nodes and no writes: its lane
   * runs alone, on the main thread, the runner walking the event's blocks (RunDirectLane) and the
   * op handlers recording into the tick's buffer; `bridged` is set too (its lanes capture their
   * whole snapshot). `direct_why`: why the column compiler refused it (empty in the Force mode).
   * `batch_safe`: the program is a tree batch candidate (LN_AnalyzeTreeBatchSafety), so its
   * trees' column passes may record in tree batches while the tick has helpers; `has_route`: the
   * event has a compiled route, which the column pass runs. */
  bool direct = false;
  bool batch_safe = false;
  bool has_route = false;
  std::string direct_why;
};

/** A setting asking for the old system, which plan C2 P10 removed (2026-11-07): said once. */
static void warn_old_system_setting(const char *variable, const char *value)
{
  if (value != nullptr && (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0)) {
    CM_Warning("Logic Nodes: " << variable << "=" << value
                               << " is ignored: the old Logic Nodes system was removed, every "
                                  "tree runs on the column system");
  }
}

LN_ColumnMode LN_GetColumnMode()
{
  const int override_mode = g_column_mode_override.load(std::memory_order_relaxed);
  if (override_mode >= 0) {
    return override_mode == 2 ? LN_ColumnMode::All : LN_ColumnMode::On;
  }
  static const LN_ColumnMode mode = []() {
    /* all: runs of 1 too (plan C2 P0, tests and coverage). */
    const char *value = std::getenv("UPBGE_LN_COLUMNS");
    warn_old_system_setting("UPBGE_LN_COLUMNS", value);
    if (value != nullptr && std::strcmp(value, "all") == 0) {
      return LN_ColumnMode::All;
    }
    return LN_ColumnMode::On;
  }();
  return mode;
}

void LN_SetColumnModeForTests(const int mode)
{
  g_column_mode_override.store(mode < 0 ? -1 : std::min(mode, 2), std::memory_order_relaxed);
}

uint64_t LN_ColumnBridgeFallbackCount()
{
  return g_column_bridge_fallbacks.load(std::memory_order_relaxed);
}

uint64_t LN_ColumnPushedSplitCount()
{
  return g_column_pushed_splits.load(std::memory_order_relaxed);
}

uint64_t LN_ColumnRuntimeFallbackCount()
{
  return g_column_runtime_fallbacks.load(std::memory_order_relaxed);
}

void LN_ColumnDependCounts(uint64_t &r_splits, uint64_t &r_fallbacks)
{
  r_splits = g_column_depend_splits.load(std::memory_order_relaxed);
  r_fallbacks = g_column_depend_fallbacks.load(std::memory_order_relaxed);
}

uint64_t LN_ColumnCountMismatchCount()
{
  return g_column_count_mismatches.load(std::memory_order_relaxed);
}

void LN_SetColumnCountVerifyForTests(const int mode)
{
  g_column_count_verify_override.store(mode < 0 ? -1 : (mode == 0 ? 0 : 1),
                                       std::memory_order_relaxed);
}

uint64_t LN_ColumnPreAppliedLaneCount()
{
  return g_column_pre_applied.load(std::memory_order_relaxed);
}

void LN_SetColumnPreApplyModeForTests(const int mode)
{
  g_column_pre_apply_override.store(mode < 0 ? -1 : std::min(mode, 2), std::memory_order_relaxed);
}

void LN_SetColumnNativeRaysForTests(const int mode)
{
  g_native_rays_override.store(mode < 0 ? -1 : (mode != 0 ? 1 : 0), std::memory_order_relaxed);
}

LN_ColumnDirectMode LN_GetColumnDirectMode()
{
  const int override_mode = g_column_direct_override.load(std::memory_order_relaxed);
  if (override_mode >= 0) {
    return override_mode == 2 ? LN_ColumnDirectMode::Force : LN_ColumnDirectMode::Auto;
  }
  static const LN_ColumnDirectMode from_environment = [] {
    const char *value = std::getenv("UPBGE_LN_COLUMN_DIRECT");
    warn_old_system_setting("UPBGE_LN_COLUMN_DIRECT", value);
    return value != nullptr && std::strcmp(value, "force") == 0 ? LN_ColumnDirectMode::Force :
                                                                   LN_ColumnDirectMode::Auto;
  }();
  return from_environment;
}

void LN_SetColumnDirectModeForTests(const int mode)
{
  g_column_direct_override.store(mode < 0 ? -1 : std::min(mode, 2), std::memory_order_relaxed);
}

void LN_AddColumnPasses(const uint64_t passes)
{
  if (passes != 0) {
    g_column_direct_lanes.fetch_add(passes, std::memory_order_relaxed);
  }
}

uint64_t LN_ColumnDirectLaneCount()
{
  return g_column_direct_lanes.load(std::memory_order_relaxed);
}

void LN_ColumnSegmentCounts(uint64_t &r_helper_segments, uint64_t &r_main_segments)
{
  r_helper_segments = g_column_helper_segments.load(std::memory_order_relaxed);
  r_main_segments = g_column_main_segments.load(std::memory_order_relaxed);
}

uint64_t LN_ColumnLaneCount()
{
  return g_column_lanes.load(std::memory_order_relaxed);
}

/* -------------------------------------------------------------------- */
/** \name The compiler
 * \{ */

namespace {

bool column_native_rays_on();

/** The ops the bridge (plan C1-8) runs through the VM's handler: they read, keep their own tree's
 * state and record commands, and change nothing of the scene while trees record (no Add Object,
 * no parenting, no scene, file, window, camera or UI change, no Run Logic Tree queue). Moves have
 * kernels of their own (a read may apply a move early: a caught one could not be). */
/** The recorded kind input table (plan C1-8): a kind's name and the fields it was seen to read. */
struct KindInputRow {
  const char *name;
  const char *fields;
};
constexpr KindInputRow kKindInputRows[] = {
#include "LN_KindInputsData.inc"
};

/** Fields the recording never saw read, checked by reading the kinds' code (plan C1-8): the rig
 * has no objects, no actions and default modes, so a kind that returns before such a field (no
 * target object, another mode) read it on no recorded path. Family '-': the field holds data (an
 * instruction, a stick number) or is never read (the second input a one-input math node gets). A
 * set field neither the table nor this list names makes the walk answer "unknown". */
constexpr KindInputRow kKindInputSupplement[] = {
    {"Bool::InstructionExecuted", "input0:-"},
    {"Bool::InstructionReached", "input0:-"},
    {"Vector::InstructionNextPoint", "input0:-"},
    {"Vector::GamepadStick", "input2:-"},
    {"Float::Absolute", "input1:-"},
    {"Float::Sign", "input1:-"},
    {"Float::Round", "input1:-"},
    {"Float::Floor", "input1:-"},
    {"Float::Ceil", "input1:-"},
    {"Float::Truncate", "input1:-"},
    {"Float::Fraction", "input1:-"},
    {"Float::Sine", "input1:-"},
    {"Float::Cosine", "input1:-"},
    {"Float::Radians", "input1:-"},
    {"Float::Degrees", "input1:-"},
    {"Bool::RepeatLimitExhausted", "input0:I"},
    {"Bool::AnimationPlaying", "int_expr_index:I"},
    {"Bool::ActionDone", "int_expr_index:I"},
    {"Bool::ActionStarted", "input0:B"},
    {"Bool::ActionCompleted", "input0:B"},
    {"Bool::ActionInterrupted", "input0:B"},
    {"Bool::ActionFailed", "input0:B"},
    {"Float::StoreValue", "input0:F"},
    /* Its float_expr_index is the Store Value expression, whose input it reads. */
    {"Bool::StoreValueDone", "input3:F"},
    {"Value::ObjectAttribute", "input1:S"},
    {"Value::ObjectGameProperty", "input1:S"},
    /* Its index is read only after a parent was found (the rig has none). */
    {"Value::ObjectChild", "input1:I"},
    {"Value::CollectionObjects", "input3:B"},
    {"Value::MaterialNodeValue", "input1:I"},
    {"Value::RecipeInfo", "input1:A"},
    {"Value::ListElement", "input1:I"},
    {"Value::ListSetIndex", "input2:A"},
    {"Vector::RotateAroundAxis", "input2:R input2:V"},
    {"Vector::EvaluateCurveAtFactor", "input4:F"},
    {"Vector::BoneHeadWorld", "input1:S"},
    {"Vector::BoneTailWorld", "input1:S"},
    {"Vector::BoneCenterWorld", "input1:S"},
    {"Float::BoneLength", "input1:S"},
    /* The rays and shape casts read their filters after the scene's physics (the rig has none). */
    {"Query::Raycast",
     "input3:A ignore_list_expr_index:A int_expr_index:I string_expr_index:S "
     "secondary_bool_expr_index:B secondary_int_expr_index:I tertiary_bool_expr_index:B "
     "quaternary_bool_expr_index:B quinary_bool_expr_index:B"},
    {"Query::RaycastAll",
     "input3:A ignore_list_expr_index:A int_expr_index:I string_expr_index:S "
     "secondary_bool_expr_index:B secondary_int_expr_index:I tertiary_bool_expr_index:B "
     "quaternary_bool_expr_index:B quinary_bool_expr_index:B"},
    {"Query::ShapeCast",
     "input3:A ignore_list_expr_index:A input4:R input5:V secondary_float_expr_index:F "
     "tertiary_float_expr_index:F int_expr_index:I secondary_int_expr_index:I "
     "string_expr_index:S secondary_bool_expr_index:B tertiary_bool_expr_index:B "
     "quaternary_bool_expr_index:B quinary_bool_expr_index:B"},
    {"Query::ShapeCastAll",
     "input3:A ignore_list_expr_index:A input4:R input5:V secondary_float_expr_index:F "
     "tertiary_float_expr_index:F int_expr_index:I secondary_int_expr_index:I "
     "string_expr_index:S secondary_bool_expr_index:B tertiary_bool_expr_index:B "
     "quaternary_bool_expr_index:B quinary_bool_expr_index:B"},
    /* An end is a position (a vector) or an object, by its endpoint mode. */
    {"Query::DistanceBetween", "input0:V input1:A input1:V"},
};

struct KindInputs {
  /** A read of an index no field of the expression holds (the kind reads through its state). */
  bool unknown = false;
  /** "input0" -> 'F' ...; a field may be listed with more than one family. */
  std::vector<std::pair<std::string, char>> fields;
};

const std::unordered_map<std::string, KindInputs> &KindInputTable()
{
  static const std::unordered_map<std::string, KindInputs> table = [] {
    std::unordered_map<std::string, KindInputs> rows;
    std::vector<KindInputRow> all(std::begin(kKindInputRows), std::end(kKindInputRows));
    all.insert(all.end(), std::begin(kKindInputSupplement), std::end(kKindInputSupplement));
    for (const KindInputRow &row : all) {
      KindInputs &inputs = rows[row.name];
      std::istringstream words(row.fields);
      std::string word;
      while (words >> word) {
        const size_t colon = word.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 2 != word.size()) {
          inputs.unknown = true;
          continue;
        }
        const std::string field = word.substr(0, colon);
        if (field == "(self)") {
          continue; /* a vector kind read as a rotation reads itself */
        }
        if (field[0] == '?') {
          inputs.unknown = true;
          continue;
        }
        inputs.fields.emplace_back(field, word[colon + 1]);
      }
    }
    return rows;
  }();
  return table;
}

bool BridgeableOp(const LN_ExecOpKind kind, const LN_OpCode opcode)
{
  using Op = LN_OpCode;
  switch (kind) {
    case LN_ExecOpKind::ActionCommand:
      return opcode == Op::PlayAction || opcode == Op::StopAction ||
             opcode == Op::SetActionFrame || opcode == Op::SetShapeKeyValue;
    case LN_ExecOpKind::PhysicsCommand:
      switch (opcode) {
        case Op::ApplyImpulse:
        case Op::CharacterJump:
        case Op::SetCharacterGravity:
        case Op::SetCharacterJumpSpeed:
        case Op::SetCharacterMaxJumps:
        case Op::SetCharacterVelocity:
        case Op::SetCharacterWalkDirection:
        case Op::SetCollisionGroup:
        case Op::SetDynamics:
        case Op::SetPhysics:
        case Op::SetRigidBodyAttribute:
        case Op::RebuildCollisionShape:
        case Op::VehicleControl:
        case Op::VehicleApplyEngineForce:
        case Op::VehicleApplyBraking:
        case Op::VehicleApplySteering:
        case Op::SetVehicleSuspensionCompression:
        case Op::SetVehicleSuspensionStiffness:
        case Op::SetVehicleSuspensionDamping:
        case Op::SetVehicleWheelFriction:
        case Op::SlowFollow:
          return true;
        default:
          return false;
      }
    case LN_ExecOpKind::LifecycleCommand:
      /* Add Object makes its copy at once (ExecuteAddObjectFromRefImmediate): the compiler's
       * own rules for it (CompileBridgedOp). */
      return opcode == Op::RemoveObject || opcode == Op::RemoveParent || opcode == Op::AddObject;
    case LN_ExecOpKind::ObjectReferenceCommand:
      /* Recorded commands, applied at the flush. */
      return opcode == Op::SetParent || opcode == Op::ReplaceMesh || opcode == Op::CopyProperty;
    case LN_ExecOpKind::SendEvent:
    case LN_ExecOpKind::BoneCommand:
    case LN_ExecOpKind::MaterialCommand:
    case LN_ExecOpKind::AudioCommand:
    case LN_ExecOpKind::LightCommand:
    case LN_ExecOpKind::DebugDrawCommand:
    case LN_ExecOpKind::NavigationCommand:
      return true;
    case LN_ExecOpKind::GamePropertyCommand:
      return opcode == Op::SetGameProperty || opcode == Op::CopyProperty;
    default:
      /* (Once, Flow Router and Repeat Limit resets stay out: the kernels keep those states' outputs
       * in slots, which a reset made by the VM's code would leave behind.) */
      return opcode == Op::Print;
  }
}

class ColumnCompiler {
 public:
  ColumnCompiler(const LN_Program &program,
                 const LN_LeanProgram &lean,
                 const LN_Event event,
                 LN_ColumnProgram &out)
      : m_program(program), m_lean(lean), m_event(event), m_out(out)
  {
  }

  bool Compile(std::string &r_refusal);
  /** The walk alone (LN_ColumnQueryReachForTests). */
  std::string DescribeReachForTests(const char family, const uint32_t index)
  {
    const QueryReach &reach = ReachOf(family, index);
    if (reach.unknown) {
      return "unknown: " + reach.why;
    }
    std::vector<uint32_t> queries = reach.queries;
    std::sort(queries.begin(), queries.end());
    std::string text;
    for (const uint32_t query : queries) {
      text += (text.empty() ? "q" : " q") + std::to_string(query);
    }
    return text;
  }

 private:

  /** A compiled value: its slot, whether it depends on where it is read in the pass (an op's mask,
   * a tree property, a flow state's output: then it is never shared between reads), and the
   * distances surely read whenever it is read (bits; an input read only on some lanes, as And's
   * second, adds none). */
  struct Slot {
    uint32_t slot = kNone;
    bool reads_ops = false;
    uint64_t queries = 0;
  };
  struct Vec {
    VecSlots slots;
    bool reads_ops = false;
    uint64_t queries = 0;
  };
  /** The bridge (plan C1-8): whether this program may run nodes without a kernel through the VM's
   * code (its queries are ones the run keeps exact), and whether it reads a ray or a distance
   * from its own object (a bridged node after a move of it would not see that move applied). */
  bool m_bridge_ok = false;
  bool m_reads_owner_queries = false;
  /** Set by a refusal the bridge can take instead (a kind or a generic value without a kernel). */
  bool m_want_bridge = false;
  bool BridgeOrRefuse(const std::string &why)
  {
    if (m_bridge_ok && m_refusal.empty()) {
      m_want_bridge = true;
      return false;
    }
    return Refuse(why);
  }
  bool TakeBridge()
  {
    return std::exchange(m_want_bridge, false) && m_refusal.empty();
  }
  /** The queries (rays, distances: a read of one in the VM applies the pending moves of what it
   * measures) an expression may read, through the inputs each kind was seen to read (the recorded
   * kind input table, `LN_KindInputsData.inc`). `unknown`: a kind the table does not know, a read
   * no field explains, or a set field the kind's row does not list; `why` names the first. */
  struct QueryReach {
    bool unknown = false;
    std::string why;
    std::vector<uint32_t> queries;
    /** Some kind it reaches finds or lists objects of the scene (by name, parent, child,
     * collection; a main-thread kind): it could see a copy Add Object made in this tick. */
    bool finds_objects = false;
    /** Some expression it reaches may not be worked out on a helper thread (plan C1-9: the tree
     * batches' lists, plus rays and shape casts, which only read Jolt's world, and Receive
     * Event, which only reads the tick's events); `main_why` names the first. */
    bool main_thread = false;
    std::string main_why;
    /** It reads the owner's logic collisions (the step's contact cache): on a helper thread
     * unless some lane's object queries them on demand (the run looks, LN_ColumnRunner). */
    bool collisions = false;
  };
  /** May a bridged node be compiled here: never inside a dividend or a stateful node's input (the
   * VM evaluates those only sometimes, and the VM's code may keep state), and after a move of the
   * owner only when what it may read is read as the VM reads it there (ReachUnsafeHere). */
  bool BridgeAllowedHere(const char *what, const QueryReach &reach, const bool repeatable = false)
  {
    /* A read that gives the same all the tick and keeps nothing but the tree's per-tick cache
     * (`repeatable`, RepeatableBridgedBool) may sit in a stateful node's input (plan C2 fix 1: a
     * Boolean Edge of Colliding): the VM reads it at that node's first evaluation of the tick, the
     * run at each read site there, with the same result. Never in a dividend (the VM may never
     * read it in the tick). */
    if (!(repeatable && m_in_dividend == 0) && !StatefulAllowed(what)) {
      return false;
    }
    if (m_reads_owner_queries && OwnerMovedHere()) {
      const std::string why = ReachUnsafeHere(reach);
      if (!why.empty()) {
        return Refuse(std::string(what) + " after a move of the owner: " + why);
      }
    }
    if (reach.unknown || reach.finds_objects || !reach.queries.empty()) {
      m_bridged_sees_objects = true;
    }
    m_out.bridged = true;
    return true;
  }
  /** Family: B I F S V R C A Q, as in the table. */
  const QueryReach &ReachOf(char family, uint32_t index);
  /** What the op's handler may read: its instruction's fields and its command payload's. */
  QueryReach OpReach(const LN_Instruction &instruction);
  static void MergeReach(QueryReach &into, const QueryReach &from);
  /** Empty when a bridged node reaching these queries reads what the VM reads here; else why not:
   * after a move of the owner, a ray from it is cast again with the move applied, and a distance
   * from it is the pass's kept one only when it was surely read before every move (as for the
   * native reads, m_owner_move_protect). */
  std::string ReachUnsafeHere(const QueryReach &reach);
  std::unordered_map<uint64_t, QueryReach> m_query_reach;
  /** A bridged node or op may see objects (QueryReach::finds_objects, a query, unknown): with an
   * Add Object in the program, its copies (made at once, NPC by NPC in column order) could be
   * seen where the VM would not have made them yet. */
  bool m_bridged_sees_objects = false;
  uint32_t m_add_objects = 0;
  bool m_add_object_at_owner = false;
  /** Plan C2 P5: a bridged Predict Motion of the owner (it applies the owner's pending moves). */
  bool m_samples_owner = false;
  /** Why some bridged node stays on the main thread (the first; the report says it). */
  void NoteMainThread(const QueryReach &reach)
  {
    if (reach.main_thread) {
      NoteMainThread(reach.main_why);
    }
  }
  void NoteMainThread(const std::string &why)
  {
    /* Each reason once, in the program's order. */
    if (m_out.main_thread_why.find(why) == std::string::npos) {
      m_out.main_thread_why += (m_out.main_thread_why.empty() ? "" : "; ") + why;
    }
  }
  Slot BridgeKind(NodeOp op, uint32_t index, uint32_t out_slot, Node node);
  Slot BridgeBool(uint32_t index, bool fallback);
  Slot BridgeInt(uint32_t index, int32_t fallback);
  Slot BridgeFloat(uint32_t index, float fallback);
  Vec BridgeVector(uint32_t index, const MT_Vector3 &fallback);
  Slot BridgeRotation(uint32_t index, const LN_Rotation &fallback);
  bool BridgeColor(uint32_t index, const MT_Vector4 &fallback, Slot r_rgba[4]);
  /** Plan C1-9's Loops: per op, the body block starting there and the frame whose Completed route
   * holds it (kNone for none). */
  std::vector<uint32_t> m_loop_block_at, m_completed_frame;
  std::unordered_map<uint32_t, uint32_t> m_loop_done_slots;
  /** A Loop body block as one bridged node (BLoop), the bookkeeping of its ops. */
  bool CompileLoopBlock(const LN_ExecBlockProgram &ir,
                        uint32_t block_index,
                        const std::vector<LN_InstructionHeader> &headers,
                        const LN_LeanEvent &lean_event);
  /** Loop frame `frame` completed in this pass (SLoopDone), one node a frame. */
  uint32_t LoopDoneSlot(uint32_t frame);
  /** Add Object's own rules (the bridge makes its copies at once, as the VM does). */
  bool AddObjectAllowedHere(const LN_Instruction &instruction);
  /** A bridged op: its ran slot (the stamp after it), or kNone when refused. */
  uint32_t CompileBridgedOp(const LN_Instruction &instruction,
                            const LN_LeanOp &lean_op,
                            uint32_t mask);
  /** A tree property's fields as the pass has made them so far (SSA), and the last write. */
  struct TreeRef {
    uint32_t f = kNone, i = kNone, b = kNone;
    uint32_t last_write = kNone;
    bool written = false;
  };

  bool Refuse(const std::string &why)
  {
    if (m_refusal.empty()) {
      m_refusal = why;
    }
    return false;
  }
  /** Where an expression being compiled is evaluated by the VM: the AND of these terms (a slot,
   * negated or not), empty = wherever the pass reaches it. A node that changes state when it is
   * evaluated runs only there (EvalMask). */
  struct EvalTerm {
    uint32_t slot;
    bool negate;
  };
  std::vector<EvalTerm> m_eval;
  std::map<std::vector<std::pair<uint32_t, bool>>, uint32_t> m_eval_masks;
  /** Inside a dividend (read only for a divisor not near zero), a stateful node is refused; inside
   * a stateful node's inputs another one is (the VM would evaluate it only sometimes). */
  uint32_t m_in_dividend = 0;
  uint32_t m_in_stateful = 0;
  struct EvalScope {
    ColumnCompiler &compiler;
    size_t size;
    EvalScope(ColumnCompiler &compiler, const uint32_t slot, const bool negate)
        : compiler(compiler), size(compiler.m_eval.size())
    {
      compiler.m_eval.push_back({slot, negate});
    }
    ~EvalScope()
    {
      compiler.m_eval.resize(size);
    }
  };
  uint32_t EvalMask()
  {
    if (m_eval.empty()) {
      return kNone;
    }
    std::vector<std::pair<uint32_t, bool>> key;
    for (const EvalTerm &term : m_eval) {
      key.emplace_back(term.slot, term.negate);
    }
    if (const auto found = m_eval_masks.find(key); found != m_eval_masks.end()) {
      return found->second;
    }
    uint32_t mask = kNone;
    for (const EvalTerm &term : m_eval) {
      uint32_t slot = term.slot;
      if (term.negate) {
        Node not_node;
        not_node.op = NodeOp::BNot;
        not_node.a = slot;
        not_node.out = NewBool();
        m_out.nodes.push_back(not_node);
        slot = not_node.out;
      }
      mask = mask == kNone ? slot : AndSlot(mask, slot);
    }
    m_eval_masks.emplace(key, mask);
    return mask;
  }
  /** A node that changes state when evaluated may be compiled here. */
  bool StatefulAllowed(const char *what)
  {
    if (m_in_dividend != 0) {
      return Refuse(std::string(what) + " inside a dividend");
    }
    if (m_in_stateful != 0) {
      return Refuse(std::string(what) + " inside another stateful node's input");
    }
    return true;
  }
  /** The pass's delta (`raw`: as is, else not below 0) where the op being compiled runs: the
   * gate's time behind a gate (ScopedStepGateTime), else the pass's. */
  Slot DeltaSlot(const bool raw)
  {
    Node node;
    node.out = NewFloat();
    if (m_gate != kNone) {
      node.op = NodeOp::FGateDelta;
      node.property_ref = m_gate;
      node.compare = raw ? 1 : 0;
    }
    else {
      node.op = NodeOp::FUniform;
      node.compare = uint8_t(raw ? Uniform::DeltaTimeRaw : Uniform::DeltaTime);
    }
    m_out.nodes.push_back(node);
    Slot result;
    result.slot = node.out;
    result.reads_ops = true; /* depends on the route that reads it */
    return result;
  }
  /** The stamp index of the instruction `instruction` of this event (InstructionStateIndex). */
  uint32_t StateIndexOf(uint32_t instruction) const;
  uint32_t NewFloat()
  {
    return m_out.float_slots++;
  }
  uint32_t NewInt()
  {
    return m_out.int_slots++;
  }
  uint32_t NewBool()
  {
    return m_out.bool_slots++;
  }
  Slot FloatConst(const float value)
  {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if (const auto found = m_float_constants.find(bits); found != m_float_constants.end()) {
      return {found->second};
    }
    Node node;
    node.op = NodeOp::FConst;
    node.out = NewFloat();
    node.float_value = value;
    m_out.constants.push_back(node);
    m_float_constants.emplace(bits, node.out);
    return {node.out};
  }
  Slot IntConst(const int32_t value)
  {
    if (const auto found = m_int_constants.find(value); found != m_int_constants.end()) {
      return {found->second};
    }
    Node node;
    node.op = NodeOp::IConst;
    node.out = NewInt();
    node.int_value = value;
    m_out.constants.push_back(node);
    m_int_constants.emplace(value, node.out);
    return {node.out};
  }
  Slot BoolConst(const bool value)
  {
    uint32_t &slot = value ? m_true : m_false;
    if (slot == kNone) {
      Node node;
      node.op = NodeOp::BConst;
      node.out = NewBool();
      node.bool_value = value;
      m_out.constants.push_back(node);
      slot = node.out;
    }
    return {slot};
  }
  Vec VectorConst(const MT_Vector3 &value)
  {
    Vec vec;
    vec.slots.x = FloatConst(value.x()).slot;
    vec.slots.y = FloatConst(value.y()).slot;
    vec.slots.z = FloatConst(value.z()).slot;
    return vec;
  }
  /** `a` is read whenever the result is. */
  static Slot Merge(Slot result, const Slot &a)
  {
    result.reads_ops = result.reads_ops || a.reads_ops;
    result.queries |= a.queries;
    return result;
  }
  static Slot Merge(Slot result, const Vec &a)
  {
    result.reads_ops = result.reads_ops || a.reads_ops;
    result.queries |= a.queries;
    return result;
  }
  /** `a` is read only on some lanes (the VM reads it only then): its distances are not surely
   * read. */
  static Slot MergeSometimes(Slot result, const Slot &a)
  {
    result.reads_ops = result.reads_ops || a.reads_ops;
    return result;
  }
  Slot Emit(Node node, Slot result)
  {
    m_out.nodes.push_back(node);
    return result;
  }
  /** A node of one float out of `a` (and `b`): its result. */
  Slot FloatNode(const NodeOp op, const Slot &a, const Slot *b = nullptr, const bool a_sometimes = false)
  {
    Node node;
    node.op = op;
    node.a = a.slot;
    node.b = b != nullptr ? b->slot : kNone;
    node.out = NewFloat();
    Slot result;
    result.slot = node.out;
    result = a_sometimes ? MergeSometimes(result, a) : Merge(result, a);
    if (b != nullptr) {
      result = Merge(result, *b);
    }
    return Emit(node, result);
  }
  /** Per lane `condition ? a : b` (slots of one type: FSelect, ISelect, BSelect). */
  uint32_t SelectSlot(const NodeOp op, const uint32_t condition, const uint32_t a, const uint32_t b)
  {
    Node node;
    node.op = op;
    node.c = condition;
    node.a = a;
    node.b = b;
    node.out = op == NodeOp::FSelect ? NewFloat() : (op == NodeOp::ISelect ? NewInt() : NewBool());
    m_out.nodes.push_back(node);
    return node.out;
  }
  uint32_t AndSlot(const uint32_t a, const uint32_t b)
  {
    Node node;
    node.op = NodeOp::BAnd;
    node.a = a;
    node.b = b;
    node.out = NewBool();
    m_out.nodes.push_back(node);
    return node.out;
  }
  uint32_t OrSlot(const uint32_t a, const uint32_t b)
  {
    Node node;
    node.op = NodeOp::BOr;
    node.a = a;
    node.b = b;
    node.out = NewBool();
    m_out.nodes.push_back(node);
    return node.out;
  }
  uint32_t IntEqSlot(const uint32_t a, const int32_t value)
  {
    Node node;
    node.op = NodeOp::BIntEq;
    node.a = a;
    node.int_value = value;
    node.out = NewBool();
    m_out.nodes.push_back(node);
    return node.out;
  }

  Slot Float(uint32_t index, float fallback);
  Slot Int(uint32_t index, int32_t fallback);
  Slot Bool(uint32_t index, bool fallback);
  Vec Vector(uint32_t index, const MT_Vector3 &fallback);
  /** A rotation input (the lean reader's Rotation): its rotation slot. */
  Slot Rotation(uint32_t index, const LN_Rotation &fallback);
  Slot RotationConst(const LN_Rotation &value)
  {
    Node node;
    node.op = NodeOp::RConst;
    node.rotation = value;
    node.out = m_out.rot_slots++;
    m_out.constants.push_back(node);
    return {node.out};
  }
  Vec Componentwise(NodeOp op, const Vec &a, const Vec &b);
  /** The distance (`squared` false) or squared distance of query `index` (`valid`: its Valid). */
  Slot Distance(uint32_t index, bool squared, bool valid = false);
  /** Plan C2 P3: a Raycast's outputs from a QRay node (one per query and evaluation mask). */
  struct RayOut {
    uint32_t hit = kNone, distance = kNone;
    VecSlots point, normal;
    /** The condition (Done): a bool slot, kNone when it is constant true. */
    uint32_t done = kNone;
    /** The query whose settings the node casts with. */
    uint32_t query = kNone;
  };
  /** Keyed by (state, evaluation mask, condition slot): reads of one Raycast node (one query
   * expression a read, one state) share the node when their settings are the same. */
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::vector<RayOut>> m_rays;
  /** -1 unknown, 0 / 1: the event has an Add Object (its copy is made while the tree records: a
   * later cast of the VM could hit it). */
  int m_event_adds_objects = -1;
  /** Whether query `index` runs as a QRay node here (else the caller bridges or refuses it). */
  bool NativeRay(uint32_t index, RayOut &r_out);
  /** Plan C1-9: a Get Average Center Position query whose source is the same for every NPC (a
   * constant, a list of constants): its index in average_reads; kNone when it is not (the caller
   * bridges it). */
  uint32_t Average(uint32_t query_index);
  bool LaneInvariantValue(uint32_t value_index, int depth = 0) const;
  /** Plan C2 fix 1: Get Game Property of a named object (a constant object and a constant name):
   * the same for every NPC and all the tick (game property writes wait for the flush). */
  bool RunInvariantProperty(uint32_t value_index) const;
  /** A node reading expression `index` of `family` ('F', 'I', 'B') worked out once a run, with the
   * fallback of this read site (LN_ColumnProgram::run_invariants). */
  Slot RunInvariant(char family, uint32_t index, float f, int32_t i, bool b);
  /** Plan C2 fix 1: a bridged Bool kind that may sit in a stateful node's input (Colliding of the
   * owner or a named object by constant filters). */
  bool RepeatableBridgedBool(uint32_t index) const;
  bool Endpoint(LN_DistanceEndpointMode mode,
                uint32_t input,
                bool owner_when_empty,
                EndKind &r_kind,
                uint32_t &r_object,
                VecSlots &r_position,
                Slot &r_reads);
  /** A value expression that names an object (a Constant Object): its index in object_values. */
  bool NamedObject(uint32_t value_index, uint32_t &r_object);
  /** Whether instruction `instruction` ran in the pass so far: the OR of the masks of the ops of
   * it before this one (false when none); `r_always`: one of them marks whenever reached. */
  uint32_t RanSlot(uint32_t instruction, bool &r_always);
  /** The fields of tree property `ref` as the pass has them now (loaded at its first use). */
  TreeRef *Tree(uint32_t ref);
  /** A tree property read as a vector where it is read (VTreeProp; `convert`: as a generic
   * value). */
  Vec TreeVector(uint32_t ref, bool convert, const MT_Vector3 &fallback);
  bool CompileTreeWrite(const LN_Instruction &instruction, uint32_t mask, const std::function<void(uint64_t)> &read);
  /** Whether CompileTreeWrite takes the write (else the bridge runs the VM's handler). */
  bool TreeWriteIsNative(const LN_Instruction &instruction) const;
  /** The fields of tree property `ref` read again (after a bridged write of it). */
  void ReloadTree(uint32_t ref);
  /** A Delay, Timer, Wait for Next Frame, Pulsify, Barrier, Cooldown or Reset Cooldown op. */
  bool CompileTimeOp(const LN_Instruction &instruction,
                     const LN_InstructionHeader &header,
                     uint32_t mask,
                     const std::function<void(uint64_t)> &read);
  /** A time flow state read (STimeRead) of the kind's state `int_value`. */
  Slot TimeReadSlot(int32_t state, TimeRead which, bool fallback);

  const LN_Program &m_program;
  const LN_LeanProgram &m_lean;
  LN_Event m_event;
  LN_ColumnProgram &m_out;
  std::string m_refusal;
  std::unordered_map<uint32_t, uint32_t> m_float_constants;
  std::unordered_map<int32_t, uint32_t> m_int_constants;
  uint32_t m_true = kNone, m_false = kNone;
  std::unordered_map<uint32_t, Slot> m_float_memo, m_int_memo, m_bool_memo;
  std::unordered_map<uint32_t, Vec> m_vector_memo;
  struct QuerySlots {
    uint32_t distance = kNone, squared = kNone, valid = kNone;
    uint64_t bit = 0;
    bool measures_object = false;
    EndKind a = EndKind::None, b = EndKind::None;
    uint32_t object_a = kNone, object_b = kNone;
  };
  /** A generic value a FromGenericValue kind converts: one of a known type in a slot, a constant,
   * or a tree property as the pass has it. */
  struct ValueSource {
    enum class Kind : uint8_t { Float, Int, Bool, Constant, Tree } kind = Kind::Float;
    Slot slot;
    LN_Value constant;
    uint32_t tree_ref = kNone;
    TreeRef tree;
  };
  bool Source(uint32_t value_index, ValueSource &r_source);
  Slot ConvertSource(const ValueSource &source, uint8_t target, LN_FloatToIntMode mode, float fallback);
  std::unordered_map<uint32_t, QuerySlots> m_queries;
  /** The op being compiled, the masks of the ops before it, their instructions, and which of them
   * mark their stamp whenever the pass reaches them. */
  uint32_t m_op = 0;
  std::vector<uint32_t> m_masks;
  std::vector<uint32_t> m_op_instruction;
  std::vector<uint8_t> m_always_marks;
  /** Every N Steps: the gate of each op before this one, this op's (kNone: none), the mask of
   * each gate, and the distances surely read whenever a gate's routes run. */
  std::vector<uint32_t> m_op_gate;
  uint32_t m_gate = kNone;
  std::unordered_map<uint32_t, uint32_t> m_gate_slot;
  std::unordered_map<uint32_t, uint64_t> m_gate_reads;
  /** Distances some op reads whatever the route of the NPC (a guard is read at every op). */
  uint64_t m_always_read = 0;
  /** What reads apply pending moves (distances) and what reads poses live (attributes, axes):
   * never both of one object (the VM's live read would see the moves applied or not depending on
   * where in the pass it happens). */
  bool m_measures_owner = false;
  /** A live read of the owner's pose (Get Object Attribute, Move To's position) sees the pending
   * moves of other trees applied only after the pass's first read of a distance from the owner;
   * the run applies them when it starts. So such a read must follow a sure read of every owner
   * distance (the bits of those compiled so far), and no owner distance may come after it. */
  bool m_owner_live_pose = false;
  uint64_t m_owner_query_bits = 0;
  bool NoteOwnerLiveRead()
  {
    if ((m_owner_query_bits & ~m_always_read) != 0) {
      /* Was refused: "the owner's pose read before a distance from it is surely read". */
      NoteQuietReads("a live pose read of the owner where a distance from it is not surely read");
    }
    m_owner_live_pose = true;
    return true;
  }
  /** Plan C2 fix 1: reads whose order the run cannot keep (a live pose read next to a distance
   * read that applies pending moves, a distance read on some routes only, a velocity read next to
   * a distance) give the VM's answer whenever nothing those reads would apply is pending when the
   * run starts: no other tree moved a lane's object (or its parents) or a measured object this
   * tick, so a distance read applies nothing and every read sees the objects as the run's start
   * does. The run then starts quietly (LN_ColumnProgram::quiet_start: lanes with pending moves go
   * back to the VM, a named object's moves are applied by the first trees played on the VM) instead
   * of the program being refused. The program's own moves stay under the owner move rules. */
  bool m_quiet_reads = false;
  std::string m_quiet_why;
  void NoteQuietReads(const char *why)
  {
    if (!m_quiet_reads) {
      m_quiet_why = why;
    }
    m_quiet_reads = true;
  }
  /** Velocities are read when the run starts: never next to a distance from the owner. */
  bool m_owner_velocity_read = false;
  /** An op before this one moves the owner (its own Get Distance read after it would apply that
   * move first: LN_TreeBatch's owner_reads_before_moves rule). */
  bool m_moved_owner = false;
  /** The owner distances surely read before every owner move compiled so far (the reads of the
   * pass, or of the move's own gate): a later read of one of them is never the pass's first. */
  uint64_t m_owner_move_protect = ~uint64_t(0);
  /** Plan C2 fix 1: each owner move with the mask of the NPCs it runs for and the distances surely
   * read before it. A read where the op being compiled runs (`m_here`) follows only the moves
   * whose mask may hold for one NPC together with it: a move on a Branch's True route never runs
   * before a read on its False route (the dead enemy's teleport and the live one's distance). */
  struct OwnerMove {
    uint32_t mask;
    uint64_t protect;
  };
  std::vector<OwnerMove> m_owner_moves;
  /** The mask of the NPCs the op being compiled runs for (kNone: unknown, e.g. a Loop body). */
  uint32_t m_here = kNone;
  void NoteOwnerMove()
  {
    const uint64_t protect = m_always_read | (m_gate != kNone ? m_gate_reads[m_gate] : 0);
    m_owner_move_protect &= protect;
    m_moved_owner = true;
    m_owner_moves.push_back({m_here, protect});
  }
  /** Whether an owner move may have run before the op being compiled for an NPC it runs for, and
   * (`r_protect`) the owner distances surely read before all such moves. */
  bool OwnerMovedHere(uint64_t &r_protect)
  {
    r_protect = ~uint64_t(0);
    bool moved = false;
    for (const OwnerMove &move : m_owner_moves) {
      if (!Disjoint(move.mask, m_here)) {
        moved = true;
        r_protect &= move.protect;
      }
    }
    return moved;
  }
  bool OwnerMovedHere()
  {
    uint64_t protect;
    return OwnerMovedHere(protect);
  }
  /** Masks as ANDs of literals (a bool slot or its negation): a route's mask is its guard AND its
   * Branch's condition (or the condition negated), Every N Steps' gate, a Loop's done. Slots are
   * written once a pass (SSA), so two masks holding one slot with opposite signs never hold for
   * one NPC. Anything else (an OR of several ops' masks, a comparison) is one literal. */
  struct Lit {
    uint32_t slot;
    bool positive;
  };
  std::unordered_map<uint32_t, std::vector<Lit>> m_mask_lits;
  std::unordered_map<uint32_t, uint32_t> m_bool_producer;
  size_t m_producers_scanned = 0;
  const Node *BoolProducer(const uint32_t slot)
  {
    for (; m_producers_scanned < m_out.nodes.size(); m_producers_scanned++) {
      const Node &node = m_out.nodes[m_producers_scanned];
      if (node.op == NodeOp::BAnd || node.op == NodeOp::BNot) {
        m_bool_producer.emplace(node.out, uint32_t(m_producers_scanned));
      }
    }
    const auto found = m_bool_producer.find(slot);
    return found == m_bool_producer.end() ? nullptr : &m_out.nodes[found->second];
  }
  Lit NegatedLit(const uint32_t slot)
  {
    const Node *producer = BoolProducer(slot);
    if (producer != nullptr && producer->op == NodeOp::BNot) {
      return {producer->a, true};
    }
    return {slot, false};
  }
  const std::vector<Lit> &MaskLits(const uint32_t slot, const int depth = 0)
  {
    if (const auto found = m_mask_lits.find(slot); found != m_mask_lits.end()) {
      return found->second;
    }
    std::vector<Lit> lits;
    const Node *producer = slot == m_true ? nullptr : BoolProducer(slot);
    if (slot == m_true) {
      /* (always: no literal) */
    }
    else if (producer != nullptr && producer->op == NodeOp::BAnd && depth < 256) {
      const uint32_t a = producer->a, b = producer->b;
      lits = MaskLits(a, depth + 1);
      const std::vector<Lit> &more = MaskLits(b, depth + 1);
      lits.insert(lits.end(), more.begin(), more.end());
    }
    else if (producer != nullptr && producer->op == NodeOp::BNot) {
      lits.push_back(NegatedLit(producer->a));
    }
    else {
      lits.push_back({slot, true});
    }
    return m_mask_lits.emplace(slot, std::move(lits)).first->second;
  }
  /** No NPC's pass can run under both masks. */
  bool Disjoint(const uint32_t a, const uint32_t b)
  {
    if ((a != kNone && a == m_false) || (b != kNone && b == m_false)) {
      return true;
    }
    if (a == kNone || b == kNone) {
      return false;
    }
    const std::vector<Lit> la = MaskLits(a);
    const std::vector<Lit> &lb = MaskLits(b);
    for (const Lit &x : la) {
      for (const Lit &y : lb) {
        if (x.slot == y.slot && x.positive != y.positive) {
          return true;
        }
      }
    }
    return false;
  }
  std::unordered_set<uint32_t> m_measured_objects;
  /** Plan C1-9's runtime objects (LN_ColumnProgram::runtime_objects): measured by a distance,
   * read live before a sure distance to them. */
  std::unordered_set<uint32_t> m_measured_runtime;
  std::unordered_set<uint32_t> m_runtime_pose_unprotected;
  /** Runtime objects read live (Get Object Attribute), moved, moved relatively (E2). */
  std::unordered_set<uint32_t> m_pose_read_runtime, m_moved_runtime, m_relative_runtime;
  /** Runtime objects a property write or change writes (plan C1-10). */
  std::unordered_set<uint32_t> m_written_runtime;
  /** The instructions compiled as native property writes, changes and vector commands: their
   * object field names what they write, never an object whose pending moves they apply (their VM
   * handlers record only). */
  std::unordered_set<uint32_t> m_native_write_instructions;
  bool RuntimeObject(uint32_t value_index, uint32_t &r_index);
  /** RuntimeObject for a read in the run (a live pose): not one a collision finds (only written;
   * the bridge reads those). */
  bool RuntimeReadable(const uint32_t value_index, uint32_t &r_index)
  {
    bool invariant = false, collision = false;
    return RuntimeSource(value_index, 0, invariant, collision) && !collision &&
           RuntimeObject(value_index, r_index);
  }
  /** Whether RuntimeObject takes `value_index`, and how it is found (plan C1-10): the same for
   * every NPC, by a collision. */
  bool RuntimeSource(uint32_t value_index, int depth, bool &r_invariant, bool &r_collision) const;
  /** The object a property write or change, a motion or an object state command acts on: the
   * owner (both kNone), a runtime object (`r_runtime`), or the object a tree property the program
   * writes holds where the op runs (`r_column`, plan C1-11: an OTreeObj node is made here). */
  bool CommandTarget(uint32_t value_index, uint32_t &r_runtime, uint32_t &r_column);
  bool ProgramWritesTreeProperty(uint32_t ref) const;
  /** The bits of the distances compiled so far that measure runtime object `object`. */
  uint64_t RuntimeMeasureBits(const uint32_t object) const
  {
    uint64_t bits = 0;
    for (const auto &entry : m_queries) {
      const QuerySlots &query = entry.second;
      if ((query.a == EndKind::Runtime && query.object_a == object) ||
          (query.b == EndKind::Runtime && query.object_b == object))
      {
        bits |= query.bit;
      }
    }
    return bits;
  }
  std::unordered_set<uint32_t> m_live_pose_objects;
  std::map<uint32_t, TreeRef> m_tree;
  /** The flow states' outputs of this pass, once their op has run (by state index). */
  std::unordered_map<uint32_t, uint32_t> m_once_out;
  std::unordered_map<uint32_t, uint32_t> m_router_out;
  std::unordered_map<uint32_t, uint32_t> m_router_last;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> m_repeat_out;
};

uint32_t ColumnCompiler::RanSlot(const uint32_t instruction, bool &r_always)
{
  r_always = false;
  uint32_t ran = kNone;
  for (uint32_t k = 0; k < m_op && k < m_op_instruction.size(); k++) {
    if (m_op_instruction[k] != instruction) {
      continue;
    }
    /* Marks whenever this op's route runs: always marking where its own route runs, and that
     * route runs whenever this one does (no gate, or this op's own). */
    r_always = r_always || (m_always_marks[k] != 0 &&
                            (m_op_gate[k] == kNone || m_op_gate[k] == m_gate));
    ran = ran == kNone ? m_masks[k] : OrSlot(ran, m_masks[k]);
  }
  return ran == kNone ? BoolConst(false).slot : ran;
}

uint32_t ColumnCompiler::StateIndexOf(const uint32_t instruction) const
{
  /* InstructionStateIndex: the event's offset plus the instruction (the lowering's state_index). */
  const LN_LeanEvent &lean_event = m_lean.events[size_t(m_event)];
  const LN_ExecBlockProgram &ir = m_program.GetExecBlockProgram(m_event);
  for (size_t k = 0; k < ir.ops.size() && k < lean_event.ops.size(); k++) {
    if (ir.ops[k].instruction_index == instruction) {
      return lean_event.ops[k].state_index;
    }
  }
  return kNone;
}

ColumnCompiler::TreeRef *ColumnCompiler::Tree(const uint32_t ref)
{
  if (ref >= m_program.GetTreePropertyRefs().size()) {
    Refuse("a tree property without its ref");
    return nullptr;
  }
  auto found = m_tree.find(ref);
  if (found == m_tree.end()) {
    TreeRef tree;
    Node node;
    node.property_ref = ref;
    node.op = NodeOp::FTreeProp;
    node.out = tree.f = NewFloat();
    m_out.nodes.push_back(node);
    node.op = NodeOp::ITreeProp;
    node.out = tree.i = NewInt();
    m_out.nodes.push_back(node);
    node.op = NodeOp::BTreeProp;
    node.out = tree.b = NewBool();
    m_out.nodes.push_back(node);
    tree.last_write = IntConst(-1).slot;
    found = m_tree.emplace(ref, tree).first;
  }
  return &found->second;
}

ColumnCompiler::Vec ColumnCompiler::TreeVector(const uint32_t ref,
                                               const bool convert,
                                               const MT_Vector3 &fallback)
{
  if (ref >= m_program.GetTreePropertyRefs().size()) {
    Refuse("a tree property without its ref");
    return {};
  }
  Node node;
  node.op = NodeOp::VTreeProp;
  node.compare = convert ? 1 : 0;
  node.property_ref = ref;
  node.position_b = VectorConst(fallback).slots;
  node.out = NewFloat();
  node.out2 = NewFloat();
  node.out3 = NewFloat();
  m_out.nodes.push_back(node);
  Vec result;
  result.slots = {node.out, node.out2, node.out3};
  result.reads_ops = true; /* its value depends on where in the pass it is read */
  return result;
}

bool ColumnCompiler::NamedObject(const uint32_t value_index, uint32_t &r_object)
{
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  if (value_index >= values.size() || values[value_index].kind != LN_ValueExpressionKind::Constant ||
      values[value_index].value.type != LN_ValueType::ObjectRef)
  {
    return false;
  }
  const auto found = std::find(m_out.object_values.begin(), m_out.object_values.end(), value_index);
  r_object = uint32_t(found - m_out.object_values.begin());
  if (found == m_out.object_values.end()) {
    m_out.object_values.push_back(value_index);
  }
  return true;
}

ColumnCompiler::Slot ColumnCompiler::Float(const uint32_t index, const float fallback)
{
  const std::vector<LN_FloatExpression> &floats = m_program.GetFloatExpressions();
  if (index == kNone || index >= floats.size()) {
    return FloatConst(fallback);
  }
  if (const auto found = m_float_memo.find(index); found != m_float_memo.end()) {
    return found->second;
  }
  const LN_FloatExpression &e = floats[index];
  if (e.kind == LN_FloatExpressionKind::PhysicsQueryDistance) {
    RayOut ray;
    if (NativeRay(e.input0, ray)) {
      Slot distance;
      distance.slot = ray.distance;
      distance.reads_ops = true;
      return distance;
    }
  }
  Slot result;
  Node node;
  const auto uniform = [&](const Uniform which) {
    node.op = NodeOp::FUniform;
    node.compare = uint8_t(which);
    node.out = NewFloat();
    result.slot = node.out;
    if (which != Uniform::UnscaledDeltaTime) {
      m_out.needs_timing = true;
    }
    return Emit(node, result);
  };
  switch (e.kind) {
    case LN_FloatExpressionKind::Constant:
      result = FloatConst(e.float_value);
      break;
    case LN_FloatExpressionKind::SnapshotGameProperty:
      if (e.property_ref_index >= m_program.GetGamePropertyRefs().size()) {
        Refuse("a game property read without its ref");
        return {};
      }
      node.op = NodeOp::FProp;
      node.property_ref = e.property_ref_index;
      node.float_value = fallback;
      node.out = NewFloat();
      result.slot = node.out;
      result = Emit(node, result);
      break;
    case LN_FloatExpressionKind::RuntimeTreeProperty: {
      TreeRef *tree = Tree(e.property_ref_index);
      if (tree == nullptr) {
        return {};
      }
      result.slot = tree->f;
      result.reads_ops = true; /* its value depends on where in the pass it is read */
      return result;
    }
    case LN_FloatExpressionKind::UnscaledDeltaTime:
      if (m_gate != kNone) {
        /* Behind a gate the route reads its gate's time (ScopedStepGateTime). */
        node.op = NodeOp::FGateUnscaled;
        node.property_ref = m_gate;
        node.out = NewFloat();
        result.slot = node.out;
        result = Emit(node, result);
      }
      else {
        result = uniform(Uniform::UnscaledDeltaTime);
      }
      result.reads_ops = true; /* what it reads depends on the route that reads it */
      return result;
    case LN_FloatExpressionKind::StepTimePassed:
      node.op = NodeOp::FStepTime;
      node.property_ref = e.int_value >= 0 ? uint32_t(e.int_value) : kNone;
      node.out = NewFloat();
      result.slot = node.out;
      result = Emit(node, result);
      break;
    case LN_FloatExpressionKind::SnapshotElapsedTime:
      result = uniform(Uniform::ElapsedTime);
      break;
    case LN_FloatExpressionKind::SnapshotFrameDelta:
      result = uniform(Uniform::FrameDelta);
      break;
    case LN_FloatExpressionKind::SnapshotFPS:
      result = uniform(Uniform::FPS);
      break;
    case LN_FloatExpressionKind::SnapshotDeltaFactor:
      result = uniform(Uniform::DeltaFactor);
      break;
    case LN_FloatExpressionKind::SnapshotTimeScale:
      result = uniform(Uniform::TimeScale);
      break;
    case LN_FloatExpressionKind::Add: {
      const Slot a = Float(e.input0, 0.0f), b = Float(e.input1, 0.0f);
      result = FloatNode(NodeOp::FAdd, a, &b);
      break;
    }
    case LN_FloatExpressionKind::Subtract: {
      const Slot a = Float(e.input0, 0.0f), b = Float(e.input1, 0.0f);
      result = FloatNode(NodeOp::FSub, a, &b);
      break;
    }
    case LN_FloatExpressionKind::Multiply: {
      const Slot a = Float(e.input0, 0.0f), b = Float(e.input1, 0.0f);
      result = FloatNode(NodeOp::FMul, a, &b);
      break;
    }
    case LN_FloatExpressionKind::Divide: {
      /* The dividend is read only for a divisor that is not (almost) zero. */
      const Slot b = Float(e.input1, 0.0f);
      m_in_dividend++;
      const Slot a = Float(e.input0, 0.0f);
      m_in_dividend--;
      result = FloatNode(NodeOp::FDiv, a, &b, true);
      break;
    }
    case LN_FloatExpressionKind::SafeDivide: {
      const Slot b = Float(e.input1, 0.0f);
      m_in_dividend++;
      const Slot a = Float(e.input0, 0.0f);
      m_in_dividend--;
      result = FloatNode(NodeOp::FSafeDiv, a, &b, true);
      break;
    }
    case LN_FloatExpressionKind::Power: {
      const Slot b = Float(e.input1, 0.0f), a = Float(e.input0, 0.0f);
      result = FloatNode(NodeOp::FPow, a, &b);
      break;
    }
    case LN_FloatExpressionKind::Minimum: {
      const Slot b = Float(e.input1, 0.0f), a = Float(e.input0, 0.0f);
      result = FloatNode(NodeOp::FMin, a, &b);
      break;
    }
    case LN_FloatExpressionKind::Maximum: {
      const Slot b = Float(e.input1, 0.0f), a = Float(e.input0, 0.0f);
      result = FloatNode(NodeOp::FMax, a, &b);
      break;
    }
    case LN_FloatExpressionKind::Modulo: {
      const Slot b = Float(e.input1, 0.0f);
      m_in_dividend++;
      const Slot a = Float(e.input0, 0.0f);
      m_in_dividend--;
      result = FloatNode(NodeOp::FMod, a, &b, true);
      break;
    }
    case LN_FloatExpressionKind::CooldownRemaining:
    case LN_FloatExpressionKind::CooldownProgress: {
      if (e.int_value < 0) {
        result = FloatConst(fallback);
        break;
      }
      if (uint32_t(e.int_value) >= m_program.GetTimeFlowStateCount()) {
        Refuse("a time flow read without its state");
        return {};
      }
      node.op = NodeOp::FTimeRead;
      node.compare = e.kind == LN_FloatExpressionKind::CooldownRemaining ? 0 : 1;
      node.property_ref = uint32_t(e.int_value);
      node.float_value = fallback;
      node.out = NewFloat();
      m_out.nodes.push_back(node);
      result.slot = node.out;
      result.reads_ops = true;
      return result;
    }
    case LN_FloatExpressionKind::StoreValue: {
      /* The stored value; its first read stores the input when Store On Init (bool_value). */
      if (!StatefulAllowed("a Store Value read")) {
        return {};
      }
      m_in_stateful++;
      const Slot value = Float(e.input0, e.float_value);
      m_in_stateful--;
      if (!m_refusal.empty()) {
        return {};
      }
      node.op = NodeOp::FStoreValue;
      node.property_ref = index;
      node.bool_value = e.bool_value;
      node.a = value.slot;
      node.d = EvalMask();
      node.out = NewFloat();
      result.slot = node.out;
      m_out.nodes.push_back(node);
      result.reads_ops = true;
      return result;
    }
    case LN_FloatExpressionKind::Absolute:
      result = FloatNode(NodeOp::FAbs, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Negate:
      result = FloatNode(NodeOp::FNeg, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Sign:
      result = FloatNode(NodeOp::FSign, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Round:
      result = FloatNode(NodeOp::FRound, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Floor:
      result = FloatNode(NodeOp::FFloor, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Ceil:
      result = FloatNode(NodeOp::FCeil, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Truncate:
      result = FloatNode(NodeOp::FTrunc, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Fraction:
      result = FloatNode(NodeOp::FFract, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Sine:
      result = FloatNode(NodeOp::FSin, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Cosine:
      result = FloatNode(NodeOp::FCos, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Radians:
      result = FloatNode(NodeOp::FRad, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Degrees:
      result = FloatNode(NodeOp::FDeg, Float(e.input0, 0.0f));
      break;
    case LN_FloatExpressionKind::Clamp: {
      const Slot value = Float(e.input0, 0.0f), low = Float(e.input1, 0.0f),
                 high = Float(e.input2, 1.0f);
      node.op = NodeOp::FClamp;
      node.a = value.slot;
      node.b = low.slot;
      node.c = high.slot;
      node.out = NewFloat();
      result.slot = node.out;
      result = Emit(node, Merge(Merge(Merge(result, value), low), high));
      break;
    }
    case LN_FloatExpressionKind::Threshold: {
      const Slot value = Float(e.input0, 0.0f), threshold = Float(e.input1, 0.0f);
      const Slot else_zero = Bool(e.bool_expr_index, e.bool_value);
      node.op = NodeOp::FThreshold;
      node.compare = uint8_t(e.threshold_operation);
      node.float_value = fallback;
      node.a = value.slot;
      node.b = threshold.slot;
      node.c = else_zero.slot;
      node.out = NewFloat();
      result.slot = node.out;
      result = Emit(node, Merge(Merge(Merge(result, value), threshold), else_zero));
      break;
    }
    case LN_FloatExpressionKind::RangedThreshold: {
      const Slot value = Float(e.input0, 0.0f), low = Float(e.input1, 0.0f),
                 high = Float(e.input2, 0.0f);
      node.op = NodeOp::FRanged;
      node.compare = uint8_t(e.range_operation);
      node.float_value = fallback;
      node.a = value.slot;
      node.b = low.slot;
      node.c = high.slot;
      node.out = NewFloat();
      result.slot = node.out;
      result = Emit(node, Merge(Merge(Merge(result, value), low), high));
      break;
    }
    case LN_FloatExpressionKind::VectorComponent: {
      const Vec vector = Vector(e.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      if (!m_refusal.empty()) {
        return {};
      }
      switch (e.component_index) {
        case 0:
          result.slot = vector.slots.x;
          break;
        case 1:
          result.slot = vector.slots.y;
          break;
        case 2:
          result.slot = vector.slots.z;
          break;
        default:
          result.slot = FloatConst(fallback).slot;
          break;
      }
      result = Merge(result, vector);
      break;
    }
    case LN_FloatExpressionKind::Select: {
      const Slot condition = Bool(e.bool_expr_index, e.bool_value);
      Slot a, b;
      {
        EvalScope scope(*this, condition.slot, false);
        a = Float(e.input0, e.float_value);
      }
      {
        EvalScope scope(*this, condition.slot, true);
        b = Float(e.input1, e.float_value);
      }
      if (!m_refusal.empty()) {
        return {};
      }
      result.slot = SelectSlot(NodeOp::FSelect, condition.slot, a.slot, b.slot);
      result = MergeSometimes(MergeSometimes(Merge(result, condition), a), b);
      break;
    }
    case LN_FloatExpressionKind::Random: {
      const Slot low = Float(e.input0, 0.0f), high = Float(e.input1, 1.0f);
      node.op = NodeOp::FRandom;
      node.property_ref = index;
      node.a = low.slot;
      node.b = high.slot;
      node.out = NewFloat();
      result.slot = node.out;
      result = Emit(node, Merge(Merge(result, low), high));
      break;
    }
    case LN_FloatExpressionKind::FromInt: {
      const Slot a = Int(e.input0, 0);
      node.op = NodeOp::FFromInt;
      node.a = a.slot;
      node.out = NewFloat();
      result.slot = node.out;
      result = Emit(node, Merge(result, a));
      break;
    }
    case LN_FloatExpressionKind::FromBool: {
      const Slot a = Bool(e.bool_expr_index, false);
      node.op = NodeOp::FFromBool;
      node.a = a.slot;
      node.out = NewFloat();
      result.slot = node.out;
      result = Emit(node, Merge(result, a));
      break;
    }
    case LN_FloatExpressionKind::FromGenericValue: {
      if (RunInvariantProperty(e.input0)) {
        return RunInvariant('F', index, fallback, 0, false);
      }
      ValueSource source;
      if (!Source(e.input0, source)) {
        if (TakeBridge()) {
          return BridgeFloat(index, fallback);
        }
        return {};
      }
      result = ConvertSource(source, 0, LN_FloatToIntMode::Truncate, fallback);
      if (result.reads_ops) {
        return result;
      }
      break;
    }
    case LN_FloatExpressionKind::QueryDistance:
      result = Distance(e.input0, false);
      break;
    case LN_FloatExpressionKind::QuerySquaredDistance:
      result = Distance(e.input0, true);
      break;
    default:
      if (m_bridge_ok) {
        return BridgeFloat(index, fallback);
      }
      Refuse(std::string("float kind ") + LN_KindName(LN_KindDomain::Float, uint16_t(e.kind)));
      return {};
  }
  if (!m_refusal.empty()) {
    return {};
  }
  if (!result.reads_ops) {
    m_float_memo.emplace(index, result);
  }
  return result;
}

ColumnCompiler::Slot ColumnCompiler::Int(const uint32_t index, const int32_t fallback)
{
  const std::vector<LN_IntExpression> &ints = m_program.GetIntExpressions();
  if (index == kNone || index >= ints.size()) {
    return IntConst(fallback);
  }
  if (const auto found = m_int_memo.find(index); found != m_int_memo.end()) {
    return found->second;
  }
  const LN_IntExpression &e = ints[index];
  Slot result;
  Node node;
  switch (e.kind) {
    case LN_IntExpressionKind::Constant:
      result = IntConst(e.int_value);
      break;
    case LN_IntExpressionKind::SnapshotGameProperty: {
      if (e.property_ref_index >= m_program.GetGamePropertyRefs().size()) {
        Refuse("a game property read without its ref");
        return {};
      }
      node.op = NodeOp::IProp;
      node.property_ref = e.property_ref_index;
      node.int_value = fallback;
      node.out = NewInt();
      result.slot = node.out;
      result = Emit(node, result);
      break;
    }
    case LN_IntExpressionKind::RuntimeTreeProperty: {
      TreeRef *tree = Tree(e.property_ref_index);
      if (tree == nullptr) {
        return {};
      }
      result.slot = tree->i;
      result.reads_ops = true;
      return result;
    }
    case LN_IntExpressionKind::FromBool: {
      const Slot a = Bool(e.input0, false);
      node.op = NodeOp::IFromBool;
      node.a = a.slot;
      node.out = NewInt();
      result.slot = node.out;
      result = Emit(node, Merge(result, a));
      break;
    }
    case LN_IntExpressionKind::FromFloat: {
      const Slot a = Float(e.input0, 0.0f);
      node.op = NodeOp::IFromFloat;
      node.compare = uint8_t(e.float_to_int_mode);
      node.a = a.slot;
      node.out = NewInt();
      result.slot = node.out;
      result = Emit(node, Merge(result, a));
      break;
    }
    case LN_IntExpressionKind::FromGenericValue: {
      if (RunInvariantProperty(e.input0)) {
        return RunInvariant('I', index, 0.0f, fallback, false);
      }
      ValueSource source;
      if (!Source(e.input0, source)) {
        if (TakeBridge()) {
          return BridgeInt(index, fallback);
        }
        return {};
      }
      result = ConvertSource(source, 1, e.float_to_int_mode, 0.0f);
      if (result.reads_ops) {
        return result;
      }
      break;
    }
    case LN_IntExpressionKind::RepeatLimitRemaining: {
      if (e.int_value < 0 || e.input0 == kNone) {
        result = IntConst(fallback);
        break;
      }
      const Slot limit = Int(e.input0, 0);
      node.op = NodeOp::IRepeatRemaining;
      node.property_ref = uint32_t(e.int_value);
      node.int_value = fallback;
      node.a = limit.slot;
      node.out = NewInt();
      result.slot = node.out;
      result = Emit(node, Merge(result, limit));
      result.reads_ops = true; /* the state as the pass has it here */
      return result;
    }
    case LN_IntExpressionKind::QueryVectorCount: {
      const uint32_t average = Average(e.input0);
      if (average == kNone) {
        if (m_bridge_ok) {
          return BridgeInt(index, fallback);
        }
        Refuse("Get Average Center Position of a source that differs between the NPCs");
        return {};
      }
      node.op = NodeOp::IAvgCount;
      node.property_ref = average;
      node.out = NewInt();
      result.slot = node.out;
      result = Emit(node, result);
      break;
    }
    case LN_IntExpressionKind::Random: {
      const Slot low = Int(e.input0, 0), high = Int(e.input1, 1);
      node.op = NodeOp::IRandom;
      node.property_ref = index;
      node.a = low.slot;
      node.b = high.slot;
      node.out = NewInt();
      result.slot = node.out;
      result = Emit(node, Merge(Merge(result, low), high));
      break;
    }
    default:
      if (m_bridge_ok) {
        return BridgeInt(index, fallback);
      }
      Refuse(std::string("int kind ") + LN_KindName(LN_KindDomain::Int, uint16_t(e.kind)));
      return {};
  }
  if (!m_refusal.empty()) {
    return {};
  }
  if (!result.reads_ops) {
    m_int_memo.emplace(index, result);
  }
  return result;
}

ColumnCompiler::Slot ColumnCompiler::Bool(const uint32_t index, const bool fallback)
{
  const std::vector<LN_BoolExpression> &bools = m_program.GetBoolExpressions();
  if (index == kNone || index >= bools.size()) {
    return BoolConst(fallback);
  }
  if (const auto found = m_bool_memo.find(index); found != m_bool_memo.end()) {
    return found->second;
  }
  const LN_BoolExpression &e = bools[index];
  if (e.kind == LN_BoolExpressionKind::PhysicsQueryHit ||
      e.kind == LN_BoolExpressionKind::PhysicsQueryDone)
  {
    RayOut ray;
    if (NativeRay(e.input0, ray)) {
      Slot read;
      /* Done: the query was not disabled, i.e. its condition (the VM casts to tell). */
      read.slot = e.kind == LN_BoolExpressionKind::PhysicsQueryHit ? ray.hit :
                  ray.done != kNone                                 ? ray.done :
                                                                      BoolConst(true).slot;
      read.reads_ops = true; /* (never kept for another read: each read site is checked) */
      return read;
    }
  }
  Slot result;
  Node node;
  const auto unary = [&](const NodeOp op, const Slot &a) {
    node.op = op;
    node.a = a.slot;
    node.out = NewBool();
    result.slot = node.out;
    result = Emit(node, Merge(result, a));
  };
  switch (e.kind) {
    case LN_BoolExpressionKind::Constant:
      result = BoolConst(e.bool_value);
      break;
    case LN_BoolExpressionKind::SnapshotGameProperty:
      if (e.property_ref_index >= m_program.GetGamePropertyRefs().size()) {
        Refuse("a game property read without its ref");
        return {};
      }
      node.op = NodeOp::BProp;
      node.property_ref = e.property_ref_index;
      node.bool_value = fallback;
      node.out = NewBool();
      result.slot = node.out;
      result = Emit(node, result);
      break;
    case LN_BoolExpressionKind::RuntimeTreeProperty: {
      TreeRef *tree = Tree(e.property_ref_index);
      if (tree == nullptr) {
        return {};
      }
      result.slot = tree->b;
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::Not:
      unary(NodeOp::BNot, Bool(e.input0, false));
      break;
    case LN_BoolExpressionKind::And:
    case LN_BoolExpressionKind::Or: {
      /* The VM reads the second input only when the first does not decide. */
      const Slot a = Bool(e.input0, false);
      Slot b;
      {
        /* The second input is evaluated only where the first does not decide. */
        EvalScope scope(*this, a.slot, e.kind == LN_BoolExpressionKind::Or);
        b = Bool(e.input1, false);
      }
      node.op = e.kind == LN_BoolExpressionKind::And ? NodeOp::BAnd : NodeOp::BOr;
      node.a = a.slot;
      node.b = b.slot;
      node.out = NewBool();
      result.slot = node.out;
      result = Emit(node, MergeSometimes(Merge(result, a), b));
      break;
    }
    case LN_BoolExpressionKind::FloatCompare: {
      const Slot a = Float(e.input0, 0.0f), b = Float(e.input1, 0.0f);
      node.op = NodeOp::BCmp;
      node.compare = uint8_t(e.float_compare_operation);
      node.a = a.slot;
      node.b = b.slot;
      node.out = NewBool();
      result.slot = node.out;
      result = Emit(node, Merge(Merge(result, a), b));
      break;
    }
    case LN_BoolExpressionKind::FromInt:
      unary(NodeOp::BFromInt, Int(e.input0, 0));
      break;
    case LN_BoolExpressionKind::FromFloat:
      unary(NodeOp::BFromFloat, Float(e.input0, 0.0f));
      break;
    case LN_BoolExpressionKind::FromVector: {
      const Vec v = Vector(e.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      node.op = NodeOp::BFromVector;
      node.a = v.slots.x;
      node.b = v.slots.y;
      node.c = v.slots.z;
      node.out = NewBool();
      result.slot = node.out;
      result = Emit(node, Merge(result, v));
      break;
    }
    case LN_BoolExpressionKind::FromGenericValue: {
      if (RunInvariantProperty(e.input0)) {
        return RunInvariant('B', index, 0.0f, 0, fallback);
      }
      ValueSource source;
      if (!Source(e.input0, source)) {
        if (TakeBridge()) {
          return BridgeBool(index, fallback);
        }
        return {};
      }
      result = ConvertSource(source, 2, LN_FloatToIntMode::Truncate, false);
      if (result.reads_ops) {
        return result;
      }
      break;
    }
    case LN_BoolExpressionKind::QueryValid: {
      const std::vector<LN_QueryExpression> &queries = m_program.GetQueryExpressions();
      const LN_QueryExpressionKind query_kind = e.input0 < queries.size() ?
                                                    queries[e.input0].kind :
                                                    LN_QueryExpressionKind::DistanceBetween;
      if (query_kind == LN_QueryExpressionKind::DistanceBetween) {
        result = Distance(e.input0, false, true);
        break;
      }
      const uint32_t average = query_kind == LN_QueryExpressionKind::VectorAverage ?
                                   Average(e.input0) :
                                   kNone;
      if (average == kNone) {
        /* A ray's or a shape cast's Valid, an average of a source that differs: the VM's code. */
        if (m_bridge_ok) {
          return BridgeBool(index, fallback);
        }
        Refuse(std::string("the Valid of a query of another kind: ") +
               LN_KindName(LN_KindDomain::Query, uint16_t(query_kind)));
        return {};
      }
      node.op = NodeOp::BAvgValid;
      node.property_ref = average;
      node.out = NewBool();
      result.slot = node.out;
      result = Emit(node, result);
      break;
    }
    case LN_BoolExpressionKind::SnapshotGamePropertyExists:
      if (e.property_ref_index >= m_program.GetGamePropertyRefs().size()) {
        Refuse("a game property check without its ref");
        return {};
      }
      node.op = NodeOp::BExists;
      node.property_ref = e.property_ref_index;
      node.out = NewBool();
      result.slot = node.out;
      result = Emit(node, result);
      break;
    case LN_BoolExpressionKind::BooleanEdge: {
      if (!StatefulAllowed("a Boolean Edge")) {
        return {};
      }
      m_in_stateful++;
      const Slot condition = Bool(e.input0, false);
      m_in_stateful--;
      if (!m_refusal.empty()) {
        return {};
      }
      node.op = NodeOp::SEdge;
      node.property_ref = index;
      node.a = condition.slot;
      node.d = EvalMask();
      node.out = NewBool();
      m_out.nodes.push_back(node);
      result.slot = node.out;
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::BooleanEdgeFalling: {
      if (e.input0 >= bools.size() || bools[e.input0].kind != LN_BoolExpressionKind::BooleanEdge) {
        result = BoolConst(fallback);
        break;
      }
      /* Evaluates the edge (it updates), then reads its falling side. */
      Bool(e.input0, false);
      if (!m_refusal.empty()) {
        return {};
      }
      node.op = NodeOp::SEdgeFalling;
      node.property_ref = e.input0;
      node.d = EvalMask();
      node.out = NewBool();
      m_out.nodes.push_back(node);
      result.slot = node.out;
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::StoreValueDone: {
      const std::vector<LN_FloatExpression> &floats = m_program.GetFloatExpressions();
      if (e.float_expr_index >= floats.size() ||
          floats[e.float_expr_index].kind != LN_FloatExpressionKind::StoreValue)
      {
        result = BoolConst(fallback);
        break;
      }
      if (!StatefulAllowed("Store Value's Flow")) {
        return {};
      }
      const LN_FloatExpression &store = floats[e.float_expr_index];
      m_in_stateful++;
      const Slot value = Float(store.input0, store.float_value);
      m_in_stateful--;
      if (!m_refusal.empty()) {
        return {};
      }
      node.op = NodeOp::SStoreDone;
      node.property_ref = e.float_expr_index;
      node.a = value.slot;
      node.d = EvalMask();
      node.out = NewBool();
      m_out.nodes.push_back(node);
      result.slot = node.out;
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::RepeatLimitExhausted: {
      if (e.int_value < 0 || e.input0 == kNone) {
        result = BoolConst(fallback);
        break;
      }
      const Slot limit = Int(e.input0, 0);
      node.op = NodeOp::BRepeatExhausted;
      node.property_ref = uint32_t(e.int_value);
      node.bool_value = fallback;
      node.a = limit.slot;
      node.out = NewBool();
      result.slot = node.out;
      result = Emit(node, Merge(result, limit));
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::FlowGateOpen: {
      /* Its first evaluation takes Start Open. */
      if (!StatefulAllowed("a Flow Gate read")) {
        return {};
      }
      m_in_stateful++;
      const Slot start_open = Bool(e.input0, false);
      m_in_stateful--;
      if (!m_refusal.empty()) {
        return {};
      }
      node.op = NodeOp::SGateRead;
      node.property_ref = index;
      node.b = start_open.slot;
      node.d = EvalMask();
      node.out = NewBool();
      m_out.nodes.push_back(node);
      result.slot = node.out;
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::TimerElapsed:
    case LN_BoolExpressionKind::DelayDone:
    case LN_BoolExpressionKind::NextFrameDone:
    case LN_BoolExpressionKind::CooldownCompleted:
      return TimeReadSlot(e.int_value, TimeRead::Pulse, fallback);
    case LN_BoolExpressionKind::PulsifyPulse:
      return TimeReadSlot(e.int_value, TimeRead::PulsifyPulse, fallback);
    case LN_BoolExpressionKind::BarrierPassed:
      return TimeReadSlot(e.int_value, TimeRead::BarrierPassed, fallback);
    case LN_BoolExpressionKind::CooldownAccepted:
      return TimeReadSlot(e.int_value, TimeRead::CooldownAccepted, fallback);
    case LN_BoolExpressionKind::CooldownBlocked:
      return TimeReadSlot(e.int_value, TimeRead::CooldownBlocked, fallback);
    case LN_BoolExpressionKind::CooldownReady:
      return TimeReadSlot(e.int_value, TimeRead::CooldownReady, fallback);
    case LN_BoolExpressionKind::InstructionReached: {
      const uint32_t state = StateIndexOf(e.input0);
      if (state == kNone) {
        Refuse("a Reached of an instruction outside the event");
        return {};
      }
      node.op = NodeOp::SReached;
      node.property_ref = state;
      node.out = NewBool();
      m_out.nodes.push_back(node);
      result.slot = node.out;
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::InstructionExecuted: {
      /* The stamp of an op of this pass: its mask once it ran, false before. */
      bool always = false;
      result.slot = RanSlot(e.input0, always);
      result.reads_ops = true;
      return result; /* never kept: its value depends on where it is read */
    }
    case LN_BoolExpressionKind::Once: {
      /* True in the pass its Once ran and let the flow through. */
      const auto found = m_once_out.find(index);
      result = found != m_once_out.end() ? Slot{found->second} : BoolConst(false);
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::FlowRouterSelected: {
      if (e.int_value < 0 || e.secondary_int_value < 0) {
        Refuse("a Flow Router output without its state");
        return {};
      }
      const auto found = m_router_out.find(uint32_t(e.int_value));
      result = found != m_router_out.end() ?
                   Slot{IntEqSlot(found->second, e.secondary_int_value)} :
                   BoolConst(false);
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::FlowRouterLastRoute: {
      if (e.int_value < 0 || e.secondary_int_value < 0) {
        Refuse("a Flow Router output without its state");
        return {};
      }
      auto found = m_router_last.find(uint32_t(e.int_value));
      if (found == m_router_last.end()) {
        Node load;
        load.op = NodeOp::SFlowLast;
        load.property_ref = uint32_t(e.int_value);
        load.out = NewInt();
        m_out.nodes.push_back(load);
        found = m_router_last.emplace(uint32_t(e.int_value), load.out).first;
      }
      result.slot = IntEqSlot(found->second, e.secondary_int_value);
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::RepeatLimitAccepted:
    case LN_BoolExpressionKind::RepeatLimitBlocked: {
      if (e.int_value < 0) {
        Refuse("a Repeat Limit output without its state");
        return {};
      }
      const auto found = m_repeat_out.find(uint32_t(e.int_value));
      if (found == m_repeat_out.end()) {
        result = BoolConst(false);
      }
      else {
        result.slot = e.kind == LN_BoolExpressionKind::RepeatLimitAccepted ? found->second.first :
                                                                             found->second.second;
      }
      result.reads_ops = true;
      return result;
    }
    case LN_BoolExpressionKind::ValueChanged: {
      /* Of a bool, an int or a float (plan C2 fix 1: the villagers' awake flag, the crates'
       * speed): the value made from a slot, the VM's own step NPC by NPC. Anything else
       * through the bridge. */
      const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
      const LN_ValueExpression *source = e.input0 < values.size() ? &values[e.input0] : nullptr;
      const int type = source == nullptr                                 ? -1 :
                       source->kind == LN_ValueExpressionKind::FromBool  ? 0 :
                       source->kind == LN_ValueExpressionKind::FromInt   ? 1 :
                       source->kind == LN_ValueExpressionKind::FromFloat ? 2 :
                                                                           -1;
      if (type < 0) {
        if (m_bridge_ok) {
          return BridgeBool(index, fallback);
        }
        Refuse("On Value Changed of a value that is no bool, int or float");
        return {};
      }
      if (!StatefulAllowed("On Value Changed")) {
        return {};
      }
      m_in_stateful++;
      const Slot value = type == 0 ? Bool(source->input0, false) :
                         type == 1 ? Int(source->input0, 0) :
                                     Float(source->input0, 0.0f);
      m_in_stateful--;
      if (!m_refusal.empty()) {
        return {};
      }
      node.op = NodeOp::SValueChanged;
      node.property_ref = index;
      node.compare = uint8_t(type);
      node.bool_value = e.bool_value;
      node.a = value.slot;
      node.d = EvalMask();
      node.out = NewBool();
      m_out.nodes.push_back(node);
      result.slot = node.out;
      result.reads_ops = true;
      return result;
    }
    default:
      if (m_bridge_ok) {
        return BridgeBool(index, fallback);
      }
      Refuse(std::string("bool kind ") + LN_KindName(LN_KindDomain::Bool, uint16_t(e.kind)));
      return {};
  }
  if (!m_refusal.empty()) {
    return {};
  }
  if (!result.reads_ops) {
    m_bool_memo.emplace(index, result);
  }
  return result;
}

ColumnCompiler::Vec ColumnCompiler::Componentwise(const NodeOp op, const Vec &a, const Vec &b)
{
  Vec result;
  const uint32_t a_axes[3] = {a.slots.x, a.slots.y, a.slots.z};
  const uint32_t b_axes[3] = {b.slots.x, b.slots.y, b.slots.z};
  uint32_t out[3];
  for (int axis = 0; axis < 3; axis++) {
    Node node;
    node.op = op;
    node.a = a_axes[axis];
    node.b = b_axes[axis];
    node.out = out[axis] = NewFloat();
    m_out.nodes.push_back(node);
  }
  result.slots = {out[0], out[1], out[2]};
  result.reads_ops = a.reads_ops || b.reads_ops;
  result.queries = a.queries | b.queries;
  return result;
}

ColumnCompiler::Vec ColumnCompiler::Vector(const uint32_t index, const MT_Vector3 &fallback)
{
  const std::vector<LN_VectorExpression> &vectors = m_program.GetVectorExpressions();
  if (index == kNone || index >= vectors.size()) {
    return VectorConst(fallback);
  }
  if (const auto found = m_vector_memo.find(index); found != m_vector_memo.end()) {
    return found->second;
  }
  const LN_VectorExpression &e = vectors[index];
  if (e.kind == LN_VectorExpressionKind::PhysicsQueryPoint ||
      e.kind == LN_VectorExpressionKind::PhysicsQueryNormal)
  {
    RayOut ray;
    if (NativeRay(e.input0, ray)) {
      Vec vector;
      vector.slots = e.kind == LN_VectorExpressionKind::PhysicsQueryPoint ? ray.point : ray.normal;
      vector.reads_ops = true;
      return vector;
    }
  }
  const MT_Vector3 zero(0.0f, 0.0f, 0.0f);
  Vec result;
  switch (e.kind) {
    case LN_VectorExpressionKind::Constant:
      result = VectorConst(e.vector_value);
      break;
    case LN_VectorExpressionKind::VectorToRotation: {
      /* Read order (D20): the second input first. */
      const Vec up = Vector(e.input1, MT_Vector3(0.0f, 1.0f, 0.0f));
      const Vec forward = Vector(e.input0, MT_Vector3(0.0f, 0.0f, 1.0f));
      if (!m_refusal.empty()) {
        return {};
      }
      Node node;
      node.op = NodeOp::VVecToRot;
      node.int_value = int32_t(e.property_ref_index);
      node.position_a = forward.slots;
      node.position_b = up.slots;
      node.out = NewFloat();
      node.out2 = NewFloat();
      node.out3 = NewFloat();
      m_out.nodes.push_back(node);
      result.slots = {node.out, node.out2, node.out3};
      result.reads_ops = up.reads_ops || forward.reads_ops;
      result.queries = up.queries | forward.queries;
      break;
    }
    case LN_VectorExpressionKind::Combine: {
      /* Each axis falls back to the node's own value. */
      const Slot z = Float(e.input2, e.vector_value.z());
      const Slot y = Float(e.input1, e.vector_value.y());
      const Slot x = Float(e.input0, e.vector_value.x());
      result.slots = {x.slot, y.slot, z.slot};
      result.reads_ops = x.reads_ops || y.reads_ops || z.reads_ops;
      result.queries = x.queries | y.queries | z.queries;
      break;
    }
    case LN_VectorExpressionKind::Add: {
      const Vec b = Vector(e.input1, zero), a = Vector(e.input0, zero);
      result = Componentwise(NodeOp::FAdd, a, b);
      break;
    }
    case LN_VectorExpressionKind::Subtract: {
      const Vec b = Vector(e.input1, zero), a = Vector(e.input0, zero);
      result = Componentwise(NodeOp::FSub, a, b);
      break;
    }
    case LN_VectorExpressionKind::Multiply: {
      const Vec a = Vector(e.input0, zero), b = Vector(e.input1, zero);
      result = Componentwise(NodeOp::FMul, a, b);
      break;
    }
    case LN_VectorExpressionKind::Divide: {
      const Vec a = Vector(e.input0, zero), b = Vector(e.input1, zero);
      result = Componentwise(NodeOp::FDiv, a, b);
      break;
    }
    case LN_VectorExpressionKind::Minimum: {
      const Vec a = Vector(e.input0, zero), b = Vector(e.input1, zero);
      result = Componentwise(NodeOp::FMin, a, b);
      break;
    }
    case LN_VectorExpressionKind::Maximum: {
      const Vec a = Vector(e.input0, zero), b = Vector(e.input1, zero);
      result = Componentwise(NodeOp::FMax, a, b);
      break;
    }
    case LN_VectorExpressionKind::Absolute: {
      const Vec a = Vector(e.input0, zero);
      result = Componentwise(NodeOp::FAbs, a, a);
      break;
    }
    case LN_VectorExpressionKind::Scale: {
      const Slot factor = Float(e.float_expr_index, e.float_value);
      const Vec v = Vector(e.input0, zero);
      Vec f;
      f.slots = {factor.slot, factor.slot, factor.slot};
      f.reads_ops = factor.reads_ops;
      f.queries = factor.queries;
      result = Componentwise(NodeOp::FMul, v, f);
      break;
    }
    case LN_VectorExpressionKind::Normalize: {
      const Vec v = Vector(e.input0, zero);
      if (!m_refusal.empty()) {
        return {};
      }
      Node node;
      node.op = NodeOp::VNormalize;
      node.a = v.slots.x;
      node.b = v.slots.y;
      node.c = v.slots.z;
      node.out = NewFloat();
      node.out2 = NewFloat();
      node.out3 = NewFloat();
      m_out.nodes.push_back(node);
      result.slots = {node.out, node.out2, node.out3};
      result.reads_ops = v.reads_ops;
      result.queries = v.queries;
      break;
    }
    case LN_VectorExpressionKind::RotateAroundAxis: {
      const int32_t mode = int32_t(e.property_ref_index) / 4;
      const int32_t fixed_axis = int32_t(e.property_ref_index) % 4;
      if (mode == 3) {
        Refuse("Vector Rotate by a rotation");
        return {};
      }
      const Vec origin = Vector(e.input0, zero);
      const Vec pivot = Vector(e.input1, zero);
      Vec axis = VectorConst(MT_Vector3(0.0f, 0.0f, 1.0f));
      if (mode == 2) {
        axis = Vector(e.input2, MT_Vector3(0.0f, 0.0f, 1.0f));
      }
      const Slot angle = Float(e.float_expr_index, e.float_value);
      if (!m_refusal.empty()) {
        return {};
      }
      Node node;
      node.op = NodeOp::VRotate;
      node.compare = uint8_t(mode < 0 ? 0 : mode);
      node.int_value = fixed_axis;
      node.position_a = origin.slots;
      node.position_b = pivot.slots;
      node.vec_c = axis.slots;
      node.a = angle.slot;
      node.out = NewFloat();
      node.out2 = NewFloat();
      node.out3 = NewFloat();
      m_out.nodes.push_back(node);
      result.slots = {node.out, node.out2, node.out3};
      result.reads_ops = origin.reads_ops || pivot.reads_ops || axis.reads_ops || angle.reads_ops;
      result.queries = origin.queries | pivot.queries | axis.queries | angle.queries;
      break;
    }
    case LN_VectorExpressionKind::Random: {
      const Vec axes = Vector(e.input0, MT_Vector3(1.0f, 1.0f, 1.0f));
      if (!m_refusal.empty()) {
        return {};
      }
      Node node;
      node.op = NodeOp::VRandom;
      node.property_ref = index;
      node.position_a = axes.slots;
      node.out = NewFloat();
      node.out2 = NewFloat();
      node.out3 = NewFloat();
      m_out.nodes.push_back(node);
      result.slots = {node.out, node.out2, node.out3};
      result.reads_ops = axes.reads_ops;
      result.queries = axes.queries;
      break;
    }
    case LN_VectorExpressionKind::SnapshotGravity: {
      m_out.needs_gravity = true;
      uint32_t axes[3];
      for (int axis = 0; axis < 3; axis++) {
        Node node;
        node.op = NodeOp::FUniform;
        node.compare = uint8_t(axis == 0 ? Uniform::GravityX :
                               axis == 1 ? Uniform::GravityY :
                                           Uniform::GravityZ);
        node.out = axes[axis] = NewFloat();
        m_out.nodes.push_back(node);
      }
      result.slots = {axes[0], axes[1], axes[2]};
      break;
    }
    case LN_VectorExpressionKind::Resize: {
      const Vec v = Vector(e.input0, zero);
      result = v;
      if (e.float_value < 3.0f) {
        result.slots.z = FloatConst(0.0f).slot;
      }
      break;
    }
    case LN_VectorExpressionKind::BroadcastFloat: {
      const Slot s = Float(e.float_expr_index, 0.0f);
      result.slots = {s.slot, s.slot, s.slot};
      result.reads_ops = s.reads_ops;
      result.queries = s.queries;
      break;
    }
    case LN_VectorExpressionKind::BroadcastInt: {
      const Slot i = Int(e.input0, 0);
      Node node;
      node.op = NodeOp::FFromInt;
      node.a = i.slot;
      node.out = NewFloat();
      m_out.nodes.push_back(node);
      result.slots = {node.out, node.out, node.out};
      result.reads_ops = i.reads_ops;
      result.queries = i.queries;
      break;
    }
    case LN_VectorExpressionKind::RuntimeTreeProperty:
      /* Plan C1-11: VectorKind's tree property read, from the value as it is here. */
      return TreeVector(e.property_ref_index, false, fallback);
    case LN_VectorExpressionKind::FromGenericValue: {
      /* Get Object Attribute's vector: the typed read (FastObjectAttribute), which gives what the
       * value's path gives (the verify mode checks it). */
      const LN_RegisterExpressionProgram &ir = m_program.GetRegisterExpressionProgram();
      const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
      if (e.input0 >= values.size() || e.input0 >= ir.value_object_attribute.size()) {
        Refuse("a vector from a value");
        return {};
      }
      if (values[e.input0].kind == LN_ValueExpressionKind::RuntimeTreeProperty) {
        /* Plan C1-11: ValueAsVector of the tree property's value (else the fallback). */
        return TreeVector(values[e.input0].property_ref_index, true, fallback);
      }
      const LN_ObjectAttributeId attribute = LN_ObjectAttributeId(
          ir.value_object_attribute[e.input0]);
      switch (attribute) {
        case LN_ObjectAttributeId::WorldPosition:
        case LN_ObjectAttributeId::LocalPosition:
        case LN_ObjectAttributeId::WorldOrientation:
        case LN_ObjectAttributeId::LocalOrientation:
        case LN_ObjectAttributeId::WorldScale:
        case LN_ObjectAttributeId::LocalScale:
        case LN_ObjectAttributeId::Color:
          break;
        case LN_ObjectAttributeId::WorldLinearVelocity:
        case LN_ObjectAttributeId::LocalLinearVelocity:
        case LN_ObjectAttributeId::WorldAngularVelocity:
        case LN_ObjectAttributeId::LocalAngularVelocity: {
          LN_ColumnProgram::VelocityRead velocity;
          velocity.attribute = attribute;
          const LN_ValueExpression &source = values[e.input0];
          if (source.input0 == kNone) {
            m_owner_velocity_read = true;
          }
          else if (NamedObject(source.input0, velocity.object)) {
            velocity.owner = false;
            m_live_pose_objects.insert(velocity.object);
          }
          else {
            if (m_bridge_ok) {
              return BridgeVector(index, fallback);
            }
            Refuse("an attribute of an object that is not named");
            return {};
          }
          Node node;
          node.op = NodeOp::VVel;
          node.property_ref = uint32_t(m_out.velocity_reads.size());
          node.position_b = VectorConst(fallback).slots;
          m_out.velocity_reads.push_back(velocity);
          node.out = NewFloat();
          node.out2 = NewFloat();
          node.out3 = NewFloat();
          m_out.nodes.push_back(node);
          result.slots = {node.out, node.out2, node.out3};
          break;
        }
        default:
          if (m_bridge_ok) {
            return BridgeVector(index, fallback);
          }
          Refuse("a vector from a value that is no pose attribute");
          return {};
      }
      if (result.slots.x != kNone) {
        break; /* a velocity */
      }
      Node node;
      node.op = NodeOp::VAttr;
      node.compare = uint8_t(attribute);
      const LN_ValueExpression &source = values[e.input0];
      if (source.input0 == kNone) {
        node.end_a = EndKind::Owner;
        if (!NoteOwnerLiveRead()) {
          return {};
        }
      }
      else if (NamedObject(source.input0, node.object_a)) {
        node.end_a = EndKind::Object;
        m_live_pose_objects.insert(node.object_a);
      }
      else if (RuntimeReadable(source.input0, node.object_a)) {
        /* An object the NPC found while playing, read live: after a sure distance to it (the
         * VM's read applied its pending moves first, as the run's start did), or in a program
         * measuring no distance to it (CompileEnd). */
        node.end_a = EndKind::Runtime;
        m_pose_read_runtime.insert(node.object_a);
        if ((m_always_read & RuntimeMeasureBits(node.object_a)) == 0) {
          m_runtime_pose_unprotected.insert(node.object_a);
        }
      }
      else {
        if (m_bridge_ok) {
          return BridgeVector(index, fallback);
        }
        Refuse("an attribute of an object that is not named");
        return {};
      }
      node.position_b = VectorConst(fallback).slots;
      node.out = NewFloat();
      node.out2 = NewFloat();
      node.out3 = NewFloat();
      m_out.nodes.push_back(node);
      result.slots = {node.out, node.out2, node.out3};
      break;
    }
    case LN_VectorExpressionKind::QueryVectorAverage: {
      const uint32_t average = Average(e.input0);
      if (average == kNone) {
        if (m_bridge_ok) {
          return BridgeVector(index, fallback);
        }
        Refuse("Get Average Center Position of a source that differs between the NPCs");
        return {};
      }
      Node node;
      node.op = NodeOp::VAvg;
      node.property_ref = average;
      node.out = NewFloat();
      node.out2 = NewFloat();
      node.out3 = NewFloat();
      m_out.nodes.push_back(node);
      result.slots = {node.out, node.out2, node.out3};
      break;
    }
    case LN_VectorExpressionKind::AxisVector: {
      /* The owner's axis from its snapshot; another object's from its live pose. */
      Node node;
      node.op = NodeOp::VAxis;
      node.int_value = int32_t(e.property_ref_index);
      if (e.input0 == kNone) {
        node.end_a = EndKind::Owner;
        m_out.needs_owner_transform = true;
      }
      else if (NamedObject(e.input0, node.object_a)) {
        node.end_a = EndKind::Object;
        m_live_pose_objects.insert(node.object_a);
      }
      else if (RuntimeReadable(e.input0, node.object_a)) {
        /* Plan C1-11: an object the NPC found, its live pose (as Get Object Attribute's). */
        node.end_a = EndKind::Runtime;
        m_pose_read_runtime.insert(node.object_a);
        if ((m_always_read & RuntimeMeasureBits(node.object_a)) == 0) {
          m_runtime_pose_unprotected.insert(node.object_a);
        }
      }
      else {
        if (m_bridge_ok) {
          return BridgeVector(index, fallback);
        }
        Refuse("an axis of an object that is not named");
        return {};
      }
      node.position_b = VectorConst(fallback).slots;
      node.out = NewFloat();
      node.out2 = NewFloat();
      node.out3 = NewFloat();
      m_out.nodes.push_back(node);
      result.slots = {node.out, node.out2, node.out3};
      break;
    }
    default:
      if (m_bridge_ok) {
        return BridgeVector(index, fallback);
      }
      Refuse(std::string("vector kind ") + LN_KindName(LN_KindDomain::Vector, uint16_t(e.kind)));
      return {};
  }
  if (!m_refusal.empty()) {
    return {};
  }
  if (!result.reads_ops) {
    m_vector_memo.emplace(index, result);
  }
  return result;
}

bool ColumnCompiler::Endpoint(const LN_DistanceEndpointMode mode,
                              const uint32_t input,
                              const bool owner_when_empty,
                              EndKind &r_kind,
                              uint32_t &r_object,
                              VecSlots &r_position,
                              Slot &r_reads)
{
  r_kind = EndKind::None;
  if (mode == LN_DistanceEndpointMode::Position) {
    if (input == kNone) {
      return true; /* no position: the distance is invalid */
    }
    const Vec position = Vector(input, MT_Vector3(0.0f, 0.0f, 0.0f));
    if (!m_refusal.empty()) {
      return false;
    }
    r_kind = EndKind::Position;
    r_position = position.slots;
    r_reads.reads_ops = r_reads.reads_ops || position.reads_ops;
    r_reads.queries |= position.queries;
    return true;
  }
  if (mode != LN_DistanceEndpointMode::Object) {
    return Refuse("a distance end of another mode");
  }
  if (input == kNone) {
    if (owner_when_empty) {
      r_kind = EndKind::Owner;
      m_out.needs_owner_position = true;
      m_measures_owner = true;
    }
    return true;
  }
  if (NamedObject(input, r_object)) {
    r_kind = EndKind::Object;
    m_measured_objects.insert(r_object);
    return true;
  }
  if (RuntimeReadable(input, r_object)) {
    r_kind = EndKind::Runtime;
    m_measured_runtime.insert(r_object);
    return true;
  }
  return Refuse("a distance to an object that is neither named nor found from a property");
}

uint32_t ColumnCompiler::Average(const uint32_t query_index)
{
  const std::vector<LN_QueryExpression> &queries = m_program.GetQueryExpressions();
  if (query_index >= queries.size()) {
    return kNone;
  }
  const LN_QueryExpression &e = queries[query_index];
  if (e.kind != LN_QueryExpressionKind::VectorAverage || e.condition_bool_expr_index != kNone ||
      !LaneInvariantValue(e.input0))
  {
    return kNone;
  }
  m_out.live_positions = true;
  const auto found = std::find(
      m_out.average_reads.begin(), m_out.average_reads.end(), query_index);
  if (found != m_out.average_reads.end()) {
    return uint32_t(found - m_out.average_reads.begin());
  }
  m_out.average_reads.push_back(query_index);
  return uint32_t(m_out.average_reads.size() - 1);
}

bool ColumnCompiler::LaneInvariantValue(const uint32_t value_index, const int depth) const
{
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  if (value_index == kNone) {
    return true; /* in.Value's fallback: nothing */
  }
  if (value_index >= values.size() || depth > 16) {
    return false;
  }
  const LN_ValueExpression &value = values[value_index];
  switch (value.kind) {
    case LN_ValueExpressionKind::Constant:
      /* An object or a collection by name: the same for every lane (no lane is a menu copy, the
       * one case whose names mean other objects). */
      return true;
    case LN_ValueExpressionKind::ListFromItems:
      for (const uint32_t item : value.input_indices) {
        if (!LaneInvariantValue(item, depth + 1)) {
          return false;
        }
      }
      return true;
    default:
      return false;
  }
}

bool ColumnCompiler::RuntimeObject(const uint32_t value_index, uint32_t &r_index)
{
  bool invariant = false, collision = false;
  if (!RuntimeSource(value_index, 0, invariant, collision)) {
    return false;
  }
  const auto found = std::find(
      m_out.runtime_objects.begin(), m_out.runtime_objects.end(), value_index);
  r_index = uint32_t(found - m_out.runtime_objects.begin());
  if (found == m_out.runtime_objects.end()) {
    m_out.runtime_objects.push_back(value_index);
  }
  return true;
}

bool ColumnCompiler::RuntimeSource(const uint32_t value_index,
                                   const int depth,
                                   bool &r_invariant,
                                   bool &r_collision) const
{
  r_invariant = false;
  r_collision = false;
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  if (value_index >= values.size() || depth > 8) {
    return false;
  }
  const LN_ValueExpression &value = values[value_index];
  const std::vector<LN_StringExpression> &strings = m_program.GetStringExpressions();
  const auto constant_text = [&](const uint32_t string) {
    return string < strings.size() && strings[string].kind == LN_StringExpressionKind::Constant &&
           !strings[string].string_value.empty();
  };
  const auto owner = [&](const uint32_t input) {
    return input == kNone ||
           (input < values.size() && values[input].kind == LN_ValueExpressionKind::OwnerObject);
  };
  switch (value.kind) {
    case LN_ValueExpressionKind::Constant:
      /* A named object (plan C1-10): the same for every NPC (no lane is a menu copy, the one case
       * whose names mean other objects). */
      r_invariant = true;
      return value.value.type == LN_ValueType::ObjectRef;
    case LN_ValueExpressionKind::ObjectGameProperty:
      /* The owner's property (writes wait for the flush: the same all the tick). */
      return owner(value.input0) && constant_text(value.input1);
    case LN_ValueExpressionKind::ObjectByName: {
      if (constant_text(value.input0)) {
        r_invariant = true;
        return true;
      }
      /* A name the NPC keeps (plan C1-10): its own text property (the snapshot's, the same all
       * the tick) or a text tree property the program never writes. Names find the same object
       * all the pass: copies are made only by Add Object, refused next to these (CompileEnd) and
       * in the trees a group runs between them (Plan). */
      if (value.input0 >= strings.size()) {
        return false;
      }
      const LN_StringExpression &name = strings[value.input0];
      if (name.kind == LN_StringExpressionKind::SnapshotGameProperty) {
        return name.property_ref_index < m_program.GetGamePropertyRefs().size();
      }
      if (name.kind == LN_StringExpressionKind::RuntimeTreeProperty) {
        return name.property_ref_index != kNone &&
               !ProgramWritesTreeProperty(name.property_ref_index);
      }
      return false;
    }
    case LN_ValueExpressionKind::RuntimeTreeProperty:
      /* Never written in the program: the same all the pass. */
      return value.property_ref_index != kNone &&
             !ProgramWritesTreeProperty(value.property_ref_index);
    case LN_ValueExpressionKind::ObjectChildByName:
    case LN_ValueExpressionKind::ObjectChild: {
      /* A child of the owner (its turret, its weapon): by the name of a named object, or by a
       * constant index. Children change only at the flush (Set / Remove Parent, Remove Object
       * are commands) and Add Object is refused next to these (CompileEnd). */
      if (value.kind == LN_ValueExpressionKind::ObjectChildByName) {
        return owner(value.input0) && value.input1 < values.size() &&
               values[value.input1].kind == LN_ValueExpressionKind::Constant &&
               values[value.input1].value.type == LN_ValueType::ObjectRef;
      }
      const std::vector<LN_IntExpression> &ints = m_program.GetIntExpressions();
      return owner(value.input0) && value.input1 < ints.size() &&
             ints[value.input1].kind == LN_IntExpressionKind::Constant;
    }
    case LN_ValueExpressionKind::ObjectParent:
      /* Plan C1-10: the parent of the owner (a lane has none: nothing), or of an object found
       * as these are (parents change only at the flush). */
      if (owner(value.input0)) {
        return true;
      }
      return RuntimeSource(value.input0, depth + 1, r_invariant, r_collision);
    case LN_ValueExpressionKind::ActiveCamera:
      /* Plan C2 P5: the scene's active camera (Set Camera is a command: the same all the tick). */
      r_invariant = true;
      return true;
    case LN_ValueExpressionKind::PlayerViewCamera: {
      /* Plan C2 P5: a player's camera, of a constant player (the views are placed before the
       * drawing, Set Camera is a command: the same all the tick). */
      const std::vector<LN_IntExpression> &ints = m_program.GetIntExpressions();
      if (value.input0 == kNone ||
          (value.input0 < ints.size() && ints[value.input0].kind == LN_IntExpressionKind::Constant))
      {
        r_invariant = true;
        return true;
      }
      return false;
    }
    case LN_ValueExpressionKind::CollisionHitObject: {
      /* Plan C1-10: the first object the owner touches (the contacts of the last physics step,
       * the same all the tick while nothing applies a move: CompileEnd, Plan), with constant
       * property and material filters. */
      const bool material_ok = value.input2 == kNone ||
                               (value.input2 < values.size() &&
                                values[value.input2].kind == LN_ValueExpressionKind::Constant);
      const bool property_ok = value.input1 == kNone ||
                               (value.input1 < strings.size() &&
                                strings[value.input1].kind == LN_StringExpressionKind::Constant);
      r_collision = true;
      return owner(value.input0) && property_ok && material_ok;
    }
    default:
      return false;
  }
}

bool ColumnCompiler::CommandTarget(const uint32_t value_index,
                                   uint32_t &r_runtime,
                                   uint32_t &r_column)
{
  r_runtime = kNone;
  r_column = kNone;
  if (value_index == kNone) {
    return true; /* ResolveObjectExpression: the owner */
  }
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  if (value_index < values.size() &&
      values[value_index].kind == LN_ValueExpressionKind::OwnerObject)
  {
    return true;
  }
  if (RuntimeObject(value_index, r_runtime)) {
    m_written_runtime.insert(r_runtime);
    return true;
  }
  /* Plan C1-11: an object the NPC remembers in a tree property the program writes (its target):
   * ResolveObjectValue of the property's value where the op runs (Set Tree Property writes at
   * once, so the value there is the VM's at the op). */
  if (value_index < values.size() &&
      values[value_index].kind == LN_ValueExpressionKind::RuntimeTreeProperty &&
      values[value_index].property_ref_index < m_program.GetTreePropertyRefs().size())
  {
    Node node;
    node.op = NodeOp::OTreeObj;
    node.property_ref = values[value_index].property_ref_index;
    r_column = m_out.target_columns++;
    node.int_value = int32_t(r_column);
    m_out.nodes.push_back(node);
    return true;
  }
  return false;
}

bool ColumnCompiler::ProgramWritesTreeProperty(const uint32_t ref) const
{
  const std::vector<LN_InstructionHeader> &headers = m_program.GetInstructionHeaders(m_event);
  for (const LN_InstructionHeader &header : headers) {
    const LN_Instruction &instruction = m_program.GetInstructionPayload(m_event, header);
    /* Every op that sets a tree property while the tree records (Set Tree Property, Add Object's
     * copy, Inventory's result, Assign Geometry Nodes Modifier's result). */
    if ((instruction.opcode == LN_OpCode::SetTreeProperty ||
         instruction.opcode == LN_OpCode::AddObject ||
         instruction.opcode == LN_OpCode::InventoryEdit ||
         instruction.opcode == LN_OpCode::AssignGeometryNodesModifier) &&
        instruction.property_ref_index == ref)
    {
      return true;
    }
  }
  return false;
}

bool ColumnCompiler::NativeRay(const uint32_t index, RayOut &r_out)
{
  if (!column_native_rays_on()) {
    return false;
  }
  const std::vector<LN_QueryExpression> &queries = m_program.GetQueryExpressions();
  if (index >= queries.size()) {
    return false;
  }
  const LN_QueryExpression &q = queries[index];
  /* A Raycast from the owner (Local) or between world points, its settings constants: the cast is
   * QueryKind's own code, its inputs read as the VM reads them (constants: no state, no order).
   * Not here (bridged): Raycast All, a caster or an ignore list, a condition, debug drawing (the
   * main thread draws), inputs worked out per NPC. */
  if (q.kind != LN_QueryExpressionKind::Raycast || q.input0 != kNone) {
    return false;
  }
  /* No object to ignore: none, an empty object (ResolveObjectValue gives none) and no list (an
   * empty or missing one: ResolveIgnoreObjectRefs adds nothing). */
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  if (q.input3 != kNone) {
    if (q.input3 >= values.size() || values[q.input3].kind != LN_ValueExpressionKind::Constant) {
      return false;
    }
    const LN_Value &object = values[q.input3].value;
    if (object.type == LN_ValueType::ObjectRef &&
        (object.runtime_ref.IsValid() || !object.reference_name.empty()))
    {
      return false;
    }
  }
  if (q.ignore_list_expr_index != kNone) {
    if (q.ignore_list_expr_index >= values.size() ||
        values[q.ignore_list_expr_index].kind != LN_ValueExpressionKind::Constant)
    {
      return false;
    }
    const LN_Value &list = values[q.ignore_list_expr_index].value;
    if (list.exists && list.type == LN_ValueType::List && !list.list_value.empty()) {
      return false;
    }
  }
  const auto vector_const = [&](const uint32_t i) {
    const std::vector<LN_VectorExpression> &v = m_program.GetVectorExpressions();
    return i == kNone || (i < v.size() && v[i].kind == LN_VectorExpressionKind::Constant);
  };
  const auto float_const = [&](const uint32_t i) {
    const std::vector<LN_FloatExpression> &v = m_program.GetFloatExpressions();
    return i == kNone || (i < v.size() && v[i].kind == LN_FloatExpressionKind::Constant);
  };
  const auto int_const = [&](const uint32_t i) {
    const std::vector<LN_IntExpression> &v = m_program.GetIntExpressions();
    return i == kNone || (i < v.size() && v[i].kind == LN_IntExpressionKind::Constant);
  };
  const auto string_const = [&](const uint32_t i) {
    const std::vector<LN_StringExpression> &v = m_program.GetStringExpressions();
    return i == kNone || (i < v.size() && v[i].kind == LN_StringExpressionKind::Constant);
  };
  const std::vector<LN_BoolExpression> &bools = m_program.GetBoolExpressions();
  const auto bool_const = [&](const uint32_t i) {
    return i == kNone || (i < bools.size() && bools[i].kind == LN_BoolExpressionKind::Constant);
  };
  if (!vector_const(q.input1) || !vector_const(q.input2) || !vector_const(q.input4) ||
      !float_const(q.float_expr_index) || !bool_const(q.bool_expr_index) ||
      !int_const(q.int_expr_index) || !string_const(q.string_expr_index) ||
      !bool_const(q.secondary_bool_expr_index) || !bool_const(q.quaternary_bool_expr_index) ||
      !bool_const(q.quinary_bool_expr_index))
  {
    return false;
  }
  if (q.tertiary_bool_expr_index != kNone &&
      !(bool_const(q.tertiary_bool_expr_index) && !bools[q.tertiary_bool_expr_index].bool_value))
  {
    return false;
  }
  /* The condition (the Raycast's Flow): constant true (an event's flow) or an op that ran. */
  uint32_t done = kNone;
  if (q.condition_bool_expr_index != kNone) {
    if (q.condition_bool_expr_index >= bools.size()) {
      return false;
    }
    const LN_BoolExpression &condition = bools[q.condition_bool_expr_index];
    if (condition.kind == LN_BoolExpressionKind::Constant) {
      if (!condition.bool_value) {
        return false;
      }
    }
    else if (condition.kind == LN_BoolExpressionKind::InstructionExecuted) {
      bool always = false;
      done = RanSlot(condition.input0, always);
    }
    else {
      return false;
    }
  }
  const bool local = q.bool_expr_index == kNone ? q.bool_value :
                                                  bools[q.bool_expr_index].bool_value;
  /* After a move of the owner the VM's cast from it applies that move first. */
  if (local && OwnerMovedHere()) {
    return false;
  }
  if (m_event_adds_objects < 0) {
    m_event_adds_objects = 0;
    for (const LN_InstructionHeader &header : m_program.GetInstructionHeaders(m_event)) {
      if (m_program.GetInstructionPayload(m_event, header).opcode == LN_OpCode::AddObject) {
        m_event_adds_objects = 1;
      }
    }
  }
  if (m_event_adds_objects != 0) {
    return false;
  }
  const auto same_settings = [&](const LN_QueryExpression &a, const LN_QueryExpression &b) {
    return a.kind == b.kind && a.ray_input_mode == b.ray_input_mode &&
           a.ray_query_detail_flags == b.ray_query_detail_flags && a.input1 == b.input1 &&
           a.input2 == b.input2 && a.input4 == b.input4 &&
           a.float_expr_index == b.float_expr_index && a.bool_expr_index == b.bool_expr_index &&
           a.int_expr_index == b.int_expr_index && a.string_expr_index == b.string_expr_index &&
           a.secondary_bool_expr_index == b.secondary_bool_expr_index &&
           a.tertiary_bool_expr_index == b.tertiary_bool_expr_index &&
           a.quaternary_bool_expr_index == b.quaternary_bool_expr_index &&
           a.quinary_bool_expr_index == b.quinary_bool_expr_index &&
           a.condition_bool_expr_index == b.condition_bool_expr_index &&
           a.vector_value == b.vector_value && a.secondary_vector_value == b.secondary_vector_value &&
           a.tertiary_vector_value == b.tertiary_vector_value && a.float_value == b.float_value &&
           a.int_value == b.int_value && a.bool_value == b.bool_value;
  };
  const uint32_t state = q.runtime_state_index != kNone ? q.runtime_state_index : index;
  const std::tuple<uint32_t, uint32_t, uint32_t> key(state, EvalMask(), done);
  std::vector<RayOut> &shared = m_rays[key];
  for (const RayOut &existing : shared) {
    if (same_settings(queries[existing.query], q)) {
      r_out = existing;
      return true;
    }
  }
  Node node;
  node.op = NodeOp::QRay;
  node.property_ref = index;
  node.d = std::get<1>(key);
  node.a = done;
  node.compare = uint8_t(m_gate == kNone ? 0 : m_gate + 1);
  node.out = NewBool();
  node.out2 = NewFloat();
  node.position_a = {NewFloat(), NewFloat(), NewFloat()};
  node.position_b = {NewFloat(), NewFloat(), NewFloat()};
  m_out.nodes.push_back(node);
  m_out.native_rays = true;
  RayOut out;
  out.hit = node.out;
  out.distance = node.out2;
  out.point = node.position_a;
  out.normal = node.position_b;
  out.done = done;
  out.query = index;
  shared.push_back(out);
  r_out = out;
  return true;
}

ColumnCompiler::Slot ColumnCompiler::Distance(const uint32_t index, const bool squared, const bool valid)
{
  auto found = m_queries.find(index);
  if (found == m_queries.end()) {
    const std::vector<LN_QueryExpression> &queries = m_program.GetQueryExpressions();
    if (index >= queries.size()) {
      Refuse("a distance without its query");
      return {};
    }
    const LN_QueryExpression &e = queries[index];
    if (e.kind != LN_QueryExpressionKind::DistanceBetween || e.condition_bool_expr_index != kNone)
    {
      Refuse("a distance query that is not between fixed ends");
      return {};
    }
    if (m_queries.size() >= 64) {
      Refuse("more than 64 distances");
      return {};
    }
    Node node;
    node.op = NodeOp::QDistance;
    Slot reads;
    if (!Endpoint(e.distance_endpoint_a, e.input0, true, node.end_a, node.object_a, node.position_a, reads) ||
        !Endpoint(e.distance_endpoint_b, e.input1, false, node.end_b, node.object_b, node.position_b, reads))
    {
      return {};
    }
    if (reads.reads_ops) {
      Refuse("a distance end that reads an op");
      return {};
    }
    if (!LN_DistanceQueryEndsAreFixed(m_program, e) &&
        !((node.end_a == EndKind::Runtime || node.end_b == EndKind::Runtime) &&
          e.distance_endpoint_a == LN_DistanceEndpointMode::Object &&
          e.distance_endpoint_b == LN_DistanceEndpointMode::Object))
    {
      /* The VM works a distance that is not between fixed ends out again at every op that reads
       * it (after the pass's moves): only one between objects, one of them found while playing
       * (it changes nowhere in the run; no read after a move of the owner), runs here. */
      Refuse("a distance query that is not between fixed ends");
      return {};
    }
    if (node.end_a == EndKind::Position &&
        (node.end_b == EndKind::Object || node.end_b == EndKind::Runtime))
    {
      /* The VM reads end B only when end A is valid: whether a position is, the run cannot know
       * when it starts (it would apply B's pending moves or not). */
      Refuse("a distance from a position to an object");
      return {};
    }
    const bool owner_end = node.end_a == EndKind::Owner || node.end_b == EndKind::Owner;
    if (owner_end && m_owner_live_pose) {
      /* The live read before it saw other trees' pending moves unapplied (was refused: "a
       * distance from the owner after a live read of its pose"). */
      NoteQuietReads("a distance from the owner after a live read of its pose");
    }
    node.out = NewFloat();
    node.out2 = NewFloat();
    node.out3 = NewBool();
    m_out.nodes.push_back(node);
    QuerySlots slots;
    slots.distance = node.out;
    slots.squared = node.out2;
    slots.valid = node.out3;
    slots.bit = uint64_t(1) << m_queries.size();
    if (owner_end) {
      m_owner_query_bits |= slots.bit;
    }
    /* A read of the owner or of a named object applies its pending moves first: the run applies
     * them when it starts, so every route must read it. */
    slots.measures_object = node.end_a == EndKind::Object || node.end_b == EndKind::Object ||
                            node.end_a == EndKind::Owner || node.end_b == EndKind::Owner ||
                            node.end_a == EndKind::Runtime || node.end_b == EndKind::Runtime;
    slots.a = node.end_a;
    slots.b = node.end_b;
    const auto object_of = [](const EndKind kind, const uint32_t object) {
      return kind == EndKind::Object || kind == EndKind::Runtime ? object : kNone;
    };
    slots.object_a = object_of(node.end_a, node.object_a);
    slots.object_b = object_of(node.end_b, node.object_b);
    found = m_queries.emplace(index, slots).first;
  }
  Slot result;
  result.slot = valid ? found->second.valid : (squared ? found->second.squared : found->second.distance);
  result.queries = found->second.bit;
  const bool runtime_end = found->second.a == EndKind::Runtime ||
                           found->second.b == EndKind::Runtime;
  if (runtime_end && (found->second.a == EndKind::Owner || found->second.b == EndKind::Owner) &&
      OwnerMovedHere())
  {
    /* Not a fixed distance: the VM works it out again at every op that reads it, the owner's
     * moves of the pass applied first. */
    Refuse("a distance from the owner to an object found while playing, after a move of it");
    return {};
  }
  if (runtime_end) {
    result.reads_ops = true;
  }
  if (found->second.a == EndKind::Owner || found->second.b == EndKind::Owner) {
    /* Taken once a pass at its first read: after a move of the owner the VM's read applies that
     * move first, while the run reads the owner when it starts. So every read of it is checked
     * where it is (nothing that reads it is kept for another op): it must follow a sure read of
     * it before every owner move so far (the pass's or the move's gate's) that may run before it
     * for one NPC (a move on another route of a Branch never does). */
    uint64_t protect = 0;
    if (OwnerMovedHere(protect) && (protect & found->second.bit) == 0) {
      Refuse("a distance from the owner first read after a move of it");
      return {};
    }
    result.reads_ops = true;
  }
  return result;
}

bool ColumnCompiler::Source(const uint32_t value_index, ValueSource &r_source)
{
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  if (value_index == kNone || value_index >= values.size()) {
    /* in.Value's fallback: MakeNoneValue (converts to nothing). */
    r_source.kind = ValueSource::Kind::Constant;
    r_source.constant = LN_Value();
    return true;
  }
  const LN_ValueExpression &value = values[value_index];
  switch (value.kind) {
    case LN_ValueExpressionKind::FromFloat:
      r_source.kind = ValueSource::Kind::Float;
      r_source.slot = Float(value.input0, 0.0f);
      return m_refusal.empty();
    case LN_ValueExpressionKind::FromInt:
      r_source.kind = ValueSource::Kind::Int;
      r_source.slot = Int(value.input0, 0);
      return m_refusal.empty();
    case LN_ValueExpressionKind::FromBool:
      r_source.kind = ValueSource::Kind::Bool;
      r_source.slot = Bool(value.input0, false);
      return m_refusal.empty();
    case LN_ValueExpressionKind::Constant:
      /* An object constant resolves at run time; others convert as they are. */
      if (value.value.type == LN_ValueType::ObjectRef ||
          value.value.type == LN_ValueType::CollectionRef ||
          value.value.type == LN_ValueType::SceneRef ||
          value.value.type == LN_ValueType::DatablockRef)
      {
        return Refuse("a generic value of an object");
      }
      r_source.kind = ValueSource::Kind::Constant;
      r_source.constant = value.value;
      return true;
    case LN_ValueExpressionKind::RuntimeTreeProperty: {
      TreeRef *tree = Tree(value.property_ref_index);
      if (tree == nullptr) {
        return false;
      }
      r_source.kind = ValueSource::Kind::Tree;
      r_source.tree_ref = value.property_ref_index;
      r_source.tree = *tree;
      r_source.slot.reads_ops = true;
      return true;
    }
    default:
      return BridgeOrRefuse("a generic value of another kind");
  }
}

ColumnCompiler::Slot ColumnCompiler::ConvertSource(const ValueSource &source,
                                                   const uint8_t target,
                                                   const LN_FloatToIntMode mode,
                                                   const float fallback)
{
  /* ValueAsFloat / ValueAsInt (through the float) / ValueAsBool of the value. */
  using K = ValueSource::Kind;
  Slot result;
  Node node;
  const auto emit = [&](const NodeOp op, const Slot &a) {
    Node n;
    n.op = op;
    n.a = a.slot;
    n.compare = uint8_t(mode);
    n.out = op == NodeOp::IFromFloat ? NewInt() :
            (op == NodeOp::BFromInt || op == NodeOp::BFromFloat) ? NewBool() :
                                                                     NewFloat();
    m_out.nodes.push_back(n);
    Slot out;
    out.slot = n.out;
    return Merge(out, a);
  };
  switch (source.kind) {
    case K::Float:
      return target == 0 ? source.slot :
             target == 1 ? emit(NodeOp::IFromFloat, source.slot) :
                           emit(NodeOp::BFromFloat, source.slot);
    case K::Int:
      return target == 0 ? emit(NodeOp::FFromInt, source.slot) :
             target == 1 ? emit(NodeOp::IFromFloat, emit(NodeOp::FFromInt, source.slot)) :
                           emit(NodeOp::BFromInt, source.slot);
    case K::Bool:
      return target == 0 ? emit(NodeOp::FFromBool, source.slot) :
             target == 1 ? emit(NodeOp::IFromFloat, emit(NodeOp::FFromBool, source.slot)) :
                           source.slot;
    case K::Constant: {
      if (target == 0) {
        float value = 0.0f;
        return FloatConst(LN_RuntimeTree::ColumnValueAsFloat(source.constant, value) ? value :
                                                                                        fallback);
      }
      if (target == 1) {
        return IntConst(LN_RuntimeTree::ColumnValueAsInt(source.constant, mode));
      }
      return BoolConst(LN_RuntimeTree::ColumnValueAsBool(source.constant));
    }
    case K::Tree:
      node.op = NodeOp::VTreeConvert;
      node.compare = target;
      node.int_value = int32_t(mode);
      node.float_value = fallback;
      node.property_ref = source.tree_ref;
      node.a = source.tree.f;
      node.b = source.tree.i;
      node.c = source.tree.b;
      node.d = source.tree.last_write;
      node.out = target == 0 ? NewFloat() : (target == 1 ? NewInt() : NewBool());
      m_out.nodes.push_back(node);
      result.slot = node.out;
      result.reads_ops = true;
      return result;
  }
  return result;
}

ColumnCompiler::Slot ColumnCompiler::Rotation(const uint32_t index, const LN_Rotation &fallback)
{
  /* LeanRotation / RotationKind. */
  const std::vector<LN_VectorExpression> &vectors = m_program.GetVectorExpressions();
  if (index == kNone || index >= vectors.size()) {
    return RotationConst(fallback);
  }
  const LN_VectorExpression &e = vectors[index];
  Node node;
  Slot result;
  switch (e.kind) {
    case LN_VectorExpressionKind::Constant:
      return RotationConst(e.output_type == LN_ValueType::Rotation ?
                               e.rotation_value :
                               LN_Rotation::FromEulerXYZ(e.vector_value));
    case LN_VectorExpressionKind::Combine: {
      const Slot x = Float(e.input0, e.vector_value.x());
      const Slot y = Float(e.input1, e.vector_value.y());
      const Slot z = Float(e.input2, e.vector_value.z());
      node.op = NodeOp::RFromEuler;
      node.a = x.slot;
      node.b = y.slot;
      node.c = z.slot;
      result = Merge(Merge(Merge(result, x), y), z);
      break;
    }
    case LN_VectorExpressionKind::VectorToRotation: {
      /* Read order (D20): the second input first. */
      const Vec up = Vector(e.input1, MT_Vector3(0.0f, 1.0f, 0.0f));
      const Vec forward = Vector(e.input0, MT_Vector3(0.0f, 0.0f, 1.0f));
      node.op = NodeOp::RVecToRot;
      node.int_value = int32_t(e.property_ref_index);
      node.position_a = forward.slots;
      node.position_b = up.slots;
      result.reads_ops = up.reads_ops || forward.reads_ops;
      result.queries = up.queries | forward.queries;
      break;
    }
    case LN_VectorExpressionKind::FromGenericValue: {
      /* Get Object Attribute's typed read (FastObjectAttribute). */
      const LN_RegisterExpressionProgram &ir = m_program.GetRegisterExpressionProgram();
      const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
      if (e.input0 >= values.size() || e.input0 >= ir.value_object_attribute.size()) {
        Refuse("a rotation from a value");
        return {};
      }
      const LN_ObjectAttributeId attribute = LN_ObjectAttributeId(
          ir.value_object_attribute[e.input0]);
      switch (attribute) {
        case LN_ObjectAttributeId::WorldPosition:
        case LN_ObjectAttributeId::LocalPosition:
        case LN_ObjectAttributeId::WorldOrientation:
        case LN_ObjectAttributeId::LocalOrientation:
        case LN_ObjectAttributeId::WorldScale:
        case LN_ObjectAttributeId::LocalScale:
        case LN_ObjectAttributeId::Color:
          break;
        default:
          if (m_bridge_ok) {
            return BridgeRotation(index, fallback);
          }
          Refuse("a rotation from a value that is no pose attribute");
          return {};
      }
      node.op = NodeOp::RAttr;
      node.compare = uint8_t(attribute);
      node.rotation = fallback;
      const LN_ValueExpression &source = values[e.input0];
      if (source.input0 == kNone) {
        node.end_a = EndKind::Owner;
        if (!NoteOwnerLiveRead()) {
          return {};
        }
      }
      else if (NamedObject(source.input0, node.object_a)) {
        node.end_a = EndKind::Object;
        m_live_pose_objects.insert(node.object_a);
      }
      else {
        if (m_bridge_ok) {
          return BridgeRotation(index, fallback);
        }
        Refuse("an attribute of an object that is not named");
        return {};
      }
      break;
    }
    case LN_VectorExpressionKind::RuntimeTreeProperty:
    case LN_VectorExpressionKind::BoneRotationToObjectRotation:
    case LN_VectorExpressionKind::TweenRotationResult:
      if (m_bridge_ok) {
        return BridgeRotation(index, fallback);
      }
      Refuse(std::string("rotation kind ") +
             LN_KindName(LN_KindDomain::Vector, uint16_t(e.kind)));
      return {};
    default: {
      /* Any other vector, as Euler angles. */
      const Vec euler = Vector(index, fallback.ToEulerXYZ());
      node.op = NodeOp::RFromEuler;
      node.a = euler.slots.x;
      node.b = euler.slots.y;
      node.c = euler.slots.z;
      result.reads_ops = euler.reads_ops;
      result.queries = euler.queries;
      break;
    }
  }
  if (!m_refusal.empty()) {
    return {};
  }
  node.out = m_out.rot_slots++;
  m_out.nodes.push_back(node);
  result.slot = node.out;
  return result;
}

bool ColumnCompiler::RunInvariantProperty(const uint32_t value_index) const
{
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  const std::vector<LN_StringExpression> &strings = m_program.GetStringExpressions();
  if (value_index >= values.size()) {
    return false;
  }
  const LN_ValueExpression &value = values[value_index];
  if (value.kind != LN_ValueExpressionKind::ObjectGameProperty || value.input0 >= values.size()) {
    return false;
  }
  const LN_ValueExpression &object = values[value.input0];
  /* (A menu copy's names mean its own objects: its runs hold it alone, read by its own tree.) */
  return object.kind == LN_ValueExpressionKind::Constant &&
         object.value.type == LN_ValueType::ObjectRef && value.input1 < strings.size() &&
         strings[value.input1].kind == LN_StringExpressionKind::Constant;
}

ColumnCompiler::Slot ColumnCompiler::RunInvariant(const char family,
                                                   const uint32_t index,
                                                   const float f,
                                                   const int32_t i,
                                                   const bool b)
{
  uint32_t entry = 0;
  for (; entry < m_out.run_invariants.size(); entry++) {
    const LN_ColumnProgram::RunInvariant &known = m_out.run_invariants[entry];
    if (known.family == family && known.index == index &&
        std::memcmp(&known.float_fallback, &f, sizeof(f)) == 0 && known.int_fallback == i &&
        known.bool_fallback == b)
    {
      break;
    }
  }
  if (entry == m_out.run_invariants.size()) {
    LN_ColumnProgram::RunInvariant read;
    read.family = family;
    read.index = index;
    read.float_fallback = f;
    read.int_fallback = i;
    read.bool_fallback = b;
    m_out.run_invariants.push_back(read);
  }
  Node node;
  node.op = family == 'F' ? NodeOp::FRunInv : (family == 'I' ? NodeOp::IRunInv : NodeOp::BRunInv);
  node.property_ref = entry;
  node.out = family == 'F' ? NewFloat() : (family == 'I' ? NewInt() : NewBool());
  m_out.nodes.push_back(node);
  Slot result;
  result.slot = node.out;
  return result;
}

bool ColumnCompiler::RepeatableBridgedBool(const uint32_t index) const
{
  const std::vector<LN_BoolExpression> &bools = m_program.GetBoolExpressions();
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  const std::vector<LN_StringExpression> &strings = m_program.GetStringExpressions();
  if (index >= bools.size() || bools[index].kind != LN_BoolExpressionKind::CollisionDetected) {
    return false;
  }
  /* Colliding: ResolveCollisionResult's StateOnly result, kept per tick and filter (the physics'
   * contacts do not change while trees record: a bridged run starts with nothing pending), of the
   * owner or a named object, by constant filters (nothing that keeps state of its own). */
  const LN_BoolExpression &e = bools[index];
  const auto plain_value = [&](const uint32_t value) {
    return value == kNone ||
           (value < values.size() && (values[value].kind == LN_ValueExpressionKind::OwnerObject ||
                                      values[value].kind == LN_ValueExpressionKind::Constant));
  };
  const bool plain_text = e.input1 == kNone ||
                          (e.input1 < strings.size() &&
                           strings[e.input1].kind == LN_StringExpressionKind::Constant);
  return plain_value(e.input0) && plain_text && plain_value(e.input2);
}

ColumnCompiler::Slot ColumnCompiler::BridgeBool(const uint32_t index, const bool fallback)
{
  if (!BridgeAllowedHere("a bridged node", ReachOf('B', index), RepeatableBridgedBool(index))) {
    return {};
  }
  Node node;
  node.op = NodeOp::BKBool;
  node.property_ref = index;
  node.main_thread = ReachOf('B', index).main_thread;
  node.collision_reads = ReachOf('B', index).collisions;
  m_out.collision_reads = m_out.collision_reads || node.collision_reads;
  NoteMainThread(ReachOf('B', index));
  node.bool_value = fallback;
  node.compare = uint8_t(m_gate == kNone ? 0 : m_gate + 1);
  node.d = EvalMask();
  node.out = NewBool();
  m_out.nodes.push_back(node);
  Slot result;
  result.slot = node.out;
  result.reads_ops = true; /* the VM's code may read state: never kept for another read */
  return result;
}

ColumnCompiler::Slot ColumnCompiler::BridgeInt(const uint32_t index, const int32_t fallback)
{
  if (!BridgeAllowedHere("a bridged node", ReachOf('I', index))) {
    return {};
  }
  Node node;
  node.op = NodeOp::BKInt;
  node.property_ref = index;
  node.main_thread = ReachOf('I', index).main_thread;
  node.collision_reads = ReachOf('I', index).collisions;
  m_out.collision_reads = m_out.collision_reads || node.collision_reads;
  NoteMainThread(ReachOf('I', index));
  node.int_value = fallback;
  node.compare = uint8_t(m_gate == kNone ? 0 : m_gate + 1);
  node.d = EvalMask();
  node.out = NewInt();
  m_out.nodes.push_back(node);
  Slot result;
  result.slot = node.out;
  result.reads_ops = true;
  return result;
}

ColumnCompiler::Slot ColumnCompiler::BridgeFloat(const uint32_t index, const float fallback)
{
  if (!BridgeAllowedHere("a bridged node", ReachOf('F', index))) {
    return {};
  }
  Node node;
  node.op = NodeOp::BKFloat;
  node.property_ref = index;
  node.main_thread = ReachOf('F', index).main_thread;
  node.collision_reads = ReachOf('F', index).collisions;
  m_out.collision_reads = m_out.collision_reads || node.collision_reads;
  NoteMainThread(ReachOf('F', index));
  node.float_value = fallback;
  node.compare = uint8_t(m_gate == kNone ? 0 : m_gate + 1);
  node.d = EvalMask();
  node.out = NewFloat();
  m_out.nodes.push_back(node);
  Slot result;
  result.slot = node.out;
  result.reads_ops = true;
  return result;
}

ColumnCompiler::Vec ColumnCompiler::BridgeVector(const uint32_t index, const MT_Vector3 &fallback)
{
  if (!BridgeAllowedHere("a bridged node", ReachOf('V', index))) {
    return {};
  }
  Node node;
  node.op = NodeOp::BKVector;
  node.property_ref = index;
  node.main_thread = ReachOf('V', index).main_thread;
  node.collision_reads = ReachOf('V', index).collisions;
  m_out.collision_reads = m_out.collision_reads || node.collision_reads;
  NoteMainThread(ReachOf('V', index));
  node.position_b = VectorConst(fallback).slots;
  node.compare = uint8_t(m_gate == kNone ? 0 : m_gate + 1);
  node.d = EvalMask();
  node.out = NewFloat();
  node.out2 = NewFloat();
  node.out3 = NewFloat();
  m_out.nodes.push_back(node);
  Vec result;
  result.slots = {node.out, node.out2, node.out3};
  result.reads_ops = true;
  return result;
}

ColumnCompiler::Slot ColumnCompiler::BridgeRotation(const uint32_t index,
                                                    const LN_Rotation &fallback)
{
  if (!BridgeAllowedHere("a bridged node", ReachOf('R', index))) {
    return {};
  }
  Node node;
  node.op = NodeOp::BKRotation;
  node.property_ref = index;
  node.main_thread = ReachOf('R', index).main_thread;
  node.collision_reads = ReachOf('R', index).collisions;
  m_out.collision_reads = m_out.collision_reads || node.collision_reads;
  NoteMainThread(ReachOf('R', index));
  node.rotation = fallback;
  node.compare = uint8_t(m_gate == kNone ? 0 : m_gate + 1);
  node.d = EvalMask();
  node.out = m_out.rot_slots++;
  m_out.nodes.push_back(node);
  Slot result;
  result.slot = node.out;
  result.reads_ops = true;
  return result;
}

bool ColumnCompiler::BridgeColor(const uint32_t index, const MT_Vector4 &fallback, Slot r_rgba[4])
{
  if (!BridgeAllowedHere("a bridged node", ReachOf('C', index))) {
    return false;
  }
  Node node;
  node.op = NodeOp::BKColor;
  node.property_ref = index;
  node.main_thread = ReachOf('C', index).main_thread;
  node.collision_reads = ReachOf('C', index).collisions;
  m_out.collision_reads = m_out.collision_reads || node.collision_reads;
  NoteMainThread(ReachOf('C', index));
  for (int c = 0; c < 4; c++) {
    node.color[c] = fallback[c];
  }
  node.compare = uint8_t(m_gate == kNone ? 0 : m_gate + 1);
  node.d = EvalMask();
  node.out = NewFloat();
  node.out2 = NewFloat();
  node.out3 = NewFloat();
  node.out4 = NewFloat();
  m_out.nodes.push_back(node);
  const uint32_t outs[4] = {node.out, node.out2, node.out3, node.out4};
  for (int c = 0; c < 4; c++) {
    r_rgba[c].slot = outs[c];
    r_rgba[c].reads_ops = true;
    r_rgba[c].queries = 0;
  }
  return true;
}

void ColumnCompiler::MergeReach(QueryReach &into, const QueryReach &from)
{
  if (from.unknown && !into.unknown) {
    into.unknown = true;
    into.why = from.why;
  }
  into.finds_objects = into.finds_objects || from.finds_objects;
  if (from.main_thread && !into.main_thread) {
    into.main_thread = true;
    into.main_why = from.main_why;
  }
  into.collisions = into.collisions || from.collisions;
  for (const uint32_t query : from.queries) {
    if (std::find(into.queries.begin(), into.queries.end(), query) == into.queries.end()) {
      into.queries.push_back(query);
    }
  }
}

const ColumnCompiler::QueryReach &ColumnCompiler::ReachOf(const char family, const uint32_t index)
{
  static const QueryReach nothing;
  if (index == kNone) {
    return nothing;
  }
  const uint64_t key = (uint64_t(uint8_t(family)) << 32) | index;
  if (const auto found = m_query_reach.find(key); found != m_query_reach.end()) {
    return found->second;
  }
  /* Unknown while it is being walked: a cycle (none is known) reads as unknown. (References to
   * the map's entries stay valid while the walk adds others.) */
  QueryReach &reach = m_query_reach[key];
  reach.unknown = true;
  reach.why = "a cycle of inputs";
  QueryReach result;
  if (family == 'Q') {
    /* The query itself, then what it reads (its condition first, as ResolveQueryExpression). */
    result.queries.push_back(index);
  }
  /* The fields by the table's names: the recording rig put index k in the k-th field and names
   * the fields by those numbers (LN_ReadOrder_test's AddKind and FieldNameOf), so "input3" of a
   * Bool expression is its float_expr_index, of a Float or Value its bool_expr_index. */
  using Field = std::pair<const char *, uint32_t>;
  std::vector<Field> fields;
  const std::vector<uint32_t> *list = nullptr;
  LN_KindDomain domain = LN_KindDomain::Count;
  uint16_t kind = 0;
  uint32_t condition = kNone;
  switch (family) {
    case 'B': {
      const std::vector<LN_BoolExpression> &table = m_program.GetBoolExpressions();
      if (index < table.size()) {
        const LN_BoolExpression &e = table[index];
        domain = LN_KindDomain::Bool;
        kind = uint16_t(e.kind);
        fields = {{"input0", e.input0},
                  {"input1", e.input1},
                  {"input2", e.input2},
                  {"input3", e.float_expr_index},
                  {"input4", e.secondary_float_expr_index},
                  {"int_expr_index", e.int_expr_index}};
      }
      break;
    }
    case 'I': {
      const std::vector<LN_IntExpression> &table = m_program.GetIntExpressions();
      if (index < table.size()) {
        const LN_IntExpression &e = table[index];
        domain = LN_KindDomain::Int;
        kind = uint16_t(e.kind);
        fields = {{"input0", e.input0}, {"input1", e.input1}, {"input2", e.input2}};
      }
      break;
    }
    case 'F': {
      const std::vector<LN_FloatExpression> &table = m_program.GetFloatExpressions();
      if (index < table.size()) {
        const LN_FloatExpression &e = table[index];
        domain = LN_KindDomain::Float;
        kind = uint16_t(e.kind);
        fields = {{"input0", e.input0},
                  {"input1", e.input1},
                  {"input2", e.input2},
                  {"input3", e.bool_expr_index},
                  {"input4", e.string_expr_index}};
        list = &e.input_indices;
      }
      break;
    }
    case 'S': {
      const std::vector<LN_StringExpression> &table = m_program.GetStringExpressions();
      if (index < table.size()) {
        const LN_StringExpression &e = table[index];
        domain = LN_KindDomain::String;
        kind = uint16_t(e.kind);
        fields = {{"input0", e.input0},
                  {"input1", e.input1},
                  {"input2", e.input2},
                  {"input3", e.input3},
                  {"input4", e.input4},
                  {"int_expr_index", e.int_expr_index},
                  {"value_expr_index", e.value_expr_index}};
      }
      break;
    }
    case 'V':
    case 'R': {
      const std::vector<LN_VectorExpression> &table = m_program.GetVectorExpressions();
      if (index < table.size()) {
        const LN_VectorExpression &e = table[index];
        domain = LN_KindDomain::Vector;
        kind = uint16_t(e.kind);
        fields = {{"input0", e.input0},
                  {"input1", e.input1},
                  {"input2", e.input2},
                  {"input3", e.bool_expr_index},
                  {"input4", e.float_expr_index}};
      }
      break;
    }
    case 'C': {
      const std::vector<LN_ColorExpression> &table = m_program.GetColorExpressions();
      if (index < table.size()) {
        const LN_ColorExpression &e = table[index];
        domain = LN_KindDomain::Color;
        kind = uint16_t(e.kind);
        fields = {{"input0", e.input0},
                  {"input1", e.input1},
                  {"input2", e.input2},
                  {"input3", e.input3}};
      }
      break;
    }
    case 'A': {
      const std::vector<LN_ValueExpression> &table = m_program.GetValueExpressions();
      if (index < table.size()) {
        const LN_ValueExpression &e = table[index];
        domain = LN_KindDomain::Value;
        kind = uint16_t(e.kind);
        fields = {{"input0", e.input0},
                  {"input1", e.input1},
                  {"input2", e.input2},
                  {"input3", e.bool_expr_index},
                  {"input4", e.string_expr_index}};
        list = &e.input_indices;
      }
      break;
    }
    case 'Q': {
      const std::vector<LN_QueryExpression> &table = m_program.GetQueryExpressions();
      if (index < table.size()) {
        const LN_QueryExpression &e = table[index];
        domain = LN_KindDomain::Query;
        kind = uint16_t(e.kind);
        /* The rig kept these fields' own names (FieldNameOf's Query branch). */
        fields = {{"input0", e.input0},
                  {"input1", e.input1},
                  {"input2", e.input2},
                  {"input3", e.input3},
                  {"input4", e.input4},
                  {"input5", e.input5},
                  {"input6", e.input6},
                  {"input7", e.input7},
                  {"input8", e.input8},
                  {"input9", e.input9},
                  {"ignore_list_expr_index", e.ignore_list_expr_index},
                  {"bool_expr_index", e.bool_expr_index},
                  {"secondary_bool_expr_index", e.secondary_bool_expr_index},
                  {"tertiary_bool_expr_index", e.tertiary_bool_expr_index},
                  {"quaternary_bool_expr_index", e.quaternary_bool_expr_index},
                  {"quinary_bool_expr_index", e.quinary_bool_expr_index},
                  {"senary_bool_expr_index", e.senary_bool_expr_index},
                  {"int_expr_index", e.int_expr_index},
                  {"secondary_int_expr_index", e.secondary_int_expr_index},
                  {"tertiary_int_expr_index", e.tertiary_int_expr_index},
                  {"quaternary_int_expr_index", e.quaternary_int_expr_index},
                  {"float_expr_index", e.float_expr_index},
                  {"secondary_float_expr_index", e.secondary_float_expr_index},
                  {"tertiary_float_expr_index", e.tertiary_float_expr_index},
                  {"string_expr_index", e.string_expr_index}};
        list = &e.input_indices;
        /* Read before the kind (the rig left it out): a bool, walked after the fields. */
        condition = e.condition_bool_expr_index;
      }
      break;
    }
    default:
      break;
  }
  /* May this expression's own kind be worked out on a helper thread? */
  if (domain != LN_KindDomain::Count) {
    const char *refusal = LN_BatchExpressionRefusal(m_program, uint8_t(domain), index);
    if (refusal != nullptr && domain == LN_KindDomain::Query) {
      const LN_QueryExpressionKind query = LN_QueryExpressionKind(kind);
      /* Jolt's narrow phase under read locks, the filter reading objects (nothing writes the
       * world while trees record); hits register their objects under the manager's mutex. */
      if (query == LN_QueryExpressionKind::Raycast || query == LN_QueryExpressionKind::RaycastAll ||
          query == LN_QueryExpressionKind::ShapeCast ||
          query == LN_QueryExpressionKind::ShapeCastAll)
      {
        refusal = nullptr;
      }
    }
    if (refusal != nullptr && domain == LN_KindDomain::Value &&
        LN_ValueExpressionKind(kind) == LN_ValueExpressionKind::Constant)
    {
      /* A scene or datablock constant is given back as stored (ValueKind's Constant case
       * resolves only objects and collections). */
      const LN_ValueType type = m_program.GetValueExpressions()[index].value.type;
      if (type == LN_ValueType::SceneRef || type == LN_ValueType::DatablockRef) {
        refusal = nullptr;
      }
    }
    if (refusal != nullptr && domain == LN_KindDomain::Bool &&
        LN_BoolExpressionKind(kind) == LN_BoolExpressionKind::EventReceived)
    {
      /* The tick's event bus (written at the flush) and the dense ids: lookups only. */
      refusal = nullptr;
    }
    if (refusal != nullptr) {
      /* The collision reads (ResolveCollisionResult): the tree's own result cache and lookups in
       * the environment's contact cache, made when the step ended (CollectLogicObjectQueryOverlaps
       * writes it only for a sensor queried on demand: the run checks its lanes). */
      static const char *const collision_kinds[] = {"Bool::CollisionDetected",
                                                    "Bool::CollisionEnter",
                                                    "Bool::CollisionStay",
                                                    "Bool::CollisionExit",
                                                    "Int::CollisionContactCount",
                                                    "Vector::CollisionHitPoint",
                                                    "Vector::CollisionHitNormal",
                                                    "Value::CollisionHitObject",
                                                    "Value::CollisionHitObjects",
                                                    "Value::CollisionHitPoints",
                                                    "Value::CollisionHitNormals"};
      const char *name = LN_KindName(domain, kind);
      for (const char *collision : collision_kinds) {
        if (name != nullptr && std::strcmp(name, collision) == 0) {
          refusal = nullptr;
          result.collisions = true;
          break;
        }
      }
    }
    if (refusal != nullptr && domain != LN_KindDomain::Query) {
      /* A ray's or a shape cast's result fields (in.Query(input0).field): the query itself is
       * walked as an input and judged on its own. */
      const char *name = LN_KindName(domain, kind);
      const char *field = name != nullptr ? std::strstr(name, "::") : nullptr;
      if (field != nullptr &&
          (std::strncmp(field + 2, "PhysicsQuery", 12) == 0 || std::strcmp(field + 2, "QueryValid") == 0))
      {
        refusal = nullptr;
      }
    }
    if (refusal != nullptr && !result.main_thread) {
      const char *name = LN_KindName(domain, kind);
      result.main_thread = true;
      result.main_why = std::string(name != nullptr ? name : "a kind") + " (" + refusal + ")";
    }
  }
  result.unknown = true;
  result.why = "an expression the walk does not know";
  if (domain != LN_KindDomain::Count) {
    const std::unordered_map<std::string, KindInputs> &rows = KindInputTable();
    const char *name = LN_KindName(domain, kind);
    const auto row = name != nullptr ? rows.find(name) : rows.end();
    result.why = std::string(name != nullptr ? name : "a kind") +
                 (row == rows.end() ? " (not in the kind input table)" :
                                      " (reads through its state)");
    {
      const LN_KindInfo *info = LN_FindKind(domain, kind);
      const bool finder = name != nullptr && (std::strcmp(name, "Value::ObjectByName") == 0 ||
                                              std::strcmp(name, "Value::CollectionObjectNames") == 0);
      if (info == nullptr || (info->classes & LN_KIND_MAIN_THREAD) != 0 || finder) {
        result.finds_objects = true;
      }
    }
    if (row != rows.end() && !row->second.unknown) {
      const KindInputs &inputs = row->second;
      /* The families a field is read as (none: the row does not list it). */
      const auto families_of = [&](const std::string &field, std::string &r_families) {
        r_families.clear();
        for (const auto &[listed, listed_family] : inputs.fields) {
          if (listed == field) {
            r_families.push_back(listed_family);
          }
        }
      };
      result.unknown = false;
      result.why.clear();
      std::string families;
      for (const Field &field : fields) {
        if (field.second == kNone) {
          continue;
        }
        families_of(field.first, families);
        if (families.empty()) {
          /* A set field the kind was never seen to read. */
          result.unknown = true;
          result.why = std::string(name) + "'s " + field.first;
          break;
        }
        for (const char read : families) {
          if (read != '-') {
            MergeReach(result, ReachOf(read, field.second));
          }
        }
      }
      if (!result.unknown && list != nullptr) {
        /* A list's entries: each read as its own "input_indices[k]" families, else as the
         * families any listed entry was read as (lists are read alike). */
        std::string any;
        for (const auto &[listed, listed_family] : inputs.fields) {
          if (listed.rfind("input_indices[", 0) == 0 && any.find(listed_family) == std::string::npos)
          {
            any.push_back(listed_family);
          }
        }
        for (size_t k = 0; k < list->size() && !result.unknown; k++) {
          const uint32_t entry = (*list)[k];
          if (entry == kNone) {
            continue;
          }
          families_of("input_indices[" + std::to_string(k) + "]", families);
          if (families.empty()) {
            families = any;
          }
          if (families.empty()) {
            result.unknown = true;
            result.why = std::string(name) + "'s list";
            break;
          }
          for (const char read : families) {
            if (read != '-') {
              MergeReach(result, ReachOf(read, entry));
            }
          }
        }
      }
    }
  }
  MergeReach(result, ReachOf('B', condition));
  reach = std::move(result);
  return reach;
}

ColumnCompiler::QueryReach ColumnCompiler::OpReach(const LN_Instruction &instruction)
{
  QueryReach reach;
  const auto add = [&](const char family, const uint32_t index) {
    MergeReach(reach, ReachOf(family, index));
  };
  const LN_Instruction &i = instruction;
  for (const uint32_t index : {i.bool_guard_expr_index,
                               i.bool_expr_index,
                               i.secondary_bool_expr_index,
                               i.tertiary_bool_expr_index,
                               i.quaternary_bool_expr_index})
  {
    add('B', index);
  }
  for (const uint32_t index :
       {i.int_expr_index, i.secondary_int_expr_index, i.tertiary_int_expr_index})
  {
    add('I', index);
  }
  for (const uint32_t index : {i.float_expr_index,
                               i.secondary_float_expr_index,
                               i.tertiary_float_expr_index,
                               i.quaternary_float_expr_index})
  {
    add('F', index);
  }
  for (const uint32_t index :
       {i.string_expr_index, i.secondary_string_expr_index, i.tertiary_string_expr_index})
  {
    add('S', index);
  }
  /* A vector field may be read as a vector or as a rotation: the same table either way. */
  add('V', i.vector_expr_index);
  add('V', i.secondary_vector_expr_index);
  add('C', i.color_expr_index);
  for (const uint32_t index : {i.value_expr_index,
                               i.secondary_value_expr_index,
                               i.tertiary_value_expr_index,
                               i.quaternary_value_expr_index})
  {
    add('A', index);
  }
  const uint32_t payload = i.command_payload_index;
  if (payload == kNone) {
    return reach;
  }
  if (LN_OpcodeUsesVectorCommandPayload(i.opcode)) {
    const std::vector<LN_VectorCommandPayload> &vectors = m_program.GetVectorCommandPayloads();
    if (payload < vectors.size()) {
      const LN_VectorCommandPayload &p = vectors[payload];
      add('A', p.object_value_expr_index);
      add('V', p.vector_expr_index);
      add('V', p.secondary_vector_expr_index);
      add('B', p.bool_expr_index);
      return reach;
    }
  }
  else if (i.opcode == LN_OpCode::SetGameProperty) {
    const std::vector<LN_GamePropertyCommandPayload> &properties =
        m_program.GetGamePropertyCommandPayloads();
    if (payload < properties.size()) {
      const LN_GamePropertyCommandPayload &p = properties[payload];
      add('A', p.object_value_expr_index);
      add('A', p.value_expr_index);
      add('B', p.bool_expr_index);
      add('I', p.int_expr_index);
      add('F', p.float_expr_index);
      add('S', p.string_expr_index);
      add('V', p.vector_expr_index);
      add('C', p.color_expr_index);
      add('F', p.modify_min_expr_index);
      add('F', p.modify_max_expr_index);
      return reach;
    }
  }
  if (!reach.unknown) {
    reach.unknown = true;
    reach.why = std::string(LN_KindName(LN_KindDomain::Op, uint16_t(i.opcode))) +
                "'s command payload";
  }
  return reach;
}

std::string ColumnCompiler::ReachUnsafeHere(const QueryReach &reach)
{
  if (reach.unknown) {
    return "it may read a ray or a distance (" + reach.why + ")";
  }
  const std::vector<LN_QueryExpression> &queries = m_program.GetQueryExpressions();
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  /* The distances surely read before the owner moves that may run before this op (plan C2 fix
   * 1: a move on another route never does). */
  uint64_t protect = 0;
  OwnerMovedHere(protect);
  for (const uint32_t index : reach.queries) {
    if (index >= queries.size()) {
      return "it reads a query the program does not have";
    }
    const LN_QueryExpression &query = queries[index];
    if (query.kind == LN_QueryExpressionKind::VectorAverage) {
      continue; /* live positions: the owner's move is pending there in the VM too */
    }
    if (query.kind == LN_QueryExpressionKind::DistanceBetween) {
      const auto found = m_queries.find(index);
      if (found == m_queries.end()) {
        return "it reads a distance no native node read first";
      }
      const bool owner_end = found->second.a == EndKind::Owner ||
                             found->second.b == EndKind::Owner;
      if (owner_end && (protect & found->second.bit) == 0) {
        return "it reads a distance from the owner not surely read before the move";
      }
      continue;
    }
    /* A ray or a shape cast (the bridge takes no other query): cast again at every op that reads
     * it, after the caster's pending moves are applied. From a named object there are none (the
     * run starts only without them, and the lanes move only their owners). */
    const uint32_t caster = query.input0;
    const bool from_owner = caster == kNone ||
                            (caster < values.size() &&
                             values[caster].kind == LN_ValueExpressionKind::OwnerObject);
    if (from_owner) {
      return "it reads a ray or a shape cast from the owner";
    }
  }
  return {};
}

uint32_t ColumnCompiler::LoopDoneSlot(const uint32_t frame)
{
  const auto found = m_loop_done_slots.find(frame);
  if (found != m_loop_done_slots.end()) {
    return found->second;
  }
  Node node;
  node.op = NodeOp::SLoopDone;
  node.property_ref = frame;
  node.out = NewBool();
  m_out.nodes.push_back(node);
  m_loop_done_slots.emplace(frame, node.out);
  return node.out;
}

bool ColumnCompiler::CompileLoopBlock(const LN_ExecBlockProgram &ir,
                                      const uint32_t block_index,
                                      const std::vector<LN_InstructionHeader> &headers,
                                      const LN_LeanEvent &lean_event)
{
  const LN_ExecBlock &block = ir.blocks[block_index];
  const std::vector<LN_LoopFrame> &frames = m_program.GetLoopFrames();
  if (block.loop_frame_index >= frames.size()) {
    return Refuse("a Loop without its frame");
  }
  if (!m_bridge_ok) {
    return Refuse("a Loop (its program's queries keep the bridge off)");
  }
  /* What the VM's walk reads: the frame's inputs (BeginLoopFrame) and every op of the body. */
  const LN_LoopFrame &frame = frames[block.loop_frame_index];
  QueryReach reach;
  MergeReach(reach, ReachOf('B', frame.trigger_bool_expr_index));
  MergeReach(reach, ReachOf('I', frame.count_int_expr_index));
  MergeReach(reach, ReachOf('A', frame.list_value_expr_index));
  MergeReach(reach, ReachOf('I', frame.ticks_to_complete_int_expr_index));
  bool main = false;
  std::vector<uint32_t> tree_writes;
  for (uint32_t op = block.first_op; op < block.first_op + block.op_count; op++) {
    const LN_ExecOp &exec = ir.ops[op];
    if (exec.ui_event_state_index != kNone) {
      return Refuse("a UI event route");
    }
    if (exec.instruction_index >= headers.size()) {
      return Refuse("an op without its instruction");
    }
    const LN_Instruction &instruction = m_program.GetInstructionPayload(
        m_event, headers[exec.instruction_index]);
    const LN_OpCode opcode = exec.opcode;
    /* The body's ops record commands (never a move: a caught one could not be applied early) or
     * change the tree's own states. */
    const bool allowed = exec.kind == LN_ExecOpKind::BranchRoute ||
                         exec.kind == LN_ExecOpKind::Nop || opcode == LN_OpCode::BreakLoop ||
                         opcode == LN_OpCode::SetTreeProperty ||
                         (exec.kind == LN_ExecOpKind::GamePropertyCommand &&
                          opcode == LN_OpCode::SetGameProperty) ||
                         (BridgeableOp(exec.kind, opcode) && opcode != LN_OpCode::AddObject);
    if (!allowed) {
      return Refuse(std::string("a Loop body with ") +
                    LN_KindName(LN_KindDomain::Op, uint16_t(opcode)));
    }
    MergeReach(reach, OpReach(instruction));
    MergeReach(reach, ReachOf('B', exec.guard_bool_expr_index));
    if (const char *refusal = LN_BatchOpRefusal(m_program, instruction)) {
      main = true;
      NoteMainThread(std::string(LN_KindName(LN_KindDomain::Op, uint16_t(opcode))) + ": " +
                     refusal);
    }
    if (opcode == LN_OpCode::SetTreeProperty && instruction.property_ref_index != kNone) {
      tree_writes.push_back(instruction.property_ref_index);
    }
  }
  if (!BridgeAllowedHere("a Loop", reach)) {
    return false;
  }
  NoteMainThread(reach);
  /* A tree property the body writes is read live afterwards (the VM's code wrote it there). */
  for (const uint32_t ref : tree_writes) {
    const auto found = m_tree.find(ref);
    if (found != m_tree.end() && found->second.written) {
      return Refuse("a tree property a Loop body and a column node both write");
    }
    m_tree.erase(ref);
  }
  uint32_t mask = BoolConst(true).slot;
  if (m_gate != kNone) {
    auto gate = m_gate_slot.find(m_gate);
    if (gate == m_gate_slot.end()) {
      Node node;
      node.op = NodeOp::SGate;
      node.property_ref = m_gate;
      node.out = NewBool();
      m_out.nodes.push_back(node);
      gate = m_gate_slot.emplace(m_gate, node.out).first;
    }
    mask = gate->second;
  }
  Write write;
  write.kind = WriteKind::Captured;
  write.mask = mask;
  const uint32_t write_index = uint32_t(m_out.writes.size());
  m_out.writes.push_back(write);
  Node node;
  node.op = NodeOp::BLoop;
  node.a = mask;
  node.property_ref = block_index;
  node.compare = uint8_t(m_gate == kNone ? 0 : m_gate + 1);
  node.int_value = int32_t(write_index);
  node.main_thread = main || reach.main_thread;
  node.collision_reads = reach.collisions;
  m_out.collision_reads = m_out.collision_reads || reach.collisions;
  m_out.nodes.push_back(node);
  /* The body's ops: stamped by the VM's code in each iteration's own scope (a Ran of them read in
   * the pass's scope is false, as the VM's). */
  const uint32_t never = BoolConst(false).slot;
  for (uint32_t op = block.first_op; op < block.first_op + block.op_count; op++) {
    m_op_instruction.push_back(ir.ops[op].instruction_index);
    m_always_marks[op] = 0;
    m_op_gate.push_back(m_gate);
    m_masks.push_back(never);
    m_out.op_state.push_back(lean_event.ops[op].state_index);
    m_out.op_mask.push_back(never);
  }
  return true;
}

bool ColumnCompiler::AddObjectAllowedHere(const LN_Instruction &instruction)
{
  /* Add Object makes its copy while the tree records (ExecuteAddObjectFromRefImmediate), at the
   * reference's pose after applying the reference's pending moves, and keeps the copy in its
   * result tree property. The run makes the copies NPC by NPC in lane order, as the VM does, when:
   * it is the program's only Add Object (two would be made in another order), nothing bridged can
   * see a copy (CompileEnd: m_bridged_sees_objects), the reference is the owner before any move of
   * it (the VM would apply the move first) or a named object, the copy's template is a named
   * object no other named object of the program shares a name with (a name looked up after the
   * copy exists could find it), and no column read of its result property comes after it. */
  m_add_objects++;
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  const auto named = [&](const uint32_t value) {
    return value != kNone && value < values.size() &&
           values[value].kind == LN_ValueExpressionKind::Constant &&
           values[value].value.type == LN_ValueType::ObjectRef;
  };
  const uint32_t reference = instruction.value_expr_index;
  const bool owner_reference = reference == kNone ||
                               (reference < values.size() &&
                                values[reference].kind == LN_ValueExpressionKind::OwnerObject);
  /* (Its reference's pending moves are applied first: a move of the owner that may run before it
   * for one NPC would be. A move on another route of a Branch never does: plan C2 fix 1.) */
  if (owner_reference && OwnerMovedHere()) {
    return Refuse("Add Object at the owner after a move of it");
  }
  m_add_object_at_owner = m_add_object_at_owner || owner_reference;
  if (!owner_reference && !named(reference)) {
    return Refuse("Add Object at an object found at run time");
  }
  const uint32_t source = instruction.secondary_value_expr_index;
  if (!named(source)) {
    return Refuse("Add Object of an object found at run time");
  }
  const std::string &source_name = values[source].value.reference_name;
  for (uint32_t value = 0; value < values.size(); value++) {
    if (value != source && named(value) && values[value].value.reference_name == source_name) {
      return Refuse("Add Object of an object the program also names");
    }
  }
  const uint32_t result = instruction.property_ref_index;
  if (result != kNone) {
    const auto found = m_tree.find(result);
    if (found != m_tree.end() && found->second.written) {
      return Refuse("Add Object's result written by a column node too");
    }
    /* A later column read reads the property as Add Object left it (FTreeProp at its place). */
    m_tree.erase(result);
  }
  return true;
}

uint32_t ColumnCompiler::CompileBridgedOp(const LN_Instruction &instruction,
                                          const LN_LeanOp &lean_op,
                                          const uint32_t mask)
{
  if (!BridgeAllowedHere("a bridged op", OpReach(instruction))) {
    return kNone;
  }
  if (instruction.opcode == LN_OpCode::AddObject && !AddObjectAllowedHere(instruction)) {
    return kNone;
  }
  Write write;
  write.kind = WriteKind::Captured;
  write.mask = mask;
  const uint32_t write_index = uint32_t(m_out.writes.size());
  m_out.writes.push_back(write);
  Node node;
  node.op = NodeOp::BOp;
  node.a = mask;
  node.property_ref = m_op;
  {
    const QueryReach reach = OpReach(instruction);
    const char *refusal = LN_BatchOpRefusal(m_program, instruction);
    node.main_thread = refusal != nullptr || reach.main_thread;
    node.collision_reads = reach.collisions;
    m_out.collision_reads = m_out.collision_reads || reach.collisions;
    if (refusal != nullptr) {
      NoteMainThread(std::string(LN_KindName(LN_KindDomain::Op, uint16_t(instruction.opcode))) +
                     ": " + refusal);
    }
    else {
      NoteMainThread(reach);
    }
  }
  node.compare = uint8_t(m_gate == kNone ? 0 : m_gate + 1);
  node.int_value = int32_t(write_index);
  m_out.nodes.push_back(node);
  if (lean_op.state_index == kNone) {
    return mask;
  }
  /* The handler stamps the op itself, when it ran (some return before). */
  Node ran;
  ran.op = NodeOp::SRan;
  ran.property_ref = lean_op.state_index;
  ran.out = NewBool();
  m_out.nodes.push_back(ran);
  return ran.out;
}

ColumnCompiler::Slot ColumnCompiler::TimeReadSlot(const int32_t state,
                                                  const TimeRead which,
                                                  const bool fallback)
{
  if (state < 0) {
    return BoolConst(fallback);
  }
  if (uint32_t(state) >= m_program.GetTimeFlowStateCount()) {
    Refuse("a time flow read without its state");
    return {};
  }
  Node node;
  node.op = NodeOp::STimeRead;
  node.compare = uint8_t(which);
  node.property_ref = uint32_t(state);
  node.bool_value = fallback;
  node.out = NewBool();
  m_out.nodes.push_back(node);
  Slot result;
  result.slot = node.out;
  result.reads_ops = true; /* the state as the pass has it here */
  return result;
}

bool ColumnCompiler::CompileTimeOp(const LN_Instruction &instruction,
                                   const LN_InstructionHeader &header,
                                   const uint32_t mask,
                                   const std::function<void(uint64_t)> &read)
{
  /* OpTimeFlowControlInstruction: the inputs in the order and on the paths the VM reads them
   * (LN_RuntimeTree::TimeFlow*). */
  if (instruction.int_value < 0) {
    return true; /* no state: the op only marks itself */
  }
  const uint32_t state = uint32_t(instruction.int_value);
  if (state >= m_program.GetTimeFlowStateCount()) {
    return Refuse("a time flow op without its state");
  }
  Node node;
  node.op = NodeOp::STimeOp;
  node.a = mask;
  node.property_ref = state;
  node.int_value = m_gate == kNone ? -1 : int32_t(m_gate);
  node.bool_value = (header.flags & LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE) != 0;
  switch (instruction.opcode) {
    case LN_OpCode::ArmNextFrame:
      node.compare = uint8_t(TimeOp::NextFrame);
      break;
    case LN_OpCode::ArmTimer: {
      node.compare = uint8_t(TimeOp::Timer);
      const Slot duration = Float(instruction.float_expr_index, 0.0f);
      const Slot ignore = Bool(instruction.secondary_bool_expr_index, false);
      read(duration.queries | ignore.queries);
      node.b = duration.slot;
      node.c = ignore.slot;
      break;
    }
    case LN_OpCode::ArmDelay: {
      node.compare = uint8_t(TimeOp::Delay);
      const Slot ignore = Bool(instruction.secondary_bool_expr_index, false);
      const Slot duration = Float(instruction.float_expr_index, 0.0f);
      read(duration.queries | ignore.queries);
      node.b = duration.slot;
      node.c = ignore.slot;
      break;
    }
    case LN_OpCode::UpdatePulsify: {
      node.compare = uint8_t(TimeOp::Pulsify);
      const Slot interval = Float(instruction.float_expr_index, 0.5f);
      const Slot ignore = Bool(instruction.secondary_bool_expr_index, false);
      read(interval.queries | ignore.queries);
      node.b = interval.slot;
      node.c = ignore.slot;
      break;
    }
    case LN_OpCode::UpdateBarrier: {
      node.compare = uint8_t(TimeOp::Barrier);
      const Slot condition = instruction.bool_expr_index == kNone ?
                                 BoolConst(true) :
                                 Bool(instruction.bool_expr_index, false);
      const Slot ignore = Bool(instruction.secondary_bool_expr_index, false);
      read(condition.queries | ignore.queries);
      Slot duration;
      {
        /* Read only while the condition holds. */
        EvalScope scope(*this, condition.slot, false);
        duration = Float(instruction.float_expr_index, 0.0f);
      }
      node.b = duration.slot;
      node.c = ignore.slot;
      node.d = condition.slot;
      break;
    }
    case LN_OpCode::TryCooldown: {
      node.compare = uint8_t(TimeOp::Cooldown);
      /* Its inputs are read only when the cooldown is not running. */
      const Slot ready = TimeReadSlot(int32_t(state), TimeRead::CooldownReady, false);
      if (!m_refusal.empty()) {
        return false;
      }
      Slot duration, ignore;
      {
        EvalScope scope(*this, ready.slot, false);
        duration = Float(instruction.float_expr_index, 0.0f);
        ignore = Bool(instruction.secondary_bool_expr_index, false);
      }
      node.b = duration.slot;
      node.c = ignore.slot;
      break;
    }
    case LN_OpCode::ResetCooldown:
      node.compare = uint8_t(TimeOp::ResetCooldown);
      break;
    default:
      return Refuse("a time flow op of another kind");
  }
  if (!m_refusal.empty()) {
    return false;
  }
  m_out.nodes.push_back(node);
  return true;
}

bool ColumnCompiler::CompileTreeWrite(const LN_Instruction &instruction,
                                      const uint32_t mask,
                                      const std::function<void(uint64_t)> &read)
{
  const std::vector<LN_TreePropertyRef> &refs = m_program.GetTreePropertyRefs();
  if (instruction.property_ref_index >= refs.size()) {
    return Refuse("a tree property write without its ref");
  }
  LN_ColumnProgram::TreeWrite write;
  write.mask = mask;
  Slot value;
  Vec vector;
  /* OpDirectTreePropertyCommand: the value expression's value, the ref's default without one. */
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  if (instruction.value_expr_index == kNone) {
    write.constant = true;
    write.value = refs[instruction.property_ref_index].default_value;
  }
  else if (instruction.value_expr_index >= values.size()) {
    return Refuse("a tree property write without its value");
  }
  else {
    const LN_ValueExpression &source = values[instruction.value_expr_index];
    switch (source.kind) {
      case LN_ValueExpressionKind::FromFloat:
        write.type = LN_ValueType::Float;
        value = Float(source.input0, 0.0f);
        break;
      case LN_ValueExpressionKind::FromInt:
        write.type = LN_ValueType::Int;
        value = Int(source.input0, 0);
        break;
      case LN_ValueExpressionKind::FromBool:
        write.type = LN_ValueType::Bool;
        value = Bool(source.input0, false);
        break;
      case LN_ValueExpressionKind::FromVector:
        /* Plan C1-11: ValueKind's FromVector (a fresh Vector value). */
        write.type = LN_ValueType::Vector;
        vector = Vector(source.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
        break;
      case LN_ValueExpressionKind::Constant:
        /* ValueKind's Constant: the value as it is, but for an object (found per tree: those
         * go through the bridge, TreeWriteIsNative). */
        if (source.value.type == LN_ValueType::ObjectRef) {
          return Refuse("a tree property write of an object");
        }
        write.constant = true;
        write.value = source.value;
        break;
      default:
        return Refuse("a tree property write of another value");
    }
  }
  if (!m_refusal.empty()) {
    return false;
  }
  read(value.queries | vector.queries);
  const uint32_t ref = instruction.property_ref_index;
  TreeRef *tree = Tree(ref);
  if (tree == nullptr) {
    return false;
  }
  /* The written value's fields: a fresh value has the others at their defaults. */
  const LN_Value fresh;
  uint32_t f, i, b;
  if (write.constant) {
    f = FloatConst(write.value.float_value).slot;
    i = IntConst(write.value.int_value).slot;
    b = BoolConst(write.value.bool_value).slot;
  }
  else {
    f = write.type == LN_ValueType::Float ? value.slot : FloatConst(fresh.float_value).slot;
    i = write.type == LN_ValueType::Int ? value.slot : IntConst(fresh.int_value).slot;
    b = write.type == LN_ValueType::Bool ? value.slot : BoolConst(fresh.bool_value).slot;
  }
  const uint32_t write_index = uint32_t(m_out.tree_write_ops.size());
  tree->f = SelectSlot(NodeOp::FSelect, mask, f, tree->f);
  tree->i = SelectSlot(NodeOp::ISelect, mask, i, tree->i);
  tree->b = SelectSlot(NodeOp::BSelect, mask, b, tree->b);
  tree->last_write = SelectSlot(NodeOp::ISelect, mask, IntConst(int32_t(write_index)).slot, tree->last_write);
  /* Set Tree Property sets at once (OpDirectTreePropertyCommand): later reads of the VM's own
   * code (bridged reads, the next pass) see it. */
  Node now;
  now.op = NodeOp::STreeWrite;
  now.a = mask;
  now.property_ref = ref;
  now.int_value = int32_t(write_index);
  now.b = f;
  now.c = i;
  now.d = b;
  if (write.type == LN_ValueType::Vector && !write.constant) {
    now.position_a = vector.slots;
  }
  m_out.nodes.push_back(now);
  tree->written = true;
  m_out.tree_write_ops.push_back(std::move(write));
  return true;
}

bool ColumnCompiler::TreeWriteIsNative(const LN_Instruction &instruction) const
{
  /* CompileTreeWrite's own cases (any other value: the bridge, then the fields read again). */
  const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
  if (instruction.value_expr_index == kNone) {
    return true;
  }
  if (instruction.value_expr_index >= values.size()) {
    return true; /* (refused there) */
  }
  const LN_ValueExpression &source = values[instruction.value_expr_index];
  switch (source.kind) {
    case LN_ValueExpressionKind::FromFloat:
    case LN_ValueExpressionKind::FromInt:
    case LN_ValueExpressionKind::FromBool:
    case LN_ValueExpressionKind::FromVector:
      return true;
    case LN_ValueExpressionKind::Constant:
      return source.value.type != LN_ValueType::ObjectRef;
    default:
      return false;
  }
}

void ColumnCompiler::ReloadTree(const uint32_t ref)
{
  /* After a bridged Set Tree Property: the fields as the VM's handler left the value. */
  const auto found = m_tree.find(ref);
  if (found == m_tree.end()) {
    return; /* its first read loads them from the value as it is then */
  }
  TreeRef &tree = found->second;
  Node node;
  node.property_ref = ref;
  node.op = NodeOp::FTreeProp;
  node.out = tree.f = NewFloat();
  m_out.nodes.push_back(node);
  node.op = NodeOp::ITreeProp;
  node.out = tree.i = NewInt();
  m_out.nodes.push_back(node);
  node.op = NodeOp::BTreeProp;
  node.out = tree.b = NewBool();
  m_out.nodes.push_back(node);
  /* A generic read takes the stored value (VTreeConvert's "no write of the pass"). */
  tree.last_write = IntConst(-1).slot;
  tree.written = true;
}

bool ColumnCompiler::Compile(std::string &r_refusal)
{
  const LN_LeanEvent &lean_event = m_lean.events[size_t(m_event)];
  const LN_ExecBlockProgram &ir = m_program.GetExecBlockProgram(m_event);
  const std::vector<LN_InstructionHeader> &headers = m_program.GetInstructionHeaders(m_event);
  const auto refuse = [&](const std::string &why) {
    r_refusal = why;
    return false;
  };
  if (!ir.valid || ir.ops.empty()) {
    return refuse("the event has no ops");
  }
  if (!ir.ui_event_states.empty()) {
    return refuse("a UI event route");
  }
  if (lean_event.ops.size() != ir.ops.size()) {
    return refuse("no lean ops");
  }
  /* The ops run in their order: blocks one after the other, no Loop; a block behind an Every N
   * Steps gate runs only on the gate's steps (RunLeanRoute). */
  uint32_t next_op = 0;
  std::vector<uint32_t> op_gate(ir.ops.size(), kNone);
  const size_t gate_count = m_program.GetStepGates().size();
  m_loop_block_at.assign(ir.ops.size(), kNone);
  m_completed_frame.assign(ir.ops.size(), kNone);
  for (uint32_t block_index = 0; block_index < ir.blocks.size(); block_index++) {
    const LN_ExecBlock &block = ir.blocks[block_index];
    if (block.loop_frame_index != kNone || block.completed_loop_frame_index != kNone) {
      /* Plan C1-9: a Loop's body runs through the VM's own block walk NPC by NPC (the bridge's
       * BLoop), its Completed route in columns behind the loop's completion. */
      const char *bridge = std::getenv("UPBGE_LN_COLUMN_BRIDGE");
      const bool bridge_off = bridge != nullptr &&
                              (std::strcmp(bridge, "0") == 0 || std::strcmp(bridge, "off") == 0);
      if (bridge_off) {
        return refuse("a Loop");
      }
      if (block.loop_frame_index != kNone && block.completed_loop_frame_index != kNone) {
        return refuse("a Loop body that is another Loop's Completed route");
      }
      if (block.loop_frame_index != kNone) {
        if (block.op_count == 0 || block.first_op >= ir.ops.size()) {
          return refuse("a Loop with an empty body");
        }
        m_loop_block_at[block.first_op] = block_index;
      }
      else {
        for (uint32_t op = block.first_op; op < block.first_op + block.op_count && op < ir.ops.size();
             op++)
        {
          m_completed_frame[op] = block.completed_loop_frame_index;
        }
      }
    }
    if (block.first_op != next_op || block.first_op + block.op_count > ir.ops.size()) {
      return refuse("blocks out of op order");
    }
    if (block.step_gate_index != kNone) {
      if (block.step_gate_index >= gate_count) {
        return refuse("a block behind a gate the program does not have");
      }
      if (block.step_gate_index >= 64) {
        return refuse("more than 64 step gates");
      }
    }
    for (uint32_t op = block.first_op; op < block.first_op + block.op_count; op++) {
      op_gate[op] = block.step_gate_index;
    }
    next_op += block.op_count;
  }
  if (next_op != ir.ops.size()) {
    return refuse("blocks out of op order");
  }
  /* The bridge (plan C1-8): the program's queries must be ones whose reads the run keeps exact
   * (distances between fixed ends, rays and shape casts from the own object or a named one: their
   * reads apply only those objects' pending moves, and the run starts only when they have none). */
  {
    const char *bridge = std::getenv("UPBGE_LN_COLUMN_BRIDGE");
    m_bridge_ok = !(bridge != nullptr && (std::strcmp(bridge, "0") == 0 ||
                                          std::strcmp(bridge, "off") == 0));
    const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
    const auto owner_or_named = [&](const uint32_t value) {
      return value == kNone ||
             (value < values.size() &&
              (values[value].kind == LN_ValueExpressionKind::OwnerObject ||
               (values[value].kind == LN_ValueExpressionKind::Constant &&
                values[value].value.type == LN_ValueType::ObjectRef)));
    };
    /* Plan C2 P5: the queries this event's ops reach (another event's On UI Event query refused
     * the step event's bridge of a whole tree before); all of them when a walk is incomplete. */
    const std::vector<LN_QueryExpression> &all_queries = m_program.GetQueryExpressions();
    std::vector<uint8_t> reached(all_queries.size(), 0);
    bool reach_complete = true;
    for (const LN_InstructionHeader &header : m_program.GetInstructionHeaders(m_event)) {
      const QueryReach reach = OpReach(m_program.GetInstructionPayload(m_event, header));
      if (reach.unknown) {
        reach_complete = false;
        break;
      }
      for (const uint32_t q : reach.queries) {
        if (q < reached.size()) {
          reached[q] = 1;
        }
      }
    }
    for (uint32_t q = 0; q < all_queries.size(); q++) {
      if (reach_complete && reached[q] == 0) {
        continue;
      }
      const LN_QueryExpression &query = all_queries[q];
      switch (query.kind) {
        case LN_QueryExpressionKind::DistanceBetween:
          m_reads_owner_queries = true;
          if (query.condition_bool_expr_index != kNone ||
              !LN_DistanceQueryEndsAreFixed(m_program, query))
          {
            m_bridge_ok = false;
          }
          break;
        case LN_QueryExpressionKind::Raycast:
        case LN_QueryExpressionKind::RaycastAll:
        case LN_QueryExpressionKind::ShapeCast:
        case LN_QueryExpressionKind::ShapeCastAll:
          m_reads_owner_queries = true;
          if (!owner_or_named(query.input0)) {
            m_bridge_ok = false;
          }
          break;
        case LN_QueryExpressionKind::VectorAverage:
          /* Get Average Center Position (plan C1-9 E3): the live positions of its vectors,
           * objects or collection, no pending move applied; the same at every op of a run (no
           * lane applies another's moves while the run records, in the VM either). */
          break;
        default:
          m_bridge_ok = false;
          break;
      }
    }
    if (m_bridge_ok) {
      /* The objects the program names: a run starts only when none has a pending move. */
      for (uint32_t value = 0; value < values.size(); value++) {
        if (values[value].kind == LN_ValueExpressionKind::Constant &&
            values[value].value.type == LN_ValueType::ObjectRef)
        {
          m_out.constant_objects.push_back(value);
        }
      }
      m_out.on_main_thread = !LN_AnalyzeTreeBatchSafety(m_program).eligible;
    }
  }
  const std::vector<LN_GamePropertyRef> &refs = m_program.GetGamePropertyRefs();
  m_out.property_names.reserve(refs.size());
  for (const LN_GamePropertyRef &ref : refs) {
    m_out.property_names.push_back(ref.name);
  }
  const std::vector<LN_BoolExpression> &bools = m_program.GetBoolExpressions();
  const std::vector<LN_GamePropertyCommandPayload> &property_payloads =
      m_program.GetGamePropertyCommandPayloads();
  const std::vector<LN_VectorCommandPayload> &vector_payloads =
      m_program.GetVectorCommandPayloads();
  using CT = LN_CommandBuffer::CommandType;
  const auto last_write_wins = [](const CT type) {
    return LN_CommandBuffer::GetCoalescingPolicy(type) ==
           LN_CommandBuffer::CoalescingPolicy::LastWriteWinsWithinBarrierSegment;
  };
  m_out.route_ops = uint32_t(ir.ops.size());
  m_always_marks.assign(ir.ops.size(), 0);
  for (m_op = 0; m_op < ir.ops.size(); m_op++) {
    const LN_ExecOp &exec = ir.ops[m_op];
    const LN_LeanOp &lean_op = lean_event.ops[m_op];
    if (lean_op.guard_is_delay_done && !m_program.GetTreePropertyRefs().empty()) {
      /* Its route reads the tree properties as they were when the Delay was armed. */
      Refuse("a Delay's Done in a tree with tree properties");
      break;
    }
    if (exec.ui_event_state_index != kNone) {
      Refuse("a UI event route");
      break;
    }
    if (exec.instruction_index >= headers.size()) {
      Refuse("an op without its instruction");
      break;
    }
    if (m_loop_block_at[m_op] != kNone) {
      /* A Loop body: its whole block in one bridged node; on after its last op. */
      m_gate = op_gate[m_op];
      m_here = kNone;
      const uint32_t block_index = m_loop_block_at[m_op];
      if (!CompileLoopBlock(ir, block_index, headers, lean_event)) {
        break;
      }
      m_op = ir.blocks[block_index].first_op + ir.blocks[block_index].op_count - 1;
      continue;
    }
    m_op_instruction.push_back(exec.instruction_index);
    m_gate = op_gate[m_op];
    const LN_Instruction &instruction = m_program.GetInstructionPayload(
        m_event, headers[exec.instruction_index]);
    Slot guard;
    bool unconditional = false;
    m_eval.clear();
    /* A Loop's Completed route runs only where the loop completed in this pass. */
    const uint32_t completed = m_completed_frame[m_op];
    uint32_t done_slot = kNone;
    if (completed != kNone) {
      done_slot = LoopDoneSlot(completed);
      m_eval.push_back({done_slot, false});
    }
    if (m_gate != kNone) {
      /* A guard behind a gate is evaluated only where the gate lets the route run. */
      auto gate = m_gate_slot.find(m_gate);
      if (gate == m_gate_slot.end()) {
        Node node;
        node.op = NodeOp::SGate;
        node.property_ref = m_gate;
        node.out = NewBool();
        m_out.nodes.push_back(node);
        gate = m_gate_slot.emplace(m_gate, node.out).first;
      }
      m_eval.push_back({gate->second, false});
    }
    if (exec.guard_bool_expr_index == kNone) {
      guard = BoolConst(true);
      unconditional = true;
    }
    else if (exec.guard_bool_expr_index < bools.size() &&
             bools[exec.guard_bool_expr_index].kind == LN_BoolExpressionKind::InstructionExecuted)
    {
      /* Ran: the stamp of an op of this pass (none yet: it never stamped this pass). */
      guard.slot = RanSlot(bools[exec.guard_bool_expr_index].input0, unconditional);
    }
    else {
      guard = Bool(exec.guard_bool_expr_index, false);
      /* A guard is read whenever the pass reaches the op (behind a gate: whenever its route runs). */
      if (completed == kNone) {
        (m_gate == kNone ? m_always_read : m_gate_reads[m_gate]) |= guard.queries;
      }
    }
    if (completed != kNone) {
      unconditional = false;
    }
    if (!m_refusal.empty()) {
      break;
    }
    if (m_gate != kNone) {
      /* Off its gate's steps the route does nothing: every op of it is masked by the gate. */
      auto gate = m_gate_slot.find(m_gate);
      if (gate == m_gate_slot.end()) {
        Node node;
        node.op = NodeOp::SGate;
        node.property_ref = m_gate;
        node.out = NewBool();
        m_out.nodes.push_back(node);
        gate = m_gate_slot.emplace(m_gate, node.out).first;
      }
      guard.slot = AndSlot(gate->second, guard.slot);
    }
    if (done_slot != kNone) {
      guard.slot = AndSlot(done_slot, guard.slot);
    }
    m_here = guard.slot;
    /* The op's inputs are evaluated only where it runs. */
    m_eval.clear();
    m_eval.push_back({guard.slot, false});
    const auto read = [&](const uint64_t queries) {
      if (unconditional) {
        (m_gate == kNone ? m_always_read : m_gate_reads[m_gate]) |= queries;
      }
    };
    uint32_t mask = guard.slot;
    bool op_marks = unconditional;
    bool bridged_op = false;
    switch (LN_LeanNative(lean_op.native)) {
      case LN_LeanNative::Nop:
        break;
      case LN_LeanNative::BranchRoute: {
        if (exec.kind != LN_ExecOpKind::BranchRoute) {
          Refuse("a branch op of another kind");
          break;
        }
        const Slot condition = instruction.bool_expr_index == kNone ?
                                   BoolConst(false) :
                                   Bool(instruction.bool_expr_index, false);
        if (!m_refusal.empty()) {
          break;
        }
        read(condition.queries);
        uint32_t matches = condition.slot;
        if (!instruction.bool_value) {
          Node not_node;
          not_node.op = NodeOp::BNot;
          not_node.a = condition.slot;
          not_node.out = NewBool();
          m_out.nodes.push_back(not_node);
          matches = not_node.out;
        }
        mask = AndSlot(guard.slot, matches);
        /* A branch on a constant that matches (a node's Done) marks whenever it is reached. */
        op_marks = unconditional &&
                   ((condition.slot == m_true && instruction.bool_value) ||
                    (condition.slot == m_false && !instruction.bool_value));
        break;
      }
      case LN_LeanNative::SetNumber: {
        if (instruction.command_payload_index >= property_payloads.size()) {
          Refuse("a property write without its payload");
          break;
        }
        const LN_GamePropertyCommandPayload &payload =
            property_payloads[instruction.command_payload_index];
        if (payload.property_ref_index >= refs.size()) {
          Refuse("a property write without its ref");
          break;
        }
        /* The object it writes (plan C1-10): the owner, a named object or one each NPC finds
         * (LeanSetNumberBody resolves it after reading the value: nothing it reads changes). */
        uint32_t runtime_target = kNone, target_column = kNone;
        if (!CommandTarget(payload.object_value_expr_index, runtime_target, target_column)) {
          Refuse("a property write to an object that is neither the owner, named nor found while "
                 "playing");
          break;
        }
        m_native_write_instructions.insert(exec.instruction_index);
        Write write;
        write.kind = WriteKind::Number;
        write.runtime_target = runtime_target;
        write.target_column = target_column;
        write.number_type = payload.value_type;
        write.property_ref = payload.property_ref_index;
        write.last_write_wins = last_write_wins(CT::SetGameProperty);
        Slot value;
        switch (payload.value_type) {
          case LN_ValueType::Bool:
            value = Bool(payload.bool_expr_index, false);
            write.out_bool = m_out.out_bool_columns++;
            break;
          case LN_ValueType::Int:
            value = Int(payload.int_expr_index, 0);
            write.out_int = m_out.out_int_columns++;
            break;
          case LN_ValueType::Float:
            value = Float(payload.float_expr_index, 0.0f);
            write.out_float = m_out.out_float_columns++;
            break;
          default:
            Refuse("a property write of another type");
            break;
        }
        if (!m_refusal.empty()) {
          break;
        }
        read(value.queries);
        write.value = value.slot;
        write.mask = guard.slot;
        m_out.writes.push_back(write);
        break;
      }
      case LN_LeanNative::ModifyProperty: {
        if (instruction.command_payload_index >= property_payloads.size()) {
          Refuse("a property change without its payload");
          break;
        }
        const LN_GamePropertyCommandPayload &payload =
            property_payloads[instruction.command_payload_index];
        if (payload.property_ref_index >= refs.size()) {
          Refuse("a property change without its ref");
          break;
        }
        /* The object it changes (plan C1-10), as a property write's. */
        uint32_t runtime_target = kNone, target_column = kNone;
        if (!CommandTarget(payload.object_value_expr_index, runtime_target, target_column)) {
          Refuse("a property change of an object that is neither the owner, named nor found "
                 "while playing");
          break;
        }
        m_native_write_instructions.insert(exec.instruction_index);
        Write write;
        write.kind = WriteKind::Modify;
        write.runtime_target = runtime_target;
        write.target_column = target_column;
        write.property_ref = payload.property_ref_index;
        write.modify_op = payload.modify_op;
        write.last_write_wins = last_write_wins(CT::ModifyGameProperty);
        const Slot operand = Float(payload.float_expr_index, 0.0f);
        read(operand.queries);
        write.operand = operand.slot;
        write.clamped = payload.modify_min_expr_index != kNone &&
                        payload.modify_max_expr_index != kNone;
        write.out_float = m_out.out_float_columns;
        m_out.out_float_columns += write.clamped ? 3 : 1;
        if (write.clamped) {
          const Slot low = Float(payload.modify_min_expr_index, 0.0f);
          const Slot high = Float(payload.modify_max_expr_index, 0.0f);
          read(low.queries | high.queries);
          write.min = low.slot;
          write.max = high.slot;
        }
        write.mask = guard.slot;
        m_out.writes.push_back(write);
        break;
      }
      case LN_LeanNative::VectorCommand: {
        if (instruction.command_payload_index >= vector_payloads.size()) {
          Refuse("a vector command without its payload");
          break;
        }
        const LN_VectorCommandPayload &payload = vector_payloads[instruction.command_payload_index];
        /* The object it moves: the owner, or (plan C1-9 E2) one each NPC finds while playing (its
         * turret, its weapon: a child, an Object property...), resolved when the run starts. */
        uint32_t runtime_target = kNone;
        if (payload.object_value_expr_index != kNone) {
          const std::vector<LN_ValueExpression> &target_values = m_program.GetValueExpressions();
          const bool owner_value = payload.object_value_expr_index < target_values.size() &&
                                   target_values[payload.object_value_expr_index].kind ==
                                       LN_ValueExpressionKind::OwnerObject;
          if (!owner_value && !RuntimeObject(payload.object_value_expr_index, runtime_target)) {
            Refuse("a move of an object that is neither the owner nor found while playing");
            break;
          }
        }
        m_native_write_instructions.insert(exec.instruction_index);
        Write write;
        write.kind = WriteKind::Transform;
        write.runtime_target = runtime_target;
        const LN_VectorOperationChannel channel = LN_VectorOperationChannel(
            payload.operation_channel);
        bool reads_local = false;
        bool orientation = false;
        switch (instruction.opcode) {
          case LN_OpCode::ApplyTransformVector:
            if (channel == LN_VectorOperationChannel::Movement) {
              write.command = CT::ApplyMovement;
            }
            else if (channel == LN_VectorOperationChannel::Rotation) {
              write.command = CT::ApplyRotation;
              orientation = true;
            }
            else {
              Refuse("Apply Transform Vector of another channel");
            }
            write.is_move = true;
            reads_local = true;
            if (runtime_target == kNone) {
              m_out.relative_moves = true;
            }
            else {
              m_relative_runtime.insert(runtime_target);
            }
            break;
          case LN_OpCode::SetWorldPosition:
            write.command = CT::SetWorldPosition;
            write.is_move = true;
            break;
          case LN_OpCode::SetLocalPosition:
            write.command = CT::SetLocalPosition;
            write.is_move = true;
            break;
          case LN_OpCode::SetWorldScale:
            write.command = CT::SetWorldScale;
            write.is_move = true;
            break;
          case LN_OpCode::SetLocalScale:
            write.command = CT::SetLocalScale;
            write.is_move = true;
            break;
          case LN_OpCode::SetWorldOrientation:
            write.command = CT::SetWorldOrientation;
            write.is_move = true;
            orientation = true;
            break;
          case LN_OpCode::SetLocalOrientation:
            write.command = CT::SetLocalOrientation;
            write.is_move = true;
            orientation = true;
            break;
          case LN_OpCode::SetTransformVector:
            if (channel == LN_VectorOperationChannel::Position) {
              write.command = CT::SetWorldPosition;
              write.local_command = CT::SetLocalPosition;
            }
            else if (channel == LN_VectorOperationChannel::Scale) {
              write.command = CT::SetWorldScale;
              write.local_command = CT::SetLocalScale;
            }
            else if (channel == LN_VectorOperationChannel::Orientation) {
              write.command = CT::SetWorldOrientation;
              write.local_command = CT::SetLocalOrientation;
              orientation = true;
            }
            else {
              Refuse("Set Transform Vector of another channel");
            }
            write.type_from_local = true;
            write.is_move = true;
            reads_local = true;
            break;
          case LN_OpCode::SetLinearVelocity:
            write.command = CT::SetLinearVelocity;
            break;
          case LN_OpCode::SetLocalLinearVelocity:
            write.command = CT::SetLocalLinearVelocity;
            break;
          case LN_OpCode::SetAngularVelocity:
            write.command = CT::SetAngularVelocity;
            break;
          case LN_OpCode::SetLocalAngularVelocity:
            write.command = CT::SetLocalAngularVelocity;
            break;
          case LN_OpCode::SetVelocityVector:
            if (channel == LN_VectorOperationChannel::LinearVelocity) {
              write.command = CT::SetLinearVelocity;
              write.local_command = CT::SetLocalLinearVelocity;
            }
            else if (channel == LN_VectorOperationChannel::AngularVelocity) {
              write.command = CT::SetAngularVelocity;
              write.local_command = CT::SetLocalAngularVelocity;
            }
            else {
              Refuse("Set Velocity Vector of another channel");
            }
            write.type_from_local = true;
            reads_local = true;
            break;
          case LN_OpCode::ApplyPhysicsVector:
            /* The force's Local is its bool value (AppendApplyForce). */
            if (channel == LN_VectorOperationChannel::Force) {
              write.command = CT::ApplyForce;
            }
            else if (channel == LN_VectorOperationChannel::Torque) {
              write.command = CT::ApplyTorque;
            }
            else {
              Refuse("Apply Physics Vector of another channel");
            }
            reads_local = true;
            break;
          default:
            Refuse(std::string("vector opcode ") +
                   LN_KindName(LN_KindDomain::Op, uint16_t(instruction.opcode)));
            break;
        }
        if (!m_refusal.empty()) {
          break;
        }
        write.last_write_wins = last_write_wins(write.command);
        write.local_last_write_wins = write.type_from_local ? last_write_wins(write.local_command) :
                                                              write.last_write_wins;
        if (reads_local) {
          const LN_VectorOperationMode mode = LN_VectorOperationMode(payload.operation_mode);
          if (mode == LN_VectorOperationMode::LocalFromBool) {
            const Slot local = Bool(payload.bool_expr_index, false);
            read(local.queries);
            write.local = local.slot;
            write.out_bool = m_out.out_bool_columns++;
          }
          else {
            write.local_constant = mode == LN_VectorOperationMode::Local;
          }
        }
        if (orientation != (lean_op.constant_index != kNone)) {
          Refuse("an orientation command the lowering did not give its constant");
          break;
        }
        if (orientation) {
          /* LeanVectorBody: the rotation read with the lowered constant as its fallback; only a
           * constant one here. */
          if (lean_op.constant_index >= m_lean.rotation_constants.size()) {
            Refuse("a rotation without its constant");
            break;
          }
          write.is_rotation = true;
          const std::vector<LN_VectorExpression> &vectors = m_program.GetVectorExpressions();
          if (payload.vector_expr_index == kNone || payload.vector_expr_index >= vectors.size()) {
            write.rotation = m_lean.rotation_constants[lean_op.constant_index];
          }
          else if (vectors[payload.vector_expr_index].kind == LN_VectorExpressionKind::Constant) {
            const LN_VectorExpression &constant = vectors[payload.vector_expr_index];
            write.rotation = constant.output_type == LN_ValueType::Rotation ?
                                 constant.rotation_value :
                                 LN_Rotation::FromEulerXYZ(constant.vector_value);
          }
          else {
            /* LeanVectorBody: ef.Rotation with the lowered constant as its fallback. */
            const Slot rotation = Rotation(payload.vector_expr_index,
                                           m_lean.rotation_constants[lean_op.constant_index]);
            if (!m_refusal.empty()) {
              break;
            }
            read(rotation.queries);
            write.rot = rotation.slot;
            write.out_rot = m_out.out_rot_columns++;
          }
        }
        else {
          const Vec value = Vector(payload.vector_expr_index, payload.vector_value);
          if (!m_refusal.empty()) {
            break;
          }
          read(value.queries);
          write.vector = value.slots;
          write.out_float = m_out.out_float_columns;
          m_out.out_float_columns += 3;
        }
        write.mask = guard.slot;
        m_out.writes.push_back(write);
        if (runtime_target != kNone) {
          /* Velocities and forces of it too: at the flush, as the VM's commands. */
          m_moved_runtime.insert(runtime_target);
        }
        else if (write.is_move) {
          NoteOwnerMove();
        }
        break;
      }
      case LN_LeanNative::Generic: {
        const LN_OpCode opcode = headers[exec.instruction_index].opcode;
        if (opcode == LN_OpCode::TryFlowGate || opcode == LN_OpCode::SetFlowGate) {
          const std::vector<LN_BoolExpression> &gate_bools = m_program.GetBoolExpressions();
          if (instruction.int_value < 0 || uint32_t(instruction.int_value) >= gate_bools.size() ||
              gate_bools[uint32_t(instruction.int_value)].kind != LN_BoolExpressionKind::FlowGateOpen)
          {
            Refuse("a Flow Gate without its state");
            break;
          }
          const uint32_t state = uint32_t(instruction.int_value);
          const LN_FlowGateControl control = LN_FlowGateControl(instruction.secondary_int_value);
          if (opcode == LN_OpCode::SetFlowGate && control != LN_FlowGateControl::Toggle &&
              control != LN_FlowGateControl::Open && control != LN_FlowGateControl::Close)
          {
            Refuse("a Flow Gate control of another kind");
            break;
          }
          /* Start Open, read only when the gate is first evaluated. */
          m_in_stateful++;
          const Slot start_open = Bool(gate_bools[state].input0, false);
          m_in_stateful--;
          if (!m_refusal.empty()) {
            break;
          }
          Node node;
          node.op = opcode == LN_OpCode::TryFlowGate ? NodeOp::SGateTry : NodeOp::SGateSet;
          node.a = guard.slot;
          node.b = start_open.slot;
          node.property_ref = state;
          node.int_value = instruction.secondary_int_value;
          node.out = NewBool();
          m_out.nodes.push_back(node);
          if (opcode == LN_OpCode::TryFlowGate) {
            /* A refused Enter is not marked executed: the op ran only where the gate passed. */
            mask = node.out;
            op_marks = false;
          }
          break;
        }
        if (opcode == LN_OpCode::Translate || opcode == LN_OpCode::MoveToward ||
            opcode == LN_OpCode::RotateToward || opcode == LN_OpCode::AlignAxisToVector)
        {
          /* The object it moves or turns (plan C1-11): the owner, a named object or one each NPC
           * finds (the handlers read their values first, then ResolveCommandTarget). */
          uint32_t motion_target = kNone, motion_column = kNone;
          if (!CommandTarget(instruction.value_expr_index, motion_target, motion_column)) {
            Refuse("a motion of an object that is neither the owner, named nor found while "
                   "playing");
            break;
          }
          if (motion_column != kNone && opcode == LN_OpCode::Translate) {
            /* A move needs its object when the run starts (the found objects' move rules). */
            Refuse("a move of an object a tree property holds");
            break;
          }
          m_native_write_instructions.insert(exec.instruction_index);
          Write write;
          write.mask = guard.slot;
          write.runtime_target = motion_target;
          write.target_column = motion_column;
          if (opcode == LN_OpCode::Translate) {
            /* AppendApplyMovement(vector * (speed * tick_seconds), local). */
            const Vec vector = Vector(instruction.vector_expr_index, instruction.vector_value);
            const Slot speed = Float(instruction.float_expr_index, 1.0f);
            const Slot local = Bool(instruction.bool_expr_index, false);
            if (!m_refusal.empty()) {
              break;
            }
            read(vector.queries | speed.queries | local.queries);
            const Slot dt = DeltaSlot(false);
            const Slot step = FloatNode(NodeOp::FMul, speed, &dt);
            Vec scale;
            scale.slots = {step.slot, step.slot, step.slot};
            const Vec movement = Componentwise(NodeOp::FMul, vector, scale);
            write.kind = WriteKind::Transform;
            write.command = CT::ApplyMovement;
            write.is_move = true;
            write.last_write_wins = last_write_wins(CT::ApplyMovement);
            write.local_last_write_wins = write.last_write_wins;
            write.local = local.slot;
            write.out_bool = m_out.out_bool_columns++;
            write.vector = movement.slots;
            write.out_float = m_out.out_float_columns;
            m_out.out_float_columns += 3;
            m_out.writes.push_back(write);
            if (motion_target != kNone) {
              /* A move of a found object: the C1-9 E2 rules (no frame check: Translate's handler
               * makes none). */
              m_moved_runtime.insert(motion_target);
            }
            else {
              NoteOwnerMove();
            }
            break;
          }
          /* Move To, Rotate To and Align Axis record a motion the flush applies (never early):
           * their object is only written. */
          write.kind = WriteKind::Motion;
          Vec vector;
          Slot scalar;
          Slot int_a = IntConst(0), int_b = IntConst(0);
          Vec secondary = VectorConst(MT_Vector3(0.0f, 0.0f, 0.0f));
          if (opcode == LN_OpCode::MoveToward) {
            write.command = CT::MoveToward;
            vector = Vector(instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f));
            scalar = Float(instruction.float_expr_index, 1.0f);
            const Slot stop = Float(instruction.secondary_float_expr_index, 0.5f);
            const float nan = std::numeric_limits<float>::quiet_NaN();
            const Vec position = instruction.secondary_vector_expr_index == kNone ?
                                     VectorConst(MT_Vector3(nan, nan, nan)) :
                                     Vector(instruction.secondary_vector_expr_index,
                                            MT_Vector3(nan, nan, nan));
            if (!m_refusal.empty()) {
              break;
            }
            read(vector.queries | scalar.queries | stop.queries | position.queries);
            /* Reached: worked out where the op records (MoveToward's handler). */
            const uint32_t state = lean_op.state_index;
            if (state != kNone) {
              const Slot dt = DeltaSlot(false);
              Node reached;
              reached.op = NodeOp::SMoveReached;
              reached.a = guard.slot;
              reached.property_ref = state;
              reached.position_a = vector.slots;
              reached.position_b = position.slots;
              reached.b = scalar.slot;
              reached.c = stop.slot;
              reached.d = dt.slot;
              m_out.nodes.push_back(reached);
            }
            const Slot frame_delta = DeltaSlot(true);
            secondary.slots = {stop.slot, frame_delta.slot, FloatConst(0.0f).slot};
            write.motion_bool = false;
            write.motion_secondary_bool = true;
          }
          else if (opcode == LN_OpCode::RotateToward) {
            write.command = CT::RotateToward;
            vector = Vector(instruction.vector_expr_index, MT_Vector3(1.0f, 0.0f, 0.0f));
            scalar = Float(instruction.float_expr_index, 1.0f);
            int_a = Int(instruction.int_expr_index, 2);
            int_b = Int(instruction.secondary_int_expr_index, 1);
            if (!m_refusal.empty()) {
              break;
            }
            read(vector.queries | scalar.queries | int_a.queries | int_b.queries);
          }
          else {
            write.command = CT::AlignAxisToVector;
            vector = Vector(instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 1.0f));
            int_a = Int(instruction.int_expr_index, 2);
            scalar = Float(instruction.float_expr_index, 1.0f);
            if (!m_refusal.empty()) {
              break;
            }
            read(vector.queries | int_a.queries | scalar.queries);
          }
          write.vector = vector.slots;
          write.secondary = secondary.slots;
          write.scalar = scalar.slot;
          write.int_a = int_a.slot;
          write.int_b = int_b.slot;
          write.counts_last_write_wins = last_write_wins(write.command);
          write.out_float = m_out.out_float_columns;
          m_out.out_float_columns += 7;
          write.out_int = m_out.out_int_columns;
          m_out.out_int_columns += 2;
          m_out.writes.push_back(write);
          break;
        }
        if (opcode == LN_OpCode::ApplyForceToTarget) {
          /* Plan C2 P5: its handler's math (ColumnForceToTarget) on where the moved object is (a
           * live pose read of the owner, under the owner's read rules); its force is applied at
           * the flush as the handler's recorded command (never early: no move). An object found
           * while playing or named stays a direct program's. */
          uint32_t force_target = kNone, force_column = kNone;
          if (!CommandTarget(instruction.value_expr_index, force_target, force_column) ||
              force_target != kNone || force_column != kNone)
          {
            Refuse("Apply Force To Target of an object other than the owner");
            break;
          }
          const Vec target_position = Vector(instruction.vector_expr_index,
                                             instruction.vector_value);
          const Slot force = Float(instruction.float_expr_index, 10.0f);
          const Slot reached = Float(instruction.secondary_float_expr_index, 0.1f);
          const Slot stop = Float(instruction.tertiary_float_expr_index, 0.1f);
          if (!m_refusal.empty()) {
            break;
          }
          read(target_position.queries | force.queries | reached.queries | stop.queries);
          if (!NoteOwnerLiveRead()) {
            break;
          }
          Node pose;
          pose.op = NodeOp::VAttr;
          pose.compare = uint8_t(LN_ObjectAttributeId::WorldPosition);
          pose.end_a = EndKind::Owner;
          pose.position_b = VectorConst(MT_Vector3(0.0f, 0.0f, 0.0f)).slots;
          pose.out = NewFloat();
          pose.out2 = NewFloat();
          pose.out3 = NewFloat();
          m_out.nodes.push_back(pose);
          Node node;
          node.op = NodeOp::MForceToTarget;
          node.a = guard.slot;
          node.property_ref = lean_op.state_index;
          node.position_a = target_position.slots;
          node.position_b = {pose.out, pose.out2, pose.out3};
          node.b = force.slot;
          node.c = reached.slot;
          node.d = stop.slot;
          node.out = NewBool();
          node.vec_c = {NewFloat(), NewFloat(), NewFloat()};
          m_out.nodes.push_back(node);
          m_native_write_instructions.insert(exec.instruction_index);
          Write write;
          write.kind = WriteKind::Transform;
          write.command = CT::ApplyForce;
          write.mask = node.out;
          write.vector = node.vec_c;
          write.last_write_wins = last_write_wins(CT::ApplyForce);
          write.local_last_write_wins = write.last_write_wins;
          write.out_float = m_out.out_float_columns;
          m_out.out_float_columns += 3;
          m_out.writes.push_back(write);
          break;
        }
        if (exec.kind == LN_ExecOpKind::TreePropertyCommand && opcode == LN_OpCode::SetTreeProperty) {
          if (TreeWriteIsNative(instruction)) {
            CompileTreeWrite(instruction, guard.slot, read);
            break;
          }
          /* Plan C1-11: any other value (text, an object, a colour, a rotation, a list...)
           * through the VM's own handler, NPC by NPC (it sets the property at once and records the
           * command); the property's fields are read again after it. */
          if (!m_bridge_ok) {
            Refuse("a tree property write of another value");
            break;
          }
          const uint32_t ran = CompileBridgedOp(instruction, lean_op, guard.slot);
          if (ran != kNone) {
            mask = ran;
            op_marks = false;
            bridged_op = true;
            ReloadTree(instruction.property_ref_index);
          }
          break;
        }
        if (exec.kind == LN_ExecOpKind::TimeFlowControl && opcode == LN_OpCode::TryOnce) {
          if (instruction.int_value < 0) {
            break; /* no state: the op only marks itself */
          }
          const uint32_t state = uint32_t(instruction.int_value);
          if (m_once_out.count(state) != 0) {
            Refuse("two Once ops of one state");
            break;
          }
          Node node;
          node.op = NodeOp::SOnce;
          node.a = guard.slot;
          node.property_ref = state;
          node.out = NewBool();
          m_out.nodes.push_back(node);
          m_once_out.emplace(state, node.out);
          break;
        }
        if (exec.kind == LN_ExecOpKind::TimeFlowControl && opcode == LN_OpCode::TryFlowRouter) {
          if (instruction.int_value < 0 || instruction.secondary_int_value <= 0) {
            Refuse("a Flow Router without its state");
            break;
          }
          const uint32_t state = uint32_t(instruction.int_value);
          if (m_router_out.count(state) != 0) {
            Refuse("two Flow Router ops of one state");
            break;
          }
          auto last = m_router_last.find(state);
          if (last == m_router_last.end()) {
            Node load;
            load.op = NodeOp::SFlowLast;
            load.property_ref = state;
            load.out = NewInt();
            m_out.nodes.push_back(load);
            last = m_router_last.emplace(state, load.out).first;
          }
          Node node;
          node.op = NodeOp::SFlowRouter;
          node.a = guard.slot;
          node.b = last->second;
          node.property_ref = state;
          node.int_value = instruction.secondary_int_value;
          node.out = NewInt();  /* the route taken, -1: none */
          node.out2 = NewInt(); /* the last route after the op */
          m_out.nodes.push_back(node);
          m_router_out.emplace(state, node.out);
          last->second = node.out2;
          break;
        }
        if (exec.kind == LN_ExecOpKind::TimeFlowControl &&
            (opcode == LN_OpCode::ArmNextFrame || opcode == LN_OpCode::ArmTimer ||
             opcode == LN_OpCode::ArmDelay || opcode == LN_OpCode::UpdatePulsify ||
             opcode == LN_OpCode::UpdateBarrier || opcode == LN_OpCode::TryCooldown ||
             opcode == LN_OpCode::ResetCooldown))
        {
          CompileTimeOp(instruction, headers[exec.instruction_index], guard.slot, read);
          break;
        }
        if ((exec.kind == LN_ExecOpKind::ObjectStateCommand &&
             (opcode == LN_OpCode::SetVisibility || opcode == LN_OpCode::ResetSmoothing)) ||
            (exec.kind == LN_ExecOpKind::ObjectColorCommand && opcode == LN_OpCode::SetObjectColor))
        {
          /* OpDirectObjectStateCommand / OpDirectObjectColorCommand of the owner, a named object or
           * one each NPC finds (plan C1-11: values read first, then ResolveCommandTarget; applied
           * at the flush, never early: the object is only written). */
          uint32_t service_target = kNone, service_column = kNone;
          if (!CommandTarget(instruction.value_expr_index, service_target, service_column)) {
            Refuse("an object state command for an object that is neither the owner, named nor "
                   "found while playing");
            break;
          }
          m_native_write_instructions.insert(exec.instruction_index);
          Write write;
          write.kind = WriteKind::Service;
          write.runtime_target = service_target;
          write.target_column = service_column;
          write.mask = guard.slot;
          if (opcode == LN_OpCode::SetVisibility) {
            const Slot visible = Bool(instruction.bool_expr_index, true);
            const Slot children = Bool(instruction.secondary_bool_expr_index, false);
            if (!m_refusal.empty()) {
              break;
            }
            read(visible.queries | children.queries);
            write.command = CT::SetVisibility;
            write.flag_a = visible.slot;
            write.flag_b = children.slot;
            write.out_bool = m_out.out_bool_columns;
            m_out.out_bool_columns += 2;
          }
          else if (opcode == LN_OpCode::ResetSmoothing) {
            /* Set Object Attribute's Teleport, right after its write: only when it is on. */
            const Slot teleport = Bool(instruction.bool_expr_index, false);
            if (!m_refusal.empty()) {
              break;
            }
            read(teleport.queries);
            write.command = CT::ResetSmoothing;
            write.mask = AndSlot(guard.slot, teleport.slot);
          }
          else {
            const std::vector<LN_ColorExpression> &colors = m_program.GetColorExpressions();
            const uint32_t index = instruction.color_expr_index;
            Slot rgba[4];
            if (index == kNone || index >= colors.size()) {
              for (int c = 0; c < 4; c++) {
                rgba[c] = FloatConst(instruction.color_value[c]);
              }
            }
            else if (colors[index].kind == LN_ColorExpressionKind::Constant) {
              for (int c = 0; c < 4; c++) {
                rgba[c] = FloatConst(colors[index].color_value[c]);
              }
            }
            else if (colors[index].kind == LN_ColorExpressionKind::Combine) {
              /* Read order (D20): A (input 3), then B, G and R. */
              const LN_ColorExpression &color = colors[index];
              rgba[3] = Float(color.input3, color.color_value.w());
              rgba[2] = Float(color.input2, color.color_value.z());
              rgba[1] = Float(color.input1, color.color_value.y());
              rgba[0] = Float(color.input0, color.color_value.x());
            }
            else if (m_bridge_ok) {
              if (!BridgeColor(index, instruction.color_value, rgba)) {
                break;
              }
            }
            else {
              Refuse(std::string("color kind ") +
                     LN_KindName(LN_KindDomain::Color, uint16_t(colors[index].kind)));
              break;
            }
            if (!m_refusal.empty()) {
              break;
            }
            read(rgba[0].queries | rgba[1].queries | rgba[2].queries | rgba[3].queries);
            write.command = CT::SetObjectColor;
            for (int c = 0; c < 4; c++) {
              write.color[c] = rgba[c].slot;
            }
            write.out_float = m_out.out_float_columns;
            m_out.out_float_columns += 4;
          }
          write.last_write_wins = last_write_wins(write.command);
          write.counts_last_write_wins = write.last_write_wins;
          m_out.writes.push_back(write);
          break;
        }
        if (exec.kind == LN_ExecOpKind::TimeFlowControl && opcode == LN_OpCode::TryRepeatLimit) {
          if (instruction.int_value < 0 || instruction.int_expr_index == kNone) {
            Refuse("a Repeat Limit without its state");
            break;
          }
          const uint32_t state = uint32_t(instruction.int_value);
          if (m_repeat_out.count(state) != 0) {
            Refuse("two Repeat Limit ops of one state");
            break;
          }
          const Slot limit = Int(instruction.int_expr_index, 0);
          if (!m_refusal.empty()) {
            break;
          }
          read(limit.queries);
          Node node;
          node.op = NodeOp::SRepeat;
          node.a = guard.slot;
          node.b = limit.slot;
          node.property_ref = state;
          node.out = NewBool();
          node.out2 = NewBool();
          m_out.nodes.push_back(node);
          m_repeat_out.emplace(state, std::make_pair(node.out, node.out2));
          break;
        }
        if (opcode == LN_OpCode::SampleMotion || opcode == LN_OpCode::ResetMotion) {
          /* Plan C2 P5: Predict Motion through the VM's own handler (its state is the tree's, its
           * outputs bridged reads of it). Of a position it reads nothing of the scene; of the
           * owner or a named object it applies that object's pending moves and reads its pose, as
           * a distance to it does: the owner only before any move of it in the pass (and the run
           * starts with none pending: owner_pending_ok), a named object as the bridge's named
           * objects (applied_named: the run's first tree plays on the VM while it has some). One
           * found while playing stays a direct program's. */
          if (!m_bridge_ok) {
            Refuse("Predict Motion without the bridge");
            break;
          }
          if (opcode == LN_OpCode::SampleMotion) {
            const LN_MotionSourceMode source = LN_MotionSourceMode(instruction.secondary_int_value);
            const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
            const uint32_t object = instruction.value_expr_index;
            const bool object_set = source == LN_MotionSourceMode::Object && object != kNone;
            const bool owner_value = object_set && object < values.size() &&
                                     values[object].kind == LN_ValueExpressionKind::OwnerObject;
            const bool named = object_set && object < values.size() &&
                               values[object].kind == LN_ValueExpressionKind::Constant &&
                               values[object].value.type == LN_ValueType::ObjectRef;
            if (source == LN_MotionSourceMode::OwnerObject || owner_value) {
              if (OwnerMovedHere()) {
                Refuse("Predict Motion of the owner after a move of it");
                break;
              }
              m_samples_owner = true;
            }
            else if (object_set && !named) {
              Refuse("Predict Motion of an object found while playing");
              break;
            }
          }
          const uint32_t ran = CompileBridgedOp(instruction, lean_op, guard.slot);
          if (ran != kNone) {
            mask = ran;
            op_marks = false;
            bridged_op = true;
          }
          break;
        }
        if (m_bridge_ok && BridgeableOp(exec.kind, opcode)) {
          /* The VM's own handler NPC by NPC; it stamps the op itself. */
          const uint32_t ran = CompileBridgedOp(instruction, lean_op, guard.slot);
          if (ran != kNone) {
            mask = ran;
            op_marks = false;
            bridged_op = true;
          }
          break;
        }
        Refuse(std::string("no column kernel: ") + LN_KindName(LN_KindDomain::Op, uint16_t(opcode)));
        break;
      }
    }
    if (!m_refusal.empty()) {
      break;
    }
    m_always_marks[m_op] = op_marks ? 1 : 0;
    m_op_gate.push_back(m_gate);
    m_masks.push_back(mask);
    if (lean_op.state_index != kNone && !bridged_op) {
      Node stamp;
      stamp.op = NodeOp::SStamp;
      stamp.a = mask;
      stamp.property_ref = lean_op.state_index;
      m_out.nodes.push_back(stamp);
    }
    m_out.op_state.push_back(lean_op.state_index);
    m_out.op_mask.push_back(mask);
  }
  if (m_refusal.empty()) {
    /* An object (the owner or a named one) measured on some routes only: its pending moves would
     * be applied at a time the VM does not apply them. Read whenever a gate's routes run is
     * enough: the run applies them when some NPC's gate lets its route run this step. */
    for (const auto &entry : m_queries) {
      const QuerySlots &query = entry.second;
      if (!query.measures_object) {
        continue;
      }
      LN_ColumnProgram::ReadWhen when;
      when.always = (m_always_read & query.bit) != 0;
      for (const auto &gate : m_gate_reads) {
        if ((gate.second & query.bit) != 0) {
          when.gates |= uint64_t(1) << gate.first;
        }
      }
      if (!when.always && when.gates == 0) {
        /* Was refused ("an object measured on some routes only"): the run starts only with
         * nothing pending for it, so no read applies anything (`when` stays never). */
        NoteQuietReads("an object measured on some routes only");
      }
      LN_ColumnProgram::QueryRead read;
      read.when = when;
      read.a = query.a;
      read.b = query.b;
      read.a_object = query.object_a;
      read.b_object = query.object_b;
      m_out.query_reads.push_back(read);
    }
  }
  {
    /* Does a read of the program apply the owner's pending moves (OwnerPendingOk)? */
    const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
    const auto owner = [&](const uint32_t value) {
      return value == kNone ||
             (value < values.size() && values[value].kind == LN_ValueExpressionKind::OwnerObject);
    };
    bool reads_owner = m_add_object_at_owner || m_samples_owner;
    for (const LN_QueryExpression &query : m_program.GetQueryExpressions()) {
      switch (query.kind) {
        case LN_QueryExpressionKind::DistanceBetween:
          reads_owner = reads_owner ||
                        (query.distance_endpoint_a != LN_DistanceEndpointMode::Position &&
                         owner(query.input0)) ||
                        (query.distance_endpoint_b != LN_DistanceEndpointMode::Position &&
                         owner(query.input1));
          break;
        case LN_QueryExpressionKind::Raycast:
        case LN_QueryExpressionKind::RaycastAll:
        case LN_QueryExpressionKind::ShapeCast:
        case LN_QueryExpressionKind::ShapeCastAll:
          reads_owner = reads_owner || owner(query.input0);
          break;
        case LN_QueryExpressionKind::VectorAverage:
          break; /* live positions: it applies no pending move */
        default:
          reads_owner = true;
          break;
      }
    }
    m_out.owner_pending_ok = !reads_owner;
  }
  if (m_refusal.empty()) {
    for (const uint32_t object : m_runtime_pose_unprotected) {
      if (m_measured_runtime.count(object) != 0) {
        /* Was refused: the run gives its NPCs back when such an object has pending moves. */
        NoteQuietReads("a pose read of an object found while playing before a distance to it is "
                       "surely read");
        break;
      }
    }
  }
  if (m_refusal.empty()) {
    for (const uint32_t object : m_moved_runtime) {
      if (m_measured_runtime.count(object) != 0) {
        /* The VM's distance read would apply the program's own moves of it first. */
        Refuse("a distance to an object the program moves");
        break;
      }
    }
  }
  if (m_refusal.empty()) {
    const size_t count = m_out.runtime_objects.size();
    m_out.runtime_measured.assign(count, 0);
    m_out.runtime_pose_unprotected.assign(count, 0);
    m_out.runtime_moved.assign(count, 0);
    m_out.runtime_relative.assign(count, 0);
    for (const uint32_t object : m_measured_runtime) {
      m_out.runtime_measured[object] = 1;
    }
    for (const uint32_t object : m_runtime_pose_unprotected) {
      m_out.runtime_pose_unprotected[object] = 1;
    }
    for (const uint32_t object : m_moved_runtime) {
      m_out.runtime_moved[object] = 1;
      m_out.moves_others = true;
    }
    for (const uint32_t object : m_relative_runtime) {
      m_out.runtime_relative[object] = 1;
    }
    /* The named objects whose reads may apply pending moves (the VM's FlushPendingRaycast-
     * Transforms and Add Object's reference: queries and ops): those the native nodes read, and
     * in a bridged program every named object a ray's or a shape cast's field or an op's object
     * field holds (the same index anywhere there: more than they use, never less). Others are
     * read live, for their name (Get Child By Name's hint) or listed for Get Average Center
     * Position: their pending moves stay pending in the VM too. Native reads, bridged program or
     * not: only a distance applies pending moves (a pose or axis read reads them pending; plan
     * C1-11: a bridged program counted every named object the native nodes read, so a named
     * object it moves and reads gave the run back at every pass). */
    m_out.applied_named.clear();
    for (const uint32_t k : m_measured_objects) {
      m_out.applied_named.push_back(m_out.object_values[k]);
    }
    std::sort(m_out.applied_named.begin(), m_out.applied_named.end());
    std::unordered_set<uint32_t> named(m_out.constant_objects.begin(),
                                       m_out.constant_objects.end());
    const auto apply = [&](const uint32_t value) {
      if (named.erase(value) != 0) {
        m_out.applied_named.push_back(value);
      }
    };
    for (const uint32_t value : m_out.applied_named) {
      named.erase(value);
    }
    for (const LN_QueryExpression &query : m_program.GetQueryExpressions()) {
      if (query.kind == LN_QueryExpressionKind::VectorAverage) {
        continue;
      }
      for (const uint32_t field : {query.input0,
                                   query.input1,
                                   query.input2,
                                   query.input3,
                                   query.input4,
                                   query.input5,
                                   query.input6,
                                   query.input7,
                                   query.input8,
                                   query.input9,
                                   query.ignore_list_expr_index})
      {
        apply(field);
      }
    }
    const std::vector<LN_InstructionHeader> &all_headers = m_program.GetInstructionHeaders(m_event);
    for (uint32_t index = 0; index < all_headers.size(); index++) {
      if (m_native_write_instructions.count(index) != 0) {
        continue; /* (plan C1-10: a write's target applies nothing) */
      }
      const LN_Instruction &instruction = m_program.GetInstructionPayload(m_event,
                                                                          all_headers[index]);
      apply(instruction.value_expr_index);
      apply(instruction.secondary_value_expr_index);
    }
  }
  if (m_refusal.empty() && m_add_objects > 0) {
    /* A copy made by an earlier NPC could be what a later NPC finds (by a name from a property,
     * a constant name of an object not in the scene, a child). A named object (an object picked
     * in the tree, plan C2 fix 1: the shooter's Player) cannot be: a copy has its template's
     * name, and a template the program also names is refused (AddObjectAllowedHere). */
    const std::vector<LN_ValueExpression> &values = m_program.GetValueExpressions();
    for (const uint32_t value : m_out.runtime_objects) {
      const bool named = value < values.size() &&
                         values[value].kind == LN_ValueExpressionKind::Constant &&
                         values[value].value.type == LN_ValueType::ObjectRef;
      if (!named) {
        Refuse("Add Object next to an object found while playing");
        break;
      }
    }
  }
  if (m_refusal.empty()) {
    /* Plan C1-10: how each runtime object is found. */
    const size_t count = m_out.runtime_objects.size();
    m_out.runtime_invariant.assign(count, 0);
    m_out.runtime_after_run.assign(count, 0);
    m_out.runtime_collision.assign(count, 0);
    for (size_t k = 0; k < count; k++) {
      bool invariant = false, collision = false;
      RuntimeSource(m_out.runtime_objects[k], 0, invariant, collision);
      const bool read_in_run = m_measured_runtime.count(uint32_t(k)) != 0 ||
                               m_moved_runtime.count(uint32_t(k)) != 0 ||
                               m_pose_read_runtime.count(uint32_t(k)) != 0;
      m_out.runtime_invariant[k] = invariant && !collision ? 1 : 0;
      /* Only written: found after the run where a write's op ran (the VM finds it at the op,
       * and only there: a collision's find keeps the tree's collision bookkeeping). */
      m_out.runtime_after_run[k] = !read_in_run && !invariant ? 1 : 0;
      m_out.runtime_collision[k] = collision ? 1 : 0;
      if (collision) {
        m_out.collision_sources = true;
        if (read_in_run) {
          Refuse("an object a collision finds, read or moved (only its properties are written)");
          break;
        }
      }
    }
    m_out.adds_objects = m_add_objects > 0;
    m_out.applies_moves = !m_out.owner_pending_ok || !m_out.applied_named.empty() ||
                          !m_measured_runtime.empty() || m_add_objects > 0;
    /* A move applied while trees record may drop the contacts the logic reads (Jolt's
     * RemoveLogicActiveContactsForBody): a collision's object found after it is found otherwise.
     * Such a run starts when nothing it would apply is pending. */
    m_out.quiet_start = m_out.live_positions || (m_out.collision_sources && m_out.applies_moves) ||
                        m_out.native_rays;
    if (m_refusal.empty() && m_out.collision_sources) {
      for (const LN_BoolExpression &expression : m_program.GetBoolExpressions()) {
        if (m_refusal.empty() && expression.kind == LN_BoolExpressionKind::CollisionExit) {
          /* On Exit marks the payload Collision Hit Object then gives (the last object touched):
           * the find after the run would see the mark of a later op. */
          Refuse("a collision's object next to an On Exit");
        }
      }
    }
  }
  if (m_refusal.empty() && m_add_objects > 1) {
    Refuse("more than one Add Object (the copies would be made in another order)");
  }
  if (m_refusal.empty() && m_add_objects > 0 && m_bridged_sees_objects) {
    Refuse("Add Object next to a bridged node that may see its copies");
  }
  if (m_refusal.empty() && m_measures_owner && m_owner_velocity_read) {
    /* Velocities are read when the run starts (was refused). */
    NoteQuietReads("the owner's velocity read next to a distance from it");
  }
  if (m_refusal.empty()) {
    for (const uint32_t object : m_live_pose_objects) {
      if (m_measured_objects.count(object) != 0) {
        /* Was refused: a live read sees its pending moves, a distance applies them. */
        NoteQuietReads("an object's pose read next to a distance to it");
        break;
      }
    }
  }
  if (m_refusal.empty() && m_quiet_reads) {
    m_out.quiet_start = true;
    m_out.quiet_reads = m_quiet_why;
  }
  if (!m_refusal.empty()) {
    r_refusal = m_refusal;
    return false;
  }
  /* Writes that may share a key within one tree's pass (the flush's merge must see them). */
  const auto types_of = [](const Write &write, LN_CommandBuffer::CommandType r_types[2]) {
    r_types[0] = write.command;
    r_types[1] = write.type_from_local ? write.local_command : write.command;
  };
  for (size_t one = 0; one < m_out.writes.size(); one++) {
    for (size_t other = one + 1; other < m_out.writes.size(); other++) {
      Write &a = m_out.writes[one];
      Write &b = m_out.writes[other];
      bool same = false;
      if (a.kind == WriteKind::Number && b.kind == WriteKind::Number) {
        same = a.property_ref == b.property_ref;
      }
      else if (a.kind == WriteKind::Transform && b.kind == WriteKind::Transform) {
        LN_CommandBuffer::CommandType ta[2], tb[2];
        types_of(a, ta);
        types_of(b, tb);
        same = ta[0] == tb[0] || ta[0] == tb[1] || ta[1] == tb[0] || ta[1] == tb[1];
      }
      if (same) {
        a.may_collide = b.may_collide = true;
        m_out.any_collide = true;
      }
    }
  }
  for (const Write &write : m_out.writes) {
    if (write.runtime_target != kNone || write.target_column != kNone) {
      m_out.any_found_target_write = true;
    }
    /* (ListWrites' list_lane for a lane neither shared nor captured.) */
    const bool may_last_write_wins = write.last_write_wins ||
                                     (write.type_from_local && write.local_last_write_wins);
    if (may_last_write_wins && write.kind != WriteKind::Captured &&
        (write.may_collide || write.runtime_target != kNone || write.target_column != kNone))
    {
      m_out.lists_unshared_last_write_wins = true;
    }
    if (write.kind == WriteKind::Number && write.last_write_wins) {
      m_out.any_last_write_wins_number = true;
    }
    const bool own = write.runtime_target == kNone && write.target_column == kNone;
    if (own && write.kind == WriteKind::Number && write.last_write_wins &&
        write.property_ref < m_out.property_names.size())
    {
      m_out.own_last_write_wins_keys.push_back(m_out.property_names[write.property_ref]);
    }
    if (own && write.kind == WriteKind::Transform) {
      if (write.last_write_wins) {
        m_out.own_last_write_wins_keys.push_back("\x01" + std::to_string(int(write.command)));
      }
      if (write.type_from_local && write.local_last_write_wins) {
        m_out.own_last_write_wins_keys.push_back("\x01" +
                                                 std::to_string(int(write.local_command)));
      }
    }
    if (write.kind == WriteKind::Number || write.kind == WriteKind::Modify) {
      (write.runtime_target != kNone || write.target_column != kNone ?
           m_out.found_property_writes :
           m_out.own_property_writes) = true;
    }
    if (write.kind == WriteKind::Captured) {
      m_out.captured_writes = true;
    }
  }
  std::vector<std::string> &own_names = m_out.own_last_write_wins_keys;
  std::sort(own_names.begin(), own_names.end());
  own_names.erase(std::unique(own_names.begin(), own_names.end()), own_names.end());
  for (const auto &entry : m_tree) {
    if (entry.second.written) {
      LN_ColumnProgram::TreeProperty property;
      property.ref = entry.first;
      property.f = entry.second.f;
      property.i = entry.second.i;
      property.b = entry.second.b;
      property.last_write = entry.second.last_write;
      m_out.tree_properties.push_back(property);
    }
  }
  return true;
}

/** Plan C2 P5: `event` of `program` as a direct program. Its lane runs alone in its tree's place
 * on the main thread; the runner walks the event's blocks with the column walk
 * (RunDirectLane: a gated block on its gate's steps with its gate's time, a Loop's Completed route
 * where the loop completed in the pass, a Loop body through RunLoopBody) and runs each op through
 * the shared handlers (OpLean, OpDirect in the reference mode), which read their own inputs and
 * record into the tick's buffer, in the trees' own order (On UI Event routes included: plan C2
 * P7). */
bool CompileDirect(const LN_Program &program,
                   const LN_LeanProgram &lean,
                   const LN_Event event,
                   LN_ColumnProgram &out,
                   std::string &r_refusal)
{
  const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(event);
  /* (Plan C2 P7: On UI Event routes too: the walk repeats a route per further event of the
   * frame. An event without ops too: a tree in its first tick runs On
   * Init, one with Delays or Timers counts them down, as ExecuteReady does.) */
  if (size_t(event) >= std::size(lean.events) ||
      lean.events[size_t(event)].ops.size() != ir.ops.size())
  {
    r_refusal = "no lean ops";
    return false;
  }
  out.direct = true;
  out.bridged = true;
  out.on_main_thread = true;
  out.route_ops = uint32_t(ir.ops.size());
  out.batch_safe = LN_AnalyzeTreeBatchSafety(program).eligible;
  out.has_route = lean.events[size_t(event)].has_route;
  out.main_thread_why = "a direct program";
  return true;
}

}  // namespace

std::shared_ptr<const LN_ColumnProgram> LN_CompileColumnProgram(const LN_Program &program,
                                                                 const LN_Event event,
                                                                 std::string *r_refusal,
                                                                 const bool allow_direct)
{
  std::string refusal;
  std::shared_ptr<LN_ColumnProgram> out;
  program.EnsureLeanProgram();
  const std::shared_ptr<const LN_LeanProgram> &lean = program.GetLeanProgram();
  if (event == LN_Event::OnInit) {
    refusal = "On Init";
  }
  else if (lean == nullptr || !lean->valid) {
    refusal = "no lean program";
  }
  else {
    /* No tree batch rule is needed: the compiler takes only nodes it has kernels for, and checks
     * itself what makes a run read what the VM reads (distances, live poses, moves). */
    /* (Without `allow_direct`, the compiler's answer alone: native or refused.) */
    const bool force_direct = allow_direct &&
                              LN_GetColumnDirectMode() == LN_ColumnDirectMode::Force;
    const auto fresh = [&]() {
      out = std::make_shared<LN_ColumnProgram>();
      out->serial = g_column_program_serial.fetch_add(1, std::memory_order_relaxed) + 1;
      out->program_id = program.GetUniqueId();
      out->event = event;
      out->source_name = program.GetSourceTreeName();
    };
    bool lowered = false;
    if (!force_direct) {
      fresh();
      ColumnCompiler compiler(program, *lean, event, *out);
      lowered = compiler.Compile(refusal);
      if (!lowered) {
        out.reset();
      }
    }
    if (!lowered && allow_direct) {
      /* Plan C2 P5: the event as a direct program (its trees run alone, the op handlers recording
       * in the trees' places); `refusal` stays the compiler's answer (the report). */
      fresh();
      std::string direct_refusal;
      if (CompileDirect(program, *lean, event, *out, direct_refusal)) {
        out->direct_why = refusal;
      }
      else {
        out.reset();
        if (refusal.empty()) {
          refusal = direct_refusal;
        }
      }
    }
  }
  if (r_refusal != nullptr) {
    *r_refusal = refusal;
  }
  return out;
}

std::string LN_ColumnQueryReachForTests(const LN_Program &program,
                                        const char family,
                                        const uint32_t index)
{
  program.EnsureLeanProgram();
  const std::shared_ptr<const LN_LeanProgram> &lean = program.GetLeanProgram();
  if (lean == nullptr) {
    return "no lean program";
  }
  LN_ColumnProgram out;
  ColumnCompiler compiler(program, *lean, LN_Event::OnFixedUpdate, out);
  return compiler.DescribeReachForTests(family, index);
}

std::string LN_ColumnKindInputsText()
{
  std::string text;
  for (const KindInputRow &row : kKindInputRows) {
    text += std::string(row.name) + ":" + (row.fields[0] != '\0' ? " " : "") + row.fields + "\n";
  }
  return text;
}

std::string LN_DescribeColumnProgram(const LN_ColumnProgram &program)
{
  std::ostringstream text;
  if (program.direct) {
    text << "columns '" << program.source_name << "' direct route_ops=" << program.route_ops
         << " batch_safe=" << (program.batch_safe ? 1 : 0)
         << " has_route=" << (program.has_route ? 1 : 0) << " why=" << program.direct_why
         << "\n";
    return text.str();
  }
  text << "columns '" << program.source_name << "' route_ops=" << program.route_ops
       << " constants=" << program.constants.size() << " nodes=" << program.nodes.size()
       << " writes=" << program.writes.size() << " float_slots=" << program.float_slots
       << " int_slots=" << program.int_slots << " bool_slots=" << program.bool_slots
       << " named_objects=" << program.object_values.size()
       << " owner_position=" << (program.needs_owner_position ? 1 : 0)
       << " bridged=" << (program.bridged ? 1 : 0)
       << " averages_once=" << program.average_reads.size()
       << " live_positions=" << (program.live_positions ? 1 : 0)
       << " found_objects=" << program.runtime_objects.size() << " found_after_run="
       << std::count(program.runtime_after_run.begin(), program.runtime_after_run.end(), 1)
       << " collision_finds=" << (program.collision_sources ? 1 : 0)
       << " native_rays=" << (program.native_rays ? 1 : 0)
       << " main_thread=" << (program.main_thread_why.empty() ? 0 : 1)
       << " quiet_reads=" << (program.quiet_reads.empty() ? 0 : 1)
       << " run_invariants=" << program.run_invariants.size() << "\n";
  return text.str();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The runner
 * \{ */

namespace {

constexpr float kPi = 3.14159265358979323846f; /* LN_RuntimeTree.cpp's LN_PI */

/** One thread's arrays for a chunk: every slot of a program, kChunkLanes values each. */
struct ChunkScratch {
  std::vector<float> floats;
  std::vector<int32_t> ints;
  std::vector<uint8_t> bools;
  std::vector<LN_Rotation> rots;
  float px[kChunkLanes], py[kChunkLanes], pz[kChunkLanes];
  uint8_t owner_ok[kChunkLanes];
  /** Each lane's execution scope of the pass (LN_RuntimeTree::BeginColumnPass). */
  uint64_t serial[kChunkLanes];
  /** The program whose constants are filled in, and for how many lanes. */
  uint64_t constants_serial = 0;
  size_t constants_lanes = 0;
};

ChunkScratch &chunk_scratch()
{
  thread_local ChunkScratch scratch;
  return scratch;
}

struct NamedObject {
  KX_GameObject *object = nullptr;
  MT_Vector3 position = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool ok = false;
};

}  // namespace

/* Plan C2 P1's D3 gate, by program size (measured in the game, 1,000 trees of 10 programs of
 * one size in turn, waves on, against the old system's tree batches, Logic ms): a run's start and
 * end are this thread's, so a small program needs more trees in its run to win. Column nodes (not
 * constants): 15 (the brain with no chain) 0.29 vs 0.24 in runs of 1, 0.25 vs 0.24 in runs of 2,
 * 0.21 vs 0.23 in runs of 4; 20 0.31 vs 0.30 and 25 0.33 vs 0.32 in runs of 1; programs of 20-65
 * nodes mixed 0.40 vs 0.45. Later phases that cut a run's fixed cost should lower these. */
size_t LN_ColumnSizeFloor(const LN_ColumnProgram &program)
{
  if (program.direct) {
    return 1; /* (plan C2 P5: it runs alone, as a column pass) */
  }
  const size_t nodes = program.nodes.size();
  return nodes < 16 ? 4 : nodes < LN_COLUMN_TAKE_ALL_AVERAGE_NODES ? 2 : 1;
}

size_t LN_ColumnNodeCount(const LN_ColumnProgram &program)
{
  return program.nodes.size();
}

LN_ColumnMemory LN_ColumnProgramMemory(const LN_ColumnProgram &program)
{
  const auto bytes = [](const auto &vector) {
    return vector.capacity() * sizeof(typename std::decay_t<decltype(vector)>::value_type);
  };
  LN_ColumnMemory memory;
  size_t &p = memory.program_bytes;
  p = sizeof(LN_ColumnProgram) + program.source_name.capacity() + bytes(program.constants) +
      bytes(program.nodes) + bytes(program.writes) + bytes(program.object_values) +
      bytes(program.runtime_objects) + bytes(program.runtime_measured) +
      bytes(program.runtime_pose_unprotected) + bytes(program.runtime_moved) +
      bytes(program.runtime_relative) + bytes(program.runtime_invariant) +
      bytes(program.runtime_after_run) + bytes(program.runtime_collision) +
      bytes(program.property_names) + bytes(program.query_reads) +
      bytes(program.average_reads) + program.main_thread_why.capacity() +
      bytes(program.constant_objects) + bytes(program.applied_named) +
      bytes(program.velocity_reads) + bytes(program.op_state) + bytes(program.op_mask) +
      bytes(program.tree_properties) + bytes(program.tree_write_ops);
  for (const std::string &name : program.property_names) {
    p += name.capacity() > 15 ? name.capacity() + 1 : 0;
  }
  const size_t writes = program.writes.size();
  memory.lane_bytes = sizeof(LN_RuntimeTree *) + sizeof(uint32_t) + writes * 2 +
                      size_t(program.out_float_columns) * sizeof(float) +
                      size_t(program.out_int_columns) * sizeof(int32_t) +
                      size_t(program.out_bool_columns) +
                      size_t(program.out_rot_columns) * sizeof(LN_Rotation) +
                      program.runtime_objects.size() * sizeof(NamedObject) +
                      size_t(program.target_columns) * sizeof(KX_GameObject *) +
                      (program.bridged ? writes * 2 * sizeof(uint32_t) : 0) +
                      program.velocity_reads.size() * (sizeof(MT_Vector3) + 1);
  memory.chunk_bytes = sizeof(ChunkScratch) +
                       (size_t(program.float_slots) * sizeof(float) +
                        size_t(program.int_slots) * sizeof(int32_t) +
                        size_t(program.bool_slots) +
                        size_t(program.rot_slots) * sizeof(LN_Rotation)) *
                           kChunkLanes;
  return memory;
}

namespace {

/** UPBGE_LN_COLUMN_NATIVE_RAYS (plan C2 P3): unset / 1 = a Raycast with constant settings runs as
 * a column node (QRay), 0 = through the bridge as before. Tests: LN_SetColumnNativeRaysForTests. */
bool column_native_rays_on()
{
  const int override_mode = g_native_rays_override.load(std::memory_order_relaxed);
  if (override_mode >= 0) {
    return override_mode != 0;
  }
  static const bool on = [] {
    const char *value = std::getenv("UPBGE_LN_COLUMN_NATIVE_RAYS");
    return value == nullptr || std::strcmp(value, "0") != 0;
  }();
  return on;
}

/** UPBGE_LN_COLUMN_SEGMENTS: unset / 1 = a bridged run's nodes in segments (plan C1-9), 0 = the
 * runs before it (a bridged program that is not batch safe runs all its chunks on the main
 * thread). */
bool column_segments_on()
{
  static const bool on = []() {
    const char *value = std::getenv("UPBGE_LN_COLUMN_SEGMENTS");
    return !(value != nullptr && (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0));
  }();
  return on;
}

bool is_last_write_wins(const LN_CommandBuffer::CommandType type)
{
  return LN_CommandBuffer::GetCoalescingPolicy(type) ==
         LN_CommandBuffer::CoalescingPolicy::LastWriteWinsWithinBarrierSegment;
}

}  // namespace

struct LN_ColumnRunner::Impl {
  struct ProgramEntry {
    const void *lean = nullptr;
    std::shared_ptr<const LN_ColumnProgram> columns[3];
    bool compiled[3] = {false, false, false};
    std::string refusal[3];
    /** The names of the objects the program names (Constant Object values). */
    std::vector<std::string> names;
    /** The Plan (`plan_serial`) that last added its named objects to the barriers and kept its
     * column program alive for the tick (plan C2 P1: a set of programs cost an allocation a
     * program and tick, a shared pointer per lane two atomic operations). */
    uint64_t plan_stamp[3] = {0, 0, 0};
  };
  struct Run {
    /** Kept alive for the tick by `kept`. */
    const LN_ColumnProgram *program = nullptr;
    size_t count = 0;
    /** Its lanes' trees, in lane order, and their indices in the plan (a group's are strided). */
    std::vector<LN_RuntimeTree *> lane_trees;
    std::vector<uint32_t> lane_index;
    std::vector<NamedObject> objects;
    /** runtime_objects x count (plan C1-9): each NPC's object, found when the run starts. */
    std::vector<NamedObject> runtime;
    /** target_columns x count (plan C1-11): the objects writes act on, found where they run
     * (OTreeObj). */
    std::vector<KX_GameObject *> target_objects;
    /** writes x count, column by column: the write ran for the NPC, and the flush's merge left it
     * out. */
    std::vector<uint8_t> masks;
    std::vector<uint8_t> superseded;
    /** Per NPC: some write ran (any of its masks set when its chunk ended; masks are only cleared
     * after that). A lane without one has nothing to apply, count or list: the per-lane walks of
     * the run's end and the flush skip it (2026-11-06, the farm game: 10,000 crops a step, one in
     * 30 due, each walk over every write of every crop). */
    std::vector<uint8_t> lane_any;
    std::vector<float> floats;
    std::vector<int32_t> ints;
    std::vector<uint8_t> bools;
    std::vector<LN_Rotation> rots;
    /** The bridge (plan C1-8): per chunk, the buffer its bridged ops record into and the
     * references of what each recorded (kept until the flush applied them); per write and NPC,
     * where its references start and how many there are. */
    struct Capture {
      std::unique_ptr<LN_CommandBuffer> buffer;
      std::vector<uint64_t> refs;
      /** Per ref: the flush's merge supersedes the command (a later write of its key), as it
       * leaves out the VM's recorded one (MarkSuperseded; sized there, empty: none). */
      std::vector<uint8_t> superseded;
      bool Superseded(const size_t at) const
      {
        return at < superseded.size() && superseded[at] != 0;
      }
    };
    std::vector<Capture> captures;
    std::vector<uint32_t> captured_begin, captured_count;
    /** The velocity reads' values (velocity_reads x count), read when the run starts, and
     * whether there was one (else the read's fallback). */
    std::vector<MT_Vector3> velocities;
    std::vector<uint8_t> velocity_ok;
    /** The averages worked out once (average_reads), when the run starts. */
    std::vector<LN_QueryResult> averages;
    /** The run invariants (LN_ColumnProgram::run_invariants), when the run starts. */
    struct Invariant {
      float f = 0.0f;
      int32_t i = 0;
      uint8_t b = 0;
    };
    std::vector<Invariant> invariants;
    /** Set Tree Property ops that ran, per chunk (their commands count for the flush's gate). */
    std::vector<uint32_t> chunk_tree_sets;
    const LN_TickContext *context = nullptr;
    float uniforms[size_t(Uniform::Count)] = {};
    /** CountWrites' answer for this run, worked out column by column when the run ends (plan
     * C1-11 C-1: the per-lane walk cost a tenth of the flush). Valid while no mask or superseded
     * mark changed since and no lane of the run was written (`counts_valid`). */
    uint32_t count_total = 0;
    uint32_t count_last_write_wins = 0;
    bool counts_valid = false;
  };

  /** Per planned tree: where its writes are and what is left of them. */
  struct LaneInfo {
    /** The tree's sort key (what its recorded commands would carry). */
    uint64_t sort_key = 0;
    uint32_t run = kNone;
    uint32_t offset = 0;
    /** Its run has run, with a move not applied yet (a read may apply it early). */
    bool moves_pending = false;
    /** Its writes are applied (the flush passed its sort key). */
    bool written = false;
    /** Its Number and Modify writes of its owner were applied ahead (PrepareApply). */
    bool own_properties_applied = false;
  };

  std::function<bool(KX_GameObject *)> is_overlay_copy;
  /** Whether the scene has any menu copy (asked once a Plan; unset: ask every lane). */
  std::function<bool()> any_overlay_copy;
  /* UPBGE_LN_COLUMN_MIN_RUN: the shortest run (default kDefaultMinRunLanes; research for the
   * one-system plan, which measures shorter runs; tests set it with SetMinRunLanes). */
  size_t min_run_lanes = [] {
    const char *value = std::getenv("UPBGE_LN_COLUMN_MIN_RUN");
    const int lanes = value != nullptr ? std::atoi(value) : 0;
    return lanes >= 1 ? size_t(lanes) : kDefaultMinRunLanes;
  }();
  /** The shortest run grows for small programs (RunFloor) unless the shortest run was asked for
   * (UPBGE_LN_COLUMN_MIN_RUN, SetMinRunLanes). */
  bool size_floors = std::getenv("UPBGE_LN_COLUMN_MIN_RUN") == nullptr;
  static size_t RunFloor(const LN_ColumnProgram &program)
  {
    return LN_ColumnSizeFloor(program);
  }
  /** The helpers of the pool this tick's runs get (SetPlanHelpers): the size floors only with
   * helpers (what they leave out runs as tree batches). */
  size_t plan_helpers = 0;
  /** Plan's "no run is possible" answer for an event, kept while the tick's tree list, the
   * programs (LN_ProgramsChangedSerial) and the settings it read are the same: the walk touches
   * every tree object only to find it again (1,000 small trees no run takes: 0.01 ms a tick in
   * the game, cache misses after the drawing). */
  struct NoRunAnswer {
    bool valid = false;
    uint64_t programs = 0;
    size_t helpers = 0;
    size_t min_run = 0;
    bool floors = false;
    std::vector<LN_RuntimeTree *> trees;
    std::string refusal;
  };
  NoRunAnswer no_run_answer[3];
  /** Plan's runs of consecutive trees of one program, before the floors decide them. */
  struct PlanRun {
    size_t begin = 0;
    size_t end = 0;
    /** Its lanes' LaneFlags together. */
    uint8_t flags = 0;
  };
  /** Lanes whose writes the flush's merge lists all (kLaneListAll). */
  size_t list_all_lanes = 0;
  std::vector<PlanRun> plan_runs;
  std::unordered_map<uint64_t, ProgramEntry> programs;
  /** Never repeated (LN_RuntimeTree::m_columnEntryRunner: a runner made where an old one was). */
  const uint64_t runner_id = []() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }();
  std::vector<LN_RuntimeTree *> trees;
  std::vector<uint8_t> lane;
  std::vector<const LN_ColumnProgram *> lane_program;
  /** A lane a run gives back (a pending move when it starts, an object found while playing that
   * is a lane's): the column pass in its place (plan C2 P6). */
  void GiveBack(const size_t index, LN_CommandBuffer &buffer, const LN_TickContext &context)
  {
    RunDirectLane(index, buffer, context);
  }
  /** The column programs of this tick's lanes, each once (a program may grow and be compiled
   * again while its old column program still runs). */
  std::vector<std::shared_ptr<const LN_ColumnProgram>> kept;
  /** Counts the Plans (ProgramEntry::plan_stamp). */
  uint64_t plan_serial = 0;
  /** Plan set this tick's state up and EndTick has not cleaned it yet. */
  bool dirty = false;
  /** The lanes this tick's runs ran, added to LN_ColumnLaneCount at EndTick. */
  uint64_t lanes_ran = 0;
  /** Lanes given back that took the column pass this tick (LN_AddColumnPasses at EndTick). */
  uint64_t direct_ran = 0;
  /** This tick's runs of a program whose unshared lanes may list a last-write-wins write
   * (LN_ColumnProgram::lists_unshared_last_write_wins), and of a program with a last-write-wins
   * number write (another lane may write its key): ListWrites' shortcut. */
  size_t runs_not_plain = 0;
  size_t runs_last_write_wins_number = 0;
  std::vector<LaneInfo> info;
  std::vector<Run> runs;
  size_t run_count = 0;
  size_t lane_count = 0;
  /** This tick's lanes given back to the VM (GivenBackCount). */
  size_t given_back = 0;
  std::string refusal;
  /** LastOutsideNote. */
  std::string outside_note;
  std::unordered_set<const KX_GameObject *> barriers;
  /** This tick's lowering programs that name objects, each with a tree of it (the names are
   * looked up from), for `barriers`. */
  std::vector<std::pair<const ProgramEntry *, LN_RuntimeTree *>> named_programs;
  LN_CommandBuffer *buffer = nullptr;
  const LN_TickReadContext *read = nullptr;
  /** The run lanes with moves, by object (for the reads' early applies). Made at the first such
   * read after a run (most ticks have none: only an object carrying the pending mark is asked
   * about), on the main thread: before a tree batch records, PrepareForBatchReads makes it for its
   * helper threads. */
  /** An object's lanes with moves (several for an object of a group, plan C1-9), in sort key
   * order (ties by plan index). Lanes whose moves were applied since stay listed: the readers
   * ask LaneMovesObject. */
  using LaneList = std::vector<uint32_t>;
  mutable std::unordered_map<const KX_GameObject *, LaneList> lane_of_object;
  /** The runs the map holds: it takes each new run's lanes when asked (plan C2 P1: it was made
   * again from every lane after each run, and a pending check of a marked object between runs, a
   * Player its own tree moved, asked it after every run: 1,000 runs of 1 cost 85 ms a frame). */
  mutable size_t lane_map_runs = 0;
  /** The runs whose lanes are filled in (RunSet's end; a bridged run asks for the map before). */
  size_t runs_done = 0;
  /** A list took a lane before one of a later key (groups' runs are strided): sorted when read. */
  mutable bool lane_map_unsorted = false;
  bool LaneBefore(const uint32_t a, const uint32_t b) const
  {
    return info[a].sort_key < info[b].sort_key ||
           (info[a].sort_key == info[b].sort_key && a < b);
  }
  const std::unordered_map<const KX_GameObject *, LaneList> &LaneMap() const
  {
    const auto add = [&](const KX_GameObject *object, const uint32_t index) {
      LaneList &lanes = lane_of_object[object];
      if (!lanes.empty() && lanes.back() == index) {
        return;
      }
      if (!lanes.empty() && !LaneBefore(lanes.back(), index)) {
        lane_map_unsorted = true;
      }
      lanes.push_back(index);
    };
    for (; lane_map_runs < runs_done; lane_map_runs++) {
      const Run &run = runs[lane_map_runs];
      g_column_lane_map_work.fetch_add(run.count, std::memory_order_relaxed);
      for (size_t lane = 0; lane < run.count; lane++) {
        const uint32_t index = run.lane_index[lane];
        if (!info[index].moves_pending) {
          continue;
        }
        KX_GameObject *owner = trees[index]->GetGameObject();
        if (!run.program->moves_others) {
          add(owner, index);
          continue;
        }
        /* Each object the lane moves (plan C1-9 E2), once. */
        for (size_t w = 0; w < run.program->writes.size(); w++) {
          const Write &write = run.program->writes[w];
          if (write.kind != WriteKind::Transform || !write.is_move ||
              run.masks[w * run.count + lane] == 0)
          {
            continue;
          }
          add(WriteTarget(run, write, lane, owner), index);
        }
      }
    }
    if (lane_map_unsorted) {
      for (auto &entry : lane_of_object) {
        LaneList &lanes = entry.second;
        if (lanes.size() > 1) {
          std::sort(lanes.begin(), lanes.end(), [&](const uint32_t a, const uint32_t b) {
            return LaneBefore(a, b);
          });
        }
      }
      lane_map_unsorted = false;
    }
    return lane_of_object;
  }
  /** The object's first lane (in sort key order) whose moves are still pending, kNone. */
  uint32_t FirstPendingLane(const KX_GameObject *object) const
  {
    const auto &map = LaneMap();
    const auto found = map.find(object);
    if (found == map.end()) {
      return kNone;
    }
    for (const uint32_t lane : found->second) {
      if (LaneMovesObject(lane, object)) {
        return lane;
      }
    }
    return kNone;
  }
  /** The run lanes in sort key order, the flush's cursor in them. */
  std::vector<uint32_t> order;
  size_t order_next = 0;
  bool order_built = false;
  void BuildOrder()
  {
    if (order_built) {
      return;
    }
    /* Every lane whose run ran, in sort key order (the flush's order of their trees). */
    order.clear();
    for (size_t index = 0; index < info.size(); index++) {
      if (info[index].run != kNone) {
        order.push_back(uint32_t(index));
      }
    }
    const auto by_key = [&](const uint32_t a, const uint32_t b) {
      return info[a].sort_key < info[b].sort_key;
    };
    /* Usually already in order (trees are kept in scene order). */
    if (!std::is_sorted(order.begin(), order.end(), by_key)) {
      std::sort(order.begin(), order.end(), by_key);
    }
    order_next = 0;
    order_built = true;
  }
  /** RunLanes' scratch: the gates due per lane, the named objects whose moves it applied. */
  std::vector<uint64_t> lane_due;
  std::vector<uint8_t> object_flushed;
  /** The writes the flush's merge walks (ListWrites): keys, and (planned tree, write, the command
   * of a Captured write listed on its own: kNone = the whole write). */
  struct ListRef {
    uint32_t lane = 0;
    uint32_t write = 0;
    uint32_t command = kNone;
  };
  std::vector<LN_ExternalWriteKey> list_keys;
  std::vector<ListRef> list_refs;
  /** ListWrites: the objects the bridged ops' last-write-wins commands merge by (sorted): a lane
   * of one of them lists all its writes, which may share a key with such a command; and per
   * planned tree, whether one of its bridged ops recorded such a command. */
  std::vector<const void *> captured_objects;
  std::vector<uint8_t> lane_captured_lww;
  /** A lane's writes were applied by the flush this tick (CountWrites then walks the lanes). */
  bool any_lane_written = false;
  /** The pool the tick's runs used: PrepareApply's (plan C1-11 C-4). */
  LN_JobPool *flush_pool = nullptr;
  /** PrepareApply's scratch: objects other writes reach the properties of, the lanes to apply. */
  std::vector<const void *> pre_touched;
  std::vector<uint32_t> pre_lanes;
  /** A write of the lane's owner's game properties (Number, Modify), the ones PrepareApply takes. */
  static bool IsOwnPropertyWrite(const Write &write)
  {
    return (write.kind == WriteKind::Number || write.kind == WriteKind::Modify) &&
           write.runtime_target == kNone && write.target_column == kNone;
  }
  /** Applies lane `index`'s own property writes now when each changes an existing number property
   * in place (else nothing: the flush applies them). */
  bool PreApplyLane(uint32_t index);
  /** The number a Number write of NPC `lane` writes, the change a Modify write makes. */
  static LN_PropertyNumber NumberOf(const Run &run, const Write &write, const size_t lane)
  {
    LN_PropertyNumber number;
    number.type = write.number_type;
    if (write.number_type == LN_ValueType::Bool) {
      number.bool_value = run.bools[size_t(write.out_bool) * run.count + lane] != 0;
    }
    else if (write.number_type == LN_ValueType::Int) {
      number.int_value = run.ints[size_t(write.out_int) * run.count + lane];
    }
    else {
      number.float_value = run.floats[size_t(write.out_float) * run.count + lane];
    }
    return number;
  }
  static LN_PropertyModify ModifyOf(const Run &run, const Write &write, const size_t lane)
  {
    const auto kept_float = [&](const uint32_t column) {
      return run.floats[size_t(column) * run.count + lane];
    };
    LN_PropertyModify modify;
    modify.op = write.modify_op;
    modify.operand = kept_float(write.out_float);
    modify.clamped = write.clamped;
    if (write.clamped) {
      modify.min_value = kept_float(write.out_float + 1);
      modify.max_value = kept_float(write.out_float + 2);
    }
    return modify;
  }

  /** Whether lane `offset`'s write `w` stands for a last-write-wins command (its Transform's
   * command may depend on the NPC's Local input). Not for Captured writes. */
  static bool WriteIsLastWriteWins(const Run &run, const Write &write, const size_t offset)
  {
    if (write.kind == WriteKind::Motion) {
      return write.counts_last_write_wins;
    }
    if (write.kind == WriteKind::Transform && write.type_from_local) {
      const bool local = write.local != kNone ?
                             run.bools[size_t(write.out_bool) * run.count + offset] != 0 :
                             write.local_constant;
      return local ? write.local_last_write_wins : write.last_write_wins;
    }
    return write.last_write_wins;
  }

  /** CountWrites' numbers for the run, column by column (its masks are contiguous per write; the
   * superseded marks are all clear when a run ends). */
  void CountRun(Run &run) const
  {
    const LN_ColumnProgram &program = *run.program;
    const size_t count = run.count;
    uint32_t total = 0, last_write_wins = 0;
    for (size_t w = 0; w < program.writes.size(); w++) {
      const Write &write = program.writes[w];
      const uint8_t *mask = run.masks.data() + w * count;
      if (write.kind == WriteKind::Captured) {
        for (size_t lane = 0; lane < count; lane++) {
          if (mask[lane] == 0) {
            continue;
          }
          const size_t cell = w * count + lane;
          const Run::Capture &capture = run.captures[lane / kChunkLanes];
          for (uint32_t k = 0; k < run.captured_count[cell]; k++) {
            total++;
            last_write_wins += capture.buffer->CommandIsLastWriteWins(
                                   capture.refs[run.captured_begin[cell] + k]) ?
                                   1 :
                                   0;
          }
        }
        continue;
      }
      if (write.kind == WriteKind::Transform && write.type_from_local && write.local != kNone) {
        for (size_t lane = 0; lane < count; lane++) {
          if (mask[lane] != 0) {
            total++;
            last_write_wins += WriteIsLastWriteWins(run, write, lane) ? 1 : 0;
          }
        }
        continue;
      }
      uint32_t on = 0;
      for (size_t lane = 0; lane < count; lane++) {
        on += mask[lane] != 0 ? 1u : 0u;
      }
      total += on;
      last_write_wins += WriteIsLastWriteWins(run, write, 0) ? on : 0;
    }
    run.count_total = total;
    run.count_last_write_wins = last_write_wins;
    run.counts_valid = true;
  }

  /** Applies lane `index`'s writes in op order: its moves (`moves`) and the others (`others`);
   * `buffer`: the flush's (null for a read's early apply, which applies moves only, and only
   * those of the `count` objects `only`: the read's object and its parents; null = all). */
  void ApplyLane(uint32_t index,
                 bool moves,
                 bool others,
                 LN_CommandBuffer *buffer,
                 KX_GameObject *const *only = nullptr,
                 size_t only_count = 0);
  /** The object a write of a run's lane writes: the owner, or the object the NPC found. */
  static KX_GameObject *WriteTarget(const Run &run,
                                    const Write &write,
                                    const size_t lane,
                                    KX_GameObject *owner)
  {
    if (write.target_column != kNone) {
      return run.target_objects[size_t(write.target_column) * run.count + lane];
    }
    return write.runtime_target == kNone ?
               owner :
               run.runtime[size_t(write.runtime_target) * run.count + lane].object;
  }
  /** Whether lane `index` still has a move of `object` to apply. */
  bool LaneMovesObject(const uint32_t index, const KX_GameObject *object) const
  {
    const LaneInfo &lane_info = info[index];
    if (!lane_info.moves_pending) {
      return false;
    }
    const Run &run = runs[lane_info.run];
    KX_GameObject *owner = trees[index]->GetGameObject();
    if (!run.program->moves_others) {
      return object == owner;
    }
    for (size_t w = 0; w < run.program->writes.size(); w++) {
      const Write &write = run.program->writes[w];
      const size_t cell = w * run.count + lane_info.offset;
      if (write.kind == WriteKind::Transform && write.is_move && run.masks[cell] != 0 &&
          run.superseded[cell] == 0 && WriteTarget(run, write, lane_info.offset, owner) == object)
      {
        return true;
      }
    }
    return false;
  }

  /** `tree`: the tree whose program it is; it keeps the entry's address (entries are never
   * erased: an unordered_map's elements stay where they are). */
  ProgramEntry &Entry(const LN_Program &program, LN_RuntimeTree *tree = nullptr)
  {
    ProgramEntry *found = nullptr;
    if (tree != nullptr && tree->m_columnEntryRunner == runner_id &&
        tree->m_columnEntryProgramId == program.GetUniqueId())
    {
      found = static_cast<ProgramEntry *>(tree->m_columnEntry);
    }
    else {
      found = &programs[program.GetUniqueId()];
      if (tree != nullptr) {
        tree->m_columnEntryRunner = runner_id;
        tree->m_columnEntryProgramId = program.GetUniqueId();
        tree->m_columnEntry = found;
      }
    }
    ProgramEntry &entry = *found;
    const void *lean = program.GetLeanProgram().get();
    if (entry.lean != lean) {
      /* A new or grown program (the lean program is dropped when it grows). */
      entry = ProgramEntry();
      entry.lean = lean;
      for (const LN_ValueExpression &value : program.GetValueExpressions()) {
        if (value.kind == LN_ValueExpressionKind::Constant &&
            value.value.type == LN_ValueType::ObjectRef && value.value.exists &&
            !value.value.reference_name.empty())
        {
          entry.names.push_back(value.value.reference_name);
        }
      }
    }
    return entry;
  }
  /** `r_entry`: the program's entry. */
  const std::shared_ptr<const LN_ColumnProgram> &Columns(const LN_Program &program,
                                                         const LN_Event event,
                                                         LN_RuntimeTree *tree = nullptr,
                                                         ProgramEntry **r_entry = nullptr)
  {
    program.EnsureLeanProgram();
    ProgramEntry &entry = Entry(program, tree);
    if (r_entry != nullptr) {
      *r_entry = &entry;
    }
    const size_t slot = size_t(event);
    if (!entry.compiled[slot]) {
      entry.columns[slot] = LN_CompileColumnProgram(program, event, &entry.refusal[slot], true);
      entry.compiled[slot] = true;
      g_column_compiles.fetch_add(1, std::memory_order_relaxed);
      /* UPBGE_LN_COLUMNS_REPORT: each program's answer, with its listing when refused. */
      if (std::getenv("UPBGE_LN_COLUMNS_REPORT") != nullptr) {
        CM_Message("Logic Nodes columns: '"
                   << program.GetName() << "' on "
                   << (event == LN_Event::OnUpdate ? "frames" : "steps") << ": "
                   << (entry.columns[slot] != nullptr ?
                           std::string("lowers") +
                               (entry.columns[slot]->native_rays ? " (native rays)" : "") +
                               (entry.columns[slot]->main_thread_why.empty() ?
                                    std::string() :
                                    " (on the main thread: " +
                                        entry.columns[slot]->main_thread_why + ")") :
                           "refused, " + entry.refusal[slot]));
        if (entry.columns[slot] == nullptr && !program.GetInstructionHeaders(event).empty()) {
          CM_Message(LN_DumpProgramText(program));
        }
      }
      /* The compile may have made the lean program: keep the entry's key in step. */
      entry.lean = program.GetLeanProgram().get();
    }
    return entry.columns[slot];
  }

  /** Plan's texts (LastRefusal, LastOutsideNote), made only when what they say changed (plan C2
   * P1: a game where nothing lowers built them, with their allocations, at every tick). */
  enum class TextKind : uint8_t { NoLower, NoLane, Outside, NoRun };
  struct CachedText {
    uint64_t key[6] = {};
    size_t key_size = 0;
    bool made = false;
    std::string text;
  };
  CachedText texts[4];
  template<typename Make>
  const std::string &Text(const TextKind kind, std::initializer_list<uint64_t> key, Make &&make)
  {
    CachedText &cached = texts[size_t(kind)];
    const bool same = cached.made && cached.key_size == key.size() &&
                      std::equal(key.begin(), key.end(), cached.key);
    if (!same) {
      cached.key_size = std::min(key.size(), std::size(cached.key));
      std::copy_n(key.begin(), cached.key_size, cached.key);
      cached.text = make();
      cached.made = true;
    }
    return cached.text;
  }
  /** Why `program` does not lower on `event` (its name first), for the texts above; changes of it
   * renew `reason_version`. */
  uint64_t reason_version = 0;
  uint64_t reason_program_id = 0;
  const void *reason_lean = nullptr;
  LN_Event reason_event = LN_Event::OnInit;
  std::string reason_text;
  const std::string &FirstReason(const LN_Program *program, const LN_Event event)
  {
    static const std::string none;
    if (program == nullptr) {
      return none;
    }
    const void *lean = program->GetLeanProgram().get();
    if (program->GetUniqueId() != reason_program_id || lean != reason_lean ||
        event != reason_event)
    {
      reason_program_id = program->GetUniqueId();
      reason_lean = lean;
      reason_event = event;
      reason_text = Entry(*program).refusal[size_t(event)];
      if (reason_text.empty()) {
        reason_text = "it reads the tick's timing or gravity, which this tick has not";
      }
      if (!program->GetName().empty()) {
        reason_text = "'" + program->GetName() + "': " + reason_text;
      }
      reason_version++;
    }
    return reason_text;
  }

  /** Objects with the same trees (plan C1-9): `objects` objects of `trees` trees each from tree
   * `begin`, every tree a lane. */
  struct Group {
    size_t begin = 0;
    uint32_t trees = 0;
    uint32_t objects = 0;
  };
  std::vector<Group> groups;
  /** Per planned tree: its group + 1, 0 for none. */
  std::vector<uint32_t> group_of;
  /** Per planned tree: it lowers but is not its object's only tree (a group may take it). */
  std::vector<uint8_t> lane_shared;
  /** Per planned tree (plan C2 P2: every tree a lane): what its place needs. */
  enum LaneFlag : uint8_t {
    /** Its object has a parent: the run must not hold an ancestor of it before it, and a run
     * whose lanes depend on an object with pending moves is checked when it starts (RunSetBegin). */
    kLaneParented = 1,
    /** It runs alone, in its place: a menu copy (its names find its copy's objects), an object
     * a column program names (a later run may read it), a tree of an object with several trees
     * no group takes. */
    kLaneSingle = 2,
    /** Its run joins no wave (a wave's later runs start before its chunks run). */
    kLaneNoWave = 4,
    /** Its run's start checks the objects its lanes depend on (RunSetBegin). */
    kLaneSpecial = 8,
    /** Its own last-write-wins number writes may share a key with another lane of its object
     * (two trees writing one property): the flush's merge lists them all. */
    kLaneListAll = 16,
    /** On a run's first lane: some lane of the run joins no wave / needs the start's check. */
    kRunNoWave = 32,
    kRunSpecial = 64,
    /** A run starts here (RunEnd stops at it: ancestors, singles). */
    kRunStart = 128,
  };
  std::vector<uint8_t> lane_flags;
  /** Plan's scratch: the objects of the run it extends, made once a parented lane joins it. */
  std::unordered_set<const KX_GameObject *> plan_run_owners;
  /** RunSetBegin's scratch (plan C2 P2): the objects the run's lanes depend on (their own and
   * their ancestors) with how many lanes depend on each. */
  std::unordered_map<KX_GameObject *, uint32_t> lane_depends;
  /** The objects of this tick's lanes (made at the first run that finds objects while playing; a
   * flat set kept between ticks: no allocation per lane). */
  FlatPointerSet lane_owners;
  bool lane_owners_made = false;
  /** RunSet's scratch: the objects found while playing, runtime_objects x lanes. */
  std::vector<KX_GameObject *> runtime_found;
  /** RunSet's scratch (plan C1-10): per runtime object, found after the run for this lane. */
  std::vector<uint8_t> runtime_found_after;
  /** The merge keys of this tick's runs' property writes of objects found while playing (plan
   * C1-10): a lane's own write of one of them is listed for the flush's merge (ListWrites), as the
   * VM's command of it would meet the other one in the merge's walk. */
  struct WrittenKey {
    const void *object = nullptr;
    const void *tree = nullptr;
    uint32_t property_id = UINT32_MAX;
    uint32_t ref = UINT32_MAX;
    bool operator==(const WrittenKey &other) const
    {
      return object == other.object && tree == other.tree && property_id == other.property_id &&
             ref == other.ref;
    }
  };
  struct WrittenKeyHash {
    size_t operator()(const WrittenKey &key) const
    {
      size_t hash = std::hash<const void *>()(key.object);
      hash = hash * 31 + std::hash<const void *>()(key.tree);
      hash = hash * 31 + key.property_id;
      return hash * 31 + key.ref;
    }
  };
  /** A property write's merge key, as AppendSetGamePropertyNumberRef's command has it (key_of):
   * the object and the property's id, else the object, the tree and the ref. */
  static WrittenKey NumberWriteKey(const LN_RuntimeTree *tree,
                                   const void *object,
                                   const uint32_t property_ref)
  {
    WrittenKey key;
    key.object = object;
    const LN_GamePropertyId id = tree->GetGamePropertyId(property_ref);
    if (id.IsValid()) {
      key.property_id = id.index;
    }
    else {
      key.tree = tree;
      key.ref = property_ref;
    }
    return key;
  }
  std::unordered_set<WrittenKey, WrittenKeyHash> written_keys;
  /** written_keys' property ids (a lane's own write of another id is never looked up), and
   * whether some key has none (an object + tree + ref key). */
  std::vector<uint8_t> written_ids;
  bool written_tree_keys = false;
  void NoteWrittenKey(const WrittenKey &key)
  {
    written_keys.insert(key);
    if (key.property_id == UINT32_MAX) {
      written_tree_keys = true;
      return;
    }
    if (written_ids.size() <= key.property_id) {
      written_ids.resize(size_t(key.property_id) + 1, 0);
    }
    written_ids[key.property_id] = 1;
  }
  /** Whether another lane wrote the property of `object` an own write of `property_ref` writes. */
  bool WrittenByOthers(const LN_RuntimeTree *tree, const void *object, const uint32_t property_ref) const
  {
    const LN_GamePropertyId id = tree->GetGamePropertyId(property_ref);
    if (id.IsValid() ? (id.index >= written_ids.size() || written_ids[id.index] == 0) :
                       !written_tree_keys)
    {
      return false;
    }
    return written_keys.count(NumberWriteKey(tree, object, property_ref)) != 0;
  }
  void ClearWrittenKeys()
  {
    /* (Every note inserts a key: none, nothing to clear. A hash set's clear zeroes its buckets
     * even when empty, and runs twice a tick.) */
    if (written_keys.empty()) {
      return;
    }
    written_keys.clear();
    std::fill(written_ids.begin(), written_ids.end(), 0);
    written_tree_keys = false;
  }
  /** RunSet's scratch (plan C1-9 E2): the objects whose reads apply pending moves with their
   * parents, those the run's start applies some of. */
  std::unordered_set<KX_GameObject *> runtime_read_chains, runtime_applied_chains;
  /** Runs `lanes` (planned trees of one program, in their order) as one run. `lanes` is RunLanes'
   * scratch (plan C2 P1: a run allocates nothing once the game has warmed up); RunSet may change
   * it. */
  void RunSet(std::vector<uint32_t> &lanes,
              LN_CommandBuffer &buffer,
              const LN_TickContext &context,
              LN_JobPool *pool);
  /** Plan C2 P6: lane `index`'s tree takes the column pass (ExecuteReady) in its place, on
   * the main thread: a lane a run gives back. */
  void RunDirectLane(size_t index, LN_CommandBuffer &buffer, const LN_TickContext &context);
  /** RunSet in three parts (plan C2 P1's waves: the starts of several short runs in order, their
   * chunks together on the pool, their ends in order): the run's start on this thread (the
   * run's index, kNoRun when its lanes were given back), its chunks, its end. */
  static constexpr size_t kNoRun = SIZE_MAX;
  /** RunSetBegin: the run must run lane by lane, each lane a run of its own (plan C2 P2). */
  static constexpr size_t kSplitRun = SIZE_MAX - 1;
  /** RunSetBegin, plan C2 fix 1: lane `split_at` of a run that starts only with nothing pending
   * (bridged or quiet) has pending moves (another tree pushed its object): the lanes before it
   * run, it records on the VM in its place, the lanes after it run (each part checked again).
   * Before, one pushed NPC gave its whole run back to the VM. */
  static constexpr size_t kSplitAt = SIZE_MAX - 2;
  size_t split_at = 0;
  /** UPBGE_LN_COLUMN_SPLIT_PUSHED=0: a pushed lane gives its whole run back (before fix 1). */
  bool split_pushed_lanes = [] {
    const char *value = std::getenv("UPBGE_LN_COLUMN_SPLIT_PUSHED");
    return value == nullptr || (std::strcmp(value, "0") != 0 && std::strcmp(value, "off") != 0);
  }();
  /** The end of the run starting at planned tree `begin` (consecutive lanes of its program). */
  size_t RunEnd(size_t begin) const;
  /** Whether a program's runs may join a wave (RunLanes). */
  static bool WaveEligible(const LN_ColumnProgram &program);
  void RunWave(size_t begin,
               size_t end,
               LN_CommandBuffer &buffer,
               const LN_TickContext &context,
               LN_JobPool &pool);
  /** RunWave's runs (by index) and tasks (ranges of them). */
  std::vector<size_t> wave_runs;
  std::vector<std::pair<size_t, size_t>> wave_tasks;
  /** The fewest lanes a wave takes (0: no waves). UPBGE_LN_COLUMN_WAVES=0 turns them off,
   * UPBGE_LN_COLUMN_WAVE_MIN sets it (default 64, as tree batches; tests set it). */
  size_t min_wave_lanes = [] {
    const char *off = std::getenv("UPBGE_LN_COLUMN_WAVES");
    if (off != nullptr && (std::strcmp(off, "0") == 0 || std::strcmp(off, "off") == 0)) {
      return size_t(0);
    }
    const char *value = std::getenv("UPBGE_LN_COLUMN_WAVE_MIN");
    const int lanes = value != nullptr ? std::atoi(value) : 0;
    return lanes >= 1 ? size_t(lanes) : size_t(64);
  }();
  size_t RunSetBegin(std::vector<uint32_t> &lanes,
                     LN_CommandBuffer &buffer,
                     const LN_TickContext &context);
  /** Plan C2 P2, for a run with a parented lane or a named one (kRunSpecial): 0 when its start
   * may apply what it applies, kSplitRun when it must run lane by lane, kNoRun when its one lane
   * must record on the VM (where the VM's reads apply the pending moves its lane depends on). */
  size_t CheckPendingDepends(const std::vector<uint32_t> &lanes,
                             LN_CommandBuffer &buffer,
                             const LN_TickContext &context,
                             const LN_ColumnProgram &program);
  void RunSetChunks(size_t run_index, LN_CommandBuffer &buffer, LN_JobPool *pool);
  void RunSetEnd(size_t run_index, LN_CommandBuffer &buffer, const LN_TickContext &context);
  std::vector<uint32_t> set_lanes;
  void RunChunk(const LN_ColumnProgram &program, Run &run, size_t first, size_t n);
  /** Part of a chunk's work: its pass's start (kPartBegin: BeginColumnPass, the capture's start,
   * the owners' positions), nodes [node_begin, node_end), its pass's end (kPartEnd) and the copy
   * of its writes; `s` the chunk's arrays, kept between the parts (plan C1-9's segments). */
  static constexpr uint8_t kPartBegin = 1, kPartEnd = 2;
  void RunChunkPart(const LN_ColumnProgram &program,
                    Run &run,
                    size_t first,
                    size_t n,
                    ChunkScratch &s,
                    size_t node_begin,
                    size_t node_end,
                    uint8_t parts);
  /** The chunks' arrays of a segmented run, one a chunk. */
  std::vector<std::unique_ptr<ChunkScratch>> chunk_scratches;
};

LN_ColumnRunner::LN_ColumnRunner() : m_impl(std::make_unique<Impl>()) {}

/* Never touches the trees: they may be gone (EndTick ends every tick). */
LN_ColumnRunner::~LN_ColumnRunner() = default;

void LN_ColumnRunner::SetOverlayTest(std::function<bool(KX_GameObject *)> is_overlay_copy,
                                     std::function<bool()> any_overlay_copy)
{
  m_impl->is_overlay_copy = std::move(is_overlay_copy);
  m_impl->any_overlay_copy = std::move(any_overlay_copy);
}

void LN_ColumnRunner::SetMinRunLanes(const size_t lanes)
{
  m_impl->min_run_lanes = std::max<size_t>(1, lanes);
  m_impl->size_floors = false; /* asked for: the size floors stay out of it */
}

void LN_ColumnRunner::PrepareProgram(const LN_Program &program, LN_RuntimeTree *tree)
{
  for (const LN_Event event : {LN_Event::OnUpdate, LN_Event::OnFixedUpdate}) {
    if (!program.GetInstructionHeaders(event).empty()) {
      m_impl->Columns(program, event, tree);
    }
  }
}

std::string LN_ColumnRefusalHint(const std::string &reason)
{
  const auto has = [&](const char *part) { return reason.find(part) != std::string::npos; };
  /* (Since plan C2 fix 1 a live pose read next to a distance, a distance read on some routes
   * only and a velocity next to a distance start the run quietly instead of refusing it.) */
  if (has("first read after a move of it")) {
    return "Read Get Distance before the nodes that move this object in the same step.";
  }
  if (has("inside another stateful node")) {
    return "A Boolean Edge, On Value Changed or Store Value reads a value no column node makes: "
           "work it out into a Compare first, or keep it in a property.";
  }
  if (has("AddObject") || has("Add Object")) {
    return "Add Object at this object after it moved or turned in the same step: add the copy "
           "before the move, on the other route of a Branch, or from another object.";
  }
  if (has("UI event route")) {
    return "UI event routes run alone: keep UI logic in a tree of its own.";
  }
  return std::string();
}

std::vector<std::string> LN_NodesKeepingTreesAwake(const LN_Program &program)
{
  std::vector<std::string> awake;
  if (program.GetStepGates().empty()) {
    return awake;
  }
  const LN_ExecBlockProgram &blocks = program.GetExecBlockProgram(LN_Event::OnFixedUpdate);
  if (blocks.blocks.empty() || blocks.all_blocks_step_gated) {
    return awake;
  }
  const std::vector<LN_SourceRef> &refs = program.GetSourceRefs();
  for (const LN_ExecBlock &block : blocks.blocks) {
    if (block.step_gate_index != LN_INVALID_INDEX || block.source_ref_index >= refs.size()) {
      continue;
    }
    const std::string &node = refs[block.source_ref_index].node_name;
    if (!node.empty() && std::find(awake.begin(), awake.end(), node) == awake.end()) {
      awake.push_back(node);
    }
  }
  return awake;
}

LN_ColumnVerdict LN_ColumnRunner::Verdict(const LN_Program &program, const LN_Event event)
{
  LN_ColumnVerdict verdict;
  if (program.GetInstructionHeaders(event).empty()) {
    return verdict;
  }
  Impl::ProgramEntry *entry = nullptr;
  const std::shared_ptr<const LN_ColumnProgram> &columns = m_impl->Columns(
      program, event, nullptr, &entry);
  if (columns == nullptr || columns->direct) {
    verdict.kind = LN_ColumnVerdict::Kind::Pass;
    verdict.reason = columns != nullptr ? columns->direct_why :
                                          entry->refusal[size_t(event)];
    return verdict;
  }
  verdict.kind = LN_ColumnVerdict::Kind::Wide;
  verdict.main_thread = !columns->main_thread_why.empty();
  verdict.reason = columns->main_thread_why;
  return verdict;
}

uint64_t LN_ColumnRunner::CompiledCountForTests()
{
  return g_column_compiles.load(std::memory_order_relaxed);
}

uint64_t LN_ColumnRunner::LaneMapWorkForTests()
{
  return g_column_lane_map_work.load(std::memory_order_relaxed);
}

uint64_t LN_ColumnRunner::WaveCountForTests()
{
  return g_column_waves.load(std::memory_order_relaxed);
}

void LN_ColumnRunner::SetMinWaveLanes(const size_t lanes)
{
  m_impl->min_wave_lanes = lanes;
}

void LN_ColumnRunner::SetPlanHelpers(const size_t helpers)
{
  m_impl->plan_helpers = helpers;
}

const std::string &LN_ColumnRunner::LastRefusal() const
{
  return m_impl->refusal;
}

const std::string &LN_ColumnRunner::LastOutsideNote() const
{
  return m_impl->outside_note;
}

size_t LN_ColumnRunner::GivenBackCount() const
{
  return m_impl->given_back;
}

bool LN_ColumnRunner::IsLane(const size_t index) const
{
  return index < m_impl->lane.size() && m_impl->lane[index] != 0;
}

size_t LN_ColumnRunner::LaneCount() const
{
  return m_impl->lane_count;
}

/** Whether `min` consecutive objects have the same trees (two or more each, the same programs in
 * the same order): a group could form (plan C1-9). */
static bool GroupPossible(LN_RuntimeTree *const *trees, const size_t count, const size_t min)
{
  std::vector<const LN_Program *> previous, current;
  size_t same = 0;
  size_t index = 0;
  while (index < count) {
    const KX_GameObject *owner = trees[index] != nullptr ? trees[index]->GetGameObject() : nullptr;
    size_t end = index + 1;
    while (end < count && trees[end] != nullptr && trees[end]->GetGameObject() == owner) {
      end++;
    }
    current.clear();
    for (size_t k = index; k < end; k++) {
      current.push_back(trees[k] != nullptr ? trees[k]->GetProgramPointer() : nullptr);
    }
    if (owner != nullptr && current.size() >= 2 && current == previous) {
      if (++same >= min) {
        return true;
      }
    }
    else {
      same = owner != nullptr && current.size() >= 2 ? 1 : 0;
    }
    previous.swap(current);
    index = end;
  }
  return false;
}

bool LN_ColumnRunner::Plan(LN_RuntimeTree *const *trees,
                           const size_t count,
                           const LN_Event event,
                           LN_CommandBuffer &buffer,
                           const std::unordered_set<LN_RuntimeTree *> *forced_trees,
                           const LN_TickReadContext *read)
{
  Impl &d = *m_impl;
  EndTick();
  d.refusal.clear();
  d.outside_note.clear();
  d.given_back = 0;
  d.groups.clear();
  d.group_of.clear();
  d.lane_owners_made = false; /* (made again, Clear first, when a run asks for it) */
  d.ClearWrittenKeys();
  d.plan_serial++;
  /* The shortest run: 1 in the All mode (plan C2 P0: every tree whose program lowers). */
  const size_t min_run_lanes = LN_GetColumnMode() == LN_ColumnMode::All ? size_t(1) :
                                                                          d.min_run_lanes;
  if (event == LN_Event::OnInit) {
    d.refusal = "On Init";
    return false;
  }
  /* UPBGE_LN_COLUMNS_REPORT (plan C2 P0's report): how many trees each program has, the first
   * tick a program is seen on this event (the program's own answer is logged when it lowers). */
  static const bool report = std::getenv("UPBGE_LN_COLUMNS_REPORT") != nullptr;
  if (report) {
    static std::unordered_set<std::string> reported;
    std::map<std::string, size_t> trees_of;
    for (size_t index = 0; index < count; index++) {
      const LN_Program *program = trees[index] != nullptr ? trees[index]->GetProgramPointer() :
                                                            nullptr;
      if (program != nullptr) {
        trees_of[program->GetName()]++;
      }
    }
    const char *on = event == LN_Event::OnUpdate ? "frames" : "steps";
    for (const auto &entry : trees_of) {
      if (reported.insert(entry.first + " on " + on).second) {
        CM_Message("Logic Nodes columns trees: '" << entry.first << "' on " << on << ": "
                                                  << entry.second << " trees");
      }
    }
  }
  /* The size floors (plan C2 P1's D3 gate: a small program's run needs a few trees in a row).
   * Only with helpers: the trees they leave out are worth it only where column passes run them
   * as tree batches; on one thread a run of 1 of the smallest brain tied with the old system
   * (232 vs 233 ns a tree) and a tree left out makes the flush list the lanes' writes (1,000
   * trees of 10 programs in turn: 395 ns a tree, 470 with the floors). Not in the All mode
   * either (it runs every tree that lowers). */
  const bool floors = d.size_floors && LN_GetColumnMode() != LN_ColumnMode::All &&
                      d.plan_helpers > 0;
  const auto shortest = [&](const LN_ColumnProgram &program) {
    return floors ? std::max(min_run_lanes, Impl::RunFloor(program)) : min_run_lanes;
  };
  /* A run needs `min_run_lanes` consecutive trees of one program, more for a small program whose
   * answer its tree's entry already holds (programs are lowered when their trees register; a
   * program that never lowers makes no run): without one (trees of several programs in turn,
   * objects with two trees, a game with no crowd) nothing below can make a lane. Plan C2 P1:
   * 1,000 trees of 10 small programs in turn planned every tree at every tick to find none. */
  {
    const size_t slot = size_t(event);
    Impl::NoRunAnswer &answer = d.no_run_answer[slot];
    const uint64_t programs = LN_ProgramsChangedSerial();
    if (answer.valid && answer.programs == programs && answer.helpers == d.plan_helpers &&
        answer.min_run == min_run_lanes && answer.floors == floors &&
        answer.trees.size() == count && std::equal(trees, trees + count, answer.trees.begin()))
    {
      d.refusal = answer.refusal;
      return false;
    }
    answer.valid = false;
    const auto need_of = [&](const LN_Program &program, const LN_RuntimeTree &tree) -> size_t {
      if (tree.m_columnEntryRunner != d.runner_id ||
          tree.m_columnEntryProgramId != program.GetUniqueId())
      {
        return min_run_lanes;
      }
      const Impl::ProgramEntry &entry = *static_cast<const Impl::ProgramEntry *>(
          tree.m_columnEntry);
      if (!entry.compiled[slot] || entry.lean != program.GetLeanProgram().get()) {
        return min_run_lanes; /* not lowered yet, or the program grew */
      }
      const LN_ColumnProgram *columns = entry.columns[slot].get();
      if (columns == nullptr || columns->direct) {
        return SIZE_MAX; /* (a direct program: its trees take the column pass, unplanned) */
      }
      /* (Trees in a row take a small program's short runs only when they average
       * LN_COLUMN_TAKE_ALL_AVERAGE_NODES, i.e. with a larger program's run among them, whose
       * floor is 1.) */
      return shortest(*columns);
    };
    bool possible = false, any_floor = false;
    size_t current = 0, need = SIZE_MAX;
    /* The program of the most refused trees in a row (the text says why it does not lower). */
    const LN_Program *previous = nullptr, *refused_program = nullptr;
    size_t refused_trees = 0;
    for (size_t index = 0; index < count && !possible; index++) {
      const LN_Program *program = trees[index] != nullptr ? trees[index]->GetProgramPointer() :
                                                            nullptr;
      if (program == nullptr) {
        previous = nullptr;
        continue;
      }
      if (program != previous) {
        previous = program;
        current = 0;
        need = need_of(*program, *trees[index]);
        any_floor = any_floor || (need > min_run_lanes && need != SIZE_MAX);
      }
      current++;
      possible = current >= need;
      if (need == SIZE_MAX && current > refused_trees) {
        refused_trees = current;
        refused_program = program;
      }
    }
    if (!possible && !GroupPossible(trees, count, min_run_lanes)) {
      /* (Made only when it changed: a game that never lowers asks every tick.) */
      const std::string &reason = d.FirstReason(refused_program, event);
      d.refusal = d.Text(
          Impl::TextKind::NoRun,
          {uint64_t(min_run_lanes),
           uint64_t(any_floor),
           refused_program != nullptr ? refused_program->GetUniqueId() : 0,
           uint64_t(event),
           d.reason_version},
          [&]() {
            return "no run of " + std::to_string(min_run_lanes) +
                   (any_floor ? " trees (more for small programs)" : " trees") +
                   " of one program, nor of objects with the same trees" +
                   (reason.empty() ? std::string() :
                                     "; the most trees that do not lower: " + reason);
          });
      answer.valid = true;
      answer.programs = programs;
      answer.helpers = d.plan_helpers;
      answer.min_run = min_run_lanes;
      answer.floors = floors;
      answer.trees.assign(trees, trees + count);
      answer.refusal = d.refusal;
      return false;
    }
  }
  /* The manager fills its tick read context after the plan, before the runs. */
  d.dirty = true;
  d.read = read;
  d.trees.assign(trees, trees + count);
  d.lane.assign(count, 0);
  d.lane_program.assign(count, nullptr);

  /* The programs that lower; the objects they name (with their parents) run no lane: a run reads
   * them, and a lane's moves would move them inside the run. They are looked up only when some
   * tree is still a lane after the other checks (`named_programs`; plan C2 P1: with the shortest
   * run 1 every tick plans, and the FPS demo, where no tree can be a lane, looked its names up at
   * every tick). */
  if (!d.barriers.empty()) {
    d.barriers.clear();
  }
  d.named_programs.clear();
  const LN_Program *last_program = nullptr;
  const LN_ColumnProgram *last_columns = nullptr;
  bool any = false;
  /* Why the program of the most trees that does not lower does not (the log says it; its text
   * is made after the loop, from the program of the most refused trees in a row). */
  const LN_Program *first_reason_program = nullptr;
  size_t first_reason_trees = 0, refused_trees = 0;
  const LN_Program *refused_program = nullptr;
  const auto note_refused = [&](const LN_Program *program, const size_t trees) {
    if (program == nullptr || trees <= first_reason_trees) {
      return;
    }
    first_reason_trees = trees;
    first_reason_program = program;
  };
  /* The trees that may run in columns: nothing ExecuteReady adds to their event, no Run Logic
   * Tree pass this tick, an object of their own no column program names. */
  /* The most common reason a tree of a lowered program is no lane (the log says it). */
  static constexpr const char *kNotLaneWhy[] = {
      "not active or On Init not run",
      "paused to UI routes",
      "a Run Logic Tree pass",
      "no owner object",
      "a menu copy", /* (plan C2 P2: runs alone) */
      "an object a column program names", /* (plan C2 P2: runs alone) */
      "a relative move of a dynamic body on frames",
      "a collision read of a sensor queried on demand",
      "a group whose trees change what another one finds", /* (plan C2 P2: they run alone) */
  };
  size_t not_lane[std::size(kNotLaneWhy)] = {};
  /* Trees of a lowered program that pass the checks before the named objects' (in the same loop
   * as the programs: plan C2 P1, one walk of the trees fewer). */
  size_t candidates = 0;
  std::vector<uint8_t> &flags = d.lane_flags;
  flags.assign(count, 0);
  Impl::ProgramEntry *last_entry = nullptr;
  /* Menu copies only when the scene has any (one question, not one per lane). */
  const bool overlay_test = d.is_overlay_copy && (!d.any_overlay_copy || d.any_overlay_copy());
  for (size_t index = 0; index < count; index++) {
    LN_RuntimeTree *tree = trees[index];
    const LN_Program *program = tree != nullptr ? tree->GetProgramPointer() : nullptr;
    if (program == nullptr) {
      continue;
    }
    if (program != last_program) {
      last_program = program;
      Impl::ProgramEntry *entry = nullptr;
      const std::shared_ptr<const LN_ColumnProgram> &columns = d.Columns(
          *program, event, tree, &entry);
      last_entry = entry;
      last_columns = columns.get();
      if (last_columns != nullptr && last_columns->direct) {
        /* Plan C2 P6: a direct program makes no lane; its trees take the column pass where the
         * manager records them, its tree batches too. */
        last_columns = nullptr;
      }
      note_refused(refused_program, refused_trees);
      refused_program = last_columns == nullptr ? program : nullptr;
      refused_trees = 0;
      if (last_columns != nullptr && last_columns->needs_timing && read == nullptr) {
        last_columns = nullptr; /* no tick read context: the timing a snapshot reads is unknown */
      }
      if (last_columns != nullptr && last_columns->needs_gravity &&
          (read == nullptr || !read->has_gravity))
      {
        last_columns = nullptr; /* the gravity a snapshot reads is the context's */
      }
      /* Once a tick (objects with trees of several programs switch often): kept alive for the
       * tick, and its named objects. */
      if (last_columns != nullptr) {
        uint64_t &stamp = entry->plan_stamp[size_t(event)];
        if (stamp != d.plan_serial) {
          stamp = d.plan_serial;
          d.kept.push_back(columns);
          if (!entry->names.empty()) {
            d.named_programs.emplace_back(entry, tree);
          }
        }
      }
    }
    if (last_columns == nullptr) {
      refused_trees++;
      continue;
    }
    any = true;
    KX_GameObject *owner = tree->m_gameObject;
    int why = -1;
    if (!tree->m_runtimeActive || !tree->m_enabled || !tree->m_initExecuted) {
      why = 0;
    }
    else if (tree->m_uiEventRoutesOnly) {
      why = 1;
    }
    else if (tree->m_stepGatesForced || (forced_trees != nullptr && forced_trees->count(tree) != 0))
    {
      why = 2;
    }
    else if (owner == nullptr) {
      why = 3;
    }
    /* (Plan C2 P6-P8: these trees take the column pass where the manager records them: On Init
     * first in their first tick, the UI routes while paused, a Run Logic Tree pass through
     * ExecuteForcedUpdate.) */
    if (why >= 0) {
      not_lane[why]++;
      continue;
    }
    d.lane_program[index] = last_columns;
    candidates++;
    /* Plan C2 P2: every tree a lane. An owner with a parent: no run holds an ancestor of it
     * before it (Plan's runs), it joins no wave, and its run's start checks what its lanes
     * depend on (RunSetBegin: an ancestor's pending moves applied at the start would move it
     * before its reads where the VM's read applies them later). */
    if (owner->GetParent() != nullptr) {
      flags[index] |= Impl::kLaneParented | Impl::kLaneNoWave | Impl::kLaneSpecial;
    }
    /* A menu copy: its names find its own copy's objects (OverlayCounterpart), so it runs alone
     * (a run's named objects are its first tree's) and the objects it names are barriers too. */
    if (overlay_test && d.is_overlay_copy(owner)) {
      flags[index] |= Impl::kLaneSingle | Impl::kLaneNoWave;
      if (last_entry != nullptr && !last_entry->names.empty()) {
        d.named_programs.emplace_back(last_entry, tree);
      }
    }
  }
  note_refused(refused_program, refused_trees);
  /* (Made only when it changed: most ticks give the same text.) */
  const std::string &first_reason = d.FirstReason(first_reason_program, event);
  if (!any) {
    d.refusal = d.Text(Impl::TextKind::NoLower,
                       {first_reason_program != nullptr ? first_reason_program->GetUniqueId() : 0,
                        uint64_t(event),
                        d.reason_version},
                       [&]() {
                         return std::string("no program lowers to columns on ") +
                                (event == LN_Event::OnUpdate ? "frames" : "steps") +
                                (first_reason.empty() ? std::string() :
                                                        " (" + first_reason + ")");
                       });
    EndTick();
    return false;
  }

  /* The named objects, found only when some tree may still be a lane ... */
  if (candidates != 0) {
    for (const auto &[entry, tree] : d.named_programs) {
      for (const std::string &name : entry->names) {
        for (KX_GameObject *object = tree->FindSceneObjectByName(name, false); object != nullptr;
             object = object->GetParent())
        {
          if (!d.barriers.insert(object).second) {
            break;
          }
        }
      }
    }
  }
  /* ... and the checks after them. Trees that are not their object's only one (`shared`) run in
   * a group (plan C1-9) or, since plan C2 P2, alone in their place. */
  std::vector<uint8_t> &shared = d.lane_shared;
  shared.assign(count, 0);
  bool any_shared = false;
  for (size_t index = 0; index < count && candidates != 0; index++) {
    const LN_ColumnProgram *columns = d.lane_program[index];
    if (columns == nullptr) {
      continue;
    }
    KX_GameObject *owner = trees[index]->m_gameObject;
    const bool on_demand = columns->collision_sources && owner->GetPhysicsController() != nullptr &&
                           owner->GetPhysicsController()->LogicCollisionsQueriedOnDemand();
    int why = -1;
    if (event == LN_Event::OnUpdate && columns->relative_moves && owner->IsDynamic()) {
      /* The VM refuses Apply Movement / Rotation of a dynamic body on frames (a warning, no
       * command, no stamp): a column write would move it. */
      why = 6;
    }
    else if (on_demand && columns->applies_moves) {
      /* Plan C1-10: its contacts are collected at its first read after a change (memoized until a
       * pending move is applied); a run applies its program's pending moves when it starts, the
       * VM where it reads them, so a read between them would see other contacts. */
      why = 7;
    }
    if (why >= 0) {
      /* (Plan C2 P6: it takes the column pass where the manager records it, whose handlers
       * refuse such a move and read such a sensor in the tree's own order.) */
      not_lane[why]++;
      d.lane_program[index] = nullptr;
      continue;
    }
    /* Plan C2 P2: an object a column program names (or a parent of one) runs alone, in its place,
     * in no wave: a later run that reads it applies its moves at its start (the lane map), and its
     * own run's start checks the named objects' chains (RunSetBegin). */
    if (!d.barriers.empty() && d.barriers.find(owner) != d.barriers.end()) {
      flags[index] |= Impl::kLaneSingle | Impl::kLaneNoWave | Impl::kLaneSpecial;
    }
    /* A sensor queried on demand whose program applies no pending move: every read of the run
     * sees one cache generation, as the VM's reads; collision reads stay on this thread (RunSet's
     * segments), so no wave. */
    if (on_demand) {
      flags[index] |= Impl::kLaneNoWave;
    }
    if (!trees[index]->m_batchNote.sole_tree) {
      shared[index] = 1;
      any_shared = true;
    }
  }
  /* Groups (plan C1-9): consecutive objects whose trees are all lanes of the same programs in the
   * same order, each object's trees one block of the tick's trees (none elsewhere). */
  if (any_shared) {
    std::unordered_map<const KX_GameObject *, uint32_t> trees_of;
    for (size_t k = 0; k < count; k++) {
      if (trees[k] != nullptr) {
        trees_of[trees[k]->m_gameObject]++;
      }
    }
    /* The end of the block of `at`'s object, or `at` itself when it is no group candidate. */
    const auto block_end = [&](const size_t at) {
      const KX_GameObject *owner = trees[at] != nullptr ? trees[at]->m_gameObject : nullptr;
      size_t stop = at;
      while (stop < count && trees[stop] != nullptr && trees[stop]->m_gameObject == owner) {
        /* (Plan C2 P2: a parented object's trees and those that run alone take no group: a group
         * reorders its objects' trees, and their reads depend on others' moves.) */
        if (shared[stop] == 0 || d.lane_program[stop] == nullptr ||
            (flags[stop] & (Impl::kLaneSingle | Impl::kLaneParented)) != 0)
        {
          return at;
        }
        stop++;
      }
      if (owner == nullptr || stop - at < 2 || trees_of[owner] != stop - at) {
        return at;
      }
      return stop;
    };
    size_t at = 0;
    while (at < count) {
      const size_t first_end = block_end(at);
      if (first_end == at) {
        at++;
        continue;
      }
      const size_t per_object = first_end - at;
      size_t objects = 1;
      size_t next = first_end;
      while (next < count && block_end(next) == next + per_object) {
        bool same = true;
        for (size_t k = 0; k < per_object && same; k++) {
          same = d.lane_program[next + k] == d.lane_program[at + k];
        }
        if (!same) {
          break;
        }
        objects++;
        next += per_object;
      }
      size_t group_shortest = min_run_lanes;
      for (size_t k = 0; k < per_object; k++) {
        group_shortest = std::max(group_shortest, shortest(*d.lane_program[at + k]));
      }
      if (objects < group_shortest) {
        at = first_end;
        continue;
      }
      {
        /* Plan C1-10: a group runs an object's trees after the other objects' earlier trees, so
         * no tree may change what another one finds: copies Add Object makes while its tree
         * records (seen by name lookups), moves a read applies (a collision read sees their
         * dropped contacts). Those objects' trees run alone in their places (plan C2 P2; on the
         * VM before). */
        bool finds = false, collides = false, adds = false, applies = false;
        for (size_t k = 0; k < per_object; k++) {
          const LN_ColumnProgram *slot = d.lane_program[at + k];
          finds = finds || !slot->runtime_objects.empty();
          collides = collides || slot->collision_sources;
          adds = adds || slot->adds_objects;
          applies = applies || slot->applies_moves;
        }
        if ((finds && adds) || (collides && applies)) {
          at = next;
          continue;
        }
      }
      Impl::Group group;
      group.begin = at;
      group.trees = uint32_t(per_object);
      group.objects = uint32_t(objects);
      d.groups.push_back(group);
      d.group_of.resize(count, 0);
      for (size_t k = at; k < next; k++) {
        d.group_of[k] = uint32_t(d.groups.size());
        d.lane[k] = 1;
        trees[k]->m_columnLane = !d.lane_program[k]->bridged;
        d.lane_count++;
      }
      at = next;
    }
    /* Plan C2 P2: a tree of an object with several trees that no group took runs alone, in its
     * place, in no wave (a wave's later run of its object would start before its moves are
     * known). Its reads of its own object apply its object's earlier trees' moves (the lane map)
     * where the VM's read applies them. */
    for (size_t k = 0; k < count; k++) {
      if (shared[k] != 0 && d.lane_program[k] != nullptr &&
          (k >= d.group_of.size() || d.group_of[k] == 0))
      {
        flags[k] |= Impl::kLaneSingle | Impl::kLaneNoWave;
      }
    }
  }
  /* Runs: consecutive trees of one program; short ones record one tree after the other. A run
   * takes its program's floor, except among runs in a row (no tree between them) whose trees
   * average LN_COLUMN_TAKE_ALL_AVERAGE_NODES column nodes or more: those are all taken. A small
   * program's tree left out there cuts a wave apart and makes the flush list the lanes' writes,
   * and too few such trees are in a row for a tree batch (1,000 trees of 10 programs in turn, the
   * smallest one's trees left out: 0.40 -> 0.69 ms). Smaller trees take the floors and run as
   * tree batches (all tiny trees in turn: 0.29 ms in waves, 0.24 on the old system). Plan C2 P2:
   * a tree that runs alone is a run of its own, and a run ends before a lane one of whose
   * ancestors is an earlier lane's object of the run (that lane's moves are worked out with the
   * run's, after its start, where the VM's read of the later lane applies them). */
  const auto in_group = [&](const size_t k) {
    return k < d.group_of.size() && d.group_of[k] != 0;
  };
  std::vector<Impl::PlanRun> &plan_runs = d.plan_runs;
  plan_runs.clear();
  for (size_t index = candidates != 0 ? 0 : count; index < count;) {
    if (d.lane_program[index] == nullptr || in_group(index)) {
      index++;
      continue;
    }
    Impl::PlanRun run;
    run.begin = index;
    run.flags = flags[index];
    size_t end = index + 1;
    if ((flags[index] & Impl::kLaneSingle) == 0) {
      bool owners_made = false;
      while (end < count && d.lane_program[end] == d.lane_program[index] && !in_group(end) &&
             (flags[end] & Impl::kLaneSingle) == 0)
      {
        if ((flags[end] & Impl::kLaneParented) != 0) {
          if (!owners_made) {
            d.plan_run_owners.clear();
            for (size_t k = index; k < end; k++) {
              d.plan_run_owners.insert(trees[k]->m_gameObject);
            }
            owners_made = true;
          }
          bool ancestor = false;
          for (KX_GameObject *at = trees[end]->m_gameObject->GetParent();
               at != nullptr && !ancestor;
               at = at->GetParent())
          {
            ancestor = d.plan_run_owners.count(at) != 0;
          }
          if (ancestor) {
            break;
          }
        }
        if (owners_made) {
          d.plan_run_owners.insert(trees[end]->m_gameObject);
        }
        run.flags |= flags[end];
        end++;
      }
    }
    run.end = end;
    plan_runs.push_back(run);
    index = end;
  }
  const auto take = [&](const Impl::PlanRun &run) {
    for (size_t k = run.begin; k < run.end; k++) {
      d.lane[k] = 1;
      /* A bridged program's lanes capture their whole snapshot (the VM's code reads it). */
      trees[k]->m_columnLane = !d.lane_program[k]->bridged;
      d.lane_count++;
    }
    /* Its first lane says what the run needs (RunLanes, RunEnd, RunSetBegin). */
    flags[run.begin] |= Impl::kRunStart;
    if ((run.flags & Impl::kLaneNoWave) != 0) {
      flags[run.begin] |= Impl::kRunNoWave;
    }
    if ((run.flags & Impl::kLaneSpecial) != 0) {
      flags[run.begin] |= Impl::kRunSpecial;
    }
  };
  for (size_t r = 0; r < plan_runs.size();) {
    /* The runs in a row from `r` (adjacent: no tree between them). */
    size_t last = r + 1;
    size_t row_lanes = plan_runs[r].end - plan_runs[r].begin;
    size_t row_nodes = row_lanes * LN_ColumnNodeCount(*d.lane_program[plan_runs[r].begin]);
    while (floors && last < plan_runs.size() && plan_runs[last].begin == plan_runs[last - 1].end)
    {
      const size_t lanes = plan_runs[last].end - plan_runs[last].begin;
      row_lanes += lanes;
      row_nodes += lanes * LN_ColumnNodeCount(*d.lane_program[plan_runs[last].begin]);
      last++;
    }
    const bool all = floors && row_nodes >= row_lanes * LN_COLUMN_TAKE_ALL_AVERAGE_NODES;
    for (; r < last; r++) {
      const Impl::PlanRun &run = plan_runs[r];
      if (all || run.end - run.begin >= shortest(*d.lane_program[run.begin])) {
        take(run);
      }
    }
  }
  /* Plan C2 P2: lanes of one object whose own last-write-wins writes share a merge key (two trees
   * of an object writing "hp", or both setting its world scale): the flush's merge must see both,
   * as it sees the VM's two recorded commands (the later one leaves the earlier out: a property
   * both create in one step gets the later one's type, a pose is set once). Groups (the same
   * programs for every object) are asked once. */
  d.list_all_lanes = 0;
  if (any_shared) {
    const auto overlap = [](const LN_ColumnProgram &a, const LN_ColumnProgram &b) {
      const std::vector<std::string> &x = a.own_last_write_wins_keys;
      const std::vector<std::string> &y = b.own_last_write_wins_keys;
      for (size_t i = 0, j = 0; i < x.size() && j < y.size();) {
        const int order = x[i].compare(y[j]);
        if (order == 0) {
          return true;
        }
        order < 0 ? i++ : j++;
      }
      return false;
    };
    for (const Impl::Group &group : d.groups) {
      bool any = false;
      for (uint32_t a = 0; a < group.trees && !any; a++) {
        for (uint32_t b = a + 1; b < group.trees && !any; b++) {
          any = overlap(*d.lane_program[group.begin + a], *d.lane_program[group.begin + b]);
        }
      }
      if (any) {
        const size_t end = group.begin + size_t(group.trees) * group.objects;
        for (size_t k = group.begin; k < end; k++) {
          flags[k] |= Impl::kLaneListAll;
        }
        d.list_all_lanes += end - group.begin;
      }
    }
    std::unordered_map<const KX_GameObject *, std::vector<uint32_t>> singles;
    for (size_t k = 0; k < count; k++) {
      if (d.lane[k] != 0 && shared[k] != 0 && !in_group(k)) {
        singles[trees[k]->m_gameObject].push_back(uint32_t(k));
      }
    }
    for (const auto &[object, lanes] : singles) {
      for (size_t a = 0; a < lanes.size(); a++) {
        for (size_t b = a + 1; b < lanes.size(); b++) {
          if (overlap(*d.lane_program[lanes[a]], *d.lane_program[lanes[b]])) {
            for (const uint32_t k : {lanes[a], lanes[b]}) {
              if ((flags[k] & Impl::kLaneListAll) == 0) {
                flags[k] |= Impl::kLaneListAll;
                d.list_all_lanes++;
              }
            }
          }
        }
      }
    }
  }
  if (d.lane_count == 0) {
    size_t most = 0;
    for (size_t k = 1; k < std::size(kNotLaneWhy); k++) {
      most = not_lane[k] > not_lane[most] ? k : most;
    }
    /* Lanes, but no run long enough. */
    size_t lanes = 0, longest = 0, current = 0, on_shared = 0;
    if (not_lane[most] == 0) {
      for (size_t k = 0; k < count; k++) {
        if (d.lane_program[k] == nullptr) {
          current = 0;
          continue;
        }
        lanes++;
        current = k > 0 && d.lane_program[k - 1] == d.lane_program[k] ? current + 1 : 1;
        longest = std::max(longest, current);
      }
      on_shared = size_t(std::count(shared.begin(), shared.end(), uint8_t(1)));
    }
    d.refusal = d.Text(
        Impl::TextKind::NoLane,
        {uint64_t(most) << 48 | uint64_t(not_lane[most]),
         uint64_t(lanes) << 32 | uint64_t(count),
         uint64_t(longest) << 32 | uint64_t(on_shared),
         first_reason_program != nullptr ? first_reason_program->GetUniqueId() : 0,
         uint64_t(event),
         d.reason_version},
        [&]() {
          std::string text = "no run of trees that lower to columns";
          if (not_lane[most] != 0) {
            text += std::string(" (") + std::to_string(not_lane[most]) +
                    " trees: " + kNotLaneWhy[most] + ")";
          }
          else if (lanes != 0) {
            /* (Trees on objects with several trees run only in groups, plan C1-9.) */
            text += " (" + std::to_string(lanes) + " lanes of " + std::to_string(count) +
                    " trees, at most " + std::to_string(longest) + " in a row" +
                    (on_shared != 0 ? ", " + std::to_string(on_shared) +
                                          " on objects with several trees and no group" :
                                      std::string()) +
                    ")";
          }
          if (!first_reason.empty()) {
            text += "; the most trees that do not lower: " + first_reason;
          }
          return text;
        });
    EndTick();
    return false;
  }
  /* The trees whose program does not lower (the log names the most of one program). */
  if (!first_reason.empty()) {
    d.outside_note = d.Text(Impl::TextKind::Outside,
                            {first_reason_program->GetUniqueId(),
                             uint64_t(first_reason_trees),
                             uint64_t(event),
                             d.reason_version},
                            [&]() {
                              return first_reason + " (" + std::to_string(first_reason_trees) +
                                     " trees)";
                            });
  }
  /* What the lanes' recorded commands would carry: the tree's sort key (LN_ExecFrame::SortKey). */
  d.info.assign(count, Impl::LaneInfo());
  for (size_t k = 0; k < count; k++) {
    if (d.lane[k] != 0) {
      d.info[k].sort_key = (uint64_t(trees[k]->GetSceneObjectIndex()) << 32) |
                           uint64_t(trees[k]->GetAppliedTreeIndex());
    }
  }
  d.buffer = &buffer;
  buffer.SetExternalWrites(this);
  return true;
}

void LN_ColumnRunner::CaptureLanes(const uint64_t tick_index, LN_JobPool *pool)
{
  Impl &d = *m_impl;
  const size_t count = d.trees.size();
  const auto capture_range = [&](const size_t begin, const size_t end) {
    for (size_t index = begin; index < end; index++) {
      if (d.lane[index] != 0 && !d.lane_program[index]->bridged) {
        d.trees[index]->CaptureColumnProperties(tick_index,
                                                d.lane_program[index]->needs_owner_transform);
      }
    }
  };
  const size_t helpers = pool != nullptr ? pool->GetHelperCount() : 0;
  if (helpers > 0 && d.lane_count >= 256) {
    const size_t chunk_count = std::clamp<size_t>((helpers + 1) * 4, 1, count / 64 + 1);
    pool->Run(chunk_count, [&](const size_t chunk, uint32_t /*thread*/) {
      capture_range(count * chunk / chunk_count, count * (chunk + 1) / chunk_count);
    });
  }
  else {
    capture_range(0, count);
  }
}

void LN_ColumnRunner::Impl::RunChunk(const LN_ColumnProgram &program,
                                     Run &run,
                                     const size_t first,
                                     const size_t n)
{
  RunChunkPart(
      program, run, first, n, chunk_scratch(), 0, program.nodes.size(), kPartBegin | kPartEnd);
}

void LN_ColumnRunner::Impl::RunChunkPart(const LN_ColumnProgram &program,
                                         Run &run,
                                         const size_t first,
                                         const size_t n,
                                         ChunkScratch &s,
                                         const size_t node_begin,
                                         const size_t node_end,
                                         const uint8_t parts)
{
  const size_t lanes = kChunkLanes;
  if (s.floats.size() < size_t(program.float_slots) * lanes) {
    s.floats.resize(size_t(program.float_slots) * lanes);
    s.constants_serial = 0;
  }
  if (s.ints.size() < size_t(program.int_slots) * lanes) {
    s.ints.resize(size_t(program.int_slots) * lanes);
    s.constants_serial = 0;
  }
  if (s.bools.size() < size_t(program.bool_slots) * lanes) {
    s.bools.resize(size_t(program.bool_slots) * lanes);
    s.constants_serial = 0;
  }
  if (s.rots.size() < size_t(program.rot_slots) * lanes) {
    s.rots.resize(size_t(program.rot_slots) * lanes);
    s.constants_serial = 0;
  }
  float *const F = s.floats.data();
  int32_t *const I = s.ints.data();
  uint8_t *const B = s.bools.data();
  const auto f = [&](const uint32_t slot) { return F + size_t(slot) * lanes; };
  const auto i = [&](const uint32_t slot) { return I + size_t(slot) * lanes; };
  const auto b = [&](const uint32_t slot) { return B + size_t(slot) * lanes; };
  LN_Rotation *const R = s.rots.data();
  const auto r = [&](const uint32_t slot) { return R + size_t(slot) * lanes; };

  /* The constants of the lanes this chunk uses (plan C2 P1: a run of 1 among trees of other
   * programs filled all kChunkLanes lanes of every constant at each switch). */
  if (s.constants_serial != program.serial || s.constants_lanes < n) {
    for (const Node &node : program.constants) {
      switch (node.op) {
        case NodeOp::FConst:
          std::fill_n(f(node.out), n, node.float_value);
          break;
        case NodeOp::IConst:
          std::fill_n(i(node.out), n, node.int_value);
          break;
        case NodeOp::BConst:
          std::fill_n(b(node.out), n, uint8_t(node.bool_value ? 1 : 0));
          break;
        case NodeOp::RConst:
          std::fill_n(r(node.out), n, node.rotation);
          break;
        default:
          break;
      }
    }
    s.constants_serial = program.serial;
    s.constants_lanes = n;
  }

  const LN_TickContext &context = *run.context;
  LN_RuntimeTree *const *trees = run.lane_trees.data() + first;
  /* The bridge's buffer for this chunk (this thread owns it while it records). */
  Run::Capture *capture = program.bridged ? &run.captures[first / kChunkLanes] : nullptr;
  if ((parts & kPartBegin) != 0) {
    /* What ExecuteReady / ExecuteEvent do before a pass's ops, each tree in its pass. */
    for (size_t l = 0; l < n; l++) {
      s.serial[l] = trees[l]->BeginColumnPass(context);
    }
    if (capture != nullptr) {
      capture->buffer->BeginChunkRecording();
    }
  }
  else if (capture != nullptr) {
    capture->buffer->TakeChunkOwnership();
  }
  /* A route's context for the VM's code, NPC by NPC (ScopedStepGateTime's rule). */
  const auto route_context = [&](LN_TickContext &route, const size_t l, const uint8_t gate_plus_one) {
    route.execution_scope_serial = s.serial[l];
    route.command_buffer = capture != nullptr ? capture->buffer.get() : nullptr;
    if (gate_plus_one != 0) {
      const uint32_t gate = uint32_t(gate_plus_one) - 1;
      LN_RuntimeTree &tree = *trees[l];
      route.delta_time = context.delta_time;
      route.unscaled_dt = context.unscaled_dt;
      if (gate < tree.m_stepGates.size()) {
        route.delta_time = tree.m_stepGates[gate].time_passed;
        route.unscaled_dt = tree.m_stepGates[gate].unscaled_time_passed;
      }
      const std::vector<LN_StepGate> &gates = tree.m_program->GetStepGates();
      route.phase_tick_stride = gate < gates.size() ? gates[gate].interval :
                                                      context.phase_tick_stride;
    }
  };
  if (program.needs_owner_position && (parts & kPartBegin) != 0) {
    for (size_t l = 0; l < n; l++) {
      const MT_Vector3 &p = trees[l]->GetGameObject()->NodeGetWorldPosition();
      s.px[l] = p.x();
      s.py[l] = p.y();
      s.pz[l] = p.z();
      s.owner_ok[l] = finite3(p) ? 1 : 0;
    }
  }

  const NamedObject *objects = run.objects.data();
  const auto vector3 = [&](const Node &node, const size_t l, const MT_Vector3 &v) {
    f(node.out)[l] = v.x();
    f(node.out2)[l] = v.y();
    f(node.out3)[l] = v.z();
  };
  const auto fallback3 = [&](const Node &node) {
    return MT_Vector3(f(node.position_b.x)[0], f(node.position_b.y)[0], f(node.position_b.z)[0]);
  };
  const auto axis_of = [](const MT_Matrix3x3 &orientation, const int32_t axis) {
    /* VectorKind's AxisVector. */
    MT_Vector3 vector = axis == 0 || axis == 3 ? orientation * MT_Vector3(1.0f, 0.0f, 0.0f) :
                        axis == 1 || axis == 4 ? orientation * MT_Vector3(0.0f, 1.0f, 0.0f) :
                                                 orientation * MT_Vector3(0.0f, 0.0f, 1.0f);
    if (axis >= 3) {
      vector = -vector;
    }
    return vector;
  };

#define LN_COLUMNS_UNARY(EXPR) \
  { \
    const float *x = f(node.a); \
    float *out = f(node.out); \
    for (size_t l = 0; l < n; l++) { \
      const float v = x[l]; \
      out[l] = (EXPR); \
    } \
    break; \
  }
#define LN_COLUMNS_BINARY(EXPR) \
  { \
    const float *xa = f(node.a), *xb = f(node.b); \
    float *out = f(node.out); \
    for (size_t l = 0; l < n; l++) { \
      const float a = xa[l]; \
      const float b2 = xb[l]; \
      out[l] = (EXPR); \
    } \
    break; \
  }

  for (size_t node_index = node_begin; node_index < node_end; node_index++) {
    const Node &node = program.nodes[node_index];
    switch (node.op) {
      case NodeOp::FProp: {
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          LN_Snapshot::GamePropertyView view;
          out[l] = trees[l]->GetSnapshot().ReadGameProperty(node.property_ref, view) ?
                       view.float_value :
                       node.float_value;
        }
        break;
      }
      case NodeOp::IProp: {
        int32_t *out = i(node.out);
        for (size_t l = 0; l < n; l++) {
          LN_Snapshot::GamePropertyView view;
          out[l] = trees[l]->GetSnapshot().ReadGameProperty(node.property_ref, view) ?
                       view.int_value :
                       node.int_value;
        }
        break;
      }
      case NodeOp::BProp: {
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          LN_Snapshot::GamePropertyView view;
          out[l] = (trees[l]->GetSnapshot().ReadGameProperty(node.property_ref, view) ?
                        view.bool_value :
                        node.bool_value) ?
                       1 :
                       0;
        }
        break;
      }
      case NodeOp::FTreeProp: {
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          const LN_Value *value = trees[l]->GetTreePropertyValue(node.property_ref);
          out[l] = value != nullptr ? value->float_value : 0.0f;
        }
        break;
      }
      case NodeOp::ITreeProp: {
        int32_t *out = i(node.out);
        for (size_t l = 0; l < n; l++) {
          const LN_Value *value = trees[l]->GetTreePropertyValue(node.property_ref);
          out[l] = value != nullptr ? value->int_value : 0;
        }
        break;
      }
      case NodeOp::BTreeProp: {
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          const LN_Value *value = trees[l]->GetTreePropertyValue(node.property_ref);
          out[l] = value != nullptr && value->bool_value ? 1 : 0;
        }
        break;
      }
      case NodeOp::FAdd:
        LN_COLUMNS_BINARY(a + b2)
      case NodeOp::FSub:
        LN_COLUMNS_BINARY(a - b2)
      case NodeOp::FMul:
        LN_COLUMNS_BINARY(a * b2)
      case NodeOp::FDiv:
        LN_COLUMNS_BINARY(std::fabs(b2) <= 1.0e-20f ? 0.0f : a / b2)
      case NodeOp::FSafeDiv:
        LN_COLUMNS_BINARY(b2 == 0.0f ? 0.0f : a / b2)
      case NodeOp::FMin:
        LN_COLUMNS_BINARY(std::min(a, b2))
      case NodeOp::FMax:
        LN_COLUMNS_BINARY(std::max(a, b2))
      case NodeOp::FPow:
        LN_COLUMNS_BINARY(std::pow(a, b2))
      case NodeOp::FMod:
        LN_COLUMNS_BINARY(std::fabs(b2) <= 1.0e-20f ? 0.0f : std::fmod(a, b2))
      case NodeOp::FAbs:
        LN_COLUMNS_UNARY(std::fabs(v))
      case NodeOp::FNeg:
        LN_COLUMNS_UNARY(-v)
      case NodeOp::FSign:
        LN_COLUMNS_UNARY(float((v > 0.0f) - (v < 0.0f)))
      case NodeOp::FRound:
        LN_COLUMNS_UNARY(std::floor(v + 0.5f))
      case NodeOp::FFloor:
        LN_COLUMNS_UNARY(std::floor(v))
      case NodeOp::FCeil:
        LN_COLUMNS_UNARY(std::ceil(v))
      case NodeOp::FTrunc:
        LN_COLUMNS_UNARY(std::trunc(v))
      case NodeOp::FFract:
        LN_COLUMNS_UNARY(v - std::floor(v))
      case NodeOp::FSin:
        LN_COLUMNS_UNARY(std::sin(v))
      case NodeOp::FCos:
        LN_COLUMNS_UNARY(std::cos(v))
      case NodeOp::FRad:
        LN_COLUMNS_UNARY(v * (kPi / 180.0f))
      case NodeOp::FDeg:
        LN_COLUMNS_UNARY(v * (180.0f / kPi))
      case NodeOp::FClamp: {
        const float *x = f(node.a), *lo = f(node.b), *hi = f(node.c);
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          const float lower = std::min(lo[l], hi[l]);
          const float upper = std::max(lo[l], hi[l]);
          out[l] = std::min(std::max(x[l], lower), upper);
        }
        break;
      }
      case NodeOp::FThreshold: {
        const float *value = f(node.a), *threshold = f(node.b);
        const uint8_t *else_zero = b(node.c);
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          switch (LN_ThresholdOperation(node.compare)) {
            case LN_ThresholdOperation::Greater:
              out[l] = (value[l] > threshold[l]) ? value[l] :
                                                   (else_zero[l] != 0 ? 0.0f : threshold[l]);
              break;
            case LN_ThresholdOperation::Less:
              out[l] = (value[l] < threshold[l]) ? value[l] :
                                                   (else_zero[l] != 0 ? 0.0f : threshold[l]);
              break;
            default:
              out[l] = node.float_value;
              break;
          }
        }
        break;
      }
      case NodeOp::FRanged: {
        const float *value = f(node.a), *lo = f(node.b), *hi = f(node.c);
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          switch (LN_RangeOperation(node.compare)) {
            case LN_RangeOperation::Inside:
              out[l] = (lo[l] < value[l] && value[l] < hi[l]) ? value[l] : 0.0f;
              break;
            case LN_RangeOperation::Outside:
              out[l] = (value[l] < lo[l] || value[l] > hi[l]) ? value[l] : 0.0f;
              break;
            default:
              out[l] = node.float_value;
              break;
          }
        }
        break;
      }
      case NodeOp::FSelect: {
        const uint8_t *condition = b(node.c);
        const float *x = f(node.a), *y = f(node.b);
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = condition[l] != 0 ? x[l] : y[l];
        }
        break;
      }
      case NodeOp::ISelect: {
        const uint8_t *condition = b(node.c);
        const int32_t *x = i(node.a), *y = i(node.b);
        int32_t *out = i(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = condition[l] != 0 ? x[l] : y[l];
        }
        break;
      }
      case NodeOp::BSelect: {
        const uint8_t *condition = b(node.c);
        const uint8_t *x = b(node.a), *y = b(node.b);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = condition[l] != 0 ? x[l] : y[l];
        }
        break;
      }
      case NodeOp::FUniform:
        std::fill_n(f(node.out), n, run.uniforms[node.compare]);
        break;
      case NodeOp::FRandom: {
        /* FloatKind's Random: one hash of the expression and the tick for every NPC. */
        const float unit = LN_RuntimeTree::ColumnHashToUnitFloat(
            (uint64_t(node.property_ref) << 32) ^ context.tick_index);
        const float *lo = f(node.a), *hi = f(node.b);
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          const float lower = std::min(lo[l], hi[l]);
          const float upper = std::max(lo[l], hi[l]);
          out[l] = lower + (upper - lower) * unit;
        }
        break;
      }
      case NodeOp::IRandom: {
        const float unit = LN_RuntimeTree::ColumnHashToUnitFloat(
            (uint64_t(node.property_ref) << 32) ^ context.tick_index);
        const int32_t *lo = i(node.a), *hi = i(node.b);
        int32_t *out = i(node.out);
        for (size_t l = 0; l < n; l++) {
          const int32_t lower = std::min(lo[l], hi[l]);
          const int32_t upper = std::max(lo[l], hi[l]);
          out[l] = lower + int32_t(unit * float(upper - lower + 1));
        }
        break;
      }
      case NodeOp::FFromInt: {
        const int32_t *x = i(node.a);
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = float(x[l]);
        }
        break;
      }
      case NodeOp::FFromBool: {
        const uint8_t *x = b(node.a);
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = x[l] != 0 ? 1.0f : 0.0f;
        }
        break;
      }
      case NodeOp::IFromFloat: {
        const float *x = f(node.a);
        int32_t *out = i(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = LN_ConvertFloatToInt(x[l], LN_FloatToIntMode(node.compare));
        }
        break;
      }
      case NodeOp::IFromBool: {
        const uint8_t *x = b(node.a);
        int32_t *out = i(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = x[l] != 0 ? 1 : 0;
        }
        break;
      }
      case NodeOp::BFromInt: {
        const int32_t *x = i(node.a);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = x[l] != 0 ? 1 : 0;
        }
        break;
      }
      case NodeOp::BFromFloat: {
        const float *x = f(node.a);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = x[l] != 0.0f ? 1 : 0;
        }
        break;
      }
      case NodeOp::BFromVector: {
        const float *x = f(node.a), *y = f(node.b), *z = f(node.c);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = MT_Vector3(x[l], y[l], z[l]).length2() > 0.0f ? 1 : 0;
        }
        break;
      }
      case NodeOp::BIntEq: {
        const int32_t *x = i(node.a);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = x[l] == node.int_value ? 1 : 0;
        }
        break;
      }
      case NodeOp::VNormalize: {
        const float *x = f(node.a), *y = f(node.b), *z = f(node.c);
        for (size_t l = 0; l < n; l++) {
          /* VectorKind's Normalize. */
          const MT_Vector3 vector(x[l], y[l], z[l]);
          const float length = vector.length();
          vector3(node,
                  l,
                  length <= 1.0e-20f ? MT_Vector3(0.0f, 0.0f, 0.0f) : vector * (1.0f / length));
        }
        break;
      }
      case NodeOp::VAttr: {
        const LN_ObjectAttributeId attribute = LN_ObjectAttributeId(node.compare);
        if (node.end_a == EndKind::Owner) {
          for (size_t l = 0; l < n; l++) {
            MT_Vector3 vector = fallback3(node);
            if (!LN_RuntimeTree::ColumnObjectAttributeVector(
                    *trees[l]->GetGameObject(), attribute, vector))
            {
              vector = fallback3(node);
            }
            vector3(node, l, vector);
          }
        }
        else if (node.end_a == EndKind::Runtime) {
          const NamedObject *found = run.runtime.data() + size_t(node.object_a) * run.count + first;
          for (size_t l = 0; l < n; l++) {
            MT_Vector3 vector = fallback3(node);
            if (found[l].object == nullptr ||
                !LN_RuntimeTree::ColumnObjectAttributeVector(*found[l].object, attribute, vector))
            {
              vector = fallback3(node);
            }
            vector3(node, l, vector);
          }
        }
        else {
          KX_GameObject *object = objects[node.object_a].object;
          MT_Vector3 vector = fallback3(node);
          if (object == nullptr ||
              !LN_RuntimeTree::ColumnObjectAttributeVector(*object, attribute, vector))
          {
            vector = fallback3(node);
          }
          for (size_t l = 0; l < n; l++) {
            vector3(node, l, vector);
          }
        }
        break;
      }
      case NodeOp::VAvg: {
        const MT_Vector3 &center = run.averages[node.property_ref].vector_average;
        for (size_t l = 0; l < n; l++) {
          vector3(node, l, center);
        }
        break;
      }
      case NodeOp::IAvgCount: {
        const int32_t count_value = run.averages[node.property_ref].vector_count;
        int32_t *out = i(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = count_value;
        }
        break;
      }
      case NodeOp::BAvgValid: {
        const uint8_t valid = run.averages[node.property_ref].valid ? 1 : 0;
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = valid;
        }
        break;
      }
      case NodeOp::FRunInv: {
        std::fill_n(f(node.out), n, run.invariants[node.property_ref].f);
        break;
      }
      case NodeOp::IRunInv: {
        std::fill_n(i(node.out), n, run.invariants[node.property_ref].i);
        break;
      }
      case NodeOp::BRunInv: {
        std::fill_n(b(node.out), n, run.invariants[node.property_ref].b);
        break;
      }
      case NodeOp::VVel: {
        const size_t at = size_t(node.property_ref) * run.count + first;
        const MT_Vector3 *values = run.velocities.data() + at;
        const uint8_t *ok = run.velocity_ok.data() + at;
        for (size_t l = 0; l < n; l++) {
          vector3(node, l, ok[l] != 0 ? values[l] : fallback3(node));
        }
        break;
      }
      case NodeOp::VAxis: {
        if (node.end_a == EndKind::Owner) {
          for (size_t l = 0; l < n; l++) {
            const MT_Matrix3x3 orientation =
                trees[l]->GetSnapshot().GetObjectSnapshot().world_orientation.ToMatrix();
            vector3(node, l, axis_of(orientation, node.int_value));
          }
        }
        else if (node.end_a == EndKind::Runtime) {
          /* Plan C1-11: each NPC's found object, its live pose. */
          const NamedObject *found = run.runtime.data() + size_t(node.object_a) * run.count + first;
          for (size_t l = 0; l < n; l++) {
            vector3(node,
                    l,
                    found[l].object == nullptr ?
                        fallback3(node) :
                        axis_of(found[l].object->NodeGetWorldOrientation(), node.int_value));
          }
        }
        else {
          KX_GameObject *object = objects[node.object_a].object;
          const MT_Vector3 vector = object == nullptr ?
                                        fallback3(node) :
                                        axis_of(object->NodeGetWorldOrientation(), node.int_value);
          for (size_t l = 0; l < n; l++) {
            vector3(node, l, vector);
          }
        }
        break;
      }
      case NodeOp::OTreeObj: {
        /* ResolveCommandTarget / ResolveObjectExpression of the tree property where the op runs:
         * the object its value holds (plan C1-11). */
        KX_GameObject **out = run.target_objects.data() + size_t(node.int_value) * run.count +
                              first;
        for (size_t l = 0; l < n; l++) {
          const LN_Value *value = trees[l]->GetTreePropertyValue(node.property_ref);
          out[l] = value != nullptr ? trees[l]->ColumnObjectOfValue(*value) : nullptr;
        }
        break;
      }
      case NodeOp::VTreeProp: {
        /* VectorKind's tree property read, or FromGenericValue's ValueAsVector of its value
         * (plan C1-11), from the value as this place of the pass has it. */
        for (size_t l = 0; l < n; l++) {
          const LN_Value *value = trees[l]->GetTreePropertyValue(node.property_ref);
          MT_Vector3 vector = fallback3(node);
          if (value != nullptr) {
            if (node.compare == 0) {
              vector = value->type == LN_ValueType::Rotation ? value->RotationEulerRadians() :
                                                               value->vector_value;
            }
            else if (!LN_RuntimeTree::ColumnValueAsVector(*value, vector)) {
              vector = fallback3(node);
            }
          }
          vector3(node, l, vector);
        }
        break;
      }
      case NodeOp::VTreeConvert: {
        /* in.Value of the tree property (the stored value, or the pass's last write), then the
         * kind's conversion of it. */
        const int32_t *last = i(node.d);
        const float *fields_f = f(node.a);
        const int32_t *fields_i = i(node.b);
        const uint8_t *fields_b = b(node.c);
        const LN_FloatToIntMode mode = LN_FloatToIntMode(node.int_value);
        for (size_t l = 0; l < n; l++) {
          const LN_Value *value = nullptr;
          float as_float = 0.0f;
          bool float_ok = false;
          bool as_bool = false;
          const LN_ColumnProgram::TreeWrite *last_write =
              last[l] < 0 ? nullptr : &program.tree_write_ops[size_t(last[l])];
          if (last_write == nullptr || last_write->constant ||
              last_write->type == LN_ValueType::Vector)
          {
            /* The stored value (a vector write's too: STreeWrite set it before this read). */
            value = last_write == nullptr || !last_write->constant ?
                        trees[l]->GetTreePropertyValue(node.property_ref) :
                        &last_write->value;
            if (value != nullptr) {
              float_ok = LN_RuntimeTree::ColumnValueAsFloat(*value, as_float);
              as_bool = LN_RuntimeTree::ColumnValueAsBool(*value);
            }
          }
          else {
            /* A fresh value of the write's type: the conversions of that type. */
            float_ok = true;
            switch (program.tree_write_ops[size_t(last[l])].type) {
              case LN_ValueType::Float:
                as_float = fields_f[l];
                as_bool = fields_f[l] != 0.0f;
                break;
              case LN_ValueType::Int:
                as_float = float(fields_i[l]);
                as_bool = fields_i[l] != 0;
                break;
              default:
                as_float = fields_b[l] != 0 ? 1.0f : 0.0f;
                as_bool = fields_b[l] != 0;
                break;
            }
          }
          switch (node.compare) {
            case 0:
              f(node.out)[l] = float_ok ? as_float : node.float_value;
              break;
            case 1:
              i(node.out)[l] = float_ok ? LN_ConvertFloatToInt(as_float, mode) : 0;
              break;
            default:
              b(node.out)[l] = as_bool ? 1 : 0;
              break;
          }
        }
        break;
      }
      case NodeOp::VRotate: {
        /* VectorKind's RotateAroundAxis (modes 0-2). */
        for (size_t l = 0; l < n; l++) {
          const MT_Vector3 origin(
              f(node.position_a.x)[l], f(node.position_a.y)[l], f(node.position_a.z)[l]);
          const MT_Vector3 pivot(
              f(node.position_b.x)[l], f(node.position_b.y)[l], f(node.position_b.z)[l]);
          MT_Vector3 axis(0.0f, 0.0f, 1.0f);
          if (node.compare == 1) {
            axis = node.int_value == 0 ? MT_Vector3(1.0f, 0.0f, 0.0f) :
                   node.int_value == 1 ? MT_Vector3(0.0f, 1.0f, 0.0f) :
                                         MT_Vector3(0.0f, 0.0f, 1.0f);
          }
          else if (node.compare == 2) {
            axis = MT_Vector3(f(node.vec_c.x)[l], f(node.vec_c.y)[l], f(node.vec_c.z)[l]);
          }
          const float axis_length = axis.length();
          if (axis_length <= 1.0e-20f) {
            vector3(node, l, origin);
            continue;
          }
          axis *= 1.0f / axis_length;
          const float angle = f(node.a)[l];
          const MT_Vector3 relative = origin - pivot;
          const float cosine = std::cos(angle);
          const float sine = std::sin(angle);
          vector3(node,
                  l,
                  pivot + relative * cosine + axis.cross(relative) * sine +
                      axis * (axis.dot(relative) * (1.0f - cosine)));
        }
        break;
      }
      case NodeOp::VRandom: {
        /* VectorKind's Random: one hash per axis for the expression and the tick. */
        const uint64_t seed = (uint64_t(node.property_ref) << 32) ^ context.tick_index;
        const float hx = LN_RuntimeTree::ColumnHashToUnitFloat(seed ^ 0x13579u);
        const float hy = LN_RuntimeTree::ColumnHashToUnitFloat(seed ^ 0x24680u);
        const float hz = LN_RuntimeTree::ColumnHashToUnitFloat(seed ^ 0xabcdefu);
        for (size_t l = 0; l < n; l++) {
          vector3(node,
                  l,
                  MT_Vector3(f(node.position_a.x)[l] != 0.0f ? hx : 0.0f,
                             f(node.position_a.y)[l] != 0.0f ? hy : 0.0f,
                             f(node.position_a.z)[l] != 0.0f ? hz : 0.0f));
        }
        break;
      }
      case NodeOp::BExists: {
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          LN_Snapshot::GamePropertyView view;
          out[l] = trees[l]->GetSnapshot().ReadGameProperty(node.property_ref, view) && view.exists ?
                       1 :
                       0;
        }
        break;
      }
      case NodeOp::SEdge: {
        /* BoolKind's BooleanEdge, where the VM evaluates it. */
        const uint8_t *eval = node.d == kNone ? nullptr : b(node.d);
        const uint8_t *condition = b(node.a);
        uint8_t *out = b(node.out);
        const uint64_t tick = context.tick_index;
        for (size_t l = 0; l < n; l++) {
          if (eval != nullptr && eval[l] == 0) {
            out[l] = 0;
            continue;
          }
          LN_RuntimeTree::BoolExpressionRuntimeState &state = trees[l]->BoolExpressionState(
              node.property_ref);
          if (tick != UINT64_MAX && state.last_update_tick == tick) {
            out[l] = state.cached_result ? 1 : 0;
            continue;
          }
          const bool current = condition[l] != 0;
          const bool previous = state.initialized ? state.previous_bool : false;
          state.cached_result = current && !previous;
          state.active = !current && previous;
          state.previous_bool = current;
          state.initialized = true;
          state.last_update_tick = tick;
          out[l] = state.cached_result ? 1 : 0;
        }
        break;
      }
      case NodeOp::SValueChanged: {
        /* BoolKind's ValueChanged where the VM evaluates it, on the value its FromBool /
         * FromInt / FromFloat makes (ValueChangeStep: one code). */
        const uint8_t *eval = node.d == kNone ? nullptr : b(node.d);
        uint8_t *out = b(node.out);
        const uint64_t tick = context.tick_index;
        for (size_t l = 0; l < n; l++) {
          if (eval != nullptr && eval[l] == 0) {
            out[l] = 0;
            continue;
          }
          LN_RuntimeTree &tree = *trees[l];
          LN_RuntimeTree::BoolExpressionRuntimeState &state = tree.BoolExpressionState(
              node.property_ref);
          if (state.last_update_tick == tick) {
            out[l] = state.cached_result ? 1 : 0;
            continue;
          }
          LN_Value value;
          value.exists = true;
          switch (node.compare) {
            case 0:
              value.type = LN_ValueType::Bool;
              value.bool_value = b(node.a)[l] != 0;
              break;
            case 1:
              value.type = LN_ValueType::Int;
              value.int_value = i(node.a)[l];
              break;
            default:
              value.type = LN_ValueType::Float;
              value.float_value = f(node.a)[l];
              break;
          }
          bool changed = false;
          if (!tree.ValueChangeStep(node.property_ref, node.bool_value, value, tick, changed)) {
            out[l] = 0;
            continue;
          }
          state.cached_result = changed;
          state.last_update_tick = tick;
          out[l] = changed ? 1 : 0;
        }
        break;
      }
      case NodeOp::SEdgeFalling: {
        const uint8_t *eval = node.d == kNone ? nullptr : b(node.d);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = (eval == nullptr || eval[l] != 0) &&
                           trees[l]->BoolExpressionState(node.property_ref).active ?
                       1 :
                       0;
        }
        break;
      }
      case NodeOp::SStoreDone: {
        /* BoolKind's StoreValueDone: stores now, where the Flow arrives. */
        const uint8_t *eval = node.d == kNone ? nullptr : b(node.d);
        const float *value = f(node.a);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          if (eval != nullptr && eval[l] == 0) {
            out[l] = 0;
            continue;
          }
          LN_RuntimeTree::FloatExpressionRuntimeState &state = trees[l]->FloatExpressionState(
              node.property_ref);
          state.initialized = true;
          state.stored_value = value[l];
          state.last_store_tick = context.tick_index;
          out[l] = 1;
        }
        break;
      }
      case NodeOp::FStoreValue: {
        /* FloatKind's StoreValue: the first evaluated read stores when Store On Init. */
        const uint8_t *eval = node.d == kNone ? nullptr : b(node.d);
        const float *value = f(node.a);
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          LN_RuntimeTree::FloatExpressionRuntimeState &state = trees[l]->FloatExpressionState(
              node.property_ref);
          if ((eval == nullptr || eval[l] != 0) && !state.initialized) {
            state.initialized = true;
            if (node.bool_value) {
              state.stored_value = value[l];
              state.last_store_tick = context.tick_index;
            }
          }
          out[l] = state.stored_value;
        }
        break;
      }
      case NodeOp::SGateTry:
      case NodeOp::SGateSet: {
        /* TryFlowGate / SetFlowGate with FlowGateOpen's first evaluation (Start Open). */
        const uint8_t *ran = b(node.a);
        const uint8_t *start_open = b(node.b);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = 0;
          if (ran[l] == 0) {
            continue;
          }
          LN_RuntimeTree::BoolExpressionRuntimeState &state = trees[l]->BoolExpressionState(
              node.property_ref);
          if (!state.initialized) {
            state.active = start_open[l] != 0;
            state.initialized = true;
          }
          if (node.op == NodeOp::SGateTry) {
            out[l] = state.active ? 1 : 0;
            continue;
          }
          switch (LN_FlowGateControl(node.int_value)) {
            case LN_FlowGateControl::Toggle:
              state.active = !state.active;
              break;
            case LN_FlowGateControl::Open:
              state.active = true;
              break;
            case LN_FlowGateControl::Close:
              state.active = false;
              break;
            default:
              break;
          }
        }
        break;
      }
      case NodeOp::SGateRead: {
        const uint8_t *eval = node.d == kNone ? nullptr : b(node.d);
        const uint8_t *start_open = b(node.b);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          LN_RuntimeTree::BoolExpressionRuntimeState &state = trees[l]->BoolExpressionState(
              node.property_ref);
          if ((eval == nullptr || eval[l] != 0) && !state.initialized) {
            state.active = start_open[l] != 0;
            state.initialized = true;
          }
          out[l] = state.active ? 1 : 0;
        }
        break;
      }
      case NodeOp::STimeOp: {
        /* OpTimeFlowControlInstruction, NPC by NPC, in the route's context (ScopedStepGateTime's
         * rule behind a gate, the pass's scope). */
        const uint8_t *ran = b(node.a);
        const float *value = node.b == kNone ? nullptr : f(node.b);
        const uint8_t *ignore = node.c == kNone ? nullptr : b(node.c);
        const uint8_t *condition = node.d == kNone ? nullptr : b(node.d);
        const uint32_t gate = node.int_value < 0 ? kNone : uint32_t(node.int_value);
        LN_TickContext route = context;
        for (size_t l = 0; l < n; l++) {
          if (ran[l] == 0) {
            continue;
          }
          LN_RuntimeTree &tree = *trees[l];
          route.execution_scope_serial = s.serial[l];
          if (gate != kNone) {
            route.delta_time = context.delta_time;
            route.unscaled_dt = context.unscaled_dt;
            if (gate < tree.m_stepGates.size()) {
              route.delta_time = tree.m_stepGates[gate].time_passed;
              route.unscaled_dt = tree.m_stepGates[gate].unscaled_time_passed;
            }
            const std::vector<LN_StepGate> &gates = tree.m_program->GetStepGates();
            route.phase_tick_stride = gate < gates.size() ? gates[gate].interval :
                                                            context.phase_tick_stride;
          }
          const float number = value != nullptr ? value[l] : 0.0f;
          const bool ignore_timescale = ignore != nullptr && ignore[l] != 0;
          const bool holds = condition == nullptr || condition[l] != 0;
          const auto number_input = [number]() { return number; };
          const auto ignore_input = [ignore_timescale]() { return ignore_timescale; };
          const auto condition_input = [holds]() { return holds; };
          switch (TimeOp(node.compare)) {
            case TimeOp::NextFrame:
              tree.TimeFlowArmNextFrame(node.property_ref, node.bool_value, route);
              break;
            case TimeOp::Timer:
              tree.TimeFlowArmTimer(node.property_ref,
                                    node.bool_value,
                                    program.event,
                                    route,
                                    number_input,
                                    ignore_input);
              break;
            case TimeOp::Delay:
              tree.TimeFlowArmDelay(node.property_ref,
                                    node.bool_value,
                                    program.event,
                                    route,
                                    number_input,
                                    ignore_input);
              break;
            case TimeOp::Pulsify:
              tree.TimeFlowUpdatePulsify(
                  node.property_ref, program.event, route, number_input, ignore_input);
              break;
            case TimeOp::Barrier:
              tree.TimeFlowUpdateBarrier(node.property_ref,
                                         program.event,
                                         route,
                                         condition_input,
                                         number_input,
                                         ignore_input);
              break;
            case TimeOp::Cooldown:
              tree.TimeFlowTryCooldown(node.property_ref,
                                       node.bool_value,
                                       program.event,
                                       route,
                                       number_input,
                                       ignore_input);
              break;
            case TimeOp::ResetCooldown:
              tree.TimeFlowResetCooldown(node.property_ref, program.event, route);
              break;
          }
        }
        break;
      }
      case NodeOp::STimeRead: {
        /* The time flow kinds of BoolKind. */
        uint8_t *out = b(node.out);
        const uint64_t tick = context.tick_index;
        for (size_t l = 0; l < n; l++) {
          const LN_RuntimeTree::TimeFlowRuntimeState *state = trees[l]->FindTimeFlowState(
              node.property_ref);
          bool result = node.bool_value;
          switch (TimeRead(node.compare)) {
            case TimeRead::Pulse:
              if (tick != UINT64_MAX) {
                result = state != nullptr && state->pulse_tick == tick;
              }
              break;
            case TimeRead::PulsifyPulse:
              if (tick != UINT64_MAX) {
                result = state != nullptr &&
                         (state->pulse_tick == tick ||
                          (state->kind == LN_RuntimeTree::TimeFlowRuntimeKind::Pulsify &&
                           state->last_update_tick == tick &&
                           state->accumulated_time <= LN_TIME_FLOW_EPSILON));
              }
              break;
            case TimeRead::BarrierPassed:
              if (tick != UINT64_MAX) {
                result = state != nullptr &&
                         (state->pulse_tick == tick ||
                          (state->kind == LN_RuntimeTree::TimeFlowRuntimeKind::Barrier &&
                           state->active && state->remaining_time >= 0.0 &&
                           state->accumulated_time + LN_TIME_FLOW_EPSILON >=
                               state->remaining_time));
              }
              break;
            case TimeRead::CooldownAccepted:
            case TimeRead::CooldownBlocked:
              if (s.serial[l] != 0 && state != nullptr) {
                result = TimeRead(node.compare) == TimeRead::CooldownAccepted ?
                             state->accepted_scope_serial == s.serial[l] :
                             state->blocked_scope_serial == s.serial[l];
              }
              break;
            case TimeRead::CooldownReady:
              if (state != nullptr) {
                result = !state->active;
              }
              break;
          }
          out[l] = result ? 1 : 0;
        }
        break;
      }
      case NodeOp::FTimeRead: {
        /* FloatKind's CooldownRemaining / CooldownProgress. */
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          const LN_RuntimeTree::TimeFlowRuntimeState *state = trees[l]->FindTimeFlowState(
              node.property_ref);
          if (state == nullptr) {
            out[l] = node.float_value;
          }
          else if (node.compare == 0) {
            out[l] = float(std::max(0.0, state->remaining_time));
          }
          else if (!state->active || state->accumulated_time <= 0.0) {
            out[l] = 1.0f;
          }
          else {
            const double progress = 1.0 - state->remaining_time / state->accumulated_time;
            out[l] = float(std::clamp(progress, 0.0, 1.0));
          }
        }
        break;
      }
      case NodeOp::RFromEuler: {
        const float *x = f(node.a), *y = f(node.b), *z = f(node.c);
        LN_Rotation *out = r(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = LN_Rotation::FromEulerXYZ(MT_Vector3(x[l], y[l], z[l]));
        }
        break;
      }
      case NodeOp::RAttr: {
        const LN_ObjectAttributeId attribute = LN_ObjectAttributeId(node.compare);
        const auto rotation_of = [&](KX_GameObject *object) {
          if (object == nullptr) {
            return node.rotation;
          }
          if (attribute == LN_ObjectAttributeId::WorldOrientation) {
            return LN_Rotation::FromMatrix(object->NodeGetWorldOrientation());
          }
          if (attribute == LN_ObjectAttributeId::LocalOrientation) {
            return LN_Rotation::FromMatrix(object->NodeGetLocalOrientation());
          }
          MT_Vector3 euler;
          if (LN_RuntimeTree::ColumnObjectAttributeVector(*object, attribute, euler)) {
            return LN_Rotation::FromEulerXYZ(euler);
          }
          return node.rotation;
        };
        LN_Rotation *out = r(node.out);
        if (node.end_a == EndKind::Owner) {
          for (size_t l = 0; l < n; l++) {
            out[l] = rotation_of(trees[l]->GetGameObject());
          }
        }
        else {
          std::fill_n(out, n, rotation_of(objects[node.object_a].object));
        }
        break;
      }
      case NodeOp::RVecToRot:
      case NodeOp::VVecToRot: {
        /* VectorToRotationEuler of the kinds. */
        const float *dx = f(node.position_a.x), *dy = f(node.position_a.y),
                    *dz = f(node.position_a.z);
        const float *ux = f(node.position_b.x), *uy = f(node.position_b.y),
                    *uz = f(node.position_b.z);
        for (size_t l = 0; l < n; l++) {
          const MT_Vector3 euler = LN_RuntimeTree::ColumnVectorToRotationEuler(
              MT_Vector3(dx[l], dy[l], dz[l]), MT_Vector3(ux[l], uy[l], uz[l]), node.int_value);
          if (node.op == NodeOp::RVecToRot) {
            r(node.out)[l] = LN_Rotation::FromEulerXYZ(euler);
          }
          else {
            f(node.out)[l] = euler.x();
            f(node.out2)[l] = euler.y();
            f(node.out3)[l] = euler.z();
          }
        }
        break;
      }
      case NodeOp::BKBool:
      case NodeOp::BKInt:
      case NodeOp::BKFloat:
      case NodeOp::BKVector:
      case NodeOp::BKRotation:
      case NodeOp::BKColor: {
        /* The bridge: the VM's kind functions, NPC by NPC, where the VM evaluates the expression. */
        const uint8_t *eval = node.d == kNone ? nullptr : b(node.d);
        LN_TickContext route = context;
        for (size_t l = 0; l < n; l++) {
          const bool evaluated = eval == nullptr || eval[l] != 0;
          if (evaluated) {
            route_context(route, l, node.compare);
          }
          LN_RuntimeTree &tree = *trees[l];
          switch (node.op) {
            case NodeOp::BKBool:
              b(node.out)[l] = (evaluated ? tree.ColumnEvalBool(node.property_ref, node.bool_value,
                                                                route) :
                                            node.bool_value) ?
                                   1 :
                                   0;
              break;
            case NodeOp::BKInt:
              i(node.out)[l] = evaluated ?
                                   tree.ColumnEvalInt(node.property_ref, node.int_value, route) :
                                   node.int_value;
              break;
            case NodeOp::BKFloat:
              f(node.out)[l] = evaluated ? tree.ColumnEvalFloat(node.property_ref,
                                                                node.float_value,
                                                                route) :
                                           node.float_value;
              break;
            case NodeOp::BKVector: {
              const MT_Vector3 fallback = fallback3(node);
              vector3(node,
                      l,
                      evaluated ? tree.ColumnEvalVector(node.property_ref, fallback, route) :
                                  fallback);
              break;
            }
            case NodeOp::BKRotation:
              r(node.out)[l] = evaluated ? tree.ColumnEvalRotation(node.property_ref,
                                                                   node.rotation,
                                                                   route) :
                                           node.rotation;
              break;
            default: {
              const MT_Vector4 fallback(node.color[0], node.color[1], node.color[2], node.color[3]);
              const MT_Vector4 color = evaluated ?
                                           tree.ColumnEvalColor(node.property_ref, fallback, route) :
                                           fallback;
              f(node.out)[l] = color.x();
              f(node.out2)[l] = color.y();
              f(node.out3)[l] = color.z();
              f(node.out4)[l] = color.w();
              break;
            }
          }
        }
        break;
      }
      case NodeOp::BOp: {
        /* The bridge: the VM's handler, NPC by NPC; what it records is kept for the flush. */
        const uint8_t *ran = b(node.a);
        LN_TickContext route = context;
        const size_t write = size_t(node.int_value);
        for (size_t l = 0; l < n; l++) {
          if (ran[l] == 0) {
            continue;
          }
          route_context(route, l, node.compare);
          const LN_CommandBuffer::RecordMark mark = capture->buffer->Mark();
          trees[l]->ColumnRunOp(program.event, node.property_ref, *capture->buffer, route);
          const size_t before = capture->refs.size();
          bool moves = false;
          capture->buffer->CommandsSince(mark, capture->refs, moves);
          const size_t cell = write * run.count + first + l;
          run.captured_begin[cell] = uint32_t(before);
          run.captured_count[cell] = uint32_t(capture->refs.size() - before);
          if (moves) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
              CM_Warning("Logic Nodes: columns: a bridged op recorded a move (it is applied at the "
                         "flush, never early)");
            }
          }
        }
        break;
      }
      case NodeOp::BLoop: {
        /* Plan C1-9: a Loop body through the VM's block walk, NPC by NPC; what it records is kept
         * for the flush, as a bridged op's. */
        const uint8_t *runs = b(node.a);
        LN_TickContext route = context;
        const size_t write = size_t(node.int_value);
        for (size_t l = 0; l < n; l++) {
          if (runs[l] == 0) {
            continue;
          }
          route_context(route, l, node.compare);
          const LN_CommandBuffer::RecordMark mark = capture->buffer->Mark();
          trees[l]->ColumnRunLoopBlock(program.event, node.property_ref, *capture->buffer, route);
          const size_t before = capture->refs.size();
          bool moves = false;
          capture->buffer->CommandsSince(mark, capture->refs, moves);
          const size_t cell = write * run.count + first + l;
          run.captured_begin[cell] = uint32_t(before);
          run.captured_count[cell] = uint32_t(capture->refs.size() - before);
          if (moves) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
              CM_Warning("Logic Nodes: columns: a bridged Loop recorded a move (it is applied at "
                         "the flush, never early)");
            }
          }
        }
        break;
      }
      case NodeOp::SLoopDone: {
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = trees[l]->ColumnLoopCompleted(node.property_ref, s.serial[l]) ? 1 : 0;
        }
        break;
      }
      case NodeOp::SRan: {
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = trees[l]->ColumnStampRan(node.property_ref, context.tick_index, s.serial[l]) ? 1 :
                                                                                                 0;
        }
        break;
      }
      case NodeOp::IRepeatRemaining: {
        const int32_t *requested = i(node.a);
        int32_t *out = i(node.out);
        for (size_t l = 0; l < n; l++) {
          const LN_RuntimeTree::FlowControlRuntimeState *state = trees[l]->FindFlowControlState(
              node.property_ref);
          if (state == nullptr) {
            out[l] = node.int_value;
            continue;
          }
          const uint32_t limit = requested[l] > 0 ? uint32_t(requested[l]) : 0u;
          const uint32_t remaining = limit > state->accepted_count ? limit - state->accepted_count :
                                                                     0u;
          out[l] = remaining > uint32_t(std::numeric_limits<int32_t>::max()) ?
                       std::numeric_limits<int32_t>::max() :
                       int32_t(remaining);
        }
        break;
      }
      case NodeOp::BRepeatExhausted: {
        const int32_t *requested = i(node.a);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          const LN_RuntimeTree::FlowControlRuntimeState *state = trees[l]->FindFlowControlState(
              node.property_ref);
          if (state == nullptr) {
            out[l] = node.bool_value ? 1 : 0;
            continue;
          }
          const uint32_t limit = requested[l] > 0 ? uint32_t(requested[l]) : 0u;
          out[l] = state->accepted_count >= limit ? 1 : 0;
        }
        break;
      }
      case NodeOp::SReached: {
        /* BoolKind's InstructionReached. */
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          const LN_RuntimeTree::InstructionMotionState *state = trees[l]->FindMotionState(
              node.property_ref);
          out[l] = state != nullptr && state->last_reached_tick == context.tick_index &&
                           state->last_reached_scope_serial == s.serial[l] ?
                       1 :
                       0;
        }
        break;
      }
      case NodeOp::SMoveReached: {
        /* MoveToward's handler: Reached when this step's move ends within Stop Distance. */
        const uint8_t *ran = b(node.a);
        for (size_t l = 0; l < n; l++) {
          if (ran[l] == 0) {
            continue;
          }
          const MT_Vector3 target_pos(
              f(node.position_a.x)[l], f(node.position_a.y)[l], f(node.position_a.z)[l]);
          const MT_Vector3 position(
              f(node.position_b.x)[l], f(node.position_b.y)[l], f(node.position_b.z)[l]);
          const float speed = f(node.b)[l];
          const float stop_distance = f(node.c)[l];
          const float distance = (target_pos - position).length();
          const float step = std::max(speed * f(node.d)[l], 0.0f);
          const float tolerance = std::max(
              1.0e-4f, (std::fabs(stop_distance) + float(target_pos.length())) * 1.0e-6f);
          if (std::isfinite(distance) && std::isfinite(step) &&
              distance <= std::max(stop_distance, 0.0f) + step + tolerance)
          {
            trees[l]->ColumnNoteReached(node.property_ref, context.tick_index);
          }
        }
        break;
      }
      case NodeOp::MForceToTarget: {
        /* Apply Force To Target's handler, one code with it (ColumnForceToTarget). */
        const uint8_t *ran = b(node.a);
        uint8_t *applies = b(node.out);
        for (size_t l = 0; l < n; l++) {
          applies[l] = 0;
          f(node.vec_c.x)[l] = 0.0f;
          f(node.vec_c.y)[l] = 0.0f;
          f(node.vec_c.z)[l] = 0.0f;
          if (ran[l] == 0) {
            continue;
          }
          const MT_Vector3 target_position(
              f(node.position_a.x)[l], f(node.position_a.y)[l], f(node.position_a.z)[l]);
          const MT_Vector3 position(
              f(node.position_b.x)[l], f(node.position_b.y)[l], f(node.position_b.z)[l]);
          bool reached = false;
          MT_Vector3 force;
          if (LN_RuntimeTree::ColumnForceToTarget(target_position,
                                                  f(node.b)[l],
                                                  f(node.c)[l],
                                                  f(node.d)[l],
                                                  position,
                                                  reached,
                                                  force))
          {
            applies[l] = 1;
            f(node.vec_c.x)[l] = force.x();
            f(node.vec_c.y)[l] = force.y();
            f(node.vec_c.z)[l] = force.z();
          }
          if (reached && node.property_ref != kNone) {
            trees[l]->ColumnNoteReached(node.property_ref, context.tick_index);
          }
        }
        break;
      }
      case NodeOp::SStamp: {
        const uint8_t *ran = b(node.a);
        for (size_t l = 0; l < n; l++) {
          if (ran[l] != 0) {
            trees[l]->MarkInstructionStamp(node.property_ref, context.tick_index);
          }
        }
        break;
      }
      case NodeOp::STreeWrite: {
        /* OpDirectTreePropertyCommand's SetTreePropertyValue: a fresh value of the write's type, or
         * the constant it writes. */
        const LN_ColumnProgram::TreeWrite &write = program.tree_write_ops[size_t(node.int_value)];
        const uint8_t *ran = b(node.a);
        for (size_t l = 0; l < n; l++) {
          if (ran[l] == 0) {
            continue;
          }
          LN_Value value;
          if (write.constant) {
            value = write.value;
          }
          else {
            value.type = write.type;
            value.exists = true;
            switch (write.type) {
              case LN_ValueType::Float:
                value.float_value = f(node.b)[l];
                break;
              case LN_ValueType::Int:
                value.int_value = i(node.c)[l];
                break;
              case LN_ValueType::Vector:
                value.vector_value = MT_Vector3(f(node.position_a.x)[l],
                                                f(node.position_a.y)[l],
                                                f(node.position_a.z)[l]);
                break;
              default:
                value.bool_value = b(node.d)[l] != 0;
                break;
            }
          }
          trees[l]->SetTreePropertyValue(node.property_ref, value);
        }
        break;
      }
      case NodeOp::FGateDelta: {
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = trees[l]->ColumnGateDelta(node.property_ref, node.compare != 0, context);
        }
        break;
      }
      case NodeOp::SGate: {
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = trees[l]->StepGateRuns(node.property_ref) ? 1 : 0;
        }
        break;
      }
      case NodeOp::FStepTime: {
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = trees[l]->ColumnStepTimePassed(node.property_ref);
        }
        break;
      }
      case NodeOp::FGateUnscaled: {
        float *out = f(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = trees[l]->ColumnGateUnscaledDelta(node.property_ref, context);
        }
        break;
      }
      case NodeOp::SOnce: {
        /* OpTimeFlowControlInstruction's TryOnce. */
        const uint8_t *ran = b(node.a);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          if (ran[l] == 0) {
            out[l] = 0;
            continue;
          }
          LN_RuntimeTree::BoolExpressionRuntimeState &state = trees[l]->BoolExpressionState(
              node.property_ref);
          state.cached_result = !state.active;
          if (state.cached_result) {
            state.active = true;
          }
          state.initialized = true;
          state.last_update_tick = s.serial[l];
          out[l] = state.cached_result ? 1 : 0;
        }
        break;
      }
      case NodeOp::SFlowLast: {
        int32_t *out = i(node.out);
        for (size_t l = 0; l < n; l++) {
          const LN_RuntimeTree::FlowControlRuntimeState *state = trees[l]->FindFlowControlState(
              node.property_ref);
          out[l] = state != nullptr ? int32_t(state->last_route_index) : -1;
        }
        break;
      }
      case NodeOp::SFlowRouter: {
        /* TryFlowRouter. */
        const uint8_t *ran = b(node.a);
        const int32_t *last = i(node.b);
        int32_t *out = i(node.out), *out_last = i(node.out2);
        const uint32_t output_count = uint32_t(node.int_value);
        for (size_t l = 0; l < n; l++) {
          out[l] = -1;
          out_last[l] = last[l];
          if (ran[l] == 0) {
            continue;
          }
          LN_RuntimeTree::FlowControlRuntimeState *state = trees[l]->FindFlowControlState(
              node.property_ref);
          if (state == nullptr) {
            continue;
          }
          const uint32_t selected_index = state->next_route_index % output_count;
          state->last_route_index = selected_index;
          state->next_route_index = (selected_index + 1u) % output_count;
          state->routed_scope_serial = s.serial[l];
          out[l] = int32_t(selected_index);
          out_last[l] = int32_t(selected_index);
        }
        break;
      }
      case NodeOp::SRepeat: {
        /* TryRepeatLimit. */
        const uint8_t *ran = b(node.a);
        const int32_t *requested = i(node.b);
        uint8_t *accepted = b(node.out), *blocked = b(node.out2);
        for (size_t l = 0; l < n; l++) {
          accepted[l] = 0;
          blocked[l] = 0;
          if (ran[l] == 0) {
            continue;
          }
          LN_RuntimeTree::FlowControlRuntimeState *state = trees[l]->FindFlowControlState(
              node.property_ref);
          if (state == nullptr) {
            continue;
          }
          const uint32_t limit = requested[l] > 0 ? uint32_t(requested[l]) : 0u;
          if (state->accepted_count < limit) {
            state->accepted_count++;
            state->accepted_scope_serial = s.serial[l];
            state->blocked_scope_serial = 0;
            accepted[l] = 1;
          }
          else {
            state->accepted_scope_serial = 0;
            state->blocked_scope_serial = s.serial[l];
            blocked[l] = 1;
          }
        }
        break;
      }
      case NodeOp::QRay: {
        /* Plan C2 P3: the VM's QueryKind for the ray, lane by lane where the VM casts it; its
         * outputs as PhysicsQueryHit / Distance / Point / Normal give them. No command buffer: the
         * run started quietly (nothing pending its cast would apply). */
        thread_local LN_RuntimeTree::QueryExpressionRuntimeState scratch;
        const uint8_t *eval = node.d == kNone ? nullptr : b(node.d);
        /* The condition: a disabled query gives the reset result (its outputs zero). */
        const uint8_t *enabled = node.a == kNone ? nullptr : b(node.a);
        uint8_t *hit = b(node.out);
        float *distance = f(node.out2);
        float *px = f(node.position_a.x), *py = f(node.position_a.y), *pz = f(node.position_a.z);
        float *nx = f(node.position_b.x), *ny = f(node.position_b.y), *nz = f(node.position_b.z);
        LN_TickContext route = context;
        for (size_t l = 0; l < n; l++) {
          hit[l] = 0;
          distance[l] = 0.0f;
          px[l] = py[l] = pz[l] = 0.0f;
          nx[l] = ny[l] = nz[l] = 0.0f;
          if ((eval != nullptr && eval[l] == 0) || (enabled != nullptr && enabled[l] == 0)) {
            continue;
          }
          route_context(route, l, node.compare);
          route.command_buffer = nullptr;
          route.command_execution_key = UINT64_MAX;
          const LN_PhysicsQueryResult &q =
              trees[l]->ColumnCastQuery(node.property_ref, route, scratch).physics_query;
          hit[l] = q.hit ? 1 : 0;
          distance[l] = q.hit_distance;
          if (q.hit) {
            px[l] = q.hit_position.x();
            py[l] = q.hit_position.y();
            pz[l] = q.hit_position.z();
            nx[l] = q.hit_normal.x();
            ny[l] = q.hit_normal.y();
            nz[l] = q.hit_normal.z();
          }
        }
        break;
      }
      case NodeOp::QDistance: {
        /* DistanceBetweenKind, lane by lane. */
        float *distance = f(node.out), *squared = f(node.out2);
        const double max_float = double(std::numeric_limits<float>::max());
        const auto end_of = [&](const EndKind kind,
                                const uint32_t object,
                                const VecSlots &position,
                                const size_t l,
                                double &x,
                                double &y,
                                double &z) {
          switch (kind) {
            case EndKind::Owner:
              x = double(s.px[l]);
              y = double(s.py[l]);
              z = double(s.pz[l]);
              return s.owner_ok[l] != 0;
            case EndKind::Object:
              x = double(objects[object].position.x());
              y = double(objects[object].position.y());
              z = double(objects[object].position.z());
              return objects[object].ok;
            case EndKind::Position: {
              const float px = f(position.x)[l], py = f(position.y)[l], pz = f(position.z)[l];
              x = double(px);
              y = double(py);
              z = double(pz);
              return std::isfinite(px) && std::isfinite(py) && std::isfinite(pz);
            }
            case EndKind::Runtime: {
              const NamedObject &found = run.runtime[size_t(object) * run.count + first + l];
              x = double(found.position.x());
              y = double(found.position.y());
              z = double(found.position.z());
              return found.ok;
            }
            case EndKind::None:
              break;
          }
          return false;
        };
        uint8_t *valid = b(node.out3);
        for (size_t l = 0; l < n; l++) {
          double ax, ay, az, bx, by, bz;
          valid[l] = 0;
          if (!end_of(node.end_a, node.object_a, node.position_a, l, ax, ay, az) ||
              !end_of(node.end_b, node.object_b, node.position_b, l, bx, by, bz))
          {
            distance[l] = 0.0f;
            squared[l] = 0.0f;
            continue;
          }
          const double dx = ax - bx;
          const double dy = ay - by;
          const double dz = az - bz;
          const double sq = dx * dx + dy * dy + dz * dz;
          if (!std::isfinite(sq) || sq < 0.0) {
            distance[l] = 0.0f;
            squared[l] = 0.0f;
            continue;
          }
          squared[l] = float(std::min(sq, max_float));
          distance[l] = float(std::min(std::sqrt(sq), max_float));
          valid[l] = 1;
        }
        break;
      }
      case NodeOp::BNot: {
        const uint8_t *x = b(node.a);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = x[l] ^ 1;
        }
        break;
      }
      case NodeOp::BAnd: {
        const uint8_t *x = b(node.a), *y = b(node.b);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = x[l] & y[l];
        }
        break;
      }
      case NodeOp::BOr: {
        const uint8_t *x = b(node.a), *y = b(node.b);
        uint8_t *out = b(node.out);
        for (size_t l = 0; l < n; l++) {
          out[l] = x[l] | y[l];
        }
        break;
      }
      case NodeOp::BCmp: {
        const float *x = f(node.a), *y = f(node.b);
        uint8_t *out = b(node.out);
        switch (LN_FloatCompareOperation(node.compare)) {
          case LN_FloatCompareOperation::Equal:
            for (size_t l = 0; l < n; l++) {
              out[l] = std::fabs(x[l] - y[l]) <= 1.0e-6f;
            }
            break;
          case LN_FloatCompareOperation::NotEqual:
            for (size_t l = 0; l < n; l++) {
              out[l] = std::fabs(x[l] - y[l]) > 1.0e-6f;
            }
            break;
          case LN_FloatCompareOperation::GreaterThan:
            for (size_t l = 0; l < n; l++) {
              out[l] = x[l] > y[l];
            }
            break;
          case LN_FloatCompareOperation::LessThan:
            for (size_t l = 0; l < n; l++) {
              out[l] = x[l] < y[l];
            }
            break;
          case LN_FloatCompareOperation::GreaterEqual:
            for (size_t l = 0; l < n; l++) {
              out[l] = x[l] >= y[l];
            }
            break;
          case LN_FloatCompareOperation::LessEqual:
            for (size_t l = 0; l < n; l++) {
              out[l] = x[l] <= y[l];
            }
            break;
          default:
            std::fill_n(out, n, uint8_t(0));
            break;
        }
        break;
      }
      default:
        break;
    }
  }
#undef LN_COLUMNS_UNARY
#undef LN_COLUMNS_BINARY

  if ((parts & kPartEnd) == 0) {
    return;
  }
  /* What ExecuteReady does after the pass (the gates that ran). */
  for (size_t l = 0; l < n; l++) {
    trees[l]->EndColumnPass(context);
  }
  if (capture != nullptr) {
    capture->buffer->EndChunkRecording();
  }
  uint32_t tree_sets = 0;
  for (const LN_ColumnProgram::TreeWrite &write : program.tree_write_ops) {
    const uint8_t *ran = b(write.mask);
    for (size_t l = 0; l < n; l++) {
      tree_sets += ran[l];
    }
  }
  run.chunk_tree_sets[first / kChunkLanes] = tree_sets;

  /* What the writes need later, into the run's arrays. (CopyLanes: one value is copied as one;
   * a memcpy of a length known only now calls the library, ~20 times for a run of 1.) */
  const size_t count = run.count;
  for (size_t w = 0; w < program.writes.size(); w++) {
    const Write &write = program.writes[w];
    if (write.kind == WriteKind::Captured) {
      /* A caught op counts where it recorded something. */
      for (size_t l = 0; l < n; l++) {
        run.masks[w * count + first + l] = run.captured_count[w * count + first + l] != 0 ? 1 : 0;
      }
      continue;
    }
    CopyLanes(run.masks.data() + w * count + first, b(write.mask), n);
    const auto keep_float = [&](const uint32_t column, const uint32_t slot) {
      CopyLanes(run.floats.data() + size_t(column) * count + first, f(slot), n);
    };
    switch (write.kind) {
      case WriteKind::Number:
        if (write.number_type == LN_ValueType::Bool) {
          CopyLanes(run.bools.data() + size_t(write.out_bool) * count + first, b(write.value), n);
        }
        else if (write.number_type == LN_ValueType::Int) {
          CopyLanes(run.ints.data() + size_t(write.out_int) * count + first, i(write.value), n);
        }
        else {
          keep_float(write.out_float, write.value);
        }
        break;
      case WriteKind::Modify:
        keep_float(write.out_float, write.operand);
        if (write.clamped) {
          keep_float(write.out_float + 1, write.min);
          keep_float(write.out_float + 2, write.max);
        }
        break;
      case WriteKind::Service:
        if (write.command == LN_CommandBuffer::CommandType::SetVisibility) {
          CopyLanes(run.bools.data() + size_t(write.out_bool) * count + first, b(write.flag_a), n);
          CopyLanes(
              run.bools.data() + size_t(write.out_bool + 1) * count + first, b(write.flag_b), n);
        }
        else if (write.command == LN_CommandBuffer::CommandType::SetObjectColor) {
          for (uint32_t c = 0; c < 4; c++) {
            keep_float(write.out_float + c, write.color[c]);
          }
        }
        break;
      case WriteKind::Motion:
        keep_float(write.out_float, write.vector.x);
        keep_float(write.out_float + 1, write.vector.y);
        keep_float(write.out_float + 2, write.vector.z);
        keep_float(write.out_float + 3, write.secondary.x);
        keep_float(write.out_float + 4, write.secondary.y);
        keep_float(write.out_float + 5, write.secondary.z);
        keep_float(write.out_float + 6, write.scalar);
        CopyLanes(run.ints.data() + size_t(write.out_int) * count + first, i(write.int_a), n);
        CopyLanes(run.ints.data() + size_t(write.out_int + 1) * count + first, i(write.int_b), n);
        break;
      case WriteKind::Transform:
        if (!write.is_rotation) {
          keep_float(write.out_float, write.vector.x);
          keep_float(write.out_float + 1, write.vector.y);
          keep_float(write.out_float + 2, write.vector.z);
        }
        else if (write.out_rot != kNone) {
          std::copy_n(r(write.rot), n, run.rots.data() + size_t(write.out_rot) * count + first);
        }
        if (write.local != kNone) {
          CopyLanes(run.bools.data() + size_t(write.out_bool) * count + first, b(write.local), n);
        }
        break;
    }
  }
  /* Which NPCs of the chunk wrote anything (Run::lane_any), column by column. */
  uint8_t *any = run.lane_any.data() + first;
  std::fill_n(any, n, uint8_t(0));
  for (size_t w = 0; w < program.writes.size(); w++) {
    const uint8_t *mask = run.masks.data() + w * count + first;
    for (size_t l = 0; l < n; l++) {
      any[l] |= mask[l];
    }
  }
}

size_t LN_ColumnRunner::RunLanes(const size_t begin,
                                 LN_CommandBuffer &buffer,
                                 const LN_TickContext &context,
                                 LN_JobPool *pool)
{
  Impl &d = *m_impl;
  if (pool != nullptr) {
    d.flush_pool = pool;
  }
  if (begin < d.group_of.size() && d.group_of[begin] != 0) {
    /* Plan C1-9: objects with the same trees, one after the other. Their first trees run for
     * every object, then their second trees...: an object's trees in their order, and no lane
     * reads another object's (the run's own rules). */
    const Impl::Group &group = d.groups[d.group_of[begin] - 1];
    for (uint32_t tree = 0; tree < group.trees; tree++) {
      std::vector<uint32_t> &lanes = d.set_lanes;
      lanes.clear();
      for (uint32_t object = 0; object < group.objects; object++) {
        lanes.push_back(uint32_t(group.begin + size_t(object) * group.trees + tree));
      }
      d.RunSet(lanes, buffer, context, pool);
    }
    return group.begin + size_t(group.trees) * group.objects;
  }
  const size_t end = d.RunEnd(begin);
  /* Plan C2 P1: consecutive runs of one chunk each (runs of 1 of different programs, a scene's
   * short groups) run as a wave: their starts here in order, their chunks together on the pool,
   * their ends here in order. One after the other they used one thread where the old system's
   * tree batches used all: 1,000 trees of 10 programs in turn, 0.59 -> 0.45 ms. */
  /* (Plan C2 P2: a run with a lane that joins no wave, kRunNoWave on its first lane, runs alone.) */
  const auto wave_run = [&](const size_t at) {
    return Impl::WaveEligible(*d.lane_program[at]) &&
           (at >= d.lane_flags.size() || (d.lane_flags[at] & Impl::kRunNoWave) == 0);
  };
  if (pool != nullptr && pool->GetHelperCount() > 0 && d.min_wave_lanes != 0 &&
      end - begin < kChunkLanes && wave_run(begin))
  {
    size_t wave_end = end;
    size_t runs = 1;
    while (wave_end < d.trees.size() && d.lane[wave_end] != 0 &&
           (wave_end >= d.group_of.size() || d.group_of[wave_end] == 0) && wave_run(wave_end))
    {
      const size_t next_end = d.RunEnd(wave_end);
      if (next_end - wave_end >= kChunkLanes) {
        break; /* a long run fills the pool with its own chunks */
      }
      wave_end = next_end;
      runs++;
    }
    if (runs >= 2 && wave_end - begin >= d.min_wave_lanes) {
      d.RunWave(begin, wave_end, buffer, context, *pool);
      return wave_end;
    }
  }
  std::vector<uint32_t> &lanes = d.set_lanes;
  lanes.clear();
  for (size_t index = begin; index < end; index++) {
    lanes.push_back(uint32_t(index));
  }
  d.RunSet(lanes, buffer, context, pool);
  return end;
}

size_t LN_ColumnRunner::Impl::RunEnd(const size_t begin) const
{
  const LN_ColumnProgram *program = lane_program[begin];
  size_t end = begin + 1;
  while (end < trees.size() && lane[end] != 0 && lane_program[end] == program &&
         (end >= group_of.size() || group_of[end] == 0) &&
         (end >= lane_flags.size() || (lane_flags[end] & kRunStart) == 0))
  {
    end++;
  }
  return end;
}

bool LN_ColumnRunner::Impl::WaveEligible(const LN_ColumnProgram &program)
{
  /* Its run's start reads only its own owners and named objects (no lane of another run), its
   * chunks run on any thread, it never gives its lanes back to the VM (which would record trees
   * before the wave's earlier runs ran) and finds no object while playing. */
  return !program.bridged && !program.quiet_start && !program.live_positions &&
         program.runtime_objects.empty() && program.average_reads.empty() &&
         program.target_columns == 0;
}

void LN_ColumnRunner::Impl::RunWave(const size_t begin,
                                    const size_t end,
                                    LN_CommandBuffer &buffer,
                                    const LN_TickContext &context,
                                    LN_JobPool &pool)
{
  /* The starts in order (each may apply pending moves of its owners and named objects, on this
   * thread); runs are kept by index (a start may grow `runs`). */
  wave_runs.clear();
  size_t lanes_total = 0;
  for (size_t index = begin; index < end;) {
    const size_t run_end = RunEnd(index);
    std::vector<uint32_t> &lanes = set_lanes;
    lanes.clear();
    for (size_t k = index; k < run_end; k++) {
      lanes.push_back(uint32_t(k));
    }
    const size_t run_index = RunSetBegin(lanes, buffer, context);
    if (run_index != kNoRun) {
      wave_runs.push_back(run_index);
      lanes_total += runs[run_index].count;
    }
    index = run_end;
  }
  if (wave_runs.empty()) {
    return;
  }
  /* The chunks: consecutive runs in tasks of about the same number of lanes (about 4 a thread,
   * at least 16 lanes each, as tree batches cut their chunks). */
  const size_t threads = pool.GetHelperCount() + 1;
  const size_t task_count = std::clamp<size_t>(
      std::min(threads * 4, lanes_total / 16), 1, std::max<size_t>(1, wave_runs.size()));
  wave_tasks.clear();
  size_t first = 0, done = 0;
  for (size_t r = 0; r < wave_runs.size(); r++) {
    done += runs[wave_runs[r]].count;
    if (done * task_count >= (wave_tasks.size() + 1) * lanes_total || r + 1 == wave_runs.size()) {
      wave_tasks.emplace_back(first, r + 1);
      first = r + 1;
    }
  }
  pool.Run(wave_tasks.size(), [&](const size_t task, uint32_t /*thread*/) {
    for (size_t r = wave_tasks[task].first; r < wave_tasks[task].second; r++) {
      RunSetChunks(wave_runs[r], buffer, nullptr);
    }
  });
  g_column_waves.fetch_add(1, std::memory_order_relaxed);
  /* The ends in order (pending moves noted, counts made). */
  for (const size_t run_index : wave_runs) {
    RunSetEnd(run_index, buffer, context);
  }
}

void LN_ColumnRunner::Impl::RunSet(std::vector<uint32_t> &lanes,
                                   LN_CommandBuffer &buffer,
                                   const LN_TickContext &context,
                                   LN_JobPool *pool)
{
  const size_t run_index = RunSetBegin(lanes, buffer, context);
  if (run_index == kNoRun) {
    return;
  }
  if (run_index == kSplitAt) {
    /* In the VM's order: the lanes before the pushed one, the pushed one on the VM, the rest
     * (which may split again). */
    std::vector<uint32_t> rest(lanes.begin(), lanes.end());
    for (;;) {
      const size_t at = split_at;
      if (at > 0) {
        std::vector<uint32_t> head(rest.begin(), rest.begin() + at);
        RunSet(head, buffer, context, pool);
      }
      GiveBack(rest[at], buffer, context);
      g_column_bridge_fallbacks.fetch_add(1, std::memory_order_relaxed);
      given_back++;
      rest.erase(rest.begin(), rest.begin() + at + 1);
      if (rest.empty()) {
        return;
      }
      const size_t next = RunSetBegin(rest, buffer, context);
      if (next == kNoRun) {
        return;
      }
      if (next == kSplitAt) {
        continue;
      }
      if (next == kSplitRun) {
        std::vector<uint32_t> one(1);
        for (const uint32_t lane : rest) {
          one[0] = lane;
          RunSet(one, buffer, context, pool);
        }
        return;
      }
      RunSetChunks(next, buffer, pool);
      RunSetEnd(next, buffer, context);
      return;
    }
  }
  if (run_index == kSplitRun) {
    /* Plan C2 P2: lane by lane, each a run of its own in its place (RunSetBegin checks each). */
    const std::vector<uint32_t> each(lanes.begin(), lanes.end());
    std::vector<uint32_t> one(1);
    for (const uint32_t lane : each) {
      one[0] = lane;
      RunSet(one, buffer, context, pool);
    }
    return;
  }
  RunSetChunks(run_index, buffer, pool);
  RunSetEnd(run_index, buffer, context);
}

void LN_ColumnRunner::Impl::RunDirectLane(const size_t index,
                                          LN_CommandBuffer &buffer,
                                          const LN_TickContext &context)
{
  /* The column pass (ExecuteReady: the pass's bookkeeping, the event's compiled route or the
   * column walk of its blocks). A planned tree of any program (plan C2 P6: a lane a run gives
   * back runs this way too). */
  trees[index]->ExecuteReady(buffer, context, nullptr);
  direct_ran++;
}

size_t LN_ColumnRunner::Impl::RunSetBegin(std::vector<uint32_t> &lanes,
                                          LN_CommandBuffer &buffer,
                                          const LN_TickContext &context)
{
  Impl &d = *this;
  const LN_ColumnProgram *program = d.lane_program[lanes[0]];
  /* Plan C2 P2: a run with a parented lane or a named one (a split run's lanes each again). */
  const uint8_t first_flags = lanes[0] < d.lane_flags.size() ? d.lane_flags[lanes[0]] : 0;
  if ((first_flags & kRunSpecial) != 0 || (lanes.size() == 1 && (first_flags & kLaneSpecial) != 0))
  {
    const size_t verdict = d.CheckPendingDepends(lanes, buffer, context, *program);
    if (verdict == kSplitRun) {
      g_column_depend_splits.fetch_add(1, std::memory_order_relaxed);
      return kSplitRun;
    }
    if (verdict == kNoRun) {
      d.GiveBack(lanes[0], buffer, context);
      g_column_depend_fallbacks.fetch_add(1, std::memory_order_relaxed);
      d.given_back++;
      return kNoRun;
    }
  }
  size_t first = 0;
  if (program->bridged || program->quiet_start) {

    /* The bridge reads through the VM's code, which applies an object's pending moves where it
     * reads it (a ray from it, a distance to it) and otherwise sees them unapplied: exact only when
     * neither a lane's object nor an object the program names has one. Else the VM records them. */
    bool pending = false;
    /* A lane object's pending moves (another tree, the object's tree before this one) change
     * nothing when no read of the program applies them: no query from the owner (the VM's pass
     * leaves them pending, and so does the run). Plan C2 fix 1: a few such lanes split the run
     * (kSplitAt: each records on the VM in its place, the lanes around it run); many (over a
     * quarter) give it back whole, as before (its parts would cost more than the VM). */
    size_t first_pending = SIZE_MAX, pending_lanes = 0;
    for (size_t index = 0; index < lanes.size() && !program->owner_pending_ok; index++) {
      if (buffer.HasPendingTransforms(d.trees[lanes[index]]->GetGameObject(), true)) {
        first_pending = std::min(first_pending, index);
        pending_lanes++;
        if (pending_lanes * 4 > lanes.size()) {
          break;
        }
      }
    }
    if (first_pending != SIZE_MAX) {
      if (pending_lanes * 4 > lanes.size() || !d.split_pushed_lanes) {
        pending = true;
      }
      else {
        d.split_at = first_pending;
        g_column_pushed_splits.fetch_add(1, std::memory_order_relaxed);
        return kSplitAt;
      }
    }
    const auto named_pending = [&]() {
      for (size_t k = 0; k < program->applied_named.size(); k++) {
        KX_GameObject *object = d.trees[lanes[first]]->ResolveObjectValueExpressionForColumns(
            program->applied_named[k], context);
        if (object != nullptr && buffer.HasPendingTransforms(object, true)) {
          return true;
        }
      }
      return false;
    };
    /* Only a named object has some (a player another tree moved this step): the first tree plays
     * on the VM, whose read of the object applies them where the VM applies them; when that
     * leaves none, the others run, as the VM's later trees, with them applied. */
    constexpr size_t kVMTreesForNamedMoves = 2;
    for (size_t played = 0; !pending && played <= kVMTreesForNamedMoves && named_pending();
         played++)
    {
      if (played == kVMTreesForNamedMoves || lanes.size() - first < 2) {
        pending = true;
        break;
      }
      d.GiveBack(lanes[first], buffer, context);
      g_column_bridge_fallbacks.fetch_add(1, std::memory_order_relaxed);
      d.given_back++;
      first++;
    }
    if (pending) {
      for (size_t index = first; index < lanes.size(); index++) {
        d.GiveBack(lanes[index], buffer, context);
      }
      g_column_bridge_fallbacks.fetch_add(lanes.size() - first, std::memory_order_relaxed);
      d.given_back += lanes.size() - first;
      return kNoRun;
    }
  }
  lanes.erase(lanes.begin(), lanes.begin() + first);
  /* Plan C1-9: the objects each NPC finds while playing, resolved here (their sources read
   * nothing the pass changes). One that is a lane's object this tick, or a parent of one, would
   * have its pending moves applied by the VM's read at a moment the run cannot keep: the VM
   * records these NPCs. */
  std::vector<KX_GameObject *> &found = d.runtime_found;
  found.assign(program->runtime_objects.size() * lanes.size(), nullptr);
  if (!program->runtime_objects.empty()) {
    /* The tick's lane objects, only for the found objects that are measured or moved (a property
     * write's target needs no such check: the farm's Barn, 10,000 crops a step). */
    bool needs_lane_owners = false;
    for (size_t k = 0; k < program->runtime_objects.size(); k++) {
      needs_lane_owners = needs_lane_owners || program->runtime_measured[k] != 0 ||
                          program->runtime_moved[k] != 0;
    }
    if (needs_lane_owners && !d.lane_owners_made) {
      d.lane_owners.Clear(d.lane_count);
      for (size_t index = 0; index < d.trees.size(); index++) {
        if (index < d.lane.size() && d.lane[index] != 0) {
          d.lane_owners.Insert(d.trees[index]->GetGameObject());
        }
      }
      d.lane_owners_made = true;
    }
    /* Plan C1-10: an object the same for every NPC is found once; one only written is found
     * after the run, NPC by NPC, where a write's op ran (as the VM finds it at the op). */
    for (size_t k = 0; k < program->runtime_objects.size(); k++) {
      if (program->runtime_after_run[k] != 0) {
        continue;
      }
      if (program->runtime_invariant[k] != 0) {
        std::fill_n(found.begin() + k * lanes.size(),
                    lanes.size(),
                    d.trees[lanes[0]]->ResolveObjectValueExpressionForColumns(
                        program->runtime_objects[k], context));
        continue;
      }
      for (size_t lane = 0; lane < lanes.size(); lane++) {
        found[k * lanes.size() + lane] = d.trees[lanes[lane]]->ResolveObjectValueExpressionForColumns(
            program->runtime_objects[k], context);
      }
    }
    /* Plan C1-9 E2: what each kind of use needs of the object (LN_ColumnProgram::runtime_*). */
    const bool frame = context.event == LN_Event::OnUpdate;
    bool lane_object = false;
    bool any_moved = false, any_unprotected = false;
    for (size_t lane = 0; lane < lanes.size() && !lane_object; lane++) {
      for (size_t k = 0; k < program->runtime_objects.size() && !lane_object; k++) {
        KX_GameObject *object = found[k * lanes.size() + lane];
        if (object == nullptr) {
          continue;
        }
        if (program->runtime_measured[k] != 0) {
          for (KX_GameObject *at = object; at != nullptr && !lane_object; at = at->GetParent()) {
            lane_object = d.lane_owners.Contains(at);
          }
          /* The run would apply its pending moves when it starts, the VM at an NPC's read: a
           * position read live elsewhere (an average) could see the difference. */
          lane_object = lane_object ||
                        (program->quiet_start && buffer.HasPendingTransforms(object, true));
        }
        if (program->runtime_moved[k] != 0) {
          any_moved = true;
          /* A lane's object moved by another lane: its own reads would see the move applied by
           * the VM's. In a Frame Update the VM refuses Apply Movement / Rotation of a dynamic
           * body (a warning, no command, no stamp). */
          lane_object = lane_object || d.lane_owners.Contains(object) ||
                        (frame && program->runtime_relative[k] != 0 && object->IsDynamic());
        }
        any_unprotected = any_unprotected || program->runtime_pose_unprotected[k] != 0;
      }
    }
    if (!lane_object && (any_moved || any_unprotected)) {
      /* The objects whose reads apply pending moves (the named ones, the measured ones), with
       * their parents: a read of one applies the moves of all of them. A moved object among
       * them would have the run's moves applied by a later NPC's read in the VM. And the run
       * applies the measured ones' pending moves when it starts: an object read live before a
       * sure distance to it, under one that has some, would see them only in the run. */
      std::unordered_set<KX_GameObject *> &read_chains = d.runtime_read_chains;
      std::unordered_set<KX_GameObject *> &applied_chains = d.runtime_applied_chains;
      read_chains.clear();
      applied_chains.clear();
      const auto add_chain = [](std::unordered_set<KX_GameObject *> &set, KX_GameObject *object) {
        for (KX_GameObject *at = object; at != nullptr && set.insert(at).second;
             at = at->GetParent())
        {
        }
      };
      for (const uint32_t value : program->applied_named) {
        add_chain(read_chains,
                  d.trees[lanes[0]]->ResolveObjectValueExpressionForColumns(value, context));
      }
      for (size_t k = 0; k < program->runtime_objects.size(); k++) {
        if (program->runtime_measured[k] == 0) {
          continue;
        }
        for (size_t lane = 0; lane < lanes.size(); lane++) {
          KX_GameObject *object = found[k * lanes.size() + lane];
          add_chain(read_chains, object);
          if (any_unprotected && object != nullptr && buffer.HasPendingTransforms(object, true)) {
            add_chain(applied_chains, object);
          }
        }
      }
      for (size_t k = 0; k < program->runtime_objects.size() && !lane_object; k++) {
        const bool moved = program->runtime_moved[k] != 0;
        const bool unprotected = program->runtime_pose_unprotected[k] != 0 &&
                                 !applied_chains.empty();
        if (!moved && !unprotected) {
          continue;
        }
        for (size_t lane = 0; lane < lanes.size() && !lane_object; lane++) {
          KX_GameObject *object = found[k * lanes.size() + lane];
          lane_object = moved && read_chains.count(object) != 0;
          for (KX_GameObject *at = object; unprotected && at != nullptr && !lane_object;
               at = at->GetParent())
          {
            lane_object = applied_chains.count(at) != 0;
          }
        }
      }
    }
    if (lane_object) {
      for (const uint32_t index : lanes) {
        d.GiveBack(index, buffer, context);
      }
      g_column_runtime_fallbacks.fetch_add(lanes.size(), std::memory_order_relaxed);
      d.given_back += lanes.size();
      return kNoRun;
    }
  }
  if (d.run_count == d.runs.size()) {
    d.runs.emplace_back();
  }
  Impl::Run &run = d.runs[d.run_count++];
  run.program = program;
  run.count = lanes.size();
  run.lane_trees.resize(lanes.size());
  for (size_t lane = 0; lane < lanes.size(); lane++) {
    run.lane_trees[lane] = d.trees[lanes[lane]];
  }
  run.lane_index.assign(lanes.begin(), lanes.end());
  run.context = &context;
  run.counts_valid = false; /* (CountRun, once its lanes are planned) */
  const size_t count = run.count;
  const size_t chunks = (count + kChunkLanes - 1) / kChunkLanes;
  run.masks.resize(program->writes.size() * count);
  run.superseded.assign(program->writes.size() * count, 0);
  run.lane_any.assign(count, 0); /* (each chunk sets its NPCs' when it ends) */
  run.floats.resize(size_t(program->out_float_columns) * count);
  run.rots.resize(size_t(program->out_rot_columns) * count);
  run.ints.resize(size_t(program->out_int_columns) * count);
  run.bools.resize(size_t(program->out_bool_columns) * count);
  run.chunk_tree_sets.assign(chunks, 0);
  if (program->bridged) {
    if (run.captures.size() < chunks) {
      run.captures.resize(chunks);
    }
    for (size_t chunk = 0; chunk < chunks; chunk++) {
      Impl::Run::Capture &capture = run.captures[chunk];
      if (capture.buffer == nullptr) {
        capture.buffer = std::make_unique<LN_CommandBuffer>();
      }
      capture.buffer->SetChunkTickBuffer(&buffer);
      /* The ops that act at once (Add Object makes its copy while the tree records) need the
       * scene's manager, as on the tick's buffer (plan C2 P1: runs of 1 brought the acceptance
       * probes' Add Object trees here, and every copy failed without it). */
      capture.buffer->SetLogicManager(buffer.GetLogicManager());
      capture.refs.clear();
      capture.superseded.clear();
    }
    run.captured_begin.assign(program->writes.size() * count, 0);
    run.captured_count.assign(program->writes.size() * count, 0);
  }
  /* The tick's values (the snapshot's timing getters read the tick read context). */
  run.uniforms[size_t(Uniform::UnscaledDeltaTime)] = float(std::max(0.0, context.unscaled_dt));
  run.uniforms[size_t(Uniform::DeltaTime)] = float(std::max(0.0, context.delta_time));
  run.uniforms[size_t(Uniform::DeltaTimeRaw)] = float(context.delta_time);
  /* LN_Snapshot::GetGravity: the tick read context's (Plan refuses a program without one). */
  if (d.read != nullptr && d.read->has_gravity) {
    run.uniforms[size_t(Uniform::GravityX)] = d.read->gravity.x();
    run.uniforms[size_t(Uniform::GravityY)] = d.read->gravity.y();
    run.uniforms[size_t(Uniform::GravityZ)] = d.read->gravity.z();
  }
  /* LN_Snapshot::CaptureSnapshot's timing: the context's, else its defaults (no engine). */
  run.uniforms[size_t(Uniform::ElapsedTime)] = 0.0f;
  run.uniforms[size_t(Uniform::FrameDelta)] = 0.0f;
  run.uniforms[size_t(Uniform::FPS)] = 0.0f;
  run.uniforms[size_t(Uniform::DeltaFactor)] = 1.0f;
  run.uniforms[size_t(Uniform::TimeScale)] = 1.0f;
  if (d.read != nullptr && d.read->has_timing) {
    run.uniforms[size_t(Uniform::ElapsedTime)] = d.read->elapsed_time;
    run.uniforms[size_t(Uniform::FrameDelta)] = d.read->frame_delta;
    run.uniforms[size_t(Uniform::FPS)] = d.read->fps;
    run.uniforms[size_t(Uniform::DeltaFactor)] = d.read->delta_factor;
    run.uniforms[size_t(Uniform::TimeScale)] = d.read->time_scale;
  }

  /* Every N Steps: which of the gates the distances are read behind let their routes run this
   * step, per NPC (the gates are made ready as ExecuteReady makes them, before its pass). */
  const bool fixed = context.event == LN_Event::OnFixedUpdate;
  uint64_t read_gates = 0;
  for (const LN_ColumnProgram::QueryRead &query : program->query_reads) {
    read_gates |= query.when.gates;
  }
  d.lane_due.assign(count, 0);
  if (read_gates != 0) {
    for (size_t lane = 0; lane < count; lane++) {
      LN_RuntimeTree *tree = run.lane_trees[lane];
      if (fixed && !tree->m_stepGates.empty()) {
        tree->PrepareStepGates(context.phase_tick_index, context.delta_time, context.unscaled_dt);
      }
      for (uint64_t rest = read_gates; rest != 0; rest &= rest - 1) {
        const uint32_t gate = uint32_t(__builtin_ctzll(rest));
        if (tree->StepGateRuns(gate)) {
          d.lane_due[lane] |= uint64_t(1) << gate;
        }
      }
    }
  }
  const auto reads = [&](const LN_ColumnProgram::QueryRead &query, const size_t lane) {
    return query.when.always || (query.when.gates & d.lane_due[lane]) != 0;
  };
  /* A read of the owner or of a named object applies its pending moves first (the VM's read,
   * FlushPendingRaycastTransforms), and only a read that happens: end A whenever the distance is
   * read, end B only when end A is valid. The run applies them before any NPC reads, which no NPC
   * can tell from applying them at its own read (no NPC of the run changes them meanwhile). */
  const bool can_flush = buffer.CanFlushPendingObjectTransformCommands();
  run.objects.assign(program->object_values.size(), NamedObject());
  std::vector<uint8_t> &flushed = d.object_flushed;
  flushed.assign(program->object_values.size(), 0);
  for (size_t k = 0; k < program->object_values.size(); k++) {
    run.objects[k].object = run.lane_trees[0]->ResolveObjectValueExpressionForColumns(
        program->object_values[k], context);
  }
  const auto flush_object = [&](const uint32_t k) {
    if (k < flushed.size() && flushed[k] == 0) {
      flushed[k] = 1;
      if (can_flush && run.objects[k].object != nullptr) {
        buffer.FlushPendingObjectHierarchyTransformCommands(run.objects[k].object);
      }
    }
  };
  for (const LN_ColumnProgram::QueryRead &query : program->query_reads) {
    if (query.a != EndKind::Object) {
      continue;
    }
    for (size_t lane = 0; lane < count; lane++) {
      if (reads(query, lane)) {
        flush_object(query.a_object);
        break;
      }
    }
  }
  if (can_flush) {
    for (size_t lane = 0; lane < count; lane++) {
      for (const LN_ColumnProgram::QueryRead &query : program->query_reads) {
        if (query.a == EndKind::Owner && reads(query, lane)) {
          buffer.FlushPendingObjectHierarchyTransformCommands(run.lane_trees[lane]->GetGameObject());
          break;
        }
      }
    }
  }
  const auto a_valid = [&](const LN_ColumnProgram::QueryRead &query, const size_t lane) {
    switch (query.a) {
      case EndKind::Owner:
        return finite3(run.lane_trees[lane]->GetGameObject()->NodeGetWorldPosition());
      case EndKind::Object: {
        KX_GameObject *object = run.objects[query.a_object].object;
        return object != nullptr && finite3(object->NodeGetWorldPosition());
      }
      default:
        return false;
    }
  };
  for (const LN_ColumnProgram::QueryRead &query : program->query_reads) {
    if (query.b != EndKind::Object || flushed[query.b_object] != 0) {
      continue;
    }
    for (size_t lane = 0; lane < count; lane++) {
      if (reads(query, lane) && a_valid(query, lane)) {
        flush_object(query.b_object);
        break;
      }
    }
  }
  /* The objects found while playing: a distance read applies their pending moves (each NPC's own
   * read, end B only when end A is valid), then their positions as the reads give them. */
  run.runtime.assign(program->runtime_objects.size() * count, NamedObject());
  run.target_objects.assign(size_t(program->target_columns) * count, nullptr);
  /* Helpers may find objects by name (OTreeObj, bridged kinds): the lookup is built here. */
  run.lane_trees[0]->PrepareObjectNameLookup();
  for (size_t k = 0; k < program->runtime_objects.size(); k++) {
    for (size_t lane = 0; lane < count; lane++) {
      run.runtime[k * count + lane].object = found[k * count + lane];
    }
  }
  if (can_flush && !program->runtime_objects.empty()) {
    std::unordered_set<KX_GameObject *> applied;
    for (const LN_ColumnProgram::QueryRead &query : program->query_reads) {
      for (size_t lane = 0; lane < count; lane++) {
        if (!reads(query, lane)) {
          continue;
        }
        KX_GameObject *a = query.a == EndKind::Runtime ?
                               run.runtime[size_t(query.a_object) * count + lane].object :
                               nullptr;
        if (a != nullptr && applied.insert(a).second) {
          buffer.FlushPendingObjectHierarchyTransformCommands(a);
        }
        if (query.b != EndKind::Runtime) {
          continue;
        }
        KX_GameObject *b = run.runtime[size_t(query.b_object) * count + lane].object;
        const bool a_ok = query.a == EndKind::Runtime ?
                              a != nullptr && finite3(a->NodeGetWorldPosition()) :
                              a_valid(query, lane);
        if (b != nullptr && a_ok && applied.insert(b).second) {
          buffer.FlushPendingObjectHierarchyTransformCommands(b);
        }
      }
    }
  }
  for (size_t k = 0; k < program->runtime_objects.size(); k++) {
    NamedObject *named = run.runtime.data() + k * count;
    if (program->runtime_invariant[k] != 0 && count != 0) {
      /* One object for every NPC: its position once (nothing moves it meanwhile). */
      if (named[0].object != nullptr) {
        named[0].position = named[0].object->NodeGetWorldPosition();
        named[0].ok = finite3(named[0].position);
        std::fill(named + 1, named + count, named[0]);
      }
      continue;
    }
    for (size_t lane = 0; lane < count; lane++) {
      if (named[lane].object != nullptr) {
        named[lane].position = named[lane].object->NodeGetWorldPosition();
        named[lane].ok = finite3(named[lane].position);
      }
    }
  }
  for (NamedObject &named : run.objects) {
    if (named.object != nullptr) {
      named.position = named.object->NodeGetWorldPosition();
      named.ok = finite3(named.position);
    }
  }
  /* Velocities, here on the main thread (the VM reads them where the tree records: nothing of
   * the run changes a velocity before the flush). */
  /* The averages whose source is the same for every NPC: once, by the VM's own code (nothing the
   * run's trees read changes position while they run: the checks above). */
  run.averages.resize(program->average_reads.size());
  for (size_t k = 0; k < program->average_reads.size(); k++) {
    run.averages[k] = run.lane_trees[0]->ColumnVectorAverage(program->average_reads[k], context);
  }
  /* Plan C2 fix 1: another object's game properties, once by the VM's own code (a property
   * changes only at the flush). */
  run.invariants.resize(program->run_invariants.size());
  for (size_t k = 0; k < program->run_invariants.size(); k++) {
    const LN_ColumnProgram::RunInvariant &read = program->run_invariants[k];
    Impl::Run::Invariant &value = run.invariants[k];
    switch (read.family) {
      case 'F':
        value.f = run.lane_trees[0]->ColumnInvariantFloat(read.index, read.float_fallback, context);
        break;
      case 'I':
        value.i = run.lane_trees[0]->ColumnInvariantInt(read.index, read.int_fallback, context);
        break;
      default:
        value.b = run.lane_trees[0]->ColumnInvariantBool(read.index, read.bool_fallback, context) ?
                      1 :
                      0;
        break;
    }
  }
  run.velocities.resize(program->velocity_reads.size() * count);
  run.velocity_ok.resize(program->velocity_reads.size() * count);
  for (size_t v = 0; v < program->velocity_reads.size(); v++) {
    const LN_ColumnProgram::VelocityRead &velocity = program->velocity_reads[v];
    MT_Vector3 *values = run.velocities.data() + v * count;
    uint8_t *ok = run.velocity_ok.data() + v * count;
    const auto read = [&](KX_GameObject *object, MT_Vector3 &r_vector) {
      return object != nullptr &&
             LN_RuntimeTree::ColumnObjectAttributeVector(*object, velocity.attribute, r_vector);
    };
    if (velocity.owner) {
      for (size_t lane = 0; lane < count; lane++) {
        ok[lane] = read(run.lane_trees[lane]->GetGameObject(), values[lane]) ? 1 : 0;
      }
    }
    else {
      MT_Vector3 vector(0.0f, 0.0f, 0.0f);
      const uint8_t found = read(run.objects[velocity.object].object, vector) ? 1 : 0;
      std::fill_n(values, count, vector);
      std::fill_n(ok, count, found);
    }
  }

  return d.run_count - 1;
}

size_t LN_ColumnRunner::Impl::CheckPendingDepends(const std::vector<uint32_t> &lanes,
                                                  LN_CommandBuffer &buffer,
                                                  const LN_TickContext &context,
                                                  const LN_ColumnProgram &program)
{
  /* The objects the lanes' poses depend on: each lane's own object and its ancestors. With roots
   * that no program names (every lane before plan C2 P2) nothing the start applies can move a
   * lane: a lane's own chain only moves it, and a named object's chain holds no lane. A parented
   * lane's ancestor or a named lane breaks that: applied at the start, its pending moves would
   * move the lane before reads the VM makes with them still pending. */
  std::unordered_map<KX_GameObject *, uint32_t> &depends = lane_depends;
  depends.clear();
  for (const uint32_t index : lanes) {
    for (KX_GameObject *at = trees[index]->GetGameObject(); at != nullptr; at = at->GetParent()) {
      depends[at]++;
    }
  }
  bool pending = false;
  for (const auto &entry : depends) {
    if (buffer.HasPendingTransforms(entry.first, false)) {
      pending = true;
      break;
    }
  }
  if (!pending) {
    return 0;
  }
  /* The start applies nothing (no query, no object found while playing, no average, no quiet
   * start): the lanes read their poses with the moves pending, as the VM's trees do (their reads
   * apply none). A bridged program's own checks give its lanes back when a read of the VM's code
   * would apply them (RunSetBegin, the lanes' and named objects' chains). */
  const bool start_applies = !program.query_reads.empty() || !program.runtime_objects.empty() ||
                             !program.average_reads.empty() || program.live_positions ||
                             program.quiet_start;
  if (!start_applies) {
    return 0;
  }
  if (lanes.size() > 1) {
    return kSplitRun;
  }
  /* A run of 1: its owner's chain is applied where the VM's first read of a distance from the
   * owner applies it (the compiler puts every live read of the owner's pose after it). Any other
   * chain the start applies, and any object whose pose the lane reads as the start leaves it,
   * must hold no object the lane depends on that has pending moves. */
  if (!program.runtime_objects.empty() || !program.average_reads.empty() ||
      program.live_positions || program.quiet_start)
  {
    return kNoRun;
  }
  LN_RuntimeTree *tree = trees[lanes[0]];
  for (const uint32_t value : program.object_values) {
    for (KX_GameObject *at = tree->ResolveObjectValueExpressionForColumns(value, context);
         at != nullptr;
         at = at->GetParent())
    {
      if (depends.count(at) != 0 && buffer.HasPendingTransforms(at, false)) {
        return kNoRun;
      }
    }
  }
  return 0;
}

void LN_ColumnRunner::Impl::RunSetChunks(const size_t run_index,
                                         LN_CommandBuffer &buffer,
                                         LN_JobPool *pool)
{
  Impl &d = *this;
  Impl::Run &run = d.runs[run_index];
  const LN_ColumnProgram *program = run.program;
  const size_t count = run.count;
  const size_t chunks = (count + kChunkLanes - 1) / kChunkLanes;
  const auto run_chunk = [&](const size_t chunk) {
    const size_t first = chunk * kChunkLanes;
    d.RunChunk(*program, run, first, std::min(kChunkLanes, count - first));
  };
  const bool helpers = pool != nullptr && pool->GetHelperCount() > 0 && chunks >= 2;
  if (helpers && program->bridged && column_segments_on()) {
    /* Plan C1-9: the nodes in segments. The helpers work through a segment of nodes whose code
     * may run there, chunk by chunk; a node only this thread may run (Node::main_thread: the
     * UI, input, collisions, physics service ops, Add Object...) runs here over every chunk in
     * lane order before the next segment. A lane's nodes still run in its program's order, and
     * no lane reads another's state, so the order between lanes changes nothing; the VM's code
     * the helpers run reads the buffers' pending moves, made here first. */
    buffer.PrepareForChunkReads();
    d.LaneMap();
    while (d.chunk_scratches.size() < chunks) {
      d.chunk_scratches.push_back(std::make_unique<ChunkScratch>());
    }
    /* A lane whose object queries its collisions on demand: collision reads stay here. */
    bool collisions_here = false;
    for (size_t lane = 0; lane < count && program->collision_reads && !collisions_here; lane++) {
      const PHY_IPhysicsController *controller =
          run.lane_trees[lane]->GetGameObject()->GetPhysicsController();
      collisions_here = controller != nullptr && controller->LogicCollisionsQueriedOnDemand();
    }
    const auto on_main = [&](const Node &n) {
      return n.main_thread || (n.collision_reads && collisions_here);
    };
    const size_t node_count = program->nodes.size();
    size_t node = 0;
    do {
      const bool main = node < node_count && on_main(program->nodes[node]);
      size_t end = node;
      while (end < node_count && on_main(program->nodes[end]) == main) {
        end++;
      }
      const uint8_t parts = uint8_t((node == 0 ? Impl::kPartBegin : 0) |
                                    (end == node_count ? Impl::kPartEnd : 0));
      const auto part = [&](const size_t chunk) {
        const size_t first = chunk * kChunkLanes;
        d.RunChunkPart(*program,
                       run,
                       first,
                       std::min(kChunkLanes, count - first),
                       *d.chunk_scratches[chunk],
                       node,
                       end,
                       parts);
      };
      if (main) {
        for (size_t chunk = 0; chunk < chunks; chunk++) {
          part(chunk);
        }
        g_column_main_segments.fetch_add(1, std::memory_order_relaxed);
      }
      else {
        pool->Run(chunks, [&](const size_t chunk, uint32_t /*thread*/) { part(chunk); });
        g_column_helper_segments.fetch_add(1, std::memory_order_relaxed);
      }
      node = end;
    } while (node < node_count);
  }
  /* A bridged program that is not batch safe runs its chunks here (the VM's code it runs may
   * read what only this thread may: physics queries, the UI, input). */
  else if (helpers && !(program->bridged && program->on_main_thread)) {
    pool->Run(chunks, [&](const size_t chunk, uint32_t /*thread*/) { run_chunk(chunk); });
  }
  else {
    for (size_t chunk = 0; chunk < chunks; chunk++) {
      run_chunk(chunk);
    }
  }
}

void LN_ColumnRunner::Impl::RunSetEnd(const size_t run_index,
                                      LN_CommandBuffer &buffer,
                                      const LN_TickContext &context)
{
  Impl &d = *this;
  Impl::Run &run = d.runs[run_index];
  const LN_ColumnProgram *program = run.program;
  const size_t count = run.count;
  /* Each lane's writes now behave as its tree's recorded commands: the flush applies them at its
   * sort key, and a read may apply its moves early (the pending mark says there are some). */
  const size_t runtime_count = program->runtime_objects.size();
  std::vector<uint8_t> &found_after = d.runtime_found_after;
  for (size_t lane = 0; lane < count; lane++) {
    Impl::LaneInfo &info = d.info[run.lane_index[lane]];
    info.run = uint32_t(run_index);
    info.offset = uint32_t(lane);
    if (run.lane_any[lane] == 0) {
      continue; /* no write ran: nothing found after the run, no move pending */
    }
    KX_GameObject *owner = run.lane_trees[lane]->GetGameObject();
    if (runtime_count != 0) {
      found_after.assign(runtime_count, 0);
    }
    for (size_t w = 0; w < program->writes.size(); w++) {
      const Write &write = program->writes[w];
      uint8_t &mask = run.masks[w * count + lane];
      if (mask == 0) {
        continue;
      }
      if (write.kind != WriteKind::Transform) {
        if (write.target_column != kNone) {
          /* Plan C1-11: the object a tree property held where the op ran. */
          KX_GameObject *target = d.WriteTarget(run, write, lane, owner);
          if (target == nullptr) {
            mask = 0; /* the VM's op found no object: it stamped the op and recorded nothing */
          }
          else if (write.kind == WriteKind::Number) {
            d.NoteWrittenKey(
                Impl::NumberWriteKey(run.lane_trees[lane], target, write.property_ref));
          }
          continue;
        }
        if (write.runtime_target == kNone || write.kind == WriteKind::Captured) {
          continue;
        }
        /* Plan C1-10 / C1-11: a property write, a motion or an object state command of an object
         * found while playing. One only written is found here by the VM's own code, where its op
         * ran (the tree's collision bookkeeping as the VM's find at the op leaves it: nothing the
         * find reads changed since). */
        const size_t k = write.runtime_target;
        NamedObject &named = run.runtime[k * count + lane];
        if (program->runtime_after_run[k] != 0 && found_after[k] == 0) {
          found_after[k] = 1;
          named.object = run.lane_trees[lane]->ResolveObjectValueExpressionForColumns(
              program->runtime_objects[k], context);
        }
        if (named.object == nullptr) {
          mask = 0; /* the VM's op found no object: it stamped the op and recorded nothing */
          continue;
        }
        /* The object's own write of it may merge with this one at the flush (ListWrites); a
         * property change ends any run of merged writes (listed as found-object writes are). */
        if (write.kind == WriteKind::Number) {
          d.NoteWrittenKey(
              Impl::NumberWriteKey(run.lane_trees[lane], named.object, write.property_ref));
        }
        continue;
      }
      KX_GameObject *target = d.WriteTarget(run, write, lane, owner);
      if (target == nullptr) {
        /* The VM's op found no object: it stamped the op and recorded nothing. */
        mask = 0;
        continue;
      }
      if (write.is_move) {
        info.moves_pending = true;
        buffer.NoteExternalPendingMove(target);
      }
    }
  }
  d.CountRun(run);
  d.order_built = false;
  d.runs_done = run_index + 1; /* (its lanes join the lane map when it is next asked for) */
  d.lanes_ran += count; /* (LN_ColumnLaneCount at EndTick: one atomic a tick, not a run) */
  d.runs_not_plain += program->lists_unshared_last_write_wins ? 1 : 0;
  d.runs_last_write_wins_number += program->any_last_write_wins_number ? 1 : 0;
}

void LN_ColumnRunner::Impl::ApplyLane(const uint32_t index,
                                      const bool moves,
                                      const bool others,
                                      LN_CommandBuffer *buffer,
                                      KX_GameObject *const *only,
                                      const size_t only_count)
{
  const LaneInfo &lane_info = info[index];
  Run &run = runs[lane_info.run];
  const size_t lane = lane_info.offset;
  if (run.lane_any[lane] == 0) {
    return; /* no write ran */
  }
  const LN_ColumnProgram &program = *run.program;
  const size_t count = run.count;
  LN_RuntimeTree *tree = trees[index];
  KX_GameObject *owner = tree->GetGameObject();
  /* In op order, as the flush applies one tree's writes (sort key, then record order). */
  for (size_t w = 0; w < program.writes.size(); w++) {
    uint8_t &mask = run.masks[w * count + lane];
    if (mask == 0 || run.superseded[w * count + lane] != 0) {
      continue;
    }
    const Write &write = program.writes[w];
    if (lane_info.own_properties_applied && IsOwnPropertyWrite(write)) {
      continue; /* applied ahead (PrepareApply) */
    }
    const bool is_move = write.kind == WriteKind::Transform && write.is_move;
    if (is_move ? !moves : !others) {
      continue;
    }
    /* The object it writes: the owner, or one the NPC found (plan C1-9 E2). */
    KX_GameObject *target = WriteTarget(run, write, lane, owner);
    if (only != nullptr && std::find(only, only + only_count, target) == only + only_count) {
      continue; /* a read's early apply of other objects' moves */
    }
    if (write.kind == WriteKind::Captured) {
      /* The commands the VM's handler recorded, by the flush's own code (it checks their
       * objects: a removed one's are left out). */
      if (buffer != nullptr) {
        const size_t cell = w * count + lane;
        const Run::Capture &capture = run.captures[lane / kChunkLanes];
        for (uint32_t k = 0; k < run.captured_count[cell]; k++) {
          const size_t at = run.captured_begin[cell] + k;
          if (!capture.Superseded(at)) {
            buffer->ApplyForeignCommand(*capture.buffer, capture.refs[at]);
          }
        }
      }
      continue;
    }
    /* A Remove Object earlier in the flush (another tree's, or a bridged op of this one): the
     * object's own writes are not applied, as the flush leaves out its commands. */
    if (buffer != nullptr && buffer->ObjectRemovedDuringFlush(target)) {
      continue;
    }
    if (is_move && !others) {
      mask = 0; /* a read's early apply: the write is done (counted and listed no more) */
      run.counts_valid = false;
    }
    const auto kept_float = [&](const uint32_t column) {
      return run.floats[size_t(column) * count + lane];
    };
    switch (write.kind) {
      case WriteKind::Number: {
        /* LeanSetNumberBody's handle: only of the snapshot's object (none for another one). */
        uint64_t stamp = 0;
        EXP_Value *handle = tree->GetSnapshot().GetGamePropertyHandle(
            target, write.property_ref, stamp);
        LN_CommandBuffer::ApplyNumberWriteNow(tree,
                                              target,
                                              program.property_names[write.property_ref],
                                              NumberOf(run, write, lane),
                                              handle,
                                              stamp);
        break;
      }
      case WriteKind::Modify: {
        LN_CommandBuffer::ApplyModifyNow(
            target, program.property_names[write.property_ref], ModifyOf(run, write, lane));
        break;
      }
      case WriteKind::Service: {
        const bool visible = write.command == LN_CommandBuffer::CommandType::SetVisibility &&
                             run.bools[size_t(write.out_bool) * count + lane] != 0;
        const bool children = write.command == LN_CommandBuffer::CommandType::SetVisibility &&
                              run.bools[size_t(write.out_bool + 1) * count + lane] != 0;
        MT_Vector4 color(0.0f, 0.0f, 0.0f, 1.0f);
        if (write.command == LN_CommandBuffer::CommandType::SetObjectColor) {
          color = MT_Vector4(kept_float(write.out_float),
                             kept_float(write.out_float + 1),
                             kept_float(write.out_float + 2),
                             kept_float(write.out_float + 3));
        }
        LN_CommandBuffer::ApplyObjectStateNow(write.command, target, color, visible, children);
        break;
      }
      case WriteKind::Motion: {
        if (buffer == nullptr) {
          break; /* (never: motions are no moves, so an early apply never reaches them) */
        }
        const auto kept_int = [&](const uint32_t column) {
          return run.ints[size_t(column) * count + lane];
        };
        buffer->ApplyMotionNow(write.command,
                               target,
                               MT_Vector3(kept_float(write.out_float),
                                          kept_float(write.out_float + 1),
                                          kept_float(write.out_float + 2)),
                               MT_Vector3(kept_float(write.out_float + 3),
                                          kept_float(write.out_float + 4),
                                          kept_float(write.out_float + 5)),
                               kept_float(write.out_float + 6),
                               kept_int(write.out_int),
                               kept_int(write.out_int + 1),
                               write.motion_bool,
                               write.motion_secondary_bool);
        break;
      }
      case WriteKind::Transform: {
        const bool local = write.local != kNone ?
                               run.bools[size_t(write.out_bool) * count + lane] != 0 :
                               write.local_constant;
        MT_Vector3 value(0.0f, 0.0f, 0.0f);
        if (!write.is_rotation) {
          value = MT_Vector3(kept_float(write.out_float),
                             kept_float(write.out_float + 1),
                             kept_float(write.out_float + 2));
        }
        const LN_CommandBuffer::CommandType type = write.type_from_local && local ?
                                                       write.local_command :
                                                       write.command;
        LN_CommandBuffer::ApplyTransformNow(
            type,
            target,
            value,
            write.out_rot != kNone ? run.rots[size_t(write.out_rot) * count + lane] : write.rotation,
            local);
        break;
      }
    }
  }
}

bool LN_ColumnRunner::HasWrites() const
{
  const Impl &d = *m_impl;
  if (d.run_count == 0) {
    return false;
  }
  return !d.order_built || d.order_next < d.order.size();
}

/** UPBGE_LN_COLUMN_COUNT_VERIFY=1: CountWrites also walks every lane and warns when the runs' own
 * counts (plan C1-11 C-1) differ ("Logic Nodes: columns: write counts differ"). */
static bool column_count_verify()
{
  const int override_mode = g_column_count_verify_override.load(std::memory_order_relaxed);
  if (override_mode >= 0) {
    return override_mode != 0;
  }
  static const bool verify = [] {
    const char *value = std::getenv("UPBGE_LN_COLUMN_COUNT_VERIFY");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
  }();
  return verify;
}

void LN_ColumnRunner::CountWrites(uint32_t &r_total, uint32_t &r_last_write_wins) const
{
  const Impl &d = *m_impl;
  r_total = 0;
  r_last_write_wins = 0;
  /* One lane's writes left to apply, as their recorded commands would count. */
  const auto count_lane = [&](const Impl::LaneInfo &info, uint32_t &total, uint32_t &lww) {
    const Impl::Run &run = d.runs[info.run];
    if (run.lane_any[info.offset] == 0) {
      return; /* no write ran */
    }
    const LN_ColumnProgram &program = *run.program;
    for (size_t w = 0; w < program.writes.size(); w++) {
      const size_t at = w * run.count + info.offset;
      if (run.masks[at] == 0 || run.superseded[at] != 0) {
        continue;
      }
      const Write &write = program.writes[w];
      if (write.kind == WriteKind::Captured) {
        const Impl::Run::Capture &capture = run.captures[info.offset / kChunkLanes];
        for (uint32_t k = 0; k < run.captured_count[at]; k++) {
          const size_t ref = run.captured_begin[at] + k;
          if (capture.Superseded(ref)) {
            continue;
          }
          total++;
          lww += capture.buffer->CommandIsLastWriteWins(capture.refs[ref]) ? 1 : 0;
        }
        continue;
      }
      total++;
      lww += Impl::WriteIsLastWriteWins(run, write, info.offset) ? 1 : 0;
    }
  };
  /* Plan C1-11 C-1: the runs' own counts while they hold (the flush counts before it applies or
   * merges anything; a read's early apply or a merge mark invalidates its run's). */
  const bool by_runs = !d.any_lane_written;
  bool all_valid = by_runs;
  if (by_runs) {
    for (size_t r = 0; r < d.run_count; r++) {
      if (d.runs[r].counts_valid) {
        r_total += d.runs[r].count_total;
        r_last_write_wins += d.runs[r].count_last_write_wins;
      }
      else {
        all_valid = false;
      }
    }
  }
  if (!all_valid) {
    for (size_t index = 0; index < d.info.size(); index++) {
      const Impl::LaneInfo &info = d.info[index];
      if (info.run == kNone || info.written || (by_runs && d.runs[info.run].counts_valid)) {
        continue;
      }
      count_lane(info, r_total, r_last_write_wins);
    }
  }
  if (by_runs && column_count_verify()) {
    uint32_t total = 0, lww = 0;
    for (size_t index = 0; index < d.info.size(); index++) {
      const Impl::LaneInfo &info = d.info[index];
      if (info.run != kNone && !info.written) {
        count_lane(info, total, lww);
      }
    }
    if (total != r_total || lww != r_last_write_wins) {
      g_column_count_mismatches.fetch_add(1, std::memory_order_relaxed);
      CM_Warning("Logic Nodes: columns: write counts differ (runs "
                 << r_total << " / " << r_last_write_wins << ", lanes " << total << " / " << lww
                 << ")");
    }
  }
  /* The trees' Set Tree Property commands (applied at once; the flush would apply them again). */
  const bool tree_sets_merge = is_last_write_wins(LN_CommandBuffer::CommandType::SetTreeProperty);
  for (size_t r = 0; r < d.run_count; r++) {
    for (const uint32_t sets : d.runs[r].chunk_tree_sets) {
      r_total += sets;
      r_last_write_wins += tree_sets_merge ? sets : 0;
    }
  }
}

size_t LN_ColumnRunner::ListWrites(const std::vector<const void *> &buffer_objects,
                                   const bool buffer_can_merge)
{
  Impl &d = *m_impl;
  d.BuildOrder();
  d.list_keys.clear();
  d.list_refs.clear();
  using CT = LN_CommandBuffer::CommandType;
  /* The bridged ops' last-write-wins commands (plan C2 P0): the merge takes each with the key its
   * buffer gives it, names included, as it takes the VM's recorded command (it was a run's end, so
   * a write the VM's merge leaves out was applied too: a failing one warned, a property two writes
   * create in one step could get the other number type). */
  d.captured_objects.clear();
  bool any_captured_lww = false;
  for (size_t r = 0; r < d.run_count; r++) {
    const Impl::Run &run = d.runs[r];
    const LN_ColumnProgram &program = *run.program;
    if (!program.captured_writes) {
      continue;
    }
    for (size_t w = 0; w < program.writes.size(); w++) {
      if (program.writes[w].kind != WriteKind::Captured) {
        continue;
      }
      for (size_t lane = 0; lane < run.count; lane++) {
        const size_t cell = w * run.count + lane;
        if (run.masks[cell] == 0 || run.superseded[cell] != 0) {
          continue;
        }
        const Impl::Run::Capture &capture = run.captures[lane / kChunkLanes];
        for (uint32_t k = 0; k < run.captured_count[cell]; k++) {
          const uint64_t ref = capture.refs[run.captured_begin[cell] + k];
          if (!capture.buffer->CommandIsLastWriteWins(ref)) {
            continue;
          }
          if (!any_captured_lww) {
            any_captured_lww = true;
            d.lane_captured_lww.assign(d.info.size(), 0);
          }
          d.lane_captured_lww[run.lane_index[lane]] = 1;
          if (const void *object = capture.buffer->CommandMergeObject(ref)) {
            d.captured_objects.push_back(object);
          }
        }
      }
    }
  }
  if (!d.captured_objects.empty()) {
    std::sort(d.captured_objects.begin(), d.captured_objects.end());
    d.captured_objects.erase(std::unique(d.captured_objects.begin(), d.captured_objects.end()),
                             d.captured_objects.end());
  }
  /* Lists lane `index`'s writes the merge can see; `dry`: lists nothing, only says whether a
   * last-write-wins write would be listed. */
  const auto list_lane = [&](const uint32_t index, const bool dry) -> bool {
    const Impl::LaneInfo &info = d.info[index];
    const Impl::Run &run = d.runs[info.run];
    if (run.lane_any[info.offset] == 0) {
      return false; /* no write ran: nothing to list (the VM's tree recorded nothing) */
    }
    const LN_ColumnProgram &program = *run.program;
    LN_RuntimeTree *tree = d.trees[index];
    const void *owner = tree->GetGameObject();
    /* A bridged op of the lane recorded a last-write-wins command: listed command by command. */
    const bool captured_lww = any_captured_lww && d.lane_captured_lww[index] != 0;
    /* The buffer or a bridged op's command writes this object too, or the lane has such a
     * command: every write of the tree may merge with one. */
    const bool shared = captured_lww ||
                        (d.list_all_lanes != 0 && (d.lane_flags[index] & Impl::kLaneListAll) != 0) ||
                        (!buffer_objects.empty() &&
                         std::binary_search(buffer_objects.begin(), buffer_objects.end(), owner)) ||
                        (!d.captured_objects.empty() &&
                         std::binary_search(
                             d.captured_objects.begin(), d.captured_objects.end(), owner));
    const bool lists_all = shared || program.any_collide;
    /* Plan C1-11 C-2: a lane none of whose writes can be listed as last-write-wins lists only its
     * first write that ends a run (the general walk below lists exactly that one). */
    const bool plain = !lists_all && !program.any_found_target_write &&
                       (d.written_keys.empty() || !program.any_last_write_wins_number);
    if (plain && dry) {
      return false;
    }
    bool listed_break = false;
    for (size_t w = 0; w < program.writes.size(); w++) {
      const size_t cell = w * run.count + info.offset;
      if (run.masks[cell] == 0 || run.superseded[cell] != 0) {
        continue;
      }
      const Write &write = program.writes[w];
      if (write.kind == WriteKind::Captured && captured_lww) {
        /* Its commands one by one, in record order: a last-write-wins one with its buffer's key,
         * any other as a run's end. */
        const Impl::Run::Capture &capture = run.captures[info.offset / kChunkLanes];
        for (uint32_t k = 0; k < run.captured_count[cell]; k++) {
          const uint64_t ref = capture.refs[run.captured_begin[cell] + k];
          const bool last_write_wins = capture.buffer->CommandIsLastWriteWins(ref);
          if (dry) {
            if (last_write_wins) {
              return true;
            }
            continue;
          }
          LN_ExternalWriteKey key;
          key.sort_key = info.sort_key;
          key.type = uint8_t(CT::RemoveObject);
          key.last_write_wins = last_write_wins;
          if (last_write_wins) {
            key.foreign_buffer = capture.buffer.get();
            key.foreign_ref = ref;
          }
          d.list_keys.push_back(key);
          d.list_refs.push_back({index, uint32_t(w), k});
        }
        continue;
      }
      /* The command it stands for and whether that merges, before anything else (most writes
       * are left out). */
      CT type = CT::SetGameProperty;
      bool last_write_wins = false;
      switch (write.kind) {
        case WriteKind::Number:
          last_write_wins = write.last_write_wins;
          break;
        case WriteKind::Modify:
          type = CT::ModifyGameProperty; /* key_of ends the run at any other property command */
          break;
        case WriteKind::Motion:
          type = write.command; /* key_of has no key for the motion stream: ends the run */
          break;
        case WriteKind::Service:
          type = write.command; /* key_of has no key for object service commands: ends the run */
          break;
        case WriteKind::Captured:
          /* A bridged op's commands end the run (merged by nothing here; the flush's result is the
           * same: a last-write-wins write it would leave out is overwritten by the later one). */
          type = CT::RemoveObject;
          break;
        case WriteKind::Transform: {
          const bool local = write.local != kNone ?
                                 run.bools[size_t(write.out_bool) * run.count + info.offset] != 0 :
                                 write.local_constant;
          type = write.type_from_local && local ? write.local_command : write.command;
          last_write_wins = write.type_from_local && local ? write.local_last_write_wins :
                                                             write.last_write_wins;
          break;
        }
      }
      if (plain && last_write_wins) {
        continue;
      }
      /* A write of an object the NPC found (plan C1-9 E2) may share its key with anything
       * (another NPC's write of it, the buffer's): always listed. So is an own property write
       * another lane writes the property of this object with (plan C1-10). */
      const bool found_target = write.runtime_target != kNone || write.target_column != kNone;
      const bool written_by_others = !found_target && last_write_wins &&
                                     write.kind == WriteKind::Number && !d.written_keys.empty() &&
                                     d.WrittenByOthers(tree, tree->GetGameObject(),
                                                       write.property_ref);
      if (found_target || written_by_others) {
        /* After a listed last-write-wins write the tree's next run end must be listed too. */
        listed_break = !last_write_wins;
      }
      else if (!lists_all) {
        /* Nothing can share the key of a last-write-wins write here; a run's end is one end. */
        if (last_write_wins || listed_break) {
          continue;
        }
        listed_break = true;
      }
      else if (last_write_wins && !shared && !write.may_collide) {
        continue;
      }
      if (dry) {
        if (last_write_wins) {
          return true;
        }
        continue;
      }
      LN_ExternalWriteKey key;
      key.sort_key = info.sort_key;
      key.object = Impl::WriteTarget(run, write, info.offset, tree->GetGameObject());
      key.type = uint8_t(type);
      key.last_write_wins = last_write_wins;
      if (write.kind == WriteKind::Number) {
        /* AppendSetGamePropertyNumberRef's command: keyed by the property's id, else by the ref. */
        const LN_GamePropertyId id = tree->GetGamePropertyId(write.property_ref);
        if (id.IsValid()) {
          key.property_id = id.index;
        }
        else {
          key.runtime_tree = tree;
          key.property_ref_index = write.property_ref;
        }
      }
      d.list_keys.push_back(key);
      d.list_refs.push_back({index, uint32_t(w), kNone});
      if (plain) {
        break; /* its one run end */
      }
    }
    return false;
  };
  if (!buffer_can_merge) {
    /* Plan C1-11 C-2: no two of the buffer's own writes can merge, so only a listed
     * last-write-wins write of a lane could: without one the merge has nothing to do, and the
     * run ends need not be listed. Plan C2 P1: without a buffer write on a lane's object, a
     * bridged last-write-wins command or a run of a program that may list one, every lane is
     * plain: nothing to ask lane by lane (the dry walk cost ~6 ns a lane in runs of 1). */
    if (buffer_objects.empty() && !any_captured_lww && d.runs_not_plain == 0 &&
        d.list_all_lanes == 0 && (d.written_keys.empty() || d.runs_last_write_wins_number == 0))
    {
      return 0;
    }
    bool any = false;
    for (size_t at = d.order_next; at < d.order.size() && !any; at++) {
      any = list_lane(d.order[at], true);
    }
    if (!any) {
      return 0;
    }
  }
  for (size_t at = d.order_next; at < d.order.size(); at++) {
    list_lane(d.order[at], false);
  }
  return d.list_keys.size();
}

const LN_ExternalWriteKey &LN_ColumnRunner::WriteKey(const size_t index) const
{
  return m_impl->list_keys[index];
}

void LN_ColumnRunner::MarkSuperseded(const size_t index)
{
  Impl &d = *m_impl;
  const Impl::ListRef ref = d.list_refs[index];
  const Impl::LaneInfo &info = d.info[ref.lane];
  Impl::Run &run = d.runs[info.run];
  const size_t cell = size_t(ref.write) * run.count + info.offset;
  if (ref.command != kNone) {
    /* One command of a bridged op (plan C2 P0). */
    Impl::Run::Capture &capture = run.captures[info.offset / kChunkLanes];
    if (capture.superseded.size() < capture.refs.size()) {
      capture.superseded.resize(capture.refs.size(), 0);
    }
    capture.superseded[run.captured_begin[cell] + ref.command] = 1;
  }
  else {
    run.superseded[cell] = 1;
  }
  run.counts_valid = false;
}

void LN_ColumnRunner::ApplyWritesBefore(const uint64_t sort_key, LN_CommandBuffer &buffer)
{
  Impl &d = *m_impl;
  d.BuildOrder();
  while (d.order_next < d.order.size() && d.info[d.order[d.order_next]].sort_key < sort_key) {
    const uint32_t index = d.order[d.order_next++];
    Impl::LaneInfo &info = d.info[index];
    info.written = true;
    if (d.runs[info.run].lane_any[info.offset] == 0) {
      continue; /* no write ran: nothing to apply, and nothing the runs' counts would miss */
    }
    /* (A Remove Object earlier in the flush: ApplyLane leaves out the object's own writes; the
     * commands a bridged op caught for other objects apply, each checking its own.) */
    d.ApplyLane(index, info.moves_pending, true, &buffer);
    info.moves_pending = false;
    d.any_lane_written = true;
  }
}

bool LN_ColumnRunner::Impl::PreApplyLane(const uint32_t index)
{
  LaneInfo &lane_info = info[index];
  const Run &run = runs[lane_info.run];
  const size_t lane = lane_info.offset;
  if (run.lane_any[lane] == 0) {
    return false; /* no write ran */
  }
  const LN_ColumnProgram &program = *run.program;
  const size_t count = run.count;
  LN_RuntimeTree *tree = trees[index];
  KX_GameObject *owner = tree->GetGameObject();
  /* Every write's property first: all of them changed in place, or none now. */
  thread_local std::vector<EXP_Value *> found;
  found.clear();
  for (size_t w = 0; w < program.writes.size(); w++) {
    const Write &write = program.writes[w];
    const size_t cell = w * count + lane;
    if (!IsOwnPropertyWrite(write) || run.masks[cell] == 0 || run.superseded[cell] != 0) {
      continue;
    }
    const std::string &name = program.property_names[write.property_ref];
    EXP_Value *target = nullptr;
    if (write.kind == WriteKind::Number) {
      uint64_t stamp = 0;
      EXP_Value *handle = tree->GetSnapshot().GetGamePropertyHandle(
          owner, write.property_ref, stamp);
      target = LN_CommandBuffer::NumberWriteInPlaceTarget(owner, name, handle, stamp);
    }
    else {
      target = LN_CommandBuffer::ModifyInPlaceTarget(owner, name);
    }
    if (target == nullptr) {
      return false;
    }
    found.push_back(target);
  }
  if (found.empty()) {
    return false;
  }
  /* In op order, as ApplyLane would (in place, a property keeps its type: the ones found stay). */
  size_t next = 0;
  for (size_t w = 0; w < program.writes.size(); w++) {
    const Write &write = program.writes[w];
    const size_t cell = w * count + lane;
    if (!IsOwnPropertyWrite(write) || run.masks[cell] == 0 || run.superseded[cell] != 0) {
      continue;
    }
    if (write.kind == WriteKind::Number) {
      LN_CommandBuffer::ApplyNumberInPlace(found[next++], NumberOf(run, write, lane));
    }
    else {
      LN_CommandBuffer::ApplyModifyInPlace(found[next++], ModifyOf(run, write, lane));
    }
  }
  lane_info.own_properties_applied = true;
  return true;
}

void LN_ColumnRunner::PrepareApply(LN_CommandBuffer &buffer)
{
  /* Plan C1-11 C-4. A lane's own Number and Modify writes change existing number properties of its
   * object in place. The flush would apply them at the lane's sort key, one lane after another on
   * the main thread (a third of the flush: each property its own cache miss). Their result is
   * the same whenever they run if nothing else in the flush reads or writes those properties or
   * removes the object: then they run now, the lanes spread over the pool. Ruled out: objects a
   * command of the buffer or a bridged op's command may touch the properties of (or remove), any
   * object when one may touch anything (CommandPropertyObjects), objects a found-object write of
   * another lane writes, objects of a group (two lanes write them), properties that are missing
   * or no numbers (the flush makes or replaces them). Superseded writes stay left out; the merge
   * and the counts were decided before. */
  Impl &d = *m_impl;
  const PreApplyMode mode = pre_apply_mode();
  if (mode == PreApplyMode::Off || d.run_count == 0 || d.any_lane_written) {
    return;
  }
  LN_JobPool *pool = d.flush_pool;
  const size_t helpers = pool != nullptr ? pool->GetHelperCount() : 0;
  if (mode == PreApplyMode::Auto && helpers == 0) {
    return;
  }
  size_t lanes = 0;
  for (size_t r = 0; r < d.run_count; r++) {
    lanes += d.runs[r].program->own_property_writes ? d.runs[r].count : 0;
  }
  constexpr size_t kMinPreApplyLanes = 1024;
  if (lanes == 0 || (mode == PreApplyMode::Auto && lanes < kMinPreApplyLanes)) {
    return;
  }
  std::vector<const void *> &touched = d.pre_touched;
  touched.clear();
  if (!buffer.AllCommandsPropertyObjects(touched)) {
    return;
  }
  for (size_t r = 0; r < d.run_count; r++) {
    const Impl::Run &run = d.runs[r];
    const LN_ColumnProgram &program = *run.program;
    if (!program.captured_writes && !program.found_property_writes) {
      continue;
    }
    for (size_t w = 0; w < program.writes.size(); w++) {
      const Write &write = program.writes[w];
      const bool captured = write.kind == WriteKind::Captured;
      if (!captured && !((write.kind == WriteKind::Number || write.kind == WriteKind::Modify) &&
                         !Impl::IsOwnPropertyWrite(write)))
      {
        continue;
      }
      for (size_t lane = 0; lane < run.count; lane++) {
        const size_t cell = w * run.count + lane;
        if (run.masks[cell] == 0) {
          continue;
        }
        if (!captured) {
          touched.push_back(
              Impl::WriteTarget(run, write, lane, run.lane_trees[lane]->GetGameObject()));
          continue;
        }
        const Impl::Run::Capture &capture = run.captures[lane / kChunkLanes];
        for (uint32_t k = 0; k < run.captured_count[cell]; k++) {
          if (!capture.buffer->CommandPropertyObjects(capture.refs[run.captured_begin[cell] + k],
                                                      touched))
          {
            return;
          }
        }
      }
    }
  }
  if (!touched.empty()) {
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  }
  std::vector<uint32_t> &pre_lanes = d.pre_lanes;
  pre_lanes.clear();
  for (size_t r = 0; r < d.run_count; r++) {
    const Impl::Run &run = d.runs[r];
    if (!run.program->own_property_writes) {
      continue;
    }
    for (size_t lane = 0; lane < run.count; lane++) {
      if (run.lane_any[lane] == 0) {
        continue; /* no write ran */
      }
      const uint32_t index = run.lane_index[lane];
      /* (An object with several lanes: a group's or, since plan C2 P2, trees that run alone; two
       * lanes would write its properties side by side.) */
      if (index < d.lane_shared.size() && d.lane_shared[index] != 0) {
        continue;
      }
      if (!touched.empty() &&
          std::binary_search(touched.begin(),
                             touched.end(),
                             static_cast<const void *>(run.lane_trees[lane]->GetGameObject())))
      {
        continue;
      }
      pre_lanes.push_back(index);
    }
  }
  if (pre_lanes.empty()) {
    return;
  }
  const size_t chunks = (pre_lanes.size() + kChunkLanes - 1) / kChunkLanes;
  std::atomic<uint64_t> applied{0};
  const auto apply_chunk = [&](const size_t chunk) {
    const size_t first = chunk * kChunkLanes;
    const size_t last = std::min(first + kChunkLanes, pre_lanes.size());
    uint64_t done = 0;
    for (size_t k = first; k < last; k++) {
      done += d.PreApplyLane(pre_lanes[k]) ? 1 : 0;
    }
    applied.fetch_add(done, std::memory_order_relaxed);
  };
  if (helpers > 0 && chunks >= 2) {
    pool->Run(chunks, [&](const size_t chunk, uint32_t /*thread*/) { apply_chunk(chunk); });
  }
  else {
    for (size_t chunk = 0; chunk < chunks; chunk++) {
      apply_chunk(chunk);
    }
  }
  g_column_pre_applied.fetch_add(applied.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
}

void LN_ColumnRunner::PrepareForBatchReads() const
{
  m_impl->LaneMap();
}

bool LN_ColumnRunner::HasPendingMove(const KX_GameObject *object) const
{
  return m_impl->FirstPendingLane(object) != kNone;
}

bool LN_ColumnRunner::PendingMoveSortKey(const KX_GameObject *object, uint64_t &r_sort_key) const
{
  const uint32_t lane = m_impl->FirstPendingLane(object);
  if (lane == kNone) {
    return false;
  }
  r_sort_key = m_impl->info[lane].sort_key;
  return true;
}

void LN_ColumnRunner::ApplyPendingMoves(const uint64_t sort_key,
                                        KX_GameObject *const *objects,
                                        const size_t count)
{
  /* One lane's (the tree of that key): the buffer asks again for each object's next one (a
   * group's object has a lane a tree, and the buffer's own writes of it may sit between them). */
  Impl &d = *m_impl;
  if (count == 0) {
    return;
  }
  const auto &map = d.LaneMap();
  const auto found = map.find(objects[0]);
  if (found == map.end()) {
    return;
  }
  uint32_t lane = kNone;
  for (const uint32_t candidate : found->second) {
    /* The first of that key with the object's moves pending, as PendingMoveSortKey found it (two
     * trees never share a key in a game; a test rig's may). */
    if (d.info[candidate].sort_key == sort_key && d.LaneMovesObject(candidate, objects[0])) {
      lane = candidate;
      break;
    }
  }
  if (lane == kNone || !d.info[lane].moves_pending) {
    return;
  }
  Impl::LaneInfo &info = d.info[lane];
  const Impl::Run &run = d.runs[info.run];
  if (!run.program->moves_others) {
    /* Its moves are all its owner's. */
    d.ApplyLane(lane, true, false, nullptr);
    info.moves_pending = false;
    return;
  }
  d.ApplyLane(lane, true, false, nullptr, objects, count);
  /* Moves of objects outside the read stay (an NPC's turret after a read of the NPC). */
  info.moves_pending = false;
  for (size_t w = 0; w < run.program->writes.size() && !info.moves_pending; w++) {
    const Write &write = run.program->writes[w];
    info.moves_pending = write.kind == WriteKind::Transform && write.is_move &&
                         run.masks[w * run.count + info.offset] != 0;
  }
}

void LN_ColumnRunner::EndTick()
{
  Impl &d = *m_impl;
  /* (Plan calls it first, after the manager's call at the tick's end: nothing to clean then.) */
  if (!d.dirty) {
    return;
  }
  d.dirty = false;
  for (size_t index = 0; index < d.trees.size(); index++) {
    if (index < d.lane.size() && d.lane[index] != 0 && d.trees[index] != nullptr) {
      d.trees[index]->m_columnLane = false;
    }
  }
  for (size_t r = 0; r < d.run_count; r++) {
    d.runs[r].program = nullptr;
    d.runs[r].context = nullptr;
  }
  LN_AddColumnPasses(std::exchange(d.direct_ran, 0));
  if (d.lanes_ran != 0) {
    g_column_lanes.fetch_add(d.lanes_ran, std::memory_order_relaxed);
    d.lanes_ran = 0;
  }
  d.runs_not_plain = 0;
  d.runs_last_write_wins_number = 0;
  if (d.buffer != nullptr) {
    d.buffer->SetExternalWrites(nullptr);
    d.buffer = nullptr;
  }
  d.read = nullptr;
  d.run_count = 0;
  d.lane_count = 0;
  d.trees.clear();
  d.lane.clear();
  d.lane_program.clear();
  d.kept.clear();
  d.info.clear();
  if (!d.lane_of_object.empty()) {
    d.lane_of_object.clear();
  }
  d.lane_map_runs = 0;
  d.lane_map_unsorted = false;
  d.runs_done = 0;
  d.order.clear();
  d.order_next = 0;
  d.order_built = false;
  d.list_keys.clear();
  d.list_refs.clear();
  d.ClearWrittenKeys();
  d.any_lane_written = false;
  d.flush_pool = nullptr;
}

/** \} */
