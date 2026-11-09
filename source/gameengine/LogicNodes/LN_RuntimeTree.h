/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_RuntimeTree.h
 *  \ingroup logicnodes
 */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "LN_DenseIds.h"
#include "LN_EventBus.h"
#include "LN_Inventory.h"
#include "LN_Performance.h"
#include "LN_Program.h"
#include "LN_Snapshot.h"
#include "LN_StateBlock.h"
#include "LN_Types.h"

class KX_GameObject;
class KX_NavMeshObject;
class KX_Camera;
class KX_Scene;
class LN_CommandBuffer;
class LN_DenseIdRegistry;
class LN_Manager;
class LN_RuntimeTreeTestAccess;
template<class Reader>
struct LN_ExecFrameT;
struct LN_LeanReader;
struct LN_RuntimeProfileCounters;
class PHY_ICharacter;
class PHY_IPhysicsController;
class PHY_IVehicle;
struct PHY_RayQuerySettings;
struct PHY_ShapeCastSettings;
struct PHY_CachedCollisionContact;
struct LN_RigidBodyConstraintBreakEvent;

namespace blender {
struct Collection;
struct ID;
}  // namespace blender

/** Tests only (the unit tests have no physics): the contacts each owner touches, read by every
 * collision node instead of the physics environment's cache (an owner not in the map touches
 * nothing). nullptr (the default): the environment. Set before a tick, cleared after the test. */
using LN_CollisionContactsForTests =
    std::unordered_map<const KX_GameObject *, std::vector<const PHY_CachedCollisionContact *>>;
void LN_SetCollisionContactsForTests(const LN_CollisionContactsForTests *contacts);

/** Forget all Global Properties and which categories were read from disk. Called when a game
 * launch starts; the values persist across restarts and scene changes within one launch. */
void LN_ClearGlobalProperties();

class EXP_Value;
class LN_RuntimeTree;
/** A game property's value as Logic Nodes read it, for code without a tree (the UI service):
 * objects in it become references of \a manager's shared domain, the very objects that
 * LN_CommandBuffer::ApplyGamePropertyValue without a tree writes back (by name without a
 * manager). An empty value when \a property is null. Main thread only. */
LN_Value LN_GamePropertyValue(EXP_Value *property, LN_Manager *manager);
/** A game property (not null) read as a Bool or as text by the rules of every read of a value
 * (ValueAsBool, ValueToText of its value, as Get Game Property of another object gives it): the
 * snapshot's typed reads of a property of another type (2026-11-08). They only read the property:
 * any thread (the parallel capture). */
bool LN_GamePropertyAsBool(EXP_Value *property);
std::string LN_GamePropertyAsText(EXP_Value *property);

/** The list and dictionary nodes' change \a op on \a collection (a List for the list changes, a
 * Dict for the others), by the very rules of their Output Only results (the ListAppend ...
 * DictRemoveKey kinds, compared in LN_NodeCoverage's ListAndDictionaryChangesEndAsTheirOutputs):
 * negative indexes count from the end, an index outside the list or an empty key changes
 * nothing, Remove Value removes the first equal item, Add to Dictionary Value keeps whole counts
 * integers. Returns whether the collection changed. */
bool LN_ApplyPropertyEdit(LN_Value &collection,
                          LN_PropertyEditOp op,
                          const LN_Value &value,
                          int32_t index,
                          const std::string &key,
                          float amount);
/** CommandType::EditGameProperty at the flush: reads \a property_name of \a object live, changes
 * it (LN_ApplyPropertyEdit) and writes it back when it changed (objects in it through
 * \a runtime_tree's references, as the Inventory node). A missing property starts empty for
 * Append, Set Dictionary Key and Add to Dictionary Value (the others have nothing to change); a
 * property holding anything else is left as it is: false and \a r_reason. Main thread only. */
bool LN_ApplyGamePropertyEdit(KX_GameObject *object,
                              const std::string &property_name,
                              LN_PropertyEditOp op,
                              const LN_Value &value,
                              int32_t index,
                              const std::string &key,
                              float amount,
                              LN_RuntimeTree *runtime_tree,
                              std::string &r_reason);

/** Where the per-object state of a tree running one program lives in its state block
 * (LN_RuntimeTree::StateBlock, 2026-10-31). Built once per program and shared by all of its
 * trees (LN_Program::CachedTreeStateLayout). Dense arrays hold what every instruction, register,
 * step gate, query, Delay/Timer and flow-control node keeps; the states only some expressions or
 * instructions need (Once, Timer, Tween..., Store Value, Mouse Look and moves, Value Changed) get
 * slots, numbered here. */
struct LN_TreeStateLayout {
  enum Array : uint8_t {
    InstructionStamps = 0,
    RegisterFloatValid,
    RegisterFloats,
    RegisterVectorValid,
    RegisterVectors,
    RegisterBoolValid,
    RegisterBools,
    RegisterIntValid,
    RegisterInts,
    RegisterColorValid,
    RegisterColors,
    RegisterStringValid,
    RegisterStrings,
    RegisterGenericValid,
    RegisterGenerics,
    StepGates,
    Queries,
    BoolStates,
    FloatStates,
    TimeFlows,
    FlowControls,
    MotionStates,
    ValueChanges,
    ArrayCount,
  };

  /** What the layout was built from: a program whose counts differ (one built further after a
   * tree took it, in tests) needs a new layout. */
  uint32_t init_instruction_count = 0;
  uint32_t update_instruction_count = 0;
  uint32_t fixed_instruction_count = 0;
  uint32_t bool_expression_count = 0;
  uint32_t float_expression_count = 0;
  uint32_t time_flow_count = 0;
  uint32_t flow_control_count = 0;
  uint32_t query_count = 0;
  uint32_t step_gate_count = 0;
  uint32_t register_counts[7] = {};

  /** Slot of each bool expression's state (LN_INVALID_INDEX: it keeps none), of each float
   * expression's, of each instruction's motion state (by instruction state index), and of each
   * Value Changed expression's stored values. */
  std::vector<uint32_t> bool_state_slots;
  std::vector<uint32_t> float_state_slots;
  std::vector<uint32_t> motion_slots;
  std::vector<uint32_t> value_change_slots;

  uint32_t counts[ArrayCount] = {};
  size_t offsets[ArrayCount] = {};
  size_t bytes = 0;

  bool Matches(const LN_Program &program) const;
};

/** How close to zero a Delay, Timer... counts as done (the VM's and plan C1's time reads). */
inline constexpr double LN_TIME_FLOW_EPSILON = 1.0e-6;

class LN_RuntimeTree {
 public:
  LN_RuntimeTree(std::shared_ptr<const LN_Program> program,
                 KX_GameObject *gameobj,
                 uint32_t scene_object_index,
                 uint32_t applied_tree_index);
  ~LN_RuntimeTree();
  LN_RuntimeTree(const LN_RuntimeTree &) = delete;
  LN_RuntimeTree &operator=(const LN_RuntimeTree &) = delete;

  void CaptureSnapshot(const LN_TickReadContext *tick_context = nullptr,
                       float fixed_delta = -1.0f,
                       uint64_t tick_index = UINT64_MAX,
                       bool force_disabled = false);
  size_t WarmQueryCache(const LN_TickContext &context);
  template<class Reader>
  size_t WarmQueryCacheWith(const LN_TickContext &context);
  /** Register object scroll/grid layout before dependent interactable bounds are collected. */
  void CollectUIViews(const LN_TickContext &context);
  template<class Reader>
  void CollectUIViewsWith(const LN_TickContext &context);
  /** Forget values cached while collecting, before the UI resolved and consumed input. */
  void InvalidateUICollectionCaches();
  bool ShouldCaptureSnapshot() const;
  bool ShouldCaptureSnapshotForForcedUpdate() const;
  /** A tree's pass outside the column system's wide runs (the column pass, plan C2 P6; named
   * ColumnExecuteReady / ColumnExecuteForcedUpdate until P10 removed the old system, 2026-11-07):
   * the pass's bookkeeping, On Init first when it has not run, every event through
   * ColumnRunDirectEvent (its compiled route or the column walk of its blocks). The manager, its
   * tree batches, Run Logic Tree passes and the runner's lanes given back call them.
   * ExecuteForcedUpdate is a Run Logic Tree pass: its Every N Steps routes run too. */
  void ExecuteReady(LN_CommandBuffer &command_buffer,
                    const LN_TickContext &context,
                    LN_RuntimeProfileCounters *profile_counters = nullptr);
  void ExecuteForcedUpdate(LN_CommandBuffer &command_buffer,
                           const LN_TickContext &context,
                           LN_RuntimeProfileCounters *profile_counters = nullptr);

  bool CanTick() const;
  /** Return whether this tree has work in the requested recurring phase. A Logic + Physics step
   * whose work is all behind Every N Steps gates has none on the steps where no gate is due:
   * PrepareStepGates must have been told the step first. */
  bool CanTick(LN_Event event) const;
  /** On Logic + Physics Step's Every N Steps (LN_StepGate): which gates are due at fixed step
   * `step` (LN_TickContext::phase_tick_index) and the time each of their routes covers: the steps
   * since its last run, Run Logic Tree passes included (at most N; since the tree's first step for
   * its first run) times the step.
   * Once per step (a second call for the same step changes nothing); ExecuteReady also calls it
   * from its context. */
  void PrepareStepGates(uint64_t step, double delta_time, double unscaled_delta_time);
  /** This tree runs its spread gates' routes on the steps where (step + spread) % N == 0. The
   * manager numbers the trees of one source tree 0, 1, 2... as they start, so N objects with one
   * tree take turns. */
  void SetStepSpread(const uint32_t spread)
  {
    m_stepSpread = spread;
    m_hasStepSpread = true;
  }
  uint32_t GetStepSpread() const
  {
    return m_stepSpread;
  }
  bool HasStepSpread() const
  {
    return m_hasStepSpread;
  }
  /** The tree's random stream (LN_Random.h): what makes a Per Object random node's numbers this
   * object's own. The manager numbers the trees of one source tree as they start
   * (LN_Manager::AssignRandomStream), so the same play gives every object the same numbers again;
   * a tree no manager numbers takes one from its object and tree indexes. */
  void SetRandomStream(const uint64_t stream)
  {
    m_randomStream = stream;
  }
  uint64_t GetRandomStream() const
  {
    return m_randomStream;
  }
  /** The tick count a random node hashes now: the steps done for On Init (run in the tree's first
   * tick, a frame or a step: either way the same count), else the event's own count (steps on
   * Logic + Physics Step routes, frames on Frame Update routes). */
  uint64_t RandomTick(const LN_TickContext &context) const
  {
    return m_randomInInit && context.event != LN_Event::OnFixedUpdate ? context.steps_done :
                                                                         context.phase_tick_index;
  }
  /** The program has Every N Steps gates. */
  bool HasStepGates() const
  {
    return !m_stepGates.empty();
  }
  /** Moved into another scene (Load Scene): its steps are counted anew there, from the next one,
   * as for a tree that just started. */
  void RestartStepGates()
  {
    for (StepGateState &state : m_stepGates) {
      state = StepGateState();
    }
    m_stepGatesStep = UINT64_MAX;
    m_stepGatesFirstStep = UINT64_MAX;
    m_stepGatesAnyDue = false;
  }
  /** Whether a gate's routes run in this step (always in a Run Logic Tree pass). */
  bool StepGateRuns(uint32_t step_gate_index) const;

  /** What the tree's state block holds (LN_TreeStateLayout), for tests and diagnostics. */
  struct StateBlockInfo {
    size_t block_bytes = 0;
    uint32_t instruction_stamps = 0;
    uint32_t bool_states = 0;
    uint32_t float_states = 0;
    uint32_t motion_states = 0;
    uint32_t value_changes = 0;
    uint32_t time_flows = 0;
    uint32_t flow_controls = 0;
    uint32_t queries = 0;
    /** States kept apart because the layout planned no slot for them (should stay 0). */
    uint32_t overflow_states = 0;
    bool has_own_input = false;
  };
  StateBlockInfo GetStateBlockInfo() const;
  /** Parallel snapshot capture (a serial part of a tick moved onto the job pool): whether
   * CaptureSnapshot may run on a helper thread for this tree this tick, and if so brings what it
   * would build lazily up to date first (main thread). Only trees whose snapshot reads the
   * object's own game properties, transform, colour and visibility, the tick's shared timing,
   * gravity and input qualify: reads of the scene that nothing changes meanwhile. */
  bool PrepareParallelCapture(const LN_TickReadContext *tick_context);
  /** Plan C1 (LN_Columns.h): a tree running in columns this tick has only its game properties
   * captured (the snapshot's own rules, LN_Snapshot::CaptureGamePropertiesOnly), plus its transform
   * when the column program reads it from the snapshot (`transform`), on any thread. */
  void CaptureColumnProperties(uint64_t tick_index, bool transform);
  /** A lane given back to the column pass (LN_ColumnRunner's GiveBack): its snapshot holds only
   * what CaptureColumnProperties took, while the pass reads every channel from it (time, input,
   * gravity...). Takes the rest as the tick's capture would have, keeping the pose taken then
   * (LN_Snapshot::CompleteLaneCapture); the tree is no lane after it. Main thread. */
  void CompleteColumnLaneSnapshot(const LN_TickReadContext *tick_context,
                                  float fixed_delta,
                                  uint64_t tick_index,
                                  bool transform_taken);
  /** Plan C1: what ExecuteReady and ExecuteEvent do for a pass of `context.event` before its ops
   * (the fallback report, the state block, the loop budget, the event's delta, a new execution
   * scope); returns the pass's scope serial. A lane's tree on its chunk's thread. */
  uint64_t BeginColumnPass(const LN_TickContext &context);
  /** Plan C1: what ExecuteReady does after a pass (the step gates that ran). */
  void EndColumnPass(const LN_TickContext &context);
  /** Plan C1: On Logic + Physics Step's Time Passed (StepTimePassed: the gate's, else the
   * event's delta). */
  float ColumnStepTimePassed(uint32_t step_gate_index) const;
  /** The steps a run of a route behind gate `step_gate_index` covers (Every N Steps: N, the steps
   * since its last run; no gate: 1): LN_StepFactor's count, for the VM's handlers and the
   * columns' FStepFactor alike. */
  uint32_t StepGateStepsCovered(uint32_t step_gate_index) const;
  /** Plan C1: the unscaled delta a block behind gate `step_gate_index` reads (its gate's time). */
  float ColumnGateUnscaledDelta(uint32_t step_gate_index, const LN_TickContext &context) const;
  /** Plan C1: the delta_time a block behind gate `step_gate_index` reads, as float (`raw`) or not
   * below 0 (ScopedStepGateTime's time_passed). */
  float ColumnGateDelta(uint32_t step_gate_index, bool raw, const LN_TickContext &context) const;
  /** Plan C1: Move To's handler noting Reached for its stamp `state_index` this pass. */
  void ColumnNoteReached(uint32_t state_index, uint64_t tick_index);
  /** The time flow ops of OpTimeFlowControlInstruction (Wait for Next Frame, Timer, Delay,
   * Pulsify, Barrier, Cooldown and its Reset), one code for the VM and plan C1's columns: the op's
   * state `state_index`, `ui_event_route` its instruction's flag, `event` the event it runs in,
   * `context` the route's (its gate's time and stride behind Every N Steps, the pass's scope
   * serial). The inputs are read through the callbacks exactly where the VM reads them (some only
   * on some paths: a Barrier's Duration only while its condition holds, a Cooldown's inputs only
   * when it is not running). Nothing happens for a state the tree does not have. */
  void TimeFlowArmNextFrame(uint32_t state_index, bool ui_event_route, const LN_TickContext &context);
  void TimeFlowArmTimer(uint32_t state_index,
                        bool ui_event_route,
                        LN_Event event,
                        const LN_TickContext &context,
                        blender::FunctionRef<float()> duration,
                        blender::FunctionRef<bool()> ignore_timescale);
  void TimeFlowArmDelay(uint32_t state_index,
                        bool ui_event_route,
                        LN_Event event,
                        const LN_TickContext &context,
                        blender::FunctionRef<float()> duration,
                        blender::FunctionRef<bool()> ignore_timescale);
  void TimeFlowUpdatePulsify(uint32_t state_index,
                             LN_Event event,
                             const LN_TickContext &context,
                             blender::FunctionRef<float()> interval,
                             blender::FunctionRef<bool()> ignore_timescale);
  void TimeFlowUpdateBarrier(uint32_t state_index,
                             LN_Event event,
                             const LN_TickContext &context,
                             blender::FunctionRef<bool()> condition,
                             blender::FunctionRef<float()> duration,
                             blender::FunctionRef<bool()> ignore_timescale);
  void TimeFlowTryCooldown(uint32_t state_index,
                           bool ui_event_route,
                           LN_Event event,
                           const LN_TickContext &context,
                           blender::FunctionRef<float()> duration,
                           blender::FunctionRef<bool()> ignore_timescale);
  void TimeFlowResetCooldown(uint32_t state_index, LN_Event event, const LN_TickContext &context);
  /** Plan C1: the attribute of `object` as Get Object Attribute's vector output reads it (the
   * typed read, ObjectAttributeAsVector). */
  static bool ColumnObjectAttributeVector(KX_GameObject &object,
                                          LN_ObjectAttributeId attribute,
                                          MT_Vector3 &r_vector);
  /** Plan C1-8, the bridge: an expression of this tree worked out by the VM's own kind functions
   * (the lean reader) for a column run, in its route context (`context`: the pass's scope serial,
   * a gate's time behind Every N Steps, the column run's capture buffer). */
  bool ColumnEvalBool(uint32_t index, bool fallback, LN_TickContext &context);
  int32_t ColumnEvalInt(uint32_t index, int32_t fallback, LN_TickContext &context);
  float ColumnEvalFloat(uint32_t index, float fallback, LN_TickContext &context);
  MT_Vector3 ColumnEvalVector(uint32_t index, const MT_Vector3 &fallback, LN_TickContext &context);
  LN_Rotation ColumnEvalRotation(uint32_t index,
                                 const LN_Rotation &fallback,
                                 LN_TickContext &context);
  MT_Vector4 ColumnEvalColor(uint32_t index, const MT_Vector4 &fallback, LN_TickContext &context);
  /** Plan C1-8, the bridge: op `op_index` of `event` run by the VM's own handler for this tree
   * (OpLean, as the block walk runs it: a fresh command key, the scope of `context`), recording
   * into `buffer`. */
  void ColumnRunOp(LN_Event event, uint32_t op_index, LN_CommandBuffer &buffer, LN_TickContext &context);
  /** Plan C1-9: Loop body block `block_index` of `event` run by the VM's own block walk for this
   * tree (RunLoopBody, its ops through OpLean), recording into `buffer`, in the pass scope of
   * `context`. */
  void ColumnRunLoopBlock(LN_Event event,
                          uint32_t block_index,
                          LN_CommandBuffer &buffer,
                          LN_TickContext &context);
  /** Plan C2 P5 / P6, a direct program's lane: the pass of `event` in scope `scope_serial` (its
   * prelude made by BeginColumnPass), as ExecuteEvent runs one but with the column system's own
   * walk: the event's compiled route when it has one and nothing asks for what a route leaves
   * out (paused UI routes, the profile counters, the read order trace), else its blocks in their
   * order (a gated block only on its gate's steps with its gate's time and stride, a Loop's
   * Completed route only where the loop completed in this pass, a Loop body through RunLoopBody),
   * every op through OpLean (OpDirect in the reference mode), recording into `buffer`.
   * `context` is the pass's (one for the whole pass). */
  void ColumnRunDirectEvent(LN_Event event,
                            uint64_t scope_serial,
                            LN_CommandBuffer &buffer,
                            LN_TickContext &context);
  /** ExecuteEvent's prelude (its context a copy, the event's delta, a new scope serial) and
   * ColumnRunDirectEvent (plan C2 P6). */
  void ColumnExecuteEvent(LN_Event event, LN_CommandBuffer &buffer, LN_TickContext context);
  /** On Init through ColumnExecuteEvent, its random numbers counted in steps (RandomTick). */
  void RunInitEvent(LN_CommandBuffer &command_buffer, const LN_TickContext &context);
  /** Plan C1-9: loop frame `frame` completed in the pass of scope `scope_serial` (its Completed
   * route runs: the column walk's check). */
  bool ColumnLoopCompleted(uint32_t frame, uint64_t scope_serial) const;
  /** Plan C1-8: the op of stamp `state_index` ran in this pass (InstructionExecutedNow). */
  bool ColumnStampRan(uint32_t state_index, uint64_t tick_index, uint64_t scope_serial) const;
  /** Apply Force To Target's math, one code for its handler and plan C2 P5's column kernel: from
   * the target position, Force, Reached Threshold and Stop Distance as read (the last two taken
   * as not below 0) and where the moved object is, whether it counts as reached (`r_reached`)
   * and whether a force is applied (the return value; the force in `r_force`). */
  static bool ColumnForceToTarget(const MT_Vector3 &target_position,
                                  float force,
                                  float reached_threshold,
                                  float stop_distance,
                                  const MT_Vector3 &position,
                                  bool &r_reached,
                                  MT_Vector3 &r_force);
  /** Plan C1: Vector To Rotation's Euler angles (VectorToRotationEuler). */
  static MT_Vector3 ColumnVectorToRotationEuler(const MT_Vector3 &direction,
                                                const MT_Vector3 &up,
                                                int32_t axis_selector);
  /** Plan C1: the conversions of a generic value the kinds' FromGenericValue make. */
  static bool ColumnValueAsFloat(const LN_Value &value, float &r_value);
  /** ValueAsVector (plan C1-11: a tree property's generic value as a vector in columns). */
  static bool ColumnValueAsVector(const LN_Value &value, MT_Vector3 &r_vector);
  /** ResolveObjectValue (plan C1-11: the object a tree property holds, a write's object in
   * columns; per tree, any thread whose chunk holds the tree). */
  KX_GameObject *ColumnObjectOfValue(const LN_Value &value) const;
  /** LN_Manager::PrepareObjectNameLookup of the tree's manager (main thread). */
  void PrepareObjectNameLookup() const;
  static int32_t ColumnValueAsInt(const LN_Value &value, LN_FloatToIntMode mode);
  static bool ColumnValueAsBool(const LN_Value &value);
  /** Plan C1 prototype: the object a value expression gives, read as an op's Object input reads it
   * (the lean reader's Value, then ResolveObjectValue). Main thread. */
  KX_GameObject *ResolveObjectValueExpressionForColumns(uint32_t value_expr_index,
                                                        const LN_TickContext &context);
  /** Plan C1-9: Get Average Center Position query `query_index` worked out by the VM's own code
   * (VectorAverageKind through the lean reader), as a fresh query result. A column run calls it
   * once for all its NPCs when the average's source is the same for each. Main thread. */
  LN_QueryResult ColumnVectorAverage(uint32_t query_index, const LN_TickContext &context);
  /** Plan C2 fix 1: expression `index` worked out by the VM's own kind function (the lean
   * reader, no memo, no command key) with the fallback of its read site. A column run calls it
   * once for all its NPCs for a read that is the same for each all the tick (another object's
   * game property). Main thread. */
  float ColumnInvariantFloat(uint32_t index, float fallback, const LN_TickContext &context);
  int32_t ColumnInvariantInt(uint32_t index, int32_t fallback, const LN_TickContext &context);
  bool ColumnInvariantBool(uint32_t index, bool fallback, const LN_TickContext &context);
  /** Off: layouts built from now on plan no state slots, so every slot state lives apart (the
   * storage before the state block), for tests comparing both. */
  static void SetStateSlotsPlannedForTests(bool planned);
  /** The per-tick memo of pure expressions (plan phase A15, decision D1): 1 = on (production),
   * 0 = off (every read works the expression out), -1 = the default (on). A memo can never change a
   * result, so tests run both and compare. */
  static void SetMemoForTests(int mode);
  /** The lean reader's memo (plan phase A33, UPBGE_LN_LEAN_MEMO): 0 off, 1 on, -1 the environment. */
  static void SetLeanMemoForTests(int mode);
  /** Makes the program's lean form if the mode asks for it (main thread; a no-op elsewhere). The
   * prepare pass of a tree batch calls it for every tree before the chunks go out. */
  void PrepareLeanProgram() const;

  /** One expression read a kind made while it was worked out (plan phase A13): the family letter
   * (B bool, I int, F float, S string, V vector, R rotation, C color, A value, Q query) and the index it
   * asked for, invalid ones included. Tests record them to pin the order in which every kind
   * reads its inputs; null (the default) records nothing. */
  struct ReadTraceEntry {
    char family;
    uint32_t index;
  };
  static void SetReadTraceForTests(std::vector<ReadTraceEntry> *trace);
  /** Works out one expression of `family` (one of the letters above) through the lean reader and
   * drops the result. */
  void EvaluateExpressionForTests(char family, uint32_t index, const LN_TickContext &context);
  /** Object UI declarations stay registered every frame, even while this tree is paused or has
   * no wired output, so hover, focus and scroll state never reset behind the user's back. */
  bool CollectsObjectUI() const;
  /** While the game is paused, a tree that While Paused does not keep running still runs its
   * UI event routes (button clicks...), and only those. Set before each Frame Update. */
  void SetUIEventRoutesOnly(const bool ui_event_routes_only)
  {
    m_uiEventRoutesOnly = ui_event_routes_only;
  }
  bool UIEventRoutesOnly() const
  {
    return m_uiEventRoutesOnly;
  }
  /** On Init has run: the tree's next run is an ordinary pass (tree batches take only those). */
  bool IsInitExecuted() const
  {
    return m_initExecuted;
  }
  /** LN_Manager's tree batches: whether this tree is its object's only tree, kept by the
   * manager's tree index for the trees of the object whose trees changed. */
  struct BatchNote {
    bool sole_tree = false;
  };
  BatchNote &GetBatchNote()
  {
    return m_batchNote;
  }
  /** Plan C1 prototype: the tree runs in columns this tick (LN_ColumnRunner::Plan; its snapshot
   * holds only its game properties then). */
  bool IsColumnLane() const
  {
    return m_columnLane;
  }
  /** Resolve statically-authored While Paused scopes before paused trees are scheduled. */
  void CollectPauseProcessingObjects(std::vector<KX_GameObject *> &r_objects);
  template<class Reader>
  void CollectPauseProcessingObjectsWith(std::vector<KX_GameObject *> &r_objects);
  bool OwnsGameObject(KX_GameObject *gameobj) const;
  void DetachGameObject();
  void SetProgram(std::shared_ptr<const LN_Program> program);
  void SetLogicManager(LN_Manager *logic_manager);
  void BindDenseIds(LN_DenseIdRegistry &registry);
  /** A scene merge (LN_Manager::AbsorbRuntimeTreesFrom) renumbers the manager's object handles,
   * which On Collision's Each Object states use as identities: before it, while the tree still
   * has its old manager, they become the objects themselves (a removed object none), and after
   * it, with the new manager, handles again. So the objects touching across the merge stay, and
   * no Exit and Enter fire for them. Main thread. */
  void HoldCollisionIdentitiesAsObjects();
  void RenewCollisionIdentities();
  LN_RuntimeRef MakeObjectRef(KX_GameObject *gameobj, const std::string &debug_name = "");
  KX_GameObject *ResolveObjectRef(const LN_RuntimeRef &runtime_ref) const;
  /** Convert references to tree-independent form before storing a value where other trees read
   * it. May create object handles, so worker trees must not call it (event content is shared
   * when the command buffer flushes). */
  void ShareValue(LN_Value &value);
  void InvalidateObjectRef(KX_GameObject *gameobj);
  LN_RuntimeRef MakeSceneRef(KX_Scene *scene, const std::string &debug_name = "");
  KX_Scene *ResolveSceneRef(const LN_RuntimeRef &runtime_ref) const;
  LN_RuntimeRef MakeCollectionRef(blender::Collection *collection,
                                  const std::string &debug_name = "");
  blender::Collection *ResolveCollectionRef(const LN_RuntimeRef &runtime_ref) const;
  LN_RuntimeRef MakeDatablockRef(blender::ID *id, const std::string &debug_name = "");
  blender::ID *ResolveDatablockRef(const LN_RuntimeRef &runtime_ref) const;
  void InvalidateRuntimeRef(LN_RuntimeRefKind kind, const void *pointer);
  LN_PhysicsQueryResult Raycast(
      const MT_Vector3 &from,
      const MT_Vector3 &to,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name = std::string(),
      bool xray = false,
      const std::vector<LN_RuntimeRef> &extra_ignore_object_refs = {},
      LN_InteractionDomain interaction_domain = LN_InteractionDomain::Any,
      bool respect_runtime_visibility = false,
      const KX_Camera *view_camera = nullptr);
  std::vector<LN_PhysicsQueryResult> RaycastAll(
      const MT_Vector3 &from,
      const MT_Vector3 &to,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name = std::string(),
      bool xray = false,
      const std::vector<LN_RuntimeRef> &extra_ignore_object_refs = {});
  /** The hits into `r_results` (cleared first; plan C2 P3: a query's cached results are refilled
   * in place, not a new vector each cast). */
  void RayCast(
      const PHY_RayQuerySettings &settings,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name,
      bool xray,
      const std::vector<LN_RuntimeRef> &extra_ignore_object_refs,
      bool &r_blocked,
      LN_PhysicsQueryResult &r_blocker,
      bool &r_supported,
      std::vector<LN_PhysicsQueryResult> &r_results);
  void ShapeCast(
      const PHY_ShapeCastSettings &settings,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name,
      bool xray,
      const std::vector<LN_RuntimeRef> &extra_ignore_object_refs,
      bool &r_blocked,
      LN_PhysicsQueryResult &r_blocker,
      bool &r_supported,
      std::vector<LN_PhysicsQueryResult> &r_results);

  const LN_Value *GetTreePropertyValue(uint32_t property_ref_index) const;
  bool SetTreePropertyValue(uint32_t property_ref_index, const LN_Value &value);
  bool SetTreePropertyValue(uint32_t property_ref_index, LN_Value &&value);
  void CompletePersistenceOperation(uint32_t expression_index, bool success);
  /** Whether one deferred UI result can be accepted without dropping an older result. */
  bool CanCompleteUIOperation(uint32_t expression_index) const;
  /** Whether the graph reads a UI command's result: one of the node's outputs is wired. */
  bool ReadsUIOperation(uint32_t expression_index) const;
  /** Queue a deferred UI command result. Returns false when the bounded result queue is full. */
  bool CompleteUIOperation(uint32_t expression_index, LN_UIResult result, int32_t handle);
  /** Queue one lifecycle event for an exact Play Action request. */
  bool CompleteActionOperation(uint32_t expression_index, LN_ActionResult result);

  bool IsAttached() const
  {
    return m_gameObject != nullptr;
  }

  KX_GameObject *GetGameObject() const
  {
    return m_gameObject;
  }

  std::shared_ptr<const LN_Program> GetProgram() const
  {
    return m_program;
  }
  /** The program without a reference count, for code that uses it at once (a shared pointer
   * copy is two atomic operations: 2% of a 10,000-NPC frame in the property writes alone). */
  const LN_Program *GetProgramPointer() const
  {
    return m_program.get();
  }

  uint64_t GetProgramRevision() const
  {
    return m_programRevision;
  }

  /** A UI widget changed a game property of the owner after the snapshot: read them again. */
  void RefreshSnapshotGameProperties()
  {
    m_snapshot.RefreshGameProperties(m_gameObject, m_program.get());
  }
  const LN_Snapshot &GetSnapshot() const
  {
    return m_snapshot;
  }

  LN_Snapshot &GetSnapshot()
  {
    return m_snapshot;
  }

  bool IsEnabled() const
  {
    return m_enabled;
  }

  void SetEnabled(bool enabled)
  {
    m_enabled = enabled;
  }

  bool IsRuntimeActive() const
  {
    return m_runtimeActive;
  }

  void SetRuntimeActive(bool runtime_active)
  {
    m_runtimeActive = runtime_active;
  }

  uint32_t GetAppliedTreeIndex() const
  {
    return m_appliedTreeIndex;
  }

  uint32_t GetSceneObjectIndex() const
  {
    return m_sceneObjectIndex;
  }

  LN_RuntimeTreeId GetDenseRuntimeTreeId() const
  {
    return m_denseRuntimeTreeId;
  }

  LN_ObjectHandle GetOwnerObjectHandle() const
  {
    return m_ownerObjectHandle;
  }

  LN_EventSubjectId GetEventSubjectId(LN_StringId string_id) const;
  /** Inline: every property write the command buffer records asks it. */
  LN_GamePropertyId GetGamePropertyId(const uint32_t property_ref_index) const
  {
    return property_ref_index < m_gamePropertyIds.size() ? m_gamePropertyIds[property_ref_index] :
                                                           LN_GamePropertyId{};
  }
  LN_TreePropertyId GetTreePropertyId(uint32_t property_ref_index) const;

  void SetSceneObjectIndex(uint32_t scene_object_index)
  {
    m_sceneObjectIndex = scene_object_index;
  }

  struct RuntimeFallbackHit {
    LN_OpCode opcode = LN_OpCode::Nop;
    LN_RuntimeFallbackReason reason = LN_RuntimeFallbackReason::UnsupportedOpcode;
    LN_Event event = LN_Event::OnFixedUpdate;
    uint32_t count = 0;
    uint32_t first_instruction_index = LN_INVALID_INDEX;
    uint32_t first_source_ref_index = LN_INVALID_INDEX;
    const char *opcode_name = nullptr;
    const char *profile_counter_name = nullptr;
    const char *removal_condition = nullptr;
    bool source_ref_available = false;
    bool hot_path_warning_required = true;
  };

  struct RuntimeExpressionFallbackHit {
    LN_RuntimeExpressionFamily family = LN_RuntimeExpressionFamily::Bool;
    uint32_t kind = 0;
    uint32_t expression_index = LN_INVALID_INDEX;
    LN_RuntimeFallbackReason reason = LN_RuntimeFallbackReason::UnsupportedExpression;
    uint32_t count = 0;
    const char *expression_name = nullptr;
    const char *profile_counter_name = nullptr;
    const char *removal_condition = nullptr;
    bool source_ref_required = true;
    bool hot_path_warning_required = true;
  };

  struct RuntimeSystemFallbackHit {
    LN_RuntimeFallbackReason reason = LN_RuntimeFallbackReason::StaleHandle;
    uint32_t count = 0;
    std::string debug_name;
    const char *profile_counter_name = nullptr;
    const char *removal_condition = nullptr;
    bool hot_path_warning_required = true;
  };

  struct RuntimeFallbackReport {
    uint64_t tick_index = UINT64_MAX;
    uint32_t fallback_block_count = 0;
    uint32_t fallback_instruction_count = 0;
    uint32_t expression_fallback_count = 0;
    uint32_t system_fallback_count = 0;
    uint32_t reason_mask = 0;
    uint32_t expression_reason_mask = 0;
    uint32_t system_reason_mask = 0;
    bool optimized_execution_partial = false;
    std::vector<RuntimeFallbackHit> hits;
    std::vector<RuntimeExpressionFallbackHit> expression_hits;
    std::vector<RuntimeSystemFallbackHit> system_hits;
  };

  const RuntimeFallbackReport &GetRuntimeFallbackReport() const
  {
    return m_runtimeFallbackReport;
  }

  /** Tests (lean runtime plan A2): the tree's per-object state as text, in two parts that are
   * compared apart. `meaning`: what the tree's behaviour depends on and a save would keep (time
   * flows with their pulses, flow controls, slot states, value changes, step gates, motion and
   * loop states, tree properties, the kept game properties, overflow slots), with "ran this
   * tick" for each instruction instead of tick numbers. `bookkeeping`: the counters and cache
   * stamps (tick numbers, scope and command serials, the register file's generation). Floats are
   * bit patterns. `current_tick` is the tick the tree last ran in (LN_TickContext::tick_index). */
  struct StateDump {
    std::string meaning;
    std::string bookkeeping;
  };
  StateDump DumpStateForTests(uint64_t current_tick) const;

 private:
  static constexpr int kMaxPathLength = 128;
  /**
   * Deterministic per-tree budget for loop body executions in one scheduler tick. Excess work is
   * retained and resumed on later fixed ticks instead of freezing the main thread or dropping
   * iterations.
   */
  static constexpr size_t kMaxLoopIterationsPerTick = 10000;

  /** When an instruction last ran (InstructionExecuted pulses read it): one per instruction. */
  struct InstructionStamp {
    uint64_t last_execution_tick = UINT64_MAX;
    uint64_t last_execution_scope_serial = 0;
  };

  /** What Mouse Look, Navigate, Follow Path, Move To and Apply Force To Target keep between runs
   * (InstructionReached and Next Point read it): only their instructions have one. */
  struct InstructionMotionState {
    float mouse_look_smooth_x = 0.0f;
    float mouse_look_smooth_y = 0.0f;
    float mouse_look_angle_x = 0.0f;
    float mouse_look_angle_y = 0.0f;
    int32_t mouse_look_previous_x = 0;
    int32_t mouse_look_previous_y = 0;
    bool mouse_look_initialized = false;
    bool mouse_look_has_previous_position = false;
    /** LN_TickContext::mouse_ui_serial at its last run, and whether it held the cursor then. */
    uint64_t mouse_look_ui_serial = 0;
    bool mouse_look_centered = false;
    /** Seconds Mouse Look still holds the cursor without turning after it started (over): the
     * system may report where its pointer really is a moment later (Wayland, a window that just
     * opened or got the pointer back). */
    float mouse_look_settle = 0.0f;
    int mouse_look_settle_runs = 0;
    int path_len = 0;
    int waypoint_index = -1;
    int list_point_index = 0;
    float path[kMaxPathLength * 3] = {};
    MT_Vector3 next_path_point = MT_Vector3(0.0f, 0.0f, 0.0f);
    double path_update_time = -1.0;
    uint64_t last_reached_tick = UINT64_MAX;
    uint64_t last_reached_scope_serial = 0;
  };

  struct BoolExpressionRuntimeState {
    bool initialized = false;
    bool previous_bool = false;
    float previous_float = 0.0f;
    bool active = false;
    bool cached_result = false;
    double start_time = 0.0;
    double due_time = 0.0;
    double last_pulse_time = 0.0;
    uint64_t last_update_tick = UINT64_MAX;
    uint64_t last_update_phase_tick = UINT64_MAX;
    uint64_t pending_persistence_results = 0;
    uint8_t pending_persistence_count = 0;
    bool last_persistence_success = false;
  };

  /** Value Changed To's values: the last one seen and the change it reported (its Old / New). */
  struct ValueChangeState {
    bool has_previous_value = false;
    LN_Value previous_value;
    LN_Value stored_old_value;
    LN_Value stored_new_value;
  };

  struct UICommandCompletion {
    LN_UIResult result = LN_UIResult::RuntimeUnavailable;
    int32_t handle = 0;
  };

  struct UICommandCompletionState {
    std::deque<UICommandCompletion> pending;
    UICommandCompletion current;
    uint64_t current_tick = UINT64_MAX;
    bool has_current = false;
    /** Value outputs retain the most recently completed result after the one-tick flow pulse. */
    bool has_value = false;
  };

  struct ActionCompletionState {
    std::deque<LN_ActionResult> pending;
    LN_ActionResult current = LN_ActionResult::Failed;
    uint64_t current_tick = UINT64_MAX;
    bool has_current = false;
  };

  struct FloatExpressionRuntimeState {
    bool initialized = false;
    float stored_value = 0.0f;
    uint64_t last_store_tick = UINT64_MAX;
  };

  enum class TimeFlowRuntimeKind : uint8_t {
    None = 0,
    NextFrame,
    Timer,
    Delay,
    Pulsify,
    Barrier,
    Cooldown,
  };

  struct TimeFlowRuntimeState {
    struct PendingPulse {
      double remaining_time = 0.0;
      std::vector<LN_Value> tree_properties;
    };

    TimeFlowRuntimeKind kind = TimeFlowRuntimeKind::None;
    bool active = false;
    double remaining_time = 0.0;
    double accumulated_time = 0.0;
    uint64_t pulse_tick = UINT64_MAX;
    uint64_t last_update_tick = UINT64_MAX;
    uint64_t last_update_phase_tick = UINT64_MAX;
    /** Steps between two runs of the route that last updated it (LN_TickContext). */
    uint32_t last_update_phase_stride = 1;
    uint64_t accepted_scope_serial = 0;
    uint64_t blocked_scope_serial = 0;
    std::vector<PendingPulse> pending_pulses;
    std::vector<LN_Value> active_tree_properties;
    bool has_active_tree_properties = false;
    bool ignore_timescale = false;
    /** Armed by a UI event route: still ticked while only UI event routes run (paused game). */
    bool ui_event_route = false;
    LN_Event event = LN_Event::OnUpdate;
  };

  /** Persistent, fixed-size state for routing and bounded flow-control nodes. */
  struct FlowControlRuntimeState {
    uint32_t next_route_index = 0;
    uint32_t last_route_index = LN_INVALID_INDEX;
    uint32_t accepted_count = 0;
    uint64_t routed_scope_serial = 0;
    uint64_t accepted_scope_serial = 0;
    uint64_t blocked_scope_serial = 0;

    bool motion_initialized = false;
    bool motion_valid = false;
    int32_t motion_source_mode = int32_t(LN_MotionSourceMode::Position);
    uint64_t motion_last_sample_tick = UINT64_MAX;
    double motion_last_sample_time = 0.0;
    LN_RuntimeRef motion_source_ref;
    MT_Vector3 motion_position = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 motion_previous_position = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 motion_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 motion_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 motion_predicted_position = MT_Vector3(0.0f, 0.0f, 0.0f);
    float motion_speed = 0.0f;
  };

  struct QueryExpressionRuntimeState {
    struct HoverRuntimeState {
      uint64_t baseline_tick = UINT64_MAX;
      bool initialized = false;
      bool previous = false;
      bool baseline_initialized = false;
      bool baseline_previous = false;
    };

    uint64_t cached_tick = UINT64_MAX;
    uint64_t cached_command_execution_key = UINT64_MAX;
    LN_QueryResult cached_result;
    bool cached_command_ordered = false;
    /** The last result came from Get Distance (DistanceBetween), which sets only the distance
     * fields: the next evaluation of it starts by clearing those three instead of building a
     * fresh result (a kilobyte of strings and vectors) at every read (plan A21). */
    bool cached_result_distance_only = false;
    /** Plan C2 P3: the last result was a ray's or shape cast's (PhysicsOnlyQuery): it set its
     * physics fields and nothing else, so only those are reset. */
    bool cached_result_physics_only = false;
    std::array<HoverRuntimeState, 3> hover_by_event;
    /** On UI Event: which of this frame's matching events the query reports (0: the first). */
    uint32_t ui_event_cursor = 0;
  };
  /** Plan C2 P3: query `index` (a Raycast, LN_Columns' QRay node) worked out by QueryKind into
   * `scratch` (the run's per-thread result, not the tree's query cache), its inputs read as the
   * VM reads them. */
  const LN_QueryResult &ColumnCastQuery(uint32_t index,
                                        LN_TickContext &context,
                                        QueryExpressionRuntimeState &scratch);

  /** The register file: values in the state block, valid while their generation is the active
   * one (a new tick starts a new generation). */
  struct RegisterExpressionRuntimeState {
    uint64_t cached_tick = UINT64_MAX;
    LN_StateSpan<uint8_t> bool_values;
    LN_StateSpan<int32_t> int_values;
    LN_StateSpan<float> float_values;
    LN_StateSpan<MT_Vector3> vector_values;
    LN_StateSpan<MT_Vector4> color_values;
    LN_StateSpan<std::string> string_values;
    LN_StateSpan<LN_Value> generic_values;
    LN_StateSpan<uint32_t> bool_valid;
    LN_StateSpan<uint32_t> int_valid;
    LN_StateSpan<uint32_t> float_valid;
    LN_StateSpan<uint32_t> vector_valid;
    LN_StateSpan<uint32_t> color_valid;
    LN_StateSpan<uint32_t> string_valid;
    LN_StateSpan<uint32_t> generic_valid;
    uint32_t active_generation = 1;
  };

  struct CollisionResult {
    enum class Detail : uint8_t {
      StateOnly = 0,
      Objects = 1,
      Contacts = 2,
    };

    bool hit = false;
    KX_GameObject *owner = nullptr;
    KX_GameObject *hit_object = nullptr;
    /** Each touching object once, in contact order (from Detail::Objects). */
    std::vector<KX_GameObject *> hit_objects;
    std::vector<MT_Vector3> hit_points;
    std::vector<MT_Vector3> hit_normals;
    /** Detail::Contacts: for each entry of hit_objects, its summed contact count and its first
     * contact point and normal (On Collision's Each Object events give them per object). Kept
     * parallel to hit_objects by every change of it (InvalidateObjectRef's scrub). */
    std::vector<int32_t> object_contact_counts;
    std::vector<MT_Vector3> object_points;
    std::vector<MT_Vector3> object_normals;
    std::vector<uint8_t> object_has_point;
    std::string property_filter;
    std::string material_filter;
    int32_t contact_count = 0;
    MT_Vector3 hit_point = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
    Detail detail = Detail::StateOnly;
    uint64_t cached_tick = UINT64_MAX;
    /** Its hit was handed to the exit payload (StoreCollisionEventPayload), by the first read
     * that keeps one (Any Object's; Each Object's reads keep none). */
    bool payload_stored = false;
  };

  struct CollisionEventPayload {
    /* Persistent collision payloads own managed object references. Object pointers
     * in last_result are always cleared and reconstructed into per-tick scratch. */
    CollisionResult last_result;
    LN_RuntimeRef owner_ref;
    LN_RuntimeRef hit_object_ref;
    std::vector<LN_RuntimeRef> hit_object_refs;
    uint64_t last_hit_tick = UINT64_MAX;
    uint64_t exit_tick = UINT64_MAX;
  };

  /**
   * An On Collision node with Events = Each Object (one per LN_Program::AcquireCollisionEventNode
   * index), worked out once a tick by UpdateCollisionEvents: which objects touch now (in contact
   * order, as Collided Objects), which of them started touching (entered) or touched at the last
   * update too (stayed), and which objects of the last update touch no more (exited). Objects are
   * compared by identity (CollisionObjectIdentity: the manager's dense handle, so a new object at
   * a removed one's address is a new object), never by address alone. Main thread only (the
   * handles): programs with these routes never join tree batches nor wide column runs.
   */
  struct CollisionEventNodeState {
    uint64_t tick = UINT64_MAX;
    /** Per-object contact info of `current` worked out this tick. */
    bool contacts = false;
    /** The identities of the objects touching at the last update. */
    std::vector<uint64_t> previous;
    std::vector<KX_GameObject *> current;
    std::vector<uint64_t> current_ids;
    /** Indices into `current`, in contact order. */
    std::vector<uint32_t> entered;
    std::vector<uint32_t> stayed;
    /** Identities of `previous` no longer touching, in their old order (resolved per read: a
     * removed object gives none). */
    std::vector<uint64_t> exited;
    /** With `contacts`: each current object's summed contact count and its first contact point
     * and normal (as ResolveCollisionResult's Contact Count / Point / Normal of the object alone). */
    std::vector<int32_t> contact_counts;
    std::vector<MT_Vector3> points;
    std::vector<MT_Vector3> normals;
    std::vector<uint8_t> has_point;
  };

  template<class In>
  bool UpdateTweenValue(In &in,
                  uint32_t tween_expr_index);
  bool EvaluateMouseMoved(bool each_logic_tick) const;
  float EvaluateCurveTable(uint32_t table_index, float input, float influence) const;
  template<class In>
  float ResolveTweenMappedFactor(In &in,
                  uint32_t tween_expr_index);

  struct LoopRuntimeState {
    bool active = false;
    bool triggered = false;
    bool break_requested = false;
    bool trigger_previous = false;
    size_t iteration_index = 0;
    size_t iteration_count = 0;
    size_t batch_index = 0;
    size_t batch_count = 1;
    size_t batch_end_iteration = 0;
    uint64_t last_batch_tick_index = UINT64_MAX;
    uint64_t completed_scope_serial = 0;
    std::vector<LN_Value> items;
    LN_Value current_value;
  };

  size_t m_remainingLoopIterationBudget = kMaxLoopIterationsPerTick;
  uint64_t m_loopIterationBudgetTick = UINT64_MAX;
  /** The tick whose time the Delays, Timers... have counted (TickLatentTimeFlowStates). */
  uint64_t m_latentTimeFlowTick = UINT64_MAX;

  struct ObjectRefSlot {
    KX_GameObject *object = nullptr;
    uint32_t generation = 1;
    std::string debug_name;
  };

  struct RuntimeRefSlot {
    void *pointer = nullptr;
    LN_RuntimeRefKind kind = LN_RuntimeRefKind::None;
    uint32_t generation = 1;
    std::string debug_name;
  };

  friend class LN_Manager;
  template<class R>
  friend struct LN_ExecFrameT;
  friend struct LN_LeanReader;
  friend class LN_RuntimeTreeTestAccess;
  /** Plan C1 prototype (LN_Columns.h): reads the per-tree checks and the program it plans with. */
  friend class LN_ColumnRunner;

  void EnsureTreePropertyStateSize(bool preserve_existing);
  void RefreshOptimizedPathEligibility();
  /** The program's state layout, from its cache or built now. */
  static std::shared_ptr<const LN_TreeStateLayout> StateLayoutFor(const LN_Program &program);
  static std::shared_ptr<const LN_TreeStateLayout> BuildStateLayout(const LN_Program &program);
  /** What a new state block keeps of the old one. BetweenTicks: what a tree keeps between ticks
   * (Once, Timer, Delay, flow-control, motion... states, by expression or instruction), as a
   * program replaced by the same tree's new compile keeps it; query caches start over. All: the
   * query caches too (the same program laid out again). */
  enum class StateKeep : uint8_t { None, BetweenTicks, All };
  /** A new state block for the program's layout. */
  void RebuildStateBlock(StateKeep keep);
  /** The program grew after the tree took it (tests build programs step by step): lay the block
   * out again, keeping its states. Cheap when nothing changed. */
  void RefreshStateBlockLayout();
  void DestroyStateBlock();
  BoolExpressionRuntimeState &BoolExpressionState(uint32_t expression_index);
  FloatExpressionRuntimeState &FloatExpressionState(uint32_t expression_index);
  /** An instruction's motion state, made when missing (writers); `Find` gives null instead. */
  InstructionMotionState &MotionState(uint32_t state_index);
  const InstructionMotionState *FindMotionState(uint32_t state_index) const;
  ValueChangeState &ValueChangeStateOf(uint32_t expression_index);
  const ValueChangeState *FindValueChangeState(uint32_t expression_index) const;
  /** On Value Changed's step with the value read (plan C2 fix 1: one code for BoolKind's
   * ValueChanged / ValueChangedTo and the column kernel SValueChanged): the first sample (or, when
   * `wait_for_value`, a missing value before it) is kept and gives false (the state's result and
   * tick written, returns false); later ones set `r_changed` and the old / new values (returns
   * true: the caller writes the state's result). */
  bool ValueChangeStep(uint32_t expression_index,
                       bool wait_for_value,
                       const LN_Value &value,
                       uint64_t tick,
                       bool &r_changed);
  TimeFlowRuntimeState *FindTimeFlowState(uint32_t state_index);
  const TimeFlowRuntimeState *FindTimeFlowState(uint32_t state_index) const;
  FlowControlRuntimeState *FindFlowControlState(uint32_t state_index);
  const FlowControlRuntimeState *FindFlowControlState(uint32_t state_index) const;
  void TickLatentTimeFlowStates(const LN_TickContext &context);
  QueryExpressionRuntimeState &QueryExpressionState(uint32_t state_index);
  uint32_t InstructionStateIndex(LN_Event event, uint32_t instruction_index) const;
  bool InstructionExecutedNow(const LN_TickContext &context, uint32_t instruction_index) const;
  /** MarkInstructionExecuted for an instruction whose stamp index is known (the lean VM's
   * native ops: LN_LeanOp::state_index). */
  void MarkInstructionStamp(const uint32_t state_index, const uint64_t tick_index)
  {
    if (state_index >= m_instructionStamps.size()) { /* LN_INVALID_INDEX is past every size */
      return;
    }
    m_instructionStamps[state_index].last_execution_tick = tick_index;
    m_instructionStamps[state_index].last_execution_scope_serial = m_activeExecutionScopeSerial;
  }
  bool ResolvePathMoveTarget(InstructionMotionState &state,
                             KX_GameObject *target,
                             KX_NavMeshObject *navmesh,
                             const MT_Vector3 &destination,
                             float reach_threshold,
                             MT_Vector3 &r_move_target,
                             bool &r_reached) const;
  void DrawNavigationPath(KX_GameObject *target,
                          const InstructionMotionState &state,
                          const MT_Vector3 &destination) const;
  void MarkInstructionExecuted(LN_Event event, uint32_t instruction_index, uint64_t tick_index);
  void ProfileInstructionDispatch(const LN_Instruction &instruction);
  void ProfileExpressionEvaluation(uint32_t fallback_requirements);
  void RecordMissingSnapshotChannelsForExpression(
      const LN_RuntimeExpressionSemantics &semantics) const;
  void ClearRuntimeFallbackReport(uint64_t tick_index);
  void RecordRuntimeSystemFallback(LN_RuntimeFallbackReason reason,
                                   const std::string &debug_name,
                                   const char *profile_counter_name,
                                   const char *removal_condition) const;
  void EnsureRegisterExpressionState(const LN_TickContext &context);
  /* The handlers of one event's instructions, templates on the execution frame (LN_ExecFrame.h;
   * intern/LN_RuntimeTreeOps.inl). */
  uint64_t NextExecutionScopeSerial();
  uint64_t NextCommandExecutionKey();
  void ProfileDirectExecOp(const LN_ExecOp &op);
  template<class Frame>
  void OpMouseLookCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpTimeFlowControlInstruction(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpGlobalPropertyInstruction(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpVariableInstruction(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpPhysicsConstraintInstruction(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpNavigationInstruction(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpSceneCollectionInstruction(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDebugDrawInstruction(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectVectorCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectPhysicsCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectGamePropertyCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectObjectColorCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectObjectStateCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectCameraCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectLightCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectWindowCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectInputMotionCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectAudioCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectActionCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectBoneCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectMaterialCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectGlobalCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectFileCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectSceneCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectLifecycleCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectObjectReferenceCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectTreePropertyCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectTreeControlCommand(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirectSendEvent(Frame &frame, const LN_Instruction &instruction, uint32_t instruction_id);
  template<class Frame>
  void OpDirect(Frame &frame, const LN_ExecOp &op);
  template<class Frame>
  void OpLean(Frame &frame, const LN_ExecOp &op, const LN_LeanOp &lean_op);
  /** The native ops of the lean VM (plan phases A18, A21): the flow core (Nop, BranchRoute) and a
   * Set Game Property number write, each the old handler's work with what the lowering knows
   * already. */
  template<class Frame>
  void OpLeanFlow(Frame &frame, const LN_ExecOp &op, const LN_LeanOp &lean_op);
  template<class Frame>
  void OpLeanSetNumber(Frame &frame, const LN_ExecOp &op, const LN_LeanOp &lean_op);
  template<class Frame>
  void OpLeanVector(Frame &frame, const LN_ExecOp &op, const LN_LeanOp &lean_op);
  template<class Frame>
  void OpLeanModifyProperty(Frame &frame, const LN_ExecOp &op, const LN_LeanOp &lean_op);
  /** The native ops' work after their guard, shared by the OpLean handlers above and the compiled
   * routes (plan phase A33), so the two cannot differ. `branch` (LeanFlowBody): the BranchRoute's
   * instruction, null for a Nop. */
  template<class Frame>
  void LeanFlowBody(Frame &frame, const LN_Instruction *branch, uint32_t state_index);
  template<class Frame>
  void LeanSetNumberBody(Frame &frame,
                         const LN_Instruction &instruction,
                         const LN_GamePropertyCommandPayload &payload,
                         uint32_t state_index);
  template<class Frame>
  void LeanVectorBody(Frame &frame,
                      const LN_Instruction &instruction,
                      const LN_VectorCommandPayload &payload,
                      uint32_t constant_index,
                      uint32_t instruction_index,
                      uint32_t state_index);
  template<class Frame>
  void LeanModifyBody(Frame &frame,
                      const LN_Instruction &instruction,
                      const LN_GamePropertyCommandPayload &payload,
                      uint32_t state_index);
  /** Runs an event as its compiled route (LN_LeanRoute, plan phase A33): the walk of its blocks
   * for an event with no Loop and no UI event route, and each op its handler's prelude reduced to
   * what changes between runs (the command key, the guard), then the shared body. */
  template<class Frame>
  void RunLeanRoute(Frame &frame, const LN_LeanRoute &route, uint64_t scope_serial);
  template<class Frame>
  void AppendVectorCommand(Frame &frame,
                           const LN_Instruction &instruction,
                           const LN_VectorCommandPayload &payload,
                           KX_GameObject *target,
                           const MT_Vector3 &value,
                           const LN_Rotation &rotation);
  /** A Loop body's block: its frame begun, then as many iterations as the tick's batch and budget
   * allow (the column walk's loop part; ColumnRunLoopBlock runs it for a column lane). */
  template<class Frame, class RunOp>
  void RunLoopBody(Frame &frame,
                   const LN_ExecBlockProgram &ir,
                   const LN_ExecBlock &block,
                   std::vector<uint32_t> &repeated_loop_frames,
                   uint64_t scope_serial,
                   uint32_t ui_event_state,
                   RunOp &&run_op);
  /** Plan C2 P5-P7: the column system's walk of an event's blocks and its On UI Event repeats
   * (ColumnRunDirectEvent). */
  template<class Frame, class RunOp>
  void ColumnWalkEvent(Frame &frame,
                       const LN_ExecBlockProgram &ir,
                       uint64_t scope_serial,
                       RunOp &&run_op);
  /** The lean program this tree runs on in this call, or null (mode Off, no lean program made
   * yet or an invalid one). Makes the program's lean form on the main thread. */
  const LN_LeanProgram *LeanProgramForRun() const;
  LoopRuntimeState &LoopState(uint32_t loop_frame_index);
  template<class In>
  void BeginLoopFrame(In &in,
                  uint32_t loop_frame_index);
  bool AdvanceLoopFrame(uint32_t loop_frame_index);
  const LN_EventEntry *FindFirstActiveEvent(LN_EventSubjectId subject_id,
                                            const std::string &subject,
                                            KX_GameObject *filter_target) const;
  KX_GameObject *ResolveCollisionPayloadObjectRef(const LN_RuntimeRef &object_ref) const;
  /* The kinds (plan phases A13 and A14): the switch of each Resolve function, reading its inputs
   * through the reader `in` (LN_LeanReader). Defined in intern/LN_RuntimeTreeKinds.inl. */
  template<class In>
  bool BoolKind(In &in,
                uint32_t expression_index,
                const LN_BoolExpression &expression,
                bool fallback);
  template<class In>
  int32_t IntKind(In &in,
                  uint32_t expression_index,
                  const LN_IntExpression &expression,
                  int32_t fallback);
  template<class In>
  float FloatKind(In &in,
                  uint32_t expression_index,
                  const LN_FloatExpression &expression,
                  float fallback);
  template<class In>
  std::string StringKind(In &in,
                         uint32_t expression_index,
                         const LN_StringExpression &expression,
                         const std::string &fallback);
  template<class In>
  MT_Vector3 VectorKind(In &in,
                        uint32_t expression_index,
                        const LN_VectorExpression &expression,
                        const MT_Vector3 &fallback);
  /** `resolved` is set when the result is the expression's own (not the fallback), which is what
   * the per-tick rotation cache keeps. */
  template<class In>
  LN_Rotation RotationKind(In &in,
                           uint32_t expression_index,
                           const LN_VectorExpression &expression,
                           const LN_Rotation &fallback,
                           bool &resolved);
  template<class In>
  MT_Vector4 ColorKind(In &in,
                       uint32_t expression_index,
                       const LN_ColorExpression &expression,
                       const MT_Vector4 &fallback);
  template<class In>
  LN_Value ValueKind(In &in,
                     uint32_t expression_index,
                     const LN_ValueExpression &expression,
                     const LN_Value &fallback);
  /** Plan "Attribute and Retirement", steps G2 and G3 (UPBGE_LN_FAST_ATTRIBUTES: 0 / off = every
   * read reads the attribute's text and builds the whole value, unset / 1 = what follows, verify =
   * both, a "lean fast attributes differ" warning on any difference). The attribute id of value
   * expression `value_index` when its typed read may skip the value: an ObjectAttribute with a
   * constant attribute name, while neither profiling nor the read trace watch every read; None
   * otherwise. */
  LN_ObjectAttributeId FastObjectAttribute(uint32_t value_index) const;
  /** The object an ObjectAttribute reads (its kind's own object resolution: empty Object = the
   * owner). */
  template<class In>
  KX_GameObject *ObjectAttributeTarget(In &in, const LN_ValueExpression &attribute);
  /** ObjectAttributeTarget of value expression `value_index`, for a typed read that skips its
   * value. */
  template<class In> KX_GameObject *TypedAttributeTarget(In &in, uint32_t value_index);
  /** ResolveObjectValue(in.Value(value_index)), without making the value of a constant object
   * (its steps repeated one by one). */
  template<class In> KX_GameObject *ReadObjectInput(In &in, uint32_t value_index);
  /** The object a Constant Object value expression's value resolves to (ValueKind's Constant case
   * followed by ResolveObjectValue, without the value). */
  KX_GameObject *ConstantObjectTarget(uint32_t value_index, const LN_Value &constant);
  /** A verify-mode difference of the fast attributes (warns, counts). */
  static void NoteFastAttributeDifference(const char *what);

 public:
  /** Tests: the mode of the fast attributes (0 off, 1 on, 2 verify; -1 back to the environment)
   * and how many differences verify found. */
  static void SetFastAttributeModeForTests(int mode);
  static uint32_t FastAttributeVerifyDifferencesForTests();

 private:
  /** The query kinds fill `state.cached_result` (the caching, the condition and the pass key are
   * in ResolveQueryExpression). */
  template<class In>
  void QueryKind(In &in,
                 uint32_t expression_index,
                 const LN_QueryExpression &expression,
                 QueryExpressionRuntimeState &state);
  /** QueryKind's Get Average Center Position case (plan C1-9). */
  template<class In>
  void VectorAverageKind(In &in, const LN_QueryExpression &expression, LN_QueryResult &result);
  /** QueryKind's Get Distance case, on its own (plan phase A33): a brain's one query, called
   * without entering the big switch of every query kind. */
  template<class In>
  void DistanceBetweenKind(In &in,
                           const LN_QueryExpression &expression,
                           QueryExpressionRuntimeState &state);
  LN_StringId ResolveStringExpressionId(uint32_t expression_index) const;
  /** A query expression read through a reader: the cache entry, the condition, the pass key and
   * QueryKind. */
  template<class In>
  const LN_QueryResult &QueryEntry(In &in, uint32_t expression_index);
  /* The lean runtime's reads (plan phase A16): the kind straight, with no memo, profiling or
   * taint (intern/LN_RuntimeTreeKinds.inl). */
  template<class In>
  bool LeanBool(In &in, uint32_t expression_index, bool fallback);
  template<class In>
  int32_t LeanInt(In &in, uint32_t expression_index, int32_t fallback);
  template<class In>
  float LeanFloat(In &in, uint32_t expression_index, float fallback);

  template<class In>
  std::string LeanString(In &in, uint32_t expression_index, const std::string &fallback);
  template<class In>
  MT_Vector3 LeanVector(In &in, uint32_t expression_index, const MT_Vector3 &fallback);
  template<class In>
  LN_Rotation LeanRotation(In &in, uint32_t expression_index, const LN_Rotation &fallback);
  template<class In>
  MT_Vector4 LeanColor(In &in, uint32_t expression_index, const MT_Vector4 &fallback);
  template<class In>
  LN_Value LeanValue(In &in, uint32_t expression_index, const LN_Value &fallback);
  template<class In>
  const CollisionResult &ResolveCollisionResult(In &in,
                  uint32_t object_expr_index,
                  uint32_t property_expr_index,
                  uint32_t material_expr_index,
                  CollisionResult::Detail detail = CollisionResult::Detail::StateOnly,
                  bool allow_exit_payload = true,
                  bool store_payload = true);
  /** On Collision with Events = Each Object: the node's state for this tick (made at the first
   * call of a tick: one compare with the last update). `node_field` is the outputs' event node
   * field (index + 1); `contacts` asks for each object's contact info too. */
  template<class In>
  const CollisionEventNodeState *UpdateCollisionEvents(In &in,
                                                       int32_t node_field,
                                                       uint32_t object_expr_index,
                                                       uint32_t property_expr_index,
                                                       uint32_t material_expr_index,
                                                       bool contacts);
  /** The run of an Each Object event route being walked (ColumnWalkEvent): its object's index in
   * `state`'s lists, or false when no run of this node's events is active. */
  bool ActiveCollisionEventSlot(int32_t node_field, LN_CollisionEventKind &r_kind, uint32_t &r_cursor) const;
  KX_GameObject *CollisionEventObject(const CollisionEventNodeState &state,
                                      LN_CollisionEventKind kind,
                                      uint32_t cursor) const;
  /** The run's object's index in `state.current` (its contact info), -1 for an exit (no contact
   * left) or a cursor past the event's objects. */
  static int32_t CollisionEventCurrentIndex(const CollisionEventNodeState &state,
                                            LN_CollisionEventKind kind,
                                            uint32_t cursor);
  /** The contacts the logic reads for `owner` this step (the physics environment's cache, or the
   * tests' contacts: LN_SetCollisionContactsForTests). */
  bool CachedCollisionContacts(KX_GameObject *owner,
                               bool collect_contacts,
                               const std::vector<const PHY_CachedCollisionContact *> *&r_contacts) const;
  uint64_t CollisionObjectIdentity(KX_GameObject *object);
  KX_GameObject *ResolveCollisionObjectIdentity(uint64_t identity) const;
  template<class In>
  bool ResolveRigidBodyConstraintBreakEventQuery(In &in,
                  uint32_t object_expr_index,
                  uint32_t constraint_name_expr_index,
                  uint32_t match_any_expr_index,
                  LN_RigidBodyConstraintMatchMode match_mode,
                  KX_GameObject *&r_owner,
                  std::string &r_constraint_name,
                  LN_RigidBodyConstraintMatchMode &r_match_mode);
  template<class In>
  const LN_RigidBodyConstraintBreakEvent *ResolveRigidBodyConstraintBreakEvent(In &in,
                  uint32_t object_expr_index,
                  uint32_t constraint_name_expr_index,
                  uint32_t match_any_expr_index,
                  LN_RigidBodyConstraintMatchMode match_mode);
  template<class In>
  int32_t ResolveRigidBodyConstraintBreakEventCount(In &in,
                  uint32_t object_expr_index,
                  uint32_t constraint_name_expr_index,
                  uint32_t match_any_expr_index,
                  LN_RigidBodyConstraintMatchMode match_mode);
  template<class In>
  LN_Value ResolveRigidBodyConstraintBreakEvents(In &in,
                  uint32_t object_expr_index,
                  uint32_t constraint_name_expr_index,
                  uint32_t match_any_expr_index,
                  LN_RigidBodyConstraintMatchMode match_mode);
  KX_Camera *GetActiveCamera() const;
  KX_Camera *ResolveCameraValue(const LN_Value &value) const;
  KX_GameObject *ResolveObjectValue(const LN_Value &value, bool include_inactive = false) const;
  /** Like ResolveObjectValue, but a reference to an object that is gone stays gone: it does not
   * find another object of the same name. */
  KX_GameObject *ResolveLiveObjectValue(const LN_Value &value, bool include_inactive = false) const;
  /** An item value: text (one of it), an inventory slot, or an object, which is its `item` and
   * `count` game properties (else its name and one). */
  bool ItemOf(const LN_Value &value, std::string &r_item, int32_t &r_count) const;
  std::string ItemNameOf(const LN_Value &value) const;
  /** Where a Library input is empty: the scene's Item Library (Scene > Items and UI). */
  const blender::Collection *SceneItemLibrary() const;
  struct InventoryEditResult {
    int32_t amount = 0;
    /** Add Item: how many it was asked to add (all left over when the edit cannot be kept). */
    int32_t requested = 0;
    std::string made;
    LN_InventoryOutcome outcome = LN_InventoryOutcome::None;
    bool craft = false;
    /** The Removed output: how many it removed or crafted; Add Item: how many went in. */
    int32_t Changed(const LN_InventoryOperation operation) const
    {
      return operation == LN_InventoryOperation::AddItem ? std::max(0, requested - amount) :
                                                           amount;
    }
  };
  /** The Inventory node's operation on `slots`, with the inputs `edit` holds. Amount 0 means all:
   * the whole slot, every one of the item, or the count an added slot or object carries.
   * `other_slots`: Move To Slot into that other inventory. `list_rules` / `other_list_rules`:
   * the Inventory Rules of the two lists (of `owner`), used unless the node's Override is on;
   * null (a list value, no object) uses the node's own inputs. Their Accept Categories always
   * apply. */
  template<class In>
  InventoryEditResult RunInventoryEdit(In &in,
                  const LN_ValueExpression &edit,
                  std::vector<LN_Value> &slots,
                  std::vector<LN_Value> *other_slots = nullptr,
                  const LN_InventoryRules *list_rules = nullptr,
                  const LN_InventoryRules *other_list_rules = nullptr,
                  KX_GameObject *owner = nullptr);
  /** Convert references back into this tree's tables after reading a shared value. */
  void LocalizeValue(LN_Value &value);
  template<class In>
  std::vector<LN_RuntimeRef> ResolveIgnoreObjectRefs(In &in,
                  uint32_t object_expr_index,
                  uint32_t list_expr_index);
  template<class In>
  PHY_IPhysicsController *ResolveRigidBodyAttributeController(In &in,
                  uint32_t object_expr_index);
  KX_GameObject *FindSceneObjectByName(const std::string &name,
                                       bool include_inactive = false) const;
  /** Plan A9: what ResolveObjectValue gives a Constant Object value expression with no ref yet
   * (FindSceneObjectByName without inactive objects), found once for the object list generation
   * and not by name at every read: the object found first, kept for (scene, generation, ticking)
   * and, when there was none, kept as "none" until the generation changes; the menu copy rule
   * (OverlayCounterpart) is applied at every read. UPBGE_LN_CONST_OBJECTS: 0 = the old lookup,
   * unset/1 = this, verify = this and the old lookup, a "lean const objects differs" warning on a
   * difference. */
  KX_GameObject *ResolveConstantObject(uint32_t expression_index, const LN_Value &constant);
  /** The ref of a constant's object: the one made before while it still names `object`. */
  LN_RuntimeRef ConstantObjectRef(uint32_t expression_index, KX_GameObject *object);

 public:
  /** Tests: the mode of ResolveConstantObject (0 old lookup, 1 cached, 2 verify; -1 back to the
   * environment) and how many differences verify found. */
  static void SetConstantObjectModeForTests(int mode);
  static uint32_t ConstantObjectVerifyDifferencesForTests();

 private:
  LN_PhysicsQueryResult RaycastMouseOverTarget(const MT_Vector3 &from,
                                               const MT_Vector3 &to,
                                               KX_GameObject *target_object,
                                               KX_Camera *camera,
                                               const LN_RuntimeRef &ignore_object_ref,
                                               LN_InteractionDomain interaction_domain,
                                               uint64_t command_execution_key);
  LN_PhysicsQueryResult RaycastPointer(const MT_Vector3 &from,
                                       const MT_Vector3 &to,
                                       KX_Camera *camera,
                                       const LN_RuntimeRef &ignore_object_ref,
                                       uint32_t collision_mask,
                                       const std::string &property_name,
                                       bool xray,
                                       LN_InteractionDomain interaction_domain,
                                       uint64_t command_execution_key);
  /** The main-world camera under the mouse (null outside player 0's view while the window is
   * split: the mouse is theirs); for the object a held mouse press drags, that press's camera,
   * so the drag goes on past the view's edge. */
  KX_Camera *PointerWorldCamera(KX_Scene *scene, const KX_GameObject *target = nullptr) const;
  bool ComputeMouseRay(KX_Camera *camera,
                       float distance,
                       MT_Vector3 &r_from,
                       MT_Vector3 &r_to) const;
  bool ProjectWorldToScreen(KX_Camera *camera,
                            const MT_Vector3 &world_position,
                            MT_Vector3 &r_screen_position) const;
  bool ProjectScreenToWorld(KX_Camera *camera,
                            float screen_x,
                            float screen_y,
                            float depth,
                            MT_Vector3 &r_world_position) const;
  bool ProjectCanvasPixelToWorld(KX_Camera *camera,
                                 float pixel_x,
                                 float pixel_y,
                                 float depth,
                                 MT_Vector3 &r_world_position) const;
  /** Stable cross-tree ordering key; intra-tree order is command-record order. */
  uint64_t CommandSortKey(uint32_t instruction_index, uint32_t command_sequence) const;
  const std::vector<LN_Value> *DelayTreePropertySnapshotForGuard(uint32_t bool_expr_index,
                                                                 uint64_t tick_index) const;
  CollisionEventPayload *FindCollisionEventPayload(KX_GameObject *owner,
                                                   const std::string &property_filter,
                                                   const std::string &material_filter);
  void StoreCollisionEventPayload(const CollisionResult &result, uint64_t tick_index);
  void MarkCollisionExitPayload(const CollisionResult &current_query, uint64_t tick_index);
  const CollisionResult *FindCollisionExitPayload(KX_GameObject *owner,
                                                  const std::string &property_filter,
                                                  const std::string &material_filter,
                                                  uint64_t tick_index);

  std::shared_ptr<const LN_Program> m_program;
  uint64_t m_programRevision = 1;
  KX_GameObject *m_gameObject = nullptr;
  LN_Manager *m_logicManager = nullptr;
  uint32_t m_sceneObjectIndex = 0;
  uint32_t m_appliedTreeIndex = 0;
  bool m_enabled = true;
  bool m_runtimeActive = true;
  bool m_initExecuted = false;
  BatchNote m_batchNote;
  /** IsColumnLane (set and cleared by LN_ColumnRunner). */
  bool m_columnLane = false;
  /** LN_ColumnRunner's entry of this tree's program (plan C2 P1: no hash lookup a tick), valid
   * while the runner (its id, never repeated) and the program's unique id are those it was found
   * for. */
  uint64_t m_columnEntryRunner = 0;
  uint64_t m_columnEntryProgramId = 0;
  void *m_columnEntry = nullptr;
  bool m_uiEventRoutesOnly = false;
  /** Every N Steps gates of the program, one each (PrepareStepGates). */
  struct StepGateState {
    bool due = false;
    double time_passed = 0.0;
    double unscaled_time_passed = 0.0;
    /** The steps `time_passed` covers (StepGateStepsSinceLastRun): StepGateStepsCovered. */
    uint32_t steps = 1;
    /** The step of the routes' last run, a Run Logic Tree pass's included: their next run covers
     * the steps since (a pass between due steps counted its steps twice before 2026-10-31). */
    uint64_t last_run_step = UINT64_MAX;
  };
  LN_StateSpan<StepGateState> m_stepGates;
  /** The step PrepareStepGates last looked at, and the first one it saw. */
  uint64_t m_stepGatesStep = UINT64_MAX;
  uint64_t m_stepGatesFirstStep = UINT64_MAX;
  /** The delta time of the event being run (one step's length in a Logic + Physics step): the
   * Time Passed of a step node without Every N Steps. */
  double m_eventDeltaTime = 0.0;
  uint32_t m_stepSpread = 0;
  bool m_hasStepSpread = false;
  uint64_t m_randomStream = 0;
  /** On Init runs now (RandomTick). */
  bool m_randomInInit = false;
  bool m_stepGatesAnyDue = false;
  /** A Run Logic Tree pass runs every route of the step, gated ones too, each with the time since
   * it last ran (StepGateStepsSinceLastRun). */
  bool m_stepGatesForced = false;
  /** Steps a gate's routes cover when they run at `step`: those since their last run (since the
   * tree's first step before the first run), at least 1 (a second pass in one step counts one
   * step, as a route without Every N gets in every pass) and at most `interval`. */
  uint64_t StepGateStepsSinceLastRun(const StepGateState &state,
                                     uint64_t step,
                                     uint64_t interval) const;
  /** Gives each gate the time its routes cover in a Run Logic Tree pass at `step`. */
  void PrepareForcedStepGates(uint64_t step, double delta_time, double unscaled_delta_time);
  /** Notes `step` as the gates' last run: the due ones after a step's run, all after a pass. */
  void NoteStepGatesRan(uint64_t step, bool forced);
  uint64_t m_snapshotTick = UINT64_MAX;
  uint64_t m_queryCacheWarmTick = UINT64_MAX;
  uint64_t m_executionScopeSerial = 0;
  uint64_t m_activeExecutionScopeSerial = 0;
  /** Monotonic execution point for command-ordered query cache invalidation. */
  uint64_t m_commandExecutionSerial = 0;
  LN_SnapshotChannelMask m_snapshotChannelMask = LN_SNAPSHOT_CHANNEL_NONE;
  bool m_hasTimeFlowRuntimeState = false;
  bool m_hasQueryExpressions = false;
  /** The tree's per-object state, all in one block laid out by `m_stateLayout` (the program's):
   * the views below point into `m_stateBlock`. Built with the program (RebuildStateBlock). */
  std::shared_ptr<const LN_TreeStateLayout> m_stateLayout;
  LN_StateBlockMemory m_stateBlock;
  LN_StateSpan<InstructionStamp> m_instructionStamps;
  RegisterExpressionRuntimeState m_registerExpressionState;
  LN_StateSpan<QueryExpressionRuntimeState> m_queryExpressionStates;
  LN_StateSpan<BoolExpressionRuntimeState> m_boolStates;
  LN_StateSpan<FloatExpressionRuntimeState> m_floatStates;
  LN_StateSpan<TimeFlowRuntimeState> m_timeFlowStates;
  LN_StateSpan<FlowControlRuntimeState> m_flowControlStates;
  LN_StateSpan<InstructionMotionState> m_motionStates;
  LN_StateSpan<ValueChangeState> m_valueChangeStates;
  /** States asked for where the layout planned none (an index past the program's, or a kind not
   * listed in BuildStateLayout): they behave the same, kept here. Made when first needed. */
  struct StateOverflow {
    std::unordered_map<uint32_t, BoolExpressionRuntimeState> bool_states;
    std::unordered_map<uint32_t, FloatExpressionRuntimeState> float_states;
    std::unordered_map<uint32_t, InstructionMotionState> motion_states;
    std::unordered_map<uint32_t, ValueChangeState> value_changes;
    std::unordered_map<uint32_t, QueryExpressionRuntimeState> queries;
  };
  std::unique_ptr<StateOverflow> m_stateOverflow;
  StateOverflow &Overflow();
  std::unordered_map<uint32_t, UICommandCompletionState> m_uiCommandCompletions;
  std::unordered_map<uint32_t, ActionCompletionState> m_actionCompletions;
  uint64_t m_collisionResultsTick = UINT64_MAX;
  std::vector<CollisionResult> m_collisionResults;
  std::vector<CollisionEventPayload> m_collisionEventPayloads;
  CollisionResult m_collisionExitPayloadScratch;
  /** ResolveCollisionResult's object -> hit_objects index, once a result lists more objects than
   * a short scan pays for (kept between ticks: no allocation per tick). */
  blender::Map<const KX_GameObject *, uint32_t> m_collisionIndexScratch;
  /** UpdateCollisionEvents' identity lookup past a short scan, kept between ticks. */
  blender::Set<uint64_t> m_collisionIdentityScratch;
  /** Each Object On Collision nodes, by node index (resized on first use; reset with the
   * program). */
  std::vector<CollisionEventNodeState> m_collisionEventNodes;
  /** The Each Object event route the walk runs now (its repeat key) and which object of the
   * event (0 in the step's walk, 1... in the repeats); LN_INVALID_INDEX outside such routes. */
  uint32_t m_collisionEventRoute = LN_INVALID_INDEX;
  uint32_t m_collisionEventCursor = 0;
  /** Between HoldCollisionIdentitiesAsObjects and RenewCollisionIdentities: the states'
   * `previous` hold object addresses. */
  bool m_collisionIdentitiesHeld = false;
  std::vector<ObjectRefSlot> m_objectRefs;
  /** MakeObjectRef's object -> slot, kept once the tree holds more than kObjectRefShortScan
   * slots (each object's ref was found by a scan of every slot: listing 4,000 touching objects
   * as Collided Objects took 2.7 ms a step, 2026-11-09). */
  blender::Map<const KX_GameObject *, uint32_t> m_objectRefIndex;
  bool m_objectRefsIndexed = false;
  /** The slots InvalidateObjectRef emptied, as a min-heap: a new object takes the lowest, as the
   * scan for the first empty slot gave it. */
  std::vector<uint32_t> m_freeObjectRefSlots;
  /** ResolveConstantObject's memory, one entry per Constant Object expression this tree read. */
  struct ConstantObjectEntry {
    uint32_t expression_index = LN_INVALID_INDEX;
    const void *scene_key = nullptr;
    uint64_t generation = 0;
    bool ticking = false;
    bool resolved = false;
    KX_GameObject *base = nullptr;
    LN_RuntimeRef ref;
  };
  std::vector<ConstantObjectEntry> m_constantObjects;
  std::vector<RuntimeRefSlot> m_runtimeRefs;
  std::vector<LN_Value> m_treeProperties;
  std::vector<LoopRuntimeState> m_loopStates;
  LN_RuntimeTreeId m_denseRuntimeTreeId;
  LN_ObjectHandle m_ownerObjectHandle;
  std::vector<LN_SharedStringId> m_sharedStringIds;
  std::vector<LN_EventSubjectId> m_eventSubjectIds;
  std::vector<LN_GamePropertyId> m_gamePropertyIds;
  std::vector<LN_TreePropertyId> m_treePropertyIds;
  uint32_t m_activeLoopBodyFrame = LN_INVALID_INDEX;
  const std::vector<LN_Value> *m_activeTreePropertySnapshot = nullptr;
  LN_Snapshot m_snapshot;
  LN_RuntimeProfileCounters *m_activeProfileCounters = nullptr;
  mutable RuntimeFallbackReport m_runtimeFallbackReport;
};
