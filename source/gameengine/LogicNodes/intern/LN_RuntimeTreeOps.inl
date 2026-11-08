/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The execution of one event's instructions, written as members of LN_RuntimeTree templated on an
 * execution frame (lean runtime plan, phases A11 and A12): what used to be lambdas inside
 * LN_RuntimeTree::ExecuteEvent. The frame (LN_ExecFrame.h) bundles the event, the command buffer
 * (the write sink), the tick context and the reader of expressions (Bool, Int, Float, ...). Today
 * the one frame reads through the tree's recursive Resolve* functions; the lean VM gets a frame of
 * its own that reads from its slots, and runs the very same handlers.
 *
 * Included by LN_RuntimeTree.cpp (the handlers use that file's helpers), before ExecuteEvent. */

/** Makes the command buffer and the command execution key current for the queries one
 * instruction's reads make (a read that must see the step's earlier writes), and puts back the
 * previous ones. */
struct LN_ScopedCommandQueryContext {
  LN_TickContext &context;
  LN_CommandBuffer *previous_command_buffer;
  uint64_t previous_command_execution_key;

  LN_ScopedCommandQueryContext(LN_TickContext &context,
                               LN_CommandBuffer &command_buffer,
                               const uint64_t command_execution_key)
      : context(context),
        previous_command_buffer(context.command_buffer),
        previous_command_execution_key(context.command_execution_key)
  {
    context.command_buffer = &command_buffer;
    context.command_execution_key = command_execution_key;
  }

  ~LN_ScopedCommandQueryContext()
  {
    context.command_buffer = previous_command_buffer;
    context.command_execution_key = previous_command_execution_key;
  }
};

uint64_t LN_RuntimeTree::NextExecutionScopeSerial()
{
  m_executionScopeSerial++;
  if (m_executionScopeSerial == 0) {
    m_executionScopeSerial++;
  }
  return m_executionScopeSerial;
}

uint64_t LN_RuntimeTree::NextCommandExecutionKey()
{
  m_commandExecutionSerial++;
  if (m_commandExecutionSerial == 0) {
    m_commandExecutionSerial++;
  }
  return m_commandExecutionSerial;
}

void LN_RuntimeTree::ProfileDirectExecOp(const LN_ExecOp &op)
{
  if (m_activeProfileCounters == nullptr) {
    return;
  }
  m_activeProfileCounters->instruction_dispatch_count++;
  m_activeProfileCounters->exec_direct_instruction_count++;
  if (op.fallback_requirements != LN_RUNTIME_FALLBACK_NONE) {
    m_activeProfileCounters->fallback_path_count++;
  }
}

/* ---- the frame's helpers (they were lambdas of ExecuteEvent) ---- */

template<class R>
KX_GameObject *LN_ExecFrameT<R>::ResolveCommandTarget(const LN_Instruction &instruction)
{
  if (instruction.value_expr_index == LN_INVALID_INDEX) {
    return tree.m_gameObject;
  }

  return tree.ResolveObjectValue(Value(instruction.value_expr_index, MakeNoneValue()));
}

template<class R>
KX_GameObject *LN_ExecFrameT<R>::ResolveRequiredObject(const uint32_t expr_index)
{
  if (expr_index == LN_INVALID_INDEX) {
    return nullptr;
  }

  return tree.ResolveObjectValue(Value(expr_index, MakeNoneValue()));
}

template<class R>
bool LN_ExecFrameT<R>::RelativeTransformIsSafe(KX_GameObject *target,
                                           const LN_Instruction &instruction,
                                           const uint32_t /*instruction_id*/,
                                           const LN_CommandBuffer::CommandType command_type)
{
  if (event != LN_Event::OnUpdate || target == nullptr || !target->IsDynamic()) {
    return true;
  }

  LN_CommandBuffer::Command diagnostic;
  diagnostic.type = command_type;
  diagnostic.object = target;
  diagnostic.runtime_tree = &tree;
  diagnostic.sort_key = SortKey();
  diagnostic.source_ref_index = instruction.source_ref_index;
  LN_WarnCommandFailureOnce(
      diagnostic,
      "Frame Update cannot apply a relative transform to a dynamic physics body; drive the "
      "node from Logic + Physics flow or use a non-dynamic object");
  return false;
}

template<class R>
KX_GameObject *LN_ExecFrameT<R>::ResolveAddObjectTemplate(const uint32_t expr_index)
{
  if (expr_index == LN_INVALID_INDEX) {
    return nullptr;
  }

  const LN_Value value = Value(expr_index, MakeNoneValue());
  if (value.type != LN_ValueType::ObjectRef) {
    return nullptr;
  }

  if (!value.reference_name.empty()) {
    if (KX_GameObject *template_object = tree.FindSceneObjectByName(value.reference_name, true)) {
      return template_object;
    }
  }

  return tree.ResolveObjectValue(value, true);
}

template<class R>
KX_GameObject *LN_ExecFrameT<R>::ResolveObjectExpression(const uint32_t expr_index)
{
  if (expr_index == LN_INVALID_INDEX) {
    return tree.m_gameObject;
  }
  return ResolveRequiredObject(expr_index);
}

template<class R>
LN_VectorCommandPayload LN_ExecFrameT<R>::VectorCommandPayload(const LN_Instruction &instruction)
{
  if (tree.m_program != nullptr &&
      instruction.command_payload_index < tree.m_program->GetVectorCommandPayloads().size())
  {
    return tree.m_program->GetVectorCommandPayloads()[instruction.command_payload_index];
  }

  LN_VectorCommandPayload payload;
  payload.object_value_expr_index = instruction.value_expr_index;
  payload.vector_expr_index = instruction.vector_expr_index;
  payload.secondary_vector_expr_index = instruction.secondary_vector_expr_index;
  payload.bool_expr_index = instruction.bool_expr_index;
  payload.vector_value = instruction.vector_value;
  payload.secondary_vector_value = instruction.secondary_vector_value;
  payload.operation_mode = instruction.vector_operation_mode;
  payload.operation_channel = instruction.vector_operation_channel;
  payload.operation_mask = instruction.vector_operation_mask;
  payload.bool_value = instruction.bool_value;
  return payload;
}

template<class R>
LN_GamePropertyCommandPayload LN_ExecFrameT<R>::GamePropertyCommandPayload(
    const LN_Instruction &instruction)
{
  if (tree.m_program != nullptr &&
      instruction.command_payload_index < tree.m_program->GetGamePropertyCommandPayloads().size())
  {
    return tree.m_program->GetGamePropertyCommandPayloads()[instruction.command_payload_index];
  }

  LN_GamePropertyCommandPayload payload;
  payload.property_ref_index = instruction.property_ref_index;
  payload.value_type = instruction.property_value_type;
  payload.bool_expr_index = instruction.bool_expr_index;
  payload.int_expr_index = instruction.int_expr_index;
  payload.float_expr_index = instruction.float_expr_index;
  payload.string_expr_index = instruction.string_expr_index;
  payload.vector_expr_index = instruction.vector_expr_index;
  payload.color_expr_index = instruction.color_expr_index;
  payload.modify_op = LN_PropertyModifyOp(instruction.int_value);
  payload.modify_min_expr_index = instruction.secondary_float_expr_index;
  payload.modify_max_expr_index = instruction.tertiary_float_expr_index;
  if (instruction.property_value_type != LN_ValueType::None &&
      (payload.modify_op != LN_PropertyModifyOp::None ||
       instruction.bool_expr_index != LN_INVALID_INDEX ||
       instruction.int_expr_index != LN_INVALID_INDEX ||
       instruction.float_expr_index != LN_INVALID_INDEX ||
       instruction.string_expr_index != LN_INVALID_INDEX ||
       instruction.vector_expr_index != LN_INVALID_INDEX ||
       instruction.color_expr_index != LN_INVALID_INDEX))
  {
    payload.object_value_expr_index = instruction.value_expr_index;
  }
  else {
    payload.value_expr_index = instruction.value_expr_index;
  }
  return payload;
}

template<class R>
LN_RigidBodyConstraintCommandPayload LN_ExecFrameT<R>::RigidBodyConstraintCommandPayload(
    const LN_Instruction &instruction)
{
  if (tree.m_program != nullptr &&
      instruction.command_payload_index <
          tree.m_program->GetRigidBodyConstraintCommandPayloads().size())
  {
    return tree.m_program->GetRigidBodyConstraintCommandPayloads()[instruction.command_payload_index];
  }

  LN_RigidBodyConstraintCommandPayload payload;
  payload.object_value_expr_index = instruction.value_expr_index;
  payload.target_value_expr_index = instruction.secondary_value_expr_index;
  payload.name_string_expr_index = instruction.string_expr_index;
  payload.bool_expr_indices[size_t(LN_RigidBodyConstraintBoolInput::UseWorldSpace)] =
      instruction.bool_expr_index;
  payload.vector_expr_indices[size_t(LN_RigidBodyConstraintVectorInput::Pivot)] =
      instruction.vector_expr_index;
  return payload;
}

/* ---- the handlers (they were lambdas of ExecuteEvent) ---- */

template<class Frame>
void LN_RuntimeTree::OpMouseLookCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  KX_GameObject *body = ResolveObjectValue(
      ef.Value(instruction.value_expr_index, MakeNoneValue()));
  if (body == nullptr) {
    body = m_gameObject;
  }

  KX_GameObject *head = ResolveObjectValue(
      ef.Value(instruction.property_ref_index, MakeNoneValue()));
  if (head == nullptr) {
    head = body;
  }
  if (body == nullptr || head == nullptr) {
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }

  const MouseLookInputState mouse = ReadMouseLookInputState(
      m_snapshot.GetInputSnapshot().GetMouse());
  /* Center Mouse holds the cursor in the middle, which needs a cursor the game can move: on
   * Wayland only a hidden one. A shown cursor there turns the view by how far it moved, as
   * without Center Mouse, instead of by its distance from a middle it never reaches (the view
   * spun). */
  const KX_KetsjiEngine *look_engine = ActiveEngine();
  const bool cursor_movable = look_engine == nullptr || look_engine->IsCursorMovable();
  const bool center_mouse = instruction.bool_value && cursor_movable;
  const int32_t front_axis = std::clamp(instruction.int_value, 0, 1);

  const LN_InputSnapshot &input_snapshot = m_snapshot.GetInputSnapshot();
  RAS_ICanvas *canvas = ActiveCanvas();
  int32_t canvas_width = 0;
  int32_t canvas_height = 0;
  if (canvas != nullptr) {
    canvas_width = canvas->GetWidth();
    canvas_height = canvas->GetHeight();
  }
  else if (input_snapshot.HasCanvas()) {
    canvas_width = input_snapshot.GetCanvasWidth();
    canvas_height = input_snapshot.GetCanvasHeight();
  }
  const uint32_t state_index = InstructionStateIndex(event, instruction_id);
  InstructionMotionState *state = state_index < m_instructionStamps.size() ?
                                      &MotionState(state_index) :
                                      nullptr;
  /* The mouse is player 0's: while a menu or a UI in the game world uses it (or the window has
   * no focus), the view waits and the cursor stays free. After that (or a pause) it starts over
   * from where the cursor is, so the view does not jump. */
  if (context.mouse_look_waits) {
    if (state != nullptr) {
      state->mouse_look_initialized = false;
      state->mouse_look_has_previous_position = false;
      state->mouse_look_smooth_x = 0.0f;
      state->mouse_look_smooth_y = 0.0f;
      state->mouse_look_ui_serial = context.mouse_ui_serial;
    }
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }
  /* Only while it turns: a menu shows the cursor while the view waits. */
  if (instruction.bool_value && !cursor_movable) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      CM_Warning("Mouse Look: the cursor cannot be held in the middle here (Wayland moves only "
                 "a hidden cursor, and none with --debug), so turning stops at the window's "
                 "edge; hide the cursor with Set Cursor Visibility for endless turning");
    }
  }
  /* Also when it starts or stops holding the cursor (the cursor was hidden or shown). */
  if (state != nullptr && (state->mouse_look_ui_serial != context.mouse_ui_serial ||
                           state->mouse_look_centered != center_mouse))
  {
    state->mouse_look_ui_serial = context.mouse_ui_serial;
    state->mouse_look_centered = center_mouse;
    state->mouse_look_initialized = false;
    state->mouse_look_has_previous_position = false;
    state->mouse_look_smooth_x = 0.0f;
    state->mouse_look_smooth_y = 0.0f;
  }
  auto remember_mouse_position = [&](const int32_t x, const int32_t y) {
    if (state != nullptr) {
      state->mouse_look_previous_x = x;
      state->mouse_look_previous_y = y;
      state->mouse_look_has_previous_position = true;
    }
  };
  auto remember_current_mouse_position = [&]() {
    if (mouse.has_position) {
      remember_mouse_position(mouse.x, mouse.y);
    }
  };
  /* The cursor is held in the middle of the window; with split screen, in the middle of player
   * 0's view (the mouse is theirs; Set Split Screen's Keep Mouse in View holds it there). */
  int32_t center_x = MouseLookCenterPixel(canvas_width);
  int32_t center_y = MouseLookCenterPixel(canvas_height);
  if (KX_Scene *look_scene = ActiveEngine() != nullptr && m_gameObject != nullptr ?
                                 m_gameObject->GetScene() :
                                 nullptr)
  {
    RAS_Rect mouse_view;
    const KX_PlayerViews &views = look_scene->GetPlayerViews();
    if (views.GetMouseView(look_scene->GetActiveCamera(), views.GetWindow(), mouse_view)) {
      center_x = (mouse_view.GetLeft() + mouse_view.GetRight()) / 2;
      center_y = canvas_height - (mouse_view.GetBottom() + mouse_view.GetTop()) / 2;
    }
  }
  auto append_center_cursor_command = [&]() {
    if (canvas == nullptr) {
      return;
    }
    const int32_t x = center_x;
    const int32_t y = center_y;
    command_buffer.AppendSetCursorPosition(
        x, y, ef.SortKey(), instruction.source_ref_index);
    remember_mouse_position(x, y);
  };

  float offset_x = 0.0f;
  float offset_y = 0.0f;
  const float pixel_to_rotation_scale = MouseLookPixelToRotationScale();
  const bool has_frame_delta = mouse.delta_x != 0 || mouse.delta_y != 0;
  const bool has_canvas_position = mouse.has_position && canvas_width > 0 && canvas_height > 0;
  if (center_mouse && has_canvas_position) {
    offset_x = -float(mouse.x - center_x) * pixel_to_rotation_scale;
    offset_y = -float(mouse.y - center_y) * pixel_to_rotation_scale;
  }
  else if (center_mouse && has_frame_delta) {
    offset_x = -float(mouse.delta_x) * pixel_to_rotation_scale;
    offset_y = -float(mouse.delta_y) * pixel_to_rotation_scale;
  }
  else if (!center_mouse && mouse.has_position && state != nullptr &&
           state->mouse_look_has_previous_position)
  {
    offset_x = -float(mouse.x - state->mouse_look_previous_x) * pixel_to_rotation_scale;
    offset_y = -float(mouse.y - state->mouse_look_previous_y) * pixel_to_rotation_scale;
  }
  else {
    offset_x = -float(mouse.delta_x) * pixel_to_rotation_scale;
    offset_y = -float(mouse.delta_y) * pixel_to_rotation_scale;
  }

  const MT_Vector3 invert = ef.Vector(
      instruction.secondary_vector_expr_index,
      MT_Vector3(instruction.vector_value.x(), instruction.vector_value.y(), 0.0f));
  const MT_Vector3 cap_x = ef.Vector(
      instruction.color_expr_index,
      MT_Vector3(
          instruction.secondary_vector_value.x(), instruction.secondary_vector_value.y(), 0.0f));
  const MT_Vector3 cap_y = ef.Vector(
      instruction.int_expr_index,
      MT_Vector3(instruction.color_value.x(), instruction.color_value.y(), 0.0f));
  const bool use_cap_x = ef.Bool(
      instruction.bool_expr_index, instruction.secondary_vector_value.z() > 0.5f);
  const bool use_cap_y = ef.Bool(
      instruction.secondary_bool_expr_index, instruction.color_value.w() > 0.5f);
  const float sensitivity = std::max(0.0f,
                                     ef.Float(instruction.float_expr_index,
                                                            instruction.vector_value.z()));
  const float smoothing = std::clamp(ef.Float(instruction.string_expr_index,
                                                            instruction.color_value.z()),
                                     0.0f,
                                     1.0f);

  if (std::fabs(invert.x()) > 0.5f) {
    offset_x = -offset_x;
  }
  if (std::fabs(invert.y()) > 0.5f) {
    offset_y = -offset_y;
  }

  offset_x *= sensitivity * 0.002f;
  offset_y *= sensitivity * 0.002f;

  /* Only a real game window has a system pointer to wait for. */
  auto start_settling = [&]() {
    if (canvas != nullptr) {
      state->mouse_look_settle = MOUSE_LOOK_SETTLE_SECONDS;
      state->mouse_look_settle_runs = MOUSE_LOOK_SETTLE_MAX_RUNS;
    }
  };
  if (center_mouse && mouse.has_position && state != nullptr && !state->mouse_look_initialized) {
    state->mouse_look_initialized = true;
    state->mouse_look_smooth_x = 0.0f;
    state->mouse_look_smooth_y = 0.0f;
    start_settling();
    append_center_cursor_command();
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }
  if (!center_mouse && mouse.has_position && state != nullptr &&
      !state->mouse_look_has_previous_position)
  {
    state->mouse_look_initialized = true;
    state->mouse_look_smooth_x = 0.0f;
    state->mouse_look_smooth_y = 0.0f;
    start_settling();
    remember_current_mouse_position();
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }
  /* Settling: the cursor is held, the view does not turn yet. */
  if (state != nullptr && state->mouse_look_settle > 0.0f) {
    const double step = context.unscaled_dt > 0.0 ? context.unscaled_dt : context.delta_time;
    state->mouse_look_settle -= float(std::max(step, 0.0));
    if (--state->mouse_look_settle_runs <= 0) {
      state->mouse_look_settle = 0.0f;
    }
    if (center_mouse && mouse.has_position) {
      append_center_cursor_command();
    }
    else if (!center_mouse) {
      remember_current_mouse_position();
    }
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }
  if (state != nullptr) {
    const float target_x = offset_x * MOUSE_LOOK_REFERENCE_PIXELS;
    const float target_y = offset_y * MOUSE_LOOK_REFERENCE_PIXELS;
    const float smoothing_factor = MouseLookSmoothingFactor(smoothing, context.delta_time);
    state->mouse_look_smooth_x += (target_x - state->mouse_look_smooth_x) * smoothing_factor;
    state->mouse_look_smooth_y += (target_y - state->mouse_look_smooth_y) * smoothing_factor;
    offset_x = state->mouse_look_smooth_x / MOUSE_LOOK_REFERENCE_PIXELS;
    offset_y = state->mouse_look_smooth_y / MOUSE_LOOK_REFERENCE_PIXELS;
    state->mouse_look_initialized = true;
  }

  if (std::fabs(offset_x) <= MT_EPSILON && std::fabs(offset_y) <= MT_EPSILON) {
    if (center_mouse && mouse.has_position) {
      append_center_cursor_command();
    }
    else if (!center_mouse) {
      remember_current_mouse_position();
    }
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }

  const int rot_axis = 1 - front_axis;
  if (state != nullptr) {
    if (use_cap_x) {
      offset_x = ClampMouseLookDelta(state->mouse_look_angle_x, offset_x, cap_x);
    }
    if (use_cap_y) {
      offset_y = ClampMouseLookDelta(state->mouse_look_angle_y, offset_y, cap_y);
    }
  }

  if (std::fabs(offset_x) > MT_EPSILON) {
    command_buffer.AppendApplyRotation(body,
                                       MouseLookRotationVector(2, offset_x),
                                       false,
                                       ef.SortKey(),
                                       instruction.source_ref_index);
  }
  if (std::fabs(offset_y) > MT_EPSILON) {
    command_buffer.AppendApplyRotation(head,
                                       MouseLookRotationVector(rot_axis, offset_y),
                                       true,
                                       ef.SortKey(),
                                       instruction.source_ref_index);
  }

  if (center_mouse && mouse.has_position) {
    append_center_cursor_command();
  }
  else if (!center_mouse) {
    remember_current_mouse_position();
  }

  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpTimeFlowControlInstruction(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  const std::vector<LN_InstructionHeader> &instructions = ef.instructions;

  /* Whether a UI event route armed the state: only such states are ticked while paused. */
  const bool ui_event_route = (instructions[instruction_id].flags &
                               LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE) != 0;
  switch (instruction.opcode) {
    case LN_OpCode::ArmNextFrame: {
      if (instruction.int_value >= 0) {
        TimeFlowArmNextFrame(uint32_t(instruction.int_value), ui_event_route, context);
      }
      break;
    }
    case LN_OpCode::ArmTimer: {
      if (instruction.int_value >= 0) {
        TimeFlowArmTimer(
            uint32_t(instruction.int_value),
            ui_event_route,
            event,
            context,
            [&]() { return ef.Float(instruction.float_expr_index, 0.0f); },
            [&]() { return ef.Bool(instruction.secondary_bool_expr_index, false); });
      }
      break;
    }
    case LN_OpCode::ArmDelay: {
      if (instruction.int_value >= 0) {
        TimeFlowArmDelay(
            uint32_t(instruction.int_value),
            ui_event_route,
            event,
            context,
            [&]() { return ef.Float(instruction.float_expr_index, 0.0f); },
            [&]() { return ef.Bool(instruction.secondary_bool_expr_index, false); });
      }
      break;
    }
    case LN_OpCode::UpdatePulsify: {
      if (instruction.int_value >= 0) {
        TimeFlowUpdatePulsify(
            uint32_t(instruction.int_value),
            event,
            context,
            [&]() { return ef.Float(instruction.float_expr_index, 0.5f); },
            [&]() { return ef.Bool(instruction.secondary_bool_expr_index, false); });
      }
      break;
    }
    case LN_OpCode::UpdateBarrier: {
      if (instruction.int_value >= 0) {
        TimeFlowUpdateBarrier(
            uint32_t(instruction.int_value),
            event,
            context,
            [&]() {
              return instruction.bool_expr_index == LN_INVALID_INDEX ?
                         true :
                         ef.Bool(instruction.bool_expr_index, false);
            },
            [&]() { return ef.Float(instruction.float_expr_index, 0.0f); },
            [&]() { return ef.Bool(instruction.secondary_bool_expr_index, false); });
      }
      break;
    }
    case LN_OpCode::TryOnce: {
      if (instruction.int_value >= 0) {
        BoolExpressionRuntimeState &state = BoolExpressionState(uint32_t(instruction.int_value));
        state.cached_result = !state.active;
        if (state.cached_result) {
          state.active = true;
        }
        state.initialized = true;
        state.last_update_tick = context.execution_scope_serial;
      }
      break;
    }
    case LN_OpCode::ResetOnce: {
      if (instruction.int_value >= 0) {
        BoolExpressionRuntimeState &state = BoolExpressionState(uint32_t(instruction.int_value));
        state.active = false;
        state.cached_result = false;
        state.initialized = true;
        state.last_update_tick = 0;
      }
      break;
    }
    case LN_OpCode::SetFlowGate: {
      if (instruction.int_value < 0 || m_program == nullptr ||
          uint32_t(instruction.int_value) >= m_program->GetBoolExpressions().size())
      {
        return;
      }
      const uint32_t expression_index = uint32_t(instruction.int_value);
      const LN_BoolExpression &expression = m_program->GetBoolExpressions()[expression_index];
      if (expression.kind != LN_BoolExpressionKind::FlowGateOpen) {
        return;
      }
      /* Resolve once before mutation so Start Open is sampled exactly once. */
      (void)ef.Bool(expression_index, false);
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      switch (LN_FlowGateControl(instruction.secondary_int_value)) {
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
          return;
      }
      break;
    }
    case LN_OpCode::TryFlowGate: {
      if (instruction.int_value < 0 || m_program == nullptr ||
          uint32_t(instruction.int_value) >= m_program->GetBoolExpressions().size())
      {
        return;
      }
      const uint32_t expression_index = uint32_t(instruction.int_value);
      if (m_program->GetBoolExpressions()[expression_index].kind !=
          LN_BoolExpressionKind::FlowGateOpen)
      {
        return;
      }
      if (!ef.Bool(expression_index, false)) {
        /* A rejected Enter is intentionally not marked executed; the Exit route reads this
         * instruction's pulse and therefore remains false. */
        return;
      }
      break;
    }
    case LN_OpCode::TryCooldown: {
      if (instruction.int_value >= 0) {
        TimeFlowTryCooldown(
            uint32_t(instruction.int_value),
            ui_event_route,
            event,
            context,
            [&]() { return ef.Float(instruction.float_expr_index, 0.0f); },
            [&]() { return ef.Bool(instruction.secondary_bool_expr_index, false); });
      }
      break;
    }
    case LN_OpCode::ResetCooldown: {
      if (instruction.int_value >= 0) {
        TimeFlowResetCooldown(uint32_t(instruction.int_value), event, context);
      }
      break;
    }
    case LN_OpCode::TryFlowRouter: {
      if (instruction.int_value < 0 || instruction.secondary_int_value <= 0) {
        return;
      }
      if (FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value)))
      {
        const uint32_t output_count = uint32_t(instruction.secondary_int_value);
        const uint32_t selected_index = state->next_route_index % output_count;
        state->last_route_index = selected_index;
        state->next_route_index = (selected_index + 1u) % output_count;
        state->routed_scope_serial = context.execution_scope_serial;
      }
      break;
    }
    case LN_OpCode::ResetFlowRouter: {
      if (instruction.int_value < 0) {
        return;
      }
      if (FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value)))
      {
        state->next_route_index = 0;
        state->last_route_index = LN_INVALID_INDEX;
        state->routed_scope_serial = 0;
      }
      break;
    }
    case LN_OpCode::TryRepeatLimit: {
      if (instruction.int_value < 0 || instruction.int_expr_index == LN_INVALID_INDEX) {
        return;
      }
      if (FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value)))
      {
        const int32_t requested_limit = ef.Int(
            instruction.int_expr_index, 0);
        const uint32_t limit = requested_limit > 0 ? uint32_t(requested_limit) : 0u;
        if (state->accepted_count < limit) {
          state->accepted_count++;
          state->accepted_scope_serial = context.execution_scope_serial;
          state->blocked_scope_serial = 0;
        }
        else {
          state->accepted_scope_serial = 0;
          state->blocked_scope_serial = context.execution_scope_serial;
        }
      }
      break;
    }
    case LN_OpCode::ResetRepeatLimit: {
      if (instruction.int_value < 0) {
        return;
      }
      if (FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value)))
      {
        state->accepted_count = 0;
        state->accepted_scope_serial = 0;
        state->blocked_scope_serial = 0;
      }
      break;
    }
    case LN_OpCode::SampleMotion: {
      if (instruction.int_value < 0) {
        return;
      }
      FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value));
      if (state == nullptr) {
        return;
      }

      /* A graph may route the same pulse to this node more than once. Ignore duplicate samples
       * before resolving their source so a later branch cannot invalidate or replace the sample
       * already accepted for this engine tick. ResetMotion clears this guard intentionally. */
      if (state->motion_last_sample_tick == context.tick_index) {
        break;
      }

      const LN_MotionSourceMode source_mode = LN_MotionSourceMode(
          instruction.secondary_int_value);
      MT_Vector3 position(0.0f, 0.0f, 0.0f);
      LN_RuntimeRef source_ref;
      bool source_valid = false;
      if (source_mode == LN_MotionSourceMode::Position) {
        if (instruction.vector_expr_index != LN_INVALID_INDEX) {
          position = ef.Vector(
              instruction.vector_expr_index, instruction.vector_value);
          source_valid = vector_is_finite(position);
        }
      }
      else if (source_mode == LN_MotionSourceMode::OwnerObject ||
               source_mode == LN_MotionSourceMode::Object)
      {
        KX_GameObject *object = source_mode == LN_MotionSourceMode::OwnerObject ? m_gameObject :
                                                                                  nullptr;
        if (source_mode == LN_MotionSourceMode::Object &&
            instruction.value_expr_index != LN_INVALID_INDEX)
        {
          object = ResolveObjectValue(
              ef.Value(instruction.value_expr_index, MakeNoneValue()));
        }
        if (object != nullptr) {
          FlushPendingRaycastTransforms(object, context);
          position = object->NodeGetWorldPosition();
          source_ref = MakeObjectRef(object, object->GetName());
          source_valid = vector_is_finite(position) && source_ref.IsValid();
        }
      }
      else {
        return;
      }

      if (!source_valid || !std::isfinite(context.current_time)) {
        state->motion_initialized = false;
        state->motion_valid = false;
        state->motion_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
        state->motion_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
        state->motion_speed = 0.0f;
        state->motion_predicted_position = state->motion_position;
        break;
      }

      const bool object_source = source_mode != LN_MotionSourceMode::Position;
      const bool source_changed = state->motion_source_mode != int32_t(source_mode) ||
                                  (object_source &&
                                   !RuntimeRefsEqual(state->motion_source_ref, source_ref));
      const double elapsed = context.current_time - state->motion_last_sample_time;
      if (!state->motion_initialized || source_changed || !std::isfinite(elapsed) ||
          elapsed < 0.0)
      {
        state->motion_initialized = true;
        state->motion_valid = false;
        state->motion_source_mode = int32_t(source_mode);
        state->motion_source_ref = source_ref;
        state->motion_last_sample_tick = context.tick_index;
        state->motion_last_sample_time = context.current_time;
        state->motion_position = position;
        state->motion_previous_position = position;
        state->motion_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
        state->motion_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
        state->motion_predicted_position = position;
        state->motion_speed = 0.0f;
        break;
      }

      /* OnUpdate can run on a render frame where fixed simulation time has not advanced. Keep
       * the last timed sample and its outputs intact; rebasing here would discard displacement
       * and make Valid flicker at render rates above the fixed-step rate. */
      if (elapsed <= 1.0e-9) {
        state->motion_last_sample_tick = context.tick_index;
        break;
      }

      const double raw_x = (double(position.x()) - double(state->motion_previous_position.x())) /
                           elapsed;
      const double raw_y = (double(position.y()) - double(state->motion_previous_position.y())) /
                           elapsed;
      const double raw_z = (double(position.z()) - double(state->motion_previous_position.z())) /
                           elapsed;
      MT_Vector3 velocity{float(raw_x), float(raw_y), float(raw_z)};
      if (!vector_is_finite(velocity)) {
        state->motion_initialized = false;
        state->motion_valid = false;
        state->motion_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
        state->motion_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
        state->motion_speed = 0.0f;
        state->motion_predicted_position = position;
        state->motion_position = position;
        break;
      }

      float smoothing_time = ef.Float(instruction.secondary_float_expr_index,
                                                    instruction.secondary_vector_value.x());
      if (!std::isfinite(smoothing_time) || smoothing_time < 0.0f) {
        smoothing_time = 0.0f;
      }
      if (smoothing_time > 0.0f && state->motion_valid) {
        const double alpha = -std::expm1(-elapsed / double(smoothing_time));
        velocity = state->motion_velocity +
                   (velocity - state->motion_velocity) * float(std::clamp(alpha, 0.0, 1.0));
      }

      const double speed_double = std::sqrt(double(velocity.x()) * double(velocity.x()) +
                                            double(velocity.y()) * double(velocity.y()) +
                                            double(velocity.z()) * double(velocity.z()));
      if (!std::isfinite(speed_double) ||
          speed_double > double(std::numeric_limits<float>::max()))
      {
        state->motion_initialized = false;
        state->motion_valid = false;
        state->motion_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
        state->motion_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
        state->motion_speed = 0.0f;
        state->motion_predicted_position = position;
        state->motion_position = position;
        break;
      }

      float prediction_time = ef.Float(
          instruction.float_expr_index, instruction.vector_value.x());
      if (!std::isfinite(prediction_time) || prediction_time < 0.0f) {
        prediction_time = 0.0f;
      }
      const double predicted_x = double(position.x()) +
                                 double(velocity.x()) * double(prediction_time);
      const double predicted_y = double(position.y()) +
                                 double(velocity.y()) * double(prediction_time);
      const double predicted_z = double(position.z()) +
                                 double(velocity.z()) * double(prediction_time);
      const double max_float = double(std::numeric_limits<float>::max());
      const bool predicted_valid = std::isfinite(predicted_x) && std::isfinite(predicted_y) &&
                                   std::isfinite(predicted_z) &&
                                   std::fabs(predicted_x) <= max_float &&
                                   std::fabs(predicted_y) <= max_float &&
                                   std::fabs(predicted_z) <= max_float;

      state->motion_source_mode = int32_t(source_mode);
      state->motion_source_ref = source_ref;
      state->motion_last_sample_tick = context.tick_index;
      state->motion_last_sample_time = context.current_time;
      state->motion_position = position;
      state->motion_previous_position = position;
      state->motion_velocity = velocity;
      state->motion_speed = float(speed_double);
      state->motion_direction = speed_double > double(MT_EPSILON) ?
                                    velocity / state->motion_speed :
                                    MT_Vector3(0.0f, 0.0f, 0.0f);
      state->motion_predicted_position = predicted_valid ? MT_Vector3(float(predicted_x),
                                                                      float(predicted_y),
                                                                      float(predicted_z)) :
                                                           position;
      state->motion_valid = predicted_valid;
      break;
    }
    case LN_OpCode::ResetMotion: {
      if (instruction.int_value < 0) {
        return;
      }
      if (FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value)))
      {
        *state = FlowControlRuntimeState{};
      }
      break;
    }
    case LN_OpCode::BreakLoop: {
      if (instruction.int_value < 0 || instruction.loop_frame_index == LN_INVALID_INDEX ||
          uint32_t(instruction.int_value) != instruction.loop_frame_index ||
          m_activeLoopBodyFrame != instruction.loop_frame_index)
      {
        return;
      }
      LoopState(instruction.loop_frame_index).break_requested = true;
      break;
    }
    default:
      break;
  }

  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpGlobalPropertyInstruction(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;

  if (instruction.opcode == LN_OpCode::SetRandomSeed) {
    /* Set Random Seed: the game's seed changes after this tick, so every tree of the tick sees
     * one seed (the last request of the tick wins). int_value 1: a new seed from the clock. */
    const uint32_t seed = instruction.int_value == 1 ?
                              LN_NewRandomGameSeed() :
                              uint32_t(ef.Int(instruction.int_expr_index, 0));
    LN_RequestRandomGameSeed(seed);
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }

  const std::string category = ef.String(
      instruction.string_expr_index, std::string());
  const std::string property = ef.String(
      instruction.secondary_string_expr_index, std::string());
  if (category.empty() || property.empty()) {
    return;
  }
  LN_Value value = ef.Value(
      instruction.value_expr_index, MakeNoneValue());
  ShareValue(value);
  const bool persistent = ef.Bool(instruction.bool_expr_index, false);
  {
    std::lock_guard<std::mutex> lock(GlobalPropertiesMutex());
    if (persistent) {
      EnsureGlobalCategoryLoadedUnlocked(category);
    }
    GlobalProperties()[category][property] = value;
    if (persistent) {
      PersistGlobalCategoryUnlocked(category);
    }
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpVariableInstruction(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;

  switch (instruction.opcode) {
    case LN_OpCode::SaveVariable: {
      const std::string path = ef.String(
          instruction.string_expr_index, "//Data");
      const std::string file_name = ef.String(
          instruction.secondary_string_expr_index, "variables");
      const std::string name = ef.String(
          instruction.tertiary_string_expr_index, "var");
      if (name.empty()) {
        return;
      }
      LN_Value dict = LoadVariableDictFromFile(path, file_name);
      dict.type = LN_ValueType::Dict;
      dict.exists = true;
      dict.dict_value[name] = ef.Value(
          instruction.value_expr_index, MakeNoneValue());
      SaveVariableDictToFile(path, file_name, dict);
      break;
    }
    case LN_OpCode::SaveVariableDict: {
      const std::string path = ef.String(
          instruction.string_expr_index, "//Data");
      const std::string file_name = ef.String(
          instruction.secondary_string_expr_index, "variables");
      const LN_Value dict = ef.Value(
          instruction.value_expr_index, MakeNoneValue());
      if (!dict.exists || dict.type != LN_ValueType::Dict) {
        /* Nothing is written (2026-11-08: anything that is no dictionary, a missing property or
         * an unlinked input, wrote an empty one over the whole file). Clear Variables empties a
         * file; an empty dictionary still does. */
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
          CM_Warning("Save Variable Dict: the Variables value is no dictionary, so the file '"
                     << file_name << "' was left as it was");
        }
        break;
      }
      SaveVariableDictToFile(path, file_name, dict);
      break;
    }
    case LN_OpCode::ClearVariables: {
      const std::string path = ef.String(
          instruction.string_expr_index, "//Data");
      const std::string file_name = ef.String(
          instruction.secondary_string_expr_index, "variables");
      LN_Value dict;
      dict.type = LN_ValueType::Dict;
      dict.exists = true;
      SaveVariableDictToFile(path, file_name, dict);
      break;
    }
    case LN_OpCode::RemoveVariable: {
      const std::string path = ef.String(
          instruction.string_expr_index, "//Data");
      const std::string file_name = ef.String(
          instruction.secondary_string_expr_index, "variables");
      const std::string name = ef.String(
          instruction.tertiary_string_expr_index, "var");
      if (!name.empty()) {
        LN_Value dict = LoadVariableDictFromFile(path, file_name);
        if (dict.type != LN_ValueType::Dict) {
          dict.type = LN_ValueType::Dict;
          dict.exists = true;
          dict.dict_value.clear();
        }
        dict.dict_value.erase(name);
        SaveVariableDictToFile(path, file_name, dict);
      }
      break;
    }
    case LN_OpCode::InputMap: {
      LN_InputMap &map = LN_InputMap::Get();
      const LN_InputMapOperation operation = LN_InputMapOperation(instruction.int_value);
      if (operation == LN_InputMapOperation::Rebind) {
        const std::string action = ef.String(
            instruction.string_expr_index, std::string());
        if (m_logicManager == nullptr) {
          CompletePersistenceOperation(instruction.bool_expr_index, false);
          break;
        }
        /* secondary_int_value: the device class, and the direction in its second byte. */
        m_logicManager->StartInputRebind(
            this,
            instruction.bool_expr_index,
            instruction.source_ref_index,
            map.Find(action),
            ef.Int(instruction.int_expr_index, 0),
            LN_InputDeviceClass(instruction.secondary_int_value & 0xff),
            instruction.secondary_int_value >> 8,
            ef.Float(instruction.float_expr_index, 5.0f));
        break;
      }
      bool success = true;
      if (operation == LN_InputMapOperation::Reset) {
        map.Reset();
      }
      else {
        const std::string filepath = ResolveVariableFilePath(
            ef.String(instruction.string_expr_index, "//Data"),
            ef.String(
                instruction.secondary_string_expr_index, "input_map"));
        if (operation == LN_InputMapOperation::Save) {
          /* Write a temporary file and replace the old one only when all of it was written. */
          std::error_code error;
          std::filesystem::create_directories(std::filesystem::path(filepath).parent_path(),
                                              error);
          const std::string temporary = filepath + ".tmp";
          {
            std::ofstream stream(temporary, std::ios::trunc);
            stream << map.Save();
            stream.close();
            success = !stream.fail();
          }
          if (success) {
            std::filesystem::rename(temporary, filepath, error);
            success = !error;
          }
          if (!success) {
            std::filesystem::remove(temporary, error);
          }
        }
        else {
          std::ifstream stream(filepath);
          std::stringstream contents;
          contents << stream.rdbuf();
          success = stream.is_open() && map.Restore(contents.str());
        }
      }
      CompletePersistenceOperation(instruction.bool_expr_index, success);
      break;
    }
    default:
      return;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpPhysicsConstraintInstruction(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;

  switch (instruction.opcode) {
    case LN_OpCode::AddPhysicsConstraint: {
      const LN_RigidBodyConstraintCommandPayload payload = ef.RigidBodyConstraintCommandPayload(
          instruction);
      KX_GameObject *target = ef.ResolveRequiredObject(payload.object_value_expr_index);
      KX_GameObject *child = ef.ResolveRequiredObject(payload.target_value_expr_index);
      if (target == nullptr && m_gameObject != nullptr &&
          m_gameObject->GetPhysicsController() != nullptr)
      {
        target = m_gameObject;
      }
      KX_GameObject *constraint_object = nullptr;
      if (payload.constraint_object_value_expr_index != LN_INVALID_INDEX) {
        constraint_object = ef.ResolveRequiredObject(payload.constraint_object_value_expr_index);
      }
      else if (m_gameObject != nullptr && m_gameObject != target && m_gameObject != child) {
        constraint_object = m_gameObject;
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
      const bool use_jolt = scene && scene->GetBlenderScene() &&
                            static_cast<e_PhysicsEngine>(
                                scene->GetBlenderScene()->gm.physicsEngine) == UseJolt;
      if (target != nullptr && child != nullptr && target != child && environment != nullptr &&
          use_jolt)
      {
        PHY_IPhysicsController *ctrl1 = target->GetPhysicsController();
        PHY_IPhysicsController *ctrl2 = child->GetPhysicsController();
        if (ctrl1 != nullptr && ctrl2 != nullptr) {
          auto bool_input = [&](const LN_RigidBodyConstraintBoolInput input,
                                const bool fallback) {
            return ef.Bool(
                payload.bool_expr_indices[size_t(input)], fallback);
          };
          auto vector_input = [&](const LN_RigidBodyConstraintVectorInput input,
                                  const MT_Vector3 &fallback) {
            return ef.Vector(
                payload.vector_expr_indices[size_t(input)], fallback);
          };
          auto float_input = [&](const LN_RigidBodyConstraintFloatInput input,
                                 const float fallback) {
            return ef.Float(
                payload.float_expr_indices[size_t(input)], fallback);
          };

          MT_Vector3 pivot;
          MT_Matrix3x3 basis;
          if (constraint_object != nullptr) {
            const MT_Matrix3x3 target_inv = target->NodeGetWorldOrientation().inverse();
            pivot = target_inv *
                    (constraint_object->NodeGetWorldPosition() - target->NodeGetWorldPosition());
            basis = target_inv * constraint_object->NodeGetWorldOrientation();
          }
          else {
            pivot = vector_input(LN_RigidBodyConstraintVectorInput::Pivot,
                                 MT_Vector3(0.0f, 0.0f, 0.0f));
            basis = ef.Rotation(payload.vector_expr_indices[size_t(
                                                  LN_RigidBodyConstraintVectorInput::Rotation)],
                                              LN_Rotation::Identity())
                        .ToMatrix();
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseWorldSpace, false)) {
              const MT_Matrix3x3 target_inv = target->NodeGetWorldOrientation().inverse();
              pivot = target_inv * (pivot - target->NodeGetWorldPosition());
              basis = target_inv * basis;
            }
          }

          PHY_RigidBodyConstraintSettings settings;
          settings.type = payload.type;
          settings.spring_type = payload.spring_type;
          settings.flags = 0;
          if (bool_input(LN_RigidBodyConstraintBoolInput::Enabled, true)) {
            settings.flags |= PHY_RB_CONSTRAINT_ENABLED;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::DisableCollisions, true)) {
            settings.flags |= PHY_RB_CONSTRAINT_DISABLE_COLLISIONS;
          }
          if (settings.type != PHY_RigidBodyConstraintType::Motor &&
              bool_input(LN_RigidBodyConstraintBoolInput::Breakable, false))
          {
            settings.flags |= PHY_RB_CONSTRAINT_USE_BREAKING;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::OverrideIterations, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_JOLT_OVERRIDE_SOLVER_ITERATIONS;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitLinX, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_LIN_X;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitLinY, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_LIN_Y;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitLinZ, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_LIN_Z;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitAngX, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_ANG_X;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitAngY, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Y;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitAngZ, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Z;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringX, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_X;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringY, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_Y;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringZ, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_Z;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringAngX, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_ANG_X;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringAngY, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_ANG_Y;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringAngZ, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_ANG_Z;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseMotorLin, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_MOTOR_LIN;
          }
          if (bool_input(LN_RigidBodyConstraintBoolInput::UseMotorAng, false)) {
            settings.flags |= PHY_RB_CONSTRAINT_USE_MOTOR_ANG;
          }

          settings.jolt_velocity_solver_iterations = std::clamp(
              ef.Int(payload.velocity_solver_iterations_int_expr_index,
                                   settings.jolt_velocity_solver_iterations),
              1,
              255);
          settings.jolt_position_solver_iterations = std::clamp(
              ef.Int(payload.position_solver_iterations_int_expr_index,
                                   settings.jolt_position_solver_iterations),
              1,
              255);
          settings.breaking_threshold = float_input(
              LN_RigidBodyConstraintFloatInput::BreakingThreshold, settings.breaking_threshold);
          const MT_Vector3 linear_lower = vector_input(
              LN_RigidBodyConstraintVectorInput::LinearLower,
              MT_Vector3(settings.limit_lin_x_lower,
                         settings.limit_lin_y_lower,
                         settings.limit_lin_z_lower));
          const MT_Vector3 linear_upper = vector_input(
              LN_RigidBodyConstraintVectorInput::LinearUpper,
              MT_Vector3(settings.limit_lin_x_upper,
                         settings.limit_lin_y_upper,
                         settings.limit_lin_z_upper));
          const MT_Vector3 angular_lower = vector_input(
              LN_RigidBodyConstraintVectorInput::AngularLower,
              MT_Vector3(settings.limit_ang_x_lower,
                         settings.limit_ang_y_lower,
                         settings.limit_ang_z_lower));
          const MT_Vector3 angular_upper = vector_input(
              LN_RigidBodyConstraintVectorInput::AngularUpper,
              MT_Vector3(settings.limit_ang_x_upper,
                         settings.limit_ang_y_upper,
                         settings.limit_ang_z_upper));
          const MT_Vector3 spring_stiffness = vector_input(
              LN_RigidBodyConstraintVectorInput::SpringStiffness,
              MT_Vector3(settings.spring_stiffness_x,
                         settings.spring_stiffness_y,
                         settings.spring_stiffness_z));
          const MT_Vector3 spring_damping = vector_input(
              LN_RigidBodyConstraintVectorInput::SpringDamping,
              MT_Vector3(settings.spring_damping_x,
                         settings.spring_damping_y,
                         settings.spring_damping_z));
          const MT_Vector3 angular_spring_stiffness = vector_input(
              LN_RigidBodyConstraintVectorInput::AngularSpringStiffness,
              MT_Vector3(settings.spring_stiffness_ang_x,
                         settings.spring_stiffness_ang_y,
                         settings.spring_stiffness_ang_z));
          const MT_Vector3 angular_spring_damping = vector_input(
              LN_RigidBodyConstraintVectorInput::AngularSpringDamping,
              MT_Vector3(settings.spring_damping_ang_x,
                         settings.spring_damping_ang_y,
                         settings.spring_damping_ang_z));

          settings.limit_lin_x_lower = linear_lower.x();
          settings.limit_lin_y_lower = linear_lower.y();
          settings.limit_lin_z_lower = linear_lower.z();
          settings.limit_lin_x_upper = linear_upper.x();
          settings.limit_lin_y_upper = linear_upper.y();
          settings.limit_lin_z_upper = linear_upper.z();
          settings.limit_ang_x_lower = angular_lower.x();
          settings.limit_ang_y_lower = angular_lower.y();
          settings.limit_ang_z_lower = angular_lower.z();
          settings.limit_ang_x_upper = angular_upper.x();
          settings.limit_ang_y_upper = angular_upper.y();
          settings.limit_ang_z_upper = angular_upper.z();
          settings.spring_stiffness_x = spring_stiffness.x();
          settings.spring_stiffness_y = spring_stiffness.y();
          settings.spring_stiffness_z = spring_stiffness.z();
          settings.spring_damping_x = spring_damping.x();
          settings.spring_damping_y = spring_damping.y();
          settings.spring_damping_z = spring_damping.z();
          settings.spring_stiffness_ang_x = angular_spring_stiffness.x();
          settings.spring_stiffness_ang_y = angular_spring_stiffness.y();
          settings.spring_stiffness_ang_z = angular_spring_stiffness.z();
          settings.spring_damping_ang_x = angular_spring_damping.x();
          settings.spring_damping_ang_y = angular_spring_damping.y();
          settings.spring_damping_ang_z = angular_spring_damping.z();
          settings.motor_lin_target_velocity = float_input(
              LN_RigidBodyConstraintFloatInput::MotorLinTargetVelocity,
              settings.motor_lin_target_velocity);
          settings.motor_lin_max_impulse = float_input(
              LN_RigidBodyConstraintFloatInput::MotorLinMaxImpulse,
              settings.motor_lin_max_impulse);
          settings.motor_ang_target_velocity = float_input(
              LN_RigidBodyConstraintFloatInput::MotorAngTargetVelocity,
              settings.motor_ang_target_velocity);
          settings.motor_ang_max_impulse = float_input(
              LN_RigidBodyConstraintFloatInput::MotorAngMaxImpulse,
              settings.motor_ang_max_impulse);

          std::string constraint_name = ef.String(
              payload.name_string_expr_index, "constraint");
          if (constraint_name.empty()) {
            constraint_name = "constraint";
          }

          const int constraint_id = environment->CreateRigidBodyConstraint(
              target, child, pivot, basis, settings, constraint_name);
          if (constraint_id != -1) {
            target->AddRuntimeRigidBodyConstraint(
                constraint_name, target, child, settings, pivot, basis, constraint_id);
          }
        }
      }
      break;
    }
    case LN_OpCode::RemovePhysicsConstraint: {
      if (KX_GameObject *target = ef.ResolveObjectExpression(instruction.value_expr_index)) {
        if (!UsesJoltPhysics(target)) {
          break;
        }
        if (ef.Bool(instruction.bool_expr_index, false)) {
          target->RemoveRigidBodyConstraints();
          break;
        }
        std::string name = ef.String(
            instruction.string_expr_index, "constraint");
        if (name.empty()) {
          name = "constraint";
        }
        target->RemoveRigidBodyConstraint(name);
      }
      break;
    }
    default:
      return;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpNavigationInstruction(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  switch (instruction.opcode) {
    case LN_OpCode::Navigate: {
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        const MT_Vector3 destination = ef.Vector(
            instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f));
        const float speed = ef.Float(instruction.float_expr_index, 1.0f);
        const float threshold = ef.Float(
            instruction.secondary_float_expr_index, 1.0f);
        const bool move_dynamic = ef.Bool(
            instruction.bool_expr_index, false);
        const bool look_at = ef.Bool(
            instruction.secondary_bool_expr_index, true);
        const bool visualize = ef.Bool(
            instruction.tertiary_bool_expr_index, false);
        const int32_t rot_axis = ef.Int(instruction.int_expr_index, 2);
        const int32_t front_axis = ef.Int(
            instruction.secondary_int_expr_index, 1);
        const float rot_speed = ef.Float(
            instruction.tertiary_float_expr_index, 1.0f);
        KX_NavMeshObject *navmesh = nullptr;
        if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
          const LN_Value navmesh_value = ef.Value(
              instruction.secondary_value_expr_index, MakeNoneValue());
          navmesh = ResolveNavMeshObject(ResolveObjectValue(navmesh_value));
        }
        KX_GameObject *rotating_object = target;
        if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
          if (KX_GameObject *input_object = ResolveObjectValue(ef.Value(
                  instruction.tertiary_value_expr_index, MakeNoneValue())))
          {
            rotating_object = input_object;
          }
        }
        const uint32_t state_index = InstructionStateIndex(event, instruction_id);
        if (state_index < m_instructionStamps.size()) {
          InstructionMotionState &state = MotionState(state_index);
          MT_Vector3 move_target;
          bool reached = false;
          if (ResolvePathMoveTarget(
                  state, target, navmesh, destination, threshold, move_target, reached))
          {
            if (visualize) {
              DrawNavigationPath(target, state, destination);
            }
            command_buffer.AppendMoveToward(target,
                                            move_target,
                                            speed,
                                            threshold,
                                            move_dynamic,
                                            true,
                                            float(context.delta_time),
                                            ef.SortKey(),
                                            instruction.source_ref_index);
            if (look_at && rotating_object != nullptr) {
              command_buffer.AppendRotateToward(
                  rotating_object,
                  move_target,
                  std::max(0.0f, rot_speed) * float(std::max(0.0, context.delta_time)),
                  rot_axis,
                  front_axis,
                  true,
                  ef.SortKey(),
                  instruction.source_ref_index);
            }
          }
          if (reached) {
            state.last_reached_tick = context.tick_index;
            state.last_reached_scope_serial = m_activeExecutionScopeSerial;
          }
        }
      }
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      break;
    }
    case LN_OpCode::FollowPath: {
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        const LN_Value path_value = ef.Value(
            instruction.secondary_value_expr_index, MakeNoneValue());
        std::vector<MT_Vector3> path_points;
        if (!CollectPathPoints(path_value, path_points)) {
          return;
        }
        const bool loop = ef.Bool(instruction.bool_expr_index, false);
        const bool move_dynamic = ef.Bool(
            instruction.tertiary_bool_expr_index, false);
        const bool look_at = ef.Bool(
            instruction.quaternary_bool_expr_index, true);
        const float speed = ef.Float(instruction.float_expr_index, 1.0f);
        const float threshold = ef.Float(
            instruction.secondary_float_expr_index, 0.2f);
        const float rot_speed = ef.Float(
            instruction.tertiary_float_expr_index, 1.0f);
        const int32_t rot_axis = ef.Int(instruction.int_expr_index, 2);
        const int32_t front_axis = ef.Int(
            instruction.secondary_int_expr_index, 1);
        KX_NavMeshObject *navmesh = nullptr;
        if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
          navmesh = ResolveNavMeshObject(ResolveObjectValue(ef.Value(
              instruction.tertiary_value_expr_index, MakeNoneValue())));
        }
        KX_GameObject *rotating_object = target;
        if (instruction.quaternary_value_expr_index != LN_INVALID_INDEX) {
          if (KX_GameObject *input_object = ResolveObjectValue(ef.Value(
                  instruction.quaternary_value_expr_index, MakeNoneValue())))
          {
            rotating_object = input_object;
          }
        }

        const uint32_t state_index = InstructionStateIndex(event, instruction_id);
        if (state_index >= m_instructionStamps.size()) {
          return;
        }
        InstructionMotionState &state = MotionState(state_index);
        if (state.list_point_index < 0 || state.list_point_index >= int(path_points.size())) {
          state.list_point_index = 0;
        }

        MT_Vector3 move_target;
        while (state.list_point_index < int(path_points.size())) {
          const MT_Vector3 &point = path_points[size_t(state.list_point_index)];
          bool reached = false;
          if (ResolvePathMoveTarget(
                  state, target, navmesh, point, threshold, move_target, reached))
          {
            command_buffer.AppendMoveToward(target,
                                            move_target,
                                            speed,
                                            threshold,
                                            move_dynamic,
                                            true,
                                            float(context.delta_time),
                                            ef.SortKey(),
                                            instruction.source_ref_index);
            if (look_at && rotating_object != nullptr) {
              command_buffer.AppendRotateToward(
                  rotating_object,
                  move_target,
                  std::max(0.0f, rot_speed) * float(std::max(0.0, context.delta_time)),
                  rot_axis,
                  front_axis,
                  true,
                  ef.SortKey(),
                  instruction.source_ref_index);
            }
            break;
          }
          if (reached) {
            state.last_reached_tick = context.tick_index;
            state.last_reached_scope_serial = m_activeExecutionScopeSerial;
          }
          state.list_point_index++;
          state.path_len = 0;
          state.waypoint_index = -1;
        }

        if (state.list_point_index >= int(path_points.size()) && loop && !path_points.empty()) {
          state.list_point_index = 0;
          const MT_Vector3 &point = path_points[0];
          bool reached = false;
          if (ResolvePathMoveTarget(
                  state, target, navmesh, point, threshold, move_target, reached))
          {
            command_buffer.AppendMoveToward(target,
                                            move_target,
                                            speed,
                                            threshold,
                                            move_dynamic,
                                            true,
                                            float(context.delta_time),
                                            ef.SortKey(),
                                            instruction.source_ref_index);
            if (look_at && rotating_object != nullptr) {
              command_buffer.AppendRotateToward(
                  rotating_object,
                  move_target,
                  std::max(0.0f, rot_speed) * float(std::max(0.0, context.delta_time)),
                  rot_axis,
                  front_axis,
                  true,
                  ef.SortKey(),
                  instruction.source_ref_index);
            }
          }
          if (reached) {
            state.last_reached_tick = context.tick_index;
            state.last_reached_scope_serial = m_activeExecutionScopeSerial;
          }
        }
      }
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      break;
    }
    default:
      break;
  }
}

template<class Frame>
void LN_RuntimeTree::OpSceneCollectionInstruction(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  bool instruction_executed = false;
  auto warn_scene_collection_request = [&](const LN_CommandBuffer::CommandType command_type,
                                           const std::string &reason) {
    LN_CommandBuffer::Command diagnostic;
    diagnostic.type = command_type;
    diagnostic.object = m_gameObject;
    diagnostic.runtime_tree = this;
    diagnostic.sort_key = ef.SortKey();
    diagnostic.source_ref_index = instruction.source_ref_index;
    LN_WarnCommandFailureOnce(diagnostic, reason);
  };
  switch (instruction.opcode) {
    case LN_OpCode::SetCollectionVisibility: {
      const LN_Value collection_value = ef.Value(
          instruction.value_expr_index, MakeNoneValue());
      blender::Collection *collection = nullptr;
      if (collection_value.type == LN_ValueType::CollectionRef &&
          collection_value.runtime_ref.IsValid())
      {
        collection = ResolveCollectionRef(collection_value.runtime_ref);
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      if (scene == nullptr || collection == nullptr) {
        warn_scene_collection_request(
            LN_CommandBuffer::CommandType::SetVisibility,
            "collection did not resolve in the logic-tree owner's scene");
        break;
      }
      const bool visible = ef.Bool(instruction.bool_expr_index, true);
      const bool include_child_collections = ef.Bool(
          instruction.secondary_bool_expr_index, false);
      if (!scene->SetRuntimeCollectionRenderVisibility(
              collection, visible, include_child_collections))
      {
        warn_scene_collection_request(
            LN_CommandBuffer::CommandType::SetVisibility,
            "collection is not linked to the logic-tree owner's runtime scene");
        break;
      }
      if (m_logicManager != nullptr) {
        m_logicManager->InvalidatePointerHitCache();
      }
      instruction_executed = true;
      break;
    }
    case LN_OpCode::SetCollectionMember: {
      const LN_Value collection_value = ef.Value(
          instruction.value_expr_index, MakeNoneValue());
      blender::Collection *collection = nullptr;
      if (collection_value.type == LN_ValueType::CollectionRef &&
          collection_value.runtime_ref.IsValid())
      {
        collection = ResolveCollectionRef(collection_value.runtime_ref);
      }
      /* Object: empty is the tree's own object. */
      KX_GameObject *target = instruction.secondary_value_expr_index != LN_INVALID_INDEX ?
                                  ResolveObjectValue(ef.Value(
                                      instruction.secondary_value_expr_index,
                                      MakeNoneValue())) :
                                  m_gameObject;
      KX_Scene *scene = target ? target->GetScene() : nullptr;
      if (scene == nullptr || collection == nullptr) {
        warn_scene_collection_request(
            LN_CommandBuffer::CommandType::SetVisibility,
            "Set Collection Member: the object or the collection did not resolve");
        break;
      }
      const bool member = ef.Bool(instruction.bool_expr_index, true);
      const bool include_children = ef.Bool(
          instruction.secondary_bool_expr_index, false);
      std::vector<KX_GameObject *> objects = {target};
      if (include_children) {
        const std::vector<KX_GameObject *> children = target->GetChildrenRecursive();
        objects.insert(objects.end(), children.begin(), children.end());
      }
      bool changed = true;
      for (KX_GameObject *object : objects) {
        changed &= scene->SetCollectionMember(object, collection, member);
      }
      if (!changed) {
        warn_scene_collection_request(
            LN_CommandBuffer::CommandType::SetVisibility,
            "Set Collection Member: the collection is not part of the object's scene");
        break;
      }
      if (m_logicManager != nullptr) {
        m_logicManager->InvalidatePointerHitCache();
      }
      instruction_executed = true;
      break;
    }
    case LN_OpCode::SetOverlayCollection: {
      const LN_Value camera_value = instruction.value_expr_index != LN_INVALID_INDEX ?
                                        ef.Value(instruction.value_expr_index,
                                                               MakeNoneValue()) :
                                        MakeNoneValue();
      const LN_Value collection_value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      KX_Camera *camera = dynamic_cast<KX_Camera *>(ResolveObjectValue(camera_value, true));
      blender::Collection *collection = nullptr;
      if (collection_value.type == LN_ValueType::CollectionRef &&
          collection_value.runtime_ref.IsValid())
      {
        collection = ResolveCollectionRef(collection_value.runtime_ref);
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      auto complete_overlay = [&](const LN_UIResult result) {
        CompleteUIOperation(instruction.property_ref_index, result, 0);
        instruction_executed = true;
      };
      if (m_gameObject == nullptr || scene == nullptr) {
        complete_overlay(LN_UIResult::RuntimeUnavailable);
        warn_scene_collection_request(LN_CommandBuffer::CommandType::SetOverlayCollection,
                                      LN_UIResultMessage(LN_UIResult::RuntimeUnavailable));
        break;
      }
      if (collection == nullptr) {
        complete_overlay(LN_UIResult::InvalidCollection);
        warn_scene_collection_request(LN_CommandBuffer::CommandType::SetOverlayCollection,
                                      LN_UIResultMessage(LN_UIResult::InvalidCollection));
        break;
      }
      const KX_OverlayCollectionResult validation = scene->ValidateAddOverlayCollection(
          camera, collection);
      if (validation == KX_OverlayCollectionResult::NoChange) {
        complete_overlay(LN_UIResult::NoChange);
        break;
      }
      if (!KX_OverlayCollectionResultSucceeded(validation)) {
        complete_overlay(LN_MapOverlayResult(validation));
        warn_scene_collection_request(LN_CommandBuffer::CommandType::SetOverlayCollection,
                                      KX_OverlayCollectionResultMessage(validation));
        break;
      }
      command_buffer.AppendSetOverlayCollection(
          this,
          m_gameObject,
          camera,
          collection,
          instruction.property_ref_index,
          ef.SortKey(),
          instruction.source_ref_index);
      instruction_executed = true;
      break;
    }
    case LN_OpCode::RemoveOverlayCollection: {
      const LN_Value collection_value = ef.Value(
          instruction.value_expr_index, MakeNoneValue());
      blender::Collection *collection = nullptr;
      if (collection_value.type == LN_ValueType::CollectionRef &&
          collection_value.runtime_ref.IsValid())
      {
        collection = ResolveCollectionRef(collection_value.runtime_ref);
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      auto complete_overlay = [&](const LN_UIResult result) {
        CompleteUIOperation(instruction.property_ref_index, result, 0);
        instruction_executed = true;
      };
      if (m_gameObject == nullptr || scene == nullptr) {
        complete_overlay(LN_UIResult::RuntimeUnavailable);
        warn_scene_collection_request(LN_CommandBuffer::CommandType::RemoveOverlayCollection,
                                      LN_UIResultMessage(LN_UIResult::RuntimeUnavailable));
        break;
      }
      if (collection == nullptr) {
        complete_overlay(LN_UIResult::InvalidCollection);
        warn_scene_collection_request(LN_CommandBuffer::CommandType::RemoveOverlayCollection,
                                      LN_UIResultMessage(LN_UIResult::InvalidCollection));
        break;
      }
      const KX_OverlayCollectionResult validation = scene->ValidateRemoveOverlayCollection(
          collection);
      if (validation == KX_OverlayCollectionResult::NoChange) {
        complete_overlay(LN_UIResult::NoChange);
        break;
      }
      if (!KX_OverlayCollectionResultSucceeded(validation)) {
        complete_overlay(LN_MapOverlayResult(validation));
        warn_scene_collection_request(LN_CommandBuffer::CommandType::RemoveOverlayCollection,
                                      KX_OverlayCollectionResultMessage(validation));
        break;
      }
      command_buffer.AppendRemoveOverlayCollection(
          this,
          m_gameObject,
          collection,
          instruction.property_ref_index,
          ef.SortKey(),
          instruction.source_ref_index);
      instruction_executed = true;
      break;
    }
    case LN_OpCode::OpenObjectMenu: {
      KX_Camera *camera = instruction.value_expr_index != LN_INVALID_INDEX ?
                              dynamic_cast<KX_Camera *>(ResolveObjectValue(
                                  ef.Value(
                                      instruction.value_expr_index, MakeNoneValue()),
                                  true)) :
                              nullptr;
      const LN_Value collection_value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      blender::Collection *collection = collection_value.type == LN_ValueType::CollectionRef &&
                                                collection_value.runtime_ref.IsValid() ?
                                            ResolveCollectionRef(collection_value.runtime_ref) :
                                            nullptr;
      if (m_gameObject == nullptr || collection == nullptr) {
        CompleteUIOperation(instruction.property_ref_index,
                            m_gameObject == nullptr ? LN_UIResult::InvalidOwner :
                                                      LN_UIResult::InvalidCollection,
                            0);
        instruction_executed = true;
        break;
      }
      /* Owner: whose menu it is (empty: the opener). */
      KX_GameObject *menu_owner = instruction.tertiary_value_expr_index != LN_INVALID_INDEX ?
                                      ResolveObjectValue(ef.Value(
                                          instruction.tertiary_value_expr_index,
                                          MakeNoneValue())) :
                                      nullptr;
      /* From inside a menu: for that menu's player (each player's copy runs this tree). */
      const int32_t menu_user =
          m_logicManager != nullptr ?
              m_logicManager->GetUIService().MenuUserOfObject(m_gameObject) :
              -1;
      command_buffer.AppendOpenObjectMenu(
          this,
          m_gameObject,
          camera,
          menu_owner,
          collection,
          static_cast<LN_UIMenuRouting>(std::clamp(instruction.int_value,
                                                   int32_t(LN_UIMenuRouting::Modal),
                                                   int32_t(LN_UIMenuRouting::PassThrough))),
          ef.String(instruction.string_expr_index, std::string()),
          LN_PackUIMenuUser(
              instruction.secondary_int_value,
              menu_user >= 0 ?
                  menu_user :
                  std::clamp(ef.Int(instruction.int_expr_index, 0),
                             0,
                             LN_MAX_GAMEPADS - 1),
              24),
          instruction.property_ref_index,
          ef.SortKey(),
          instruction.source_ref_index);
      instruction_executed = true;
      break;
    }
    case LN_OpCode::CloseObjectMenu: {
      if (m_gameObject == nullptr) {
        CompleteUIOperation(instruction.property_ref_index, LN_UIResult::InvalidOwner, 0);
        instruction_executed = true;
        break;
      }
      /* From inside a menu: that menu's player's menus. */
      const int32_t menu_user =
          m_logicManager != nullptr ?
              m_logicManager->GetUIService().MenuUserOfObject(m_gameObject) :
              -1;
      command_buffer.AppendCloseObjectMenu(
          this,
          m_gameObject,
          ef.Int(instruction.int_expr_index, 0),
          LN_UIMenuCloseOperation(std::clamp(instruction.int_value,
                                             int32_t(LN_UIMenuCloseOperation::Top),
                                             int32_t(LN_UIMenuCloseOperation::All))),
          menu_user >= 0 ?
              menu_user :
              std::clamp(ef.Int(instruction.secondary_int_expr_index, 0),
                         0,
                         LN_MAX_GAMEPADS - 1),
          instruction.property_ref_index,
          ef.SortKey(),
          instruction.source_ref_index);
      instruction_executed = true;
      break;
    }
    case LN_OpCode::InventoryEdit: {
      /* Read the inventory property, change it and write it back at once: a second inventory
       * change later in this tick works on this one's result. */
      instruction_executed = true;
      KX_GameObject *target = instruction.value_expr_index != LN_INVALID_INDEX ?
                                  ResolveLiveObjectValue(ef.Value(
                                      instruction.value_expr_index, MakeNoneValue())) :
                                  m_gameObject;
      const std::string property = ef.String(
          instruction.string_expr_index, std::string());
      const std::vector<LN_ValueExpression> &expressions = m_program->GetValueExpressions();
      if (instruction.secondary_value_expr_index >= expressions.size()) {
        break;
      }
      const LN_ValueExpression &edit = expressions[instruction.secondary_value_expr_index];
      /* A missing property starts empty; one of another kind is not an inventory. */
      std::string problem;
      const auto read_inventory = [&](KX_GameObject *object, const std::string &name) {
        LN_Value list;
        list.type = LN_ValueType::List;
        list.exists = true;
        if (object == nullptr || name.empty()) {
          problem = "Inventory needs an object and a property name";
        }
        else if (EXP_Value *current = object->GetProperty(name)) {
          LN_Value value = MakeValueFromEXPValue(current, this);
          if (value.exists && value.type == LN_ValueType::List) {
            list = std::move(value);
          }
          else {
            problem = "game property '" + name + "' is not a List or Table";
          }
        }
        return list;
      };
      LN_Value inventory = read_inventory(target, property);
      /* Move To Slot into another inventory (To Property; To Object empty: the same object). */
      KX_GameObject *other_target = nullptr;
      std::string other_property;
      if (LN_InventoryOperation(edit.value.int_value) == LN_InventoryOperation::MoveToSlot &&
          edit.input_indices.size() > LN_INVENTORY_INPUT_TO_PROPERTY)
      {
        const LN_Value name = ef.Value(
            edit.input_indices[LN_INVENTORY_INPUT_TO_PROPERTY], MakeNoneValue());
        other_property = name.exists && name.type == LN_ValueType::String ? name.string_value :
                                                                            std::string();
        const LN_Value object = ef.Value(
            edit.input_indices[LN_INVENTORY_INPUT_TO_OBJECT], MakeNoneValue());
        other_target = object.exists && object.type == LN_ValueType::ObjectRef ?
                           ResolveLiveObjectValue(object) :
                           target;
        if (other_target == target && other_property == property) {
          other_property.clear();
        }
      }
      LN_Value other_inventory;
      if (!other_property.empty()) {
        other_inventory = read_inventory(other_target, other_property);
      }
      const std::vector<LN_Value> before = inventory.list_value;
      const std::vector<LN_Value> other_before = other_inventory.list_value;
      /* Each list follows its own Inventory Rules (unless the node overrides them). */
      const LN_InventoryRules list_rules = m_logicManager != nullptr && problem.empty() ?
                                               m_logicManager->InventoryRulesOf(target,
                                                                                property) :
                                               LN_InventoryRules();
      const LN_InventoryRules other_list_rules =
          m_logicManager != nullptr && !other_property.empty() ?
              m_logicManager->InventoryRulesOf(other_target, other_property) :
              LN_InventoryRules();
      InventoryEditResult result = RunInventoryEdit(ef, edit, inventory.list_value, other_property.empty() ? nullptr : &other_inventory.list_value, &list_rules, &other_list_rules, target);
      std::string reason;
      /* The receiving inventory first: if the source then cannot be written, it is put back. */
      if (problem.empty() && !other_property.empty() &&
          !LN_CommandBuffer::ApplyGamePropertyValue(
              other_target, other_property, other_inventory, this, reason))
      {
        problem = reason;
      }
      if (problem.empty() &&
          !LN_CommandBuffer::ApplyGamePropertyValue(target, property, inventory, this, reason))
      {
        problem = reason;
        if (!other_property.empty()) {
          other_inventory.list_value = other_before;
          LN_CommandBuffer::ApplyGamePropertyValue(
              other_target, other_property, other_inventory, this, reason);
        }
      }
      if (!problem.empty()) {
        /* Nothing was kept: the outputs say nothing changed (Add Item: all left over). */
        warn_scene_collection_request(LN_CommandBuffer::CommandType::SetGameProperty, problem);
        inventory.list_value = before;
        result.amount = result.requested;
        result.made.clear();
        result.outcome = LN_InventoryOutcome::None;
      }
      if (instruction.property_ref_index != LN_INVALID_INDEX) {
        LN_Value stored;
        stored.type = LN_ValueType::Dict;
        stored.exists = true;
        stored.dict_value["amount"] = MakeIntValue(result.amount);
        stored.dict_value["changed"] = MakeIntValue(
            result.Changed(LN_InventoryOperation(edit.value.int_value)));
        stored.dict_value["made"] = MakeStringValue(result.made);
        stored.dict_value["outcome"] = MakeStringValue(
            LN_InventoryOutcomeName(result.outcome, result.craft));
        stored.dict_value["inventory"] = std::move(inventory);
        SetTreePropertyValue(instruction.property_ref_index, stored);
      }
      break;
    }
    case LN_OpCode::DropItem: {
      /* Done runs only when the copy is made or queued: a chain like Drop Item → Remove From
       * Slot must not lose the item when nothing lands in the world. */
      KX_Scene *scene = m_gameObject != nullptr ? m_gameObject->GetScene() : nullptr;
      std::string item;
      int32_t carried = 1;
      if (scene == nullptr || m_logicManager == nullptr ||
          !ItemOf(ef.Value(
                      instruction.secondary_value_expr_index, MakeNoneValue()),
                  item,
                  carried))
      {
        break;
      }
      /* Amount 0 drops all it carries: a whole slot's count, a pickup's count. */
      const int32_t amount = ef.Int(instruction.int_expr_index, 0);
      if (amount < 0) {
        break;
      }
      const blender::Collection *library =
          instruction.value_expr_index != LN_INVALID_INDEX ?
              ResolveCollectionByValue(
                  this,
                  ef.Value(instruction.value_expr_index, MakeNoneValue())) :
              nullptr;
      if (library == nullptr) {
        library = SceneItemLibrary();
      }
      KX_GameObject *source = LN_FindLibraryItem(*scene, library, item);
      if (source == nullptr) {
        warn_scene_collection_request(LN_CommandBuffer::CommandType::AddObjectFromRef,
                                      "'" + item +
                                          "' has no object in the Item Library (set it on the "
                                          "node or in Scene > Items and UI)");
        break;
      }
      std::string reason;
      if (!m_logicManager->PreflightAddObjectPersistence(source, 0.0f, false, reason)) {
        warn_scene_collection_request(LN_CommandBuffer::CommandType::AddObjectFromRef,
                                      "'" + item + "' cannot be copied into the world (" +
                                          reason + "); keep the Item Library hidden");
        break;
      }
      const MT_Vector3 position = ef.Vector(
          instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f));
      const int32_t count = amount > 0 ? amount : carried;
      /* Objects join the world on Logic + Physics steps: a UI route's drop waits for the next
       * one, like Send Event does. */
      if (event == LN_Event::OnUpdate) {
        m_logicManager->QueueItemDrop(source, position, item, count);
      }
      else if (m_logicManager->SpawnItemDrop(source, position, item, count) == nullptr) {
        break;
      }
      instruction_executed = true;
      break;
    }
    case LN_OpCode::ScrollObjectView: {
      /* An empty Viewport Object is the tree's own object, like the view nodes' default. The
       * view scrolls when it next lays out. */
      KX_GameObject *viewport = instruction.value_expr_index != LN_INVALID_INDEX ?
                                    ResolveObjectValue(ef.Value(
                                        instruction.value_expr_index, MakeNoneValue())) :
                                    m_gameObject;
      if (viewport == nullptr || m_logicManager == nullptr) {
        break;
      }
      const LN_UIViewScrollMode mode = LN_UIViewScrollMode(
          std::clamp(instruction.int_value,
                     int32_t(LN_UIViewScrollMode::ShowItem),
                     int32_t(LN_UIViewScrollMode::Lines)));
      /* Show Item: a number is an index; an item name, slot or object is looked for. */
      LN_UIService::ScrollTarget target;
      if (mode == LN_UIViewScrollMode::ShowItem) {
        const LN_Value item = ef.Value(
            instruction.secondary_value_expr_index, MakeNoneValue());
        if (item.exists && (item.type == LN_ValueType::Int || item.type == LN_ValueType::Float)) {
          target.has_index = true;
          target.index = ValueAsInt(item, LN_FloatToIntMode::Round);
        }
        else if (item.exists && item.type == LN_ValueType::ObjectRef) {
          /* A grid slot (a drop's Target): the item it shows; another object: by its item. */
          KX_GameObject *object = ResolveLiveObjectValue(item, true);
          LN_Value bound;
          int32_t bound_index = -1;
          std::string bound_name;
          int32_t bound_count = 0;
          if (object != nullptr &&
              m_logicManager->GetUIService().GetBoundItem(object, bound, bound_index) &&
              bound.exists)
          {
            if (bound.type == LN_ValueType::ObjectRef) {
              target.item = bound.reference_name;
            }
            else if (LN_InventoryShownEntry(bound, bound_name, bound_count)) {
              target.item = bound_name;
            }
          }
          else {
            target.object = object;
            target.item = ItemNameOf(item);
          }
        }
        else if (item.exists && item.type == LN_ValueType::String) {
          target.item = item.string_value;
        }
        else {
          target.item = ItemNameOf(item);
        }
        if (!target.has_index && target.item.empty() && target.object == nullptr) {
          break;
        }
      }
      m_logicManager->GetUIService().RequestViewScroll(
          viewport, mode, ef.Int(instruction.int_expr_index, 1), target);
      instruction_executed = true;
      break;
    }
    case LN_OpCode::ShowMessage: {
      KX_GameObject *object = instruction.value_expr_index != LN_INVALID_INDEX ?
                                  ResolveObjectValue(ef.Value(
                                      instruction.value_expr_index, MakeNoneValue())) :
                                  m_gameObject;
      if (object == nullptr || m_logicManager == nullptr) {
        break;
      }
      instruction_executed = m_logicManager->GetUIService().ShowMessage(
          object,
          ef.Int(instruction.secondary_int_expr_index, -1),
          ef.String(instruction.string_expr_index, std::string()),
          ef.Float(instruction.float_expr_index, 3.0f),
          ef.Int(instruction.int_expr_index, 4),
          ef.Int(instruction.tertiary_int_expr_index, -1));
      break;
    }
    case LN_OpCode::SetUIFocus: {
      if (m_gameObject == nullptr) {
        CompleteUIOperation(instruction.property_ref_index, LN_UIResult::InvalidOwner, 0);
        instruction_executed = true;
        break;
      }
      KX_GameObject *target = instruction.value_expr_index != LN_INVALID_INDEX ?
                                  ResolveObjectValue(ef.Value(
                                      instruction.value_expr_index, MakeNoneValue())) :
                                  nullptr;
      command_buffer.AppendSetUIFocus(
          this,
          m_gameObject,
          target,
          ef.Int(instruction.int_expr_index, 0),
          ef.String(instruction.string_expr_index, std::string()),
          instruction.property_ref_index,
          ef.SortKey(),
          instruction.source_ref_index);
      instruction_executed = true;
      break;
    }
    case LN_OpCode::SetPlayerPointer: {
      if (m_gameObject == nullptr) {
        instruction_executed = true;
        break;
      }
      /* Surface: an empty Surface is the tree's own object; a linked one that resolves to
       * nothing turns the pointer off. View Center has neither. */
      const bool aim = instruction.bool_value;
      KX_GameObject *surface = aim ? nullptr :
                               instruction.value_expr_index != LN_INVALID_INDEX ?
                                     ResolveObjectValue(ef.Value(
                                         instruction.value_expr_index, MakeNoneValue())) :
                                     m_gameObject;
      KX_GameObject *cursor = !aim && instruction.secondary_value_expr_index != LN_INVALID_INDEX ?
                                  ResolveObjectValue(
                                      ef.Value(instruction.secondary_value_expr_index,
                                                             MakeNoneValue())) :
                                  nullptr;
      LN_CommandBuffer::PlayerPointerSettings settings;
      settings.aim = aim;
      settings.surface = surface;
      settings.cursor = cursor;
      settings.move_action = ef.String(
          instruction.string_expr_index, std::string());
      settings.speed = ef.Float(instruction.float_expr_index, 1.0f);
      settings.reach = ef.Float(
          instruction.secondary_float_expr_index, 3.0f);
      settings.press_action = ef.String(
          instruction.secondary_string_expr_index, std::string());
      command_buffer.AppendSetPlayerPointer(
          m_gameObject,
          ef.Int(instruction.int_expr_index, 0),
          ef.Bool(instruction.bool_expr_index, true),
          settings,
          ef.SortKey(),
          instruction.source_ref_index);
      instruction_executed = true;
      break;
    }
    case LN_OpCode::TakeUIControl: {
      if (m_gameObject == nullptr) {
        CompleteUIOperation(instruction.property_ref_index, LN_UIResult::InvalidOwner, 0);
        instruction_executed = true;
        break;
      }
      KX_GameObject *target = instruction.value_expr_index != LN_INVALID_INDEX ?
                                  ResolveObjectValue(ef.Value(
                                      instruction.value_expr_index, MakeNoneValue())) :
                                  m_gameObject;
      if (target == nullptr) {
        CompleteUIOperation(instruction.property_ref_index, LN_UIResult::InvalidTarget, 0);
        instruction_executed = true;
        break;
      }
      command_buffer.AppendTakeUIControl(
          this,
          m_gameObject,
          target,
          ef.Int(instruction.int_expr_index, 0),
          ef.Bool(instruction.bool_expr_index, true),
          instruction.property_ref_index,
          ef.SortKey(),
          instruction.source_ref_index);
      instruction_executed = true;
      break;
    }
    case LN_OpCode::ScreenAnchor: {
      /* An empty Object input anchors the tree's own object, like the other UI commands. */
      KX_GameObject *target = instruction.value_expr_index != LN_INVALID_INDEX ?
                                  ResolveObjectValue(ef.Value(
                                      instruction.value_expr_index, MakeNoneValue())) :
                                  m_gameObject;
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      KX_Camera *camera = instruction.secondary_value_expr_index != LN_INVALID_INDEX ?
                              ResolveCameraValue(
                                  ef.Value(instruction.secondary_value_expr_index,
                                                         MakeNoneValue())) :
                              nullptr;
      /* Area Player's View: the player's split screen view in the window; nothing while the
       * player has no view on screen. */
      const bool in_player_view = instruction.secondary_int_value == 1;
      const int32_t user = in_player_view ?
                               ef.Int(instruction.int_expr_index, 0) :
                               0;
      KX_PlayerViews::Area player_area;
      RAS_Rect player_window;
      if (in_player_view &&
          !ResolvePlayerViewArea(m_gameObject, user, player_area, &player_window))
      {
        break;
      }
      if (camera == nullptr && scene != nullptr) {
        /* The menu camera draws over the whole window; a world object in a player's view is
         * seen through that player's camera. */
        if (target != nullptr && scene->IsOverlayRuntimeObject(target) &&
            scene->GetOverlayCamera() != nullptr)
        {
          camera = scene->GetOverlayCamera();
        }
        else {
          camera = in_player_view ? scene->GetPlayerViews().GetPlayerCamera(
                                        user, scene->GetActiveCamera()) :
                                    scene->GetActiveCamera();
        }
      }
      const float depth = ef.Float(instruction.float_expr_index, 1.0f);
      KX_KetsjiEngine *engine = KX_GetActiveEngine();
      RAS_ICanvas *canvas = engine ? engine->GetCanvas() : nullptr;
      RAS_Rasterizer *rasterizer = engine ? engine->GetRasterizer() : nullptr;
      if (target == nullptr || camera == nullptr || scene == nullptr ||
          target->GetScene() != scene || camera->GetScene() != scene || canvas == nullptr ||
          rasterizer == nullptr || canvas->GetWidth() <= 0 || canvas->GetHeight() <= 0 ||
          !std::isfinite(depth) || depth < 0.0f)
      {
        break;
      }
      const RAS_Rect display_area = rasterizer->GetRenderArea(
          canvas, RAS_Rasterizer::RAS_STEREO_LEFTEYE);
      RAS_Rect camera_area;
      RAS_Rect viewport;
      engine->GetSceneViewport(scene, camera, display_area, camera_area, viewport);
      /* The rectangle anchored in, in canvas pixels from the top left. */
      const int drawn_extra = LN_CameraViewportDrawnExtra(camera);
      float width = float(viewport.GetWidth() + drawn_extra);
      float height = float(viewport.GetHeight() + drawn_extra);
      float area_left = float(viewport.GetLeft());
      float area_top = float(canvas->GetHeight() - viewport.GetTop());
      if (in_player_view) {
        width = player_area.width * float(player_window.GetWidth());
        height = player_area.height * float(player_window.GetHeight());
        area_left = float(player_window.GetLeft()) +
                    player_area.left * float(player_window.GetWidth());
        area_top = float(canvas->GetHeight() - player_window.GetTop()) +
                   player_area.top * float(player_window.GetHeight());
      }
      if (width <= 0.0f || height <= 0.0f) {
        break;
      }
      MT_Vector3 offset = ef.Vector(
          instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f));
      const MT_Vector3 safe_margin = ef.Vector(
          instruction.secondary_vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f));
      MT_Vector3 reference_resolution(1920.0f, 1080.0f, 0.0f);
      if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
        ValueAsVector(ef.Value(
                          instruction.tertiary_value_expr_index, MakeNoneValue()),
                      reference_resolution);
      }
      if (ef.Bool(instruction.bool_expr_index, true) &&
          reference_resolution.x() > 0.0f && reference_resolution.y() > 0.0f)
      {
        offset.x() *= width / reference_resolution.x();
        offset.y() *= height / reference_resolution.y();
      }
      const int anchor = std::clamp(instruction.int_value, 0, 8);
      const int column = anchor % 3;
      const int row = anchor / 3;
      const float margin_x = std::max(0.0f, safe_margin.x());
      const float margin_y = std::max(0.0f, safe_margin.y());
      const float pixel_x = area_left +
                            (column == 0 ? margin_x :
                                           (column == 1 ? width * 0.5f : width - margin_x)) +
                            offset.x();
      const float pixel_y = area_top +
                            (row == 0 ? margin_y :
                                        (row == 1 ? height * 0.5f : height - margin_y)) +
                            offset.y();
      MT_Vector3 position;
      if (ProjectCanvasPixelToWorld(camera, pixel_x, pixel_y, depth, position)) {
        command_buffer.AppendSetWorldPosition(target,
                                              position,
                                              ef.SortKey(),
                                              instruction.source_ref_index);
        instruction_executed = true;
      }
      break;
    }
    default:
      return;
  }
  if (instruction_executed) {
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  }
}

template<class Frame>
void LN_RuntimeTree::OpDebugDrawInstruction(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;

  if (!RasterizerReady()) {
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }

  switch (instruction.opcode) {
    case LN_OpCode::DrawLine: {
      const MT_Vector3 from = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const MT_Vector3 to = ef.Vector(
          instruction.secondary_vector_expr_index, instruction.secondary_vector_value);
      const MT_Vector4 color = ef.Color(
          instruction.color_expr_index, instruction.color_value);
      KX_RasterizerDrawDebugLine(from, to, color);
      break;
    }
    case LN_OpCode::DrawArrow: {
      const MT_Vector3 from = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const MT_Vector3 to = ef.Vector(
          instruction.secondary_vector_expr_index, instruction.secondary_vector_value);
      const MT_Vector4 color = ef.Color(
          instruction.color_expr_index, instruction.color_value);
      DrawDebugArrow(from, to, color);
      break;
    }
    case LN_OpCode::DrawPath: {
      const LN_Value points_value = ef.Value(
          instruction.value_expr_index, MakeNoneValue());
      std::vector<MT_Vector3> points;
      if (ValueAsVectorList(points_value, points)) {
        const MT_Vector4 color = ef.Color(
            instruction.color_expr_index, instruction.color_value);
        for (size_t index = 1; index < points.size(); index++) {
          KX_RasterizerDrawDebugLine(points[index - 1], points[index], color);
        }
      }
      break;
    }
    case LN_OpCode::DrawBox: {
      MT_Vector3 origin = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const MT_Vector4 color = ef.Color(
          instruction.color_expr_index, instruction.color_value);
      const float half_x =
          std::max(0.0f, ef.Float(instruction.float_expr_index, 1.0f)) *
          0.5f;
      const float half_y = std::max(0.0f,
                                    ef.Float(
                                        instruction.secondary_float_expr_index, 1.0f)) *
                           0.5f;
      const float half_z = std::max(0.0f,
                                    ef.Float(
                                        instruction.tertiary_float_expr_index, 1.0f)) *
                           0.5f;
      if (instruction.bool_value) {
        origin += MT_Vector3(half_x, half_y, half_z);
      }
      DrawDebugBox(origin, half_x, half_y, half_z, color);
      break;
    }
    case LN_OpCode::DrawMesh: {
      KX_GameObject *target = m_gameObject;
      if (instruction.value_expr_index != LN_INVALID_INDEX) {
        const LN_Value object_value = ef.Value(
            instruction.value_expr_index, MakeNoneValue());
        if (KX_GameObject *object = ResolveObjectValue(object_value)) {
          target = object;
        }
      }
      if (target != nullptr) {
        const MT_Vector4 color = ef.Color(
            instruction.color_expr_index, instruction.color_value);
        const MT_Transform transform = target->NodeGetWorldTransform();
        for (int mesh_index = 0; mesh_index < target->GetMeshCount(); mesh_index++) {
          RAS_MeshObject *mesh = target->GetMesh(mesh_index);
          if (mesh == nullptr) {
            continue;
          }
          for (int poly_index = 0; poly_index < mesh->NumPolygons(); poly_index++) {
            RAS_Polygon *polygon = mesh->GetPolygon(poly_index);
            if (polygon == nullptr || polygon->VertexCount() < 2) {
              continue;
            }
            const int vertex_count = polygon->VertexCount();
            for (int vertex_index = 0; vertex_index < vertex_count; vertex_index++) {
              const RAS_IVertex *from_vertex = polygon->GetVertex(vertex_index);
              const RAS_IVertex *to_vertex = polygon->GetVertex((vertex_index + 1) %
                                                                vertex_count);
              if (from_vertex == nullptr || to_vertex == nullptr) {
                continue;
              }
              KX_RasterizerDrawDebugLine(transform * MT_Vector3(from_vertex->getXYZ()),
                                         transform * MT_Vector3(to_vertex->getXYZ()),
                                         color);
            }
          }
        }
      }
      break;
    }
    case LN_OpCode::DrawAxis: {
      KX_GameObject *target = m_gameObject;
      if (instruction.value_expr_index != LN_INVALID_INDEX) {
        const LN_Value object_value = ef.Value(
            instruction.value_expr_index, MakeNoneValue());
        if (KX_GameObject *object = ResolveObjectValue(object_value)) {
          target = object;
        }
      }
      if (target != nullptr) {
        const float length = std::max(
            0.0f, ef.Float(instruction.float_expr_index, 1.0f));
        const MT_Vector3 origin = target->NodeGetWorldPosition();
        const MT_Matrix3x3 orientation = target->NodeGetWorldOrientation();
        const MT_Vector3 axes[3] = {
            orientation * MT_Vector3(length, 0.0f, 0.0f),
            orientation * MT_Vector3(0.0f, length, 0.0f),
            orientation * MT_Vector3(0.0f, 0.0f, length),
        };
        const MT_Vector4 colors[3] = {
            MT_Vector4(1.0f, 0.0f, 0.0f, 1.0f),
            MT_Vector4(0.0f, 1.0f, 0.0f, 1.0f),
            MT_Vector4(0.0f, 0.35f, 1.0f, 1.0f),
        };
        for (int axis = 0; axis < 3; axis++) {
          KX_RasterizerDrawDebugLine(origin, origin + axes[axis], colors[axis]);
          KX_RasterizerDrawDebugLine(origin, origin - axes[axis], colors[axis] * 0.55f);
        }
      }
      break;
    }
    default:
      return;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectVectorCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  const LN_VectorCommandPayload payload = ef.VectorCommandPayload(instruction);
  const LN_VectorOperationMode mode = LN_VectorOperationMode(payload.operation_mode);
  const LN_VectorOperationChannel channel = LN_VectorOperationChannel(payload.operation_channel);
  const bool is_orientation_command = instruction.opcode == LN_OpCode::SetWorldOrientation ||
                                      instruction.opcode == LN_OpCode::SetLocalOrientation ||
                                      (instruction.opcode == LN_OpCode::SetTransformVector &&
                                       channel == LN_VectorOperationChannel::Orientation) ||
                                      (instruction.opcode == LN_OpCode::ApplyTransformVector &&
                                       channel == LN_VectorOperationChannel::Rotation);
  const LN_Rotation rotation = is_orientation_command ?
                                   ef.Rotation(
                                       payload.vector_expr_index,
                                       LN_Rotation::FromEulerXYZ(payload.vector_value)) :
                                   LN_Rotation{};
  const MT_Vector3 value = is_orientation_command ?
                               MT_Vector3(0.0f, 0.0f, 0.0f) :
                               ef.Vector(
                                   payload.vector_expr_index, payload.vector_value);
  KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index);
  if (target == nullptr) {
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }

  if (instruction.opcode == LN_OpCode::ApplyTransformVector &&
      (channel == LN_VectorOperationChannel::Movement ||
       channel == LN_VectorOperationChannel::Rotation))
  {
    const LN_CommandBuffer::CommandType command_type =
        channel == LN_VectorOperationChannel::Movement ?
            LN_CommandBuffer::CommandType::ApplyMovement :
            LN_CommandBuffer::CommandType::ApplyRotation;
    if (!ef.RelativeTransformIsSafe(target, instruction, instruction_id, command_type)) {
      return;
    }
  }

  AppendVectorCommand(ef, instruction, payload, target, value, rotation);
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

/** The appends of a vector command, by opcode (OpDirectVectorCommand and the lean VM's native
 * OpLeanVector share them). `value` is the vector read, `rotation` the rotation read (only one of
 * them is meaningful, by opcode and channel). */
template<class Frame>
void LN_RuntimeTree::AppendVectorCommand(Frame &ef,
                                         const LN_Instruction &instruction,
                                         const LN_VectorCommandPayload &payload,
                                         KX_GameObject *target,
                                         const MT_Vector3 &value,
                                         const LN_Rotation &rotation)
{
  LN_CommandBuffer &command_buffer = ef.command_buffer;
  const LN_VectorOperationMode mode = LN_VectorOperationMode(payload.operation_mode);
  const LN_VectorOperationChannel channel = LN_VectorOperationChannel(payload.operation_channel);
  const uint64_t sort_key = ef.SortKey();
  switch (instruction.opcode) {
    case LN_OpCode::SetWorldPosition:
      command_buffer.AppendSetWorldPosition(
          target, value, sort_key, instruction.source_ref_index);
      break;
    case LN_OpCode::SetLocalPosition:
      command_buffer.AppendSetLocalPosition(
          target, value, sort_key, instruction.source_ref_index);
      break;
    case LN_OpCode::SetWorldOrientation:
      command_buffer.AppendSetWorldOrientation(
          target, rotation, sort_key, instruction.source_ref_index);
      break;
    case LN_OpCode::SetLocalOrientation:
      command_buffer.AppendSetLocalOrientation(
          target, rotation, sort_key, instruction.source_ref_index);
      break;
    case LN_OpCode::SetWorldScale:
      command_buffer.AppendSetWorldScale(target, value, sort_key, instruction.source_ref_index);
      break;
    case LN_OpCode::SetLocalScale:
      command_buffer.AppendSetLocalScale(target, value, sort_key, instruction.source_ref_index);
      break;
    case LN_OpCode::SetLinearVelocity:
      command_buffer.AppendSetLinearVelocity(
          target, value, sort_key, instruction.source_ref_index);
      break;
    case LN_OpCode::SetLocalLinearVelocity:
      command_buffer.AppendSetLocalLinearVelocity(
          target, value, sort_key, instruction.source_ref_index);
      break;
    case LN_OpCode::SetAngularVelocity:
      command_buffer.AppendSetAngularVelocity(
          target, value, sort_key, instruction.source_ref_index);
      break;
    case LN_OpCode::SetLocalAngularVelocity:
      command_buffer.AppendSetLocalAngularVelocity(
          target, value, sort_key, instruction.source_ref_index);
      break;
    case LN_OpCode::SetTransformVector: {
      const bool local = (mode == LN_VectorOperationMode::LocalFromBool) ?
                             ef.Bool(payload.bool_expr_index, false) :
                             (mode == LN_VectorOperationMode::Local);
      if (channel == LN_VectorOperationChannel::Position) {
        if (local) {
          command_buffer.AppendSetLocalPosition(
              target, value, sort_key, instruction.source_ref_index);
        }
        else {
          command_buffer.AppendSetWorldPosition(
              target, value, sort_key, instruction.source_ref_index);
        }
      }
      else if (channel == LN_VectorOperationChannel::Orientation) {
        if (local) {
          command_buffer.AppendSetLocalOrientation(
              target, rotation, sort_key, instruction.source_ref_index);
        }
        else {
          command_buffer.AppendSetWorldOrientation(
              target, rotation, sort_key, instruction.source_ref_index);
        }
      }
      else if (channel == LN_VectorOperationChannel::Scale) {
        if (local) {
          command_buffer.AppendSetLocalScale(
              target, value, sort_key, instruction.source_ref_index);
        }
        else {
          command_buffer.AppendSetWorldScale(
              target, value, sort_key, instruction.source_ref_index);
        }
      }
      break;
    }
    case LN_OpCode::SetVelocityVector: {
      const bool local = (mode == LN_VectorOperationMode::LocalFromBool) ?
                             ef.Bool(payload.bool_expr_index, false) :
                             (mode == LN_VectorOperationMode::Local);
      if (channel == LN_VectorOperationChannel::LinearVelocity) {
        if (local) {
          command_buffer.AppendSetLocalLinearVelocity(
              target, value, sort_key, instruction.source_ref_index);
        }
        else {
          command_buffer.AppendSetLinearVelocity(
              target, value, sort_key, instruction.source_ref_index);
        }
      }
      else if (channel == LN_VectorOperationChannel::AngularVelocity) {
        if (local) {
          command_buffer.AppendSetLocalAngularVelocity(
              target, value, sort_key, instruction.source_ref_index);
        }
        else {
          command_buffer.AppendSetAngularVelocity(
              target, value, sort_key, instruction.source_ref_index);
        }
      }
      break;
    }
    case LN_OpCode::ApplyTransformVector: {
      const bool local = (mode == LN_VectorOperationMode::LocalFromBool) ?
                             ef.Bool(payload.bool_expr_index, false) :
                             (mode == LN_VectorOperationMode::Local);
      if (channel == LN_VectorOperationChannel::Movement) {
        command_buffer.AppendApplyMovement(
            target, value, local, sort_key, instruction.source_ref_index);
      }
      else if (channel == LN_VectorOperationChannel::Rotation) {
        command_buffer.AppendApplyRotation(
            target, rotation, local, sort_key, instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::ApplyPhysicsVector: {
      const bool local = (mode == LN_VectorOperationMode::LocalFromBool) ?
                             ef.Bool(payload.bool_expr_index, false) :
                             (mode == LN_VectorOperationMode::Local);
      if (channel == LN_VectorOperationChannel::Force) {
        command_buffer.AppendApplyForce(
            target, value, local, sort_key, instruction.source_ref_index);
      }
      else if (channel == LN_VectorOperationChannel::Torque) {
        command_buffer.AppendApplyTorque(
            target, value, local, sort_key, instruction.source_ref_index);
      }
      break;
    }
    default:
      break;
  }
}

template<class Frame>
void LN_RuntimeTree::OpDirectPhysicsCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  auto mark_done = [&]() { MarkInstructionExecuted(event, instruction_id, context.tick_index); };
  auto append_targeted = [&](auto &&append_fn) {
    if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
      append_fn(target, ef.SortKey());
    }
    mark_done();
  };
  switch (instruction.opcode) {
    case LN_OpCode::ApplyMovement:
    case LN_OpCode::ApplyRotation:
    case LN_OpCode::ApplyForce:
    case LN_OpCode::ApplyTorque: {
      const LN_VectorCommandPayload payload = ef.VectorCommandPayload(instruction);
      const bool is_rotation = instruction.opcode == LN_OpCode::ApplyRotation;
      const LN_Rotation rotation = is_rotation ?
                                       ef.Rotation(
                                           payload.vector_expr_index,
                                           LN_Rotation::FromEulerXYZ(payload.vector_value)) :
                                       LN_Rotation{};
      const MT_Vector3 value = is_rotation ? MT_Vector3(0.0f, 0.0f, 0.0f) :
                                             ef.Vector(payload.vector_expr_index,
                                                                     payload.vector_value);
      const bool local = ef.Bool(payload.bool_expr_index, false);
      bool accepted = true;
      if (KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index)) {
        if (instruction.opcode == LN_OpCode::ApplyMovement ||
            instruction.opcode == LN_OpCode::ApplyRotation)
        {
          const LN_CommandBuffer::CommandType command_type =
              instruction.opcode == LN_OpCode::ApplyMovement ?
                  LN_CommandBuffer::CommandType::ApplyMovement :
                  LN_CommandBuffer::CommandType::ApplyRotation;
          accepted = ef.RelativeTransformIsSafe(
              target, instruction, instruction_id, command_type);
        }
        if (!accepted) {
          break;
        }
        const uint64_t sort_key = ef.SortKey();
        switch (instruction.opcode) {
          case LN_OpCode::ApplyMovement:
            command_buffer.AppendApplyMovement(
                target, value, local, sort_key, instruction.source_ref_index);
            break;
          case LN_OpCode::ApplyRotation:
            command_buffer.AppendApplyRotation(
                target, rotation, local, sort_key, instruction.source_ref_index);
            break;
          case LN_OpCode::ApplyForce:
            command_buffer.AppendApplyForce(
                target, value, local, sort_key, instruction.source_ref_index);
            break;
          case LN_OpCode::ApplyTorque:
            command_buffer.AppendApplyTorque(
                target, value, local, sort_key, instruction.source_ref_index);
            break;
          default:
            break;
        }
      }
      if (accepted) {
        mark_done();
      }
      break;
    }
    case LN_OpCode::ApplyImpulse: {
      const LN_VectorCommandPayload payload = ef.VectorCommandPayload(instruction);
      const MT_Vector3 impulse = ef.Vector(
          payload.vector_expr_index, payload.vector_value);
      const MT_Vector3 attach = ef.Vector(
          payload.secondary_vector_expr_index, payload.secondary_vector_value);
      if (KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index)) {
        command_buffer.AppendApplyImpulse(target,
                                          attach,
                                          impulse,
                                          ef.SortKey(),
                                          instruction.source_ref_index);
      }
      mark_done();
      break;
    }
    case LN_OpCode::ApplyForceToTarget: {
      MT_Vector3 target_position = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const float force = ef.Float(instruction.float_expr_index, 10.0f);
      const float reached_threshold = std::max(
          ef.Float(instruction.secondary_float_expr_index, 0.1f), 0.0f);
      const float stop_distance = std::max(
          ef.Float(instruction.tertiary_float_expr_index, 0.1f), 0.0f);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        /* (One code with the column kernel: ColumnForceToTarget.) */
        bool reached = false;
        MT_Vector3 force_vector;
        const bool apply = ColumnForceToTarget(target_position,
                                               force,
                                               reached_threshold,
                                               stop_distance,
                                               target->NodeGetWorldPosition(),
                                               reached,
                                               force_vector);
        if (reached) {
          const uint32_t state_index = InstructionStateIndex(event, instruction_id);
          if (state_index != LN_INVALID_INDEX && state_index < m_instructionStamps.size()) {
            InstructionMotionState &state = MotionState(state_index);
            state.last_reached_tick = context.tick_index;
            state.last_reached_scope_serial = m_activeExecutionScopeSerial;
          }
        }
        if (apply) {
          command_buffer.AppendApplyForce(
              target, force_vector, false, ef.SortKey(), instruction.source_ref_index);
        }
      }
      mark_done();
      break;
    }
    case LN_OpCode::Translate: {
      const MT_Vector3 vector = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const float speed = ef.Float(instruction.float_expr_index, 1.0f);
      const bool local = ef.Bool(instruction.bool_expr_index, false);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        const float tick_seconds = float(std::max(0.0, context.delta_time));
        command_buffer.AppendApplyMovement(target,
                                           vector * (speed * tick_seconds),
                                           local,
                                           ef.SortKey(),
                                           instruction.source_ref_index);
      }
      mark_done();
      break;
    }
    case LN_OpCode::MoveToward: {
      const MT_Vector3 target_pos = ef.Vector(
          instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f));
      const float speed = ef.Float(instruction.float_expr_index, 1.0f);
      const float stop_distance = ef.Float(
          instruction.secondary_float_expr_index, 0.5f);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        /* Reached (only when it is linked: the compiler then gives where the object is): this
         * step's move ends within Stop Distance, as the flush moves by
         * min(Speed * dt, distance - Stop Distance), or the object is already there. */
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const MT_Vector3 position = instruction.secondary_vector_expr_index == LN_INVALID_INDEX ?
                                        MT_Vector3(nan, nan, nan) :
                                        ef.Vector(
                                            instruction.secondary_vector_expr_index,
                                            MT_Vector3(nan, nan, nan));
        const float distance = (target_pos - position).length();
        const float step = std::max(speed * float(std::max(0.0, context.delta_time)), 0.0f);
        const float tolerance = std::max(
            1.0e-4f, (std::fabs(stop_distance) + float(target_pos.length())) * 1.0e-6f);
        if (std::isfinite(distance) && std::isfinite(step) &&
            distance <= std::max(stop_distance, 0.0f) + step + tolerance)
        {
          const uint32_t state_index = InstructionStateIndex(event, instruction_id);
          if (state_index != LN_INVALID_INDEX && state_index < m_instructionStamps.size()) {
            InstructionMotionState &state = MotionState(state_index);
            state.last_reached_tick = context.tick_index;
            state.last_reached_scope_serial = m_activeExecutionScopeSerial;
          }
        }
        command_buffer.AppendMoveToward(target,
                                        target_pos,
                                        speed,
                                        stop_distance,
                                        false,
                                        true,
                                        float(context.delta_time),
                                        ef.SortKey(),
                                        instruction.source_ref_index);
      }
      mark_done();
      break;
    }
    case LN_OpCode::SetCollisionGroup: {
      const int32_t group = ef.Int(instruction.int_expr_index, 0);
      append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
        command_buffer.AppendSetCollisionGroup(
            target, group, sort_key, instruction.source_ref_index);
      });
      break;
    }
    case LN_OpCode::SetPhysics: {
      const bool active = ef.Bool(instruction.bool_expr_index, true);
      append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
        command_buffer.AppendSetPhysics(target, active, sort_key, instruction.source_ref_index);
      });
      break;
    }
    case LN_OpCode::SetDynamics: {
      const bool enabled = ef.Bool(
          instruction.bool_expr_index, instruction.bool_value);
      append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
        command_buffer.AppendSetDynamics(
            target, instruction.int_value, enabled, sort_key, instruction.source_ref_index);
      });
      break;
    }
    case LN_OpCode::RebuildCollisionShape: {
      append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
        command_buffer.AppendRebuildCollisionShape(
            target, sort_key, instruction.source_ref_index);
      });
      break;
    }
    case LN_OpCode::SetRigidBodyAttribute: {
      const LN_RigidBodyAttribute attribute = static_cast<LN_RigidBodyAttribute>(
          instruction.int_value);
      MT_Vector3 value = instruction.vector_value;
      MT_Vector3 secondary_value = instruction.secondary_vector_value;
      float scalar_value = 0.0f;
      bool bool_value = false;
      bool secondary_bool_value = false;

      switch (attribute) {
        case LN_RigidBodyAttribute::Mass:
        case LN_RigidBodyAttribute::Friction:
        case LN_RigidBodyAttribute::Restitution:
        case LN_RigidBodyAttribute::MinLinearVelocity:
        case LN_RigidBodyAttribute::MaxLinearVelocity:
        case LN_RigidBodyAttribute::MinAngularVelocity:
        case LN_RigidBodyAttribute::MaxAngularVelocity:
        case LN_RigidBodyAttribute::GravityFactor:
          scalar_value = ef.Float(
              instruction.float_expr_index, instruction.vector_value.x());
          break;
        case LN_RigidBodyAttribute::Damping:
          value = MT_Vector3(
              ef.Float(
                  instruction.float_expr_index, instruction.vector_value.x()),
              ef.Float(
                  instruction.secondary_float_expr_index, instruction.vector_value.y()),
              0.0f);
          break;
        case LN_RigidBodyAttribute::Ccd:
        case LN_RigidBodyAttribute::AllowPhysicsRotation:
          bool_value = ef.Bool(
              instruction.bool_expr_index, instruction.bool_value);
          break;
        case LN_RigidBodyAttribute::Sleeping:
          bool_value = ef.Bool(
              instruction.bool_expr_index, instruction.bool_value);
          secondary_bool_value = ef.Bool(instruction.secondary_bool_expr_index,
                                                       instruction.secondary_int_value != 0);
          break;
        case LN_RigidBodyAttribute::AxisLocks:
          value = ef.Vector(
              instruction.vector_expr_index, instruction.vector_value);
          secondary_value = ef.Vector(instruction.secondary_vector_expr_index,
                                                    instruction.secondary_vector_value);
          break;
      }

      append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
        command_buffer.AppendSetRigidBodyAttribute(target,
                                                   attribute,
                                                   value,
                                                   secondary_value,
                                                   scalar_value,
                                                   bool_value,
                                                   secondary_bool_value,
                                                   sort_key,
                                                   instruction.source_ref_index);
      });
      break;
    }
    case LN_OpCode::SetGravity: {
      const LN_VectorCommandPayload payload = ef.VectorCommandPayload(instruction);
      const MT_Vector3 gravity = ef.Vector(
          payload.vector_expr_index, payload.vector_value);
      command_buffer.AppendSetGravity(m_gameObject,
                                      gravity,
                                      ef.SortKey(),
                                      instruction.source_ref_index);
      mark_done();
      break;
    }
    case LN_OpCode::CharacterJump: {
      append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
        command_buffer.AppendCharacterJump(target, sort_key, instruction.source_ref_index);
      });
      break;
    }
    case LN_OpCode::SetCharacterGravity: {
      const LN_VectorCommandPayload payload = ef.VectorCommandPayload(instruction);
      const MT_Vector3 gravity = ef.Vector(
          payload.vector_expr_index, payload.vector_value);
      if (KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index)) {
        command_buffer.AppendSetCharacterGravity(
            target,
            gravity,
            ef.SortKey(),
            instruction.source_ref_index);
      }
      mark_done();
      break;
    }
    case LN_OpCode::SetCharacterJumpSpeed: {
      const float jump_speed = ef.Float(
          instruction.float_expr_index, instruction.vector_value.x());
      append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
        command_buffer.AppendSetCharacterJumpSpeed(
            target, jump_speed, sort_key, instruction.source_ref_index);
      });
      break;
    }
    case LN_OpCode::SetCharacterMaxJumps: {
      const int32_t max_jumps = ef.Int(
          instruction.int_expr_index, instruction.int_value);
      append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
        command_buffer.AppendSetCharacterMaxJumps(
            target, max_jumps, sort_key, instruction.source_ref_index);
      });
      break;
    }
    case LN_OpCode::SetCharacterWalkDirection: {
      const LN_VectorCommandPayload payload = ef.VectorCommandPayload(instruction);
      const MT_Vector3 walk_direction = ef.Vector(
          payload.vector_expr_index, payload.vector_value);
      const bool local = ef.Bool(
          payload.bool_expr_index, payload.bool_value);
      if (KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index)) {
        command_buffer.AppendSetCharacterWalkDirection(
            target,
            walk_direction,
            local,
            ef.SortKey(),
            instruction.source_ref_index);
      }
      mark_done();
      break;
    }
    case LN_OpCode::SetCharacterVelocity: {
      const MT_Vector3 velocity = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const float time = ef.Float(
          instruction.float_expr_index, instruction.secondary_vector_value.x());
      const bool local = ef.Bool(
          instruction.bool_expr_index, instruction.bool_value);
      append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
        command_buffer.AppendSetCharacterVelocity(
            target, velocity, time, local, sort_key, instruction.source_ref_index);
      });
      break;
    }
    case LN_OpCode::VehicleControl: {
      const MT_Vector3 control = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const float steering = ef.Float(
          instruction.float_expr_index, instruction.secondary_vector_value.x());
      append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
        command_buffer.AppendVehicleControl(
            target, control, steering, sort_key, instruction.source_ref_index);
      });
      break;
    }
    case LN_OpCode::AlignAxisToVector: {
      const MT_Vector3 vector = ef.Vector(
          instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 1.0f));
      const int32_t axis = ef.Int(instruction.int_expr_index, 2);
      /* (Per step: a run behind Every N Steps covers N.) */
      const float factor = LN_StepFactor(ef.Float(instruction.float_expr_index, 1.0f),
                                         StepGateStepsCovered(instruction.step_gate_index));
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        command_buffer.AppendAlignAxisToVector(
            target,
            vector,
            axis,
            factor,
            ef.SortKey(),
            instruction.source_ref_index);
      }
      mark_done();
      break;
    }
    case LN_OpCode::RotateToward: {
      const MT_Vector3 target_point = ef.Vector(
          instruction.vector_expr_index, MT_Vector3(1.0f, 0.0f, 0.0f));
      /* (Per step: a run behind Every N Steps covers N.) */
      const float factor = LN_StepFactor(ef.Float(instruction.float_expr_index, 1.0f),
                                         StepGateStepsCovered(instruction.step_gate_index));
      const int32_t rot_axis = ef.Int(instruction.int_expr_index, 2);
      const int32_t front_axis = ef.Int(
          instruction.secondary_int_expr_index, 1);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        command_buffer.AppendRotateToward(target,
                                          target_point,
                                          factor,
                                          rot_axis,
                                          front_axis,
                                          false,
                                          ef.SortKey(),
                                          instruction.source_ref_index);
      }
      mark_done();
      break;
    }
    case LN_OpCode::SlowFollow: {
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        /* (Per step: a run behind Every N Steps covers N.) */
        const float factor = LN_StepFactor(ef.Float(instruction.float_expr_index, 1.0f),
                                           StepGateStepsCovered(instruction.step_gate_index));
        KX_GameObject *follow_target = nullptr;
        if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
          const LN_Value target_val = ef.Value(
              instruction.secondary_value_expr_index, MakeNoneValue());
          follow_target = ResolveObjectValue(target_val);
        }
        if (follow_target) {
          command_buffer.AppendSlowFollow(target,
                                          follow_target,
                                          factor,
                                          0,
                                          ef.SortKey(),
                                          instruction.source_ref_index);
        }
      }
      mark_done();
      break;
    }
    case LN_OpCode::VehicleApplyEngineForce:
    case LN_OpCode::VehicleApplyBraking:
    case LN_OpCode::VehicleApplySteering:
    case LN_OpCode::SetVehicleSuspensionCompression:
    case LN_OpCode::SetVehicleSuspensionStiffness:
    case LN_OpCode::SetVehicleSuspensionDamping:
    case LN_OpCode::SetVehicleWheelFriction: {
      const float value = ef.Float(
          instruction.float_expr_index, instruction.vector_value.x());
      const int32_t wheel_count = ef.Int(
          instruction.int_expr_index, instruction.int_value);
      const int32_t axis = ef.Int(
          instruction.secondary_int_expr_index, instruction.secondary_int_value);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        const uint64_t sort_key = ef.SortKey();
        switch (instruction.opcode) {
          case LN_OpCode::VehicleApplyEngineForce:
            command_buffer.AppendVehicleApplyEngineForce(
                target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
            break;
          case LN_OpCode::VehicleApplyBraking:
            command_buffer.AppendVehicleApplyBraking(
                target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
            break;
          case LN_OpCode::VehicleApplySteering:
            command_buffer.AppendVehicleApplySteering(
                target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
            break;
          case LN_OpCode::SetVehicleSuspensionCompression:
            command_buffer.AppendSetVehicleSuspensionCompression(
                target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
            break;
          case LN_OpCode::SetVehicleSuspensionStiffness:
            command_buffer.AppendSetVehicleSuspensionStiffness(
                target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
            break;
          case LN_OpCode::SetVehicleSuspensionDamping:
            command_buffer.AppendSetVehicleSuspensionDamping(
                target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
            break;
          case LN_OpCode::SetVehicleWheelFriction:
            command_buffer.AppendSetVehicleWheelFriction(
                target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
            break;
          default:
            break;
        }
      }
      mark_done();
      break;
    }
    default:
      mark_done();
      break;
  }
}

template<class Frame>
void LN_RuntimeTree::OpDirectGamePropertyCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  const LN_GamePropertyCommandPayload payload = ef.GamePropertyCommandPayload(instruction);
  if (payload.property_ref_index == LN_INVALID_INDEX || m_program == nullptr ||
      payload.property_ref_index >= m_program->GetGamePropertyRefs().size())
  {
    return;
  }

  if (payload.modify_op != LN_PropertyModifyOp::None) {
    LN_PropertyModify modify;
    modify.op = payload.modify_op;
    modify.operand = ef.Float(payload.float_expr_index, 0.0f);
    modify.clamped = payload.modify_min_expr_index != LN_INVALID_INDEX &&
                     payload.modify_max_expr_index != LN_INVALID_INDEX;
    if (modify.clamped) {
      modify.min_value = ef.Float(payload.modify_min_expr_index, 0.0f);
      modify.max_value = ef.Float(payload.modify_max_expr_index, 0.0f);
    }
    if (KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index)) {
      command_buffer.AppendModifyGameProperty(this,
                                              target,
                                              payload.property_ref_index,
                                              modify,
                                              ef.SortKey(),
                                              instruction.source_ref_index);
    }
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }

  /* Set Game Property Bool, Int or Float: the number stays a number (no LN_Value made), the
   * same write as the value those nodes made (LN_PropertyNumber::ToValue). */
  if (payload.value_expr_index == LN_INVALID_INDEX &&
      (payload.value_type == LN_ValueType::Bool || payload.value_type == LN_ValueType::Int ||
       payload.value_type == LN_ValueType::Float))
  {
    LN_PropertyNumber number;
    number.type = payload.value_type;
    switch (payload.value_type) {
      case LN_ValueType::Bool:
        number.bool_value = ef.Bool(payload.bool_expr_index, false);
        break;
      case LN_ValueType::Int:
        number.int_value = ef.Int(payload.int_expr_index, 0);
        break;
      default:
        number.float_value = ef.Float(payload.float_expr_index, 0.0f);
        break;
    }
    if (KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index)) {
      /* The snapshot's handle of the property, with the stamp it was found under (A7). */
      uint64_t handle_stamp = 0;
      EXP_Value *property_handle = m_snapshot.GetGamePropertyHandle(
          target, payload.property_ref_index, handle_stamp);
      command_buffer.AppendSetGamePropertyNumberRef(
          this,
          target,
          payload.property_ref_index,
          number,
          ef.SortKey(),
          instruction.source_ref_index,
          property_handle,
          handle_stamp);
    }
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }

  LN_Value value = ef.Value(payload.value_expr_index, MakeNoneValue());
  if (!value.exists && payload.value_expr_index == LN_INVALID_INDEX) {
    value.exists = true;
    value.type = payload.value_type;
    switch (payload.value_type) {
      case LN_ValueType::Bool:
        value.bool_value = ef.Bool(payload.bool_expr_index, false);
        break;
      case LN_ValueType::Int:
        value.int_value = ef.Int(payload.int_expr_index, 0);
        break;
      case LN_ValueType::Float:
        value.float_value = ef.Float(payload.float_expr_index, 0.0f);
        break;
      case LN_ValueType::String:
        value.string_value = ef.String(payload.string_expr_index, "");
        break;
      case LN_ValueType::Vector:
        value.vector_value = ef.Vector(
            payload.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f));
        break;
      case LN_ValueType::Color:
        value.color_value = ef.Color(
            payload.color_expr_index, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        break;
      default:
        value.exists = false;
        break;
    }
  }

  if (KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index)) {
    command_buffer.AppendSetGamePropertyRef(this,
                                            target,
                                            payload.property_ref_index,
                                            value,
                                            ef.SortKey(),
                                            instruction.source_ref_index);
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectObjectColorCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  const MT_Vector4 color = ef.Color(
      instruction.color_expr_index, instruction.color_value);
  if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
    command_buffer.AppendSetObjectColor(target,
                                        color,
                                        ef.SortKey(),
                                        instruction.source_ref_index);
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectObjectStateCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  switch (instruction.opcode) {
    case LN_OpCode::SetVisibility: {
      const bool visible = ef.Bool(instruction.bool_expr_index, true);
      const bool recursive = ef.Bool(
          instruction.secondary_bool_expr_index, false);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        command_buffer.AppendSetVisibility(target,
                                           visible,
                                           recursive,
                                           ef.SortKey(),
                                           instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::SetUIInteractable: {
      const bool interactable = ef.Bool(instruction.bool_expr_index, true);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        command_buffer.AppendSetUIInteractable(target,
                                               interactable,
                                               ef.SortKey(),
                                               instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::ResetSmoothing: {
      /* Set Object Attribute's Teleport, right after its write. */
      if (!ef.Bool(instruction.bool_expr_index, false)) {
        break;
      }
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        command_buffer.AppendResetSmoothing(target,
                                            ef.SortKey(),
                                            instruction.source_ref_index);
      }
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectCameraCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  KX_GameObject *camera_object = ef.ResolveRequiredObject(instruction.value_expr_index);
  const uint64_t sort_key = ef.SortKey();
  switch (instruction.opcode) {
    case LN_OpCode::SetActiveCamera: {
      /* User 0 sets the active camera; 1 to 7 a split screen player's camera. */
      const int32_t user = ef.Int(instruction.int_expr_index, 0);
      if (user == 0) {
        command_buffer.AppendSetActiveCamera(
            camera_object, sort_key, instruction.source_ref_index);
      }
      else {
        command_buffer.AppendSetPlayerCamera(
            m_gameObject, camera_object, user, sort_key, instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::SetSplitScreen: {
      const bool enabled = ef.Bool(instruction.bool_expr_index, true);
      const int32_t divider = ef.Int(instruction.int_expr_index, 0);
      const bool keep_mouse = ef.Bool(
          instruction.secondary_bool_expr_index, true);
      command_buffer.AppendSetSplitScreen(m_gameObject,
                                          enabled,
                                          instruction.int_value |
                                              (keep_mouse ? 0 : LN_SPLIT_SCREEN_FREE_MOUSE),
                                          divider,
                                          sort_key,
                                          instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetCameraViewport: {
      const bool enabled = ef.Bool(instruction.bool_expr_index, true);
      const MT_Vector4 area(
          ef.Float(instruction.float_expr_index, 0.7f),
          ef.Float(instruction.secondary_float_expr_index, 0.05f),
          ef.Float(instruction.tertiary_float_expr_index, 0.25f),
          ef.Float(instruction.quaternary_float_expr_index, 0.25f));
      const bool on_top = ef.Bool(
          instruction.secondary_bool_expr_index, true);
      /* Area Player's View: the rectangle is a part of that player's view (a User that is no
       * player has no view: not drawn). */
      const int32_t user = ef.Int(instruction.int_expr_index, 0);
      const int32_t player = instruction.secondary_int_value != 1 ? -1 :
                             user >= 0                          ? user :
                                                                  INT32_MAX;
      command_buffer.AppendSetCameraViewport(m_gameObject,
                                             camera_object,
                                             enabled,
                                             area,
                                             on_top,
                                             player,
                                             sort_key,
                                             instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetCameraVisibility: {
      /* Object: empty is the tree's own object. */
      KX_GameObject *target = instruction.secondary_value_expr_index != LN_INVALID_INDEX ?
                                  ef.ResolveRequiredObject(
                                      instruction.secondary_value_expr_index) :
                                  m_gameObject;
      command_buffer.AppendSetCameraVisibility(
          target,
          camera_object,
          ef.Bool(instruction.bool_expr_index, false),
          ef.Bool(instruction.secondary_bool_expr_index, false),
          ef.Bool(instruction.tertiary_bool_expr_index, false),
          sort_key,
          instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetCameraFov: {
      const float fov = ef.Float(instruction.float_expr_index, 90.0f);
      command_buffer.AppendSetCameraFov(
          camera_object, fov, sort_key, instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetCameraOrthoScale: {
      const float scale = ef.Float(instruction.float_expr_index, 1.0f);
      command_buffer.AppendSetCameraOrthoScale(
          camera_object, scale, sort_key, instruction.source_ref_index);
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectLightCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  KX_GameObject *target = ef.ResolveCommandTarget(instruction);
  if (target != nullptr) {
    const uint64_t sort_key = ef.SortKey();
    switch (instruction.opcode) {
      case LN_OpCode::MakeLightUnique: {
        command_buffer.AppendMakeLightUnique(target, sort_key, instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetLightColor: {
        const MT_Vector4 color = ef.Color(
            instruction.color_expr_index, instruction.color_value);
        command_buffer.AppendSetLightColor(
            target, color, sort_key, instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetLightPower: {
        const float power = ef.Float(
            instruction.float_expr_index, instruction.vector_value.x());
        command_buffer.AppendSetLightPower(
            target, power, sort_key, instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetLightShadow: {
        const bool use_shadow = ef.Bool(
            instruction.bool_expr_index, true);
        command_buffer.AppendSetLightShadow(
            target, use_shadow, sort_key, instruction.source_ref_index);
        break;
      }
      default:
        break;
    }
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectWindowCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  switch (instruction.opcode) {
    case LN_OpCode::SetWindowSize: {
      const int32_t width = std::max(
          1, ef.Int(instruction.int_expr_index, 1));
      const int32_t height = std::max(
          1, ef.Int(instruction.secondary_int_expr_index, 1));
      command_buffer.AppendSetWindowSize(width,
                                         height,
                                         ef.SortKey(),
                                         instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetFullscreen: {
      const bool fullscreen = ef.Bool(instruction.bool_expr_index, false);
      command_buffer.AppendSetFullscreen(fullscreen,
                                         ef.SortKey(),
                                         instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetVSync: {
      const int32_t mode = ef.Int(instruction.int_expr_index, VSYNC_OFF);
      command_buffer.AppendSetVSync(mode,
                                    ef.SortKey(),
                                    instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetShowFramerate: {
      const bool show = ef.Bool(instruction.bool_expr_index, false);
      command_buffer.AppendSetShowFramerate(show,
                                            ef.SortKey(),
                                            instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetShowProfile: {
      const bool show = ef.Bool(instruction.bool_expr_index, false);
      command_buffer.AppendSetShowProfile(show,
                                          ef.SortKey(),
                                          instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetCursorVisibility: {
      const bool visible = ef.Bool(instruction.bool_expr_index, true);
      command_buffer.AppendSetCursorVisibility(visible,
                                               ef.SortKey(),
                                               instruction.source_ref_index,
                                               instruction.int_value != 0);
      break;
    }
    case LN_OpCode::SetCursorPosition: {
      if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
        if (RAS_ICanvas *canvas = engine->GetCanvas()) {
          const MT_Vector3 position = ef.Vector(
              instruction.vector_expr_index, instruction.vector_value);
          const int32_t x = int32_t(
              std::lround(std::clamp(position.x(), 0.0f, 1.0f) * float(canvas->GetWidth())));
          const int32_t y = int32_t(
              std::lround(std::clamp(position.y(), 0.0f, 1.0f) * float(canvas->GetHeight())));
          command_buffer.AppendSetCursorPosition(
              x,
              y,
              ef.SortKey(),
              instruction.source_ref_index);
        }
      }
      break;
    }
    case LN_OpCode::SetPlayerGamepad:
      command_buffer.AppendSetPlayerGamepad(
          ef.Int(instruction.int_expr_index, 0),
          ef.Int(instruction.secondary_int_expr_index, 0),
          ef.SortKey(),
          instruction.source_ref_index);
      break;
    case LN_OpCode::SetGamepadVibration: {
      const int32_t gamepad_index = ef.Int(instruction.int_expr_index, 0);
      const MT_Vector3 vibration = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const float left = std::clamp(vibration.x(), 0.0f, 1.0f);
      const float right = std::clamp(vibration.y(), 0.0f, 1.0f);
      const uint32_t duration_ms = uint32_t(
          std::max<int32_t>(0, int32_t(std::lround(std::max(vibration.z(), 0.0f) * 1000.0f))));
      command_buffer.AppendSetGamepadVibration(
          gamepad_index,
          left,
          right,
          duration_ms,
          ef.SortKey(),
          instruction.source_ref_index);
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectInputMotionCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  if (instruction.opcode == LN_OpCode::MouseLook) {
    OpMouseLookCommand(ef, instruction, instruction_id);
    return;
  }

  KX_GameObject *body = ResolveObjectValue(
      ef.Value(instruction.value_expr_index, MakeNoneValue()));
  if (body == nullptr) {
    body = m_gameObject;
  }

  KX_GameObject *head = ResolveObjectValue(
      ef.Value(instruction.property_ref_index, MakeNoneValue()));
  if (head == nullptr) {
    head = body;
  }
  if (body == nullptr || head == nullptr) {
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }

  if (instruction.opcode == LN_OpCode::GamepadLook) {
    /* While a menu or a UI in the game world uses the gamepad's player, the view waits. */
    if (context.ui_input_users != 0 && instruction.secondary_int_expr_index != LN_INVALID_INDEX)
    {
      const int32_t user = LN_InputMap::Get().UserOfGamepad(
          ef.Int(instruction.secondary_int_expr_index, 0));
      if (user >= 0 && user < LN_MAX_GAMEPADS &&
          (context.ui_input_users & (1u << uint32_t(user))) != 0)
      {
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        return;
      }
    }
    MT_Vector3 stick = ef.Vector(
        instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f));
    if (stick.length2() <= (MT_EPSILON * MT_EPSILON)) {
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }

    const MT_Vector3 invert = ef.Vector(
        instruction.secondary_vector_expr_index,
        MT_Vector3(instruction.vector_value.x(), instruction.vector_value.y(), 0.0f));
    const MT_Vector3 cap_x = ef.Vector(
        instruction.color_expr_index,
        MT_Vector3(instruction.secondary_vector_value.x(),
                   instruction.secondary_vector_value.y(),
                   0.0f));
    const MT_Vector3 cap_y = ef.Vector(
        instruction.int_expr_index,
        MT_Vector3(instruction.color_value.x(), instruction.color_value.y(), 0.0f));
    const bool use_cap_x = ef.Bool(
        instruction.bool_expr_index, instruction.secondary_vector_value.z() > 0.5f);
    const bool use_cap_y = ef.Bool(
        instruction.secondary_bool_expr_index, instruction.color_value.w() > 0.5f);
    const float sensitivity = std::max(0.0f,
                                       ef.Float(instruction.float_expr_index,
                                                              instruction.vector_value.z()));
    const float exponent = std::max(0.0f,
                                    ef.Float(instruction.string_expr_index,
                                                           instruction.color_value.z()));

    /* The stick to the right turns right (clockwise seen from above), up looks up. */
    float yaw_delta = -stick.x();
    float pitch_delta = stick.y();
    if (std::fabs(yaw_delta) > MT_EPSILON) {
      yaw_delta = std::copysign(std::pow(std::fabs(yaw_delta), exponent), yaw_delta);
    }
    if (std::fabs(pitch_delta) > MT_EPSILON) {
      pitch_delta = std::copysign(std::pow(std::fabs(pitch_delta), exponent), pitch_delta);
    }

    if (std::fabs(invert.x()) > 0.5f) {
      yaw_delta = -yaw_delta;
    }
    if (std::fabs(invert.y()) > 0.5f) {
      pitch_delta = -pitch_delta;
    }

    const float tick_seconds = float(std::max(0.0, context.delta_time));
    yaw_delta *= sensitivity * tick_seconds;
    pitch_delta *= sensitivity * tick_seconds;
    if (std::fabs(yaw_delta) <= MT_EPSILON && std::fabs(pitch_delta) <= MT_EPSILON) {
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }

    MT_Vector3 body_euler = OrientationToEuler(body->NodeGetLocalOrientation());
    MT_Vector3 head_euler = (head == body) ? body_euler :
                                             OrientationToEuler(head->NodeGetLocalOrientation());

    body_euler[2] += yaw_delta;
    if (use_cap_x) {
      const float min_yaw = std::min(cap_x.x(), cap_x.y());
      const float max_yaw = std::max(cap_x.x(), cap_x.y());
      body_euler[2] = std::clamp(body_euler[2], min_yaw, max_yaw);
    }

    head_euler[0] += pitch_delta;
    if (use_cap_y) {
      const float min_pitch = std::min(cap_y.x(), cap_y.y());
      const float max_pitch = std::max(cap_y.x(), cap_y.y());
      head_euler[0] = std::clamp(head_euler[0], min_pitch, max_pitch);
    }

    if (head == body) {
      body_euler[0] = head_euler[0];
      command_buffer.AppendSetLocalOrientation(
          body,
          body_euler,
          ef.SortKey(),
          instruction.source_ref_index);
    }
    else {
      command_buffer.AppendSetLocalOrientation(
          body,
          body_euler,
          ef.SortKey(),
          instruction.source_ref_index);
      command_buffer.AppendSetLocalOrientation(
          head,
          head_euler,
          ef.SortKey(),
          instruction.source_ref_index);
    }

    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }

  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectAudioCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  switch (instruction.opcode) {
    case LN_OpCode::StopAllSounds: {
      command_buffer.AppendStopAllSounds(ef.SortKey(),
                                         instruction.source_ref_index);
      break;
    }
    case LN_OpCode::PlaySound: {
      const std::string sound_name = ef.String(
          instruction.string_expr_index, std::string());
      if (sound_name.empty()) {
        return;
      }
      const float volume = ef.Float(
          instruction.float_expr_index, instruction.vector_value.x());
      const float pitch = ef.Float(
          instruction.secondary_float_expr_index, instruction.vector_value.y());
      const bool loop = ef.Bool(
          instruction.bool_expr_index, instruction.bool_value);
      command_buffer.AppendPlaySound(sound_name,
                                     volume,
                                     pitch,
                                     loop,
                                     ef.SortKey(),
                                     instruction.source_ref_index,
                                     instruction.secondary_int_value,
                                     m_gameObject);
      break;
    }
    case LN_OpCode::SetSoundVolume: {
      const float volume = ef.Float(instruction.float_expr_index, 1.0f);
      const float fade = ef.Float(
          instruction.secondary_float_expr_index, 0.0f);
      command_buffer.AppendSetSoundVolume(instruction.int_value,
                                          volume,
                                          fade,
                                          ef.SortKey(),
                                          instruction.source_ref_index);
      break;
    }
    case LN_OpCode::StopSound: {
      const std::string sound_name = ef.String(
          instruction.string_expr_index, std::string());
      if (sound_name.empty()) {
        return;
      }
      command_buffer.AppendStopSound(sound_name,
                                     ef.SortKey(),
                                     instruction.source_ref_index);
      break;
    }
    case LN_OpCode::PlaySound3D: {
      KX_GameObject *speaker = nullptr;
      if (instruction.value_expr_index != LN_INVALID_INDEX) {
        const LN_Value speaker_value = ef.Value(
            instruction.value_expr_index, MakeNoneValue());
        speaker = ResolveObjectValue(speaker_value);
      }
      if (speaker == nullptr) {
        speaker = m_gameObject;
      }
      /* No Sound picked (an empty name): the Speaker object's own, with its settings, found when
       * the sound starts (LN_CommandBuffer::SpeakerPlayOf). */
      const std::string sound_name = ef.String(instruction.string_expr_index, std::string());
      const float volume = ef.Float(instruction.float_expr_index, 1.0f);
      const float pitch = ef.Float(instruction.secondary_float_expr_index, 1.0f);
      const bool loop = ef.Bool(instruction.bool_expr_index, instruction.bool_value);
      command_buffer.AppendPlaySound3D(
          speaker,
          sound_name,
          volume,
          pitch,
          loop,
          ef.SortKey(),
          instruction.source_ref_index,
          instruction.int_value,
          ef.Int(instruction.secondary_int_expr_index, 0),
          instruction.secondary_int_value);
      break;
    }
    case LN_OpCode::PauseSound: {
      const std::string sound_name = ef.String(
          instruction.string_expr_index, std::string());
      if (!sound_name.empty()) {
        command_buffer.AppendPauseSound(sound_name,
                                        ef.SortKey(),
                                        instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::ResumeSound: {
      const std::string sound_name = ef.String(
          instruction.string_expr_index, std::string());
      if (!sound_name.empty()) {
        command_buffer.AppendResumeSound(sound_name,
                                         ef.SortKey(),
                                         instruction.source_ref_index);
      }
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectActionCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  KX_GameObject *target_object = ef.ResolveCommandTarget(instruction);
  if (target_object == nullptr && instruction.opcode != LN_OpCode::PlayAction) {
    return;
  }

  const uint64_t sort_key = ef.SortKey();
  switch (instruction.opcode) {
    case LN_OpCode::PlayAction: {
      const std::string action_name = ef.String(
          instruction.string_expr_index, std::string());
      const float start_frame = ef.Float(
          instruction.float_expr_index, instruction.vector_value.x());
      const float end_frame = ef.Float(
          instruction.secondary_float_expr_index, instruction.vector_value.y());
      const float blendin = ef.Float(
          instruction.tertiary_float_expr_index, instruction.vector_value.z());
      const float playback_speed = ef.Float(
          instruction.quaternary_float_expr_index,
          instruction.secondary_vector_value.y());
      const float layer_weight = instruction.secondary_vector_value.x();
      const int32_t layer = ef.Int(instruction.int_expr_index, 0);
      const int32_t priority = ef.Int(
          instruction.secondary_int_expr_index, 0);
      command_buffer.AppendPlayAction(target_object,
                                      this,
                                      action_name,
                                      start_frame,
                                      end_frame,
                                      layer,
                                      priority,
                                      blendin,
                                      layer_weight,
                                      uint32_t(instruction.int_value),
                                      playback_speed,
                                      instruction.property_ref_index,
                                      uint8_t(instruction.secondary_int_value),
                                      sort_key,
                                      instruction.source_ref_index);
      break;
    }
    case LN_OpCode::StopAction: {
      const int32_t layer = ef.Int(
          instruction.int_expr_index, instruction.int_value);
      command_buffer.AppendStopAction(
          target_object, layer, sort_key, instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetActionFrame: {
      const int32_t layer = ef.Int(
          instruction.int_expr_index, instruction.int_value);
      const float frame = ef.Float(
          instruction.float_expr_index, instruction.vector_value.x());
      command_buffer.AppendSetActionFrame(
          target_object, layer, frame, sort_key, instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetShapeKeyValue: {
      const std::string shape_key_name = ef.String(
          instruction.string_expr_index, std::string());
      if (shape_key_name.empty()) {
        return;
      }
      const float value = ef.Float(
          instruction.float_expr_index, instruction.vector_value.x());
      command_buffer.AppendSetShapeKeyValue(
          target_object, shape_key_name, value, sort_key, instruction.source_ref_index);
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectBoneCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  const uint64_t sort_key = ef.SortKey();
  switch (instruction.opcode) {
    case LN_OpCode::SetBonePoseLocation: {
      const MT_Vector3 location = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const std::string bone_name = ef.String(
          instruction.string_expr_index, std::string());
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        if (!bone_name.empty()) {
          command_buffer.AppendSetBonePoseLocation(target,
                                                   bone_name,
                                                   location,
                                                   sort_key,
                                                   instruction.source_ref_index,
                                                   instruction.int_value,
                                                   instruction.secondary_int_value != 0);
        }
      }
      break;
    }
    case LN_OpCode::SetBonePoseTransform: {
      const MT_Vector3 location = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const LN_Rotation rotation = ef.Rotation(
          instruction.secondary_vector_expr_index,
          LN_Rotation::FromEulerXYZ(instruction.secondary_vector_value));
      const std::string bone_name = ef.String(
          instruction.string_expr_index, std::string());
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        if (!bone_name.empty()) {
          command_buffer.AppendSetBonePoseTransform(target,
                                                    bone_name,
                                                    location,
                                                    rotation,
                                                    sort_key,
                                                    instruction.source_ref_index,
                                                    instruction.int_value,
                                                    instruction.secondary_int_value != 0);
        }
      }
      break;
    }
    case LN_OpCode::SetBonePoseRotation: {
      const LN_Rotation rotation = ef.Rotation(
          instruction.vector_expr_index,
          LN_Rotation::FromEulerXYZ(instruction.vector_value));
      const std::string bone_name = ef.String(
          instruction.string_expr_index, std::string());
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        if (!bone_name.empty()) {
          command_buffer.AppendSetBonePoseRotation(target,
                                                   bone_name,
                                                   rotation,
                                                   sort_key,
                                                   instruction.source_ref_index,
                                                   instruction.secondary_int_value != 0,
                                                   instruction.int_value);
        }
      }
      break;
    }
    case LN_OpCode::SetBonePoseScale: {
      const MT_Vector3 scale = ef.Vector(
          instruction.vector_expr_index, instruction.vector_value);
      const std::string bone_name = ef.String(
          instruction.string_expr_index, std::string());
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        if (!bone_name.empty()) {
          command_buffer.AppendSetBonePoseScale(
              target, bone_name, scale, sort_key, instruction.source_ref_index);
        }
      }
      break;
    }
    case LN_OpCode::SetBoneAttribute: {
      const std::string bone_name = ef.String(
          instruction.string_expr_index, std::string());
      KX_GameObject *target = ef.ResolveCommandTarget(instruction);
      LN_Value value = MakeNoneValue();
      if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
        value = ef.Value(instruction.secondary_value_expr_index, value);
      }
      if (target != nullptr && !bone_name.empty()) {
        command_buffer.AppendSetBoneAttribute(target,
                                              bone_name,
                                              instruction.int_value,
                                              instruction.secondary_int_value,
                                              value,
                                              sort_key,
                                              instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::SetBoneConstraintInfluence: {
      const std::string bone_name = ef.String(
          instruction.string_expr_index, std::string());
      const std::string constraint_name = ef.String(
          instruction.secondary_string_expr_index, std::string());
      const float influence = ef.Float(
          instruction.float_expr_index, 0.0f);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        if (!bone_name.empty() && !constraint_name.empty()) {
          command_buffer.AppendSetBoneConstraintInfluence(target,
                                                          bone_name,
                                                          constraint_name,
                                                          influence,
                                                          sort_key,
                                                          instruction.source_ref_index);
        }
      }
      break;
    }
    case LN_OpCode::SetBoneConstraintTarget: {
      const std::string bone_name = ef.String(
          instruction.string_expr_index, std::string());
      const std::string constraint_name = ef.String(
          instruction.secondary_string_expr_index, std::string());
      const LN_Value target_value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        if (!bone_name.empty() && !constraint_name.empty()) {
          command_buffer.AppendSetBoneConstraintTarget(this,
                                                       target,
                                                       bone_name,
                                                       constraint_name,
                                                       target_value,
                                                       sort_key,
                                                       instruction.source_ref_index);
        }
      }
      break;
    }
    case LN_OpCode::SetBoneConstraintAttribute: {
      const std::string bone_name = ef.String(
          instruction.string_expr_index, std::string());
      const std::string constraint_name = ef.String(
          instruction.secondary_string_expr_index, std::string());
      const std::string attribute_name = ef.String(
          instruction.tertiary_string_expr_index, std::string());
      const LN_Value value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        if (!bone_name.empty() && !constraint_name.empty() && !attribute_name.empty()) {
          command_buffer.AppendSetBoneConstraintAttribute(this,
                                                          target,
                                                          bone_name,
                                                          constraint_name,
                                                          attribute_name,
                                                          value,
                                                          sort_key,
                                                          instruction.source_ref_index);
        }
      }
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectMaterialCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  const uint64_t sort_key = ef.SortKey();
  switch (instruction.opcode) {
    case LN_OpCode::SetMaterialSlot: {
      const LN_Value material_value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      const int32_t slot = ef.Int(instruction.int_expr_index, 0);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        if (slot >= 0) {
          command_buffer.AppendAssignMaterialToSlot(
              this, target, material_value, slot, sort_key, instruction.source_ref_index);
        }
      }
      break;
    }
    case LN_OpCode::SetMaterialNodeSocketValue: {
      const std::string node_name = ef.String(
          instruction.string_expr_index, std::string());
      const std::string socket_name = ef.String(
          instruction.secondary_string_expr_index, std::string());
      const LN_Value value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      if (!node_name.empty() && !socket_name.empty()) {
        if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
          const LN_Value material_value = ef.Value(
              instruction.tertiary_value_expr_index, MakeNoneValue());
          if (material_value.exists) {
            command_buffer.AppendSetMaterialNodeSocketValue(this,
                                                            material_value,
                                                            node_name,
                                                            socket_name,
                                                            value,
                                                            sort_key,
                                                            instruction.source_ref_index);
          }
        }
        else if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
          const int32_t slot = ef.Int(instruction.int_expr_index, 0);
          command_buffer.AppendSetMaterialNodeSocketValue(this,
                                                          target,
                                                          MakeNoneValue(),
                                                          slot,
                                                          node_name,
                                                          socket_name,
                                                          value,
                                                          sort_key,
                                                          instruction.source_ref_index,
                                                          instruction.secondary_int_value == 0);
        }
      }
      break;
    }
    case LN_OpCode::SetMaterialParameter: {
      const std::string node_name = ef.String(
          instruction.string_expr_index, std::string());
      const std::string socket_name = ef.String(
          instruction.secondary_string_expr_index, std::string());
      const LN_Value value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      if (!node_name.empty() && !socket_name.empty()) {
        if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
          const LN_Value material_value = ef.Value(
              instruction.tertiary_value_expr_index, MakeNoneValue());
          if (material_value.exists) {
            command_buffer.AppendSetMaterialParameter(this,
                                                      material_value,
                                                      node_name,
                                                      socket_name,
                                                      value,
                                                      sort_key,
                                                      instruction.source_ref_index);
          }
        }
        else if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
          const int32_t slot = ef.Int(instruction.int_expr_index, 0);
          command_buffer.AppendSetMaterialParameter(this,
                                                    target,
                                                    MakeNoneValue(),
                                                    slot,
                                                    node_name,
                                                    socket_name,
                                                    value,
                                                    sort_key,
                                                    instruction.source_ref_index);
        }
      }
      break;
    }
    case LN_OpCode::SetGeometryNodesInput: {
      const std::string modifier_name = ef.String(
          instruction.string_expr_index, std::string());
      const std::string input_identifier = ef.String(
          instruction.secondary_string_expr_index, std::string());
      const LN_Value value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      if (!modifier_name.empty() && !input_identifier.empty()) {
        if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
          command_buffer.AppendSetGeometryNodesInput(this,
                                                     target,
                                                     modifier_name,
                                                     input_identifier,
                                                     value,
                                                     sort_key,
                                                     instruction.source_ref_index);
        }
      }
      break;
    }
    case LN_OpCode::SetGeometryNodeSocketValue: {
      const std::string modifier_name = ef.String(
          instruction.string_expr_index, std::string());
      const std::string node_name = ef.String(
          instruction.secondary_string_expr_index, std::string());
      const std::string socket_identifier = ef.String(
          instruction.tertiary_string_expr_index, std::string());
      const LN_Value value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      if (!modifier_name.empty() && !node_name.empty() && !socket_identifier.empty()) {
        if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
          command_buffer.AppendSetGeometryNodeSocketValue(this,
                                                          target,
                                                          modifier_name,
                                                          node_name,
                                                          socket_identifier,
                                                          value,
                                                          sort_key,
                                                          instruction.source_ref_index);
        }
      }
      break;
    }
    case LN_OpCode::SetCompositorNodeSocketValue: {
      const std::string target_name = ef.String(
          instruction.string_expr_index, std::string());
      const std::string node_name = ef.String(
          instruction.secondary_string_expr_index, std::string());
      const std::string socket_identifier = ef.String(
          instruction.tertiary_string_expr_index, std::string());
      const LN_Value value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      if (!target_name.empty() && !node_name.empty() && !socket_identifier.empty()) {
        command_buffer.AppendSetCompositorNodeSocketValue(this,
                                                          int16_t(instruction.int_value),
                                                          target_name,
                                                          node_name,
                                                          socket_identifier,
                                                          value,
                                                          sort_key,
                                                          instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::MakeNodeTreeUnique: {
      const std::string target_name = ef.String(
          instruction.string_expr_index, std::string());
      const int32_t slot = ef.Int(instruction.int_expr_index, 0);
      KX_GameObject *target = nullptr;
      if (instruction.int_value != 3) {
        target = ef.ResolveCommandTarget(instruction);
      }
      if (instruction.int_value == 3 || target != nullptr) {
        command_buffer.AppendMakeNodeTreeUnique(target,
                                                instruction.int_value,
                                                slot,
                                                target_name,
                                                sort_key,
                                                instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::SetNodeMute: {
      const std::string target_name = ef.String(
          instruction.string_expr_index, std::string());
      const std::string node_name = ef.String(
          instruction.secondary_string_expr_index, std::string());
      const bool muted = ef.Bool(instruction.bool_expr_index, true);
      if (!target_name.empty() && !node_name.empty()) {
        command_buffer.AppendSetNodeMute(this,
                                         int16_t(instruction.int_value),
                                         target_name,
                                         node_name,
                                         muted,
                                         sort_key,
                                         instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::AssignGeometryNodesModifier: {
      LN_Value result;
      result.type = LN_ValueType::Int;
      result.exists = true;
      result.int_value = 0;
      if (instruction.property_ref_index != LN_INVALID_INDEX) {
        SetTreePropertyValue(instruction.property_ref_index, result);
      }
      const LN_Value node_group_value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      const std::string name = ef.String(
          instruction.string_expr_index, std::string());
      const int32_t index_or_id = ef.Int(instruction.int_expr_index, 0);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        result.int_value = command_buffer.AssignGeometryNodesModifierNow(
            this,
            target,
            node_group_value,
            static_cast<LN_CommandBuffer::ModifierAssignmentOperation>(instruction.int_value),
            static_cast<LN_CommandBuffer::ModifierTarget>(instruction.secondary_int_value),
            name,
            index_or_id);
        if (instruction.property_ref_index != LN_INVALID_INDEX) {
          SetTreePropertyValue(instruction.property_ref_index, result);
        }
      }
      break;
    }
    case LN_OpCode::EnableDisableModifier: {
      const auto target_mode = static_cast<LN_CommandBuffer::ModifierTarget>(
          instruction.int_value);
      const std::string modifier_name =
          target_mode == LN_CommandBuffer::ModifierTarget::Name ?
              ef.String(instruction.string_expr_index, std::string()) :
              std::string();
      const int32_t index = ELEM(target_mode,
                                 LN_CommandBuffer::ModifierTarget::Index,
                                 LN_CommandBuffer::ModifierTarget::PersistentId) ?
                                ef.Int(instruction.int_expr_index, 0) :
                                0;
      const bool enabled = ef.Bool(instruction.bool_expr_index, true);
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        command_buffer.AppendEnableDisableModifier(target,
                                                   target_mode,
                                                   modifier_name,
                                                   index,
                                                   enabled,
                                                   sort_key,
                                                   instruction.source_ref_index);
      }
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectGlobalCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  switch (instruction.opcode) {
    case LN_OpCode::Print: {
      const std::vector<LN_ValueExpression> &value_expressions =
          m_program->GetValueExpressions();
      const LN_ValueExpression *value_expression =
          instruction.value_expr_index < value_expressions.size() ?
              &value_expressions[instruction.value_expr_index] :
              nullptr;
      const std::vector<LN_TreePropertyRef> &tree_property_refs =
          m_program->GetTreePropertyRefs();
      const LN_TreePropertyRef *tree_property_ref = nullptr;
      if (value_expression != nullptr &&
          value_expression->property_ref_index < tree_property_refs.size())
      {
        tree_property_ref = &tree_property_refs[value_expression->property_ref_index];
      }
      const bool print_deferred_add_object_result =
          value_expression != nullptr &&
          value_expression->kind == LN_ValueExpressionKind::RuntimeTreeProperty &&
          tree_property_ref != nullptr &&
          tree_property_ref->name.rfind("__native_add_object_result_", 0) == 0;
      if (print_deferred_add_object_result && m_activeTreePropertySnapshot == nullptr) {
        command_buffer.AppendPrintTreeProperty(
            this,
            value_expression->property_ref_index,
            ef.SortKey(),
            instruction.source_ref_index);
      }
      else {
        const LN_Value value = ef.Value(
            instruction.value_expr_index, MakeNoneValue());
        const std::string message = ValueToPrintString(value);
        command_buffer.AppendPrint(message,
                                   ef.SortKey(),
                                   instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::QuitGame:
      command_buffer.AppendQuitGame(ef.SortKey(),
                                    instruction.source_ref_index);
      break;
    case LN_OpCode::RestartGame:
      command_buffer.AppendRestartGame(ef.SortKey(),
                                       instruction.source_ref_index);
      break;
    case LN_OpCode::SetTimeScale: {
      const float timescale = ef.Float(
          instruction.float_expr_index, 1.0f);
      command_buffer.AppendSetTimeScale(m_gameObject,
                                        timescale,
                                        ef.SortKey(),
                                        instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetGamePaused: {
      const bool paused = ef.Bool(instruction.bool_expr_index, true);
      command_buffer.AppendSetGamePaused(paused,
                                         ef.SortKey(),
                                         instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetPhysicsPaused: {
      const bool paused = ef.Bool(instruction.bool_expr_index, true);
      command_buffer.AppendSetPhysicsPaused(paused,
                                            ef.SortKey(),
                                            instruction.source_ref_index);
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectFileCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  switch (instruction.opcode) {
    case LN_OpCode::LoadBlendFile: {
      const std::string filepath = ef.String(
          instruction.string_expr_index, std::string());
      command_buffer.AppendLoadBlendFile(filepath,
                                         ef.SortKey(),
                                         instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SaveGame: {
      const int32_t slot = ef.Int(instruction.int_expr_index, 0);
      const std::string path = ef.String(
          instruction.string_expr_index, std::string());
      command_buffer.AppendSaveGame(this,
                                    m_gameObject,
                                    slot,
                                    path,
                                    instruction.bool_expr_index,
                                    ef.SortKey(),
                                    instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetSaveExistence: {
      KX_GameObject *target = ef.ResolveCommandTarget(instruction);
      const bool enabled = ef.Bool(instruction.bool_expr_index, true);
      command_buffer.AppendSetSaveExistence(this,
                                            m_gameObject,
                                            target,
                                            enabled,
                                            instruction.secondary_bool_expr_index,
                                            ef.SortKey(),
                                            instruction.source_ref_index);
      break;
    }
    case LN_OpCode::LoadGame: {
      const int32_t slot = ef.Int(instruction.int_expr_index, 0);
      const std::string path = ef.String(
          instruction.string_expr_index, std::string());
      command_buffer.AppendLoadGame(this,
                                    m_gameObject,
                                    slot,
                                    path,
                                    instruction.bool_expr_index,
                                    ef.SortKey(),
                                    instruction.source_ref_index);
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectSceneCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  switch (instruction.opcode) {
    case LN_OpCode::LoadScene: {
      const std::string scene_name = ef.String(
          instruction.string_expr_index, std::string());
      command_buffer.AppendLoadScene(m_gameObject,
                                     scene_name,
                                     ef.SortKey(),
                                     instruction.source_ref_index);
      break;
    }
    case LN_OpCode::SetScene: {
      const std::string scene_name = ef.String(
          instruction.string_expr_index, std::string());
      command_buffer.AppendSetScene(m_gameObject,
                                    scene_name,
                                    ef.SortKey(),
                                    instruction.source_ref_index);
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectLifecycleCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  switch (instruction.opcode) {
    case LN_OpCode::AddObject: {
      auto clear_add_object_result = [&]() {
        if (instruction.property_ref_index != LN_INVALID_INDEX) {
          LN_Value empty_result;
          empty_result.type = LN_ValueType::ObjectRef;
          SetTreePropertyValue(instruction.property_ref_index, empty_result);
        }
      };

      KX_GameObject *reference_object = ef.ResolveCommandTarget(instruction);
      KX_GameObject *source_object = ef.ResolveAddObjectTemplate(
          instruction.secondary_value_expr_index);
      if (reference_object != nullptr && source_object != nullptr) {
        command_buffer.FlushPendingObjectTransformCommands(reference_object);
        command_buffer.ExecuteAddObjectFromRefImmediate(
            this,
            reference_object,
            MakeObjectRef(source_object, source_object->GetName()),
            ef.Float(instruction.float_expr_index, 0.0f),
            ef.Bool(instruction.bool_expr_index, false),
            ef.Bool(instruction.secondary_bool_expr_index, false),
            instruction.property_ref_index,
            instruction.source_ref_index);
      }
      else {
        clear_add_object_result();
      }
      break;
    }
    case LN_OpCode::RemoveParent: {
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        command_buffer.AppendRemoveParent(target,
                                          ef.SortKey(),
                                          instruction.source_ref_index);
      }
      break;
    }
    case LN_OpCode::RemoveObject: {
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        command_buffer.AppendRemoveObject(target,
                                          ef.SortKey(),
                                          instruction.source_ref_index);
      }
      break;
    }
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectObjectReferenceCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  switch (instruction.opcode) {
    case LN_OpCode::SetParent: {
      KX_GameObject *child_object = ef.ResolveCommandTarget(instruction);
      if (child_object == nullptr || instruction.secondary_value_expr_index == LN_INVALID_INDEX)
      {
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        break;
      }

      const LN_Value parent_value = ef.Value(
          instruction.secondary_value_expr_index, MakeNoneValue());
      KX_GameObject *parent_object = ResolveObjectValue(parent_value);
      if (parent_object != nullptr) {
        LN_RuntimeRef parent_ref = parent_value.runtime_ref;
        if (parent_ref.kind != LN_RuntimeRefKind::Object ||
            ResolveObjectRef(parent_ref) != parent_object)
        {
          parent_ref = MakeObjectRef(parent_object, parent_value.reference_name);
        }
        if (instruction.bool_value) {
          KX_ParentOptions options;
          options.type = static_cast<KX_ParentType>(instruction.int_value);

          if (options.type == KX_ParentType::Object) {
            options.compound = ef.Bool(
                instruction.bool_expr_index, false);
          }
          else if (options.type == KX_ParentType::Bone) {
            options.bone_name = ef.String(
                instruction.string_expr_index, std::string());
          }

          command_buffer.AppendSetParentFromRef(
              this,
              child_object,
              parent_ref,
              options,
              ef.SortKey(),
              instruction.source_ref_index);
        }
        else {
          command_buffer.AppendSetParentFromRef(
              this,
              child_object,
              parent_ref,
              ef.Bool(instruction.bool_expr_index, true),
              ef.Bool(instruction.secondary_bool_expr_index, true),
              ef.SortKey(),
              instruction.source_ref_index);
        }
      }
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      break;
    }
    case LN_OpCode::ReplaceMesh: {
      if (KX_GameObject *target = ef.ResolveCommandTarget(instruction)) {
        KX_GameObject *mesh_obj = nullptr;
        if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
          const LN_Value mesh_val = ef.Value(
              instruction.secondary_value_expr_index, MakeNoneValue());
          /* A mesh library may sit in a hidden collection (inactive objects). */
          mesh_obj = ResolveObjectValue(mesh_val, true);
        }
        if (mesh_obj) {
          command_buffer.AppendReplaceMesh(target,
                                           mesh_obj,
                                           ef.SortKey(),
                                           instruction.source_ref_index);
        }
      }
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      break;
    }
    case LN_OpCode::CopyProperty: {
      /* No Source input: the owner. A linked Source that gives no object copies nothing. */
      KX_GameObject *source = m_gameObject;
      if (instruction.value_expr_index != LN_INVALID_INDEX) {
        source = ResolveObjectValue(
            ef.Value(instruction.value_expr_index, MakeNoneValue()));
      }
      KX_GameObject *target = nullptr;
      if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
        const LN_Value tgt_val = ef.Value(
            instruction.secondary_value_expr_index, MakeNoneValue());
        target = ResolveObjectValue(tgt_val);
      }
      const std::string property = ef.String(
          instruction.string_expr_index, std::string());
      if (source && target && !property.empty()) {
        command_buffer.AppendCopyProperty(source,
                                          target,
                                          property,
                                          ef.SortKey(),
                                          instruction.source_ref_index);
      }
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      break;
    }
    case LN_OpCode::EditGameProperty: {
      /* A list or dictionary node in Game Property mode: the change is recorded and made on the
       * live property at the flush, after this step's earlier writes of it (LN_PropertyEditOp).
       * No Object input: the owner; a linked one that gives no object changes nothing. Reads in
       * socket order. */
      KX_GameObject *target = m_gameObject;
      if (instruction.value_expr_index != LN_INVALID_INDEX) {
        target = ResolveObjectValue(ef.Value(instruction.value_expr_index, MakeNoneValue()));
      }
      const std::string property = ef.String(instruction.string_expr_index, std::string());
      const int32_t index = ef.Int(instruction.int_expr_index, 0);
      const std::string key = ef.String(instruction.secondary_string_expr_index, std::string());
      const LN_Value value = instruction.secondary_value_expr_index != LN_INVALID_INDEX ?
                                 ef.Value(instruction.secondary_value_expr_index,
                                          MakeNoneValue()) :
                                 MakeNoneValue();
      float amount = 0.0f;
      if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
        ValueAsFloat(ef.Value(instruction.tertiary_value_expr_index, MakeNoneValue()), amount);
      }
      if (target != nullptr && !property.empty()) {
        command_buffer.AppendEditGameProperty(this,
                                              target,
                                              property,
                                              LN_PropertyEditOp(instruction.int_value),
                                              value,
                                              index,
                                              key,
                                              amount,
                                              ef.SortKey(),
                                              instruction.source_ref_index);
      }
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      break;
    }
    default:
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      break;
  }
}

template<class Frame>
void LN_RuntimeTree::OpDirectTreePropertyCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  if (instruction.property_ref_index == LN_INVALID_INDEX || m_program == nullptr ||
      instruction.property_ref_index >= m_program->GetTreePropertyRefs().size())
  {
    return;
  }

  const std::vector<LN_TreePropertyRef> &property_refs = m_program->GetTreePropertyRefs();
  LN_Value value = ef.Value(
      instruction.value_expr_index,
      property_refs[instruction.property_ref_index].default_value);
  if (instruction.value_expr_index == LN_INVALID_INDEX) {
    value = property_refs[instruction.property_ref_index].default_value;
  }

  /* (Plan C2 P4: recorded first, then moved into the property: one copy fewer.) */
  command_buffer.AppendSetTreeProperty(
      this,
      instruction.property_ref_index,
      value,
      ef.SortKey(),
      instruction.source_ref_index,
      m_program->TreePropertyOnlySetBySetTreeProperty(instruction.property_ref_index));
  SetTreePropertyValue(instruction.property_ref_index, std::move(value));
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectTreeControlCommand(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  KX_GameObject *target_object = ef.ResolveCommandTarget(instruction);
  const std::string tree_name = ef.String(
      instruction.string_expr_index, std::string());
  if (target_object == nullptr || tree_name.empty()) {
    return;
  }

  KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
  if (scene == nullptr || scene->GetLogicNodeManager() == nullptr) {
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
    return;
  }

  switch (instruction.opcode) {
    case LN_OpCode::SetLogicTreeEnabled:
      command_buffer.AppendSetLogicTreeEnabled(
          target_object,
          tree_name,
          instruction.bool_value,
          ef.SortKey(),
          instruction.source_ref_index);
      break;
    case LN_OpCode::InstallLogicTree:
      command_buffer.AppendInstallLogicTree(
          target_object,
          tree_name,
          ef.Bool(instruction.bool_expr_index, instruction.bool_value),
          ef.SortKey(),
          instruction.source_ref_index);
      break;
    case LN_OpCode::RunLogicTreeOnce:
      if (LN_Manager *manager = scene->GetLogicNodeManager()) {
        manager->QueueRunLogicTreeOnce(target_object, tree_name);
      }
      break;
    default:
      break;
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirectSendEvent(Frame &ef,
                                   const LN_Instruction &instruction,
                                   const uint32_t instruction_id)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;

  const LN_StringId subject_id = ResolveStringExpressionId(instruction.string_expr_index);
  std::string subject;
  if (!subject_id.IsValid()) {
    subject = ef.String(instruction.string_expr_index, std::string());
  }
  else if (m_program->GetString(subject_id).empty()) {
    return;
  }
  if (!subject_id.IsValid() && subject.empty()) {
    return;
  }

  LN_Value content;
  if (instruction.value_expr_index != LN_INVALID_INDEX) {
    content = ef.Value(instruction.value_expr_index, MakeNoneValue());
  }
  KX_GameObject *messenger = m_gameObject;
  if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
    const LN_Value messenger_val = ef.Value(
        instruction.secondary_value_expr_index, MakeNoneValue());
    if (KX_GameObject *resolved_messenger = ResolveObjectValue(messenger_val)) {
      messenger = resolved_messenger;
    }
  }
  KX_GameObject *target = nullptr;
  if (instruction.int_expr_index != LN_INVALID_INDEX) {
    const LN_Value target_val = ef.Value(
        instruction.int_expr_index, MakeNoneValue());
    target = ResolveObjectValue(target_val);
    if (target == nullptr) {
      return;
    }
  }
  if (subject_id.IsValid()) {
    command_buffer.AppendSendEvent(this,
                                   subject_id,
                                   content,
                                   messenger,
                                   target,
                                   ef.SortKey(),
                                   instruction.source_ref_index);
  }
  else {
    command_buffer.AppendSendEvent(subject,
                                   content,
                                   messenger,
                                   target,
                                   ef.SortKey(),
                                   instruction.source_ref_index,
                                   this);
  }
  MarkInstructionExecuted(event, instruction_id, context.tick_index);
}

template<class Frame>
void LN_RuntimeTree::OpDirect(Frame &ef, const LN_ExecOp &op)
{
  const LN_Event event = ef.event;
  LN_TickContext &context = ef.context;
  LN_CommandBuffer &command_buffer = ef.command_buffer;
  const std::vector<LN_InstructionHeader> &instructions = ef.instructions;

  if (op.instruction_index >= instructions.size()) {
    return;
  }
  if (m_uiEventRoutesOnly && !op.ui_event_route) {
    return;
  }

  const LN_Instruction &instruction = m_program->GetInstructionPayload(
      event, instructions[op.instruction_index]);
  const uint32_t instruction_id = op.instruction_index;
  LN_ScopedCommandQueryContext command_query_context(
      context, command_buffer, NextCommandExecutionKey());
  ProfileDirectExecOp(op);

  const std::vector<LN_Value> *guard_tree_properties = nullptr;
  if (op.guard_bool_expr_index != LN_INVALID_INDEX) {
    if (!ef.Bool(op.guard_bool_expr_index, false)) {
      return;
    }
    guard_tree_properties = DelayTreePropertySnapshotForGuard(op.guard_bool_expr_index,
                                                              context.tick_index);
  }

  const std::vector<LN_Value> *previous_tree_property_snapshot = m_activeTreePropertySnapshot;
  if (guard_tree_properties != nullptr) {
    m_activeTreePropertySnapshot = guard_tree_properties;
  }

  switch (op.kind) {
    case LN_ExecOpKind::Nop:
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      break;
    case LN_ExecOpKind::BranchRoute: {
      const bool condition = instruction.bool_expr_index == LN_INVALID_INDEX ?
                                 false :
                                 ef.Bool(
                                     instruction.bool_expr_index, false);
      if (condition == instruction.bool_value) {
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
      }
      break;
    }
    case LN_ExecOpKind::TimeFlowControl:
      OpTimeFlowControlInstruction(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::VectorCommand:
      OpDirectVectorCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::PhysicsCommand:
      OpDirectPhysicsCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::ObjectStateCommand:
      OpDirectObjectStateCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::ObjectColorCommand:
      OpDirectObjectColorCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::CameraCommand:
      OpDirectCameraCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::LightCommand:
      OpDirectLightCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::WindowCommand:
      OpDirectWindowCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::InputMotionCommand:
      OpDirectInputMotionCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::AudioCommand:
      OpDirectAudioCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::ActionCommand:
      OpDirectActionCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::BoneCommand:
      OpDirectBoneCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::MaterialCommand:
      OpDirectMaterialCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::GlobalCommand:
      OpDirectGlobalCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::FileCommand:
      OpDirectFileCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::SceneCommand:
      OpDirectSceneCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::LifecycleCommand:
      OpDirectLifecycleCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::ObjectReferenceCommand:
      OpDirectObjectReferenceCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::GamePropertyCommand:
      OpDirectGamePropertyCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::TreePropertyCommand:
      OpDirectTreePropertyCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::TreeControlCommand:
      OpDirectTreeControlCommand(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::SendEvent:
      OpDirectSendEvent(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::GlobalPropertyCommand:
      OpGlobalPropertyInstruction(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::VariableCommand:
      OpVariableInstruction(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::NavigationCommand:
      OpNavigationInstruction(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::SceneCollectionCommand:
      OpSceneCollectionInstruction(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::DebugDrawCommand:
      OpDebugDrawInstruction(ef, instruction, instruction_id);
      break;
    case LN_ExecOpKind::PhysicsConstraintCommand:
      OpPhysicsConstraintInstruction(ef, instruction, instruction_id);
      break;
  }

  m_activeTreePropertySnapshot = previous_tree_property_snapshot;
}

/** One op of the lean form: the generic handler, or a native one (LN_LeanNative). */
template<class Frame>
void LN_RuntimeTree::OpLean(Frame &ef, const LN_ExecOp &op, const LN_LeanOp &lean_op)
{
  /* A guard that is a Delay's Done may swap the tree property snapshot the op reads through
   * (OpDirect): the generic handler keeps that. */
  const bool native = lean_op.native != uint16_t(LN_LeanNative::Generic) &&
                      !lean_op.guard_is_delay_done;
  if (m_activeProfileCounters != nullptr) {
    (native ? m_activeProfileCounters->lean_native_ops :
              m_activeProfileCounters->lean_generic_ops)++;
  }
  if (!native) {
    OpDirect(ef, op);
    return;
  }
  switch (LN_LeanNative(lean_op.native)) {
    case LN_LeanNative::Nop:
    case LN_LeanNative::BranchRoute:
      OpLeanFlow(ef, op, lean_op);
      return;
    case LN_LeanNative::SetNumber:
      OpLeanSetNumber(ef, op, lean_op);
      return;
    case LN_LeanNative::VectorCommand:
      OpLeanVector(ef, op, lean_op);
      return;
    case LN_LeanNative::ModifyProperty:
      OpLeanModifyProperty(ef, op, lean_op);
      return;
    case LN_LeanNative::Generic:
      break;
  }
  OpDirect(ef, op);
}

/** The vector commands (Set World / Local Position, Orientation, Scale, velocities, Set Transform
 * Vector, Set Velocity Vector, Apply Movement / Rotation / Force / Torque): OpDirectVectorCommand
 * with the payload read in place, the stamp by index and the rotation of a constant Euler vector
 * worked out once at the lowering (the generic handler works it out at every run as the fallback
 * of the rotation read). The appends are the shared AppendVectorCommand. */
template<class Frame>
void LN_RuntimeTree::OpLeanVector(Frame &ef, const LN_ExecOp &op, const LN_LeanOp &lean_op)
{
  const LN_Instruction &instruction = m_program->GetInstructionPayload(
      ef.event, ef.instructions[op.instruction_index]);
  const std::vector<LN_VectorCommandPayload> &payloads = m_program->GetVectorCommandPayloads();
  const LN_LeanProgram *lean = ef.lean;
  if (instruction.command_payload_index >= payloads.size() || lean == nullptr ||
      (lean_op.constant_index != LN_INVALID_INDEX &&
       lean_op.constant_index >= lean->rotation_constants.size()))
  {
    OpDirect(ef, op); /* a program that changed since it was lowered: the generic handler */
    return;
  }
  if (m_uiEventRoutesOnly && !op.ui_event_route) {
    return;
  }
  LN_TickContext &context = ef.context;
  LN_ScopedCommandQueryContext command_query_context(
      context, ef.command_buffer, NextCommandExecutionKey());
  ProfileDirectExecOp(op);
  if (op.guard_bool_expr_index != LN_INVALID_INDEX &&
      !ef.Bool(op.guard_bool_expr_index, false))
  {
    return;
  }
  LeanVectorBody(ef,
                 instruction,
                 payloads[instruction.command_payload_index],
                 lean_op.constant_index,
                 op.instruction_index,
                 lean_op.state_index);
}

template<class Frame>
void LN_RuntimeTree::LeanVectorBody(Frame &ef,
                                    const LN_Instruction &instruction,
                                    const LN_VectorCommandPayload &payload,
                                    const uint32_t constant_index,
                                    const uint32_t instruction_index,
                                    const uint32_t state_index)
{
  const LN_TickContext &context = ef.context;
  /* The lowering gave a constant exactly to the orientation commands. */
  const bool is_orientation_command = constant_index != LN_INVALID_INDEX;
  LN_Rotation rotation;
  MT_Vector3 value(0.0f, 0.0f, 0.0f);
  if (is_orientation_command) {
    rotation = ef.Rotation(payload.vector_expr_index, ef.lean->rotation_constants[constant_index]);
  }
  else {
    value = ef.Vector(payload.vector_expr_index, payload.vector_value);
  }
  KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index);
  if (target == nullptr) {
    MarkInstructionStamp(state_index, context.tick_index);
    return;
  }
  if (instruction.opcode == LN_OpCode::ApplyTransformVector) {
    const LN_VectorOperationChannel channel = LN_VectorOperationChannel(payload.operation_channel);
    if (channel == LN_VectorOperationChannel::Movement ||
        channel == LN_VectorOperationChannel::Rotation)
    {
      if (!ef.RelativeTransformIsSafe(target,
                                      instruction,
                                      instruction_index,
                                      channel == LN_VectorOperationChannel::Movement ?
                                          LN_CommandBuffer::CommandType::ApplyMovement :
                                          LN_CommandBuffer::CommandType::ApplyRotation))
      {
        return;
      }
    }
  }
  AppendVectorCommand(ef, instruction, payload, target, value, rotation);
  MarkInstructionStamp(state_index, context.tick_index);
}

/** Modify Property: the modify branch of OpDirectGamePropertyCommand with the payload read in
 * place and the stamp by index. */
template<class Frame>
void LN_RuntimeTree::OpLeanModifyProperty(Frame &ef, const LN_ExecOp &op, const LN_LeanOp &lean_op)
{
  const LN_Instruction &instruction = m_program->GetInstructionPayload(
      ef.event, ef.instructions[op.instruction_index]);
  const std::vector<LN_GamePropertyCommandPayload> &payloads =
      m_program->GetGamePropertyCommandPayloads();
  if (instruction.command_payload_index >= payloads.size()) {
    OpDirect(ef, op); /* a program that changed since it was lowered: the generic handler */
    return;
  }
  if (m_uiEventRoutesOnly && !op.ui_event_route) {
    return;
  }
  LN_TickContext &context = ef.context;
  LN_ScopedCommandQueryContext command_query_context(
      context, ef.command_buffer, NextCommandExecutionKey());
  ProfileDirectExecOp(op);
  if (op.guard_bool_expr_index != LN_INVALID_INDEX &&
      !ef.Bool(op.guard_bool_expr_index, false))
  {
    return;
  }
  LeanModifyBody(ef, instruction, payloads[instruction.command_payload_index], lean_op.state_index);
}

template<class Frame>
void LN_RuntimeTree::LeanModifyBody(Frame &ef,
                                    const LN_Instruction &instruction,
                                    const LN_GamePropertyCommandPayload &payload,
                                    const uint32_t state_index)
{
  const LN_TickContext &context = ef.context;
  LN_PropertyModify modify;
  modify.op = payload.modify_op;
  modify.operand = ef.Float(payload.float_expr_index, 0.0f);
  modify.clamped = payload.modify_min_expr_index != LN_INVALID_INDEX &&
                   payload.modify_max_expr_index != LN_INVALID_INDEX;
  if (modify.clamped) {
    modify.min_value = ef.Float(payload.modify_min_expr_index, 0.0f);
    modify.max_value = ef.Float(payload.modify_max_expr_index, 0.0f);
  }
  if (KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index)) {
    ef.command_buffer.AppendModifyGameProperty(this,
                                               target,
                                               payload.property_ref_index,
                                               modify,
                                               ef.SortKey(),
                                               instruction.source_ref_index);
  }
  MarkInstructionStamp(state_index, context.tick_index);
}

/** Nop and BranchRoute: OpDirect's work with the instruction's stamp known from the lowering. */
template<class Frame>
void LN_RuntimeTree::OpLeanFlow(Frame &ef, const LN_ExecOp &op, const LN_LeanOp &lean_op)
{
  if (m_uiEventRoutesOnly && !op.ui_event_route) {
    return;
  }
  LN_TickContext &context = ef.context;
  LN_ScopedCommandQueryContext command_query_context(
      context, ef.command_buffer, NextCommandExecutionKey());
  ProfileDirectExecOp(op);
  if (op.guard_bool_expr_index != LN_INVALID_INDEX &&
      !ef.Bool(op.guard_bool_expr_index, false))
  {
    return;
  }
  LeanFlowBody(ef,
               op.kind == LN_ExecOpKind::BranchRoute ?
                   &m_program->GetInstructionPayload(ef.event,
                                                     ef.instructions[op.instruction_index]) :
                   nullptr,
               lean_op.state_index);
}

template<class Frame>
void LN_RuntimeTree::LeanFlowBody(Frame &ef,
                                  const LN_Instruction *branch,
                                  const uint32_t state_index)
{
  if (branch != nullptr) {
    const bool condition = branch->bool_expr_index == LN_INVALID_INDEX ?
                               false :
                               ef.Bool(branch->bool_expr_index, false);
    if (condition != branch->bool_value) {
      return;
    }
  }
  MarkInstructionStamp(state_index, ef.context.tick_index);
}

/** Set Game Property Bool / Int / Float as a number write: the number branch of
 * OpDirectGamePropertyCommand with the payload read in place and no state index worked out. */
template<class Frame>
void LN_RuntimeTree::OpLeanSetNumber(Frame &ef, const LN_ExecOp &op, const LN_LeanOp &lean_op)
{
  const LN_Instruction &instruction = m_program->GetInstructionPayload(
      ef.event, ef.instructions[op.instruction_index]);
  const std::vector<LN_GamePropertyCommandPayload> &payloads =
      m_program->GetGamePropertyCommandPayloads();
  if (instruction.command_payload_index >= payloads.size()) {
    OpDirect(ef, op); /* a program that changed since it was lowered: the generic handler */
    return;
  }
  if (m_uiEventRoutesOnly && !op.ui_event_route) {
    return;
  }
  LN_TickContext &context = ef.context;
  LN_ScopedCommandQueryContext command_query_context(
      context, ef.command_buffer, NextCommandExecutionKey());
  ProfileDirectExecOp(op);
  if (op.guard_bool_expr_index != LN_INVALID_INDEX &&
      !ef.Bool(op.guard_bool_expr_index, false))
  {
    return;
  }
  LeanSetNumberBody(
      ef, instruction, payloads[instruction.command_payload_index], lean_op.state_index);
}

template<class Frame>
void LN_RuntimeTree::LeanSetNumberBody(Frame &ef,
                                       const LN_Instruction &instruction,
                                       const LN_GamePropertyCommandPayload &payload,
                                       const uint32_t state_index)
{
  const LN_TickContext &context = ef.context;
  LN_PropertyNumber number;
  number.type = payload.value_type;
  switch (payload.value_type) {
    case LN_ValueType::Bool:
      number.bool_value = ef.Bool(payload.bool_expr_index, false);
      break;
    case LN_ValueType::Int:
      number.int_value = ef.Int(payload.int_expr_index, 0);
      break;
    default:
      number.float_value = ef.Float(payload.float_expr_index, 0.0f);
      break;
  }
  if (KX_GameObject *target = ef.ResolveObjectExpression(payload.object_value_expr_index)) {
    uint64_t handle_stamp = 0;
    EXP_Value *property_handle = m_snapshot.GetGamePropertyHandle(
        target, payload.property_ref_index, handle_stamp);
    ef.command_buffer.AppendSetGamePropertyNumberRef(this,
                                                     target,
                                                     payload.property_ref_index,
                                                     number,
                                                     ef.SortKey(),
                                                     instruction.source_ref_index,
                                                     property_handle,
                                                     handle_stamp);
  }
  MarkInstructionStamp(state_index, context.tick_index);
}

/** A compiled route (plan phase A33, LN_LeanRoute): exactly the walk of the event's blocks
 * (ColumnWalkEvent) for an event without Loops and UI event routes, each op run as OpLean runs it. What the handlers'
 * preludes did at every op is left out because it cannot change during the event: the payload
 * lookups and their checks (made at the lowering; the lean program is dropped when the program
 * grows), the paused check (ExecuteEvent runs a route only when not paused), the profile counters
 * (only when not profiling) and the save and restore of the command query context (set for each
 * op here and given back once at the end: nothing between two ops reads it). A Ran guard reads its
 * stamp in place, as InstructionExecutedNow does with the index worked out. */
template<class Frame>
void LN_RuntimeTree::RunLeanRoute(Frame &ef, const LN_LeanRoute &route, const uint64_t scope_serial)
{
  LN_TickContext &context = ef.context;
  LN_CommandBuffer *const previous_command_buffer = context.command_buffer;
  const uint64_t previous_command_execution_key = context.command_execution_key;
  context.command_buffer = &ef.command_buffer;
  const std::vector<LN_StepGate> &step_gates = m_program->GetStepGates();
  for (const LN_RouteBlock &block : route.blocks) {
    const uint32_t gate = block.step_gate_index;
    /* Every N Steps: off its steps the route does nothing. */
    if (gate != LN_INVALID_INDEX && !StepGateRuns(gate)) {
      continue;
    }
    /* ScopedStepGateTime's rule: the gate's time, and its stride when gated. */
    const double delta_time = context.delta_time;
    const double unscaled_dt = context.unscaled_dt;
    const uint32_t stride = context.phase_tick_stride;
    const bool gated = gate < step_gates.size();
    if (gate != LN_INVALID_INDEX && gate < m_stepGates.size()) {
      context.delta_time = m_stepGates[gate].time_passed;
      context.unscaled_dt = m_stepGates[gate].unscaled_time_passed;
    }
    if (gated) {
      context.phase_tick_stride = step_gates[gate].interval;
    }
    context.execution_scope_serial = scope_serial;
    m_activeExecutionScopeSerial = scope_serial;
    for (uint32_t op_index = block.first_op; op_index < block.first_op + block.op_count;
         op_index++)
    {
      const LN_RouteOp &op = route.ops[op_index];
      context.command_execution_key = NextCommandExecutionKey();
      if (op.guard == LN_RouteGuard::Ran) {
        if (op.guard_state >= m_instructionStamps.size()) {
          continue;
        }
        const InstructionStamp &stamp = m_instructionStamps[op.guard_state];
        if (stamp.last_execution_tick != context.tick_index ||
            (context.execution_scope_serial != 0 &&
             stamp.last_execution_scope_serial != context.execution_scope_serial))
        {
          continue;
        }
      }
      else if (op.guard == LN_RouteGuard::Generic && !ef.Bool(op.guard_expr, false)) {
        continue;
      }
      switch (LN_LeanNative(op.native)) {
        case LN_LeanNative::Nop:
          LeanFlowBody(ef, nullptr, op.state_index);
          break;
        case LN_LeanNative::BranchRoute:
          LeanFlowBody(ef, op.instruction, op.state_index);
          break;
        case LN_LeanNative::SetNumber:
          LeanSetNumberBody(ef, *op.instruction, *op.property_payload, op.state_index);
          break;
        case LN_LeanNative::VectorCommand:
          LeanVectorBody(ef,
                         *op.instruction,
                         *op.vector_payload,
                         op.constant_index,
                         op.instruction_index,
                         op.state_index);
          break;
        case LN_LeanNative::ModifyProperty:
          LeanModifyBody(ef, *op.instruction, *op.property_payload, op.state_index);
          break;
        case LN_LeanNative::Generic:
          break;
      }
    }
    if (gate != LN_INVALID_INDEX && gate < m_stepGates.size()) {
      context.delta_time = delta_time;
      context.unscaled_dt = unscaled_dt;
    }
    if (gated) {
      context.phase_tick_stride = stride;
    }
  }
  context.command_buffer = previous_command_buffer;
  context.command_execution_key = previous_command_execution_key;
  context.execution_scope_serial = scope_serial;
  m_activeExecutionScopeSerial = scope_serial;
}

/** The block driver: `run_op(op_index)` runs the op `op_index` of `ir` (the old interpreter's
 * OpDirect, or the lean VM's OpLean). */
template<class Frame, class RunOp>
void LN_RuntimeTree::RunLoopBody(Frame &ef,
                                 const LN_ExecBlockProgram &ir,
                                 const LN_ExecBlock &block,
                                 std::vector<uint32_t> &repeated_loop_frames,
                                 const uint64_t scope_serial,
                                 const uint32_t ui_event_state,
                                 RunOp &&run_op)
{
  /* A Loop body's block (moved out of the old system's walk, plan C1-9: a column run runs it NPC
   * by NPC through this very code, ColumnRunLoopBlock; the column walk calls it too). */
  LN_TickContext &context = ef.context;
  const auto selected = [&](const LN_ExecOp &op) {
    return ui_event_state == LN_INVALID_INDEX || op.ui_event_state_index == ui_event_state;
  };
  /* A repeat starts a Loop of the route again, once per repeat (a body copy per incoming
   * path shares the frame). One that still runs over more ticks keeps going instead. */
  if (ui_event_state != LN_INVALID_INDEX &&
      std::find(repeated_loop_frames.begin(),
                repeated_loop_frames.end(),
                block.loop_frame_index) == repeated_loop_frames.end())
  {
    repeated_loop_frames.push_back(block.loop_frame_index);
    LoopRuntimeState &repeated = LoopState(block.loop_frame_index);
    if (!repeated.active) {
      repeated.last_batch_tick_index = UINT64_MAX;
    }
  }
  BeginLoopFrame(ef, block.loop_frame_index);
  LoopRuntimeState &loop_state = LoopState(block.loop_frame_index);
  if ((loop_state.triggered || loop_state.active) &&
      loop_state.last_batch_tick_index != context.tick_index)
  {
    loop_state.last_batch_tick_index = context.tick_index;
    if (loop_state.batch_end_iteration <= loop_state.iteration_index) {
      const size_t base_batch_size = loop_state.iteration_count / loop_state.batch_count;
      const size_t larger_batch_count = loop_state.iteration_count % loop_state.batch_count;
      const size_t batch_size = base_batch_size +
                                (loop_state.batch_index < larger_batch_count ? 1 : 0);
      loop_state.batch_end_iteration = std::min(loop_state.iteration_count,
                                                loop_state.iteration_index + batch_size);
    }
    const size_t remaining_in_batch = loop_state.batch_end_iteration -
                                      loop_state.iteration_index;
    const size_t iterations_to_run = std::min(remaining_in_batch,
                                              m_remainingLoopIterationBudget);
    size_t batch_iterations = 0;
    while (batch_iterations < iterations_to_run && AdvanceLoopFrame(block.loop_frame_index))
    {
      batch_iterations++;
      const uint32_t previous_loop_body_frame = m_activeLoopBodyFrame;
      m_activeLoopBodyFrame = block.loop_frame_index;
      context.execution_scope_serial = NextExecutionScopeSerial();
      m_activeExecutionScopeSerial = context.execution_scope_serial;
      for (uint32_t op_index = block.first_op; op_index < block.first_op + block.op_count;
           op_index++)
      {
        if (selected(ir.ops[op_index])) {
          run_op(op_index);
        }
        if (loop_state.break_requested) {
          break;
        }
      }
      m_activeLoopBodyFrame = previous_loop_body_frame;
      if (loop_state.break_requested) {
        loop_state.active = false;
        break;
      }
    }
    m_remainingLoopIterationBudget -= batch_iterations;
    if (loop_state.break_requested ||
        loop_state.iteration_index >= loop_state.iteration_count)
    {
      loop_state.active = false;
      loop_state.completed_scope_serial = scope_serial;
    }
    else if (loop_state.iteration_index >= loop_state.batch_end_iteration) {
      loop_state.batch_index++;
      loop_state.batch_end_iteration = 0;
    }
  }
  context.execution_scope_serial = scope_serial;
  m_activeExecutionScopeSerial = scope_serial;
}

