/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cmath>
#include <string>

#include "BKE_game_input_map.hh"
#include "BLI_string.h"
#include "DNA_scene_types.h"
#include "wm_event_types.hh"

#include "DEV_InputDevice.h"
#include "DEV_Joystick.h"
#include "LN_InputMap.h"
#include "LN_Snapshot.h"

namespace {

/** Every test leaves the engine's default map behind for the other suites. */
class LN_InputMapTest : public testing::Test {
 protected:
  void TearDown() override
  {
    LN_InputMap::Get().Load(nullptr);
  }

  /** The defaults plus Jump (Space, gamepad A) and Move (W A S D, left stick). */
  static void LoadGameMap(const int input_flag = 0, const char *jump_context = "")
  {
    blender::GameData game;
    blender::BKE_game_input_map_ensure_builtins(&game);
    game.input_flag = input_flag;
    blender::GameInputAction *jump = blender::BKE_game_input_action_add(
        &game, "Jump", GAME_INPUT_ACTION_BUTTON);
    blender::STRNCPY(jump->context, jump_context);
    blender::BKE_game_input_binding_add(jump, GAME_INPUT_KEYBOARD)->code = blender::EVT_SPACEKEY;
    blender::BKE_game_input_binding_add(jump, GAME_INPUT_GAMEPAD);
    blender::GameInputAction *move = blender::BKE_game_input_action_add(
        &game, "Move", GAME_INPUT_ACTION_VECTOR);
    const short keys[4] = {
        blender::EVT_WKEY, blender::EVT_SKEY, blender::EVT_AKEY, blender::EVT_DKEY};
    for (short direction = 0; direction < 4; ++direction) {
      blender::GameInputBinding *binding = blender::BKE_game_input_binding_add(
          move, GAME_INPUT_KEYBOARD);
      binding->code = keys[direction];
      binding->direction = direction;
    }
    blender::BKE_game_input_binding_add(move, GAME_INPUT_GAMEPAD);
    blender::GameInputAction *look = blender::BKE_game_input_action_add(
        &game, "Look Up", GAME_INPUT_ACTION_BUTTON);
    blender::BKE_game_input_binding_add(look, GAME_INPUT_GAMEPAD)->code =
        GAME_INPUT_GAMEPAD_LEFT_STICK_UP;
    LN_InputMap::Get().Load(&game);
    blender::BKE_game_input_map_free(&game);
  }
};

/** Every player in `context`. */
LN_InputContexts AllInContext(const std::string_view context)
{
  LN_InputContexts contexts;
  contexts.fill(context);
  return contexts;
}

struct Input {
  DEV_InputDevice device;
  LN_InputSnapshot snapshot;
  LN_InputActionTracker tracker;
  uint64_t tick = 0;

  /** A new frame: `events` happen in it, then the tracker samples it. */
  template<typename Events> void Frame(Events events, const std::string &context = "")
  {
    device.ClearInputs();
    events();
    snapshot.Capture(&device, nullptr);
    tracker.Update(snapshot, AllInContext(context), tick++);
  }
  void Frame()
  {
    Frame([] {});
  }
  void Key(const SCA_IInputDevice::SCA_EnumInputs key, const bool down)
  {
    device.ConvertEvent(key, down ? 1 : 0, 0);
  }
  void Gamepad(const int32_t user, LN_GamepadSnapshot gamepad)
  {
    gamepad.connected = true;
    snapshot.SetGamepadForTests(user, gamepad);
    tracker.Update(snapshot, LN_InputContexts(), tick++);
  }
  const LN_InputActionState &State(const char *action, const int32_t user = 0) const
  {
    const LN_InputActionState *state = tracker.Find(LN_InputMap::Get().Find(action), user);
    BLI_assert(state != nullptr);
    return *state;
  }
};

LN_GamepadSnapshot Stick(const float x, const float up)
{
  LN_GamepadSnapshot gamepad;
  gamepad.axes[0] = int32_t(x * 32767.0f);
  gamepad.axes[1] = int32_t(-up * 32767.0f);
  return gamepad;
}

TEST_F(LN_InputMapTest, DefaultsHaveEveryUIControl)
{
  const LN_InputMap &map = LN_InputMap::Get();
  ASSERT_EQ(map.Size(), size_t(LN_UI_ACTION_COUNT));
  EXPECT_EQ(map.Find("Accept"), map.FindBuiltin(LN_UIAction::Accept));
  EXPECT_EQ(map.Find("Back"), map.FindBuiltin(LN_UIAction::Cancel));
  EXPECT_EQ(map.Find("Scroll Down"), map.FindBuiltin(LN_UIAction::ScrollDown));
  EXPECT_EQ(map.Find("Jump"), -1);
  EXPECT_EQ(map.ActionText(map.FindBuiltin(LN_UIAction::Accept),
                           LN_InputDeviceClass::KeyboardMouse,
                           0),
            "Return");
  EXPECT_EQ(map.ActionText(map.FindBuiltin(LN_UIAction::Previous),
                           LN_InputDeviceClass::KeyboardMouse,
                           0),
            "Shift+Tab");
  EXPECT_EQ(
      map.ActionText(map.FindBuiltin(LN_UIAction::Accept), LN_InputDeviceClass::Gamepad, 0), "A");
  EXPECT_FLOAT_EQ(map.RepeatDelay(), 0.4f);
  EXPECT_FLOAT_EQ(map.StickThreshold(), 0.5f);
}

/* Esc ends the game unless a game action (a pause menu) takes it; Back on Esc does not count. */
TEST_F(LN_InputMapTest, AGameActionOnAKeyTakesItFromTheExitKey)
{
  EXPECT_FALSE(LN_InputMap::Get().GameActionUsesKey(SCA_IInputDevice::ESCKEY));
  LoadGameMap();
  const LN_InputMap &map = LN_InputMap::Get();
  EXPECT_FALSE(map.GameActionUsesKey(SCA_IInputDevice::ESCKEY));
  EXPECT_TRUE(map.GameActionUsesKey(SCA_IInputDevice::SPACEKEY));
  EXPECT_TRUE(map.GameActionUsesKey(SCA_IInputDevice::WKEY));

  blender::GameData game;
  blender::BKE_game_input_map_ensure_builtins(&game);
  blender::GameInputAction *pause = blender::BKE_game_input_action_add(
      &game, "Pause Menu", GAME_INPUT_ACTION_BUTTON);
  blender::BKE_game_input_binding_add(pause, GAME_INPUT_KEYBOARD)->code = blender::EVT_ESCKEY;
  blender::GameInputAction *quit = blender::BKE_game_input_action_add(
      &game, "Quit", GAME_INPUT_ACTION_BUTTON);
  blender::GameInputBinding *shift_q = blender::BKE_game_input_binding_add(quit,
                                                                           GAME_INPUT_KEYBOARD);
  shift_q->code = blender::EVT_QKEY;
  shift_q->modifiers = GAME_INPUT_MOD_SHIFT;
  LN_InputMap::Get().Load(&game);
  blender::BKE_game_input_map_free(&game);
  EXPECT_TRUE(map.GameActionUsesKey(SCA_IInputDevice::ESCKEY));
  /* Q alone still reaches the game's other uses. */
  EXPECT_FALSE(map.GameActionUsesKey(SCA_IInputDevice::QKEY));
}

TEST_F(LN_InputMapTest, ActionPressHoldAndRelease)
{
  LoadGameMap();
  Input input;
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, true); });
  EXPECT_TRUE(input.State("Jump").pressed);
  EXPECT_TRUE(input.State("Jump").active);
  EXPECT_FLOAT_EQ(input.State("Jump").value, 1.0f);
  input.Frame();
  EXPECT_FALSE(input.State("Jump").pressed);
  EXPECT_TRUE(input.State("Jump").active);
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, false); });
  EXPECT_TRUE(input.State("Jump").released);
  EXPECT_FALSE(input.State("Jump").active);
  input.Frame();
  EXPECT_FALSE(input.State("Jump").released);
}

TEST_F(LN_InputMapTest, ATapWithinOneStepPressesAndReleases)
{
  LoadGameMap();
  Input input;
  input.Frame([&] {
    input.Key(SCA_IInputDevice::SPACEKEY, true);
    input.Key(SCA_IInputDevice::SPACEKEY, false);
  });
  EXPECT_TRUE(input.State("Jump").pressed);
  EXPECT_TRUE(input.State("Jump").released);
  EXPECT_FALSE(input.State("Jump").active);
}

TEST_F(LN_InputMapTest, ASecondBindingOfAHeldActionDoesNotPressItAgain)
{
  LoadGameMap();
  Input input;
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, true); });
  LN_GamepadSnapshot gamepad;
  gamepad.buttons[GAME_INPUT_GAMEPAD_A] = 1;
  gamepad.pressed[GAME_INPUT_GAMEPAD_A] = 1;
  input.Gamepad(0, gamepad);
  EXPECT_FALSE(input.State("Jump").pressed);
  EXPECT_TRUE(input.State("Jump").active);
}

TEST_F(LN_InputMapTest, TheBindingWithMoreHeldModifiersWins)
{
  Input input;
  const LN_InputMap &map = LN_InputMap::Get();
  const auto state = [&](const LN_UIAction action) {
    return *input.tracker.Find(map.FindBuiltin(action), 0);
  };
  input.Frame([&] { input.Key(SCA_IInputDevice::TABKEY, true); });
  EXPECT_TRUE(state(LN_UIAction::Next).pressed);
  EXPECT_FALSE(state(LN_UIAction::Previous).pressed);
  /* Tab let go without Shift does not release Previous, which it never pressed. */
  input.Frame([&] { input.Key(SCA_IInputDevice::TABKEY, false); });
  EXPECT_TRUE(state(LN_UIAction::Next).released);
  EXPECT_FALSE(state(LN_UIAction::Previous).released);

  input.Frame([&] {
    input.Key(SCA_IInputDevice::LEFTSHIFTKEY, true);
    input.Key(SCA_IInputDevice::TABKEY, true);
  });
  EXPECT_TRUE(state(LN_UIAction::Previous).pressed);
  EXPECT_FALSE(state(LN_UIAction::Next).pressed);
  EXPECT_FALSE(state(LN_UIAction::Next).active);
  /* Letting go of Shift first ends Previous although Tab is still held. */
  input.Frame([&] { input.Key(SCA_IInputDevice::LEFTSHIFTKEY, false); });
  EXPECT_TRUE(state(LN_UIAction::Previous).released);
  EXPECT_FALSE(state(LN_UIAction::Next).pressed);
}

TEST_F(LN_InputMapTest, VectorActionsCombineKeysAndTheStick)
{
  LoadGameMap();
  Input input;
  input.Frame([&] {
    input.Key(SCA_IInputDevice::WKEY, true);
    input.Key(SCA_IInputDevice::DKEY, true);
  });
  const LN_InputActionState &move = input.State("Move");
  EXPECT_TRUE(move.active);
  EXPECT_TRUE(move.pressed);
  EXPECT_NEAR(move.vector.x(), std::sqrt(0.5f), 1.0e-5f);
  EXPECT_NEAR(move.vector.y(), std::sqrt(0.5f), 1.0e-5f);
  EXPECT_NEAR(move.value, 1.0f, 1.0e-5f);

  input.Frame([&] {
    input.Key(SCA_IInputDevice::WKEY, false);
    input.Key(SCA_IInputDevice::DKEY, false);
  });
  EXPECT_TRUE(input.State("Move").released);
  /* The stick: a drifting one gives nothing, a pushed one its direction past the deadzone. */
  input.Gamepad(0, Stick(0.05f, 0.0f));
  EXPECT_FALSE(input.State("Move").active);
  EXPECT_FLOAT_EQ(input.State("Move").vector.length(), 0.0f);
  input.Gamepad(0, Stick(-0.6f, 0.0f));
  EXPECT_TRUE(input.State("Move").active);
  EXPECT_NEAR(input.State("Move").vector.x(), -0.5f / 0.9f, 1.0e-4f);
  EXPECT_NEAR(input.State("Move").vector.y(), 0.0f, 1.0e-4f);
  /* Below the threshold the stick still moves, it is just not "active". */
  input.Gamepad(0, Stick(0.3f, 0.0f));
  EXPECT_FALSE(input.State("Move").active);
  EXPECT_GT(input.State("Move").vector.x(), 0.0f);
}

TEST_F(LN_InputMapTest, StickDirectionsHoldUntilTheStickFallsBack)
{
  LoadGameMap();
  Input input;
  input.Frame();
  input.Gamepad(0, Stick(0.0f, 0.6f));
  EXPECT_TRUE(input.State("Look Up").pressed);
  input.Gamepad(0, Stick(0.0f, 0.4f));
  EXPECT_TRUE(input.State("Look Up").active);
  EXPECT_FALSE(input.State("Look Up").pressed);
  EXPECT_FALSE(input.State("Look Up").released);
  input.Gamepad(0, Stick(0.0f, 0.3f));
  EXPECT_FALSE(input.State("Look Up").active);
  EXPECT_TRUE(input.State("Look Up").released);
  /* Down is not up. */
  input.Gamepad(0, Stick(0.0f, -0.9f));
  EXPECT_FALSE(input.State("Look Up").active);
}

TEST_F(LN_InputMapTest, PlayersGamepadsFollowTheSettings)
{
  LN_InputMap &map = LN_InputMap::Get();
  LoadGameMap();
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    EXPECT_EQ(map.GamepadOfUser(user), user);
    EXPECT_EQ(map.UserOfGamepad(user), user);
  }
  /* Player 0 has the keyboard and mouse alone; the last gamepad has no player. */
  LoadGameMap(GAME_INPUT_GAMEPADS_FROM_PLAYER_1);
  EXPECT_EQ(map.GamepadOfUser(0), -1);
  EXPECT_EQ(map.GamepadOfUser(1), 0);
  EXPECT_EQ(map.GamepadOfUser(7), 6);
  EXPECT_EQ(map.UserOfGamepad(7), -1);
  /* Press to Join: no player has a gamepad until it presses a button. */
  LoadGameMap(GAME_INPUT_PRESS_TO_JOIN);
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    EXPECT_EQ(map.GamepadOfUser(user), -1);
  }
  EXPECT_EQ(map.GamepadOfUser(-1), -1);
  EXPECT_EQ(map.GamepadOfUser(LN_MAX_GAMEPADS), -1);
  EXPECT_EQ(map.UserOfGamepad(-1), -1);
}

TEST_F(LN_InputMapTest, SetPlayerGamepadMovesAGamepadBetweenPlayers)
{
  LN_InputMap &map = LN_InputMap::Get();
  LoadGameMap();
  const uint64_t device_revision = map.DeviceRevision();
  /* Player 1 loses gamepad 1 to player 3, whose gamepad 3 then has no player. */
  map.SetUserGamepad(3, 1);
  EXPECT_EQ(map.GamepadOfUser(3), 1);
  EXPECT_EQ(map.GamepadOfUser(1), -1);
  EXPECT_EQ(map.UserOfGamepad(1), 3);
  EXPECT_EQ(map.UserOfGamepad(3), -1);
  EXPECT_NE(map.DeviceRevision(), device_revision);
  /* -1: the player has no gamepad any more. */
  map.SetUserGamepad(3, -1);
  EXPECT_EQ(map.GamepadOfUser(3), -1);
  EXPECT_FALSE(map.IsUserJoined(3));
  /* Out of range: nothing changes. */
  map.SetUserGamepad(LN_MAX_GAMEPADS, 2);
  map.SetUserGamepad(2, LN_MAX_GAMEPADS);
  map.SetUserGamepad(2, -2);
  EXPECT_EQ(map.GamepadOfUser(2), 2);
  /* No join was made without a gamepad pressing a button. */
  uint64_t serial = map.JoinSerial();
  int32_t user = -1;
  int32_t gamepad = -1;
  EXPECT_FALSE(map.NextJoin(serial, user, gamepad));
}

TEST_F(LN_InputMapTest, APlayerReadsTheGamepadTheyHave)
{
  /* Gamepads Start at Player 1: gamepad 0 is player 1's, not player 0's. */
  LoadGameMap(GAME_INPUT_GAMEPADS_FROM_PLAYER_1);
  Input input;
  LN_GamepadSnapshot gamepad;
  gamepad.buttons[GAME_INPUT_GAMEPAD_A] = 1;
  gamepad.pressed[GAME_INPUT_GAMEPAD_A] = 1;
  input.Gamepad(0, gamepad);
  EXPECT_TRUE(input.State("Jump", 1).pressed);
  EXPECT_FALSE(input.State("Jump", 0).active);
  /* Given another gamepad, the player reads that one. */
  LN_InputMap::Get().SetUserGamepad(1, 2);
  input.Gamepad(2, gamepad);
  EXPECT_TRUE(input.State("Jump", 1).active);
}

/* Gamepads as UpdatePlayers reads them: all plugged in, no button pressed yet. */
struct Gamepads {
  std::array<LN_InputMap::GamepadButtons, LN_MAX_GAMEPADS> pads;
  Gamepads()
  {
    for (size_t index = 0; index < pads.size(); ++index) {
      pads[index].connected = true;
      pads[index].serial = index + 1;
    }
  }
  void Press(const int gamepad, const int button)
  {
    pads[size_t(gamepad)].presses[size_t(button)]++;
    pads[size_t(gamepad)].held |= 1u << uint32_t(button);
  }
  void Release(const int gamepad, const int button)
  {
    pads[size_t(gamepad)].held &= ~(1u << uint32_t(button));
  }
};

TEST_F(LN_InputMapTest, PressToJoinGivesTheFirstFreePlayerAndHidesThePress)
{
  LoadGameMap(GAME_INPUT_PRESS_TO_JOIN | GAME_INPUT_GAMEPADS_FROM_PLAYER_1);
  LN_InputMap &map = LN_InputMap::Get();
  Gamepads gamepads;
  map.UpdatePlayers(gamepads.pads);
  uint64_t serial = map.JoinSerial();
  int32_t user = -1;
  int32_t gamepad = -1;
  EXPECT_FALSE(map.NextJoin(serial, user, gamepad));

  /* Gamepad 2 presses A: it is player 1's (player 0 keeps the keyboard), and that press is hidden
   * from the game while held, in the frame it is let go, and shown again after. */
  gamepads.Press(2, GAME_INPUT_GAMEPAD_A);
  map.UpdatePlayers(gamepads.pads);
  EXPECT_EQ(map.GamepadOfUser(1), 2);
  EXPECT_TRUE(map.IsUserJoined(1));
  ASSERT_TRUE(map.NextJoin(serial, user, gamepad));
  EXPECT_EQ(user, 1);
  EXPECT_EQ(gamepad, 2);
  EXPECT_TRUE(map.HidesJoinPress(2, GAME_INPUT_GAMEPAD_A));
  map.UpdatePlayers(gamepads.pads);
  EXPECT_TRUE(map.HidesJoinPress(2, GAME_INPUT_GAMEPAD_A));
  gamepads.Release(2, GAME_INPUT_GAMEPAD_A);
  map.UpdatePlayers(gamepads.pads);
  EXPECT_TRUE(map.HidesJoinPress(2, GAME_INPUT_GAMEPAD_A));
  map.UpdatePlayers(gamepads.pads);
  EXPECT_FALSE(map.HidesJoinPress(2, GAME_INPUT_GAMEPAD_A));

  /* Its next presses are the game's: no new join, nothing hidden. */
  gamepads.Press(2, GAME_INPUT_GAMEPAD_A);
  map.UpdatePlayers(gamepads.pads);
  EXPECT_FALSE(map.NextJoin(serial, user, gamepad));
  EXPECT_FALSE(map.HidesJoinPress(2, GAME_INPUT_GAMEPAD_A));

  /* A press and release within one frame joins too, and stays hidden that frame. */
  gamepads.Press(0, GAME_INPUT_GAMEPAD_B);
  gamepads.Release(0, GAME_INPUT_GAMEPAD_B);
  map.UpdatePlayers(gamepads.pads);
  ASSERT_TRUE(map.NextJoin(serial, user, gamepad));
  EXPECT_EQ(user, 2);
  EXPECT_EQ(gamepad, 0);
  EXPECT_TRUE(map.HidesJoinPress(0, GAME_INPUT_GAMEPAD_B));
  map.UpdatePlayers(gamepads.pads);
  EXPECT_FALSE(map.HidesJoinPress(0, GAME_INPUT_GAMEPAD_B));
}

TEST_F(LN_InputMapTest, AGamepadPluggedBackInTakesItsPlayersPlace)
{
  LoadGameMap(GAME_INPUT_PRESS_TO_JOIN);
  LN_InputMap &map = LN_InputMap::Get();
  Gamepads gamepads;
  map.UpdatePlayers(gamepads.pads);
  uint64_t serial = map.JoinSerial();
  int32_t user = -1;
  int32_t gamepad = -1;
  gamepads.Press(0, GAME_INPUT_GAMEPAD_A);
  gamepads.Press(1, GAME_INPUT_GAMEPAD_A);
  map.UpdatePlayers(gamepads.pads);
  ASSERT_TRUE(map.NextJoin(serial, user, gamepad));
  ASSERT_TRUE(map.NextJoin(serial, user, gamepad));
  EXPECT_EQ(map.GamepadOfUser(0), 0);
  EXPECT_EQ(map.GamepadOfUser(1), 1);

  /* Gamepad 0 is unplugged: player 0 keeps it. A gamepad that turns up as number 3 and presses
   * a button takes player 0's place: no new join. */
  gamepads.pads[0] = LN_InputMap::GamepadButtons();
  map.UpdatePlayers(gamepads.pads);
  EXPECT_EQ(map.GamepadOfUser(0), 0);
  gamepads.Press(3, GAME_INPUT_GAMEPAD_X);
  map.UpdatePlayers(gamepads.pads);
  EXPECT_EQ(map.GamepadOfUser(0), 3);
  EXPECT_TRUE(map.IsUserJoined(0));
  EXPECT_FALSE(map.NextJoin(serial, user, gamepad));
  EXPECT_TRUE(map.HidesJoinPress(3, GAME_INPUT_GAMEPAD_X));

  /* A player given a gamepad that isn't there waits for one: the next new gamepad is theirs, and
   * as they never joined, they join now. */
  map.SetUserGamepad(4, 6);
  gamepads.pads[6] = LN_InputMap::GamepadButtons();
  map.UpdatePlayers(gamepads.pads);
  gamepads.Press(5, GAME_INPUT_GAMEPAD_A);
  map.UpdatePlayers(gamepads.pads);
  EXPECT_EQ(map.GamepadOfUser(4), 5);
  ASSERT_TRUE(map.NextJoin(serial, user, gamepad));
  EXPECT_EQ(user, 4);
  EXPECT_EQ(gamepad, 5);
}

TEST_F(LN_InputMapTest, WithoutPressToJoinTheFirstPressJoinsAndStaysVisible)
{
  LoadGameMap();
  LN_InputMap &map = LN_InputMap::Get();
  Gamepads gamepads;
  map.UpdatePlayers(gamepads.pads);
  uint64_t serial = map.JoinSerial();
  int32_t user = -1;
  int32_t gamepad = -1;
  gamepads.Press(3, GAME_INPUT_GAMEPAD_A);
  map.UpdatePlayers(gamepads.pads);
  ASSERT_TRUE(map.NextJoin(serial, user, gamepad));
  EXPECT_EQ(user, 3);
  EXPECT_EQ(gamepad, 3);
  /* Player 3 already had gamepad 3: the press is the game's too (a menu's first A works). */
  EXPECT_FALSE(map.HidesJoinPress(3, GAME_INPUT_GAMEPAD_A));
  /* Given another gamepad, a joined player stays joined: no second join. */
  map.SetUserGamepad(3, 5);
  EXPECT_TRUE(map.IsUserJoined(3));
  gamepads.Press(5, GAME_INPUT_GAMEPAD_A);
  map.UpdatePlayers(gamepads.pads);
  EXPECT_FALSE(map.NextJoin(serial, user, gamepad));
}

TEST_F(LN_InputMapTest, EachPlayerHasTheirOwnGamepad)
{
  LoadGameMap();
  Input input;
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, true); });
  EXPECT_TRUE(input.State("Jump", 0).active);
  EXPECT_FALSE(input.State("Jump", 1).active);
  LN_GamepadSnapshot gamepad;
  gamepad.buttons[GAME_INPUT_GAMEPAD_A] = 1;
  gamepad.pressed[GAME_INPUT_GAMEPAD_A] = 1;
  input.Gamepad(1, gamepad);
  EXPECT_TRUE(input.State("Jump", 1).pressed);
  EXPECT_EQ(input.tracker.Find(LN_InputMap::Get().Find("Jump"), LN_MAX_GAMEPADS), nullptr);
}

TEST_F(LN_InputMapTest, AnInputContextLimitsTheAction)
{
  LoadGameMap(0, "Game");
  Input input;
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, true); }, "Inventory");
  EXPECT_FALSE(input.State("Jump").active);
  EXPECT_FALSE(input.State("Jump").pressed);
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, false); }, "Game");
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, true); }, "Game");
  EXPECT_TRUE(input.State("Jump").pressed);
  /* Leaving the context lets go of the action. */
  input.Frame([] {}, "Inventory");
  EXPECT_TRUE(input.State("Jump").released);
}

TEST_F(LN_InputMapTest, EachPlayerHasTheContextOfTheirOwnMenu)
{
  LoadGameMap(0, "Game");
  Input input;
  LN_GamepadSnapshot pad;
  pad.connected = true;
  pad.buttons[GAME_INPUT_GAMEPAD_A] = 1;
  pad.pressed[GAME_INPUT_GAMEPAD_A] = 1;
  /* Player 0 is in an inventory menu; player 1 still plays: only player 1 jumps. */
  LN_InputContexts contexts;
  contexts.fill("Game");
  contexts[0] = "Inventory";
  input.snapshot.SetGamepadForTests(0, pad);
  input.snapshot.SetGamepadForTests(1, pad);
  input.tracker.Update(input.snapshot, contexts, input.tick++);
  EXPECT_FALSE(input.State("Jump", 0).pressed);
  EXPECT_TRUE(input.State("Jump", 1).pressed);
}

TEST_F(LN_InputMapTest, RebindSaveRestoreAndReset)
{
  LoadGameMap();
  LN_InputMap &map = LN_InputMap::Get();
  const int32_t jump = map.Find("Jump");
  const uint64_t revision = map.Revision();
  LN_InputCapture capture;
  LN_InputBinding binding;
  Input input;
  input.device.ClearInputs();
  input.Key(SCA_IInputDevice::JKEY, true);
  input.snapshot.Capture(&input.device, nullptr);
  ASSERT_EQ(capture.Update(
                input.snapshot, 0, LN_InputDeviceClass::KeyboardMouse, false, binding),
            LN_InputCapture::Result::Captured);
  map.Rebind(jump, binding);
  EXPECT_NE(map.Revision(), revision);
  ASSERT_EQ(map.Action(jump).bindings.size(), size_t(2));
  EXPECT_EQ(map.ActionText(jump, LN_InputDeviceClass::KeyboardMouse, 0), "J");
  EXPECT_EQ(map.ActionText(jump, LN_InputDeviceClass::Gamepad, 0), "A");

  /* A vector action keeps one key per direction. */
  const int32_t move = map.Find("Move");
  LN_InputBinding up = map.Action(move).bindings[0];
  up.code = blender::EVT_UPARROWKEY;
  up.input = SCA_IInputDevice::UPARROWKEY;
  map.Rebind(move, up);
  EXPECT_EQ(map.ActionText(move, LN_InputDeviceClass::KeyboardMouse, GAME_INPUT_DIRECTION_UP),
            "Up Arrow");
  EXPECT_EQ(map.ActionText(move, LN_InputDeviceClass::KeyboardMouse, GAME_INPUT_DIRECTION_LEFT),
            "A");

  const std::string saved = map.Save();
  EXPECT_NE(saved.find("\"Jump\""), std::string::npos);
  EXPECT_EQ(saved.find("\"Accept\""), std::string::npos);
  map.Reset();
  EXPECT_EQ(map.ActionText(jump, LN_InputDeviceClass::KeyboardMouse, 0), "Space Bar");
  ASSERT_TRUE(map.Restore(saved));
  EXPECT_EQ(map.ActionText(jump, LN_InputDeviceClass::KeyboardMouse, 0), "J");
  EXPECT_EQ(map.ActionText(move, LN_InputDeviceClass::KeyboardMouse, GAME_INPUT_DIRECTION_UP),
            "Up Arrow");
  /* Not an Input Map file: nothing changes. */
  EXPECT_FALSE(map.Restore("{\"something\": 1}"));
  EXPECT_FALSE(map.Restore("not json"));
  EXPECT_EQ(map.ActionText(jump, LN_InputDeviceClass::KeyboardMouse, 0), "J");

  /* The rebound key works. */
  input.Frame([&] { input.Key(SCA_IInputDevice::JKEY, false); });
  input.Frame([&] { input.Key(SCA_IInputDevice::JKEY, true); });
  EXPECT_TRUE(input.State("Jump").pressed);
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, true); });
  EXPECT_FALSE(input.State("Jump").pressed);
}

TEST_F(LN_InputMapTest, SwapAcceptAndBackSwapsTheirGamepadButtons)
{
  LoadGameMap(GAME_INPUT_SWAP_ACCEPT_BACK);
  const LN_InputMap &map = LN_InputMap::Get();
  EXPECT_EQ(
      map.ActionText(map.FindBuiltin(LN_UIAction::Accept), LN_InputDeviceClass::Gamepad, 0), "B");
  EXPECT_EQ(
      map.ActionText(map.FindBuiltin(LN_UIAction::Cancel), LN_InputDeviceClass::Gamepad, 0), "A");
  EXPECT_EQ(map.ActionText(map.FindBuiltin(LN_UIAction::Accept),
                           LN_InputDeviceClass::KeyboardMouse,
                           0),
            "Return");
}

TEST_F(LN_InputMapTest, CaptureTakesTheNextControl)
{
  LN_InputBinding binding;
  {
    /* Ctrl + K. */
    LN_InputCapture capture;
    Input input;
    input.device.ClearInputs();
    input.Key(SCA_IInputDevice::LEFTCTRLKEY, true);
    input.snapshot.Capture(&input.device, nullptr);
    EXPECT_EQ(
        capture.Update(input.snapshot, 0, LN_InputDeviceClass::KeyboardMouse, false, binding),
        LN_InputCapture::Result::Waiting);
    input.device.ClearInputs();
    input.Key(SCA_IInputDevice::KKEY, true);
    input.snapshot.Capture(&input.device, nullptr);
    ASSERT_EQ(
        capture.Update(input.snapshot, 0, LN_InputDeviceClass::KeyboardMouse, false, binding),
        LN_InputCapture::Result::Captured);
    EXPECT_EQ(LN_InputMap::BindingText(binding), "Ctrl+K");
  }
  {
    /* Shift alone, when it is let go. */
    LN_InputCapture capture;
    Input input;
    input.device.ClearInputs();
    input.Key(SCA_IInputDevice::LEFTSHIFTKEY, true);
    input.snapshot.Capture(&input.device, nullptr);
    EXPECT_EQ(
        capture.Update(input.snapshot, 0, LN_InputDeviceClass::KeyboardMouse, false, binding),
        LN_InputCapture::Result::Waiting);
    input.device.ClearInputs();
    input.Key(SCA_IInputDevice::LEFTSHIFTKEY, false);
    input.snapshot.Capture(&input.device, nullptr);
    ASSERT_EQ(
        capture.Update(input.snapshot, 0, LN_InputDeviceClass::KeyboardMouse, false, binding),
        LN_InputCapture::Result::Captured);
    EXPECT_EQ(binding.input, SCA_IInputDevice::LEFTSHIFTKEY);
    EXPECT_EQ(binding.modifiers, 0);
  }
  {
    /* Esc cancels, also while waiting for a gamepad. */
    LN_InputCapture capture;
    Input input;
    input.device.ClearInputs();
    input.Key(SCA_IInputDevice::ESCKEY, true);
    input.snapshot.Capture(&input.device, nullptr);
    EXPECT_EQ(capture.Update(input.snapshot, 0, LN_InputDeviceClass::Gamepad, false, binding),
              LN_InputCapture::Result::Cancelled);
  }
  {
    /* A gamepad button; a held stick counts only after it was centred. */
    LN_InputCapture capture;
    Input input;
    input.device.ClearInputs();
    input.snapshot.Capture(&input.device, nullptr);
    LN_GamepadSnapshot gamepad = Stick(0.9f, 0.0f);
    gamepad.connected = true;
    input.snapshot.SetGamepadForTests(0, gamepad);
    EXPECT_EQ(capture.Update(input.snapshot, 0, LN_InputDeviceClass::Gamepad, true, binding),
              LN_InputCapture::Result::Waiting);
    gamepad = Stick(0.0f, 0.0f);
    gamepad.connected = true;
    input.snapshot.SetGamepadForTests(0, gamepad);
    EXPECT_EQ(capture.Update(input.snapshot, 0, LN_InputDeviceClass::Gamepad, true, binding),
              LN_InputCapture::Result::Waiting);
    gamepad = Stick(0.0f, 0.8f);
    gamepad.connected = true;
    input.snapshot.SetGamepadForTests(0, gamepad);
    ASSERT_EQ(capture.Update(input.snapshot, 0, LN_InputDeviceClass::Gamepad, true, binding),
              LN_InputCapture::Result::Captured);
    EXPECT_EQ(binding.code, GAME_INPUT_GAMEPAD_LEFT_STICK);
    /* For a button action the same push is Left Stick Up. */
    LN_InputCapture button_capture;
    gamepad = Stick(0.0f, 0.0f);
    gamepad.connected = true;
    input.snapshot.SetGamepadForTests(0, gamepad);
    button_capture.Update(input.snapshot, 0, LN_InputDeviceClass::Gamepad, false, binding);
    gamepad = Stick(0.0f, 0.8f);
    gamepad.connected = true;
    input.snapshot.SetGamepadForTests(0, gamepad);
    ASSERT_EQ(
        button_capture.Update(input.snapshot, 0, LN_InputDeviceClass::Gamepad, false, binding),
        LN_InputCapture::Result::Captured);
    EXPECT_EQ(binding.code, GAME_INPUT_GAMEPAD_LEFT_STICK_UP);
    gamepad = LN_GamepadSnapshot();
    gamepad.connected = true;
    gamepad.pressed[GAME_INPUT_GAMEPAD_Y] = 1;
    gamepad.buttons[GAME_INPUT_GAMEPAD_Y] = 1;
    input.snapshot.SetGamepadForTests(0, gamepad);
    ASSERT_EQ(capture.Update(input.snapshot, 0, LN_InputDeviceClass::Gamepad, false, binding),
              LN_InputCapture::Result::Captured);
    EXPECT_EQ(LN_InputMap::BindingText(binding), "Y");
  }
}

TEST_F(LN_InputMapTest, ThePlayersLastDeviceNamesAutomaticBindings)
{
  LoadGameMap();
  LN_InputMap &map = LN_InputMap::Get();
  const int32_t jump = map.Find("Jump");
  Input input;
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, true); });
  map.NoteInput(input.snapshot);
  EXPECT_EQ(map.ActionText(jump, LN_InputDeviceClass::Automatic, 0), "Space Bar");
  LN_GamepadSnapshot gamepad;
  gamepad.connected = true;
  gamepad.pressed[GAME_INPUT_GAMEPAD_B] = 1;
  input.snapshot.SetGamepadForTests(0, gamepad);
  map.NoteInput(input.snapshot);
  EXPECT_EQ(map.ActionText(jump, LN_InputDeviceClass::Automatic, 0), "A");
}

TEST_F(LN_InputMapTest, ActionNamesInTextFollowThePlayersDevice)
{
  LoadGameMap();
  LN_InputMap &map = LN_InputMap::Get();
  Input input;
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, true); });
  map.NoteInput(input.snapshot);
  const uint64_t keyboard_revision = map.DeviceRevision();
  /* Known actions become their key; anything else in braces stays as written. */
  EXPECT_EQ(map.ExpandActionNames("{Jump} jumps, {Nothing} and {} stay", 0),
            "Space Bar jumps, {Nothing} and {} stay");
  /* A lone brace before a name does not hide it. */
  EXPECT_EQ(map.ExpandActionNames("Press { or {Jump}, {{Jump}}", 0),
            "Press { or Space Bar, {Space Bar}");
  /* A vector action names its four keys. */
  EXPECT_EQ(map.ExpandActionNames("{Move} walks", 0), "WASD walks");
  /* A device can be named whatever the player uses. */
  EXPECT_EQ(map.ExpandActionNames("{Jump:Gamepad} / {Jump:Keyboard}", 0), "A / Space Bar");
  EXPECT_EQ(map.ExpandActionNames("{Jump:Nothing}", 0), "{Jump:Nothing}");
  /* Braces that name no action go to a resolver (game property values); unknown ones stay. */
  const LN_InputMap::TokenResolver resolver = [](const std::string &token, std::string &r_text) {
    if (token == "Player.gold" || token == "health:1") {
      r_text = token == "Player.gold" ? "42" : "75.0";
      return true;
    }
    return false;
  };
  EXPECT_EQ(map.ExpandActionNames("{Jump}: {Player.gold} gold, {health:1} hp, {nothing}", 0,
                                  &resolver),
            "Space Bar: 42 gold, 75.0 hp, {nothing}");
  /* An action name wins over a property of that name; empty braces never reach the resolver. */
  const LN_InputMap::TokenResolver everything = [](const std::string &, std::string &r_text) {
    r_text = "value";
    return true;
  };
  EXPECT_EQ(map.ExpandActionNames("{Jump} {} {x}", 0, &everything), "Space Bar {} value");
  /* Other players play with their own gamepad. */
  EXPECT_EQ(map.ExpandActionNames("{Jump} jumps", 1), "A jumps");

  LN_GamepadSnapshot gamepad;
  gamepad.connected = true;
  gamepad.pressed[GAME_INPUT_GAMEPAD_A] = 1;
  input.snapshot.SetGamepadForTests(0, gamepad);
  map.NoteInput(input.snapshot);
  EXPECT_NE(map.DeviceRevision(), keyboard_revision);
  EXPECT_EQ(map.ExpandActionNames("{Jump} jumps", 0), "A jumps");
  /* Using the same device again changes nothing. */
  const uint64_t gamepad_revision = map.DeviceRevision();
  map.NoteInput(input.snapshot);
  EXPECT_EQ(map.DeviceRevision(), gamepad_revision);
  /* Back on the keyboard, an action bound only to the gamepad shows its gamepad control. */
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, false); });
  input.Frame([&] { input.Key(SCA_IInputDevice::LEFTARROWKEY, true); });
  map.NoteInput(input.snapshot);
  ASSERT_FALSE(map.LastUsedGamepad());
  const std::string look_up = map.ActionText(
      map.Find("Look Up"), LN_InputDeviceClass::Gamepad, 0);
  ASSERT_FALSE(look_up.empty());
  EXPECT_EQ(map.ExpandActionNames("{Look Up}", 0), look_up);
}

TEST_F(LN_InputMapTest, GamepadButtonsAreNamedAsEachKindOfPadLabelsThem)
{
  const auto name = [](const int16_t code, const DEV_PadFamily family) {
    LN_InputBinding binding;
    binding.device = LN_InputDevice::Gamepad;
    binding.code = code;
    return LN_InputMap::BindingText(binding, family);
  };
  using Pad = DEV_PadFamily;
  /* Unknown pads keep the generic names. */
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_A, Pad::Generic), "A");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_SELECT, Pad::Generic), "Select");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_LEFT_STICK_PRESS, Pad::Generic), "L3");
  /* PlayStation: shapes, L1/R1/L2/R2, and the system buttons of each generation. */
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_A, Pad::PlayStation4), "Cross");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_B, Pad::PlayStation5), "Circle");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_X, Pad::PlayStation3), "Square");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_Y, Pad::PlayStation4), "Triangle");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_LEFT_SHOULDER, Pad::PlayStation4), "L1");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_RIGHT_TRIGGER, Pad::PlayStation4), "R2");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_RIGHT_STICK_PRESS, Pad::PlayStation4), "R3");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_SELECT, Pad::PlayStation4), "Share");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_SELECT, Pad::PlayStation5), "Create");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_SELECT, Pad::PlayStation3), "Select");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_START, Pad::PlayStation5), "Options");
  /* Xbox: the letters, LS/RS, View and Menu (Back and Start on the 360 pad). */
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_A, Pad::Xbox), "A");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_LEFT_TRIGGER, Pad::Xbox), "LT");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_LEFT_STICK_PRESS, Pad::Xbox), "LS");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_SELECT, Pad::Xbox), "View");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_START, Pad::Xbox), "Menu");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_SELECT, Pad::Xbox360), "Back");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_START, Pad::Xbox360), "Start");
  /* Nintendo: SDL reports the face buttons by their labels; L/R, ZL/ZR, - and +. */
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_A, Pad::Nintendo), "A");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_RIGHT_SHOULDER, Pad::Nintendo), "R");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_LEFT_TRIGGER, Pad::Nintendo), "ZL");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_SELECT, Pad::Nintendo), "-");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_START, Pad::Nintendo), "+");
  /* The D-pad and the sticks read the same on every pad. */
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_DPAD_UP, Pad::PlayStation4), "D-Pad Up");
  EXPECT_EQ(name(GAME_INPUT_GAMEPAD_LEFT_STICK, Pad::Nintendo), "Left Stick");
}

TEST_F(LN_InputMapTest, EachPlayersTextNamesTheButtonsOfTheirOwnGamepad)
{
  /* Gamepads Start at Player 1: gamepad 0 is player 1's, gamepad 1 player 2's. */
  LoadGameMap(GAME_INPUT_GAMEPADS_FROM_PLAYER_1);
  LN_InputMap &map = LN_InputMap::Get();
  Gamepads gamepads;
  gamepads.pads[0].family = DEV_PadFamily::PlayStation4;
  gamepads.pads[1].family = DEV_PadFamily::Xbox;
  const uint64_t revision = map.DeviceRevision();
  map.UpdatePlayers(gamepads.pads);
  /* A pad of another kind refreshes the texts that name its buttons. */
  EXPECT_NE(map.DeviceRevision(), revision);
  EXPECT_EQ(map.PadFamilyOfUser(1), DEV_PadFamily::PlayStation4);
  EXPECT_EQ(map.ExpandActionNames("{Jump} jumps", 1), "Cross jumps");
  EXPECT_EQ(map.ExpandActionNames("{Jump} jumps", 2), "A jumps");
  EXPECT_EQ(map.ExpandActionNames("{Jump:Gamepad} / {Jump:Keyboard}", 1), "Cross / Space Bar");
  /* Player 0 has no gamepad: a sign everyone reads names the first gamepad plugged in. */
  EXPECT_EQ(map.PadFamilyOfUser(0), DEV_PadFamily::PlayStation4);
  EXPECT_EQ(map.ExpandActionNames("{Jump:Gamepad} / {Jump:Keyboard}", 0), "Cross / Space Bar");
  /* The same pads again change nothing. */
  const uint64_t same = map.DeviceRevision();
  map.UpdatePlayers(gamepads.pads);
  EXPECT_EQ(map.DeviceRevision(), same);
  /* Unplugged, the pad keeps its names (it may come back); another kind plugged in renames. */
  gamepads.pads[0].connected = false;
  map.UpdatePlayers(gamepads.pads);
  EXPECT_EQ(map.ExpandActionNames("{Jump}", 1), "Cross");
  gamepads.pads[0].connected = true;
  gamepads.pads[0].serial = 100;
  gamepads.pads[0].family = DEV_PadFamily::Nintendo;
  map.UpdatePlayers(gamepads.pads);
  EXPECT_EQ(map.ExpandActionNames("{Jump}", 1), "A");
  /* With no gamepad plugged in, a player without one reads the generic names. */
  for (LN_InputMap::GamepadButtons &pad : gamepads.pads) {
    pad.connected = false;
  }
  map.UpdatePlayers(gamepads.pads);
  EXPECT_EQ(map.PadFamilyOfUser(0), DEV_PadFamily::Generic);
}

}  // namespace

namespace {

TEST_F(LN_InputMapTest, EveryWheelNotchPressesAgain)
{
  blender::GameData game;
  blender::BKE_game_input_map_ensure_builtins(&game);
  blender::GameInputAction *next = blender::BKE_game_input_action_add(
      &game, "Next Weapon", GAME_INPUT_ACTION_BUTTON);
  blender::BKE_game_input_binding_add(next, GAME_INPUT_MOUSE)->code = blender::WHEELUPMOUSE;
  LN_InputMap::Get().Load(&game);
  blender::BKE_game_input_map_free(&game);
  Input input;
  int presses = 0;
  for (int notch = 0; notch < 3; ++notch) {
    input.Frame([&] { input.device.ConvertWheelEvent(1); });
    presses += input.State("Next Weapon").pressed ? 1 : 0;
  }
  EXPECT_EQ(presses, 3);
  input.Frame();
  EXPECT_TRUE(input.State("Next Weapon").released);
}

TEST_F(LN_InputMapTest, ARebindDoesNotPressOrLoseTheActions)
{
  LoadGameMap();
  LN_InputMap &map = LN_InputMap::Get();
  Input input;
  /* Jump is held through a rebind of another action. */
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, true); });
  ASSERT_TRUE(input.State("Jump").active);
  /* The stick the player pushed to rebind Look Up is still held when the rebind ends. */
  input.Gamepad(0, Stick(0.0f, 0.9f));
  LN_InputBinding stick;
  stick.device = LN_InputDevice::Gamepad;
  stick.code = GAME_INPUT_GAMEPAD_RIGHT_STICK_UP;
  map.Rebind(map.Find("Look Up"), stick);
  LN_GamepadSnapshot gamepad = Stick(0.0f, 0.0f);
  gamepad.axes[3] = -32000;
  input.Gamepad(0, gamepad);
  EXPECT_TRUE(input.State("Look Up").active);
  EXPECT_FALSE(input.State("Look Up").pressed);
  EXPECT_TRUE(input.State("Jump").active);
  EXPECT_FALSE(input.State("Jump").pressed);
  /* Jump let go in the next tick still reports its release. */
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, false); });
  EXPECT_TRUE(input.State("Jump").released);
}

TEST_F(LN_InputMapTest, AGapInUpdatesStartsOver)
{
  LoadGameMap();
  Input input;
  input.Frame([&] { input.Key(SCA_IInputDevice::SPACEKEY, true); });
  ASSERT_TRUE(input.State("Jump").pressed);
  /* Space is let go while nothing samples the actions (their tree was off): no late release
   * when sampling starts again. */
  input.device.ClearInputs();
  input.Key(SCA_IInputDevice::SPACEKEY, false);
  input.snapshot.Capture(&input.device, nullptr);
  input.tick += 10;
  input.Frame();
  EXPECT_FALSE(input.State("Jump").released);
  EXPECT_FALSE(input.State("Jump").active);
}

TEST_F(LN_InputMapTest, RefreshDropsPressesTheUITook)
{
  LoadGameMap();
  Input input;
  input.device.ClearInputs();
  input.Key(SCA_IInputDevice::SPACEKEY, true);
  input.snapshot.Capture(&input.device, nullptr);
  input.tracker.Update(input.snapshot, LN_InputContexts(), input.tick++);
  ASSERT_TRUE(input.State("Jump").pressed);
  /* The UI used the press (a focused button's Accept): the frame sees it held, not pressed. */
  input.snapshot.ConsumePress(SCA_IInputDevice::SPACEKEY);
  input.tracker.Refresh(input.snapshot, LN_InputContexts());
  EXPECT_FALSE(input.State("Jump").pressed);
  EXPECT_TRUE(input.State("Jump").active);
  /* The next tick goes on from the refreshed history. */
  input.Frame();
  EXPECT_FALSE(input.State("Jump").pressed);
  EXPECT_TRUE(input.State("Jump").active);
}

}  // namespace
