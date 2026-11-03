// SPDX-FileCopyrightText: 2026 UPBGE Authors
//
// SPDX-License-Identifier: GPL-2.0-or-later

/** \file
 * The expression kinds as member templates of LN_RuntimeTree (lean runtime plan, phases A13 and
 * A14): each `<Family>Kind(In &in, ...)` is the switch of the old Resolve<Family>Expression, moved
 * unchanged except that every read of an input goes through the reader `in` (its Bool, Int, Float,
 * String, Vector, Rotation, Color, Value and Query functions) and that the order of the reads inside
 * one expression is written out as sequenced locals (the order this GCC build used, decision D20;
 * pinned by LN_ReadOrder_test). Included at the end of LN_RuntimeTree.cpp so the helpers of that file
 * are visible. The readers are LN_LeanReader's Lean* functions below (the old Resolve functions and
 * their recursive reader went with plan step R4, attribute and retirement plan). */

/** Whether instruction `instruction_index` ran in the execution scope being run (the guard of the
 * ops that follow a route). BoolKind's InstructionExecuted and the fast path of LeanBool. */
inline bool LN_RuntimeTree::InstructionExecutedNow(const LN_TickContext &context,
                                                   const uint32_t instruction_index) const
{
  const uint32_t state_index = InstructionStateIndex(context.event, instruction_index);
  if (state_index == LN_INVALID_INDEX || state_index >= m_instructionStamps.size()) {
    return false;
  }
  const InstructionStamp &state = m_instructionStamps[state_index];
  if (state.last_execution_tick != context.tick_index) {
    return false;
  }
  return context.execution_scope_serial == 0 ||
         state.last_execution_scope_serial == context.execution_scope_serial;
}

/* ---- Object attributes read without their value (plan "Attribute and Retirement", G2/G3) ---- */

template<class In>
KX_GameObject *LN_RuntimeTree::ReadObjectInput(In &in, const uint32_t value_index)
{
  if (m_activeProfileCounters == nullptr && g_read_trace == nullptr &&
      fast_attributes_mode() != FastAttributesMode::Off)
  {
    const std::vector<LN_ValueExpression> &values = m_program->GetValueExpressions();
    if (value_index < values.size()) {
      const LN_ValueExpression &constant = values[value_index];
      /* A constant is never kept nor noted by the memo: only the value is skipped. */
      if (constant.kind == LN_ValueExpressionKind::Constant &&
          constant.value.type == LN_ValueType::ObjectRef && constant.value.exists)
      {
        KX_GameObject *object = ConstantObjectTarget(value_index, constant.value);
        if (fast_attributes_mode() == FastAttributesMode::Verify &&
            ResolveObjectValue(in.Value(value_index, MakeNoneValue())) != object)
        {
          NoteFastAttributeDifference("a constant object is not the one its value gives");
        }
        return object;
      }
    }
  }
  return ResolveObjectValue(in.Value(value_index, MakeNoneValue()));
}

template<class In>
KX_GameObject *LN_RuntimeTree::ObjectAttributeTarget(In &in, const LN_ValueExpression &attribute)
{
  if (attribute.input0 != LN_INVALID_INDEX) {
    return ReadObjectInput(in, attribute.input0);
  }
  return m_gameObject;
}

template<class In>
KX_GameObject *LN_RuntimeTree::TypedAttributeTarget(In &in, const uint32_t value_index)
{
  return ObjectAttributeTarget(in, m_program->GetValueExpressions()[value_index]);
}

/** The attribute read straight into a vector, as ValueAsVector reads the attribute's value: an
 * orientation as its Euler angles, a colour as its RGB. False for Visible and Name (no vector). */
static bool ObjectAttributeAsVector(KX_GameObject &object,
                                    const LN_ObjectAttributeId attribute,
                                    MT_Vector3 &r_vector)
{
  switch (attribute) {
    case LN_ObjectAttributeId::WorldPosition:
      r_vector = object.NodeGetWorldPosition();
      return true;
    case LN_ObjectAttributeId::LocalPosition:
      r_vector = object.NodeGetLocalPosition();
      return true;
    case LN_ObjectAttributeId::WorldOrientation:
      r_vector = LN_Rotation::FromMatrix(object.NodeGetWorldOrientation()).ToEulerXYZ();
      return true;
    case LN_ObjectAttributeId::LocalOrientation:
      r_vector = LN_Rotation::FromMatrix(object.NodeGetLocalOrientation()).ToEulerXYZ();
      return true;
    case LN_ObjectAttributeId::WorldScale:
      r_vector = object.NodeGetWorldScaling();
      return true;
    case LN_ObjectAttributeId::LocalScale:
      r_vector = object.NodeGetLocalScaling();
      return true;
    case LN_ObjectAttributeId::WorldLinearVelocity:
      r_vector = object.GetLinearVelocity(false);
      return true;
    case LN_ObjectAttributeId::LocalLinearVelocity:
      r_vector = object.GetLinearVelocity(true);
      return true;
    case LN_ObjectAttributeId::WorldAngularVelocity:
      r_vector = object.GetAngularVelocity(false);
      return true;
    case LN_ObjectAttributeId::LocalAngularVelocity:
      r_vector = object.GetAngularVelocity(true);
      return true;
    case LN_ObjectAttributeId::Color: {
      const MT_Vector4 &color = object.GetObjectColor();
      r_vector = MT_Vector3(color.x(), color.y(), color.z());
      return true;
    }
    case LN_ObjectAttributeId::Visible:
    case LN_ObjectAttributeId::Name:
    case LN_ObjectAttributeId::None:
      break;
  }
  return false;
}

template<class In>
bool LN_RuntimeTree::BoolKind(In &in,
    const uint32_t expression_index,
    const LN_BoolExpression &expression,
    const bool fallback)
{
  const LN_TickContext &context = in.context;
  switch (expression.kind) {
    case LN_BoolExpressionKind::Constant:
      return expression.bool_value;
    case LN_BoolExpressionKind::GamePaused:
      return context.game_paused;
    case LN_BoolExpressionKind::SnapshotGameProperty: {
      LN_Snapshot::GamePropertyView view;
      if (m_snapshot.ReadGameProperty(expression.property_ref_index, view)) {
        return view.bool_value;
      }
      return fallback;
    }
    case LN_BoolExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return value->bool_value;
      }
      return fallback;
    case LN_BoolExpressionKind::Not:
      return !in.Bool(expression.input0, false);
    case LN_BoolExpressionKind::And:
      return in.Bool(expression.input0, false) &&
             in.Bool(expression.input1, false);
    case LN_BoolExpressionKind::Or:
      return in.Bool(expression.input0, false) ||
             in.Bool(expression.input1, false);
    case LN_BoolExpressionKind::FloatCompare: {
      const float a = in.Float(expression.input0, 0.0f);
      const float b = in.Float(expression.input1, 0.0f);
      switch (expression.float_compare_operation) {
        case LN_FloatCompareOperation::Equal:
          return std::fabs(a - b) <= 1.0e-6f;
        case LN_FloatCompareOperation::NotEqual:
          return std::fabs(a - b) > 1.0e-6f;
        case LN_FloatCompareOperation::GreaterThan:
          return a > b;
        case LN_FloatCompareOperation::LessThan:
          return a < b;
        case LN_FloatCompareOperation::GreaterEqual:
          return a >= b;
        case LN_FloatCompareOperation::LessEqual:
          return a <= b;
      }
      break;
    }
    case LN_BoolExpressionKind::StringContains: {
      /* Read order (D20): Text, then Search. */
      const std::string text = in.String(expression.input0, "");
      const std::string search = in.String(expression.input1, "");
      return text.find(search) != std::string::npos;
    }
    case LN_BoolExpressionKind::StringStartsWith: {
      /* Read order (D20): the second input first (the order the arguments were read in). */
      const std::string prefix = in.String(expression.input1, "");
      const std::string text = in.String(expression.input0, "");
      return StringStartsWith(text, prefix);
    }
    case LN_BoolExpressionKind::StringEndsWith: {
      /* Read order (D20): the second input first. */
      const std::string suffix = in.String(expression.input1, "");
      const std::string text = in.String(expression.input0, "");
      return StringEndsWith(text, suffix);
    }
    case LN_BoolExpressionKind::SnapshotVisibility: {
      std::optional<LN_ObjectSnapshot> other_object;
      const LN_ObjectSnapshot *object_snapshot = &m_snapshot.GetObjectSnapshot();
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                in.Value(expression.input0, MakeNoneValue())))
        {
          object_snapshot = &LN_CaptureObjectSnapshot(
              game_object, LN_SNAPSHOT_CHANNEL_VISIBILITY, other_object.emplace());
        }
        else {
          return fallback;
        }
      }
      return object_snapshot->visible;
    }
    case LN_BoolExpressionKind::SnapshotCharacterOnGround: {
      std::optional<LN_ObjectSnapshot> other_object;
      const LN_ObjectSnapshot *object_snapshot = &m_snapshot.GetObjectSnapshot();
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                in.Value(expression.input0, MakeNoneValue())))
        {
          object_snapshot = &LN_CaptureObjectSnapshot(
              game_object, LN_SNAPSHOT_CHANNEL_CHARACTER, other_object.emplace());
        }
        else {
          return fallback;
        }
      }
      return object_snapshot->has_character ? object_snapshot->character_on_ground : fallback;
    }
    case LN_BoolExpressionKind::WindowFullscreen:
      if (RAS_ICanvas *canvas = ActiveCanvas()) {
        return canvas->GetFullScreen();
      }
      return fallback;
    case LN_BoolExpressionKind::SnapshotGamePropertyExists: {
      LN_Snapshot::GamePropertyView view;
      if (m_snapshot.ReadGameProperty(expression.property_ref_index, view)) {
        return view.exists;
      }
      return false;
    }
    case LN_BoolExpressionKind::ValueIsNone: {
      const LN_Value value = in.Value(expression.input0, MakeNoneValue());
      return !value.exists || value.type == LN_ValueType::None;
    }
    case LN_BoolExpressionKind::FromGenericValue: {
      const LN_Value value = in.Value(expression.input0, MakeNoneValue());
      return ValueAsBool(value);
    }
    case LN_BoolExpressionKind::FromInt:
      return in.Int(expression.input0, 0) != 0;
    case LN_BoolExpressionKind::FromFloat:
      return in.Float(expression.input0, 0.0f) != 0.0f;
    case LN_BoolExpressionKind::FromString:
      return !in.String(expression.input0, std::string()).empty();
    case LN_BoolExpressionKind::FromVector:
      return in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f))
                 .length2() > 0.0f;
    case LN_BoolExpressionKind::FromColor:
      return in.Color(expression.input0, MT_Vector4(0.0f, 0.0f, 0.0f, 0.0f))
                 .length2() > 0.0f;
    case LN_BoolExpressionKind::RigidBodyAttribute: {
      PHY_IPhysicsController *controller = ResolveRigidBodyAttributeController(in, expression.input0);
      if (controller == nullptr) {
        return false;
      }
      return ReadRigidBodyBoolAttribute(*controller,
                                        static_cast<LN_RigidBodyAttribute>(expression.int_value),
                                        expression.secondary_int_value);
    }
    case LN_BoolExpressionKind::RigidBodyConstraintFound: {
      KX_GameObject *target = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      if (!UsesJoltPhysics(target)) {
        return false;
      }
      const LN_RigidBodyConstraintMatchMode match_mode = ValidRigidBodyConstraintMatchMode(
          expression.rigid_body_constraint_match_mode);
      if (match_mode == LN_RigidBodyConstraintMatchMode::All) {
        return target->HasRigidBodyConstraints();
      }
      const std::string query = in.String(expression.property_ref_index, std::string());
      return FindRigidBodyConstraintMatch(*target, query, match_mode) != nullptr;
    }
    case LN_BoolExpressionKind::ValueCompare: {
      const LN_Value a = in.Value(expression.input0, MakeNoneValue());
      const LN_Value b = in.Value(expression.input1, MakeNoneValue());
      return CompareValues(a, b, expression.float_compare_operation);
    }
    case LN_BoolExpressionKind::MaterialSlotFound: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      const int32_t slot = in.Int(expression.input1, 0);
      return ObjectMaterialAtSlot(target_object, slot) != nullptr;
    }
    case LN_BoolExpressionKind::MaterialNodeValueFound: {
      blender::Material *material = nullptr;
      const bool object_slot_target = ELEM(expression.property_ref_index, 1u, 2u, 4u);
      if (object_slot_target) {
        KX_GameObject *target_object = m_gameObject;
        if (expression.input0 != LN_INVALID_INDEX) {
          target_object = ResolveObjectValue(
              in.Value(expression.input0, MakeNoneValue()));
        }
        const int32_t slot = in.Int(expression.input1, 0);
        material = ObjectMaterialAtSlot(target_object, slot);
      }
      else {
        material = ResolveMaterialByValue(
            this, in.Value(expression.input0, MakeNoneValue()));
      }

      const std::string node_name = in.String(expression.input2, std::string());
      const std::string socket_name = in.String(expression.int_expr_index, std::string());
      if (material == nullptr || material->nodetree == nullptr || node_name.empty() ||
          socket_name.empty())
      {
        return false;
      }
      blender::bNode *node = FindNodeByUserName(material->nodetree, node_name);
      blender::bNodeSocket *socket = FindInputSocketByIdentifierOrName(node, socket_name);
      if (socket == nullptr) {
        return false;
      }
      if (expression.property_ref_index >= 3) {
        material->nodetree->ensure_topology_cache();
        if ((socket->flag & blender::SOCK_UNAVAIL) != 0 || socket->is_directly_linked()) {
          return false;
        }
      }
      if (expression.property_ref_index == 2) {
        return upbge::logic_nodes::game_state::material_parameter_socket_supports_object_attribute(
            *socket);
      }
      return ReadSocketDefaultValue(*socket, this).exists;
    }
    case LN_BoolExpressionKind::EditorNodeValueFound: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      const std::string target_name = in.String(expression.input1, std::string());
      blender::bNodeTree *tree = ResolveEditorNodeValueTree(
          EditorNodeValueTarget(expression.property_ref_index), target_object, target_name);
      const std::string node_name = in.String(expression.input2, std::string());
      const std::string socket_name = in.String(expression.int_expr_index, std::string());
      blender::bNode *node = FindNodeByUserName(tree, node_name);
      blender::bNodeSocket *socket = FindInputSocketByIdentifierOrName(node, socket_name);
      if (tree == nullptr || socket == nullptr || (socket->flag & blender::SOCK_UNAVAIL) != 0) {
        return false;
      }
      tree->ensure_topology_cache();
      return !socket->is_directly_linked() && ReadSocketDefaultValue(*socket, this).exists;
    }
    case LN_BoolExpressionKind::DictHasKey: {
      const LN_Value dict_value = expression.input0 != LN_INVALID_INDEX ?
                                      in.Value(expression.input0, MakeNoneValue()) :
                                      MakeNoneValue();
      const std::string key = expression.input1 != LN_INVALID_INDEX ?
                                  in.String(expression.input1, std::string()) :
                                  std::string();
      if (dict_value.type == LN_ValueType::Dict && dict_value.exists && !key.empty()) {
        return dict_value.dict_value.find(key) != dict_value.dict_value.end();
      }
      return false;
    }
    case LN_BoolExpressionKind::StoreValueDone: {
      /* Evaluated by the route on Store Value's Flow, when the flow arrives: store now. */
      const std::vector<LN_FloatExpression> &float_expressions = m_program->GetFloatExpressions();
      if (expression.float_expr_index >= float_expressions.size() ||
          float_expressions[expression.float_expr_index].kind != LN_FloatExpressionKind::StoreValue)
      {
        return fallback;
      }
      const LN_FloatExpression &store = float_expressions[expression.float_expr_index];
      FloatExpressionRuntimeState &state = FloatExpressionState(expression.float_expr_index);
      state.initialized = true;
      state.stored_value = in.Float(store.input0, store.float_value);
      state.last_store_tick = context.tick_index;
      return true;
    }
    case LN_BoolExpressionKind::ListContains: {
      const LN_Value list_value = expression.input0 != LN_INVALID_INDEX ?
                                      in.Value(expression.input0, MakeNoneValue()) :
                                      MakeNoneValue();
      const LN_Value needle = expression.input1 != LN_INVALID_INDEX ?
                                  in.Value(expression.input1, MakeNoneValue()) :
                                  MakeNoneValue();
      if (list_value.type != LN_ValueType::List || !list_value.exists) {
        return false;
      }
      for (const LN_Value &item : list_value.list_value) {
        if (ValuesEqual(item, needle)) {
          return true;
        }
      }
      return false;
    }
    case LN_BoolExpressionKind::InstructionExecuted:
      return InstructionExecutedNow(context, expression.input0);
    case LN_BoolExpressionKind::InstructionReached: {
      const uint32_t state_index = InstructionStateIndex(context.event, expression.input0);
      const InstructionMotionState *state = state_index == LN_INVALID_INDEX ?
                                                nullptr :
                                                FindMotionState(state_index);
      if (state == nullptr || state->last_reached_tick != context.tick_index) {
        return false;
      }
      return context.execution_scope_serial == 0 ||
             state->last_reached_scope_serial == context.execution_scope_serial;
    }
    case LN_BoolExpressionKind::InputStatus: {
      const int32_t input_code = expression.int_value;
      if (input_code == SCA_IInputDevice::NOKEY) {
        return false;
      }
      /* bool_value: also what the UI used. */
      return (expression.bool_value ? m_snapshot.GetRawInputSnapshot() :
                                      m_snapshot.GetInputSnapshot())
          .GetStatus(input_code, expression.secondary_int_value);
    }
    case LN_BoolExpressionKind::InputActionState: {
      /* Read order (D20): the player (Int input 0), then the action name (String input 1). */
      const int32_t user = in.Int(expression.input0, 0);
      const std::string action_name = in.String(expression.input1, std::string());
      const LN_InputActionState *state = m_snapshot.GetInputActionState(
          LN_InputMap::Get().Find(action_name), user, expression.bool_value);
      if (state == nullptr) {
        return false;
      }
      switch (expression.secondary_int_value) {
        case SCA_InputEvent::JUSTACTIVATED:
          return state->pressed;
        case SCA_InputEvent::JUSTRELEASED:
          return state->released;
        default:
          return state->active;
      }
    }
    case LN_BoolExpressionKind::KeyboardActive:
      return KeyboardInputIsActive(m_snapshot.GetInputSnapshot());
    case LN_BoolExpressionKind::MouseMoved: {
      return EvaluateMouseMoved(expression.bool_value);
    }
    case LN_BoolExpressionKind::MouseWheelMoved: {
      const int32_t wheel_delta = (expression.bool_value ? m_snapshot.GetRawInputSnapshot() :
                                                           m_snapshot.GetInputSnapshot())
                                      .GetMouse()
                                      .wheel_delta;
      if (wheel_delta == 0) {
        return false;
      }
      const int32_t direction = expression.int_value > 0 ? expression.int_value : 3;
      if (direction == 1) {
        return wheel_delta > 0;
      }
      if (direction == 2) {
        return wheel_delta < 0;
      }
      return true;
    }
    case LN_BoolExpressionKind::KeyLoggerPressed:
      return ScanKeyLogger(m_snapshot.GetInputSnapshot(), expression.bool_value).pressed;
    case LN_BoolExpressionKind::GamepadActive: {
      const int32_t gamepad_index = in.Int(expression.input0, 0);
      const LN_GamepadSnapshot *gamepad = m_snapshot.GetInputSnapshot().GetGamepad(gamepad_index);
      return gamepad != nullptr && LN_GamepadHasActiveInput(*gamepad);
    }
    case LN_BoolExpressionKind::GamepadButton: {
      const int32_t gamepad_index = in.Int(expression.input0, 0);
      /* secondary_int_value: also what the UI used. */
      const LN_GamepadSnapshot *gamepad = (expression.secondary_int_value != 0 ?
                                               m_snapshot.GetRawInputSnapshot() :
                                               m_snapshot.GetInputSnapshot())
                                              .GetGamepad(gamepad_index);
      if (gamepad == nullptr) {
        return false;
      }
      const int32_t input_status = expression.int_expr_index != LN_INVALID_INDEX ?
                                       in.Int(expression.int_expr_index, SCA_InputEvent::ACTIVE) :
                                       SCA_InputEvent::ACTIVE;
      return LN_GetGamepadControlState(
          *gamepad, expression.int_value, expression.bool_value, input_status);
    }
    case LN_BoolExpressionKind::PhysicsQueryDone:
      /* Invalid inputs and an unavailable backend are still completed query attempts. A disabled
       * query, however, did not receive Flow and must not emit an execution pulse. */
      return in.Query(expression.input0).physics_query.diagnostic_status !=
             LN_QueryDiagnosticStatus::Disabled;
    case LN_BoolExpressionKind::PhysicsQueryHit:
      return in.Query(expression.input0).physics_query.hit;
    case LN_BoolExpressionKind::PhysicsQueryBlocked:
      return in.Query(expression.input0).physics_query.blocked;
    case LN_BoolExpressionKind::PhysicsQueryHasUV:
      return in.Query(expression.input0).physics_query.has_uv;
    case LN_BoolExpressionKind::PhysicsQueryStartedOverlapping:
      return in.Query(expression.input0).physics_query.started_overlapping;
    case LN_BoolExpressionKind::PhysicsQueryAllHit: {
      const std::vector<LN_PhysicsQueryResult> &results =
          in.Query(expression.input0).physics_query_results;
      return !results.empty() &&
             std::all_of(results.begin(), results.end(), [](const LN_PhysicsQueryResult &result) {
               return result.hit;
             });
    }
    case LN_BoolExpressionKind::QueryValid:
      return in.Query(expression.input0).valid;
    case LN_BoolExpressionKind::MotionValid: {
      if (expression.int_value < 0) {
        return false;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      return state != nullptr && state->motion_valid;
    }
    case LN_BoolExpressionKind::TweenValue: {
      return UpdateTweenValue(in, expression_index);
    }
    case LN_BoolExpressionKind::TweenReached: {
      if (expression.input0 == LN_INVALID_INDEX || m_program == nullptr) {
        return fallback;
      }
      const std::vector<LN_BoolExpression> &expressions = m_program->GetBoolExpressions();
      if (expression.input0 >= expressions.size()) {
        return fallback;
      }
      UpdateTweenValue(in, expression.input0);
      return BoolExpressionState(expression.input0).cached_result;
    }
    case LN_BoolExpressionKind::TimerElapsed: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && state->pulse_tick == context.tick_index;
    }
    case LN_BoolExpressionKind::DelayDone: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && state->pulse_tick == context.tick_index;
    }
    case LN_BoolExpressionKind::PulsifyPulse: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && (state->pulse_tick == context.tick_index ||
                                  (state->kind == TimeFlowRuntimeKind::Pulsify &&
                                   state->last_update_tick == context.tick_index &&
                                   state->accumulated_time <= LN_TIME_FLOW_EPSILON));
    }
    case LN_BoolExpressionKind::BarrierPassed: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr &&
             (state->pulse_tick == context.tick_index ||
              (state->kind == TimeFlowRuntimeKind::Barrier && state->active &&
               state->remaining_time >= 0.0 &&
               state->accumulated_time + LN_TIME_FLOW_EPSILON >= state->remaining_time));
    }
    case LN_BoolExpressionKind::CooldownAccepted:
    case LN_BoolExpressionKind::CooldownBlocked: {
      if (expression.int_value < 0 || context.execution_scope_serial == 0) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      if (state == nullptr) {
        return fallback;
      }
      return expression.kind == LN_BoolExpressionKind::CooldownAccepted ?
                 state->accepted_scope_serial == context.execution_scope_serial :
                 state->blocked_scope_serial == context.execution_scope_serial;
    }
    case LN_BoolExpressionKind::CooldownCompleted: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && state->pulse_tick == context.tick_index;
    }
    case LN_BoolExpressionKind::CooldownReady: {
      if (expression.int_value < 0) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && !state->active;
    }
    case LN_BoolExpressionKind::PersistenceCompleted: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      if (state.last_update_tick == context.tick_index) {
        return state.cached_result;
      }
      state.cached_result = state.pending_persistence_count != 0;
      if (state.cached_result) {
        state.last_persistence_success = (state.pending_persistence_results & uint64_t(1)) != 0;
        state.pending_persistence_results >>= 1;
        state.pending_persistence_count--;
      }
      state.last_update_tick = context.tick_index;
      return state.cached_result;
    }
    case LN_BoolExpressionKind::PersistenceSucceeded: {
      if (expression.input0 == LN_INVALID_INDEX ||
          expression.input0 >= m_program->GetBoolExpressions().size() ||
          m_program->GetBoolExpressions()[expression.input0].kind !=
              LN_BoolExpressionKind::PersistenceCompleted)
      {
        return fallback;
      }
      const bool completed = in.Bool(expression.input0, false);
      return completed && BoolExpressionState(expression.input0).last_persistence_success;
    }
    case LN_BoolExpressionKind::UICommandCompleted: {
      UICommandCompletionState &state = m_uiCommandCompletions[expression_index];
      if (state.current_tick != context.tick_index) {
        state.current_tick = context.tick_index;
        state.has_current = !state.pending.empty();
        if (state.has_current) {
          state.current = state.pending.front();
          state.pending.pop_front();
          state.has_value = true;
        }
      }
      return state.has_current;
    }
    case LN_BoolExpressionKind::UICommandSucceeded:
    case LN_BoolExpressionKind::UICommandFailed:
    case LN_BoolExpressionKind::UICommandClosed: {
      if (expression.input0 == LN_INVALID_INDEX ||
          expression.input0 >= m_program->GetBoolExpressions().size() ||
          m_program->GetBoolExpressions()[expression.input0].kind !=
              LN_BoolExpressionKind::UICommandCompleted)
      {
        return fallback;
      }
      if (!in.Bool(expression.input0, false)) {
        return false;
      }
      const UICommandCompletionState &state = m_uiCommandCompletions[expression.input0];
      const bool succeeded = state.current.result == LN_UIResult::Success ||
                             state.current.result == LN_UIResult::NoChange;
      /* A toggled menu that closed is neither opened nor failed. */
      const bool closed = state.current.result == LN_UIResult::Closed;
      switch (expression.kind) {
        case LN_BoolExpressionKind::UICommandSucceeded:
          return succeeded;
        case LN_BoolExpressionKind::UICommandClosed:
          return closed;
        default:
          return !succeeded && !closed;
      }
    }
    case LN_BoolExpressionKind::PlayerJoinPulse:
      return m_logicManager != nullptr && m_logicManager->IsPlayerJoinActive();
    case LN_BoolExpressionKind::PlayerGamepadConnected: {
      const int32_t gamepad = LN_InputMap::Get().GamepadOfUser(
          in.Int(expression.input0, 0));
      DEV_Joystick *joystick = gamepad >= 0 ? DEV_Joystick::GetInstance(short(gamepad)) :
                                                    nullptr;
      return joystick != nullptr && joystick->Connected();
    }
    case LN_BoolExpressionKind::PlayerGamepadJoined:
      return LN_InputMap::Get().IsUserJoined(in.Int(expression.input0, 0));
    case LN_BoolExpressionKind::PlayerViewShown: {
      KX_PlayerViews::Area area;
      return ResolvePlayerViewArea(
          m_gameObject, in.Int(expression.input0, 0), area);
    }
    case LN_BoolExpressionKind::ActionLifecycleEvent: {
      ActionCompletionState &state = m_actionCompletions[expression_index];
      if (state.current_tick != context.tick_index) {
        state.current_tick = context.tick_index;
        state.has_current = !state.pending.empty();
        if (state.has_current) {
          state.current = state.pending.front();
          state.pending.pop_front();
        }
      }
      return state.has_current;
    }
    case LN_BoolExpressionKind::ActionStarted:
    case LN_BoolExpressionKind::ActionCompleted:
    case LN_BoolExpressionKind::ActionInterrupted:
    case LN_BoolExpressionKind::ActionFailed: {
      if (expression.input0 == LN_INVALID_INDEX ||
          expression.input0 >= m_program->GetBoolExpressions().size() ||
          m_program->GetBoolExpressions()[expression.input0].kind !=
              LN_BoolExpressionKind::ActionLifecycleEvent)
      {
        return fallback;
      }
      if (!in.Bool(expression.input0, false)) {
        return false;
      }
      const LN_ActionResult result = m_actionCompletions[expression.input0].current;
      switch (expression.kind) {
        case LN_BoolExpressionKind::ActionStarted:
          return result == LN_ActionResult::Started;
        case LN_BoolExpressionKind::ActionCompleted:
          return result == LN_ActionResult::Completed;
        case LN_BoolExpressionKind::ActionInterrupted:
          return result == LN_ActionResult::Interrupted;
        case LN_BoolExpressionKind::ActionFailed:
          return result == LN_ActionResult::Failed;
        default:
          return false;
      }
    }
    case LN_BoolExpressionKind::GameLoadPulse:
      return m_logicManager != nullptr && m_logicManager->IsGameLoadPulseActive();
    case LN_BoolExpressionKind::GameLoadSucceeded:
      return m_logicManager != nullptr && m_logicManager->DidGameLoadSucceed();
    case LN_BoolExpressionKind::MouseOverEnter:
      return in.Query(expression.input0).entered;
    case LN_BoolExpressionKind::MouseOverOver:
      return in.Query(expression.input0).over;
    case LN_BoolExpressionKind::MouseOverExit:
      return in.Query(expression.input0).exited;
    case LN_BoolExpressionKind::UIElementEventFired:
      return in.Query(expression.input0).valid;
    case LN_BoolExpressionKind::UISoundFired: {
      const LN_QueryResult &sound = in.Query(expression.input0);
      return sound.valid && sound.ui_sound == expression.int_value;
    }
    case LN_BoolExpressionKind::UIElementEventAccepted:
      return in.Query(expression.input0).ui_accepted;
    case LN_BoolExpressionKind::UIScrolled:
      return in.Query(expression.input0).ui_scrolled;
    case LN_BoolExpressionKind::UIScrollAtStart:
      return in.Query(expression.input0).ui_scroll_at_start;
    case LN_BoolExpressionKind::UIScrollAtEnd:
      return in.Query(expression.input0).ui_scroll_at_end;
    case LN_BoolExpressionKind::UIItemsRebound:
      return in.Query(expression.input0).ui_items_rebound;
    case LN_BoolExpressionKind::LogicTreeRunning: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      const std::string tree_name = expression.input1 != LN_INVALID_INDEX ?
                                        in.String(expression.input1, expression.string_value) :
                                        expression.string_value;
      if (target_object == nullptr || tree_name.empty()) {
        return fallback;
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      LN_Manager *manager = scene ? scene->GetLogicNodeManager() : nullptr;
      if (manager == nullptr) {
        return fallback;
      }
      return manager->IsLogicTreeRunning(target_object, tree_name);
    }
    case LN_BoolExpressionKind::LogicTreeStopped: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      const std::string tree_name = expression.input1 != LN_INVALID_INDEX ?
                                        in.String(expression.input1, expression.string_value) :
                                        expression.string_value;
      if (target_object == nullptr || tree_name.empty()) {
        return fallback;
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      LN_Manager *manager = scene ? scene->GetLogicNodeManager() : nullptr;
      if (manager == nullptr) {
        return fallback;
      }
      return !manager->IsLogicTreeRunning(target_object, tree_name);
    }
    case LN_BoolExpressionKind::ActionDone: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const int32_t layer = in.Int(expression.int_expr_index, expression.int_value);
      return target_object->IsActionDone(short(layer));
    }
    case LN_BoolExpressionKind::AnimationPlaying: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const int32_t layer = in.Int(expression.int_expr_index, expression.int_value);
      const std::string action_name = target_object->GetActionName(short(layer));
      return !action_name.empty() && !target_object->IsActionDone(short(layer));
    }
    case LN_BoolExpressionKind::ObjectsColliding: {
      KX_GameObject *obj_a = m_gameObject;
      KX_GameObject *obj_b = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        obj_a = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        obj_b = ResolveObjectValue(
            in.Value(expression.input1, MakeNoneValue()));
      }
      if (obj_a == nullptr || obj_b == nullptr) {
        return fallback;
      }
      PHY_IPhysicsController *ctrl_a = obj_a->GetPhysicsController();
      PHY_IPhysicsController *ctrl_b = obj_b->GetPhysicsController();
      if (ctrl_a == nullptr || ctrl_b == nullptr) {
        return false;
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      if (scene == nullptr) {
        scene = obj_a->GetScene();
      }
      if (scene == nullptr) {
        scene = obj_b->GetScene();
      }
      PHY_IPhysicsEnvironment *env = scene ? scene->GetPhysicsEnvironment() : nullptr;
      if (env == nullptr) {
        return false;
      }
      const PHY_CollisionTestResult hit = env->CheckCollision(ctrl_a, ctrl_b);
      const bool collide = hit.collide;
      delete hit.collData;
      return collide;
    }
    case LN_BoolExpressionKind::EventReceived: {
      const LN_StringId subject_string_id = ResolveStringExpressionId(expression.input0);
      const LN_EventSubjectId subject_id = GetEventSubjectId(subject_string_id);
      const std::string subject = in.String(expression.input0, expression.string_value);
      if (subject.empty()) {
        return false;
      }
      KX_GameObject *filter_target = m_gameObject;
      if (expression.bool_value) {
        if (expression.input1 == LN_INVALID_INDEX) {
          return false;
        }
        const LN_Value target_val = in.Value(expression.input1, MakeNoneValue());
        KX_GameObject *explicit_target = ResolveObjectValue(target_val);
        if (explicit_target == nullptr) {
          return false;
        }
        filter_target = explicit_target;
      }
      return FindFirstActiveEvent(subject_id, subject, filter_target) != nullptr;
    }
    case LN_BoolExpressionKind::CollisionDetected: {
      return ResolveCollisionResult(in, expression.input0, expression.input1, expression.input2, CollisionResult::Detail::StateOnly)
          .hit;
    }
    case LN_BoolExpressionKind::ConstraintBreak:
      return ResolveRigidBodyConstraintBreakEvent(in, expression.input0, expression.input1, expression.input2, expression.rigid_body_constraint_match_mode) != nullptr;
    case LN_BoolExpressionKind::CollisionEnter:
    case LN_BoolExpressionKind::CollisionStay:
    case LN_BoolExpressionKind::CollisionExit: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      if (state.last_update_tick == context.tick_index) {
        return state.cached_result;
      }

      CollisionResult::Detail detail = CollisionResult::Detail::StateOnly;
      if (expression.int_value >= static_cast<int>(CollisionResult::Detail::Contacts) ||
          expression.bool_value)
      {
        detail = CollisionResult::Detail::Contacts;
      }
      else if (expression.int_value == static_cast<int>(CollisionResult::Detail::Objects)) {
        detail = CollisionResult::Detail::Objects;
      }
      const CollisionResult &collision_query = ResolveCollisionResult(in, expression.input0, expression.input1, expression.input2, detail, false);
      const bool current = collision_query.hit;
      const bool previous = state.initialized ? state.previous_bool : false;
      switch (expression.kind) {
        case LN_BoolExpressionKind::CollisionEnter:
          state.cached_result = current && !previous;
          break;
        case LN_BoolExpressionKind::CollisionStay:
          state.cached_result = current && previous;
          break;
        case LN_BoolExpressionKind::CollisionExit:
          state.cached_result = !current && previous;
          break;
        default:
          state.cached_result = false;
          break;
      }
      if (expression.kind == LN_BoolExpressionKind::CollisionExit && state.cached_result) {
        MarkCollisionExitPayload(collision_query, context.tick_index);
      }
      state.previous_bool = current;
      state.initialized = true;
      state.last_update_tick = context.tick_index;
      return state.cached_result;
    }
    case LN_BoolExpressionKind::NextFrameDone: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && state->pulse_tick == context.tick_index;
    }
    case LN_BoolExpressionKind::BooleanEdge: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      if (context.tick_index != UINT64_MAX && state.last_update_tick == context.tick_index) {
        return state.cached_result;
      }

      const bool current = in.Bool(expression.input0, false);
      const bool previous = state.initialized ? state.previous_bool : false;
      state.cached_result = current && !previous;
      state.active = !current && previous;
      state.previous_bool = current;
      state.initialized = true;
      state.last_update_tick = context.tick_index;
      return state.cached_result;
    }
    case LN_BoolExpressionKind::BooleanEdgeFalling: {
      if (expression.input0 == LN_INVALID_INDEX || m_program == nullptr ||
          expression.input0 >= m_program->GetBoolExpressions().size())
      {
        return fallback;
      }
      in.Bool(expression.input0, false);
      return BoolExpressionState(expression.input0).active;
    }
    case LN_BoolExpressionKind::Once: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      const uint64_t scope_serial = context.execution_scope_serial;
      return scope_serial != 0 && state.initialized && state.last_update_tick == scope_serial &&
             state.cached_result;
    }
    case LN_BoolExpressionKind::FlowGateOpen: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      if (!state.initialized) {
        state.active = in.Bool(expression.input0, false);
        state.initialized = true;
      }
      return state.active;
    }
    case LN_BoolExpressionKind::FlowRouterSelected: {
      if (expression.int_value < 0 || expression.secondary_int_value < 0 ||
          context.execution_scope_serial == 0)
      {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      return state != nullptr && state->routed_scope_serial == context.execution_scope_serial &&
             state->last_route_index == uint32_t(expression.secondary_int_value);
    }
    case LN_BoolExpressionKind::FlowRouterLastRoute: {
      if (expression.int_value < 0 || expression.secondary_int_value < 0) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      return state != nullptr &&
             state->last_route_index == uint32_t(expression.secondary_int_value);
    }
    case LN_BoolExpressionKind::RepeatLimitAccepted:
    case LN_BoolExpressionKind::RepeatLimitBlocked: {
      if (expression.int_value < 0 || context.execution_scope_serial == 0) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      if (state == nullptr) {
        return fallback;
      }
      return expression.kind == LN_BoolExpressionKind::RepeatLimitAccepted ?
                 state->accepted_scope_serial == context.execution_scope_serial :
                 state->blocked_scope_serial == context.execution_scope_serial;
    }
    case LN_BoolExpressionKind::RepeatLimitExhausted: {
      if (expression.int_value < 0 || expression.input0 == LN_INVALID_INDEX) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      if (state == nullptr) {
        return fallback;
      }
      const int32_t requested_limit = in.Int(expression.input0, 0);
      const uint32_t limit = requested_limit > 0 ? uint32_t(requested_limit) : 0u;
      return state->accepted_count >= limit;
    }
    case LN_BoolExpressionKind::LoopActive: {
      const uint32_t loop_frame_index = uint32_t(expression.int_value);
      return m_activeLoopBodyFrame == loop_frame_index;
    }
    case LN_BoolExpressionKind::ValueChanged:
    case LN_BoolExpressionKind::ValueChangedTo: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      if (state.last_update_tick == context.tick_index) {
        return state.cached_result;
      }

      const LN_Value value = in.Value(expression.input0, MakeNoneValue());
      ValueChangeState &values = ValueChangeStateOf(expression_index);
      if (!values.has_previous_value) {
        if (expression.bool_value && (!value.exists || value.type == LN_ValueType::None)) {
          state.cached_result = false;
          state.last_update_tick = context.tick_index;
          return false;
        }
        values.previous_value = value;
        values.has_previous_value = true;
        values.stored_old_value = value;
        values.stored_new_value = value;
        state.cached_result = false;
        state.last_update_tick = context.tick_index;
        return false;
      }

      const bool changed = !ValuesEqual(value, values.previous_value);
      values.stored_old_value = values.previous_value;
      values.stored_new_value = value;
      values.previous_value = value;

      if (expression.kind == LN_BoolExpressionKind::ValueChanged) {
        state.cached_result = changed;
        state.last_update_tick = context.tick_index;
        return state.cached_result;
      }

      const LN_Value target = in.Value(expression.input1, MakeNoneValue());
      state.cached_result = changed && ValueChangedToTarget(values.stored_old_value, value, target);
      state.last_update_tick = context.tick_index;
      return state.cached_result;
    }
    case LN_BoolExpressionKind::Count:
      break;
  }

  return fallback;
}

template<class In>
int32_t LN_RuntimeTree::IntKind(In &in,
    const uint32_t expression_index,
    const LN_IntExpression &expression,
    const int32_t fallback)
{
  const LN_TickContext &context = in.context;
  switch (expression.kind) {
    case LN_IntExpressionKind::Constant:
      return expression.int_value;
    case LN_IntExpressionKind::SnapshotGameProperty: {
      LN_Snapshot::GamePropertyView view;
      if (m_snapshot.ReadGameProperty(expression.property_ref_index, view)) {
        return view.int_value;
      }
      return fallback;
    }
    case LN_IntExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return value->int_value;
      }
      return fallback;
    case LN_IntExpressionKind::StringCount: {
      /* Read order (D20): the second input first. */
      const std::string search = in.String(expression.input1, "");
      const std::string text = in.String(expression.input0, "");
      return CountStringOccurrences(text, search);
    }
    case LN_IntExpressionKind::SnapshotCollisionGroup:
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                in.Value(expression.input0, MakeNoneValue())))
        {
          LN_ObjectSnapshot other_object;
          return LN_CaptureObjectSnapshot(
                     game_object, LN_SNAPSHOT_CHANNEL_COLLISION, other_object)
              .collision_group;
        }
        return fallback;
      }
      return m_snapshot.GetCollisionGroup();
    case LN_IntExpressionKind::MouseWheelDelta:
      return (expression.int_value != 0 ? m_snapshot.GetRawInputSnapshot() :
                                          m_snapshot.GetInputSnapshot())
          .GetMouse()
          .wheel_delta;
    case LN_IntExpressionKind::PlayerJoinUser:
      return m_logicManager != nullptr && m_logicManager->IsPlayerJoinActive() ?
                 m_logicManager->PlayerJoinUser() :
                 fallback;
    case LN_IntExpressionKind::PlayerJoinGamepad:
      return m_logicManager != nullptr && m_logicManager->IsPlayerJoinActive() ?
                 m_logicManager->PlayerJoinGamepad() :
                 fallback;
    case LN_IntExpressionKind::PlayerGamepad:
      return LN_InputMap::Get().GamepadOfUser(in.Int(expression.input0, 0));
    case LN_IntExpressionKind::MenuPlayerUser:
      return m_logicManager != nullptr && m_gameObject != nullptr ?
                 m_logicManager->GetUIService().UserOfObject(m_gameObject) :
                 0;
    case LN_IntExpressionKind::PlayerViewCount:
      if (KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr) {
        return scene->GetPlayerViews().GetViewCount(scene->GetActiveCamera());
      }
      return fallback;
    case LN_IntExpressionKind::WindowResolutionWidth:
      if (RAS_ICanvas *canvas = ActiveCanvas()) {
        return canvas->GetWidth();
      }
      return fallback;
    case LN_IntExpressionKind::WindowResolutionHeight:
      if (RAS_ICanvas *canvas = ActiveCanvas()) {
        return canvas->GetHeight();
      }
      return fallback;
    case LN_IntExpressionKind::WindowVSyncMode: {
      int32_t mode = VSYNC_OFF;
      if (ActiveCanvasVSyncMode(mode)) {
        return mode;
      }
      return fallback;
    }
    case LN_IntExpressionKind::SnapshotCharacterMaxJumps: {
      std::optional<LN_ObjectSnapshot> other_object;
      const LN_ObjectSnapshot *object_snapshot = &m_snapshot.GetObjectSnapshot();
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                in.Value(expression.input0, MakeNoneValue())))
        {
          object_snapshot = &LN_CaptureObjectSnapshot(
              game_object, LN_SNAPSHOT_CHANNEL_CHARACTER, other_object.emplace());
        }
        else {
          return fallback;
        }
      }
      return object_snapshot->has_character ? object_snapshot->character_max_jumps : fallback;
    }
    case LN_IntExpressionKind::SnapshotCharacterJumpCount: {
      std::optional<LN_ObjectSnapshot> other_object;
      const LN_ObjectSnapshot *object_snapshot = &m_snapshot.GetObjectSnapshot();
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                in.Value(expression.input0, MakeNoneValue())))
        {
          object_snapshot = &LN_CaptureObjectSnapshot(
              game_object, LN_SNAPSHOT_CHANNEL_CHARACTER, other_object.emplace());
        }
        else {
          return fallback;
        }
      }
      return object_snapshot->has_character ? object_snapshot->character_jump_count : fallback;
    }
    case LN_IntExpressionKind::ListLength: {
      const LN_Value list_value = in.Value(expression.input0, MakeNoneValue());
      if (list_value.type == LN_ValueType::List && list_value.exists) {
        return int32_t(list_value.list_value.size());
      }
      return 0;
    }
    case LN_IntExpressionKind::DictLength: {
      const LN_Value dict_value = in.Value(expression.input0, MakeNoneValue());
      if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
        return int32_t(dict_value.dict_value.size());
      }
      return 0;
    }
    case LN_IntExpressionKind::FromGenericValue: {
      const LN_Value value = in.Value(expression.input0, MakeNoneValue());
      return ValueAsInt(value, expression.float_to_int_mode);
    }
    case LN_IntExpressionKind::FromBool:
      return in.Bool(expression.input0, false) ? 1 : 0;
    case LN_IntExpressionKind::FromFloat:
      return LN_ConvertFloatToInt(in.Float(expression.input0, 0.0f),
                                  expression.float_to_int_mode);
    case LN_IntExpressionKind::FromString: {
      const std::string text = in.String(expression.input0, std::string());
      float value = 0.0f;
      return LN_TryParseTypecastFloat(text, value) ?
                 LN_ConvertFloatToInt(value, expression.float_to_int_mode) :
                 0;
    }
    case LN_IntExpressionKind::Random: {
      const int32_t min_value = in.Int(expression.input0, 0);
      const int32_t max_value = in.Int(expression.input1, 1);
      const int32_t lower = std::min(min_value, max_value);
      const int32_t upper = std::max(min_value, max_value);
      const uint64_t seed = (uint64_t(expression_index) << 32) ^ context.tick_index;
      return lower + int32_t(HashToUnitFloat(seed) * float(upper - lower + 1));
    }
    case LN_IntExpressionKind::MaterialSlotCount: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      blender::Object *object = target_object ? target_object->GetBlenderObject() : nullptr;
      return object ? int32_t(object->totcol) : fallback;
    }
    case LN_IntExpressionKind::CollisionContactCount:
      return ResolveCollisionResult(in, expression.input0, expression.input1, expression.input2, CollisionResult::Detail::Contacts)
          .contact_count;
    case LN_IntExpressionKind::PhysicsQueryFaceIndex:
      return in.Query(expression.input0).physics_query.polygon_index;
    case LN_IntExpressionKind::PhysicsQueryHitCount: {
      const std::vector<LN_PhysicsQueryResult> &results =
          in.Query(expression.input0).physics_query_results;
      return int32_t(std::count_if(
          results.begin(), results.end(), [](const auto &result) { return result.hit; }));
    }
    case LN_IntExpressionKind::PhysicsQueryRayCount:
      return int32_t(
          in.Query(expression.input0).physics_query_results.size());
    case LN_IntExpressionKind::QueryVectorCount:
      return in.Query(expression.input0).vector_count;
    case LN_IntExpressionKind::UIFirstVisibleIndex:
      return in.Query(expression.input0).ui_first_visible_index;
    case LN_IntExpressionKind::UILastVisibleIndex:
      return in.Query(expression.input0).ui_last_visible_index;
    case LN_IntExpressionKind::UIElementEventIndex:
      return in.Query(expression.input0).ui_index;
    case LN_IntExpressionKind::UIElementEventTargetIndex:
      return in.Query(expression.input0).ui_target_index;
    case LN_IntExpressionKind::UIElementEventUser:
      return in.Query(expression.input0).ui_user;
    case LN_IntExpressionKind::UIElementEventAmount:
      return in.Query(expression.input0).ui_amount;
    case LN_IntExpressionKind::UIViewPage:
      return in.Query(expression.input0).ui_page;
    case LN_IntExpressionKind::UIViewPageCount:
      return in.Query(expression.input0).ui_page_count;
    case LN_IntExpressionKind::UICommandResult:
    case LN_IntExpressionKind::UICommandHandle: {
      if (expression.input0 == LN_INVALID_INDEX ||
          expression.input0 >= m_program->GetBoolExpressions().size() ||
          m_program->GetBoolExpressions()[expression.input0].kind !=
              LN_BoolExpressionKind::UICommandCompleted)
      {
        return fallback;
      }
      /* Advance a pending completion for this tick when the value is consumed without a connected
       * success/failure flow, then retain the last completed value for later commands. */
      in.Bool(expression.input0, false);
      const UICommandCompletionState &state = m_uiCommandCompletions[expression.input0];
      if (!state.has_value) {
        return fallback;
      }
      return expression.kind == LN_IntExpressionKind::UICommandHandle ?
                 state.current.handle :
                 int32_t(state.current.result);
    }
    case LN_IntExpressionKind::ConstraintBreakCount:
      return ResolveRigidBodyConstraintBreakEventCount(in, expression.input0, expression.input1, expression.input2, expression.rigid_body_constraint_match_mode);
    case LN_IntExpressionKind::LoopIndex: {
      const uint32_t loop_frame_index = uint32_t(expression.int_value);
      if (loop_frame_index >= m_loopStates.size()) {
        return fallback;
      }
      const LoopRuntimeState &state = m_loopStates[loop_frame_index];
      if (state.iteration_index == 0) {
        return 0;
      }
      return int32_t(state.iteration_index - 1);
    }
    case LN_IntExpressionKind::RepeatLimitRemaining: {
      if (expression.int_value < 0 || expression.input0 == LN_INVALID_INDEX) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      if (state == nullptr) {
        return fallback;
      }
      const int32_t requested_limit = in.Int(expression.input0, 0);
      const uint32_t limit = requested_limit > 0 ? uint32_t(requested_limit) : 0u;
      const uint32_t remaining = limit > state->accepted_count ? limit - state->accepted_count :
                                                                 0u;
      return remaining > uint32_t(std::numeric_limits<int32_t>::max()) ?
                 std::numeric_limits<int32_t>::max() :
                 int32_t(remaining);
    }
    case LN_IntExpressionKind::Count:
      break;
  }

  return fallback;
}

template<class In>
float LN_RuntimeTree::FloatKind(In &in,
    const uint32_t expression_index,
    const LN_FloatExpression &expression,
    const float fallback)
{
  const LN_TickContext &context = in.context;
  switch (expression.kind) {
    case LN_FloatExpressionKind::Constant:
      return expression.float_value;
    case LN_FloatExpressionKind::UnscaledDeltaTime:
      return float(std::max(0.0, context.unscaled_dt));
    case LN_FloatExpressionKind::StepTimePassed:
      /* Read from the gate, not the context: the same value wherever the output is read. */
      if (expression.int_value >= 0 && uint32_t(expression.int_value) < m_stepGates.size())
      {
        return float(m_stepGates[uint32_t(expression.int_value)].time_passed);
      }
      return float(m_eventDeltaTime);
    case LN_FloatExpressionKind::SnapshotGameProperty: {
      LN_Snapshot::GamePropertyView view;
      if (m_snapshot.ReadGameProperty(expression.property_ref_index, view)) {
        return view.float_value;
      }
      return fallback;
    }
    case LN_FloatExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return value->float_value;
      }
      return fallback;
    case LN_FloatExpressionKind::FromGenericValue: {
      const LN_Value value = in.Value(expression.input0, MakeNoneValue());
      float converted = 0.0f;
      if (ValueAsFloat(value, converted)) {
        return converted;
      }
      return fallback;
    }
    case LN_FloatExpressionKind::FromBool:
      return in.Bool(expression.bool_expr_index, false) ? 1.0f : 0.0f;
    case LN_FloatExpressionKind::FromInt:
      return float(in.Int(expression.input0, 0));
    case LN_FloatExpressionKind::FromString: {
      const std::string text = in.String(expression.string_expr_index, std::string());
      float value = 0.0f;
      return LN_TryParseTypecastFloat(text, value) ? value : 0.0f;
    }
    case LN_FloatExpressionKind::SnapshotTimeScale:
      return m_snapshot.GetTimeScale();
    case LN_FloatExpressionKind::SnapshotLightPower:
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                in.Value(expression.input0, MakeNoneValue())))
        {
          LN_ObjectSnapshot other_object;
          return LN_CaptureObjectSnapshot(game_object, LN_SNAPSHOT_CHANNEL_LIGHT, other_object)
              .light_power;
        }
        return fallback;
      }
      return m_snapshot.GetObjectSnapshot().light_power;
    case LN_FloatExpressionKind::SnapshotElapsedTime:
      return m_snapshot.GetElapsedTime();
    case LN_FloatExpressionKind::SnapshotFrameDelta:
      return m_snapshot.GetFrameDelta();
    case LN_FloatExpressionKind::SnapshotFPS:
      return m_snapshot.GetFPS();
    case LN_FloatExpressionKind::SnapshotDeltaFactor:
      return m_snapshot.GetDeltaFactor();
    case LN_FloatExpressionKind::AnimationFrame: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const int32_t layer = in.Int(expression.input1, 0);
      return target_object->GetActionFrame(short(layer));
    }
    case LN_FloatExpressionKind::BoneLength: {
      KX_GameObject *armature_object = nullptr;
      /* An empty Object is the tree's own object, read from the tree (as the other nodes here
       * do): the snapshot's copy of it is set only when the program captures one, and a tree
       * reading nothing else got none, so its bone length was 0 (fixed 2026-10-31). */
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_gameObject;
      }
      else {
        const LN_Value armature_value = in.Value(expression.input0, MakeNoneValue());
        armature_object = ResolveObjectValue(armature_value);
      }
      if (armature_object == nullptr) {
        return fallback;
      }
      blender::Object *ob = armature_object->GetBlenderObject();
      if (ob == nullptr || ob->type != blender::OB_ARMATURE) {
        return fallback;
      }
      blender::bArmature *armature = reinterpret_cast<blender::bArmature *>(ob->data);
      if (armature == nullptr) {
        return fallback;
      }
      const std::string bone_name = in.String(expression.input1, std::string());
      if (bone_name.empty()) {
        return fallback;
      }
      blender::Bone *bone = BKE_armature_find_bone_name(armature, bone_name.c_str());
      if (bone == nullptr) {
        return fallback;
      }
      return bone->length;
    }
    case LN_FloatExpressionKind::Add: {
      /* Read order (D20): A, then B. */
      const float a = in.Float(expression.input0, 0.0f);
      const float b = in.Float(expression.input1, 0.0f);
      return a + b;
    }
    case LN_FloatExpressionKind::Subtract: {
      const float a = in.Float(expression.input0, 0.0f);
      const float b = in.Float(expression.input1, 0.0f);
      return a - b;
    }
    case LN_FloatExpressionKind::Multiply: {
      const float a = in.Float(expression.input0, 0.0f);
      const float b = in.Float(expression.input1, 0.0f);
      return a * b;
    }
    case LN_FloatExpressionKind::Divide: {
      const float divisor = in.Float(expression.input1, 0.0f);
      if (std::fabs(divisor) <= 1.0e-20f) {
        return 0.0f;
      }
      return in.Float(expression.input0, 0.0f) / divisor;
    }
    case LN_FloatExpressionKind::SafeDivide: {
      const float divisor = in.Float(expression.input1, 0.0f);
      return divisor == 0.0f ? 0.0f :
                               in.Float(expression.input0, 0.0f) / divisor;
    }
    case LN_FloatExpressionKind::Power: {
      /* Read order (D20): B (the second input) first, then A. */
      const float b = in.Float(expression.input1, 0.0f);
      const float a = in.Float(expression.input0, 0.0f);
      return std::pow(a, b);
    }
    case LN_FloatExpressionKind::Minimum: {
      const float b = in.Float(expression.input1, 0.0f);
      const float a = in.Float(expression.input0, 0.0f);
      return std::min(a, b);
    }
    case LN_FloatExpressionKind::Maximum: {
      const float b = in.Float(expression.input1, 0.0f);
      const float a = in.Float(expression.input0, 0.0f);
      return std::max(a, b);
    }
    case LN_FloatExpressionKind::Absolute:
      return std::fabs(in.Float(expression.input0, 0.0f));
    case LN_FloatExpressionKind::Sign: {
      const float value = in.Float(expression.input0, 0.0f);
      return float((value > 0.0f) - (value < 0.0f));
    }
    case LN_FloatExpressionKind::Round:
      return std::floor(in.Float(expression.input0, 0.0f) + 0.5f);
    case LN_FloatExpressionKind::Floor:
      return std::floor(in.Float(expression.input0, 0.0f));
    case LN_FloatExpressionKind::Ceil:
      return std::ceil(in.Float(expression.input0, 0.0f));
    case LN_FloatExpressionKind::Truncate:
      return std::trunc(in.Float(expression.input0, 0.0f));
    case LN_FloatExpressionKind::Fraction: {
      const float value = in.Float(expression.input0, 0.0f);
      return value - std::floor(value);
    }
    case LN_FloatExpressionKind::Modulo: {
      const float divisor = in.Float(expression.input1, 0.0f);
      if (std::fabs(divisor) <= 1.0e-20f) {
        return 0.0f;
      }
      return std::fmod(in.Float(expression.input0, 0.0f), divisor);
    }
    case LN_FloatExpressionKind::Sine:
      return std::sin(in.Float(expression.input0, 0.0f));
    case LN_FloatExpressionKind::Cosine:
      return std::cos(in.Float(expression.input0, 0.0f));
    case LN_FloatExpressionKind::Radians:
      return in.Float(expression.input0, 0.0f) * (LN_PI / 180.0f);
    case LN_FloatExpressionKind::Degrees:
      return in.Float(expression.input0, 0.0f) * (180.0f / LN_PI);
    case LN_FloatExpressionKind::Negate:
      return -in.Float(expression.input0, 0.0f);
    case LN_FloatExpressionKind::Clamp: {
      const float value = in.Float(expression.input0, 0.0f);
      const float min_value = in.Float(expression.input1, 0.0f);
      const float max_value = in.Float(expression.input2, 1.0f);
      const float lower = std::min(min_value, max_value);
      const float upper = std::max(min_value, max_value);
      return std::min(std::max(value, lower), upper);
    }
    case LN_FloatExpressionKind::Threshold: {
      const float value = in.Float(expression.input0, 0.0f);
      const float threshold = in.Float(expression.input1, 0.0f);
      const bool else_zero = in.Bool(expression.bool_expr_index, expression.bool_value);
      switch (expression.threshold_operation) {
        case LN_ThresholdOperation::Greater:
          return (value > threshold) ? value : (else_zero ? 0.0f : threshold);
        case LN_ThresholdOperation::Less:
          return (value < threshold) ? value : (else_zero ? 0.0f : threshold);
      }
      break;
    }
    case LN_FloatExpressionKind::RangedThreshold: {
      const float value = in.Float(expression.input0, 0.0f);
      const float min_value = in.Float(expression.input1, 0.0f);
      const float max_value = in.Float(expression.input2, 0.0f);
      switch (expression.range_operation) {
        case LN_RangeOperation::Inside:
          return (min_value < value && value < max_value) ? value : 0.0f;
        case LN_RangeOperation::Outside:
          return (value < min_value || value > max_value) ? value : 0.0f;
      }
      break;
    }
    case LN_FloatExpressionKind::VectorComponent: {
      const MT_Vector3 vector = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      switch (expression.component_index) {
        case 0:
          return vector.x();
        case 1:
          return vector.y();
        case 2:
          return vector.z();
      }
      break;
    }
    case LN_FloatExpressionKind::ColorComponent: {
      const MT_Vector4 color = in.Color(expression.input0, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f));
      switch (expression.component_index) {
        case 0:
          return color.x();
        case 1:
          return color.y();
        case 2:
          return color.z();
        case 3:
          return color.w();
      }
      break;
    }
    case LN_FloatExpressionKind::Random: {
      const float min_value = in.Float(expression.input0, 0.0f);
      const float max_value = in.Float(expression.input1, 1.0f);
      const float lower = std::min(min_value, max_value);
      const float upper = std::max(min_value, max_value);
      const uint64_t seed = (uint64_t(expression_index) << 32) ^ context.tick_index;
      return lower + (upper - lower) * HashToUnitFloat(seed);
    }
    case LN_FloatExpressionKind::Select: {
      const bool condition = in.Bool(expression.bool_expr_index, expression.bool_value);
      return condition ?
                 in.Float(expression.input0, expression.float_value) :
                 in.Float(expression.input1, expression.float_value);
    }
    case LN_FloatExpressionKind::StoreValue: {
      /* The stored value. Storing happens when Flow arrives (StoreValueDone, on the node's Done
       * route), not when the value is read: a read in another tick or event must not store. */
      FloatExpressionRuntimeState &state = FloatExpressionState(expression_index);
      if (!state.initialized) {
        state.initialized = true;
        if (expression.bool_value) {
          state.stored_value = in.Float(expression.input0, expression.float_value);
          state.last_store_tick = context.tick_index;
        }
      }
      return state.stored_value;
    }
    case LN_FloatExpressionKind::Formula: {
      const std::string formula = in.String(expression.string_expr_index, "a + b");
      const float a = in.Float(expression.input0, 0.0f);
      const float b = in.Float(expression.input1, 0.0f);
      return LN::EvaluateFormula(formula, a, b);
    }
    case LN_FloatExpressionKind::FloatCurve: {
      const float input = in.Float(expression.input0, 1.0f);
      const float influence = in.Float(expression.input1, 1.0f);
      const uint32_t table_index = expression.int_value >= 0 ? uint32_t(expression.int_value) :
                                                               LN_INVALID_INDEX;
      return EvaluateCurveTable(table_index, input, influence);
    }
    case LN_FloatExpressionKind::TweenFactor:
      return ResolveTweenMappedFactor(in, expression.input0);
    case LN_FloatExpressionKind::TweenFloatResult: {
      const float tween_factor = ResolveTweenMappedFactor(in, expression.input0);
      const float from_value = in.Float(expression.input1, 0.0f);
      const float to_value = in.Float(expression.input2, 1.0f);
      return from_value + (to_value - from_value) * tween_factor;
    }
    case LN_FloatExpressionKind::GamepadButtonStrength: {
      const int32_t gamepad_index = in.Int(expression.input0, 0);
      const LN_GamepadSnapshot *gamepad = m_snapshot.GetInputSnapshot().GetGamepad(gamepad_index);
      float strength = 0.0f;
      if (gamepad == nullptr ||
          !LN_TryGetGamepadTriggerStrength(*gamepad, int32_t(expression.input2), strength))
      {
        return fallback;
      }
      return strength;
    }
    case LN_FloatExpressionKind::PlayerViewArea: {
      KX_PlayerViews::Area area;
      if (!ResolvePlayerViewArea(
              m_gameObject, in.Int(expression.input0, 0), area))
      {
        return 0.0f;
      }
      const float components[] = {area.left, area.top, area.width, area.height};
      return components[std::min<uint8_t>(expression.component_index, 3)];
    }
    case LN_FloatExpressionKind::InputActionValue: {
      /* Read order (D20): the player (Int input 0), then the action name. */
      const int32_t user = in.Int(expression.input0, 0);
      const std::string action_name = in.String(expression.string_expr_index, std::string());
      const LN_InputActionState *state = m_snapshot.GetInputActionState(
          LN_InputMap::Get().Find(action_name), user, expression.bool_value);
      return state != nullptr ? state->value : 0.0f;
    }
    case LN_FloatExpressionKind::ObjectDistance: {
      KX_GameObject *object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      KX_GameObject *target = nullptr;
      if (expression.input1 != LN_INVALID_INDEX) {
        target = ResolveObjectValue(
            in.Value(expression.input1, MakeNoneValue()));
      }
      if (object == nullptr || target == nullptr) {
        return fallback;
      }
      return (object->NodeGetWorldPosition() - target->NodeGetWorldPosition()).length();
    }
    case LN_FloatExpressionKind::PhysicsQueryDistance:
      return in.Query(expression.input0).physics_query.hit_distance;
    case LN_FloatExpressionKind::PhysicsQueryFraction:
      return in.Query(expression.input0).physics_query.hit_fraction;
    case LN_FloatExpressionKind::PhysicsQueryPenetrationDepth:
      return in.Query(expression.input0).physics_query.penetration_depth;
    case LN_FloatExpressionKind::QueryDistance:
      return in.Query(expression.input0).distance;
    case LN_FloatExpressionKind::QuerySquaredDistance:
      return in.Query(expression.input0).squared_distance;
    case LN_FloatExpressionKind::MotionSpeed: {
      if (expression.int_value < 0) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      return state != nullptr ? state->motion_speed : fallback;
    }
    case LN_FloatExpressionKind::RigidBodyAttribute: {
      PHY_IPhysicsController *controller = ResolveRigidBodyAttributeController(in, expression.input0);
      if (controller == nullptr) {
        return 0.0f;
      }
      return ReadRigidBodyFloatAttribute(
          *controller,
          static_cast<LN_RigidBodyAttribute>(expression.property_ref_index),
          expression.component_index);
    }
    case LN_FloatExpressionKind::ConstraintBreakLinearImpulse:
    case LN_FloatExpressionKind::ConstraintBreakAngularImpulse:
    case LN_FloatExpressionKind::ConstraintBreakAppliedImpulse: {
      const LN_RigidBodyConstraintBreakEvent *event = ResolveRigidBodyConstraintBreakEvent(in, expression.input0, expression.input1, expression.input2, expression.rigid_body_constraint_match_mode);
      if (event == nullptr) {
        return fallback;
      }
      switch (expression.kind) {
        case LN_FloatExpressionKind::ConstraintBreakLinearImpulse:
          return event->linear_impulse;
        case LN_FloatExpressionKind::ConstraintBreakAngularImpulse:
          return event->angular_impulse;
        case LN_FloatExpressionKind::ConstraintBreakAppliedImpulse:
          return event->applied_impulse;
        default:
          break;
      }
      return fallback;
    }
    case LN_FloatExpressionKind::CooldownRemaining: {
      if (expression.int_value < 0) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr ? float(std::max(0.0, state->remaining_time)) : fallback;
    }
    case LN_FloatExpressionKind::CooldownProgress: {
      if (expression.int_value < 0) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      if (state == nullptr) {
        return fallback;
      }
      if (!state->active || state->accumulated_time <= 0.0) {
        return 1.0f;
      }
      const double progress = 1.0 - state->remaining_time / state->accumulated_time;
      return float(std::clamp(progress, 0.0, 1.0));
    }
    case LN_FloatExpressionKind::Count:
      break;
  }

  return fallback;
}

template<class In>
std::string LN_RuntimeTree::StringKind(In &in,
    const uint32_t expression_index,
    const LN_StringExpression &expression,
    const std::string &fallback)
{
  const LN_TickContext &context = in.context;
  switch (expression.kind) {
    case LN_StringExpressionKind::Constant:
      return expression.string_value;
    case LN_StringExpressionKind::SnapshotGameProperty: {
      LN_Snapshot::GamePropertyView view;
      if (m_snapshot.ReadGameProperty(expression.property_ref_index, view)) {
        return *view.string_value;
      }
      return fallback;
    }
    case LN_StringExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return value->string_value;
      }
      return fallback;
    case LN_StringExpressionKind::FromGenericValue: {
      if (FastObjectAttribute(expression.value_expr_index) == LN_ObjectAttributeId::Name) {
        /* Plan G3: an object's name straight into the text, without its value (no object: the
         * text of no value, empty). */
        KX_GameObject *target = TypedAttributeTarget(in, expression.value_expr_index);
        std::string name = target != nullptr ? target->GetName() : std::string();
        if (fast_attributes_mode() == FastAttributesMode::Verify &&
            ValueToText(in.Value(expression.value_expr_index, MakeNoneValue())) != name)
        {
          NoteFastAttributeDifference("a name read is not its value's");
        }
        return name;
      }
      const LN_Value value = in.Value(expression.value_expr_index, MakeNoneValue());
      return ValueToText(value);
    }
    case LN_StringExpressionKind::FromBool:
      return in.Bool(expression.input0, false) ? "True" : "False";
    case LN_StringExpressionKind::UIElementEventAction:
      return in.Query(expression.input0).ui_action;
    case LN_StringExpressionKind::UIElementEventTargetAction:
      return in.Query(expression.input0).ui_target_action;
    case LN_StringExpressionKind::UIElementEventOutcome:
      return in.Query(expression.input0).ui_outcome;
    case LN_StringExpressionKind::UIElementEventMenu:
      return in.Query(expression.input0).ui_menu;
    case LN_StringExpressionKind::FromInt:
      return std::to_string(in.Int(expression.input0, 0));
    case LN_StringExpressionKind::FromFloat:
      return LN_TypecastFloatToString(in.Float(expression.input0, 0.0f));
    case LN_StringExpressionKind::FromVector:
      return LN_TypecastVectorToString(
          in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f)));
    case LN_StringExpressionKind::FromColor:
      return LN_TypecastColorToString(
          in.Color(expression.input0, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f)));
    case LN_StringExpressionKind::Join: {
      /* Read order (D20): the second input first. */
      const std::string b = in.String(expression.input1, "");
      const std::string a = in.String(expression.input0, "");
      return a + b;
    }
    case LN_StringExpressionKind::Replace: {
      /* Read order (D20): the last input first. */
      const std::string replacement = in.String(expression.input2, "");
      const std::string search = in.String(expression.input1, "");
      const std::string text = in.String(expression.input0, "");
      return ReplaceStringOccurrences(text, search, replacement);
    }
    case LN_StringExpressionKind::ToUppercase:
      return ToCaseString(in.String(expression.input0, ""), true);
    case LN_StringExpressionKind::ToLowercase:
      return ToCaseString(in.String(expression.input0, ""), false);
    case LN_StringExpressionKind::ZeroFill: {
      /* Read order (D20): the width first, then the text. */
      const int32_t width = in.Int(expression.int_expr_index, 0);
      const std::string text = in.String(expression.input0, "");
      return ZeroFillString(text, width);
    }
    case LN_StringExpressionKind::Format: {
      /* Read order (D20): the last input first. */
      const std::string slot4 = in.String(expression.input4, "");
      const std::string slot3 = in.String(expression.input3, "");
      const std::string slot2 = in.String(expression.input2, "");
      const std::string slot1 = in.String(expression.input1, "");
      const std::string format = in.String(expression.input0, "");
      return FormatStringSlots(format, slot1, slot2, slot3, slot4);
    }
    case LN_StringExpressionKind::KeyLoggerCharacter: {
      const bool only_characters = expression.int_expr_index != LN_INVALID_INDEX ?
                                       in.Int(expression.int_expr_index, 1) != 0 :
                                       true;
      const KeyLoggerScanResult scan = ScanKeyLogger(m_snapshot.GetInputSnapshot(),
                                                     only_characters);
      return scan.character;
    }
    case LN_StringExpressionKind::KeyLoggerKeycode: {
      const bool only_characters = expression.int_expr_index != LN_INVALID_INDEX ?
                                       in.Int(expression.int_expr_index, 1) != 0 :
                                       true;
      const KeyLoggerScanResult scan = ScanKeyLogger(m_snapshot.GetInputSnapshot(),
                                                     only_characters);
      return scan.keycode;
    }
    case LN_StringExpressionKind::InputBindingText: {
      const LN_InputMap &map = LN_InputMap::Get();
      /* Read order (D20): Player (input 2), Device (input 1), then the action name (input 0). */
      const int32_t user = in.Int(expression.input2, 0);
      const int32_t device = in.Int(expression.input1, 0);
      const std::string action_name = in.String(expression.input0, std::string());
      /* Player 0's, like Automatic: their gamepad's own button names. */
      return map.ActionText(
          map.Find(action_name),
          LN_InputDeviceClass(std::clamp(device,
                                         int32_t(LN_InputDeviceClass::Automatic),
                                         int32_t(LN_InputDeviceClass::Gamepad))),
          user,
          map.PadFamilyOfUser(0));
    }
    case LN_StringExpressionKind::AnimationActionName: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const int32_t layer = in.Int(expression.int_expr_index, 0);
      return target_object->GetActionName(short(layer));
    }
    case LN_StringExpressionKind::MasterFolder: {
      const std::string folder_name = in.String(expression.input0, std::string());
      std::filesystem::path path(KX_GetMainPath());
      if (path.empty()) {
        path = std::filesystem::current_path();
      }
      if (std::filesystem::is_regular_file(path)) {
        path = path.parent_path();
      }
      if (folder_name.empty()) {
        return path.string();
      }
      for (std::filesystem::path current = path; !current.empty(); current = current.parent_path())
      {
        if (current.filename() == folder_name) {
          return current.string();
        }
        if (current == current.parent_path()) {
          break;
        }
      }
      return fallback;
    }
    case LN_StringExpressionKind::ObjectID: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object != nullptr) {
        return game_object_id_name(target_object);
      }
      return fallback;
    }
    case LN_StringExpressionKind::MaterialName: {
      blender::Material *material = ResolveMaterialByValue(
          this, in.Value(expression.input0, MakeNoneValue()));
      return material ? std::string(material->id.name + 2) : fallback;
    }
    case LN_StringExpressionKind::ConstraintBreakName:
      if (const LN_RigidBodyConstraintBreakEvent *event = ResolveRigidBodyConstraintBreakEvent(in, expression.input0, expression.input1, expression.input2, expression.rigid_body_constraint_match_mode))
      {
        return event->constraint_name;
      }
      return fallback;
    case LN_StringExpressionKind::RigidBodyConstraintName: {
      KX_GameObject *target = m_gameObject;
      if (expression.value_expr_index != LN_INVALID_INDEX) {
        target = ResolveObjectValue(
            in.Value(expression.value_expr_index, MakeNoneValue()));
      }
      if (!UsesJoltPhysics(target)) {
        return std::string();
      }
      const LN_RigidBodyConstraintMatchMode match_mode = ValidRigidBodyConstraintMatchMode(
          expression.rigid_body_constraint_match_mode);
      const std::string query = match_mode == LN_RigidBodyConstraintMatchMode::All ?
                                    std::string() :
                                    in.String(expression.input0, std::string());
      const KX_GameObject::RigidBodyConstraintData *constraint = FindRigidBodyConstraintMatch(
          *target, query, match_mode);
      return constraint ? constraint->m_name : std::string();
    }
    case LN_StringExpressionKind::Count:
      break;
  }

  return fallback;
}

template<class In>
MT_Vector3 LN_RuntimeTree::VectorKind(In &in,
    const uint32_t expression_index,
    const LN_VectorExpression &expression,
    const MT_Vector3 &fallback)
{
  const LN_TickContext &context = in.context;
  /* Built only when a character kind reads another object (every other kind reads the owner). */
  std::optional<LN_ObjectSnapshot> other_object;
  const LN_ObjectSnapshot *object_snapshot = &m_snapshot.GetObjectSnapshot();
  auto resolve_snapshot_object = [&]() -> const LN_ObjectSnapshot * {
    if (expression.input0 == LN_INVALID_INDEX) {
      return object_snapshot;
    }
    if (KX_GameObject *game_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue())))
    {
      object_snapshot = &LN_CaptureObjectSnapshot(
          game_object, LN_SNAPSHOT_CHANNEL_CHARACTER, other_object.emplace());
      return object_snapshot;
    }
    return nullptr;
  };
  switch (expression.kind) {
    case LN_VectorExpressionKind::Constant:
      return expression.vector_value;
    case LN_VectorExpressionKind::SnapshotGravity:
      return m_snapshot.GetGravity();
    case LN_VectorExpressionKind::SnapshotCharacterGravity: {
      if (const LN_ObjectSnapshot *resolved_object_snapshot = resolve_snapshot_object()) {
        return resolved_object_snapshot->has_character ?
                   resolved_object_snapshot->character_gravity :
                   fallback;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::SnapshotCharacterWalkDirection: {
      if (const LN_ObjectSnapshot *resolved_object_snapshot = resolve_snapshot_object()) {
        return resolved_object_snapshot->has_character ?
                   resolved_object_snapshot->character_walk_direction_world :
                   fallback;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::SnapshotCharacterLocalWalkDirection: {
      if (const LN_ObjectSnapshot *resolved_object_snapshot = resolve_snapshot_object()) {
        return resolved_object_snapshot->has_character ?
                   resolved_object_snapshot->character_walk_direction_local :
                   fallback;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::WindowResolution:
      if (RAS_ICanvas *canvas = ActiveCanvas()) {
        return MT_Vector3(float(canvas->GetWidth()), float(canvas->GetHeight()), 0.0f);
      }
      return fallback;
    case LN_VectorExpressionKind::WorldToScreen: {
      const LN_Value camera_value = in.Value(expression.input1, MakeNoneValue());
      KX_Camera *camera = ResolveCameraValue(camera_value);
      MT_Vector3 screen_position;
      if (ProjectWorldToScreen(
              camera,
              in.Vector(expression.input0, expression.vector_value),
              screen_position))
      {
        return screen_position;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::BoneHeadWorld: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_gameObject;
      }
      else {
        const LN_Value armature_value = in.Value(expression.input0, MakeNoneValue());
        armature_object = ResolveObjectValue(armature_value);
      }
      if (armature_object == nullptr) {
        return fallback;
      }
      blender::Object *ob = armature_object->GetBlenderObject();
      if (ob == nullptr || ob->type != blender::OB_ARMATURE) {
        return fallback;
      }
      blender::bArmature *armature = reinterpret_cast<blender::bArmature *>(ob->data);
      if (armature == nullptr) {
        return fallback;
      }
      const std::string bone_name = in.String(expression.input1, std::string());
      if (bone_name.empty()) {
        return fallback;
      }
      blender::Bone *bone = BKE_armature_find_bone_name(armature, bone_name.c_str());
      if (bone == nullptr) {
        return fallback;
      }
      const MT_Transform world_xform = armature_object->NodeGetWorldTransform();
      return world_xform * MT_Vector3(bone->arm_head[0], bone->arm_head[1], bone->arm_head[2]);
    }
    case LN_VectorExpressionKind::BoneTailWorld: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_gameObject;
      }
      else {
        const LN_Value armature_value = in.Value(expression.input0, MakeNoneValue());
        armature_object = ResolveObjectValue(armature_value);
      }
      if (armature_object == nullptr) {
        return fallback;
      }
      blender::Object *ob = armature_object->GetBlenderObject();
      if (ob == nullptr || ob->type != blender::OB_ARMATURE) {
        return fallback;
      }
      blender::bArmature *armature = reinterpret_cast<blender::bArmature *>(ob->data);
      if (armature == nullptr) {
        return fallback;
      }
      const std::string bone_name = in.String(expression.input1, std::string());
      if (bone_name.empty()) {
        return fallback;
      }
      blender::Bone *bone = BKE_armature_find_bone_name(armature, bone_name.c_str());
      if (bone == nullptr) {
        return fallback;
      }
      const MT_Transform world_xform = armature_object->NodeGetWorldTransform();
      return world_xform * MT_Vector3(bone->arm_tail[0], bone->arm_tail[1], bone->arm_tail[2]);
    }
    case LN_VectorExpressionKind::BoneHeadPoseWorld:
    case LN_VectorExpressionKind::BoneTailPoseWorld:
    case LN_VectorExpressionKind::BoneCenterPoseWorld: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_gameObject;
      }
      else {
        const LN_Value armature_value = in.Value(expression.input0, MakeNoneValue());
        armature_object = ResolveObjectValue(armature_value);
      }
      const std::string bone_name = in.String(expression.input1, std::string());
      MT_Vector3 pose_position;
      BonePosePoint point = BonePosePoint::Head;
      if (expression.kind == LN_VectorExpressionKind::BoneTailPoseWorld) {
        point = BonePosePoint::Tail;
      }
      else if (expression.kind == LN_VectorExpressionKind::BoneCenterPoseWorld) {
        point = BonePosePoint::Center;
      }
      const LN_BonePosePositionSpace space = NormalizeBonePosePositionSpace(
          expression.property_ref_index);
      if (ResolveBonePosePosition(armature_object, bone_name, point, space, pose_position)) {
        return pose_position;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::BonePoseLocation:
    case LN_VectorExpressionKind::BonePoseScale: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_gameObject;
      }
      else {
        const LN_Value armature_value = in.Value(expression.input0, MakeNoneValue());
        armature_object = ResolveObjectValue(armature_value);
      }
      const std::string bone_name = in.String(expression.input1, std::string());
      MT_Vector3 value;
      if (expression.kind == LN_VectorExpressionKind::BonePoseScale) {
        return ResolveBonePoseScale(armature_object, bone_name, value) ? value : fallback;
      }
      const LN_BonePoseRotationSpace space = NormalizeBonePoseRotationSpace(
          expression.property_ref_index);
      return ResolveBonePoseLocation(armature_object, bone_name, space, value) ? value : fallback;
    }
    case LN_VectorExpressionKind::BoneCenterWorld: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_gameObject;
      }
      else {
        const LN_Value armature_value = in.Value(expression.input0, MakeNoneValue());
        armature_object = ResolveObjectValue(armature_value);
      }
      if (armature_object == nullptr) {
        return fallback;
      }
      blender::Object *ob = armature_object->GetBlenderObject();
      if (ob == nullptr || ob->type != blender::OB_ARMATURE) {
        return fallback;
      }
      blender::bArmature *armature = reinterpret_cast<blender::bArmature *>(ob->data);
      if (armature == nullptr) {
        return fallback;
      }
      const std::string bone_name = in.String(expression.input1, std::string());
      if (bone_name.empty()) {
        return fallback;
      }
      blender::Bone *bone = BKE_armature_find_bone_name(armature, bone_name.c_str());
      if (bone == nullptr) {
        return fallback;
      }
      const MT_Transform world_xform = armature_object->NodeGetWorldTransform();
      const MT_Vector3 head = world_xform *
                              MT_Vector3(bone->arm_head[0], bone->arm_head[1], bone->arm_head[2]);
      const MT_Vector3 tail = world_xform *
                              MT_Vector3(bone->arm_tail[0], bone->arm_tail[1], bone->arm_tail[2]);
      return (head + tail) * 0.5f;
    }
    case LN_VectorExpressionKind::ScreenToWorld: {
      const LN_Value camera_value = in.Value(expression.input0, MakeNoneValue());
      KX_Camera *camera = ResolveCameraValue(camera_value);
      MT_Vector3 world_position;
      /* Read order (D20): Z (the float input), Y (input 2), then X (input 1). */
      const float screen_z = in.Float(expression.float_expr_index, expression.vector_value.z());
      const float screen_y = in.Float(expression.input2, expression.vector_value.y());
      const float screen_x = in.Float(expression.input1, expression.vector_value.x());
      if (ProjectScreenToWorld(camera, screen_x, screen_y, screen_z, world_position))
      {
        return world_position;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return (value->type == LN_ValueType::Rotation) ? value->RotationEulerRadians() :
                                                         value->vector_value;
      }
      return fallback;
    case LN_VectorExpressionKind::FromGenericValue: {
      const LN_ObjectAttributeId attribute = FastObjectAttribute(expression.input0);
      if (attribute != LN_ObjectAttributeId::None) {
        /* Plan G3: the attribute straight into the vector, without its value. */
        MT_Vector3 vector = fallback;
        KX_GameObject *target = TypedAttributeTarget(in, expression.input0);
        if (target == nullptr || !ObjectAttributeAsVector(*target, attribute, vector)) {
          vector = fallback;
        }
        if (fast_attributes_mode() == FastAttributesMode::Verify) {
          MT_Vector3 expected;
          if (!ValueAsVector(in.Value(expression.input0, MakeNoneValue()), expected)) {
            expected = fallback;
          }
          if (LN_DumpVector3(expected) != LN_DumpVector3(vector)) {
            NoteFastAttributeDifference("a vector read is not its value's");
          }
        }
        return vector;
      }
      const LN_Value value = in.Value(expression.input0, MakeNoneValue());
      MT_Vector3 vector;
      if (ValueAsVector(value, vector)) {
        return vector;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::BroadcastFloat: {
      const float scalar = in.Float(expression.float_expr_index, 0.0f);
      return MT_Vector3(scalar, scalar, scalar);
    }
    case LN_VectorExpressionKind::BroadcastInt: {
      const float scalar = float(in.Int(expression.input0, 0));
      return MT_Vector3(scalar, scalar, scalar);
    }
    case LN_VectorExpressionKind::FromColor: {
      const MT_Vector4 color = in.Color(expression.input0, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f));
      return MT_Vector3(color.x(), color.y(), color.z());
    }
    case LN_VectorExpressionKind::CursorPosition: {
      const LN_MouseSnapshot &mouse = m_snapshot.GetInputSnapshot().GetMouse();
      float y = mouse.normalized_y;
      if (expression.property_ref_index != 0) {
        y = 1.0f - y;
      }
      return MT_Vector3(mouse.normalized_x, y, 0.0f);
    }
    case LN_VectorExpressionKind::CursorMovement: {
      const LN_MouseSnapshot &mouse = m_snapshot.GetInputSnapshot().GetMouse();
      return MT_Vector3(float(mouse.delta_x), float(mouse.delta_y), 0.0f);
    }
    case LN_VectorExpressionKind::GamepadStick: {
      const int32_t gamepad_index = in.Int(expression.input0, 0);
      const float threshold = std::max(0.0f,
                                       in.Float(expression.input1, 0.1f));
      const float sensitivity = in.Float(expression.float_expr_index, expression.float_value);
      const LN_GamepadSnapshot *gamepad = m_snapshot.GetInputSnapshot().GetGamepad(gamepad_index);
      if (gamepad == nullptr || !gamepad->connected) {
        return fallback;
      }

      const int32_t invert_flags = int32_t(expression.property_ref_index);
      const int32_t axis_offset = (expression.input2 == 1u) ? 2 : 0;
      float x = NormalizeGamepadAxisValue(gamepad->axes[size_t(axis_offset)]);
      float y = -NormalizeGamepadAxisValue(gamepad->axes[size_t(axis_offset + 1)]);
      if (invert_flags & 1) {
        x = -x;
      }
      if (invert_flags & 2) {
        y = -y;
      }
      if (std::fabs(x) < threshold) {
        x = 0.0f;
      }
      if (std::fabs(y) < threshold) {
        y = 0.0f;
      }
      return MT_Vector3(x * sensitivity, y * sensitivity, 0.0f);
    }
    case LN_VectorExpressionKind::InputActionVector: {
      /* Read order (D20): the player (input 0), then the action name (input 1). */
      const int32_t user = in.Int(expression.input0, 0);
      const std::string action_name = in.String(expression.input1, std::string());
      const LN_InputActionState *state = m_snapshot.GetInputActionState(
          LN_InputMap::Get().Find(action_name), user, expression.bool_value);
      return state != nullptr ? state->vector : MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    case LN_VectorExpressionKind::TweenVectorResult: {
      const float tween_factor = ResolveTweenMappedFactor(in, expression.input0);
      const MT_Vector3 from_value = in.Vector(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f));
      const MT_Vector3 to_value = in.Vector(expression.input2, MT_Vector3(1.0f, 1.0f, 1.0f));
      return from_value + (to_value - from_value) * tween_factor;
    }
    case LN_VectorExpressionKind::TweenRotationResult: {
      if (expression.property_ref_index != 0) {
        return in.Rotation(expression_index, LN_Rotation::Identity())
            .ToEulerXYZ();
      }
      const float tween_factor = ResolveTweenMappedFactor(in, expression.input0);
      const MT_Vector3 from_value = in.Vector(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f));
      const MT_Vector3 to_value = in.Vector(expression.input2, MT_Vector3(0.0f, 0.0f, 0.0f));
      return InterpolateRotationEuler(from_value, to_value, tween_factor);
    }
    case LN_VectorExpressionKind::PhysicsQueryPoint: {
      const LN_PhysicsQueryResult &query =
          in.Query(expression.input0).physics_query;
      return query.hit ? query.hit_position : MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    case LN_VectorExpressionKind::PhysicsQueryNormal: {
      const LN_PhysicsQueryResult &query =
          in.Query(expression.input0).physics_query;
      return query.hit ? query.hit_normal : MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    case LN_VectorExpressionKind::UIScrollOffset:
      return in.Query(expression.input0).ui_scroll_offset;
    case LN_VectorExpressionKind::UIScrollNormalized:
      return in.Query(expression.input0).ui_scroll_normalized;
    case LN_VectorExpressionKind::PhysicsQueryCastPosition:
      return in.Query(expression.input0).physics_query.cast_position;
    case LN_VectorExpressionKind::PhysicsQueryDirection:
      return in.Query(expression.input0).physics_query.ray_direction;
    case LN_VectorExpressionKind::PhysicsQueryEndPoint:
      return in.Query(expression.input0).physics_query.end_position;
    case LN_VectorExpressionKind::PhysicsQueryUV: {
      const MT_Vector2 uv =
          in.Query(expression.input0).physics_query.hit_uv;
      return MT_Vector3(uv.x(), uv.y(), 0.0f);
    }
    case LN_VectorExpressionKind::QueryVectorAverage:
      return in.Query(expression.input0).vector_average;
    case LN_VectorExpressionKind::MotionPosition:
    case LN_VectorExpressionKind::MotionVelocity:
    case LN_VectorExpressionKind::MotionDirection:
    case LN_VectorExpressionKind::MotionPredictedPosition: {
      if (expression.property_ref_index == LN_INVALID_INDEX) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(expression.property_ref_index);
      if (state == nullptr) {
        return fallback;
      }
      if (expression.kind == LN_VectorExpressionKind::MotionPosition) {
        return state->motion_position;
      }
      if (expression.kind == LN_VectorExpressionKind::MotionVelocity) {
        return state->motion_velocity;
      }
      if (expression.kind == LN_VectorExpressionKind::MotionDirection) {
        return state->motion_direction;
      }
      return state->motion_predicted_position;
    }
    case LN_VectorExpressionKind::Add: {
      /* Read order (D20): the second input first. */
      const MT_Vector3 b = in.Vector(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f));
      const MT_Vector3 a = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      return a + b;
    }
    case LN_VectorExpressionKind::Subtract: {
      const MT_Vector3 b = in.Vector(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f));
      const MT_Vector3 a = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      return a - b;
    }
    case LN_VectorExpressionKind::Multiply: {
      const MT_Vector3 a = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      const MT_Vector3 b = in.Vector(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f));
      return MT_Vector3(a.x() * b.x(), a.y() * b.y(), a.z() * b.z());
    }
    case LN_VectorExpressionKind::Divide: {
      const MT_Vector3 a = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      const MT_Vector3 b = in.Vector(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f));
      return MT_Vector3(std::fabs(b.x()) <= 1.0e-20f ? 0.0f : a.x() / b.x(),
                        std::fabs(b.y()) <= 1.0e-20f ? 0.0f : a.y() / b.y(),
                        std::fabs(b.z()) <= 1.0e-20f ? 0.0f : a.z() / b.z());
    }
    case LN_VectorExpressionKind::Absolute: {
      const MT_Vector3 vector = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      return MT_Vector3(std::fabs(vector.x()), std::fabs(vector.y()), std::fabs(vector.z()));
    }
    case LN_VectorExpressionKind::Minimum: {
      const MT_Vector3 a = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      const MT_Vector3 b = in.Vector(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f));
      return MT_Vector3(std::min(a.x(), b.x()), std::min(a.y(), b.y()), std::min(a.z(), b.z()));
    }
    case LN_VectorExpressionKind::Maximum: {
      const MT_Vector3 a = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      const MT_Vector3 b = in.Vector(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f));
      return MT_Vector3(std::max(a.x(), b.x()), std::max(a.y(), b.y()), std::max(a.z(), b.z()));
    }
    case LN_VectorExpressionKind::Scale: {
      /* Read order (D20): the factor first, then the vector. */
      const float factor = in.Float(expression.float_expr_index, expression.float_value);
      const MT_Vector3 vector = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      return vector * factor;
    }
    case LN_VectorExpressionKind::Normalize: {
      const MT_Vector3 vector = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      const float length = vector.length();
      return length <= 1.0e-20f ? MT_Vector3(0.0f, 0.0f, 0.0f) : vector * (1.0f / length);
    }
    case LN_VectorExpressionKind::Resize: {
      const MT_Vector3 vector = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      if (expression.float_value >= 3.0f) {
        return vector;
      }
      return MT_Vector3(vector.x(), vector.y(), 0.0f);
    }
    case LN_VectorExpressionKind::RotateAroundAxis: {
      const MT_Vector3 origin = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      const MT_Vector3 pivot = in.Vector(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f));
      const int32_t rotate_mode = int32_t(expression.property_ref_index) / 4;
      const int32_t rotate_axis = int32_t(expression.property_ref_index) % 4;
      if (rotate_mode == 3) {
        const LN_Rotation rotation = in.Rotation(expression.input2, LN_Rotation::Identity());
        return pivot + rotation.ToMatrix() * (origin - pivot);
      }
      MT_Vector3 axis(0.0f, 0.0f, 1.0f);
      if (rotate_mode == 1) {
        axis = rotate_axis == 0 ? MT_Vector3(1.0f, 0.0f, 0.0f) :
               rotate_axis == 1 ? MT_Vector3(0.0f, 1.0f, 0.0f) :
                                  MT_Vector3(0.0f, 0.0f, 1.0f);
      }
      else if (rotate_mode == 2) {
        axis = in.Vector(expression.input2, MT_Vector3(0.0f, 0.0f, 1.0f));
      }
      const float axis_length = axis.length();
      if (axis_length <= 1.0e-20f) {
        return origin;
      }
      axis *= 1.0f / axis_length;
      const float angle = in.Float(expression.float_expr_index, expression.float_value);
      const MT_Vector3 relative = origin - pivot;
      const float cosine = std::cos(angle);
      const float sine = std::sin(angle);
      return pivot + relative * cosine + axis.cross(relative) * sine +
             axis * (axis.dot(relative) * (1.0f - cosine));
    }
    case LN_VectorExpressionKind::VectorToRotation: {
      /* Read order (D20): the second input first. */
      const MT_Vector3 up = in.Vector(expression.input1, MT_Vector3(0.0f, 1.0f, 0.0f));
      const MT_Vector3 forward = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 1.0f));
      return VectorToRotationEuler(forward, up, int32_t(expression.property_ref_index));
    }
    case LN_VectorExpressionKind::BoneRotationToObjectRotation:
      return LN_BoneRotationToObjectRotation(
                 in.Rotation(expression.input0, LN_Rotation::Identity()))
          .ToEulerXYZ();
    case LN_VectorExpressionKind::AxisVector: {
      /* The own object gives its orientation from the tick's snapshot (the pose at the start of
       * the step: plan phase A15, decision D1 e; the register path always did, the tree path read
       * the live pose, so which one ran decided the answer). Another object gives its live pose. */
      MT_Matrix3x3 orientation;
      if (expression.input0 == LN_INVALID_INDEX) {
        if (m_gameObject == nullptr) {
          return fallback;
        }
        orientation = m_snapshot.GetObjectSnapshot().world_orientation.ToMatrix();
      }
      else {
        KX_GameObject *target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
        if (target_object == nullptr) {
          return fallback;
        }
        orientation = target_object->NodeGetWorldOrientation();
      }
      const int32_t axis = int32_t(expression.property_ref_index);
      MT_Vector3 vector = axis == 0 || axis == 3 ? orientation * MT_Vector3(1.0f, 0.0f, 0.0f) :
                          axis == 1 || axis == 4 ? orientation * MT_Vector3(0.0f, 1.0f, 0.0f) :
                                                   orientation * MT_Vector3(0.0f, 0.0f, 1.0f);
      if (axis >= 3) {
        vector = -vector;
      }
      return vector;
    }
    case LN_VectorExpressionKind::RigidBodyAttribute: {
      PHY_IPhysicsController *controller = ResolveRigidBodyAttributeController(in, expression.input0);
      if (controller == nullptr) {
        return MT_Vector3(0.0f, 0.0f, 0.0f);
      }
      return ReadRigidBodyAxisLockAttribute(*controller, expression.bool_value);
    }
    case LN_VectorExpressionKind::Combine: {
      /* Read order (D20): Z, Y, then X. */
      const float z = in.Float(expression.input2, expression.vector_value.z());
      const float y = in.Float(expression.input1, expression.vector_value.y());
      const float x = in.Float(expression.input0, expression.vector_value.x());
      return MT_Vector3(x, y, z);
    }
    case LN_VectorExpressionKind::CollisionHitPoint:
      if (const CollisionResult &collision_result = ResolveCollisionResult(in, expression.input0, expression.input1, expression.input2, CollisionResult::Detail::Contacts);
          collision_result.hit)
      {
        return collision_result.hit_point;
      }
      return fallback;
    case LN_VectorExpressionKind::CollisionHitNormal:
      if (const CollisionResult &collision_result = ResolveCollisionResult(in, expression.input0, expression.input1, expression.input2, CollisionResult::Detail::Contacts);
          collision_result.hit)
      {
        return collision_result.hit_normal;
      }
      return fallback;
    case LN_VectorExpressionKind::ConstraintBreakAnchorPosition:
      if (const LN_RigidBodyConstraintBreakEvent *event = ResolveRigidBodyConstraintBreakEvent(in, expression.input0, expression.input1, expression.input2, expression.rigid_body_constraint_match_mode))
      {
        return event->anchor_position;
      }
      return fallback;
    case LN_VectorExpressionKind::EvaluateCurveAtFactor: {
      KX_GameObject *curve_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        curve_object = m_gameObject;
      }
      else {
        const LN_Value curve_value = in.Value(expression.input0, MakeNoneValue());
        curve_object = ResolveObjectValue(curve_value);
      }
      if (curve_object == nullptr) {
        return fallback;
      }
      blender::Object *ob = curve_object->GetBlenderObject();
      if (ob == nullptr) {
        return fallback;
      }
      /* The path lives in the evaluated copy of the curve (the original has no curve cache: it
       * always gave the fallback and Blender logged "No curve cache!" every tick; plan phase A15,
       * bug B4). */
      if (KX_Scene *scene = curve_object->GetScene()) {
        blender::Scene *blender_scene = scene->GetBlenderScene();
        blender::ViewLayer *view_layer = blender_scene ?
                                             blender::BKE_view_layer_default_view(blender_scene) :
                                             nullptr;
        blender::Depsgraph *depsgraph = blender_scene && view_layer ?
                                            blender::BKE_scene_get_depsgraph(blender_scene,
                                                                             view_layer) :
                                            nullptr;
        if (depsgraph != nullptr) {
          if (blender::Object *evaluated = blender::DEG_get_evaluated(depsgraph, ob)) {
            ob = evaluated;
          }
        }
      }
      const float factor = in.Float(expression.float_expr_index, 0.0f);
      float path_position[4]; /* x, y, z and the tilt BKE_where_on_path writes last */
      if (!BKE_where_on_path(ob, factor, path_position, nullptr, nullptr, nullptr, nullptr)) {
        return fallback;
      }
      const MT_Transform world_xform = curve_object->NodeGetWorldTransform();
      return world_xform * MT_Vector3(path_position[0], path_position[1], path_position[2]);
    }
    case LN_VectorExpressionKind::InstructionNextPoint: {
      const uint32_t state_index = InstructionStateIndex(context.event, expression.input0);
      if (state_index == LN_INVALID_INDEX || state_index >= m_instructionStamps.size()) {
        return fallback;
      }
      const InstructionMotionState *state = FindMotionState(state_index);
      if (state == nullptr) {
        /* No run yet: the point a fresh motion state holds. */
        return InstructionMotionState().next_path_point;
      }
      return state->next_path_point;
    }
    case LN_VectorExpressionKind::Random: {
      const MT_Vector3 axes = in.Vector(expression.input0, MT_Vector3(1.0f, 1.0f, 1.0f));
      return MT_Vector3(
          axes.x() != 0.0f ?
              HashToUnitFloat((uint64_t(expression_index) << 32) ^ context.tick_index ^ 0x13579u) :
              0.0f,
          axes.y() != 0.0f ?
              HashToUnitFloat((uint64_t(expression_index) << 32) ^ context.tick_index ^ 0x24680u) :
              0.0f,
          axes.z() != 0.0f ? HashToUnitFloat((uint64_t(expression_index) << 32) ^
                                             context.tick_index ^ 0xabcdefu) :
                             0.0f);
    }
    case LN_VectorExpressionKind::Count:
      break;
  }

  return fallback;
}

template<class In>
LN_Rotation LN_RuntimeTree::RotationKind(In &in,
    const uint32_t expression_index,
    const LN_VectorExpression &expression,
    const LN_Rotation &fallback,
    bool &resolved)
{
  const LN_TickContext &context = in.context;
  LN_Rotation result = fallback;
  switch (expression.kind) {
    case LN_VectorExpressionKind::Constant:
      result = expression.output_type == LN_ValueType::Rotation ?
                   expression.rotation_value :
                   LN_Rotation::FromEulerXYZ(expression.vector_value);
      resolved = true;
      break;
    case LN_VectorExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        if (value->type == LN_ValueType::Rotation) {
          result = value->rotation_value;
          resolved = true;
        }
        else if (value->type == LN_ValueType::Vector) {
          result = LN_Rotation::FromEulerXYZ(value->vector_value);
          resolved = true;
        }
      }
      break;
    case LN_VectorExpressionKind::FromGenericValue: {
      const LN_ObjectAttributeId attribute = FastObjectAttribute(expression.input0);
      if (attribute != LN_ObjectAttributeId::None) {
        /* Plan G3: the attribute straight into the rotation, without its value (an orientation as
         * it is, a vector as Euler angles; Visible and Name give none). */
        if (KX_GameObject *target = TypedAttributeTarget(in, expression.input0)) {
          MT_Vector3 euler;
          if (attribute == LN_ObjectAttributeId::WorldOrientation) {
            result = LN_Rotation::FromMatrix(target->NodeGetWorldOrientation());
            resolved = true;
          }
          else if (attribute == LN_ObjectAttributeId::LocalOrientation) {
            result = LN_Rotation::FromMatrix(target->NodeGetLocalOrientation());
            resolved = true;
          }
          else if (ObjectAttributeAsVector(*target, attribute, euler)) {
            result = LN_Rotation::FromEulerXYZ(euler);
            resolved = true;
          }
        }
        if (fast_attributes_mode() == FastAttributesMode::Verify) {
          const LN_Value value = in.Value(expression.input0, MakeNoneValue());
          LN_Rotation expected = fallback;
          MT_Vector3 euler;
          if (value.type == LN_ValueType::Rotation) {
            expected = value.rotation_value;
          }
          else if (ValueAsVector(value, euler)) {
            expected = LN_Rotation::FromEulerXYZ(euler);
          }
          if (LN_DumpRotation(expected) != LN_DumpRotation(result)) {
            NoteFastAttributeDifference("a rotation read is not its value's");
          }
        }
        break;
      }
      const LN_Value value = in.Value(expression.input0, MakeNoneValue());
      if (value.type == LN_ValueType::Rotation) {
        result = value.rotation_value;
        resolved = true;
      }
      else {
        MT_Vector3 euler;
        if (ValueAsVector(value, euler)) {
          result = LN_Rotation::FromEulerXYZ(euler);
          resolved = true;
        }
      }
      break;
    }
    case LN_VectorExpressionKind::Combine: {
      const float x = in.Float(expression.input0, expression.vector_value.x());
      const float y = in.Float(expression.input1, expression.vector_value.y());
      const float z = in.Float(expression.input2, expression.vector_value.z());
      result = LN_Rotation::FromEulerXYZ(MT_Vector3(x, y, z));
      resolved = true;
      break;
    }
    case LN_VectorExpressionKind::VectorToRotation: {
      /* Read order (D20): the second input first. */
      const MT_Vector3 up = in.Vector(expression.input1, MT_Vector3(0.0f, 1.0f, 0.0f));
      const MT_Vector3 forward = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 1.0f));
      result = LN_Rotation::FromEulerXYZ(
          VectorToRotationEuler(forward, up, int32_t(expression.property_ref_index)));
      resolved = true;
      break;
    }
    case LN_VectorExpressionKind::BoneRotationToObjectRotation:
      result = LN_BoneRotationToObjectRotation(
          in.Rotation(expression.input0, LN_Rotation::Identity()));
      resolved = true;
      break;
    case LN_VectorExpressionKind::TweenRotationResult: {
      const float factor = ResolveTweenMappedFactor(in, expression.input0);
      if (expression.property_ref_index != 0) {
        const LN_Rotation from = in.Rotation(expression.input1, LN_Rotation::Identity());
        const LN_Rotation to = in.Rotation(expression.input2, LN_Rotation::Identity());
        result = from.Slerp(to, factor);
      }
      else {
        /* Euler mode is an explicit component-wise node contract. Preserve authored radians,
         * including turns outside the canonical [-pi, pi] representation, until after the
         * interpolation. */
        const MT_Vector3 from = in.Vector(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f));
        const MT_Vector3 to = in.Vector(expression.input2, MT_Vector3(0.0f, 0.0f, 0.0f));
        result = LN_Rotation::FromEulerXYZ(from + (to - from) * factor);
      }
      resolved = true;
      break;
    }
    default:
      result = LN_Rotation::FromEulerXYZ(
          in.Vector(expression_index, fallback.ToEulerXYZ()));
      break;
  }
  return result;
}

template<class In>
MT_Vector4 LN_RuntimeTree::ColorKind(In &in,
    const uint32_t expression_index,
    const LN_ColorExpression &expression,
    const MT_Vector4 &fallback)
{
  const LN_TickContext &context = in.context;
  switch (expression.kind) {
    case LN_ColorExpressionKind::Constant:
      return expression.color_value;
    case LN_ColorExpressionKind::SnapshotObjectColor:
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                in.Value(expression.input0, MakeNoneValue())))
        {
          LN_ObjectSnapshot other_object;
          return LN_CaptureObjectSnapshot(game_object, LN_SNAPSHOT_CHANNEL_COLOR, other_object)
              .object_color;
        }
        return fallback;
      }
      return m_snapshot.GetObjectSnapshot().object_color;
    case LN_ColorExpressionKind::SnapshotLightColor:
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                in.Value(expression.input0, MakeNoneValue())))
        {
          LN_ObjectSnapshot other_object;
          return LN_CaptureObjectSnapshot(game_object, LN_SNAPSHOT_CHANNEL_LIGHT, other_object)
              .light_color;
        }
        return fallback;
      }
      return m_snapshot.GetObjectSnapshot().light_color;
    case LN_ColorExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return value->color_value;
      }
      return fallback;
    case LN_ColorExpressionKind::Combine: {
      /* Read order (D20): A (input 3), then B, G and R. */
      const float a = in.Float(expression.input3, expression.color_value.w());
      const float b = in.Float(expression.input2, expression.color_value.z());
      const float g = in.Float(expression.input1, expression.color_value.y());
      const float r = in.Float(expression.input0, expression.color_value.x());
      return MT_Vector4(r, g, b, a);
    }
    case LN_ColorExpressionKind::FromGenericValue: {
      MT_Vector4 value;
      return ValueAsColor(in.Value(expression.input0, MakeNoneValue()),
                          value) ?
                 value :
                 fallback;
    }
    case LN_ColorExpressionKind::Count:
      break;
  }

  return fallback;
}

template<class In>
LN_Value LN_RuntimeTree::ValueKind(In &in,
    const uint32_t expression_index,
    const LN_ValueExpression &expression,
    const LN_Value &fallback)
{
  const LN_TickContext &context = in.context;
  switch (expression.kind) {
    case LN_ValueExpressionKind::Constant:
      if (expression.value.type == LN_ValueType::ObjectRef && expression.value.exists) {
        LN_Value value = expression.value;
        if (expression.value.runtime_ref.IsValid()) {
          value.exists = ResolveObjectRef(expression.value.runtime_ref) != nullptr;
          if (!value.exists) {
            value.runtime_ref = LN_RuntimeRef();
          }
        }
        else if (KX_GameObject *game_object = ResolveConstantObject(expression_index,
                                                                    expression.value))
        {
          value.runtime_ref = ConstantObjectRef(expression_index, game_object);
          value.reference_name = game_object->GetName();
          value.exists = true;
        }
        else {
          value.runtime_ref = LN_RuntimeRef();
          value.exists = false;
        }
        return value;
      }
      if (expression.value.type == LN_ValueType::CollectionRef && expression.value.exists) {
        LN_Value value = expression.value;
        if (blender::Collection *collection = ResolveCollectionByValue(this, value)) {
          value.runtime_ref = MakeCollectionRef(collection, collection->id.name + 2);
          value.reference_name = collection->id.name + 2;
          value.exists = true;
        }
        else {
          value.runtime_ref = LN_RuntimeRef();
          value.exists = false;
        }
        return value;
      }
      return expression.value;
    case LN_ValueExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return *value;
      }
      return fallback;
    case LN_ValueExpressionKind::Mix: {
      /* Read order (D20): the factor first, then B, then A. */
      const float factor = in.Float(expression.input2, 0.0f);
      const LN_Value b = in.Value(expression.input1, LN_Value());
      const LN_Value a = in.Value(expression.input0, LN_Value());
      return ln::value_interpolation::Mix(a, b, factor);
    }
    case LN_ValueExpressionKind::FromBool: {
      LN_Value value;
      value.type = LN_ValueType::Bool;
      value.exists = true;
      value.bool_value = in.Bool(expression.input0, false);
      return value;
    }
    case LN_ValueExpressionKind::FromInt: {
      LN_Value value;
      value.type = LN_ValueType::Int;
      value.exists = true;
      value.int_value = in.Int(expression.input0, 0);
      return value;
    }
    case LN_ValueExpressionKind::FromFloat: {
      LN_Value value;
      value.type = LN_ValueType::Float;
      value.exists = true;
      value.float_value = in.Float(expression.input0, 0.0f);
      return value;
    }
    case LN_ValueExpressionKind::FromString: {
      LN_Value value;
      value.type = LN_ValueType::String;
      value.exists = true;
      value.string_value = in.String(expression.input0, "");
      return value;
    }
    case LN_ValueExpressionKind::FromVector: {
      LN_Value value;
      value.type = LN_ValueType::Vector;
      value.exists = true;
      value.vector_value = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      return value;
    }
    case LN_ValueExpressionKind::FromColor: {
      LN_Value value;
      value.type = LN_ValueType::Color;
      value.exists = true;
      value.color_value = in.Color(expression.input0, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f));
      return value;
    }
    case LN_ValueExpressionKind::FromRotation: {
      LN_Value value;
      value.type = LN_ValueType::Rotation;
      value.exists = true;
      const LN_Rotation rotation = in.Rotation(expression.input0, LN_Rotation::Identity());
      value.SetRotation(
          rotation, in.Vector(expression.input0, rotation.ToEulerXYZ()));
      return value;
    }
    case LN_ValueExpressionKind::CombineVector4: {
      if (expression.input_indices.size() < 4) {
        return fallback;
      }
      LN_Value value;
      value.type = LN_ValueType::Vector4;
      value.exists = true;
      /* Read order (D20): the fourth input first, then the third, second and first. */
      const float w = in.Float(expression.input_indices[3], expression.value.vector4_value.w());
      const float z = in.Float(expression.input_indices[2], expression.value.vector4_value.z());
      const float y = in.Float(expression.input_indices[1], expression.value.vector4_value.y());
      const float x = in.Float(expression.input_indices[0], expression.value.vector4_value.x());
      value.vector4_value = MT_Vector4(x, y, z, w);
      return value;
    }
    case LN_ValueExpressionKind::ResizeVectorValue: {
      const LN_Value input = in.Value(expression.input0, MakeNoneValue());
      MT_Vector4 vector;
      if (!ValueAsVector4(input, vector)) {
        return fallback;
      }
      const int32_t target_size = std::clamp(int32_t(expression.property_ref_index), 2, 4);
      LN_Value value;
      value.exists = true;
      if (target_size == 4) {
        value.type = LN_ValueType::Vector4;
        value.vector4_value = vector;
      }
      else {
        value.type = LN_ValueType::Vector;
        value.vector_value = MT_Vector3(
            vector.x(), vector.y(), target_size >= 3 ? vector.z() : 0.0f);
      }
      return value;
    }
    case LN_ValueExpressionKind::EulerToMatrix: {
      const MT_Vector3 euler = in.Vector(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f));
      LN_Value value;
      value.type = LN_ValueType::Matrix;
      value.exists = true;
      value.matrix_value = MatrixFromEuler(euler, expression.property_ref_index);
      return value;
    }
    case LN_ValueExpressionKind::MatrixToEuler: {
      const LN_Value input = in.Value(expression.input0, MakeNoneValue());
      MT_Matrix3x3 matrix;
      if (!ValueAsMatrix(input, matrix)) {
        return fallback;
      }
      const MT_Vector3 euler = EulerFromMatrix(matrix, expression.property_ref_index);
      LN_Value value;
      value.type = expression.value.type == LN_ValueType::Rotation ? LN_ValueType::Rotation :
                                                                     LN_ValueType::Vector;
      value.exists = true;
      if (value.type == LN_ValueType::Rotation) {
        value.SetRotation(LN_Rotation::FromMatrix(matrix), euler);
      }
      else {
        value.vector_value = euler;
      }
      return value;
    }
    case LN_ValueExpressionKind::PlayerViewCamera: {
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      KX_Camera *camera = scene ? scene->GetPlayerViews().GetPlayerCamera(
                                      in.Int(expression.input0, 0),
                                      scene->GetActiveCamera()) :
                                  nullptr;
      if (camera == nullptr) {
        return MakeNoneValue();
      }
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = camera->GetName();
      value.runtime_ref = MakeObjectRef(camera, camera->GetName());
      return value;
    }
    case LN_ValueExpressionKind::MenuPlayerOwner: {
      KX_GameObject *menu_owner = m_logicManager != nullptr && m_gameObject != nullptr ?
                                      m_logicManager->GetUIService().MenuOwnerOfObject(
                                          m_gameObject) :
                                      nullptr;
      if (menu_owner == nullptr) {
        return MakeNoneValue();
      }
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = menu_owner->GetName();
      value.runtime_ref = MakeObjectRef(menu_owner, menu_owner->GetName());
      return value;
    }
    case LN_ValueExpressionKind::ActiveCamera: {
      KX_Camera *camera = GetActiveCamera();
      if (camera == nullptr) {
        return MakeNoneValue();
      }

      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = camera->GetName();
      value.runtime_ref = MakeObjectRef(camera, camera->GetName());
      return value;
    }
    case LN_ValueExpressionKind::OwnerObject: {
      if (m_gameObject == nullptr) {
        return MakeNoneValue();
      }

      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = m_gameObject->GetName();
      value.runtime_ref = MakeObjectRef(m_gameObject, m_gameObject->GetName());
      return value;
    }
    case LN_ValueExpressionKind::ObjectByName: {
      const std::string name = in.String(expression.input0, std::string());
      if (name.empty()) {
        return MakeNoneValue();
      }

      KX_GameObject *object = FindSceneObjectByName(name);
      if (object == nullptr) {
        return MakeNoneValue();
      }

      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = object->GetName();
      value.runtime_ref = MakeObjectRef(object, object->GetName());
      return value;
    }
    case LN_ValueExpressionKind::ObjectParent: {
      const LN_Value child_value = in.Value(expression.input0, MakeNoneValue());
      KX_GameObject *child_object = ResolveObjectValue(child_value);
      KX_GameObject *parent_object = child_object ? child_object->GetParent() : nullptr;
      if (parent_object == nullptr) {
        return MakeNoneValue();
      }

      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = parent_object->GetName();
      value.runtime_ref = MakeObjectRef(parent_object, parent_object->GetName());
      return value;
    }
    case LN_ValueExpressionKind::ObjectChild: {
      const LN_Value parent_value = in.Value(expression.input0, MakeNoneValue());
      KX_GameObject *parent_object = ResolveObjectValue(parent_value);
      if (parent_object == nullptr) {
        return MakeNoneValue();
      }

      const int32_t child_index = in.Int(expression.input1, 0);
      if (child_index < 0) {
        return MakeNoneValue();
      }

      const std::vector<KX_GameObject *> children = parent_object->GetChildren();
      if (size_t(child_index) >= children.size() || children[size_t(child_index)] == nullptr) {
        return MakeNoneValue();
      }

      KX_GameObject *child_object = children[size_t(child_index)];
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = child_object->GetName();
      value.runtime_ref = MakeObjectRef(child_object, child_object->GetName());
      return value;
    }
    case LN_ValueExpressionKind::ObjectChildByName: {
      const LN_Value parent_value = in.Value(expression.input0, MakeNoneValue());
      KX_GameObject *parent_object = ResolveObjectValue(parent_value);
      KX_GameObject *child_hint = ResolveObjectValue(
          in.Value(expression.input1, MakeNoneValue()));
      if (parent_object == nullptr || child_hint == nullptr) {
        return MakeNoneValue();
      }

      const std::string child_name = child_hint->GetName();
      for (KX_GameObject *child_object : parent_object->GetChildren()) {
        if (child_object != nullptr && child_object->GetName() == child_name) {
          LN_Value value;
          value.type = LN_ValueType::ObjectRef;
          value.exists = true;
          value.reference_name = child_object->GetName();
          value.runtime_ref = MakeObjectRef(child_object, child_object->GetName());
          return value;
        }
      }
      return MakeNoneValue();
    }
    case LN_ValueExpressionKind::PhysicsQueryObject: {
      const LN_QueryResult &query = in.Query(expression.input0);
      if (!query.physics_query.hit) {
        return MakeNoneValue();
      }

      KX_GameObject *game_object = ResolveObjectRef(query.physics_query.object_ref);
      if (game_object == nullptr) {
        return MakeNoneValue();
      }

      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.runtime_ref = query.physics_query.object_ref;
      value.reference_name = game_object->GetName();
      return value;
    }
    case LN_ValueExpressionKind::UIElementEventObject:
    case LN_ValueExpressionKind::UIElementEventTarget: {
      const LN_QueryResult &query = in.Query(expression.input0);
      const LN_RuntimeRef &object_ref = expression.kind ==
                                                LN_ValueExpressionKind::UIElementEventObject ?
                                            query.ui_source_object :
                                            query.ui_target_object;
      KX_GameObject *game_object = ResolveObjectRef(object_ref);
      if (game_object == nullptr) {
        return MakeNoneValue();
      }
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.runtime_ref = object_ref;
      value.reference_name = game_object->GetName();
      return value;
    }
    case LN_ValueExpressionKind::UIElementEventItem:
    case LN_ValueExpressionKind::UIElementEventTargetItem: {
      const LN_QueryResult &query = in.Query(expression.input0);
      const std::shared_ptr<LN_Value> &item =
          expression.kind == LN_ValueExpressionKind::UIElementEventItem ? query.ui_item :
                                                                          query.ui_target_item;
      return item != nullptr ? *item : MakeNoneValue();
    }
    case LN_ValueExpressionKind::UIFocusObject: {
      if (m_logicManager == nullptr) {
        return MakeNoneValue();
      }
      const int32_t user = in.Int(expression.input0, 0);
      const std::string input_context = in.String(expression.input1, std::string());
      KX_GameObject *game_object = m_logicManager->GetUIService().GetFocus(user, input_context);
      if (game_object == nullptr) {
        return MakeNoneValue();
      }
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.runtime_ref = MakeObjectRef(game_object, game_object->GetName());
      value.reference_name = game_object->GetName();
      return value;
    }
    case LN_ValueExpressionKind::PhysicsQueryObjects:
    case LN_ValueExpressionKind::PhysicsQueryPoints:
    case LN_ValueExpressionKind::PhysicsQueryNormals:
    case LN_ValueExpressionKind::PhysicsQueryDistances:
    case LN_ValueExpressionKind::PhysicsQueryCastPositions:
    case LN_ValueExpressionKind::PhysicsQueryFractions:
    case LN_ValueExpressionKind::PhysicsQueryPenetrationDepths:
    case LN_ValueExpressionKind::PhysicsQueryStartedOverlappingList:
    case LN_ValueExpressionKind::PhysicsQueryFaceIndices:
    case LN_ValueExpressionKind::PhysicsQueryHasUVs:
    case LN_ValueExpressionKind::PhysicsQueryUVs:
    case LN_ValueExpressionKind::PhysicsQueryHits:
    case LN_ValueExpressionKind::PhysicsQueryBlockedList:
    case LN_ValueExpressionKind::PhysicsQueryRayOrigins:
    case LN_ValueExpressionKind::PhysicsQueryRayDirections:
    case LN_ValueExpressionKind::PhysicsQueryEndPoints: {
      const LN_QueryResult &query = in.Query(expression.input0);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(query.physics_query_results.size());
      const bool hit_detail_output =
          expression.kind == LN_ValueExpressionKind::PhysicsQueryObjects ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryPoints ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryNormals ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryDistances ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryCastPositions ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryFractions ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryPenetrationDepths ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryStartedOverlappingList ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryFaceIndices ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryHasUVs ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryUVs;
      for (const LN_PhysicsQueryResult &hit : query.physics_query_results) {
        /* Raycast from Mesh retains one internal slot per generated ray so its Hits, Blocked and
         * ray-geometry outputs can diagnose misses. Collision-detail outputs follow the regular
         * physics-query contract, however: they contain matching hits only. In particular, never
         * expose the placeholder end position of a miss as a hit point. */
        if (hit_detail_output && !hit.hit) {
          continue;
        }
        switch (expression.kind) {
          case LN_ValueExpressionKind::PhysicsQueryObjects: {
            LN_Value object_value = MakeNoneValue();
            object_value.type = LN_ValueType::ObjectRef;
            KX_GameObject *game_object = hit.hit ? ResolveObjectRef(hit.object_ref) : nullptr;
            object_value.exists = game_object != nullptr;
            if (game_object) {
              object_value.runtime_ref = hit.object_ref;
              object_value.reference_name = game_object->GetName();
            }
            value.list_value.push_back(object_value);
            break;
          }
          case LN_ValueExpressionKind::PhysicsQueryPoints:
            value.list_value.push_back(MakeVectorValue(hit.hit_position));
            break;
          case LN_ValueExpressionKind::PhysicsQueryNormals:
            value.list_value.push_back(MakeVectorValue(hit.hit_normal));
            break;
          case LN_ValueExpressionKind::PhysicsQueryDistances:
            value.list_value.push_back(MakeFloatValue(hit.hit_distance));
            break;
          case LN_ValueExpressionKind::PhysicsQueryCastPositions:
            value.list_value.push_back(MakeVectorValue(hit.cast_position));
            break;
          case LN_ValueExpressionKind::PhysicsQueryFractions:
            value.list_value.push_back(MakeFloatValue(hit.hit_fraction));
            break;
          case LN_ValueExpressionKind::PhysicsQueryPenetrationDepths:
            value.list_value.push_back(MakeFloatValue(hit.penetration_depth));
            break;
          case LN_ValueExpressionKind::PhysicsQueryStartedOverlappingList:
            value.list_value.push_back(MakeBoolValue(hit.started_overlapping));
            break;
          case LN_ValueExpressionKind::PhysicsQueryFaceIndices:
            value.list_value.push_back(MakeIntValue(hit.polygon_index));
            break;
          case LN_ValueExpressionKind::PhysicsQueryHasUVs:
            value.list_value.push_back(MakeBoolValue(hit.has_uv));
            break;
          case LN_ValueExpressionKind::PhysicsQueryUVs:
            value.list_value.push_back(
                MakeVectorValue(MT_Vector3(hit.hit_uv.x(), hit.hit_uv.y(), 0.0f)));
            break;
          case LN_ValueExpressionKind::PhysicsQueryHits:
            value.list_value.push_back(MakeBoolValue(hit.hit));
            break;
          case LN_ValueExpressionKind::PhysicsQueryBlockedList:
            value.list_value.push_back(MakeBoolValue(hit.blocked));
            break;
          case LN_ValueExpressionKind::PhysicsQueryRayOrigins:
            value.list_value.push_back(MakeVectorValue(hit.ray_origin));
            break;
          case LN_ValueExpressionKind::PhysicsQueryRayDirections:
            value.list_value.push_back(MakeVectorValue(hit.ray_direction));
            break;
          case LN_ValueExpressionKind::PhysicsQueryEndPoints:
            value.list_value.push_back(MakeVectorValue(hit.end_position));
            break;
          default:
            break;
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::ProjectileParabola: {
      const LN_QueryResult &query = in.Query(expression.input0);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(query.physics_query.parabola_points.size());
      for (const MT_Vector3 &point : query.physics_query.parabola_points) {
        LN_Value point_value;
        point_value.type = LN_ValueType::Vector;
        point_value.exists = true;
        point_value.vector_value = point;
        value.list_value.push_back(point_value);
      }
      return value;
    }
    case LN_ValueExpressionKind::ValueChangedOld:
    case LN_ValueExpressionKind::ValueChangedNew: {
      if (expression.input0 == LN_INVALID_INDEX) {
        return fallback;
      }
      in.Bool(expression.input0, false);
      const ValueChangeState *values = FindValueChangeState(expression.input0);
      if (values == nullptr) {
        return LN_Value();
      }
      return expression.kind == LN_ValueExpressionKind::ValueChangedOld ?
                 values->stored_old_value :
                 values->stored_new_value;
    }
    case LN_ValueExpressionKind::EventContent: {
      const LN_StringId subject_string_id = ResolveStringExpressionId(expression.input0);
      const LN_EventSubjectId subject_id = GetEventSubjectId(subject_string_id);
      const std::string subject = in.String(expression.input0, std::string());
      if (subject.empty()) {
        return fallback;
      }
      KX_GameObject *filter_target = m_gameObject;
      if (expression.value.exists && expression.value.bool_value) {
        if (expression.input1 == LN_INVALID_INDEX) {
          return fallback;
        }
        const LN_Value target_val = in.Value(expression.input1, MakeNoneValue());
        KX_GameObject *explicit_target = ResolveObjectValue(target_val);
        if (explicit_target == nullptr) {
          return fallback;
        }
        filter_target = explicit_target;
      }
      if (const LN_EventEntry *event = FindFirstActiveEvent(subject_id, subject, filter_target)) {
        LN_Value content = event->content;
        LocalizeValue(content);
        return content;
      }
      return fallback;
    }
    case LN_ValueExpressionKind::EventMessenger: {
      const LN_StringId subject_string_id = ResolveStringExpressionId(expression.input0);
      const LN_EventSubjectId subject_id = GetEventSubjectId(subject_string_id);
      const std::string subject = in.String(expression.input0, std::string());
      if (subject.empty()) {
        return fallback;
      }
      KX_GameObject *filter_target = m_gameObject;
      if (expression.value.exists && expression.value.bool_value) {
        if (expression.input1 == LN_INVALID_INDEX) {
          return fallback;
        }
        const LN_Value target_val = in.Value(expression.input1, MakeNoneValue());
        KX_GameObject *explicit_target = ResolveObjectValue(target_val);
        if (explicit_target == nullptr) {
          return fallback;
        }
        filter_target = explicit_target;
      }
      if (const LN_EventEntry *event = FindFirstActiveEvent(subject_id, subject, filter_target)) {
        /* The messenger may have been removed since the event was sent. */
        if (KX_GameObject *messenger = m_logicManager->ResolveObjectHandle(
                event->messenger_handle))
        {
          LN_Value result;
          result.type = LN_ValueType::ObjectRef;
          result.exists = true;
          result.reference_name = messenger->GetName();
          result.runtime_ref = MakeObjectRef(messenger, result.reference_name);
          return result;
        }
      }
      return fallback;
    }
    case LN_ValueExpressionKind::ObjectAttribute: {
      KX_GameObject *target_object = ObjectAttributeTarget(in, expression);
      if (target_object == nullptr) {
        return fallback;
      }
      /* Plan G2: a constant attribute name is a number worked out once per program; any other
       * text is read and looked up here, at every read, as before. */
      LN_ObjectAttributeId attribute = FastObjectAttribute(expression_index);
      std::string attr;
      if (attribute == LN_ObjectAttributeId::None) {
        attr = in.String(expression.input1, std::string());
        if (attr.empty()) {
          return fallback;
        }
        attribute = LN_ObjectAttributeIdFromName(attr);
      }
      else if (fast_attributes_mode() == FastAttributesMode::Verify &&
               LN_ObjectAttributeIdFromName(in.String(expression.input1, std::string())) !=
                   attribute)
      {
        NoteFastAttributeDifference("an attribute id is not the one its name gives");
      }
      LN_Value value;
      value.exists = true;
      switch (attribute) {
        case LN_ObjectAttributeId::WorldPosition:
          value.type = LN_ValueType::Vector;
          value.vector_value = target_object->NodeGetWorldPosition();
          return value;
        case LN_ObjectAttributeId::LocalPosition:
          value.type = LN_ValueType::Vector;
          value.vector_value = target_object->NodeGetLocalPosition();
          return value;
        case LN_ObjectAttributeId::WorldOrientation:
          value.type = LN_ValueType::Rotation;
          value.SetRotation(LN_Rotation::FromMatrix(target_object->NodeGetWorldOrientation()));
          return value;
        case LN_ObjectAttributeId::LocalOrientation:
          value.type = LN_ValueType::Rotation;
          value.SetRotation(LN_Rotation::FromMatrix(target_object->NodeGetLocalOrientation()));
          return value;
        case LN_ObjectAttributeId::WorldScale:
          value.type = LN_ValueType::Vector;
          value.vector_value = target_object->NodeGetWorldScaling();
          return value;
        case LN_ObjectAttributeId::LocalScale:
          value.type = LN_ValueType::Vector;
          value.vector_value = target_object->NodeGetLocalScaling();
          return value;
        case LN_ObjectAttributeId::WorldLinearVelocity:
          value.type = LN_ValueType::Vector;
          value.vector_value = target_object->GetLinearVelocity(false);
          return value;
        case LN_ObjectAttributeId::LocalLinearVelocity:
          value.type = LN_ValueType::Vector;
          value.vector_value = target_object->GetLinearVelocity(true);
          return value;
        case LN_ObjectAttributeId::WorldAngularVelocity:
          value.type = LN_ValueType::Vector;
          value.vector_value = target_object->GetAngularVelocity(false);
          return value;
        case LN_ObjectAttributeId::LocalAngularVelocity:
          value.type = LN_ValueType::Vector;
          value.vector_value = target_object->GetAngularVelocity(true);
          return value;
        case LN_ObjectAttributeId::Color:
          value.type = LN_ValueType::Color;
          value.color_value = target_object->GetObjectColor();
          return value;
        case LN_ObjectAttributeId::Visible:
          value.type = LN_ValueType::Bool;
          value.bool_value = target_object->GetVisible();
          return value;
        case LN_ObjectAttributeId::Name:
          value.type = LN_ValueType::String;
          value.string_value = target_object->GetName();
          return value;
        case LN_ObjectAttributeId::None:
          break;
      }
      /* Try as a game property. */
      EXP_Value *prop = target_object->GetProperty(attr);
      if (prop != nullptr) {
        return MakeValueFromEXPValue(prop, this);
      }
      return fallback;
    }
    case LN_ValueExpressionKind::BonePoseRotation: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      const std::string bone_name = in.String(expression.string_expr_index, std::string());
      LN_Rotation rotation;
      const LN_BonePoseRotationSpace space = NormalizeBonePoseRotationSpace(
          expression.property_ref_index);
      if (!ResolveBonePoseRotation(target_object, bone_name, space, rotation)) {
        return fallback;
      }
      LN_Value value;
      value.type = LN_ValueType::Rotation;
      value.SetRotation(rotation);
      value.exists = true;
      return value;
    }
    case LN_ValueExpressionKind::BoneAttribute: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      const std::string bone_name = in.String(expression.string_expr_index, std::string());
      BL_ArmatureObject *armature = ArmatureFacade(target_object);
      blender::Object *ob = armature ? armature->GetArmatureObject() : nullptr;
      if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr ||
          bone_name.empty())
      {
        return fallback;
      }
      blender::bPoseChannel *pchan = armature->FindPoseChannel(bone_name);
      if (pchan == nullptr || pchan->bone == nullptr) {
        return fallback;
      }
      blender::Bone *bone = pchan->bone;
      const uint32_t property_ref = expression.property_ref_index == LN_INVALID_INDEX ?
                                        0 :
                                        expression.property_ref_index;
      const uint32_t attribute = property_ref & LN_BONE_ATTRIBUTE_INDEX_MASK;
      const bool world_space = (property_ref & LN_BONE_ATTRIBUTE_WORLD_SPACE_FLAG) != 0;

      auto make_position_value = [&](const MT_Vector3 &position) {
        return MakeVectorValue(world_space ? ArmaturePointToWorld(target_object, position) :
                                             position);
      };
      auto make_rest_position_value = [&](const float parent_position[3],
                                          const float armature_position[3]) {
        const float *position = world_space ? armature_position : parent_position;
        MT_Vector3 result(position[0], position[1], position[2]);
        if (world_space) {
          result = ArmaturePointToWorld(target_object, result);
        }
        return MakeVectorValue(result);
      };

      LN_Value value = MakeNoneValue();
      switch (attribute) {
        case 0:
          value.type = LN_ValueType::String;
          value.string_value = bone->name;
          value.exists = true;
          return value;
        case 1:
          return MakeVectorValue(MT_Vector3(pchan->loc[0], pchan->loc[1], pchan->loc[2]));
        case 2:
          value.type = LN_ValueType::Int;
          value.int_value = bone->inherit_scale_mode;
          value.exists = true;
          return value;
        case 4:
          return make_rest_position_value(bone->head, bone->arm_head);
        case 5:
          return make_position_value(
              MT_Vector3(bone->arm_head[0], bone->arm_head[1], bone->arm_head[2]));
        case 6:
          armature->ApplyPose();
          return make_position_value(
              MT_Vector3(pchan->pose_head[0], pchan->pose_head[1], pchan->pose_head[2]));
        case 7: {
          const float parent_center[3] = {
              (bone->head[0] + bone->tail[0]) * 0.5f,
              (bone->head[1] + bone->tail[1]) * 0.5f,
              (bone->head[2] + bone->tail[2]) * 0.5f,
          };
          const float armature_center[3] = {
              (bone->arm_head[0] + bone->arm_tail[0]) * 0.5f,
              (bone->arm_head[1] + bone->arm_tail[1]) * 0.5f,
              (bone->arm_head[2] + bone->arm_tail[2]) * 0.5f,
          };
          return make_rest_position_value(parent_center, armature_center);
        }
        case 8:
          return make_position_value(MT_Vector3((bone->arm_head[0] + bone->arm_tail[0]) * 0.5f,
                                                (bone->arm_head[1] + bone->arm_tail[1]) * 0.5f,
                                                (bone->arm_head[2] + bone->arm_tail[2]) * 0.5f));
        case 9:
          armature->ApplyPose();
          return make_position_value(
              MT_Vector3((pchan->pose_head[0] + pchan->pose_tail[0]) * 0.5f,
                         (pchan->pose_head[1] + pchan->pose_tail[1]) * 0.5f,
                         (pchan->pose_head[2] + pchan->pose_tail[2]) * 0.5f));
        case 10:
          return make_rest_position_value(bone->tail, bone->arm_tail);
        case 11:
          return make_position_value(
              MT_Vector3(bone->arm_tail[0], bone->arm_tail[1], bone->arm_tail[2]));
        case 12:
          armature->ApplyPose();
          return make_position_value(
              MT_Vector3(pchan->pose_tail[0], pchan->pose_tail[1], pchan->pose_tail[2]));
        case 13:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->flag & blender::BONE_HINGE) == 0;
          value.exists = true;
          return value;
        case 14:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->flag & blender::BONE_CONNECTED) != 0;
          value.exists = true;
          return value;
        case 15:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->flag & blender::BONE_NO_DEFORM) == 0;
          value.exists = true;
          return value;
        case 16:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->flag & blender::BONE_NO_LOCAL_LOCATION) == 0;
          value.exists = true;
          return value;
        case 17:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->flag & blender::BONE_RELATIVE_PARENTING) != 0;
          value.exists = true;
          return value;
        case 18:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->bbone_flag & blender::BBONE_SCALE_EASING) != 0;
          value.exists = true;
          return value;
        default:
          return fallback;
      }
    }
    case LN_ValueExpressionKind::MaterialSlot: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      const int32_t slot = in.Int(expression.input1, 0);
      return MakeMaterialValue(this, ObjectMaterialAtSlot(target_object, slot));
    }
    case LN_ValueExpressionKind::MaterialNodeValue: {
      blender::Material *material = nullptr;
      blender::Object *parameter_object = nullptr;
      const bool object_slot_target = ELEM(expression.property_ref_index, 1u, 2u, 4u);
      if (object_slot_target) {
        KX_GameObject *target_object = m_gameObject;
        if (expression.input0 != LN_INVALID_INDEX) {
          target_object = ResolveObjectValue(
              in.Value(expression.input0, MakeNoneValue()));
        }
        parameter_object = target_object ? target_object->GetBlenderObject() : nullptr;
        const int32_t slot = in.Int(expression.input1, 0);
        material = ObjectMaterialAtSlot(target_object, slot);
      }
      else {
        material = ResolveMaterialByValue(
            this, in.Value(expression.input0, MakeNoneValue()));
      }

      const std::string node_name = in.String(expression.string_expr_index, std::string());
      const std::string socket_name = in.String(expression.input2, std::string());
      if (material == nullptr || material->nodetree == nullptr || node_name.empty() ||
          socket_name.empty())
      {
        return fallback;
      }
      blender::bNode *node = FindNodeByUserName(material->nodetree, node_name);
      blender::bNodeSocket *socket = FindInputSocketByIdentifierOrName(node, socket_name);
      if (socket == nullptr) {
        return fallback;
      }
      if (expression.property_ref_index >= 3) {
        material->nodetree->ensure_topology_cache();
        if ((socket->flag & blender::SOCK_UNAVAIL) != 0 || socket->is_directly_linked()) {
          return fallback;
        }
      }
      if (expression.property_ref_index == 2) {
        return ReadMaterialParameterValue(parameter_object, material, *node, *socket, this);
      }
      return ReadSocketDefaultValue(*socket, this);
    }
    case LN_ValueExpressionKind::EditorNodeValue: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      const std::string target_name = in.String(expression.input1, std::string());
      blender::bNodeTree *tree = ResolveEditorNodeValueTree(
          EditorNodeValueTarget(expression.property_ref_index), target_object, target_name);
      const std::string node_name = in.String(expression.string_expr_index, std::string());
      const std::string socket_name = in.String(expression.input2, std::string());
      blender::bNode *node = FindNodeByUserName(tree, node_name);
      blender::bNodeSocket *socket = FindInputSocketByIdentifierOrName(node, socket_name);
      if (tree == nullptr || socket == nullptr || (socket->flag & blender::SOCK_UNAVAIL) != 0) {
        return fallback;
      }
      tree->ensure_topology_cache();
      return socket->is_directly_linked() ? fallback : ReadSocketDefaultValue(*socket, this);
    }
    case LN_ValueExpressionKind::ObjectGameProperty: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const std::string property_name = in.String(expression.input1, std::string());
      if (EXP_Value *property = target_object->GetProperty(property_name)) {
        return MakeValueFromEXPValue(property, this);
      }
      return MakeNoneValue();
    }
    case LN_ValueExpressionKind::CollisionHitObject: {
      const CollisionResult &collision_result = ResolveCollisionResult(in, expression.input0, expression.input1, expression.input2, CollisionResult::Detail::Objects);
      if (!collision_result.hit || collision_result.hit_object == nullptr) {
        return MakeNoneValue();
      }
      return MakeObjectValue(this, collision_result.hit_object);
    }
    case LN_ValueExpressionKind::CollisionHitObjects: {
      const CollisionResult &collision_result = ResolveCollisionResult(in, expression.input0, expression.input1, expression.input2, CollisionResult::Detail::Objects);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(collision_result.hit_objects.size());
      for (KX_GameObject *object : collision_result.hit_objects) {
        value.list_value.push_back(MakeObjectValue(this, object));
      }
      return value;
    }
    case LN_ValueExpressionKind::ConstraintBreakTargetObject: {
      const LN_RigidBodyConstraintBreakEvent *event = ResolveRigidBodyConstraintBreakEvent(in, expression.input0, expression.input1, expression.input2, expression.rigid_body_constraint_match_mode);
      if (event == nullptr || m_logicManager == nullptr) {
        return MakeNoneValue();
      }
      return MakeObjectValue(this, m_logicManager->ResolveObjectHandle(event->target));
    }
    case LN_ValueExpressionKind::ConstraintBreakEvents:
      return ResolveRigidBodyConstraintBreakEvents(in, expression.input0, expression.input1, expression.input2, expression.rigid_body_constraint_match_mode);
    case LN_ValueExpressionKind::CollisionHitPoints: {
      const CollisionResult &collision_result = ResolveCollisionResult(in, expression.input0, expression.input1, expression.input2, CollisionResult::Detail::Contacts);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(collision_result.hit_points.size());
      for (const MT_Vector3 &point : collision_result.hit_points) {
        value.list_value.push_back(MakeVectorValue(point));
      }
      return value;
    }
    case LN_ValueExpressionKind::CollisionHitNormals: {
      const CollisionResult &collision_result = ResolveCollisionResult(in, expression.input0, expression.input1, expression.input2, CollisionResult::Detail::Contacts);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(collision_result.hit_normals.size());
      for (const MT_Vector3 &normal : collision_result.hit_normals) {
        value.list_value.push_back(MakeVectorValue(normal));
      }
      return value;
    }
    case LN_ValueExpressionKind::LoopCurrentValue: {
      const uint32_t loop_frame_index = expression.property_ref_index;
      if (loop_frame_index >= m_loopStates.size()) {
        return fallback;
      }
      return m_loopStates[loop_frame_index].current_value;
    }
    case LN_ValueExpressionKind::ListElement: {
      const LN_Value list_value = in.Value(expression.input0, MakeNoneValue());
      if (list_value.type != LN_ValueType::List || !list_value.exists) {
        return fallback;
      }
      const int32_t index = in.Int(expression.input1, 0);
      if (index < 0 || size_t(index) >= list_value.list_value.size()) {
        return fallback;
      }
      return list_value.list_value[size_t(index)];
    }
    case LN_ValueExpressionKind::ListRandomItem: {
      const LN_Value list_value = in.Value(expression.input0, MakeNoneValue());
      if (list_value.type != LN_ValueType::List || !list_value.exists ||
          list_value.list_value.empty())
      {
        return fallback;
      }
      const uint64_t seed = (uint64_t(expression_index) << 32) ^ context.tick_index;
      const size_t index = std::min(size_t(HashToUnitFloat(seed) * list_value.list_value.size()),
                                    list_value.list_value.size() - 1);
      return list_value.list_value[index];
    }
    case LN_ValueExpressionKind::ValueSwitchList: {
      const size_t pair_count = expression.input_indices.size() / 2;
      for (size_t pair_index = 0; pair_index < pair_count; pair_index++) {
        const uint32_t condition_index = expression.input_indices[pair_index * 2];
        const uint32_t value_index = expression.input_indices[pair_index * 2 + 1];
        if (condition_index != LN_INVALID_INDEX &&
            in.Bool(condition_index, false))
        {
          return in.Value(value_index, MakeNoneValue());
        }
      }
      return MakeNoneValue();
    }
    case LN_ValueExpressionKind::ValueSwitchListCompare: {
      const LN_Value switch_value = in.Value(expression.input0, MakeNoneValue());
      const size_t pair_count = expression.input_indices.size() / 2;
      for (size_t pair_index = 0; pair_index < pair_count; pair_index++) {
        const uint32_t case_index = expression.input_indices[pair_index * 2];
        const uint32_t value_index = expression.input_indices[pair_index * 2 + 1];
        const LN_Value case_value = in.Value(case_index, MakeNoneValue());
        if (CompareValues(switch_value,
                          case_value,
                          expression.value.exists ?
                              static_cast<LN_FloatCompareOperation>(expression.value.int_value) :
                              LN_FloatCompareOperation::Equal))
        {
          return in.Value(value_index, MakeNoneValue());
        }
      }
      return in.Value(expression.input1, MakeNoneValue());
    }
    case LN_ValueExpressionKind::MakeList: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        value.list_value.push_back(
            in.Value(expression.input0, MakeNoneValue()));
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        value.list_value.push_back(
            in.Value(expression.input1, MakeNoneValue()));
      }
      if (expression.input2 != LN_INVALID_INDEX) {
        value.list_value.push_back(
            in.Value(expression.input2, MakeNoneValue()));
      }
      return value;
    }
    case LN_ValueExpressionKind::ListDuplicate: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value source = in.Value(expression.input0, MakeNoneValue());
        if (source.type == LN_ValueType::List && source.exists) {
          value.list_value = source.list_value;
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::DictMerge: {
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value dict_a = in.Value(expression.input0, MakeNoneValue());
        if (dict_a.type == LN_ValueType::Dict && dict_a.exists) {
          value.dict_value = dict_a.dict_value;
        }
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        const LN_Value dict_b = in.Value(expression.input1, MakeNoneValue());
        if (dict_b.type == LN_ValueType::Dict && dict_b.exists) {
          for (const auto &entry : dict_b.dict_value) {
            value.dict_value[entry.first] = entry.second;
          }
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::ListExtend: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value list_a = in.Value(expression.input0, MakeNoneValue());
        if (list_a.type == LN_ValueType::List && list_a.exists) {
          value.list_value.insert(
              value.list_value.end(), list_a.list_value.begin(), list_a.list_value.end());
        }
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        const LN_Value list_b = in.Value(expression.input1, MakeNoneValue());
        if (list_b.type == LN_ValueType::List && list_b.exists) {
          value.list_value.insert(
              value.list_value.end(), list_b.list_value.begin(), list_b.list_value.end());
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::ListAppend: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value list_value = in.Value(expression.input0, MakeNoneValue());
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          value.list_value = list_value.list_value;
        }
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        value.list_value.push_back(
            in.Value(expression.input1, MakeNoneValue()));
      }
      return value;
    }
    case LN_ValueExpressionKind::ListRemoveIndex: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value list_value = in.Value(expression.input0, MakeNoneValue());
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          value.list_value = list_value.list_value;
        }
      }
      const int32_t index = expression.input1 != LN_INVALID_INDEX ?
                                in.Int(expression.input1, 0) :
                                0;
      size_t normalized_index = 0;
      if (NormalizeListIndex(index, value.list_value.size(), normalized_index)) {
        value.list_value.erase(value.list_value.begin() + normalized_index);
      }
      return value;
    }
    case LN_ValueExpressionKind::ListRemoveValue: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value list_value = in.Value(expression.input0, MakeNoneValue());
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          value.list_value = list_value.list_value;
        }
      }
      const LN_Value needle = expression.input1 != LN_INVALID_INDEX ?
                                  in.Value(expression.input1, MakeNoneValue()) :
                                  MakeNoneValue();
      const auto iter = std::find_if(
          value.list_value.begin(), value.list_value.end(), [&](const LN_Value &item) {
            return ValuesEqual(item, needle);
          });
      if (iter != value.list_value.end()) {
        value.list_value.erase(iter);
      }
      return value;
    }
    case LN_ValueExpressionKind::ListSetIndex: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value list_value = in.Value(expression.input0, MakeNoneValue());
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          value.list_value = list_value.list_value;
        }
      }
      const int32_t index = expression.input1 != LN_INVALID_INDEX ?
                                in.Int(expression.input1, 0) :
                                0;
      size_t normalized_index = 0;
      if (NormalizeListIndex(index, value.list_value.size(), normalized_index)) {
        value.list_value[normalized_index] = expression.input2 != LN_INVALID_INDEX ?
                                                 in.Value(expression.input2, MakeNoneValue()) :
                                                 MakeNoneValue();
      }
      return value;
    }
    case LN_ValueExpressionKind::DictGetKey: {
      const LN_Value dict_value = expression.input0 != LN_INVALID_INDEX ?
                                      in.Value(expression.input0, MakeNoneValue()) :
                                      MakeNoneValue();
      const std::string key = expression.input1 != LN_INVALID_INDEX ?
                                  in.String(expression.input1, std::string()) :
                                  std::string();
      const LN_Value default_value = expression.input2 != LN_INVALID_INDEX ?
                                         in.Value(expression.input2, MakeNoneValue()) :
                                         MakeNoneValue();
      if (dict_value.type == LN_ValueType::Dict && dict_value.exists && !key.empty()) {
        const auto iter = dict_value.dict_value.find(key);
        if (iter != dict_value.dict_value.end()) {
          return iter->second;
        }
      }
      return default_value;
    }
    case LN_ValueExpressionKind::MakeDict: {
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      const std::string key = expression.input0 != LN_INVALID_INDEX ?
                                  in.String(expression.input0, std::string()) :
                                  std::string();
      if (!key.empty()) {
        value.dict_value[key] = expression.input1 != LN_INVALID_INDEX ?
                                    in.Value(expression.input1, MakeNoneValue()) :
                                    MakeNoneValue();
      }
      return value;
    }
    case LN_ValueExpressionKind::DictSetKey: {
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value dict_value = in.Value(expression.input0, MakeNoneValue());
        if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
          value.dict_value = dict_value.dict_value;
        }
      }
      const std::string key = expression.input1 != LN_INVALID_INDEX ?
                                  in.String(expression.input1, std::string()) :
                                  std::string();
      if (!key.empty()) {
        value.dict_value[key] = expression.input2 != LN_INVALID_INDEX ?
                                    in.Value(expression.input2, MakeNoneValue()) :
                                    MakeNoneValue();
      }
      return value;
    }
    case LN_ValueExpressionKind::DictAddValue: {
      LN_Value value = in.Value(expression.input0, MakeNoneValue());
      if (value.type != LN_ValueType::Dict || !value.exists) {
        value = LN_Value();
        value.type = LN_ValueType::Dict;
        value.exists = true;
      }
      const std::string key = in.String(expression.input1, std::string());
      if (key.empty()) {
        return value;
      }
      float amount = 0.0f;
      ValueAsFloat(in.Value(expression.input2, MakeNoneValue()), amount);
      LN_Value &entry = value.dict_value[key];
      const bool was_int = entry.exists && entry.type == LN_ValueType::Int;
      if ((was_int || !entry.exists) && std::isfinite(amount) && amount == std::trunc(amount)) {
        /* Whole amounts keep counts as integers. */
        const double sum = double(was_int ? entry.int_value : 0) + double(amount);
        entry = MakeIntValue(int32_t(std::clamp(sum, double(INT32_MIN), double(INT32_MAX))));
      }
      else {
        float current = 0.0f;
        ValueAsFloat(entry, current);
        entry = MakeFloatValue(current + amount);
      }
      return value;
    }
    case LN_ValueExpressionKind::ListSort:
    case LN_ValueExpressionKind::ListFindIndex: {
      LN_Value list = in.Value(expression.input0, MakeNoneValue());
      if (list.type != LN_ValueType::List || !list.exists) {
        list = LN_Value();
        list.type = LN_ValueType::List;
        list.exists = true;
      }
      const LN_Value key = in.Value(expression.input1, MakeNoneValue());
      if (expression.kind == LN_ValueExpressionKind::ListFindIndex) {
        const auto iter = std::find_if(
            list.list_value.begin(), list.list_value.end(), [&](const LN_Value &item) {
              return ValuesEqual(item, key);
            });
        return MakeIntValue(iter == list.list_value.end() ?
                                -1 :
                                int32_t(iter - list.list_value.begin()));
      }
      SortListValues(list.list_value,
                     key.exists && key.type == LN_ValueType::String ? key.string_value :
                                                                       std::string(),
                     ValueAsBool(in.Value(expression.input2, MakeNoneValue())));
      return list;
    }
    case LN_ValueExpressionKind::InventoryEdit:
    case LN_ValueExpressionKind::InventoryAmount:
    case LN_ValueExpressionKind::InventoryChanged:
    case LN_ValueExpressionKind::InventoryMade:
    case LN_ValueExpressionKind::InventoryOutcome: {
      const bool amount = expression.kind == LN_ValueExpressionKind::InventoryAmount ||
                          expression.kind == LN_ValueExpressionKind::InventoryChanged;
      const char *key = expression.kind == LN_ValueExpressionKind::InventoryAmount  ? "amount" :
                        expression.kind == LN_ValueExpressionKind::InventoryChanged ? "changed" :
                        expression.kind == LN_ValueExpressionKind::InventoryMade    ? "made" :
                        expression.kind == LN_ValueExpressionKind::InventoryOutcome ?
                                                                                    "outcome" :
                                                                                    "inventory";
      /* The Inventory node's outputs: what its last run did, kept in a tree property (a dict of
       * the new list, the amount, what it made and the outcome). */
      if (expression.property_ref_index != LN_INVALID_INDEX) {
        const LN_Value *stored = GetTreePropertyValue(expression.property_ref_index);
        if (stored != nullptr && stored->exists && stored->type == LN_ValueType::Dict) {
          const auto item = stored->dict_value.find(key);
          if (item != stored->dict_value.end()) {
            return item->second;
          }
        }
        if (amount) {
          return MakeIntValue(0);
        }
        if (expression.kind != LN_ValueExpressionKind::InventoryEdit) {
          return MakeStringValue(std::string());
        }
        LN_Value empty;
        empty.type = LN_ValueType::List;
        empty.exists = true;
        return empty;
      }
      LN_Value inventory = in.Value(expression.input0, MakeNoneValue());
      if (inventory.type != LN_ValueType::List || !inventory.exists) {
        inventory = LN_Value();
        inventory.type = LN_ValueType::List;
        inventory.exists = true;
      }
      const InventoryEditResult result = RunInventoryEdit(in, expression, inventory.list_value);
      switch (expression.kind) {
        case LN_ValueExpressionKind::InventoryAmount:
          return MakeIntValue(result.amount);
        case LN_ValueExpressionKind::InventoryChanged:
          return MakeIntValue(result.Changed(LN_InventoryOperation(expression.value.int_value)));
        case LN_ValueExpressionKind::InventoryMade:
          return MakeStringValue(result.made);
        case LN_ValueExpressionKind::InventoryOutcome:
          return MakeStringValue(LN_InventoryOutcomeName(result.outcome, result.craft));
        default:
          return inventory;
      }
    }
    case LN_ValueExpressionKind::InventoryItemCount: {
      const LN_Value inventory = in.Value(expression.input0, MakeNoneValue());
      const std::string item = ItemNameOf(
          in.Value(expression.input1, MakeNoneValue()));
      return MakeIntValue(inventory.type == LN_ValueType::List && inventory.exists ?
                              LN_InventoryCount(inventory.list_value, item) :
                              0);
    }
    case LN_ValueExpressionKind::ItemLibraryInfo: {
      const std::string item = ItemNameOf(
          in.Value(expression.input1, MakeNoneValue()));
      const blender::Collection *library = ResolveCollectionByValue(
          this, in.Value(expression.input0, MakeNoneValue()));
      if (library == nullptr) {
        library = SceneItemLibrary();
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      KX_GameObject *object = scene ? LN_FindLibraryItem(*scene, library, item) : nullptr;
      const auto property = [&](const std::string &name) {
        EXP_Value *value = object ? object->GetProperty(name) : nullptr;
        return value ? MakeValueFromEXPValue(value, this) : MakeNoneValue();
      };
      switch (LN_ItemInfoField(expression.value.int_value)) {
        case LN_ItemInfoField::Found:
          return MakeBoolValue(object != nullptr);
        case LN_ItemInfoField::Object:
          return MakeObjectValue(this, object);
        case LN_ItemInfoField::Name: {
          const LN_Value name = property("name");
          return MakeStringValue(name.exists && name.type == LN_ValueType::String ?
                                     name.string_value :
                                     item);
        }
        case LN_ItemInfoField::MaxStack:
          return MakeIntValue(ValueAsInt(property("max_stack"), LN_FloatToIntMode::Round));
        case LN_ItemInfoField::Category:
        case LN_ItemInfoField::Description: {
          const LN_Value text = property(LN_ItemInfoField(expression.value.int_value) ==
                                                 LN_ItemInfoField::Category ?
                                             "category" :
                                             "description");
          return MakeStringValue(text.exists && text.type == LN_ValueType::String ?
                                     text.string_value :
                                     std::string());
        }
      }
      return fallback;
    }
    case LN_ValueExpressionKind::RecipeInfo: {
      const LN_Value text = in.Value(expression.input0, MakeNoneValue());
      LN_Recipe recipe;
      const bool valid = text.exists && text.type == LN_ValueType::String &&
                         LN_ParseRecipe(text.string_value, recipe);
      switch (LN_RecipeInfoField(expression.value.int_value)) {
        case LN_RecipeInfoField::Valid:
          return MakeBoolValue(valid);
        case LN_RecipeInfoField::Result:
          return MakeStringValue(valid ? recipe.results.front().item : std::string());
        case LN_RecipeInfoField::Count:
          return MakeIntValue(valid ? recipe.results.front().count : 0);
        case LN_RecipeInfoField::Results:
        case LN_RecipeInfoField::Ingredients: {
          LN_Value parts;
          parts.type = LN_ValueType::List;
          parts.exists = true;
          for (const LN_RecipePart &part :
               LN_RecipeInfoField(expression.value.int_value) == LN_RecipeInfoField::Results ?
                   recipe.results :
                   recipe.ingredients)
          {
            parts.list_value.push_back(LN_MakeInventoryEntry(part.item, part.count));
          }
          return parts;
        }
        case LN_RecipeInfoField::Station:
          return MakeStringValue(recipe.station);
        case LN_RecipeInfoField::CanCraft:
        case LN_RecipeInfoField::Craftable: {
          const LN_Value inventory = valid ? in.Value(expression.input1, MakeNoneValue()) :
                                             MakeNoneValue();
          const int32_t craftable = inventory.exists && inventory.type == LN_ValueType::List ?
                                        LN_RecipeCraftable(inventory.list_value, recipe) :
                                        0;
          if (LN_RecipeInfoField(expression.value.int_value) == LN_RecipeInfoField::CanCraft) {
            return MakeBoolValue(craftable > 0);
          }
          return MakeIntValue(craftable);
        }
      }
      return fallback;
    }
    case LN_ValueExpressionKind::RecipeFind: {
      const LN_Value recipes_value = in.Value(expression.input0, MakeNoneValue());
      const LN_Value items = in.Value(expression.input1, MakeNoneValue());
      const LN_Value station = in.Value(expression.input2, MakeNoneValue());
      /* Trimmed like the stations of the recipe lines. */
      std::string station_name = station.exists && station.type == LN_ValueType::String ?
                                     station.string_value :
                                     std::string();
      const size_t station_start = station_name.find_first_not_of(" \t\r\n");
      station_name = station_start == std::string::npos ?
                         std::string() :
                         station_name.substr(station_start,
                                             station_name.find_last_not_of(" \t\r\n") -
                                                 station_start + 1);
      std::vector<LN_RecipePart> available;
      const auto add_item = [&](const LN_Value &item) {
        LN_RecipePart part;
        if (item.exists && item.type == LN_ValueType::ObjectRef) {
          part.item = item.reference_name;
        }
        else if (!LN_InventoryEntry(item, part.item, part.count)) {
          return;
        }
        available.push_back(std::move(part));
      };
      if (items.exists && items.type == LN_ValueType::List) {
        for (const LN_Value &item : items.list_value) {
          add_item(item);
        }
      }
      else {
        add_item(items);
      }
      std::vector<LN_Recipe> recipes;
      std::vector<int32_t> indices;
      LN_ParseRecipes(recipes_value, recipes, &indices);
      int32_t found = -1;
      for (size_t index = 0; index < recipes.size(); ++index) {
        if (recipes[index].station == station_name &&
            LN_RecipeMatches(recipes[index], available))
        {
          found = int32_t(index);
          break;
        }
      }
      switch (LN_RecipeFindField(expression.value.int_value)) {
        case LN_RecipeFindField::Found:
          return MakeBoolValue(found >= 0);
        case LN_RecipeFindField::Recipe:
          if (found < 0) {
            return MakeStringValue(std::string());
          }
          return MakeStringValue(recipes_value.type == LN_ValueType::List ?
                                     recipes_value.list_value[size_t(indices[size_t(found)])]
                                         .string_value :
                                     recipes_value.string_value);
        case LN_RecipeFindField::Index:
          return MakeIntValue(found >= 0 ? indices[size_t(found)] : -1);
      }
      return fallback;
    }
    case LN_ValueExpressionKind::DictRemoveKey:
    case LN_ValueExpressionKind::DictRemoveKeyValue: {
      LN_Value dict_output;
      dict_output.type = LN_ValueType::Dict;
      dict_output.exists = true;
      LN_Value removed_value = MakeNoneValue();
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value dict_value = in.Value(expression.input0, MakeNoneValue());
        if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
          dict_output.dict_value = dict_value.dict_value;
        }
      }
      const std::string key = expression.input1 != LN_INVALID_INDEX ?
                                  in.String(expression.input1, std::string()) :
                                  std::string();
      if (!key.empty()) {
        const auto iter = dict_output.dict_value.find(key);
        if (iter != dict_output.dict_value.end()) {
          removed_value = iter->second;
          dict_output.dict_value.erase(iter);
        }
      }
      return expression.kind == LN_ValueExpressionKind::DictRemoveKeyValue ? removed_value :
                                                                             dict_output;
    }
    case LN_ValueExpressionKind::ListFromItems: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(expression.input_indices.size());
      for (const uint32_t input_index : expression.input_indices) {
        if (input_index != LN_INVALID_INDEX) {
          value.list_value.push_back(
              in.Value(input_index, MakeNoneValue()));
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::GetGlobalProperty: {
      const std::string category = in.String(expression.input0, std::string());
      const std::string property = in.String(expression.input1, std::string());
      if (!category.empty() && !property.empty()) {
        std::lock_guard<std::mutex> lock(GlobalPropertiesMutex());
        EnsureGlobalCategoryLoadedUnlocked(category);
        const auto category_iter = GlobalProperties().find(category);
        if (category_iter != GlobalProperties().end()) {
          const auto property_iter = category_iter->second.find(property);
          if (property_iter != category_iter->second.end()) {
            LN_Value value = property_iter->second;
            LocalizeValue(value);
            return value;
          }
        }
      }
      if (expression.input2 != LN_INVALID_INDEX) {
        return in.Value(expression.input2, MakeNoneValue());
      }
      return MakeNoneValue();
    }
    case LN_ValueExpressionKind::ListGlobalProperties: {
      const std::string category = in.String(expression.input0, std::string());
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      if (!category.empty()) {
        std::lock_guard<std::mutex> lock(GlobalPropertiesMutex());
        EnsureGlobalCategoryLoadedUnlocked(category);
        const auto category_iter = GlobalProperties().find(category);
        if (category_iter != GlobalProperties().end()) {
          value.dict_value = category_iter->second;
        }
      }
      LocalizeValue(value);
      return value;
    }
    case LN_ValueExpressionKind::LoadVariable: {
      const std::string path = in.String(expression.input0, "//Data");
      const std::string file_name = in.String(expression.input1, "variables");
      const std::string name = in.String(expression.input2, "var");
      const LN_Value dict = LoadVariableDictFromFile(path, file_name);
      if (dict.type == LN_ValueType::Dict) {
        const auto iter = dict.dict_value.find(name);
        if (iter != dict.dict_value.end()) {
          return iter->second;
        }
      }
      if (!expression.input_indices.empty()) {
        return in.Value(expression.input_indices.front(), MakeNoneValue());
      }
      return MakeNoneValue();
    }
    case LN_ValueExpressionKind::LoadVariableDict: {
      const std::string path = in.String(expression.input0, "//Data");
      const std::string file_name = in.String(expression.input1, "variables");
      return LoadVariableDictFromFile(path, file_name);
    }
    case LN_ValueExpressionKind::ListSavedVariables: {
      const std::string path = in.String(expression.input0, "//Data");
      const std::string file_name = in.String(expression.input1, "variables");
      const LN_Value dict = LoadVariableDictFromFile(path, file_name);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (dict.type == LN_ValueType::Dict && dict.exists) {
        for (const auto &item : dict.dict_value) {
          LN_Value key;
          key.type = LN_ValueType::String;
          key.exists = true;
          key.string_value = item.first;
          value.list_value.push_back(key);
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::CurrentScene: {
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      if (scene == nullptr) {
        return MakeNoneValue();
      }
      LN_Value value;
      value.type = LN_ValueType::SceneRef;
      value.exists = true;
      value.reference_name = scene->GetName();
      value.runtime_ref = MakeSceneRef(scene, scene->GetName());
      return value;
    }
    case LN_ValueExpressionKind::CollectionObjects:
    case LN_ValueExpressionKind::CollectionObjectNames: {
      const LN_Value collection_value = in.Value(expression.input0, MakeNoneValue());
      blender::Collection *collection = nullptr;
      if (collection_value.type == LN_ValueType::CollectionRef &&
          collection_value.runtime_ref.IsValid())
      {
        collection = ResolveCollectionRef(collection_value.runtime_ref);
      }

      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (collection == nullptr) {
        return value;
      }
      if (expression.kind == LN_ValueExpressionKind::CollectionObjects) {
        /* The scene's objects in the collection while the game runs (KX_Scene::
         * CollectionMembers): Add Object copies and Set Collection Member included. Before
         * 2026-10-15 only the file's direct entries, found by name. */
        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        if (scene == nullptr) {
          return value;
        }
        const bool include_children = in.Bool(expression.bool_expr_index, false);
        const std::vector<KX_GameObject *> &members = scene->CollectionMembers(collection,
                                                                               include_children);
        value.list_value.reserve(members.size());
        for (KX_GameObject *game_object : members) {
          /* Logic in a player's copy of a menu sees its own copy's objects, not the other
           * players' copies of that menu (as when this list was made by name). */
          if (scene->OverlayCounterpart(game_object, m_gameObject) != game_object) {
            continue;
          }
          LN_Value item;
          item.type = LN_ValueType::ObjectRef;
          item.exists = true;
          item.reference_name = game_object->GetName();
          item.runtime_ref = MakeObjectRef(game_object, game_object->GetName());
          value.list_value.push_back(std::move(item));
        }
        return value;
      }
      /* Names: the file's entries (such as a library's template names). */
      for (blender::CollectionObject &collection_object : collection->gobject) {
        if (collection_object.ob == nullptr) {
          continue;
        }
        LN_Value item;
        item.type = LN_ValueType::String;
        item.exists = true;
        item.string_value = collection_object.ob->id.name + 2;
        value.list_value.push_back(item);
      }
      return value;
    }
    case LN_ValueExpressionKind::RigidBodyConstraintNames: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;

      KX_GameObject *target = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target = ResolveObjectValue(
            in.Value(expression.input0, MakeNoneValue()));
      }
      if (!UsesJoltPhysics(target)) {
        return value;
      }

      const std::vector<KX_GameObject::RigidBodyConstraintData> &constraints =
          target->GetRigidBodyConstraints();
      const LN_RigidBodyConstraintMatchMode match_mode = ValidRigidBodyConstraintMatchMode(
          expression.rigid_body_constraint_match_mode);
      const std::string query = match_mode == LN_RigidBodyConstraintMatchMode::All ?
                                    std::string() :
                                    in.String(expression.string_expr_index, std::string());
      if (match_mode != LN_RigidBodyConstraintMatchMode::All && query.empty()) {
        return value;
      }
      if (match_mode == LN_RigidBodyConstraintMatchMode::Exact) {
        if (const KX_GameObject::RigidBodyConstraintData *constraint =
                target->FindRigidBodyConstraint(query))
        {
          LN_Value item;
          item.type = LN_ValueType::String;
          item.exists = true;
          item.string_value = constraint->m_name;
          value.list_value.push_back(std::move(item));
        }
        return value;
      }
      value.list_value.reserve(constraints.size());
      for (const KX_GameObject::RigidBodyConstraintData &constraint : constraints) {
        if (!RigidBodyConstraintNameMatches(constraint.m_name, query, match_mode)) {
          continue;
        }
        LN_Value item;
        item.type = LN_ValueType::String;
        item.exists = true;
        item.string_value = constraint.m_name;
        value.list_value.push_back(std::move(item));
      }
      return value;
    }
    case LN_ValueExpressionKind::DictGetKeys: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      const LN_Value dict_value = expression.input0 != LN_INVALID_INDEX ?
                                      in.Value(expression.input0, MakeNoneValue()) :
                                      MakeNoneValue();
      if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
        for (const auto &entry : dict_value.dict_value) {
          LN_Value key_value;
          key_value.type = LN_ValueType::String;
          key_value.exists = true;
          key_value.string_value = entry.first;
          value.list_value.push_back(key_value);
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::EmptyList: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      const int32_t length = expression.input0 != LN_INVALID_INDEX ?
                                 in.Int(expression.input0, 0) :
                                 0;
      if (length > 0) {
        value.list_value.reserve(size_t(length));
        for (int32_t index = 0; index < length; index++) {
          value.list_value.push_back(MakeNoneValue());
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::EmptyDict: {
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      return value;
    }
    case LN_ValueExpressionKind::Count:
      break;
  }

  return fallback;
}

template<class In>
void LN_RuntimeTree::QueryKind(In &in,
    const uint32_t expression_index,
    const LN_QueryExpression &expression,
    QueryExpressionRuntimeState &state)
{
  const LN_TickContext &context = in.context;
  switch (expression.kind) {
    case LN_QueryExpressionKind::Raycast:
    case LN_QueryExpressionKind::RaycastAll: {
      const bool collect_all = expression.kind == LN_QueryExpressionKind::RaycastAll;
      const LN_Value caster_value = in.Value(expression.input0, MakeNoneValue());
      KX_GameObject *caster_object = ResolveObjectValue(caster_value);
      const std::vector<LN_RuntimeRef> extra_ignore_refs = ResolveIgnoreObjectRefs(in, expression.input3, expression.ignore_list_expr_index);

      MT_Vector3 origin = in.Vector(expression.input1, expression.vector_value);
      MT_Vector3 destination = in.Vector(expression.input2, expression.secondary_vector_value);
      MT_Vector3 direction = in.Vector(expression.input4, expression.tertiary_vector_value);
      const float requested_distance = in.Float(expression.float_expr_index, expression.float_value);
      const bool local_space = in.Bool(expression.bool_expr_index, expression.bool_value);
      if (local_space && !caster_object) {
        caster_object = m_gameObject;
      }
      if (local_space && caster_object) {
        FlushPendingRaycastTransforms(caster_object, context);
        if (expression.ray_input_mode == LN_RayInputMode::EndPoint) {
          const MT_Transform transform = caster_object->NodeGetWorldTransform();
          origin = transform * origin;
          destination = transform * destination;
        }
        else {
          origin = caster_object->NodeGetWorldTransform() * origin;
          direction = caster_object->NodeGetWorldOrientation() * direction;
        }
      }

      const bool direction_mode = expression.ray_input_mode == LN_RayInputMode::DirectionDistance;
      const bool finite_inputs = vector_is_finite(origin) &&
                                 (direction_mode ? vector_is_finite(direction) &&
                                                       std::isfinite(requested_distance) :
                                                   vector_is_finite(destination));
      if (!finite_inputs) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      if (direction_mode) {
        const float direction_length = direction.length();
        if (requested_distance <= 0.0f || !std::isfinite(direction_length) ||
            direction_length <= MT_EPSILON)
        {
          state.cached_result.physics_query.diagnostic_status =
              LN_QueryDiagnosticStatus::InvalidTarget;
          break;
        }
        direction /= direction_length;
        destination = origin + direction * requested_distance;
        if (!vector_is_finite(destination)) {
          state.cached_result.physics_query.diagnostic_status =
              LN_QueryDiagnosticStatus::InvalidTarget;
          break;
        }
      }
      const float cast_distance = (destination - origin).length();
      if (!std::isfinite(cast_distance) || cast_distance <= MT_EPSILON) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      if (!direction_mode) {
        direction = (destination - origin) / cast_distance;
      }

      LN_RuntimeRef caster_ref;
      if (caster_object) {
        caster_ref = MakeObjectRef(caster_object, caster_object->GetName());
      }

      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
      if (!environment) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::MissingPhysicsWorld;
        break;
      }

      const int32_t mask_value = in.Int(expression.int_expr_index, expression.int_value);
      if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidFilter;
        break;
      }
      const std::string property_name = in.String(expression.string_expr_index, std::string());
      const bool xray = in.Bool(expression.secondary_bool_expr_index, false);
      const int32_t max_results = collect_all ? std::clamp(in.Int(expression.secondary_int_expr_index, expression.secondary_int_value),
                                                           1,
                                                           256) :
                                                1;

      PHY_RayQuerySettings settings;
      settings.origin = origin;
      settings.destination = destination;
      settings.max_results = uint32_t(max_results +
                                      (collect_all && !xray && !property_name.empty() ? 1 : 0));
      if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_FACE_INDEX) != 0) {
        settings.detail_flags |= PHY_RAY_QUERY_DETAIL_FACE_INDEX;
      }
      if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_UV) != 0) {
        settings.detail_flags |= PHY_RAY_QUERY_DETAIL_UV;
      }
      settings.include_sensors = in.Bool(expression.quaternary_bool_expr_index, false);
      settings.hit_back_faces = in.Bool(expression.quinary_bool_expr_index, false);

      LN_PhysicsQueryResult fallback;
      fallback.hit_position = destination;
      fallback.hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
      fallback.ray_direction = direction;
      fallback.end_position = destination;
      fallback.hit_fraction = 1.0f;
      fallback.hit_distance = cast_distance;
      state.cached_result.physics_query = fallback;

      bool blocked = false;
      bool supported = false;
      LN_PhysicsQueryResult blocker;
      state.cached_result.physics_query_results = RayCast(settings,
                                                          caster_ref,
                                                          uint32_t(mask_value),
                                                          property_name,
                                                          xray,
                                                          extra_ignore_refs,
                                                          blocked,
                                                          blocker,
                                                          supported);
      if (!supported) {
        state.cached_result.physics_query = LN_PhysicsQueryResult();
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::UnsupportedPhysicsBackend;
        state.cached_result.physics_query_results.clear();
        break;
      }
      if (!state.cached_result.physics_query_results.empty()) {
        state.cached_result.physics_query = state.cached_result.physics_query_results.front();
      }
      else if (blocked) {
        state.cached_result.physics_query = blocker;
      }
      state.cached_result.physics_query.blocked = blocked;
      if (state.cached_result.physics_query_results.size() > size_t(max_results)) {
        state.cached_result.physics_query_results.resize(size_t(max_results));
      }
      if (in.Bool(expression.tertiary_bool_expr_index, false)) {
        const bool physical_stop = state.cached_result.physics_query.hit || blocked;
        DrawDebugRaycast(origin,
                         state.cached_result.physics_query.hit ?
                             state.cached_result.physics_query.hit_position :
                         blocked ? blocker.hit_position :
                                   destination,
                         physical_stop,
                         state.cached_result.physics_query.hit ?
                             &state.cached_result.physics_query.hit_normal :
                             nullptr,
                         cast_distance);
      }
      break;
    }
    case LN_QueryExpressionKind::RaycastFromMesh: {
      KX_GameObject *source_object = expression.input0 != LN_INVALID_INDEX ?
                                         ResolveObjectValue(in.Value(expression.input0, MakeNoneValue())) :
                                         m_gameObject;
      if (!source_object) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      FlushPendingRaycastTransforms(source_object, context);
      const float start_offset = in.Float(expression.float_expr_index, expression.float_value);
      const float max_distance = in.Float(expression.secondary_float_expr_index, expression.secondary_float_value);
      if (!std::isfinite(start_offset) || start_offset < 0.0f || !std::isfinite(max_distance) ||
          max_distance <= MT_EPSILON)
      {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      const int32_t mask_value = in.Int(expression.int_expr_index, expression.int_value);
      if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidFilter;
        break;
      }
      if ((expression.mesh_ray_domain != LN_MeshRayDomain::Vertices &&
           expression.mesh_ray_domain != LN_MeshRayDomain::Faces) ||
          (expression.mesh_ray_direction != LN_MeshRayDirection::Normals &&
           expression.mesh_ray_direction != LN_MeshRayDirection::InvertedNormals))
      {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      const size_t max_rays = size_t(std::clamp(
          in.Int(expression.secondary_int_expr_index, expression.secondary_int_value),
          1,
          LN_RAYCAST_FROM_MESH_MAX_RAYS));

      const MT_Transform world_transform = source_object->NodeGetWorldTransform();
      if (!vector_is_finite(world_transform.getOrigin())) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      MT_Matrix3x3 normal_matrix;
      if (!BuildMeshNormalMatrix(world_transform.getBasis(), normal_matrix)) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      const std::vector<LN_MeshRaySample> samples = SampleMeshRays(
          *source_object, expression.mesh_ray_domain, max_rays);
      if (samples.empty()) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      std::vector<PHY_RayQuerySettings> settings;
      settings.reserve(samples.size());
      state.cached_result.physics_query_results.reserve(samples.size());
      const bool invert_direction = expression.mesh_ray_direction ==
                                    LN_MeshRayDirection::InvertedNormals;
      const bool include_sensors = in.Bool(expression.quaternary_bool_expr_index, false);
      const bool hit_back_faces = in.Bool(expression.quinary_bool_expr_index, false);
      for (const LN_MeshRaySample &sample : samples) {
        MT_Vector3 direction = normal_matrix * sample.normal;
        if (!NormalizeFinite(direction)) {
          continue;
        }
        if (invert_direction) {
          direction = -direction;
        }
        const MT_Vector3 surface_position = world_transform * sample.position;
        const MT_Vector3 origin = surface_position + direction * start_offset;
        const MT_Vector3 destination = origin + direction * max_distance;
        if (!vector_is_finite(surface_position) || !vector_is_finite(origin) ||
            !vector_is_finite(destination))
        {
          continue;
        }
        /* Validate the segment after performing the world-space float arithmetic. At very large
         * coordinates a finite positive Max Distance can round away entirely, while an extreme
         * finite distance can still overflow the derived length. Report the segment that is
         * actually sent to physics rather than the idealized input distance. */
        MT_Vector3 cast_direction = destination - origin;
        const float cast_distance = cast_direction.length();
        if (!std::isfinite(cast_distance) || cast_distance <= MT_EPSILON) {
          continue;
        }
        cast_direction /= cast_distance;
        if (!vector_is_finite(cast_direction)) {
          continue;
        }

        PHY_RayQuerySettings ray_settings;
        ray_settings.origin = origin;
        ray_settings.destination = destination;
        ray_settings.max_results = 1;
        if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_FACE_INDEX) != 0) {
          ray_settings.detail_flags |= PHY_RAY_QUERY_DETAIL_FACE_INDEX;
        }
        if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_UV) != 0) {
          ray_settings.detail_flags |= PHY_RAY_QUERY_DETAIL_UV;
        }
        ray_settings.include_sensors = include_sensors;
        ray_settings.hit_back_faces = hit_back_faces;
        settings.push_back(ray_settings);

        LN_PhysicsQueryResult miss;
        miss.diagnostic_status = LN_QueryDiagnosticStatus::NoHit;
        miss.hit_position = destination;
        miss.hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
        miss.ray_origin = origin;
        miss.ray_direction = cast_direction;
        miss.end_position = destination;
        miss.hit_fraction = 1.0f;
        miss.hit_distance = cast_distance;
        miss.polygon_index = -1;
        state.cached_result.physics_query_results.push_back(std::move(miss));
      }
      if (settings.empty()) {
        state.cached_result.physics_query_results.clear();
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      KX_Scene *scene = source_object->GetScene();
      PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
      if (!environment) {
        state.cached_result.physics_query = state.cached_result.physics_query_results.front();
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::MissingPhysicsWorld;
        break;
      }

      const std::vector<LN_RuntimeRef> extra_ignore_refs = ResolveIgnoreObjectRefs(in, expression.input1, expression.ignore_list_expr_index);
      const LN_PhysicsControllerIgnoreSet extra_ignore_controllers(*this, extra_ignore_refs);
      const std::string property_name = in.String(expression.string_expr_index, std::string());
      const bool xray = in.Bool(expression.secondary_bool_expr_index, false);
      LN_RayQueryFilterCallback filter(RaycastOwningPhysicsController(source_object),
                                       extra_ignore_controllers,
                                       uint32_t(mask_value),
                                       property_name,
                                       xray);
      std::vector<std::vector<PHY_RayCastResult>> physics_results;
      if (!environment->RayCastBatch(
              settings, filter, PHY_RayBatchExecution::Adaptive, physics_results))
      {
        state.cached_result.physics_query = state.cached_result.physics_query_results.front();
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::UnsupportedPhysicsBackend;
        break;
      }

      bool any_hit = false;
      bool any_blocked = false;
      size_t first_hit = size_t(-1);
      const size_t result_count = std::min(physics_results.size(),
                                           state.cached_result.physics_query_results.size());
      for (size_t ray_index = 0; ray_index < result_count; ray_index++) {
        LN_PhysicsQueryResult &result = state.cached_result.physics_query_results[ray_index];
        if (physics_results[ray_index].empty()) {
          continue;
        }
        const PHY_RayCastResult &physics_result = physics_results[ray_index].front();
        KX_ClientObjectInfo *info = physics_result.m_controller ?
                                        static_cast<KX_ClientObjectInfo *>(
                                            physics_result.m_controller->GetNewClientInfo()) :
                                        nullptr;
        KX_GameObject *hit_object = info ? info->m_gameobject : nullptr;
        if (!hit_object) {
          continue;
        }

        if (!std::isfinite(physics_result.m_fraction) ||
            !vector_is_finite(physics_result.m_hitPoint))
        {
          continue;
        }
        const float fraction = std::clamp(physics_result.m_fraction, 0.0f, 1.0f);
        result.hit_position = physics_result.m_hitPoint;
        result.hit_fraction = fraction;
        result.hit_distance *= fraction;
        const bool property_matches = property_name.empty() ||
                                      hit_object->GetProperty(property_name);
        if (!property_matches) {
          if (!xray) {
            result.blocked = true;
            any_blocked = true;
          }
          continue;
        }

        result.hit = true;
        result.diagnostic_status = LN_QueryDiagnosticStatus::Hit;
        result.object_ref = MakeObjectRef(hit_object, hit_object->GetName());
        result.hit_normal = vector_is_finite(physics_result.m_hitNormal) ?
                                physics_result.m_hitNormal :
                                MT_Vector3(0.0f, 0.0f, 0.0f);
        result.polygon_index = physics_result.m_polygon;
        result.has_uv = physics_result.m_hitUVOK != 0 &&
                        std::isfinite(physics_result.m_hitUV.x()) &&
                        std::isfinite(physics_result.m_hitUV.y());
        result.hit_uv = result.has_uv ? physics_result.m_hitUV : MT_Vector2(0.0f, 0.0f);
        any_hit = true;
        if (first_hit == size_t(-1)) {
          first_hit = ray_index;
        }
      }

      state.cached_result.physics_query =
          first_hit != size_t(-1) ? state.cached_result.physics_query_results[first_hit] :
                                    state.cached_result.physics_query_results.front();
      state.cached_result.physics_query.hit = any_hit;
      state.cached_result.physics_query.blocked = any_blocked;
      state.cached_result.physics_query.diagnostic_status = any_hit ?
                                                                LN_QueryDiagnosticStatus::Hit :
                                                                LN_QueryDiagnosticStatus::NoHit;

      if (in.Bool(expression.tertiary_bool_expr_index, false)) {
        for (const LN_PhysicsQueryResult &result : state.cached_result.physics_query_results) {
          const bool physical_stop = result.hit || result.blocked;
          DrawDebugRaycast(result.ray_origin,
                           physical_stop ? result.hit_position : result.end_position,
                           physical_stop,
                           result.hit ? &result.hit_normal : nullptr,
                           max_distance);
        }
      }
      break;
    }
    case LN_QueryExpressionKind::ShapeCast:
    case LN_QueryExpressionKind::ShapeCastAll: {
      const bool collect_all = expression.kind == LN_QueryExpressionKind::ShapeCastAll;
      const LN_Value caster_value = in.Value(expression.input0, MakeNoneValue());
      KX_GameObject *caster_object = ResolveObjectValue(caster_value);
      const std::vector<LN_RuntimeRef> extra_ignore_refs = ResolveIgnoreObjectRefs(in, expression.input3, expression.ignore_list_expr_index);

      MT_Vector3 origin = in.Vector(expression.input1, expression.vector_value);
      MT_Vector3 destination = in.Vector(expression.input2, expression.secondary_vector_value);
      const LN_Rotation rotation = in.Rotation(expression.input4, LN_Rotation::FromEulerXYZ(expression.tertiary_vector_value));
      const MT_Vector3 half_extents = in.Vector(expression.input5, expression.quaternary_vector_value);
      MT_Matrix3x3 orientation = rotation.ToMatrix();
      const bool local_space = in.Bool(expression.bool_expr_index, expression.bool_value);
      if (local_space && !caster_object) {
        caster_object = m_gameObject;
      }
      if (local_space && caster_object) {
        FlushPendingRaycastTransforms(caster_object, context);
        const MT_Transform transform = caster_object->NodeGetWorldTransform();
        origin = transform * origin;
        destination = transform * destination;
        orientation = caster_object->NodeGetWorldOrientation() * orientation;
      }

      LN_RuntimeRef caster_ref;
      if (caster_object) {
        caster_ref = MakeObjectRef(caster_object, caster_object->GetName());
      }

      const float radius = in.Float(expression.float_expr_index, expression.float_value);
      const float height = in.Float(expression.secondary_float_expr_index, expression.secondary_float_value);
      const float extra_radius = in.Float(expression.tertiary_float_expr_index, expression.tertiary_float_value);
      const bool finite_inputs = vector_is_finite(origin) && vector_is_finite(destination) &&
                                 vector_is_finite(half_extents) && std::isfinite(radius) &&
                                 std::isfinite(height) && std::isfinite(extra_radius);
      const bool valid_shape = expression.shape_cast_type == LN_ShapeCastType::Box ?
                                   half_extents.x() > 0.0f && half_extents.y() > 0.0f &&
                                       half_extents.z() > 0.0f :
                               expression.shape_cast_type == LN_ShapeCastType::Capsule ?
                                   radius > 0.0f && height > 0.0f :
                                   radius > 0.0f;
      if (!finite_inputs || !valid_shape || extra_radius < 0.0f) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      const MT_Vector3 cast_displacement = destination - origin;
      const float cast_distance = cast_displacement.length();
      if (!std::isfinite(cast_distance)) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      const int32_t mask_value = in.Int(expression.int_expr_index, expression.int_value);
      if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidFilter;
        break;
      }
      const int32_t max_results = collect_all ? std::clamp(in.Int(expression.secondary_int_expr_index, expression.secondary_int_value),
                                                           1,
                                                           256) :
                                                1;
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      if (!scene || !scene->GetPhysicsEnvironment()) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::MissingPhysicsWorld;
        break;
      }

      PHY_ShapeCastSettings settings;
      settings.type = static_cast<PHY_ShapeCastType>(expression.shape_cast_type);
      settings.origin = origin;
      settings.destination = destination;
      settings.orientation = orientation;
      settings.half_extents = half_extents;
      settings.radius = radius;
      settings.height = height;
      settings.extra_radius = extra_radius;
      if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_FACE_INDEX) != 0) {
        settings.detail_flags |= PHY_RAY_QUERY_DETAIL_FACE_INDEX;
      }
      if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_UV) != 0) {
        settings.detail_flags |= PHY_RAY_QUERY_DETAIL_UV;
      }
      settings.include_sensors = in.Bool(expression.quaternary_bool_expr_index, false);
      settings.hit_back_faces = in.Bool(expression.quinary_bool_expr_index, false);

      const std::string property_name = in.String(expression.string_expr_index, std::string());
      const bool xray = in.Bool(expression.secondary_bool_expr_index, false);
      settings.max_results = uint32_t(max_results +
                                      (collect_all && !xray && !property_name.empty() ? 1 : 0));
      LN_PhysicsQueryResult fallback;
      fallback.cast_position = destination;
      fallback.ray_direction = cast_distance > MT_EPSILON ? cast_displacement / cast_distance :
                                                            MT_Vector3(0.0f, 0.0f, 0.0f);
      fallback.hit_fraction = 1.0f;
      fallback.hit_distance = cast_distance;
      state.cached_result.physics_query = fallback;

      bool blocked = false;
      bool supported = false;
      LN_PhysicsQueryResult blocker;
      state.cached_result.physics_query_results = ShapeCast(settings,
                                                            caster_ref,
                                                            uint32_t(mask_value),
                                                            property_name,
                                                            xray,
                                                            extra_ignore_refs,
                                                            blocked,
                                                            blocker,
                                                            supported);
      if (!supported) {
        state.cached_result.physics_query = LN_PhysicsQueryResult();
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::UnsupportedPhysicsBackend;
        break;
      }
      if (!state.cached_result.physics_query_results.empty()) {
        state.cached_result.physics_query = state.cached_result.physics_query_results.front();
      }
      else if (blocked) {
        state.cached_result.physics_query = blocker;
      }
      state.cached_result.physics_query.blocked = blocked;
      if (state.cached_result.physics_query_results.size() > size_t(max_results)) {
        state.cached_result.physics_query_results.resize(size_t(max_results));
      }

      if (in.Bool(expression.tertiary_bool_expr_index, false)) {
        DrawDebugShapeCast(settings,
                           state.cached_result.physics_query_results,
                           blocked ? &blocker : nullptr,
                           collect_all);
      }
      break;
    }
    case LN_QueryExpressionKind::MouseRay: {
      const float distance = in.Float(expression.float_expr_index, expression.float_value);
      if (distance < 0.0f) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
      }
      else {
        const std::string property_name = in.String(expression.string_expr_index, std::string());
        const bool xray = in.Bool(expression.bool_expr_index, expression.bool_value);
        const int32_t mask_value = in.Int(expression.int_expr_index, expression.int_value);
        if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
          state.cached_result.physics_query.diagnostic_status =
              LN_QueryDiagnosticStatus::InvalidFilter;
          break;
        }

        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        auto cast_view = [&](KX_Camera *camera,
                             const LN_InteractionDomain domain) -> LN_PhysicsQueryResult {
          LN_PhysicsQueryResult result;
          if (camera == nullptr || camera->GetScene() != scene) {
            result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
            return result;
          }
          FlushPendingRaycastTransforms(camera, context);
          MT_Vector3 from;
          MT_Vector3 to;
          if (!ComputeMouseRay(camera, distance, from, to)) {
            result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
            return result;
          }
          const LN_RuntimeRef camera_ref = MakeObjectRef(camera, camera->GetName());
          return RaycastPointer(from,
                                to,
                                camera,
                                camera_ref,
                                uint32_t(mask_value),
                                property_name,
                                xray,
                                domain,
                                context.command_execution_key);
        };

        switch (expression.interaction_view) {
          case LN_InteractionView::Automatic: {
            if (scene != nullptr && scene->GetOverlayCamera() != nullptr) {
              LN_PhysicsQueryResult overlay_result = cast_view(scene->GetOverlayCamera(),
                                                               LN_InteractionDomain::Overlay);
              if (overlay_result.hit || overlay_result.blocked) {
                state.cached_result.physics_query = std::move(overlay_result);
                break;
              }
            }
            state.cached_result.physics_query = cast_view(PointerWorldCamera(scene),
                                                          LN_InteractionDomain::Main);
            break;
          }
          case LN_InteractionView::Main:
            state.cached_result.physics_query = cast_view(PointerWorldCamera(scene),
                                                          LN_InteractionDomain::Main);
            break;
          case LN_InteractionView::Overlay:
            state.cached_result.physics_query = cast_view(
                scene ? scene->GetOverlayCamera() : nullptr, LN_InteractionDomain::Overlay);
            break;
          case LN_InteractionView::CustomCamera: {
            KX_Camera *camera = expression.input0 != LN_INVALID_INDEX ?
                                    ResolveCameraValue(in.Value(expression.input0, MakeNoneValue())) :
                                    nullptr;
            state.cached_result.physics_query = cast_view(camera, LN_InteractionDomain::Any);
            break;
          }
        }
      }
      break;
    }
    case LN_QueryExpressionKind::CameraRay: {
      /* The Camera input when it is a camera the engine has drawn with (its projection matrix is
       * set: a camera never drawn has none to cast through), else the scene's active camera. */
      KX_Camera *camera = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        KX_Camera *requested = ResolveCameraValue(in.Value(expression.input0, MakeNoneValue()));
        if (requested != nullptr && requested->hasValidProjectionMatrix()) {
          camera = requested;
        }
      }
      if (camera == nullptr) {
        camera = GetActiveCamera();
      }
      FlushPendingRaycastTransforms(camera, context);
      const MT_Vector3 aim = in.Vector(expression.input1, expression.vector_value);
      const float distance = in.Float(expression.float_expr_index, expression.float_value);
      MT_Vector3 from;
      MT_Vector3 to;
      if (distance < 0.0f) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
      }
      else if (ProjectScreenToWorld(camera, aim.x(), aim.y(), 0.0f, from) &&
               ProjectScreenToWorld(camera, aim.x(), aim.y(), std::max(distance, 0.0f), to))
      {
        const std::string property_name = in.String(expression.string_expr_index, std::string());
        const bool xray = in.Bool(expression.bool_expr_index, expression.bool_value);
        const int32_t mask_value = in.Int(expression.int_expr_index, expression.int_value);
        if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
          state.cached_result.physics_query.diagnostic_status =
              LN_QueryDiagnosticStatus::InvalidFilter;
          break;
        }
        LN_RuntimeRef camera_ref;
        if (camera != nullptr) {
          camera_ref = MakeObjectRef(camera, camera->GetName());
        }
        state.cached_result.physics_query = Raycast(
            from, to, camera_ref, uint32_t(mask_value), property_name, xray);
      }
      else {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
      }
      break;
    }
    case LN_QueryExpressionKind::UIElementEvent: {
      if (LN_UIElementEvent(expression.int_value) == LN_UIElementEvent::UISound) {
        /* On UI Sound: this frame's UI sounds, one a player, heard like On UI Event's events. */
        size_t count = 0;
        const LN_UIService::SoundEvent *sound =
            m_logicManager != nullptr ?
                m_logicManager->GetUIService().FindSoundEvent(
                    m_logicManager->GetUIService().MenuUserOfObject(m_gameObject),
                    state.ui_event_cursor,
                    &count) :
                nullptr;
        state.cached_result.ui_event_count = uint32_t(count);
        state.cached_result.valid = sound != nullptr;
        state.cached_result.physics_query.hit = sound != nullptr;
        state.cached_result.physics_query.diagnostic_status =
            sound != nullptr ? LN_QueryDiagnosticStatus::Hit : LN_QueryDiagnosticStatus::NoHit;
        if (sound == nullptr) {
          break;
        }
        state.cached_result.ui_sound = int32_t(sound->sound);
        state.cached_result.ui_menu = sound->menu;
        state.cached_result.ui_action = sound->action;
        state.cached_result.ui_user = sound->user;
        if (KX_GameObject *object = m_logicManager->ResolveObjectHandle(sound->object)) {
          state.cached_result.ui_source_object = MakeObjectRef(object, object->GetName());
        }
        break;
      }
      /* A tree in a player's menu hears only that player's UI events (split screen: each menu's
       * copy of the same tree); elsewhere it hears every player's. The cursor picks the event
       * whose turn it is: the route runs again for each further one (ExecuteEvent), last event
       * first, so the topmost element's event runs last and what it sets stays. */
      size_t count = 0;
      const LN_UIService::ElementEvent *event =
          m_logicManager != nullptr ?
              m_logicManager->GetUIService().FindElementEvent(
                  LN_UIElementEvent(expression.int_value),
                  in.String(expression.string_expr_index, std::string()),
                  m_logicManager->GetUIService().MenuUserOfObject(m_gameObject),
                  state.ui_event_cursor,
                  &count,
                  true) :
              nullptr;
      state.cached_result.ui_event_count = uint32_t(count);
      state.cached_result.valid = event != nullptr;
      state.cached_result.physics_query.hit = event != nullptr;
      state.cached_result.physics_query.diagnostic_status =
          event != nullptr ? LN_QueryDiagnosticStatus::Hit : LN_QueryDiagnosticStatus::NoHit;
      if (event == nullptr) {
        break;
      }
      state.cached_result.physics_query.hit_position = event->point;
      state.cached_result.physics_query.hit_normal = event->normal;
      state.cached_result.ui_action = event->action;
      state.cached_result.ui_target_action = event->target_action;
      state.cached_result.ui_outcome = event->outcome;
      if (KX_GameObject *object = m_logicManager->ResolveObjectHandle(event->object)) {
        state.cached_result.ui_source_object = MakeObjectRef(object, object->GetName());
      }
      if (KX_GameObject *object = m_logicManager->ResolveObjectHandle(event->target)) {
        state.cached_result.ui_target_object = MakeObjectRef(object, object->GetName());
      }
      if (event->item.exists) {
        state.cached_result.ui_item = std::make_shared<LN_Value>(event->item);
      }
      if (event->target_item.exists) {
        state.cached_result.ui_target_item = std::make_shared<LN_Value>(event->target_item);
      }
      state.cached_result.ui_index = event->index;
      state.cached_result.ui_amount = event->amount;
      state.cached_result.ui_target_index = event->target_index;
      state.cached_result.ui_user = event->user;
      state.cached_result.ui_accepted = event->accepted;
      break;
    }
    case LN_QueryExpressionKind::ObjectScrollView:
    case LN_QueryExpressionKind::ObjectGridView: {
      if (m_logicManager == nullptr) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      const LN_UIService::ViewState &view = m_logicManager->GetUIService().GetViewState(
          {this, expression_index});
      state.cached_result.valid = view.valid;
      state.cached_result.ui_scrolled = view.scrolled;
      state.cached_result.ui_scroll_at_start = view.at_start;
      state.cached_result.ui_scroll_at_end = view.at_end;
      state.cached_result.ui_items_rebound = view.items_rebound;
      state.cached_result.ui_first_visible_index = view.first_visible_index;
      state.cached_result.ui_last_visible_index = view.last_visible_index;
      state.cached_result.ui_page = view.page;
      state.cached_result.ui_page_count = view.page_count;
      state.cached_result.ui_scroll_offset = view.offset;
      state.cached_result.ui_scroll_normalized = view.normalized;
      state.cached_result.physics_query.diagnostic_status =
          view.valid ? LN_QueryDiagnosticStatus::Hit : LN_QueryDiagnosticStatus::InvalidTarget;
      break;
    }
    case LN_QueryExpressionKind::MouseOver: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        target_object = m_gameObject;
      }
      else {
        const LN_Value target_value = in.Value(expression.input0, MakeNoneValue());
        target_object = ResolveObjectValue(target_value);
      }

      LN_PhysicsQueryResult hit_result;
      bool is_over = false;
      if (target_object != nullptr) {
        FlushPendingRaycastTransforms(target_object, context);
        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        const bool target_is_overlay = scene != nullptr &&
                                       scene->IsOverlayRuntimeObject(target_object);
        const LN_InteractionDomain target_domain = target_is_overlay ?
                                                       LN_InteractionDomain::Overlay :
                                                       LN_InteractionDomain::Main;

        auto cast_target = [&](KX_Camera *camera,
                               const LN_InteractionDomain domain) -> LN_PhysicsQueryResult {
          LN_PhysicsQueryResult result;
          if (camera == nullptr || camera->GetScene() != scene) {
            result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
            return result;
          }
          FlushPendingRaycastTransforms(camera, context);
          const float distance = std::max(camera->GetCameraFar(), 0.0f);
          MT_Vector3 from;
          MT_Vector3 to;
          if (!ComputeMouseRay(camera, distance, from, to)) {
            result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
            return result;
          }
          const LN_RuntimeRef camera_ref = MakeObjectRef(camera, camera->GetName());
          return RaycastMouseOverTarget(
              from, to, target_object, camera, camera_ref, domain, context.command_execution_key);
        };

        if (expression.interaction_view == LN_InteractionView::Automatic && !target_is_overlay &&
            scene != nullptr && scene->GetOverlayCamera() != nullptr)
        {
          KX_Camera *overlay_camera = scene->GetOverlayCamera();
          FlushPendingRaycastTransforms(overlay_camera, context);
          const float distance = std::max(overlay_camera->GetCameraFar(), 0.0f);
          MT_Vector3 from;
          MT_Vector3 to;
          if (ComputeMouseRay(overlay_camera, distance, from, to)) {
            const LN_RuntimeRef camera_ref = MakeObjectRef(overlay_camera,
                                                           overlay_camera->GetName());
            LN_PhysicsQueryResult overlay_blocker = RaycastPointer(from,
                                                                   to,
                                                                   overlay_camera,
                                                                   camera_ref,
                                                                   0,
                                                                   std::string(),
                                                                   false,
                                                                   LN_InteractionDomain::Overlay,
                                                                   context.command_execution_key);
            if (overlay_blocker.hit || overlay_blocker.blocked) {
              overlay_blocker.hit = false;
              overlay_blocker.blocked = true;
              overlay_blocker.object_ref = LN_RuntimeRef();
              hit_result = std::move(overlay_blocker);
            }
          }
        }

        if (!hit_result.blocked) {
          KX_Camera *camera = nullptr;
          LN_InteractionDomain domain = target_domain;
          switch (expression.interaction_view) {
            case LN_InteractionView::Automatic:
              camera = target_is_overlay ? (scene ? scene->GetOverlayCamera() : nullptr) :
                                           PointerWorldCamera(scene, target_object);
              break;
            case LN_InteractionView::Main:
              camera = PointerWorldCamera(scene, target_object);
              domain = LN_InteractionDomain::Main;
              break;
            case LN_InteractionView::Overlay:
              camera = scene ? scene->GetOverlayCamera() : nullptr;
              domain = LN_InteractionDomain::Overlay;
              break;
            case LN_InteractionView::CustomCamera:
              camera = expression.input1 != LN_INVALID_INDEX ?
                           ResolveCameraValue(in.Value(expression.input1, MakeNoneValue())) :
                           nullptr;
              break;
          }
          hit_result = cast_target(camera, domain);
        }
        is_over = hit_result.hit;
      }
      else {
        hit_result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
      }

      const size_t event_index = std::min<size_t>(size_t(context.event),
                                                  state.hover_by_event.size() - 1);
      QueryExpressionRuntimeState::HoverRuntimeState &hover = state.hover_by_event[event_index];
      if (hover.baseline_tick != context.tick_index) {
        hover.baseline_tick = context.tick_index;
        hover.baseline_initialized = hover.initialized;
        hover.baseline_previous = hover.previous;
      }
      state.cached_result.over = is_over;
      state.cached_result.entered = is_over &&
                                    (!hover.baseline_initialized || !hover.baseline_previous);
      state.cached_result.exited = !is_over && hover.baseline_initialized &&
                                   hover.baseline_previous;
      state.cached_result.physics_query = hit_result;
      hover.initialized = true;
      hover.previous = is_over;
      break;
    }
    case LN_QueryExpressionKind::ProjectileRay: {
      const LN_Value caster_value = in.Value(expression.input0, MakeNoneValue());
      KX_GameObject *caster_object = ResolveObjectValue(caster_value);

      MT_Vector3 origin = in.Vector(expression.input1, expression.vector_value);
      MT_Vector3 aim = in.Vector(expression.input2, expression.secondary_vector_value);
      const bool local_space = in.Bool(expression.bool_expr_index, expression.bool_value);
      /* As Raycast: an empty Caster in Local space is the owner (and the ray ignores it). */
      if (local_space && caster_object == nullptr) {
        caster_object = m_gameObject;
      }
      LN_RuntimeRef caster_ref;
      if (caster_object != nullptr) {
        caster_ref = MakeObjectRef(caster_object, caster_object->GetName());
      }
      if (local_space && caster_object != nullptr) {
        origin = caster_object->NodeGetWorldPosition() +
                 caster_object->NodeGetWorldOrientation() * origin;
        aim = caster_object->NodeGetWorldOrientation() * aim;
      }
      else {
        aim -= origin;
      }
      if (aim.length2() > 0.0f) {
        aim.normalize();
      }
      else {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      const float power = in.Float(expression.float_expr_index, expression.float_value);
      aim *= power;

      MT_Vector3 gravity(0.0f, 0.0f, -9.8f);
      if (KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr) {
        gravity = scene->GetGravity();
      }

      const std::string property_name = in.String(expression.string_expr_index, std::string());
      const bool xray = in.Bool(expression.secondary_bool_expr_index, false);
      const int32_t mask_value = in.Int(expression.int_expr_index, expression.int_value);
      if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidFilter;
        break;
      }
      const uint32_t collision_mask = uint32_t(mask_value);
      const bool visualize = in.Bool(expression.tertiary_bool_expr_index, false);

      const float distance = std::max(
          0.0f, in.Float(expression.secondary_float_expr_index, 100.0f));
      const float resolution = std::clamp(
          in.Float(expression.tertiary_float_expr_index, 0.9f),
          0.0f,
          0.99f);
      const float time_step = std::max(0.01f, 1.0f - resolution);
      float total_distance = 0.0f;
      float time_index = time_step;
      std::vector<MT_Vector3> points;
      points.push_back(origin);

      LN_PhysicsQueryResult hit_result;
      for (int step = 0; step < 2048 && total_distance < distance; step++) {
        const MT_Vector3 target = ProjectilePoint(aim, origin, gravity, time_index);
        const MT_Vector3 start = points.back();
        const MT_Vector3 segment = target - start;
        const float segment_distance = segment.length();
        if (segment_distance <= 1.0e-6f) {
          break;
        }

        /* Distance limits the arc length, not merely which sample starts the
         * final ray. Clip the last segment so a coarse resolution cannot hit
         * an object beyond the requested range. */
        const float remaining_distance = distance - total_distance;
        const float ray_distance = std::min(segment_distance, remaining_distance);
        const MT_Vector3 segment_end = ray_distance < segment_distance ?
                                           start + segment * (ray_distance / segment_distance) :
                                           target;
        hit_result = Raycast(start, segment_end, caster_ref, collision_mask, property_name, xray);
        total_distance += ray_distance;
        const MT_Vector3 end = hit_result.hit ? hit_result.hit_position : segment_end;
        if (visualize) {
          DrawDebugRaycast(start,
                           end,
                           hit_result.hit,
                           hit_result.hit ? &hit_result.hit_normal : nullptr,
                           ray_distance);
        }
        if (!hit_result.hit) {
          points.push_back(segment_end);
        }
        else {
          points.push_back(hit_result.hit_position);
          break;
        }
        time_index += time_step;
      }

      hit_result.parabola_points = points;
      state.cached_result.physics_query = hit_result;
      break;
    }
    case LN_QueryExpressionKind::VectorAverage: {
      const LN_Value source = in.Value(expression.input0, MakeNoneValue());
      double sum[3] = {0.0, 0.0, 0.0};
      uint64_t count = 0;

      switch (expression.average_center_source) {
        case LN_AverageCenterSource::VectorList:
          if (source.exists && source.type == LN_ValueType::List) {
            for (const LN_Value &item : source.list_value) {
              if (item.exists && item.type == LN_ValueType::Vector) {
                accumulate_finite_position(item.vector_value, sum, count);
              }
            }
          }
          break;
        case LN_AverageCenterSource::ObjectList:
          if (source.exists && source.type == LN_ValueType::List) {
            for (const LN_Value &item : source.list_value) {
              if (KX_GameObject *game_object = ResolveObjectValue(item)) {
                accumulate_finite_position(game_object->NodeGetWorldPosition(), sum, count);
              }
            }
          }
          break;
        case LN_AverageCenterSource::Collection: {
          blender::Collection *collection = source.exists ?
                                                ResolveCollectionByValue(this, source) :
                                                nullptr;
          KX_Scene *scene = m_gameObject != nullptr ? m_gameObject->GetScene() : nullptr;
          if (collection != nullptr && scene != nullptr) {
            for (blender::CollectionObject &collection_object : collection->gobject) {
              if (collection_object.ob == nullptr) {
                continue;
              }
              if (KX_GameObject *game_object = scene->GetGameObjectFromObject(
                      collection_object.ob))
              {
                accumulate_finite_position(game_object->NodeGetWorldPosition(), sum, count);
              }
            }
          }
          break;
        }
        default:
          break;
      }
      if (count == 0) {
        break;
      }

      const double inverse_count = 1.0 / double(count);
      const MT_Vector3 average(float(sum[0] * inverse_count),
                               float(sum[1] * inverse_count),
                               float(sum[2] * inverse_count));
      if (!vector_is_finite(average)) {
        break;
      }
      state.cached_result.vector_average = average;
      state.cached_result.vector_count = int32_t(
          std::min(count, uint64_t(std::numeric_limits<int32_t>::max())));
      state.cached_result.valid = true;
      break;
    }
    case LN_QueryExpressionKind::DistanceBetween:
      DistanceBetweenKind(in, expression, state);
      break;
    case LN_QueryExpressionKind::Count:
      break;
  }
}


template<class In>
void LN_RuntimeTree::DistanceBetweenKind(In &in,
                                         const LN_QueryExpression &expression,
                                         QueryExpressionRuntimeState &state)
{
  const LN_TickContext &context = in.context;
  auto resolve_endpoint = [&](const LN_DistanceEndpointMode mode,
                              const uint32_t input,
                              const bool owner_when_empty,
                              MT_Vector3 &r_position) {
    if (mode == LN_DistanceEndpointMode::Position) {
      if (input == LN_INVALID_INDEX) {
        return false;
      }
      r_position = in.Vector(input, MT_Vector3(0.0f, 0.0f, 0.0f));
      return vector_is_finite(r_position);
    }
    if (mode != LN_DistanceEndpointMode::Object) {
      return false;
    }

    KX_GameObject *object = nullptr;
    if (input != LN_INVALID_INDEX) {
      object = ResolveObjectValue(in.Value(input, MakeNoneValue()));
    }
    else if (owner_when_empty) {
      object = m_gameObject;
    }
    if (object == nullptr) {
      return false;
    }
    FlushPendingRaycastTransforms(object, context);
    r_position = object->NodeGetWorldPosition();
    return vector_is_finite(r_position);
  };

  MT_Vector3 a(0.0f, 0.0f, 0.0f);
  MT_Vector3 b(0.0f, 0.0f, 0.0f);
  if (!resolve_endpoint(expression.distance_endpoint_a, expression.input0, true, a) ||
      !resolve_endpoint(expression.distance_endpoint_b, expression.input1, false, b))
  {
    return;
  }

  const double dx = double(a.x()) - double(b.x());
  const double dy = double(a.y()) - double(b.y());
  const double dz = double(a.z()) - double(b.z());
  const double squared_distance = dx * dx + dy * dy + dz * dz;
  if (!std::isfinite(squared_distance) || squared_distance < 0.0) {
    return;
  }
  const double max_float = double(std::numeric_limits<float>::max());
  state.cached_result.squared_distance = float(std::min(squared_distance, max_float));
  state.cached_result.distance = float(std::min(std::sqrt(squared_distance), max_float));
  state.cached_result.valid = true;
}

template<class In>
const LN_QueryResult &LN_RuntimeTree::QueryEntry(In &in, const uint32_t expression_index)
{
  const LN_TickContext &context = in.context;
  NoteExpressionRead('Q', expression_index);
    static const LN_QueryResult fallback;
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }

  const std::vector<LN_QueryExpression> &expressions = m_program->GetQueryExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }

  const LN_QueryExpression &expression = expressions[expression_index];
  if (m_activeProfileCounters != nullptr) { /* Profiling only: a call at every evaluation. */
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        expression.kind);
    ProfileExpressionEvaluation(semantics.fallback_requirements);
    RecordMissingSnapshotChannelsForExpression(semantics);
  }
  const uint32_t state_index = expression.runtime_state_index != LN_INVALID_INDEX ?
                                   expression.runtime_state_index :
                                   expression_index;
  QueryExpressionRuntimeState &state = QueryExpressionState(state_index);
  const bool command_ordered_cache = QueryExpressionNeedsCommandOrderedEvaluation(
                                         expression.kind) &&
                                     context.command_buffer != nullptr &&
                                     context.command_execution_key != UINT64_MAX;
  /* A Get Distance between fixed ends (the owner, a named object, a constant position) gives
   * one value a pass: the distance at its first use, which sees the step's writes recorded so
   * far. Every other command-ordered query is evaluated again for each instruction that reads
   * it. (Before 2026-10-31 Get Distance was too: a Branch on it could take both routes in one
   * step, its True route moving the object across the threshold before the False route read.) */
  bool once_a_pass = command_ordered_cache && context.execution_scope_serial != 0;
  if (once_a_pass) {
    /* Worked out at the lowering for the lean VM (the same function, the same program). */
    const LN_LeanProgram *lean = nullptr;
    if constexpr (std::is_same_v<In, LN_LeanReader>) {
      lean = in.lean;
    }
    once_a_pass = lean != nullptr && expression_index < lean->query_fixed_distance.size() ?
                      lean->query_fixed_distance[expression_index] != 0 :
                      LN_DistanceQueryEndsAreFixed(*m_program, expression);
  }
  const uint64_t command_execution_key = !command_ordered_cache ? UINT64_MAX :
                                         once_a_pass ?
                                                    (context.execution_scope_serial |
                                                     LN_QUERY_PASS_KEY_BIT) :
                                                    context.command_execution_key;
  if (state.cached_tick == context.tick_index &&
      state.cached_command_ordered == command_ordered_cache &&
      (!command_ordered_cache || state.cached_command_execution_key == command_execution_key))
  {
    return state.cached_result;
  }

  state.cached_tick = context.tick_index;
  state.cached_command_execution_key = command_execution_key;
  state.cached_command_ordered = command_ordered_cache;
  if (state.cached_result_distance_only && expression.kind == LN_QueryExpressionKind::DistanceBetween)
  {
    /* The last result of this query set its distance fields and nothing else. */
    state.cached_result.distance = 0.0f;
    state.cached_result.squared_distance = 0.0f;
    state.cached_result.valid = false;
  }
  else {
    state.cached_result = LN_QueryResult();
  }
  state.cached_result_distance_only = false;

  if (expression.condition_bool_expr_index != LN_INVALID_INDEX &&
      !in.Bool(expression.condition_bool_expr_index, false))
  {
    state.cached_result.physics_query.diagnostic_status = LN_QueryDiagnosticStatus::Disabled;
    return state.cached_result;
  }

  if (expression.kind == LN_QueryExpressionKind::DistanceBetween) {
    DistanceBetweenKind(in, expression, state);
  }
  else {
    QueryKind(in, expression_index, expression, state);
  }
  state.cached_result_distance_only = expression.kind == LN_QueryExpressionKind::DistanceBetween;

  return state.cached_result;
}

template<class In>
bool LN_RuntimeTree::LeanBool(In &in, const uint32_t expression_index, const bool fallback)
{
  NoteExpressionRead('B', expression_index);
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }
  const auto &expressions = m_program->GetBoolExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }
  if (m_activeProfileCounters != nullptr) { /* Profiling only: a call at every evaluation. */
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        expressions[expression_index].kind);
    ProfileExpressionEvaluation(semantics.fallback_requirements);
    RecordMissingSnapshotChannelsForExpression(semantics);
  }
  const LN_BoolExpression &expression = expressions[expression_index];
  if constexpr (std::is_same_v<In, LN_LeanReader>) {
    /* The lean memo (plan phase A33): the old interpreter's per-tick registers. */
    if (in.lean != nullptr && in.lean->memo_count != 0 &&
        expression_index < in.lean->memo_bool.size() &&
        in.lean->memo_bool[expression_index] != 0 && g_memo_enabled &&
        lean_memo_enabled() && in.context.tick_index != UINT64_MAX)
    {
      EnsureRegisterExpressionState(in.context);
      const std::vector<uint32_t> &registers =
          m_program->GetRegisterExpressionProgram().bool_expression_registers;
      const uint32_t memo_register = expression_index < registers.size() ?
                                         registers[expression_index] :
                                         LN_INVALID_INDEX;
      if (memo_register < m_registerExpressionState.bool_values.size()) {
        if (m_registerExpressionState.bool_valid[memo_register] ==
            m_registerExpressionState.active_generation)
        {
          return m_registerExpressionState.bool_values[memo_register];
        }
        /* The kind function (LeanBool's fast kinds are the same code as its cases). */
        const bool value = BoolKind(in, expression_index, expression, fallback);
        m_registerExpressionState.bool_values[memo_register] = value;
        m_registerExpressionState.bool_valid[memo_register] =
            m_registerExpressionState.active_generation;
        return value;
      }
    }
  }
  /* The kinds the guards and conditions of a brain are made of, without the big switch of
   * BoolKind (its entry and exit cost more than they do): the same code as its cases. */
  switch (expression.kind) {
    case LN_BoolExpressionKind::Constant:
      return expression.bool_value;
    case LN_BoolExpressionKind::InstructionExecuted:
      return InstructionExecutedNow(in.context, expression.input0);
    default:
      break;
  }
  return BoolKind(in, expression_index, expression, fallback);
}

template<class In>
int32_t LN_RuntimeTree::LeanInt(In &in, const uint32_t expression_index, const int32_t fallback)
{
  NoteExpressionRead('I', expression_index);
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }
  const auto &expressions = m_program->GetIntExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }
  if (m_activeProfileCounters != nullptr) { /* Profiling only: a call at every evaluation. */
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        expressions[expression_index].kind);
    ProfileExpressionEvaluation(semantics.fallback_requirements);
    RecordMissingSnapshotChannelsForExpression(semantics);
  }
  if constexpr (std::is_same_v<In, LN_LeanReader>) {
    /* The lean memo (plan phase A33): the old interpreter's per-tick registers. */
    if (in.lean != nullptr && in.lean->memo_count != 0 &&
        expression_index < in.lean->memo_int.size() &&
        in.lean->memo_int[expression_index] != 0 && g_memo_enabled &&
        lean_memo_enabled() && in.context.tick_index != UINT64_MAX)
    {
      EnsureRegisterExpressionState(in.context);
      const std::vector<uint32_t> &registers =
          m_program->GetRegisterExpressionProgram().int_expression_registers;
      const uint32_t memo_register = expression_index < registers.size() ?
                                         registers[expression_index] :
                                         LN_INVALID_INDEX;
      if (memo_register < m_registerExpressionState.int_values.size()) {
        if (m_registerExpressionState.int_valid[memo_register] ==
            m_registerExpressionState.active_generation)
        {
          return m_registerExpressionState.int_values[memo_register];
        }
        const int32_t value = IntKind(in, expression_index, expressions[expression_index], fallback);
        m_registerExpressionState.int_values[memo_register] = value;
        m_registerExpressionState.int_valid[memo_register] =
            m_registerExpressionState.active_generation;
        return value;
      }
    }
  }
  return IntKind(in, expression_index, expressions[expression_index], fallback);
}

template<class In>
float LN_RuntimeTree::LeanFloat(In &in, const uint32_t expression_index, const float fallback)
{
  NoteExpressionRead('F', expression_index);
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }
  const auto &expressions = m_program->GetFloatExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }
  if (m_activeProfileCounters != nullptr) { /* Profiling only: a call at every evaluation. */
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        expressions[expression_index].kind);
    ProfileExpressionEvaluation(semantics.fallback_requirements);
    RecordMissingSnapshotChannelsForExpression(semantics);
  }
  const LN_FloatExpression &expression = expressions[expression_index];
  if constexpr (std::is_same_v<In, LN_LeanReader>) {
    /* The lean memo (plan phase A33): the old interpreter's per-tick registers. */
    if (in.lean != nullptr && in.lean->memo_count != 0 &&
        expression_index < in.lean->memo_float.size() &&
        in.lean->memo_float[expression_index] != 0 && g_memo_enabled &&
        lean_memo_enabled() && in.context.tick_index != UINT64_MAX)
    {
      EnsureRegisterExpressionState(in.context);
      const std::vector<uint32_t> &registers =
          m_program->GetRegisterExpressionProgram().float_expression_registers;
      const uint32_t memo_register = expression_index < registers.size() ?
                                         registers[expression_index] :
                                         LN_INVALID_INDEX;
      if (memo_register < m_registerExpressionState.float_values.size()) {
        if (m_registerExpressionState.float_valid[memo_register] ==
            m_registerExpressionState.active_generation)
        {
          return m_registerExpressionState.float_values[memo_register];
        }
        /* The kind function (LeanFloat's fast kinds are the same code as its cases). */
        const float value = FloatKind(in, expression_index, expression, fallback);
        m_registerExpressionState.float_values[memo_register] = value;
        m_registerExpressionState.float_valid[memo_register] =
            m_registerExpressionState.active_generation;
        return value;
      }
    }
  }
  /* The kinds a brain's values are made of, without the big switch of FloatKind: the same code as
   * its cases (Add reads A, then B). */
  switch (expression.kind) {
    case LN_FloatExpressionKind::Constant:
      return expression.float_value;
    case LN_FloatExpressionKind::SnapshotGameProperty: {
      LN_Snapshot::GamePropertyView view;
      if (m_snapshot.ReadGameProperty(expression.property_ref_index, view)) {
        return view.float_value;
      }
      return fallback;
    }
    case LN_FloatExpressionKind::QueryDistance:
      return in.Query(expression.input0).distance;
    case LN_FloatExpressionKind::Add: {
      const float a = in.Float(expression.input0, 0.0f);
      const float b = in.Float(expression.input1, 0.0f);
      return a + b;
    }
    default:
      break;
  }
  return FloatKind(in, expression_index, expression, fallback);
}

template<class In>
std::string LN_RuntimeTree::LeanString(In &in, const uint32_t expression_index, const std::string &fallback)
{
  NoteExpressionRead('S', expression_index);
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }
  const auto &expressions = m_program->GetStringExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }
  if (m_activeProfileCounters != nullptr) { /* Profiling only: a call at every evaluation. */
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        expressions[expression_index].kind);
    ProfileExpressionEvaluation(semantics.fallback_requirements);
    RecordMissingSnapshotChannelsForExpression(semantics);
  }
  return StringKind(in, expression_index, expressions[expression_index], fallback);
}

template<class In>
MT_Vector3 LN_RuntimeTree::LeanVector(In &in, const uint32_t expression_index, const MT_Vector3 &fallback)
{
  NoteExpressionRead('V', expression_index);
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }
  const auto &expressions = m_program->GetVectorExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }
  if (m_activeProfileCounters != nullptr) { /* Profiling only: a call at every evaluation. */
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        expressions[expression_index].kind);
    ProfileExpressionEvaluation(semantics.fallback_requirements);
    RecordMissingSnapshotChannelsForExpression(semantics);
  }
  if constexpr (std::is_same_v<In, LN_LeanReader>) {
    /* The lean memo (plan phase A33): the old interpreter's per-tick registers. */
    if (in.lean != nullptr && in.lean->memo_count != 0 &&
        expression_index < in.lean->memo_vector.size() &&
        in.lean->memo_vector[expression_index] != 0 && g_memo_enabled &&
        lean_memo_enabled() && in.context.tick_index != UINT64_MAX)
    {
      EnsureRegisterExpressionState(in.context);
      const std::vector<uint32_t> &registers =
          m_program->GetRegisterExpressionProgram().vector_expression_registers;
      const uint32_t memo_register = expression_index < registers.size() ?
                                         registers[expression_index] :
                                         LN_INVALID_INDEX;
      if (memo_register < m_registerExpressionState.vector_values.size()) {
        if (m_registerExpressionState.vector_valid[memo_register] ==
            m_registerExpressionState.active_generation)
        {
          return m_registerExpressionState.vector_values[memo_register];
        }
        const MT_Vector3 value = VectorKind(in, expression_index, expressions[expression_index], fallback);
        m_registerExpressionState.vector_values[memo_register] = value;
        m_registerExpressionState.vector_valid[memo_register] =
            m_registerExpressionState.active_generation;
        return value;
      }
    }
  }
  return VectorKind(in, expression_index, expressions[expression_index], fallback);
}

template<class In>
LN_Rotation LN_RuntimeTree::LeanRotation(In &in, const uint32_t expression_index, const LN_Rotation &fallback)
{
  NoteExpressionRead('R', expression_index);
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }
  const auto &expressions = m_program->GetVectorExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }
  if (m_activeProfileCounters != nullptr) { /* Profiling only: a call at every evaluation. */
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        expressions[expression_index].kind);
    ProfileExpressionEvaluation(semantics.fallback_requirements);
    RecordMissingSnapshotChannelsForExpression(semantics);
  }
  bool resolved = false;
  return RotationKind(in, expression_index, expressions[expression_index], fallback, resolved);
}

template<class In>
MT_Vector4 LN_RuntimeTree::LeanColor(In &in, const uint32_t expression_index, const MT_Vector4 &fallback)
{
  NoteExpressionRead('C', expression_index);
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }
  const auto &expressions = m_program->GetColorExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }
  if (m_activeProfileCounters != nullptr) { /* Profiling only: a call at every evaluation. */
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        expressions[expression_index].kind);
    ProfileExpressionEvaluation(semantics.fallback_requirements);
    RecordMissingSnapshotChannelsForExpression(semantics);
  }
  return ColorKind(in, expression_index, expressions[expression_index], fallback);
}

template<class In>
LN_Value LN_RuntimeTree::LeanValue(In &in, const uint32_t expression_index, const LN_Value &fallback)
{
  NoteExpressionRead('A', expression_index);
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }
  const auto &expressions = m_program->GetValueExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }
  if (m_activeProfileCounters != nullptr) { /* Profiling only: a call at every evaluation. */
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        expressions[expression_index].kind);
    ProfileExpressionEvaluation(semantics.fallback_requirements);
    RecordMissingSnapshotChannelsForExpression(semantics);
  }
  return ValueKind(in, expression_index, expressions[expression_index], fallback);
}

